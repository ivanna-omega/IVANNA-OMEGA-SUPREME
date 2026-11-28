package com.ivanna.omega.organism

import android.os.SystemClock
import android.util.Log
import com.ivanna.omega.audio.RealAcousticSensor
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.dsp.DSPBridge

/**
 * SensorSnapshotConsumer — consumidor DEDICADO del canal lateral (v2.5.506).
 *
 * Hilo propio ("IvannaSensorSnapshot"), NUNCA el hilo de audio ni la UI.
 * Cada ~40 ms lee el índice del sensor; con cada frame nuevo:
 *   1. Lo publica como snapshot versionado por JNI
 *      (nativeSensorSnapshotIngest: construir → canal latest-wins →
 *      validar). Un snapshot rechazado no toca el estado. La JNI NO
 *      entrega a Φ (defecto #1: el único escritor del staging es el
 *      hilo de audio); el canal es el registro observacional de Ruta B.
 *   2. Lo pasa al agregador de sesión (ventanas 1–5 s → cola durable).
 *
 * NO roba RealAcousticSensor.onFrame (ese listener lo tiene
 * NeuroMicDriver para el bucle neuro): este consumidor sondea
 * latestFrame()/frameIndex(), que son lecturas lock-free del sensor.
 * Si no hay música (frames inválidos / silencio), no se agrega nada y
 * no se encola nada: la captura y el envío solo arrancan con música.
 *
 * C23/510 — WATCHDOG DE STALL: si frameIndex() deja de avanzar
 * [STALL_THRESHOLD_MS] con música sonando, el sensor está parado (la
 * sesión no se genera y el estado diría «En espera» como si no hubiera
 * música). El watchdog registra el stall ([stalls]/[lastStallAtMs]) y
 * dispara [onStall] (OrganismController reintenta el anclaje del mic
 * con el mismo SessionAggregator/sessionId: la sesión NO se parte).
 * Sin música no hay stall (el sensor parado en silencio es lo normal).
 * La salud del consumidor ([snapshotsAccepted]/[snapshotsRejected]/
 * [stalls]) vive en OrganismController.State y la tarjeta la muestra.
 */
class SensorSnapshotConsumer(
    private val aggregator: SessionAggregator,
    private val onChunk: (SessionAggregator.SessionChunk) -> Unit,
    private val pollMs: Long = 40L,
    /**
     * Respaldo del sistema para "hay música": AudioManager.isMusicActive().
     * Lo inyecta OrganismController (tiene el Context); null = sin respaldo.
     * Se evalúa en cada llamada a isMusicActive(), nunca se cachea.
     */
    private val audioManagerMusicActive: (() -> Boolean)? = null,
    /**
     * C23/510 — umbral del watchdog de stall (ms sin avance de
     * frameIndex() con música sonando). Parámetro para tests; en
     * producción es [STALL_THRESHOLD_MS].
     */
    private val stallThresholdMs: Long = STALL_THRESHOLD_MS,
    /**
     * C23/510 — qué hacer ante un stall. Lo cablea OrganismController
     * (reintento del anclaje del mic, mismo SessionAggregator/sessionId:
     * la sesión NO se parte). Null = solo contar y registrar. Corre en
     * el hilo IvannaSensorSnapshot; lo que hace debe ser barato o
     * lanzar a otro hilo (el controlador lanza a Dispatchers.IO).
     */
    private val onStall: (() -> Unit)? = null
) {
    companion object {
        private const val TAG = "SensorSnapshotConsumer"
        const val SOURCE_ROUTE_A = 1

        /**
         * C23/510 — umbral PROVISIONAL del watchdog de stall (10 s de
         * música continua sin avance del índice).
         *
         * Frames ~85 ms y poll 40 ms: 10 s son ~117 frames perdidos
         * (~250 polls), órdenes de magnitud por encima de cualquier
         * jitter real (pausa de GC, planificación del hilo). Un stall
         * genuino (hilo de captura muerto o nativo atascado) dura
         * indefinidamente, así que 10 s lo caza igual y sin re-anclas
         * espurias. La propuesta EXIGE medir la cadencia real de
         * frameIndex() con música sonando antes de fijar el umbral
         * definitivo: este valor es el punto de partida conservador y
         * SOLO se cambia con esa medición en vivo (ver reporte C23).
         * No bajarlo sin medir. El reloj solo acumula con música
         * (muestreada 1 vez/s, [STALL_MUSIC_SAMPLE_MS]): en silencio el
         * reloj se resetea, así que el umbral efectivo es 10 s ± 1 s.
         */
        const val STALL_THRESHOLD_MS = 10_000L

        /**
         * C23/510 — cadencia del muestreo de «hay música» en el
         * watchdog (1 s). La consulta toca AudioManager (binder) y no
         * se hace a 25 Hz; 1 s frente a un umbral de 10 s no cambia el
         * diagnóstico (el stall se detecta con 10 s ± 1 s de música
         * continua sin avance).
         */
        const val STALL_MUSIC_SAMPLE_MS = 1_000L

        /**
         * C23/510 — decisión pura del watchdog (testeable en JVM): hay
         * stall solo si suena música Y el índice lleva >= umbral sin
         * avanzar. Sin música no hay stall (el sensor parado en
         * silencio es lo normal) y un avance reciente tampoco lo es.
         */
        fun shouldReportStall(
            nowUptimeMs: Long,
            lastAdvanceUptimeMs: Long,
            musicActive: Boolean,
            thresholdMs: Long
        ): Boolean = musicActive && nowUptimeMs - lastAdvanceUptimeMs >= thresholdMs
    }

    @Volatile
    var isRunning: Boolean = false
        private set

    @Volatile
    var snapshotsAccepted: Long = 0L
        private set

    @Volatile
    var snapshotsRejected: Long = 0L
        private set

    /**
     * C23/510 — stalls detectados por el watchdog (frameIndex() sin
     * avanzar [stallThresholdMs] con música sonando). Cada stall
     * dispara [onStall] una vez; el siguiente no puede disparar hasta
     * pasado otro umbral (el watchdog resetea su reloj al registrar).
     */
    @Volatile
    var stalls: Long = 0L
        private set

    /** C23/510 — fecha (epoch ms) del último stall; 0 = ninguno. */
    @Volatile
    var lastStallAtMs: Long = 0L
        private set

    /**
     * Línea base del beacon de Ruta B para detectar crecimiento entre
     * llamadas a isMusicActive(). -1 = sin línea base (primer ciclo o
     * beacon no disponible): no se puede probar crecimiento (fail-closed).
     */
    @Volatile
    private var lastBeaconFrames: Long = -1L

    @Volatile
    private var thread: Thread? = null

    @Synchronized
    fun start() {
        if (isRunning) return
        isRunning = true
        val t = Thread({
            var lastIndex = -1L
            // C23/510 — watchdog de stall: último avance del índice
            // (uptime ms, monótono; el intervalo es lo que importa) y
            // último muestreo de «hay música».
            var lastAdvanceMs = SystemClock.uptimeMillis()
            var lastMusicSampleMs = lastAdvanceMs
            while (isRunning) {
                try {
                    val nowMs = SystemClock.uptimeMillis()
                    val idx = RealAcousticSensor.frameIndex()
                    if (idx > lastIndex ||
                        // C23 — el índice RETROCEDIÓ con valor válido:
                        // el nativo se re-inicializó (prepare() pone
                        // frame_index a 0, p. ej. stop/start del barrido
                        // Genezi). Es una época nueva, no un stall: sin
                        // esta rama el watermark quedaría obsoleto y el
                        // watchdog dispararía un stall espurio al volver
                        // la música. idx == -1 (nativo no disponible)
                        // NO es época nueva: cae a la rama de stall.
                        (idx >= 0L && idx < lastIndex)
                    ) {
                        lastIndex = idx
                        lastAdvanceMs = nowMs
                        val frame = RealAcousticSensor.latestFrame()
                        if (frame.size == 76) {
                            val tsUs = SystemClock.uptimeMillis() * 1000L
                            val ok = IvannaNativeLib.guardedNative(false) {
                                IvannaNativeLib.nativeSensorSnapshotIngest(
                                    frame, tsUs, idx, SOURCE_ROUTE_A
                                )
                            }
                            if (ok) snapshotsAccepted++ else snapshotsRejected++
                            aggregator.addFrame(frame, tsUs, idx)?.let { chunk ->
                                onChunk(chunk)
                            }
                        }
                    } else if (nowMs - lastMusicSampleMs >= STALL_MUSIC_SAMPLE_MS) {
                        // C23 — «hay música» se muestrea como mucho 1
                        // vez/s: la consulta toca AudioManager (binder)
                        // y no se hace a 25 Hz.
                        lastMusicSampleMs = nowMs
                        val music = isMusicActive()
                        if (shouldReportStall(nowMs, lastAdvanceMs, music, stallThresholdMs)) {
                            stalls++
                            lastStallAtMs = System.currentTimeMillis()
                            Log.w(TAG, "C23: stall del sensor: frameIndex() sin avanzar ${stallThresholdMs} ms con música sonando (aceptados=$snapshotsAccepted rechazados=$snapshotsRejected stalls=$stalls); re-anclando el mic")
                            // Cooldown de un umbral: el próximo stall no
                            // dispara hasta pasados otros 10 s (un
                            // reinicio tarda ~1 s, así que dos
                            // recuperaciones no se pisan y no hay
                            // tormenta de reintentos si el mic no vuelve).
                            lastAdvanceMs = nowMs
                            // El reinicio re-inicializa el nativo y su
                            // frame_index vuelve a 0: sin resetear el
                            // watermark, el consumidor ignoraría los
                            // frames nuevos hasta superar el valor viejo.
                            lastIndex = -1L
                            runCatching { onStall?.invoke() }
                                .onFailure { Log.w(TAG, "C23: onStall falló: $it") }
                        } else if (!music) {
                            // Sin música no hay stall Y el reloj no
                            // acumula: el sensor parado en silencio es
                            // lo normal. Sin este reset, un silencio
                            // largo dejaría lastAdvanceMs obsoleto y el
                            // primer compás de música dispararía un
                            // stall espurio (el sensor sano tarda
                            // ~170 ms en publicar su primer frame).
                            lastAdvanceMs = nowMs
                        }
                    }
                } catch (e: Exception) {
                    Log.w(TAG, "Consumidor: $e")
                }
                try {
                    Thread.sleep(pollMs)
                } catch (_: InterruptedException) {
                    break
                }
            }
        }, "IvannaSensorSnapshot")
        thread = t
        t.start()
        Log.i(TAG, "Consumidor de snapshots arrancado (canal lateral, fuera del hilo de audio)")
    }

    @Synchronized
    fun stop() {
        isRunning = false
        thread?.interrupt()
        thread = null
    }

    /**
     * ¿Hay música sonando? La Ruta A ya NO decide: su medida (0 µs/hop)
     * se atora aunque suene música, así que su frame válido no es señal
     * fiable. Deciden, en OR:
     *   1. El beacon de Ruta B AVANZA frames (el efecto procesa en
     *      audioserver = hay música fluyendo por Ruta B). Se compara con
     *      la línea base de la llamada anterior; sin línea base (-1) o
     *      con beacon no disponible (-1) no se puede probar crecimiento
     *      (fail-closed) y se pasa al respaldo.
     *   2. AudioManager.isMusicActive() (respaldo del sistema, inyectado
     *      por OrganismController; null = sin respaldo).
     * nowMs se conserva por compatibilidad de firma; la decisión ya no
     * depende del tiempo desde el último frame válido de Ruta A.
     */
    fun isMusicActive(nowMs: Long = SystemClock.uptimeMillis()): Boolean {
        val frames = runCatching { DSPBridge.effectBeaconFrames() }.getOrDefault(-1L)
        val beaconGrowing = frames > 0 && lastBeaconFrames >= 0 && frames > lastBeaconFrames
        if (frames >= 0) lastBeaconFrames = frames
        if (beaconGrowing) return true
        if (audioManagerMusicActive?.invoke() == true) return true
        return false
    }
}
