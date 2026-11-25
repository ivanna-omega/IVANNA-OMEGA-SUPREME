// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// Conservación de potencia por banda (L1) + Cantidad por señal (L2).
//
// Origen: adaptado de las leyes L1–L2 del material Portal 5D v3 ("TODO",
// IVANNA_PORTAL_5D_TODO__3__3_tren.txt, ivanna_nueva_fisica.hpp). El
// material las presentaba como "leyes de nueva física consolidada"; ese
// estatus es FALSO (veredicto del consejo, Luna 2026-11-23): son
// componentes de control DSP — conservación de energía por banda y
// escalado de la señal física por Cantidad — matemática clásica, no
// física nueva. El nombre del archivo original ("ivanna_nueva_fisica")
// NO se conserva por eso mismo.
//
// L1 — CONSERVACIÓN DE POTENCIA POR BANDA (sonoridad):
//   BS.1770: LUFS = −0.691 + 10·log10 Σ_ch G·z_ch. Si Σ_ch |Y|² = Σ_ch |X|²
//   en cada banda, el LUFS es idéntico para CUALQUIER ponderación ⇒
//   ganancia neta 0.0 dB por construcción (Regla de Oro Matched-LUFS).
//   g_b = sqrt(Ē_in,b / Ē_out,b), Ē = integrador con τ_E.
// L2 — PROFUNDIDAD POR SEÑAL (no por mezcla):
//   La Cantidad escala la SEÑAL FÍSICA (ITD, ILD, estante), no mezcla
//   salidas: itd_eff = a·itd, ild_eff = a·ild, shelf_eff = a·shelf,
//   side = 1 (kSideGain). |H_mid| = 1 para todo a ⇒ sin peine;
//   a=0 es identidad exacta del render.
//   AmountSmoother suaviza la Cantidad por frame (sin zipper).
//
// Uso: OrbeBinaural (spatial/OrbeBinaural.cpp), detrás del flag
// IVANNA_ORBE_POWER_CONSERVATION (ON por defecto, orden Esse 2026-11-25,
// todo activo en la 502; P0 se re-congela con el flag ON).
#include <algorithm>
#include <cmath>

namespace ivanna::nf {

inline constexpr int kMaxBands = 24;

inline float clamp01(float x) noexcept {
    return std::isfinite(x) ? std::min(1.0f, std::max(0.0f, x)) : 0.0f;
}

// ── L2 ───────────────────────────────────────────────────────────────────
// Suavizador de Cantidad por frame (sin zipper al mover el slider).
struct AmountSmoother {
    float v = 0.0f;
    float step(float target, float hopSec, float tauSec = 0.025f) noexcept {
        const float a = 1.0f - std::exp(-hopSec / tauSec);
        v += a * (clamp01(target) - v);
        return v;
    }
};
// L2: S nunca se atenúa (fix del defecto M1: OrbeBinaural perdía −2.05 dB
// con diffuse = 0.5; ver analysis-portal5d/FASE0_M1_MEDICION.md).
inline constexpr float kSideGain = 1.0f;

// ── L1 ───────────────────────────────────────────────────────────────────
struct BandPowerConserver {
    double eIn[kMaxBands]{}, eOut[kMaxBands]{};   // integradores
    double fIn[kMaxBands]{}, fOut[kMaxBands]{};   // acumuladores del frame
    float  gain[kMaxBands]{};
    int    bands = 24;
    float  alpha = 0.1f;
    void prepare(int nBands, float hopSec, float tauSec = 0.030f) noexcept {
        bands = std::min(std::max(nBands, 1), kMaxBands);
        alpha = 1.0f - std::exp(-hopSec / tauSec);
        reset();
    }
    void reset() noexcept {
        for (int b = 0; b < kMaxBands; ++b) { eIn[b] = eOut[b] = 1e-12; fIn[b] = fOut[b] = 0.0; gain[b] = 1.0f; }
    }
    void beginFrame() noexcept { for (int b = 0; b < kMaxBands; ++b) fIn[b] = fOut[b] = 0.0; }
    void addIn (int b, double e) noexcept { fIn [b] += e; }
    void addOut(int b, double e) noexcept { fOut[b] += e; }
    // Suma de potencias de ambos oídos: Σ_ch |Y|² = Σ_ch |X|² (L1).
    void commit() noexcept {
        for (int b = 0; b < bands; ++b) {
            eIn [b] += alpha * (fIn [b] - eIn [b]);
            eOut[b] += alpha * (fOut[b] - eOut[b]);
            const double r = (eOut[b] > 1e-12 && eIn[b] > 1e-12) ? std::sqrt(eIn[b] / eOut[b]) : 1.0;
            gain[b] = static_cast<float>(std::min(2.0, std::max(0.5, r)));  // ±6 dB, guarda
        }
    }
};

// Forma cerrada del defecto M1 (para auditar L1 sin audio): potencia de
// salida/entrada del render legacy con side-gain d, fracción de M μ,
// mezcla a y C = Re(gL e^{jαL} + gR e^{jαR}) (C=2 en el centro).
// legacyPowerRatio(1.0, 0.5, 0.5) = −2.0412 dB (Fase 0: medido −2.05 dB).
inline double legacyPowerRatio(double a, double mu, double d, double C = 2.0) noexcept {
    return (1 - a) * (1 - a) + a * a * (mu + d * d * (1 - mu)) +
           a * (1 - a) * (mu * C + 2.0 * d * (1 - mu));
}

}  // namespace ivanna::nf
