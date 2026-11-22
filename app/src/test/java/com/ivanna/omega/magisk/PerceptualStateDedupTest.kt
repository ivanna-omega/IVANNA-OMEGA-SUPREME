// PerceptualStateDedupTest.kt — Unit test de la deduplicación del empuje
// SET/SEND_PERCEPTUAL_STATE (micro-cortes TIDAL, 2026-11-21).
//
// El defecto: la app re-empujaba estado perceptual ~2/s aunque nada
// cambiara y el daemon republicaba el snapshot en cada generación; el
// efecto re-aplicaba y ese re-trabajo producía underruns (~5.6/s). El fix
// (PerceptualStateDedup) solo envía si algo cambió de verdad. Este test
// fija la decisión (enviar / suprimir) con mapas puros, sin Android ni
// socket: si el epsilon o la máquina de estados cambian, el test falla.
// La integración en OmegaEngineBridge.sendCommand (conversión JSON→mapa)
// se testea en PerceptualStateDedupJsonTest (Robolectric).
package com.ivanna.omega.magisk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PerceptualStateDedupTest {

    // Campos reales de SEND_PERCEPTUAL_STATE (PerceptualDecisionEngine).
    private fun sendFields(
        compressor: Double = -12.0,
        exciterRed: Double = 0.2,
        highCut: Double = 16000.0,
        spatialWidth: Double = 1.0,
        loudnessTarget: Double = -16.0,
        harmonicGain: Double = 0.4,
        antiDolby: Double = 0.85,
    ): Map<String, Any?> = mapOf(
        "compressor" to compressor,
        "exciterRed" to exciterRed,
        "highCut" to highCut,
        "spatialWidth" to spatialWidth,
        "loudnessTarget" to loudnessTarget,
        "harmonicGain" to harmonicGain,
        "antiDolby" to antiDolby,
    )

    // Subconjunto representativo de SET_PERCEPTUAL_STATE (FusedControls):
    // ORBE 0.386 / HOLOS 0.0 son los valores medidos en vivo en TIDAL.
    private fun setFields(
        orbeAmount: Double = 0.386,
        holosAmount: Double = 0.0,
        outputMode: Double = 2.0,
        spatialWidth: Double = 1.2,
        compressor: Double = -12.0,
        consvAmount: Double = 0.0,
    ): Map<String, Any?> = mapOf(
        "orbeAmount" to orbeAmount,
        "holosAmount" to holosAmount,
        "outputMode" to outputMode,
        "spatialWidth" to spatialWidth,
        "compressor" to compressor,
        "consvAmount" to consvAmount,
    )

    @Test
    fun `primer envio siempre se envia`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
    }

    @Test
    fun `valores identicos no se re-envian`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
    }

    @Test
    fun `cambio real si se envia y luego se vuelve a suprimir`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // ORBE 0.386 → 0.5: cambio real, se envía.
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.5)))
        // El nuevo valor ya está en el caché: se suprime.
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.5)))
    }

    @Test
    fun `escenario TIDAL misma decision 100 veces solo envia la primera`() {
        // El loop de PerceptualCortex re-empujaba la misma decisión ~2/s.
        // Con dedup, solo el primer empuje sale; los 99 siguientes se suprimen.
        val dedup = PerceptualStateDedup()
        var sends = 0
        repeat(100) {
            if (dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields())) sends++
        }
        assertEquals(1, sends)
    }

    @Test
    fun `orbeAmount mas 0_01 se envia`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // +0.01 (2.6% relativo): cambio real, se envía.
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.396)))
    }

    @Test
    fun `highCut mas 100 Hz se envia`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        // +100 Hz (0.6% relativo): cambio real, se envía.
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(highCut = 16100.0)))
    }

    @Test
    fun `compressor menos 0_05 dB se envia`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        // −0.05 dB: cambio real, se envía.
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(compressor = -12.05)))
    }

    @Test
    fun `outputMode 1 a 2 se envia`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields(outputMode = 1.0)))
        // Cambio de modo de salida: se envía aunque el resto sea idéntico.
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields(outputMode = 2.0)))
    }

    @Test
    fun `jitter de orbeAmount se suprime`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // 0.386 → 0.3861: jitter numérico, dentro del epsilon.
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.3861)))
    }

    @Test
    fun `jitter de highCut se suprime`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        // 16000 → 16001 Hz: 1 Hz no es un cambio audible.
        assertFalse(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(highCut = 16001.0)))
    }

    @Test
    fun `jitter de compressor se suprime`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        // −12.0 → −12.005 dB: dentro del epsilon.
        assertFalse(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(compressor = -12.005)))
    }

    @Test
    fun `acciones distintas tienen caches independientes`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // SEND es otra acción: primera vez, se envía aunque SET ya esté en caché.
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        // Y cada una sigue suprimiendo sus propios duplicados.
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
    }

    @Test
    fun `clave anadida o quitada es un cambio`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        val added = setFields() + ("newField" to 1.0)
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", added))
        val removed = setFields() - "consvAmount"
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", removed))
    }

    @Test
    fun `NaN e Inf se tratan como cambio fail-closed`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        // NaN nunca es igual a nada (ni a sí mismo): siempre se envía.
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(compressor = Double.NaN)))
        assertTrue(dedup.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(compressor = Double.NaN)))
        assertTrue(
            dedup.shouldSend(
                "SEND_PERCEPTUAL_STATE",
                sendFields(compressor = Double.POSITIVE_INFINITY)
            )
        )
    }

    @Test
    fun `reset obliga a re-enviar`() {
        val dedup = PerceptualStateDedup()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // El daemon pudo perder su estado (caída/reinicio): tras reset,
        // el mismo payload se re-envía.
        dedup.reset()
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
    }

    @Test
    fun `valuesEqual con epsilon relativo y piso absoluto`() {
        assertTrue(PerceptualStateDedup.valuesEqual(1.0, 1.0))
        assertTrue(PerceptualStateDedup.valuesEqual(0.386, 0.3861))
        assertTrue(PerceptualStateDedup.valuesEqual(16000.0, 16001.0))
        assertTrue(PerceptualStateDedup.valuesEqual(0.0, 0.00005))
        assertFalse(PerceptualStateDedup.valuesEqual(0.386, 0.396))
        assertFalse(PerceptualStateDedup.valuesEqual(0.0, 0.001))
        assertFalse(PerceptualStateDedup.valuesEqual(1.0, 2.0))
        assertFalse(PerceptualStateDedup.valuesEqual(0.5, Double.NaN))
        assertFalse(PerceptualStateDedup.valuesEqual(0.5, Double.POSITIVE_INFINITY))
    }
}
