#if defined(__clang__)
#pragma clang optimize on
#else
#pragma GCC optimize("O3", "unroll-loops")
#endif

#include "MultibandCompressor.hpp"
#include <cmath>
#include <algorithm>

namespace ivanna {

namespace {
// Q de Butterworth 2o orden (Linkwitz-Riley = 2x Butterworth en serie).
constexpr double kButterworthQ = 0.7071067811865476;
// Fundido de activacion/desactivacion (sin clic).
constexpr double kMixTimeMs = 20.0;
}  // namespace

MultibandCompressor::MultibandCompressor() {
    reset();
}

void MultibandCompressor::updateCrossovers() noexcept {
    if (!crossoversDirty_) return;
    crossoversDirty_ = false;
    const double sr = sr_;
    const double lo = lowHz_;
    const double hi = highHz_;
    auto setup = [&](Crossover& x) {
        x.lp1a.setLowpass(lo, kButterworthQ, sr);
        x.lp1b.setLowpass(lo, kButterworthQ, sr);
        x.hp1a.setHighpass(lo, kButterworthQ, sr);
        x.hp1b.setHighpass(lo, kButterworthQ, sr);
        x.lp2a.setLowpass(hi, kButterworthQ, sr);
        x.lp2b.setLowpass(hi, kButterworthQ, sr);
        x.hp2a.setHighpass(hi, kButterworthQ, sr);
        x.hp2b.setHighpass(hi, kButterworthQ, sr);
    };
    setup(xL_);
    setup(xR_);
}

void MultibandCompressor::setParams(const DSPParams& p) {
    sr_ = static_cast<double>(p.sampleRate);
    crossoversDirty_ = true;
    for (int b = 0; b < kNumBands; ++b) band_[b].setParams(p);
}

void MultibandCompressor::setThreshold(float db) {
    for (int b = 0; b < kNumBands; ++b) band_[b].setThreshold(db);
}

void MultibandCompressor::setRatio(float ratio) {
    for (int b = 0; b < kNumBands; ++b) band_[b].setRatio(ratio);
}

void MultibandCompressor::setAttack(float ms) {
    for (int b = 0; b < kNumBands; ++b) band_[b].setAttack(ms);
}

void MultibandCompressor::setRelease(float ms) {
    for (int b = 0; b < kNumBands; ++b) band_[b].setRelease(ms);
}

void MultibandCompressor::setRuntimeAmount(float amount01) noexcept {
    for (int b = 0; b < kNumBands; ++b) band_[b].setRuntimeAmount(amount01);
}

void MultibandCompressor::setEnabled(bool on) noexcept {
    enabled_ = on;
    mixTarget_ = on ? 1.0 : 0.0;
}

void MultibandCompressor::setCrossovers(float lowHz, float highHz) {
    // Guardas: orden y rango valido; si no, se ignoran (fail-closed).
    if (!(lowHz > 20.0f) || !(highHz > lowHz) || !(highHz < sr_ * 0.5 - 100.0)) return;
    lowHz_ = static_cast<double>(lowHz);
    highHz_ = static_cast<double>(highHz);
    crossoversDirty_ = true;
}

void MultibandCompressor::reset() {
    auto resetX = [](Crossover& x) {
        x.lp1a.reset(); x.lp1b.reset();
        x.hp1a.reset(); x.hp1b.reset();
        x.lp2a.reset(); x.lp2b.reset();
        x.hp2a.reset(); x.hp2b.reset();
    };
    resetX(xL_);
    resetX(xR_);
    for (int b = 0; b < kNumBands; ++b) band_[b].reset();
    mix_ = mixTarget_;
    crossoversDirty_ = true;
}

void MultibandCompressor::process(double* left, double* right, int frames) {
    if (frames <= 0 || !left || !right) return;
    if (frames > kMaxFrames) frames = kMaxFrames;
    updateCrossovers();

    // ── Split LR4 (corre SIEMPRE, aunque esté en bypass) ─────────────
    // Mantiene los filtros cebados: al activar no hay transitorio de
    // arranque (los filtros ya están en régimen) y el fundido es limpio.
    for (int i = 0; i < frames; ++i) {
        const double l = left[i];
        const double r = right[i];
        // Graves: LP(200) x2 en serie.
        bandL_[0][i] = xL_.lp1b.process(xL_.lp1a.process(l));
        bandR_[0][i] = xR_.lp1b.process(xR_.lp1a.process(r));
        // Agudos: HP(4000) x2 en serie.
        bandL_[2][i] = xL_.hp2b.process(xL_.hp2a.process(l));
        bandR_[2][i] = xR_.hp2b.process(xR_.hp2a.process(r));
        // Medios: HP(200) x2 + LP(4000) x2 en serie.
        bandL_[1][i] = xL_.lp2b.process(xL_.lp2a.process(
                            xL_.hp1b.process(xL_.hp1a.process(l))));
        bandR_[1][i] = xR_.lp2b.process(xR_.lp2a.process(
                            xR_.hp1b.process(xR_.hp1a.process(r))));
    }

    // Fundido 20 ms hacia el objetivo (one-pole por bloque).
    {
        const double rc = std::exp(-static_cast<double>(frames) / (sr_ * (kMixTimeMs * 0.001)));
        mix_ = rc * mix_ + (1.0 - rc) * mixTarget_;
        // Snap exacto: el bypass bit-exacto exige mix_ == 0.0, no 1e-7.
        if (std::fabs(mix_ - mixTarget_) < 1e-6) mix_ = mixTarget_;
    }

    // Bypass: sin tocar el buffer (bit-exacto). El split ya corrió
    // (filtros cebados) y los compresores no corren (ahorro de CPU).
    if (mix_ <= 0.0) return;

    // ── Un Compressor por banda (detector link estéreo, soft knee) ───
    for (int b = 0; b < kNumBands; ++b) {
        band_[b].process(bandL_[b], bandR_[b], frames);
    }

    // ── Suma + fundido seco/procesado ────────────────────────────────
    if (mix_ >= 1.0) {
        for (int i = 0; i < frames; ++i) {
            left[i]  = bandL_[0][i] + bandL_[1][i] + bandL_[2][i];
            right[i] = bandR_[0][i] + bandR_[1][i] + bandR_[2][i];
        }
    } else {
        const double wet = mix_;
        const double dry = 1.0 - mix_;
        for (int i = 0; i < frames; ++i) {
            left[i]  = left[i] * dry +
                       (bandL_[0][i] + bandL_[1][i] + bandL_[2][i]) * wet;
            right[i] = right[i] * dry +
                       (bandR_[0][i] + bandR_[1][i] + bandR_[2][i]) * wet;
        }
    }
}

void MultibandCompressor::process(float* left, float* right, int frames) {
    if (frames <= 0 || !left || !right) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(left[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(right[i]);
    }
    process(wrapL_.data(), wrapR_.data(), frames);
    const int outFrames = frames > kMaxFrames ? kMaxFrames : frames;
    for (int i = 0; i < outFrames; ++i) {
        left[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        right[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}

}  // namespace ivanna
