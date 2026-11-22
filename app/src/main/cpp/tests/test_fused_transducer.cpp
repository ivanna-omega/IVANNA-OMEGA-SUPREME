// test_fused_transducer.cpp — OPERADOR #4: CORRECTOR DE TRANSDUCTOR.
// Fusión Magistral (2026-11-18). 48 kHz, tono 1 kHz, GoogleTest.
//
// NOTA HONESTA (host, sin kernel ni medida): en host no hay kernel
// Volterra .ivk1 cargado ni respuesta medida del transductor, así que
//   * VolterraH2Symmetric es bypass fail-closed (memcpy, sin kernel
//     efectivo — verificado: volterraHasEffectiveKernel()==false),
//   * DiagonalVolterra es identidad (sin coeficientes medidos),
//   * TransducerOperator diseña ganancias 0 dB (medida plana).
// El stage con efecto audible real en host es TemporalSpatialDynamics
// (lateral >250 Hz), que EngagedSmoke verifica que SÍ actúa con una
// señal estéreo con lateral. El resto se verifica por finitud,
// acotación, clamping y bit-exactitud en neutro.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "../fused/fused_measure.hpp"
#include "../fused/fused_transducer.hpp"

using ivanna::fused::FusedControls;
using ivanna::fused::FusedTransducer;
using ivanna::fused::TransducerControls;
using ivanna::fused::measure::isFiniteBuffer;
using ivanna::fused::measure::measureThdDouble;
using ivanna::fused::measure::nullTest;
using ivanna::fused::measure::peakAbs;

namespace {

constexpr double kSr = 48000.0;
constexpr double kFreq = 1000.0;
constexpr int kFrames = 48000;  // 1 s = 1000 ciclos exactos a 1 kHz.
constexpr double kPi = 3.14159265358979323846;

// Tono 1 kHz en ambos canales + lateral opcional a sideFreq (para que
// la dinámica temporal tenga lateral que mover: L==R no tiene lateral).
void fillTone(std::vector<double>& l, std::vector<double>& r, int n, double amp,
              double sideAmp = 0.0, double sideFreq = 3000.0) {
    l.assign(static_cast<std::size_t>(n), 0.0);
    r.assign(static_cast<std::size_t>(n), 0.0);
    for (int i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        const double mid = amp * std::sin(2.0 * kPi * kFreq * t);
        const double side = sideAmp * std::sin(2.0 * kPi * sideFreq * t);
        l[static_cast<std::size_t>(i)] = mid + side;
        r[static_cast<std::size_t>(i)] = mid - side;
    }
}

double rmsOf(const std::vector<double>& x) {
    double p = 0.0;
    for (double v : x) p += v * v;
    return std::sqrt(p / static_cast<double>(x.size()));
}

double maxAbsDiff(const std::vector<double>& a, const std::vector<double>& b) {
    double m = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const double d = std::fabs(a[i] - b[i]);
        if (d > m) m = d;
    }
    return m;
}

TransducerControls neutralControls() {
    FusedControls c;
    c.setNeutral();
    return c.transducer;
}

TransducerControls engagedControls() {
    TransducerControls c;
    c.correction = 1.0f;
    c.temporalAmount = 0.5f;
    c.emfEnabled = true;
    return c;
}

}  // namespace

TEST(FusedTransducer, NameIsTransductor) {
    EXPECT_STREQ(FusedTransducer::name(), "VOLTERRA·Ω");
}

TEST(FusedTransducer, NeutralIsBitExact) {
    FusedTransducer op;
    op.prepare(kSr);
    op.setControls(neutralControls());
    ASSERT_TRUE(op.isNeutral());

    std::vector<double> l, r;
    fillTone(l, r, kFrames, 0.7071);  // 48000 frames > 8192: ejercita el troceado.
    const std::vector<double> l0 = l, r0 = r;
    op.process(l.data(), r.data(), kFrames);
    const auto nulL = nullTest(l0.data(), l.data(), l0.size());
    const auto nulR = nullTest(r0.data(), r.data(), r0.size());
    ASSERT_TRUE(nulL.valid && nulR.valid);
    EXPECT_TRUE(nulL.bitExact) << "residuo L: " << nulL.residualDb << " dB";
    EXPECT_TRUE(nulR.bitExact) << "residuo R: " << nulR.residualDb << " dB";
}

TEST(FusedTransducer, NeutralGainUnity) {
    FusedTransducer op;
    op.prepare(kSr);
    op.setControls(neutralControls());
    std::vector<double> l, r;
    fillTone(l, r, kFrames, 0.7071);
    const double rmsIn = rmsOf(l);
    op.process(l.data(), r.data(), kFrames);
    const double rmsOut = rmsOf(l);
    EXPECT_NEAR(rmsOut, rmsIn, 1e-12) << "RMS in=" << rmsIn << " out=" << rmsOut;
}

TEST(FusedTransducer, NoNaNOrInf) {
    FusedTransducer op;
    op.prepare(kSr);
    op.setControls(engagedControls());
    ASSERT_FALSE(op.isNeutral());

    // Silencio.
    {
        std::vector<double> l(4800, 0.0), r(4800, 0.0);
        op.process(l.data(), r.data(), 4800);
        EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
        EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
        EXPECT_LE(peakAbs(l.data(), l.size()), 1.5);
    }
    // DC 0.5.
    {
        std::vector<double> l(4800, 0.5), r(4800, 0.5);
        op.reset();
        op.process(l.data(), r.data(), 4800);
        EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
        EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
        EXPECT_LE(peakAbs(l.data(), l.size()), 1.5);
        EXPECT_LE(peakAbs(r.data(), r.size()), 1.5);
    }
    // Seno 0.99.
    {
        std::vector<double> l, r;
        fillTone(l, r, 4800, 0.99);
        op.reset();
        op.process(l.data(), r.data(), 4800);
        EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
        EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
        EXPECT_LE(peakAbs(l.data(), l.size()), 1.5);
        EXPECT_LE(peakAbs(r.data(), r.size()), 1.5);
    }
    // Ruido blanco determinista (LCG), 16384 frames (> 8192).
    {
        std::vector<double> l(16384), r(16384);
        std::uint32_t s = 12345u;
        auto next = [&s]() {
            s = s * 1664525u + 1013904223u;
            return static_cast<double>(s >> 8) / 16777216.0 * 2.0 - 1.0;
        };
        for (std::size_t i = 0; i < l.size(); ++i) {
            l[i] = 0.5 * next();
            r[i] = 0.5 * next();
        }
        op.reset();
        op.process(l.data(), r.data(), 16384);
        EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
        EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
        EXPECT_LE(peakAbs(l.data(), l.size()), 1.5);
        EXPECT_LE(peakAbs(r.data(), r.size()), 1.5);
    }
}

TEST(FusedTransducer, ControlClamping) {
    FusedTransducer op;
    op.prepare(kSr);
    TransducerControls c;
    c.correction = 5.0f;       // fuera de rango: debe quedar en 1.0
    c.temporalAmount = -2.0f;  // fuera de rango: debe quedar en 0.0
    c.emfEnabled = true;
    op.setControls(c);
    const TransducerControls got = op.controls();
    EXPECT_FLOAT_EQ(got.correction, 1.0f);
    EXPECT_FLOAT_EQ(got.temporalAmount, 0.0f);
    EXPECT_TRUE(got.emfEnabled);

    std::vector<double> l, r;
    fillTone(l, r, 4096, 0.7071);
    op.process(l.data(), r.data(), 4096);  // sin crash, salida finita
    EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
    EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
}

TEST(FusedTransducer, EngagedSmoke) {
    FusedTransducer op;
    op.prepare(kSr);
    op.setControls(engagedControls());
    ASSERT_FALSE(op.isNeutral());
    // En host no hay kernel Volterra cargado: el stage H2 es bypass
    // fail-closed por diseño (no inventa coeficientes). Se verifica.
    EXPECT_FALSE(op.volterraHasEffectiveKernel());
    EXPECT_TRUE(op.transducerDesignValid());
    EXPECT_NEAR(op.transducerDesignMeanGainDb(), 0.0f, 1e-6f);

    // Señal con lateral a 3 kHz (>250 Hz) para que la dinámica temporal
    // tenga algo que mover: L==R no tiene lateral y no cambiaría nada.
    std::vector<double> l, r;
    fillTone(l, r, kFrames, 0.5, 0.15, 3000.0);
    const std::vector<double> l0 = l, r0 = r;
    op.process(l.data(), r.data(), kFrames);
    EXPECT_TRUE(isFiniteBuffer(l.data(), l.size()));
    EXPECT_TRUE(isFiniteBuffer(r.data(), r.size()));
    EXPECT_LT(peakAbs(l.data(), l.size()), 4.0);
    EXPECT_LT(peakAbs(r.data(), r.size()), 4.0);
    // La dinámica temporal SÍ actúa: la salida difiere de la entrada
    // (el lateral se recoge en transientes y se abre en lo sostenido).
    EXPECT_GT(maxAbsDiff(l, l0), 1e-3)
        << "temporalAmount=0.5 debería mover el lateral";
    EXPECT_GT(maxAbsDiff(r, r0), 1e-3);
}

TEST(FusedTransducer, ThdAtNeutral) {
    FusedTransducer op;
    op.prepare(kSr);
    op.setControls(neutralControls());
    std::vector<double> l, r;
    fillTone(l, r, kFrames, 0.7071);  // -3 dBFS, 1000 ciclos exactos
    const std::vector<double> l0 = l;
    op.process(l.data(), r.data(), kFrames);
    const auto nul = nullTest(l0.data(), l.data(), l0.size());
    ASSERT_TRUE(nul.valid);
    const auto thd = measureThdDouble(l.data(), l.size(), kSr, kFreq);
    ASSERT_TRUE(thd.valid);
    std::printf("[FusedTransducer] THD+N neutro (1 kHz, -3 dBFS, 48 kHz): "
                "%.2f dB (bitExact=%s, residual=%.3g dB)\n",
                thd.thdPlusNoiseDb, nul.bitExact ? "true" : "false",
                nul.residualDb);
    EXPECT_TRUE(nul.bitExact || thd.thdPlusNoiseDb <= -200.0)
        << "THD+N medido: " << thd.thdPlusNoiseDb << " dB";
    EXPECT_LE(thd.thdPlusNoiseDb, -200.0)
        << "THD+N medido: " << thd.thdPlusNoiseDb << " dB";
}
