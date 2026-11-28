// FusedControlsBridgeTest.kt — Unit test del empuje fusionado a Ruta B (v2.5.469).
//
// El defecto: safeSetFusedControls solo escribía g_fusedChainRouteA (Ruta A);
// la Ruta B nunca se enteraba. El fix empuja los valores fusionados al
// daemon por SET_PERCEPTUAL_STATE (buildFusedPerceptualFields +
// pushFusedToDaemon). Este test fija el mapeo fusionado → JSON (campo →
// valor) sin tocar el socket: si el Kotlin cambia un nombre de clave o un
// valor, el test falla. El lado daemon (JSON → snapshot) lo fija
// test_route_b_perceptual_state.cpp (sección 10, mismo JSON).
package com.ivanna.omega.audio

import com.ivanna.omega.magisk.PerceptualStatePayload
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

// Robolectric: el test de outputMode usa PerceptualStatePayload.build*,
// que construye org.json.JSONObject — stub en el android.jar de los unit
// tests sin Robolectric (el put no guarda nada y getDouble devuelve 0.0).
// Mismo patrón que RouteBDoctorTest y PerceptualStateDedupJsonTest.
@RunWith(RobolectricTestRunner::class)
class FusedControlsBridgeTest {

    private fun distinctiveControls(): FusedControls = FusedControls(
        spatial = SpatialControls(
            amount = 1.0f,
            azimuthDeg = 45.0f,
            distanceM = 1.0f,
            width = 1.5f,
            mode = FusedSpatialMode.SPEAKERS,
            wfsSpread = 0.5f,
            hrtfWet = 0.8f,
            headTracking = false,
            xtcEnabled = true,
            upmixEnabled = false,
            upmixImmersivity = 1.25f,
        ),
        field = FieldControls(
            amount = 0.5f,
            phiAmount = 0.5f,
            phiPreset = FusedPhiPreset.ROOM,
            pumping = 0.0f,
            coherence = 0.8f,
            harmonics = 0.001f,
            gamma = 0.75f,
            warmth = 0.3f,
            dynamics = 0.8f,
            roomRt60 = 0.5f,
        ),
        listenerRoom = ListenerRoomControls(
            rt60Sec = 0.4f,
            roomSizeM = 5.0f,
            roomWet = 0.3f,
            hearingProfile = FusedHearingProfile.FLAT,
        ),
        transducer = TransducerControls(
            correction = 1.0f,
            temporalAmount = 0.6f,
            emfEnabled = true,
        ),
        tonal = TonalControls(
            curve = FusedTonalCurve.HARMAN,
            trimDb = 0.0f,
            lowDb = 2.0f,
            midDb = -1.0f,
            highDb = 1.0f,
            loudnessComp = true,
        ),
        dynamics = DynamicsControls(
            punch = 0.7f,
            width = 1.2f,
            safety = 1.0f,
            compThresholdDb = -12.0f,
            compRatio = 4.0f,
            exciter = 0.4f,
            voice = 0.0f,
            upmixImmersivity = 0.0f,
        ),
        brain = BrainControls(
            mode = FusedBrainMode.AUTO,
            adaptation = 0.8f,
        ),
        conservation = ConservationControls(
            amount = 0.6f,
            energyCap = 0.4f,
            phaseCoherence = 0.75f,
            inertia = 0.9f,
        ),
    )

    @Test
    fun `mapeo fusionado a SET_PERCEPTUAL_STATE es exacto`() {
        val fields = FusedControlsBridge.buildFusedPerceptualFields(distinctiveControls())
        // ORBE·Ω
        assertEquals(1.0f, fields["orbeAmount"]!!, 0.0f)
        assertEquals(1.0f, fields["orbeDistanceM"]!!, 0.0f)
        assertEquals(1.5f, fields["orbeSpread"]!!, 0.0f)
        assertEquals(0.0f, fields["orbeHeadTracking"]!!, 0.0f)
        // Unificación 2.5.509 (paso 3): el mapa YA NO lleva outputMode —
        // lo estampa el constructor único (PerceptualStatePayload.buildSet)
        // desde el modo publicado por AudioRouteManager. El mapa no debe
        // contenerlo (si lo llevara, el constructor lo sobrescribiría).
        assertNull(fields["outputMode"])
        assertEquals(0.0f, fields["upmixingEnabled"]!!, 0.0f)
        assertEquals(1.25f, fields["upmixingImmersivity"]!!, 0.0f)
        // HOLOS·Ω
        assertEquals(0.5f, fields["holosAmount"]!!, 0.0f)
        assertEquals(0.001f, fields["holosHarmonics"]!!, 0.0f)
        assertEquals(0.3f, fields["holosWarmth"]!!, 0.0f)
        assertEquals(0.8f, fields["holosDynamics"]!!, 0.0f)
        // gamma 0.75: dentro de [0.5, 1.0] (el daemon clampa ahí; el slider
        // fusionado es 0..2, así que en Ruta B solo [0.5, 1.0] es efectivo).
        assertEquals(0.75f, fields["holosGamma"]!!, 0.0f)
        assertEquals(0.5f, fields["holosRoomRt60"]!!, 0.0f)
        // Φ + Volterra
        assertEquals(0.5f, fields["hyperAmount"]!!, 0.0f)
        assertEquals(1.0f, fields["hyperPreset"]!!, 0.0f)
        assertEquals(1.0f, fields["hyperXtcEnabled"]!!, 0.0f)
        assertEquals(1.0f, fields["volterraEnabled"]!!, 0.0f)
        // ARMONÍA
        assertEquals(2.0f, fields["holosEqLow"]!!, 0.0f)
        assertEquals(-1.0f, fields["holosEqMid"]!!, 0.0f)
        assertEquals(1.0f, fields["holosEqHigh"]!!, 0.0f)
        // PULSO
        assertEquals(1.2f, fields["spatialWidth"]!!, 0.0f)
        assertEquals(-12.0f, fields["compressor"]!!, 0.0f)
        assertEquals(0.4f, fields["harmonicGain"]!!, 0.0f)
        // CONSERVACIÓN PERCEPTUAL (árbitro de campo, ABI v17)
        assertEquals(0.6f, fields["consvAmount"]!!, 0.0f)
        assertEquals(0.4f, fields["consvEnergyCap"]!!, 0.0f)
        assertEquals(0.75f, fields["consvPhaseCoherence"]!!, 0.0f)
        assertEquals(0.9f, fields["consvInertia"]!!, 0.0f)
    }

    @Test
    fun `el constructor unico estampa outputMode 1-2-3 y el mapa no lo lleva`() {
        // Unificación 2.5.509 (paso 3): outputMode lo estampa
        // PerceptualStatePayload.buildSet desde el modo publicado
        // (HEADPHONES 1 / SPEAKERS 2 / GENEZI 3), no el mapa.
        val fields = FusedControlsBridge.buildFusedPerceptualFields(FusedControls())
        assertNull(fields["outputMode"])
        val hp = PerceptualStatePayload.buildSet(fields, RouteBOutput.HEADPHONES)
        assertEquals(1.0, hp.getDouble("outputMode"), 0.0)
        val sp = PerceptualStatePayload.buildSet(fields, RouteBOutput.SPEAKERS)
        assertEquals(2.0, sp.getDouble("outputMode"), 0.0)
        val gz = PerceptualStatePayload.buildSet(fields, RouteBOutput.GENEZI)
        assertEquals(3.0, gz.getDouble("outputMode"), 0.0)
        // El constructor gana aunque el mapa trajera un outputMode viejo.
        val stale = fields.toMutableMap().apply { put("outputMode", 99.0f) }
        val fixed = PerceptualStatePayload.buildSet(stale, RouteBOutput.SPEAKERS)
        assertEquals(2.0, fixed.getDouble("outputMode"), 0.0)
        // buildNeutral: acción SET + mapa neutro + outputMode del modo.
        val neutral = PerceptualStatePayload.buildNeutral(RouteBOutput.HEADPHONES)
        assertEquals("SET_PERCEPTUAL_STATE", neutral.getString("action"))
        assertEquals(1.0, neutral.getDouble("outputMode"), 0.0)
        assertEquals(0.0, neutral.getDouble("orbeAmount"), 0.0)
        assertEquals(0.0, neutral.getDouble("holosAmount"), 0.0)
        // buildSend: acción SEND con el mismo sello de modo.
        val send = PerceptualStatePayload.buildSend(fields, RouteBOutput.GENEZI)
        assertEquals("SEND_PERCEPTUAL_STATE", send.getString("action"))
        assertEquals(3.0, send.getDouble("outputMode"), 0.0)
    }

    @Test
    fun `volterraEnabled es on-off segun correction`() {
        val on = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().copy(transducer = TransducerControls(correction = 0.5f))
        )
        assertEquals(1.0f, on["volterraEnabled"]!!, 0.0f)
        val off = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().copy(transducer = TransducerControls(correction = 0.0f))
        )
        assertEquals(0.0f, off["volterraEnabled"]!!, 0.0f)
    }

    @Test
    fun `orbeSpread clamp a 1_5 aunque width llegue a 2`() {
        val fields = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().copy(spatial = SpatialControls(width = 2.0f))
        )
        assertEquals(1.5f, fields["orbeSpread"]!!, 0.0f)
    }

    @Test
    fun `campos sin equivalente en SET_PERCEPTUAL_STATE no se envian`() {
        val fields = FusedControlsBridge.buildFusedPerceptualFields(distinctiveControls())
        // Estos necesitan su propia acción del daemon (SET_WFS,
        // SET_TEMPORAL_FIELD, SET_PUNCH, SET_ADAPTIVE_STATE, SET_INTENSITY,
        // SET_NPE, SET_XTC, SET_ROOM_RT60, SET_AUDIO_FEATURES) o no tienen
        // campo en el snapshot; no deben colarse en el SET_PERCEPTUAL_STATE.
        assertFalse(fields.containsKey("azimuthDeg"))
        assertFalse(fields.containsKey("wfsSpread"))
        assertFalse(fields.containsKey("wfsEnabled"))
        assertFalse(fields.containsKey("hrtfWet"))
        assertFalse(fields.containsKey("hrtfEnabled"))
        assertFalse(fields.containsKey("temporalAmount"))
        assertFalse(fields.containsKey("temporalEnabled"))
        assertFalse(fields.containsKey("punchAmount"))
        assertFalse(fields.containsKey("compAmount"))
        assertFalse(fields.containsKey("compRatio"))
        assertFalse(fields.containsKey("intensity"))
        assertFalse(fields.containsKey("npeEnabled"))
        assertFalse(fields.containsKey("xtcEnabled"))
        assertFalse(fields.containsKey("roomRt60S"))
        assertFalse(fields.containsKey("roomWet"))
        assertFalse(fields.containsKey("roomSizeM"))
        assertFalse(fields.containsKey("hearingProfile"))
        assertFalse(fields.containsKey("trimDb"))
        assertFalse(fields.containsKey("safety"))
        assertFalse(fields.containsKey("voice"))
        assertFalse(fields.containsKey("pumping"))
        assertFalse(fields.containsKey("coherence"))
        assertTrue(fields.isNotEmpty())
    }

    @Test
    fun `neutro empuja amounts a cero y widths a uno`() {
        val fields = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().setNeutral()
        )
        assertEquals(0.0f, fields["orbeAmount"]!!, 0.0f)
        assertEquals(0.0f, fields["holosAmount"]!!, 0.0f)
        assertEquals(0.0f, fields["hyperAmount"]!!, 0.0f)
        assertEquals(0.0f, fields["volterraEnabled"]!!, 0.0f)
        assertEquals(0.0f, fields["holosEqLow"]!!, 0.0f)
        assertEquals(0.0f, fields["holosEqMid"]!!, 0.0f)
        assertEquals(0.0f, fields["holosEqHigh"]!!, 0.0f)
        assertEquals(1.0f, fields["spatialWidth"]!!, 0.0f)
        assertEquals(0.0f, fields["harmonicGain"]!!, 0.0f)
        // CONSERVACIÓN PERCEPTUAL: neutro = apagada (amount 0), resto a defaults.
        assertEquals(0.0f, fields["consvAmount"]!!, 0.0f)
        assertEquals(1.0f, fields["consvEnergyCap"]!!, 0.0f)
        assertEquals(0.5f, fields["consvPhaseCoherence"]!!, 0.0f)
        assertEquals(0.82f, fields["consvInertia"]!!, 0.0f)
    }

    @Test
    fun `gemelo en SPEAKERS empuja defaults de conservacion cuando no estan fijados`() {
        // Caza 2026-11-22: el snapshot marcaba consv=0.0 aunque el gemelo
        // SÍ actuaba dentro de la FusedChain (el 0.0 es la entrada, no el
        // efectivo). El push aplica aquí los mismos defaults del gemelo
        // (FusedChain §8: amount 0.5 / techo 0.8) para que el snapshot lo
        // muestre y se pueda verificar. En audífonos no aplica.
        val sp = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().copy(spatial = SpatialControls(mode = FusedSpatialMode.SPEAKERS))
        )
        assertEquals(0.5f, sp["consvAmount"]!!, 0.0f)
        assertEquals(0.8f, sp["consvEnergyCap"]!!, 0.0f)
        val hp = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().copy(spatial = SpatialControls(mode = FusedSpatialMode.HEADPHONES))
        )
        assertEquals(0.0f, hp["consvAmount"]!!, 0.0f)
        assertEquals(1.0f, hp["consvEnergyCap"]!!, 0.0f)
        // El amount del usuario (> 0) y un techo fijado (< 1.0) mandan.
        val user = FusedControlsBridge.buildFusedPerceptualFields(
            FusedControls().copy(
                spatial = SpatialControls(mode = FusedSpatialMode.SPEAKERS),
                conservation = ConservationControls(amount = 0.6f, energyCap = 0.4f),
            )
        )
        assertEquals(0.6f, user["consvAmount"]!!, 0.0f)
        assertEquals(0.4f, user["consvEnergyCap"]!!, 0.0f)
    }
}
