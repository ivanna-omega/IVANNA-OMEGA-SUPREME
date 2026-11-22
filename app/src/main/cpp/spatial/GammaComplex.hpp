// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// GammaComplex — Γ(f,t) COMPLEJO de fase mixta (Ley 1).
//
// HolosGainField.hpp define Γ_b como producto de magnitudes REALES
// (fase cero por diseño: el FIR EvoEQ se reemplazó por magnitud pura
// para evitar latencia y pre-ringing). Este header añade la LEY DE FASE
// que le falta a ese campo, SIN tocar las leyes de magnitud:
//
//   Γ_b = |Γ_b| · e^{j·φ_b}
//
// donde |Γ_b| = gammaLin_[b] (las leyes existentes) y φ_b es la fase de
// fase mixta:
//
// - Fase MÍNIMA (corrección de resonancias): contraparte de Bode de la
//   magnitud. Para un sistema de fase mínima, la fase es (aprox.) la
//   pendiente de la log-magnitud:
//       φ_min ≈ +(π/2) · d(ln|Γ|) / d(ln f)
//   En dB/octava: φ_min[rad] ≈ +0.2609 · pendiente[dB/oct].
//   Se calcula por diferencias centrales sobre los centros ERB. Magnitud
//   plana (pendiente 0) → φ = 0. Un corte de resonancia (pendiente
//   fuerte) → fase que compacta el impulso. Fase mínima = causal: TODA
//   la energía queda DESPUÉS de t=0, cero pre-ringing por construcción.
// - Fase LINEAL (forma tonal ancha): retardo de grupo CONSTANTE τ0:
//       φ_lin(f) = −2π·f·τ0
//   τ0 = 0 por defecto (la forma tonal queda en fase cero, como hoy).
//   Un retardo de grupo constante no ensancha el impulso: lo desplaza
//   entero, sin smearing.
//
// REGLA MIXTA: φ_b = φ_min[b] − 2π·f_b·τ0. La fase mínima se aplica a
// TODA la magnitud: es su contraparte causal. Una magnitud SUAVE tiene
// fase pequeña y suave (como un EQ analógico real); una resonancia aguda
// tiene fase concentrada en sus flancos. Una pendiente CONSTANTE (p. ej.
// +6 dB/oct) SÍ tiene fase de Bode (+90°, como un diferenciador jω):
// ponderarla por curvatura (que es 0 en una pendiente constante) sería
// físicamente incorrecto. La parte lineal es el retardo constante τ0.
//
// Funciones puras, sin estado, sin alloc: testeables sin audio.
// Con |Γ|=1 y φ=0 la multiplicación compleja es BIT-IDÉNTICA a la real
// (x·1 − y·0 = x; y·1 + x·0 = y), así que el camino neutro no cambia.
// DC/Nyquist: el llamador DEBE usar solo la parte real (esos bins son
// reales; rotarlos rompería la simetría hermitiana).
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>
#include <vector>

namespace ivanna {
namespace holos {

// Fase mínima (rad) desde la pendiente local de la magnitud en dB/octava.
// Relación de Bode: φ ≈ +(π/2)·d(ln|Γ|)/d(ln f) ≈ +0.2609·pendiente.
// Signo: magnitud que SUBE con f (pendiente > 0, como un paso-alto) →
// fase POSITIVA (adelanto; un diferenciador jω: +90° con +6 dB/oct).
// Magnitud que BAJA → fase negativa (retraso, como un integrador 1/jω).
inline double minPhaseFromSlopeDbPerOct(double slopeDbPerOct) noexcept {
    if (!std::isfinite(slopeDbPerOct)) return 0.0;
    constexpr double kBode = 0.260899;  // (π/2)·ln(10)/(20·ln(2))
    const double phi = kBode * slopeDbPerOct;
    return std::clamp(phi, -1.2, 1.2);  // ~±69°: sin sobre-rotación
}

// Peso de corrección aguda [0,1] desde la curvatura local (dB/oct²).
// Reservado para un refinamiento futuro (p. ej. ponderar la fase mínima
// solo donde la magnitud corrige una resonancia aguda). Hoy no se usa:
// la fase mínima se aplica a toda la magnitud (ver complexGammaBands).
inline double correctionWeightFromCurvature(double curvatureDbPerOct2) noexcept {
    if (!std::isfinite(curvatureDbPerOct2)) return 0.0;
    return std::clamp(std::fabs(curvatureDbPerOct2) / 12.0, 0.0, 1.0);
}

// Fase de Bode (fase mínima + retardo lineal) de una magnitud.
// magLin: magnitud lineal por banda; centerHz: centros en Hz (creciente).
// groupDelaySec: τ0 del término lineal (0 = fase cero tonal).
// Salida: phi[b] en radianes. Magnitud plana → phi = 0 exacto.
// nBands < 3 → phi = 0 en todas.
inline void phaseFromMagnitude(const double* magLin, const double* centerHz,
                               int nBands, double sampleRate,
                               double groupDelaySec, double* phi) noexcept {
    if (!magLin || !centerHz || !phi || nBands <= 0) return;
    const double tau = std::isfinite(groupDelaySec) ? groupDelaySec : 0.0;
    const double sr = (std::isfinite(sampleRate) && sampleRate >= 8000.0)
                         ? sampleRate : 48000.0;
    // Log-magnitud en dB por banda (con piso para evitar log(0)).
    // Se usa un scratch en stack (kMaxBands = 24 en HolosOperator).
    double magDb[64];
    double slope[64];
    const int n = std::min(nBands, 64);
    for (int b = 0; b < n; ++b) {
        const double m = std::isfinite(magLin[b]) ? std::max(magLin[b], 1e-6) : 1.0;
        magDb[b] = 20.0 * std::log10(m);
    }
    // Pendiente local dB/octava por diferencias centrales sobre log2(f).
    for (int b = 0; b < n; ++b) {
        const int lo = (b > 0) ? b - 1 : b;
        const int hi = (b < n - 1) ? b + 1 : b;
        const double fLo = std::max(centerHz[lo], 1.0);
        const double fHi = std::max(centerHz[hi], fLo * 1.0001);
        const double octaves = std::log2(fHi / fLo);
        slope[b] = (octaves > 1e-6) ? (magDb[hi] - magDb[lo]) / octaves : 0.0;
    }
    for (int b = 0; b < n; ++b) {
        // Fase mínima de Bode desde la pendiente local (sin peso por
        // curvatura: una pendiente constante SÍ tiene fase).
        const double phiMin = (n >= 3) ? minPhaseFromSlopeDbPerOct(slope[b]) : 0.0;
        const double f = std::clamp(centerHz[b], 0.0, sr * 0.5);
        phi[b] = phiMin - 2.0 * 3.14159265358979323846 * f * tau;
    }
    for (int b = n; b < nBands; ++b) phi[b] = 0.0;
}

// Aplica una fase a una magnitud: re = mag·cos φ, im = mag·sin φ.
// La magnitud y la fase pueden venir de fuentes DISTINTAS: la fase es
// la del FILTRO (parte estática), la magnitud es la TOTAL (estática ×
// dinámica × Ω). La ganancia dinámica y la Ω son moduladores (ganancia
// que varía en el tiempo), no filtros: su "pendiente" entre bandas NO
// tiene contraparte de fase. Darles fase sería tratar un compresor
// como si fuera un filtro, y eso degrada el THD+N.
inline void complexGammaBandsFromPhase(const double* magLin, const double* phi,
                                       int nBands, double* re,
                                       double* im) noexcept {
    if (!magLin || !phi || !re || !im || nBands <= 0) return;
    for (int b = 0; b < nBands; ++b) {
        const double m = std::isfinite(magLin[b]) ? std::max(magLin[b], 0.0) : 1.0;
        const double p = std::isfinite(phi[b]) ? phi[b] : 0.0;
        re[b] = m * std::cos(p);
        im[b] = m * std::sin(p);
    }
}

// Ganancia compleja por banda. magLin: |Γ_b| lineal por banda (nBands).
// centerHz: centro de cada banda en Hz (estrictamente creciente).
// groupDelaySec: τ0 del término lineal (0 = fase cero tonal, como hoy).
// Salida: re[b], im[b] (mag·cos φ, mag·sin φ). Con magnitud plana
// (todas 1.0) → (1, 0) exacto. nBands < 3 → fase 0 en todas.
// NOTA: esta forma usa la MISMA magnitud para fase y salida (correcto
// cuando la magnitud es la de un filtro). HolosOperator usa la forma
// separada (fase del estático, magnitud total) — ver arriba.
inline void complexGammaBands(const double* magLin, const double* centerHz,
                              int nBands, double sampleRate,
                              double groupDelaySec,
                              double* re, double* im) noexcept {
    if (!magLin || !centerHz || !re || !im || nBands <= 0) return;
    double phi[64];
    const int n = std::min(nBands, 64);
    phaseFromMagnitude(magLin, centerHz, n, sampleRate, groupDelaySec, phi);
    complexGammaBandsFromPhase(magLin, phi, n, re, im);
    // Bandas más allá del scratch (nunca en la práctica: kMaxBands=24).
    for (int b = n; b < nBands; ++b) {
        const double m = std::isfinite(magLin[b]) ? std::max(magLin[b], 0.0) : 1.0;
        re[b] = m;
        im[b] = 0.0;
    }
}

// ── Envoltorios float (F2/486, compat) ────────────────────────────────────
// Misma matemática en double, con conversión por vectores locales. Los
// usan los tests (test_ley1_tiempo_fase) y llamadores legacy; el camino
// vivo (HolosOperator) usa las versiones double directamente.
inline void phaseFromMagnitude(const float* magLin, const float* centerHz,
                               int nBands, float sampleRate,
                               float groupDelaySec, float* phi) noexcept {
    if (!magLin || !centerHz || !phi || nBands <= 0) return;
    std::vector<double> mag(magLin, magLin + nBands);
    std::vector<double> fc(centerHz, centerHz + nBands);
    std::vector<double> ph(nBands, 0.0);
    phaseFromMagnitude(mag.data(), fc.data(), nBands, static_cast<double>(sampleRate),
                       static_cast<double>(groupDelaySec), ph.data());
    for (int b = 0; b < nBands; ++b) phi[b] = static_cast<float>(ph[b]);
}

inline void complexGammaBandsFromPhase(const float* magLin, const float* phi,
                                       int nBands, float* re,
                                       float* im) noexcept {
    if (!magLin || !phi || !re || !im || nBands <= 0) return;
    std::vector<double> mag(magLin, magLin + nBands);
    std::vector<double> ph(phi, phi + nBands);
    std::vector<double> r(nBands, 0.0), im2(nBands, 0.0);
    complexGammaBandsFromPhase(mag.data(), ph.data(), nBands, r.data(), im2.data());
    for (int b = 0; b < nBands; ++b) {
        re[b] = static_cast<float>(r[b]);
        im[b] = static_cast<float>(im2[b]);
    }
}

inline void complexGammaBands(const float* magLin, const float* centerHz,
                              int nBands, float sampleRate,
                              float groupDelaySec,
                              float* re, float* im) noexcept {
    if (!magLin || !centerHz || !re || !im || nBands <= 0) return;
    std::vector<double> mag(magLin, magLin + nBands);
    std::vector<double> fc(centerHz, centerHz + nBands);
    std::vector<double> r(nBands, 0.0), im2(nBands, 0.0);
    complexGammaBands(mag.data(), fc.data(), nBands, static_cast<double>(sampleRate),
                      static_cast<double>(groupDelaySec), r.data(), im2.data());
    for (int b = 0; b < nBands; ++b) {
        re[b] = static_cast<float>(r[b]);
        im[b] = static_cast<float>(im2[b]);
    }
}

}  // namespace holos
}  // namespace ivanna
