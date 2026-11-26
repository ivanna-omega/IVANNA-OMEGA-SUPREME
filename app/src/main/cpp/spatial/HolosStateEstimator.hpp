// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// HolosStateEstimator — s_b: el vector de estado por banda de HOLOS.
//
// FUSION.md §4.5: UNA sola estimación por frame (hop 128), tres usos
// (G, K, Γ). Es el sentido literal de "mezcla homogénea".
//
//   s_b(t) = ( θ_b, r_b, coh_b, DRR_b, env_b, lvl_b, tr_b )
//
// | Componente | Origen legacy | Estimación en HOLOS |
// | θ_b, r_b   | ORBE (ILD/IPD/coherencia, bisección 24 iter) | Igual, sin cambios |
// | coh_b      | ORBE (\|X\|/√(pL·pR)) | Igual; gobierna directo vs difuso |
// | DRR_b      | (ORBE no la calcula aparte) | Proxy desde coherencia:
// |            | | DRR = 10·log10(coh/(1−coh)) clamp ±20 dB. Se estima y se
// |            | | expone; su uso como modulador de Γ_dinámico queda para
// |            | | fase 2 (el balance directo/difuso ya lo gobierna coh_b).
// |            | | No se inventa un mapeo sin medirlo. |
// | env_b      | PDEngine + coclear Heun + detector compresor | UN solo
// |            | | seguidor: Heun (RK2) con ataque 10 ms / release 80 ms
// |            | | (las constantes fisiológicas del coclear), sobre \|X_b\|,
// |            | | actualizado 1×/frame (hop 128). |
// | lvl_b      | Compresor (2 etapas) + limiter (true-peak) | Pico de banda
// |            | | (máx \|bin\|), ataque inmediato, release 50 ms. |
// | tr_b       | TransientDetector (ratio>3) + PhaseOracle | Ratio
// |            | | envFast/envSlow sobre \|X_b\| (5 ms / 150 ms, umbral 3.0);
// |            | | en ataque, el azimut se estrecha ×0.55 (como el upmixer). |
//
// Header-only, sin alloc en estimate() (tablas de bandas prestadas por
// HolosOperator, que es quien las posee). Estado por instancia.
// La bisección de azimut/distancia es la misma de OrbeBinaural (24 iter,
// NearFieldHeadModel): si el estimador unificado no coincide con ORBE
// dentro de tolerancia, el estimador está mal y se para aquí (FUSION §10
// paso 2).
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>
#include <vector>

#include "NearFieldHeadModel.hpp"

namespace ivanna {
namespace holos {

inline constexpr double kHolosPiF = 3.14159265358979323846;
inline constexpr int kHolosMaxBands = 24;

struct HolosBandState {
    double theta = 0.0;      // azimut estimado (rad, + = derecha)
    double r = 0.35;         // distancia estimada (m)
    double coh = 1.0;        // coherencia interaural [0,1]
    double drrDb = 0.0;      // DRR proxy (dB, clamp ±20)
    double env = 0.0;        // envolvente Heun de |X_b| (lineal)
    double lvl = 0.0;        // pico de banda (lineal)
    bool transient = false;  // ataque detectado este frame
    double envSlow = 0.0;    // envolvente RMS lenta (260 ms) para densidad sostenida
    double crestDb = 0.0;    // crest factor de banda (dB): pico vs RMS sostenido
};

class HolosStateEstimator {
public:
    static constexpr int kHop = 128;

    void prepare(double sr) noexcept {
        sampleRate_ = std::isfinite(sr) ? std::clamp(sr, 8000.0, 384000.0) : 48000.0;
        reset();
    }

    void reset() noexcept {
        meanCoherence_ = 1.0;
        for (int b = 0; b < kHolosMaxBands; ++b) {
            state_[b] = HolosBandState{};
            state_[b].r = distanceM_;
            envFast_[b] = 0.0;
            envSlow_[b] = 0.0;
            envProgram64_[b] = 0.0;
            envSustained64_[b] = 0.0;
            trHold_[b] = 0;
            xReS_[b] = 0.0;
            xImS_[b] = 0.0;
            pLS_[b] = 0.0;
            pRS_[b] = 0.0;
        }
    }

    void setParams(double distanceM, double spread) noexcept {
        distanceM_ = std::isfinite(distanceM) ? std::clamp(distanceM, 0.15, 2.0) : 0.35;
        spread_ = std::isfinite(spread) ? std::clamp(spread, 0.0, 1.5) : 1.0;
    }

    // Tablas de bandas activas (prestadas: las posee HolosOperator).
    // loBin/hiBin/centerHz tienen `count` entradas (12 o 24).
    // F2/486: centerHz sigue en float (tabla de control de HolosOperator);
    // se castea a double al leerla.
    void setBandTables(const int* loBin, const int* hiBin, const float* centerHz,
                       int count) noexcept {
        loBin_ = loBin;
        hiBin_ = hiBin;
        centerHz_ = centerHz;
        bandCount_ = (count == 12) ? 12 : 24;
    }

    int bandCount() const noexcept { return bandCount_; }
    float meanCoherence() const noexcept { return static_cast<float>(meanCoherence_); }
    const HolosBandState& state(int b) const noexcept {
        static const HolosBandState kEmpty{};
        if (b < 0 || b >= bandCount_) return kEmpty;
        return state_[b];
    }

    // Estima s_b para un frame a partir de los espectros (re/im, L/R).
    // n = fftSize (los espectros tienen n bins; solo se usan 0..n/2).
    void estimate(const double* reL, const double* imL, const double* reR, const double* imR,
                  int n) noexcept {
        if (reL == nullptr || imL == nullptr || reR == nullptr || imR == nullptr ||
            loBin_ == nullptr || hiBin_ == nullptr || centerHz_ == nullptr || n <= 0)
            return;
        const double sr = sampleRate_;
        const double hopSec = (double)kHop / sr;
        // Suavizado de escena ~20 ms (como ORBE).
        const double alpha = 1.0 - std::exp(-hopSec / 0.020);
        // Envolvente dual en 64-bit con auto-release dependiente del programa:
        // ataque 10 ms; release continuo entre 65 ms (transitorio/crest alto)
        // y 280 ms (pasaje sostenido denso, cero bombeo).
        const double hopSec64 = static_cast<double>(kHop) / static_cast<double>(sr);
        const double attC64 = 1.0 - std::exp(-hopSec64 / 0.010);
        const double relFastC64 = 1.0 - std::exp(-hopSec64 / 0.065);
        const double relSlowC64 = 1.0 - std::exp(-hopSec64 / 0.280);
        const double rmsAttC64 = 1.0 - std::exp(-hopSec64 / 0.015);
        const double rmsRelC64 = 1.0 - std::exp(-hopSec64 / 0.260);
        // Transientes: envFast 5 ms / envSlow 150 ms.
        const double fastC = 1.0 - std::exp(-hopSec / 0.005);
        const double slowC = 1.0 - std::exp(-hopSec / 0.150);
        // Release del pico de banda: 50 ms.
        const double lvlRel = std::exp(-hopSec / 0.050);
        const double spread = spread_;
        const double distParam = distanceM_;
        double cohSum = 0.0;
        for (int b = 0; b < bandCount_; ++b) {
            const int lo = loBin_[b], hi = hiBin_[b];
            const double fc = static_cast<double>(centerHz_[b]);
            double pL = 0.0, pR = 0.0, xRe = 0.0, xIm = 0.0;
            double peakPow = 0.0;
            for (int k = lo; k < hi && k <= n / 2; ++k) {
                const double lRe = reL[k], lIm = imL[k], rRe = reR[k], rIm = imR[k];
                const double powL = lRe * lRe + lIm * lIm;
                const double powR = rRe * rRe + rIm * rIm;
                pL += powL;
                pR += powR;
                xRe += lRe * rRe + lIm * rIm;   // cross = conj(L)·R
                xIm += lRe * rIm - lIm * rRe;
                // v2.5.453 (F2): pico por POTENCIA, sin sqrt por bin. sqrt
                // es monótona, así que max(magnitud) = sqrt(max(potencia)).
                // 2 sqrt/bin → 1 sqrt/banda. Matemática exacta, mismo
                // resultado; elimina ~514 sqrt por bloque en el bucle
                // más caliente del estimador.
                peakPow = std::max(peakPow, std::max(powL, powR));
            }
            const double peak = (double)std::sqrt(peakPow);
            const int bins = std::max(1, hi - lo);
            // ── Amplitud lineal de la banda (Parseval, ventana Hann) ──
            // D9 (bug encontrado por test_holos_operator): mag era la
            // magnitud FFT cruda (~n/4 × la amplitud temporal) y Γ la
            // leía como dBFS → un tono de 0.5 (−6 dBFS) se veía como
            // +36 dBFS y el compresor lo aplastaba −29 dB. Para un tono
            // de amplitud A, Σ|Y|² (un canal, frec. positivas) =
            // 3n²A²/32 (Parseval, Σw²=3n/8) → A = √(32·p/(3n²)); con
            // dos canales se promedia la potencia: A=√(16·(pL+pR)/(3n²)).
            // Es independiente del ancho de banda (suma potencia, no
            // promedia bins) y da la amplitud equivalente en seno para
            // ruido (√2·RMS de la banda). El pico de banda se normaliza
            // igual (bin pico = A·n/4, con pérdida de scalloping 0.85–1).
            const double mag = (double)std::sqrt(16.0 * (pL + pR) / (3.0 * (double)n * (double)n));
            (void)bins;
            HolosBandState& s = state_[b];

            // ── Pico de banda (amplitud lineal, misma escala que mag) ──
            s.lvl = std::max(peak * (4.0 / (double)n), s.lvl * lvlRel);
            // ── Transiente (ratio envFast/envSlow) ──
            envFast_[b] += fastC * (mag - envFast_[b]);
            envSlow_[b] += slowC * (mag - envSlow_[b]);
            const bool trNow = (envSlow_[b] > 1e-9) && (envFast_[b] / envSlow_[b] > 3.0) &&
                               (envFast_[b] > 1e-4);
            if (trNow) trHold_[b] = 5;  // hold de 5 frames (~13 ms)
            s.transient = trHold_[b] > 0;
            if (trHold_[b] > 0) --trHold_[b];

            // ── Detector dual RMS lento + programa con auto-release (64-bit) ──
            const double mag64 = std::isfinite(mag) ? static_cast<double>(mag) : 0.0;
            const double lvl64 = std::isfinite(s.lvl) ? static_cast<double>(s.lvl) : 0.0;
            const double rcSust = (mag64 > envSustained64_[b]) ? rmsAttC64 : rmsRelC64;
            envSustained64_[b] += rcSust * (mag64 - envSustained64_[b]);
            if (!std::isfinite(envSustained64_[b]) || envSustained64_[b] < 0.0) envSustained64_[b] = 0.0;

            const double crestRatio = lvl64 / std::max(envSustained64_[b], 1.0e-6);
            const double crestDb = std::clamp(20.0 * std::log10(std::max(crestRatio, 1.0)), 0.0, 24.0);
            const double relBlend = s.transient
                ? 1.0
                : std::clamp((crestDb - 3.5) / 8.5, 0.0, 1.0); // 3.5 dB=denso (280 ms) .. 12 dB=transitorio (65 ms)
            const double relC64 = relSlowC64 + relBlend * (relFastC64 - relSlowC64);
            const double hc = (mag64 > envProgram64_[b]) ? attC64 : relC64;
            const double k1 = (mag64 - envProgram64_[b]) * hc;
            const double pred = envProgram64_[b] + k1;
            const double k2 = (mag64 - pred) * hc;
            envProgram64_[b] += 0.5 * (k1 + k2);
            if (!std::isfinite(envProgram64_[b]) || envProgram64_[b] < 0.0) envProgram64_[b] = 0.0;

            // ── Envolvente solo-Parseval (Fase B v2, ajuste 1 de Esse) ──
            // s.env = envProgram64_ ÚNICAMENTE (mag de banda Parseval en
            // double). s.lvl (pico de bin) NO se mezcla: su scalloping
            // Hann a la tasa de hop modulaba Γ y daba AM -> THD+N
            // -67.8 dB @1 kHz. s.lvl se sigue calculando (crestDb lo usa
            // para el auto-release) pero no entra en s.env.
            s.env = static_cast<double>(envProgram64_[b]);
            s.envSlow = static_cast<double>(envSustained64_[b]);
            s.crestDb = static_cast<double>(crestDb);

            // ── Coherencia del cross-espectro COMPLEJO suavizado ──
            // D10 (bug encontrado por test_holos_operator): se promediaba
            // la MAGNITUD de la coherencia por frame, lo que sesga
            // coh → 1/√bins con ruido incorrelado (medido 0.62 en vez
            // de ~0) y roba el término difuso. Se suavizan el
            // cross-espectro complejo y las potencias (20 ms) y la
            // coherencia se calcula DEL PROMEDIO (estimador tipo Welch):
            // el ruido incorrelado cancela vectorialmente y coh → ~0,
            // mientras una señal coherente suma en fase y coh → 1.
            xReS_[b] += alpha * (xRe - xReS_[b]);
            xImS_[b] += alpha * (xIm - xImS_[b]);
            pLS_[b] += alpha * (pL - pLS_[b]);
            pRS_[b] += alpha * (pR - pRS_[b]);

            // ── Escena (ORBE, sin cambios) ──
            double coh = s.coh;
            if (pL + pR > 1e-10) {
                const double denom = std::sqrt(pLS_[b] * pRS_[b]);
                coh = denom > 1e-12
                          ? (double)(std::sqrt(xReS_[b] * xReS_[b] + xImS_[b] * xImS_[b]) / denom)
                          : 0.0;
                coh = std::clamp(coh, 0.0, 1.0);
                const double ildMeas = 10.0 * (double)std::log10((pR + 1e-12) / (pL + 1e-12));
                const double ipd = (double)std::atan2(xIm, xRe);
                const double itdMeas = std::clamp(ipd / (2.0 * kHolosPiF * fc), -0.0008, 0.0008);
                const double rCur = s.r;
                double thetaEst;
                // P3/3.2 (guarda antifase, 2026-11-26): IPD ≈ ±π con ILD ≈ 0
                // es POLARIDAD (contenido antifase, p. ej. side puro), NO un
                // retardo — ver OrbeBinaural::estimateBands (misma guarda).
                // Sin ella, thetaFromItd saturaba a ±90° aleatorio y el
                // side rotado perdía potencia (S puro −0.06 dB).
                const bool antiphase = std::fabs(ipd) > 2.9 && std::fabs(ildMeas) < 1.0;
                if (antiphase) {
                    thetaEst = 0.0;
                } else if (fc < 700.0) {
                    thetaEst = thetaFromItd(itdMeas, rCur);
                } else {
                    thetaEst = thetaFromIld(ildMeas, rCur, fc);
                }
                // En ataque el azimut se estrecha ×0.55 (como el upmixer).
                // No hay realimentación: el transiente depende solo de la
                // envolvente, no de θ_b.
                const double trScale = s.transient ? 0.55 : 1.0;
                thetaEst = std::clamp(thetaEst * spread * trScale, -kHolosPiF * 0.5,
                                      kHolosPiF * 0.5);
                s.theta += alpha * (thetaEst - s.theta);
                if (fc < 800.0 && coh > 0.5) {
                    const double rEst = distanceFromIld(ildMeas, thetaEst, fc);
                    s.r += alpha * (rEst - s.r);
                } else {
                    s.r += alpha * (distParam - s.r);
                }
            }
            // s.coh es ya suave: el suavizado vive en las entradas
            // (cross complejo + potencias), no en el cociente.
            s.coh = coh;
            // DRR proxy desde coherencia (ver cabecera).
            const double c = std::clamp(s.coh, 0.001, 0.999);
            s.drrDb = std::clamp(10.0 * std::log10(c / (1.0 - c)), -20.0, 20.0);
            cohSum += s.coh;
        }
        meanCoherence_ = bandCount_ > 0 ? cohSum / (double)bandCount_ : 1.0;
    }

    // Envoltorio float (compat: HolosOperator legacy / tests): convierte
    // los espectros por scratch y delega en el núcleo double. El estado
    // es compartido (double).
    void estimate(const float* reL, const float* imL, const float* reR, const float* imR,
                  int n) noexcept {
        if (reL == nullptr || imL == nullptr || reR == nullptr || imR == nullptr || n <= 0)
            return;
        const std::size_t un = static_cast<std::size_t>(n);
        if (wrapReL_.size() < un) {
            wrapReL_.resize(un); wrapImL_.resize(un);
            wrapReR_.resize(un); wrapImR_.resize(un);
        }
        for (int k = 0; k < n; ++k) {
            wrapReL_[k] = std::isfinite(reL[k]) ? static_cast<double>(reL[k]) : 0.0;
            wrapImL_[k] = std::isfinite(imL[k]) ? static_cast<double>(imL[k]) : 0.0;
            wrapReR_[k] = std::isfinite(reR[k]) ? static_cast<double>(reR[k]) : 0.0;
            wrapImR_[k] = std::isfinite(imR[k]) ? static_cast<double>(imR[k]) : 0.0;
        }
        estimate(wrapReL_.data(), wrapImL_.data(), wrapReR_.data(), wrapImR_.data(), n);
    }

    double thetaFromItd(double tauSeconds, double r) const noexcept {
        const double a = std::fabs(tauSeconds);
        if (a <= 1e-9) return 0.0;
        const double sign = (tauSeconds > 0.0) ? 1.0 : -1.0;
        const double hi = kHolosPiF * 0.5;
        if (nearfield::itdSeconds(hi, r) <= a) return sign * hi;
        double lo = 0.0, hiB = hi;
        for (int i = 0; i < 24; ++i) {
            const double mid = 0.5 * (lo + hiB);
            if (nearfield::itdSeconds(mid, r) < a) lo = mid; else hiB = mid;
        }
        return sign * 0.5 * (lo + hiB);
    }

    double thetaFromIld(double ildDbMeas, double r, double freqHz) const noexcept {
        const double a = std::fabs(ildDbMeas);
        if (a <= 1e-6) return 0.0;
        const double sign = (ildDbMeas > 0.0) ? 1.0 : -1.0;
        const double hi = kHolosPiF * 0.5;
        if (nearfield::ildDb(hi, r, freqHz) <= a) return sign * hi;
        double lo = 0.0, hiB = hi;
        for (int i = 0; i < 24; ++i) {
            const double mid = 0.5 * (lo + hiB);
            if (nearfield::ildDb(mid, r, freqHz) < a) lo = mid; else hiB = mid;
        }
        return sign * 0.5 * (lo + hiB);
    }

    double distanceFromIld(double ildDbMeas, double theta, double freqHz) const noexcept {
        const double a = std::fabs(ildDbMeas);
        const double th = std::fabs(theta);
        if (th < 0.05) return distanceM_;  // sin info de ILD en el centro
        if (a <= nearfield::ildDb(th, 2.0, freqHz)) return 2.0;
        if (a >= nearfield::ildDb(th, 0.15, freqHz)) return 0.15;
        double lo = 0.15, hi = 2.0;  // ildDb decrece con r
        for (int i = 0; i < 24; ++i) {
            const double mid = 0.5 * (lo + hi);
            if (nearfield::ildDb(th, mid, freqHz) > a) lo = mid; else hi = mid;
        }
        return 0.5 * (lo + hi);
    }

private:
    double sampleRate_ = 48000.0;
    double distanceM_ = 0.35;
    double spread_ = 1.0;
    const int* loBin_ = nullptr;
    const int* hiBin_ = nullptr;
    const float* centerHz_ = nullptr;
    int bandCount_ = 24;
    HolosBandState state_[kHolosMaxBands];
    double envFast_[kHolosMaxBands] = {};
    double envSlow_[kHolosMaxBands] = {};
    double envProgram64_[kHolosMaxBands] = {};
    double envSustained64_[kHolosMaxBands] = {};
    int trHold_[kHolosMaxBands] = {};
    // Estado suavizado para la coherencia (D10): cross-espectro complejo
    // y potencias por banda, EMA 20 ms. Se resetean en reset().
    double xReS_[kHolosMaxBands] = {};
    double xImS_[kHolosMaxBands] = {};
    double pLS_[kHolosMaxBands] = {};
    double pRS_[kHolosMaxBands] = {};
    double meanCoherence_ = 1.0;
    // Scratch del envoltorio float de estimate() (crece bajo demanda).
    std::vector<double> wrapReL_{};
    std::vector<double> wrapImL_{};
    std::vector<double> wrapReR_{};
    std::vector<double> wrapImR_{};
};

}  // namespace holos
}  // namespace ivanna
