#pragma once
#include "dsp_types.h"
#include "../dsp/ChebHarmonicShaper.hpp"
#include <algorithm>
#include <cmath>
#include <vector>

namespace ivanna {

// Harmonic exciter: drive -> Chebyshev T2+T3 (M9) or soft-clip Padé [3/2] (legacy fallback)
// CON ANTI-ALIASING: Oversampling 2x + pre-LPF 8 kHz + post-LPF 12 kHz
//
// F2/486 (2026-11-20) — MOTOR DOUBLE: estado y matemática en double 64-bit.
// process(double*, double*, int) es el núcleo; process(float*, float*, int)
// es un envoltorio de compat (convierte al núcleo y redondea la salida).
class HarmonicExciter {
public:
    void setParams(const DSPParams& p);
    void process(double* left, double* right, int frames);
    void process(float* left, float* right, int frames);
    void reset();

    // FASE 4C — cierre del lazo adaptativo: reducción runtime sugerida por
    // AdaptiveDecisionEngine (exciter_reduction, 0..1).
    void setRuntimeReduction(float reduction01) noexcept {
        runtimeReductionMul_ = reduction01 < 0.f ? 1.0 : (reduction01 > 1.f ? 0.0 : 1.0 - static_cast<double>(reduction01));
    }

    // Controles Atlas-Escena (M9 / R3):
    // mode = 1: Chebyshev T2+T3 (default), mode = 0: softClip Padé legacy
    void setShaperMode(int mode) noexcept { shaperMode_ = (mode == 0) ? 0 : 1; }
    int  shaperMode() const noexcept { return shaperMode_; }

    void setWarmth(float warmth) noexcept {
        if (std::isfinite(warmth)) warmth_ = std::clamp(static_cast<double>(warmth), 0.0, 1.0);
    }
    double warmth() const noexcept { return warmth_; }

    void setFlatness1m(float f8) noexcept {
        if (std::isfinite(f8)) flatness1m_ = std::clamp(static_cast<double>(f8), 0.0, 1.0);
    }
    double flatness1m() const noexcept { return flatness1m_; }

private:
    double drive_ = 1.0;
    double driveNow_    = 1.0;
    double driveSmooth_ = 0.9995;
    double wet_   = 0.5;
    double dry_   = 0.5;
    double runtimeReductionMul_ = 1.0;  // 1.0 = sin reducción, 0.0 = exciter mudo

    int   shaperMode_ = 1;     // 1 = Chebyshev T2+T3 M9 (default), 0 = Padé legacy
    double warmth_     = 0.5;  // [0..1] balance par T2 / impar T3
    double flatness1m_ = 0.25; // [0..1] anti-IMD spectral flatness proxy

    // Anti-zipper: wetNow_ converge por muestra OS (~15 ms).
    // Arranca en 0.0 para garantizar bypass bit-exacto cuando wet=0.
    double wetNow_    = 0.0;
    double wetSmooth_ = 0.9995;

    // HPF to feed only highs into exciter (3 kHz cutoff)
    Biquad hpfL_, hpfR_;

    // Pre-saturation LPF a 8 kHz @ 48 kHz base
    Biquad preLpfL_, preLpfR_;

    // Anti-aliasing: oversampling 2x buffers
    static constexpr int OS_FACTOR = 2;
    static constexpr int MAX_OS_FRAMES = 4096;
    double osLeft_[MAX_OS_FRAMES * OS_FACTOR]{};
    double osRight_[MAX_OS_FRAMES * OS_FACTOR]{};

    // Resampling interpolation filter (LPF para downsample)
    Biquad osLpfL_, osLpfR_;

    // Polinomios de Chebyshev T2 + T3 con bloqueador DC a tasa OS (2*fs)
    dsp::ChebHarmonicShaperd chebL_{};
    dsp::ChebHarmonicShaperd chebR_{};

    // Interpolación lineal para upsample
    double lastL_ = 0.0, lastR_ = 0.0;

    // Techo interno del exciter (anti clipping digital)
    double excScaleL_ = 1.0, excScaleR_ = 1.0;
    double excRelCoef_ = 0.999;

    // Ortogonalización contra la fundamental (solo armónicos, cero ganancia lineal).
    // Regresión de 1 tap en línea: g = <exc,h>/<h,h> con promedios de ~3 ms a tasa OS.
    // exc − g·h elimina la componente en fase con la entrada (la ganancia lineal del
    // shaper) y deja únicamente los productos no lineales (H2, H3, ...).
    double fundCrossL_ = 0.0, fundPowL_ = 0.0, fundGainL_ = 0.0;
    double fundCrossR_ = 0.0, fundPowR_ = 0.0, fundGainR_ = 0.0;
    double fundCoef_ = 0.99;
    int lastSampleRate_ = 0;

    // Scratch del envoltorio float (crece bajo demanda).
    std::vector<double> wrapL_, wrapR_;
};

} // namespace ivanna
