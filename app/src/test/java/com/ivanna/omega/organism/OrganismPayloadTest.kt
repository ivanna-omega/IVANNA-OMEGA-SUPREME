package com.ivanna.omega.organism

import com.google.gson.JsonObject
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
        val f = FloatArray(76)
        for (b in 0 until 24) {
            f[b] = -40f
            f[24 + b] = 2f
            f[48 + b] = 0.9f
        }
        f[72] = -30f
        f[73] = 1f
        f[74] = 1f
        f[75] = -35f
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
        // S1/510: frescura de referencia en telemetry.
        assertEquals(1.0, telemetry.get("referenceFreshFrac").asDouble, 0.001)
        assertEquals(60, telemetry.get("framesReferenceFresh").asInt)
        assertEquals(-35.0, telemetry.get("referenceLevelMeanDbfs").asDouble, 0.001)
        assertFalse("sin WAV por defecto", body.has("wavBase64"))
    }

    @Test
    fun `sin cancion conocida usa la etiqueta de sesion en vivo`() {
        val body = OrganismPayload.sessionBody(chunk())
        assertEquals("Sesión en vivo IVANNA", body.get("song").asString)
        assertEquals("IVANNA OMEGA", body.get("artist").asString)
    }

    // T2/510 — atribución A/B en telemetry.

    @Test
    fun `telemetry lleva appliedProfileId version abState y hash del estado aplicado`() {
        val applied = OrganismPayload.AppliedProfile(
            id = "omega-1795857152818-9lec",
            version = 3L,
            abState = "ab_running",
            hash = "sha256:abc123"
        )
        val telemetry = OrganismPayload.sessionBody(chunk(), applied = applied)
            .getAsJsonObject("telemetry")
        assertEquals("omega-1795857152818-9lec", telemetry.get("appliedProfileId").asString)
        assertEquals(3L, telemetry.get("appliedProfileVersion").asLong)
        assertEquals("ab_running", telemetry.get("abState").asString)
        assertEquals("sha256:abc123", telemetry.get("appliedProfileHash").asString)
    }

    @Test
    fun `sin perfil aplicado telemetry lleva neutros y el chunk sigue valido`() {
        val telemetry = OrganismPayload.sessionBody(chunk())
            .getAsJsonObject("telemetry")
        assertEquals("factory", telemetry.get("appliedProfileId").asString)
        assertEquals(0L, telemetry.get("appliedProfileVersion").asLong)
        assertEquals("none", telemetry.get("abState").asString)
        assertEquals("", telemetry.get("appliedProfileHash").asString)
        // El chunk sigue válido: los campos previos no cambian.
        assertTrue(telemetry.has("sessionId"))
        assertTrue(telemetry.has("chunkId"))
        assertTrue(telemetry.has("engineVersion"))
        assertEquals(60, telemetry.get("framesValid").asInt)
    }

    @Test
    fun `tras actualizar el estado aplicado el siguiente chunk refleja la nueva version`() {
        val first = OrganismPayload.sessionBody(
            chunk(),
            applied = OrganismPayload.AppliedProfile(id = "omega-v1", version = 1L, abState = "promoted")
        ).getAsJsonObject("telemetry")
        assertEquals(1L, first.get("appliedProfileVersion").asLong)
        assertEquals("promoted", first.get("abState").asString)
        val second = OrganismPayload.sessionBody(
            chunk(),
            applied = OrganismPayload.AppliedProfile(id = "omega-v2", version = 2L, abState = "candidate", hash = "sha256:def456")
        ).getAsJsonObject("telemetry")
        assertEquals("omega-v2", second.get("appliedProfileId").asString)
        assertEquals(2L, second.get("appliedProfileVersion").asLong)
        assertEquals("candidate", second.get("abState").asString)
        assertEquals("sha256:def456", second.get("appliedProfileHash").asString)
    }

    @Test
    fun `appliedConfirmation lleva profileId version hash previousVersion y appliedAt ISO`() {
        val applied = OrganismPayload.AppliedProfile(
            id = "omega-1795857152818-9lec",
            version = 3L,
            abState = "candidate",
            hash = "sha256:abc123"
        )
        val body = OrganismPayload.appliedConfirmation(
            applied = applied,
            previousVersion = 2L,
            engineVersion = "2.5.510",
            appliedAtMs = 1_795_857_152_818L
        )
        assertEquals("omega-1795857152818-9lec", body.get("profileId").asString)
        assertEquals(3L, body.get("version").asLong)
        assertEquals("sha256:abc123", body.get("profileHash").asString)
        assertEquals(2L, body.get("previousVersion").asLong)
        assertEquals("2.5.510", body.get("engineVersion").asString)
        assertTrue(body.get("appliedAt").asString.startsWith("2026-11-"))
    }

    // C24/510 — versión del esquema del payload.

    @Test
    fun `telemetry lleva payloadVersion igual a PAYLOAD_VERSION`() {
        val telemetry = OrganismPayload.sessionBody(chunk())
            .getAsJsonObject("telemetry")
        assertEquals(OrganismPayload.PAYLOAD_VERSION, telemetry.get("payloadVersion").asInt)
        // El body que se encola lleva la misma versión que lee el descarte.
        assertEquals(
            OrganismPayload.PAYLOAD_VERSION,
            OrganismPayload.payloadVersionOf(OrganismPayload.sessionBody(chunk()))
        )
    }

    @Test
    fun `payloadVersionOf lee la version y 0 si falta o es invalida`() {
        val withVersion = JsonObject().apply {
            add("telemetry", JsonObject().apply { addProperty("payloadVersion", 3) })
        }
        assertEquals(3, OrganismPayload.payloadVersionOf(withVersion))
        // Sin telemetry o sin campo: chunk de antes de C24 = versión 0.
        assertEquals(0, OrganismPayload.payloadVersionOf(JsonObject()))
        assertEquals(0, OrganismPayload.payloadVersionOf(JsonObject().apply {
            add("telemetry", JsonObject())
        }))
        // Campo no numérico o negativo: 0, no excepción.
        assertEquals(0, OrganismPayload.payloadVersionOf(JsonObject().apply {
            add("telemetry", JsonObject().apply { addProperty("payloadVersion", "nueva") })
        }))
        assertEquals(0, OrganismPayload.payloadVersionOf(JsonObject().apply {
            add("telemetry", JsonObject().apply { addProperty("payloadVersion", -2) })
        }))
    }
}
