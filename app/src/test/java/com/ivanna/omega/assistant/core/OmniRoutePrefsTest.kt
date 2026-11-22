package com.ivanna.omega.assistant.core

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Tests de OmniRoutePrefs (v2.5.471, Repair 2: sin red personal en el APK).
 *
 * - DEFAULT_BASE_URL es "" (ninguna dirección hardcodeada).
 * - El placeholder de la UI muestra solo el formato, nunca una IP real.
 * - La migración borra una URL guardada de la red personal (host del
 *   rango CGNAT) y deja la URL vacía; las URLs del usuario se
 *   conservan intactas.
 * - La detección legacy es por patrón (host CGNAT): la dirección vieja
 *   no aparece en ningún literal del código ni del test.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class OmniRoutePrefsTest {

    private lateinit var context: Context

    @Before
    fun setUp() {
        context = ApplicationProvider.getApplicationContext()
        context.getSharedPreferences("ivanna_llm_provider", Context.MODE_PRIVATE)
            .edit().clear().apply()
    }

    @Test fun defaultBaseUrlIsEmpty() {
        assertEquals("", OmniRoutePrefs.DEFAULT_BASE_URL)
    }

    @Test fun getBaseUrlIsEmptyWhenNothingStored() {
        assertEquals("", OmniRoutePrefs.getBaseUrl(context))
        assertFalse(OmniRoutePrefs.isBaseUrlConfigured(context))
    }

    @Test fun placeholderShowsFormatOnly() {
        assertEquals("http://TU-GATEWAY:20128/v1", OmniRoutePrefs.BASE_URL_PLACEHOLDER)
        assertFalse(OmniRoutePrefs.BASE_URL_PLACEHOLDER.contains("100."))
    }

    @Test fun setAndGetRoundTripsWithoutTrailingSlash() {
        OmniRoutePrefs.setBaseUrl(context, "https://mi-gateway.example.com:20128/v1/")
        assertEquals("https://mi-gateway.example.com:20128/v1", OmniRoutePrefs.getBaseUrl(context))
        assertTrue(OmniRoutePrefs.isBaseUrlConfigured(context))
    }

    @Test fun blankBaseUrlStaysBlank() {
        OmniRoutePrefs.setBaseUrl(context, "https://mi-gateway.example.com/v1")
        OmniRoutePrefs.setBaseUrl(context, "   ")
        assertEquals("", OmniRoutePrefs.getBaseUrl(context))
        assertFalse(OmniRoutePrefs.isBaseUrlConfigured(context))
    }

    @Test fun legacyUrlIsDetectedByPattern() {
        assertTrue(OmniRoutePrefs.isLegacyBaseUrl("http://100.99.1.2:20128/v1"))
        assertTrue(OmniRoutePrefs.isLegacyBaseUrl("http://100.127.0.5:20128/v1"))
        assertTrue(OmniRoutePrefs.isLegacyBaseUrl("https://100.64.0.1/v1"))
        assertTrue(OmniRoutePrefs.isLegacyBaseUrl("100.99.1.2:8080/v1"))
    }

    @Test fun normalUrlsAreNotLegacy() {
        assertFalse(OmniRoutePrefs.isLegacyBaseUrl("https://mi-gateway.example.com/v1"))
        assertFalse(OmniRoutePrefs.isLegacyBaseUrl("http://192.168.1.10:20128/v1"))
        assertFalse(OmniRoutePrefs.isLegacyBaseUrl("http://localhost:20128/v1"))
        assertFalse(OmniRoutePrefs.isLegacyBaseUrl(""))
    }

    @Test fun migrateLegacyBaseUrlWipesOldNetwork() {
        OmniRoutePrefs.setBaseUrl(context, "http://100.99.1.2:20128/v1")
        assertTrue(OmniRoutePrefs.migrateLegacyBaseUrl(context))
        assertEquals("", OmniRoutePrefs.getBaseUrl(context))
        // El diálogo de primera apertura debe volver a pedirse.
        assertFalse(OmniRoutePrefs.isGatewaySetupDone(context))
        // Idempotente: la segunda vez ya no hay nada que borrar.
        assertFalse(OmniRoutePrefs.migrateLegacyBaseUrl(context))
    }

    @Test fun migrateKeepsUserUrl() {
        OmniRoutePrefs.setBaseUrl(context, "https://mi-gateway.example.com/v1")
        assertFalse(OmniRoutePrefs.migrateLegacyBaseUrl(context))
        assertEquals("https://mi-gateway.example.com/v1", OmniRoutePrefs.getBaseUrl(context))
    }
}
