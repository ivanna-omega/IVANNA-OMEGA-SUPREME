package com.ivanna.omega.organism

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests del payload Muselink (schema verificado contra
 * ~/workspace/muselink-source/src/lib/museTypes.ts + ivannaDsp.ts).
 */
class OrganismPayloadTest {

    private fun chunk(): SessionAggregator.SessionChunk {
        val agg = SessionAggregator(windowSeconds = 5f)
        var emitted: SessionAggregator.SessionChunk? = null
        val f = FloatArray(74)
        for (b in 0 until 24) {
            f[b] = -40f
            f[24 + b] = 2f
            f[48 + b] = 0.9f
        }
        f[72] = -30f
        f[73] = 1f
        for (i in 0 until 60) {
            agg.addFrame(f, 1_000_000L + i * 85_000L, i.toLong())?.let { emitted = it }
        }
        return emitted!!
    }

    @Test
    fun `audioFeatures lleva 24 bandas ERB con source microphone-erb`() {
        val features = OrganismPayload.audioFeatures(chunk())
        assertEquals("microphone-erb", features.get("source").asString)
        val bands = features.getAsJsonArray("bands")
        assertEquals(24, bands.size())
        val band0 = bands[0].asJsonObject
        assertEquals("ERB-1", band0.get("band").asString)
        assertEquals(-40.0, band0.get("magnitudeDb").asDouble, 0.001)
        assertEquals(2.0, band0.get("errorDb").asDouble, 0.001)
        assertEquals(0.9, band0.get("coherence").asDouble, 0.001)
        assertTrue(band0.get("freqHz").asDouble > 60.0)
        val band23 = bands[23].asJsonObject
        assertTrue(band23.get("freqHz").asDouble < 16000.0)
        assertEquals(0.9, features.get("meanCoherence").asDouble, 0.001)
        assertEquals(2.0, features.get("spectralErrorRmsDb").asDouble, 0.001)
    }

    @Test
    fun `sessionBody lleva song artist room equipment y telemetry`() {
        val c = chunk()
        val body = OrganismPayload.sessionBody(c, song = "Voodoo Child", artist = "Jimi Hendrix")
        assertEquals("Voodoo Child", body.get("song").asString)
        assertEquals("Jimi Hendrix", body.get("artist").asString)
        assertTrue(body.has("audioFeatures"))
        assertTrue(body.has("room"))
        assertTrue(body.has("equipment"))
        val telemetry = body.getAsJsonObject("telemetry")
        assertEquals(c.sessionId, telemetry.get("sessionId").asString)
        assertEquals(c.chunkId, telemetry.get("chunkId").asInt)
        assertEquals(60, telemetry.get("framesValid").asInt)
        assertEquals(60, telemetry.get("framesTotal").asInt)
        assertFalse("sin WAV por defecto", body.has("wavBase64"))
    }

    @Test
    fun `sin cancion conocida usa la etiqueta de sesion en vivo`() {
        val body = OrganismPayload.sessionBody(chunk())
        assertEquals("Sesión en vivo IVANNA", body.get("song").asString)
        assertEquals("IVANNA OMEGA", body.get("artist").asString)
    }
}
