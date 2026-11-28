package com.ivanna.omega.organism

import android.content.Context
import android.media.AudioManager
import android.os.SystemClock
import android.util.Log
import com.ivanna.omega.audio.IvannaAudioProfile
import com.ivanna.omega.audio.ProfilesLoader
import com.ivanna.omega.audio.RealAcousticSensor
import com.ivanna.omega.core.ParameterStore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import java.io.File

/**
 * OrganismController — orquestador del cableado mic → organismo (v2.5.506).
 *
 * AUTONOMÍA (desde que se abre la app, sin que Esse tenga que batallar):
 *   1. Al arrancar (PersistedStateRestorer), si el interruptor maestro
 *      está ON, arranca el consumidor de snapshots + el agregador + el
 *      vaciado de la cola. La conexión se establece sola; la captura y
 *      el envío arrancan solos cuando suena música (beacon de Ruta B
 *      avanzando o AudioManager.isMusicActive() — la Ruta A ya no decide:
 *      su 0 µs/hop atoraba el Organismo aunque sonara música).
 *   2. Si algo falla, la app sigue sonando igual y lo reintenta sola
 *      (cola durable + backoff). El audio NUNCA depende del envío.
 *   3. UN interruptor maestro ("Organismo automático: ON/OFF") apaga
 *      todo el tráfico al organismo. Apagarlo NO apaga el micrófono de Φ
 *      (ese ya corría por defecto desde la 2.5.369 y tiene su propio
 *      control): apaga la lectura de frames para el organismo y el
 *      envío. Nada sale del teléfono con el interruptor en OFF.
 *   4. CIERRE DIFERIDO (consejo 2026-11-26, defecto #5): stop() NO
 *      intenta un último envío. El chunk final de aggregator.close()
 *      queda encolado durable y se envía al próximo arranque. Intentar
 *      un flush y cancelar el scope acto seguido era una carrera: el
 *      envío final no estaba garantizado. El diferido sí lo está (la
 *      cola es durable y el arranque reanuda el vaciado).
 *   5. RETENCIÓN Y BORRADO (defecto #9): la cola vive 7 días como
 *      máximo y el usuario puede borrarla entera desde la tarjeta
 *      (clearQueue). Los JSON están en filesDir (privado de la app,
 *      sin cifrar en reposo).
 *
 * ESTADO (el indicador de la UI):
 *   Capturando / En cola / Sin conexión / Error recuperable, más los
 *   honestos: Apagado / Sin token / Token inválido.
 *
 * LÍMITES HONESTOS (consejo, no se esconden):
 *   * Android exige RECORD_AUDIO (una vez, revocable) y la notificación
 *     persistente del foreground service para captura prolongada.
 *   * Si el token expira sin endpoint de refresh, hay que reautorizar
 *     una vez (el estado lo dice: "Token inválido").
 */
object OrganismController {
    private const val TAG = "OrganismController"
    private const val PREFS = "organism_prefs"
    private const val KEY_ENABLED = "organism_enabled"

    /**
     * T1/510 — sondeo de perfil (VUELTA §3.2): cada 30 min se pide el
     * perfil vigente; la primera comprobación es a los 60 s del arranque
     * (la app ya asentó) y `flushOnce()` además comprueba de forma barata
     * (solo un timestamp, sin red) si toca sondear. Un GET es idempotente
     * y el sobre es diminuto; la descarga completa solo ocurre si la
     * versión es mayor que la aplicada.
     */
    const val PROFILE_POLL_INTERVAL_MS = 30L * 60L * 1000L
    const val PROFILE_POLL_INITIAL_DELAY_MS = 60_000L

    /**
     * Vocabulario de `appliedProfileStatus` (VUELTA §3.1 `status`, más
     * los dos estados locales): `none` = aún no hay perfil del servidor;
     * `stable` / `candidate` / `ab_running` / `rolled_back` = estado del
     * perfil aplicado según el servidor; `quarantined` = la última
     * descarga se rechazó en validación (el último bueno sigue sonando).
     * Un perfil `promoted` por el servidor queda en `stable` (ya ES el
     * estable) y su `abState` registra `promoted`.
     */
    const val PROFILE_STATUS_NONE = "none"
    const val PROFILE_STATUS_STABLE = "stable"
    const val PROFILE_STATUS_CANDIDATE = "candidate"
    const val PROFILE_STATUS_AB_RUNNING = "ab_running"
    const val PROFILE_STATUS_ROLLED_BACK = "rolled_back"
    const val PROFILE_STATUS_QUARANTINED = "quarantined"

    enum class Status {
        OFF, CAPTURING, QUEUED, OFFLINE, ERROR, NO_TOKEN, TOKEN_INVALID
    }

    data class State(
        val status: Status = Status.OFF,
        val enabled: Boolean = true,
        val detail: String = "",
        val queued: Int = 0,
        val sentTotal: Long = 0L,
        val micReanchors: Int = 0,
        /** Dispositivo por el que entra el mic ahora mismo (defecto #11). */
        val micRoutedDevice: String = "desconocido",
        /**
         * T2/510 — perfil aplicado que está sonando (atribución A/B).
         * Lo actualiza [recordAppliedProfile] tras cada aplicación y lo
         * lee `enqueueChunk` para etiquetar cada chunk en `telemetry`.
         * Neutros = fábrica (`factory` / 0 / `none` / sin hash).
         * `version` es `Long` como el sobre de `GET /profile/current`
         * y como `ProfileApplier` (>= 1 cuando viene del servidor).
         * `abState` usa el vocabulario de la VUELTA §3.1:
         * `none | candidate | ab_running | promoted | rolled_back`.
         * Solo vive en memoria: tras un reinicio vuelve a neutros hasta
         * la próxima aplicación (pendiente T1: re-derivarlo del
         * `.ivanna.json` guardado o persistirlo).
         */
        val appliedProfileId: String = OrganismPayload.DEFAULT_APPLIED_PROFILE_ID,
        val appliedProfileVersion: Long = OrganismPayload.DEFAULT_APPLIED_PROFILE_VERSION,
        val abState: String = OrganismPayload.DEFAULT_AB_STATE,
        val appliedProfileHash: String = "",
        /**
         * T1/510 — estado del perfil aplicado (para la tarjeta del
         * organismo). Vocabulario en [PROFILE_STATUS_NONE] y siguientes.
         * Lo actualiza el sondeo tras cada aplicación, rechazo o
         * rollback; `refreshState()` lo conserva (solo lo cambia el
         * camino de la VUELTA). No duplica `appliedProfileVersion`
         * (T2): es el estado, no la versión.
         */
        val appliedProfileStatus: String = PROFILE_STATUS_NONE
    )

    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()

    private var appContext: Context? = null
    private var queue: OrganismQueue? = null
    private var sender: OrganismSender? = null
    private var tokenStore: TokenStore? = null
    private var aggregator: SessionAggregator? = null
    private var consumer: SensorSnapshotConsumer? = null
    private var scope: CoroutineScope? = null
    private var flushJob: Job? = null
    private var sensorReanchorsSeen = 0
    private var pipelineReanchorsSeen = 0

    // ── T1/510 — la VUELTA (sondeo + aplicación) ────────────────────────

    /**
     * H5 — cableado que la especificación omitió: el controlador no tiene
     * `DSPState`, así que MainActivity registra aquí cómo aplicar un
     * `IvannaAudioProfile` al motor (`ProfileManagerBridge.applyProfile`
     * → `applyToDsp`). Devuelve true solo si el read-back confirma
     * `muselink-live` como perfil vigente. Null = la UI aún no compuso:
     * el install falla cerrado (diario PENDING) y se reintenta en el
     * próximo sondeo. Se registra una vez en `OmegaApp` (DisposableEffect)
     * y sobrevive a stop()/start() (el `dsp` que captura es estable en
     * toda la vida de la app); NO se anula en stop().
     */
    @Volatile
    var onApplyProfile: ((IvannaAudioProfile) -> Boolean)? = null

    /** Read-back del perfil aplicado, misma instancia de puente que [onApplyProfile]. */
    @Volatile
    var onReadAppliedProfileId: (() -> String?)? = null

    private var profileFetcher: ProfileFetcher? = null
    private var profileApplier: ProfileApplier? = null
    private var pollJob: Job? = null
    private val profilePollMutex = Mutex()

    /** Último sondeo de perfil que ARRANCÓ (el due-check de flushOnce lo lee). */
    @Volatile
    private var lastProfileCheckMs: Long = 0L

    /**
     * Último perfil válido aplicado en este proceso (guarda C2.g: sin
     * saltos de operador > 0.2 por versión). Solo vive en memoria: tras
     * un reinicio la guarda no aplica hasta la próxima aplicación (el
     * `.ivanna.json` estable guardado podría re-validarse al arrancar;
     * pendiente, no bloqueante: el A/B del servidor también gatea).
     */
    @Volatile
    private var lastAppliedValidated: ValidatedProfile? = null

    /** Confirmación `POST applied` pendiente de reintento (un POST por ciclo). */
    @Volatile
    private var pendingConfirmation: com.google.gson.JsonObject? = null

    /** Derivación pura del estado visible (testeable en JVM). */
    fun deriveStatus(
        enabled: Boolean,
        hasToken: Boolean,
        tokenInvalid: Boolean,
        musicActive: Boolean,
        queued: Int,
        lastFlush: OrganismSender.Outcome?,
        lastErrorAtMs: Long,
        nowMs: Long
    ): Status {
        if (!enabled) return Status.OFF
        if (!hasToken) return Status.NO_TOKEN
        if (tokenInvalid) return Status.TOKEN_INVALID
        if (musicActive) return Status.CAPTURING
        if (queued > 0) return Status.QUEUED
        return when (lastFlush) {
            OrganismSender.Outcome.RETRY -> Status.OFFLINE
            OrganismSender.Outcome.QUARANTINE,
            OrganismSender.Outcome.TOKEN_INVALID -> Status.ERROR
            else -> if (nowMs - lastErrorAtMs < 60_000L && lastFlush != null) Status.ERROR
            else Status.QUEUED
        }
    }

    fun isEnabled(context: Context): Boolean =
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getBoolean(KEY_ENABLED, true)

    fun setEnabled(context: Context, enabled: Boolean) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit().putBoolean(KEY_ENABLED, enabled).apply()
        if (enabled) start(context) else stop()
        refreshState()
    }

    /** Arranque autónomo al abrir la app. Idempotente. */
    @Synchronized
    fun start(context: Context) {
        val ctx = context.applicationContext
        appContext = ctx
        if (!isEnabled(ctx)) {
            _state.value = State(status = Status.OFF, enabled = false)
            return
        }
        if (scope != null) {
            refreshState()
            return
        }
        // C8: diario de aplicación pendiente (muerte a mitad de VUELTA).
        // Se intenta como mucho una vez por arranque: el flag se levanta
        // aquí y se resetea en stop(); start() además retorna antes si
        // scope != null, así que en un mismo proceso solo se intenta una
        // vez por arranque. No toca State (otro agente trabaja ahí): el
        // resultado queda visible en lastRollbackAttempt + Log.
        restorePendingApplyOnce(ctx)
        val qDir = File(ctx.filesDir, "organism-queue")
        val q = OrganismQueue(qDir)
        val tokens = OrganismTokenStore(ctx)
        queue = q
        tokenStore = tokens
        sender = OrganismSender(tokens, q)
        // T1/510 — la VUELTA: el fetcher GET + el validador/aplicador.
        profileApplier = ProfileApplier(ctx.filesDir)
        profileFetcher = ProfileFetcher(tokens)
        val agg = SessionAggregator()
        aggregator = agg
        // Vía ÚNICA de encolado: el agregador emite cada chunk (ventana
        // cerrada o close() final) por onChunk. El consumidor solo le
        // pasa frames; no encola directo (si no, el chunk final de
        // close() se perdería y las ventanas se duplicarían).
        agg.onChunk = { chunk -> enqueueChunk(chunk, q) }
        val cons = SensorSnapshotConsumer(
            agg,
            onChunk = {},
            // Respaldo del sistema para "hay música": AudioManager lo sabe
            // aunque la Ruta A marque 0 µs/hop. Se evalúa en cada llamada,
            // nunca se cachea; sin AudioManager vale false (fail-closed).
            audioManagerMusicActive = {
                runCatching {
                    (ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager)?.isMusicActive
                }.getOrDefault(false) == true
            }
        )
        consumer = cons
        scope = CoroutineScope(Dispatchers.IO + SupervisorJob())
        RealAcousticSensor.start(ctx)
        cons.start()
        flushJob = scope!!.launch {
            while (true) {
                delay(30_000L)
                flushOnce()
                refreshState()
            }
        }
        // T1/510 — sondeo de perfil junto al flushJob: primera comprobación
        // a los 60 s del arranque y luego cada 30 min. El interruptor
        // maestro lo apaga también: stop() cancela este job (y start() no
        // lo crea si el interruptor está OFF). Todo en Dispatchers.IO;
        // aplicar solo vía el puente registrado por MainActivity.
        lastProfileCheckMs = System.currentTimeMillis()
        pollJob = scope!!.launch {
            delay(PROFILE_POLL_INITIAL_DELAY_MS)
            while (true) {
                pollProfileOnce()
                delay(PROFILE_POLL_INTERVAL_MS)
            }
        }
        // Vigilar el ancla del mic: cada cambio de ruta queda en el estado
        // (dispositivo enrutado actual + contador de re-anclas, defecto #11).
        // Dos fuentes (sensor Φ y pipeline de Ruta A, defecto #7): el
        // dispositivo es el último visto con check real (lastCheckMs > 0)
        // y los re-anclas se suman (son AudioRecords distintos).
        scope!!.launch {
            RealAcousticSensor.micRouteStatus.collect { route ->
                sensorReanchorsSeen = route.reanchorCount
                if (route.lastCheckMs > 0L) lastRoutedDevice = route.routedTypeName
                refreshState()
            }
        }
        scope!!.launch {
            com.ivanna.omega.audio.AudioPipeline.sharedMicRouteStatus.collect { route ->
                pipelineReanchorsSeen = route.reanchorCount
                if (route.lastCheckMs > 0L) lastRoutedDevice = route.routedTypeName
                refreshState()
            }
        }
        Log.i(TAG, "Organismo automático arrancado (consumidor + agregador + cola)")
        refreshState()
    }

    @Synchronized
    fun stop() {
        consumer?.stop()
        consumer = null
        flushJob?.cancel()
        flushJob = null
        // T1/510: el sondeo muere con el resto (nada sale ni entra con el
        // interruptor en OFF). Los callbacks de MainActivity NO se anulan:
        // solo se usan durante los sondeos, que ya no corren.
        pollJob?.cancel()
        pollJob = null
        profileFetcher = null
        profileApplier = null
        lastAppliedValidated = null
        pendingConfirmation = null
        // CIERRE DIFERIDO (defecto #5): el scope se cancela ANTES de
        // cerrar el agregador, para que el enqueueChunk del chunk final
        // no pueda lanzar un flush que se cancelaría a medias. El chunk
        // queda durable en la cola y se envía al próximo arranque.
        scope?.cancel()
        scope = null
        runCatching {
            aggregator?.close()?.let { summary ->
                Log.i(TAG, "Sesión cerrada: ${summary.chunks} chunks, ${summary.framesValid} frames válidos (envío diferido al próximo arranque)")
            }
        }
        aggregator = null
        queue = null
        sender = null
        tokenStore = null
        // C8: el próximo start() es un arranque nuevo y tiene su propio intento de rollback.
        rollbackAttemptedThisBoot = false
        _state.value = State(status = Status.OFF, enabled = false)
        Log.i(TAG, "Organismo detenido (sin tráfico al organismo)")
    }

    /** Borra toda la cola local (defecto #9). Devuelve cuántos se borraron. */
    fun clearQueue(): Int {
        val deleted = queue?.clearAll() ?: 0
        refreshState()
        return deleted
    }

    /** Guarda el token (lo pega Esse una vez, en la tarjeta del organismo). */
    fun saveToken(raw: String): Boolean {
        val store = tokenStore ?: return false
        val ok = store.setToken(raw)
        if (ok) {
            // Defecto #4: un token nuevo re-valida el estado aunque la
            // cola esté vacía (antes quedaba TOKEN_INVALID para siempre).
            tokenInvalidSeen = false
            // T1/510: con token nuevo, el próximo flushOnce sondea el
            // perfil enseguida (sin esperar al ciclo de 30 min).
            lastProfileCheckMs = 0L
            scope?.launch(Dispatchers.IO) { flushOnce() }
            refreshState()
        }
        return ok
    }

    fun hasToken(): Boolean = tokenStore?.getToken()?.isNotBlank() == true

    /**
     * T2/510 — registra el perfil que se acaba de aplicar (lo llama el
     * camino de aplicación de T1 tras validar + traducir + aplicar, y
     * también al restaurar el estable en un rollback). Devuelve el
     * snapshot aplicado para construir el `POST /profile/applied` con
     * [OrganismPayload.appliedConfirmation] (hash + versión incluidos).
     * El siguiente chunk que se encole ya lleva la nueva versión en
     * `telemetry` (vía [enqueueChunk]); no hay que esperar al sondeo.
     * T1/510: [profileStatus] actualiza además `appliedProfileStatus`
     * (null = se conserva el anterior).
     */
    @Synchronized
    fun recordAppliedProfile(
        profileId: String,
        version: Long,
        abState: String,
        profileHash: String = "",
        profileStatus: String? = null
    ): OrganismPayload.AppliedProfile {
        val applied = OrganismPayload.AppliedProfile(
            id = profileId,
            version = version,
            abState = abState,
            hash = profileHash
        )
        _state.value = _state.value.copy(
            appliedProfileId = applied.id,
            appliedProfileVersion = applied.version,
            abState = applied.abState,
            appliedProfileHash = applied.hash,
            appliedProfileStatus = profileStatus ?: _state.value.appliedProfileStatus
        )
        Log.i(TAG, "Perfil aplicado registrado: $profileId v$version abState=$abState hash=${profileHash.take(12)}")
        return applied
    }

    /** Snapshot del perfil aplicado actual (para etiquetar chunks y para T1). */
    fun appliedProfile(): OrganismPayload.AppliedProfile =
        _state.value.let {
            OrganismPayload.AppliedProfile(
                id = it.appliedProfileId,
                version = it.appliedProfileVersion,
                abState = it.abState,
                hash = it.appliedProfileHash
            )
        }

    @Volatile
    private var lastFlushOutcome: OrganismSender.Outcome? = null
    @Volatile
    private var lastErrorAtMs: Long = 0L
    @Volatile
    private var tokenInvalidSeen: Boolean = false
    @Volatile
    private var sentTotal: Long = 0L
    @Volatile
    private var lastRoutedDevice: String = "desconocido"

    // C8: diario de aplicación. Un intento de restauración por arranque
    // como mucho (mitigación contra bucle de rollback).
    @Volatile
    private var rollbackAttemptedThisBoot: Boolean = false

    /** Estado visible del último intento de restauración por diario PENDING (C8). */
    data class RollbackAttemptInfo(
        val journalVersion: Long?,
        val restored: Boolean,
        val reason: String,
        val atMs: Long
    )

    @Volatile
    var lastRollbackAttempt: RollbackAttemptInfo? = null
        private set

    /** Decisión pura del intento de rollback por arranque (C8, testeable en JVM). */
    fun shouldAttemptRollback(
        alreadyAttempted: Boolean,
        check: ProfileApplier.RollbackCheck
    ): Boolean = !alreadyAttempted && check !is ProfileApplier.RollbackCheck.None

    /**
     * C8: al arrancar, si el diario quedó PENDING (la app murió a mitad de
     * aplicar un candidato), reinstala el estable verificado como candidato
     * para que el próximo intento de T1 re-aplique el estable y no el
     * candidato a medias. El diario queda PENDING hasta que T1 lo marque
     * COMMITTED o ROLLED_BACK tras re-aplicar con read-back; la re-aplicación
     * al DSP es T1 (aquí no hay DSP a mano). Si el hash del estable no
     * verifica, NO se restaura nada. Máximo 1 intento por arranque.
     */
    private fun restorePendingApplyOnce(ctx: Context) {
        val check = ProfileApplier(ctx.filesDir).checkPendingRollback()
        if (!shouldAttemptRollback(rollbackAttemptedThisBoot, check)) return
        rollbackAttemptedThisBoot = true
        val nowMs = System.currentTimeMillis()
        when (check) {
            is ProfileApplier.RollbackCheck.Ready -> {
                val restored = ProfileApplier(ctx.filesDir).restoreStableCandidate()
                lastRollbackAttempt = RollbackAttemptInfo(
                    journalVersion = check.version,
                    restored = restored,
                    reason = if (restored) {
                        "estable v${check.version} verificado (SHA-256) y reinstalado como candidato; re-aplicación al DSP pendiente de T1"
                    } else {
                        "no se pudo reinstalar el candidato aunque el estable verificó"
                    },
                    atMs = nowMs
                )
                Log.i(TAG, "C8 rollback: diario PENDING v${check.version}, estable restaurado=$restored")
            }
            is ProfileApplier.RollbackCheck.StableMissing -> {
                lastRollbackAttempt = RollbackAttemptInfo(
                    journalVersion = check.version,
                    restored = false,
                    reason = "diario PENDING v${check.version} pero no hay estable en disco; nada que restaurar",
                    atMs = nowMs
                )
                Log.w(TAG, "C8 rollback: diario PENDING v${check.version} sin estable; no se restaura nada")
            }
            is ProfileApplier.RollbackCheck.StableHashMismatch -> {
                lastRollbackAttempt = RollbackAttemptInfo(
                    journalVersion = check.version,
                    restored = false,
                    reason = "estable v${check.version} NO verifica su SHA-256 (esperado ${check.expectedHash}, actual ${check.actualHash}); no se restaura",
                    atMs = nowMs
                )
                Log.w(TAG, "C8 rollback: estable v${check.version} con hash distinto; NO se restaura")
            }
            is ProfileApplier.RollbackCheck.None -> { /* inalcanzable: shouldAttemptRollback ya lo filtró */ }
        }
    }

    private fun enqueueChunk(chunk: SessionAggregator.SessionChunk, q: OrganismQueue) {
        // T2/510: el chunk lleva el perfil aplicado actual en telemetry.
        val body = OrganismPayload.sessionBody(chunk, applied = appliedProfile())
        val enqueued = q.enqueue(chunk.sessionId, chunk.chunkId, body)
        if (enqueued) {
            scope?.launch(Dispatchers.IO) { flushOnce() }
        }
        refreshState()
    }

    private fun flushOnce() {
        val s = sender ?: return
        val result = runCatching { s.flush() }.getOrNull() ?: return
        lastFlushOutcome = result.outcome
        // Defecto #4: cualquier flush que NO sea 401 limpia el estado de
        // token inválido (el token se corrigió o el fallo era otro).
        if (result.outcome == OrganismSender.Outcome.TOKEN_INVALID) tokenInvalidSeen = true
        else tokenInvalidSeen = false
        if (result.outcome == OrganismSender.Outcome.RETRY) lastErrorAtMs = System.currentTimeMillis()
        if (result.sent > 0) {
            sentTotal += result.sent
            tokenInvalidSeen = false
        }
        // T1/510 — comprobación de versión BARATA en cada flushOnce: solo
        // un timestamp en memoria, sin red. El GET solo ocurre si toca
        // sondear (cada 30 min); un GET cada 30 s serían ~2.900
        // peticiones/día contra un endpoint gratuito, eso no es barato.
        maybePollProfile()
        refreshState()
    }

    private fun refreshState() {
        val ctx = appContext
        val enabled = ctx?.let { isEnabled(it) } ?: true
        val queued = queue?.counts()?.values?.sum() ?: 0
        val music = consumer?.isMusicActive() ?: false
        val status = deriveStatus(
            enabled = enabled,
            hasToken = hasToken(),
            tokenInvalid = tokenInvalidSeen,
            musicActive = music,
            queued = queued,
            lastFlush = lastFlushOutcome,
            lastErrorAtMs = lastErrorAtMs,
            nowMs = System.currentTimeMillis()
        )
        val detail = when (status) {
            Status.OFF -> "Organismo apagado: nada sale del teléfono."
            Status.NO_TOKEN -> "Sin token: pega el token de Muselink una vez para activar el envío."
            Status.TOKEN_INVALID -> "Token inválido o expirado: vuelve a pegar el token de Muselink."
            Status.CAPTURING -> "Capturando: el mic escucha la sala y agrega la sesión."
            Status.QUEUED -> if (queued > 0) "En cola: $queued chunk(s) esperando envío." else "En espera: sin música, nada que enviar."
            Status.OFFLINE -> "Sin conexión: la sesión se guarda local y se reintenta sola."
            Status.ERROR -> "Error recuperable: se reintenta solo; el audio no se toca."
        }
        _state.value = State(
            status = status,
            enabled = enabled,
            detail = detail,
            queued = queued,
            sentTotal = sentTotal,
            micReanchors = sensorReanchorsSeen + pipelineReanchorsSeen,
            micRoutedDevice = lastRoutedDevice,
            // T2/510: refreshState reconstruye el State; el perfil
            // aplicado se conserva (solo lo cambia recordAppliedProfile).
            appliedProfileId = _state.value.appliedProfileId,
            appliedProfileVersion = _state.value.appliedProfileVersion,
            abState = _state.value.abState,
            appliedProfileHash = _state.value.appliedProfileHash,
            // T1/510: el estado del perfil aplicado se conserva igual
            // (solo lo cambia el camino de la VUELTA).
            appliedProfileStatus = _state.value.appliedProfileStatus
        )
    }

    // ── T1/510 — la VUELTA: sondeo, aplicación, rollback, confirmación ──
    //
    // Todo en Dispatchers.IO (el pollJob y maybePollProfile corren ahí);
    // aplicar solo vía el puente registrado por MainActivity (H5). El
    // audio nunca se toca desde aquí: el puente es la única vía al DSP.

    /**
     * Decisión pura del sondeo (testeable en JVM): ¿toca pedir el perfil
     * vigente? El interruptor maestro en OFF apaga también el sondeo
     * (nada sale ni entra con el interruptor en OFF); sin token no hay
     * nada que pedir; y solo se pide si pasaron 30 min desde el último
     * sondeo que arrancó.
     */
    fun shouldPollProfile(
        enabled: Boolean,
        hasToken: Boolean,
        nowMs: Long,
        lastCheckMs: Long
    ): Boolean = enabled && hasToken && nowMs - lastCheckMs >= PROFILE_POLL_INTERVAL_MS

    /**
     * Decisión pura de aplicación (testeable en JVM): ¿se aplica la
     * versión del servidor? Solo si es MAYOR que la aplicada y el
     * `status` lo permite: `stable`/`promoted` siempre; `candidate` /
     * `ab_running` solo con un `abState` que permite probar
     * (`candidate` / `ab_running`). `rolled_back` NO se aplica por aquí:
     * lo gestiona [handleRollback]. Cualquier otro `status` es falso
     * (fail-closed).
     */
    fun shouldApplyServerVersion(
        status: String,
        abState: String,
        serverVersion: Long,
        appliedVersion: Long
    ): Boolean {
        if (serverVersion <= appliedVersion) return false
        return when (status) {
            "stable", "promoted" -> true
            "candidate", "ab_running" -> abState == "candidate" || abState == "ab_running"
            else -> false
        }
    }

    /**
     * Decisión pura del rollback (testeable en JVM): ¿hay un estable
     * anterior al que volver? Solo si el estable guardado existe y su
     * versión es ESTRICTAMENTE menor que la aplicada. Si el estable ES
     * lo aplicado (o no hay estable), no hay nada que restaurar: no se
     * re-aplica ni se pone nada en cuarentena.
     */
    fun shouldRestoreStable(appliedVersion: Long, stableVersion: Long?): Boolean =
        stableVersion != null && stableVersion < appliedVersion

    /** Versión aplicada actual, o null si aún no hay ninguna. */
    private fun appliedVersionOrNull(): Long? =
        _state.value.appliedProfileVersion.takeIf { it > 0L }

    /**
     * Disparador barato desde [flushOnce]: si toca sondear, lanza el
     * sondeo en el scope (el mutex de [pollProfileOnce] evita que dos
     * sondeos corran a la vez). Sin red aquí: solo un timestamp.
     */
    private fun maybePollProfile() {
        val s = scope ?: return
        val ctx = appContext ?: return
        if (!shouldPollProfile(
                enabled = isEnabled(ctx),
                hasToken = hasToken(),
                nowMs = System.currentTimeMillis(),
                lastCheckMs = lastProfileCheckMs
            )
        ) return
        s.launch { pollProfileOnce() }
    }

    /** Un ciclo de sondeo: GET → decidir → (validar → traducir → aplicar → confirmar). */
    private fun pollProfileOnce() {
        if (!profilePollMutex.tryLock()) return
        try {
            val ctx = appContext ?: return
            val fetcher = profileFetcher ?: return
            // Cinturón y tirantes: el job muere con stop(), pero un
            // lanzamiento residual no debe sondear con el interruptor OFF.
            if (!isEnabled(ctx)) return
            lastProfileCheckMs = System.currentTimeMillis()
            when (val res = runCatching { fetcher.fetch(appliedVersionOrNull()) }.getOrNull()) {
                null -> Log.w(TAG, "sondeo de perfil: fetch lanzó excepción; se reintenta en el próximo ciclo")
                is ProfileFetcher.Result.NoToken -> { /* sin token: nada que sondear */ }
                is ProfileFetcher.Result.TokenInvalid -> {
                    tokenInvalidSeen = true
                    refreshState()
                }
                is ProfileFetcher.Result.Retry ->
                    Log.i(TAG, "sondeo de perfil: reintentable (red/429/5xx); próximo ciclo")
                is ProfileFetcher.Result.Quarantine ->
                    Log.i(TAG, "sondeo de perfil: ${res.reason}; se conserva el último bueno")
                is ProfileFetcher.Result.NotModified -> { /* versión igual: nada que hacer */ }
                is ProfileFetcher.Result.Current -> handleCurrent(ctx, res)
            }
            retryPendingConfirmation()
        } finally {
            profilePollMutex.unlock()
        }
    }

    private fun handleCurrent(ctx: Context, current: ProfileFetcher.Result.Current) {
        val previousVersion = _state.value.appliedProfileVersion
        when (current.status) {
            "rolled_back" -> handleRollback(ctx, current, previousVersion)
            "stable", "promoted", "candidate", "ab_running" -> {
                if (!shouldApplyServerVersion(current.status, current.abState, current.version, previousVersion)) {
                    Log.i(TAG, "sondeo: v${current.version} ${current.status}/${current.abState} no se aplica (aplicada v$previousVersion)")
                    return
                }
                val isCandidate = current.status == "candidate" || current.status == "ab_running"
                applyServerProfile(
                    ctx = ctx,
                    current = current,
                    rolledBack = false,
                    // Un candidato se commitea SIN promover el estable: el
                    // estable sigue siendo la versión bendecida anterior.
                    promoteToStable = !isCandidate,
                    previousVersion = previousVersion
                )
            }
            else -> Log.w(TAG, "sondeo: status desconocido '${current.status}' (el fetcher lo valida; no debería pasar)")
        }
    }

    /**
     * Aplica un perfil del servidor por la vía completa: validar (C2) →
     * instalar con diario (C8) → confirmar por read-back → registrar →
     * confirmar al servidor. Devuelve true solo si quedó commiteado.
     */
    private fun applyServerProfile(
        ctx: Context,
        current: ProfileFetcher.Result.Current,
        rolledBack: Boolean,
        promoteToStable: Boolean,
        previousVersion: Long
    ): Boolean {
        val applier = profileApplier ?: return false
        val result = applier.applyAndCommit(
            body = current.profileBody,
            version = current.version,
            appliedVersion = previousVersion.takeIf { it > 0L },
            rolledBack = rolledBack,
            // La guarda C2.g (sin saltos de operador > 0.2) solo aplica
            // hacia adelante: un rollback restaura un perfil bendecido y
            // está exento (ya pasó la validación cuando se aplicó).
            previous = if (rolledBack) null else lastAppliedValidated,
            profileId = current.profileId,
            promoteToStable = promoteToStable,
            install = { validated -> installValidatedProfile(ctx, current, validated) },
            readAppliedProfileId = { onReadAppliedProfileId?.invoke() }
        )
        return when (result) {
            is ProfileApplier.CommitResult.Committed -> {
                lastAppliedValidated = result.profile
                val abState = when {
                    rolledBack -> "rolled_back"
                    current.status == "candidate" || current.status == "ab_running" -> "ab_running"
                    current.status == "promoted" -> "promoted"
                    else -> "none"
                }
                val profileStatus = when {
                    rolledBack -> PROFILE_STATUS_ROLLED_BACK
                    current.status == "candidate" || current.status == "ab_running" -> PROFILE_STATUS_AB_RUNNING
                    else -> PROFILE_STATUS_STABLE
                }
                val applied = recordAppliedProfile(
                    profileId = current.profileId,
                    version = current.version,
                    abState = abState,
                    profileHash = ProfileApplier.sha256Hex(current.profileBody),
                    profileStatus = profileStatus
                )
                confirmApplied(applied, previousVersion)
                refreshState()
                Log.i(TAG, "perfil v${current.version} ${current.status} aplicado y confirmado (abState=$abState)")
                true
            }
            is ProfileApplier.CommitResult.NotCommitted -> {
                Log.w(TAG, "perfil v${current.version} no confirmado: ${result.reason}; diario PENDING, se reintenta en el próximo sondeo")
                false
            }
            is ProfileApplier.CommitResult.Rejected -> {
                _state.value = _state.value.copy(appliedProfileStatus = PROFILE_STATUS_QUARANTINED)
                refreshState()
                Log.w(TAG, "perfil v${current.version} en cuarentena: ${result.validation.reasons}")
                false
            }
        }
    }

    /**
     * El hook `install` de [ProfileApplier.applyAndCommit]: traduce a
     * `IvannaAudioProfile`, instala en la lista (reemplaza o añade
     * `muselink-live`, conservando los perfiles de fábrica) con
     * `saveOverride()`, y aplica por el puente registrado por
     * MainActivity. Devuelve false si algo falta (fail-closed: el diario
     * queda PENDING y el estable intacto).
     */
    private fun installValidatedProfile(
        ctx: Context,
        current: ProfileFetcher.Result.Current,
        validated: ValidatedProfile
    ): Boolean {
        val apply = onApplyProfile ?: run {
            Log.w(TAG, "sondeo: onApplyProfile aún no registrado (MainActivity no compuso); se reintenta en el próximo sondeo")
            return false
        }
        val profiles = runCatching { ProfilesLoader.load(ctx) }.getOrNull() ?: run {
            Log.w(TAG, "sondeo: no se pudo cargar la lista de perfiles; no se instala nada")
            return false
        }
        val base = profiles.find { it.id == ProfileApplier.LIVE_PROFILE_ID }
            ?: profiles.find { it.id == runCatching { ParameterStore(ctx).getCurrentAudioProfileId() }.getOrNull() }
            ?: profiles.firstOrNull()
            ?: run {
                Log.w(TAG, "sondeo: lista de perfiles vacía; no hay base de la que heredar, no se instala nada")
                return false
            }
        val ivanna = ProfileApplier.translate(
            validated = validated,
            status = current.status,
            profileId = current.profileId,
            base = base
        )
        val updated = profiles.filter { it.id != ProfileApplier.LIVE_PROFILE_ID } + ivanna
        val meta = ProfilesLoader.loadMetadata(ctx) ?: run {
            Log.w(TAG, "sondeo: sin metadata de perfiles; no se instala nada")
            return false
        }
        ProfilesLoader.saveOverride(
            ctx,
            updated,
            meta.copy(
                totalProfiles = updated.size,
                lastModified = java.time.LocalDate.now().toString()
            )
        )
        return runCatching { apply(ivanna) }.getOrDefault(false)
    }

    /**
     * El servidor marcó `rolled_back`: restaurar el estable guardado y
     * confirmar. Si no hay estable verificado, o el estable ES lo
     * aplicado (no hay versión anterior), no se re-aplica nada: se marca
     * el estado, se confirma lo que hay aplicado para que el servidor
     * sepa que el teléfono vio el rollback, y el servidor debe
     * re-publicar el estable anterior como versión nueva (el teléfono no
     * puede aceptar una versión menor sin `rolled_back` y el sobre trae
     * una sola versión; ver reporte T1).
     */
    private fun handleRollback(
        ctx: Context,
        current: ProfileFetcher.Result.Current,
        previousVersion: Long
    ) {
        val applier = profileApplier ?: return
        val stableBody = applier.readStableBody()
        val journal = applier.readJournal()
        val stableVersion = journal?.stableVersion
        if (stableBody == null || stableVersion == null) {
            Log.w(TAG, "rollback del servidor (v${current.version}) sin estable verificado en disco; se conserva lo aplicado (v$previousVersion)")
            _state.value = _state.value.copy(appliedProfileStatus = PROFILE_STATUS_ROLLED_BACK)
            refreshState()
            confirmApplied(appliedProfile(), previousVersion)
            return
        }
        if (!shouldRestoreStable(previousVersion, stableVersion)) {
            Log.w(TAG, "rollback: el estable guardado es v$stableVersion y lo aplicado es v$previousVersion; no hay versión anterior que restaurar")
            _state.value = _state.value.copy(appliedProfileStatus = PROFILE_STATUS_ROLLED_BACK)
            refreshState()
            confirmApplied(appliedProfile(), previousVersion)
            return
        }
        // Re-aplicar el estable verificado (versión menor, rollback explícito).
        // profileId: el del diario (el del estable); si el diario es
        // anterior a T1 (sin profileId), el del sobre como fallback.
        val stableCurrent = ProfileFetcher.Result.Current(
            profileId = journal?.profileId?.ifBlank { current.profileId } ?: current.profileId,
            version = stableVersion,
            status = "rolled_back",
            abState = "rolled_back",
            profileBody = stableBody
        )
        val ok = applyServerProfile(
            ctx = ctx,
            current = stableCurrent,
            rolledBack = true,
            promoteToStable = true,
            previousVersion = previousVersion
        )
        if (!ok) {
            Log.w(TAG, "rollback: no se pudo re-aplicar el estable v$stableVersion; se conserva lo aplicado (v$previousVersion)")
        }
    }

    /**
     * Envía `POST /api/v1/muse/ivanna/profile/applied` con el cuerpo de
     * [OrganismPayload.appliedConfirmation]. Si es reintentable, la
     * confirmación queda pendiente (un POST por ciclo, en
     * [retryPendingConfirmation]); si no lo es, se descarta: lo aplicado
     * ya está registrado localmente y la telemetría T2 lo atribuye.
     * PENDIENTE: el endpoint aún no existe en el servidor (ver
     * [ProfileFetcher]); hasta entonces esto confirma contra un 404.
     */
    private fun confirmApplied(applied: OrganismPayload.AppliedProfile, previousVersion: Long) {
        val fetcher = profileFetcher ?: return
        val body = OrganismPayload.appliedConfirmation(
            applied = applied,
            previousVersion = previousVersion,
            engineVersion = com.ivanna.omega.BuildConfig.VERSION_NAME
        )
        when (runCatching { fetcher.confirmApplied(body) }.getOrNull()) {
            ProfileFetcher.ConfirmOutcome.Confirmed ->
                Log.i(TAG, "confirmación applied enviada: ${applied.id} v${applied.version}")
            ProfileFetcher.ConfirmOutcome.Retry, null -> {
                pendingConfirmation = body
                Log.i(TAG, "confirmación applied pendiente (reintentable); se reintenta en el próximo sondeo")
            }
            else ->
                Log.w(TAG, "confirmación applied no reintentable; se descarta (lo aplicado sigue registrado localmente)")
        }
    }

    /** Reintento de la confirmación pendiente (una por ciclo de sondeo). */
    private fun retryPendingConfirmation() {
        val pending = pendingConfirmation ?: return
        val fetcher = profileFetcher ?: return
        when (runCatching { fetcher.confirmApplied(pending) }.getOrNull()) {
            ProfileFetcher.ConfirmOutcome.Confirmed -> {
                pendingConfirmation = null
                Log.i(TAG, "confirmación applied pendiente enviada")
            }
            ProfileFetcher.ConfirmOutcome.Retry, null -> { /* sigue pendiente; próximo ciclo */ }
            else -> {
                pendingConfirmation = null
                Log.w(TAG, "confirmación applied pendiente descartada (no reintentable)")
            }
        }
    }
}
