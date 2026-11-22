// NpeStereoCore.hpp
// ============================================================================
// IVANNA — Motor NPE neuromórfico para la RUTA B (módulo Magisk / audioserver)
// ============================================================================
// El NPE (NHO + pool LIF + banco de envolventes coclear + inhibición lateral + compresión OHC + AGC) vivía SOLO en
// la app (jni/ivanna_npe_jni.cpp, Ruta A). Con el módulo instalado y la app procesando por omega_effect, el NPE no
// existía en la cadena: sus controles no tenían efecto. Esta clase es la misma cadena por muestra, sin lo que no es
// seguro en audioserver (mutex de métricas, std::clock, AutonomousBrain, scope, manifold): RT-safe, sin
// asignaciones. Reutiliza NHOEngine, LIFPool32 y BiquadEnvelopeBank tal cual.
//
// Flujo por muestra (idéntico al de Ruta A):
//   x → envolventes (cues L,T,S,R) → LIF(spikes) ; NHO(x) → inhibición lateral (spikes/32, cue S)
//     → compresión OHC (cue L) → ensanche M/S por cue S → AGC lento (10 ms/300 ms, paso acotado)
//     → master gain → rodilla suave 0.8→1.0
//   Mezcla con el original por `fade` (20 ms): encender/apagar sin clic; apagado y fade 0 = BIT-EXACTO.
// ============================================================================
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "nho_engine.hpp"
#include "lif_neuron_pool.hpp"
#include "biquad_envelope_bank.hpp"

namespace ivanna::neuromorphic {

// F2/486 (labor grande, motor double): plantilla con default float +
// alias. NpeStereoCore = float histórico bit-idéntico;
// NpeStereoCoreD = núcleo double (lo usa FusedBrain). La SEÑAL (NHO,
// OHC, M/S, AGC, rodilla, fade) es Scalar. El plano NEURAL/CONTROL
// queda float por diseño: BiquadEnvelopeBank extrae los cues en float
// (sus salidas son valores de control) y LIFPool32 simula las neuronas
// en float (su salida es un conteo de espigas, no audio).
template <typename Scalar = float>
class NpeStereoCoreT {
public:
    void prepare(float sampleRate) noexcept {
        sr_ = (std::isfinite(sampleRate) && sampleRate > 8000.0f) ? sampleRate : 48000.0f;
        envBank_.init(static_cast<uint32_t>(sr_));
        ivannanpe::LIFParams lp;
        lp.sample_rate = static_cast<float>(sr_);
        lifPool_.reset(lp);
        aAtt_  = Scalar(1) - std::exp(Scalar(-1) / (Scalar(0.010) * sr_));
        aRel_  = Scalar(1) - std::exp(Scalar(-1) / (Scalar(0.300) * sr_));
        fadeStep_ = Scalar(1) / (Scalar(0.020) * sr_);
        reset();
        nho_.set_harmonic_gain(harmonic_);
    }

    void reset() noexcept {
        nho_.reset();
        envBank_.reset();
        agcGain_ = Scalar(1); rms_ = Scalar(0); fade_ = Scalar(0); desired_ = Scalar(1); agcTick_ = 0;
    }

    /// Hilo de audio, al inicio del bloque. Rangos = los de Ruta A.
    void setParams(bool enabled, float harmonicGain, float lateralInhib, float ohcCompression, float masterGainDb) noexcept {
        enabled_ = enabled;
        if (std::isfinite(harmonicGain)) { harmonic_ = std::clamp(static_cast<Scalar>(harmonicGain), Scalar(0), Scalar(2)); nho_.set_harmonic_gain(harmonic_); }
        if (std::isfinite(lateralInhib))  lateral_ = std::clamp(static_cast<Scalar>(lateralInhib), Scalar(0), Scalar(1));
        if (std::isfinite(ohcCompression)) ohc_ = std::clamp(static_cast<Scalar>(ohcCompression), Scalar(0), Scalar(1));
        if (std::isfinite(masterGainDb))   masterLin_ = std::pow(Scalar(10), std::clamp(static_cast<Scalar>(masterGainDb), Scalar(-24), Scalar(12)) * Scalar(0.05));
    }

    bool   isActive() const noexcept { return enabled_ || fade_ > Scalar(0); }
    Scalar fade()     const noexcept { return fade_; }
    Scalar agcGain()  const noexcept { return agcGain_; }
    /// Espigas LIF del último bloque (telemetría / pruebas): prueba de que la red neuronal realmente corre.
    int   lastSpikes() const noexcept { return lastSpikes_; }

    void process(Scalar* L, Scalar* R, int n) noexcept {
        if (L == nullptr || R == nullptr || n <= 0) return;
        if (!enabled_ && fade_ <= Scalar(0)) { return; }                 // bypass bit-exacto
        int spikesBlock = 0;
        for (int i = 0; i < n; ++i) {
            fade_ = enabled_ ? std::min(Scalar(1), fade_ + fadeStep_) : std::max(Scalar(0), fade_ - fadeStep_);
            const Scalar xL = L[i], xR = R[i];
            // Cues en float (plano de control): el banco de envolventes
            // recibe la muestra casteada y devuelve cues float.
            const ivanna::PerceptualCues cues = envBank_.extract(static_cast<float>(xL), static_cast<float>(xR));

            std::array<float, 32> currents{};
            for (int b = 0; b < ivanna::BEB_BANDS; ++b) {
                const float iL = envBank_.envL[b] * 0.05f;
                const float iR = envBank_.envR[b] * 0.05f;
                currents[b * 4 + 0] = iL;
                currents[b * 4 + 1] = iR;
                currents[b * 4 + 2] = static_cast<float>(iL * harmonic_ * Scalar(0.5));
                currents[b * 4 + 3] = static_cast<float>(iR * harmonic_ * Scalar(0.5));
            }
            const int spikes = lifPool_.tick(currents);
            spikesBlock += spikes;

            Scalar yL, yR;
            nho_.process_sample(xL, xR, yL, yR);

            const Scalar inhib = std::clamp(Scalar(1) - lateral_ * (static_cast<Scalar>(spikes) / Scalar(32)), Scalar(0.2), Scalar(1));
            yL *= inhib; yR *= inhib;

            if (ohc_ > Scalar(0)) {
                const Scalar g = Scalar(1) / (Scalar(1) + ohc_ * static_cast<Scalar>(cues.L) * Scalar(4));
                yL *= g; yR *= g;
            }
            {   // ensanche M/S dirigido por el cue espacial
                const Scalar mid = Scalar(0.5) * (yL + yR), side = Scalar(0.5) * (yL - yR);
                const Scalar w = Scalar(1) + Scalar(0.3) * static_cast<Scalar>(cues.S);
                yL = mid + side * w; yR = mid - side * w;
            }
            // AGC: envolvente del programa (no de la onda) y paso por muestra acotado
            const Scalar inst = Scalar(0.5) * (std::fabs(yL) + std::fabs(yR));
            rms_ += (inst > rms_ ? aAtt_ : aRel_) * (inst - rms_);
            // log10/pow solo cada 16 muestras (la envolvente tiene tau >= 10 ms): ~16x menos CPU en audioserver.
            if ((agcTick_++ & 15u) == 0u) {
                const Scalar rmsDb = Scalar(20) * std::log10(std::max(rms_, Scalar(1.0e-6)));
                desired_ = std::pow(Scalar(10), (kAgcTargetDb - rmsDb) * Scalar(0.05));
            }
            agcGain_ = std::clamp(agcGain_ + std::clamp(kAgcRate * Scalar(0.0004) * (desired_ - agcGain_), Scalar(-0.0005), Scalar(0.0005)), Scalar(0.25), Scalar(4));
            yL *= agcGain_ * masterLin_; yR *= agcGain_ * masterLin_;

            auto knee = [](Scalar x) {
                const Scalar a = std::fabs(x);
                // Padé [5/4] tanh (ivanna::nho_tanh, |err|<2e-5) en vez de std::tanh:
                // la rodilla se evalúa 2 veces por muestra y std::tanh es trascendental
                // costoso; la aproximación es inaudible y quita presión al DSP.
                return a <= Scalar(0.8) ? x : std::copysign(Scalar(0.8) + Scalar(0.2) * ivanna::nho_tanh((a - Scalar(0.8)) * Scalar(5)), x);
            };
            yL = knee(yL); yR = knee(yR);
            if (!std::isfinite(yL)) yL = xL;
            if (!std::isfinite(yR)) yR = xR;
            L[i] = xL + fade_ * (yL - xL);
            R[i] = xR + fade_ * (yR - xR);
        }
        lastSpikes_ = spikesBlock;
    }

private:
    static constexpr Scalar kAgcTargetDb = Scalar(-18.0);
    static constexpr Scalar kAgcRate     = Scalar(0.3);
    ivanna::NHOEngineT<Scalar> nho_;
    ivannanpe::LIFPool32       lifPool_;
    ivanna::BiquadEnvelopeBank envBank_;
    Scalar sr_ = Scalar(48000), aAtt_ = Scalar(0), aRel_ = Scalar(0), fadeStep_ = Scalar(0);
    bool   enabled_ = false;
    Scalar harmonic_ = Scalar(0.2), lateral_ = Scalar(0.2), ohc_ = Scalar(0.3), masterLin_ = Scalar(1);
    Scalar agcGain_ = Scalar(1), rms_ = Scalar(0), fade_ = Scalar(0), desired_ = Scalar(1);
    uint32_t agcTick_ = 0;
    int   lastSpikes_ = 0;
};

using NpeStereoCore = NpeStereoCoreT<float>;
using NpeStereoCoreD = NpeStereoCoreT<double>;

}  // namespace ivanna::neuromorphic
