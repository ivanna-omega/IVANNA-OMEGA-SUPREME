#pragma once
// fused_brain.hpp — OPERADOR #7: CEREBRO NEURAL (Fusión Magistral, 2026-11-18).
//
// Fusiona en un solo operador: NPE (clasificador de género) +
// AudioBandEnergyClassifier + Predictive Audio Engine + Phase Oracle
// (Kalman) + Self-Healing Engine + IvannaTinyML / TinyMLKernel +
// Content Profile Engine + CinematicEngineHost + bucle cerrado
// neuro-acústico (ClosedLoopController + NeuroBandEq) + memoria del
// super-agente.
//
// Controles (BrainControls, ver fused/fused_controls.hpp): SOLO modo
// (0=AUTO, 1=MANUAL) y adaptation 0..1. Este operador NO tiene controles
// de audio: en MANUAL (o adaptation==0) OBSERVA pero NO actúa sobre la
// señal (bit-transparente, ganancia exactamente 1.0); en AUTO actúa vía
// NPE + bucle cerrado (NeuroBandEq).
//
// Qué vive dónde (honesto):
//  - Nativo (envuelto aquí): IvannaAudioClassifier, IvannaVoiceProsodyEngine,
//    IvannaTinyML (TinyMLAudioEngine) + IvannaTinyMLKernel, PredictiveAudioEngine,
//    PhaseKalman3 (Phase Oracle — ver nota abajo), IvannaSelfHealingEngine,
//    IvannaSuperAgentMemory, ClosedLoopController + NeuroBandEq, NpeStereoCore.
//  - Solo Kotlin, NO envueltos en C++ (no existe equivalente nativo):
//    AudioBandEnergyClassifier (audio/AudioBandEnergyClassifier.kt, `internal
//    object`, lo consume AudioPipeline.kt), ContentProfileEngine
//    (audio/ContentProfileEngine.kt) y CinematicEngineHost
//    (audio/CinematicEngineHost.kt, dueño del RealTimeCinematicEngine).
//    Viven en la capa de app y se gobiernan por el modo del cerebro: la
//    pantalla fusionada los cablea — en MANUAL quedan en observación /
//    apagados y en AUTO activos — pero este núcleo nativo NO los toca.
//    (grep 2026-11-18: cero hits de los tres en C++; solo fused_controls.hpp
//    los nombra como parte de la fusión.)
//  - Phase Oracle: se usa PhaseKalman3 (phase_oracle_kalman.hpp), la fuente
//    única de verdad del filtro (P = F·P·Fᵀ + Q completa, guardas de
//    finitud). PhaseOracle (phase_oracle_engine.hpp) es la variante previa
//    con covarianza diagonal aproximada; phase_oracle.cpp mismo delega en
//    PhaseKalman3. No se duplica el filtro.
//
// Bucle cerrado: ClosedLoopController::compute() se alimenta SIN sensor
// real enchufado (sin driver EEG/ABR/fNIRS ni micrófono — ver
// neuro_acoustic_closed_loop.hpp: sin frames el bucle "permanece apagado").
// Aquí NO se fabrican frames de sensor: compute() corre con su contador
// interno de bloques y, sin frames frescos, devuelve identidad
// (fail-closed) y NeuroBandEq queda en bypass bit-exacto. En el teléfono
// el driver real (JNI nativeNeuroLoopIngest / modo AcousticMic) alimenta
// ingest() y el bucle aprende de verdad; en host el cableado está
// verificado pero el bucle no actúa. No se finge telemetría de sensor.
//
// Memoria del super-agente: prepare() llama a
// IvannaSuperAgentMemory::initialize() con la ruta del dispositivo
// (/data/adb/ivanna_omega/agent_memory.bin, la misma del FusionEngine).
// En host /data no existe → open() falla → initialize() devuelve false
// (fail-closed, sin crear archivos, sin crashear) y la memoria trabaja
// solo en local (updateContext sigue alimentando el estado local).
//
// Hilos: IvannaAudioClassifier, IvannaTinyMLAudioKernel y (solo en
// Android) TinyMLAudioEngine arrancan su hilo de inferencia en su
// constructor y lo cierran (join) en su destructor — igual que en el
// resto del árbol. En host, TinyMLAudioEngine NO arranca hilo y su
// IngestAudio() publica una salida FIJA de stub (probabilidades
// {0.05,0.40,0.15,0.10,0.20,0.10}, MUSIC, confianza 0.85 — ver
// IvannaTinyML.cpp, rama !__ANDROID__): su telemetría en host NO es
// una medición real. La del clasificador y la del kernel sí corren su
// inferencia real (con pesos cero / no entrenados en host).
// Self-Healing: NO se arranca su hilo de monitorización
// (startMonitoring) desde el camino de audio; solo se alimenta su
// heartbeat (pingAudioEngine) por trozo. El monitor es del hilo de
// control, no del DSP.
//
// GUARDIÁN AntiDolby (dentro de NEURO·Ω, NO es un efecto de sonido:
// es protección). AntiDolbyState se alimenta del clasificador: cada
// vez que hay clasificación válida, updateFromNeuralContext(clase,
// confianza, energía) fija los objetivos (MUSIC: widener 1.35,
// spread 1.25, EQ 0 dB, exciter solo <120 Hz; VOICE: widener 0.75,
// spread 0.90, EQ +2.5 dB en 2-4 kHz) y tick(dt) suaviza hacia ellos
// (attack 20 ms / release 200 ms). Sus salidas son la modulación
// guardiana para los OTROS operadores (Dinámica lee el widener,
// Tonal el boost 2-4 kHz, Espacial el spread): currentWidener(),
// currentEqBoost(), currentSpreadMultiplier(), currentIntensity().
// En MANUAL el guardián sigue observando (clasifica y suaviza) pero
// su intensidad efectiva es 0 (no modula: widener 1.0, boost 0 dB,
// spread 1.0); en AUTO modula con su intensidad (= adaptation).
//
// NOMBRE: NEURO·Ω (aprobado por Esse, 2026-11-18 ~07:06 CST).
// La IA Auto ES NEURO·Ω en modo AUTO (el cerebro funcionando solo,
// sin fusión aparte). IvannaSuperAgentMemory ES la memoria / capa de
// agentes del cerebro (ya envuelta aquí).
//
// Capa de app (Kotlin, NO implementada en C++): los directorios
// app/src/main/java/com/ivanna/omega/agent/ (AgentApi, BehaviorEvolution,
// IvannaAgentCore, IvannaDecisionAuthority, SelfHealingAgent) y
// app/src/main/java/com/ivanna/omega/ai/ (AIInferenceEngine,
// AdaptiveLearning, AntiDolbyCrnnClassifier, DSPDecision,
// HybridDecisionEngine, PerceptualBrainCortex/Engine, PerceptualCortex,
// PerceptualDecisionEngine, RealtimeLearningController, SAFCore, ...)
// viven en la capa de app y se gobiernan por el modo del cerebro
// (AUTO = actúan, MANUAL = solo observan). Este núcleo nativo NO los
// toca; la pantalla fusionada los cablea al modo.
//
// FotoHolo NO va en este operador (es foto holográfica, no audio):
// queda como su propio módulo, alimentado por las mediciones de los
// operadores. No se toca aquí.
//
// Tiempo real: sin malloc/locks/I-O en process(). Scratch pre-reservado
// (miembros). La entrada no finita se sanea a 0.0f en el scratch (los
// motores nunca ven NaN/Inf); la salida no finita se sanea a 0.0 en
// double (fail-closed). En neutro process() NO toca el buffer de entrada.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "fused/fused_controls.hpp"
#include "IvannaAudioClassifier.hpp"
#include "IvannaVoiceProsodyEngine.hpp"
#include "IvannaTinyML.hpp"
#include "IvannaTinyMLKernel.hpp"
#include "predictive_audio_engine.hpp"
#include "phase_oracle_kalman.hpp"
#include "IvannaSelfHealingEngine.hpp"
#include "IvannaSuperAgentMemory.hpp"
#include "anti_dolby.h"
#include "neuromorphic/neuro_acoustic_closed_loop.hpp"
#include "neuromorphic/NpeStereoCore.hpp"

namespace ivanna::fused {

// Trozo interno de proceso: 128 frames == Ivanna::BLOCK_SIZE
// (IvannaFusionCore.hpp). El scratch admite hasta kFusedMaxBlockFrames.
inline constexpr int kBrainChunkFrames = 128;

// Parámetros del NPE en AUTO: los defaults del FusionEngine.
inline constexpr float kBrainNpeHarmonic = 0.2f;
inline constexpr float kBrainNpeLateral = 0.2f;
inline constexpr float kBrainNpeOhc = 0.3f;
inline constexpr float kBrainNpeMasterDb = 0.0f;

// Ruta del archivo de memoria del super-agente en el dispositivo
// (la misma que usa IvannaFusionEngine).
inline constexpr const char* kBrainAgentMemoryPath =
    "/data/adb/ivanna_omega/agent_memory.bin";

class FusedBrain {
public:
    FusedBrain() = default;
    ~FusedBrain() = default;
    FusedBrain(const FusedBrain&) = delete;
    FusedBrain& operator=(const FusedBrain&) = delete;

    void prepare(double sampleRate) noexcept;
    void setControls(const BrainControls& c) noexcept;
    BrainControls controls() const noexcept { return controls_; }

    // Camino caliente. Neutro (MANUAL o adaptation==0): solo observa
    // (análisis) y retorna sin tocar el buffer (bit-transparente).
    // AUTO: observa + actúa (bucle cerrado + NPE).
    void process(double* left, double* right, int frames) noexcept;

    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "NEURO·Ω"; }

    // Telemetría (lo último observado; 0 si aún no hay clasificación
    // válida). confidence() = confianza del clasificador de género
    // (IvannaAudioClassifier, 0..1; 0 = sin salida válida todavía).
    int dominantGenre() const noexcept { return dominantGenre_; }
    float confidence() const noexcept { return confidence_; }
    // Telemetría adicional del bucle cerrado / predictivo (0 si inactivo).
    float predictiveConfidence() const noexcept { return predictiveConfidence_; }
    float loopConfidence() const noexcept { return loop_.confidence(); }
    bool agentMemoryReady() const noexcept { return agentMemoryReady_; }

    // ── Guardián AntiDolby (modulación para los otros operadores) ───
    // En MANUAL devuelven el neutro (intensidad 0: widener 1.0, boost
    // 0 dB, spread 1.0); en AUTO, los valores suavizados del guardián.
    float antiDolbyWidener() const noexcept { return antiDolby_.currentWidener(); }
    float antiDolbyEqBoostDb() const noexcept { return antiDolby_.currentEqBoost(); }
    float antiDolbySpreadMul() const noexcept { return antiDolby_.currentSpreadMultiplier(); }
    float antiDolbyIntensity() const noexcept { return antiDolby_.currentIntensity(); }

    // ── Clasificación adaptativa (fix CORONA Frente A, defecto 3) ────
    // Reenvía el flag del frontend mel real del clasificador (ver
    // IvannaAudioClassifier::setRealFrontendEnabled). Default ON
    // (orden Esse 2026-11-25, todo activo en la 502): el clasificador
    // usa la FFT + banco mel reales y el guardián AntiDolby es
    // adaptativo. Apagarlo vuelve al legacy (siempre MUSIC).
    // Sin cableado JNI/UI: nadie lo apaga por accidente desde la app.
    void setAdaptiveClassificationEnabled(bool on) noexcept {
        classifier_.setRealFrontendEnabled(on);
    }
    bool adaptiveClassificationEnabled() const noexcept {
        return classifier_.isRealFrontendEnabled();
    }

private:
    void runAnalysisChunk(const float* l, const float* r, int n) noexcept;
    // F2/486 (labor grande, motor double): la actuación corre en double
    // de punta a punta (NeuroBandEqD + NpeStereoCoreD); el análisis sigue
    // en float (clasificadores/TinyML son control por diseño).
    void runActuationChunk(double* l, double* r, int n) noexcept;
    // Intensidad del guardián según el modo: 0 en neutro, adaptation en AUTO.
    void applyGuardianIntensity() noexcept {
        antiDolby_.setAntiDolbyIntensity(controls_.isNeutral() ? 0.0f
                                                               : controls_.adaptation);
    }

    BrainControls controls_{};  // default: AUTO, adaptation 1.0
    double sampleRate_ = 48000.0;
    bool prepared_ = false;

    // ── Análisis (siempre corren; nunca tocan el audio) ─────────────
    Ivanna::IvannaAudioClassifier classifier_;
    Ivanna::IvannaVoiceProsodyEngine prosody_;
    Ivanna::TinyMLAudioEngine tinyMl_;
    ivanna::tinyml::IvannaTinyMLAudioKernel tinyMlKernel_;
    ivanna::PredictiveAudioEngine predictive_;
    ivanna::PhaseKalman3 oracle_;  // Phase Oracle (fuente única de verdad)
    Ivanna::IvannaSelfHealingEngine selfHealing_;
    Ivanna::IvannaSuperAgentMemory agentMemory_;
    bool agentMemoryReady_ = false;

    // ── Guardián AntiDolby (protección; no toca el audio directamente)
    // Se alimenta del clasificador en runAnalysisChunk(); sus salidas
    // modulan a los OTROS operadores (no a este buffer).
    AntiDolbyState antiDolby_;

    // ── Actuación (solo en AUTO, adaptation > 0) ─────────────────────
    // loop_ (ClosedLoopController) queda float: produce ganancias de
    // control. bandEq_/npe_ son la señal en double (F2/486).
    ivanna::neuromorphic::ClosedLoopController loop_;
    ivanna::neuromorphic::NeuroBandEqD bandEq_;
    ivanna::neuromorphic::NpeStereoCoreD npe_;

    // ── Telemetría ────────────────────────────────────────────────────
    int dominantGenre_ = 0;
    float confidence_ = 0.0f;
    float predictiveConfidence_ = 0.0f;

    // ── Scratch pre-reservado (sin malloc en process()) ──────────────
    // Señal en double por rebanada (máx kFusedMaxBlockFrames) para la
    // actuación; copia float por trozo de 128 para el análisis
    // (clasificadores/TinyML) y buffer entrelazado para TinyMLAudioEngine.
    double scratchL_[kFusedMaxBlockFrames]{};
    double scratchR_[kFusedMaxBlockFrames]{};
    float anaL_[kBrainChunkFrames]{};
    float anaR_[kBrainChunkFrames]{};
    float scratchInterleaved_[2 * kBrainChunkFrames]{};
};

}  // namespace ivanna::fused
