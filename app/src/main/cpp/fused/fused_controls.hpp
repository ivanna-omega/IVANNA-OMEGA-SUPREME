#pragma once
// fused_controls.hpp — FUSIÓN MAGISTRAL (2026-11-18, directiva de Esse).
//
// Los ~45 efectos del producto se fusionan en 7 operadores únicos. Este
// header es LA ÚNICA superficie de control de la cadena fusionada: cada
// operador expone SOLO su struct de aquí. Los cascarones y controles de los
// efectos fusionados quedan como LEGACY (sus bridges *_controls_bridge.cpp y
// *_globals_effect.cpp siguen existiendo — nunca se borra código — pero la
// UI y la cadena caliente ya no los usan; la fuente de verdad es
// FusedControls).
//
// Reglas duras:
//  - Matched-LUFS: ganancia neta 0.0 dB SOLO EN NEUTRO (bit-transparente)
//    y como objetivo NOMINAL de P8 (el compilador compensa la pérdida
//    MEDIDA con el trim; ver experience_compiler.cpp). NO es una
//    propiedad de la cadena enganchada: la P0 enganchada sin trim mide
//    −6.065/−6.274 dB en banda ancha (p0_reference_values.hpp) y ALL7
//    a 1.0 mide −14.18 dB (test_p1p4_ablation.cpp, sonda #1432). La
//    afirmación vieja ("0.0 dB siempre") quedó RETIRADA el 2026-11-26
//    (P1+P4, ver tests/p1p4_golden_rule.md): fuera de neutro, la única
//    ganancia es la que dictan los controles Y la que mide la cadena.
//  - Neutro = bit-transparente: process() no toca el buffer cuando
//    isNeutral() es true (así la cadena en doble precisión mide el piso
//    aritmético, no el redondeo float32).
//  - Todos los controles se clampan (fail-closed); ningún valor fuera de
//    rango llega al DSP.
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "../include/omega_control_bus.h"

namespace ivanna::fused {

// Máximo de frames por llamada a process() sin re-allocar: cada operador
// pre-reserva su scratch en prepare() para este tamaño y procesa en
// trozos si le llega un bloque mayor. Sin malloc en el camino caliente.
constexpr int kFusedMaxBlockFrames = 8192;

inline float clampf(float v, float lo, float hi) noexcept {
    if (!std::isfinite(v)) return lo;
    return std::clamp(v, lo, hi);
}

// ── 1. OPERADOR ESPACIAL ÚNICO ──────────────────────────────────────────────
// Fusiona: ORBE + WFS + HOA + HRTF (SOFA) + Crosstalk Canceller + Head
// Tracker SO(3) + Scene Geometry Renderer + Stereo Object Decomposer +
// AuxFieldCoherence + AuxGeneziRoute + DopPacketizer.
// Controles (solo estos): azimut, distancia, ancho, modo.
// El upmixer HOA vive aquí (crea el campo que el decoder HOA consume —
// unidad indivisible); el control de inmersión se expone en Dinámica.
struct SpatialControls {
    float amount = 1.0f;            // ORBE amount 0..1 (0 = bypass). Default 1.0 (directiva 2026-11-08).
    float azimuthDeg = 0.0f;        // azimut -180..180 deg.
    float distanceM = 1.0f;         // distancia 0.15..2.0 m.
    float width = 1.0f;             // ancho 0..2 (1 = neutro).
    OmegaOutputMode mode = OmegaOutputMode::HEADPHONES;  // audífonos / bocinas (Genezi).
    float wfsSpread = 0.0f;         // apertura WFS 0..1 (0 = WFS apagado).
    float hrtfWet = 1.0f;           // wet HRTF 0..1.
    bool headTracking = false;      // seguimiento de cabeza (yaw). v2.5.471: la pose la alimenta IvannaHeadTracker (sensor → feedRotationVector en Ruta A, SET_HEAD_POSE en Ruta B); el default se mantiene en false (opt-in) porque activarlo sin pose medida cambia el THD+N activo (ver test ThdAtEngaged).
    bool xtcEnabled = false;        // cancelación de diafonía (altavoces).
    bool upmixEnabled = false;      // upmixer HOA (off por defecto).
    float upmixImmersivity = 1.25f; // inmersividad del upmixer 0..2.
    // PARCHE v2.6 (Fallo 4 / Fallo 3): coherencia AUX en SPEAKERS (preset
    // GeneziRoom). Solo cuando auxEnabled==true el AuxField corre en
    // SPEAKERS-neutro; por defecto false, así un SPEAKERS-neutro fresco
    // sigue siendo bit-exacto y el test SpeakersModeGate pasa. Quien
    // quiera la coherencia de sala en altavoces pone auxEnabled=true.
    bool auxEnabled = false;
    // v2.5.472 (Fase 2, ítem E): escala del parallax por head-tracking
    // 0..1 (1 = comportamiento actual). Con 0 el tracker no mueve el
    // sonido (el azimut del control sigue mandando); con 1 el yaw/pitch/
    // roll del tracker se suman íntegros. Vive en SpatialControls (junto
    // a headTracking), NO en PhysicsControls (eliminado por el paquete
    // Física Nueva Consolidada). Lo cablea FusedSpatial::applyPose() y lo
    // escala el daemon en Ruta B (SET_HEAD_POSE / SET_PERCEPTUAL_STATE).
    float parallaxAmount = 1.0f;

    void clamp() noexcept {
        amount = clampf(amount, 0.0f, 1.0f);
        azimuthDeg = clampf(azimuthDeg, -180.0f, 180.0f);
        distanceM = clampf(distanceM, 0.15f, 2.0f);
        width = clampf(width, 0.0f, 2.0f);
        wfsSpread = clampf(wfsSpread, 0.0f, 1.0f);
        hrtfWet = clampf(hrtfWet, 0.0f, 1.0f);
        upmixImmersivity = clampf(upmixImmersivity, 0.0f, 2.0f);
        parallaxAmount = clampf(parallaxAmount, 0.0f, 1.0f);
        // GENEZI (evol-holos, 2026-11-26) es modo válido: NO se reescribe
        // a HEADPHONES (antes cualquier modo distinto de HEADPHONES/
        // SPEAKERS caía a HEADPHONES y el Genezi perdía su distinción).
        if (mode != OmegaOutputMode::HEADPHONES && mode != OmegaOutputMode::SPEAKERS &&
            mode != OmegaOutputMode::GENEZI)
            mode = OmegaOutputMode::HEADPHONES;
        // Puerta HRTF (phone-loop): sin ORBE no hay escena binaural que
        // renderizar. Si amount es 0, la HRTF también es 0: la cadena hace
        // bypass limpio en vez de sustituir por la HRTF sintética de reserva
        // (notch audible). El clamp corre en FusedChain::setControls, así que
        // cubre el slider, el JNI y el snapshot.
        // Excepción: con headTracking el HRTF SÍ es la escena — la pose
        // del tracker se renderiza por la etapa HRTF, que solo es
        // alcanzable con amount = 0 (con amount > 0 la etapa es ORBE y
        // el pitch no se oye por ella). Sin esta excepción la puerta
        // mataba el pitch audible por tracking (test PitchAudibleViaHrtfStage).
        if (amount <= 0.0f && !headTracking) hrtfWet = 0.0f;
    }
    // Neutro = nada de espacialización: la señal sale idéntica.
    // PARCHE v2.6 (Fallo 4, reemplaza el "nunca neutro en SPEAKERS" del
    // Fallo 4): en SPEAKERS el operador es neutro salvo que auxEnabled
    // esté activo (entonces el AuxField GeneziRoom corre por diseño y la
    // salida NO es bit-exacta). Con auxEnabled==false (default), un
    // SPEAKERS-neutro fresco es bit-exacto (el test SpeakersModeGate lo
    // exige); las colas de fundido las cubre FusedSpatial::process() con
    // hasActiveTail(), no isNeutral().
    bool isNeutral() const noexcept {
        // En GENEZI el AuxField también corre (como en SPEAKERS): con
        // auxEnabled la salida NO es bit-exacta.
        const bool auxActive = ivanna::omega_output_is_speakers_like(mode) && auxEnabled;
        return !auxActive && amount <= 0.0f && wfsSpread <= 0.0f && width == 1.0f &&
               hrtfWet <= 0.0f && !xtcEnabled && !upmixEnabled;
    }
};

// ── 2. OPERADOR DE CAMPO UNIFICADO ──────────────────────────────────────────
// Fusiona: HOLOS + Phi (Φ) + Gamma (Ley 1, campo Γ) + SpectralCore +
// NEU·Ω (compilador espectral holográfico) + IvannaFusionCore (etapa de
// campo del engine) + OmniField (IVANNA).
// Controles (solo estos): cantidad, bombeo cero, coherencia.
// La Fase B −102.79 dB (ancla linkBandGainsDb) se preserva: el operador
// envuelve HolosOperator sin tocarlo.
struct FieldControls {
    float amount = 1.0f;     // HOLOS amount 0..1. Default 1.0 (preset ATREVIDA 502, directiva Esse 2026-11-24).
    float phiAmount = 0.5f;  // Phi amount 0..1. Default 0.5 (directiva 2026-11-08).
    int phiPreset = 0;       // Phi: 0 teléfono, 1 sala, 2 auto.
    float pumping = 0.0f;    // bombeo 0..1. TARGET 0.0 (bombeo cero, magistral).
    float coherence = 0.8f;  // coherencia 0..1.
    float harmonics = 0.002f;  // armónicos HOLOS 0..0.002 (kHolosHarmonicsMaxValidated; ATREVIDA 502).
    float gamma = 1.0f;      // campo Γ (Ley 1) 0..2.
    float warmth = 0.0f;     // calidez 0..1.
    float dynamics = 1.0f;   // dinámica 0..1 (1 = sin compresión extra).
    float roomRt60 = 0.34f;  // RT60 de la sala del campo 0.05..1.5 s.
    // PARCHE v2.6 (Fallo 3a, Puerta Genezi en HOLOS·Ω): modo de salida.
    // FusedChain lo copia desde spatial.mode (pushEffectiveControls);
    // en SPEAKERS, FusedField apaga el render binaural GreenKernel y
    // la cola rir_ (diffuse=0). Default HEADPHONES (fail-closed).
    OmegaOutputMode mode = OmegaOutputMode::HEADPHONES;

    void clamp() noexcept {
        amount = clampf(amount, 0.0f, 1.0f);
        phiAmount = clampf(phiAmount, 0.0f, 1.0f);
        phiPreset = std::clamp(phiPreset, 0, 2);
        pumping = clampf(pumping, 0.0f, 1.0f);
        coherence = clampf(coherence, 0.0f, 1.0f);
        harmonics = clampf(harmonics, 0.0f, 0.002f);
        gamma = clampf(gamma, 0.0f, 2.0f);
        warmth = clampf(warmth, 0.0f, 1.0f);
        dynamics = clampf(dynamics, 0.0f, 1.0f);
        roomRt60 = clampf(roomRt60, 0.05f, 1.5f);
        // GENEZI (evol-holos, 2026-11-26) es modo válido: NO se reescribe
        // a HEADPHONES (ver SpatialControls::clamp).
        if (mode != OmegaOutputMode::HEADPHONES && mode != OmegaOutputMode::SPEAKERS &&
            mode != OmegaOutputMode::GENEZI)
            mode = OmegaOutputMode::HEADPHONES;
    }
    bool isNeutral() const noexcept { return amount <= 0.0f && phiAmount <= 0.0f; }
};

// ── 3. MODELO OYENTE+SALA ───────────────────────────────────────────────────
// Fusiona: RIR (room_model / RirConvolver) + SAF (modelo espacial / PCA /
// HRTF) + Hearing Adaptation + BioControl 5D + corrección de sala (cut-only).
// Controles (solo estos): sala (RT60, tamaño), perfil auditivo.
struct ListenerRoomControls {
    float rt60Sec = 0.34f;    // RT60 0.05..1.5 s.
    float roomSizeM = 5.0f;   // tamaño de sala 2..12 m.
    float roomWet = 0.22f;    // wet RIR 0..1. Default 0.22 (valor vivo).
    int hearingProfile = 0;   // 0 = plano, 1 = Harman, 2 = personalizado.
    // v2.5.472 Fase 2 (Sala Viva): sala firma del catálogo SOFA medido
    // (SignatureRooms.kt). 0 = ninguna, 1..3 = salas. La sala pertenece
    // al operador oyente+sala (no a un struct Physics: eliminado por el
    // paquete Física Nueva Consolidada). Con 0 la sala firma no actúa.
    int signatureRoom = 0;
    float roomCorrDb[10] = {};  // corrección de sala, cut-only (-12..0 dB).

    void clamp() noexcept {
        rt60Sec = clampf(rt60Sec, 0.05f, 1.5f);
        roomSizeM = clampf(roomSizeM, 2.0f, 12.0f);
        roomWet = clampf(roomWet, 0.0f, 1.0f);
        hearingProfile = std::clamp(hearingProfile, 0, 2);
        signatureRoom = std::clamp(signatureRoom, 0, 3);
        for (int i = 0; i < 10; ++i)
            roomCorrDb[i] = clampf(roomCorrDb[i], -12.0f, 0.0f);
    }
    bool isNeutral() const noexcept {
        if (roomWet > 0.0f || hearingProfile != 0 || signatureRoom != 0) return false;
        for (int i = 0; i < 10; ++i)
            if (roomCorrDb[i] != 0.0f) return false;
        return true;
    }
};

// ── 4. CORRECTOR DE TRANSDUCTOR ─────────────────────────────────────────────
// Fusiona: Volterra (H2) + EmfEstimator (Back-EMF) + Transducer Operator +
// Temporal Spatial Dynamics + Transient Detector.
// Controles (solo este): cantidad de corrección.
struct TransducerControls {
    float correction = 1.0f;    // cantidad de corrección 0..1. Default 1.0 (Volterra activo).
    float temporalAmount = 0.0f;  // dinámica espacial temporal 0..1.
    bool emfEnabled = true;       // sensado Back-EMF.

    void clamp() noexcept {
        correction = clampf(correction, 0.0f, 1.0f);
        temporalAmount = clampf(temporalAmount, 0.0f, 1.0f);
    }
    bool isNeutral() const noexcept { return correction <= 0.0f && temporalAmount <= 0.0f; }
};

// ── 5. BALANCE TONAL ÚNICO ──────────────────────────────────────────────────
// Fusiona: EvolutionaryEQ (512 bandas) + Psychoacoustics (ISO 226) +
// Perceptual Loudness (LUFS/trim) + EliteTargetCurves (Harman + insignia) +
// auto_eq_filter + EQ de usuario (10 bandas).
// Controles (solo estos): curva, trim.
struct TonalControls {
    int curve = 1;           // 0 = plana, 1 = Harman 2018, 2 = élite (insignia).
    float trimDb = 0.0f;     // trim -24..+24 dB (compensador maestro P8:
                             // lleva la ganancia neta NOMINAL a 0.0 dB
                             // compensando la pérdida MEDIDA de la cadena;
                             // la cadena sin trim NO es 0.0 dB, ver
                             // tests/p1p4_golden_rule.md).
    // El rango es ±24 dB (antes ±6 dB) porque el trim es el compensador
    // maestro de P8: la cadena real pierde ~14.2 dB con los 7 operadores
    // a 1.0 (NEURO solo −15.57 dB/unidad, medido, fix #1432 2026-11-25)
    // y el trim debe poder compensarla (+14.18 dB). El techo
    // (0.9885531f, −0.1 dBFS) sigue siendo la protección dura contra
    // clipping; el clamp solo acota el rango del control.
    float lowDb = 0.0f;      // graves -12..+12 dB.
    float midDb = 0.0f;      // medios -12..+12 dB.
    float highDb = 0.0f;     // agudos -12..+12 dB.
    bool loudnessComp = true;  // compensación de loudness (ISO 226).
    // P8 — CORONA D10 (2026-11-25): escalar tonal CONTINUO 0..1.
    // Default 1.0 = comportamiento actual, sin cambio. Escala las 10
    // ganancias de banda (applyControls) y el EvoEQ por mezcla wet/dry
    // (processBlock128); el trimDb (trim maestro / compensador P8) NO se
    // escala. amount == 0 → neutro total (isNeutral true).
    float amount = 1.0f;     // escalar tonal 0..1 (1 = actual, 0 = neutro).

    void clamp() noexcept {
        curve = std::clamp(curve, 0, 2);
        trimDb = clampf(trimDb, -24.0f, 24.0f);
        lowDb = clampf(lowDb, -12.0f, 12.0f);
        midDb = clampf(midDb, -12.0f, 12.0f);
        highDb = clampf(highDb, -12.0f, 12.0f);
        amount = clampf(amount, 0.0f, 1.0f);
    }
    bool isNeutral() const noexcept {
        if (amount <= 0.0f) return true;  // P8: escalar 0 = neutro total.
        return curve == 0 && trimDb == 0.0f && lowDb == 0.0f &&
               midDb == 0.0f && highDb == 0.0f;
    }
};

// ── 6. DINÁMICA MAESTRA ─────────────────────────────────────────────────────
// Fusiona: compresor multibanda LR4 + exciter + widener M/S + safety
// limiter + voice (prosodia) + upmix (ensanchamiento estéreo / inmersión).
// El upmixer HOA vive en el Operador Espacial (unidad indivisible con el
// decoder HOA); aquí "upmix" es el ensanchamiento estéreo final y el
// control de inmersión que gobierna al upmixer espacial.
// Controles (solo estos): pegada, ancho, seguridad.
struct DynamicsControls {
    float punch = 0.0f;           // pegada 0..1.
    float width = 1.0f;           // ancho M/S 0..2 (1 = neutro).
    float safety = 1.0f;          // seguridad / limitador 0..1 (1 = activo).
    float compThresholdDb = -12.0f;  // umbral del compresor -60..0 dB.
    float compRatio = 1.0f;       // ratio 1..20 (1 = sin compresión).
    float exciter = 0.0f;         // exciter 0..1.
    float voice = 0.0f;           // voz / prosodia 0..1.
    float upmixImmersivity = 0.0f;  // inmersión del upmixer espacial 0..2 (0 = off).

    void clamp() noexcept {
        punch = clampf(punch, 0.0f, 1.0f);
        width = clampf(width, 0.0f, 2.0f);
        safety = clampf(safety, 0.0f, 1.0f);
        compThresholdDb = clampf(compThresholdDb, -60.0f, 0.0f);
        compRatio = clampf(compRatio, 1.0f, 20.0f);
        exciter = clampf(exciter, 0.0f, 1.0f);
        voice = clampf(voice, 0.0f, 1.0f);
        upmixImmersivity = clampf(upmixImmersivity, 0.0f, 2.0f);
    }
    bool isNeutral() const noexcept {
        return punch <= 0.0f && width == 1.0f && compRatio <= 1.0f &&
               exciter <= 0.0f && voice <= 0.0f && safety <= 0.0f &&
               upmixImmersivity <= 0.0f;
    }
};

// ── 7. CEREBRO NEURAL ───────────────────────────────────────────────────────
// Fusiona: NPE (clasificador de género) + AudioBandEnergyClassifier +
// Predictive Audio Engine + Phase Oracle (Kalman) + Self-Healing Engine +
// IvannaTinyML / TinyMLKernel + Content Profile Engine + CinematicEngineHost
// (estos dos últimos viven en la capa de app/Kotlin y se gobiernan por el
// modo del cerebro; el núcleo nativo los cubre NPE + clasificador +
// predictivo + oráculo + self-healing + TinyML + bucle cerrado).
// Controles (solo este): modo auto/manual. Sin controles de audio: el
// cerebro observa y gobierna, no procesa la señal directamente (salvo el
// NPE y el bucle cerrado, que en MANUAL quedan en bypass).
struct BrainControls {
    int mode = 0;            // 0 = AUTO (adapta), 1 = MANUAL (observa, no actúa).
    float adaptation = 1.0f;  // cantidad de adaptación 0..1 (en MANUAL se fuerza a 0).

    void clamp() noexcept {
        mode = std::clamp(mode, 0, 1);
        adaptation = clampf(adaptation, 0.0f, 1.0f);
        if (mode == 1) adaptation = 0.0f;
    }
    // Neutro (audio) = el cerebro no actúa sobre la señal.
    bool isNeutral() const noexcept { return mode == 1 || adaptation <= 0.0f; }
};

// ── 8. CONSERVACIÓN PERCEPTUAL (árbitro de campo, 2026-11-21) ────────────
// NO es un operador más: es el árbitro físico-perceptual común del secreto
// de luna. Estado de campo único por banda (presión, velocidad de partícula
// 3D, energía, tiempo perceptual, curvatura) que los 7 operadores respetan;
// el solucionador central combina sus propuestas bajo energía-fase-
// causalidad. Vive en fused/perceptual_conservation.hpp/.cpp y se enchufa
// en FusedChain tras los 7 operadores + APEX, antes de la matriz TΩ.
// APAGADO por defecto (amount 0 = bypass bit-exacto, fail-closed).
struct ConservationControls {
    float amount = 0.0f;          // 0..1: cuánto aplica la conservación (0 = apagado)
    float energyCap = 1.0f;       // 0..1: techo de energía por banda (1 = sin techo)
    float phaseCoherence = 0.5f;  // 0..1: suavidad del cambio de ganancia (coherencia de fase)
    float inertia = 0.82f;        // 0..0.99: continuidad temporal del estado de campo

    void clamp() noexcept {
        amount = clampf(amount, 0.0f, 1.0f);
        energyCap = clampf(energyCap, 0.0f, 1.0f);
        phaseCoherence = clampf(phaseCoherence, 0.0f, 1.0f);
        inertia = clampf(inertia, 0.0f, 0.99f);
    }
    // Neutro (audio) = la capa no toca la señal.
    bool isNeutral() const noexcept { return amount <= 0.0f; }
};

// ── AGREGADO: LA ÚNICA SUPERFICIE DE CONTROL DE LA CADENA ──────────────────
struct FusedControls {
    SpatialControls spatial;
    FieldControls field;
    ListenerRoomControls listenerRoom;
    TransducerControls transducer;
    TonalControls tonal;
    DynamicsControls dynamics;
    BrainControls brain;
    ConservationControls conservation;  // 8. Árbitro de campo (apagado por defecto).

    void clamp() noexcept {
        spatial.clamp();
        field.clamp();
        listenerRoom.clamp();
        transducer.clamp();
        tonal.clamp();
        dynamics.clamp();
        brain.clamp();
        conservation.clamp();
    }
    // Toda la cadena neutra = bit-transparente (la medida de THD+N en
    // neutro es el piso aritmético de la cadena en doble precisión).
    bool isNeutral() const noexcept {
        return spatial.isNeutral() && field.isNeutral() &&
               listenerRoom.isNeutral() && transducer.isNeutral() &&
               tonal.isNeutral() && dynamics.isNeutral() &&
               brain.isNeutral() && conservation.isNeutral();
    }
    // Pone toda la cadena en neutro (bypass total, ganancia 1.0 exacta).
    void setNeutral() noexcept {
        spatial.amount = 0.0f;
        spatial.wfsSpread = 0.0f;
        spatial.width = 1.0f;
        spatial.hrtfWet = 0.0f;
        spatial.xtcEnabled = false;
        spatial.upmixEnabled = false;
        spatial.upmixImmersivity = 0.0f;
        spatial.auxEnabled = false;  // PARCHE v2.6: sin coherencia AUX en neutro.
        field.amount = 0.0f;
        field.phiAmount = 0.0f;
        listenerRoom.roomWet = 0.0f;
        listenerRoom.hearingProfile = 0;
        listenerRoom.signatureRoom = 0;  // v2.5.472: sin sala firma en neutro.
        for (int i = 0; i < 10; ++i) listenerRoom.roomCorrDb[i] = 0.0f;
        transducer.correction = 0.0f;
        transducer.temporalAmount = 0.0f;
        tonal.curve = 0;
        tonal.trimDb = 0.0f;
        tonal.lowDb = 0.0f;
        tonal.midDb = 0.0f;
        tonal.highDb = 0.0f;
        dynamics.punch = 0.0f;
        dynamics.width = 1.0f;
        dynamics.compRatio = 1.0f;
        dynamics.exciter = 0.0f;
        dynamics.voice = 0.0f;
        dynamics.safety = 0.0f;
        dynamics.upmixImmersivity = 0.0f;
        brain.mode = 1;
        brain.adaptation = 0.0f;
        conservation.amount = 0.0f;
        conservation.energyCap = 1.0f;
        conservation.phaseCoherence = 0.5f;
        conservation.inertia = 0.82f;
    }
};

}  // namespace ivanna::fused
