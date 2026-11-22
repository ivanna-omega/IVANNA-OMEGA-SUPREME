// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// HolosGreenKernel — G_b: la función de Green unificada de HOLOS.
//
// FUSION.md §4.4.1: HOA, WFS, RIR, HRTF, ORBE y crossfeed son todos
// soluciones de la misma ecuación de onda con condiciones de contorno
// distintas. G_b las evalúa UNA vez por banda ERB en vez de cinco veces
// con cinco constantes distintas:
//
//   Directo: fase = exp(∓jω·ITD_esférico(θ,r)/2)  [rotación de fase exacta]
//            magnitud = ILD_esférico(θ,r,f) · pinna(θ,f) · (1/r)^γ
//   Difuso:  (1 − coh_b) → cola de sala compartida (RirConvolver)
//
// Funciones puras, sin estado, sin alloc: testeables sin audio
// (tests/test_holos_green_kernel.cpp). Reutilizan NearFieldHeadModel
// (el modelo esférico ya testeado de ORBE) — no lo reescriben.
//
// Decisiones de v1 (honestas, documentadas):
// - γ (exponente de divergencia) entra SOLO diferencialmente: el término
//   de divergencia 20·log10(dL/dR) del ILD se escala por γ. γ=1 → ley
//   esférica 1/r (ORBE/HRTF); γ=½ → ley cilíndrica 1/√d (WFS 2.5D).
//   El nivel GLOBAL (1/r)^γ se cancela con la normalización de potencia
//   constante, como ORBE: HOLOS nunca sube el nivel global (FUSION §5.4).
// - pinna(θ,f): magnitud de pinna MEDIDA, interpolada linealmente (v1;
//   OT en fase 2). Sin tabla cargada → 0 dB (identidad). No se inventa
//   ninguna curva: la tabla se alimenta desde HrtfManager/datasets .ihr1
//   cuando exista; hasta entonces la coloración de pabellón sigue llegando
//   por el camino legacy HOA+HRTF. En v1 la pinna es un término
//   DIFERENCIAL (R−L, como el ILD); la tabla por oído separada es fase 2.
// - El crossfeed es el límite de baja frecuencia de esta misma fórmula
//   (f<700 Hz, θ=30° fijo): no es un módulo aparte.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>
#include <vector>

#include "NearFieldHeadModel.hpp"

namespace ivanna {
namespace holos {

inline constexpr double kPi = 3.14159265358979323846;
// Compat: el test y código legacy usan holos::kPiF; mismo valor, double.
inline constexpr double kPiF = kPi;

// ILD esférico con exponente de divergencia γ (FUSION §4.4.1).
// El término de divergencia 20·log10(dL/dR) se escala por γ; la sombra
// cefálica dependiente de la frecuencia no depende de γ.
// Se deriva de nearfield::ildDb sin duplicar su geometría: a f=0 la sombra
// es cero, así que ildDb(θ,r,0) ES el término de divergencia puro.
inline double ildDbGamma(double thetaRad, double r, double freqHz, double gamma) noexcept {
    if (!std::isfinite(thetaRad) || !std::isfinite(r) || !std::isfinite(freqHz) ||
        !std::isfinite(gamma))
        return 0.0;
    const double g = std::clamp(gamma, 0.5, 1.0);
    const double spreading = nearfield::ildDb(thetaRad, r, 0.0);
    const double total = nearfield::ildDb(thetaRad, r, freqHz);
    const double shadow = total - spreading;
    return g * spreading + shadow;
}

// Ganancias binaurales de G_b para una banda (θ, r, f):
// ILD (con γ) + magnitud de pinna diferencial + estante de proximidad,
// con normalización de potencia constante (la parte común se cancela).
struct GreenGains {
    double gL = 1.0;         // ganancia oído izquierdo (lineal)
    double gR = 1.0;         // ganancia oído derecho (lineal)
    double itdSeconds = 0.0; // ITD esférico (s); >0 = fuente a la derecha
    double ildDb = 0.0;      // ILD aplicado (dB, R−L)
    double proximityDb = 0.0; // estante de proximidad aplicado (dB)
};

inline GreenGains greenGains(double thetaRad, double r, double freqHz, double gamma,
                             double pinnaDb, double proximityCapDb) noexcept {
    GreenGains out;
    if (!std::isfinite(thetaRad) || !std::isfinite(r) || !std::isfinite(freqHz))
        return out;
    const double theta = std::clamp(thetaRad, -kPi, kPi);
    const double rr = std::clamp(r, 0.15, 2.0);
    const double f = std::clamp(freqHz, 0.0, 20000.0);
    out.itdSeconds = nearfield::itdSeconds(theta, rr);
    out.ildDb = ildDbGamma(theta, rr, f, gamma);
    const double proxScale = std::clamp(proximityCapDb, 0.0, 6.0) / 6.0;
    out.proximityDb = nearfield::proximityShelfDb(rr, f) * proxScale;
    // La pinna diferencial se suma al ILD (misma convención R−L).
    const double ildTotal = out.ildDb + std::clamp(pinnaDb, -24.0, 24.0);
    double gL = std::pow(10.0, -ildTotal / 40.0);
    double gR = std::pow(10.0, +ildTotal / 40.0);
    // Estante de proximidad repartido por lateralización (wR = ½(1+sinθ)),
    // como ORBE: en el centro ambos oídos reciben la mitad y la parte común
    // la cancela la normalización — el estante es un componente de ILD,
    // no una ganancia global.
    const double wR = 0.5 * (1.0 + std::sin(theta));
    const double wL = 1.0 - wR;
    gL *= std::pow(10.0, out.proximityDb * wL / 20.0);
    gR *= std::pow(10.0, out.proximityDb * wR / 20.0);
    const double norm = std::sqrt(2.0 / (gL * gL + gR * gR));
    out.gL = gL * norm;
    out.gR = gR * norm;
    return out;
}

// Mezcla directo/difuso de una banda: cuánto del side va a la cola
// compartida (0 = todo directo, 1 = todo difuso). La coherencia gobierna:
// fuente puntual (coh≈1) → directo; campo difuso (coh≈0) → cola.
inline double diffuseMix(double coherence, double diffuseParam) noexcept {
    if (!std::isfinite(coherence) || !std::isfinite(diffuseParam)) return 0.0;
    const double coh = std::clamp(coherence, 0.0, 1.0);
    const double d = std::clamp(diffuseParam, 0.0, 1.0);
    return (1.0 - coh) * d;
}

// ── Reforma v2.5.429: G_b por banda + por bin ──────────────────────────
// Problema medido en vivo (2026-11-12 23:43, v2.5.428): HOLOS p99 13–25 ms
// (presupuesto 6.67 ms). El cuello es renderBins(): llamaba a greenGains()
// por BIN (512×/frame), y greenGains() recalcula por bin cosas que solo
// dependen de la BANDA (24 bandas): itdSeconds() (2× pathLengthToEar con
// atan2/acos/sin/cos/sqrt), el ILD por divergencia ildDb(θ,r,0) y
// sin(θ). La reforma separa G_b en dos partes con la MISMA matemática
// (bit-idéntica a greenGains, verificada por test):
//   - GreenBandBase: todo lo independiente de la frecuencia, 1×/banda/frame.
//   - greenGainsBin(): solo lo dependiente de f (sombra cefálica,
//     estante de proximidad, pow de ganancias), 1×/bin.
// El render pasa de ~15–20 trascendentales por bin a ~4 (1 log10, 4 pow,
// 1 sqrt). HOLOS deja de ser el cuello y corre a 24 bandas dentro del
// presupuesto — IVANNA cumple lo que ofrece, sin bypass ni degradación.
// greenGains() se conserva (tests y referencia); el camino vivo usa la
// versión por banda+bin.
struct GreenBandBase {
    double theta = 0.0;        // azimut de banda (clamp −π..π)
    double itdSeconds = 0.0;   // ITD esférico (s), independiente de f
    double spreadingDb = 0.0;  // ILD por divergencia 20·log10(dL/dR) (f=0)
    double sinTheta = 0.0;
    double sin2Theta = 0.0;    // sin²θ (sombra cefálica)
    double wR = 0.5, wL = 0.5;  // reparto de proximidad por lateralización
    double proxCapDb = 0.0;    // min(20·log10(1/r), 6), 0 si r≥1 m
    double gamma = 1.0;        // exponente de divergencia (clamp 0.5..1)
    double proxScale = 0.0;    // clamp(proximityCapDb,0,6)/6
};

inline GreenBandBase greenBandBase(double thetaRad, double r, double gamma,
                                   double proximityCapDb) noexcept {
    GreenBandBase b;
    if (!std::isfinite(thetaRad) || !std::isfinite(r) || !std::isfinite(gamma) ||
        !std::isfinite(proximityCapDb))
        return b;
    const double theta = std::clamp(thetaRad, -kPi, kPi);
    const double rr = std::clamp(r, 0.15, 2.0);
    b.theta = theta;
    b.itdSeconds = nearfield::itdSeconds(theta, rr);
    // A f=0 la sombra es 0 (ka=0), así que ildDb(θ,r,0) ES el spreading.
    b.spreadingDb = nearfield::ildDb(theta, rr, 0.0);
    b.sinTheta = std::sin(theta);
    b.sin2Theta = b.sinTheta * b.sinTheta;
    b.wR = 0.5 * (1.0 + b.sinTheta);
    b.wL = 1.0 - b.wR;
    if (rr >= 1.0) {
        b.proxCapDb = 0.0;
    } else {
        b.proxCapDb = std::min(20.0 * std::log10(1.0 / rr), 6.0);
    }
    b.gamma = std::clamp(gamma, 0.5, 1.0);
    b.proxScale = std::clamp(proximityCapDb, 0.0, 6.0) / 6.0;
    return b;
}

// G_b por bin desde la base de banda: misma matemática que greenGains().
// Solo lo dependiente de f: sombra cefálica (ka=2πfa/c) y estante de
// proximidad (shape 1/(1+(f/300)²)); las ganancias combinan los mismos
// 4 pow() que greenGains() en el mismo orden.
inline GreenGains greenGainsBin(const GreenBandBase& b, double freqHz,
                                double pinnaDb) noexcept {
    GreenGains out;
    if (!std::isfinite(freqHz) || !std::isfinite(pinnaDb)) return out;
    const double f = std::clamp(freqHz, 0.0, 20000.0);
    const double ka = 2.0 * kPi * f * nearfield::kHeadRadiusM /
                     nearfield::kSoundSpeedMps;
    const double shadowMagDb =
        20.0 * std::log10(1.0 + 0.25 * ka * ka * b.sin2Theta);
    const double shadowDb = (b.sinTheta >= 0.0) ? shadowMagDb : -shadowMagDb;
    const double ildDb = b.gamma * b.spreadingDb + shadowDb;
    const double shape = 1.0 / (1.0 + (f / 300.0) * (f / 300.0));
    const double proximityDb = b.proxCapDb * shape * b.proxScale;
    const double ildTotal = ildDb + std::clamp(pinnaDb, -24.0, 24.0);
    // v2.5.453 (F2): 4 pow() → 2 pow() por bin. pow(10,a)·pow(10,b) =
    // pow(10,a+b) es matemática exacta (mismo resultado, la mitad de
    // trascendentales en el bucle por bin más caliente del render).
    // gL = 10^(-ildTotal/40) · 10^(proximityDb·wL/20)
    // gR = 10^(+ildTotal/40) · 10^(proximityDb·wR/20)
    double gL = std::pow(10.0, -ildTotal / 40.0 + proximityDb * b.wL / 20.0);
    double gR = std::pow(10.0, +ildTotal / 40.0 + proximityDb * b.wR / 20.0);
    const double norm = std::sqrt(2.0 / (gL * gL + gR * gR));
    out.gL = gL * norm;
    out.gR = gR * norm;
    out.itdSeconds = b.itdSeconds;
    out.ildDb = ildDb;
    out.proximityDb = proximityDb;
    return out;
}

// Tabla de magnitud de pinna medida (diferencial R−L, dB), interpolación
// lineal en azimut y en log-frecuencia (v1; transporte óptimo en fase 2).
// Sin tabla cargada → 0 dB (identidad). La tabla se alimenta desde
// HrtfManager/datasets .ihr1; NUNCA se rellena con una curva inventada.
struct PinnaTable {
    std::vector<double> azimuthsRad; // crecientes, p. ej. −π/2..π/2
    std::vector<double> freqsHz;     // crecientes, p. ej. 1k..16k
    std::vector<double> magDb;       // [azimut][frecuencia], diferencial R−L

    bool empty() const noexcept { return azimuthsRad.empty() || freqsHz.empty(); }

    // F2/486: almacenamiento double (la interpolación es plano de
    // señal); set() sigue aceptando vectores float (datos medidos
    // .ihr1) con cast exacto.
    void set(const std::vector<float>& az, const std::vector<float>& fr,
             const std::vector<float>& mag) noexcept {
        if (az.empty() || fr.empty() || mag.size() != az.size() * fr.size()) return;
        azimuthsRad.assign(az.begin(), az.end());
        freqsHz.assign(fr.begin(), fr.end());
        magDb.assign(mag.begin(), mag.end());
    }

    double pinnaDb(double thetaRad, double freqHz) const noexcept {
        if (empty() || !std::isfinite(thetaRad) || !std::isfinite(freqHz)) return 0.0;
        const double th = std::clamp(thetaRad, azimuthsRad.front(), azimuthsRad.back());
        const double f = std::clamp(freqHz, freqsHz.front(), freqsHz.back());
        // Índices de interpolación (lineal en θ, lineal en log-f).
        size_t ia = 0;
        while (ia + 1 < azimuthsRad.size() && azimuthsRad[ia + 1] < th) ++ia;
        size_t ib = 0;
        const double lf = std::log(std::max(f, 1.0));
        while (ib + 1 < freqsHz.size() &&
               std::log(std::max(freqsHz[ib + 1], 1.0)) < lf)
            ++ib;
        const double tA = (azimuthsRad.size() > 1 && ia + 1 < azimuthsRad.size())
                             ? (th - azimuthsRad[ia]) /
                                   std::max(azimuthsRad[ia + 1] - azimuthsRad[ia], 1e-9)
                             : 0.0;
        const double l0 = std::log(std::max(freqsHz[ib], 1.0));
        const double l1 = (ib + 1 < freqsHz.size())
                             ? std::log(std::max(freqsHz[ib + 1], 1.0))
                             : l0;
        const double tB = (l1 > l0) ? (lf - l0) / (l1 - l0) : 0.0;
        const size_t na = azimuthsRad.size(), nf = freqsHz.size();
        const size_t ja = std::min(ia + 1, na - 1), jb = std::min(ib + 1, nf - 1);
        const double v00 = magDb[ia * nf + ib];
        const double v10 = magDb[ja * nf + ib];
        const double v01 = magDb[ia * nf + jb];
        const double v11 = magDb[ja * nf + jb];
        return (v00 * (1.0 - tA) + v10 * tA) * (1.0 - tB) +
               (v01 * (1.0 - tA) + v11 * tA) * tB;
    }
};

}  // namespace holos
}  // namespace ivanna
