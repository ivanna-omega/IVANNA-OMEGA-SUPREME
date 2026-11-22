package com.ivanna.omega.assistant

import android.app.Application
import android.content.Context
import androidx.test.core.app.ApplicationProvider
import com.ivanna.omega.assistant.VoiceSettingsControl.Action
import com.ivanna.omega.assistant.VoiceSettingsControl.Target
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.OmegaEquipment
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.neuromorphic.IvannaNpeEngine
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * v2.5.445 — Control por voz de ajustes DSP.
 *
 * Directiva de Esse: la asistente DEBÍA controlar y auto arreglar, no solo
 * explicar. Estos tests afirman el parseo (qué orden es cada frase) y la
 * aplicación real (el parámetro cambia de verdad en ParameterStore y la
 * confirmación dice lo que se aplicó). Bajo Robolectric la librería nativa
 * NO está cargada (IvannaNativeLib.isLoaded == false), así que los tests
 * afirman la persistencia y la honestidad cuando el motor no está listo —
 * en el teléfono, con la librería cargada, el mismo apply empuja además
 * el valor por JNI en vivo.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33], application = Application::class, manifest = Config.NONE)
class VoiceSettingsControlTest {

    private lateinit var context: Context

    @Before
    fun setup() {
        context = ApplicationProvider.getApplicationContext()
        context.getSharedPreferences("ivanna_omega_params", 0).edit().clear().commit()
        context.getSharedPreferences("ivanna_iso226", 0).edit().clear().commit()
    }

    // ── Parseo ─────────────────────────────────────────────────────────────

    @Test
    fun parseOrbeSet() {
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 1.0")
        assertNotNull(cmd)
        assertEquals(Target.ORBE, cmd!!.target)
        assertEquals(Action.SET, cmd.action)
        assertEquals(1.0f, cmd.value!!, 0.001f)
    }

    // ── "aplica" / "aplicar" (2026-11-14, reporte del propietario) ─────────
    // El propietario pedía "aplicar algo" por voz y por escrito y NADA se
    // aplicaba: el verbo "aplica" no existía en el parser, así que la orden
    // caía al flujo de respuesta (LLM/base local) que solo responde sin
    // tocar el DSP. "aplica" es SET con valor y ENABLE sin valor.

    @Test
    fun parseAplicaOrbeSet() {
        val cmd = VoiceSettingsControl.parse("aplica el ORBE a 1.0")
        assertNotNull(cmd)
        assertEquals(Target.ORBE, cmd!!.target)
        assertEquals(Action.SET, cmd.action)
        assertEquals(1.0f, cmd.value!!, 0.001f)
    }

    @Test
    fun parseAplicaOrbeEnable() {
        val cmd = VoiceSettingsControl.parse("aplica el ORBE")
        assertNotNull(cmd)
        assertEquals(Target.ORBE, cmd!!.target)
        assertEquals(Action.ENABLE, cmd.action)
    }

    @Test
    fun parseAplicarHolosSet() {
        val cmd = VoiceSettingsControl.parse("aplicar el HOLOS a 0.5")
        assertNotNull(cmd)
        assertEquals(Target.HOLOS, cmd!!.target)
        assertEquals(Action.SET, cmd.action)
        assertEquals(0.5f, cmd.value!!, 0.001f)
    }

    @Test
    fun parseAplicaQuestionIsNotCommand() {
        // "¿cómo se aplica el ORBE?" es una PREGUNTA (va al LLM), no una
        // orden de encender el ORBE. Sin la guarda de pregunta, "aplica"
        // en ENABLE_WORDS la convertiría en comando.
        assertNull(VoiceSettingsControl.parse("¿cómo se aplica el ORBE?"))
        assertNull(VoiceSettingsControl.parse("¿qué hace el HOLOS?"))
        assertNull(VoiceSettingsControl.parse("como se aplica el ORBE"))
    }

    @Test
    fun applyAplicaOrbePersists() {
        val cmd = VoiceSettingsControl.parse("aplica el ORBE a 0.7")
        assertNotNull(cmd)
        val result = VoiceSettingsControl.apply(context, cmd!!)
        assertTrue(result.applied)
        assertEquals(0.7f, ParameterStore(context).getOrbeAmount(), 0.001f)
    }

    @Test
    fun parseOrbePercent() {
        val cmd = VoiceSettingsControl.parse("pon el ORBE al 70%")
        assertNotNull(cmd)
        assertEquals(Target.ORBE, cmd!!.target)
        assertEquals(Action.SET, cmd.action)
        assertEquals(0.7f, cmd.value!!, 0.001f)
    }

    @Test
    fun parseHolosRaise() {
        val cmd = VoiceSettingsControl.parse("sube el HOLOS")
        assertNotNull(cmd)
        assertEquals(Target.HOLOS, cmd!!.target)
        assertEquals(Action.RAISE, cmd.action)
        assertEquals(0.1f, cmd.value!!, 0.001f)
    }

    @Test
    fun parsePhiDisable() {
        val cmd = VoiceSettingsControl.parse("apaga el Phi")
        assertNotNull(cmd)
        assertEquals(Target.PHI, cmd!!.target)
        assertEquals(Action.DISABLE, cmd.action)
    }

    @Test
    fun parseVolterraGenezi() {
        val cmd = VoiceSettingsControl.parse("pon el kernel GENEZI")
        assertNotNull(cmd)
        assertEquals(Target.VOLTERRA, cmd!!.target)
        assertEquals(Action.SELECT_KERNEL, cmd.action)
        assertEquals(IvannaNativeLib.VOLTERRA_KERNEL_GENEZI, cmd.kernelId)
    }

    @Test
    fun parseVolterraPz1d() {
        val cmd = VoiceSettingsControl.parse("pon el kernel PZ1D refinado")
        assertNotNull(cmd)
        assertEquals(Target.VOLTERRA, cmd!!.target)
        assertEquals(Action.SELECT_KERNEL, cmd.action)
        assertEquals(IvannaNativeLib.VOLTERRA_KERNEL_PZ1D, cmd.kernelId)
    }

    @Test
    fun parseOmegaEquipment() {
        val cmd = VoiceSettingsControl.parse("pon el Omega en audífonos")
        assertNotNull(cmd)
        assertEquals(Target.OMEGA, cmd!!.target)
        assertEquals(Action.SELECT_EQUIPMENT, cmd.action)
        assertEquals(OmegaEquipment.HEADPHONES, cmd.equipment)
    }

    @Test
    fun parseIso226Disable() {
        val cmd = VoiceSettingsControl.parse("apaga el ISO 226")
        assertNotNull(cmd)
        assertEquals(Target.ISO226, cmd!!.target)
        assertEquals(Action.DISABLE, cmd.action)
    }

    @Test
    fun parseOpenQuestionReturnsNull() {
        assertNull(
            VoiceSettingsControl.parse(
                "¿Qué es la holofonía y por qué se siente diferente al estéreo normal?"
            )
        )
    }

    @Test
    fun parseNonSettingReturnsNull() {
        assertNull(VoiceSettingsControl.parse("cuéntame un chiste"))
    }

    // ── Negación y ambigüedad (fail-closed) ───────────────────────────────

    @Test
    fun parseNegatedCommandIsNegated() {
        // "no apaga el HOLOS" ejecutaría DISABLE sin la guarda de negación.
        val outcome = VoiceSettingsControl.parseDetailed("no apaga el HOLOS")
        assertTrue(outcome is VoiceSettingsControl.ParseOutcome.Negated)
        assertNull(VoiceSettingsControl.parse("no apaga el HOLOS"))
    }

    @Test
    fun parseNegatedSubjunctiveIsNegated() {
        assertTrue(
            VoiceSettingsControl.parseDetailed("no subas el ORBE")
                is VoiceSettingsControl.ParseOutcome.Negated
        )
        assertTrue(
            VoiceSettingsControl.parseDetailed("no quiero que subas el ORBE")
                is VoiceSettingsControl.ParseOutcome.Negated
        )
    }

    @Test
    fun parseQuestionAboutNoSoundIsNotNegated() {
        // "¿por qué no suena el ORBE?" es una pregunta, no una orden
        // negada: debe seguir al LLM/diagnóstico, no a "no cambio nada".
        assertTrue(
            VoiceSettingsControl.parseDetailed("¿por qué no suena el ORBE?")
                is VoiceSettingsControl.ParseOutcome.None
        )
        assertTrue(
            VoiceSettingsControl.parseDetailed("no suena el ORBE")
                is VoiceSettingsControl.ParseOutcome.None
        )
    }

    @Test
    fun parseNegationAfterVerbIsNotNegated() {
        // "pon el ORBE a 0.7, no al 0.5": el "no" va después del verbo.
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 0.7, no al 0.5")
        assertNotNull(cmd)
        assertEquals(Action.SET, cmd!!.action)
        assertEquals(0.7f, cmd.value!!, 0.001f)
    }

    @Test
    fun parseSimultaneousOrdersIsAmbiguous() {
        val outcome = VoiceSettingsControl.parseDetailed("sube el ORBE y el HOLOS")
        assertTrue(outcome is VoiceSettingsControl.ParseOutcome.Ambiguous)
        val targets = (outcome as VoiceSettingsControl.ParseOutcome.Ambiguous).targets
        assertTrue(targets.contains(Target.ORBE))
        assertTrue(targets.contains(Target.HOLOS))
        // Nunca se ejecuta el primero e ignora el segundo.
        assertNull(VoiceSettingsControl.parse("sube el ORBE y el HOLOS"))
    }

    @Test
    fun parseTwoTargetsDifferentVerbsIsAmbiguous() {
        assertTrue(
            VoiceSettingsControl.parseDetailed("pon el kernel GENEZI y sube el ORBE")
                is VoiceSettingsControl.ParseOutcome.Ambiguous
        )
    }

    // ── Valores extremos ───────────────────────────────────────────────────

    @Test
    fun parseDecimalAboveOneClampsToOne() {
        // "a 2.0" es fuera de rango, NO 2 % (= 0.02, lo contrario).
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 2.0")
        assertNotNull(cmd)
        assertEquals(1.0f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parsePercentAboveHundredClampsToOne() {
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 150%")
        assertNotNull(cmd)
        assertEquals(1.0f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parseIntegerAboveOneIsStillPercent() {
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 70")
        assertNotNull(cmd)
        assertEquals(0.7f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parseSmallDecimalKeepsValue() {
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 0.05")
        assertNotNull(cmd)
        assertEquals(0.05f, cmd!!.value!!, 0.001f)
    }

    // ── Transcripción incorrecta y otros idiomas ───────────────────────────

    @Test
    fun parseNearMissTargetReturnsNull() {
        // "orve" mal transcrito: no ejecutar el objetivo equivocado.
        assertNull(VoiceSettingsControl.parse("pon el orve a 0.5"))
        assertNull(VoiceSettingsControl.parse("pon el horbe a 0.5"))
    }

    @Test
    fun parseEnglishReturnsNull() {
        // Solo español por ahora: no adivinar en inglés.
        assertNull(VoiceSettingsControl.parse("turn up the ORBE"))
    }

    // ── Graves y volumen (controles musicales, no ORBE) ───────────────────

    @Test
    fun parseRaiseBassIsNotOrbe() {
        // "Sube el grave" NO es ORBE: es el trim de graves del sensor.
        val cmd = VoiceSettingsControl.parse("sube el grave")
        assertNotNull(cmd)
        assertEquals(Target.BASS, cmd!!.target)
        assertEquals(Action.RAISE, cmd.action)
    }

    @Test
    fun parseBassSetDb() {
        val cmd = VoiceSettingsControl.parse("pon el grave a 2")
        assertNotNull(cmd)
        assertEquals(Target.BASS, cmd!!.target)
        assertEquals(Action.SET, cmd.action)
        assertEquals(2.0f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parseBassMaxClampsTo3Db() {
        val cmd = VoiceSettingsControl.parse("pon el grave al máximo")
        assertNotNull(cmd)
        assertEquals(3.0f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parseVolumeRaise() {
        val cmd = VoiceSettingsControl.parse("sube el volumen")
        assertNotNull(cmd)
        assertEquals(Target.VOLUME, cmd!!.target)
        assertEquals(Action.RAISE, cmd.action)
    }

    @Test
    fun parseVolumeLowWordIsLow() {
        // "pon el volumen bajo" es volumen BAJO, no ENABLE (= 0.85).
        val cmd = VoiceSettingsControl.parse("pon el volumen bajo")
        assertNotNull(cmd)
        assertEquals(Target.VOLUME, cmd!!.target)
        assertEquals(Action.SET, cmd.action)
        assertEquals(0.2f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parseVolumeSet() {
        val cmd = VoiceSettingsControl.parse("pon el volumen a 0.5")
        assertNotNull(cmd)
        assertEquals(0.5f, cmd!!.value!!, 0.001f)
    }

    @Test
    fun parseRoomCorrectionEnable() {
        val cmd = VoiceSettingsControl.parse("enciende la corrección de sala")
        assertNotNull(cmd)
        assertEquals(Target.ROOM_CORRECTION, cmd!!.target)
        assertEquals(Action.ENABLE, cmd.action)
    }

    // ── Aplicación ─────────────────────────────────────────────────────────

    @Test
    fun applyOrbeSetPersists() {
        val cmd = VoiceSettingsControl.parse("pon el ORBE a 0.7")!!
        val result = VoiceSettingsControl.apply(context, cmd)
        assertTrue(result.applied)
        assertTrue(result.confirmation.contains("ORBE"))
        assertEquals(0.7f, ParameterStore(context).getOrbeAmount(), 0.001f)
    }

    @Test
    fun applyHolosRaiseFromKnownValue() {
        VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el HOLOS a 0.5")!!)
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("sube el HOLOS")!!)
        assertTrue(result.applied)
        assertEquals(0.6f, ParameterStore(context).getHolosAmount(), 0.001f)
    }

    @Test
    fun applyOrbeDisableSetsZero() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("apaga el ORBE")!!)
        assertTrue(result.applied)
        assertEquals(0f, ParameterStore(context).getOrbeAmount(), 0.001f)
    }

    @Test
    fun applyPhiWithoutEngineIsHonest() {
        // Sin motor NPE listo (handle==0 en tests), setPhi devuelve false:
        // el control por voz NO debe afirmar que se aplicó, y getPhi()
        // sigue en el 0.5 de la directiva.
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("apaga el Phi")!!)
        assertFalse(result.applied)
        assertTrue(result.confirmation.contains("motor"))
        assertEquals(0.5f, IvannaNpeEngine.getPhi(), 0.001f)
    }

    @Test
    fun applyVolterraDisablePersists() {
        val off = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("apaga el Volterra")!!)
        assertTrue(off.applied)
        assertFalse(ParameterStore(context).isVolterraEnabled())
        val on = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("enciende el Volterra")!!)
        assertTrue(on.applied)
        assertTrue(ParameterStore(context).isVolterraEnabled())
    }

    @Test
    fun applyVolterraKernelWithoutNativeLibIsHonest() {
        // Sin librería nativa (isLoaded==false en tests), el kernel no se
        // puede cargar: el apply devuelve applied=false con el código, no
        // un éxito inventado. En el teléfono (lib cargada) devuelve 0.
        assertFalse(IvannaNativeLib.isLoaded)
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el kernel GENEZI")!!)
        assertFalse(result.applied)
        assertTrue(result.confirmation.contains("-1"))
    }

    @Test
    fun applyIso226DisableClears() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("apaga el ISO 226")!!)
        assertTrue(result.applied)
        assertFalse(ParameterStore(context).loadIso226Calibrated())
    }

    @Test
    fun applyOmegaEnablePersists() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("enciende el Omega")!!)
        assertTrue(result.applied)
        assertTrue(ParameterStore(context).isOmegaEnabled())
    }

    @Test
    fun applyOmegaEquipmentSelectsAndEnables() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el Omega en audífonos")!!)
        assertTrue(result.applied)
        assertTrue(ParameterStore(context).isOmegaEnabled())
        assertEquals(OmegaEquipment.HEADPHONES, ParameterStore(context).getOmegaEquipment())
    }

    // ── Readback y modo offline (sin librería nativa) ─────────────────────

    @Test
    fun applyOrbeWithoutNativeLibSaysSavedNotLive() {
        // Sin .so, el readback no existe: la confirmación debe decir
        // "guardado", nunca afirmar que el motor ya suena.
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el ORBE a 0.7")!!)
        assertTrue(result.applied)
        assertTrue(result.confirmation.contains("guardado"))
        // "(motor)" es la marca del readback en vivo: sin .so no debe aparecer.
        assertFalse(result.confirmation.contains("(motor)"))
        assertEquals(0.7f, ParameterStore(context).getOrbeAmount(), 0.001f)
    }

    @Test
    fun applyBassRaisePersistsTrim() {
        // Sin sensor activo (tests), el trim se persiste y se dice
        // "guardado": no se finge que el motor lo aplicó.
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("sube el grave")!!)
        assertTrue(result.applied)
        assertTrue(result.confirmation.contains("guardado"))
        assertEquals(0.5f, ParameterStore(context).getSensorBassTrimDb(), 0.001f)
    }

    @Test
    fun applyBassSetDbPersists() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el grave a 2")!!)
        assertTrue(result.applied)
        assertEquals(2.0f, ParameterStore(context).getSensorBassTrimDb(), 0.001f)
    }

    @Test
    fun applyBassDisableIsNeutral() {
        VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el grave a 2")!!)
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("apaga el grave")!!)
        assertTrue(result.applied)
        assertEquals(0f, ParameterStore(context).getSensorBassTrimDb(), 0.001f)
    }

    @Test
    fun applyVolumePersists() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("pon el volumen a 0.5")!!)
        assertTrue(result.applied)
        assertTrue(result.confirmation.contains("0.5"))
        assertEquals(0.5f, ParameterStore(context).getMasterVolume(), 0.001f)
    }

    @Test
    fun applyRoomCorrectionEnablePersists() {
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("enciende la corrección de sala")!!)
        assertTrue(result.applied)
        assertTrue(result.confirmation.contains("guardado"))
        assertTrue(ParameterStore(context).isSensorRoomCorrectionEnabled())
        assertEquals(0.5f, ParameterStore(context).getSensorRoomCorrectionAmount(), 0.001f)
    }

    @Test
    fun applyRoomCorrectionDisablePersists() {
        VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("enciende la corrección de sala")!!)
        val result = VoiceSettingsControl.apply(context, VoiceSettingsControl.parse("apaga la corrección de sala")!!)
        assertTrue(result.applied)
        assertFalse(ParameterStore(context).isSensorRoomCorrectionEnabled())
    }
}
