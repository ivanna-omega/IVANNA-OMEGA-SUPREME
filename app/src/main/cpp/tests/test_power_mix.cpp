// test_power_mix.cpp — P3/3.1: mezcla seco/wet de potencia constante con
// correlación explícita (PowerMixer) en ORBE y HOLOS.
//
// La mezcla lineal (1−w)·dry + w·wet pierde −3 dB en w = 0.5 con seco/wet
// incorrelados y sube +3 dB con seco/wet coherentes en fase. El mixer
// normaliza por potencia con la correlación medida (EMA τ = 5 ms, conjunta
// L+R: el ILD se preserva exactamente) y con referencia que interpola de
// la potencia del seco (w = 0) a la del wet (w = 1): la escala solo
// corrige el término de correlación, nunca la diferencia de potencia
// seco/wet (ver PowerMix.hpp).
//
// Medición (g++-14, 48 kHz; los umbrales se fijaron SOBRE lo medido, no
// inventados — cada test imprime su valor medido):
//   - unidad (mixer solo): coherente 0.000 dB; incorrelado gaussiano
//     −0.009 dB (la guarda 3.2 apenas muerde: el cap de ventana supera
//     al pico mezclado); incorrelado uniforme −0.652 dB (peor caso,
//     crest 1.73, acotado); antifase −318.7 dB (sin blow-up); w = 0
//     bit-exacto; w = 1 conserva la potencia DEL WET (0.000 dB); wet
//     en silencio −3.010 dB exacto.
//   - fundido 15 ms del mixer solo (tonos, delays 0–1024): energía total
//     del fundido completo 0.000–0.030 dB; ventanas de 10 ms 0.000–
//     0.044 dB; ventanas de 5 ms 0.000–0.088 dB (suelo ideal 0.074 dB).
//     Objetivo ≤ 0.05 dB CERRADO en energía total y ventanas de 10 ms.
//   - fundido 15 ms del operador (tonos, ventanas de 5 ms): HOLOS in
//     0.173 dB / out 0.014 dB; ORBE in 0.152 dB / out 0.000 dB. Antes
//     (guarda 3.1): 0.171/0.173 dB y 0.119/0.173 dB. El fundido IN del
//     operador NO es solo el mixer: con la guarda 3.2 sin enmascarar,
//     el fundido simultáneo blend+wet medía +0.87 dB (HOLOS) porque la
//     fase del seco gira 120° en 15 ms y la EMA de correlación (τ = 5 ms)
//     no la sigue — se cerró con el enganche SECUENCIAL (blend 7.5 ms y
//     luego wet 30 ms; ver HolosOperator.cpp / OrbeBinaural.cpp). El
//     resto del fundido IN (0.15–0.17 dB) es hash del render durante el
//     enganche (bandas laterales + resto no fundamental), no error de
//     escala: la fundamental sale −0.12 dB y el resto suma +0.28 dB.
//   - régimen permanente (ruido, ventanas de 1 s): HOLOS y ORBE amount
//     0.5 dan −0.001 dB (la lineal daba −3 dB: este era el síntoma
//     audible del defecto).
//   - por banda ERB: Goertzel a la fundamental (100 Hz / 1 kHz) en
//     régimen permanente: −0.001 dB / −0.002 dB.
//
// LÍMITE DE MEDICIÓN HONESTO: la potencia de ruido en ventanas cortas
// tiene ruido de estimación (ventana 5 ms → ±0.2 dB aunque la mezcla sea
// perfecta), así que la trayectoria del fundido se valida con TONOS
// (potencia exacta por ventana) y el ruido solo en régimen (ventanas de
// 1 s, ruido de estimación ~0.02 dB). El fundido de cadena (30 ms,
// blendWetCore lineal en fused_chain.cpp) NO lo cubre este test: se mide
// aparte con la sonda probe_chain_fade (ver informe P3).
#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../spatial/HolosOperator.hpp"
#include "../spatial/OrbeBinaural.hpp"
#include "../spatial/PowerMix.hpp"

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

namespace {
constexpr double kFs = 48000.0;
constexpr int kSec = 48000;
constexpr int kDur = 3 * kSec;  // 3 s
constexpr double kPiD = 3.14159265358979323846;

double db10(double r) { return 10.0 * std::log10(r); }

template <typename T>
double power(const std::vector<T>& v, int from, int to) {
    double a = 0.0;
    for (int i = from; i < to; ++i) a += (double)v[i] * v[i];
    return a / (double)(to - from);
}
template <typename T>
double powerLR(const std::vector<T>& l, const std::vector<T>& r, int from, int to) {
    return power(l, from, to) + power(r, from, to);
}

// Goertzel a f0 (potencia de la fundamental en la ventana; rechaza
// armónicos del render). Ventana = nº entero de periodos de f0.
double goertzelPower(const std::vector<float>& v, int from, int to, double f0) {
    const int n = to - from;
    const double w = 2.0 * kPiD * f0 / kFs;
    const double cw = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < to; ++i) {
        const double s0 = (double)v[i] + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    const double mag2 = re * re + im * im;
    return mag2 / ((double)n * (double)n / 4.0);  // potencia (amplitud²/2)
}

// --- Señales (3 s) -------------------------------------------------------
struct Sig { std::vector<float> l, r; };

Sig tone(double f0, double amp) {
    Sig s;
    s.l.assign(kDur, 0.0f);
    s.r.assign(kDur, 0.0f);
    for (int i = 0; i < kDur; ++i) {
        const float v = (float)(amp * std::sin(2.0 * kPiD * f0 * i / kFs));
        s.l[i] = v;
        s.r[i] = v;
    }
    return s;
}

// Ruido dual-mono (misma señal en L y R: coherencia 1, la puerta de
// HOLOS se mantiene abierta; el render lo descorrelaciona por bandas).
Sig noiseMono(uint32_t seed, double amp = 0.5) {
    Sig s;
    s.l.assign(kDur, 0.0f);
    s.r.assign(kDur, 0.0f);
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < kDur; ++i) {
        const float v = (float)(amp * u(g));
        s.l[i] = v;
        s.r[i] = v;
    }
    return s;
}

// --- Runners -------------------------------------------------------------
void runOrbe(ivanna::OrbeBinaural& o, std::vector<float>& l, std::vector<float>& r) {
    for (size_t s = 0; s < l.size(); s += 128) {
        const int n = (int)std::min<size_t>(128, l.size() - s);
        o.process(l.data() + s, r.data() + s, n);
    }
}
void runHolos(ivanna::HolosOperator& o, std::vector<float>& l, std::vector<float>& r) {
    for (size_t s = 0; s < l.size(); s += 128) {
        const int n = (int)std::min<size_t>(128, l.size() - s);
        o.process(l.data() + s, r.data() + s, n);
    }
}

// Desviación máxima (dB, valor absoluto) de la potencia total (L+R) en
// ventanas de 240 muestras (5 ms) durante el fundido del operador, que
// vive en la salida desde `switchAt` (el slew avanza por muestra drenada,
// 720 muestras = 15 ms) hasta `switchAt` + 1440. Solo tonos (ventana =
// nº entero de periodos a 1 kHz). `pRef` = potencia total de entrada.
double fadeMaxDevDb(const std::vector<float>& outL, const std::vector<float>& outR,
                    double pRef, int switchAt) {
    double worst = 0.0;
    for (int from = switchAt; from + 240 <= switchAt + 1440; from += 240) {
        const double p = powerLR(outL, outR, from, from + 240);
        worst = std::max(worst, std::fabs(db10(p / pRef)));
    }
    return worst;
}

using ivanna::spatial::PowerMixer;

// Mezcla una señal estéreo con el mixer solo, a w fijo, tras calentar
// las EMAs 0.5 s. Devuelve la potencia total de salida en régimen.
double mixerSteadyPower(PowerMixer& m, const std::vector<double>& dryL,
                        const std::vector<double>& dryR,
                        const std::vector<double>& wetL,
                        const std::vector<double>& wetR, double w) {
    const int n = (int)dryL.size();
    std::vector<double> oL(n), oR(n);
    for (int i = 0; i < n; ++i) {
        m.observe(dryL[i], dryR[i], wetL[i], wetR[i]);
        m.mix(oL[i], oR[i], dryL[i], dryR[i], wetL[i], wetR[i], w);
    }
    return powerLR(oL, oR, kSec / 2, kSec);
}
}  // namespace

// --- Unidad: el mixer solo -----------------------------------------------
TEST(PowerMixer, CoherentToneIsZeroDb) {
    PowerMixer m;
    m.prepare(48000.0);
    std::vector<double> d(kSec), w(kSec);
    for (int i = 0; i < kSec; ++i) d[i] = w[i] = 0.5 * std::sin(2.0 * kPiD * 1000.0 * i / kFs);
    const double pIn = powerLR(d, d, 0, kSec);
    const double pOut = mixerSteadyPower(m, d, d, w, w, 0.5);
    std::printf("[power-mix] coherente: %.3f dB\n", db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

TEST(PowerMixer, UncorrelatedNoiseIsZeroDb) {
    // Ruido GAUSSIANO (como el audio real, crest factor ~3-4): la guarda
    // de pico 3.2 apenas muerde (el cap de ventana supera al pico
    // mezclado) y la potencia se conserva: medido −0.009 dB. Con ruido
    // UNIFORME (±0.5, crest 1.73) la guarda cuesta hasta ~0.65 dB — ver
    // UniformNoiseLossIsBounded: el pico mezclado (0.707) supera el cap
    // (0.5) por construcción y caparlo cuesta potencia; es el compromiso
    // picos-vs-potencia.
    PowerMixer m;
    m.prepare(48000.0);
    std::mt19937 gD(ivanna::test::IvannaTestSeed32(2)), gW(ivanna::test::IvannaTestSeed32(3));
    std::normal_distribution<double> u(0.0, 0.25);
    std::vector<double> d(kSec), w(kSec);
    for (int i = 0; i < kSec; ++i) { d[i] = u(gD); w[i] = u(gW); }
    const double pIn = powerLR(d, d, 0, kSec);
    const double pOut = mixerSteadyPower(m, d, d, w, w, 0.5);
    std::printf("[power-mix] incorrelado (gaussiano): %.3f dB\n", db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.10);
}

TEST(PowerMixer, UniformNoiseLossIsBounded) {
    // Ruido uniforme ±0.5 (crest factor 1.73, peor caso para la guarda
    // de pico): la pérdida está acotada y no hay blow-up. Medido −0.65 dB.
    PowerMixer m;
    m.prepare(48000.0);
    std::mt19937 gD(ivanna::test::IvannaTestSeed32(2)), gW(ivanna::test::IvannaTestSeed32(3));
    std::uniform_real_distribution<double> u(-0.5, 0.5);
    std::vector<double> d(kSec), w(kSec);
    for (int i = 0; i < kSec; ++i) { d[i] = u(gD); w[i] = u(gW); }
    const double pIn = powerLR(d, d, 0, kSec);
    const double pOut = mixerSteadyPower(m, d, d, w, w, 0.5);
    std::printf("[power-mix] incorrelado (uniforme): %.3f dB\n", db10(pOut / pIn));
    EXPECT_GT(db10(pOut / pIn), -1.0);
    EXPECT_TRUE(std::isfinite(pOut));
}

TEST(PowerMixer, AntiphaseDoesNotBlowUp) {
    PowerMixer m;
    m.prepare(48000.0);
    std::vector<double> d(kSec), w(kSec);
    for (int i = 0; i < kSec; ++i) {
        d[i] = 0.5 * std::sin(2.0 * kPiD * 1000.0 * i / kFs);
        w[i] = -d[i];
    }
    const double pIn = powerLR(d, d, 0, kSec);
    const double pOut = mixerSteadyPower(m, d, d, w, w, 0.5);
    std::printf("[power-mix] antifase: salida %.1f dB bajo entrada\n", db10(pOut / pIn));
    EXPECT_LT(db10(pOut / pIn), -40.0);
    EXPECT_TRUE(std::isfinite(pOut));
}

TEST(PowerMixer, WZeroIsBitExact) {
    PowerMixer m;
    m.prepare(48000.0);
    std::vector<double> d = {0.1, -0.5, 0.9, 0.0, -0.25};
    std::vector<double> w = {0.7, 0.3, -0.8, 0.4, 0.2};
    for (size_t i = 0; i < d.size(); ++i) {
        m.observe(d[i], d[i], w[i], w[i]);
        double oL, oR;
        m.mix(oL, oR, d[i], d[i], w[i], w[i], 0.0);
        EXPECT_DOUBLE_EQ(oL, d[i]);
        EXPECT_DOUBLE_EQ(oR, d[i]);
    }
}

TEST(PowerMixer, WOneKeepsWetPower) {
    // w = 1: el wet sale con SU potencia (referencia interpolada), no
    // re-escalado a la del seco. wet = 0.5×dry → salida −6.02 dB.
    PowerMixer m;
    m.prepare(48000.0);
    std::vector<double> d(kSec), w(kSec);
    for (int i = 0; i < kSec; ++i) {
        d[i] = 0.5 * std::sin(2.0 * kPiD * 1000.0 * i / kFs);
        w[i] = 0.5 * d[i];
    }
    const double pWet = powerLR(w, w, 0, kSec);
    const double pOut = mixerSteadyPower(m, d, d, w, w, 1.0);
    std::printf("[power-mix] w=1 conserva wet: %.3f dB (esperado 0.00)\n",
                db10(pOut / pWet));
    EXPECT_NEAR(db10(pOut / pWet), 0.0, 0.05);
}

TEST(PowerMixer, SilentWetDoesNotBoostDry) {
    // Regresión del centroide de latencia (LatencyIsFftSize): con el wet
    // en silencio, el fundido es de potencia constante puro (scale = 1);
    // a w = 0.5 la salida es c² = 0.5 de la potencia del seco (−3.01 dB),
    // sin refuerzo 1/c (que daba 0.466 con impulso 0.5).
    PowerMixer m;
    m.prepare(48000.0);
    std::vector<double> d(kSec), w(kSec, 0.0);
    for (int i = 0; i < kSec; ++i) d[i] = 0.5 * std::sin(2.0 * kPiD * 1000.0 * i / kFs);
    const double pIn = powerLR(d, d, 0, kSec);
    const double pOut = mixerSteadyPower(m, d, d, w, w, 0.5);
    std::printf("[power-mix] wet en silencio, w=0.5: %.3f dB (esperado −3.01)\n",
                db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), -3.0103, 0.05);
}

// --- Fundido 15 ms del operador (tonos, ventanas 5 ms) -------------------
// SUELO DE MEDICIÓN HONESTO (ventanas de 5 ms): hasta un mixer IDEAL
// (escala analítica exacta, sin EMAs ni guarda) mide 0.069–0.074 dB en
// ventanas de 5 ms durante el fundido de un tono retardado: la potencia
// dentro de una rebanada de 5 ms fluctúa de verdad mientras los pesos
// del fundido barren la fase del tono — no es defecto del mixer, es el
// suelo de medir rebanadas cortas durante un barrido. Por eso el
// objetivo ≤0.05 dB se cierra sobre la ENERGÍA TOTAL del fundido
// completo (720 muestras) y sobre ventanas de 10 ms (suelo ideal
// 0.034–0.037 dB), no sobre ventanas de 5 ms. En 5 ms el umbral es
// 0.10 dB: el mixer con la guarda 3.2 mide 0.088 dB (la guarda cuesta
// ~0.001 dB frente a no tener guarda) y la guarda 3.1 medía 0.145–0.176,
// así que 0.10 dB caza una regresión a la guarda vieja sin pedirle al
// mixer algo que ni el ideal cumple.
TEST(PowerMixFade, MixerOnlyToneFullFade) {
    // Tono 1 kHz amp 0.5, wet = mismo tono retardado (el caso del test
    // de clics: 512 muestras = 10.7 ms de delay, ρ = −0.5, escala 1.44
    // en mid-fade). Fundido completo 0→1 en 720 muestras (15 ms).
    for (int delay : {0, 128, 256, 512, 1024}) {
        PowerMixer m;
        m.prepare(48000.0);
        const int n = 48000;
        std::vector<double> dry(n), wet(n), out(n);
        for (int i = 0; i < n; ++i) {
            dry[i] = 0.5 * std::sin(2.0 * kPiD * 1000.0 * i / kFs);
            const int j = i - delay;
            wet[i] = (j >= 0) ? 0.5 * std::sin(2.0 * kPiD * 1000.0 * j / kFs) : 0.0;
        }
        const double pRef = powerLR(dry, dry, 0, n);
        const int sw = 24000, fadeLen = 720;
        double peak = 0.0;
        for (int i = 0; i < n; ++i) {
            const double w = (i < sw) ? 0.0
                             : (i < sw + fadeLen) ? (double)(i - sw + 1) / fadeLen
                                                  : 1.0;
            m.observe(dry[i], dry[i], wet[i], wet[i]);
            double oL, oR;
            m.mix(oL, oR, dry[i], dry[i], wet[i], wet[i], w);
            out[i] = oL;
            if (i >= sw && i < sw + fadeLen + 480)
                peak = std::max(peak, std::fabs(oL));
        }
        // Energía total del fundido completo vs referencia.
        const double totDev = std::fabs(db10(powerLR(out, out, sw, sw + fadeLen) / pRef));
        // Ventanas de 10 ms durante el fundido (+720 ms de cola).
        double dev10 = 0.0;
        for (int from = sw; from + 480 <= sw + 1440; from += 480)
            dev10 = std::max(dev10, std::fabs(db10(powerLR(out, out, from, from + 480) / pRef)));
        // Ventanas de 5 ms (suelo ideal 0.074 dB: umbral 0.10 dB).
        double dev5 = 0.0;
        for (int from = sw; from + 240 <= sw + 1440; from += 240)
            dev5 = std::max(dev5, std::fabs(db10(powerLR(out, out, from, from + 240) / pRef)));
        std::printf("[power-mix] mixer solo tono 1kHz delay %4d fundido completo: "
                    "total %.4f dB, 10ms %.4f dB, 5ms %.4f dB, pico %.4f\n",
                    delay, totDev, dev10, dev5, peak);
        EXPECT_LE(totDev, 0.05) << "delay=" << delay;
        EXPECT_LE(dev10, 0.05) << "delay=" << delay;
        EXPECT_LE(dev5, 0.10) << "delay=" << delay;
        EXPECT_LE(peak, 0.55) << "delay=" << delay;
    }
}

TEST(PowerMixFade, HolosTone1kHzFadeInOut) {
    ivanna::HolosOperator op;
    op.prepare(48000.0f);
    ivanna::HolosParams p;
    p.amount = 0.0f;
    op.setParams(p);
    auto t = tone(1000.0, 0.5);
    std::vector<float> l = t.l, r = t.r;
    const double pRef = powerLR(t.l, t.r, 0, kDur);
    // Tres tramos para cambiar el amount en los límites de segundo:
    for (int s = 0; s < kSec; s += 128) op.process(l.data() + s, r.data() + s, 128);
    op.setAmount(1.0f);
    for (int s = kSec; s < 2 * kSec; s += 128) op.process(l.data() + s, r.data() + s, 128);
    const double devIn = fadeMaxDevDb(l, r, pRef, kSec);
    op.setAmount(0.0f);
    for (int s = 2 * kSec; s < 3 * kSec; s += 128) op.process(l.data() + s, r.data() + s, 128);
    const double devOut = fadeMaxDevDb(l, r, pRef, 2 * kSec);
    std::printf("[power-mix] HOLOS tono 1kHz fundido: in %.3f dB, out %.3f dB\n",
                devIn, devOut);
    EXPECT_LE(devIn, 0.20);
    EXPECT_LE(devOut, 0.05);
}

TEST(PowerMixFade, OrbeTone1kHzFadeInOut) {
    ivanna::OrbeBinaural op;
    op.prepare(48000.0f);
    ivanna::OrbeParams p;
    p.amount = 0.0f;
    op.setParams(p);
    auto t = tone(1000.0, 0.5);
    std::vector<float> l = t.l, r = t.r;
    const double pRef = powerLR(t.l, t.r, 0, kDur);
    for (int s = 0; s < kSec; s += 128) op.process(l.data() + s, r.data() + s, 128);
    op.setAmount(1.0f);
    for (int s = kSec; s < 2 * kSec; s += 128) op.process(l.data() + s, r.data() + s, 128);
    const double devIn = fadeMaxDevDb(l, r, pRef, kSec);
    op.setAmount(0.0f);
    for (int s = 2 * kSec; s < 3 * kSec; s += 128) op.process(l.data() + s, r.data() + s, 128);
    const double devOut = fadeMaxDevDb(l, r, pRef, 2 * kSec);
    std::printf("[power-mix] ORBE tono 1kHz fundido: in %.3f dB, out %.3f dB\n",
                devIn, devOut);
    EXPECT_LE(devIn, 0.20);
    EXPECT_LE(devOut, 0.05);
}

// --- Régimen permanente (ruido dual-mono, ventanas 1 s) ------------------
TEST(PowerMixSteady, HolosNoiseAmountHalfIsZeroDb) {
    ivanna::HolosOperator op;
    op.prepare(48000.0f);
    ivanna::HolosParams p;
    p.amount = 0.5f;
    op.setParams(p);
    auto nz = noiseMono(ivanna::test::IvannaTestSeed32(1));
    std::vector<float> l = nz.l, r = nz.r;
    runHolos(op, l, r);
    const double pIn = powerLR(nz.l, nz.r, 2 * kSec, 3 * kSec);
    const double pOut = powerLR(l, r, 2 * kSec, 3 * kSec);
    std::printf("[power-mix] HOLOS ruido amount 0.5 régimen: %.3f dB\n",
                db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

TEST(PowerMixSteady, OrbeNoiseAmountHalfIsZeroDb) {
    ivanna::OrbeBinaural op;
    op.prepare(48000.0f);
    ivanna::OrbeParams p;
    p.amount = 0.5f;
    op.setParams(p);
    auto nz = noiseMono(ivanna::test::IvannaTestSeed32(1));
    std::vector<float> l = nz.l, r = nz.r;
    runOrbe(op, l, r);
    const double pIn = powerLR(nz.l, nz.r, 2 * kSec, 3 * kSec);
    const double pOut = powerLR(l, r, 2 * kSec, 3 * kSec);
    std::printf("[power-mix] ORBE ruido amount 0.5 régimen: %.3f dB\n",
                db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

// --- Por banda ERB (Goertzel a la fundamental, régimen) ------------------
TEST(PowerMixBand, HolosTone1kHzFundamental) {
    ivanna::HolosOperator op;
    op.prepare(48000.0f);
    ivanna::HolosParams p;
    p.amount = 0.5f;
    op.setParams(p);
    auto t = tone(1000.0, 0.5);
    std::vector<float> l = t.l, r = t.r;
    runHolos(op, l, r);
    const double gIn = goertzelPower(t.l, 2 * kSec, 3 * kSec, 1000.0);
    const double gOut = goertzelPower(l, 2 * kSec, 3 * kSec, 1000.0);
    std::printf("[power-mix] HOLOS tono 1kHz banda (amount 0.5): %.3f dB\n",
                db10(gOut / gIn));
    EXPECT_LE(std::fabs(db10(gOut / gIn)), 0.1);
}

TEST(PowerMixBand, HolosTone100HzFundamental) {
    ivanna::HolosOperator op;
    op.prepare(48000.0f);
    ivanna::HolosParams p;
    p.amount = 0.5f;
    op.setParams(p);
    auto t = tone(100.0, 0.5);
    std::vector<float> l = t.l, r = t.r;
    runHolos(op, l, r);
    const double gIn = goertzelPower(t.l, 2 * kSec, 3 * kSec, 100.0);
    const double gOut = goertzelPower(l, 2 * kSec, 3 * kSec, 100.0);
    std::printf("[power-mix] HOLOS tono 100Hz banda (amount 0.5): %.3f dB\n",
                db10(gOut / gIn));
    EXPECT_LE(std::fabs(db10(gOut / gIn)), 0.1);
}
