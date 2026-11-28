package com.ivanna.omega.ai

import android.util.Log

/**
 * PerceptualCortex
 *
 * Unificación espacial 2.5.509 (paso 7): el loop perceptual POR BLOQUE
 * (process() → barkToDSP() → dispatch a PerceptualDecisionEngine) se
 * BORRÓ. Era código muerto con CERO llamadores (verificado por grep:
 * nadie llamaba a process(); solo se usaban updateContentClassification
 * y addStateListener), y resucitarlo implicaría cablear un dispatch por
 * bloque (375 Hz) en la ruta de audio, lo que contradice el dedup del
 * paso 6 y exige verificación en vivo. El comentario viejo vendía un
 * "loop cerrado por bloque" que nunca corría.
 *
 * Lo que QUEDA (superficie viva): la clasificación de contenido
 * (updateContentClassification — publica los scores AntiDolby al bus
 * nativo; la usan AntiDolbyController y AudioPipeline) y la gestión de
 * listeners (add/removeStateListener — los registra
 * PlaybackCaptureService). El loop vivo del cerebro es
 * PerceptualBrainEngine (10 Hz) + PerceptualViewModel (500 ms), no
 * esta clase.
 *
 * Los tipos DSPPerceptualControl / PerceptualState se conservan: son
 * los tipos de la interfaz PerceptualStateListener.
 */
class PerceptualCortex {

    private data class ContentClassification(
        val speech: Float = 0f,
        val music: Float = 0f,
        val bass: Float = 0f
    )
    @Volatile private var contentClassification = ContentClassification()

    // ── AUDIT FIX PR 1: Listeners para notificar cambios de estado ──────────
    private val listeners = mutableListOf<PerceptualStateListener>()

    /**
     * Single ingress for AntiDolby CRNN evidence. It publishes sanitized scores
     * to the native DSP bus and retains them for the next perceptual decision.
     * This is content classification only; it must not be treated as room data.
     */
    @Synchronized
    fun updateContentClassification(speech: Float, music: Float, bass: Float) {
        val safe = ContentClassification(
            speech = speech.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 0f,
            music = music.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 0f,
            bass = bass.takeIf { it.isFinite() }?.coerceIn(0f, 1f) ?: 0f
        )
        contentClassification = safe
        runCatching {
            com.ivanna.omega.audio.AudioEngine.nativeSetAntiDolbyScoresStatic(safe.speech, safe.music, safe.bass)
        }.onFailure { Log.w("PerceptualCortex", "AntiDolby score publish unavailable") }
    }

    @Synchronized
    fun clearContentClassification() {
        contentClassification = ContentClassification()
        runCatching {
            com.ivanna.omega.audio.AudioEngine.nativeSetAntiDolbyScoresStatic(0f, 0f, 0f)
        }.onFailure { Log.w("PerceptualCortex", "AntiDolby reset unavailable") }
    }


    // ── AUDIT FIX PR 1: Listener management methods ─────────────────────────
    /**
     * Agregar un listener para recibir notificaciones de cambios de estado.
     * Se usa para conectar componentes como IvannaBridgePlayer, DSPBridge, etc.
     *
     * @param listener que implementa PerceptualStateListener
     */
    fun addStateListener(listener: PerceptualStateListener) {
        if (!listeners.contains(listener)) {
            listeners.add(listener)
            Log.d("PerceptualCortex", "Listener agregado: ${listener.javaClass.simpleName}")
        }
    }

    /**
     * Remover un listener.
     *
     * @param listener a remover
     */
    fun removeStateListener(listener: PerceptualStateListener) {
        listeners.remove(listener)
        Log.d("PerceptualCortex", "Listener removido: ${listener.javaClass.simpleName}")
    }
}



data class DSPPerceptualControl(

    val gain: Float,

    val compressor: Float,

    val exciter: Float,

    val spatial: Float,

    // Canales adaptativos perceptuales
    // ISO226 + protección por fatiga auditiva
    val lowEqDb: Float = 0f,

    val midEqDb: Float = 0f,

    val highEqDb: Float = 0f,

    val iso226Compensation: Float = 0f,

    val fatigueProtection: Float = 0f
)



data class PerceptualState(

    val psychoacoustic:
        PsychoacousticAnalysis,

    val emotion:
        EmotionalState,

    val fatigue:
        HearingFatigueState,

    val dsp:
        DSPPerceptualControl
)


// (PerceptualSnapshot local eliminada — duplicaba la de PerceptualBrainEngine.kt:64;
//  mismo paquete com.ivanna.omega.ai → Redeclaration. Ver fix arriba.)
