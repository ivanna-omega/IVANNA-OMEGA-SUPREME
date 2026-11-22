// ─────────────────────────────────────────────────────────────────────────────
// laguerre_volterra_h2.hpp — Física 4: Volterra Laguerre H2 (P=6, α=0.92).
// Resuelve la sintonía a 68.5 Hz (14.6 ms) del Genezi en tiempo real con solo
// 6 filtros IIR de Laguerre (α = 0.92 → ~25 ms) y una matriz simétrica de 6×6
// (21 coeficientes activos, ~35 MAC/muestra). Paquete
// FISICA_NUEVA_CONSOLIDADA (Esse, 2026-11-18). Header-only, RT-safe.
// NOTA DE CONSISTENCIA: la recurrencia del banco (u[m] = α·(u[m]−u[m−1]) +
// u[m−1] previo) es LA MISMA que usa tools/genezi_calibrate.py al ajustar
// los 21 coeficientes — el fit y el runtime comparten la base exacta; no
// cambiar una sin la otra.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace ivanna::newphys {

// Kernel Volterra H2 ortogonalizado en base de Laguerre de orden P=6.
// A 48 kHz con polo alpha=0.92, el horizonte de memoria es > 25 ms (cubre 1.7 ciclos
// completos del bass-reflex 68.5 Hz del Sony Genezi MHC-PZ1D con ~35 MAC/muestra).
class LaguerreVolterraH2 {
public:
    static constexpr int kOrder = 6;
    static constexpr int kTriCount = (kOrder * (kOrder + 1)) / 2; // 21

    void prepare(float sampleRate) noexcept {
        sampleRate_ = (sampleRate >= 8000.0f && sampleRate <= 384000.0f) ? sampleRate : 48000.0f;
        // Escalar polo para mantener constante temporal física (~12.5 ms por polo) invariante a SR
        const double tau = 0.0125;
        alpha_ = static_cast<float>(std::exp(-1.0 / (tau * sampleRate_)));
        sqrtOneMinusA2_ = std::sqrt(std::max(1e-6f, 1.0f - alpha_ * alpha_));

        // Pasa-bajos complementario a 1.5 kHz para aislar la banda donde vive H2 (60-450 Hz)
        const double w0 = 2.0 * M_PI * 1500.0 / sampleRate_;
        lpCoef_ = static_cast<float>(1.0 - std::exp(-w0));
        reset();
    }

    void reset() noexcept {
        for (int ch = 0; ch < 2; ++ch) {
            u_[ch].fill(0.0f);
            lpState_[ch] = 0.0f;
        }
    }

    // Carga los 21 coeficientes triangulares medidos del Genezi (o identidad si nulos)
    bool loadCoefficients(const float* tri21L, const float* tri21R, float maxAllowedAbs = 1.5f) noexcept {
        if (!tri21L || !tri21R) return false;
        float normL = 0.0f, normR = 0.0f;
        for (int i = 0; i < kTriCount; ++i) {
            if (!std::isfinite(tri21L[i]) || !std::isfinite(tri21R[i])) return false;
            if (std::abs(tri21L[i]) > maxAllowedAbs || std::abs(tri21R[i]) > maxAllowedAbs) return false;
            normL += std::abs(tri21L[i]);
            normR += std::abs(tri21R[i]);
        }
        // Limite análogo a puerta IVK1: A^2 * sum|h2| <= 0.35
        if (normL > 0.35f || normR > 0.35f) return false;

        std::memcpy(k2_[0].data(), tri21L, kTriCount * sizeof(float));
        std::memcpy(k2_[1].data(), tri21R, kTriCount * sizeof(float));
        hasKernel_ = (normL > 1e-7f || normR > 1e-7f);
        return true;
    }

    void clearKernel() noexcept {
        k2_[0].fill(0.0f);
        k2_[1].fill(0.0f);
        hasKernel_ = false;
    }

    bool hasKernel() const noexcept { return hasKernel_; }

    // Procesa in-place un bloque estéreo. Con mix<=0 o sin kernel es no-op bit-exacto.
    float processBlock(float* L, float* R, int n, float mix, float splDb) noexcept {
        if (!hasKernel_ || mix <= 1e-6f || n <= 0) return 0.0f;
        mix = std::clamp(mix, 0.0f, 1.0f);

        // Escalamiento físico con nivel (M1 de Σ): la excursión ξ(L) crece sobre ~78 dB SPL
        const float levelScale = std::clamp((splDb - 72.0f) / 28.0f, 0.25f, 1.35f);
        const float effMix = mix * levelScale;

        float peakCorr = 0.0f;
        float* chans[2] = {L, R};

        for (int c = 0; c < 2; ++c) {
            float* xbuf = chans[c];
            auto& u = u_[c];
            const auto& k = k2_[c];
            float lp = lpState_[c];

            for (int nIdx = 0; nIdx < n; ++nIdx) {
                const float x = xbuf[nIdx];
                // Aislar banda de excursión del cono (< 1.5 kHz)
                lp += lpCoef_ * (x - lp);

                // Banco recursivo de Laguerre (6 modos ortogonales)
                float prevU = u[0];
                u[0] = sqrtOneMinusA2_ * lp + alpha_ * u[0];
                for (int m = 1; m < kOrder; ++m) {
                    const float curPrev = u[m];
                    u[m] = alpha_ * (u[m] - u[m - 1]) + prevU;
                    prevU = curPrev;
                }

                // Forma cuadrática simétrica: y2 = Σ_{i<=j} mult * k[idx] * u[i] * u[j]
                float y2 = 0.0f;
                int idx = 0;
                for (int i = 0; i < kOrder; ++i) {
                    const float ui = u[i];
                    y2 += k[idx++] * ui * ui;
                    for (int j = i + 1; j < kOrder; ++j) {
                        y2 += 2.0f * k[idx++] * ui * u[j];
                    }
                }

                // Tope suave para garantizar Ω0 (nunca clipar ni degradar)
                const float corr = std::clamp(y2 * effMix, -0.25f, 0.25f);
                peakCorr = std::max(peakCorr, std::abs(corr));
                const float out = x + corr;
                xbuf[nIdx] = softCeil(out);
            }
            lpState_[c] = lp;
        }
        return peakCorr;
    }

private:
    static float softCeil(float x) noexcept {
        constexpr float kCeiling = 0.98855f;
        constexpr float kKnee = 0.97f * kCeiling;
        const float ax = std::abs(x);
        if (ax <= kKnee) return x;
        const float over = (ax - kKnee) / (kCeiling - kKnee);
        const float y = kKnee + (kCeiling - kKnee) * (over / (1.0f + over));
        return (x >= 0.0f) ? y : -y;
    }

    float sampleRate_ = 48000.0f;
    float alpha_ = 0.92f;
    float sqrtOneMinusA2_ = 0.3919f;
    float lpCoef_ = 0.18f;
    bool hasKernel_ = false;

    std::array<std::array<float, kOrder>, 2> u_{};
    std::array<float, 2> lpState_{};
    std::array<std::array<float, kTriCount>, 2> k2_{};
};

} // namespace ivanna::newphys
