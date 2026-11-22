#if defined(__clang__)
#pragma clang optimize on
#else
#pragma GCC optimize("O3", "unroll-loops")
#endif
#include "../include/HarmonicExciter.h"
#include <cmath>
#include <cstring>
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace ivanna {

void HarmonicExciter::reset() {
    lastL_ = 0.0;
    lastR_ = 0.0;
    std::memset(osLeft_, 0, sizeof(osLeft_));
    std::memset(osRight_, 0, sizeof(osRight_));
    hpfL_.reset();
    hpfR_.reset();
    osLpfL_.reset();
    osLpfR_.reset();
    preLpfL_.reset();
    preLpfR_.reset();
    chebL_.reset();
    chebR_.reset();
    excScaleL_ = 1.0;
    excScaleR_ = 1.0;
    fundCrossL_ = fundPowL_ = fundGainL_ = 0.0;
    fundCrossR_ = fundPowR_ = fundGainR_ = 0.0;
    wetNow_ = wet_;
    driveNow_ = drive_;
    lastSampleRate_ = 0;
}

void HarmonicExciter::setParams(const DSPParams& p) {
    drive_ = 1.0 + static_cast<double>(p.drive) * 3.0;
    wet_ = static_cast<double>(p.wet);
    dry_ = 1.0 - static_cast<double>(p.wet);

    const int srInt = p.sampleRate > 0 ? static_cast<int>(p.sampleRate) : 48000;
    if (srInt == lastSampleRate_) {
        return;
    }
    lastSampleRate_ = srInt;

    {
        const double srOS = static_cast<double>(srInt) * static_cast<double>(OS_FACTOR);
        wetSmooth_ = std::exp(-1.0 / (srOS * 0.015));
        driveSmooth_ = wetSmooth_;
        chebL_.prepare(srOS);
        chebR_.prepare(srOS);
    }

    double sampleRateOS = static_cast<double>(srInt) * static_cast<double>(OS_FACTOR);

    double fc = 12000.0;
    if (fc > sampleRateOS * 0.45) fc = sampleRateOS * 0.45;
    double omegaOS = 2.0 * M_PI * fc / sampleRateOS;
    double swOS = std::sin(omegaOS);
    double cwOS = std::cos(omegaOS);
    double alphaOS = swOS / (2.0 * 0.707);
    double a0OS_inv = 1.0 / (1.0 + alphaOS);

    osLpfL_.b0 = (1.0 - cwOS) * 0.5 * a0OS_inv;
    osLpfL_.b1 = (1.0 - cwOS) * a0OS_inv;
    osLpfL_.b2 = osLpfL_.b0;
    osLpfL_.a1 = -2.0 * cwOS * a0OS_inv;
    osLpfL_.a2 = (1.0 - alphaOS) * a0OS_inv;
    osLpfR_.b0 = osLpfL_.b0;
    osLpfR_.b1 = osLpfL_.b1;
    osLpfR_.b2 = osLpfL_.b2;
    osLpfR_.a1 = osLpfL_.a1;
    osLpfR_.a2 = osLpfL_.a2;

    {
        double sr = static_cast<double>(srInt);
        double fc_pre = 8000.0;
        if (fc_pre > sr * 0.45) fc_pre = sr * 0.45;
        double K = std::tan(M_PI * fc_pre / sr);
        double KK = K * K;
        double Q = 0.707106781;
        double norm_pre = 1.0 + K / Q + KK;
        preLpfL_.b0 = KK / norm_pre;
        preLpfL_.b1 = 2.0 * KK / norm_pre;
        preLpfL_.b2 = preLpfL_.b0;
        preLpfL_.a1 = 2.0 * (KK - 1.0) / norm_pre;
        preLpfL_.a2 = (1.0 - K / Q + KK) / norm_pre;
        preLpfR_.b0 = preLpfL_.b0;
        preLpfR_.b1 = preLpfL_.b1;
        preLpfR_.b2 = preLpfL_.b2;
        preLpfR_.a1 = preLpfL_.a1;
        preLpfR_.a2 = preLpfL_.a2;
    }

    double hpfFc = 3000.0;
    if (hpfFc > sampleRateOS * 0.45) hpfFc = sampleRateOS * 0.45;
    hpfL_.setHighpass(hpfFc, 0.707, sampleRateOS);
    hpfR_.setHighpass(hpfFc, 0.707, sampleRateOS);

    excRelCoef_ = std::exp(-1.0 / (static_cast<double>(srInt) * OS_FACTOR * 0.020));
    // Promedio de la regresión de fundamental: 3 ms (>= 9 periodos del contenido
    // que pasa el HPF de 3 kHz → rizado de 2f atenuado > 35 dB).
    fundCoef_ = std::exp(-1.0 / (static_cast<double>(srInt) * OS_FACTOR * 0.003));
}

static inline __attribute__((always_inline)) double softClip(double x, double drive) {
    x *= drive;
    double absX = x < 0.0 ? -x : x;
    if (absX > 3.0) {
        x = x > 0.0 ? (3.0 + 0.5 * std::tanh((x - 3.0) * 0.5))
                    : (-3.0 - 0.5 * std::tanh((-x - 3.0) * 0.5));
    }
    double x2 = x * x;
    return x * (1.0 + x2 * 0.037037) / (1.0 + x2 * 0.333333);
}

static constexpr double kExcCeiling = 0.98855;

__attribute__((hot, flatten))
void HarmonicExciter::process(double* __restrict__ left, double* __restrict__ right, int frames) {
    if (frames <= 0 || frames > MAX_OS_FRAMES) return;

    const double wetTarget = wet_ * runtimeReductionMul_;
    double wetNow = wetNow_;
    const double wetSm = wetSmooth_ > 0.0 ? wetSmooth_ : 0.9995;

    if (wetTarget <= 0.00001 && wetNow <= 0.00001) {
        wetNow_ = 0.0;
        return;
    }

    driveNow_ = driveSmooth_ * driveNow_ + (1.0 - driveSmooth_) * drive_;
    const double drive = driveNow_;
    const double dNorm = std::clamp((drive - 1.0) / 3.0, 0.0, 1.0);

    double scaleL = excScaleL_;
    double scaleR = excScaleR_;
    const double rel = excRelCoef_;

    double preFiltL[MAX_OS_FRAMES];
    double preFiltR[MAX_OS_FRAMES];
    for (int i = 0; i < frames; ++i) {
        preFiltL[i] = preLpfL_.process(left[i]);
        preFiltR[i] = preLpfR_.process(right[i]);
    }

    int osIdx = 0;
    for (int i = 0; i < frames; ++i) {
        osLeft_[osIdx]  = preFiltL[i];
        osRight_[osIdx] = preFiltR[i];
        osIdx++;

        double nextLF = (i + 1 < frames) ? preFiltL[i + 1] : preFiltL[i];
        double nextRF = (i + 1 < frames) ? preFiltR[i + 1] : preFiltR[i];
        osLeft_[osIdx]  = 0.5 * (preFiltL[i] + nextLF);
        osRight_[osIdx] = 0.5 * (preFiltR[i] + nextRF);
        osIdx++;
    }
    int osFrames = osIdx;
    lastL_ = left[frames - 1];
    lastR_ = right[frames - 1];

    double dryCacheL = 0.0, dryCacheR = 0.0;
    for (int i = 0; i < osFrames; ++i) {
        double l = osLeft_[i];
        double r = osRight_[i];

        double hL = hpfL_.process(l);
        double hR = hpfR_.process(r);

        double excL = 0.0, excR = 0.0;
        if (shaperMode_ == 1) {
            const double chebOutL = chebL_.tick(hL * drive, dNorm, warmth_, flatness1m_);
            const double chebOutR = chebR_.tick(hR * drive, dNorm, warmth_, flatness1m_);
            const double padeL = softClip(hL, drive) - hL;
            const double padeR = softClip(hR, drive) - hR;
            excL = 0.65 * chebOutL + 0.35 * padeL;
            excR = 0.65 * chebOutR + 0.35 * padeR;
        } else {
            excL = softClip(hL, drive) - hL;
            excR = softClip(hR, drive) - hR;
        }

        // SOLO ARMÓNICOS: el shaper (softClip−x o Chebyshev) trae una componente lineal
        // en la fundamental (medido: +7.5 dB a 3.5 kHz con drive .6 / wet .8, nivel -20 dBFS)
        // que se sumaba al seco como realce de 3-8 kHz dependiente del nivel. Se resta la
        // parte en fase con la entrada del shaper; queda H2/H3/... sin ganancia lineal.
        {
            fundCrossL_ = fundCoef_ * fundCrossL_ + (1.0 - fundCoef_) * (excL * hL);
            fundPowL_   = fundCoef_ * fundPowL_   + (1.0 - fundCoef_) * (hL * hL);
            fundCrossR_ = fundCoef_ * fundCrossR_ + (1.0 - fundCoef_) * (excR * hR);
            fundPowR_   = fundCoef_ * fundPowR_   + (1.0 - fundCoef_) * (hR * hR);
            if (fundPowL_ > 1.0e-8) fundGainL_ = std::clamp(fundCrossL_ / fundPowL_, -8.0, 8.0);
            if (fundPowR_ > 1.0e-8) fundGainR_ = std::clamp(fundCrossR_ / fundPowR_, -8.0, 8.0);
            excL -= fundGainL_ * hL;
            excR -= fundGainR_ * hR;
        }

        excL = osLpfL_.process(excL);
        excR = osLpfR_.process(excR);

        // Referencia SECA original: a índice impar left[i>>1] ya contiene la salida del
        // índice par (procesada). Se usa el seco interpolado (par: muestra; impar: punto
        // medio con la siguiente, aún sin tocar).
        double dryL, dryR;
        if ((i & 1) == 0) {
            dryL = left[i >> 1];
            dryR = right[i >> 1];
            dryCacheL = dryL;
            dryCacheR = dryR;
        } else {
            const int k1 = (i >> 1) + 1;
            const double nxL = (k1 < frames) ? left[k1]  : dryCacheL;
            const double nxR = (k1 < frames) ? right[k1] : dryCacheR;
            dryL = 0.5 * (dryCacheL + nxL);
            dryR = 0.5 * (dryCacheR + nxR);
        }
        const double peakRefL = std::max(std::fabs(l), std::fabs(dryL));
        const double peakRefR = std::max(std::fabs(r), std::fabs(dryR));
        const double headL = std::max(0.0, 1.0 - peakRefL);
        const double headR = std::max(0.0, 1.0 - peakRefR);
        const double reqL = kExcCeiling * wetNow * std::fabs(excL);
        const double reqR = kExcCeiling * wetNow * std::fabs(excR);
        const double needL = (reqL > headL) ? (headL / (reqL > 1e-9 ? reqL : 1e-9)) : 1.0;
        const double needR = (reqR > headR) ? (headR / (reqR > 1e-9 ? reqR : 1e-9)) : 1.0;
        if (needL < scaleL) scaleL = needL; else scaleL = rel * scaleL + (1.0 - rel);
        if (needR < scaleR) scaleR = needR; else scaleR = rel * scaleR + (1.0 - rel);
        scaleL = std::clamp(scaleL, 0.0, 1.0);
        scaleR = std::clamp(scaleR, 0.0, 1.0);

        double outL = dryL + kExcCeiling * wetNow * excL * scaleL;
        double outR = dryR + kExcCeiling * wetNow * excR * scaleR;

        wetNow = wetSm * wetNow + (1.0 - wetSm) * wetTarget;
        wetNow_ = wetNow;

        const double ceilL = std::max(1.0, std::fabs(dryL));
        const double ceilR = std::max(1.0, std::fabs(dryR));
        if (outL > ceilL) outL = ceilL; else if (outL < -ceilL) outL = -ceilL;
        if (outR > ceilR) outR = ceilR; else if (outR < -ceilR) outR = -ceilR;

        if (!std::isfinite(outL)) outL = 0.0;
        if (!std::isfinite(outR)) outR = 0.0;

        if ((i & 1) == 0) {
            left[i >> 1]  = outL;
            right[i >> 1] = outR;
        }
    }

    excScaleL_ = scaleL;
    excScaleR_ = scaleR;
}

void HarmonicExciter::process(float* left, float* right, int frames) {
    if (frames <= 0 || frames > MAX_OS_FRAMES) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(left[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(right[i]);
    }
    process(wrapL_.data(), wrapR_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        right[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}

} // namespace ivanna
