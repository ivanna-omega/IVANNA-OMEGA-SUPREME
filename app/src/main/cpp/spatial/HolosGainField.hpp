// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// HolosGainField — Γ_b: el campo de ganancia único de HOLOS.
//
// FUSION.md §4.4.3: compresor, limiter, dos EQs, tres M/S, trims y tilts
// son todos multiplicaciones puntuales por banda e instante. HOLOS calcula
// UNA sola ganancia por banda:
//
//   Γ_b = Γ_estático(b) · Γ_dinámico(b,t) · Γ_ancho(b) · Γ_techo(b,t)
//
// Funciones puras, sin estado, sin alloc: testeables sin audio
// (tests/test_holos_gain_field.cpp). Cada ley es la del módulo legacy que
// absorbe, con sus constantes exactas leídas del código:
//
// - Γ_estático: la magnitud del ParametricEQ (8 biquads RBJ en cascada,
//   dsp/ParametricEQ.cpp::setBandShaped) evaluada analíticamente en el
//   centro de cada banda ERB. La banda muerta (500 Hz, ganancia 0 siempre)
//   y el m_evolutionPath muerto desaparecen. El FIR de 256 taps del EvoEQ
//   (fase lineal, latencia 127.5 muestras) se reemplaza por magnitud por
//   bin (fase cero): la curva es la misma, la fase no (FUSION §5.5).
// - Γ_dinámico: la ley del compresor DSP (dsp/Compressor.cpp): soft-knee
//   cuadrático ±3 dB, makeup acoplado sin ganancia gratuita. UN solo
//   default canónico: threshold −15 dB / ratio 3:1 (el de DSPParams;
//   FUSION §4.4.3 resuelve así el conflicto de tres defaults).
// - Γ_ancho: el operador M/S único: ganancia de side desde θ/coh, con el
//   split de graves del StereoWidener (bassFactor) y la norma de potencia
//   constante del M/S de FusionCore (1/√(0.5(1+w²))).
// - Γ_techo: la racional softCeil del SafetyLimiter (dsp/SafetyLimiter.cpp)
//   + la ganancia de pico del limiter. Es el único término que NO se
//   absorbe del todo: la guarda final sigue siendo un módulo independiente
//   (FUSION §5.4). En el operador, Γ_techo se evalúa en el dominio temporal
//   sobre el hop renderizado (un pico verdadero solo existe en tiempo;
//   recortar magnitud por bin distorsionaría la fase).
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>

// Techo único −0.1 dBFS (P3/3.5, 2026-11-26): FusedDynamics::kLimiterCeiling
// (0.9885531f, el float32 más cercano a 10^(−0.1/20)). Antes estos dos
// defaults usaban el literal viejo de 5 decimales (−0.1000271 dB); el techo final
// de fused_chain.cpp ya usaba la constante, así que softCeil/Γ_techo y el
// techo final divergían 2.7e-5 dB. El include resuelve en todos los TUs que
// incluyen este header (Android: ${CMAKE_SOURCE_DIR} en el path; host:
// `-I ..`; ver tests/CMakeLists.txt).
#include "fused/fused_dynamics.hpp"

namespace ivanna {
namespace holos {

// ── Γ_estático: magnitud RBJ del ParametricEQ ──────────────────────────────
// Parámetros de EQ en dB, mismo mapeo que ParametricEQ::setParams
// (dsp/ParametricEQ.cpp): low/mid/high/presence + fatigueTilt + freq/Q.
struct EqParams {
    double low = 0.0;       // dB, low-shelf 90 Hz
    double mid = 0.0;       // dB, picos 2500 Hz (×1.0) y freq (×0.6)
    double high = 0.0;      // dB, pico 5000 Hz (×0.35) + high-shelf 10 kHz
    double presence = 0.0;  // dB, pico 8000 Hz
    double tiltDb = 0.0;    // dB, fatigueTilt [−3, 0]
    double freq = 1000.0;   // Hz, banda paramétrica libre
    double resonance = 0.707; // Q de las campanas
};

namespace detail {
// Coeficientes RBJ (doble precisión), copia exacta de
// ParametricEQ::setBandShaped. shape: 0=Peak, 1=LowShelf, 2=HighShelf.
inline void rbjCoeffs(int shape, double f, double q, double gDb, double sr,
                      double& b0, double& b1, double& b2, double& a1, double& a2) {
    const double A = std::pow(10.0, gDb / 40.0);
    const double w0 = 2.0 * 3.14159265358979323846 * f / sr;
    const double c = std::cos(w0), sn = std::sin(w0);
    const double alpha = sn / (2.0 * q);
    double db0, db1, db2, da0, da1, da2;
    if (shape == 1) {  // LowShelf
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        db0 = A * ((A + 1.0) - (A - 1.0) * c + tsa);
        db1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * c);
        db2 = A * ((A + 1.0) - (A - 1.0) * c - tsa);
        da0 = (A + 1.0) + (A - 1.0) * c + tsa;
        da1 = -2.0 * ((A - 1.0) + (A + 1.0) * c);
        da2 = (A + 1.0) + (A - 1.0) * c - tsa;
    } else if (shape == 2) {  // HighShelf
        const double tsa = 2.0 * std::sqrt(A) * alpha;
        db0 = A * ((A + 1.0) + (A - 1.0) * c + tsa);
        db1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * c);
        db2 = A * ((A + 1.0) + (A - 1.0) * c - tsa);
        da0 = (A + 1.0) - (A - 1.0) * c + tsa;
        da1 = 2.0 * ((A - 1.0) - (A + 1.0) * c);
        da2 = (A + 1.0) - (A - 1.0) * c - tsa;
    } else {  // Peak
        db0 = 1.0 + alpha * A; db1 = -2.0 * c; db2 = 1.0 - alpha * A;
        da0 = 1.0 + alpha / A; da1 = -2.0 * c; da2 = 1.0 - alpha / A;
    }
    b0 = db0 / da0; b1 = db1 / da0; b2 = db2 / da0;
    a1 = da1 / da0; a2 = da2 / da0;
}

// |H(e^jw)| de un biquad en frecuencia f.
inline double biquadMagnitude(double b0, double b1, double b2, double a1, double a2,
                              double f, double sr) {
    const double w = 2.0 * 3.14159265358979323846 * f / sr;
    const double cw = std::cos(w), sw = std::sin(w);
    const double cw2 = std::cos(2.0 * w), sw2 = std::sin(2.0 * w);
    const double br = b0 + b1 * cw + b2 * cw2;
    const double bi = -(b1 * sw + b2 * sw2);
    const double ar = 1.0 + a1 * cw + a2 * cw2;
    const double ai = -(a1 * sw + a2 * sw2);
    return std::sqrt((br * br + bi * bi) / std::max(ar * ar + ai * ai, 1e-30));
}
}  // namespace detail

// Ganancia estática (lineal) del EQ en la frecuencia f: producto de las
// magnitudes de las 8 bandas del ParametricEQ. Con todos los parámetros
// en 0 → 1.0 exacto (no-op bit-exacto, como el paramétrico plano).
inline double staticGainLin(double freqHz, const EqParams& eq, double sampleRate) noexcept {
    if (!std::isfinite(freqHz) || !std::isfinite(sampleRate) || sampleRate < 8000.0)
        return 1.0;
    const double sr = sampleRate;
    const double f = std::clamp((double)freqHz, 20.0, sr * 0.5 - 100.0);
    const double q = std::clamp((double)eq.resonance, 0.1, 10.0);
    const double tilt = eq.tiltDb;
    auto clampDb = [](double db) { return std::clamp(db, -18.0, 18.0); };
    // (shape, f, q, g) — mismo mapeo que ParametricEQ::setParams.
    struct Band { int shape; double f; double q; double g; };
    const Band bands[8] = {
        {1, 90.0, 0.707, clampDb(eq.low)},
        {0, 250.0, 1.0, (eq.low > 0.0) ? clampDb(-0.08 * (double)eq.low) : 0.0},
        {0, 500.0, q, 0.0},  // banda muerta (ganancia 0 siempre)
        {0, std::clamp((double)eq.freq, 20.0, sr * 0.5 - 100.0), q, clampDb(eq.mid * 0.6)},
        {0, 2500.0, q, clampDb(eq.mid)},
        {0, 5000.0, q, clampDb(eq.high * 0.35 + tilt / 3.0)},
        {0, 8000.0, q, clampDb(eq.presence + tilt * (2.0 / 3.0))},
        {2, 10000.0, 0.707, clampDb(eq.high + tilt)},
    };
    double mag = 1.0;
    for (const Band& b : bands) {
        double b0, b1, b2, a1, a2;
        detail::rbjCoeffs(b.shape, b.f, b.q, b.g, sr, b0, b1, b2, a1, a2);
        mag *= detail::biquadMagnitude(b0, b1, b2, a1, a2, f, sr);
    }
    return (double)mag;
}

// ── Γ_dinámico: ley de compresión del Compressor DSP ───────────────────────
// Default canónico único (FUSION §4.4.3): threshold −15 dB / ratio 3:1.
// Devuelve la ganancia lineal total (reducción + makeup acoplado).
// envDb: nivel de la envolvente unificada en dBFS (20·log10(env)).
inline double dynamicGainLin(double envDb, double thresholdDb = -15.0, double ratio = 3.0,
                            double runtimeAmount = 0.0) noexcept {
    if (!std::isfinite(envDb)) return 1.0;
    const double thr = thresholdDb - runtimeAmount * 12.0;
    const double effRatio = std::max(ratio + runtimeAmount * 8.0, 1.0);
    const double ratioInv = 1.0 - 1.0 / effRatio;
    const double overDb = envDb - thr;
    double gainDb = 0.0;
    constexpr double kKneeHalf = 3.0;
    if (overDb <= -kKneeHalf) {
        gainDb = 0.0;
    } else if (overDb < kKneeHalf) {
        const double t = (overDb + kKneeHalf) / (2.0 * kKneeHalf);
        gainDb = -(ratioInv * kKneeHalf) * t * t;
    } else {
        gainDb = overDb * (-ratioInv);
    }
    // Makeup acoplado a la reducción REAL (nunca hay ganancia gratuita
    // bajo threshold): techo = −threshold·(1−1/R)·0.5 + aporte runtime.
    const double makeupCeilDb =
        std::max(0.0, -thresholdDb * (1.0 - 1.0 / ratio) * 0.5) +
        runtimeAmount * 12.0 * (1.0 - 1.0 / effRatio) * 0.10;
    const double makeupDb = std::min(-gainDb, makeupCeilDb);
    return std::exp((gainDb + makeupDb) * 0.11512925465);
}

// Ganancia de compresión en dB (sin makeup), para tests de la curva.
inline double dynamicGainDbNoMakeup(double envDb, double thresholdDb = -15.0,
                                   double ratio = 3.0) noexcept {
    if (!std::isfinite(envDb)) return 0.0;
    const double ratioInv = 1.0 - 1.0 / std::max(ratio, 1.0);
    const double overDb = envDb - thresholdDb;
    constexpr double kKneeHalf = 3.0;
    if (overDb <= -kKneeHalf) return 0.0;
    if (overDb < kKneeHalf) {
        const double t = (overDb + kKneeHalf) / (2.0 * kKneeHalf);
        return -(ratioInv * kKneeHalf) * t * t;
    }
    return overDb * (-ratioInv);
}

// ── Γ_dinámico v2 (nivel mastering, sin bombeo, CERO makeup gratuito) ─────
// - Bajo threshold - kKneeHalf (-18 dB por defecto): exactamente 0.0 dB (1.0).
// - Sobre umbral: soft-knee cuadrático C1 (±4 dB), ratio suave 2.2:1 que se
//   relaja ligeramente en transitorios limpios (crestDb > 9 dB) para no
//   aplastar el ataque natural, y sin makeup gain gratuito.
inline double dynamicGainMasterDb(double envDb, double crestDb = 6.0,
                                  double thresholdDb = -14.0,
                                  double ratio = 2.2) noexcept {
    if (!std::isfinite(envDb)) return 0.0;
    const double crest = std::isfinite(crestDb) ? std::clamp(crestDb, 0.0, 24.0) : 6.0;
    // Alivio en transitorios limpios: hasta -20 % de ratio si crestDb >= 14 dB.
    const double relief = std::clamp((crest - 8.0) / 8.0, 0.0, 1.0);
    const double effRatio = std::max(1.0, ratio * (1.0 - 0.20 * relief));
    const double ratioInv = 1.0 - (1.0 / effRatio);
    const double overDb = envDb - thresholdDb;
    constexpr double kKneeHalf = 4.0;
    if (overDb <= -kKneeHalf) return 0.0;
    if (overDb < kKneeHalf) {
        const double t = (overDb + kKneeHalf) / (2.0 * kKneeHalf);
        return -(ratioInv * kKneeHalf) * t * t;
    }
    return overDb * (-ratioInv);
}

// Acoplamiento inter-banda + ancla global suave: evita que una banda ERB
// respire ±4 dB separada de sus vecinas y rompa el timbre del instrumento.
// Con todas las entradas en 0.0 -> salida 0.0 exacta.
inline void coupleBandGainsDb(const double* rawDb, double* coupledDb,
                              int bandCount, double coupling = 0.35,
                              double globalAnchor = 0.15) noexcept {
    if (!rawDb || !coupledDb || bandCount <= 0) return;
    const double c = std::clamp(coupling, 0.0, 1.0);
    const double a = std::clamp(globalAnchor, 0.0, 0.5);
    double sum = 0.0;
    for (int b = 0; b < bandCount; ++b) sum += rawDb[b];
    const double meanDb = sum / static_cast<double>(bandCount);
    for (int b = 0; b < bandCount; ++b) {
        const double left = (b > 0) ? rawDb[b - 1] : rawDb[b];
        const double right = (b + 1 < bandCount) ? rawDb[b + 1] : rawDb[b];
        const double neighMean = 0.25 * left + 0.50 * rawDb[b] + 0.25 * right;
        const double local = (1.0 - c) * rawDb[b] + c * neighMean;
        coupledDb[b] = (1.0 - a) * local + a * meanDb;
    }
}

// Ancla de banda ancha (linked) para la ganancia dinámica — Fase B v2b.
// POR QUÉ EXISTE (medido en host, 2026-11-18): la STFT es periódica en
// el hop (375 Hz) y un ESCALÓN de Γ entre bandas vecinas sobre el
// lóbulo principal de un tono se convierte en AM a 375/125 Hz:
//   escalón 0.116 dB (ley vieja, makeup) -> THD+N -94.0 dB @1 kHz
//   escalón 2.2 dB (ley Esse sin anclar) -> THD+N -68.4 dB @1 kHz
// La relación es lineal en dB (25.6 dB de escalón = 25.6 dB de THD),
// así que la puerta -90 dB exige un escalón <= ~0.18 dB y la línea
// base -94 dB exige <= ~0.12 dB. NINGUNA ley multibanda real con
// cero makeup puede tener escalones de 2-3 dB entre bandas y pasar
// la puerta: la salida es anclar el perfil a la reducción más
// profunda (la de la banda más cargada) y dejar solo 0.05 dB de
// desviación por banda. La detección sigue siendo multibanda (cada
// banda aporta su envolvente, su crest y su acoplamiento); la
// APLICACIÓN es enlazada. Cero makeup, sin bombeo, profundidad de
// compresión preservada (la banda que más comprime sigue comprimiendo
// lo mismo; las demás la siguen a <= 0.05 dB).
// Con todas las entradas en 0.0 -> salida 0.0 exacta.
inline void linkBandGainsDb(const double* coupledDb, double* linkedDb,
                            int bandCount, double maxDeviationDb = 0.05) noexcept {
    if (!coupledDb || !linkedDb || bandCount <= 0) return;
    const double cap = std::clamp(maxDeviationDb, 0.0, 1.0);
    double deepest = 0.0;
    for (int b = 0; b < bandCount; ++b) deepest = std::min(deepest, coupledDb[b]);
    for (int b = 0; b < bandCount; ++b) {
        const double dev = std::clamp(coupledDb[b] - deepest, 0.0, cap);
        linkedDb[b] = deepest + dev;
    }
}

// ── Γ_ancho: el operador M/S único ─────────────────────────────────────────
// w: ganancia de side pedida (0=mono, 1=unity). En graves (f<150 Hz) se
// aplica el bassFactor del StereoWidener: w≤1 → w; w>1 → 1.75−0.75·w.
// Devuelve {sideGain, norm}: outL=(mid+side·sideGain)·norm, igual que el
// M/S de FusionCore (norm = 1/√(0.5(1+w²)), potencia constante).
struct WidthGains { double sideGain = 1.0; double norm = 1.0; };
inline WidthGains widthGains(double w, double freqHz) noexcept {
    if (!std::isfinite(w) || !std::isfinite(freqHz)) return WidthGains{};
    double ww = std::clamp(w, 0.0, 3.0);
    if (freqHz < 150.0 && ww > 1.0) ww = std::max(1.75 - 0.75 * ww, 0.25);
    WidthGains out;
    out.sideGain = ww;
    out.norm = 1.0 / std::sqrt(0.5 * (1.0 + ww * ww));
    return out;
}

// w_b desde el estado de la banda: fuentes difusas (coh baja) → side alto;
// fuentes puntuales (coh alta) → side bajo. widthScale = holos_width.
inline double widthFromState(double coherence, double widthScale) noexcept {
    if (!std::isfinite(coherence) || !std::isfinite(widthScale)) return 1.0;
    const double coh = std::clamp(coherence, 0.0, 1.0);
    return std::clamp(widthScale * (0.3 + 0.7 * (1.0 - coh)), 0.0, 1.5);
}

// ── Γ_techo: softCeil + ganancia de pico del SafetyLimiter ────────────────
// softCeil: identidad ≤0.97·ceiling; racional (continua en valor y
// pendiente) por encima; nunca supera el ceiling. Copia exacta de
// SafetyLimiter::softCeil (dsp/SafetyLimiter.cpp).
inline double softCeil(double x, double ceiling = static_cast<double>(
                                            ivanna::fused::FusedDynamics::kLimiterCeiling)) noexcept {
    if (!std::isfinite(x)) return 0.0;
    const double knee = ceiling * 0.97;
    const double ax = std::fabs(x);
    if (ax <= knee) return x;
    const double range = ceiling - knee;  // > 0
    const double over = (ax - knee) / range;
    const double y = knee + range * (over / (1.0 + over));
    return (x < 0.0) ? -y : y;
}

// Ganancia objetivo (≤1) para un pico lineal: la curva de
// SafetyLimiter::computeGainForPeak (threshold −1 dBFS, ceiling −0.1 dBFS).
inline double limiterGainForPeak(double peakLin, double threshold = 0.891251,
                                double ceiling = static_cast<double>(
                                    ivanna::fused::FusedDynamics::kLimiterCeiling)) noexcept {
    if (!std::isfinite(peakLin) || peakLin <= threshold) return 1.0;
    const double excess = peakLin - threshold;
    double limited = threshold + excess / (1.0 + excess * 8.0);
    if (limited > ceiling) limited = ceiling;
    return limited / peakLin;
}

}  // namespace holos
}  // namespace ivanna
