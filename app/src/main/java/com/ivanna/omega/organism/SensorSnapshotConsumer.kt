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
    private val audioManagerMusicActive: (() -> Boolean)? = null
) {
    companion object {
        private const val TAG = "SensorSnapshotConsumer"
        const val SOURCE_ROUTE_A = 1
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
            while (isRunning) {
                try {
                    val idx = RealAcousticSensor.frameIndex()
                    if (idx > lastIndex) {
                        lastIndex = idx
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
