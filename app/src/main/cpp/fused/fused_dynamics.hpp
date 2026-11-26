#pragma once
// fused_dynamics.hpp — OPERADOR #6: DINÁMICA MAESTRA (Fusión Magistral,
// 2026-11-18, directiva de Esse).
//
// Fusiona: compresor multibanda LR4 + exciter + widener M/S + voice
// (presencia) + gain stage + safety limiter. Los cascarones y controles de
// los efectos fusionados quedan como LEGACY; la fuente de verdad es
// DynamicsControls (fused_controls.hpp).
//
// Orden de la cadena (fijo):
//   1. MultibandCompressor (LR4, 3 bandas)
//   2. HarmonicExciter
//   3. StereoWidener (M/S, "upmix" estéreo final de la Dinámica)
//   4. Voice: lift de presencia inline (high-shelf 1er orden, 3 kHz)
//   5. GainStage (trim de ganancia, unidad — ver nota de punch)
//   6. SafetyLimiter (AL FINAL; protege la salida, techo -0.1 dBFS)
//
// Mapeos exactos control → motor (la única ganancia es la que dictan los
// controles; Matched-LUFS: en neutro ganancia exactamente 1.0):
//   - punch (0..1, pegada): modula el compresor multibanda:
//       umbral efectivo = compThresholdDb - 6.0 * punch   [dB]
//       ratio efectivo  = min(20, compRatio * (1.0 + punch))
//     A más punch, más pegada: el umbral baja hasta -6 dB y el ratio
//     sube hasta el doble. El compresor corre cuando el ratio efectivo
//     es > 1.0 (con punch > 0 el compresor se activa aunque compRatio
//     sea 1.0; con punch == 0 y compRatio == 1.0 hay bypass total).
//     La ganancia de pegada NO es una ganancia extra: es esta
//     modulación de umbral/ratio más el makeup del propio Compressor
//     (limitado a la reducción real instantánea, nunca más de lo que
//     se quitó). El GainStage queda en unidad (mix 0.5 → 0 dB,
//     master 0 → 0 dB) como etapa de trim de la cadena.
//     Ataque 5 ms, release 100 ms (fijos, musicales, sin bombeo).
//   - compThresholdDb / compRatio: umbral y ratio base del compresor
//     multibanda (antes del mapeo de punch de arriba).
//   - exciter (0..1): wet del HarmonicExciter (drive fijo 0.45, el
//     default de DSPParams → drive interno 1 + 0.45*3 = 2.35).
//     Solo corre cuando exciter > 0.
//   - width (0..2, 1 = neutro): ancho M/S del StereoWidener
//     (setWidth directo). Solo corre cuando width != 1.0.
//   - voice (0..1): lift de presencia INLINE (no hay motor de prosodia
//     aquí; el análisis de prosodia vive en el Cerebro): high-shelf
//     RBJ de primer orden a 3 kHz, ganancia = +3.0 * voice dB
//     (0 dB en voice == 0 → bypass, el filtro ni se ejecuta).
//   - safety (0..1): 0 = limitador en bypass; > 0 = activo, con los
//     parámetros de ruta reales (-1 dBFS umbral / -0.1 dBFS techo,
//     los mismos que usan Ruta A y Ruta B). El techo es 0.9885531f
//     lineal (-0.1 dBFS): la salida nunca pasa de 1.0.
//   - upmixImmersivity: este operador lo GUARDA pero NO lo procesa:
//     el upmixer HOA vive en el Operador Espacial y la cadena
//     fusionada enruta este control allí. Por eso process() lo ignora
//     al decidir si hay audio que procesar: solo upmixImmersivity > 0
//     (todo lo demás neutro) es transparente bit-exacto.
//
// Reglas duras (contrato fused_controls.hpp):
//   - Neutro = bit-transparente: process() retorna sin tocar el buffer
//     cuando no hay nada audible que hacer.
//   - Sin malloc / locks / I-O en process(). Scratch miembro
//     pre-reservado en prepare() (kFusedMaxBlockFrames = 8192);
//     bloques mayores se procesan en trozos; dentro, en trozos de
//     128 frames (= Ivanna::BLOCK_SIZE).
//   - Fail-closed: cualquier muestra no finita a la salida es 0.0.

#include "fused/fused_controls.hpp"

#include "dsp/MultibandCompressor.hpp"
#include "../include/GainStage.h"
#include "../include/HarmonicExciter.h"
#include "../include/SafetyLimiter.h"
#include "../include/StereoWidener.h"

#include <cmath>
#include <cstddef>

namespace ivanna::fused {

class FusedDynamics {
public:
    // Trozo interno de proceso (= Ivanna::BLOCK_SIZE). Los motores
    // envueltos tienen tope por llamada (MultibandCompressor 2048,
    // HarmonicExciter 4096); 128 está por debajo de ambos.
    static constexpr int kProcessChunkFrames = 128;
    // Mapeo de punch (ver cabecera): bajada máxima del umbral y
    // factor máximo del ratio.
    static constexpr float kPunchThresholdDropDb = 6.0f;
    // Voice: high-shelf a 3 kHz, ganancia máxima +3 dB.
    static constexpr double kVoiceShelfFreqHz = 3000.0;
    static constexpr double kVoiceMaxGainDb = 3.0;
    // SafetyLimiter: parámetros de ruta reales (-1 dBFS / -0.1 dBFS).
    // Valores EXACTOS a precisión float32 (no redondeos a 5 decimales):
    //   threshold = float32 más cercano a 10^(-1/20)   → -0.9999991 dB
    //   ceiling   = float32 más cercano a 10^(-0.1/20) → -0.0999999 dB
    // El literal anterior de 5 decimales era -0.1000271 dB (2.7e-5 dB más
    // estricto que el -0.1 dB nominal); el techo final de fused_chain.cpp
    // usa ESTA misma constante, así que limiter y techo final no divergen.
    static constexpr float kLimiterThreshold = 0.891251f;
    static constexpr float kLimiterCeiling = 0.9885531f;

    FusedDynamics() = default;
    FusedDynamics(const FusedDynamics&) = delete;
    FusedDynamics& operator=(const FusedDynamics&) = delete;

    void prepare(double sampleRate) noexcept;
    void setControls(const DynamicsControls& c) noexcept;
    DynamicsControls controls() const noexcept { return controls_; }
    void process(double* left, double* right, int frames) noexcept;
    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "PULSO"; }

    // Ratio y umbral efectivos tras el mapeo de punch (para tests /
    // telemetría). No tocan el audio.
    float effectiveRatio() const noexcept;
    float effectiveThresholdDb() const noexcept;

private:
    // Neutro AUDIBLE: ignora upmixImmersivity (lo gobierna el
    // Operador Espacial, no este operador). Si esto es true, process()
    // no toca el buffer aunque controls_.isNeutral() sea false por el
    // upmixImmersivity solo.
    bool audioNeutral() const noexcept;
    void applyControls() noexcept;
    void updateVoiceCoeffs() noexcept;
    void voiceShelf(double* left, double* right, int frames) noexcept;

    DynamicsControls controls_{};
    double sampleRate_ = 48000.0;

    MultibandCompressor comp_{};
    HarmonicExciter exciter_{};
    StereoWidener widener_{};
    SafetyLimiter limiter_{};
    GainStage gain_{};

    // High-shelf de presencia (voice): coeficientes RBJ normalizados
    // (a0 = 1) en double + estado por canal en double.
    double voiceB0_ = 1.0, voiceB1_ = 0.0, voiceB2_ = 0.0;
    double voiceA1_ = 0.0, voiceA2_ = 0.0;
    double voiceX1L_ = 0.0, voiceX2L_ = 0.0, voiceY1L_ = 0.0, voiceY2L_ = 0.0;
    double voiceX1R_ = 0.0, voiceX2R_ = 0.0, voiceY1R_ = 0.0, voiceY2R_ = 0.0;

    // Scratch miembro pre-reservado (sin malloc en process()).
    // F2/486 — MOTOR DOUBLE: scratch en double 64-bit; la cadena ya no
    // baja a float en este operador (los motores envueltos tienen núcleo
    // double; el puente float↔double desaparece de aquí).
    double scratchL_[kFusedMaxBlockFrames]{};
    double scratchR_[kFusedMaxBlockFrames]{};
};

}  // namespace ivanna::fused
