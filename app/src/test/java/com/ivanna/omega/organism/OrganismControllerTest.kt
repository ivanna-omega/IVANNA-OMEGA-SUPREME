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
}
