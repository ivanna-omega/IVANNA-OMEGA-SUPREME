package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Puerta de Ruta B por dispositivo (0032, v2.5.460; tercer modo v2.5.483):
 * RouteBDeviceGate.gate es pura (sobre tipos de dispositivo) y se testea
 * en host.
 *
 * El caso real (verificado en vivo 2026-11-16 16:00-16:21 CST, Esse):
 * adaptador DAC USB-C JieLi → Genezi. v2.5.483: tercer modo GENEZI
 * (DAC USB con selector SPEAKERS): los montos BINAURALES del usuario
 * pasan (ORBE 1.0 / HOLOS 1.0) y los GENERALES van a neutro (compressor
 * 0, loudness 0, exciterRed 0 — protección 0032 intacta). El JieLi
 * enumera como USB_HEADSET (dumpsys audio 2026-11-20: "USB Headset
 * Out"), a veces solo, a veces con USB_DEVICE: el selector decide para
 * cualquier endpoint USB, no el tipo. Con audífonos BT (A2DP) los
 * montos del usuario pasan tal cual (binaural Y general).
 *
 * Montos de usuario de los tests = los defaults reales del teléfono de
 * Esse: ORBE 1.0, HOLOS 1.0, spatialWidth 1.55, harmonicGain 0.78,
 * antiDolby 0.85 (PersistedStateRestorer + BOOT_PROFILE; ATREVIDA 502,
 * directiva 2026-11-24 — antes HOLOS 0.5).
 */
class RouteBDeviceGateTest {

    private val userAmounts = RouteBBinauralAmounts(
        orbeAmount = 1.0f,
        holosAmount = 1.0f,
        spatialWidth = 1.55f,
        harmonicGain = 0.78f,
        antiDolby = 0.85f
    )

    @Test fun usbDeviceGeneziPassesBinaural() {
        // v2.5.483 tercer modo: el DAC JieLi de Esse (TYPE_USB_DEVICE,
        // selector SPEAKERS por defecto) → GENEZI: el holograma pasa,
        // lo general va a neutro (ver speakersNeutralGeneralAmounts).
        val gated = RouteBDeviceGate.gate(userAmounts, intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE))
        assertEquals(userAmounts, gated)
        assertEquals(1.0f, gated.orbeAmount, 0f)
        assertEquals(1.0f, gated.holosAmount, 0f)
        assertEquals(1.55f, gated.spatialWidth, 0f)
        assertEquals(0.78f, gated.harmonicGain, 0f)
        assertEquals(0.85f, gated.antiDolby, 0f)
    }

    @Test fun builtinSpeakerIsNeutral() {
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER))
        assertEquals(ROUTE_B_NEUTRAL_AMOUNTS, gated)
    }

    @Test fun a2dpKeepsUserAmounts() {
        // Audífonos BT: los montos del usuario pasan intactos.
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP))
        assertEquals(userAmounts, gated)
    }

    @Test fun wiredHeadphonesKeepUserAmounts() {
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_WIRED_HEADPHONES))
        assertEquals(userAmounts, gated)
    }

    @Test fun wiredHeadsetKeepsUserAmounts() {
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_WIRED_HEADSET))
        assertEquals(userAmounts, gated)
    }

    @Test fun usbHeadsetKeepsUserAmounts() {
        // USB_HEADSET con selector SPEAKERS (default) → GENEZI: el
        // JieLi enumera como usb_headset (dumpsys 2026-11-20) y el
        // holograma pasa. Con selector HEADPHONES también pasa (audífono
        // real en el adaptador): el resultado binaural es el mismo; lo
        // que cambia es lo general (ver gateGeneral).
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET))
        assertEquals(userAmounts, gated)
    }

    @Test fun hearingAidKeepsUserAmounts() {
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_HEARING_AID))
        assertEquals(userAmounts, gated)
    }

    @Test fun noDevicesIsNeutral() {
        // Sin dispositivo determinable → neutro (fail-closed).
        val gated = RouteBDeviceGate.gate(userAmounts, intArrayOf())
        assertEquals(ROUTE_B_NEUTRAL_AMOUNTS, gated)
    }

    @Test fun usbDeviceWinsOverStaleA2dp() {
        // DAC USB conectado + audífono BT conectado (sin usar): gana USB
        // (precedencia OutputRoutePolicy) → GENEZI (v2.5.483): el
        // holograma del usuario pasa por el DAC.
        val gated = RouteBDeviceGate.gate(
            userAmounts,
            intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
        )
        assertEquals(userAmounts, gated)
    }

    @Test fun a2dpWinsOverSpeaker() {
        // Audífono BT + bocina integrada (siempre listada): gana BT.
        val gated = RouteBDeviceGate.gate(
            userAmounts,
            intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP, AudioDeviceInfo.TYPE_BUILTIN_SPEAKER)
        )
        assertEquals(userAmounts, gated)
    }

    @Test fun hdmiIsNeutral() {
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_HDMI))
        assertEquals(ROUTE_B_NEUTRAL_AMOUNTS, gated)
    }

    @Test fun lineAnalogIsNeutral() {
        // Salida de línea analógica (al Genezi o a bocinas) → neutro.
        val gated = RouteBDeviceGate.gate(
            userAmounts, intArrayOf(AudioDeviceInfo.TYPE_LINE_ANALOG))
        assertEquals(ROUTE_B_NEUTRAL_AMOUNTS, gated)
    }

    // ── Extensión 0032: montos generales (compresor/loudness/exciter) ──
    // Hallazgo Esse 2026-11-16 18:26 CST: la caída de volumen INMEDIATA
    // al prender la APK era el compresor (−5.5) y el loudness (−16 LUFS),
    // no solo ORBE/HOLOS/widener. En bocinas también van a neutro.
    private val userGeneral = RouteBGeneralAmounts(
        compressor = -5.5f,
        exciterRed = 0.15f,
        loudnessTarget = -16.0f
    )

    @Test fun neutralGeneralAmountsAreZero() {
        assertEquals(0f, ROUTE_B_NEUTRAL_GENERAL_AMOUNTS.compressor, 0f)
        assertEquals(0f, ROUTE_B_NEUTRAL_GENERAL_AMOUNTS.exciterRed, 0f)
        assertEquals(0f, ROUTE_B_NEUTRAL_GENERAL_AMOUNTS.loudnessTarget, 0f)
    }

    @Test fun speakersNeutralGeneralAmounts() {
        // El DAC JieLi (USB_DEVICE) → GENEZI (v2.5.483): lo GENERAL
        // (compresor/loudness/exciter) sigue a neutro aunque el
        // holograma pase (protección 0032 intacta).
        val gated = RouteBDeviceGate.gateGeneral(
            userGeneral, intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE))
        assertEquals(ROUTE_B_NEUTRAL_GENERAL_AMOUNTS, gated)
        assertEquals(0f, gated.compressor, 0f)
        assertEquals(0f, gated.loudnessTarget, 0f)
        assertEquals(0f, gated.exciterRed, 0f)
    }

    @Test fun headphonesKeepGeneralAmounts() {
        val gated = RouteBDeviceGate.gateGeneral(
            userGeneral, intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP))
        assertEquals(userGeneral, gated)
    }

    @Test fun gateGeneralBooleanOverload() {
        assertEquals(
            ROUTE_B_NEUTRAL_GENERAL_AMOUNTS,
            RouteBDeviceGate.gateGeneral(userGeneral, false)
        )
        assertEquals(userGeneral, RouteBDeviceGate.gateGeneral(userGeneral, true))
    }

    // ── 0037: gate del JSON perceptual en los funnels ──
    // El comando coloreado real que la APK publica al daemon (valores de
    // PersistedStateRestorer + BOOT_PROFILE + paneles): ORBE 1.0, HOLOS
    // 1.0 (ATREVIDA 502), widener 1.55, harmonicGain 0.78, antiDolby 0.85, compressor
    // -5.5, loudness -16, exciterRed 0.15. highCut NO es perceptual.
    private val coloredPerceptual =
        "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1.0,\"holosAmount\":1.0," +
        "\"spatialWidth\":1.55,\"harmonicGain\":0.78,\"antiDolby\":0.85," +
        "\"compressor\":-5.5,\"loudnessTarget\":-16.0,\"exciterRed\":0.15," +
        "\"highCut\":19500.0}"
    private val neutralPerceptual =
        "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":0,\"holosAmount\":0," +
        "\"spatialWidth\":1.0,\"harmonicGain\":0,\"antiDolby\":0," +
        "\"compressor\":0,\"loudnessTarget\":0,\"exciterRed\":0," +
        "\"highCut\":19500.0}"

    @Test fun headphonesKeepPerceptualJsonIntact() {
        assertEquals(
            coloredPerceptual,
            RouteBDeviceGate.gatePerceptualCommandJson(coloredPerceptual, true)
        )
    }

    @Test fun speakersNeutralizePerceptualJson() {
        // Cada campo perceptual a neutro; highCut y action intactos.
        assertEquals(
            neutralPerceptual,
            RouteBDeviceGate.gatePerceptualCommandJson(coloredPerceptual, false)
        )
    }

    @Test fun nonPerceptualJsonIntact() {
        // Comandos ajenos pasan intactos aunque la salida sea bocinas.
        val status = "{\"action\":\"GET_STATUS\"}"
        assertEquals(status, RouteBDeviceGate.gatePerceptualCommandJson(status, false))
        val preset = "SET_PRESET:Spatial"
        assertEquals(preset, RouteBDeviceGate.gatePerceptualCommandJson(preset, false))
    }

    @Test fun missingFieldsNotAdded() {
        // Solo spatialWidth presente: solo ese cambia; no se inventa nada.
        assertEquals(
            "{\"action\":\"SEND_PERCEPTUAL_STATE\",\"spatialWidth\":1.0}",
            RouteBDeviceGate.gatePerceptualCommandJson(
                "{\"action\":\"SEND_PERCEPTUAL_STATE\",\"spatialWidth\":1.55}", false)
        )
    }

    @Test fun alreadyNeutralUnchanged() {
        // Idempotente: un comando ya neutro sale byte-idéntico.
        assertEquals(
            neutralPerceptual,
            RouteBDeviceGate.gatePerceptualCommandJson(neutralPerceptual, false)
        )
    }

    @Test fun numberFormatsNeutralized() {
        // Enteros, decimales y notación científica se neutralizan igual.
        assertEquals(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":0,\"holosAmount\":0," +
            "\"spatialWidth\":1.0,\"compressor\":0}",
            RouteBDeviceGate.gatePerceptualCommandJson(
                "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1," +
                "\"holosAmount\":5e-1,\"spatialWidth\":1.55,\"compressor\":-5.5}", false)
        )
    }

    @Test fun nestedStateFieldsNeutralized() {
        // Campos anidados en "state" (OmegaEngineBridge) también se
        // neutralizan: el regex corre sobre el string entero.
        assertEquals(
            "{\"action\":\"SEND_PERCEPTUAL_STATE\",\"state\":{\"orbeAmount\":0,\"holosAmount\":0}}",
            RouteBDeviceGate.gatePerceptualCommandJson(
                "{\"action\":\"SEND_PERCEPTUAL_STATE\",\"state\":{\"orbeAmount\":1.0,\"holosAmount\":0.5}}", false)
        )
    }

    @Test fun holosEqFieldsNeutralizedForSpeakers() {
        // 0038+0040 Parche D: holosEqLow/Mid/High/Presence/TiltDb llegaban
        // sin gate al daemon y al snapshot (holosEqLow +12 / holosEqMid
        // +4.13 medidos en vivo con Qobuz sonando). Con bocinas/DAC deben
        // ir a 0; con audífonos pasan intactos.
        val colored = "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":0.5," +
            "\"holosEqLow\":12.0,\"holosEqMid\":4.13,\"holosEqHigh\":2.5," +
            "\"holosEqPresence\":1.5,\"holosEqTiltDb\":-1.0,\"holosEqFreq\":1000.0}"
        val gated = RouteBDeviceGate.gatePerceptualCommandJson(colored, false)
        assertEquals(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":0," +
            "\"holosEqLow\":0,\"holosEqMid\":0,\"holosEqHigh\":0," +
            "\"holosEqPresence\":0,\"holosEqTiltDb\":0,\"holosEqFreq\":1000.0}",
            gated
        )
        // Con audífonos, el comando vuelve intacto.
        assertEquals(colored, RouteBDeviceGate.gatePerceptualCommandJson(colored, true))
    }

    // FASE 2 — selector de transductor para DAC USB.

    @Test fun usbDacWithHeadphonesSelectorKeepsUserAmounts() {
        // Audífonos en el adaptador JieLi: el selector HEADPHONES vence
        // al TYPE_USB_DEVICE (antes era neutro siempre).
        assertEquals(
            true,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.HEADPHONES)
        )
        assertEquals(
            userAmounts,
            RouteBDeviceGate.gate(
                userAmounts,
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun usbDacWithSpeakersSelectorIsNeutral() {
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.SPEAKERS)
        )
    }

    @Test fun usbDacDefaultOverloadIsGenezi() {
        // El overload viejo (sin selector) delega con SPEAKERS: el DAC
        // USB resuelve a GENEZI (v2.5.483) — isHeadphonesOutput sigue
        // false, pero el holograma pasa (gate) y lo general no.
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE))
        )
        assertEquals(
            userAmounts,
            RouteBDeviceGate.gate(userAmounts, intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE))
        )
        assertEquals(
            RouteBOutput.GENEZI,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.SPEAKERS)
        )
    }

    @Test fun usbHeadsetAloneDefersToSelector() {
        // v2.5.483 (corrige el test anterior): un USB_HEADSET puro YA
        // NO es audífono siempre. El JieLi enumera como usb_headset
        // (dumpsys 2026-11-20) aunque alimente el Genezi: con selector
        // SPEAKERS resuelve a GENEZI (no audífonos); con HEADPHONES,
        // audífonos. El selector decide, no el tipo.
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            RouteBOutput.GENEZI,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            true,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.HEADPHONES)
        )
        assertEquals(
            RouteBOutput.HEADPHONES,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun usbDacReportingHeadsetDefersToSelector() {
        // El adaptador DAC JieLi se reporta como USB_DEVICE + USB_HEADSET
        // aunque alimente el Genezi: con DAC presente decide el selector,
        // no el tipo (antes el USB_HEADSET vencía al selector y el Genezi
        // recibía la cadena de audífonos = bofo).
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET),
                UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            true,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET),
                UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun usbAccessoryWithHeadphonesSelectorIsHeadphones() {
        assertEquals(
            true,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_ACCESSORY), UsbDacTarget.HEADPHONES)
        )
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_ACCESSORY), UsbDacTarget.SPEAKERS)
        )
    }

    @Test fun nonUsbRoutesIgnoreSelector() {
        // Sin DAC USB, la precedencia por tipos manda y el selector no
        // cambia nada: A2DP audífonos, bocina integrada neutro.
        assertEquals(
            true,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(
                intArrayOf(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER), UsbDacTarget.HEADPHONES)
        )
        assertEquals(
            false,
            RouteBDeviceGate.isHeadphonesOutput(intArrayOf(), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun hasUsbDacDetectsUsbAudioEndpoints() {
        // v2.5.483: USB_HEADSET también es endpoint USB ambiguo (el
        // JieLi enumera así); el selector decide. USB_DEVICE y
        // USB_ACCESSORY siguen contando; BT y vacío, no.
        assertEquals(true, RouteBDeviceGate.hasUsbDac(intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE)))
        assertEquals(true, RouteBDeviceGate.hasUsbDac(intArrayOf(AudioDeviceInfo.TYPE_USB_ACCESSORY)))
        assertEquals(true, RouteBDeviceGate.hasUsbDac(intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET)))
        assertEquals(false, RouteBDeviceGate.hasUsbDac(intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)))
        assertEquals(false, RouteBDeviceGate.hasUsbDac(intArrayOf()))
    }

    // ── v2.5.483 — tercer modo GENEZI ──

    @Test fun routeBOutputThreeStates() {
        assertEquals(
            RouteBOutput.GENEZI,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            RouteBOutput.GENEZI,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_HEADSET), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            RouteBOutput.GENEZI,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE, AudioDeviceInfo.TYPE_USB_HEADSET),
                UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            RouteBOutput.HEADPHONES,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_USB_DEVICE), UsbDacTarget.HEADPHONES)
        )
        assertEquals(
            RouteBOutput.HEADPHONES,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_BLUETOOTH_A2DP), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            RouteBOutput.SPEAKERS,
            RouteBDeviceGate.routeBOutput(
                intArrayOf(AudioDeviceInfo.TYPE_BUILTIN_SPEAKER), UsbDacTarget.SPEAKERS)
        )
        assertEquals(
            RouteBOutput.SPEAKERS,
            RouteBDeviceGate.routeBOutput(intArrayOf(), UsbDacTarget.HEADPHONES)
        )
    }

    @Test fun geneziGateSplitsBinauralAndGeneral() {
        // GENEZI: binaural del usuario, general neutro. HEADPHONES:
        // todo del usuario. SPEAKERS: todo neutro.
        assertEquals(
            userAmounts,
            RouteBDeviceGate.gate(userAmounts, RouteBOutput.GENEZI)
        )
        assertEquals(
            ROUTE_B_NEUTRAL_GENERAL_AMOUNTS,
            RouteBDeviceGate.gateGeneral(userGeneral, RouteBOutput.GENEZI)
        )
        assertEquals(userAmounts, RouteBDeviceGate.gate(userAmounts, RouteBOutput.HEADPHONES))
        assertEquals(userGeneral, RouteBDeviceGate.gateGeneral(userGeneral, RouteBOutput.HEADPHONES))
        assertEquals(
            ROUTE_B_NEUTRAL_AMOUNTS,
            RouteBDeviceGate.gate(userAmounts, RouteBOutput.SPEAKERS)
        )
        assertEquals(
            ROUTE_B_NEUTRAL_GENERAL_AMOUNTS,
            RouteBDeviceGate.gateGeneral(userGeneral, RouteBOutput.SPEAKERS)
        )
    }

    @Test fun geneziPerceptualJsonPassesBinauralNeutralizesGeneral() {
        // El comando coloreado real de la APK en modo GENEZI: ORBE/
        // HOLOS/widener/harmonicGain/antiDolby intactos; compressor/
        // loudnessTarget/exciterRed a 0; highCut intacto.
        assertEquals(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1.0,\"holosAmount\":0.5," +
            "\"spatialWidth\":1.55,\"harmonicGain\":0.78,\"antiDolby\":0.85," +
            "\"compressor\":0,\"loudnessTarget\":0,\"exciterRed\":0," +
            "\"highCut\":19500.0}",
            RouteBDeviceGate.gatePerceptualCommandJson(coloredPerceptual, RouteBOutput.GENEZI)
        )
        // SPEAKERS sigue a neutro total; HEADPHONES intacto.
        assertEquals(
            neutralPerceptual,
            RouteBDeviceGate.gatePerceptualCommandJson(coloredPerceptual, RouteBOutput.SPEAKERS)
        )
        assertEquals(
            coloredPerceptual,
            RouteBDeviceGate.gatePerceptualCommandJson(coloredPerceptual, RouteBOutput.HEADPHONES)
        )
    }

    @Test fun geneziPerceptualJsonStillNeutralizesHolosEq() {
        // 0038+0040 también en GENEZI: holosEq_* van a 0 aunque el
        // holograma pase (la lista de pase es solo orbe/holos/
        // spatialWidth/harmonicGain/antiDolby).
        val colored = "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":0.5," +
            "\"holosEqLow\":12.0,\"holosEqMid\":4.13}"
        assertEquals(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":0.5," +
            "\"holosEqLow\":0,\"holosEqMid\":0}",
            RouteBDeviceGate.gatePerceptualCommandJson(colored, RouteBOutput.GENEZI)
        )
    }
}
