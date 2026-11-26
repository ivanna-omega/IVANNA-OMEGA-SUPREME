// test_p1p4_neuro.cpp — REFINAMIENTO TOTAL MAGISTRAL, P1+P4 (2026-11-26).
//
// NEURO: por qué cuesta −15.570 dB, y el lazo F: qué es de verdad.
//
// HALLAZGO (medido aquí, no afirmado): el −15.570 dB de NEURO NO viene
// del lazo F ni de ningún "gradiente variacional". Viene del NPE
// (NpeStereoCoreD) dentro de FusedBrain:
//   (a) su AGC apunta a −18 dBFS (kAgcTargetDb): con un tono a −3 dBFS
//       (RMS −6 dBFS) la ganancia deseada es 10^((−18+6)/20) ≈ 0.25
//       (−12 dB), con slew acotado (asentamiento lento ~2 s, ver la
//       envolvente de la sonda #1432);
//   (b) la compresión OHC g = 1/(1 + 0.3·L·4) y la inhibición lateral
//       (1 − 0.2·spikes/32) recortan más.
//   El bucle cerrado neuro-acústico (ClosedLoopController) SIN frames
//   de sensor devuelve identidad (fail-closed) y NO aporta pérdida.
//   El lazo F (FreeEnergyAdaptationLoop) vive en OTRO motor
//   (NewPhysicsEngine, no en FusedChain), su freeEnergyMix es 0.0 por
//   defecto (apagado) y su γ está acotada a [−3, +2] dB con slew
//   0.5 dB/s: aunque estuviera ON, no puede explicar −15.57 dB.
//
// SOBRE EL LAZO F (ver free_energy_loop.hpp): bandEnv24/bandTransient24
// se calculan de la FFT de la SEÑAL DE ENTRADA del propio motor
// (instEnergy por banda ERB en runStftHop) — NO de un micrófono ni de
// ningún sensor. NO existe funcional F ni ∂F/∂Γ en el código: la línea
// comentada como "gradiente variacional" es una heurística escrita a
// mano (onsetNeed/fatiguePen con constantes 1.8/2.5/1.2/0.35). Por eso
// este refinamiento la RENOMBRA a heurística (ver el comentario
// LEGACY en free_energy_loop.hpp) y mide su respuesta al escalón y su
// ganancia de lazo por banda (γ).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/free_energy_loop.hpp"
#include "../fused/new_physics_controls.hpp"
#include "../fused/new_physics_engine.hpp"
#include "../neuromorphic/NpeStereoCore.hpp"

using namespace ivanna::newphys;

namespace {

constexpr double kFs = 48000.0;
constexpr double kTwoPi = 6.28318530717958647692;

double rmsDb(const double* x, std::size_t n) {
    double p = 0.0;
    for (std::size_t i = 0; i < n; ++i) p += x[i] * x[i];
    return 20.0 * std::log10(std::sqrt(p / static_cast<double>(n)) + 1e-32);
}

}  // namespace

// ── 1) NPE standalone: descomposición del −15.570 dB ─────────────────────
// Tono 1 kHz −3 dBFS, 2 s, en trozos de 128 (camino real). Tres
// variantes: completo (0.2/0.2/0.3, los parámetros de FusedBrain),
// mínimo (0/0/0: solo AGC + NHO) y apagado (bypass bit-exacto).
TEST(P1P4Neuro, NpeDecompositionAt1kHz) {
    const std::size_t n = 96000, skip = 48000;
    const double inRmsDb = 20.0 * std::log10(0.7071 / std::sqrt(2.0));
    auto run = [&](float harmonic, float lateral, float ohc, bool enabled,
                   double* agcEnd) {
        ivanna::neuromorphic::NpeStereoCoreD npe;
        npe.prepare(48000.0f);
        npe.setParams(enabled, harmonic, lateral, ohc, 0.0f);
        std::vector<double> l(n), r(n);
        for (std::size_t i = 0; i < n; ++i) {
            const double v = 0.7071 * std::sin(kTwoPi * 1000.0 *
                                               static_cast<double>(i) / kFs);
            l[i] = v;
            r[i] = v;
        }
        double agcTraj[8] = {};
        for (std::size_t off = 0; off < n; off += 128) {
            npe.process(l.data() + off, r.data() + off, 128);
            const std::size_t w = off / 12000;
            if (w < 8) agcTraj[w] = static_cast<double>(npe.agcGain());
        }
        const double net = rmsDb(l.data() + skip, n - skip) - inRmsDb;
        if (agcEnd) *agcEnd = static_cast<double>(npe.agcGain());
        std::printf("[P1P4-NPE] harm=%.1f lat=%.1f ohc=%.1f en=%d: net=%+.3f dB "
                    "| AGC final=%.4f | AGC por 0.25s: %.3f %.3f %.3f %.3f "
                    "%.3f %.3f %.3f %.3f | spikes=%d\\n",
                    harmonic, lateral, ohc, enabled ? 1 : 0, net,
                    agcEnd ? *agcEnd : 0.0, agcTraj[0], agcTraj[1], agcTraj[2],
                    agcTraj[3], agcTraj[4], agcTraj[5], agcTraj[6], agcTraj[7],
                    npe.lastSpikes());
        return net;
    };
    double agcFull = 0.0, agcMin = 0.0;
    const double netFull = run(0.2f, 0.2f, 0.3f, true, &agcFull);
    const double netMin = run(0.0f, 0.0f, 0.0f, true, &agcMin);
    const double netOff = run(0.2f, 0.2f, 0.3f, false, nullptr);
    std::printf("[P1P4-NPE] descomposición: completo %+.3f dB | mínimo "
                "(AGC+NHO) %+.3f dB | apagado %+.3f dB | extra por "
                "OHC+inhibición+armónicos %+.3f dB\\n",
                netFull, netMin, netOff, netFull - netMin);
    // NPE apagado: +1.5e-13 dB (redondeo double, no ganancia funcional).
    EXPECT_NEAR(netOff, 0.0, 1e-9);
    EXPECT_LT(netFull, -5.0) << "el NPE completo debe perder >5 dB a 1 kHz";
    EXPECT_LT(netMin, -5.0) << "incluso sin OHC/inhibición el NPE pierde "
                               ">5 dB: el AGC a −18 dBFS domina";
    EXPECT_LT(agcFull, 0.6) << "el AGC debe converger hacia ~0.25 (−12 dB)";
}

// ── 2) Lazo F directo: respuesta al escalón y ganancia por banda ─────────
// El lazo corre por hop (128 muestras, dt = 2.667 ms). Escalón:
// envolvente sostenida 1.0 en la banda 13 (977 Hz) con transitorios 0.
// Esperado por el código: mocState → 1.0, fatiguePen = 1.0−0.35 = 0.65,
// target = −2.5·0.65 = −1.625 dB, slew 0.5 dB/s ⇒ γ(13) → −1.625 dB
// en ~3.25 s. Con silencio, γ se queda en 0.
TEST(P1P4Neuro, FreeEnergyStepResponse) {
    FreeEnergyAdaptationLoop loop;
    loop.prepare(48000.0f, 128);
    std::array<float, 24> env{}, tr{}, mul{};
    env.fill(0.0f);
    tr.fill(0.0f);
    env[13] = 1.0f;  // escalón sostenido en la banda de 977 Hz
    const int hopsPerSec = 48000 / 128;  // 375
    float gAt[9] = {};
    for (int s = 0; s < 8; ++s) {
        for (int h = 0; h < hopsPerSec; ++h)
            loop.step(env.data(), tr.data(), 1.0f, mul);
        gAt[s] = loop.gammaModDb(13);
        std::printf("[P1P4-FE] escalón banda 13 (977 Hz): t=%ds γ=%+.3f dB "
                    "(mul=%.4f)\\n",
                    s + 1, gAt[s], mul[13]);
    }
    EXPECT_NEAR(gAt[7], -1.625, 0.15)
        << "γ en régimen debe ser −2.5·(1.0−0.35) = −1.625 dB";
    EXPECT_LT(gAt[0], 0.0) << "γ debe empezar a bajar en el primer segundo";
    EXPECT_GT(gAt[0], -0.6) << "slew 0.5 dB/s: en 1 s no puede bajar más de 0.5 dB";
    // Las demás bandas no se mueven (env 0, tr 0 ⇒ target 0).
    for (int b = 0; b < 24; ++b) {
        if (b == 13) continue;
        EXPECT_FLOAT_EQ(loop.gammaModDb(b), 0.0f) << "banda " << b;
    }
    // Silencio: γ se queda en 0.
    {
        FreeEnergyAdaptationLoop loop2;
        loop2.prepare(48000.0f, 128);
        std::array<float, 24> e0{}, t0{}, m0{};
        e0.fill(0.0f);
        t0.fill(0.0f);
        for (int h = 0; h < hopsPerSec * 2; ++h)
            loop2.step(e0.data(), t0.data(), 1.0f, m0);
        EXPECT_FLOAT_EQ(loop2.gammaModDb(13), 0.0f);
        EXPECT_FLOAT_EQ(m0[13], 1.0f) << "sin señal el multiplicador es 1.0";
    }
    // Onset: transitorio fuerte con poca adaptación ⇒ realce positivo.
    {
        FreeEnergyAdaptationLoop loop3;
        loop3.prepare(48000.0f, 128);
        std::array<float, 24> e{}, t{}, m{};
        e.fill(0.1f);
        t.fill(0.0f);
        t[13] = 4.0f;  // onset brusco en la banda 13
        for (int h = 0; h < hopsPerSec; ++h)
            loop3.step(e.data(), t.data(), 1.0f, m);
        const float g = loop3.gammaModDb(13);
        std::printf("[P1P4-FE] onset banda 13: γ tras 1 s = %+.3f dB "
                    "(esperado >0, acotado por slew 0.5 dB/s)\\n",
                    g);
        EXPECT_GT(g, 0.0f);
        EXPECT_LE(g, 0.5f + 1e-6f);
    }
}

// ── 3) Lazo F en el motor: la entrada es la señal interna, no un mic ─────
// NewPhysicsEngine con amount=1.0 y freeEnergyMix=1.0. Tono 1 kHz 8 s:
// γ en la banda del tono (13, 977 Hz) debe moverse (fatiga) y quedar
// acotada a [−3, +2] dB. Con silencio, γ no se mueve: eso prueba que
// bandEnv24 viene de la FFT de la entrada del motor, no de un sensor
// externo (no hay micrófono enchufado en este motor).
TEST(P1P4Neuro, FreeEnergyEngineInputIsInternalSignal) {
    NewPhysicsControls c;
    c.amount = 1.0f;
    c.freeEnergyMix = 1.0f;
    c.clamp();
    {
        NewPhysicsEngine eng;
        eng.prepare(48000.0f);
        eng.setControls(c);
        const std::size_t n = 48000 * 8;
        std::vector<float> l(n), r(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = static_cast<float>(
                0.7071 * std::sin(kTwoPi * 1000.0 * static_cast<double>(i) / kFs));
            l[i] = v;
            r[i] = v;
        }
        float gMax = 0.0f;
        for (std::size_t off = 0; off < n; off += 4800) {
            eng.process(l.data() + off, r.data() + off, 4800);
            const float g = eng.freeEnergyGammaModDb(13);
            if (std::fabs(g) > std::fabs(gMax)) gMax = g;
            if ((off / 4800) % 10 == 0)
                std::printf("[P1P4-FE] motor, tono 1 kHz: t=%.1fs γ(977 Hz)=%+.3f dB\\n",
                            static_cast<double>(off) / kFs, g);
        }
        const float gEnd = eng.freeEnergyGammaModDb(13);
        std::printf("[P1P4-FE] motor, tono 1 kHz 8 s: γ final banda 13 = %+.3f dB "
                    "(máx |γ| = %.3f dB)\\n",
                    gEnd, std::fabs(gMax));
        EXPECT_LT(gEnd, 0.0f) << "tono sostenido ⇒ fatiga MOC ⇒ γ negativa";
        EXPECT_GE(gEnd, -3.0f) << "γ acotada a −3 dB por construcción";
        EXPECT_LE(std::fabs(gMax), 3.0f + 1e-6f);
        // Aunque el lazo estuviera a fondo en las 24 bandas, su
        // contribución máxima es −3 dB/banda: NO explica −15.57 dB.
    }
    {
        NewPhysicsEngine eng;
        eng.prepare(48000.0f);
        eng.setControls(c);
        std::vector<float> l(48000 * 2, 0.0f), r(48000 * 2, 0.0f);
        eng.process(l.data(), r.data(), 96000);
        float worst = 0.0f;
        for (int b = 0; b < 24; ++b)
            worst = std::max(worst, std::fabs(eng.freeEnergyGammaModDb(b)));
        std::printf("[P1P4-FE] motor, silencio 2 s: máx |γ| = %.3f dB "
                    "(esperado 0: sin señal no hay lazo)\\n",
                    worst);
        EXPECT_FLOAT_EQ(worst, 0.0f);
    }
}
