// test_thd_meter.cpp — Tests de host del medidor THD+N (Punto 3 de la crítica
// externa a IVANNA, 2026-11-16; propuesta 0022). El medidor es la
// instrumentación que produce la evidencia física que la crítica exige
// (THD+N, niveles, clipping) — la misma matemática corre en host y en el
// análisis de las grabaciones del teléfono.
//
// Método (ThdMeter.hpp): ajuste por mínimos cuadrados de un seno a la
// fundamental; THD+N = (potencia total − potencia fundamental) / fundamental.
// Buffer con número entero de ciclos (4800 muestras a 48 kHz = 100 ciclos
// de 1 kHz) para medida exacta sin leakage.
//
// Lo que se verifica (números exactos, no aproximados):
// - Seno puro: THD+N < −120 dB (límite de float), fundamental correcta.
// - Seno + 2º armónico a −40 dB: THD+N = −40 dB (±0.5 dB).
// - Seno + 2º armónico a −60 dB: THD+N = −60 dB (±0.5 dB).
// - Seno recortado (clipping): THD alto y clippedSamples > 0.
// - Silencio y parámetros inválidos: valid=false, sin crash.
// - Niveles: pico y RMS correctos para un seno conocido.
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>

#include "dsp/ThdMeter.hpp"

namespace {

using ivanna::measure::fillTestTone;
using ivanna::measure::measureThdPlusNoise;
using ivanna::measure::ThdResult;

constexpr double kSr = 48000.0;
constexpr double kF0 = 1000.0;
constexpr std::size_t kFrames = 4800;  // 100 ms = 100 ciclos de 1 kHz

TEST(ThdMeterTest, PureSineHasNegligibleThd) {
    float buf[kFrames];
    fillTestTone(buf, kFrames, kSr, kF0, 0.5f);
    const ThdResult r = measureThdPlusNoise(buf, kFrames, kSr, kF0);
    ASSERT_TRUE(r.valid);
    EXPECT_LT(r.thdPlusNoiseDb, -120.0)
        << "un seno puro (float) debe medir THD+N por debajo de -120 dB";
    EXPECT_LT(r.thdPercent, 0.001);
    // Fundamental de amplitud 0.5: 20*log10(0.5) = -6.02 dBFS.
    EXPECT_NEAR(r.fundamentalDbfs, -6.02, 0.05);
    EXPECT_NEAR(r.peakDbfs, -6.02, 0.05);
    EXPECT_NEAR(r.rmsDbfs, -9.03, 0.05);  // RMS = 0.5/sqrt(2)
    EXPECT_EQ(r.clippedSamples, 0u);
}

TEST(ThdMeterTest, SecondHarmonicMinus40dB) {
    float buf[kFrames];
    fillTestTone(buf, kFrames, kSr, kF0, 0.5f, -40.0);
    const ThdResult r = measureThdPlusNoise(buf, kFrames, kSr, kF0);
    ASSERT_TRUE(r.valid);
    // El 2º armónico a -40 dB domina el residuo: THD+N = -40 dB.
    EXPECT_NEAR(r.thdPlusNoiseDb, -40.0, 0.5);
    EXPECT_NEAR(r.thdPercent, 1.0, 0.05);  // 10^(-40/20) = 1%
}

TEST(ThdMeterTest, SecondHarmonicMinus60dB) {
    float buf[kFrames];
    fillTestTone(buf, kFrames, kSr, kF0, 0.5f, -60.0);
    const ThdResult r = measureThdPlusNoise(buf, kFrames, kSr, kF0);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.thdPlusNoiseDb, -60.0, 0.5);
    EXPECT_NEAR(r.thdPercent, 0.1, 0.01);  // 10^(-60/20) = 0.1%
}

TEST(ThdMeterTest, ClippedSineHasHighThdAndClippedSamples) {
    float buf[kFrames];
    fillTestTone(buf, kFrames, kSr, kF0, 1.5f);  // amplitud > 1.0
    // Recorte duro a ±1.0 (como haría un saturador).
    for (std::size_t i = 0; i < kFrames; ++i) {
        if (buf[i] > 1.0f) buf[i] = 1.0f;
        if (buf[i] < -1.0f) buf[i] = -1.0f;
    }
    const ThdResult r = measureThdPlusNoise(buf, kFrames, kSr, kF0);
    ASSERT_TRUE(r.valid);
    EXPECT_GT(r.thdPlusNoiseDb, -30.0)
        << "un seno recortado debe medir THD+N alto (armónicos de recorte)";
    EXPECT_GT(r.clippedSamples, 0u);
    EXPECT_NEAR(r.peakDbfs, 0.0, 0.1);
}

TEST(ThdMeterTest, SilenceIsInvalidNotCrash) {
    float buf[kFrames] = {};
    const ThdResult r = measureThdPlusNoise(buf, kFrames, kSr, kF0);
    EXPECT_FALSE(r.valid) << "sin fundamental medible, el resultado no es válido";
    EXPECT_EQ(r.clippedSamples, 0u);
}

TEST(ThdMeterTest, InvalidParamsAreInvalidNotCrash) {
    float buf[kFrames];
    fillTestTone(buf, kFrames, kSr, kF0, 0.5f);
    EXPECT_FALSE(measureThdPlusNoise(nullptr, kFrames, kSr, kF0).valid);
    EXPECT_FALSE(measureThdPlusNoise(buf, 0, kSr, kF0).valid);
    EXPECT_FALSE(measureThdPlusNoise(buf, kFrames, 0.0, kF0).valid);
    EXPECT_FALSE(measureThdPlusNoise(buf, kFrames, kSr, 0.0).valid);
    EXPECT_FALSE(measureThdPlusNoise(buf, kFrames, kSr, 30000.0).valid);  // > Nyquist
}

TEST(ThdMeterTest, FillTestToneGeneratesExactSine) {
    float buf[kFrames];
    fillTestTone(buf, kFrames, kSr, kF0, 0.25f);
    // Pico 0.25, RMS 0.25/sqrt(2), primer ciclo completo en 48 muestras.
    float peak = 0.0f;
    for (std::size_t i = 0; i < kFrames; ++i) {
        const float a = std::fabs(buf[i]);
        if (a > peak) peak = a;
    }
    EXPECT_NEAR(peak, 0.25f, 1e-6f);
    EXPECT_NEAR(buf[0], 0.0f, 1e-6f);
    EXPECT_NEAR(buf[12], 0.25f, 1e-5f);  // cuarto de ciclo (12 muestras)
}

}  // namespace
