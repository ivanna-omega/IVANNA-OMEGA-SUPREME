// test_route_a_npe_live_params.cpp — Regresión 0024 (elevación magistral del DSP).
//
// El NPE (NpeStereoCore) es el modelo coclear que corre en la cadena viva
// de Ruta A vía routeAApplyNpe() (ivanna_omega_jni.cpp). La app publica sus
// parámetros desde AdaptiveControlsPrefs (la fuente de verdad en vivo):
//   harmonic 0.60, lateral 0.50, ohc 0.40, master +2.5 dB
// (NO los defaults nativos 0.2/0.2/0.3/0.0). Este test verifica el NPE a
// ESOS parámetros vivos — los que realmente suenan — y no solo a los
// defaults (eso ya lo cubre test_npe_core.cpp).
//
// Exigencias del revisor del consejo (ronda 3, 0024):
//   - Ruta A con NPE no es bit-exacto respecto a bypass (efecto medible).
//   - Sin clic al conmutar.
//   - Salida finita y acotada, coste por bloque aceptable.
//   - Al menos un check de THD+N / energía / nulidad.
// El NPE es un procesador de color armónico por diseño (añade armónicos):
// el THD+N sube respecto a bypass y eso es ESPERADO, no un defecto. El
// límite es que no sea catastrófico y que la fundamental no colapse.
// La respuesta en frecuencia no debe desviarse más de lo acotado.
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <vector>
#include "neuromorphic/NpeStereoCore.hpp"
#include "dsp/ThdMeter.hpp"

using ivanna::neuromorphic::NpeStereoCore;
using ivanna::measure::measureThdPlusNoise;
using ivanna::measure::fillTestTone;

namespace {
constexpr float kSr = 48000.f;
// Parámetros VIVOS de AdaptiveControlsPrefs (fuente de verdad en vivo).
constexpr float kLiveHarm = 0.60f;
constexpr float kLiveLat = 0.50f;
constexpr float kLiveOhc = 0.40f;
constexpr float kLiveMasterDb = 2.5f;

std::vector<float> music(size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / kSr;
        const float env = 0.5f + 0.5f * std::sin(2.f * 3.14159265f * 2.f * t);
        x[i] = 0.25f * env * std::sin(2.f * 3.14159265f * 110.f * t)
             + 0.12f * std::sin(2.f * 3.14159265f * 1500.f * t)
             + 0.05f * std::sin(2.f * 3.14159265f * 6000.f * t);
    }
    return x;
}
void run(NpeStereoCore& c, std::vector<float>& L, std::vector<float>& R) {
    for (size_t k = 0; k + 1024 <= L.size(); k += 1024)
        c.process(L.data() + k, R.data() + k, 1024);
}
// RMS de una banda (tono puro a freqHz) tras procesar.
// Núcleo FRESCO por banda: el NPE tiene estado (banco de envolventes,
// neuronas LIF, AGC) y compartirlo entre bandas contamina la medida.
double bandRmsFresh(double freqHz, float amp, bool enabled) {
    NpeStereoCore c; c.prepare(kSr);
    c.setParams(enabled, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    std::vector<float> L(4800), R(4800);
    fillTestTone(L.data(), L.size(), 48000.0, freqHz, amp);
    R = L;
    c.process(L.data(), R.data(), 4800);
    double s = 0.0;
    for (size_t i = 2400; i < L.size(); ++i) s += double(L[i]) * double(L[i]);
    return std::sqrt(s / double(L.size() - 2400));
}
}  // namespace

// 0024-1: a parámetros vivos el NPE cambia la salida de forma medible.
TEST(RouteANpeLiveParams, LiveParamsNotBitExact) {
    NpeStereoCore c; c.prepare(kSr);
    auto L = music(48000), R = L; const auto L0 = L;
    c.setParams(true, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    run(c, L, R);
    EXPECT_NE(L, L0);
    EXPECT_TRUE(c.isActive());
    double diff = 0, ref = 0;
    for (size_t i = L.size() / 2; i < L.size(); ++i) {
        diff += double(L[i] - L0[i]) * double(L[i] - L0[i]);
        ref += double(L0[i]) * double(L0[i]);
    }
    EXPECT_GT(10.0 * std::log10(diff / ref), -30.0);  // residuo audible
}

// 0024-2: a parámetros vivos, apagado sigue siendo bit-exacto (null test).
TEST(RouteANpeLiveParams, LiveParamsOffIsBitExact) {
    NpeStereoCore c; c.prepare(kSr);
    auto L = music(48000), R = L; const auto L0 = L, R0 = R;
    c.setParams(false, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    run(c, L, R);
    EXPECT_EQ(L, L0); EXPECT_EQ(R, R0);
    EXPECT_FALSE(c.isActive());
}

// 0024-3: THD+N a parámetros vivos sobre seno 1 kHz.
// El NPE es un procesador de COLOR armónico por diseño (añade armónicos
// vía LIF/NHO): el THD+N sobre un seno puro sube respecto al bypass
// (~-140 dB) y eso es ESPERADO, no un defecto. Por eso el límite NO es
// un THD+N bajo, sino que no sea catastrófico y que la fundamental no
// colapse. Valores medidos en host (2026-11-16, build0024, herramienta
// tools/measure/npe_live_vs_native.cpp): VIVO THD+N -10.35 dB,
// fundamental -14.09 dBFS (entrada 0.5 = -6.02 dBFS), sin clipping.
// NATIVO (0.2/0.2/0.3/0.0) da THD+N -6.99 dB y fundamental -16.32 dBFS:
// los parámetros vivos son MEJORES que los nativos en THD+N y en
// preservación de la fundamental (el master +2.5 dB compensa). Ver
// REPORT 0024 para la tabla completa.
TEST(RouteANpeLiveParams, LiveParamsThdPlusNoise) {
    NpeStereoCore c; c.prepare(kSr);
    std::vector<float> L(48000), R(48000);
    fillTestTone(L.data(), L.size(), 48000.0, 1000.0, 0.5f);
    R = L;
    c.setParams(true, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    run(c, L, R);
    // Medir sobre la segunda mitad (estabilizado).
    const auto r = measureThdPlusNoise(L.data() + 24000, 24000, 48000.0, 1000.0);
    ASSERT_TRUE(r.valid);
    EXPECT_GT(r.fundamentalDbfs, -20.0);   // la fundamental no colapsa
    EXPECT_LT(r.thdPlusNoiseDb, -5.0);     // no catastrófico (residuo < 56%)
    EXPECT_EQ(r.clippedSamples, 0u);       // sin clipping a 0.5 de amplitud
    // El THD+N medido se registra en el REPORT 0024 (coloración del NPE).
    std::printf("[NPE-vivo] THD+N=%.2f dB fund=%.2f dBFS\n",
                r.thdPlusNoiseDb, r.fundamentalDbfs);
}

// 0024-4: respuesta en frecuencia a parámetros vivos.
// El NPE no debe desviar el balance tonal más de 6 dB en ninguna banda
// (grave 110 Hz / medio 1 kHz / agudo 8 kHz) respecto a bypass.
// Valores medidos en host (núcleo fresco por banda): VIVO -0.58 dB
// (110 Hz), -2.24 dB (1 kHz), -2.08 dB (8 kHz) — atenuación suave y
// pareja, sin desplome de agudos. Ver REPORT 0024.
TEST(RouteANpeLiveParams, LiveParamsFrequencyResponse) {
    const double freqs[3] = {110.0, 1000.0, 8000.0};
    for (double f : freqs) {
        const double on = bandRmsFresh(f, 0.5f, true);
        const double off = bandRmsFresh(f, 0.5f, false);
        const double ratioDb = 20.0 * std::log10((on + 1e-9) / (off + 1e-9));
        EXPECT_GT(ratioDb, -6.0) << "banda " << f << " Hz atenuada de más";
        EXPECT_LT(ratioDb, 6.0) << "banda " << f << " Hz realzada de más";
    }
}

// 0024-5: salida finita y acotada con entrada caliente a parámetros vivos.
TEST(RouteANpeLiveParams, LiveParamsFiniteAndBoundedWithHotInput) {
    NpeStereoCore c; c.prepare(kSr);
    std::vector<float> L(48000), R(48000);
    for (size_t i = 0; i < L.size(); ++i) {
        L[i] = (i % 97 < 48) ? 1.5f : -1.5f; R[i] = -L[i];
    }
    c.setParams(true, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    run(c, L, R);
    for (size_t i = 0; i < L.size(); ++i) {
        ASSERT_TRUE(std::isfinite(L[i]) && std::isfinite(R[i]));
    }
    float pk = 0;
    for (size_t i = 40000; i < L.size(); ++i) pk = std::max(pk, std::fabs(L[i]));
    EXPECT_LE(pk, 2.0f);  // acotado (master +2.5 dB sobre 1.5 de entrada)
}

// 0024-6: encendido sin clic a parámetros vivos (fundido de 20 ms).
TEST(RouteANpeLiveParams, LiveParamsSwitchingOnIsClickFree) {
    NpeStereoCore c; c.prepare(kSr);
    auto L = music(48000), R = L; const auto L0 = L;
    c.setParams(false, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    c.process(L.data(), R.data(), 1024);
    c.setParams(true, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    c.process(L.data() + 1024, R.data() + 1024, 1024);
    float maxJump = 0;
    for (int i = 1024; i < 2048; ++i)
        maxJump = std::max(maxJump, std::fabs((L[i] - L0[i]) - (L[i - 1] - L0[i - 1])));
    EXPECT_LT(maxJump, 0.02f);
}

// 0024-7: coste por bloque a parámetros vivos dentro de presupuesto.
// Un bloque de 1024 muestras = 21.3 ms de audio a 48 kHz; el NPE debe
// procesarlo en una fracción pequeña de ese tiempo (< 2 ms, >10x margen).
TEST(RouteANpeLiveParams, LiveParamsCostWithinBudget) {
    NpeStereoCore c; c.prepare(kSr);
    std::vector<float> L(1024), R(1024);
    for (size_t i = 0; i < L.size(); ++i) {
        L[i] = 0.3f * std::sin(2.f * 3.14159265f * 440.f * float(i) / kSr);
        R[i] = L[i];
    }
    c.setParams(true, kLiveHarm, kLiveLat, kLiveOhc, kLiveMasterDb);
    // Calentamiento.
    for (int i = 0; i < 20; ++i) c.process(L.data(), R.data(), 1024);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kReps = 200;
    for (int i = 0; i < kReps; ++i) c.process(L.data(), R.data(), 1024);
    const auto t1 = std::chrono::steady_clock::now();
    const double usPerBlock =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() /
        double(kReps);
    EXPECT_LT(usPerBlock, 2000.0);  // < 2 ms por bloque de 1024 muestras
}
