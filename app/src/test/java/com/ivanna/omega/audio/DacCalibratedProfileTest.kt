// DacCalibratedProfileTest.kt
// ============================================================================
// PRUEBAS UNITARIAS — DacCalibratedProfile (v2.5.451)
// Conversión de las 10 ganancias de sala (RoomProbe, centros de octava) a
// 24 errores ERB para el modo CALIBRADO de Φ (DAC sin micrófono).
// © 2026 Luis Uriel Pimentel Pérez
// ============================================================================

package com.ivanna.omega.audio

import org.junit.Test
import org.junit.Assert.*

class DacCalibratedProfileTest {

    @Test
    fun flatGainsGiveZeroError() {
        val err = DacCalibratedProfile.roomGainsToErbError(FloatArray(10))
        assertNotNull(err)
        err!!.forEach { assertEquals(0f, it, 1e-6f) }
    }

    @Test
    fun signIsNegatedGain() {
        // Sala +6 dB caliente a 1 kHz → ganancia de corrección −6 dB →
        // error +6 dB en la banda ERB cercana a 1 kHz. El centro ERB no
        // cae exactamente en 1000 Hz (973.7 Hz), así que la interpolación
        // logarítmica da ~+5.76 dB, no +6.00 exactos: se afirma el rango,
        // no el valor exacto.
        val gains = FloatArray(10)
        gains[5] = -6f  // centro 1000 Hz
        val err = DacCalibratedProfile.roomGainsToErbError(gains)
        assertNotNull(err)
        // La banda ERB más cercana a 1000 Hz debe leer error fuerte y positivo.
        var best = 0
        var bestD = Float.MAX_VALUE
        for (b in 0 until DacCalibratedProfile.kErbBands) {
            val d = kotlin.math.abs(
                kotlin.math.ln(DacCalibratedProfile.erbCentersHz[b]) - kotlin.math.ln(1000f)
            )
            if (d < bestD) { bestD = d; best = b }
        }
        assertTrue(err!![best] > 5f && err[best] <= 6f)
        // Y las bandas lejanas (graves) no deben heredar ese error.
        assertTrue(err[0] > -1f && err[0] < 1f)
    }

    @Test
    fun errorIsClampedToTwelveDb() {
        val gains = FloatArray(10) { -20f }  // fuera de rango
        val err = DacCalibratedProfile.roomGainsToErbError(gains)
        assertNotNull(err)
        err!!.forEach { assertEquals(12f, it, 1e-6f) }
    }

    @Test
    fun invalidInputReturnsNull() {
        assertNull(DacCalibratedProfile.roomGainsToErbError(FloatArray(9)))
        assertNull(DacCalibratedProfile.roomGainsToErbError(FloatArray(11)))
        val nan = FloatArray(10)
        nan[3] = Float.NaN
        assertNull(DacCalibratedProfile.roomGainsToErbError(nan))
    }

    @Test
    fun erbCentersAreMonotonicAndInRange() {
        val c = DacCalibratedProfile.erbCentersHz
        assertEquals(DacCalibratedProfile.kErbBands, c.size)
        for (b in 0 until c.size) {
            assertTrue(c[b] >= 60f && c[b] <= 16000f)
            if (b > 0) assertTrue(c[b] > c[b - 1])
        }
        // Primer centro ~70 Hz, último ~14 kHz (escala ERB 60 Hz–16 kHz).
        assertTrue(c.first() > 60f && c.first() < 120f)
        assertTrue(c.last() > 10000f && c.last() < 16000f)
    }
}
