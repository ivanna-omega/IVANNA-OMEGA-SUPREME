package com.ivanna.omega.organism

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests S13/510 — presupuesto por hop y contador de xruns alrededor de
 * la aplicación (release 510).
 *
 * Lógica pura: sin Android, sin red, sin hilos. Se prueba la guarda
 * ([OrganismController.evaluateApplyDefect]), el delta del contador
 * ([OrganismController.xrunDelta]), la transición del [State]
 * ([OrganismController.applyMeasurementToState]) y el single-flight
 * ([OrganismController.ApplySingleFlight]).
 *
 * NO se prueba aquí `recordApplyMeasurement` (escribe en el `StateFlow`
 * del singleton y usa `android.util.Log`, que no existe en JVM puro) ni
 * el camino completo de aplicación (necesita Context + puente DSP): la
 * verificación en el teléfono queda para la CI y para la medición
 * física pendiente en el G85 (primera aplicación de la v1.1 con música
 * sonando: levelMaxDbfs, tiempo de aplicación y xruns — sin esa
 * medición, ninguna afirmación de «no hay xruns» es válida).
 */
class ApplyBudgetGuardTest {

    private val budget = OrganismController.APPLY_HOP_BUDGET_US // 2_667 µs = 1 hop (128 muestras @ 48 kHz)

    private fun measurement(
        version: Long = 7L,
        durationUs: Long = 1_200L,
        xrunsBefore: Int? = 10,
        xrunsAfter: Int? = 10,
        xrunSource: String = OrganismController.XRUN_SOURCE_AUDIOTRACK,
        applied: Boolean = true,
        atMs: Long = 1_000_000L
    ) = OrganismController.ApplyMeasurement(
        version = version,
        durationUs = durationUs,
        xrunsBefore = xrunsBefore,
        xrunsAfter = xrunsAfter,
        xrunSource = xrunSource,
        applied = applied,
        atMs = atMs
    )

    // ── (a) la telemetría registra el tiempo de aplicación ──────────────

    @Test
    fun `la telemetria registra el tiempo de aplicacion en el State`() {
        val next = OrganismController.applyMeasurementToState(
            OrganismController.State(), measurement(durationUs = 1_200L)
        )
        assertEquals(7L, next.lastApplyVersion)
        assertEquals(1_200L, next.lastApplyDurationUs)
        assertEquals(1_000_000L, next.lastApplyAtMs)
        assertEquals(OrganismController.XRUN_SOURCE_AUDIOTRACK, next.lastApplyXrunSource)
        assertFalse(next.lastApplyDefective)
        assertEquals(OrganismController.APPLY_DEFECT_NONE, next.lastApplyDefectReason)
    }

    @Test
    fun `la telemetria registra el contador de xruns antes y despues`() {
        val next = OrganismController.applyMeasurementToState(
            OrganismController.State(), measurement(xrunsBefore = 41, xrunsAfter = 42)
        )
        assertEquals(41, next.lastApplyXrunsBefore)
        assertEquals(42, next.lastApplyXrunsAfter)
        assertEquals(1, next.lastApplyXrunDelta)
    }

    @Test
    fun `sin contador observable el delta es null y la fuente es none`() {
        val next = OrganismController.applyMeasurementToState(
            OrganismController.State(),
            measurement(
                xrunsBefore = null, xrunsAfter = null,
                xrunSource = OrganismController.XRUN_SOURCE_NONE
            )
        )
        assertNull(next.lastApplyXrunsBefore)
        assertNull(next.lastApplyXrunsAfter)
        assertNull(next.lastApplyXrunDelta)
        assertEquals(OrganismController.XRUN_SOURCE_NONE, next.lastApplyXrunSource)
        assertFalse(next.lastApplyDefective)
    }

    // ── (b) la guarda marca «aplicado con defecto» ──────────────────────

    @Test
    fun `mas de 1 hop marca aplicado con defecto por presupuesto`() {
        val verdict = OrganismController.evaluateApplyDefect(
            durationUs = budget + 1, xrunDelta = 0, applied = true
        )
        assertTrue(verdict.defective)
        assertTrue(verdict.overBudget)
        assertFalse(verdict.xrunCoincided)
        assertEquals(OrganismController.APPLY_DEFECT_OVER_BUDGET, verdict.reason)
    }

    @Test
    fun `exactamente 1 hop NO es defecto (el limite es mayor que, no mayor o igual)`() {
        val verdict = OrganismController.evaluateApplyDefect(
            durationUs = budget, xrunDelta = 0, applied = true
        )
        assertFalse(verdict.defective)
        assertFalse(verdict.overBudget)
        assertEquals(OrganismController.APPLY_DEFECT_NONE, verdict.reason)
    }

    @Test
    fun `un xrun coincidente marca aplicado con defecto aunque el tiempo quepa`() {
        val verdict = OrganismController.evaluateApplyDefect(
            durationUs = 500L, xrunDelta = 1, applied = true
        )
        assertTrue(verdict.defective)
        assertFalse(verdict.overBudget)
        assertTrue(verdict.xrunCoincided)
        assertEquals(OrganismController.APPLY_DEFECT_XRUN, verdict.reason)
    }

    @Test
    fun `sobrepresupuesto y xrun a la vez dan el motivo combinado`() {
        val verdict = OrganismController.evaluateApplyDefect(
            durationUs = budget + 100, xrunDelta = 2, applied = true
        )
        assertTrue(verdict.defective)
        assertEquals(
            OrganismController.APPLY_DEFECT_OVER_BUDGET_AND_XRUN, verdict.reason
        )
    }

    @Test
    fun `sin contador la pata de xrun no dispara pero la de presupuesto si`() {
        assertFalse(
            OrganismController.evaluateApplyDefect(
                durationUs = 500L, xrunDelta = null, applied = true
            ).defective
        )
        assertTrue(
            OrganismController.evaluateApplyDefect(
                durationUs = budget + 1, xrunDelta = null, applied = true
            ).defective
        )
    }

    @Test
    fun `un intento que no aplico no es aplicado con defecto aunque tarde mucho`() {
        val verdict = OrganismController.evaluateApplyDefect(
            durationUs = 50_000L, xrunDelta = 3, applied = false
        )
        assertFalse(verdict.defective)
        assertEquals(OrganismController.APPLY_DEFECT_NOT_APPLIED, verdict.reason)
        val next = OrganismController.applyMeasurementToState(
            OrganismController.State(), measurement(durationUs = 50_000L, applied = false)
        )
        assertFalse(next.lastApplyDefective)
        assertEquals(OrganismController.APPLY_DEFECT_NOT_APPLIED, next.lastApplyDefectReason)
        // …pero el tiempo del intento sí queda registrado.
        assertEquals(50_000L, next.lastApplyDurationUs)
    }

    @Test
    fun `el veredicto queda en el State con su version para que T4 lo atribuya`() {
        val next = OrganismController.applyMeasurementToState(
            OrganismController.State(), measurement(version = 9L, durationUs = budget + 5)
        )
        assertEquals(9L, next.lastApplyVersion)
        assertTrue(next.lastApplyDefective)
        assertTrue(next.lastApplyOverBudget)
        assertEquals(OrganismController.APPLY_DEFECT_OVER_BUDGET, next.lastApplyDefectReason)
    }

    // ── delta del contador ───────────────────────────────────────────────

    @Test
    fun `xrunDelta es la diferencia y null si falta un extremo`() {
        assertEquals(3, OrganismController.xrunDelta(10, 13))
        assertEquals(0, OrganismController.xrunDelta(10, 10))
        assertNull(OrganismController.xrunDelta(null, 10))
        assertNull(OrganismController.xrunDelta(10, null))
        assertNull(OrganismController.xrunDelta(null, null))
    }

    @Test
    fun `xrunDelta negativo es un reset del contador y se clampa a 0`() {
        // El AudioTrack se recrea y underrunCount vuelve a 0: no es un dropout.
        assertEquals(0, OrganismController.xrunDelta(128, 0))
    }

    // ── (c) el Mutex impide dos aplicaciones concurrentes ───────────────
    //
    // Diseño elegido: la segunda se RECHAZA (tryLock), no espera — el
    // reintento llega en el próximo sondeo. El controlador usa este mismo
    // single-flight alrededor de applyServerProfile.

    @Test
    fun `la segunda aplicacion concurrente se rechaza, no espera`() {
        val gate = OrganismController.ApplySingleFlight()
        assertTrue(gate.tryAcquire())
        assertTrue(gate.isLocked)
        assertFalse(gate.tryAcquire())
        assertFalse(gate.tryAcquire())
    }

    @Test
    fun `tras liberar, la siguiente aplicacion vuelve a entrar`() {
        val gate = OrganismController.ApplySingleFlight()
        assertTrue(gate.tryAcquire())
        gate.release()
        assertFalse(gate.isLocked)
        assertTrue(gate.tryAcquire())
        gate.release()
    }

    @Test
    fun `liberar sin poseedor no hace nada (no lanza)`() {
        val gate = OrganismController.ApplySingleFlight()
        gate.release()
        assertFalse(gate.isLocked)
        assertTrue(gate.tryAcquire())
        gate.release()
    }
}
