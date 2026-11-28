package com.ivanna.omega.organism

import android.content.Context
import android.media.AudioManager
import android.os.SystemClock
import android.util.Log
import com.ivanna.omega.audio.IvannaAudioProfile
import com.ivanna.omega.audio.OmegaMetrics
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
 *   5. RETENCIÓN Y BORRADO (defecto #9; C22/510 para perfiles): la
 *      cola vive 7 días como máximo y el usuario puede borrarla entera
 *      desde la tarjeta (clearQueue). Borrar la cola borra TAMBIÉN los
 *      candidatos de perfil no estables (`muselink-profile-<n>.ivanna.json`),
 *      pero NUNCA el estable: el último perfil bueno solo se borra con
 *      borrado explícito ([deleteStableProfile]) y con el interruptor
 *      maestro en OFF, para que «borrar» nunca deje el teléfono sin
 *      perfil bueno. En disco solo viven estable + candidato vigente +
 *      diario (el resto se poda al instalar/promover). Los JSON de
 *      perfil no contienen secretos (el token nunca va en ellos), así
 *      que filesDir privado sin cifrar en reposo es aceptable,
 *      documentado igual que la cola.
 *   6. FOREGROUND SERVICE (v2.5.510, fix del «nada» de la 509): sin
 *      servicio en primer plano, al salir de la app Android congela
 *      el proceso y silencia el mic, así que el organismo no captura
 *      ni envía nada. start() arranca OrganismForegroundService
 *      (tipo microphone, notificación persistente) y stop() lo
 *      detiene al final. El servicio solo mantiene vivo el proceso;
 *      la captura y el envío siguen aquí.
 *   7. DIAGNÓSTICO DEL TOKEN (C21/510, no auto-renovación mágica):
 *      no hay endpoint de refresh y no se inventa uno. La tarjeta
 *      tiene un diagnóstico de un toque (hosts alcanzables,
 *      DNS directo OK/fallo, último 401 — ver
 *      [OrganismTokenDiagnostics]) y al pegar un token nuevo se
 *      re-valida con un GET barato de versión antes de reanudar el
 *      sondeo. La fecha del último 401 se registra (sin el valor del
 *      token, jamás) para distinguir «token muerto» de «red caída».
 *   8. WATCHDOG DE STALL DEL SENSOR (C23/510): el consumidor cuenta
 *      snapshotsAccepted/Rejected y detecta si frameIndex() deja de
 *      avanzar 10 s con música sonando (umbral PROVISIONAL, pendiente
 *      de medir la cadencia real en vivo — ver
 *      SensorSnapshotConsumer.STALL_THRESHOLD_MS): reintenta el anclaje
 *      del mic (restartMic: stop + start, mismo SessionAggregator/
 *      sessionId — la sesión NO se parte) y registra el stall. La salud
 *      del consumidor vive en el State (snapshotsAccepted/Rejected,
 *      sensorStalls, lastSensorStallAtMs) y la tarjeta la muestra
 *      cuando hay algo que ver (rechazos o stalls).
 *   9. CUARENTENA VERSIONADA (C24/510): el payload lleva versión
 *      (`telemetry.payloadVersion`, ver [OrganismPayload.
 *      PAYLOAD_VERSION]) y el sobre del GET de perfil puede anunciar
 *      `minSupportedPayloadVersion`. Si el servidor exige más que lo
 *      que envía este teléfono, la cola vieja se descarta
 *      EXPLÍCITAMENTE (con registro en el Log y en el State) en vez de
 *      pudrirse 7 días en cuarentena: un cambio de esquema del
 *      servidor ya no tira sesiones en silencio. El State expone
 *      además cuántos chunks hay en cuarentena y el último error
 *      (antes solo había un conteo total de cola). La señal del
 *      servidor vive en el endpoint de Muselink, no en este repo: hasta
 *      que la anuncie, `minSupportedPayloadVersion` es null y nada
 *      cambia (el descarte solo corre con señal explícita).
 *
 * ESTADO (el indicador de la UI):
 *   Capturando / En cola / Sin conexión / Error recuperable, más los
 *   honestos: Apagado / Sin token / Token inválido.
 *
 * LÍMITES HONESTOS (consejo, no se esconden):
 *   * Android exige RECORD_AUDIO (una vez, revocable) y la notificación
 *     persistente del foreground service para captura prolongada
 *     (v2.5.510: OrganismForegroundService, tipo microphone; sin
 *     RECORD_AUDIO no se arranca el servicio y el organismo corre sin
 *     primer plano, como en la 509).
 *   * Si el token expira sin endpoint de refresh, hay que reautorizar
 *     una vez (el estado lo dice: "Token inválido"). La tarjeta tiene
 *     el diagnóstico de un toque (C21) para que el re-pegado sea de un
 *     minuto: si algún host responde, la red vive y el token es el
 *     muerto; si ninguno responde, es la red.
 */
object OrganismController {
    private const val TAG = "OrganismController"
    private const val PREFS = "organism_prefs"
    private const val KEY_ENABLED = "organism_enabled"
    /** C21/510: fecha (epoch ms) del último 401 con el token actual (0 = ninguno). */
    private const val KEY_LAST_401_MS = "organism_last_401_ms"

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

    // ── S13/510 — presupuesto por hop y contador de xruns ────────────────
    //
    // 1 hop = 128 muestras @ 48 kHz = 2.6667 ms. Una aplicación automática
    // que tarde más de 1 hop en el hilo que la ejecuta, o que coincida con
    // un xrun/dropout, marca el perfil «aplicado con defecto» y la puerta
    // A/B lo trata como fallo (rollback), no como dato — ver
    // [evaluateApplyDefect] y el enganche T4 en [recordApplyMeasurement].
    // La medición es solo telemetría: ningún trabajo nuevo en el hilo de
    // audio (el apply corre en Dispatchers.IO, ver (c) abajo) y ningún
    // recálculo por hop (los coeficientes EQ se fijan una vez al aplicar,
    // verificado en código vivo — nota (d) al final de este bloque).
    //
    // (a) CONTADOR DE XRUNS — DISCREPANCIA HONESTA (código vivo manda):
    // el contador del motor SÍ existe en nativo — `IvannaFusionState.
    // xrun_count` (app/src/main/cpp/include/ivanna_fusion_state.h:152,
    // músculo → cerebro, lo escribe el HAL stage) — pero NO hay ningún
    // lector Kotlin/JNI que lo exponga a la app: ningún `.kt` lee el
    // snapshot del daemon ni `xrun_count` (grep: 0 hits). Cablear ese
    // lector es trabajo del motor/JNI, no de S13, así que NO se crea aquí
    // (la especificación lo prohíbe expresamente) y queda como pendiente
    // del motor. Lo que SÍ se puede leer hoy desde la app es el contador
    // de underruns del AudioTrack propio, publicado en
    // `OmegaMetrics.shared.value.underrunCount` (IvannaBridgePlayer lo
    // refresca a ~1 Hz; la captura publica los suyos por la misma vía).
    // Es un contador DISTINTO del xrun_count del daemon (underruns del
    // reproductor/captura de la app, no del motor), y su refresco a ~1 Hz
    // hace gruesa la detección de coincidencia: un delta > 0 entre el
    // antes y el después de la aplicación es evidencia suficiente de un
    // dropout alrededor de la aplicación, pero un delta == 0 NO prueba
    // que no hubo xrun (el refresco puede no haber caído aún en la
    // ventana de ~ms). Por eso la fuente queda registrada en el estado
    // (`lastApplyXrunSource`): cuando el motor exponga `xrun_count`, el
    // lector se cambia por él sin tocar la guarda.
    // (b) GUARDA: [evaluateApplyDefect] (pura, testeable en JVM).
    // (c) LA APLICACIÓN NUNCA CORRE EN EL HILO DE AUDIO NI EN LA UI:
    // el camino automático de T1 (pollJob en `scope` =
    // CoroutineScope(Dispatchers.IO + SupervisorJob()) →
    // pollProfileOnce → applyServerProfile → applyAndCommit → install →
    // onApplyProfile → ProfileManagerBridge.applyProfile → applyToDsp →
    // pushToNative) es síncrono en el hilo del pollJob: ni
    // ProfileManagerBridge ni ProfileManager cambian de dispatcher
    // (grep: 0 `withContext`/`launch`/`Dispatchers` en ambos) y el
    // puente se invoca síncrono desde el callback `install`. La vía
    // MANUAL desde la UI (MainActivity:544,663,672) sigue corriendo en el
    // hilo que la llama (Main): es acción del usuario, no aplicación
    // automática — fuera del alcance de S13. Single-flight: el sondeo ya
    // se serializaba con `profilePollMutex`, pero eso serializa SONDEOS,
    // no aplicaciones; S13 añade [ApplySingleFlight] dedicado alrededor
    // de [applyServerProfile]: la segunda aplicación concurrente se
    // RECHAZA (tryLock, fail-closed) y se reintenta en el próximo sondeo
    // — no espera, para no bloquear el ciclo de sondeo.
    // (d) SIN RECÁLCULO POR HOP — VERIFICADO EN CÓDIGO VIVO, SIN DEFECTO:
    // `FusedTonal.setControls` → `applyControls()` → `RoomCorrectionEq.
    // setTarget(gains)` calcula las 10 ganancias UNA vez por aplicación;
    // `FusedChain.process()` sí llama a `tonal_.setControls(eff.tonal)`
    // en cada bloque, pero `setTarget` solo copia 10 floats con clamp
    // (sin diseñar biquads). Los coeficientes biquad se rediseñan
    // (`eq_.setBand`) solo dentro de `RoomCorrectionEq.process()` y solo
    // mientras `cur_` está en slew hacia `tgt_` (≤0.25 dB/bloque,
    // `kSlewDbPerBlock`), y se detiene al converger. No hay recálculo por
    // hop a causa del perfil: la publicación es por intercambio de
    // parámetros (`setControls` con los controles efectivos) y el slew
    // acotado es el diseño existente (S12), no un defecto. Nada que
    // corregir en S13.
    /** Presupuesto de 1 hop en µs (128 muestras @ 48 kHz = 2.6667 ms). */
    const val APPLY_HOP_BUDGET_US = 2_667L

    /** Fuente del contador muestreado alrededor de la aplicación. */
    const val XRUN_SOURCE_NONE = "none"
    /** AudioTrack.underrunCount vía OmegaMetrics (refresco ~1 Hz). */
    const val XRUN_SOURCE_AUDIOTRACK = "audiotrack_underrun"
    /**
     * xrun_count del daemon (IvannaFusionState): SIN lector Kotlin/JNI
     * — pendiente del motor (ver nota (a) arriba). No usar hasta que
     * exista el lector.
     */
    const val XRUN_SOURCE_DAEMON = "daemon_xrun_count"

    /** Motivos de `lastApplyDefectReason` ("" = sin defecto). */
    const val APPLY_DEFECT_NONE = ""
    const val APPLY_DEFECT_OVER_BUDGET = "over_budget"
    const val APPLY_DEFECT_XRUN = "xrun"
    const val APPLY_DEFECT_OVER_BUDGET_AND_XRUN = "over_budget+xrun"
    /** El intento no llegó a aplicar (install false / read-back fallido). */
    const val APPLY_DEFECT_NOT_APPLIED = "not_applied"

    /**
     * S13 — veredicto puro de la guarda de aplicación (testeable en JVM).
     * `defective` solo si el perfil QUEDÓ aplicado ([applied] true) y
     * además tardó más de 1 hop o coincidió con un xrun. Un intento que
     * no aplicó no es «aplicado con defecto»: ya lo cubre el camino
     * NotCommitted (diario PENDING, reintento en el próximo sondeo).
     */
    data class ApplyDefectVerdict(
        val defective: Boolean,
        val overBudget: Boolean,
        val xrunCoincided: Boolean,
        val reason: String
    )

    /** Medición de UNA aplicación automática (la registra [recordApplyMeasurement]). */
    data class ApplyMeasurement(
        val version: Long,
        val durationUs: Long,
        val xrunsBefore: Int?,
        val xrunsAfter: Int?,
        val xrunSource: String,
        val applied: Boolean,
        val atMs: Long
    )

    /**
     * S13 — guarda pura (testeable en JVM): ¿la aplicación quedó
     * «aplicada con defecto»? `xrunDelta` null = sin contador observable
     * (la pata de xrun no dispara; la de presupuesto sí).
     */
    fun evaluateApplyDefect(
        durationUs: Long,
        xrunDelta: Int?,
        applied: Boolean
    ): ApplyDefectVerdict {
        if (!applied) {
            return ApplyDefectVerdict(
                defective = false,
                overBudget = false,
                xrunCoincided = false,
                reason = APPLY_DEFECT_NOT_APPLIED
            )
        }
        val overBudget = durationUs > APPLY_HOP_BUDGET_US
        val xrunCoincided = (xrunDelta ?: 0) > 0
        val reason = when {
            overBudget && xrunCoincided -> APPLY_DEFECT_OVER_BUDGET_AND_XRUN
            overBudget -> APPLY_DEFECT_OVER_BUDGET
            xrunCoincided -> APPLY_DEFECT_XRUN
            else -> APPLY_DEFECT_NONE
        }
        return ApplyDefectVerdict(
            defective = overBudget || xrunCoincided,
            overBudget = overBudget,
            xrunCoincided = xrunCoincided,
            reason = reason
        )
    }

    /**
     * S13 — delta del contador entre el antes y el después de aplicar.
     * null si no hay contador observable. Un delta negativo es un reset
     * del contador (el AudioTrack se recrea y `underrunCount` vuelve a 0):
     * se clampa a 0, no es un dropout.
     */
    fun xrunDelta(before: Int?, after: Int?): Int? {
        if (before == null || after == null) return null
        return (after - before).coerceAtLeast(0)
    }

    /**
     * S13 — transición pura del [State] con una medición de aplicación
     * (testeable en JVM). El veredicto pertenece a [ApplyMeasurement.
     * version]: T4/Fase 3 debe comprobar que coincide con
     * `appliedProfileVersion` antes de usar `lastApplyDefective` (un
     * veredicto de una versión anterior no condena a la vigente).
     */
    fun applyMeasurementToState(state: State, m: ApplyMeasurement): State {
        val delta = xrunDelta(m.xrunsBefore, m.xrunsAfter)
        val verdict = evaluateApplyDefect(m.durationUs, delta, m.applied)
        return state.copy(
            lastApplyVersion = m.version,
            lastApplyDurationUs = m.durationUs,
            lastApplyAtMs = m.atMs,
            lastApplyXrunsBefore = m.xrunsBefore,
            lastApplyXrunsAfter = m.xrunsAfter,
            lastApplyXrunDelta = delta,
            lastApplyXrunSource = m.xrunSource,
            lastApplyOverBudget = verdict.overBudget,
            lastApplyDefective = verdict.defective,
            lastApplyDefectReason = verdict.reason
        )
    }

    /**
     * S13 — single-flight de aplicación (dos aplicaciones concurrentes
     * prohibidas). tryLock = la segunda se RECHAZA, no espera (el
     * reintento llega en el próximo sondeo). Solo el poseedor libera.
     * Puro JVM: [Mutex.tryLock]/[Mutex.unlock] no suspenden.
     */
    class ApplySingleFlight {
        private val mutex = Mutex()
        /** true si esta llamada obtuvo el turno; false si otra aplicación está en curso. */
        fun tryAcquire(): Boolean = mutex.tryLock()
        /** Libera el turno; sin efecto si nadie lo tiene. */
        fun release() {
            if (mutex.isLocked) mutex.unlock()
        }
        val isLocked: Boolean get() = mutex.isLocked
    }

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
        val appliedProfileStatus: String = PROFILE_STATUS_NONE,
        // ── S13/510 — telemetría de la última aplicación automática ──
        /** Versión a la que pertenece esta medición (null = aún no hay ninguna). */
        val lastApplyVersion: Long? = null,
        /** Tiempo de `applyProfile` + `pushToNative()` en µs (Dispatchers.IO). Null = aún no hay medición. */
        val lastApplyDurationUs: Long? = null,
        /** Cuándo se midió (System.currentTimeMillis). 0 = aún no hay medición. */
        val lastApplyAtMs: Long = 0L,
        /** Contador de dropouts antes / después de aplicar (null = sin contador observable). */
        val lastApplyXrunsBefore: Int? = null,
        val lastApplyXrunsAfter: Int? = null,
        /** Delta after − before (null = sin contador; 0 = sin dropout observado). */
        val lastApplyXrunDelta: Int? = null,
        /** Qué contador se muestreó: [XRUN_SOURCE_AUDIOTRACK] hoy; [XRUN_SOURCE_DAEMON] cuando el motor lo exponga. */
        val lastApplyXrunSource: String = XRUN_SOURCE_NONE,
        /** true si la aplicación tardó más de 1 hop (2.667 ms). */
        val lastApplyOverBudget: Boolean = false,
        /**
         * S13 — «aplicado con defecto»: la aplicación tardó más de 1 hop
         * o coincidió con un xrun. ENGANCHE T4/Fase 3: la puerta A/B lee
         * este flag (junto a [lastApplyVersion] == `appliedProfileVersion`
         * y [lastApplyDefectReason]) y trata ese perfil como FALLO
         * (rollback), no como dato. S13 solo marca; T4 implementa el
         * tratamiento (la confirmación `POST /profile/applied` vive en
         * OrganismPayload, fuera de S13).
         */
        val lastApplyDefective: Boolean = false,
        /** Motivo del defecto: "" | "over_budget" | "xrun" | "over_budget+xrun" | "not_applied". */
        val lastApplyDefectReason: String = APPLY_DEFECT_NONE,
        // ── C21/510 — diagnóstico de re-auth del token ───────────────
        /**
         * Fecha (epoch ms) del último 401 con el token actual; 0 =
         * nunca hubo uno (o el token actual ya re-validó y lo limpió).
         * Solo la fecha, jamás el valor del token. Persiste en
         * SharedPreferences (sobrevive a reinicios) para distinguir
         * «token muerto» de «red caída» aunque la app se cierre.
         */
        val last401AtMs: Long = 0L,
        /** Resultado del último diagnóstico de un toque (null = aún no se pidió). */
        val lastTokenDiagnosis: OrganismTokenDiagnostics.Diagnosis? = null,
        /** true mientras corre el diagnóstico de un toque. */
        val tokenDiagnosing: Boolean = false,
        // ── C23/510 — salud del consumidor de snapshots (watchdog de stall) ──
        /**
         * Snapshots aceptados por la JNI (canal latest-wins) desde el
         * arranque del consumidor. Crece ~1 por frame (~85 ms con
         * música); es el contador de «el sensor vive».
         */
        val snapshotsAccepted: Long = 0L,
        /**
         * Snapshots rechazados por la JNI (no tocan el estado ni el
         * agregador). > 0 con música = el canal lateral tiene un
         * problema que mirar.
         */
        val snapshotsRejected: Long = 0L,
        /**
         * Stalls detectados por el watchdog: frameIndex() sin avanzar
         * >= 10 s con música sonando. Cada stall reintenta el anclaje
         * del mic (ver [RealAcousticSensor.restartMic]) y queda
         * contado aquí; sin música no hay stall.
         */
        val sensorStalls: Long = 0L,
        /** Fecha (epoch ms) del último stall; 0 = ninguno. */
        val lastSensorStallAtMs: Long = 0L,
        // ── C24/510 — cuarentena versionada ──
        /**
         * Chunks en QUARANTINED (4xx no reintentable o max-intentos).
         * Antes el estado solo mostraba el conteo total de la cola:
         * una cuarentena que crece en silencio era invisible.
         */
        val quarantined: Int = 0,
        /**
         * `lastError` del chunk en cuarentena más reciente (ver
         * [OrganismQueue.lastQuarantinedError]). null = nada en
         * cuarentena. Es el motivo visible, sin el cuerpo del chunk.
         */
        val lastQuarantineError: String? = null,
        /**
         * Mínima versión de payload que el servidor acepta, según el
         * último sobre del GET de perfil. null = el servidor aún no
         * anuncia ninguna (nada que descartar).
         */
        val minSupportedPayloadVersion: Long? = null,
        /**
         * true si el servidor exige un payload más nuevo que el que
         * envía este teléfono ([OrganismPayload.PAYLOAD_VERSION]).
         * Se actualiza en cada sondeo con la señal fresca (un sobre
         * sin señal lo limpia: el servidor la anuncia en cada GET).
         */
        val payloadObsolete: Boolean = false,
        /**
         * Chunks obsoletos descartados explícitamente por C24 en este
         * proceso (acumulado). 0 = aún no hubo descarte por
         * obsolescencia (el prune de retención de 7 días no cuenta
         * aquí: ese borra por edad, no por esquema).
         */
        val obsoleteChunksDiscarded: Int = 0,
        /** Fecha (epoch ms) del último descarte por obsolescencia; 0 = ninguno. */
        val lastPayloadDiscardAtMs: Long = 0L
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

    /** S13 — single-flight de aplicación (dos aplicaciones concurrentes prohibidas; la segunda se rechaza). */
    private val applySingleFlight = ApplySingleFlight()

    /**
     * S13 — lector del contador de dropouts observable desde la app.
     * Por defecto lee `OmegaMetrics.shared.value.underrunCount`
     * (AudioTrack.underrunCount del bridge/captura, refresco ~1 Hz;
     * null si no se puede leer). Es `var` para que los tests y el
     * futuro lector del daemon (`IvannaFusionState.xrun_count`, pendiente
     * del motor) lo sustituyan sin tocar la guarda. La fuente muestreada
     * queda en `State.lastApplyXrunSource`.
     */
    @Volatile
    var xrunCounterReader: (() -> Int?)? = {
        runCatching { OmegaMetrics.shared.value.underrunCount }.getOrNull()
    }

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
        // C21 (e): la fecha del último 401 sobrevive a reinicios.
        last401AtMs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .getLong(KEY_LAST_401_MS, 0L)
        if (!isEnabled(ctx)) {
            _state.value = State(status = Status.OFF, enabled = false)
            return
        }
        // 510 — FOREGROUND SERVICE (fix del «nada» de la 509): sin
        // servicio en primer plano, al salir de la app Android congela
        // el proceso (do_freezer_trap) y silencia el mic, así que el
        // organismo no captura ni envía nada. El servicio solo mantiene
        // vivo el proceso; la captura y el envío siguen en este
        // controlador. Va ANTES del early-return de scope != null para
        // re-asegurar el servicio si murió con el controlador vivo.
        // Idempotente y con runCatching dentro (si el arranque desde
        // segundo plano está restringido — p. ej. restore en boot — se
        // registra y se reintenta en la próxima apertura; sin crash).
        OrganismForegroundService.start(ctx)
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
            },
            // C23/510 — watchdog de stall: el consumidor dispara
            // handleSensorStall() en su hilo; la recuperación corre en
            // Dispatchers.IO (no bloquea el hilo del consumidor).
            onStall = { handleSensorStall() }
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
        // 510 — el servicio es hijo del interruptor maestro: solo vive
        // mientras el organismo está enabled. Se detiene AL FINAL de
        // stop(), para que el proceso siga en primer plano mientras se
        // cierra la sesión y el chunk final queda durable en la cola.
        appContext?.let { OrganismForegroundService.stop(it) }
    }

    /**
     * Borra toda la cola local (defecto #9). C22/510: borra TAMBIÉN los
     * candidatos de perfil no estables (`muselink-profile-<n>.ivanna.json`);
     * el estable, su sidecar, el diario y la cuarentena se conservan, así
     * que «borrar» nunca deja el teléfono sin perfil bueno. El estable
     * solo se borra con [deleteStableProfile] (maestro en OFF + explícito).
     * Funciona aunque el controlador esté detenido (sin cola ni applier
     * vivos): usa un [ProfileApplier] desechable sobre filesDir. Devuelve
     * cuántos chunks de cola se borraron (los candidatos van al Log).
     */
    fun clearQueue(): Int {
        val deleted = queue?.clearAll() ?: 0
        val candidatesDeleted = profileApplier?.deleteCandidates()
            ?: appContext?.let { ProfileApplier(it.filesDir).deleteCandidates() }
            ?: 0
        if (candidatesDeleted > 0) {
            Log.i(TAG, "C22: clearQueue borró $candidatesDeleted candidato(s) no estables (el estable se conserva)")
        }
        refreshState()
        return deleted
    }

    /**
     * C22/510 — borrado EXPLÍCITO del perfil estable (archivo + sidecar
     * `.sha256` + diario). Solo con el interruptor maestro en OFF: con el
     * organismo encendido se rechaza (fail-closed; sin contexto no se puede
     * verificar el interruptor y también se rechaza). Sin estable que
     * borrar devuelve false. La versión aplicada en memoria no se toca
     * (solo vive en memoria y vuelve a neutros tras reiniciar); tras
     * reiniciar, el sondeo vuelve a descargar el perfil vigente porque
     * ya no hay versión aplicada que comparar.
     */
    fun deleteStableProfile(): Boolean {
        val ctx = appContext ?: return false
        if (isEnabled(ctx)) {
            Log.w(TAG, "C22: borrar el estable exige el interruptor maestro en OFF")
            return false
        }
        val deleted = ProfileApplier(ctx.filesDir).deleteStable()
        if (deleted) Log.i(TAG, "C22: perfil estable borrado (archivo + sidecar + diario)")
        refreshState()
        return deleted
    }

    /**
     * Guarda el token (lo pega Esse una vez, en la tarjeta del organismo).
     *
     * C21 (d): guardar NO es validar. El token nuevo se re-valida con
     * un GET barato de versión ([validateSavedToken]) ANTES de reanudar
     * el sondeo: solo si el servidor lo acepta se limpia el estado de
     * token inválido, se limpia el último 401 y se fuerza el sondeo +
     * el vaciado de la cola. Si el GET dice 401, el token pegado es el
     * incorrecto y el estado sigue en «Token inválido» (con la fecha
     * del 401 registrada); si la red está caída, no se afirma nada y el
     * sondeo normal lo reintentará. El token SOLO entra aquí, pegado
     * por Esse: ningún endpoint puede entregar un token al teléfono
     * (C21 (c)) y ningún camino de red llama a `setToken()`.
     */
    fun saveToken(raw: String): Boolean {
        val store = tokenStore ?: return false
        val ok = store.setToken(raw)
        if (ok) {
            // La validación decide el estado: no se limpia
            // tokenInvalidSeen ni se fuerza el sondeo hasta que el
            // servidor acepte el token (ver validateSavedToken).
            // Sin scope (controlador no arrancado), el arranque normal
            // sondea a los 60 s y el estado se corrige ahí.
            scope?.launch(Dispatchers.IO) { validateSavedToken() }
            refreshState()
        }
        return ok
    }

    /**
     * C21 (d) — re-validación del token recién pegado con el mismo GET
     * del sondeo (`fetch(appliedVersionOrNull())`): es la comprobación
     * de versión barata — si la versión es la aplicada, el servidor
     * responde NotModified sin descargar el perfil; si hay versión
     * nueva, el sobre llega pero NO se aplica aquí (eso lo hace el
     * sondeo normal). Solo valida, no aplica ni confirma nada.
     */
    private fun validateSavedToken() {
        val fetcher = profileFetcher ?: return
        val result = runCatching { fetcher.fetch(appliedVersionOrNull()) }.getOrNull()
        when (classifyTokenValidation(result)) {
            TokenValidation.VALID -> {
                tokenInvalidSeen = false
                clear401()
                // Token aceptado: reanudar el sondeo enseguida y vaciar
                // la cola acumulada (el flushOnce además sondea).
                lastProfileCheckMs = 0L
                flushOnce()
            }
            TokenValidation.INVALID -> {
                tokenInvalidSeen = true
                record401()
                Log.w(TAG, "C21: el token pegado no valida (401); sigue «Token inválido»")
            }
            TokenValidation.UNKNOWN -> {
                // Red caída / excepción: no se pudo validar. No se
                // afirma ni válido ni inválido; el sondeo normal (30
                // min) lo reintentará. El estado previo se conserva.
                Log.i(TAG, "C21: no se pudo re-validar el token (red); se conserva el estado")
            }
        }
        refreshState()
    }

    /**
     * C21 (a) — diagnóstico de un toque (lo dispara el botón de la
     * tarjeta). Corre en Dispatchers.IO, no bloquea la UI, y no envía
     * el token. Anti doble toque: si ya corre uno, no se lanza otro.
     * El resultado queda en `State.lastTokenDiagnosis` y la tarjeta lo
     * muestra; sin scope (controlador no arrancado) no hace nada.
     */
    fun runTokenDiagnosis() {
        val s = scope ?: return
        if (tokenDiagnosing) return
        tokenDiagnosing = true
        refreshState()
        s.launch(Dispatchers.IO) {
            val diag = runCatching {
                (tokenDiagnostics ?: OrganismTokenDiagnostics()).diagnose(last401AtMs)
            }.getOrNull()
            if (diag != null) {
                lastTokenDiagnosis = diag
                Log.i(TAG, "C21 diagnóstico: ${diag.hosts.size} hosts, alcanzable=${diag.anyReachable}, DNS OK=${diag.anyDnsOk}")
            } else {
                Log.w(TAG, "C21 diagnóstico: falló sin resultado")
            }
            tokenDiagnosing = false
            refreshState()
        }
    }

    /**
     * C23/510 — recuperación ante un stall del sensor (la dispara el
     * watchdog del consumidor en el hilo IvannaSensorSnapshot).
     * Reinicia el anclaje del mic ([RealAcousticSensor.restartMic]:
     * stop + start) en Dispatchers.IO, sin bloquear el hilo del
     * consumidor. El mismo SessionAggregator/sessionId sigue vivo: la
     * sesión NO se parte (el agregador solo ve un hueco de frames) y
     * el reinicio con éxito cuenta como UN re-ancla en
     * `State.micReanchors`. Sin scope o sin consumidor (controlador
     * deteniéndose) no se hace nada: el stall ya quedó contado en el
     * consumidor y el sensor lo gestiona su ciclo de vida.
     *
     * Anti-solapamiento sin flag: el consumidor no vuelve a disparar
     * hasta pasados otros 10 s (su cooldown) y un reinicio tarda ~1 s
     * como mucho, así que dos recuperaciones no se pisan; si el mic no
     * vuelve, hay un intento cada 10 s, no una tormenta. No hay flag
     * que pueda quedarse atascado si stop() cae en medio.
     */
    private fun handleSensorStall() {
        val s = scope ?: return
        if (consumer == null) return
        s.launch(Dispatchers.IO) {
            val ctx = appContext
            val ok = ctx != null &&
                runCatching { RealAcousticSensor.restartMic(ctx) }.getOrDefault(false)
            if (ok) {
                Log.i(TAG, "C23: stall del sensor: mic re-anclado (misma sesión ${aggregator?.sessionId ?: "?"}); contadores en State")
            } else {
                Log.w(TAG, "C23: stall del sensor: no se pudo reiniciar el mic; el próximo stall lo reintentará")
            }
            refreshState()
        }
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

    /**
     * S13 — registra la medición de una aplicación automática en el
     * [State] (vía la transición pura [applyMeasurementToState]) y
     * devuelve el veredicto de la guarda. Lo llama el camino de
     * aplicación de T1 ([installValidatedProfile]) tras cada intento de
     * `applyProfile` + `pushToNative()`, aplicado o no. Si el veredicto
     * es defectuoso, el perfil queda marcado «aplicado con defecto» y
     * la puerta A/B lo trata como fallo: ENGANCHE T4/Fase 3 — leer
     * `state.lastApplyDefective` con `lastApplyVersion ==
     * appliedProfileVersion` (ver KDoc de `State.lastApplyDefective`).
     * No hace rollback aquí: el rollback lo decide la puerta A/B (T4).
     */
    @Synchronized
    fun recordApplyMeasurement(m: ApplyMeasurement): ApplyDefectVerdict {
        _state.value = applyMeasurementToState(_state.value, m)
        // El veredicto devuelto es el mismo que quedó en el State (sin
        // recalcular: se deriva de los campos recién escritos).
        val s = _state.value
        val verdict = ApplyDefectVerdict(
            defective = s.lastApplyDefective,
            overBudget = s.lastApplyOverBudget,
            xrunCoincided = (s.lastApplyXrunDelta ?: 0) > 0,
            reason = s.lastApplyDefectReason
        )
        if (verdict.defective) {
            Log.w(TAG, "S13: perfil v${m.version} aplicado con defecto (${verdict.reason}; ${m.durationUs} µs, xrunDelta=${s.lastApplyXrunDelta}); la puerta A/B debe tratarlo como fallo")
        } else {
            Log.i(TAG, "S13: aplicación v${m.version} medida: ${m.durationUs} µs (presupuesto 1 hop = $APPLY_HOP_BUDGET_US µs), xruns ${m.xrunsBefore}→${m.xrunsAfter} (${m.xrunSource})")
        }
        return verdict
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

    // ── C21/510 — diagnóstico de re-auth del token ───────────────────
    /**
     * Fecha (epoch ms) del último 401 con el token actual (0 = ninguno).
     * Se persiste en [PREFS]/[KEY_LAST_401_MS] (solo la fecha, jamás el
     * valor del token) y se carga en start(). Se limpia cuando un token
     * nuevo re-valida: el 401 pertenecía al token viejo.
     */
    @Volatile
    private var last401AtMs: Long = 0L

    /** Resultado del último diagnóstico de un toque (null = aún no se pidió). */
    @Volatile
    var lastTokenDiagnosis: OrganismTokenDiagnostics.Diagnosis? = null
        private set

    /** true mientras corre el diagnóstico de un toque (anti doble toque). */
    @Volatile
    private var tokenDiagnosing: Boolean = false

    /**
     * Diagnóstico inyectable (tests y sustitución futura). Null = se
     * crea el real ([OrganismTokenDiagnostics]) al pedirlo.
     */
    @Volatile
    var tokenDiagnostics: OrganismTokenDiagnostics? = null

    // ── C24/510 — cuarentena versionada ──────────────────────────────
    /** Chunks obsoletos descartados explícitamente en este proceso (acumulado; ver State). */
    @Volatile
    private var obsoleteChunksDiscarded: Int = 0

    /** Fecha (epoch ms) del último descarte por payload obsoleto; 0 = ninguno. */
    @Volatile
    private var lastPayloadDiscardAtMs: Long = 0L

    /**
     * C21 — veredicto puro de la re-validación de un token recién
     * pegado (testeable en JVM). El GET barato de versión es el mismo
     * `fetch()` del sondeo: si el servidor responde 2xx (perfil nuevo
     * o sobre malformado — la auth pasó) o 404 (aún no hay perfil
     * compilado — la auth también pasó), el token ES válido. 401 =
     * inválido. Red caída / timeout / sin token / excepción = no se
     * pudo validar: no se afirma ni válido ni inválido.
     */
    enum class TokenValidation { VALID, INVALID, UNKNOWN }

    fun classifyTokenValidation(result: ProfileFetcher.Result?): TokenValidation =
        when (result) {
            is ProfileFetcher.Result.Current,
            // C24/510: NotModified dejó de ser `data object` (lleva la
            // señal minSupportedPayloadVersion), así que va con `is`.
            is ProfileFetcher.Result.NotModified,
            is ProfileFetcher.Result.Quarantine -> TokenValidation.VALID
            ProfileFetcher.Result.TokenInvalid -> TokenValidation.INVALID
            else -> TokenValidation.UNKNOWN
        }

    /**
     * C21 (e) — registra la fecha del último 401 (sin el valor del
     * token, jamás) y la persiste. La llama cualquier camino que vea
     * un 401 (flush, sondeo, confirmación). No toca `tokenInvalidSeen`:
     * cada llamador decide el estado visible.
     */
    private fun record401(nowMs: Long = System.currentTimeMillis()) {
        last401AtMs = nowMs
        appContext?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            ?.edit()?.putLong(KEY_LAST_401_MS, nowMs)?.apply()
    }

    /** C21 (e) — el token actual re-validó: el 401 pertenecía al viejo. */
    private fun clear401() {
        last401AtMs = 0L
        appContext?.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            ?.edit()?.remove(KEY_LAST_401_MS)?.apply()
    }

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
        // C21 (e): un 401 registra su fecha (sin el valor del token).
        if (result.outcome == OrganismSender.Outcome.TOKEN_INVALID) {
            tokenInvalidSeen = true
            record401()
        } else tokenInvalidSeen = false
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
        // C24/510: los conteos se leen UNA vez (la cola lee el disco):
        // el total para `queued` y el de cuarentena para el State.
        val counts = queue?.counts()
        val queued = counts?.values?.sum() ?: 0
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
            appliedProfileStatus = _state.value.appliedProfileStatus,
            // S13/510: la telemetría de la última aplicación se conserva
            // igual (solo la cambia recordApplyMeasurement).
            lastApplyVersion = _state.value.lastApplyVersion,
            lastApplyDurationUs = _state.value.lastApplyDurationUs,
            lastApplyAtMs = _state.value.lastApplyAtMs,
            lastApplyXrunsBefore = _state.value.lastApplyXrunsBefore,
            lastApplyXrunsAfter = _state.value.lastApplyXrunsAfter,
            lastApplyXrunDelta = _state.value.lastApplyXrunDelta,
            lastApplyXrunSource = _state.value.lastApplyXrunSource,
            lastApplyOverBudget = _state.value.lastApplyOverBudget,
            lastApplyDefective = _state.value.lastApplyDefective,
            lastApplyDefectReason = _state.value.lastApplyDefectReason,
            // C21/510: el diagnóstico del token se conserva igual (solo
            // lo cambian record401/clear401/runTokenDiagnosis).
            last401AtMs = last401AtMs,
            lastTokenDiagnosis = lastTokenDiagnosis,
            tokenDiagnosing = tokenDiagnosing,
            // C23/510: la salud del consumidor se lee del consumidor
            // vivo; sin consumidor (controlador detenido) se conserva
            // el último valor conocido, como el perfil aplicado.
            snapshotsAccepted = consumer?.snapshotsAccepted ?: _state.value.snapshotsAccepted,
            snapshotsRejected = consumer?.snapshotsRejected ?: _state.value.snapshotsRejected,
            sensorStalls = consumer?.stalls ?: _state.value.sensorStalls,
            lastSensorStallAtMs = consumer?.lastStallAtMs ?: _state.value.lastSensorStallAtMs,
            // C24/510: la cuarentena se lee de la cola viva; sin cola
            // (controlador detenido) se conserva el último valor
            // conocido. El error es el del chunk en cuarentena más
            // reciente; con la cola vacía de cuarentena vuelve a null
            // (no se conserva un error de chunks ya borrados).
            quarantined = counts?.getValue(OrganismQueue.State.QUARANTINED) ?: _state.value.quarantined,
            lastQuarantineError = queue?.lastQuarantinedError()
                ?: if (queue == null) _state.value.lastQuarantineError else null,
            // C24/510: la señal del servidor solo la cambia el sondeo
            // (handlePayloadVersionSignal); aquí se conserva. El
            // acumulado de descartes vive en los vars volátiles, como
            // sentTotal/last401AtMs.
            minSupportedPayloadVersion = _state.value.minSupportedPayloadVersion,
            payloadObsolete = _state.value.payloadObsolete,
            obsoleteChunksDiscarded = obsoleteChunksDiscarded,
            lastPayloadDiscardAtMs = lastPayloadDiscardAtMs
        )
    }

    // ── T1/510 — la VUELTA: sondeo, aplicación, rollback, confirmación ──
    //
    // Todo en Dispatchers.IO (el pollJob y maybePollProfile corren ahí);
    // aplicar solo vía el puente registrado por MainActivity (H5). El
    // audio nunca se toca desde aquí: el puente es la única vía al DSP.

    /**
     * C24/510 — decisión pura (testeable en JVM): ¿el payload de este
     * teléfono quedó obsoleto? Solo si el servidor anunció un mínimo
     * mayor que [OrganismPayload.PAYLOAD_VERSION]. Sin señal (null) no
     * hay obsolescencia: el descarte explícito solo corre con señal
     * del servidor, nunca por suposición.
     */
    fun isPayloadObsolete(minSupportedPayloadVersion: Long?): Boolean =
        minSupportedPayloadVersion != null && minSupportedPayloadVersion > OrganismPayload.PAYLOAD_VERSION

    /**
     * C24/510 — procesa la señal `minSupportedPayloadVersion` del
     * sobre del GET (la traen [ProfileFetcher.Result.Current] y
     * [ProfileFetcher.Result.NotModified]). Registra la señal en el
     * State (siempre, con la fresca: un sobre sin señal la limpia) y,
     * si el payload quedó obsoleto, descarta EXPLÍCITAMENTE la cola
     * vieja ([OrganismQueue.discardPayloadOlderThan]) con registro en
     * el Log y en el State (acumulado + fecha). Un chunk en SENDING no
     * se toca (su outcome lo reescribe y el próximo sondeo lo
     * reevalúa); un chunk nuevo encolado DESPUÉS del descarte también
     * es obsoleto (misma versión) y cae en el próximo sondeo — o antes
     * en cuarentena por su 4xx, que el sondeo siguiente también
     * descarta. No toca el perfil aplicado ni el audio.
     */
    private fun handlePayloadVersionSignal(minSupported: Long?) {
        _state.value = _state.value.copy(
            minSupportedPayloadVersion = minSupported,
            payloadObsolete = isPayloadObsolete(minSupported)
        )
        if (!isPayloadObsolete(minSupported)) return
        val q = queue ?: return
        val discarded = q.discardPayloadOlderThan(minSupported!!)
        if (discarded > 0) {
            obsoleteChunksDiscarded += discarded
            lastPayloadDiscardAtMs = System.currentTimeMillis()
            Log.w(TAG, "C24: el servidor exige payload >= v$minSupported y este teléfono envía v${OrganismPayload.PAYLOAD_VERSION}: $discarded chunk(s) obsoletos descartados explícitamente (no se dejan pudrir 7 días en cuarentena)")
        } else {
            Log.i(TAG, "C24: el servidor exige payload >= v$minSupported (este teléfono envía v${OrganismPayload.PAYLOAD_VERSION}); cola sin chunks obsoletos que descartar")
        }
        refreshState()
    }

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
                    record401()
                    refreshState()
                }
                is ProfileFetcher.Result.Retry ->
                    Log.i(TAG, "sondeo de perfil: reintentable (red/429/5xx); próximo ciclo")
                is ProfileFetcher.Result.Quarantine ->
                    Log.i(TAG, "sondeo de perfil: ${res.reason}; se conserva el último bueno")
                is ProfileFetcher.Result.NotModified ->
                    // C24/510: versión igual, pero el sobre puede traer
                    // la señal de payload mínimo (el servidor puede
                    // subirla sin publicar un perfil nuevo).
                    handlePayloadVersionSignal(res.minSupportedPayloadVersion)
                is ProfileFetcher.Result.Current -> handleCurrent(ctx, res)
            }
            retryPendingConfirmation()
        } finally {
            profilePollMutex.unlock()
        }
    }

    private fun handleCurrent(ctx: Context, current: ProfileFetcher.Result.Current) {
        // C24/510: la señal de payload mínimo se procesa ANTES de
        // decidir sobre el perfil (el descarte de la cola vieja no
        // depende de que el perfil nuevo se aplique o no).
        handlePayloadVersionSignal(current.minSupportedPayloadVersion)
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
     *
     * S13 (c): single-flight — si otra aplicación está en curso, este
     * intento se RECHAZA (no espera) y se reintenta en el próximo
     * sondeo. El `profilePollMutex` de [pollProfileOnce] ya serializa
     * sondeos, pero este mutex serializa APLICACIONES como tales.
     */
    private fun applyServerProfile(
        ctx: Context,
        current: ProfileFetcher.Result.Current,
        rolledBack: Boolean,
        promoteToStable: Boolean,
        previousVersion: Long
    ): Boolean {
        if (!applySingleFlight.tryAcquire()) {
            Log.w(TAG, "S13: otra aplicación en curso; se rechaza la v${current.version} (se reintenta en el próximo sondeo)")
            return false
        }
        try {
            return applyServerProfileLocked(ctx, current, rolledBack, promoteToStable, previousVersion)
        } finally {
            applySingleFlight.release()
        }
    }

    private fun applyServerProfileLocked(
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
        // S13 (a): telemetría mínima de la aplicación — tiempo de
        // `applyProfile` + `pushToNative()` (el puente es síncrono y
        // corre en el hilo del pollJob, Dispatchers.IO; ver nota (c) en
        // las constantes S13) y contador de dropouts antes/después.
        // System.nanoTime() es monótono y puro JVM. La traducción y el
        // saveOverride() de arriba NO se miden: la especificación pide
        // applyProfile + pushToNative, no la instalación en disco.
        // El veredicto de defecto solo marca si el perfil QUEDÓ aplicado
        // (`applied` true); un intento fallido queda como "not_applied".
        val xrunsBefore = runCatching { xrunCounterReader?.invoke() }.getOrNull()
        val startNs = System.nanoTime()
        val applied = runCatching { apply(ivanna) }.getOrDefault(false)
        val durationUs = (System.nanoTime() - startNs) / 1_000L
        val xrunsAfter = runCatching { xrunCounterReader?.invoke() }.getOrNull()
        recordApplyMeasurement(
            ApplyMeasurement(
                version = current.version,
                durationUs = durationUs,
                xrunsBefore = xrunsBefore,
                xrunsAfter = xrunsAfter,
                xrunSource = if (xrunsBefore == null && xrunsAfter == null) XRUN_SOURCE_NONE else XRUN_SOURCE_AUDIOTRACK,
                applied = applied,
                atMs = System.currentTimeMillis()
            )
        )
        return applied
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
            // C21: un 401 en la confirmación también es token muerto
            // (el mismo Bearer): marca el estado y registra la fecha.
            ProfileFetcher.ConfirmOutcome.TokenInvalid -> {
                tokenInvalidSeen = true
                record401()
                pendingConfirmation = body
                Log.w(TAG, "confirmación applied con 401 (token inválido); queda pendiente hasta reponer el token")
                refreshState()
            }
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
            // C21: 401 = token muerto; la confirmación queda pendiente
            // (no se descarta: con token nuevo debe salir) y el estado
            // visible lo dice.
            ProfileFetcher.ConfirmOutcome.TokenInvalid -> {
                tokenInvalidSeen = true
                record401()
                refreshState()
            }
            ProfileFetcher.ConfirmOutcome.Retry, null -> { /* sigue pendiente; próximo ciclo */ }
            else -> {
                pendingConfirmation = null
                Log.w(TAG, "confirmación applied pendiente descartada (no reintentable)")
            }
        }
    }
}
