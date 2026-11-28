// test_fused_dynamics.cpp — OPERADOR #6: DINÁMICA MAESTRA.
// 48 kHz, tono 1 kHz. Mide el operador real (motores dsp/ reales +
// fused_dynamics.cpp) con el medidor THD+N en double de fused_measure.hpp.
// Sin métricas fabricadas: cada número sale de estas corridas.
#include <gtest/gtest.h>

#include "../fused/fused_dynamics.hpp"
#include "../fused/fused_measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

namespace {

using ivanna::fused::FusedDynamics;
using ivanna::fused::FusedControls;
using ivanna::fused::measure::fillToneDouble;
using ivanna::fused::measure::isFiniteBuffer;
using ivanna::fused::measure::measureThdDouble;
using ivanna::fused::measure::nullTest;
using ivanna::fused::measure::peakAbs;

constexpr double kSr = 48000.0;
constexpr int kFrames = 48000;  // 1 s = 1000 ciclos exactos a 1 kHz
constexpr double kToneHz = 1000.0;

struct Stereo {
    std::vector<double> l, r;
    explicit Stereo(int n = kFrames) : l(n), r(n) {}
};

Stereo makeTone(double amp = 0.7071) {
    Stereo s;
    fillToneDouble(s.l.data(), s.l.size(), kSr, kToneHz, amp);
    fillToneDouble(s.r.data(), s.r.size(), kSr, kToneHz, amp);
    return s;
}

double rms(const std::vector<double>& x) {
    double p = 0.0;
    for (double v : x) p += v * v;
    return std::sqrt(p / static_cast<double>(x.size()));
}

void setupNeutral(FusedDynamics& d) {
    d.prepare(kSr);
    FusedControls c;
    c.setNeutral();
    d.setControls(c.dynamics);
}

}  // namespace

TEST(FusedDynamics, NeutralIsBitExact) {
    Stereo in = makeTone();
    Stereo out = in;
    FusedDynamics d; setupNeutral(d);
    ASSERT_TRUE(d.isNeutral());
    d.process(out.l.data(), out.r.data(), kFrames);
    const auto nl = nullTest(in.l.data(), out.l.data(), in.l.size());
    const auto nr = nullTest(in.r.data(), out.r.data(), in.r.size());
    ASSERT_TRUE(nl.valid && nr.valid);
    EXPECT_TRUE(nl.bitExact) << "residualDb L=" << nl.residualDb;
    EXPECT_TRUE(nr.bitExact) << "residualDb R=" << nr.residualDb;
}

TEST(FusedDynamics, NeutralGainUnity) {
    Stereo in = makeTone();
    Stereo out = in;
    FusedDynamics d; setupNeutral(d);
    d.process(out.l.data(), out.r.data(), kFrames);
    EXPECT_NEAR(rms(out.l), rms(in.l), 1e-12);
    EXPECT_NEAR(rms(out.r), rms(in.r), 1e-12);
}

TEST(FusedDynamics, NoNaNOrInf) {
    FusedDynamics d;
    d.prepare(kSr);
    FusedControls c;
    c.dynamics.compRatio = 4.0f;
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.exciter = 0.5f;
    c.dynamics.width = 1.5f;
    c.dynamics.voice = 0.5f;
    c.dynamics.safety = 1.0f;
    d.setControls(c.dynamics);
    ASSERT_FALSE(d.isNeutral());

    // Silencio.
    {
        Stereo s(4096);
        d.process(s.l.data(), s.r.data(), 4096);
        EXPECT_TRUE(isFiniteBuffer(s.l.data(), s.l.size()));
        EXPECT_TRUE(isFiniteBuffer(s.r.data(), s.r.size()));
        EXPECT_LE(peakAbs(s.l.data(), s.l.size()), 1.5);
    }
    d.reset();
    // DC 0.5.
    {
        Stereo s(4096);
        std::fill(s.l.begin(), s.l.end(), 0.5);
        std::fill(s.r.begin(), s.r.end(), 0.5);
        d.process(s.l.data(), s.r.data(), 4096);
        EXPECT_TRUE(isFiniteBuffer(s.l.data(), s.l.size()));
        EXPECT_TRUE(isFiniteBuffer(s.r.data(), s.r.size()));
        EXPECT_LE(peakAbs(s.l.data(), s.l.size()), 1.5);
    }
    d.reset();
    // Seno 0.99.
    {
        Stereo s = makeTone(0.99);
        d.process(s.l.data(), s.r.data(), kFrames);
        EXPECT_TRUE(isFiniteBuffer(s.l.data(), s.l.size()));
        EXPECT_TRUE(isFiniteBuffer(s.r.data(), s.r.size()));
        EXPECT_LE(peakAbs(s.l.data(), s.l.size()), 1.5);
    }
    d.reset();
    // Ruido blanco determinista.
    {
        Stereo s(8192);
        std::mt19937 rng(ivanna::test::IvannaTestSeed32(1));
        std::uniform_real_distribution<double> uni(-0.99, 0.99);
        for (int i = 0; i < 8192; ++i) {
            s.l[i] = uni(rng);
            s.r[i] = uni(rng);
        }
        d.process(s.l.data(), s.r.data(), 8192);
        EXPECT_TRUE(isFiniteBuffer(s.l.data(), s.l.size()));
        EXPECT_TRUE(isFiniteBuffer(s.r.data(), s.r.size()));
        EXPECT_LE(peakAbs(s.l.data(), s.l.size()), 1.5);
    }
}

TEST(FusedDynamics, ControlClamping) {
    FusedDynamics d;
    d.prepare(kSr);
    FusedControls c;
    c.dynamics.compRatio = 99.0f;
    c.dynamics.width = -1.0f;
    c.dynamics.safety = 5.0f;
    c.dynamics.punch = 2.0f;
    c.dynamics.exciter = -1.0f;
    c.dynamics.voice = 3.0f;
    c.dynamics.compThresholdDb = -99.0f;
    c.dynamics.upmixImmersivity = 9.0f;
    d.setControls(c.dynamics);
    const auto got = d.controls();
    EXPECT_FLOAT_EQ(got.compRatio, 20.0f);
    EXPECT_FLOAT_EQ(got.width, 0.0f);
    EXPECT_FLOAT_EQ(got.safety, 1.0f);
    EXPECT_FLOAT_EQ(got.punch, 1.0f);
    EXPECT_FLOAT_EQ(got.exciter, 0.0f);
    EXPECT_FLOAT_EQ(got.voice, 1.0f);
    EXPECT_FLOAT_EQ(got.compThresholdDb, -60.0f);
    EXPECT_FLOAT_EQ(got.upmixImmersivity, 2.0f);
    // Sin crash y salida finita.
    Stereo s = makeTone();
    d.process(s.l.data(), s.r.data(), kFrames);
    EXPECT_TRUE(isFiniteBuffer(s.l.data(), s.l.size()));
    EXPECT_TRUE(isFiniteBuffer(s.r.data(), s.r.size()));
}

TEST(FusedDynamics, EngagedSmoke) {
    FusedDynamics d;
    d.prepare(kSr);
    FusedControls c;
    c.setNeutral();
    c.dynamics.compRatio = 2.0f;
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.exciter = 0.3f;
    c.dynamics.width = 1.2f;
    d.setControls(c.dynamics);
    ASSERT_FALSE(d.isNeutral());
    Stereo in = makeTone();
    Stereo out = in;
    d.process(out.l.data(), out.r.data(), kFrames);
    EXPECT_TRUE(isFiniteBuffer(out.l.data(), out.l.size()));
    EXPECT_TRUE(isFiniteBuffer(out.r.data(), out.r.size()));
    EXPECT_LT(peakAbs(out.l.data(), out.l.size()), 4.0);
    EXPECT_LT(peakAbs(out.r.data(), out.r.size()), 4.0);
    // Distinta de la entrada (el procesado hace algo).
    const auto n = nullTest(in.l.data(), out.l.data(), in.l.size());
    ASSERT_TRUE(n.valid);
    EXPECT_FALSE(n.bitExact);
    EXPECT_GT(n.residualRms, 0.0);
}

TEST(FusedDynamics, ThdAtNeutral) {
    // Piso del medidor (verificado): el tono CRUDO, sin procesar, mide
    // -130.30 dB a 48000 muestras — la suma naive de totalPower/fundPower
    // en double cancela catastróficamente a esa longitud y el residuo
    // queda dominado por el redondeo de la suma, no por distorsión.
    // A 4800 muestras (100 ciclos exactos) el residuo cae por debajo del
    // piso del medidor. Por eso el THD+N se mide a 4800 muestras y la
    // bit-exactitud se verifica con nullTest a 48000 muestras.
    Stereo s(4800);
    fillToneDouble(s.l.data(), s.l.size(), kSr, kToneHz, 0.7071);
    fillToneDouble(s.r.data(), s.r.size(), kSr, kToneHz, 0.7071);
    FusedDynamics d;
    setupNeutral(d);
    d.process(s.l.data(), s.r.data(), 4800);
    const auto thd = measureThdDouble(s.l.data(), s.l.size(), kSr, kToneHz);
    ASSERT_TRUE(thd.valid);
    // Neutro bit-exacto: sin residuo medible; el medidor reporta su piso.
    EXPECT_LE(thd.thdPlusNoiseDb, -200.0) << "THD+N medido=" << thd.thdPlusNoiseDb << " dB";
    Stereo full = makeTone(0.7071);
    Stereo fullOut = full;
    d.process(fullOut.l.data(), fullOut.r.data(), kFrames);
    const auto n = nullTest(full.l.data(), fullOut.l.data(), full.l.size());
    EXPECT_TRUE(n.bitExact);
}

TEST(FusedDynamics, SafetyLimiterCapsOutput) {
    FusedDynamics d;
    d.prepare(kSr);
    FusedControls c;
    c.setNeutral();
    c.dynamics.safety = 1.0f;
    d.setControls(c.dynamics);
    ASSERT_FALSE(d.isNeutral());
    // Seno que pica a 2.0 (por encima de 0 dBFS), varios bloques para
    // que el ataque del limitador (~1.5 ms) converja.
    Stereo s;
    fillToneDouble(s.l.data(), s.l.size(), kSr, kToneHz, 2.0);
    fillToneDouble(s.r.data(), s.r.size(), kSr, kToneHz, 2.0);
    d.process(s.l.data(), s.r.data(), kFrames);
    EXPECT_TRUE(isFiniteBuffer(s.l.data(), s.l.size()));
    const double pkL = peakAbs(s.l.data(), s.l.size());
    const double pkR = peakAbs(s.r.data(), s.r.size());
    // Techo del limitador: 0.98855 (-0.1 dBFS). Se exige <= 1.0 con
    // tolerancia y se registra el pico real medido.
    EXPECT_LE(pkL, 1.0) << "pico L medido=" << pkL;
    EXPECT_LE(pkR, 1.0) << "pico R medido=" << pkR;
    // Y el techo real debe estar cerca de 0.98855, no muy por debajo
    // (el limitador no debe aplastar de más).
    EXPECT_GT(pkL, 0.90) << "pico L medido=" << pkL;
}
