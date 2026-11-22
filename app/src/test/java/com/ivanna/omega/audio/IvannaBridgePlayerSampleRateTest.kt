package com.ivanna.omega.audio

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * FIX 5 (v2.5.465) — el reproductor corría el DSP a 96 kHz (tronidos por
 * shed crónico + trim LUFS al doble de dB/s). La tasa DSP queda en 48 kHz:
 * la cadena está medida y presupuestada a 48 kHz y el DAC de Esse solo
 * anuncia 24-bit @ 44100/48000.
 */
class IvannaBridgePlayerSampleRateTest {

    @Test fun usbAdvertised96000CapsAt48000() {
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf(44_100, 48_000, 96_000)))
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf(96_000)))
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf(192_000)))
    }

    @Test fun usbAdvertised44100StaysNative() {
        assertEquals(44_100, selectBridgePlayerRate(intArrayOf(44_100)))
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf(44_100, 48_000)))
    }

    @Test fun noAdvertisedRatesFallsBackTo48000() {
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf()))
    }

    @Test fun bluetooth96000CapsAt48000() {
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf(96_000)))
        assertEquals(48_000, selectBridgePlayerRate(intArrayOf(88_200)))
    }

    @Test fun directRouteAbove48000IsRejected() {
        assertNull(clampDirectRouteForDsp(DirectRoute(0, 96_000)))
        assertNull(clampDirectRouteForDsp(DirectRoute(0, 192_000)))
    }

    @Test fun directRouteAt48000IsKept() {
        val kept = clampDirectRouteForDsp(DirectRoute(123, 48_000))
        assertEquals(48_000, kept?.sampleRate)
        assertEquals(123, kept?.encoding)
        assertEquals(44_100, clampDirectRouteForDsp(DirectRoute(123, 44_100))?.sampleRate)
    }

    @Test fun nullDirectRouteStaysNull() {
        assertNull(clampDirectRouteForDsp(null))
    }
}
