// ─────────────────────────────────────────────────────────────────────────────
// free_energy_loop.hpp — Física 3: Lazo Variacional F (MOC + Anti-Adaptación).
// Unifica el lazo eferente MOC + anti-adaptación sináptica bajo el principio
// variacional F (con límites estrictos de velocidad 0.5 dB/s, cero aumento de
// ganancia maestra). Paquete FISICA_NUEVA_CONSOLIDADA (Esse, 2026-11-18).
// Header-only, RT-safe.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <array>
#include <cmath>

namespace ivanna::newphys {

class FreeEnergyAdaptationLoop {
public:
    static constexpr int kBands = 24;

    void prepare(float sampleRate, int hopSize = 128) noexcept {
        dt_ = static_cast<float>(hopSize) / std::clamp(sampleRate, 8000.0f, 384000.0f);
        // MOC Smalt et al.: ataque ~25 ms, decaimiento ~150 ms
        mocAtk_ = 1.0f - std::exp(-dt_ / 0.025f);
        mocRel_ = 1.0f - std::exp(-dt_ / 0.150f);
        // Adaptación sináptica: τ_rec ~ 350 ms
        synRec_ = 1.0f - std::exp(-dt_ / 0.350f);
        reset();
    }

    void reset() noexcept {
        mocState_.fill(0.0f);
        synAdapt_.fill(0.0f);
        gammaModDb_.fill(0.0f);
        fatigueIntegral_ = 0.0f;
    }

    // Actualiza en Reloj 2 (por hop 128). Devuelve multiplicadores de ganancia lineal por banda.
    void step(const float* bandEnv24, const float* bandTransient24,
              float freeEnergyMix, std::array<float, kBands>& outLinearMul) noexcept {
        if (freeEnergyMix <= 1e-6f || !bandEnv24) {
            outLinearMul.fill(1.0f);
            return;
        }
        const float mix = std::clamp(freeEnergyMix, 0.0f, 1.0f);
        // Tasa máxima por hop: <= 0.5 dB/s (seguro anti-oscilación de BUCLE_CERRADO_NEURAL)
        const float maxStepDb = 0.5f * dt_;

        float sumMoc = 0.0f;
        for (int b = 0; b < kBands; ++b) {
            const float env = std::clamp(bandEnv24[b], 0.0f, 4.0f);
            const float tr  = bandTransient24 ? std::clamp(bandTransient24[b], 0.0f, 4.0f) : 0.0f;

            // 1) Eferente MOC (protección cuando energía sostenida es alta)
            const float mc = (env > mocState_[b]) ? mocAtk_ : mocRel_;
            mocState_[b] += mc * (env - mocState_[b]);
            sumMoc += mocState_[b];

            // 2) Depresión sináptica (recupera saliencia de onsets atenuados tras pasaje denso)
            synAdapt_[b] += synRec_ * (env - synAdapt_[b]);

            // Gradiente variacional: -∂F/∂Γ_b = realce leve en ataque tras adaptación - atenuación por fatiga MOC
            const float onsetNeed = std::max(0.0f, tr - 1.2f) * std::min(1.0f, synAdapt_[b] * 2.0f);
            const float fatiguePen = std::max(0.0f, mocState_[b] - 0.35f);
            const float targetDb = std::clamp((1.8f * onsetNeed - 2.5f * fatiguePen) * mix, -3.0f, 2.0f);

            // Slew estrictamente acotado
            const float delta = std::clamp(targetDb - gammaModDb_[b], -maxStepDb, maxStepDb);
            gammaModDb_[b] += delta;
            outLinearMul[b] = std::pow(10.0f, gammaModDb_[b] / 20.0f);
        }

        fatigueIntegral_ = 0.995f * fatigueIntegral_ + 0.005f * (sumMoc / kBands);
    }

    float fatigueIntegral() const noexcept { return fatigueIntegral_; }

private:
    float dt_ = 128.0f / 48000.0f;
    float mocAtk_ = 0.1f, mocRel_ = 0.02f, synRec_ = 0.01f;
    float fatigueIntegral_ = 0.0f;
    std::array<float, kBands> mocState_{};
    std::array<float, kBands> synAdapt_{};
    std::array<float, kBands> gammaModDb_{};
};

} // namespace ivanna::newphys
