package com.ivanna.omega.core

import android.app.Application
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

// Perfiles de fábrica (v2.5.411, tarea 2): una sola fuente de verdad.
// El perfil de arranque es UNA constante (BOOT_PROFILE = Espacial);
// las preferencias existentes no se migran ni se borran.
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], application = Application::class, manifest = Config.NONE)
class FactoryProfileTest {
    private lateinit var context: Context

    @Before fun setup() {
        context = ApplicationProvider.getApplicationContext()
        context.getSharedPreferences("ivanna_omega_params", 0).edit().clear().commit()
    }

    // v2.5.473 (§3.6.4): el arranque de fábrica pasa de ESPACIAL a COMPLETO;
    // el test viejo pedía Espacial y por eso fallaba en CI (2 tests).
    @Test fun bootProfileIsCompleto() {
        assertEquals("Completo", ParameterStore.BOOT_PROFILE.name)
        assertSame(ParameterStore.PROFILE_COMPLETO, ParameterStore.BOOT_PROFILE)
    }

    @Test fun limpioIsHoaHrtfOnly() {
        val p = ParameterStore.PROFILE_LIMPIO
        assertEquals(0f, p.orbeAmount, 0f)
        assertEquals(0f, p.holosAmount, 0f)
        assertEquals(0f, p.omniAmount, 0f)
        assertEquals(0f, p.hyperAmount, 0f)
        assertFalse(p.volterraEnabled)
    }

    @Test fun espacialAddsOrbeAndHolosOnly() {
        val p = ParameterStore.PROFILE_ESPACIAL
        // ATREVIDA 502 (directiva Esse 2026-11-24) — ORBE 1.0 / HOLOS 1.0
        // (antes HOLOS 0.5), spread 1.2, proximity 6.0, diffuse 0.85.
        // Spread 1.2 (no 1.5): acantilado ≥1.3 espeja la imagen (Green).
        assertEquals(1.0f, p.orbeAmount, 0f)
        assertEquals(1.0f, p.holosAmount, 0f)
        assertEquals(0.002f, p.holosHarmonics, 0f)
        assertEquals(1.2f, p.holosSpread, 0f)
        assertEquals(6.0f, p.holosProximityDb, 0f)
        assertEquals(0.85f, p.holosDiffuse, 0f)
        assertEquals(0f, p.omniAmount, 0f)
        assertEquals(0f, p.hyperAmount, 0f)
        assertFalse(p.volterraEnabled)
    }

    @Test fun completoMatchesReadmeDefaults() {
        val p = ParameterStore.PROFILE_COMPLETO
        assertEquals(1.0f, p.orbeAmount, 0f)
        assertEquals(1.0f, p.holosAmount, 0f)
        assertEquals(0.002f, p.holosHarmonics, 0f)
        assertEquals(1.2f, p.holosSpread, 0f)
        assertEquals(6.0f, p.holosProximityDb, 0f)
        assertEquals(0.85f, p.holosDiffuse, 0f)
        assertEquals(1.0f, p.omniAmount, 0f)
        assertEquals(0.5f, p.hyperAmount, 0f)
        assertTrue(p.volterraEnabled)
        // Valores de partida (tarea 2): EQ HOLOS plano, distance 0.35 m,
        // harmonics 0.002 (ATREVIDA 502), hyper_bass_trim 0, Harman 2018 (targetKind 0).
        assertEquals(0f, p.holosEqLow, 0f)
        assertEquals(0f, p.holosEqMid, 0f)
        assertEquals(0f, p.holosEqHigh, 0f)
        assertEquals(0f, p.holosEqPresence, 0f)
        assertEquals(0f, p.holosEqTiltDb, 0f)
        assertEquals(0.35f, p.holosDistanceM, 0f)
        assertEquals(0.35f, p.orbeDistanceM, 0f)
        assertEquals(0f, p.hyperBassTrimDb, 0f)
        assertEquals(0, p.hyperTargetKind)
    }

    @Test fun freshStoreReturnsBootProfileDefaults() {
        val store = ParameterStore(context)
        // v2.5.473 (§3.6.4) — BOOT_PROFILE = PROFILE_COMPLETO (antes ESPACIAL):
        // ORBE 1.0, HOLOS 1.0, OmniField 1.0, Φ 0.5, Volterra ON
        // (ATREVIDA 502, directiva Esse 2026-11-24; antes HOLOS 0.5).
        assertEquals(1.0f, store.loadFusedControls().spatial.amount, 0f)
        assertEquals(1.0f, store.loadFusedControls().field.amount, 0f)
        assertEquals(0.002f, store.getHolosHarmonics(), 0f)
        assertEquals(1.2f, store.getHolosSpread(), 0f)
        assertEquals(6.0f, store.getHolosProximityDb(), 0f)
        assertEquals(0.85f, store.getHolosDiffuse(), 0f)
        assertEquals(1.0f, store.getOmniAmount(), 0f)
        assertEquals(0.5f, store.getHyperAmount(), 0f)
        assertTrue(store.isVolterraEnabled())
        assertEquals(0f, store.getHolosEqLow(), 0f)
        assertEquals(0, store.getHyperTargetKind())
    }

    @Test fun storedPrefsWinOverProfile() {
        val store = ParameterStore(context)
        // Valores vivos de Esse: se respetan, no los pisa el perfil.
        store.setHolosParams(0.359f, 0.35f, 1.0f, 3.0f, 0.5f, 0.298f, 0.5f, 1.0f, 1.0f, 1.0f, 0.34f, true, 24)
        store.setHolosEq(7.8f, -8.1f, -8.6f, 0f, 5.2f, 1000f)
        assertEquals(0.359f, store.loadFusedControls().field.amount, 0f)
        assertEquals(0.298f, store.getHolosHarmonics(), 0f)
        assertEquals(7.8f, store.getHolosEqLow(), 0f)
        assertEquals(-8.1f, store.getHolosEqMid(), 0f)
    }
}
