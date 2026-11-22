// fused_dynamics.cpp — OPERADOR #6: DINÁMICA MAESTRA (Fusión Magistral).
// Envuelve los motores dsp/ reales en el orden fijo:
//   MultibandCompressor → HarmonicExciter → StereoWidener → voice shelf
//   → GainStage (unidad) → SafetyLimiter (al final).
// Sin malloc / locks / I-O en process(). Ver fused_dynamics.hpp para los
// mapeos exactos de cada control.
#include "fused/fused_dynamics.hpp"

#include <algorithm>

namespace ivanna::fused {

namespace {
// DSPParams base: mix 0.5 → GainStage input 0 dB; master 0 → output 0 dB.
// (El GainStage de esta cadena es trim en unidad; la pegada vive en el
// mapeo punch → umbral/ratio del compresor, no en una ganancia extra.)
DSPParams unityParams(double sampleRate) noexcept {
    DSPParams p;
    p.mix = 0.5f;
    p.master = 0.0f;
    p.drive = 0.45f;
    p.wet = 0.0f;
    p.sampleRate = static_cast<std::uint32_t>(sampleRate);
    return p;
}
}  // namespace

float FusedDynamics::effectiveRatio() const noexcept {
    const float r = controls_.compRatio * (1.0f + controls_.punch);
    return std::min(20.0f, r);
}

float FusedDynamics::effectiveThresholdDb() const noexcept {
    return controls_.compThresholdDb - kPunchThresholdDropDb * controls_.punch;
}

bool FusedDynamics::audioNeutral() const noexcept {
    return controls_.punch <= 0.0f && controls_.width == 1.0f &&
           controls_.compRatio <= 1.0f && controls_.exciter <= 0.0f &&
           controls_.voice <= 0.0f && controls_.safety <= 0.0f;
}

void FusedDynamics::prepare(double sampleRate) noexcept {
    if (!std::isfinite(sampleRate) || sampleRate < 8000.0 ||
        sampleRate > 768000.0) {
        sampleRate = 48000.0;
    }
    sampleRate_ = sampleRate;

    const DSPParams p = unityParams(sampleRate_);
    comp_.setParams(p);
    comp_.setAttack(5.0f);
    comp_.setRelease(100.0f);
    exciter_.setParams(p);
    widener_.setParams(p);
    gain_.setParams(p);
    limiter_.setSampleRate(static_cast<float>(sampleRate_));
    limiter_.setParams(kLimiterThreshold, kLimiterCeiling);

    applyControls();
    reset();
}

void FusedDynamics::setControls(const DynamicsControls& c) noexcept {
    controls_ = c;
    controls_.clamp();
    applyControls();
}

void FusedDynamics::applyControls() noexcept {
    // Compresor: umbral/ratio efectivos tras el mapeo de punch.
    // setParams() del MultibandCompressor deriva umbral/ratio de
    // DSPParams (alpha/beta), así que el override va DESPUÉS.
    comp_.setThreshold(effectiveThresholdDb());
    comp_.setRatio(effectiveRatio());

    // Exciter: wet = exciter; drive fijo 0.45 (DSPParams default).
    // setParams() aplica drive/wet aunque el sample rate no cambie
    // (el early-return por sr solo salta la reconfiguración de
    // filtros); el wet se re-fija aquí en cada cambio de controles.
    DSPParams p = unityParams(sampleRate_);
    p.wet = controls_.exciter;
    exciter_.setParams(p);

    // Widener: ancho directo (1.0 = neutro).
    widener_.setWidth(controls_.width);

    // Limitador: 0 = bypass, > 0 = activo (parámetros de ruta fijos).
    limiter_.bypass(controls_.safety <= 0.0f);

    updateVoiceCoeffs();
}

void FusedDynamics::updateVoiceCoeffs() noexcept {
    // High-shelf RBJ (S = 1), ganancia +3 dB * voice a 3 kHz.
    // voice == 0 → ganancia 0 dB → identidad; el filtro ni se ejecuta
    // (ver voiceShelf), así que aquí solo se guardan coeficientes.
    const double gainDb = kVoiceMaxGainDb * static_cast<double>(controls_.voice);
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * kVoiceShelfFreqHz / sampleRate_;
    const double cw = std::cos(w0);
    const double sw = std::sin(w0);
    // S = 1 → alpha = sin(w0)/2 * sqrt(2).
    const double alpha = sw * 0.5 * std::sqrt(2.0);
    const double sqrtA = std::sqrt(a);
    const double twoSqrtAAlpha = 2.0 * sqrtA * alpha;
    const double a0 = (a + 1.0) + (a - 1.0) * cw + twoSqrtAAlpha;
    if (!(a0 > 1e-12) || !std::isfinite(a0)) {
        voiceB0_ = 1.0; voiceB1_ = 0.0; voiceB2_ = 0.0;
        voiceA1_ = 0.0; voiceA2_ = 0.0;
        return;
    }
    voiceB0_ = (a * ((a + 1.0) - (a - 1.0) * cw + twoSqrtAAlpha)) / a0;
    voiceB1_ = (2.0 * a * ((a - 1.0) - (a + 1.0) * cw)) / a0;
    voiceB2_ = (a * ((a + 1.0) - (a - 1.0) * cw - twoSqrtAAlpha)) / a0;
    voiceA1_ = (-2.0 * ((a - 1.0) + (a + 1.0) * cw)) / a0;
    voiceA2_ = ((a + 1.0) + (a - 1.0) * cw - twoSqrtAAlpha) / a0;
}

void FusedDynamics::voiceShelf(double* left, double* right, int frames) noexcept {
    if (frames <= 0 || !left || !right) return;
    const double b0 = voiceB0_, b1 = voiceB1_, b2 = voiceB2_;
    const double a1 = voiceA1_, a2 = voiceA2_;
    for (int i = 0; i < frames; ++i) {
        const double xl = left[i];
        double yl = b0 * xl + b1 * voiceX1L_ + b2 * voiceX2L_ - a1 * voiceY1L_ - a2 * voiceY2L_;
        if (!std::isfinite(yl)) {
            yl = 0.0;
            voiceX1L_ = voiceX2L_ = voiceY1L_ = voiceY2L_ = 0.0;
        } else {
            voiceX2L_ = voiceX1L_; voiceX1L_ = xl;
            voiceY2L_ = voiceY1L_; voiceY1L_ = yl;
        }
        left[i] = yl;
        const double xr = right[i];
        double yr = b0 * xr + b1 * voiceX1R_ + b2 * voiceX2R_ - a1 * voiceY1R_ - a2 * voiceY2R_;
        if (!std::isfinite(yr)) {
            yr = 0.0;
            voiceX1R_ = voiceX2R_ = voiceY1R_ = voiceY2R_ = 0.0;
        } else {
            voiceX2R_ = voiceX1R_; voiceX1R_ = xr;
            voiceY2R_ = voiceY1R_; voiceY1R_ = yr;
        }
        right[i] = yr;
    }
}

void FusedDynamics::reset() noexcept {
    comp_.reset();
    exciter_.reset();
    widener_.reset();
    limiter_.reset();
    gain_.reset();
    voiceX1L_ = voiceX2L_ = voiceY1L_ = voiceY2L_ = 0.0;
    voiceX1R_ = voiceX2R_ = voiceY1R_ = voiceY2R_ = 0.0;
}

void FusedDynamics::process(double* left, double* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    // Neutro audible = bit-transparente (upmixImmersivity solo no
    // procesa nada aquí: lo gobierna el Operador Espacial).
    if (audioNeutral()) return;

    const bool compActive = effectiveRatio() > 1.0f;
    const bool exciterActive = controls_.exciter > 0.0f;
    const bool widenerActive = controls_.width != 1.0f;
    const bool voiceActive = controls_.voice > 0.0f;
    const bool limiterActive = controls_.safety > 0.0f;

    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int block = std::min(remaining, kFusedMaxBlockFrames);
        // F2/486 — MOTOR DOUBLE: la cadena ya no baja a float aquí.
        // Se sanea la entrada (no-finito → 0.0) y los motores corren su
        // núcleo double directamente sobre el scratch.
        for (int i = 0; i < block; ++i) {
            const double l = left[offset + i];
            const double r = right[offset + i];
            scratchL_[i] = std::isfinite(l) ? l : 0.0;
            scratchR_[i] = std::isfinite(r) ? r : 0.0;
        }
        // Motores en trozos de 128 frames.
        for (int c = 0; c < block; c += kProcessChunkFrames) {
            const int n = std::min(kProcessChunkFrames, block - c);
            double* l = scratchL_ + c;
            double* r = scratchR_ + c;
            if (compActive) comp_.process(l, r, n);
            if (exciterActive) exciter_.process(l, r, n);
            if (widenerActive) widener_.process(l, r, n);
            if (voiceActive) voiceShelf(l, r, n);
            gain_.processOutput(l, r, n);  // trim en unidad (0 dB)
            if (limiterActive) limiter_.process(l, r, n);  // AL FINAL
        }
        // Fail-closed: no-finito → 0.0.
        for (int i = 0; i < block; ++i) {
            const double l = scratchL_[i];
            const double r = scratchR_[i];
            left[offset + i] = std::isfinite(l) ? l : 0.0;
            right[offset + i] = std::isfinite(r) ? r : 0.0;
        }
        offset += block;
        remaining -= block;
    }
}

}  // namespace ivanna::fused
