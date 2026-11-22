// TransducerProfileTest.kt
// ============================================================================
// PRUEBAS — Perfil de transductor (FASE 2, v2.5.462)
// © 2026 Luis Uriel Pimentel Pérez
// ============================================================================
//
// Tres modos, no dos: el perfil Genezi solo aplica a la cadena DAC USB →
// Genezi. Cualquier otra bocina es SPEAKERS_GENERIC = neutro (el kernel
// Volterra Genezi y el XTC calibrado para los dos bafles del Genezi NO
// deben aplicarse a una bocina que no es el Genezi).
//
// HEADPHONES: orbe/holos/hyper del usuario, XTC apagado (en audífonos no
// hay diafonía acústica que cancelar), Volterra del usuario con kernel
// PZ1D, Aux Field apagado. SPEAKERS_GENEZI (v2.5.483): orbe/holos DEL
// USUARIO (el holograma del Genezi; antes 0 — el perfil envenenaba
// desired.orbe y cada re-seed por cambio de sesión devolvía ORBE 0,
// hallazgo Esse 2026-11-20 05:18 CST), hyper del usuario, XTC
// encendido, Volterra del usuario con kernel Genezi, Aux Field
// encendido con cruce clampado a 80–120 Hz. SPEAKERS_GENERIC: todo
// neutro (comportamiento vigente para bocinas).

package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class TransducerProfileTest {

    private val orbeUser = 1.0f
    private val holosUser = 0.5f
    private val hyperUser = 0.5f
    private val hyperPresetUser = 1

    private fun profileForMode(mode: TransducerMode) =
        TransducerProfileFactory.profileFor(
            mode, orbeUser, holosUser, hyperUser, hyperPresetUser,
            true, 120f, 1.0f
        )

    @Test fun resolveUsbDacHeadphones() {
        assertEquals(
            TransducerMode.HEADPHONES,
            TransducerProfileFactory.resolveMode(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun resolveUsbDacSpeakersIsGenezi() {
        assertEquals(
            TransducerMode.SPEAKERS_GENEZI,
            TransducerProfileFactory.resolveMode(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.SPEAKERS)
        )
    }

    @Test fun resolveNonUsbSpeakerIsGeneric() {
        assertEquals(
            TransducerMode.SPEAKERS_GENERIC,
            TransducerProfileFactory.resolveMode(
                intArrayOf(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER), UsbDacTarget.HEADPHONES)
        )
        // A2DP es audífono aunque el selector diga SPEAKERS.
        assertEquals(
            TransducerMode.HEADPHONES,
            TransducerProfileFactory.resolveMode(
                intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            TransducerMode.SPEAKERS_GENERIC,
            TransducerProfileFactory.resolveMode(intArrayOf(), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun headphonesProfileIsBinauralPure() {
        val p = profileForMode(TransducerMode.HEADPHONES)
        assertEquals(orbeUser, p.orbeAmount, 0f)
        assertEquals(holosUser, p.holosAmount, 0f)
        assertEquals(hyperUser, p.hyperAmount, 0f)
        assertEquals(hyperPresetUser, p.hyperPreset)
        assertFalse(p.hyperXtcEnabled)
        assertTrue(p.volterraEnabled)
        assertEquals(VolterraKernel.PZ1D, p.volterraKernel)
        assertFalse(p.auxFieldEnabled)
    }

    @Test fun resolveUsbHeadsetSpeakersIsGenezi() {
        // v2.5.483: el JieLi enumera como USB_HEADSET (dumpsys 2026-11-20)
        // y con selector SPEAKERS es la cadena del Genezi, no audífonos.
        assertEquals(
            TransducerMode.SPEAKERS_GENEZI,
            TransducerProfileFactory.resolveMode(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            TransducerMode.HEADPHONES,
            TransducerProfileFactory.resolveMode(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun geneziProfilePassesUserBinauralEnablesSpeakerChain() {
        // v2.5.483 (corrige geneziProfileForbidsBinaural): ORBE/HOLOS
        // del usuario. CASO ORBE 05:18 CST: stored 1.0/0.5 → vivo 1.0/0.5
        // (orbeUser/holosUser de este test SON esos valores guardados):
        // el perfil es el último escritor en restore()/pushRouteBState y
        // con 0f envenenaba desired.orbe; con el usuario, cada re-seed
        // por cambio de sesión restaura el holograma en vez de matarlo.
        val p = profileForMode(TransducerMode.SPEAKERS_GENEZI)
        assertEquals(orbeUser, p.orbeAmount, 0f)
        assertEquals(holosUser, p.holosAmount, 0f)
        assertEquals(hyperUser, p.hyperAmount, 0f)
        assertEquals(hyperPresetUser, p.hyperPreset)
        assertTrue(p.hyperXtcEnabled)
        assertTrue(p.volterraEnabled)
        assertEquals(VolterraKernel.GENEZI, p.volterraKernel)
        assertTrue(p.auxFieldEnabled)
        assertEquals(120f, p.auxXoverHz, 0f)
        assertEquals(1.0f, p.auxCoherence, 0f)
    }

    @Test fun geneziXoverClampsToSpecRange() {
        // El cruce del Genezi es 80–120 Hz: fuera de rango se clampa.
        val high = TransducerProfileFactory.profileFor(
            TransducerMode.SPEAKERS_GENEZI, orbeUser, holosUser, hyperUser,
            hyperPresetUser, true, 200f, 1.0f
        )
        assertEquals(120f, high.auxXoverHz, 0f)
        val low = TransducerProfileFactory.profileFor(
            TransducerMode.SPEAKERS_GENEZI, orbeUser, holosUser, hyperUser,
            hyperPresetUser, true, 60f, 1.0f
        )
        assertEquals(80f, low.auxXoverHz, 0f)
        val mid = TransducerProfileFactory.profileFor(
            TransducerMode.SPEAKERS_GENEZI, orbeUser, holosUser, hyperUser,
            hyperPresetUser, true, 100f, 0.8f
        )
        assertEquals(100f, mid.auxXoverHz, 0f)
        assertEquals(0.8f, mid.auxCoherence, 0f)
        val overCoh = TransducerProfileFactory.profileFor(
            TransducerMode.SPEAKERS_GENEZI, orbeUser, holosUser, hyperUser,
            hyperPresetUser, true, 100f, 1.5f
        )
        assertEquals(1.0f, overCoh.auxCoherence, 0f)
    }

    @Test fun genericSpeakerProfileIsNeutral() {
        val p = profileForMode(TransducerMode.SPEAKERS_GENERIC)
        assertEquals(0f, p.orbeAmount, 0f)
        assertEquals(0f, p.holosAmount, 0f)
        assertEquals(0f, p.hyperAmount, 0f)
        assertFalse(p.hyperXtcEnabled)
        assertFalse(p.volterraEnabled)
        assertFalse(p.auxFieldEnabled)
    }

    @Test fun volterraUserOffStaysOff() {
        // El on/off de Volterra lo decide el usuario; el modo solo elige
        // el kernel. Si el usuario lo apagó, sigue apagado en ambos modos.
        val hp = TransducerProfileFactory.profileFor(
            TransducerMode.HEADPHONES, orbeUser, holosUser, hyperUser,
            hyperPresetUser, false, 120f, 1.0f
        )
        assertFalse(hp.volterraEnabled)
        val gz = TransducerProfileFactory.profileFor(
            TransducerMode.SPEAKERS_GENEZI, orbeUser, holosUser, hyperUser,
            hyperPresetUser, false, 120f, 1.0f
        )
        assertFalse(gz.volterraEnabled)
        assertEquals(VolterraKernel.GENEZI, gz.volterraKernel)
    }
}
