/*
 * © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
 * Driver del micrófono del teléfono para NeuroSensorGateway (v2.5.449).
 */
package com.ivanna.omega.neuromorphic

import android.content.Context
import android.util.Log
import com.ivanna.omega.audio.RealAcousticSensor
import java.util.concurrent.atomic.AtomicLong

/**
 * Driver que alimenta el bucle cerrado neuro-acústico con el MICRÓFONO
 * DEL TELÉFONO (directiva de Esse: sin root, sin hardware extra, con lo
 * que el equipo ya tiene + matemática).
 *
 * No genera datos: toma los frames ERB[74] reales que publica
 * RealAcousticSensor (AudioRecord, 48 kHz, sin root — solo permiso
 * RECORD_AUDIO) y los reenvía al gateway, que los manda a JNI
 * (nativeNeuroLoopIngestMic). El mapeo 24 ERB -> 8 Greenwood y la
 * validación viven en C++ (micErbToNeuroFrame). Sin micrófono, sin
 * permiso o en silencio, no llega nada y el bucle permanece fail-closed.
 *
 * Es un bucle cerrado ACÚSTICO: el teléfono escucha su propia salida
 * (altavoz+habitación) y corrige el balance tonal hacia el target. NO
 * es una medición cortical: no lee el cerebro y no es un dispositivo
 * médico. La matemática NPE (pool LIF) sigue procesando la respuesta.
 *
 * onFrame: nadie más lo usa hoy (el gancho de Φ sigue sin conectarse),
 * así que este driver lo toma y lo libera en stop(). Si otro
 * consumidor lo tomara en el futuro, habría que multiplexarlo aquí.
 */
class NeuroMicDriver(
    context: Context,
    override val kind: NeuroSensorGateway.Kind = NeuroSensorGateway.Kind.ACOUSTIC_MIC,
    private val nowUs: () -> Long = { NeuroSensorGateway.nowUs() }
) : NeuroSensorGateway.Driver {

    private val appContext = context.applicationContext

    val received = AtomicLong()
    val accepted = AtomicLong()
    val rejected = AtomicLong()

    @Volatile private var running = false

    @Synchronized override fun start() {
        if (running) return
        running = true
        // La captura del micrófono arranca sola al abrir la app
        // (PersistedStateRestorer); si no hay permiso, start() devuelve
        // false y no llegan frames — fail-closed, sin crash.
        val micOk = RealAcousticSensor.start(appContext)
        if (!micOk) Log.w(TAG, "start: micrófono no disponible (sin permiso RECORD_AUDIO o mic ocupado)")
        RealAcousticSensor.onFrame = { frame ->
            received.incrementAndGet()
            val ok = NeuroSensorGateway.submitMicFrame(frame, nowUs())
            if (ok) accepted.incrementAndGet() else rejected.incrementAndGet()
        }
    }

    @Synchronized override fun stop() {
        running = false
        RealAcousticSensor.onFrame = null
        // La captura del micrófono sigue (la usa también Φ / corrección
        // de sala); solo se desconecta el reenvío al bucle neuro.
    }

    companion object {
        private const val TAG = "NeuroMicDriver"
    }
}
