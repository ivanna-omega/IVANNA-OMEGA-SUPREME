package com.ivanna.omega.organism

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * C23/510 — watchdog de stall del sensor. Lógica pura: sin Android,
 * sin red, sin hilos. (La recuperación real — restartMic — es código
 * Android y se verifica en vivo; aquí se fija la decisión y el umbral.)
 */
class SensorStallWatchdogTest {

    @Test
    fun `stall justo en el umbral con musica`() {
        assertTrue(
            SensorSnapshotConsumer.shouldReportStall(
                nowUptimeMs = 20_000L,
                lastAdvanceUptimeMs = 10_000L,
                musicActive = true,
                thresholdMs = 10_000L
            )
        )
    }

    @Test
    fun `un ms antes del umbral no es stall`() {
        assertFalse(
            SensorSnapshotConsumer.shouldReportStall(
                nowUptimeMs = 19_999L,
                lastAdvanceUptimeMs = 10_000L,
                musicActive = true,
                thresholdMs = 10_000L
            )
        )
    }

    @Test
    fun `sin musica no hay stall aunque el indice lleve parado una hora`() {
        assertFalse(
            SensorSnapshotConsumer.shouldReportStall(
                nowUptimeMs = 3_600_000L,
                lastAdvanceUptimeMs = 0L,
                musicActive = false,
                thresholdMs = 10_000L
            )
        )
    }

    @Test
    fun `el indice avanzando hace poco no es stall`() {
        assertFalse(
            SensorSnapshotConsumer.shouldReportStall(
                nowUptimeMs = 20_000L,
                lastAdvanceUptimeMs = 19_000L,
                musicActive = true,
                thresholdMs = 10_000L
            )
        )
    }

    @Test
    fun `el umbral es conservador y provisional`() {
        // ~117 frames perdidos (frame ~85 ms) / ~250 polls (40 ms):
        // órdenes de magnitud por encima de cualquier jitter real.
        // La propuesta exige medir la cadencia real de frameIndex()
        // con música sonando antes de fijar el umbral definitivo:
        // este valor SOLO se cambia con esa medición en vivo (ver
        // reporte C23). El test lo fija para que nadie lo baje a
        // escondidas.
        assertTrue(SensorSnapshotConsumer.STALL_THRESHOLD_MS >= 10_000L)
        assertTrue(SensorSnapshotConsumer.STALL_THRESHOLD_MS >= 100L * 85L)
        // El muestreo de «hay música» es 10× más fino que el umbral:
        // el reloj del watchdog no acumula más de 1 s de error.
        assertTrue(
            SensorSnapshotConsumer.STALL_MUSIC_SAMPLE_MS <=
                SensorSnapshotConsumer.STALL_THRESHOLD_MS / 10L
        )
    }

    @Test
    fun `el consumidor nace con la salud a cero`() {
        val consumer = SensorSnapshotConsumer(SessionAggregator(), {})
        assertEquals(0L, consumer.snapshotsAccepted)
        assertEquals(0L, consumer.snapshotsRejected)
        assertEquals(0L, consumer.stalls)
        assertEquals(0L, consumer.lastStallAtMs)
    }

    @Test
    fun `State por defecto lleva la salud del sensor a cero`() {
        val state = OrganismController.State()
        assertEquals(0L, state.snapshotsAccepted)
        assertEquals(0L, state.snapshotsRejected)
        assertEquals(0L, state.sensorStalls)
        assertEquals(0L, state.lastSensorStallAtMs)
    }
}
