// FusedControlsPersistenceTest.kt — Round-trip de FusedControls en ParameterStore (v2.5.470).
//
// Antes de la 470 los sliders de FUSIÓN vivían solo en memoria (remember en
// FusedOperatorsScreen): un reinicio de la app revertía los valores
// fusionados a los legacy. El fix persiste el FusedControls completo en
// SharedPreferences ("ivanna_omega_params", claves "fused_*") al cambiar y
// lo restaura al arrancar (PersistedStateRestorer, al final del restore).
// Este test fija el round-trip: cada campo de los 7 operadores (incluidos
// los enums y las 10 bandas de corrección de sala) sobrevive a
// save → load, los defaults vuelven cuando no hay nada guardado, y los
// valores fuera de rango se clampan.
// © 2026 Luis Uriel Pimentel Pérez
package com.ivanna.omega.core

import android.app.Application
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.ivanna.omega.audio.BrainControls
import com.ivanna.omega.audio.ConservationControls
import com.ivanna.omega.audio.DynamicsControls
import com.ivanna.omega.audio.FieldControls
import com.ivanna.omega.audio.FusedBrainMode
import com.ivanna.omega.audio.FusedControls
import com.ivanna.omega.audio.FusedHearingProfile
import com.ivanna.omega.audio.FusedPhiPreset
import com.ivanna.omega.audio.FusedSpatialMode
import com.ivanna.omega.audio.FusedTonalCurve
import com.ivanna.omega.audio.ListenerRoomControls
import com.ivanna.omega.audio.SpatialControls
import com.ivanna.omega.audio.TonalControls
import com.ivanna.omega.audio.TransducerControls
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], application = Application::class, manifest = Config.NONE)
class FusedControlsPersistenceTest {

    private lateinit var context: Context
    private lateinit var store: ParameterStore

    @Before
    fun setup() {
        context = ApplicationProvider.getApplicationContext()
        context.getSharedPreferences("ivanna_omega_params", 0).edit().clear().commit()
        store = ParameterStore(context)
    }

    /** Valores distintivos en TODOS los campos (ninguno es el default). */
    private fun distinctiveControls(): FusedControls = FusedControls(
        spatial = SpatialControls(
            amount = 0.7f,
            azimuthDeg = -45.0f,
            distanceM = 1.5f,
            width = 1.4f,
            mode = FusedSpatialMode.SPEAKERS,
            wfsSpread = 0.6f,
            hrtfWet = 0.3f,
            headTracking = true,
            xtcEnabled = true,
            upmixEnabled = true,
            upmixImmersivity = 1.8f,
        ),
        field = FieldControls(
            amount = 0.8f,
            phiAmount = 0.25f,
            phiPreset = FusedPhiPreset.CAR,
            pumping = 0.1f,
            coherence = 0.9f,
            harmonics = 0.0015f,
            gamma = 1.5f,
            warmth = 0.4f,
            dynamics = 0.7f,
            roomRt60 = 0.8f,
        ),
        listenerRoom = ListenerRoomControls(
            rt60Sec = 0.9f,
            roomSizeM = 8.0f,
            roomWet = 0.5f,
            hearingProfile = FusedHearingProfile.HARMAN,
            roomCorrDb = listOf(-1f, -2f, -3f, -4f, -5f, -6f, -7f, -8f, -9f, -10f),
        ),
        transducer = TransducerControls(
            correction = 0.6f,
            temporalAmount = 0.3f,
            emfEnabled = false,
        ),
        tonal = TonalControls(
            curve = FusedTonalCurve.ELITE,
            trimDb = 2.0f,
            lowDb = 3.0f,
            midDb = -2.0f,
            highDb = 1.0f,
            loudnessComp = false,
        ),
        dynamics = DynamicsControls(
            punch = 0.5f,
            width = 1.5f,
            safety = 0.8f,
            compThresholdDb = -20.0f,
            compRatio = 4.0f,
            exciter = 0.2f,
            voice = 0.6f,
            upmixImmersivity = 1.1f,
        ),
        brain = BrainControls(
            mode = FusedBrainMode.MANUAL,
            adaptation = 0.0f,
        ),
        conservation = ConservationControls(
            amount = 0.6f,
            energyCap = 0.4f,
            phaseCoherence = 0.75f,
            inertia = 0.9f,
        ),
    )

    @Test
    fun roundTripPreservesAllFields() {
        val input = distinctiveControls()
        store.saveFusedControls(input)
        // Una segunda instancia (como un reinicio) lee lo mismo.
        val reloaded = ParameterStore(context).loadFusedControls()
        assertEquals(input.clamp(), reloaded)
    }

    @Test
    fun loadWithNothingSavedReturnsDefaults() {
        assertEquals(FusedControls(), store.loadFusedControls())
    }

    @Test
    fun saveClampsOutOfRangeValues() {
        val wild = FusedControls(
            spatial = SpatialControls(amount = 5.0f, azimuthDeg = 999.0f, distanceM = 99.0f),
            field = FieldControls(gamma = 99.0f, harmonics = 1.0f),
            tonal = TonalControls(trimDb = 99.0f),
            dynamics = DynamicsControls(compRatio = 99.0f),
            conservation = ConservationControls(amount = 5.0f, energyCap = -1.0f, inertia = 5.0f),
        )
        store.saveFusedControls(wild)
        assertEquals(wild.clamp(), store.loadFusedControls())
    }

    @Test
    fun neutralRoundTrip() {
        val neutral = FusedControls().setNeutral()
        store.saveFusedControls(neutral)
        assertEquals(neutral, ParameterStore(context).loadFusedControls())
    }

    @Test
    fun saveOverwritesPreviousValues() {
        store.saveFusedControls(distinctiveControls())
        store.saveFusedControls(FusedControls())
        assertEquals(FusedControls(), store.loadFusedControls())
    }
}
