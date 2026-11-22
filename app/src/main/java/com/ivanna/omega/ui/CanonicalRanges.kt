package com.ivanna.omega.ui

/**
 * CanonicalRanges — tabla canónica de rangos de los controles de audio.
 *
 * Origen: los rangos vivían incrustados en los sliders de `SoundScreen.kt`
 * (EQ, DINÁMICA, BINAURAL, NHO), pantalla huérfana borrada en la Fase 2 de
 * la refactorización visual (2026-11-22, orden de Esse: "Se borran").
 * Esta tabla preserva esos valores como referencia única, para que ningún
 * archivo vuelva a citar una pantalla muerta como fuente de rangos.
 *
 * REGLA: esto es solo documentación + constantes. NO cambia ningún valor
 * vivo del motor ni de ninguna pantalla: cada pantalla que hoy existe sigue
 * usando sus propios rangos. Unificar los rangos divergentes es trabajo de
 * la Fase 3 de la refactorización, no de este archivo.
 *
 * Divergencias conocidas (verificadas en el código vivo, inventario §1.6):
 *  - EQ Graves/Medios/Agudos: ±18 dB aquí; ±12 dB en FusedOperatorsScreen
 *    (ARMONÍA) y en IvannaControlPanel. Presencia: ±12 dB en ambas.
 *  - Compresor Umbral: −60…0 dB aquí y en FusedOperatorsScreen;
 *    −40…0 dB en AdaptiveEngineScreen.
 *  - Compresor Ratio: 1…20 aquí; 1…10 en otras pantallas.
 *  - Compresor Release: 10…2000 ms aquí; 10…500 ms en otras pantallas.
 */
object CanonicalRanges {

    // ── EQ (tab EQ de SoundScreen) ──────────────────────────────────────────
    /** Graves / Medios / Agudos del EQ paramétrico. */
    val eqBandDb: ClosedFloatingPointRange<Float> = -18f..18f
    /** Presencia del EQ. */
    val eqPresenceDb: ClosedFloatingPointRange<Float> = -12f..12f
    /** Volumen maestro (ganancia lineal, 1.0 = neutro). */
    val masterGain: ClosedFloatingPointRange<Float> = 0.5f..2f

    // ── Compresor (tab DINÁMICA de SoundScreen) ─────────────────────────────
    /** Umbral del compresor. */
    val compressorThresholdDb: ClosedFloatingPointRange<Float> = -60f..0f
    /** Ratio del compresor. */
    val compressorRatio: ClosedFloatingPointRange<Float> = 1f..20f
    /** Ataque del compresor. */
    val compressorAttackMs: ClosedFloatingPointRange<Float> = 0.1f..200f
    /** Release del compresor. */
    val compressorReleaseMs: ClosedFloatingPointRange<Float> = 10f..2000f

    // ── AGC / Loudness LUFS (tab DINÁMICA de SoundScreen) ───────────────────
    /** Objetivo del AGC (ITU-R BS.1770). */
    val agcTargetLufs: ClosedFloatingPointRange<Float> = -36f..-6f
    /** Velocidad del AGC (0 = lento, 1 = rápido). */
    val agcRate: ClosedFloatingPointRange<Float> = 0f..1f

    // ── Binaural / HRTF (tab BINAURAL de SoundScreen) ───────────────────────
    /** Azimut de la escena binaural. */
    val binauralAzimuthDeg: ClosedFloatingPointRange<Float> = -180f..180f
    /** Elevación de la escena binaural. */
    val binauralElevationDeg: ClosedFloatingPointRange<Float> = -45f..45f
    /** Ancho espacial (1.0 = neutro). */
    val spatialWidth: ClosedFloatingPointRange<Float> = 0f..2f

    // ── Motor híbrido magistral (tab BINAURAL de SoundScreen) ───────────────
    /** Mezcla binaural 128-Tap. */
    val hybridBinauralWet: ClosedFloatingPointRange<Float> = 0f..1f
    /** Tamaño de sala acústica. */
    val hybridRoomSize: ClosedFloatingPointRange<Float> = 0.1f..1f
    /** Absorción de paredes. */
    val hybridRoomAbsorption: ClosedFloatingPointRange<Float> = 0.05f..0.95f
    /** Amortiguamiento de aire (HF). */
    val hybridRoomDampening: ClosedFloatingPointRange<Float> = 0.05f..0.90f
    /** Reverb de sala Schroeder (wet). */
    val hybridRoomWetMix: ClosedFloatingPointRange<Float> = 0f..0.75f

    // ── Motor NHO / PDEngine (tab NHO de SoundScreen) ───────────────────────
    /** Wet NHO (η). */
    val nhoEta: ClosedFloatingPointRange<Float> = 0f..1f
    /** Ganancia armónica NHO. */
    val nhoHarmonicGain: ClosedFloatingPointRange<Float> = 0f..1f
    /** Inhibición lateral NHO. */
    val nhoLateralInhibition: ClosedFloatingPointRange<Float> = 0f..1f
    /** Ganancia OHC NHO. */
    val nhoOhcGain: ClosedFloatingPointRange<Float> = 0f..1f
}
