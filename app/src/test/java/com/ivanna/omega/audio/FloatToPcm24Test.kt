package com.ivanna.omega.audio

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Puerta Hi-Fi USB (0030, v2.5.459): conversión float → PCM_24_BIT_PACKED.
 * floatToPcm24Packed es top-level y pura: se testea en host sin Android.
 * Por muestra: v = (f * 8388607f).toInt().coerceIn(-8388608, 8388607),
 * empaquetado little-endian 3 bytes.
 */
class FloatToPcm24Test {

    @Test fun silenceIsAllZeros() {
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(0f, 0f), 1, out)
        assertEquals(6, bytes)
        assertArrayEquals(byteArrayOf(0, 0, 0, 0, 0, 0), out)
    }

    @Test fun fullScalePositiveIsMax24Bit() {
        // 1.0 → 8388607 = 0x7FFFFF → LE: FF FF 7F (ambos canales).
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(1f, 1f), 1, out)
        assertEquals(6, bytes)
        assertArrayEquals(
            byteArrayOf(0xFF.toByte(), 0xFF.toByte(), 0x7F.toByte(),
                        0xFF.toByte(), 0xFF.toByte(), 0x7F.toByte()),
            out
        )
    }

    @Test fun fullScaleNegativeIsMin24Bit() {
        // -1.0 → -8388607 = 0x800001 → LE: 01 00 80. (La fórmula
        // multiplica por 8388607 = 2^23-1, así que -1.0 no llega al
        // mínimo -8388608 = 0x800000; ese solo sale del clamp con
        // valores < -1.0, ver clampsAboveFullScale.)
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(-1f, -1f), 1, out)
        assertEquals(6, bytes)
        assertArrayEquals(
            byteArrayOf(0x01.toByte(), 0, 0x80.toByte(), 0x01.toByte(), 0, 0x80.toByte()),
            out
        )
    }

    @Test fun halfScaleIsExact() {
        // 0.5 * 8388607 = 4194303.5 → toInt() = 4194303 = 0x3FFFFF → FF FF 3F.
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(0.5f, -0.5f), 1, out)
        assertEquals(6, bytes)
        assertArrayEquals(
            byteArrayOf(0xFF.toByte(), 0xFF.toByte(), 0x3F.toByte(),
                        0x01.toByte(), 0x00.toByte(), 0xC0.toByte()),
            out
        )
    }

    @Test fun clampsAboveFullScale() {
        // 1.5 y -2.0 se acotan a ±8388607/-8388608, no desbordan.
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(1.5f, -2.0f), 1, out)
        assertEquals(6, bytes)
        assertArrayEquals(
            byteArrayOf(0xFF.toByte(), 0xFF.toByte(), 0x7F.toByte(),
                        0, 0, 0x80.toByte()),
            out
        )
    }

    @Test fun nanBecomesSilence() {
        // NaN.toInt() es 0 en JVM: no mete basura en el stream.
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(Float.NaN, 0f), 1, out)
        assertEquals(6, bytes)
        assertArrayEquals(byteArrayOf(0, 0, 0, 0, 0, 0), out)
    }

    @Test fun stereoFramesKeepLeftRightOrder() {
        // Frame 0: L=0.0, R=1.0; Frame 1: L=-1.0 (-8388607 → 01 00 80),
        // R=0.5 (4194303 → FF FF 3F).
        val out = ByteArray(12)
        val bytes = floatToPcm24Packed(floatArrayOf(0f, 1f, -1f, 0.5f), 2, out)
        assertEquals(12, bytes)
        assertArrayEquals(
            byteArrayOf(
                0, 0, 0,
                0xFF.toByte(), 0xFF.toByte(), 0x7F.toByte(),
                0x01.toByte(), 0, 0x80.toByte(),
                0xFF.toByte(), 0xFF.toByte(), 0x3F.toByte()
            ),
            out
        )
    }

    @Test fun returnsBytesWrittenAndClampsToBuffers() {
        // out pequeño: solo cabe 1 frame (6 bytes), no desborda.
        val out = ByteArray(6)
        val bytes = floatToPcm24Packed(floatArrayOf(1f, 1f, 1f, 1f), 2, out)
        assertEquals(6, bytes)
        // input corto: 1 muestra no es un frame estéreo completo.
        val out2 = ByteArray(12)
        val bytes2 = floatToPcm24Packed(floatArrayOf(1f), 2, out2)
        assertEquals(0, bytes2)
    }
}
