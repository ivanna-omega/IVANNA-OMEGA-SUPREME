// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// ListenerGeometry — geometría del oyente → ITD (Ley 1).
//
// El SupremeMsoFarrowArbitrator alineaba L/R solo con la parte FRACCIONAL
// (±1 muestra: fracL/fracR saturan) y su ITD venía de un valor crudo en ns
// (JNI/snapshot), NUNCA de la geometría. Este header es la fuente
// geométrica que faltaba, con la misma convención que CrosstalkCanceller
// (span/dist/offset, c = 343 m/s):
//
//   Altavoces en x = ∓span/2, z = 0. Oyente en x = offset, z = distance.
//   dL = √((offset + span/2)² + dist²)   (izquierdo)
//   dR = √((offset − span/2)² + dist²)   (derecho)
//   ITD = (dL − dR) / c   (segundos; >0 → L más lejos → L llega tarde)
//
// El ITD se parte en RETARDO ENTERO + FRACCIÓN:
//   itdSamples = ITD · sr
//   intTotal   = round(itdSamples)          (muestras enteras, con signo)
//   frac       = itdSamples − intTotal      (∈ [−0.5, 0.5] muestras)
//   Si intTotal ≥ 0 → se retrasa R intTotal muestras (R llega antes);
//   si intTotal < 0 → se retrasa L −intTotal muestras.
//   La fracción residual va al Farrow como ns (su rango válido ±1 muestra).
// El retardo entero máximo es ±80 muestras (≈ ±1.67 ms @48 kHz ≈ ±57 cm
// de diferencia de camino: más que cualquier escucha real frente a los
// Genezi). Funciones puras, sin estado, sin alloc.
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>

namespace ivanna {
namespace supreme {

// Geometría del oyente frente al par de altavoces (misma convención que
// CrosstalkCanceller::Geometry). offset > 0 = oyente a la derecha.
struct ListenerGeometry {
    float spanMeters = 0.90f;      // separación entre altavoces
    float distanceMeters = 0.85f;  // distancia al eje del par
    float listenerOffsetM = 0.0f;  // corrimiento lateral del oyente (m)
    float speedOfSound = 343.0f;   // m/s a 20 °C
    float sampleRate = 48000.0f;   // Hz
};

// Partición del ITD: retardo entero por canal + fracción residual en ns.
struct ItdSplit {
    float itdSeconds = 0.0f;   // (dL − dR)/c, con signo
    int intDelayL = 0;         // muestras enteras a retrasar L (≥0)
    int intDelayR = 0;         // muestras enteras a retrasar R (≥0)
    float fracNs = 0.0f;       // fracción residual en nanosegundos (±0.5 muestra)
};

inline constexpr int kMaxIntDelaySamples = 80;

// Geometría → ITD partido. Pura y total: entradas no finitas o fuera de
// rango se acotan a la geometría de referencia (nunca NaN, nunca huérfana).
inline ItdSplit geometryToItd(const ListenerGeometry& g) noexcept {
    ItdSplit out;
    const float span = std::isfinite(g.spanMeters) ? std::clamp(g.spanMeters, 0.15f, 4.0f) : 0.90f;
    const float dist = std::isfinite(g.distanceMeters) ? std::clamp(g.distanceMeters, 0.25f, 5.0f) : 0.85f;
    const float off = std::isfinite(g.listenerOffsetM) ? std::clamp(g.listenerOffsetM, -1.0f, 1.0f) : 0.0f;
    const float c = std::isfinite(g.speedOfSound) ? std::clamp(g.speedOfSound, 300.0f, 380.0f) : 343.0f;
    const float sr = std::isfinite(g.sampleRate) && g.sampleRate >= 8000.0f ? g.sampleRate : 48000.0f;
    const float xl = off + 0.5f * span;  // distancia lateral al altavoz L
    const float xr = off - 0.5f * span;  // distancia lateral al altavoz R
    const float dL = std::sqrt(xl * xl + dist * dist);
    const float dR = std::sqrt(xr * xr + dist * dist);
    const float itd = (dL - dR) / c;
    out.itdSeconds = itd;
    const float itdSamples = std::clamp(itd * sr, (float)-kMaxIntDelaySamples, (float)kMaxIntDelaySamples);
    const int intTotal = (int)std::lround(itdSamples);
    const float fracSamples = itdSamples - (float)intTotal;
    if (intTotal >= 0) {
        out.intDelayR = intTotal;
        out.intDelayL = 0;
    } else {
        out.intDelayL = -intTotal;
        out.intDelayR = 0;
    }
    out.fracNs = fracSamples / sr * 1.0e9f;
    return out;
}

// Un ITD crudo en ns (la API histórica setMsoItdNanoseconds) partido con
// la misma regla: entero + fracción. Así el ITD completo se honra en vez
// de saturar el Farrow en ±1 muestra.
inline ItdSplit nsToItdSplit(float itdNs, float sampleRate) noexcept {
    ItdSplit out;
    const float sr = std::isfinite(sampleRate) && sampleRate >= 8000.0f ? sampleRate : 48000.0f;
    const float ns = std::isfinite(itdNs) ? std::clamp(itdNs, -750000.0f, 750000.0f) : 0.0f;
    out.itdSeconds = ns * 1.0e-9f;
    const float itdSamples = std::clamp(ns * 1.0e-9f * sr, (float)-kMaxIntDelaySamples, (float)kMaxIntDelaySamples);
    const int intTotal = (int)std::lround(itdSamples);
    const float fracSamples = itdSamples - (float)intTotal;
    if (intTotal >= 0) {
        out.intDelayR = intTotal;
        out.intDelayL = 0;
    } else {
        out.intDelayL = -intTotal;
        out.intDelayR = 0;
    }
    out.fracNs = fracSamples / sr * 1.0e9f;
    return out;
}

}  // namespace supreme
}  // namespace ivanna
