package com.ivanna.omega.assistant

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * v2.5.445 — Enrutado LLM.
 *
 * Contrato que protege el hallazgo del teléfono (444): la pregunta
 * "¿Qué es la holofonía y por qué se siente diferente al estéreo normal?"
 * recibió la frase enlatada "He dejado el sonido neutro, sin coloración"
 * porque el clasificador local la interceptó por substring ("normal" →
 * FLAT_NEUTRAL). Las preguntas abiertas van al LLM; las órdenes DSP y las
 * acciones locales NO.
 *
 * Puro y determinista (sin Android): JUnit plano, sin Robolectric.
 */
class OpenQuestionRouterTest {

    @Test
    fun holofoniaQuestionRoutesToLlm() {
        assertTrue(
            OpenQuestionRouter.isOpenQuestion(
                "¿Qué es la holofonía y por qué se siente diferente al estéreo normal?"
            )
        )
    }

    @Test
    fun whatIsQuestionRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("¿qué es HRTF?"))
    }

    @Test
    fun explainHowItWorksRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("explícame cómo funciona el ASRC"))
    }

    @Test
    fun whyDoesItSoundRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("¿por qué suena bofo?"))
    }

    @Test
    fun orbeCommandDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("pon el ORBE a 1.0"))
    }

    @Test
    fun volumeCommandDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("sube el volumen"))
    }

    @Test
    fun bassCommandDoesNotRouteToLlm() {
        // "Sube el grave" es una orden (la resuelve VoiceSettingsControl),
        // no una pregunta abierta: nunca va al LLM como pregunta.
        assertFalse(OpenQuestionRouter.isOpenQuestion("sube el grave"))
    }

    @Test
    fun roomCorrectionCommandDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("enciende la corrección de sala"))
    }

    @Test
    fun whyNoSoundRoutesToLlm() {
        // "¿por qué no suena el ORBE?" es una pregunta (diagnóstico), no
        // una orden negada: va al LLM.
        assertTrue(OpenQuestionRouter.isOpenQuestion("¿por qué no suena el ORBE?"))
    }

    @Test
    fun phiCommandDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("apaga el Phi"))
    }

    @Test
    fun kernelCommandDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("pon el kernel GENEZI"))
    }

    @Test
    fun greetingDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("hola"))
    }

    @Test
    fun selfHealCommandDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("repárate"))
    }

    @Test
    fun profileListDoesNotRouteToLlm() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("qué perfiles hay"))
    }

    @Test
    fun whyDidYouChangeRoutesToLocalExplain() {
        // "por qué" sobre la última decisión → EXPLAIN local, no pregunta.
        assertFalse(OpenQuestionRouter.isOpenQuestion("¿por qué cambiaste el sonido?"))
    }

    @Test
    fun blankIsNotAnOpenQuestion() {
        assertFalse(OpenQuestionRouter.isOpenQuestion("   "))
    }

    // ── Análisis / evaluación → al LLM (2026-11-14, reporte del propietario) ──
    // "analiza" no es orden DSP ni "qué es": es un pedido abierto de analizar.
    // Antes caía al núcleo local (TECHNICAL_QUESTION) y el panel mostraba
    // "IvannaStaticCore (bloqueó)" con la respuesta enlatada local.

    @Test
    fun analyzeRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("analiza"))
    }

    @Test
    fun analyzeTheSoundRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("analiza el sonido"))
    }

    @Test
    fun evaluateRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("evalúa la música"))
    }

    @Test
    fun howDoesItSoundRoutesToLlm() {
        assertTrue(OpenQuestionRouter.isOpenQuestion("cómo suena esto"))
    }

    @Test
    fun analyzeDoesNotStealDspCommand() {
        // Una orden DSP real sigue sin ir al LLM aunque contenga análisis.
        assertFalse(OpenQuestionRouter.isOpenQuestion("analiza y sube el ORBE"))
    }

    @Test
    fun unavailableMessageIsHonest() {
        val msg = OpenQuestionRouter.LLM_UNAVAILABLE_MESSAGE
        assertTrue(msg.isNotBlank())
        // No es una frase enlatada de DSP: no afirma haber cambiado nada.
        assertFalse(msg.contains("He dejado"))
        assertFalse(msg.contains("neutro"))
    }

    @Test
    fun errorMessageCarriesTheDetail() {
        assertTrue(OpenQuestionRouter.llmErrorMessage("timeout").contains("timeout"))
    }

    @Test
    fun emptyMessageIsNotBlank() {
        assertTrue(OpenQuestionRouter.LLM_EMPTY_MESSAGE.isNotBlank())
    }
}
