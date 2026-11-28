// test_holos_operator.cpp — el operador HOLOS completo (G·K·Γ en STFT).
//
// Verifica las garantías de la v1 (FUSION §5, §10 paso 4):
// amount=0 es bit-exacto; mono intacto; crossfade sin clic; estable en
// todas las SR; tiers térmicos; sin NaN/Inf con silencio/DC/ruido.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../spatial/HolosOperator.hpp"
#include "ivanna_measure.hpp"

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

using namespace ivanna;
using namespace ivanna::measure;

namespace {
constexpr double kFs = 48000.0;

std::vector<float> tone(double f0, double amp, double seconds, double sr = kFs) {
    const int n = (int)(seconds * sr);
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = (float)(amp * std::sin(2.0 * kPi * f0 * i / sr));
    return v;
}

std::vector<float> noise(double amp, double seconds, uint32_t seed, double sr = kFs) {
    const int n = (int)(seconds * sr);
    std::vector<float> v(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-1.0f, 1.0f);
    for (int i = 0; i < n; ++i) v[i] = amp * d(rng);
    return v;
}

void run(HolosOperator& op, std::vector<float>& l, std::vector<float>& r) {
    for (int s = 0; s < (int)l.size(); s += 128) {
        const int m = std::min(128, (int)l.size() - s);
        op.process(l.data() + s, r.data() + s, m);
    }
}

HolosParams onParams() {
    HolosParams p;
    p.amount = 1.0f;
    return p;
}

double maxAbs(const std::vector<float>& v, int from = 0) {
    double m = 0.0;
    for (int i = from; i < (int)v.size(); ++i) m = std::max(m, (double)std::fabs(v[i]));
    return m;
}

// Amplitud de la componente en f (Hz) en el último segundo de x.
double ampAt(const std::vector<float>& x, double f) {
    const int n = 48000, off = (int)x.size() - n;
    double a, b;
    project(x.data() + off, n, kFs, f, a, b);
    return std::sqrt(a * a + b * b);
}

bool allFinite(const std::vector<float>& v) {
    for (float x : v)
        if (!std::isfinite(x)) return false;
    return true;
}
}  // namespace

TEST(HolosOperator, BypassIsBitExact) {
    HolosOperator op;
    op.prepare(48000.0f);
    HolosParams p;  // amount = 0 por defecto
    op.setParams(p);
    auto t = tone(1000.0, 0.5, 2.0);
    auto nz = noise(0.3, 2.0, ivanna::test::IvannaTestSeed32(1));
    for (int i = 0; i < (int)t.size(); ++i) t[i] += nz[i];
    std::vector<float> l = t, r = t;
    // R distinto de L en la segunda mitad (estéreo real).
    for (int i = 48000; i < (int)r.size(); ++i) r[i] = -t[i];
    const std::vector<float> inL = l, inR = r;
    run(op, l, r);
    EXPECT_EQ(l, inL);
    EXPECT_EQ(r, inR);
    EXPECT_FALSE(op.isActive());
    EXPECT_FLOAT_EQ(op.appliedAmount(), 0.0f);
}

TEST(HolosOperator, MonoStaysMono) {
    HolosOperator op;
    op.prepare(48000.0f);
    op.setParams(onParams());
    auto t = tone(1000.0, 0.5, 3.0);
    std::vector<float> l = t, r = t;
    run(op, l, r);
    // En régimen permanente (último segundo) L y R deben coincidir:
    // θ=0, ILD=0, y la cola difusa no entra (coh≈1 → diffuseMix≈0).
    double maxDiff = 0.0;
    for (int i = 96000; i < (int)l.size(); ++i)
        maxDiff = std::max(maxDiff, (double)std::fabs(l[i] - r[i]));
    std::printf("[holos-op] mono: max|L−R| último segundo %.2e, fund %.3f\n",
                maxDiff, ampAt(l, 1000.0));
    EXPECT_LT(maxDiff, 1e-4);
    EXPECT_GT(ampAt(l, 1000.0), 0.3) << "el mono no debe enmudecer";
    EXPECT_TRUE(allFinite(l));
    EXPECT_TRUE(allFinite(r));
}

TEST(HolosOperator, CrossfadeHasNoClick) {
    HolosOperator op;
    op.prepare(48000.0f);
    HolosParams p;
    p.amount = 0.0f;
    op.setParams(p);
    auto t = tone(1000.0, 0.5, 3.0);
    std::vector<float> l = t, r = t;
    // 1 s en bypass, activar amount=1 a t=1 s, 1 s más en wet, volver a 0.
    for (int s = 0; s < 48000; s += 128) op.process(l.data() + s, r.data() + s, 128);
    op.setAmount(1.0f);
    for (int s = 48000; s < 96000; s += 128) op.process(l.data() + s, r.data() + s, 128);
    op.setAmount(0.0f);
    for (int s = 96000; s < 144000; s += 128) op.process(l.data() + s, r.data() + s, 128);
    EXPECT_TRUE(allFinite(l));
    // Sin sobreimpulso de clic: ningún pico en las ventanas de transición
    // (100 ms tras cada cambio) supera 1.1× la amplitud del tono.
    EXPECT_LT(maxAbs(l, 48000), 0.55);
    EXPECT_LT(maxAbs(l, 48000 + 4800), 0.55);
    EXPECT_LT(maxAbs(l, 96000), 0.55);
    // Tras volver a 0 y completarse el fade, el último medio segundo es
    // bit-exacto con la entrada.
    for (int i = 120000; i < (int)l.size(); ++i) EXPECT_FLOAT_EQ(l[i], t[i]);
}

TEST(HolosOperator, StableAtAllSampleRates) {
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
        HolosOperator op;
        op.prepare((float)sr);
        op.setParams(onParams());
        auto t = tone(1000.0, 0.5, 1.0, sr);
        auto nz = noise(0.1, 1.0, ivanna::test::IvannaTestSeed32(2), sr);
        for (int i = 0; i < (int)t.size(); ++i) t[i] += nz[i] + 0.1f;  // + DC
        std::vector<float> l = t, r = t;
        for (int i = 0; i < (int)r.size(); ++i) r[i] = -t[i];
        run(op, l, r);
        std::printf("[holos-op] sr %.0f: fftSize %d, latencia %d muestras\n",
                    sr, op.fftSize(), op.latencySamples());
        EXPECT_TRUE(allFinite(l)) << "sr=" << sr;
        EXPECT_TRUE(allFinite(r)) << "sr=" << sr;
        EXPECT_EQ(op.latencySamples(), op.fftSize());
        EXPECT_LT(maxAbs(l), 2.0) << "sr=" << sr;
    }
}

TEST(HolosOperator, ThermalTiers) {
    // Señal mayormente coherente (tonos estéreo, coh≈1) + ruido
    // incorrelado SOLO en 2–4 kHz (bandas con coh≈0.2): la coherencia
    // media queda >0.3 (el operador sigue activo, sin fail-closed) y
    // las bandas de ruido muestran el diffuseMix. Con ruido incorrelado
    // en TODAS las bandas la coherencia media cae <0.3 y el operador se
    // apaga entero por diseño (ver FailClosedOnIncoherentContent).
    auto bpNoise = [](uint32_t seed) {
        auto nz = noise(0.3, 2.0, seed);
        ivanna::Biquad hp, lp;
        hp.setHighpass(2000.0, 0.707, 48000.0);
        lp.setLowpass(4000.0, 0.707, 48000.0);
        for (auto& x : nz) x = lp.process(hp.process(x));
        return nz;
    };
    auto mkSignal = [&](uint32_t sL, uint32_t sR) {
        auto t1 = tone(1000.0, 0.4, 2.0);
        auto t2 = tone(6000.0, 0.3, 2.0);
        auto nL = bpNoise(sL), nR = bpNoise(sR);
        std::vector<float> l(96000), r(96000);
        for (int i = 0; i < 96000; ++i) {
            l[i] = t1[i] + t2[i] + nL[i];
            r[i] = t1[i] + t2[i] + nR[i];
        }
        return std::pair<std::vector<float>, std::vector<float>>{l, r};
    };
    HolosOperator op;
    op.prepare(48000.0f);
    op.setParams(onParams());
    auto sig = mkSignal(21, 22);
    run(op, sig.first, sig.second);
    EXPECT_EQ(op.activeBandCount(), 24);
    EXPECT_GT(op.meanCoherence(), 0.3f) << "el operador debe seguir activo";
    auto meanDiffuse = [&op]() {
        double s = 0.0;
        for (int b = 0; b < op.activeBandCount(); ++b) s += op.bandDiffuseMix(b);
        return (float)(s / op.activeBandCount());
    };
    const float dN = meanDiffuse();
    std::printf("[holos-op] coh media %.2f, diffuseMix medio: Normal %.3f\n",
                op.meanCoherence(), dN);
    EXPECT_GT(dN, 0.05f) << "las bandas de ruido deben mostrar difuso";

    op.setThermalTier(HolosThermalTier::Warm);
    sig = mkSignal(23, 24);
    run(op, sig.first, sig.second);
    EXPECT_EQ(op.activeBandCount(), 12);
    const float dW = meanDiffuse();
    std::printf("[holos-op] diffuseMix medio: Warm %.3f (esperado ≈½ de Normal)\n", dW);
    EXPECT_NEAR(dW, dN * 0.5f, 0.05f);

    op.setThermalTier(HolosThermalTier::Limited);
    sig = mkSignal(25, 26);
    run(op, sig.first, sig.second);
    EXPECT_FLOAT_EQ(meanDiffuse(), 0.0f) << "Limited: difuso off";

    op.setThermalTier(HolosThermalTier::Bypass);
    auto t = tone(1000.0, 0.5, 2.0);
    std::vector<float> bl = t, br = t;
    run(op, bl, br);
    EXPECT_FALSE(op.isActive());
    EXPECT_FLOAT_EQ(op.appliedAmount(), 0.0f);
    for (int i = 72000; i < (int)bl.size(); ++i) EXPECT_FLOAT_EQ(bl[i], t[i]);
}

TEST(HolosOperator, FailClosedOnIncoherentContent) {
    // Regla fail-closed (FUSION §5): coherencia media < 0.3 → wet = 0.
    // Con ruido incorrelado en todas las bandas, el operador se apaga
    // y la salida es bit-exacta con la entrada.
    HolosOperator op;
    op.prepare(48000.0f);
    op.setParams(onParams());
    auto l = noise(0.4, 2.0, ivanna::test::IvannaTestSeed32(3));
    auto r = noise(0.4, 2.0, ivanna::test::IvannaTestSeed32(4));
    const std::vector<float> inL = l, inR = r;
    run(op, l, r);
    std::printf("[holos-op] fail-closed: coh media %.2f, activo %d\n",
                op.meanCoherence(), (int)op.isActive());
    EXPECT_LT(op.meanCoherence(), 0.3f);
    EXPECT_FALSE(op.isActive());
    for (int i = 72000; i < (int)l.size(); ++i) {
        EXPECT_FLOAT_EQ(l[i], inL[i]);
        EXPECT_FLOAT_EQ(r[i], inR[i]);
    }
}

TEST(HolosOperator, CoherenceGateReopensAfterProbe415) {
    // Bandera 5 de la v2.5.414: la puerta de coherencia era un LATCH — el
    // estimador solo se actualiza en runFrame(), que se detiene al
    // cerrarse la puerta, así que una coherencia < 0.3 la cerraba para
    // siempre (en captura HOLOS medía ~1 µs). v2.5.415: en bypass por
    // coherencia, cada ~150 bloques se sondean 8 bloques seguidos para
    // re-evaluar el estimador; si la señal vuelve a ser coherente, la
    // etapa se re-activa sola (sin tocar amount ni tier).
    HolosOperator op;
    op.prepare(48000.0f);
    op.setParams(onParams());
    constexpr int kBlock = 320;  // bloque real de la Ruta A (6.67 ms)
    auto runBlocks = [&](std::vector<float>& l, std::vector<float>& r, int blocks) {
        for (int b = 0; b < blocks; ++b)
            op.process(l.data() + b * kBlock, r.data() + b * kBlock, kBlock);
    };
    // 1) Ruido incorrelado: la puerta se cierra.
    auto l = noise(0.4, 4.0, ivanna::test::IvannaTestSeed32(3));
    auto r = noise(0.4, 4.0, ivanna::test::IvannaTestSeed32(4));
    runBlocks(l, r, (int)l.size() / kBlock);
    EXPECT_LT(op.meanCoherence(), 0.3f);
    EXPECT_FALSE(op.isActive());
    // 2) Más ruido incorrelado: la puerta sigue cerrada (el sondeo no la
    // reabre sin señal coherente) y la salida sigue en passthrough.
    auto l2 = noise(0.4, 4.0, ivanna::test::IvannaTestSeed32(5));
    auto r2 = noise(0.4, 4.0, ivanna::test::IvannaTestSeed32(6));
    const std::vector<float> inL2 = l2, inR2 = r2;
    runBlocks(l2, r2, (int)l2.size() / kBlock);
    EXPECT_FALSE(op.isActive());
    for (int i = 72000; i < (int)l2.size(); ++i) {
        EXPECT_FLOAT_EQ(l2[i], inL2[i]);
        EXPECT_FLOAT_EQ(r2[i], inR2[i]);
    }
    // 3) Tono estéreo coherente: tras la espera (~150 bloques) + el
    // sondeo (8 bloques), el estimador se recupera y la etapa se
    // re-activa sola.
    auto t = tone(1000.0, 0.5, 4.0);
    std::vector<float> tl = t, tr = t;
    runBlocks(tl, tr, (int)tl.size() / kBlock);
    std::printf("[holos-op] re-sondeo: coh media %.2f, activo %d, wet %.2f\n",
                op.meanCoherence(), (int)op.isActive(), op.appliedAmount());
    EXPECT_GT(op.meanCoherence(), 0.3f);
    EXPECT_TRUE(op.isActive());
    EXPECT_GT(op.appliedAmount(), 0.0f);
}

TEST(HolosOperator, NoNanInfOnSilenceDcAndNoise) {
    for (const char* name : {"silence", "dc", "noise", "hot"}) {
        HolosOperator op;
        op.prepare(48000.0f);
        op.setParams(onParams());
        std::vector<float> l(96000), r(96000);
        if (std::string(name) == "dc") {
            std::fill(l.begin(), l.end(), 0.5f);
            std::fill(r.begin(), r.end(), 0.5f);
        } else if (std::string(name) == "noise") {
            l = noise(1.0, 2.0, ivanna::test::IvannaTestSeed32(7));
            r = noise(1.0, 2.0, ivanna::test::IvannaTestSeed32(8));
        } else if (std::string(name) == "hot") {
            l = tone(1000.0, 1.5, 2.0);
            r = tone(1000.0, 1.5, 2.0);
        }
        run(op, l, r);
        EXPECT_TRUE(allFinite(l)) << name;
        EXPECT_TRUE(allFinite(r)) << name;
        if (std::string(name) == "silence") {
            for (int i = 48000; i < (int)l.size(); ++i) EXPECT_FLOAT_EQ(l[i], 0.0f);
        }
        if (std::string(name) == "hot") {
            // El techo (softCeil 0.98855) limita el wet en régimen permanente.
            EXPECT_LE(maxAbs(l, 48000), 0.99) << name;
        }
        std::printf("[holos-op] %-8s: pico salida %.3f\n", name, maxAbs(l, 48000));
    }
}

TEST(HolosOperator, LatencyIsFftSize) {
    HolosOperator op;
    op.prepare(48000.0f);
    EXPECT_EQ(op.latencySamples(), 512);
    EXPECT_EQ(op.fftSize(), 512);
    EXPECT_EQ(op.latencySamples(), op.fftSize());
}

TEST(HolosOperator, AntiPumpingProgramRelease) {
    // Tono sostenido -4 dBFS (1 kHz) + ráfagas de 20 ms a -1 dBFS cada 0.5 s,
    // 3 s. La ley dual + acoplamiento + slew debe mover Γ sin bombeo:
    // pendiente máx por hop <= 0.17 dB bajando y <= 0.05 dB subiendo
    // (límites de diseño: 60 dB/s bajada = 0.16 dB/hop, 18 dB/s subida
    // = 0.048 dB/hop @48 kHz, hop 128).
    HolosOperator op;
    op.prepare(48000.0f);
    HolosParams p;
    p.amount = 0.5f;
    p.harmonics = 0.0f;
    op.setParams(p);
    const double ampBase = std::pow(10.0, -4.0 / 20.0);
    const double ampBurst = std::pow(10.0, -1.0 / 20.0);
    std::vector<float> l(144000), r(144000);
    for (int i = 0; i < 144000; ++i) {
        const double t = i / kFs;
        const int inBurst = ((i % 24000) < 960) ? 1 : 0;  // 20 ms cada 0.5 s
        const double a = inBurst ? ampBurst : ampBase;
        l[i] = r[i] = (float)(a * std::sin(2.0 * kPi * 1000.0 * t));
    }
    double prevGamma[24];
    for (int b = 0; b < 24; ++b) prevGamma[b] = 0.0;
    double maxDown = 0.0, maxUp = 0.0;
    bool first = true;
    for (int s = 0; s < (int)l.size(); s += 128) {
        const int m = std::min(128, (int)l.size() - s);
        op.process(l.data() + s, r.data() + s, m);
        for (int b = 0; b < 24; ++b) {
            const double g = op.bandGammaDb(b);
            EXPECT_TRUE(std::isfinite(g));
            if (!first) {
                maxDown = std::max(maxDown, prevGamma[b] - g);
                maxUp = std::max(maxUp, g - prevGamma[b]);
            }
            prevGamma[b] = g;
        }
        first = false;
    }
    std::printf("[holos-op] anti-pumping: max bajada %.4f dB/hop, max subida %.4f dB/hop\n",
                maxDown, maxUp);
    EXPECT_LE(maxDown, 0.17);
    EXPECT_LE(maxUp, 0.05);
    // La salida es finita en todo el programa.
    for (int i = 0; i < (int)l.size(); ++i) {
        EXPECT_TRUE(std::isfinite(l[i]));
        EXPECT_TRUE(std::isfinite(r[i]));
    }
}
