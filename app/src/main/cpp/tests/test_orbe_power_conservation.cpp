// test_orbe_power_conservation.cpp — Fase 1 (consejo, plan 5D):
// conservación de potencia por banda L1/L2 en OrbeBinaural (fix del
// defecto M1: ORBE perdía −2.05 dB con diffuse=0.5, lado S −6.02 dB;
// ver analysis-portal5d/FASE0_M1_MEDICION.md).
//
// El flag IVANNA_ORBE_POWER_CONSERVATION es ON por defecto (orden Esse
// 2026-11-25, todo activo en la 502): TODA la suite y producción corren
// el camino ON. Este target lo ejercita directamente (su -D=1 es
// redundante con el default, se conserva como cinturón); el camino OFF
// (legacy, −2.05 dB) ya no se compila por defecto en ningún sitio.
//
// Límites informados por la medición (harness m1_measure, 48 kHz,
// ventana 1–2 s, g++-13): con el fix ON, decorr a=1 tot −0.01 dB,
// side a=1 S +0.00 dB, corr08 a=1 tot −0.02 dB, decorr a=0.5 tot
// −0.01 dB, núcleo double idéntico. El límite ±0.05 dB deja margen
// para variación de compilador; no es un número inventado.
//
// NOTA HONESTA: el conservador garantiza la potencia TOTAL por banda
// (L+R), no M y S por separado. Con contenido tipo música (corr08),
// el render ILD/ITD fuga M→S y el lado S GANA +1.55 dB mientras M
// pierde −0.23 dB (total −0.02 dB). Si ese reparto M/S audible es
// deseable o no, lo decide ABX — este test solo fija la conservación
// total (Regla de Oro: ganancia neta 0.0 dB por procesador).
#include <gtest/gtest.h>
#include <cmath>
#include <random>
#include <vector>
#include "../spatial/BandPowerConserver.hpp"
#include "../spatial/OrbeBinaural.hpp"

namespace {
constexpr float kFs = 48000.0f;
constexpr int kDur = 96000;        // 2 s @48 kHz
constexpr int kMeasFrom = 48000;   // ventana de medición: 2ª mitad
constexpr int kMeasTo = 96000;

double db10(double r) { return 10.0 * std::log10(r); }

double power(const std::vector<float>& v, int from, int to) {
    double a = 0.0;
    for (int i = from; i < to; ++i) a += (double)v[i] * v[i];
    return a / (double)(to - from);
}

void runBlocks(ivanna::OrbeBinaural& o, std::vector<float>& l, std::vector<float>& r) {
    for (size_t s = 0; s < l.size(); s += 128) {
        const int n = (int)std::min<size_t>(128, l.size() - s);
        o.process(l.data() + s, r.data() + s, n);
    }
}

struct Sig { std::vector<float> l, r; };

Sig makeSignal(const std::string& kind) {
    Sig s;
    s.l.assign(kDur, 0.0f);
    s.r.assign(kDur, 0.0f);
    if (kind == "decorr") {
        std::mt19937 gL(1), gR(2);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (int i = 0; i < kDur; ++i) { s.l[i] = u(gL); s.r[i] = u(gR); }
    } else if (kind == "side") {
        std::mt19937 g(3);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (int i = 0; i < kDur; ++i) { float n = u(g); s.l[i] = n; s.r[i] = -n; }
    } else if (kind == "mid") {
        std::mt19937 g(4);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (int i = 0; i < kDur; ++i) { float n = u(g); s.l[i] = n; s.r[i] = n; }
    } else if (kind == "corr08") {
        const float a = std::sqrt(0.8f), b = std::sqrt(0.2f);
        std::mt19937 gC(5), gL(6), gR(7);
        std::uniform_real_distribution<float> u(-0.5f, 0.5f);
        for (int i = 0; i < kDur; ++i) {
            float c = u(gC);
            s.l[i] = a * c + b * u(gL);
            s.r[i] = a * c + b * u(gR);
        }
    }
    return s;
}

// Ganancia neta total (L+R) en dB, salida/entrada, ventana 1–2 s.
double netGainDb(const Sig& in, float amount, float diffuse) {
    ivanna::OrbeBinaural o;
    o.prepare(kFs);
    ivanna::OrbeParams p;
    p.amount = amount;
    p.diffuse = diffuse;
    p.distanceM = 0.35f;
    p.spread = 1.0f;
    p.proximityDb = 3.0f;
    p.headTracking = true;
    p.bandCount = 24;
    o.setParams(p);
    std::vector<float> l = in.l, r = in.r;
    runBlocks(o, l, r);
    const double pIn = power(in.l, kMeasFrom, kMeasTo) + power(in.r, kMeasFrom, kMeasTo);
    const double pOut = power(l, kMeasFrom, kMeasTo) + power(r, kMeasFrom, kMeasTo);
    return db10(pOut / pIn);
}

// Ganancia del lado S (L−R)/2 en dB, salida/entrada.
double sideGainDb(const Sig& in, float amount, float diffuse) {
    ivanna::OrbeBinaural o;
    o.prepare(kFs);
    ivanna::OrbeParams p;
    p.amount = amount;
    p.diffuse = diffuse;
    p.distanceM = 0.35f;
    p.spread = 1.0f;
    p.proximityDb = 3.0f;
    p.headTracking = true;
    p.bandCount = 24;
    o.setParams(p);
    std::vector<float> l = in.l, r = in.r;
    runBlocks(o, l, r);
    double pSIn = 0.0, pSOut = 0.0;
    for (int i = kMeasFrom; i < kMeasTo; ++i) {
        const double sIn = 0.5 * ((double)in.l[i] - in.r[i]);
        const double sOut = 0.5 * ((double)l[i] - r[i]);
        pSIn += sIn * sIn;
        pSOut += sOut * sOut;
    }
    return db10(pSOut / pSIn);
}
}  // namespace

TEST(OrbePowerConservation, FlagIsOn) {
    // Este target solo tiene sentido con el flag ON; si alguien lo
    // compila sin -DIVANNA_ORBE_POWER_CONSERVATION=1, que caiga en vez
    // de dar un verde falso (patrón test_poincare_pan).
    EXPECT_TRUE(ivanna::OrbeBinaural::kOrbePowerConservation);
}

TEST(OrbePowerConservation, UncorrelatedNoiseNetGainNearZero) {
    // Fase 0: sin el fix, −2.05 dB (defecto M1). Con el fix: −0.01 dB.
    const Sig s = makeSignal("decorr");
    EXPECT_NEAR(netGainDb(s, 1.0f, 0.5f), 0.0, 0.05);
}

TEST(OrbePowerConservation, PureSideIsPreserved) {
    // Fase 0: sin el fix, el lado S puro pierde −6.02 dB (= d²).
    // Con el fix: S +0.00 dB, total +0.00 dB.
    const Sig s = makeSignal("side");
    EXPECT_NEAR(sideGainDb(s, 1.0f, 0.5f), 0.0, 0.05);
    EXPECT_NEAR(netGainDb(s, 1.0f, 0.5f), 0.0, 0.05);
}

TEST(OrbePowerConservation, MonoIsPreserved) {
    const Sig s = makeSignal("mid");
    EXPECT_NEAR(netGainDb(s, 1.0f, 0.5f), 0.0, 0.05);
}

TEST(OrbePowerConservation, MusicLikeNetGainNearZero) {
    // Contenido tipo música (corr08, μ=0.9): sin el fix −0.36 dB;
    // con el fix −0.02 dB. El total se conserva aunque el reparto M/S
    // cambie (S +1.55 dB por fuga M→S del ILD/ITD — ver nota arriba).
    const Sig s = makeSignal("corr08");
    EXPECT_NEAR(netGainDb(s, 1.0f, 0.5f), 0.0, 0.05);
}

TEST(OrbePowerConservation, AmountHalfStillConservesPower) {
    // L2: la Cantidad escala la señal física (aLaw), no la mezcla:
    // a=0.5 también conserva la potencia total (medido −0.01 dB).
    const Sig s = makeSignal("decorr");
    EXPECT_NEAR(netGainDb(s, 0.5f, 0.5f), 0.0, 0.05);
}

TEST(OrbePowerConservation, DiffuseParameterHasNoEffectOnSide) {
    // L2: side = 1 siempre, el parámetro diffuse deja de atenuar S.
    // Con d=0.0 el S puro ya no se bloquea (antes: −inf dB).
    const Sig s = makeSignal("side");
    EXPECT_NEAR(sideGainDb(s, 1.0f, 0.0f), 0.0, 0.05);
    EXPECT_NEAR(sideGainDb(s, 1.0f, 1.0f), 0.0, 0.05);
}

TEST(OrbePowerConservation, AmountZeroIsBitExactBypass) {
    // El bypass con amount=0 sigue siendo bit-exacto con el flag ON.
    ivanna::OrbeBinaural o;
    o.prepare(kFs);
    o.setAmount(0.0f);
    std::mt19937 g(1);
    std::uniform_real_distribution<float> u(-1, 1);
    std::vector<float> l(48000), r(48000);
    for (auto& v : l) v = u(g);
    for (auto& v : r) v = u(g);
    auto l0 = l, r0 = r;
    runBlocks(o, l, r);
    EXPECT_EQ(l, l0);
    EXPECT_EQ(r, r0);
}

TEST(OrbePowerConservation, DoubleCoreNetGainNearZero) {
    // El núcleo double (lo usa FusedSpatial) conserva igual que el float
    // (harness: tot −0.01 dB en ambos).
    ivanna::OrbeBinauralD o;
    o.prepare(kFs);
    ivanna::OrbeParams p;
    p.amount = 1.0f;
    p.diffuse = 0.5f;
    p.distanceM = 0.35f;
    p.spread = 1.0f;
    p.proximityDb = 3.0f;
    p.headTracking = true;
    p.bandCount = 24;
    o.setParams(p);
    const Sig s = makeSignal("decorr");
    std::vector<double> l(s.l.begin(), s.l.end()), r(s.r.begin(), s.r.end());
    for (size_t i = 0; i < l.size(); i += 128) {
        const int n = (int)std::min<size_t>(128, l.size() - i);
        o.process(l.data() + i, r.data() + i, n);
    }
    double pIn = 0.0, pOut = 0.0;
    for (int i = kMeasFrom; i < kMeasTo; ++i) {
        pIn += (double)s.l[i] * s.l[i] + (double)s.r[i] * s.r[i];
        pOut += l[i] * l[i] + r[i] * r[i];
    }
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

TEST(OrbePowerConservation, LegacyPowerRatioAudit) {
    // Forma cerrada del defecto M1 (auditoría sin audio): el render
    // legacy con a=1, μ=½, d=0.5 pierde −2.0412 dB (Fase 0: −2.05 dB).
    const double dB = 10.0 * std::log10(ivanna::nf::legacyPowerRatio(1.0, 0.5, 0.5));
    EXPECT_NEAR(dB, -2.0412, 1e-3);
    // Con side-gain 1.0 (kSideGain) la forma cerrada da 0 dB.
    EXPECT_NEAR(10.0 * std::log10(ivanna::nf::legacyPowerRatio(1.0, 0.5, 1.0)), 0.0, 1e-9);
    EXPECT_FLOAT_EQ(ivanna::nf::kSideGain, 1.0f);
}
