// test_double_precision.cpp
// ============================================================================
// 0029 — Núcleo en doble precisión (acumulación float64)
// ----------------------------------------------------------------------------
// La cadena IVANNA es float32 en las acumulaciones críticas, mientras SHARC
// (ADSP-21569) trabaja 32/40/64 bits y Helix/Brax usa doble SHARC 64 bits.
// Esta propuesta pasa la ACUMULACIÓN interna a float64 en dos puntos,
// manteniendo la salida en float (PCM_FLOAT):
//  (1) FIR ASRC 319 taps (ivanna_route_a_asrc.cpp): acumulación double.
//  (2) FDL del RirConvolver (32 particiones): acumulación double.
// Con fallback explícito a float32 (flag runtime, default double) si el
// coste no cabe en presupuesto.
//
// Honestidad: el THD+N del ASRC hoy es -86/-84/-83 dB, dominado por el
// diseño del FIR/ripple y la Psi, NO por el error de acumulación float32
// (~-120 dB). Pasar a float64 NO mejora el THD+N de forma audible; ELIMINA
// el techo de precisión. El criterio es NO EMPEORAR (THD+N igual o mejor,
// coste dentro de presupuesto) y respuesta idéntica dentro de tolerancia.
// Los tests usan tolerancia, no memcmp (los últimos bits cambian).
//
// Tests:
//  1. AsrcThdNUnchanged — THD+N double <= float32 + 1 dB (no empeora).
//  2. AsrcCostWithinBudget — coste double < 2x float32 y dentro del
//     presupuesto absoluto por bloque.
//  3. RirFdlTolerance — respuesta FDL double vs float32 idéntica dentro
//     de tolerancia (no memcmp).
//  4. FiniteAndBounded — salida finita y acotada en ambas precisiones.
// ============================================================================
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "../ivanna_route_a_asrc.h"
#include "../spatial/RirConvolver.hpp"

namespace {

using ivanna::routea::RouteAAsrc;
using Ivanna::RirConvolver;

constexpr int kSr = 48000;
constexpr double kPi = 3.14159265358979323846;

// Ajuste de seno por mínimos cuadrados (A*sin + B*cos) y THD+N
// (mismo patrón que ivanna_route_a_asrc_test.cpp).
double ThdnDb(const float* x, int n, double freqHz, int skip) {
    double ss = 0.0, sc = 0.0, scc = 0.0, sxS = 0.0, sxC = 0.0;
    int m = 0;
    for (int i = skip; i < n - skip; ++i) {
        const double ph = 2.0 * kPi * freqHz * i / kSr;
        const double s = std::sin(ph), c = std::cos(ph);
        const double v = x[i];
        ss += s * s; sc += c * c; scc += s * c;
        sxS += v * s; sxC += v * c;
        ++m;
    }
    const double det = ss * sc - scc * scc;
    if (std::fabs(det) < 1e-12 || m == 0) return 0.0;
    const double A = (sxS * sc - sxC * scc) / det;
    const double B = (sxC * ss - sxS * scc) / det;
    double res2 = 0.0, sig2 = 0.0;
    for (int i = skip; i < n - skip; ++i) {
        const double ph = 2.0 * kPi * freqHz * i / kSr;
        const double fit = A * std::sin(ph) + B * std::cos(ph);
        const double r = x[i] - fit;
        res2 += r * r;
        sig2 += fit * fit;
    }
    if (sig2 <= 0.0) return 0.0;
    return 10.0 * std::log10(res2 / sig2);
}

std::vector<float> SineStereo(int frames, double freqHz, float amp = 0.5f) {
    std::vector<float> in((size_t)frames * 2);
    for (int i = 0; i < frames; ++i) {
        // La fase DEBE ser double: en float, a ph ~12566 rad (2 s a 1 kHz)
        // la resolución es ~0.001 rad y el propio tono de prueba tiene
        // distorsión ~-60 dB (eso medía -74 dB y no era el ASRC).
        const double ph = 2.0 * kPi * freqHz * i / kSr;
        in[(size_t)i * 2] = amp * (float)std::sin(ph);      // L: seno
        in[(size_t)i * 2 + 1] = amp * (float)std::cos(ph);  // R: coseno
    }
    return in;
}

std::vector<float> Noise(int n, uint32_t seed = 12345u, float amp = 0.5f) {
    std::vector<float> x((size_t)n);
    uint32_t s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        x[(size_t)i] = amp * ((float)((int)(s >> 9) & 0x7FFFFF) / 4194304.0f - 1.0f);
    }
    return x;
}

// Procesa `in` por el ASRC en bloques y devuelve toda la salida.
std::vector<float> ProcessAsrc(RouteAAsrc& asrc, const std::vector<float>& in,
                               int inFrames, int blockFrames = 2048) {
    std::vector<float> out;
    out.reserve((size_t)inFrames * 2 + 4096);
    std::vector<float> scratch((size_t)blockFrames * 2 + 4096);
    int done = 0;
    while (done < inFrames) {
        int n = inFrames - done;
        if (n > blockFrames) n = blockFrames;
        const int cap = n * 1002 / 1000 + 64;
        const int produced = asrc.process(in.data() + (size_t)done * 2, n, scratch.data(), cap);
        out.insert(out.end(), scratch.begin(), scratch.begin() + (size_t)produced * 2);
        done += n;
    }
    return out;
}

double MeasureAsrcThdn(bool useDouble) {
    RouteAAsrc asrc;
    asrc.setClockCorrectionPpm(0.0);
    asrc.setPsiActive(false);
    asrc.setDoubleAccum(useDouble);
    const int inFrames = 2 * kSr;
    const auto in = SineStereo(inFrames, 1000.0);
    const auto out = ProcessAsrc(asrc, in, inFrames);
    const int outFrames = (int)out.size() / 2;
    std::vector<float> chL(outFrames), chR(outFrames);
    for (int i = 0; i < outFrames; ++i) {
        chL[(size_t)i] = out[(size_t)i * 2];
        chR[(size_t)i] = out[(size_t)i * 2 + 1];
    }
    const double tL = ThdnDb(chL.data(), outFrames, 1000.0, 2000);
    const double tR = ThdnDb(chR.data(), outFrames, 1000.0, 2000);
    return std::max(tL, tR);  // el peor canal
}

// 1. THD+N: double no empeora respecto a float32 (y de hecho mejora:
// medido en host, ratio 1.0, Ψ off: double -151.7 dB vs float -137.1 dB).
TEST(DoublePrecision, AsrcThdNUnchanged) {
    const double thdDouble = MeasureAsrcThdn(true);
    const double thdFloat = MeasureAsrcThdn(false);
    EXPECT_LT(thdDouble, -120.0) << "THD+N double peor que -120 dB (regresión grave)";
    EXPECT_LE(thdDouble, thdFloat + 1.0)
        << "La acumulación double EMPEORA el THD+N (double " << thdDouble
        << " dB vs float " << thdFloat << " dB)";
}

// 2. Coste: double < 2x float32 y dentro del presupuesto absoluto.
// Medida con MEDIANA de 3 rondas (2026-11-16, fix flaky en CI): una sola
// medida es ruidosa bajo ASan+UBSan y carga paralela (ctest -j2) — el
// test fallaba en CI (ratio > 2.0 por un pico) aunque pasa en local.
// Los UMBRALES no cambian (ratio < 2.0, absoluto < 1 s): solo la medida
// es robusta. No es debilitamiento del gate.
TEST(DoublePrecision, AsrcCostWithinBudget) {
    const int inFrames = 4 * kSr;
    const auto in = SineStereo(inFrames, 1000.0);
    auto costUs = [&](bool useDouble) {
        std::vector<double> samples;
        for (int round = 0; round < 3; ++round) {
            RouteAAsrc asrc;
            asrc.setClockCorrectionPpm(0.0);
            asrc.setPsiActive(false);
            asrc.setDoubleAccum(useDouble);
            // Warm-up.
            ProcessAsrc(asrc, in, kSr);
            asrc.reset();
            asrc.setClockCorrectionPpm(0.0);
            asrc.setPsiActive(false);
            asrc.setDoubleAccum(useDouble);
            const auto t0 = std::chrono::steady_clock::now();
            ProcessAsrc(asrc, in, inFrames);
            const auto t1 = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        std::sort(samples.begin(), samples.end());
        return samples[1];  // mediana de 3
    };
    const double usDouble = costUs(true);
    const double usFloat = costUs(false);
    const double ratio = usDouble / std::max(usFloat, 1e-9);
    EXPECT_LT(ratio, 2.0) << "La acumulación double cuesta más del doble que float32";
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__)
    // Presupuesto absoluto: 4 s de audio a 48 kHz deben procesarse en
    // menos de 4 s de tiempo real (el ASRC no puede ser más lento que el
    // audio); con margen, menos de 1 s para 4 s de audio (4x realtime).
    // SOLO en build sin sanitizadores: bajo ASan+UBSan el ASRC double
    // mide ~1.25 s aunque la propiedad real (ratio < 2.0) SÍ se cumple
    // — la CI 459 lo demostró (falla solo este tope, no el ratio).
    // El tope absoluto es un presupuesto de release, no una propiedad.
    EXPECT_LT(usDouble, 1.0e6) << "ASRC double demasiado lento (>1 s para 4 s de audio)";
#endif
}

// 3. FDL RIR: double vs float32, respuesta idéntica dentro de tolerancia.
TEST(DoublePrecision, RirFdlTolerance) {
    // IR sintética: espiga en 0 + ruido con decaimiento (T60 0.34 s).
    const int kIrLen = 12000;
    std::vector<float> irL(kIrLen, 0.0f), irR(kIrLen, 0.0f);
    irL[0] = 1.0f; irR[0] = 1.0f;
    uint32_t s = 999u;
    for (int i = 1; i < kIrLen; ++i) {
        s = s * 1664525u + 1013904223u;
        const float env = std::pow(10.0f, -3.0f * ((float)i / (float)kSr) / 0.34f);
        const float v = 0.5f * env * ((float)((int)(s >> 9) & 0x7FFFFF) / 4194304.0f - 1.0f);
        irL[(size_t)i] = v; irR[(size_t)i] = v;
    }
    auto run = [&](bool useDouble) {
        RirConvolver rc;
        rc.setSampleRate(kSr);
        rc.load(irL.data(), irR.data(), kIrLen);
        rc.setWetDry(1.0f);
        rc.setDoubleAccum(useDouble);
        const auto nL = Noise(8192, 42u), nR = Noise(8192, 43u);
        std::vector<float> out;
        for (int b = 0; b < 16; ++b) {
            std::vector<float> L(nL.begin() + (size_t)b * 512, nL.begin() + (size_t)(b + 1) * 512);
            std::vector<float> R(nR.begin() + (size_t)b * 512, nR.begin() + (size_t)(b + 1) * 512);
            rc.process(L.data(), R.data(), 512);
            out.insert(out.end(), L.begin(), L.end());
            out.insert(out.end(), R.begin(), R.end());
        }
        return out;
    };
    const auto oDouble = run(true);
    const auto oFloat = run(false);
    ASSERT_EQ(oDouble.size(), oFloat.size());
    // Tolerancia, no memcmp: los últimos bits cambian con la precisión.
    double maxDiff = 0.0, rmsD = 0.0, rmsS = 0.0;
    for (size_t i = 0; i < oDouble.size(); ++i) {
        const double d = (double)oDouble[i] - (double)oFloat[i];
        maxDiff = std::max(maxDiff, std::fabs(d));
        rmsD += d * d;
        rmsS += (double)oDouble[i] * (double)oDouble[i];
    }
    rmsD = std::sqrt(rmsD / (double)oDouble.size());
    rmsS = std::sqrt(rmsS / (double)oDouble.size());
    const double relDb = 20.0 * std::log10(rmsD / std::max(rmsS, 1e-12));
    EXPECT_LT(maxDiff, 1e-4) << "La FDL double difiere demasiado de float32 (max diff)";
    EXPECT_LT(relDb, -90.0) << "La diferencia double/float32 está por encima de -90 dB (relativo)";
    EXPECT_GT(rmsS, 1e-4) << "La salida del RIR es casi silencio (el test no mide nada)";
}

// 4. Finita y acotada en ambas precisiones (ASRC + RIR).
TEST(DoublePrecision, FiniteAndBounded) {
    // ASRC con ruido caliente a +100 ppm.
    for (bool useDouble : {true, false}) {
        RouteAAsrc asrc;
        asrc.setClockCorrectionPpm(100.0);
        asrc.setPsiActive(false);
        asrc.setDoubleAccum(useDouble);
        const auto in = SineStereo(2 * kSr, 1000.0);
        const auto out = ProcessAsrc(asrc, in, 2 * kSr);
        float mx = 0.0f;
        for (float v : out) {
            ASSERT_TRUE(std::isfinite(v));
            mx = std::max(mx, std::fabs(v));
        }
        EXPECT_LT(mx, 2.0f) << "ASRC no acotado (double=" << useDouble << ")";
    }
    // RIR con ruido caliente.
    for (bool useDouble : {true, false}) {
        RirConvolver rc;
        rc.setSampleRate(kSr);
        std::vector<float> irL(12000, 0.0f), irR(12000, 0.0f);
        irL[0] = 1.0f; irR[0] = 1.0f;
        uint32_t s = 7u;
        for (int i = 1; i < 12000; ++i) {
            s = s * 1664525u + 1013904223u;
            irL[(size_t)i] = 0.3f * ((float)((int)(s >> 9) & 0x7FFFFF) / 4194304.0f - 1.0f);
            irR[(size_t)i] = irL[(size_t)i];
        }
        rc.load(irL.data(), irR.data(), 12000);
        rc.setWetDry(1.0f);
        rc.setDoubleAccum(useDouble);
        std::vector<float> L(512, 0.9f), R(512, -0.9f);
        for (int b = 0; b < 20; ++b) {
            rc.process(L.data(), R.data(), 512);
            for (int i = 0; i < 512; ++i) {
                ASSERT_TRUE(std::isfinite(L[(size_t)i]) && std::isfinite(R[(size_t)i]));
                EXPECT_LT(std::fabs(L[(size_t)i]), 5.0f);
            }
        }
    }
}

}  // namespace
