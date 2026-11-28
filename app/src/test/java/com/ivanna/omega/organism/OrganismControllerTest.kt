package com.ivanna.omega.organism

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests de la derivación del estado visible del organismo (v2.5.506).
 * Lógica pura: sin Android, sin red, sin hilos.
 */
class OrganismControllerTest {

    private val now = 1_000_000L

    @Test
    fun `apagado manda sobre todo lo demas`() {
        assertEquals(
            OrganismController.Status.OFF,
            OrganismController.deriveStatus(
                enabled = false, hasToken = false, tokenInvalid = false,
                musicActive = true, queued = 5,
                lastFlush = OrganismSender.Outcome.RETRY,
                lastErrorAtMs = now, nowMs = now
            )
        )
    }

    @Test
    fun `sin token el estado lo dice`() {
        assertEquals(
            OrganismController.Status.NO_TOKEN,
            OrganismController.deriveStatus(
                enabled = true, hasToken = false, tokenInvalid = false,
                musicActive = false, queued = 0,
                lastFlush = null, lastErrorAtMs = 0L, nowMs = now
            )
        )
    }

    @Test
    fun `token invalido se reporta aunque haya musica`() {
        assertEquals(
            OrganismController.Status.TOKEN_INVALID,
            OrganismController.deriveStatus(
                enabled = true, hasToken = true, tokenInvalid = true,
                musicActive = true, queued = 3,
                lastFlush = OrganismSender.Outcome.TOKEN_INVALID,
                lastErrorAtMs = now, nowMs = now
            )
        )
    }

    @Test
    fun `musica sonando es Capturando`() {
        assertEquals(
            OrganismController.Status.CAPTURING,
            OrganismController.deriveStatus(
                enabled = true, hasToken = true, tokenInvalid = false,
                musicActive = true, queued = 0,
                lastFlush = OrganismSender.Outcome.SENT,
                lastErrorAtMs = 0L, nowMs = now
            )
        )
    }

    @Test
    fun `sin musica y con cola es En cola`() {
        assertEquals(
            OrganismController.Status.QUEUED,
            OrganismController.deriveStatus(
                enabled = true, hasToken = true, tokenInvalid = false,
                musicActive = false, queued = 2,
                lastFlush = OrganismSender.Outcome.SENT,
                lastErrorAtMs = 0L, nowMs = now
            )
        )
    }

    @Test
    fun `ultimo flush con error de red es Sin conexion`() {
        assertEquals(
            OrganismController.Status.OFFLINE,
            OrganismController.deriveStatus(
                enabled = true, hasToken = true, tokenInvalid = false,
                musicActive = false, queued = 0,
                lastFlush = OrganismSender.Outcome.RETRY,
                lastErrorAtMs = now, nowMs = now
            )
        )
    }

    @Test
    fun `ultimo flush en cuarentena es Error recuperable`() {
        assertEquals(
            OrganismController.Status.ERROR,
            OrganismController.deriveStatus(
                enabled = true, hasToken = true, tokenInvalid = false,
                musicActive = false, queued = 0,
                lastFlush = OrganismSender.Outcome.QUARANTINE,
                lastErrorAtMs = now, nowMs = now
            )
        )
    }

    @Test
    fun `sin musica sin cola y sin errores es En cola (en espera)`() {
        assertEquals(
            OrganismController.Status.QUEUED,
            OrganismController.deriveStatus(
                enabled = true, hasToken = true, tokenInvalid = false,
                musicActive = false, queued = 0,
                lastFlush = null, lastErrorAtMs = 0L, nowMs = now
            )
        )
    }

    // T2/510 — estado del perfil aplicado (atribución A/B).

    @Test
    fun `State por defecto lleva perfil aplicado neutro de fabrica`() {
        val state = OrganismController.State()
        assertEquals("factory", state.appliedProfileId)
        assertEquals(0L, state.appliedProfileVersion)
        assertEquals("none", state.abState)
        assertEquals("", state.appliedProfileHash)
    }

    @Test
    fun `recordAppliedProfile actualiza el estado y el siguiente chunk lo refleja`() {
        try {
            val applied = OrganismController.recordAppliedProfile(
                profileId = "omega-1795857152818-9lec",
                version = 3L,
                abState = "ab_running",
                profileHash = "sha256:abc123"
            )
            assertEquals("omega-1795857152818-9lec", applied.id)
            assertEquals(3L, applied.version)
            val state = OrganismController.state.value
            assertEquals("omega-1795857152818-9lec", state.appliedProfileId)
            assertEquals(3L, state.appliedProfileVersion)
            assertEquals("ab_running", state.abState)
            assertEquals("sha256:abc123", state.appliedProfileHash)
            // El snapshot que lee enqueueChunk es el mismo estado.
            assertEquals(applied, OrganismController.appliedProfile())
        } finally {
            // Restaura los neutros: el controlador es un singleton.
            OrganismController.recordAppliedProfile("factory", 0L, "none", "")
        }
    }

    // C8 — decisión pura del intento de rollback por arranque (máximo 1 vez).

    @Test
    fun `rollback se intenta una sola vez por arranque`() {
        val ready = ProfileApplier.RollbackCheck.Ready(2L, "{}")
        assertEquals(true, OrganismController.shouldAttemptRollback(false, ready))
        assertEquals(false, OrganismController.shouldAttemptRollback(true, ready))
    }

    @Test
    fun `sin diario pendiente no se intenta nada`() {
        assertEquals(false, OrganismController.shouldAttemptRollback(false, ProfileApplier.RollbackCheck.None))
        assertEquals(false, OrganismController.shouldAttemptRollback(true, ProfileApplier.RollbackCheck.None))
    }

    @Test
    fun `hash distinto tambien consume el unico intento del arranque`() {
        val mismatch = ProfileApplier.RollbackCheck.StableHashMismatch(2L, "aaa", "bbb")
        assertEquals(true, OrganismController.shouldAttemptRollback(false, mismatch))
        assertEquals(false, OrganismController.shouldAttemptRollback(true, mismatch))
    }

    @Test
    fun `estable ausente tambien consume el unico intento del arranque`() {
        val missing = ProfileApplier.RollbackCheck.StableMissing(2L)
        assertEquals(true, OrganismController.shouldAttemptRollback(false, missing))
        assertEquals(false, OrganismController.shouldAttemptRollback(true, missing))
    }

    // T1/510 — decisiones puras del sondeo de perfil (la VUELTA).

    @Test
    fun `State por defecto lleva appliedProfileStatus none`() {
        assertEquals("none", OrganismController.State().appliedProfileStatus)
    }

    @Test
    fun `recordAppliedProfile con profileStatus actualiza appliedProfileStatus`() {
        try {
            OrganismController.recordAppliedProfile(
                profileId = "omega-1795857152818-9lec",
                version = 3L,
                abState = "ab_running",
                profileHash = "sha256:abc123",
                profileStatus = "ab_running"
            )
            assertEquals("ab_running", OrganismController.state.value.appliedProfileStatus)
            assertEquals(3L, OrganismController.state.value.appliedProfileVersion)
        } finally {
            OrganismController.recordAppliedProfile("factory", 0L, "none", "", "none")
        }
    }

    @Test
    fun `interruptor maestro en OFF apaga tambien el sondeo`() {
        assertEquals(
            false,
            OrganismController.shouldPollProfile(
                enabled = false, hasToken = true,
                nowMs = now, lastCheckMs = 0L
            )
        )
    }

    @Test
    fun `sin token no se sondea aunque toque`() {
        assertEquals(
            false,
            OrganismController.shouldPollProfile(
                enabled = true, hasToken = false,
                nowMs = now, lastCheckMs = 0L
            )
        )
    }

    @Test
    fun `sondeo toca a los 30 min y no antes`() {
        val interval = OrganismController.PROFILE_POLL_INTERVAL_MS
        assertEquals(
            true,
            OrganismController.shouldPollProfile(
                enabled = true, hasToken = true,
                nowMs = now, lastCheckMs = now - interval
            )
        )
        assertEquals(
            false,
            OrganismController.shouldPollProfile(
                enabled = true, hasToken = true,
                nowMs = now, lastCheckMs = now - interval + 1000L
            )
        )
    }

    @Test
    fun `stable mas nuevo se aplica`() {
        assertEquals(true, OrganismController.shouldApplyServerVersion("stable", "none", 3L, 2L))
        assertEquals(true, OrganismController.shouldApplyServerVersion("promoted", "promoted", 3L, 2L))
    }

    @Test
    fun `version igual o mas vieja no se aplica`() {
        assertEquals(false, OrganismController.shouldApplyServerVersion("stable", "none", 2L, 2L))
        assertEquals(false, OrganismController.shouldApplyServerVersion("stable", "none", 1L, 2L))
    }

    @Test
    fun `candidate solo se aplica con abState que permite probar`() {
        assertEquals(true, OrganismController.shouldApplyServerVersion("candidate", "candidate", 3L, 2L))
        assertEquals(true, OrganismController.shouldApplyServerVersion("ab_running", "ab_running", 3L, 2L))
        assertEquals(false, OrganismController.shouldApplyServerVersion("candidate", "none", 3L, 2L))
        assertEquals(false, OrganismController.shouldApplyServerVersion("candidate", "promoted", 3L, 2L))
        assertEquals(false, OrganismController.shouldApplyServerVersion("candidate", "rolled_back", 3L, 2L))
    }

    @Test
    fun `rolled_back no se aplica por la via normal (lo gestiona el rollback)`() {
        assertEquals(false, OrganismController.shouldApplyServerVersion("rolled_back", "rolled_back", 3L, 2L))
        assertEquals(false, OrganismController.shouldApplyServerVersion("rolled_back", "rolled_back", 1L, 2L))
    }

    @Test
    fun `status desconocido no se aplica (fail-closed)`() {
        assertEquals(false, OrganismController.shouldApplyServerVersion("experimental", "none", 3L, 2L))
    }

    @Test
    fun `rollback solo restaura un estable estrictamente anterior`() {
        assertEquals(true, OrganismController.shouldRestoreStable(appliedVersion = 4L, stableVersion = 3L))
        assertEquals(false, OrganismController.shouldRestoreStable(appliedVersion = 4L, stableVersion = 4L))
        assertEquals(false, OrganismController.shouldRestoreStable(appliedVersion = 4L, stableVersion = 5L))
        assertEquals(false, OrganismController.shouldRestoreStable(appliedVersion = 4L, stableVersion = null))
    }

    // C21/510 — veredicto puro de la re-validación del token recién pegado.

    @Test
    fun `State por defecto no lleva ultimo 401 ni diagnostico`() {
        val state = OrganismController.State()
        assertEquals(0L, state.last401AtMs)
        assertEquals(null, state.lastTokenDiagnosis)
        assertEquals(false, state.tokenDiagnosing)
    }

    @Test
    fun `Current y NotModified validan el token pegado (el servidor lo acepto)`() {
        val current = ProfileFetcher.Result.Current("omega-1", 3L, "stable", "promoted", "{}")
        assertEquals(
            OrganismController.TokenValidation.VALID,
            OrganismController.classifyTokenValidation(current)
        )
        assertEquals(
            OrganismController.TokenValidation.VALID,
            // C24/510: NotModified es data class (lleva la señal
            // minSupportedPayloadVersion); el default es null.
            OrganismController.classifyTokenValidation(ProfileFetcher.Result.NotModified())
        )
    }

    @Test
    fun `Quarantine tambien valida el token (404 = auth paso, aun no hay perfil)`() {
        val quarantine = ProfileFetcher.Result.Quarantine("GET current → HTTP 404 (aún no hay perfil compilado)")
        assertEquals(
            OrganismController.TokenValidation.VALID,
            OrganismController.classifyTokenValidation(quarantine)
        )
    }

    @Test
    fun `TokenInvalid es el unico veredicto invalido`() {
        assertEquals(
            OrganismController.TokenValidation.INVALID,
            OrganismController.classifyTokenValidation(ProfileFetcher.Result.TokenInvalid)
        )
    }

    @Test
    fun `Retry NoToken y excepcion son desconocido, no invalido`() {
        assertEquals(
            OrganismController.TokenValidation.UNKNOWN,
            OrganismController.classifyTokenValidation(ProfileFetcher.Result.Retry)
        )
        assertEquals(
            OrganismController.TokenValidation.UNKNOWN,
            OrganismController.classifyTokenValidation(ProfileFetcher.Result.NoToken)
        )
        assertEquals(
            OrganismController.TokenValidation.UNKNOWN,
            OrganismController.classifyTokenValidation(null)
        )
    }

    // C24/510 — cuarentena versionada.

    @Test
    fun `State por defecto no lleva cuarentena ni payload obsoleto`() {
        val state = OrganismController.State()
        assertEquals(0, state.quarantined)
        assertNull(state.lastQuarantineError)
        assertNull(state.minSupportedPayloadVersion)
        assertFalse(state.payloadObsolete)
        assertEquals(0, state.obsoleteChunksDiscarded)
        assertEquals(0L, state.lastPayloadDiscardAtMs)
    }

    @Test
    fun `payload obsoleto solo si el servidor exige mas que PAYLOAD_VERSION`() {
        assertFalse(OrganismController.isPayloadObsolete(null))
        assertFalse(OrganismController.isPayloadObsolete(1L))
        assertFalse(
            OrganismController.isPayloadObsolete(OrganismPayload.PAYLOAD_VERSION.toLong())
        )
        assertTrue(
            OrganismController.isPayloadObsolete(OrganismPayload.PAYLOAD_VERSION.toLong() + 1)
        )
    }

    @Test
    fun `NotModified con senal tambien valida el token pegado`() {
        assertEquals(
            OrganismController.TokenValidation.VALID,
            OrganismController.classifyTokenValidation(
                ProfileFetcher.Result.NotModified(minSupportedPayloadVersion = 2L)
            )
        )
    }

    // C16/510 — salud del LLM del cerebro.

    @Test
    fun `State por defecto no lleva salud LLM (desconocida)`() {
        val state = OrganismController.State()
        assertEquals(OrganismController.LLM_STATUS_UNKNOWN, state.llmStatus)
        assertNull(state.llmModel)
        assertNull(state.llmDrafted)
        assertEquals(0L, state.lastLlmStatusAtMs)
    }

    @Test
    fun `parseLlmStatus normaliza nulo vacio y fuera de vocabulario a unknown`() {
        assertEquals(OrganismController.LLM_STATUS_UNKNOWN, OrganismController.parseLlmStatus(null))
        assertEquals(OrganismController.LLM_STATUS_UNKNOWN, OrganismController.parseLlmStatus(""))
        assertEquals(OrganismController.LLM_STATUS_UNKNOWN, OrganismController.parseLlmStatus("muerto"))
        assertEquals(OrganismController.LLM_STATUS_OK, OrganismController.parseLlmStatus("ok"))
        assertEquals(OrganismController.LLM_STATUS_DEGRADED, OrganismController.parseLlmStatus("degraded"))
        assertEquals(OrganismController.LLM_STATUS_UNAVAILABLE, OrganismController.parseLlmStatus("unavailable"))
    }

    @Test
    fun `applyLlmSignal registra la senal fresca en el State`() {
        val state = OrganismController.applyLlmSignal(
            OrganismController.State(),
            status = "degraded",
            model = "duckduckgo-web/claude-haiku-4-5",
            drafted = true,
            atMs = 1_000L
        )
        assertEquals(OrganismController.LLM_STATUS_DEGRADED, state.llmStatus)
        assertEquals("duckduckgo-web/claude-haiku-4-5", state.llmModel)
        assertEquals(true, state.llmDrafted)
        assertEquals(1_000L, state.lastLlmStatusAtMs)
    }

    @Test
    fun `applyLlmSignal sin senal deja unknown sin fecha`() {
        val state = OrganismController.applyLlmSignal(
            OrganismController.State(),
            status = null,
            model = null,
            drafted = null,
            atMs = 1_000L
        )
        assertEquals(OrganismController.LLM_STATUS_UNKNOWN, state.llmStatus)
        assertNull(state.llmModel)
        assertNull(state.llmDrafted)
        assertEquals(0L, state.lastLlmStatusAtMs)
    }

    @Test
    fun `applyLlmSignal sin senal limpia una senal anterior (salud fresca o desconocida)`() {
        val withSignal = OrganismController.applyLlmSignal(
            OrganismController.State(),
            status = "ok",
            model = "gemini/gemini-3-flash-preview",
            drafted = true,
            atMs = 1_000L
        )
        val cleared = OrganismController.applyLlmSignal(
            withSignal,
            status = null,
            model = null,
            drafted = null,
            atMs = 2_000L
        )
        assertEquals(OrganismController.LLM_STATUS_UNKNOWN, cleared.llmStatus)
        assertNull(cleared.llmModel)
        assertNull(cleared.llmDrafted)
        assertEquals(0L, cleared.lastLlmStatusAtMs)
    }

    @Test
    fun `applyLlmSignal con drafted false marca sin redaccion LLM`() {
        val state = OrganismController.applyLlmSignal(
            OrganismController.State(),
            status = "unavailable",
            model = null,
            drafted = false,
            atMs = 1_000L
        )
        assertEquals(OrganismController.LLM_STATUS_UNAVAILABLE, state.llmStatus)
        assertEquals(false, state.llmDrafted)
        assertEquals(1_000L, state.lastLlmStatusAtMs)
    }
}
