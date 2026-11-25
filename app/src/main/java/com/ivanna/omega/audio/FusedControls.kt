package com.ivanna.omega.audio

import android.util.Log
import com.ivanna.omega.core.NativeLibraryLoader
import com.ivanna.omega.magisk.OmegaEngineBridge
import org.json.JSONObject

/**
 * FusedControls.kt — ESPEJO KOTLIN DE app/src/main/cpp/fused/fused_controls.hpp
 * (FUSIÓN MAGISTRAL, 2026-11-18, directiva de Esse).
 *
 * Los ~45 efectos del producto se fusionan en 7 operadores únicos. Este
 * archivo es el espejo exacto en Kotlin de `ivanna::fused::FusedControls`:
 * mismos operadores, mismos controles, mismos rangos y mismos defaults.
 * Si el header C++ cambia, este archivo cambia con él — la UI nunca inventa
 * un control ni un rango.
 *
 * Reglas duras (del header):
 *  - Matched-LUFS: ganancia neta 0.0 dB siempre. Cada operador es ganancia
 *    1.0 exacta en neutro.
 *  - Neutro = bit-transparente: con isNeutral() == true el DSP no toca el
 *    buffer.
 *  - Todos los controles se clampan (fail-closed); ningún valor fuera de
 *    rango llega al DSP. `clamp()` replica `clampf()` del C++ (NaN → lo).
 *
 * Las pantallas viejas por efecto quedan como LEGACY y no se tocan; la
 * única superficie de control nueva es esta.
 */

/** clampf del C++: no-finito → lo, si no coerceIn(lo, hi). */
private fun clampf(v: Float, lo: Float, hi: Float): Float =
    if (!v.isFinite()) lo else v.coerceIn(lo, hi)

// ── Enums con su valor JNI ──────────────────────────────────────────────────
// El Int que viaja por JNI es el valor `jni` de cada enum. El C++ lo traduce
// a su propio enum; el Kotlin nunca pasa el valor crudo del enum C++.

/**
 * Modo de salida (Operador Espacial).
 *
 * CONTRATO JNI: 0 = HEADPHONES, 1 = SPEAKERS. El enum C++
 * `OmegaOutputMode` (omega_control_bus.h) es OTRO: UNKNOWN = 0,
 * HEADPHONES = 1, SPEAKERS = 2. El nativo que implementa
 * [FusedControlsBridge.nativeSetFusedControls] DEBE traducir:
 * 0 → OmegaOutputMode::HEADPHONES, 1 → OmegaOutputMode::SPEAKERS.
 * Pasar el Int JNI tal cual al campo C++ pondría UNKNOWN = 0, que es
 * fail-closed y deja el audio en silencio.
 */
enum class FusedSpatialMode(val jni: Int, val label: String) {
    HEADPHONES(0, "Audífonos"),
    SPEAKERS(1, "Bocinas · Genezi");

    companion object {
        fun fromJni(v: Int): FusedSpatialMode = if (v == 1) SPEAKERS else HEADPHONES
    }
}

/** Curva tonal (ARMONÍA): 0 = plana, 1 = Harman 2018, 2 = élite (insignia). */
enum class FusedTonalCurve(val jni: Int, val label: String) {
    FLAT(0, "Plana"),
    HARMAN(1, "Harman 2018"),
    ELITE(2, "Élite");

    companion object {
        fun fromJni(v: Int): FusedTonalCurve = when (v) {
            1 -> HARMAN
            2 -> ELITE
            else -> FLAT
        }
    }
}

/** Perfil auditivo (SALA VIVA): 0 = plano, 1 = Harman, 2 = personalizado. */
enum class FusedHearingProfile(val jni: Int, val label: String) {
    FLAT(0, "Plano"),
    HARMAN(1, "Harman"),
    CUSTOM(2, "Personalizado");

    companion object {
        fun fromJni(v: Int): FusedHearingProfile = when (v) {
            1 -> HARMAN
            2 -> CUSTOM
            else -> FLAT
        }
    }
}

/** Preset Phi (HOLOS·Ω): 0 = teléfono, 1 = sala, 2 = auto. */
enum class FusedPhiPreset(val jni: Int, val label: String) {
    PHONE(0, "Teléfono"),
    ROOM(1, "Sala"),
    CAR(2, "Auto");

    companion object {
        fun fromJni(v: Int): FusedPhiPreset = when (v) {
            1 -> ROOM
            2 -> CAR
            else -> PHONE
        }
    }
}

/** Modo del cerebro (NEURO·Ω): 0 = AUTO (adapta), 1 = MANUAL (observa, no actúa). */
enum class FusedBrainMode(val jni: Int, val label: String) {
    AUTO(0, "AUTO"),
    MANUAL(1, "MANUAL");

    companion object {
        fun fromJni(v: Int): FusedBrainMode = if (v == 1) MANUAL else AUTO
    }
}

// ── 1. ORBE·Ω — OPERADOR ESPACIAL ÚNICO ─────────────────────────────────────
// Fusiona: ORBE + WFS + HOA + HRTF (SOFA) + Crosstalk Canceller + Head
// Tracker SO(3) + Scene Geometry Renderer + Stereo Object Decomposer +
// AuxFieldCoherence + AuxGeneziRoute + DopPacketizer.
data class SpatialControls(
    val amount: Float = 1.0f,            // ORBE amount 0..1 (0 = bypass). Default 1.0 (directiva 2026-11-08).
    val azimuthDeg: Float = 0.0f,        // azimut -180..180 deg.
    val distanceM: Float = 1.0f,         // distancia 0.15..2.0 m.
    val width: Float = 1.0f,             // ancho 0..2 (1 = neutro).
    val mode: FusedSpatialMode = FusedSpatialMode.HEADPHONES, // audífonos / bocinas (Genezi).
    val wfsSpread: Float = 0.0f,         // apertura WFS 0..1 (0 = WFS apagado).
    val hrtfWet: Float = 1.0f,           // wet HRTF 0..1.
    val headTracking: Boolean = false,   // seguimiento de cabeza (yaw). v2.5.471: la pose la alimenta IvannaHeadTracker; el default se mantiene en false (opt-in) porque activarlo sin pose medida cambia el THD+N activo (ver test ThdAtEngaged).
    val xtcEnabled: Boolean = false,     // cancelación de diafonía (altavoces).
    val upmixEnabled: Boolean = false,   // upmixer HOA (off por defecto).
    val upmixImmersivity: Float = 1.25f, // inmersividad del upmixer 0..2.
    val parallaxAmount: Float = 1.0f,    // escala del parallax por head-tracking 0..1 (1 = actual; 0 = el tracker no mueve el sonido). v2.5.472.
    // v2.5.473 (§2.2 #13): radio de cabeza medido por foto (HeadMeasureScreen).
    // No es estado de audio: setNeutral() lo conserva; es un dato medido, no un knob.
    val headRadiusM: Float = 0.0875f, // radio de cabeza 0.05..0.12 m (default KEMAR, PROFILE_BASE).
) {
    fun clamp(): SpatialControls = copy(
        amount = clampf(amount, 0.0f, 1.0f),
        azimuthDeg = clampf(azimuthDeg, -180.0f, 180.0f),
        distanceM = clampf(distanceM, 0.15f, 2.0f),
        width = clampf(width, 0.0f, 2.0f),
        wfsSpread = clampf(wfsSpread, 0.0f, 1.0f),
        hrtfWet = clampf(hrtfWet, 0.0f, 1.0f),
        upmixImmersivity = clampf(upmixImmersivity, 0.0f, 2.0f),
        parallaxAmount = clampf(parallaxAmount, 0.0f, 1.0f),
        headRadiusM = clampf(headRadiusM, 0.05f, 0.12f),
    )

    /** Neutro = nada de espacialización: la señal sale idéntica. */
    fun isNeutral(): Boolean =
        amount <= 0.0f && wfsSpread <= 0.0f && width == 1.0f &&
            hrtfWet <= 0.0f && !xtcEnabled && !upmixEnabled
}

// ── 2. HOLOS·Ω — OPERADOR DE CAMPO UNIFICADO ────────────────────────────────
// Fusiona: HOLOS + Phi (Φ) + Gamma (Ley 1, campo Γ) + SpectralCore +
// NEU·Ω (compilador espectral holográfico) + IvannaFusionCore (etapa de
// campo del engine) + OmniField (IVANNA).
data class FieldControls(
    val amount: Float = 1.0f,     // HOLOS amount 0..1. Default 1.0 (preset ATREVIDA 502, directiva Esse 2026-11-24).
    val phiAmount: Float = 0.5f,  // Phi amount 0..1. Default 0.5 (directiva 2026-11-08).
    val phiPreset: FusedPhiPreset = FusedPhiPreset.PHONE, // Phi: 0 teléfono, 1 sala, 2 auto.
    val pumping: Float = 0.0f,    // bombeo 0..1. TARGET 0.0 (bombeo cero, magistral).
    val coherence: Float = 0.8f,  // coherencia 0..1.
    val harmonics: Float = 0.002f,  // armónicos HOLOS 0..0.002 (kHolosHarmonicsMaxValidated; ATREVIDA 502).
    val gamma: Float = 1.0f,      // campo Γ (Ley 1) 0..2.
    val warmth: Float = 0.0f,     // calidez 0..1.
    val dynamics: Float = 1.0f,   // dinámica 0..1 (1 = sin compresión extra).
    val roomRt60: Float = 0.34f,  // RT60 de la sala del campo 0.05..1.5 s.
) {
    fun clamp(): FieldControls = copy(
        amount = clampf(amount, 0.0f, 1.0f),
        phiAmount = clampf(phiAmount, 0.0f, 1.0f),
        pumping = clampf(pumping, 0.0f, 1.0f),
        coherence = clampf(coherence, 0.0f, 1.0f),
        harmonics = clampf(harmonics, 0.0f, 0.002f),
        gamma = clampf(gamma, 0.0f, 2.0f),
        warmth = clampf(warmth, 0.0f, 1.0f),
        dynamics = clampf(dynamics, 0.0f, 1.0f),
        roomRt60 = clampf(roomRt60, 0.05f, 1.5f),
    )

    fun isNeutral(): Boolean = amount <= 0.0f && phiAmount <= 0.0f
}

// ── 3. SALA VIVA — MODELO OYENTE+SALA ───────────────────────────────────────
// Fusiona: RIR (room_model / RirConvolver) + SAF (modelo espacial / PCA /
// HRTF) + Hearing Adaptation + BioControl 5D + corrección de sala (cut-only).
data class ListenerRoomControls(
    val rt60Sec: Float = 0.34f,    // RT60 0.05..1.5 s.
    val roomSizeM: Float = 5.0f,   // tamaño de sala 2..12 m.
    val roomWet: Float = 0.22f,    // wet RIR 0..1. Default 0.22 (valor vivo).
    val hearingProfile: FusedHearingProfile = FusedHearingProfile.FLAT, // 0 plano, 1 Harman, 2 personalizado.
    // v2.5.472 Fase 2 (Sala Viva): sala firma del catálogo SOFA medido
    // (SignatureRooms.kt). 0 = ninguna, 1..3 = salas. Espejo exacto del
    // C++ (ListenerRoomControls::signatureRoom); la sala pertenece al
    // operador oyente+sala. Con 0 la sala firma no actúa.
    val signatureRoom: Int = 0,
    // Corrección de sala, cut-only (-12..0 dB), 10 bandas.
    // CAVEAT: el JNI fusionado (nativeSetFusedControls) NO tiene slot para
    // estas 10 bandas — el espejo las conserva (clamp + setNeutral las
    // cubren, como el C++), pero la integración nativa debe decidir su vía
    // (extender la firma o mantener la ruta existente) antes de exponerlas
    // en la UI. Esta pantalla no muestra controles para ellas.
    val roomCorrDb: List<Float> = List(10) { 0.0f },
) {
    fun clamp(): ListenerRoomControls = copy(
        rt60Sec = clampf(rt60Sec, 0.05f, 1.5f),
        roomSizeM = clampf(roomSizeM, 2.0f, 12.0f),
        roomWet = clampf(roomWet, 0.0f, 1.0f),
        signatureRoom = signatureRoom.coerceIn(0, 3),
        roomCorrDb = roomCorrDb.take(10).map { clampf(it, -12.0f, 0.0f) }
            .let { if (it.size < 10) it + List(10 - it.size) { 0.0f } else it },
    )

    fun isNeutral(): Boolean {
        if (roomWet > 0.0f || hearingProfile != FusedHearingProfile.FLAT) return false
        if (signatureRoom != 0) return false
        return roomCorrDb.all { it == 0.0f }
    }
}

// ── 4. VOLTERRA·Ω — CORRECTOR DE TRANSDUCTOR ────────────────────────────────
// Fusiona: Volterra (H2) + EmfEstimator (Back-EMF) + Transducer Operator +
// Temporal Spatial Dynamics + Transient Detector.
data class TransducerControls(
    val correction: Float = 1.0f,    // cantidad de corrección 0..1. Default 1.0 (Volterra activo).
    val temporalAmount: Float = 0.0f, // dinámica espacial temporal 0..1.
    val emfEnabled: Boolean = true,   // sensado Back-EMF.
) {
    fun clamp(): TransducerControls = copy(
        correction = clampf(correction, 0.0f, 1.0f),
        temporalAmount = clampf(temporalAmount, 0.0f, 1.0f),
    )

    fun isNeutral(): Boolean = correction <= 0.0f && temporalAmount <= 0.0f
}

// ── 5. ARMONÍA — BALANCE TONAL ÚNICO ────────────────────────────────────────
// Fusiona: EvolutionaryEQ (512 bandas) + Psychoacoustics (ISO 226) +
// Perceptual Loudness (LUFS/trim) + EliteTargetCurves (Harman + insignia) +
// auto_eq_filter + EQ de usuario (10 bandas).
data class TonalControls(
    val curve: FusedTonalCurve = FusedTonalCurve.HARMAN, // 0 plana, 1 Harman 2018, 2 élite (insignia).
    val trimDb: Float = 0.0f,     // trim -6..+6 dB (neto 0.0 dB por Matched-LUFS).
    val lowDb: Float = 0.0f,      // graves ±18 dB (canónico, CanonicalRanges.eqBandDb).
    val midDb: Float = 0.0f,      // medios ±18 dB (canónico, CanonicalRanges.eqBandDb).
    val highDb: Float = 0.0f,     // agudos ±18 dB (canónico, CanonicalRanges.eqBandDb).
    // v2.5.473 (§2.2 #38): presencia 8–12 kHz. Antes no existía como campo
    // fusionado (solo vivía en DSPState.presence); ahora viaja al daemon
    // vía buildFusedPerceptualFields ("holosEqPresence") y se persiste.
    val presenceDb: Float = 0.0f,  // presencia -12..+12 dB.
    val loudnessComp: Boolean = true, // compensación de loudness (ISO 226).
) {
    fun clamp(): TonalControls = copy(
        trimDb = clampf(trimDb, -6.0f, 6.0f),
        lowDb = clampf(lowDb, -18.0f, 18.0f),
        midDb = clampf(midDb, -18.0f, 18.0f),
        highDb = clampf(highDb, -18.0f, 18.0f),
        presenceDb = clampf(presenceDb, -12.0f, 12.0f),
    )

    fun isNeutral(): Boolean =
        curve == FusedTonalCurve.FLAT && trimDb == 0.0f && lowDb == 0.0f &&
            midDb == 0.0f && highDb == 0.0f && presenceDb == 0.0f
}

// ── 6. PULSO — DINÁMICA MAESTRA ─────────────────────────────────────────────
// Fusiona: compresor multibanda LR4 + exciter + widener M/S + safety
// limiter + voice (prosodia) + upmix (ensanchamiento estéreo / inmersión).
// El upmixer HOA vive en el Operador Espacial; aquí "upmix" es el
// ensanchamiento estéreo final y el control de inmersión que gobierna al
// upmixer espacial.
data class DynamicsControls(
    val punch: Float = 0.0f,           // pegada 0..1.
    val width: Float = 1.0f,           // ancho M/S 0..2 (1 = neutro).
    val safety: Float = 1.0f,          // seguridad / limitador 0..1 (1 = activo).
    val compThresholdDb: Float = -12.0f, // umbral del compresor -60..0 dB.
    val compRatio: Float = 1.0f,       // ratio 1..20 (1 = sin compresión).
    val exciter: Float = 0.0f,         // exciter 0..1.
    val voice: Float = 0.0f,           // voz / prosodia 0..1.
    val upmixImmersivity: Float = 0.0f, // inmersión del upmixer espacial 0..2 (0 = off).
) {
    fun clamp(): DynamicsControls = copy(
        punch = clampf(punch, 0.0f, 1.0f),
        width = clampf(width, 0.0f, 2.0f),
        safety = clampf(safety, 0.0f, 1.0f),
        compThresholdDb = clampf(compThresholdDb, -60.0f, 0.0f),
        compRatio = clampf(compRatio, 1.0f, 20.0f),
        exciter = clampf(exciter, 0.0f, 1.0f),
        voice = clampf(voice, 0.0f, 1.0f),
        upmixImmersivity = clampf(upmixImmersivity, 0.0f, 2.0f),
    )

    fun isNeutral(): Boolean =
        punch <= 0.0f && width == 1.0f && compRatio <= 1.0f &&
            exciter <= 0.0f && voice <= 0.0f && safety <= 0.0f &&
            upmixImmersivity <= 0.0f
}

// ── 7. NEURO·Ω — CEREBRO NEURAL ─────────────────────────────────────────────
// Fusiona: NPE (clasificador de género) + AudioBandEnergyClassifier +
// Predictive Audio Engine + Phase Oracle (Kalman) + Self-Healing Engine +
// IvannaTinyML / TinyMLKernel + Content Profile Engine + CinematicEngineHost.
// Sin controles de audio: el cerebro observa y gobierna, no procesa la
// señal directamente (salvo el NPE y el bucle cerrado, que en MANUAL quedan
// en bypass).
data class BrainControls(
    val mode: FusedBrainMode = FusedBrainMode.AUTO, // 0 AUTO (adapta), 1 MANUAL (observa, no actúa).
    val adaptation: Float = 1.0f, // cantidad de adaptación 0..1 (en MANUAL se fuerza a 0).
) {
    fun clamp(): BrainControls {
        val a = clampf(adaptation, 0.0f, 1.0f)
        return copy(adaptation = if (mode == FusedBrainMode.MANUAL) 0.0f else a)
    }

    /** Neutro (audio) = el cerebro no actúa sobre la señal. */
    fun isNeutral(): Boolean = mode == FusedBrainMode.MANUAL || adaptation <= 0.0f
}

// ── 8. CONSERVACIÓN PERCEPTUAL — ÁRBITRO DE CAMPO ───────────────────────────
// NO es un operador más: es el árbitro físico-perceptual común del secreto
// de luna. Estado de campo único por banda que los 7 operadores respetan;
// el solucionador central combina sus propuestas bajo energía-fase-
// causalidad. Réplica exacta de `ConservationControls` del C++
// (fused/fused_controls.hpp). APAGADO por defecto (amount 0 = bypass
// bit-exacto, fail-closed).
data class ConservationControls(
    val amount: Float = 0.0f,          // 0..1: cuánto aplica la conservación (0 = apagado)
    val energyCap: Float = 1.0f,       // 0..1: techo de energía por banda (1 = sin techo)
    val phaseCoherence: Float = 0.5f,  // 0..1: suavidad del cambio de ganancia (coherencia de fase)
    val inertia: Float = 0.82f,        // 0..0.99: continuidad temporal del estado de campo
) {
    fun clamp(): ConservationControls = copy(
        amount = clampf(amount, 0.0f, 1.0f),
        energyCap = clampf(energyCap, 0.0f, 1.0f),
        phaseCoherence = clampf(phaseCoherence, 0.0f, 1.0f),
        inertia = clampf(inertia, 0.0f, 0.99f),
    )

    /** Neutro (audio) = la capa no toca la señal. */
    fun isNeutral(): Boolean = amount <= 0.0f
}

// ── AGREGADO: LA ÚNICA SUPERFICIE DE CONTROL DE LA CADENA ──────────────────
data class FusedControls(
    val spatial: SpatialControls = SpatialControls(),
    val field: FieldControls = FieldControls(),
    val listenerRoom: ListenerRoomControls = ListenerRoomControls(),
    val transducer: TransducerControls = TransducerControls(),
    val tonal: TonalControls = TonalControls(),
    val dynamics: DynamicsControls = DynamicsControls(),
    val brain: BrainControls = BrainControls(),
    val conservation: ConservationControls = ConservationControls(), // 8. Árbitro de campo (apagado por defecto).
) {
    fun clamp(): FusedControls = copy(
        spatial = spatial.clamp(),
        field = field.clamp(),
        listenerRoom = listenerRoom.clamp(),
        transducer = transducer.clamp(),
        tonal = tonal.clamp(),
        dynamics = dynamics.clamp(),
        brain = brain.clamp(),
        conservation = conservation.clamp(),
    )

    /** Toda la cadena neutra = bit-transparente. */
    fun isNeutral(): Boolean =
        spatial.isNeutral() && field.isNeutral() &&
            listenerRoom.isNeutral() && transducer.isNeutral() &&
            tonal.isNeutral() && dynamics.isNeutral() &&
            brain.isNeutral() && conservation.isNeutral()

    /**
     * Pone toda la cadena en neutro (bypass total, ganancia 1.0 exacta).
     * Réplica exacta de `FusedControls::setNeutral()` del C++: solo toca
     * los campos que el C++ toca — azimut, distancia, modo, head-tracking,
     * inmersividad espacial, preset Phi, bombeo/coherencia/gamma/calidez/
     * dinámica/RT60 del campo, RT60/tamaño de sala, loudness, umbral del
     * compresor y EMF conservan su valor (no afectan al neutro).
     */
    fun setNeutral(): FusedControls = copy(
        spatial = spatial.copy(
            amount = 0.0f, wfsSpread = 0.0f, width = 1.0f,
            hrtfWet = 0.0f, xtcEnabled = false, upmixEnabled = false,
            upmixImmersivity = 0.0f,
        ),
        field = field.copy(amount = 0.0f, phiAmount = 0.0f),
        listenerRoom = listenerRoom.copy(
            roomWet = 0.0f, hearingProfile = FusedHearingProfile.FLAT,
            signatureRoom = 0, // v2.5.472: sin sala firma en neutro.
            roomCorrDb = List(10) { 0.0f },
        ),
        transducer = transducer.copy(correction = 0.0f, temporalAmount = 0.0f),
        tonal = tonal.copy(
            curve = FusedTonalCurve.FLAT, trimDb = 0.0f,
            lowDb = 0.0f, midDb = 0.0f, highDb = 0.0f, presenceDb = 0.0f,
        ),
        dynamics = dynamics.copy(
            punch = 0.0f, width = 1.0f, compRatio = 1.0f,
            exciter = 0.0f, voice = 0.0f, safety = 0.0f, upmixImmersivity = 0.0f,
        ),
        brain = brain.copy(mode = FusedBrainMode.MANUAL, adaptation = 0.0f),
        conservation = conservation.copy(
            amount = 0.0f, energyCap = 1.0f,
            phaseCoherence = 0.5f, inertia = 0.82f,
        ),
    )
}

/**
 * Telemetría de solo lectura del guardián AntiDolby, publicada por NEURO·Ω.
 * NaN = sin lectura (nativo sin implementar o sin señal). Nunca se inventa
 * un valor: la UI muestra "—" cuando no hay lectura.
 */
data class AntiDolbyTelemetry(
    val widener: Float = Float.NaN,
    val eqBoostDb: Float = Float.NaN,
    val spread: Float = Float.NaN,
    val intensity: Float = Float.NaN,
) {
    val isAvailable: Boolean =
        widener.isFinite() || eqBoostDb.isFinite() ||
            spread.isFinite() || intensity.isFinite()
}

// ── BRIDGE JNI ───────────────────────────────────────────────────────────────
/**
 * FusedControlsBridge — puente JNI de la cadena fusionada.
 *
 * Patrón del repo (NativeBridge / IvannaNativeLib): objeto único, carga por
 * [NativeLibraryLoader.ensureLoaded] (punto único de System.loadLibrary, no
 * un loadLibrary propio), `external fun` con @JvmStatic y envoltorios
 * `safe*` fail-closed (sin lib cargada o sin símbolo, log y no-op; los
 * getters devuelven NaN / null, nunca un valor inventado).
 *
 * SÍMBOLOS JNI QUE EL C++ DEBE IMPLEMENTAR (integración pendiente):
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeSetFusedControls
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeSetFusedNeutral
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeGetFusedThdDb
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeGetAntiDolbyWidener
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeGetAntiDolbyEqBoostDb
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeGetAntiDolbySpread
 *   Java_com_ivanna_omega_audio_FusedControlsBridge_nativeGetAntiDolbyIntensity
 *
 * Hasta que el C++ los implemente, cada llamada lanza UnsatisfiedLinkError
 * y los envoltorios safe* la absorben (log + no-op / NaN). La UI sigue
 * funcionando y mostrando "—" en la telemetría.
 */
object FusedControlsBridge {
    private const val TAG = "FusedControlsBridge"

    val isLoaded: Boolean = NativeLibraryLoader.ensureLoaded()

    // ── Escritura: cadena completa ──────────────────────────────────────
    /**
     * Empuja los 7 operadores al nativo de una sola vez.
     *
     * Mapeo de enteros (contrato JNI, NO los valores crudos del C++):
     *  - spatialMode: 0 = HEADPHONES, 1 = SPEAKERS. El nativo traduce a
     *    OmegaOutputMode (HEADPHONES = 1u, SPEAKERS = 2u); jamás pasar el
     *    Int tal cual al campo C++ (0 crudo = UNKNOWN = silencio fail-closed).
     *  - fieldPhiPreset: 0 = teléfono, 1 = sala, 2 = auto.
     *  - roomHearingProfile: 0 = plano, 1 = Harman, 2 = personalizado.
     *  - tonalCurve: 0 = plana, 1 = Harman 2018, 2 = élite.
     *  - brainMode: 0 = AUTO, 1 = MANUAL.
     *
     * La corrección de sala de 10 bandas (cut-only, ListenerRoomControls)
     * NO viaja en esta llamada: la firma no tiene slot para ella (ver
     * caveat en [ListenerRoomControls.roomCorrDb]).
     */
    @JvmStatic external fun nativeSetFusedControls(
        spatialAmount: Float,
        spatialAzimuthDeg: Float,
        spatialDistanceM: Float,
        spatialWidth: Float,
        spatialMode: Int,
        spatialWfsSpread: Float,
        spatialHrtfWet: Float,
        spatialHeadTracking: Boolean,
        spatialXtc: Boolean,
        spatialUpmix: Boolean,
        spatialImmersivity: Float,
        fieldAmount: Float,
        fieldPhiAmount: Float,
        fieldPhiPreset: Int,
        fieldPumping: Float,
        fieldCoherence: Float,
        fieldHarmonics: Float,
        fieldGamma: Float,
        fieldWarmth: Float,
        fieldDynamics: Float,
        fieldRt60: Float,
        roomRt60: Float,
        roomSizeM: Float,
        roomWet: Float,
        roomHearingProfile: Int,
        transCorrection: Float,
        transTemporal: Float,
        transEmf: Boolean,
        tonalCurve: Int,
        tonalTrimDb: Float,
        tonalLowDb: Float,
        tonalMidDb: Float,
        tonalHighDb: Float,
        tonalLoudness: Boolean,
        dynPunch: Float,
        dynWidth: Float,
        dynSafety: Float,
        dynThrDb: Float,
        dynRatio: Float,
        dynExciter: Float,
        dynVoice: Float,
        dynUpmix: Float,
        brainMode: Int,
        brainAdaptation: Float,
        consvAmount: Float,
        consvEnergyCap: Float,
        consvPhaseCoherence: Float,
        consvInertia: Float,
    )

    /** Bypass total de la cadena (equivale a FusedControls::setNeutral()). */
    @JvmStatic external fun nativeSetFusedNeutral()

    /** THD+N medido de la cadena fusionada, en dB (telemetría). */
    @JvmStatic external fun nativeGetFusedThdDb(): Float

    /**
     * Estado de la conservación perceptual en la Ruta A (JSON).
     * {"active":0|1,"appliedGain":0..1,"amount":0..1,"energyCap":0..1,
     *  "phaseCoherence":0..1,"inertia":0..0.99}. appliedGain es la ganancia
     * que la capa está aplicando AHORA (1 = no recorta nada).
     */
    @JvmStatic external fun nativeGetConservationStatusJson(): String

    // ── Telemetría del guardián AntiDolby (solo lectura, la publica NEURO·Ω) ──
    /** Widener actual que el guardián AntiDolby está limitando. */
    @JvmStatic external fun nativeGetAntiDolbyWidener(): Float

    /** Boost de EQ actual que el guardián AntiDolby está limitando, en dB. */
    @JvmStatic external fun nativeGetAntiDolbyEqBoostDb(): Float

    /** Spread actual que el guardián AntiDolby está limitando. */
    @JvmStatic external fun nativeGetAntiDolbySpread(): Float

    /** Intensidad actual del guardián AntiDolby (0..1). */
    @JvmStatic external fun nativeGetAntiDolbyIntensity(): Float

    // ── Envoltorios seguros (fail-closed, patrón NativeBridge.safe*) ──────

    /** Empuja [controls] (previamente clampados) al nativo. No-op si la lib no cargó. */
    fun safeSetFusedControls(controls: FusedControls) {
        if (!isLoaded) {
            Log.w(TAG, "Native library not loaded; ignoring setFusedControls")
            return
        }
        val c = controls.clamp()
        try {
            nativeSetFusedControls(
                spatialAmount = c.spatial.amount,
                spatialAzimuthDeg = c.spatial.azimuthDeg,
                spatialDistanceM = c.spatial.distanceM,
                spatialWidth = c.spatial.width,
                spatialMode = c.spatial.mode.jni,
                spatialWfsSpread = c.spatial.wfsSpread,
                spatialHrtfWet = c.spatial.hrtfWet,
                spatialHeadTracking = c.spatial.headTracking,
                spatialXtc = c.spatial.xtcEnabled,
                spatialUpmix = c.spatial.upmixEnabled,
                spatialImmersivity = c.spatial.upmixImmersivity,
                fieldAmount = c.field.amount,
                fieldPhiAmount = c.field.phiAmount,
                fieldPhiPreset = c.field.phiPreset.jni,
                fieldPumping = c.field.pumping,
                fieldCoherence = c.field.coherence,
                fieldHarmonics = c.field.harmonics,
                fieldGamma = c.field.gamma,
                fieldWarmth = c.field.warmth,
                fieldDynamics = c.field.dynamics,
                fieldRt60 = c.field.roomRt60,
                roomRt60 = c.listenerRoom.rt60Sec,
                roomSizeM = c.listenerRoom.roomSizeM,
                roomWet = c.listenerRoom.roomWet,
                roomHearingProfile = c.listenerRoom.hearingProfile.jni,
                transCorrection = c.transducer.correction,
                transTemporal = c.transducer.temporalAmount,
                transEmf = c.transducer.emfEnabled,
                tonalCurve = c.tonal.curve.jni,
                tonalTrimDb = c.tonal.trimDb,
                tonalLowDb = c.tonal.lowDb,
                tonalMidDb = c.tonal.midDb,
                tonalHighDb = c.tonal.highDb,
                tonalLoudness = c.tonal.loudnessComp,
                dynPunch = c.dynamics.punch,
                dynWidth = c.dynamics.width,
                dynSafety = c.dynamics.safety,
                dynThrDb = c.dynamics.compThresholdDb,
                dynRatio = c.dynamics.compRatio,
                dynExciter = c.dynamics.exciter,
                dynVoice = c.dynamics.voice,
                dynUpmix = c.dynamics.upmixImmersivity,
                brainMode = c.brain.mode.jni,
                brainAdaptation = c.brain.adaptation,
                // 8. Conservación perceptual (árbitro de campo, 2026-11-21).
                consvAmount = c.conservation.amount,
                consvEnergyCap = c.conservation.energyCap,
                consvPhaseCoherence = c.conservation.phaseCoherence,
                consvInertia = c.conservation.inertia,
            )
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to call nativeSetFusedControls", t)
        }
        // Ruta B (v2.5.469): el JNI solo escribe la Ruta A; sin este empuje
        // el efecto en audioserver nunca se entera. Independiente del JNI
        // (si el nativo falla, la Ruta B igual recibe los valores).
        pushFusedToDaemon(c)
    }

    // ── Ruta B: empuje fusionado al daemon (v2.5.469) ─────────────────────
    /**
     * Mapeo fusionado → SET_PERCEPTUAL_STATE (canal legacy app → daemon →
     * snapshot SHM → efecto). Sin este empuje, los sliders de FUSIÓN solo
     * escribían g_fusedChainRouteA (Ruta A, app) y la Ruta B (efecto en
     * audioserver, la vía de escucha de Qobuz/Tidal) nunca se enteraba:
     * mover sliders no cambiaba el sonido (defecto 2026-11-18).
     *
     * Solo se envían los campos que SET_PERCEPTUAL_STATE acepta (los que
     * el responder del daemon parsea; ver daemon_command_responder.h).
     * El daemon hace merge (ausente = se conserva el vigente), así que
     * los campos legacy que la pantalla fusionada no controla (p. ej.
     * orbeProximityDb, holosDistanceM, highCutHz) NO se tocan.
     *
     * La puerta de bocinas (RouteBDeviceGate, en OmegaEngineBridge.sendCommand)
     * neutraliza en bocinas/Genezi: orbeAmount, holosAmount, spatialWidth,
     * harmonicGain, compressor, upmixing*, holosEq*. Esos sliders solo suenan
     * en audífonos por diseño (fix del bofo). Los NO gateados (Φ hyper*,
     * Volterra, outputMode) SÍ llegan a Ruta B en bocinas.
     *
     * Campos fusionados SIN equivalente en SET_PERCEPTUAL_STATE (no viajan
     * en este fix; necesitan su propia acción del daemon):
     *  - spatial.azimuthDeg (no hay campo de azimut en el snapshot).
     *  - field.pumping, field.coherence (sin campo HOLOS en el snapshot).
     *  - listenerRoom.roomSizeM, hearingProfile (sin campo en el snapshot).
     *  - listenerRoom.roomCorrDb[10] (sin slot en la firma JNI).
     *  - transducer.emfEnabled (sin campo Back-EMF en el snapshot).
     *  - transducer.correction: solo on/off via volterraEnabled (el monto
     *    0..1 no viaja; no hay campo de corrección en el snapshot).
     *  - tonal.curve, tonal.trimDb, tonal.loudnessComp (sin campo en el
     *    snapshot; el trim es Matched-LUFS de Ruta A).
     *  - dynamics.safety, dynamics.voice (sin campo en el snapshot).
     *  - dynamics.upmixImmersivity (comparte upmixingImmersivity con el
     *    espacial; se envía el valor espacial).
     *  - spatial.hrtfWet (solo on/off: no hay monto wet en el snapshot;
     *    además hrtfEnabled es SET_AUDIO_FEATURES, no SET_PERCEPTUAL_STATE).
     *  - spatial.wfsSpread/wfsEnabled (SET_WFS), transducer.temporalAmount
     *    (SET_TEMPORAL_FIELD), dynamics.punch (SET_PUNCH),
     *    dynamics.compRatio (SET_ADAPTIVE_STATE como compAmount),
     *    brain.adaptation (SET_INTENSITY), brain.mode (SET_NPE),
     *    spatial.xtcEnabled (SET_XTC; el XTC de Φ sí viaja por
     *    hyperXtcEnabled), listenerRoom.rt60Sec/roomWet (SET_ROOM_RT60).
     *
     * Devuelve el mapa campo → valor (puro, sin Android) para poder
     * unit-testear el mapeo sin el socket.
     */
    fun buildFusedPerceptualFields(c: FusedControls): Map<String, Float> {
        val clamped = c.clamp()
        // ── CONTROL DEL GEMELO (orden de Esse, 2026-11-21; caza 2026-11-22) ──
        // La FusedChain (C++, sección 8 de pushEffectiveControls) ya aplica
        // estos mismos defaults cuando la calibración P3 está aplicada
        // (SPEAKERS, válida) y el usuario no fijó amount ni techo: amount
        // 0.5 / techo 0.8 (kTwinCalibratedDefaultConservation*). PERO el
        // snapshot solo guarda lo que la app EMPUJA, y la app empujaba el
        // amount crudo de la pref (0.0), así que el snapshot marcaba
        // consv=0.0 aunque el gemelo SÍ actuaba dentro de la FusedChain
        // (el 0.0 es la entrada, no el valor efectivo). Eso hacía imposible
        // verificar que el gemelo actúa. Por eso el push aplica aquí los
        // mismos defaults del gemelo: el snapshot muestra consv=0.5 y el
        // efecto recibe 0.5 (lo mismo que la FusedChain pondría sola), así
        // que el audio NO cambia — solo se vuelve VISIBLE y verificable.
        // El amount del usuario (> 0) y un techo fijado (< 1.0) mandan.
        // En audífonos no se aplica (la calibración del Genezi no aplica).
        val twinSpeakers = clamped.spatial.mode == FusedSpatialMode.SPEAKERS
        val twinConsvAmount =
            if (twinSpeakers && clamped.conservation.amount <= 0f) 0.5f
            else clamped.conservation.amount
        val twinConsvEnergyCap =
            if (twinSpeakers && clamped.conservation.energyCap >= 1f) 0.8f
            else clamped.conservation.energyCap
        return mapOf(
            // ORBE·Ω (espacial). orbeSpread = ancho espacial (0..2 → 0..1.5).
            "orbeAmount" to clamped.spatial.amount,
            "orbeDistanceM" to clamped.spatial.distanceM,
            "orbeSpread" to clamped.spatial.width.coerceIn(0f, 1.5f),
            "orbeHeadTracking" to if (clamped.spatial.headTracking) 1f else 0f,
            // v2.5.472: escala del parallax (0..1). El daemon la guarda como
            // deseada y la aplica a la pose de SET_HEAD_POSE (Ruta B).
            "parallaxAmount" to clamped.spatial.parallaxAmount,
            // Modo de salida: JNI 0 = audífonos, 1 = bocinas; el daemon
            // usa 1 = audífonos, 2 = bocinas (0 = desconocido).
            "outputMode" to if (clamped.spatial.mode == FusedSpatialMode.SPEAKERS) 2f else 1f,
            "upmixingEnabled" to if (clamped.spatial.upmixEnabled) 1f else 0f,
            "upmixingImmersivity" to clamped.spatial.upmixImmersivity,
            // HOLOS·Ω (campo).
            "holosAmount" to clamped.field.amount,
            "holosHarmonics" to clamped.field.harmonics,
            "holosWarmth" to clamped.field.warmth,
            "holosDynamics" to clamped.field.dynamics,
            "holosGamma" to clamped.field.gamma,
            "holosRoomRt60" to clamped.field.roomRt60,
            // Φ (HyperState) + Volterra. Φ NO está gateado en bocinas.
            "hyperAmount" to clamped.field.phiAmount,
            "hyperPreset" to clamped.field.phiPreset.jni.toFloat(),
            "hyperXtcEnabled" to if (clamped.spatial.xtcEnabled) 1f else 0f,
            "volterraEnabled" to if (clamped.transducer.correction > 0f) 1f else 0f,
            // ARMONÍA (tonal): EQ de 4 bandas → holosEq* (v2.5.473 §2.2 #38:
            // presence viajaba solo por DSPState; ahora también va al daemon).
            "holosEqLow" to clamped.tonal.lowDb,
            "holosEqMid" to clamped.tonal.midDb,
            "holosEqHigh" to clamped.tonal.highDb,
            "holosEqPresence" to clamped.tonal.presenceDb,
            // v2.5.473 §2.2 #13: radio de cabeza medido (Ruta B lo recibe
            // como dato; la HRTF de Ruta B sigue KEMAR hasta ABI v15).
            "orbeHeadRadiusM" to clamped.spatial.headRadiusM,
            // PULSO (dinámica): ancho M/S + umbral del compresor + exciter.
            "spatialWidth" to clamped.dynamics.width,
            "compressor" to clamped.dynamics.compThresholdDb,
            "harmonicGain" to clamped.dynamics.exciter,
            // CONSERVACIÓN PERCEPTUAL (árbitro de campo, ABI v17, 2026-11-21).
            // El daemon los guarda y los aplica al snapshot (consv_*); el
            // efecto los empuja a la FusedChain de Ruta B vía
            // setConservationParams → atomics → syncFusedControlsFromAtomics.
            // amount 0 = apagado (bypass bit-exacto). NO gateado en bocinas:
            // es un guarda de energía, válido en ambos modos.
            // (Gemelo: amount/techo con defaults del gemelo en SPEAKERS —
            // ver arriba. La escala de confianza 0.80 la aplica la
            // FusedChain vía twinInterventionScale; el efectivo es 0.5×0.80.)
            "consvAmount" to twinConsvAmount,
            "consvEnergyCap" to twinConsvEnergyCap,
            "consvPhaseCoherence" to clamped.conservation.phaseCoherence,
            "consvInertia" to clamped.conservation.inertia,
        )
    }

    /**
     * Empuja los controles fusionados al daemon por SET_PERCEPTUAL_STATE
     * (mismo canal que los controles legacy). El daemon hace merge y
     * publica un snapshot nuevo; el efecto lo aplica vía
     * omega_apply_snapshot → setters → atomics → syncFusedControlsFromAtomics.
     * Fail-closed: si el daemon no está o el envío falla, se loguea y no se
     * rompe nada (la Ruta A ya recibió los valores por JNI).
     */
    fun pushFusedToDaemon(controls: FusedControls) {
        try {
            val fields = buildFusedPerceptualFields(controls)
            val json = JSONObject().apply {
                put("action", "SET_PERCEPTUAL_STATE")
                for ((key, value) in fields) {
                    put(key, value.toDouble())
                }
            }
            val ok = OmegaEngineBridge.sendCommand(json)
            if (!ok) Log.w(TAG, "pushFusedToDaemon: SET_PERCEPTUAL_STATE no llegó al daemon")
        } catch (t: Throwable) {
            Log.e(TAG, "pushFusedToDaemon falló", t)
        }
    }

    /** Bypass total de la cadena en el nativo. No-op si la lib no cargó. */
    fun safeSetFusedNeutral() {
        if (!isLoaded) {
            Log.w(TAG, "Native library not loaded; ignoring setFusedNeutral")
            return
        }
        try {
            nativeSetFusedNeutral()
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to call nativeSetFusedNeutral", t)
        }
        // Ruta B (v2.5.469): el neutro también debe llegar al daemon, si no
        // la Ruta B seguiría con los valores viejos aunque la Ruta A quede neutra.
        pushFusedToDaemon(FusedControls().setNeutral())
    }

    /** THD+N de la cadena en dB, o NaN si no hay lectura. */
    fun safeGetFusedThdDb(): Float {
        if (!isLoaded) return Float.NaN
        return try {
            nativeGetFusedThdDb().takeIf { it.isFinite() } ?: Float.NaN
        } catch (t: Throwable) {
            Float.NaN
        }
    }

    /** Estado JSON de la conservación en la Ruta A, o null si no hay nativo. */
    fun safeGetConservationStatusJson(): String? {
        if (!isLoaded) return null
        return try {
            nativeGetConservationStatusJson()
        } catch (t: Throwable) {
            Log.e(TAG, "Failed to call nativeGetConservationStatusJson", t)
            null
        }
    }

    /** Telemetría del guardián AntiDolby (4 lecturas), o todo-NaN si no hay lectura. */
    fun safeGetAntiDolbyTelemetry(): AntiDolbyTelemetry {
        if (!isLoaded) return AntiDolbyTelemetry()
        return try {
            AntiDolbyTelemetry(
                widener = nativeGetAntiDolbyWidener().takeIf { it.isFinite() } ?: Float.NaN,
                eqBoostDb = nativeGetAntiDolbyEqBoostDb().takeIf { it.isFinite() } ?: Float.NaN,
                spread = nativeGetAntiDolbySpread().takeIf { it.isFinite() } ?: Float.NaN,
                intensity = nativeGetAntiDolbyIntensity().takeIf { it.isFinite() } ?: Float.NaN,
            )
        } catch (t: Throwable) {
            AntiDolbyTelemetry()
        }
    }
}
