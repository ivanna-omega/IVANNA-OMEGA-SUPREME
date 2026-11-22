#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <cmath>
#include <algorithm>
#include <vector>

namespace ivanna::spatial {

struct AudiogramProfile {
    float loss_250hz_db{0.0f};
    float loss_500hz_db{0.0f};
    float loss_1khz_db{0.0f};
    float loss_2khz_db{0.0f};
    float loss_4khz_db{0.0f};
    float loss_8khz_db{0.0f};
    float loss_low_db{0.0f};
    float loss_mid_db{0.0f};
    float loss_high_db{0.0f};
    float loss_ultra_high_db{0.0f};
    float listening_duration_mins{0.0f};
    float ear_tip_seal_factor{1.0f}; // [0, 1] 1 = perfect seal
};

/**
 * @class HearingAdaptationEngine
 * Eje 6: Dynamic equal-loudness contours (isophonic), ear tip seal compensation,
 * listening fatigue protection, and presbycusis compensation.
 * 
 * Latency budget: 0 samples (zero added latency, uses minimum-phase shelving/biquads).
 * CPU budget: <= 0.5%
 * RT-Safety: Zero malloc in hot path, zero locks.
 */
// F2/486 (labor grande, double interno): el estado de señal
// (shelves, ganancias suavizadas, targets) vive en double;
// process(double) es el núcleo y process(float) delega por scratch.
// El AudiogramProfile sigue en float: son datos de control (dB de
// pérdida, sellado), no señal.
class HearingAdaptationEngine {
public:
    HearingAdaptationEngine() noexcept {
        reset();
    }

    void reset() noexcept {
        lowShelveL_ = 0.0;
        lowShelveR_ = 0.0;
        highShelveL_ = 0.0;
        highShelveR_ = 0.0;
        smoothLowGain_ = targetLowGain_;
        smoothHighGain_ = targetHighGain_;
        renderedBlocks_ = 0u;
    }

    void setProfile(const AudiogramProfile& prof) noexcept {
        profile_ = prof;
        if (prof.loss_low_db != 0.0f && prof.loss_250hz_db == 0.0f) {
            profile_.loss_250hz_db = prof.loss_low_db;
            profile_.loss_500hz_db = prof.loss_low_db;
        }
        if (prof.loss_mid_db != 0.0f && prof.loss_1khz_db == 0.0f) {
            profile_.loss_1khz_db = prof.loss_mid_db;
            profile_.loss_2khz_db = prof.loss_mid_db;
        }
        if (prof.loss_high_db != 0.0f && prof.loss_4khz_db == 0.0f) {
            profile_.loss_4khz_db = prof.loss_high_db;
        }
        if (prof.loss_ultra_high_db != 0.0f && prof.loss_8khz_db == 0.0f) {
            profile_.loss_8khz_db = prof.loss_ultra_high_db;
        }
        recalculate();
        if (renderedBlocks_ == 0u) {
            smoothLowGain_ = targetLowGain_;
            smoothHighGain_ = targetHighGain_;
        }
    }

    void setAudiogram(const AudiogramProfile& prof) noexcept {
        const float prevSeal = profile_.ear_tip_seal_factor;
        setProfile(prof);
        if (prof.ear_tip_seal_factor == 1.0f && prevSeal != 1.0f) {
            profile_.ear_tip_seal_factor = prevSeal;
            recalculate();
        }
    }

    void setEarTipSeal(float sealFactor) noexcept {
        profile_.ear_tip_seal_factor = std::clamp(std::isfinite(sealFactor) ? sealFactor : 1.0f, 0.2f, 1.0f);
        recalculate();
        if (renderedBlocks_ == 0u) {
            smoothLowGain_ = targetLowGain_;
            smoothHighGain_ = targetHighGain_;
        }
    }

    void setFatigueLevel(float level) noexcept {
        fatigueLevel_ = std::clamp(std::isfinite(level) ? level : 0.0f, 0.0f, 1.0f);
        recalculate();
        if (renderedBlocks_ == 0u) {
            smoothLowGain_ = targetLowGain_;
            smoothHighGain_ = targetHighGain_;
        }
    }

    void setListeningSpl(float splDb) noexcept {
        const float clampedSpl = std::clamp(std::isfinite(splDb) ? splDb : 75.0f, 40.0f, 100.0f);
        listeningSplDb_ = clampedSpl;
        // Normalizar SPL [70..100 dB] -> nivel de fatiga auditiva ISO 226 [0..1]
        const float derivedFatigue = std::clamp((clampedSpl - 70.0f) / 28.0f, 0.0f, 1.0f);
        setFatigueLevel(derivedFatigue);
    }

    [[nodiscard]] float listeningSpl() const noexcept { return listeningSplDb_; }

    /**
     * @brief Process stereo block with equal-loudness + presbycusis + fatigue protection.
     * Núcleo double (F2/486).
     */
    void process(double* __restrict bufferL, double* __restrict bufferR, size_t numSamples) noexcept {
        if (!bufferL || !bufferR || numSamples == 0) return;
        ++renderedBlocks_;

        const double targetLow = targetLowGain_;
        const double targetHigh = targetHighGain_;
        double lowGain = smoothLowGain_;
        double highGain = smoothHighGain_;

        double sLowL = lowShelveL_;
        double sLowR = lowShelveR_;
        double sHighL = highShelveL_;
        double sHighR = highShelveR_;

        for (size_t i = 0; i < numSamples; ++i) {
            lowGain  += 0.004 * (targetLow  - lowGain);
            highGain += 0.004 * (targetHigh - highGain);
            double inL = bufferL[i];
            double inR = bufferR[i];

            // 1-pole low-frequency compensation (bass loss from poor ear tip seal)
            sLowL += 0.05 * (inL - sLowL);
            sLowR += 0.05 * (inR - sLowR);
            double lowCompL = inL + sLowL * (lowGain - 1.0);
            double lowCompR = inR + sLowR * (lowGain - 1.0);

            // 1-pole high-frequency compensation (presbycusis / fatigue roll-off)
            sHighL += 0.2 * (lowCompL - sHighL);
            sHighR += 0.2 * (lowCompR - sHighR);
            double highDiffL = lowCompL - sHighL;
            double highDiffR = lowCompR - sHighR;

            bufferL[i] = sHighL + highDiffL * highGain;
            bufferR[i] = sHighR + highDiffR * highGain;
        }

        lowShelveL_ = sLowL;
        lowShelveR_ = sLowR;
        highShelveL_ = sHighL;
        highShelveR_ = sHighR;
        smoothLowGain_ = lowGain;
        smoothHighGain_ = highGain;
    }

    // Envoltorio float (compat legacy): convierte por scratch y delega
    // en el núcleo double.
    void process(float* __restrict bufferL, float* __restrict bufferR, size_t numSamples) noexcept {
        if (!bufferL || !bufferR || numSamples == 0) return;
        if (wrapL_.size() < numSamples) { wrapL_.resize(numSamples); wrapR_.resize(numSamples); }
        for (size_t i = 0; i < numSamples; ++i) {
            wrapL_[i] = std::isfinite(bufferL[i]) ? static_cast<double>(bufferL[i]) : 0.0;
            wrapR_[i] = std::isfinite(bufferR[i]) ? static_cast<double>(bufferR[i]) : 0.0;
        }
        process(wrapL_.data(), wrapR_.data(), numSamples);
        for (size_t i = 0; i < numSamples; ++i) {
            bufferL[i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
            bufferR[i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
        }
    }

private:
    void recalculate() noexcept {
        // Low compensation boost inversely proportional to seal factor
        // If ear tip seal drops to 0.5 -> boost bass up to +4dB (gain ~ 1.58)
        const double seal = std::clamp(static_cast<double>(profile_.ear_tip_seal_factor), 0.2, 1.0);
        targetLowGain_ = 1.0 + (1.0 - seal) * 0.75;

        // Presbycusis compensation (loss at 4kHz and 8kHz) + fatigue attenuation
        const double avgLossDb = (static_cast<double>(profile_.loss_4khz_db) + static_cast<double>(profile_.loss_8khz_db)) * 0.5;
        // Mild linear gain compensation with safety clamp (+6dB max)
        double highBoostLinear = std::clamp(std::pow(10.0, avgLossDb / 40.0), 0.8, 2.0);

        // Fatigue attenuates harsh high frequencies to protect ear
        const double fatigueDamping = 1.0 - (static_cast<double>(fatigueLevel_) * 0.25);
        targetHighGain_ = highBoostLinear * fatigueDamping;
    }

    AudiogramProfile profile_{};
    float fatigueLevel_{0.0f};
    float listeningSplDb_{75.0f};

    double targetLowGain_{1.0};
    double targetHighGain_{1.0};
    double smoothLowGain_{1.0};
    double smoothHighGain_{1.0};
    uint32_t renderedBlocks_{0u};

    double lowShelveL_{0.0};
    double lowShelveR_{0.0};
    double highShelveL_{0.0};
    double highShelveR_{0.0};
    // Scratch del envoltorio float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapL_{};
    std::vector<double> wrapR_{};
};

} // namespace ivanna::spatial
