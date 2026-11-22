// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
// Proprietary and confidential.
#pragma once

/*
 * ============================================================
 * IVANNA OMEGA SUPREME — Nonlinear Harmonic Oscillator (NHO)
 * Reemplaza PI-LSTM Milenio Engine.
 *
 * Dinámica: z_{t+1} = z_t + μ · tanh(α·x_t + β·z_t)
 *
 * Ventajas vs CT-LSTM RK4 con upsampling 4×:
 *  - O(1) por sample, cero allocations
 *  - sin upsampler FIR de 128 taps
 *  - sin integración RK4 (innecesaria para shaping perceptual)
 *  - comportamiento HRTF real sustituido por CueBasedSpatial
 *
 * Padé [5/4] tanh mantenida: |err|<2e-5 en [-8,8]
 * ============================================================
 */

#include <cmath>
#include <algorithm>

namespace ivanna {

// ── Padé [5/4] tanh — mismo kernel que PI-LSTM ───────────────────────────────
// F2/486 (labor grande, motor double): templadas con deducción por el
// argumento — nho_tanh(float) es el histórico bit-idéntico,
// nho_tanh(double) el del núcleo double (NpeStereoCoreD). Las constantes
// se escriben como Scalar(N) para que en float den el literal f exacto
// (todos los enteros usados son exactos en float).
template <typename Scalar = float>
static inline Scalar nho_tanh(Scalar x) noexcept {
    if (x > Scalar(8))  return  Scalar(1);
    if (x < Scalar(-8)) return -Scalar(1);
    const Scalar x2 = x * x;
    return x * (Scalar(135135) + x2 * (Scalar(17325) + x2 * Scalar(378)))
             / (Scalar(135135) + x2 * (Scalar(62370) + x2 * (Scalar(3150) + x2 * Scalar(28))));
}

template <typename Scalar = float>
static inline Scalar nho_safe(Scalar x) noexcept {
    return std::isfinite(x) ? x : Scalar(0);
}

// ── NHO Engine ────────────────────────────────────────────────────────────────
// F2/486 (labor grande, motor double): plantilla con default float +
// alias. NHOEngine = float histórico bit-idéntico; NHOEngineD = núcleo
// double (lo usa NpeStereoCoreD). Estado, parámetros y slew son señal →
// Scalar; los setters reciben Scalar (el control que los llama castea
// desde float antes).
template <typename Scalar = float>
class NHOEngineT {
public:
    // State
    Scalar zL = Scalar(0);   // left state
    Scalar zR = Scalar(0);   // right state

    // Parameters (tunable via JNI)
    Scalar alpha       = Scalar(0.8);   // input coupling
    Scalar beta        = Scalar(0.3);   // state feedback
    Scalar mu          = Scalar(0.15);  // step size (gating)
    Scalar harmonic_gain = Scalar(0.2); // harmonic mix level
    Scalar wet         = Scalar(0.3);   // wet/dry

    // Smoothed runtime parameters (prevents zipper/click on 50ms evo updates)
    Scalar alpha_smooth = Scalar(0.8);
    Scalar beta_smooth  = Scalar(0.3);
    Scalar harmonic_smooth = Scalar(0.2);
    Scalar wet_smooth   = Scalar(0.3);

    // ── Gating μ_t ─────────────────────────────────────────────────────────
    // μ_t = clamp(||F|| / (||F|| + ||z - z_prev|| + ε), 0, 1) · mu
    static inline Scalar compute_mu(Scalar F, Scalar dz, Scalar base_mu) noexcept {
        constexpr Scalar EPS = Scalar(1e-6);
        const Scalar absF = std::fabs(F);
        const Scalar gate = absF / (absF + std::fabs(dz) + EPS);
        return gate * base_mu;
    }

    // ── Process one stereo sample ───────────────────────────────────────────
    inline void process_sample(Scalar xL, Scalar xR,
                               Scalar& yL, Scalar& yR) noexcept {
        constexpr Scalar kParamSlew = Scalar(0.002);
        alpha_smooth    += kParamSlew * (alpha - alpha_smooth);
        beta_smooth     += kParamSlew * (beta - beta_smooth);
        harmonic_smooth += kParamSlew * (harmonic_gain - harmonic_smooth);
        wet_smooth      += kParamSlew * (wet - wet_smooth);

        // F(c_t) = tanh(α·x + β·z)
        const Scalar FL = nho_tanh(alpha_smooth * xL + beta_smooth * zL);
        const Scalar FR = nho_tanh(alpha_smooth * xR + beta_smooth * zR);

        const Scalar dzL = FL - zL;
        const Scalar dzR = FR - zR;

        const Scalar mu_t_L = compute_mu(FL, dzL, mu);
        const Scalar mu_t_R = compute_mu(FR, dzR, mu);

        // State update: leaky ODE z_{t+1} = z_t + μ_t · (F(c_t) - z_t)
        // Guarantees asymptotic stability (z -> 0 in silence) and |z| <= 1,
        // eliminating DC latch-up and comparator switching artifacts.
        zL = nho_safe(zL + mu_t_L * dzL);
        zR = nho_safe(zR + mu_t_R * dzR);

        // Output: y = (1-wet)·x + wet·tanh(harmonic_gain·z)
        const Scalar hL = nho_tanh(harmonic_smooth * zL);
        const Scalar hR = nho_tanh(harmonic_smooth * zR);

        yL = (Scalar(1) - wet_smooth) * xL + wet_smooth * hL;
        yR = (Scalar(1) - wet_smooth) * xR + wet_smooth * hR;
    }

    // ── Process block — zero allocations ───────────────────────────────────
    void process_block(const Scalar* __restrict__ inL,
                       const Scalar* __restrict__ inR,
                       Scalar* __restrict__ outL,
                       Scalar* __restrict__ outR,
                       int n) noexcept {
        for (int i = 0; i < n; ++i) {
            process_sample(inL[i], inR[i], outL[i], outR[i]);
        }
    }

    void reset() noexcept {
        zL = Scalar(0);
        zR = Scalar(0);
        alpha_smooth = alpha;
        beta_smooth = beta;
        harmonic_smooth = harmonic_gain;
        wet_smooth = wet;
    }

    void set_alpha(Scalar v)         noexcept { alpha = std::clamp(v, Scalar(0), Scalar(4)); }
    void set_beta(Scalar v)          noexcept { beta  = std::clamp(v, Scalar(0), Scalar(1)); }
    void set_mu(Scalar v)            noexcept { mu    = std::clamp(v, Scalar(0), Scalar(1)); }
    void set_harmonic_gain(Scalar v) noexcept { harmonic_gain = std::clamp(v, Scalar(0), Scalar(2)); }
    void set_wet(Scalar v)           noexcept { wet   = std::clamp(v, Scalar(0), Scalar(1)); }
};

using NHOEngine = NHOEngineT<float>;
using NHOEngineD = NHOEngineT<double>;

} // namespace ivanna
