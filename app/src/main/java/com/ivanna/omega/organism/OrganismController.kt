package com.ivanna.omega.organism

import android.content.Context
import android.media.AudioManager
import android.os.SystemClock
import android.util.Log
import com.ivanna.omega.audio.RealAcousticSensor
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
        val micRoutedDevice: String = "desconocido"
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
        val qDir = File(ctx.filesDir, "organism-queue")
        val q = OrganismQueue(qDir)
        val tokens = OrganismTokenStore(ctx)
        queue = q
        tokenStore = tokens
        sender = OrganismSender(tokens, q)
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
            scope?.launch(Dispatchers.IO) { flushOnce() }
            refreshState()
        }
        return ok
    }

    fun hasToken(): Boolean = tokenStore?.getToken()?.isNotBlank() == true

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

    private fun enqueueChunk(chunk: SessionAggregator.SessionChunk, q: OrganismQueue) {
        val body = OrganismPayload.sessionBody(chunk)
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
            micRoutedDevice = lastRoutedDevice
        )
    }
}
