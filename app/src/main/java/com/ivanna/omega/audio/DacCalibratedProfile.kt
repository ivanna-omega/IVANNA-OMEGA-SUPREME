package com.ivanna.omega.audio

import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.sqrt

/**
 * Perfil CALIBRADO de Φ para el DAC sin micrófono (v2.5.451).
 *
 * El DAC USB (Genezi) no tiene entrada de micrófono, así que Φ no puede
 * medir en vivo cuando está conectado. La vía es el perfil MEDIDO guardado:
 * la medición de sala (barrido, [RoomMeasurement]) produce 10 ganancias de
 * corrección (centros de octava RoomProbe); aquí se convierten a 24 errores
 * ERB (las bandas de Φ / HOLOS / sensor) y se empujan al operador como
 * perfil calibrado cuando la ruta es USB.
 *
 * Signo: las ganancias de sala son la CORRECCIÓN que el EQ aplica (si la
 * sala está +6 dB caliente a 1 kHz, la ganancia es −6 dB). El error que Φ
 * necesita es el negativo: error = −ganancia (Γ = −error = la ganancia).
 * La interpolación es logarítmica en frecuencia, igual que la escala ERB.
 */
object DacCalibratedProfile {

    /** Centros de las 10 bandas RoomProbe (RoomProbe.hpp kBandHz), en Hz. */
    val roomBandHz = floatArrayOf(
        31.5f, 63f, 125f, 250f, 500f, 1000f, 2000f, 4000f, 8000f, 16000f
    )

    const val kErbBands = 24

    // ── Escala ERB, IDÉNTICA a C++ (RealAcousticSensor.hpp sensorErbRate) ──
    private fun erbRate(fHz: Float): Float =
        21.4f * log10(1.0f + 0.00437f * fHz)

    private fun erbInverse(rate: Float): Float =
        ((10.0f.pow(rate / 21.4f)) - 1.0f) / 0.00437f

    /** Centros ERB a 48 kHz (60 Hz–16 kHz), idénticos a HyperStateOperator::prepare. */
    val erbCentersHz: FloatArray = run {
        val rLo = erbRate(60f)
        val rHi = erbRate(16000f)
        val edges = FloatArray(kErbBands + 1) { i ->
            erbInverse(rLo + (rHi - rLo) * i.toFloat() / kErbBands.toFloat())
        }
        FloatArray(kErbBands) { b -> sqrt(edges[b] * edges[b + 1]) }
    }

    /**
     * Convierte las 10 ganancias de sala (dB) en 24 errores ERB (dB) para Φ.
     * Devuelve null si las ganancias no son válidas (tamaño ≠ 10, no
     * finitas). El error se limita a [−12, +12] dB (el mismo rango que
     * guarda ParameterStore para las ganancias).
     */
    fun roomGainsToErbError(gainsDb: FloatArray): FloatArray? {
        if (gainsDb.size != roomBandHz.size) return null
        if (gainsDb.any { !it.isFinite() }) return null
        val err = FloatArray(kErbBands)
        for (b in 0 until kErbBands) {
            val f = erbCentersHz[b]
            err[b] = (-interpolateLog(roomBandHz, gainsDb, f)).coerceIn(-12f, 12f)
        }
        return err
    }

    /** Interpolación lineal en log(f) entre puntos (xs crecientes). */
    internal fun interpolateLog(xs: FloatArray, ys: FloatArray, x: Float): Float {
        if (x <= xs.first()) return ys.first()
        if (x >= xs.last()) return ys.last()
        val lx = log10(x)
        for (i in 0 until xs.size - 1) {
            if (x <= xs[i + 1]) {
                val lx0 = log10(xs[i])
                val lx1 = log10(xs[i + 1])
                val t = ((lx - lx0) / (lx1 - lx0)).coerceIn(0f, 1f)
                return ys[i] + t * (ys[i + 1] - ys[i])
            }
        }
        return ys.last()
    }
}
