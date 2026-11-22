package com.ivanna.omega.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Bypass nativo del ASRC (0033, decisión de Esse 2026-11-16).
 *
 * El ASRC no va en el Snapdragon: se bypassa y el reloj lo maneja el
 * AudioTrack nativo. `RouteAAsrc.asrcEnabled` es el interruptor global
 * (default false = bypass, conmutable en runtime para reactivarlo en
 * pruebas futuras). Con el flag en false, `isAvailable` es false y
 * `processInto()` es passthrough bit-exacto, sin instancia nativa.
 *
 * En los tests JVM la .so no existe, así que `isAvailable` es false con
 * el flag en true TAMBIÉN (sin nativo): estos tests verifican el
 * comportamiento del flag, no del nativo (eso lo cubre el test host C++
 * test_route_a_asrc_bypass). El flag es global: cada test lo restaura
 * con try-finally para no contaminar a los demás.
 */
class RouteAAsrcBypassTest {

    @Test fun bypassFlagOffMeansUnavailableAndPassthrough() {
        val prev = RouteAAsrc.asrcEnabled
        try {
            RouteAAsrc.asrcEnabled = false
            val asrc = RouteAAsrc()
            assertFalse("con el flag en false, isAvailable es false", asrc.isAvailable)
            val inBuf = FloatArray(512 * 2) { i -> (i % 11) * 0.01f }
            val outBuf = FloatArray(512 * 2)
            val produced = asrc.processInto(inBuf, 512, outBuf)
            assertEquals("en bypass, processInto devuelve los mismos frames", 512, produced)
            assertTrue("en bypass, la salida es bit-exacta a la entrada", inBuf.contentEquals(outBuf))
            asrc.close()
        } finally {
            RouteAAsrc.asrcEnabled = prev
        }
    }

    @Test fun flagOnWithoutNativeStillFallsBackToPassthrough() {
        val prev = RouteAAsrc.asrcEnabled
        try {
            RouteAAsrc.asrcEnabled = true
            val asrc = RouteAAsrc()
            // Sin .so en JVM, isAvailable sigue en false (sin nativo):
            // el flag solo no crea el ASRC; el fallback es passthrough.
            assertFalse("sin .so, isAvailable es false aunque el flag esté en true", asrc.isAvailable)
            val inBuf = FloatArray(320 * 2) { i -> (i % 7) * 0.01f }
            val outBuf = FloatArray(320 * 2)
            val produced = asrc.processInto(inBuf, 320, outBuf)
            assertEquals(320, produced)
            assertTrue(inBuf.contentEquals(outBuf))
            asrc.close()
        } finally {
            RouteAAsrc.asrcEnabled = prev
        }
    }

    @Test fun settersAreNoOpsWhenBypassed() {
        val prev = RouteAAsrc.asrcEnabled
        try {
            RouteAAsrc.asrcEnabled = false
            val asrc = RouteAAsrc()
            // No deben lanzar nada con el flag en false.
            asrc.setSampleRate(48000f)
            asrc.setClockCorrectionPpm(-500.0)
            asrc.setPsiActive(true)
            asrc.setPsiAuto(true)
            asrc.reset()
            asrc.close()
        } finally {
            RouteAAsrc.asrcEnabled = prev
        }
    }

    @Test fun statusLabelSaysBypassWhenFlagOff() {
        // 470 (crítica externa): la UI/telemetría debe mostrar explícitamente
        // el bypass; sin esta etiqueta, la tarjeta CORE ASRC presentaba el
        // ASRC como "corrigiendo activamente" aunque el nativo ni se creara.
        // (Solo la función pura: el flag global es mutable y otros tests
        // lo tocan; su default false vive en RouteAAsrc.asrcEnabled.)
        assertEquals("ASRC: BYPASS (0033)", RouteAAsrc.statusLabel(false))
        assertEquals("ASRC: ACTIVO (Ruta A)", RouteAAsrc.statusLabel(true))
    }
}
