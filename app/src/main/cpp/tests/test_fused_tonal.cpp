// test_fused_tonal.cpp — OPERADOR #5: BALANCE TONAL ÚNICO (Fusión
// Magistral, 2026-11-18).
//
// 48 kHz; tono de 1 kHz, 48000 frames = 1000 ciclos exactos (salvo
// EngagedSmoke, que usa 100 Hz + 1 kHz: un tono de 1 kHz NO ejercita
// lowDb — las bandas de graves llegan a 125 Hz — y el smoke debe probar
// que el EQ de USUARIO actúa, no solo el EvoEQ).
//
// Enlaza contra el FusedTonal REAL + EvolutionaryEQ.cpp y
// Psychoacoustics.cpp reales (RoomCorrectionEq, EliteTargetCurves y
// LoudnessMeter son header-only, sin .cpp).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include "../fused/fused_measure.hpp"
#include "../fused/fused_tonal.hpp"

namespace {

using ivanna::fused::FusedControls;
using ivanna::fused::FusedTonal;
using ivanna::fused::TonalControls;
using namespace ivanna::fused::measure;

constexpr double kSr = 48000.0;
constexpr int kFrames = 48000;  // 1 s @ 48 kHz; 1000 ciclos de 1 kHz.

double rmsOf(const std::vector<double>& x) {
    double p = 0.0;
    for (double v : x) p += v * v;
    return std::sqrt(p / static_cast<double>(x.size()));
}

bool allFiniteBounded(const std::vector<double>& x, double bound) {
    for (double v : x) {
        if (!std::isfinite(v) || std::fabs(v) > bound) return false;
    }
    return true;
}

std::vector<double> tone1k(double amp = 0.707) {
    std::vector<double> x(kFrames);
    fillToneDouble(x.data(), x.size(), kSr, 1000.0, amp);
    return x;
}

// Ruido blanco determinista (LCG 32-bit, semilla fija), ±amp.
std::vector<double> whiteNoise(double amp = 0.5) {
    std::vector<double> x(kFrames);
    std::uint32_t s = 0x12345678u;
    for (int i = 0; i < kFrames; ++i) {
        s = s * 1664525u + 1013904223u;
        const double u = static_cast<double>(s >> 8) / 16777216.0;  // [0,1)
        x[i] = (u * 2.0 - 1.0) * amp;
    }
    return x;
}

void prepareNeutral(FusedTonal& t) {
    FusedControls fc;
    fc.setNeutral();
    t.prepare(kSr);
    t.setControls(fc.tonal);
    EXPECT_TRUE(t.isNeutral());
}

}  // namespace

// 0. Nombre aprobado por Esse (2026-11-18 ~07:06 CST): ARMONÍA.
TEST(FusedTonal, NameIsArmonia) {
    EXPECT_STREQ(FusedTonal::name(), "ARMONÍA");
}

// 1. Neutro = bit-exacto (el operador no toca el buffer).
TEST(FusedTonal, NeutralIsBitExact) {
    FusedTonal t; prepareNeutral(t);
    auto inL = tone1k();
    auto inR = tone1k();
    auto outL = inL, outR = inR;
    t.process(outL.data(), outR.data(), kFrames);
    const NullResult nl = nullTest(inL.data(), outL.data(), kFrames);
    const NullResult nr = nullTest(inR.data(), outR.data(), kFrames);
    ASSERT_TRUE(nl.valid && nr.valid);
    EXPECT_TRUE(nl.bitExact) << "residualDb L = " << nl.residualDb;
    EXPECT_TRUE(nr.bitExact) << "residualDb R = " << nr.residualDb;
}

// 2. Neutro = ganancia unidad exacta.
TEST(FusedTonal, NeutralGainUnity) {
    FusedTonal t; prepareNeutral(t);
    auto inL = tone1k();
    auto inR = tone1k();
    auto outL = inL, outR = inR;
    t.process(outL.data(), outR.data(), kFrames);
    EXPECT_NEAR(rmsOf(outL), rmsOf(inL), 1e-12);
    EXPECT_NEAR(rmsOf(outR), rmsOf(inR), 1e-12);
}

// 3. Nada de NaN/Inf ni explosión con señales de peor caso y EQ activo.
TEST(FusedTonal, NoNaNOrInf) {
    TonalControls c;
    c.curve = 1;  // Harman
    c.trimDb = 3.0f;
    c.lowDb = 3.0f;
    c.midDb = 2.0f;
    c.highDb = 2.0f;
    FusedTonal t;
    t.prepare(kSr);
    t.setControls(c);
    ASSERT_TRUE(t.evoReady());

    std::vector<std::vector<double>> signals;
    signals.emplace_back(kFrames, 0.0);              // silencio
    signals.emplace_back(kFrames, 0.5);              // DC 0.5
    signals.push_back(tone1k(0.99));                 // seno 1 kHz a 0.99
    signals.push_back(whiteNoise(0.5));              // ruido blanco ±0.5
    for (auto& s : signals) {
        auto l = s, r = s;
        t.process(l.data(), r.data(), kFrames);
        EXPECT_TRUE(allFiniteBounded(l, 1.5)) << "canal L no finito o >1.5";
        EXPECT_TRUE(allFiniteBounded(r, 1.5)) << "canal R no finito o >1.5";
    }
}

// 4. Los controles se clampan (fail-closed) y el proceso sigue finito.
TEST(FusedTonal, ControlClamping) {
    TonalControls c;
    c.curve = 99;
    c.trimDb = 99.0f;
    c.lowDb = -99.0f;
    c.midDb = 99.0f;
    c.highDb = -99.0f;
    FusedTonal t;
    t.prepare(kSr);
    t.setControls(c);
    const TonalControls got = t.controls();
    EXPECT_EQ(got.curve, 2);
    EXPECT_FLOAT_EQ(got.trimDb, 6.0f);
    EXPECT_FLOAT_EQ(got.lowDb, -12.0f);
    EXPECT_FLOAT_EQ(got.midDb, 12.0f);
    EXPECT_FLOAT_EQ(got.highDb, -12.0f);
    auto l = tone1k(0.5), r = tone1k(0.5);
    t.process(l.data(), r.data(), kFrames);
    EXPECT_TRUE(allFiniteBounded(l, 1.5));
    EXPECT_TRUE(allFiniteBounded(r, 1.5));
}

// 5. Con controles no neutros el EQ SÍ actúa (y no explota).
TEST(FusedTonal, EngagedSmoke) {
    TonalControls c;
    c.curve = 1;  // Harman
    c.trimDb = 0.0f;
    c.lowDb = 3.0f;
    FusedTonal t;
    t.prepare(kSr);
    t.setControls(c);
    ASSERT_TRUE(t.evoReady());

    // 100 Hz (lo mueve lowDb: bandas 31.5/63/125 Hz) + 1 kHz.
    std::vector<double> inL(kFrames), inR(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        const double ph = 2.0 * 3.141592653589793 * 100.0 * i / kSr;
        const double ph1 = 2.0 * 3.141592653589793 * 1000.0 * i / kSr;
        inL[i] = 0.35 * std::sin(ph) + 0.35 * std::sin(ph1);
        inR[i] = inL[i];
    }
    auto outL = inL, outR = inR;
    t.process(outL.data(), outR.data(), kFrames);
    EXPECT_TRUE(allFiniteBounded(outL, 4.0));
    EXPECT_TRUE(allFiniteBounded(outR, 4.0));
    EXPECT_LT(peakAbs(outL.data(), kFrames), 4.0);
    // El EQ actúa: la salida difiere de la entrada y el RMS sube
    // (lowDb +3 dB sobre la componente de 100 Hz).
    const NullResult n = nullTest(inL.data(), outL.data(), kFrames);
    ASSERT_TRUE(n.valid);
    EXPECT_FALSE(n.bitExact);
    EXPECT_GT(n.residualDb, -60.0) << "el EQ no cambió la señal";
    EXPECT_GT(rmsOf(outL), rmsOf(inL) * 1.05);
}

// 6. THD+N en neutro: el piso aritmético (bit-exacto ⇒ −320 dB).
TEST(FusedTonal, ThdAtNeutral) {
    FusedTonal t; prepareNeutral(t);
    auto l = tone1k(0.707);
    auto r = tone1k(0.707);
    t.process(l.data(), r.data(), kFrames);
    const ThdDoubleResult thd = measureThdDouble(l.data(), kFrames, kSr, 1000.0);
    std::cout << "[ThdAtNeutral] THD+N = " << thd.thdPlusNoiseDb << " dB"
              << " (valid=" << thd.valid << ")\n";
    ASSERT_TRUE(thd.valid);
    EXPECT_LE(thd.thdPlusNoiseDb, -200.0);
    const NullResult n = nullTest(tone1k(0.707).data(), l.data(), kFrames);
    EXPECT_TRUE(n.bitExact);
}

// 7. El trim es la ÚNICA ganancia neta (Matched-LUFS).
TEST(FusedTonal, TrimIsOnlyNetGain) {
    // (a) trim +3 dB, todo lo demás neutro: RMS +3 dB ±0.5 dB.
    {
        TonalControls c;
        c.curve = 0;
        c.trimDb = 3.0f;
        FusedTonal t;
        t.prepare(kSr);
        t.setControls(c);
        auto l = tone1k(0.5), r = tone1k(0.5);
        const double rmsIn = rmsOf(l);
        t.process(l.data(), r.data(), kFrames);
        const double deltaDb = 20.0 * std::log10(rmsOf(l) / rmsIn);
        EXPECT_NEAR(deltaDb, 3.0, 0.5) << "RMS in=" << rmsIn
                                        << " RMS out=" << rmsOf(l);
        EXPECT_TRUE(allFiniteBounded(l, 1.5));
    }
    // (b) trim 0 dB, todo neutro: RMS idéntico.
    {
        TonalControls c;
        c.curve = 0;
        c.trimDb = 0.0f;
        FusedTonal t;
        t.prepare(kSr);
        t.setControls(c);
        EXPECT_TRUE(t.isNeutral());
        auto l = tone1k(0.5), r = tone1k(0.5);
        const double rmsIn = rmsOf(l);
        t.process(l.data(), r.data(), kFrames);
        EXPECT_NEAR(rmsOf(l), rmsIn, 1e-12);
    }
}

// 8. Extra: bloques de longitud no múltiplo de 128 (la cola real se
// acarrea entre llamadas — FALLO 1, 2026-11-18 — jamás se rellena con
// ceros dentro del DSP; el cebado inicial solo pone ceros a la salida).
TEST(FusedTonal, OddFrameCountIsFinite) {
    TonalControls c;
    c.curve = 1;
    c.trimDb = 3.0f;
    c.lowDb = 3.0f;
    FusedTonal t;
    t.prepare(kSr);
    t.setControls(c);
    constexpr int n = 1000;  // 7×128 + 104
    std::vector<double> l(n), r(n);
    fillToneDouble(l.data(), n, kSr, 1000.0, 0.5);
    fillToneDouble(r.data(), n, kSr, 1000.0, 0.5);
    t.process(l.data(), r.data(), n);
    EXPECT_TRUE(allFiniteBounded(l, 1.5));
    EXPECT_TRUE(allFiniteBounded(r, 1.5));
}

// 9. FALLO 1 (2026-11-18, HAL 192 frames — CRÍTICO): procesar en trozos
// de 192 (128+64) jamás debe meter ceros en el historial FIR del EvoEQ
// ni en las envolventes IIR del psicoacústico. Si lo hiciera, cada bloque
// de 192 envenenaría el estado y la salida en trozos diferiría de la de
// una sola pasada aunque se alineara por el retardo. Con el carry-over,
// la salida en trozos de 192 es BIT-IDÉNTICA a la de una sola pasada
// salvo (a) los primeros 128 frames (sin retardo aún), (b) el cebado
// inicial de 64 ceros a la salida en [128, 192), y (c) la cola final de
// 64 frames, que queda pendiente en el carry al terminar el flujo.
// Por eso se compara la zona estable [192, kFrames-64) con la referencia
// desplazada 64 frames: out[i] == ref[i-64].
TEST(FusedTonal, Hal192FrameChunksMatchOneShot) {
    TonalControls c;
    c.curve = 1;  // Harman (EvoEQ activo: FIR con historial largo).
    c.trimDb = 0.0f;
    c.lowDb = 3.0f;
    c.midDb = 2.0f;
    c.highDb = 2.0f;
    // Ruido blanco: banda ancha, ejercita todo el historial FIR/IIR
    // (un tono puro podría esconder el envenenado por ceros).
    const auto inL = whiteNoise(0.5);
    const auto inR = whiteNoise(0.5);

    // Referencia: una sola pasada de 48000 frames (= 375×128 exactos).
    FusedTonal ref;
    ref.prepare(kSr);
    ref.setControls(c);
    auto refL = inL, refR = inR;
    ref.process(refL.data(), refR.data(), kFrames);

    // Flujo HAL: 250 trozos de 192 frames (250×192 = 48000).
    FusedTonal t;
    t.prepare(kSr);
    t.setControls(c);
    auto outL = inL, outR = inR;
    for (int off = 0; off < kFrames; off += 192) {
        t.process(outL.data() + off, outR.data() + off, 192);
    }
    EXPECT_TRUE(allFiniteBounded(outL, 1.5));
    EXPECT_TRUE(allFiniteBounded(outR, 1.5));

    // (a) Primeros 128 frames: sin retardo aún, bit-idénticos.
    for (int i = 0; i < 128; ++i) {
        EXPECT_DOUBLE_EQ(outL[i], refL[i]) << "i=" << i;
        EXPECT_DOUBLE_EQ(outR[i], refR[i]) << "i=" << i;
    }
    // (b) Cebado inicial: 64 ceros SOLO a la salida, jamás al DSP.
    for (int i = 128; i < 192; ++i) {
        EXPECT_DOUBLE_EQ(outL[i], 0.0) << "i=" << i;
        EXPECT_DOUBLE_EQ(outR[i], 0.0) << "i=" << i;
    }
    // (c) Zona estable: bit-idéntica a la referencia con retardo de 64.
    // Si un solo cero hubiera entrado al FIR/IIR, aquí divergería.
    double maxDiff = 0.0;
    for (int i = 192; i < kFrames - 64; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(outL[i] - refL[i - 64]));
        maxDiff = std::max(maxDiff, std::fabs(outR[i] - refR[i - 64]));
    }
    EXPECT_LT(maxDiff, 1e-12) << "la salida en trozos de 192 diverge de "
                                  "la referencia: ceros en el estado DSP";
}
