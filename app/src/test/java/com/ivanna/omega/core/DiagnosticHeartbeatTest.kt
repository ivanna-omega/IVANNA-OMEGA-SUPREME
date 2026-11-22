/*
 * © 2026 Luis Uriel Pimentel Pérez — IVANNA N-P-E
 * DiagnosticHeartbeatTest.kt — Latido PUSH F8 (v2.5.446).
 *
 * Prueba el latido que la app manda al puente Floot de diagnóstico
 * (https://ivanna-diagnostic-bridge.floot.app): formato exacto, ausencia
 * de secretos, cadencia de 60 s (más lenta que el snapshot de 15 s) y el
 * gancho de alcance del gateway OmniRoute.
 * Sin red: se prueba el constructor puro buildHeartbeat() y la recolección
 * de salud en memoria; el POST real solo ocurre en el teléfono con el
 * opt-in activado. org.json es un stub en la JVM sin Robolectric (mismo
 * patrón que TidalApiParsingTest / AppUpdateCheckerTest): por eso esta
 * clase usa Robolectric.
 */
package com.ivanna.omega.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class DiagnosticHeartbeatTest {

    private fun sampleHealth() = LiveDiagnosticConnector.HeartbeatHealth(
        gatewayReachable = true,
        gatewayKnown = true,
        dspAlive = true,
        sessionActive = false
    )

    @Test fun heartbeatExactFormat() {
        val heartbeat = LiveDiagnosticConnector.buildHeartbeat(
            appVersion = "2.5.445",
            health = sampleHealth(),
            sequence = 7L,
            uptimeMs = 123_456L,
            timestampMs = 1_793_000_000_000L
        )
        assertEquals("heartbeat", heartbeat.getString("type"))
        assertEquals(1, heartbeat.getInt("schemaVersion"))
        assertEquals(7L, heartbeat.getLong("sequence"))
        assertEquals(1_793_000_000_000L, heartbeat.getLong("timestampMs"))
        assertEquals(123_456L, heartbeat.getLong("uptimeMs"))
        assertEquals("2.5.445", heartbeat.getString("appVersion"))

        val health = heartbeat.getJSONObject("health")
        assertTrue(health.getBoolean("gatewayReachable"))
        assertTrue(health.getBoolean("gatewayKnown"))
        assertTrue(health.getBoolean("dspAlive"))
        assertFalse(health.getBoolean("sessionActive"))
    }

    @Test fun heartbeatCarriesNoSecrets() {
        // El latido lleva EXACTAMENTE estas claves: ni token, ni endpoint,
        // ni métricas de detalle, ni identificadores. Si alguien añade un
        // campo, este test lo caza.
        val heartbeat = LiveDiagnosticConnector.buildHeartbeat(
            appVersion = "2.5.445",
            health = sampleHealth(),
            sequence = 1L,
            uptimeMs = 1L,
            timestampMs = 1L
        )
        val topKeys = mutableSetOf<String>()
        heartbeat.keys().forEach { topKeys.add(it) }
        assertEquals(
            setOf("type", "schemaVersion", "sequence", "timestampMs", "uptimeMs", "appVersion", "health"),
            topKeys
        )
        val healthKeys = mutableSetOf<String>()
        heartbeat.getJSONObject("health").keys().forEach { healthKeys.add(it) }
        assertEquals(
            setOf("gatewayReachable", "gatewayKnown", "dspAlive", "sessionActive"),
            healthKeys
        )
        val serialized = heartbeat.toString()
        assertFalse(serialized.contains("token", ignoreCase = true))
        assertFalse(serialized.contains("Bearer"))
        assertFalse(serialized.contains("Authorization"))
        assertFalse(serialized.contains("metrics"))
        assertFalse(serialized.contains("\"agent\""))
    }

    @Test fun heartbeatSequenceIsCarriedVerbatim() {
        val first = LiveDiagnosticConnector.buildHeartbeat(
            appVersion = "2.5.445", health = sampleHealth(),
            sequence = 41L, uptimeMs = 1L, timestampMs = 1L
        )
        val second = LiveDiagnosticConnector.buildHeartbeat(
            appVersion = "2.5.445", health = sampleHealth(),
            sequence = 42L, uptimeMs = 2L, timestampMs = 2L
        )
        assertEquals(41L, first.getLong("sequence"))
        assertEquals(42L, second.getLong("sequence"))
        assertTrue(second.getLong("sequence") > first.getLong("sequence"))
    }

    @Test fun heartbeatCadenceIsSixtySeconds() {
        // Más lenta que el snapshot de diagnóstico (15 s): el latido es
        // solo "estoy vivo", no telemetría. 60 s = 1/min; junto al snapshot
        // (4/min) queda muy por debajo del límite del puente (60 req/min/IP).
        assertEquals(60_000L, LiveDiagnosticConnector.HEARTBEAT_PERIOD_MS)
    }

    @Test fun gatewayHookReportsReachability() {
        // El gancho que IvannaGeminiAgent debe llamar desde su health-check.
        LiveDiagnosticConnector.reportGatewayReachable(true)
        var collected = LiveDiagnosticConnector.collectHeartbeatHealth()
        assertTrue(collected.gatewayKnown)
        assertTrue(collected.gatewayReachable)

        LiveDiagnosticConnector.reportGatewayReachable(false)
        collected = LiveDiagnosticConnector.collectHeartbeatHealth()
        assertTrue(collected.gatewayKnown)
        assertFalse(collected.gatewayReachable)
    }
}
