// test_fused_brain.cpp — Tests del OPERADOR #7: CEREBRO NEURAL (NEURO·Ω).
// Fusión Magistral (2026-11-18). 48 kHz, tono 1 kHz, GoogleTest.
//
// Reglas que se verifican: en neutro (MANUAL o adaptation==0) el cerebro
// SOLO observa y la señal sale bit-exacta (ganancia 1.0, THD+N = piso
// double); en AUTO actúa (NPE + bucle cerrado) sin NaN/Inf ni
// desbordes; todos los controles se clampan (fail-closed).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_brain.hpp"
#include "../fused/fused_measure.hpp"
#include "../anti_dolby.h"

namespace {

using ivanna::fused::FusedBrain;
using ivanna::fused::FusedControls;
using ivanna::fused::measure::fillToneDouble;
using ivanna::fused::measure::isFiniteBuffer;
using ivanna::fused::measure::measureThdDouble;
using ivanna::fused::measure::nullTest;
using ivanna::fused::measure::peakAbs;

constexpr double kSr = 48000.0;
constexpr double kFreq = 1000.0;
constexpr int kFrames = 48000;  // 1000 ciclos enteros a 1 kHz

std::vector<double> makeTone(double amp = 0.707) {
    std::vector<double> x(static_cast<std::size_t>(kFrames));
    fillToneDouble(x.data(), x.size(), kSr, kFreq, amp);
    return x;
}

double rmsOf(const std::vector<double>& x) {
    double p = 0.0;
    for (double v : x) p += v * v;
    return std::sqrt(p / static_cast<double>(x.size()));
}

// Ruido blanco determinista (LCG, sin <random> ni estado global).
std::vector<double> makeNoise(int frames, double amp = 0.5) {
    std::vector<double> x(static_cast<std::size_t>(frames));
    std::uint32_t s = 0x12345678u;
    for (int i = 0; i < frames; ++i) {
        s = s * 1664525u + 1013904223u;
        const double u = static_cast<double>(s >> 8) * (1.0 / 16777216.0);
        x[static_cast<std::size_t>(i)] = amp * (2.0 * u - 1.0);
    }
    return x;
}

// FusedBrain no es copiable (hilos de inferencia en sus miembros):
// los helpers devuelven unique_ptr.
std::unique_ptr<FusedBrain> makeBrainNeutral() {
    auto b = std::make_unique<FusedBrain>();
    b->prepare(kSr);
    FusedControls fc;
    fc.setNeutral();  // brain -> MANUAL, adaptation 0
    b->setControls(fc.brain);
    return b;
}

std::unique_ptr<FusedBrain> makeBrainAuto(float adaptation = 1.0f) {
    auto b = std::make_unique<FusedBrain>();
    b->prepare(kSr);
    ivanna::fused::BrainControls c;
    c.mode = 0;  // AUTO
    c.adaptation = adaptation;
    b->setControls(c);
    return b;
}

TEST(FusedBrain, NeutralIsBitExact) {
    auto inL = makeTone();
    auto inR = makeTone();
    auto outL = inL;
    auto outR = inR;
    auto b = makeBrainNeutral();
    ASSERT_TRUE(b->isNeutral());
    b->process(outL.data(), outR.data(), kFrames);
    const auto nl = nullTest(inL.data(), outL.data(), inL.size());
    const auto nr = nullTest(inR.data(), outR.data(), inR.size());
    ASSERT_TRUE(nl.valid && nr.valid);
    EXPECT_TRUE(nl.bitExact) << "residuo L: " << nl.residualDb << " dB";
    EXPECT_TRUE(nr.bitExact) << "residuo R: " << nr.residualDb << " dB";
}

TEST(FusedBrain, NeutralGainUnity) {
    auto inL = makeTone();
    auto inR = makeTone();
    auto outL = inL;
    auto outR = inR;
    auto b = makeBrainNeutral();
    b->process(outL.data(), outR.data(), kFrames);
    EXPECT_NEAR(rmsOf(outL), rmsOf(inL), 1e-12);
    EXPECT_NEAR(rmsOf(outR), rmsOf(inR), 1e-12);
}

TEST(FusedBrain, NoNaNOrInf) {
    // Señales: silencio, DC 0.5, seno 0.99 y ruido blanco determinista.
    std::vector<std::vector<double>> signals;
    signals.emplace_back(static_cast<std::size_t>(kFrames), 0.0);
    signals.emplace_back(static_cast<std::size_t>(kFrames), 0.5);
    signals.push_back(makeTone(0.99));
    signals.push_back(makeNoise(kFrames, 0.5));

    for (int mode = 0; mode < 2; ++mode) {
        for (auto& in : signals) {
            auto outL = in;
            auto outR = in;
            auto b = (mode == 0) ? makeBrainNeutral() : makeBrainAuto(1.0f);
            b->process(outL.data(), outR.data(), kFrames);
            EXPECT_TRUE(isFiniteBuffer(outL.data(), outL.size()))
                << "modo " << mode << ": NaN/Inf en L";
            EXPECT_TRUE(isFiniteBuffer(outR.data(), outR.size()))
                << "modo " << mode << ": NaN/Inf en R";
            EXPECT_LE(peakAbs(outL.data(), outL.size()), 1.5)
                << "modo " << mode << ": pico L fuera de rango";
            EXPECT_LE(peakAbs(outR.data(), outR.size()), 1.5)
                << "modo " << mode << ": pico R fuera de rango";
        }
    }
}

TEST(FusedBrain, ControlClamping) {
    FusedBrain b;
    b.prepare(kSr);
    ivanna::fused::BrainControls c;
    c.mode = 99;
    c.adaptation = 5.0f;
    b.setControls(c);
    const auto got = b.controls();
    EXPECT_LE(got.mode, 1);
    EXPECT_GE(got.mode, 0);
    EXPECT_LE(got.adaptation, 1.0f);
    EXPECT_GE(got.adaptation, 0.0f);
    // mode 99 clampa a 1 (MANUAL) y en MANUAL adaptation se fuerza a 0.
    EXPECT_EQ(got.mode, 1);
    EXPECT_EQ(got.adaptation, 0.0f);
    EXPECT_TRUE(b.isNeutral());

    auto inL = makeTone();
    auto inR = makeTone();
    b.process(inL.data(), inR.data(), kFrames);
    EXPECT_TRUE(isFiniteBuffer(inL.data(), inL.size()));
    EXPECT_TRUE(isFiniteBuffer(inR.data(), inR.size()));
}

TEST(FusedBrain, EngagedSmoke) {
    auto inL = makeTone();
    auto inR = makeTone();
    auto b = makeBrainAuto(1.0f);
    ASSERT_FALSE(b->isNeutral());
    b->process(inL.data(), inR.data(), kFrames);
    EXPECT_TRUE(isFiniteBuffer(inL.data(), inL.size()));
    EXPECT_TRUE(isFiniteBuffer(inR.data(), inR.size()));
    EXPECT_LT(peakAbs(inL.data(), inL.size()), 4.0);
    EXPECT_LT(peakAbs(inR.data(), inR.size()), 4.0);
}

TEST(FusedBrain, ThdAtNeutral) {
    auto inL = makeTone();
    auto outL = inL;
    auto outR = inL;
    auto b = makeBrainNeutral();
    b->process(outL.data(), outR.data(), kFrames);
    const auto thd = measureThdDouble(outL.data(), outL.size(), kSr, kFreq);
    ASSERT_TRUE(thd.valid);
    // Bit-exacto: sin residuo medible -> -320 dB (piso del medidor).
    EXPECT_LE(thd.thdPlusNoiseDb, -200.0)
        << "THD+N neutro medido: " << thd.thdPlusNoiseDb << " dB";
    const auto n = nullTest(inL.data(), outL.data(), inL.size());
    ASSERT_TRUE(n.valid);
    EXPECT_TRUE(n.bitExact);
}

TEST(FusedBrain, ManualDoesNotActuate) {
    // El mismo tono que en AUTO sí pasaría por el NPE: en MANUAL la
    // salida debe ser idéntica a la entrada aunque el análisis corra.
    auto inL = makeTone();
    auto inR = makeTone();
    auto outL = inL;
    auto outR = inR;
    auto b = makeBrainNeutral();
    b->process(outL.data(), outR.data(), kFrames);
    const auto nl = nullTest(inL.data(), outL.data(), inL.size());
    const auto nr = nullTest(inR.data(), outR.data(), inR.size());
    ASSERT_TRUE(nl.valid && nr.valid);
    EXPECT_TRUE(nl.bitExact) << "residuo L: " << nl.residualDb << " dB";
    EXPECT_TRUE(nr.bitExact) << "residuo R: " << nr.residualDb << " dB";
    // Y el cerebro observa de verdad: la telemetría existe (0 = aún sin
    // clasificación válida del hilo asíncrono; nunca un valor absurdo).
    EXPECT_GE(b->dominantGenre(), 0);
    EXPECT_LE(b->dominantGenre(), 5);
    EXPECT_GE(b->confidence(), 0.0f);
    EXPECT_LE(b->confidence(), 1.0f);
}

TEST(FusedBrain, NameIsNeuroOmega) {
    EXPECT_STREQ(FusedBrain::name(), "NEURO·Ω");
}

TEST(FusedBrain, GuardianWidenerFollowsScene) {
    // ── A. Música a través del cerebro (cableado real) ──────────────
    // El tono de 1 kHz clasifica como MUSIC (género 1, medido en el
    // probe host); el guardián debe tender a widener ~1.35. La
    // clasificación es asíncrona (hilo de inferencia del clasificador),
    // así que se procesa en bucle hasta que el guardián se mueve.
    {
        auto toneL = makeTone();
        auto toneR = makeTone();
        auto b = makeBrainAuto(1.0f);
        ASSERT_EQ(b->antiDolbyIntensity(), 1.0f);
        float w = 1.0f;
        for (int i = 0; i < 10 && w <= 1.2f; ++i) {
            b->process(toneL.data(), toneR.data(), kFrames);
            w = b->antiDolbyWidener();
        }
        EXPECT_TRUE(std::isfinite(w));
        EXPECT_GT(w, 1.0f) << "el guardián no se movió hacia música";
        EXPECT_NEAR(w, 1.35f, 0.15f)
            << "widener medido con escena MUSIC: " << w;
        EXPECT_GT(b->antiDolbySpreadMul(), 1.0f);
        EXPECT_NEAR(b->antiDolbySpreadMul(), 1.25f, 0.15f);
        EXPECT_TRUE(std::isfinite(b->antiDolbyIntensity()));
    }

    // ── B. Voz, directo sobre el guardián ───────────────────────────
    // El cerebro alimenta al guardián solo desde el clasificador y el
    // clasificador no produce VOICE con un tono sintético de forma
    // determinista; la vía voz se verifica sobre el mismo
    // updateFromNeuralContext + tick() que el cerebro usa (objetivo
    // real en código: widener 0.75, EQ +2.5 dB en 2-4 kHz).
    {
        AntiDolbyState g;
        g.setAntiDolbyIntensity(1.0f);
        g.updateFromNeuralContext(
            static_cast<std::uint8_t>(AcousticScene::VOICE), 0.9f, 0.5f);
        const float dt = 128.0f / 48000.0f;
        for (int i = 0; i < 400; ++i) g.tick(dt);  // ~1.07 s
        const float w = g.currentWidener();
        EXPECT_TRUE(std::isfinite(w));
        EXPECT_LT(w, 1.0f) << "el guardián no se movió hacia voz";
        EXPECT_NEAR(w, 0.75f, 0.15f)
            << "widener medido con escena VOICE: " << w;
        EXPECT_GT(g.currentEqBoost(), 0.0f);
        EXPECT_NEAR(g.currentEqBoost(), 2.5f, 0.5f);
        EXPECT_TRUE(std::isfinite(g.currentIntensity()));
    }

    // ── C. MANUAL: el guardián observa pero no modula ───────────────
    {
        auto toneL = makeTone();
        auto toneR = makeTone();
        auto b = makeBrainNeutral();
        b->process(toneL.data(), toneR.data(), kFrames);
        EXPECT_EQ(b->antiDolbyIntensity(), 0.0f);
        EXPECT_EQ(b->antiDolbyWidener(), 1.0f);
        EXPECT_EQ(b->antiDolbyEqBoostDb(), 0.0f);
        EXPECT_EQ(b->antiDolbySpreadMul(), 1.0f);
    }
}

TEST(FusedBrain, LargeBlockIsChunked) {
    // Más de kFusedMaxBlockFrames (8192): se procesa en rebanadas sin
    // tocar el buffer en neutro y finito en AUTO.
    const int frames = 20000;
    auto tone = makeTone();
    tone.resize(static_cast<std::size_t>(frames), 0.0);
    auto in = tone;
    auto b = makeBrainNeutral();
    b->process(tone.data(), tone.data(), frames);
    const auto n = nullTest(in.data(), tone.data(), in.size());
    ASSERT_TRUE(n.valid);
    EXPECT_TRUE(n.bitExact);

    auto a = makeBrainAuto(1.0f);
    auto tone2 = in;
    a->process(tone2.data(), tone2.data(), frames);
    EXPECT_TRUE(isFiniteBuffer(tone2.data(), tone2.size()));
    EXPECT_LT(peakAbs(tone2.data(), tone2.size()), 4.0);
}

}  // namespace
