package com.ivanna.omega.organism

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests del agregador de sesión (ventanas 1–5 s, v2.5.506).
 * Puro JVM: frames sintéticos con el layout ERB[74] del sensor.
 */
class SessionAggregatorTest {

    private fun frame(
        mag: Float = -40f,
        err: Float = 2f,
        coh: Float = 0.9f,
        level: Float = -30f,
        valid: Float = 1f
    ): FloatArray {
        val f = FloatArray(74)
        for (b in 0 until 24) {
            f[b] = mag
            f[24 + b] = err
            f[48 + b] = coh
        }
        f[72] = level
        f[73] = valid
        return f
    }

    @Test
    fun `ventana de 5s emite un chunk con medias correctas`() {
        val agg = SessionAggregator(windowSeconds = 5f)
        var emitted: SessionAggregator.SessionChunk? = null
        // 60 frames × 85 ms = 5.1 s de ventana (cierra en el frame 60).
        for (i in 0 until 60) {
            val chunk = agg.addFrame(frame(), 1_000_000L + i * 85_000L, i.toLong())
            if (chunk != null) emitted = chunk
        }
        assertNotNull("la ventana de 5 s debe cerrar", emitted)
        val chunk = emitted!!
        assertEquals(60, chunk.framesValid)
        assertEquals(60, chunk.framesTotal)
        assertEquals(-40f, chunk.erbMeanDb[0], 0.001f)
        assertEquals(-40f, chunk.erbMeanDb[23], 0.001f)
        assertEquals(2f, chunk.errorMeanDb[5], 0.001f)
        assertEquals(0.9f, chunk.coherenceMean[5], 0.001f)
        assertEquals(-30f, chunk.levelMeanDbfs, 0.001f)
        assertEquals(-30f, chunk.levelMaxDbfs, 0.001f)
        // Sin dispersión (todos los frames iguales): std = 0.
        assertEquals(0f, chunk.erbStdDb[5], 0.001f)
        assertEquals(0.9f, chunk.meanCoherence, 0.001f)
        assertEquals(2f, chunk.spectralErrorRmsDb, 0.001f)
        assertEquals(0, chunk.chunkId)
        assertEquals(1, agg.chunksEmitted)
    }

    @Test
    fun `frames invalidos no agregan pero cuentan en framesTotal`() {
        val agg = SessionAggregator(windowSeconds = 5f)
        // 10 válidos + 5 inválidos (valid=0) + 3 silencio (-80 dBFS) + 2 NaN.
        for (i in 0 until 10) agg.addFrame(frame(), 1_000_000L + i * 85_000L, i.toLong())
        for (i in 10 until 15) agg.addFrame(frame(valid = 0f), 1_000_000L + i * 85_000L, i.toLong())
        for (i in 15 until 18) agg.addFrame(frame(level = -80f), 1_000_000L + i * 85_000L, i.toLong())
        val nan = frame().also { it[3] = Float.NaN }
        for (i in 18 until 20) agg.addFrame(nan, 1_000_000L + i * 85_000L, i.toLong())
        assertEquals(20, agg.framesSeen)
        assertEquals(10, agg.framesValidSeen)
        assertEquals(0, agg.chunksEmitted)
        val summary = agg.close()
        assertEquals(1, summary.chunks)
        assertEquals(10, summary.framesValid)
        assertEquals(20, summary.framesTotal)
    }

    @Test
    fun `dispersion refleja variacion real entre frames`() {
        val agg = SessionAggregator(windowSeconds = 1f)
        // Err alterna +2 / -2 → media 0, std 2.
        // Con 80 ms entre frames la ventana de 1 s cierra en el frame 13
        // (14 frames, 7 de cada signo: media exacta 0). Con 85 ms cerraba
        // en 13 frames (impar) y la media salía 2/13.
        var emitted: SessionAggregator.SessionChunk? = null
        for (i in 0 until 14) {
            val err = if (i % 2 == 0) 2f else -2f
            val chunk = agg.addFrame(frame(err = err), 1_000_000L + i * 80_000L, i.toLong())
            if (chunk != null) emitted = chunk
        }
        assertNotNull(emitted)
        assertEquals(0f, emitted!!.errorMeanDb[0], 0.001f)
        assertEquals(2f, emitted.erbStdDb[0], 0.01f)
    }

    @Test
    fun `ventana se limita a 1-5 segundos`() {
        assertEquals(1_000_000L, SessionAggregator(windowSeconds = 0.2f).windowUs)
        assertEquals(5_000_000L, SessionAggregator(windowSeconds = 30f).windowUs)
        assertEquals(2_000_000L, SessionAggregator(windowSeconds = 2f).windowUs)
    }

    @Test
    fun `close sin frames validos no emite chunk`() {
        val agg = SessionAggregator(windowSeconds = 5f)
        agg.addFrame(frame(valid = 0f), 1_000_000L, 0L)
        val summary = agg.close()
        assertEquals(0, summary.chunks)
        assertEquals(0, summary.framesValid)
        assertEquals(1, summary.framesTotal)
        assertEquals(-120f, summary.levelMeanDbfs, 0.001f)
    }

    @Test
    fun `centros ERB cubren 60 Hz a 16 kHz en orden`() {
        val centers = SessionAggregator.erbCenterHz()
        assertEquals(24, centers.size)
        assertTrue(centers[0] > 60f && centers[0] < 120f)
        assertTrue(centers[23] > 8000f && centers[23] < 16000f)
        for (i in 1 until centers.size) assertTrue(centers[i] > centers[i - 1])
    }
}
