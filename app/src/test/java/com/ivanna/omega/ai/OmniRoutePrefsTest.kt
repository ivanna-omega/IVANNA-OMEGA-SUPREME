package com.ivanna.omega.ai

import com.ivanna.omega.assistant.core.OmniRoutePrefs
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

/**
 * Tests de OmniRoutePrefs (v2.5.509, auto-enlace).
 *
 * Solo funciones puras (sin Context, sin red): el camino vivo se
 * verifica contra el archivo real de descubrimiento por curl
 * (GET raw.githubusercontent.com/.../omniroute-url/omniroute.json).
 *
 * Robolectric: org.json.JSONObject es un stub en el android.jar de los
 * unit tests (igual que OmniRouteOrchestratorTest).
 */
@RunWith(RobolectricTestRunner::class)
class OmniRoutePrefsTest {

    // ── Descubrimiento ───────────────────────────────────────────────────

    @Test fun discoveryUrlPointsAtRepoBranch() {
        assertEquals(
            "https://raw.githubusercontent.com/ivanna-omega/IVANNA-OMEGA-SUPREME/omniroute-url/omniroute.json",
            OmniRoutePrefs.DISCOVERY_URL
        )
    }

    @Test fun discoveredUrlParses() {
        assertEquals(
            "https://abc123def456.app.lhr.life/v1",
            OmniRoutePrefs.parseDiscoveredBaseUrl(
                """{"omniroute_url": "https://abc123def456.app.lhr.life/v1"}"""
            )
        )
    }

    @Test fun discoveredUrlAddsMissingV1() {
        assertEquals(
            "https://abc123def456.app.lhr.life/v1",
            OmniRoutePrefs.parseDiscoveredBaseUrl(
                """{"omniroute_url": "https://abc123def456.app.lhr.life"}"""
            )
        )
    }

    @Test fun discoveredUrlRejectsGarbage() {
        assertNull(OmniRoutePrefs.parseDiscoveredBaseUrl("no-json"))
        assertNull(OmniRoutePrefs.parseDiscoveredBaseUrl("{}"))
        assertNull(OmniRoutePrefs.parseDiscoveredBaseUrl("""{"omniroute_url": ""}"""))
        assertNull(OmniRoutePrefs.parseDiscoveredBaseUrl("""{"omniroute_url": "ftp://x/v1"}"""))
        assertNull(OmniRoutePrefs.parseDiscoveredBaseUrl("""{"other": "https://x/v1"}"""))
        assertNull(OmniRoutePrefs.parseDiscoveredBaseUrl("""{"omniroute_url": "https://v1"}"""))
    }

    // ── Normalización ────────────────────────────────────────────────────

    @Test fun normalizeGatewayUrlTrimsAndValidates() {
        assertEquals(
            "https://abc123def456.app.lhr.life/v1",
            OmniRoutePrefs.normalizeGatewayUrl("  https://abc123def456.app.lhr.life/v1/ ")
        )
        assertNull(OmniRoutePrefs.normalizeGatewayUrl(""))
        assertNull(OmniRoutePrefs.normalizeGatewayUrl("https://"))
        // http solo para localhost (pruebas locales del gateway).
        assertNull(OmniRoutePrefs.normalizeGatewayUrl("http://example.com/v1"))
        assertEquals(
            "http://localhost:20128/v1",
            OmniRoutePrefs.normalizeGatewayUrl("http://localhost:20128")
        )
    }

    // ── Túnel ────────────────────────────────────────────────────────────

    @Test fun isTunnelUrlOnlyMatchesLocalhostRun() {
        assertTrue(OmniRoutePrefs.isTunnelUrl("https://abc123def456.app.lhr.life/v1"))
        assertFalse(OmniRoutePrefs.isTunnelUrl("https://mi-gateway.example.com/v1"))
        assertFalse(OmniRoutePrefs.isTunnelUrl(""))
    }
}
