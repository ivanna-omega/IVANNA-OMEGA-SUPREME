// test_param_ramp_s12.cpp — S12/510: fundido de aplicación por hop (slew).
//
// Qué verifica: al aplicar un perfil descargado con setControlsRamped(),
// la FusedChain interpola cada parámetro continuo (EQ por banda, trim,
// compresor…) desde el valor vigente hasta el objetivo en N hops de 128
// muestras, ejecutada por el hilo de audio como parte de su trabajo por hop
// existente — sin trabajo nuevo en el hilo de audio y sin escalón audible.
//   - La rampa es monótona por campo y avanza exactamente ≤0.5 dB por hop
//     en los parámetros en dB (regla conservadora §S12; la v1.1 — +2/−2 dB
//     por banda, trim −4 dB, umbral del compresor −12 → −24 dB — pide
//     24 hops, ~64 ms @ 48 kHz; el umbral domina el cálculo).
//   - Tras exactamente N hops los controles SON el objetivo bit-exacto
//     (snap final, sin deriva float) y la rampa se desactiva.
//   - Con rampa de longitud 0 (o negativa) el comportamiento es el anterior
//     exacto: aplicación inmediata, sin fundido.
//   - Un push nuevo supersede la rampa (gana el último) y una rampa
//     re-programada re-basa desde el valor interpolado vigente: el motor
//     llega al objetivo o vuelve al valor anterior, nunca queda a medias.
//   - La rampa sobrevive a bloques grandes (un solo process() de 1024
//     muestras consume 8 hops) y un cambio de modo sigue yendo por el
//     fundido de modo existente, no por la rampa.
//   - La salida durante la rampa es finita y no supera el techo final
//     (−0.1 dBFS): la puerta A/B «sin clip» se evalúa durante la ventana
//     de aplicación (el enganche de telemetría es paramRampActive() /
//     paramRampRemainingHops(); la medición física levelMaxDbfs + escucha
//     de Esse en la primera aplicación real de la v1.1 queda PENDIENTE).
//
// Compilación: ivanna_add_test(test_param_ramp_s12) + ${FUSED_CHAIN_SOURCES}
// (mismo patrón que test_tronidos_layer_step).
#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"

using namespace ivanna::fused;

namespace {

constexpr double kFs = 48000.0;
constexpr int kHop = 128;  // hop del motor (muestras)

// Estado vigente: IVANNA enganchada con EQ +2 dB en graves, sin trim,
// compresor −12 dB / 2:1 (mismos valores que engagedControls() de
// test_tronidos_layer_step / test_fused_chain).
FusedControls currentControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.field.amount = 0.5f;
    c.field.omniAmount = 0.5f;
    c.field.phiAmount = 0.5f;
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 1.0f;
    c.tonal.curve = 1;
    c.tonal.lowDb = 2.0f;
    c.dynamics.compRatio = 2.0f;
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.safety = 1.0f;
    c.brain.mode = 0;  // AUTO
    c.brain.adaptation = 1.0f;
    c.clamp();
    return c;
}

// Objetivo estilo v1.1: EQ por banda +2/−2 dB, presencia +2 dB, trim −4 dB,
// compresor −24 dB / 4:1. El mayor |Δ| en dB es el umbral del compresor
// (12 dB: −12 → −24) → ceil(12 / 0.5) = 24 hops (~64 ms @ 48 kHz).
FusedControls targetControls() {
    FusedControls c = currentControls();
    c.tonal.lowDb = -2.0f;
    c.tonal.midDb = 2.0f;
    c.tonal.highDb = 2.0f;
    c.tonal.presenceDb = 2.0f;
    c.tonal.trimDb = -4.0f;
    c.dynamics.compThresholdDb = -24.0f;
    c.dynamics.compRatio = 4.0f;
    c.clamp();
    return c;
}

// Tono 1 kHz a −3 dBFS, continuo (sin transitorios de entrada).
void fillTone(std::vector<double>& l, std::vector<double>& r, std::size_t n) {
    l.assign(n, 0.0);
    r.assign(n, 0.0);
    for (std::size_t i = 0; i < n; ++i) {
        const double s = 0.7071 * std::sin(2.0 * 3.14159265358979323846 * 1000.0 *
                                           static_cast<double>(i) / kFs);
        l[i] = s;
        r[i] = s;
    }
}

void processHops(FusedChain& chain, std::vector<double>& l, std::vector<double>& r,
                 int hops) {
    for (int h = 0; h < hops; ++h) {
        chain.process(l.data(), r.data(), kHop);
    }
}

double lerpD(double a, double b, double t) { return a + (b - a) * t; }

}  // namespace

TEST(ParamRampS12, RampInterpolatesMonotonicallyOverExpectedHops) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    const FusedControls to = targetControls();
    chain.setControls(from);
    std::vector<double> l, r;
    fillTone(l, r, static_cast<std::size_t>(kHop));
    processHops(chain, l, r, 8);  // asentar (wets de enganche a objetivo)

    // La v1.1 pide 24 hops por la regla ≤0.5 dB/hop (máx |Δ| = 12 dB del
    // umbral del compresor; el trim aporta 4 dB y las bandas 4 dB).
    ASSERT_EQ(FusedChain::rampHopsFor(from, to), 24);
    chain.setControlsRamped(to, 24);
    ASSERT_TRUE(chain.paramRampActive());
    ASSERT_EQ(chain.paramRampRemainingHops(), 24);

    double prevLow = from.tonal.lowDb;
    double prevTrim = from.tonal.trimDb;
    double prevThr = from.dynamics.compThresholdDb;
    double prevPres = from.tonal.presenceDb;
    for (int hop = 1; hop <= 24; ++hop) {
        chain.process(l.data(), r.data(), kHop);
        const FusedControls c = chain.controls();
        const double t = static_cast<double>(hop) / 24.0;
        // Valor EXACTO del lerp en ese hop (misma aritmética que la cadena).
        EXPECT_FLOAT_EQ(c.tonal.lowDb,
                        static_cast<float>(lerpD(from.tonal.lowDb, to.tonal.lowDb, t)))
            << "hop " << hop;
        EXPECT_FLOAT_EQ(c.tonal.trimDb,
                        static_cast<float>(lerpD(from.tonal.trimDb, to.tonal.trimDb, t)))
            << "hop " << hop;
        EXPECT_FLOAT_EQ(c.dynamics.compThresholdDb,
                        static_cast<float>(lerpD(from.dynamics.compThresholdDb,
                                                 to.dynamics.compThresholdDb, t)))
            << "hop " << hop;
        EXPECT_FLOAT_EQ(c.tonal.presenceDb,
                        static_cast<float>(lerpD(from.tonal.presenceDb,
                                                 to.tonal.presenceDb, t)))
            << "hop " << hop;
        // Monótona por campo (low/trim/umbral bajan, presencia sube).
        EXPECT_LE(c.tonal.lowDb, prevLow + 1e-6f) << "hop " << hop;
        EXPECT_LE(c.tonal.trimDb, prevTrim + 1e-6f) << "hop " << hop;
        EXPECT_LE(c.dynamics.compThresholdDb, prevThr + 1e-6f) << "hop " << hop;
        EXPECT_GE(c.tonal.presenceDb, prevPres - 1e-6f) << "hop " << hop;
        // Paso por hop ≤ 0.5 dB en los parámetros en dB (regla §S12).
        EXPECT_LE(std::fabs(c.tonal.lowDb - prevLow), 0.5f + 1e-4f) << "hop " << hop;
        EXPECT_LE(std::fabs(c.tonal.trimDb - prevTrim), 0.5f + 1e-4f) << "hop " << hop;
        EXPECT_LE(std::fabs(c.dynamics.compThresholdDb - prevThr), 0.5f + 1e-4f)
            << "hop " << hop;
        prevLow = c.tonal.lowDb;
        prevTrim = c.tonal.trimDb;
        prevThr = c.dynamics.compThresholdDb;
        prevPres = c.tonal.presenceDb;
        EXPECT_EQ(chain.paramRampRemainingHops(), 24 - hop) << "hop " << hop;
        EXPECT_EQ(chain.paramRampActive(), hop < 24) << "hop " << hop;
    }
    // Snap final EXACTO al objetivo (incluye los campos discretos) y la
    // rampa se desactiva: más bloques no mueven los controles.
    const FusedControls settled = chain.controls();
    EXPECT_FLOAT_EQ(settled.tonal.lowDb, to.tonal.lowDb);
    EXPECT_FLOAT_EQ(settled.tonal.midDb, to.tonal.midDb);
    EXPECT_FLOAT_EQ(settled.tonal.highDb, to.tonal.highDb);
    EXPECT_FLOAT_EQ(settled.tonal.presenceDb, to.tonal.presenceDb);
    EXPECT_FLOAT_EQ(settled.tonal.trimDb, to.tonal.trimDb);
    EXPECT_FLOAT_EQ(settled.dynamics.compThresholdDb, to.dynamics.compThresholdDb);
    EXPECT_FLOAT_EQ(settled.dynamics.compRatio, to.dynamics.compRatio);
    EXPECT_EQ(settled.tonal.curve, to.tonal.curve);
    EXPECT_EQ(settled.spatial.mode, to.spatial.mode);
    processHops(chain, l, r, 4);
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, to.tonal.lowDb);
    EXPECT_FALSE(chain.paramRampActive());
}

TEST(ParamRampS12, ZeroLengthRampBehavesAsBefore) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    const FusedControls to = targetControls();
    chain.setControls(from);

    // Longitud 0 = aplicación inmediata, SIN fundido y SIN process().
    chain.setControlsRamped(to, 0);
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, to.tonal.lowDb);
    EXPECT_FLOAT_EQ(chain.controls().tonal.trimDb, to.tonal.trimDb);
    EXPECT_FLOAT_EQ(chain.controls().dynamics.compThresholdDb,
                    to.dynamics.compThresholdDb);

    // Longitud negativa = lo mismo (fail-closed al camino anterior).
    chain.setControls(from);
    chain.setControlsRamped(to, -5);
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, to.tonal.lowDb);
}

TEST(ParamRampS12, RampHopsForRespectsHalfDbPerHop) {
    const FusedControls from = currentControls();
    FusedControls to = targetControls();
    EXPECT_EQ(FusedChain::rampHopsFor(from, to), 24);  // máx |Δ| 12 dB / 0.5

    EXPECT_EQ(FusedChain::rampHopsFor(from, from), 0);  // sin cambio

    FusedControls small = from;
    small.tonal.lowDb = 2.2f;  // Δ 0.2 dB → 1 hop
    EXPECT_EQ(FusedChain::rampHopsFor(from, small), 1);

    FusedControls amountsOnly = from;
    amountsOnly.field.amount = 1.0f;  // sin cambio en dB → precedente ~18 ms
    EXPECT_EQ(FusedChain::rampHopsFor(from, amountsOnly),
              FusedChain::kParamRampDefaultHops);

    FusedControls huge = from;
    huge.tonal.trimDb = 24.0f;  // Δ 24 dB → 48 hops, tope 32
    EXPECT_EQ(FusedChain::rampHopsFor(from, huge), FusedChain::kParamRampMaxHops);

    // Solo cambia un campo discreto (curva): la rampa no tiene nada que
    // interpolar — aplicación inmediata (0 hops).
    FusedControls discreteOnly = from;
    discreteOnly.tonal.curve = 2;
    EXPECT_EQ(FusedChain::rampHopsFor(from, discreteOnly), 0);
}

TEST(ParamRampS12, NewPushSupersedesRamp) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    const FusedControls to = targetControls();
    FusedControls other = from;
    other.tonal.lowDb = 0.0f;
    other.tonal.trimDb = -1.0f;
    other.clamp();
    chain.setControls(from);
    std::vector<double> l, r;
    fillTone(l, r, static_cast<std::size_t>(kHop));
    processHops(chain, l, r, 8);

    chain.setControlsRamped(to, 8);
    processHops(chain, l, r, 3);  // a medias: low = 2 − 4·3/8 = 0.5
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, 0.5f);
    ASSERT_TRUE(chain.paramRampActive());

    // Un push explícito nuevo gana: inmediato, rampa cancelada, el motor
    // termina en el objetivo más reciente, nunca a medias.
    chain.setControls(other);
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, other.tonal.lowDb);
    EXPECT_FLOAT_EQ(chain.controls().tonal.trimDb, other.tonal.trimDb);
    processHops(chain, l, r, 8);
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, other.tonal.lowDb);
}

TEST(ParamRampS12, ReprogrammedRampRebasesFromCurrentValue) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    const FusedControls to = targetControls();
    FusedControls third = from;
    third.tonal.lowDb = -1.0f;   // desde 0.5: Δ 1.5 dB en 4 hops → 0.375/hop
    third.tonal.trimDb = -2.0f;  // desde −1.5: Δ 0.5 dB en 4 hops
    third.dynamics.compThresholdDb = -18.0f;  // desde −16.5: Δ 1.5 dB
    third.clamp();
    chain.setControls(from);
    std::vector<double> l, r;
    fillTone(l, r, static_cast<std::size_t>(kHop));
    processHops(chain, l, r, 8);

    chain.setControlsRamped(to, 8);
    processHops(chain, l, r, 3);
    ASSERT_FLOAT_EQ(chain.controls().tonal.lowDb, 0.5f);
    ASSERT_FLOAT_EQ(chain.controls().tonal.trimDb, -1.5f);

    // Re-programar re-basa `from` en el valor interpolado vigente: la
    // segunda rampa es monótona desde 0.5/−1.5, no desde el `from` original.
    chain.setControlsRamped(third, 4);
    ASSERT_TRUE(chain.paramRampActive());
    double prevLow = 0.5;
    for (int hop = 1; hop <= 4; ++hop) {
        chain.process(l.data(), r.data(), kHop);
        const FusedControls c = chain.controls();
        EXPECT_LE(c.tonal.lowDb, prevLow + 1e-6f) << "hop " << hop;
        EXPECT_LE(std::fabs(c.tonal.lowDb - prevLow), 0.5f + 1e-4f) << "hop " << hop;
        prevLow = c.tonal.lowDb;
    }
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, third.tonal.lowDb);
    EXPECT_FLOAT_EQ(chain.controls().tonal.trimDb, third.tonal.trimDb);
    EXPECT_FLOAT_EQ(chain.controls().dynamics.compThresholdDb,
                    third.dynamics.compThresholdDb);
}

TEST(ParamRampS12, RampSurvivesLargeBlocks) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    const FusedControls to = targetControls();
    chain.setControls(from);
    std::vector<double> l, r;
    fillTone(l, r, 1024);
    processHops(chain, l, r, 8);

    // UN solo process() de 1024 muestras = 8 hops: la rampa se consume
    // entera dentro de la llamada (partición por hop dentro del chunk).
    chain.setControlsRamped(to, 8);
    chain.process(l.data(), r.data(), 1024);
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_EQ(chain.paramRampRemainingHops(), 0);
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, to.tonal.lowDb);
    EXPECT_FLOAT_EQ(chain.controls().tonal.trimDb, to.tonal.trimDb);
    EXPECT_FLOAT_EQ(chain.controls().dynamics.compThresholdDb,
                    to.dynamics.compThresholdDb);
}

TEST(ParamRampS12, ModeChangeStillUsesModeFadeNotRamp) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    FusedControls to = targetControls();
    to.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;  // cambio de ruta
    to.clamp();
    chain.setControls(from);
    std::vector<double> l, r;
    fillTone(l, r, static_cast<std::size_t>(kHop));
    processHops(chain, l, r, 8);

    // El cambio de modo NO se rampea: delega en el fundido de modo
    // existente (pendiente, controles viejos hasta ganancia 0).
    chain.setControlsRamped(to, 8);
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_EQ(chain.controls().spatial.mode, ivanna::OmegaOutputMode::HEADPHONES);
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, from.tonal.lowDb);

    // Tras el fundido (OUT 256 + IN 512 + margen) el modo nuevo aplica.
    processHops(chain, l, r, 16);
    EXPECT_EQ(chain.controls().spatial.mode, ivanna::OmegaOutputMode::SPEAKERS);
    EXPECT_FLOAT_EQ(chain.controls().tonal.lowDb, to.tonal.lowDb);
    EXPECT_FLOAT_EQ(chain.controls().tonal.trimDb, to.tonal.trimDb);
}

TEST(ParamRampS12, RampOutputStaysFiniteBelowCeiling) {
    FusedChain chain;
    chain.prepare(kFs);
    const FusedControls from = currentControls();
    const FusedControls to = targetControls();
    chain.setControls(from);
    // Tono caliente (−3 dBFS) CONTINUO: el peor caso para un transitorio
    // de aplicación es un pico por encima de 0 dBFS en la ventana del
    // cambio. Se procesa por rebanadas de 128 muestras del mismo buffer.
    std::vector<double> l, r;
    fillTone(l, r, 32 * static_cast<std::size_t>(kHop));
    for (int hop = 0; hop < 8; ++hop) {
        chain.process(l.data() + hop * kHop, r.data() + hop * kHop, kHop);
    }

    // AUTO = 24 hops por la regla ≤0.5 dB/hop (umbral del compresor 12 dB).
    ASSERT_EQ(FusedChain::rampHopsFor(from, to), 24);
    chain.setControlsRamped(to, FusedChain::rampHopsFor(from, to));
    double peak = 0.0;
    for (int hop = 8; hop < 32; ++hop) {
        chain.process(l.data() + hop * kHop, r.data() + hop * kHop, kHop);
        for (int i = 0; i < kHop; ++i) {
            const double vl = l[static_cast<std::size_t>(hop * kHop + i)];
            const double vr = r[static_cast<std::size_t>(hop * kHop + i)];
            ASSERT_TRUE(std::isfinite(vl)) << "hop " << hop << " i " << i;
            ASSERT_TRUE(std::isfinite(vr)) << "hop " << hop << " i " << i;
            peak = std::max({peak, std::fabs(vl), std::fabs(vr)});
        }
    }
    // Techo final −0.1 dBFS (0.9885531): ningún chunk de la ventana de
    // aplicación llega a 0 dBFS. La puerta A/B levelMaxDbfs < 0 dBFS en
    // el teléfono (S13) es la que lo certifica en vivo; aquí se fija que
    // la rampa no introduce el clip por sí misma.
    EXPECT_LE(peak, 0.9885531 + 1e-9) << "peak " << peak;
    EXPECT_FALSE(chain.paramRampActive());
    EXPECT_FLOAT_EQ(chain.controls().tonal.trimDb, to.tonal.trimDb);
}
