// test_fused_listener_room.cpp — OPERADOR #3, MODELO OYENTE+SALA ("SALA VIVA").
// GoogleTest, 48 kHz, tono 1 kHz. Sin dataset RIR en host: el stage RIR es
// bypass salvo que el test inyecte una IR con setRir() (ver
// RirConstantPowerMix). La corrección de sala sí actúa en host y es lo que
// cambia la señal en EngagedSmoke.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_listener_room.hpp"
#include "../fused/fused_measure.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 48000;  // 1 s = 1000 ciclos enteros de 1 kHz
constexpr double kFreq = 1000.0;

std::vector<double> Tone(double amp, int frames = kFrames) {
    std::vector<double> x(static_cast<std::size_t>(frames));
    fillToneDouble(x.data(), static_cast<std::size_t>(frames), kSr, kFreq, amp);
    return x;
}

double Rms(const std::vector<double>& x, std::size_t from = 0) {
    double p = 0.0;
    const std::size_t n = x.size() - from;
    for (std::size_t i = from; i < x.size(); ++i) p += x[i] * x[i];
    return std::sqrt(p / static_cast<double>(n));
}

std::uint32_t Lcg(std::uint32_t& s) {
    s = s * 1664525u + 1013904223u;
    return s;
}

float LcgUni(std::uint32_t& s) {  // uniforme en [-1, 1), media 0
    return static_cast<float>(Lcg(s) >> 9) / 8388608.0f * 2.0f - 1.0f;
}

// Ruido blanco determinista, uniforme en [-amp, amp].
std::vector<double> Noise(double amp, int frames, std::uint32_t seed = 12345u) {
    std::vector<double> x(static_cast<std::size_t>(frames));
    std::uint32_t s = seed;
    for (int i = 0; i < frames; ++i)
        x[static_cast<std::size_t>(i)] = amp * static_cast<double>(LcgUni(s));
    return x;
}

ListenerRoomControls EngagedControls() {
    ListenerRoomControls c;
    c.roomWet = 0.22f;
    c.rt60Sec = 0.34f;
    c.roomSizeM = 5.0f;
    c.hearingProfile = 0;
    c.roomCorrDb[5] = -3.0f;  // banda de 1 kHz
    return c;
}

void ExpectFiniteBounded(const std::vector<double>& x, double bound) {
    EXPECT_TRUE(isFiniteBuffer(x.data(), x.size()));
    EXPECT_LE(peakAbs(x.data(), x.size()), bound);
}

}  // namespace

TEST(FusedListenerRoom, NameIsSalaViva) {
    EXPECT_STREQ(FusedListenerRoom::name(), "SALA VIVA");
}

TEST(FusedListenerRoom, NeutralIsBitExact) {
    FusedListenerRoom op;
    op.prepare(kSr);
    FusedControls fc;
    fc.setNeutral();
    op.setControls(fc.listenerRoom);
    ASSERT_TRUE(op.isNeutral());

    const auto inL = Tone(0.5);
    const auto inR = Tone(0.5);
    auto outL = inL;
    auto outR = inR;
    op.process(outL.data(), outR.data(), kFrames);

    const NullResult nL = nullTest(inL.data(), outL.data(), kFrames);
    const NullResult nR = nullTest(inR.data(), outR.data(), kFrames);
    ASSERT_TRUE(nL.valid && nR.valid);
    EXPECT_TRUE(nL.bitExact) << "residualDb L = " << nL.residualDb;
    EXPECT_TRUE(nR.bitExact) << "residualDb R = " << nR.residualDb;
}

TEST(FusedListenerRoom, NeutralGainUnity) {
    FusedListenerRoom op;
    op.prepare(kSr);
    FusedControls fc;
    fc.setNeutral();
    op.setControls(fc.listenerRoom);

    const auto inL = Tone(0.5);
    const auto inR = Tone(0.5);
    auto outL = inL;
    auto outR = inR;
    op.process(outL.data(), outR.data(), kFrames);

    EXPECT_NEAR(Rms(outL), Rms(inL), 1e-12);
    EXPECT_NEAR(Rms(outR), Rms(inR), 1e-12);
}

TEST(FusedListenerRoom, NoNaNOrInf) {
    FusedListenerRoom op;
    op.prepare(kSr);
    ListenerRoomControls c = EngagedControls();
    c.hearingProfile = 1;  // HARMAN: ejerce el perfil auditivo
    op.setControls(c);
    ASSERT_FALSE(op.isNeutral());

    // Silencio.
    {
        std::vector<double> l(4096, 0.0), r(4096, 0.0);
        op.process(l.data(), r.data(), 4096);
        ExpectFiniteBounded(l, 1.5);
        ExpectFiniteBounded(r, 1.5);
    }
    // DC 0.5.
    {
        std::vector<double> l(4096, 0.5), r(4096, 0.5);
        op.process(l.data(), r.data(), 4096);
        ExpectFiniteBounded(l, 1.5);
        ExpectFiniteBounded(r, 1.5);
    }
    // Seno 0.99 a 1 kHz (100 ciclos).
    {
        auto l = Tone(0.99, 4800);
        auto r = l;
        op.process(l.data(), r.data(), 4800);
        ExpectFiniteBounded(l, 1.5);
        ExpectFiniteBounded(r, 1.5);
    }
    // Ruido blanco determinista 0.9.
    {
        auto l = Noise(0.9, 8192, 999u);
        auto r = Noise(0.9, 8192, 555u);
        op.process(l.data(), r.data(), 8192);
        ExpectFiniteBounded(l, 1.5);
        ExpectFiniteBounded(r, 1.5);
    }
    // Bloque grande (> kFusedMaxBlockFrames=8192) y bloque no múltiplo de 128.
    {
        auto l = Noise(0.9, 20000, 77u);
        auto r = Noise(0.9, 20000, 78u);
        op.process(l.data(), r.data(), 20000);
        ExpectFiniteBounded(l, 1.5);
        ExpectFiniteBounded(r, 1.5);
        auto l2 = Tone(0.5, 1000);
        auto r2 = l2;
        op.process(l2.data(), r2.data(), 1000);
        ExpectFiniteBounded(l2, 1.5);
        ExpectFiniteBounded(r2, 1.5);
    }
}

TEST(FusedListenerRoom, ControlClamping) {
    FusedListenerRoom op;
    op.prepare(kSr);
    ListenerRoomControls c;
    c.roomWet = 5.0f;       // -> 1.0
    c.rt60Sec = -1.0f;      // -> 0.05
    c.roomSizeM = 100.0f;   // -> 12.0
    c.hearingProfile = 99;  // -> 2
    c.roomCorrDb[0] = 5.0f;   // positivo -> 0.0 (cut-only)
    c.roomCorrDb[1] = -99.0f; // -> -12.0
    op.setControls(c);
    const ListenerRoomControls got = op.controls();
    EXPECT_FLOAT_EQ(got.roomWet, 1.0f);
    EXPECT_FLOAT_EQ(got.rt60Sec, 0.05f);
    EXPECT_FLOAT_EQ(got.roomSizeM, 12.0f);
    EXPECT_EQ(got.hearingProfile, 2);
    EXPECT_FLOAT_EQ(got.roomCorrDb[0], 0.0f);
    EXPECT_FLOAT_EQ(got.roomCorrDb[1], -12.0f);
    EXPECT_LE(op.effectiveRoomWet(), 1.0f);

    auto l = Tone(0.5);
    auto r = l;
    op.process(l.data(), r.data(), kFrames);
    ExpectFiniteBounded(l, 1.5);
    ExpectFiniteBounded(r, 1.5);
}

TEST(FusedListenerRoom, EngagedSmoke) {
    FusedListenerRoom op;
    op.prepare(kSr);
    op.setControls(EngagedControls());
    ASSERT_FALSE(op.isNeutral());
    // Sin dataset RIR en host el stage RIR es bypass; lo afirma el test.
    EXPECT_FALSE(op.rirLoaded());

    const auto inL = Tone(0.5);
    const auto inR = Tone(0.5);
    auto outL = inL;
    auto outR = inR;
    op.process(outL.data(), outR.data(), kFrames);

    ExpectFiniteBounded(outL, 4.0);
    ExpectFiniteBounded(outR, 4.0);

    // La corrección de sala SÍ cambia la señal: -3 dB en la banda de 1 kHz
    // (el tono es de 1 kHz). Residuo no nulo y RMS de la segunda mitad
    // cerca de 10^(-3/20) = 0.708 del RMS de entrada.
    const NullResult nL = nullTest(inL.data(), outL.data(), kFrames);
    ASSERT_TRUE(nL.valid);
    EXPECT_FALSE(nL.bitExact);
    EXPECT_GT(nL.maxAbsDiff, 1e-3);
    const double rmsIn = Rms(inL);
    const double rmsOut2nd = Rms(outL, kFrames / 2);
    EXPECT_LT(rmsOut2nd, 0.85 * rmsIn);
    EXPECT_GT(rmsOut2nd, 0.50 * rmsIn);
    std::printf("[EngagedSmoke] residualDb=%.2f rmsIn=%.4f rmsOut(2a mitad)=%.4f\n",
                nL.residualDb, rmsIn, rmsOut2nd);
}

TEST(FusedListenerRoom, ThdAtNeutral) {
    FusedListenerRoom op;
    op.prepare(kSr);
    FusedControls fc;
    fc.setNeutral();
    op.setControls(fc.listenerRoom);
    ASSERT_TRUE(op.isNeutral());

    // Tono a -3 dBFS (amp = 10^(-3/20)), 1000 ciclos enteros.
    const auto inL = Tone(0.7079457843841379);
    auto outL = inL;
    auto outR = inL;
    op.process(outL.data(), outR.data(), kFrames);

    const ThdDoubleResult thd = measureThdDouble(outL.data(), kFrames, kSr, kFreq);
    ASSERT_TRUE(thd.valid);
    std::printf("[ThdAtNeutral] THD+N = %.2f dB (fundamental %.2f dBFS, pico %.2f dBFS)\n",
                thd.thdPlusNoiseDb, thd.fundamentalDbfs, thd.peakDbfs);
    EXPECT_LE(thd.thdPlusNoiseDb, -200.0);

    const NullResult nL = nullTest(inL.data(), outL.data(), kFrames);
    ASSERT_TRUE(nL.valid);
    EXPECT_TRUE(nL.bitExact);
}

TEST(FusedListenerRoom, RoomCorrectionIsCutOnly) {
    FusedListenerRoom op;
    op.prepare(kSr);
    ListenerRoomControls c;
    c.roomWet = 0.0f;
    c.hearingProfile = 0;
    for (int i = 0; i < 10; ++i) c.roomCorrDb[i] = -6.0f;
    op.setControls(c);
    ASSERT_FALSE(op.isNeutral());

    // Ruido blanco de banda ancha: todas las bandas recortan.
    const auto inL = Noise(0.5, kFrames, 4242u);
    const auto inR = Noise(0.5, kFrames, 4343u);
    auto outL = inL;
    auto outR = inR;
    op.process(outL.data(), outR.data(), kFrames);

    ExpectFiniteBounded(outL, 1.5);
    ExpectFiniteBounded(outR, 1.5);
    // Nunca realza: RMS de salida <= RMS de entrada (tolerancia 1e-6) y
    // ningún pico supera al de entrada (+0.05 de tolerancia: los biquads
    // peaking realinean la fase y el pico de la suma puede subir ~5%
    // aunque TODAS las ganancias sean negativas; el criterio de ganancia
    // es el RMS, no el pico instantáneo).
    EXPECT_LE(Rms(outL), Rms(inL) + 1e-6);
    EXPECT_LE(Rms(outR), Rms(inR) + 1e-6);
    EXPECT_LE(peakAbs(outL.data(), kFrames), peakAbs(inL.data(), kFrames) + 0.05);
    EXPECT_LE(peakAbs(outR.data(), kFrames), peakAbs(inR.data(), kFrames) + 0.05);
    // El recorte es real: con -6 dB en las 10 bandas el RMS cae de forma
    // clara (no un bypass disfrazado).
    EXPECT_LT(Rms(outL), 0.8 * Rms(inL));
    EXPECT_LT(Rms(outR), 0.8 * Rms(inR));
}

// La ley de mezcla RIR de potencia constante (gDry^2+gWet^2=1) se verifica
// sin suponer la ganancia de la IR: RirConvolver::load() normaliza la IR
// (L2 0.72 + supresión de directa), así que la ganancia del wet G es
// desconocida pero COMPARTIDA entre dos pasadas con el mismo ruido y la
// misma IR: con wet=1.0, ratio^2 = G^2; con wet=0.5,
// ratio^2 = gDry^2 + gWet^2*G^2 = 0.75 + 0.25*G^2
// (gDry = sqrt(1-0.5^2) = 0.866, NO 0.7071: ese es el punto -3 dB).
// Si la mezcla fuera lineal (gDry+gWet=1) o el wet se sumara sin atenuar
// el dry, la predicción fallaría.
TEST(FusedListenerRoom, RirConstantPowerMix) {
    // IR difusa determinista: ruido con caída exponencial, 512 taps.
    std::vector<float> irL(512), irR(512);
    std::uint32_t sL = 0x12345678u, sR = 0x87654321u;
    for (int i = 0; i < 512; ++i) {
        const float env = std::exp(-3.0f * static_cast<float>(i) / 512.0f);
        irL[static_cast<std::size_t>(i)] = env * LcgUni(sL);
        irR[static_cast<std::size_t>(i)] = env * LcgUni(sR);
    }

    auto run = [&](float wet) -> double {
        FusedListenerRoom op;
        op.prepare(kSr);
        op.setRir(irL.data(), irR.data(), 512);
        EXPECT_TRUE(op.rirLoaded());  // EXPECT, no ASSERT: esto es una lambda con retorno
        ListenerRoomControls c;
        c.roomWet = wet;
        c.rt60Sec = 0.34f;  // sala seca: el limitador deja pasar el wet íntegro
        c.hearingProfile = 0;
        op.setControls(c);
        EXPECT_FLOAT_EQ(op.effectiveRoomWet(), wet);
        const auto inL = Noise(0.4, kFrames, 777u);
        const auto inR = Noise(0.4, kFrames, 778u);
        auto outL = inL;
        auto outR = inR;
        op.process(outL.data(), outR.data(), kFrames);
        EXPECT_TRUE(isFiniteBuffer(outL.data(), outL.size()));
        EXPECT_TRUE(isFiniteBuffer(outR.data(), outR.size()));
        // Segunda mitad: convolver cebado y wet asentado.
        return Rms(outL, kFrames / 2) / Rms(inL, kFrames / 2);
    };

    const double rWet1 = run(1.0f);
    const double rWet05 = run(0.5f);
    std::printf("[RirConstantPowerMix] ratio wet=1.0: %.4f, wet=0.5: %.4f\n",
                rWet1, rWet05);
    EXPECT_GT(rWet1, 0.05);  // el wet está presente de verdad
    const double predicted05 = std::sqrt(0.75 + 0.25 * rWet1 * rWet1);
    EXPECT_NEAR(rWet05, predicted05, 0.10 * predicted05);
}
