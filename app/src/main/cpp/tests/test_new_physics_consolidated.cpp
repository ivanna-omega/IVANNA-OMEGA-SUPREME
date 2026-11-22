// test_new_physics_consolidated.cpp — FÍSICA NUEVA CONSOLIDADA (Fase 2,
// v2.5.472). 5 tests del paquete (§5) + presupuesto CPU por hop.
//   1. Bypass bit-exacto con amount == 0.
//   2. LGN 3D: migración del notch de elevación −40° → +70° (~6.3 → ~11.2 kHz)
//      + amortiguación bright-spot contralateral a 90°, 12 kHz.
//   3. Laguerre Volterra H2 a 68.5 Hz: sin kernel = identidad; con kernel
//      peak corr > 1e-4 y ≤ 0.25, todo finito y |L| < 1.0.
//   4. Ω: D_max(0.7) = −20·log10(0.70); banda 0 (72 Hz < 350 Hz) sin refuerzo
//      y banda 10 (626 Hz) sí corrige.
//   5. Motor completo con NaN/Inf y conmutación sin crash ni valores no finitos
//      (STFT 512 + Fase Mínima Cepstral + Lazo F, 2048 muestras, E4).
//   6. (Extra) cpuUsPerHop en régimen permanente < 6670 µs.
//   7. Klippel H2 por el motor (v3, v2.5.482): kernel Genezi diagonal 0.02.
//   8. (v2.5.483) Sub-graves modal: etapa −4 dB @49 Hz atenúa ~0.63x
//      (10^(-4/20), peaking RBJ a la frecuencia central); NaN/Inf en
//      la etapa -> OFF sin contaminar la salida.
//   9. (v2.5.483) Cola de sala: carga sintética arma la cola (eco real);
//      nulos o totalLen ≤ 128 -> false, fail-closed.
//  10. (v2.5.483) Klippel H3 aproximado: actúa (difiere de H2 solo);
//      h3Mix=0 == H2 solo; bobina se calienta con E4 y no con E1.
//  11. (v2.5.483) wakeV3Organs: kernel + cola + 5 etapas armados;
//      cpuUsPerHop con los órganos despiertos < 6670 µs.
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "fused/geodesic_pinna_3d.hpp"
#include "fused/new_physics_engine.hpp"
#include "fused/np483_defaults.hpp"
#include "fused/omega_sigma_operator.hpp"
#include "neuromorphic/laguerre_volterra_h2.hpp"

namespace {

using namespace ivanna::newphys;

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;

std::vector<float> makeTone(int n, double freqHz, double amp, double sr = kSr) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        x[static_cast<size_t>(i)] =
            static_cast<float>(amp * std::sin(2.0 * kPi * freqHz * i / sr));
    }
    return x;
}

bool allFinite(const std::vector<float>& v) {
    for (float f : v) {
        if (!std::isfinite(f)) return false;
    }
    return true;
}

// 1 ── Bypass bit-exacto cuando amount == 0 ──────────────────────────────────
TEST(NewPhysicsConsolidated, BypassBitExact) {
    NewPhysicsEngine eng;
    eng.prepare(48000.0f);
    NewPhysicsControls c;  // amount = 0 por defecto
    eng.setControls(c);

    auto L = makeTone(512, 1000.0, 0.5);
    auto R = makeTone(512, 1000.0, 0.5);
    const auto origL = L, origR = R;
    eng.process(L.data(), R.data(), 512);
    EXPECT_EQ(std::memcmp(L.data(), origL.data(), 512 * sizeof(float)), 0);
    EXPECT_EQ(std::memcmp(R.data(), origR.data(), 512 * sizeof(float)), 0);
}

// 2 ── LGN 3D: migración del notch + bright-spot ─────────────────────────────
TEST(NewPhysicsConsolidated, Lgn3dNotchMigrationAndBrightSpot) {
    GeodesicPinna3D pinna;
    const double az0 = 0.0;
    const double notchLow = pinna.predictNotchHz(0, az0, -40.0 * kPi / 180.0, false, 0.0875);
    const double notchHigh = pinna.predictNotchHz(0, az0, 70.0 * kPi / 180.0, false, 0.0875);
    // ~6.3 kHz a −40° de elevación, ~11.2 kHz a +70° (familia baja, ΔL 3D exacto)
    EXPECT_NEAR(notchLow, 6300.0, 500.0);
    EXPECT_NEAR(notchHigh, 11200.0, 800.0);
    EXPECT_GT(notchHigh, notchLow);

    // Bright-spot contralateral a 90°, 12 kHz: con damp 0.65 el oído
    // contralateral (derecho, fuente a la izquierda) cae a 0.35x.
    const double az90 = 90.0 * kPi / 180.0;
    const auto hDamp = pinna.evalTransfer(12000.0, az90, 0.0, true, 0.0875, 1.0, 0.65, 0.18);
    const auto hNoDamp = pinna.evalTransfer(12000.0, az90, 0.0, true, 0.0875, 1.0, 0.0, 0.18);
    EXPECT_GT(std::abs(hNoDamp), 0.0);
    EXPECT_LT(std::abs(hDamp), std::abs(hNoDamp) * 0.5);
}

// 3 ── Laguerre Volterra H2 a 68.5 Hz ─────────────────────────────────────────
TEST(NewPhysicsConsolidated, LaguerreVolterra68Hz) {
    LaguerreVolterraH2 v;
    v.prepare(48000.0f);

    // Sin kernel = identidad exacta.
    auto L = makeTone(48000, 68.5, 0.5);
    auto R = makeTone(48000, 68.5, 0.5);
    const auto origL = L;
    EXPECT_EQ(v.processBlock(L.data(), R.data(), 48000, 1.0f, 85.0f), 0.0f);
    EXPECT_EQ(std::memcmp(L.data(), origL.data(), 48000 * sizeof(float)), 0);

    // Con kernel: solo el término u0² = 0.02 (Σ|k| = 0.02 ≤ 0.35, puerta IVK1).
    float triL[21] = {};
    float triR[21] = {};
    triL[0] = 0.02f;
    triR[0] = 0.02f;
    ASSERT_TRUE(v.loadCoefficients(triL, triR));
    ASSERT_TRUE(v.hasKernel());

    L = makeTone(48000, 68.5, 0.5);
    R = makeTone(48000, 68.5, 0.5);
    const float peak = v.processBlock(L.data(), R.data(), 48000, 1.0f, 85.0f);
    EXPECT_GT(peak, 1e-4f);
    EXPECT_LE(peak, 0.25f);
    EXPECT_TRUE(allFinite(L));
    EXPECT_TRUE(allFinite(R));
    for (float s : L) EXPECT_LT(std::abs(s), 1.0f);
    // La corrección realmente actuó: la salida difiere de la entrada.
    bool differs = false;
    const auto clean = makeTone(48000, 68.5, 0.5);
    for (size_t i = 0; i < L.size(); ++i) {
        if (L[i] != clean[i]) { differs = true; break; }
    }
    EXPECT_TRUE(differs);
}

// 4 ── Ω: techo de diafonía + regla fuera de banda ───────────────────────────
TEST(NewPhysicsConsolidated, OmegaDmaxAndBandRule) {
    OmegaSigmaOperator op;
    // H_b medida: el Genezi cae −5 dB en 72 Hz y −2 dB en 626 Hz; headroom 6 dB.
    double hb[24] = {};
    double hr[24];
    for (int i = 0; i < 24; ++i) hr[i] = 6.0;
    hb[0] = -5.0;
    hb[10] = -2.0;
    op.setMeasuredEquipmentResponse(hb, hr, 350.0, 14000.0);

    NewPhysicsControls c;
    c.eqClass = EquipmentClass::PHONE_SPEAKER_E2;  // κ = 0.70
    c.omegaSignatureMix = 1.0f;
    c.boostCapDb = 3.0f;
    const auto out = op.compute(c);

    // D_max(0.7) = −20·log10(0.70) ≈ 3.10 dB
    EXPECT_NEAR(out.dMaxDb, -20.0 * std::log10(0.70), 1e-9);
    // Banda 0 (72 Hz < f_lo = 350 Hz): NO se refuerza fuera de banda útil.
    EXPECT_DOUBLE_EQ(out.bands[0].gainDb, 0.0);
    // Banda 10 (626 Hz, dentro de [350, 14000]): sí corrige (+2 dB pedidos,
    // topados por boostCap 3 dB).
    EXPECT_GT(out.bands[10].gainDb, 0.0);
    EXPECT_NEAR(out.bands[10].gainDb, 2.0, 1e-9);
}

// 5 ── Motor completo con NaN/Inf + conmutación ──────────────────────────────
TEST(NewPhysicsConsolidated, FullEngineNanInfAndSwitching) {
    NewPhysicsEngine eng;
    eng.prepare(48000.0f);
    NewPhysicsControls c;
    c.amount = 1.0f;
    c.eqClass = EquipmentClass::HOME_STEREO_E4;  // Sony Genezi
    c.freeEnergyMix = 1.0f;  // lazo F activo
    c.lgnPinnaMix = 1.0f;
    eng.setControls(c);

    auto L = makeTone(2048, 440.0, 0.5);
    auto R = makeTone(2048, 440.0, 0.5);
    L[100] = std::numeric_limits<float>::quiet_NaN();
    L[500] = std::numeric_limits<float>::infinity();
    R[1000] = -std::numeric_limits<float>::infinity();
    R[1500] = std::numeric_limits<float>::quiet_NaN();
    // Sin crash y sin valores no finitos a la salida.
    eng.process(L.data(), R.data(), 2048);
    EXPECT_TRUE(allFinite(L));
    EXPECT_TRUE(allFinite(R));

    // Conmutación a bypass: tras el fundido (15 ms), el bloque es bit-exacto.
    c.amount = 0.0f;
    eng.setControls(c);
    auto L2 = makeTone(2048, 440.0, 0.5);
    auto R2 = makeTone(2048, 440.0, 0.5);
    eng.process(L2.data(), R2.data(), 2048);  // funde wet -> 0
    auto L3 = makeTone(512, 440.0, 0.5);
    auto R3 = makeTone(512, 440.0, 0.5);
    const auto origL3 = L3, origR3 = R3;
    eng.process(L3.data(), R3.data(), 512);
    EXPECT_EQ(std::memcmp(L3.data(), origL3.data(), 512 * sizeof(float)), 0);
    EXPECT_EQ(std::memcmp(R3.data(), origR3.data(), 512 * sizeof(float)), 0);

    // Y de vuelta a activo: sin crash, salida finita.
    c.amount = 1.0f;
    eng.setControls(c);
    eng.process(L3.data(), R3.data(), 512);
    EXPECT_TRUE(allFinite(L3));
    EXPECT_TRUE(allFinite(R3));
}

// 6 ── Presupuesto CPU por hop (6670 µs) en régimen permanente ───────────────
TEST(NewPhysicsConsolidated, CpuBudgetPerHop) {
    NewPhysicsEngine eng;
    eng.prepare(48000.0f);
    NewPhysicsControls c;
    c.amount = 1.0f;
    c.eqClass = EquipmentClass::HOME_STEREO_E4;
    c.freeEnergyMix = 1.0f;
    c.omegaSignatureMix = 1.0f;
    eng.setControls(c);

    auto L = makeTone(2048, 440.0, 0.5);
    auto R = makeTone(2048, 440.0, 0.5);
    // Calentamiento (incluye la primera compilación de matrices, Reloj 2).
    const auto tCompile0 = std::chrono::steady_clock::now();
    eng.process(L.data(), R.data(), 2048);
    const auto tCompile1 = std::chrono::steady_clock::now();
    const double compileUs =
        std::chrono::duration<double, std::micro>(tCompile1 - tCompile0).count();
    for (int i = 0; i < 10; ++i) eng.process(L.data(), R.data(), 2048);

    constexpr int kBlocks = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kBlocks; ++i) eng.process(L.data(), R.data(), 2048);
    const auto t1 = std::chrono::steady_clock::now();
    const double totalUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
    const double usPerHop = totalUs * 128.0 / (kBlocks * 2048.0);
    std::printf("[NewPhysics] cpuUsPerHop(steady) = %.1f us (telemetria EMA = %.1f us), "
                "primera compilacion Reloj 2 = %.1f us, presupuesto = 6670 us\n",
                usPerHop, eng.telemetry().cpuUsPerHop, compileUs);
    EXPECT_LT(usPerHop, 6670.0);
    EXPECT_LT(eng.telemetry().cpuUsPerHop, 6670.0f);
}

// 7 ── Klippel H2 por el motor (v3, v2.5.482): kernel Genezi diagonal 0.02
// a 68.5 Hz. La corrección en régimen permanente debe ser ~0.0995
// (modelo Laguerre con polo calibrado 0.9983, ×2 en cruces, DC-block 15 Hz;
// verificado contra modelo Python independiente en el harness local).
TEST(NewPhysicsConsolidated, KlippelEngineSteadyState) {
    NewPhysicsControls c;
    c.amount = 1.0f;
    c.lgnPinnaMix = 0.0f;      // matriz WOLA identidad: la WOLA es retardo
    c.brightSpotDamp = 0.0f;   // puro de 512, ganancia 1 (el Klippel ve el
    c.torsoEchoMix = 0.0f;     // tono limpio, como el modelo de referencia)
    c.omegaSignatureMix = 0.0f;
    c.crosstalkCompMix = 0.0f;
    c.eqClass = EquipmentClass::HEADPHONES_E1;
    c.volterraMix = 1.0f;
    c.splEstimateDb = 85.0f;

    float tri[21] = {};
    tri[0] = 0.02f; // (0,0) diagonal, Σ|k| = 0.02 ≤ 0.35 (puerta IVK1)

    NewPhysicsEngine engK, eng0;
    engK.prepare(48000.0f);
    eng0.prepare(48000.0f);
    engK.setControls(c);
    eng0.setControls(c);
    ASSERT_TRUE(engK.loadGeneziVolterra(tri, tri));
    ASSERT_EQ(engK.telemetry().kernelLoaded, 1);

    auto tone = []() { return makeTone(48000, 68.5, 0.5); };
    auto LK = tone(); auto RK = tone();
    auto L0 = tone(); auto R0 = tone();
    engK.process(LK.data(), RK.data(), 48000); // calentamiento (transitorio
    eng0.process(L0.data(), R0.data(), 48000); // de arranque del banco)
    LK = tone(); RK = tone();
    L0 = tone(); R0 = tone();
    engK.process(LK.data(), RK.data(), 48000);
    eng0.process(L0.data(), R0.data(), 48000);

    // Corrección real = salida con kernel − salida sin kernel, solo en
    // régimen permanente (muestras 24000..48000).
    float peakSteady = 0.0f;
    for (int i = 24000; i < 48000; ++i)
        peakSteady = std::max(peakSteady, std::abs(LK[i] - L0[i]));
    EXPECT_GT(peakSteady, 0.05f);
    EXPECT_LT(peakSteady, 0.15f);
    EXPECT_TRUE(allFinite(LK));
    EXPECT_TRUE(allFinite(L0));
}

// 8 ── Sub-graves modal (v2.5.483): etapa −4 dB @49 Hz atenúa ~0.79x
// (RBJ peaking: |H(f0)| = 10^(gain/40)); NaN/Inf -> etapa OFF sin
// contaminar la salida.
TEST(NewPhysicsConsolidated, SubBassStageCutAndNanGuard) {
    auto neutral = []() {
        NewPhysicsControls c;
        c.amount = 1.0f;
        c.lgnPinnaMix = 0.0f;
        c.brightSpotDamp = 0.0f;
        c.torsoEchoMix = 0.0f;
        c.omegaSignatureMix = 0.0f;
        c.crosstalkCompMix = 0.0f;
        c.eqClass = EquipmentClass::HEADPHONES_E1;
        return c;
    };

    NewPhysicsEngine engA, engB;
    engA.prepare(48000.0f);
    engB.prepare(48000.0f);
    engA.setControls(neutral());
    engB.setControls(neutral());
    NewPhysicsEngine::SubBassStageParams p;
    p.freqHz = 49.0;
    p.q = 4.0;
    p.gainDb = -4.0;
    p.lowShelf = false;
    p.active = true;
    engA.setSubBassModalStage(3, p);
    ASSERT_NE(engA.subBassActiveMask() & (1u << 3), 0u);

    auto tone = []() { return makeTone(48000, 49.0, 0.5); };
    auto LA = tone();
    auto RA = tone();
    auto LB = tone();
    auto RB = tone();
    engA.process(LA.data(), RA.data(), 48000);
    engB.process(LB.data(), RB.data(), 48000);
    auto rms = [](const std::vector<float>& v, int from) {
        double s = 0.0;
        for (int i = from; i < static_cast<int>(v.size()); ++i)
            s += static_cast<double>(v[i]) * v[i];
        return std::sqrt(s / (v.size() - static_cast<size_t>(from)));
    };
    // |H(49 Hz)| = 10^(-4/20) = 0.631 para peaking RBJ (A = 10^(g/40)
    // y |H(f0)| = A^2 = 10^(g/20): el corte COMPLETO a la frecuencia
    // central. Medido 0.631 exacto — el motor está bien; la expectativa
    // anterior (0.794 = 10^(-4/40) = A) confundía A con |H(f0)|.
    const double ratio = rms(LA, 24000) / rms(LB, 24000);
    EXPECT_GT(ratio, 0.55);
    EXPECT_LT(ratio, 0.72);
    EXPECT_TRUE(allFinite(LA));

    // Guarda NaN/Inf (v2.5.483): la etapa queda OFF, sin NaN en la salida.
    NewPhysicsEngine engC;
    engC.prepare(48000.0f);
    engC.setControls(neutral());
    NewPhysicsEngine::SubBassStageParams bad;
    bad.freqHz = std::numeric_limits<double>::quiet_NaN();
    bad.q = 4.0;
    bad.gainDb = -4.0;
    bad.lowShelf = false;
    bad.active = true;
    engC.setSubBassModalStage(2, bad);
    EXPECT_EQ(engC.subBassActiveMask() & (1u << 2), 0u);
    bad.freqHz = 49.0;
    bad.gainDb = std::numeric_limits<double>::infinity();
    engC.setSubBassModalStage(2, bad);
    EXPECT_EQ(engC.subBassActiveMask() & (1u << 2), 0u);
    auto LC = tone();
    auto RC = tone();
    engC.process(LC.data(), RC.data(), 48000);
    EXPECT_TRUE(allFinite(LC));
    EXPECT_TRUE(allFinite(RC));
}

// 9 ── Cola de sala (v2.5.483): carga sintética arma la cola (eco real);
// nulos o totalLen ≤ 128 -> false, fail-closed.
TEST(NewPhysicsConsolidated, RoomTailLoadAndReject) {
    auto neutral = []() {
        NewPhysicsControls c;
        c.amount = 1.0f;
        c.lgnPinnaMix = 0.0f;
        c.brightSpotDamp = 0.0f;
        c.torsoEchoMix = 0.0f;
        c.omegaSignatureMix = 0.0f;
        c.crosstalkCompMix = 0.0f;
        c.eqClass = EquipmentClass::HEADPHONES_E1;
        return c;
    };

    NewPhysicsEngine eng;
    eng.prepare(48000.0f);
    eng.setControls(neutral());
    // Rechazos: nulos y totalLen ≤ hop (128).
    std::vector<float> ir(3200, 0.0f);
    EXPECT_FALSE(eng.loadMeasuredRoomMimoTail(nullptr, nullptr, nullptr, nullptr, 3200, 1.0f));
    EXPECT_FALSE(eng.loadMeasuredRoomMimoTail(ir.data(), ir.data(), ir.data(), ir.data(), 128, 1.0f));
    EXPECT_FALSE(eng.loadMeasuredRoomMimoTail(ir.data(), ir.data(), ir.data(), ir.data(), 100, 1.0f));
    EXPECT_FALSE(eng.roomTailActive());

    // Carga válida: delta en el tap 200 (tras el salto de 128 del cargador).
    std::vector<float> ll(3200, 0.0f), lr(3200, 0.0f), rl(3200, 0.0f), rr(3200, 0.0f);
    ll[200] = 1.0f;
    ASSERT_TRUE(eng.loadMeasuredRoomMimoTail(ll.data(), lr.data(), rl.data(), rr.data(), 3200, 1.0f));
    EXPECT_TRUE(eng.roomTailActive());

    // El eco es real: impulso con cola != impulso sin cola.
    NewPhysicsEngine eng0;
    eng0.prepare(48000.0f);
    eng0.setControls(neutral());
    auto impulse = []() {
        std::vector<float> x(8192, 0.0f);
        x[0] = 1.0f;
        return x;
    };
    auto L1 = impulse();
    auto R1 = impulse();
    auto L0 = impulse();
    auto R0 = impulse();
    eng.process(L1.data(), R1.data(), 8192);
    eng0.process(L0.data(), R0.data(), 8192);
    float peakDiff = 0.0f;
    for (int i = 0; i < 8192; ++i) peakDiff = std::max(peakDiff, std::abs(L1[i] - L0[i]));
    EXPECT_GT(peakDiff, 1e-5f);
    EXPECT_TRUE(allFinite(L1));
    EXPECT_TRUE(allFinite(L0));
}

// 10 ── Klippel H3 aproximado + térmica (v2.5.483).
TEST(NewPhysicsConsolidated, KlippelH3AndCoilTemps) {
    auto controlsE4 = []() {
        NewPhysicsControls c;
        c.amount = 1.0f;
        c.lgnPinnaMix = 0.0f;
        c.brightSpotDamp = 0.0f;
        c.torsoEchoMix = 0.0f;
        c.omegaSignatureMix = 0.0f;
        c.crosstalkCompMix = 0.0f;
        c.eqClass = EquipmentClass::HOME_STEREO_E4;  // Genezi: bocina -> térmica activa
        c.volterraMix = 1.0f;
        c.splEstimateDb = 85.0f;
        return c;
    };
    const float* tri = kNp483Tri21Approx;
    const float* c3 = kNp483C3Approx;
    float c3zero[6] = {};

    NewPhysicsEngine engA, engB;
    engA.prepare(48000.0f);
    engB.prepare(48000.0f);
    engA.setControls(controlsE4());
    engB.setControls(controlsE4());
    ASSERT_TRUE(engA.loadKlippelH2H3(tri, tri, c3, c3, kNp483Alpha3Approx, kNp483Alpha3Approx, kNp483LagPoleApprox));
    ASSERT_TRUE(engB.loadKlippelH2H3(tri, tri, c3zero, c3zero, 0.0f, 0.0f, kNp483LagPoleApprox));

    auto tone = []() { return makeTone(96000, 68.5, 0.75); };
    auto LA = tone();
    auto RA = tone();
    auto LB = tone();
    auto RB = tone();
    engA.process(LA.data(), RA.data(), 96000);
    engB.process(LB.data(), RB.data(), 96000);
    EXPECT_TRUE(allFinite(LA));
    EXPECT_TRUE(allFinite(LB));
    // La bobina se calienta con señal fuerte en bocinas (E4).
    EXPECT_GT(engA.coilTempCL(), 26.0f);
    EXPECT_GT(engB.coilTempCL(), 26.0f);
    EXPECT_GT(engA.telemetry().volterraPeakCorr, 0.0f);
    // H3 actúa: A (H2+H3) difiere de B (H2 solo) en régimen permanente.
    float peakDiff = 0.0f;
    for (int i = 48000; i < 96000; ++i)
        peakDiff = std::max(peakDiff, std::abs(LA[i] - LB[i]));
    EXPECT_GT(peakDiff, 1e-4f);
    EXPECT_LT(peakDiff, 0.05f);

    // h3Mix=0 == H2 solo (a precisión float).
    NewPhysicsEngine engA0;
    engA0.prepare(48000.0f);
    engA0.setControls(controlsE4());
    ASSERT_TRUE(engA0.loadKlippelH2H3(tri, tri, c3, c3, kNp483Alpha3Approx, kNp483Alpha3Approx, kNp483LagPoleApprox));
    engA0.setKlippelH3Mix(0.0f);
    EXPECT_FLOAT_EQ(engA0.klippelH3Mix(), 0.0f);
    auto LA0 = tone();
    auto RA0 = tone();
    engA0.process(LA0.data(), RA0.data(), 96000);
    float peakDiff0 = 0.0f;
    for (int i = 0; i < 96000; ++i)
        peakDiff0 = std::max(peakDiff0, std::abs(LA0[i] - LB[i]));
    EXPECT_LT(peakDiff0, 1e-7f);

    // Con audífonos (E1) la térmica NO se modela: la bobina queda en 25.0 °C.
    NewPhysicsEngine engH;
    engH.prepare(48000.0f);
    auto cH = controlsE4();
    cH.eqClass = EquipmentClass::HEADPHONES_E1;
    engH.setControls(cH);
    ASSERT_TRUE(engH.loadKlippelH2H3(tri, tri, c3, c3, kNp483Alpha3Approx, kNp483Alpha3Approx, kNp483LagPoleApprox));
    auto LH = tone();
    auto RH = tone();
    engH.process(LH.data(), RH.data(), 96000);
    EXPECT_FLOAT_EQ(engH.coilTempCL(), 25.0f);
    EXPECT_FLOAT_EQ(engH.coilTempCR(), 25.0f);
}

// 11 ── wakeV3Organs (v2.5.483): kernel + cola + 5 etapas armados; CPU.
TEST(NewPhysicsConsolidated, WakeV3DefaultsAndCpu) {
    NewPhysicsEngine eng;
    eng.prepare(48000.0f);
    NewPhysicsControls c;
    c.amount = 1.0f;
    c.eqClass = EquipmentClass::HOME_STEREO_E4;
    c.volterraMix = 1.0f;
    c.splEstimateDb = 85.0f;
    eng.setControls(c);

    ASSERT_TRUE(wakeV3Organs(eng));
    EXPECT_TRUE(eng.roomTailActive());
    EXPECT_FLOAT_EQ(eng.roomTailMix(), 0.5f);
    EXPECT_FLOAT_EQ(eng.klippelH3Mix(), 1.0f);
    EXPECT_EQ(eng.telemetry().kernelLoaded, 1);
    EXPECT_EQ(eng.subBassActiveMask() & 0b111110u, 0b111110u);

    auto L = makeTone(2048, 440.0, 0.5);
    auto R = makeTone(2048, 440.0, 0.5);
    eng.process(L.data(), R.data(), 2048);  // calentamiento
    for (int i = 0; i < 10; ++i) eng.process(L.data(), R.data(), 2048);
    constexpr int kBlocks = 50;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kBlocks; ++i) eng.process(L.data(), R.data(), 2048);
    const auto t1 = std::chrono::steady_clock::now();
    const double totalUs = std::chrono::duration<double, std::micro>(t1 - t0).count();
    const double usPerHop = totalUs * 128.0 / (kBlocks * 2048.0);
    std::printf("[NewPhysics] wakeV3 cpuUsPerHop(steady) = %.1f us (telemetria EMA = %.1f us), "
                "presupuesto = 6670 us\n",
                usPerHop, eng.telemetry().cpuUsPerHop);
    EXPECT_LT(usPerHop, 6670.0);
    EXPECT_LT(eng.telemetry().cpuUsPerHop, 6670.0f);
    EXPECT_TRUE(allFinite(L));
    EXPECT_TRUE(allFinite(R));
}

} // namespace
