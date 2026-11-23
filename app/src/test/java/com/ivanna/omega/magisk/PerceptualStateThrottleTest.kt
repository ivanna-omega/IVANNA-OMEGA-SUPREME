// PerceptualStateThrottleTest.kt — Unit test del throttle del empuje
// SET/SEND_PERCEPTUAL_STATE (crujido al abrir/cambiar ventana, 2026-11-21).
//
// El defecto: la app empuja estado perceptual en ráfagas durante las
// transiciones de UI y el daemon reconfigura ~20/s; el dedup corta los
// duplicados exactos, pero los valores jitterean apenas por encima de su
// epsilon y cada jitter se envía. El throttle (PerceptualStateThrottle) es
// el segundo filtro: no enviar la misma acción más de una vez por 100 ms,
// salvo cambio grande (>5% relativo, flip 0↔no-cero, o cambio de modo).
// Este test fija la decisión (enviar / suprimir) con mapas puros y un fake
// clock, sin Android ni socket: si la ventana, el umbral del 5% o la
// máquina de estados cambian, el test falla.
package com.ivanna.omega.magisk

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class PerceptualStateThrottleTest {

    /** Reloj falso: el test avanza el tiempo a mano, determinista. */
    private class FakeClock(var nowMs: Long = 0L) {
        fun advance(ms: Long) {
            nowMs += ms
        }
    }

    private fun throttle(clock: FakeClock) =
        PerceptualStateThrottle(
            windowMs = PerceptualStateThrottle.WINDOW_MS,
            clockMs = { clock.nowMs },
        )

    // Campos reales de SET_PERCEPTUAL_STATE (FusedControls).
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

    @Test
    fun `primer envio siempre se envia`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertTrue(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
    }

    @Test
    fun `rafaga de jitter colapsa a un envio por ventana de 100ms`() {
        // El defecto: 165 evaluaciones por transición de ventana, cada una
        // con un jitter apenas por encima del epsilon del dedup. Con el
        // throttle, solo el primer empuje sale; el resto se suprime hasta
        // que vence la ventana de 100 ms. El jitter oscila (0.386 ↔ 0.388,
        // 0.5% relativo: por encima del epsilon del dedup, por debajo del
        // 5% del throttle), así que el acumulado nunca supera el 5%.
        val clock = FakeClock()
        val t = throttle(clock)
        var sends = 0
        // Primer empuje: se envía.
        if (t.shouldSend("SET_PERCEPTUAL_STATE", setFields())) sends++
        // 164 empujes más con jitter oscilante.
        repeat(164) { i ->
            val v = if (i % 2 == 0) 0.388 else 0.386
            if (t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = v))) sends++
        }
        assertEquals(1, sends)
        // Al vencer la ventana, el siguiente empuje sale (con el último valor).
        clock.advance(100)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.55)))
    }

    @Test
    fun `empujes identicos dentro de la ventana se suprimen`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        assertFalse(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        clock.advance(50)
        assertFalse(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        clock.advance(50)
        // Ventana vencida (100 ms): se envía aunque sea idéntico.
        assertTrue(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
    }

    @Test
    fun `cambio grande mas del 5 por ciento pasa inmediato`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // orbeAmount 0.386 → 0.5: +29% relativo, cambio grande.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.5)))
        // Y el nuevo valor ya es el último enviado: un jitter sobre él se suprime.
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.501)))
    }

    @Test
    fun `cambio justo por debajo del 5 por ciento se suprime`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // orbeAmount 0.386 → 0.40: +3.6% relativo, por debajo del 5%.
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
    }

    @Test
    fun `cambio de modo pasa inmediato`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(outputMode = 1.0)))
        // outputMode 1 → 2 (bocinas→audífonos): cambio de modo, inmediato.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(outputMode = 2.0)))
    }

    @Test
    fun `flip 0 a no-cero pasa inmediato`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(holosAmount = 0.0)))
        // holosAmount 0.0 → 0.5: encendido, siempre grande.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(holosAmount = 0.5)))
        // Y el apagado también: 0.5 → 0.0.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(holosAmount = 0.0)))
    }

    @Test
    fun `cambio grande reinicia la ventana`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        clock.advance(50)
        // Cambio grande a los 50 ms: se envía y la ventana reinicia desde aquí.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.6)))
        clock.advance(50)
        // Solo 50 ms desde el último envío: un cambio pequeño se suprime.
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.601)))
        clock.advance(50)
        // 100 ms desde el último envío: se envía.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.601)))
    }

    @Test
    fun `trailing diferido entrega el ultimo valor al vencer la ventana`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        // Un cambio pequeño dentro de la ventana se suprime pero queda
        // como trailing: el envío diferido debe salir al vencer la ventana.
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
        assertEquals(100L, t.delayUntilFlushMs("SET_PERCEPTUAL_STATE"))
        clock.advance(50)
        assertEquals(50L, t.delayUntilFlushMs("SET_PERCEPTUAL_STATE"))
        // Todavía dentro de la ventana: el trailing no está vencido.
        clock.advance(50)
        assertEquals(0L, t.delayUntilFlushMs("SET_PERCEPTUAL_STATE"))
        assertTrue(t.noteFlushed("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
        // Ya enviado por el diferido: no se re-envía.
        assertFalse(t.noteFlushed("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
    }

    @Test
    fun `diferido obsoleto por cambio grande no se re-envia`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
        // Un cambio grande supersede el trailing: se envía inmediato y el
        // diferido pendiente queda obsoleto.
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.7)))
        // El diferido del valor viejo ya no procede (es el último enviado).
        assertFalse(t.noteFlushed("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
    }

    @Test
    fun `acciones distintas tienen ventanas independientes`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.387)))
        // SEND es otra acción: su propia ventana, primer envío.
        assertTrue(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields()))
        assertFalse(t.shouldSend("SEND_PERCEPTUAL_STATE", sendFields(highCut = 16010.0)))
    }

    @Test
    fun `reset obliga a re-enviar`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.387)))
        t.reset()
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
    }

    @Test
    fun `isLargeChange con epsilon del 5 por ciento`() {
        // 5% exacto no es grande (estricto mayor que); por encima sí.
        assertFalse(
            PerceptualStateThrottle.isLargeChange(
                setFields(orbeAmount = 0.40),
                setFields(orbeAmount = 0.42),
            )
        )
        assertTrue(
            PerceptualStateThrottle.isLargeChange(
                setFields(orbeAmount = 0.40),
                setFields(orbeAmount = 0.43),
            )
        )
        assertFalse(
            PerceptualStateThrottle.isLargeChange(
                setFields(),
                setFields(),
            )
        )
        // Cambio de clave: estructural, siempre grande.
        assertTrue(
            PerceptualStateThrottle.isLargeChange(
                setFields(),
                setFields() + ("newField" to 1.0),
            )
        )
        // highCut 16000 → 16800: +5% exacto, no grande.
        assertFalse(
            PerceptualStateThrottle.isLargeChange(
                sendFields(),
                sendFields(highCut = 16800.0),
            )
        )
        // highCut 16000 → 17000: +6.25%, grande.
        assertTrue(
            PerceptualStateThrottle.isLargeChange(
                sendFields(),
                sendFields(highCut = 17000.0),
            )
        )
    }

    @Test
    fun `isLargeChange con valores no finitos es grande fail-closed`() {
        assertTrue(
            PerceptualStateThrottle.isLargeChange(
                setFields(),
                setFields(orbeAmount = Double.NaN),
            )
        )
        assertTrue(
            PerceptualStateThrottle.isLargeChange(
                setFields(),
                setFields(orbeAmount = Double.POSITIVE_INFINITY),
            )
        )
    }

    // ── Tronidos 493 (c): SET_HEAD_POSE por el mismo throttle ─────────────
    // La pose publicaba hasta 20/s por MagiskBridge directo, sin throttle;
    // cada envío ensucia la generación del snapshot y el efecto re-aplica
    // todo. El throttle la capa a 1/100 ms para cambios pequeños (el giro
    // rápido pasa inmediato como cambio grande) y el diferido garantiza
    // que la última pose llegue en ≤100 ms. Estos tests fijan la decisión
    // del throttle sobre un payload de pose real (yawRad/pitchRad).

    // Campos reales de SET_HEAD_POSE (IvannaSpatialEngine).
    private fun poseFields(
        yawRad: Double = 0.5,
        pitchRad: Double = 0.1,
    ): Map<String, Any?> = mapOf(
        "yawRad" to yawRad,
        "pitchRad" to pitchRad,
    )

    @Test
    fun `pose pequena dentro de la ventana se suprime`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_HEAD_POSE", poseFields()))
        // yaw 0.5 → 0.51 rad (+2%): cambio pequeño, se suprime.
        assertFalse(t.shouldSend("SET_HEAD_POSE", poseFields(yawRad = 0.51)))
        // Al vencer la ventana (100 ms), el mismo valor se envía.
        clock.advance(100)
        assertTrue(t.shouldSend("SET_HEAD_POSE", poseFields(yawRad = 0.51)))
    }

    @Test
    fun `giro rapido de pose pasa inmediato`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_HEAD_POSE", poseFields()))
        // yaw 0.5 → 0.6 rad (+20%): giro rápido, cambio grande.
        assertTrue(t.shouldSend("SET_HEAD_POSE", poseFields(yawRad = 0.6)))
    }

    @Test
    fun `pose y perceptual tienen ventanas independientes`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_PERCEPTUAL_STATE", setFields()))
        assertTrue(t.shouldSend("SET_HEAD_POSE", poseFields()))
        // El throttle es por acción: un cambio pequeño se suprime en su
        // propia ventana, sin afectar a la otra acción.
        assertFalse(t.shouldSend("SET_HEAD_POSE", poseFields(yawRad = 0.51)))
        assertFalse(t.shouldSend("SET_PERCEPTUAL_STATE", setFields(orbeAmount = 0.40)))
    }

    @Test
    fun `trailing de pose entrega el ultimo valor al vencer la ventana`() {
        val clock = FakeClock()
        val t = throttle(clock)
        assertTrue(t.shouldSend("SET_HEAD_POSE", poseFields()))
        assertFalse(t.shouldSend("SET_HEAD_POSE", poseFields(yawRad = 0.51)))
        assertEquals(100L, t.delayUntilFlushMs("SET_HEAD_POSE"))
        clock.advance(100)
        assertTrue(t.noteFlushed("SET_HEAD_POSE", poseFields(yawRad = 0.51)))
    }
}
