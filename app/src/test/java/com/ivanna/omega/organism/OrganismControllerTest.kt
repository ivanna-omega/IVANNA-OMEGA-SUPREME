package com.ivanna.omega.organism

import org.junit.Assert.assertEquals
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
}
