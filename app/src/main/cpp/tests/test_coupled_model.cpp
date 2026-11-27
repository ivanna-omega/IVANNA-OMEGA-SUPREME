// test_coupled_model.cpp — FASE 3, modelo acoplado no lineal (D10).
//
// Tests GTest del modelo de ~/.../fused/coupled_model/coupled_model.hpp.
// REGLA DE MEDICIÓN (patrón test_d10_corona): los umbrales se fijan con
// lo MEDIDO en este host + margen, nunca con un objetivo no medido. Cada
// test IMPRIME el valor medido. Si el sistema no hace lo que el test
// espera, el test lo muestra con el número real, no se maquilla.
//
// Cobertura:
//  1. Modos de sala: conteo exacto (35, no 37 de Weyl) y frecuencias.
//  2. Gamma_k corregido [D1] y |Z_rad| en resonancia (99.4 Pa·s/m³).
//  3. Westervelt: tabla F2 (-56.6 dB a 110 dB/1 kHz/3 m) + Fubini vs
//     Beyer-pico (-53.6 dB): divergencia de 3.01 dB registrada [D2].
//  4. P-A: Delta phi (máx 2.71°) + Delta f_s modal (0.017 Hz, [D5]:
//     F2 decía 0.2-0.8 Hz y la suma exacta NO lo reproduce).
//  5. P-C: muesca |v_c/v_0| analítica (0.9838) y dinámica (0.9906 @49 Hz).
//  6. P-B2: tau de la componente 137.2 Hz de Pi_2[p²] (22.4 ms ~ tau_1/2).
//  7. Balance energético discreto por hop <= 1e-6 (medido 3.2e-9).
//  8. Integrador exponencial exacto vs RK4 (auditoría F2 §3.1, valores
//     exactos [D7]: RK4 adelanta +14.4°/ciclo a 16 kHz y +164.6° a 20 kHz).
//  9. Aliasing Duffing: DC a 48 kHz, limpio a 192 kHz (F2 §3.3).
// 10. RT: coste por hop (medido ~4-30 µs vs 1330 µs) y fidelidad 77%.
// 11. J-A: misma corriente, distinta fuerza (3.28%, rango F2 2-8%).
// 12. Tensor C_ijk disperso: 1614 tripletes no nulos + valor 1/√2.
// 13. RIR: modos de rir_0000 (5.96x3.08x9.50 m) + f_Sch medida/nominal.
// 14. Sabine/Z_w desde RIR (alpha 0.252 nominal / 0.110 medida).
// 15. KEMAR dos oídos + lectura multirreceptor 2.1 (mismo estado).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/coupled_model/coupled_model.hpp"

using namespace ivanna::fused::coupled;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Amplitud por correlación seno/coseno a frecuencia exacta (sin fuga
// espectral con frecuencias de ciclo entero; misma matemática que
// measureThdDouble en test_d10_corona).
double amplitudeAt(const std::vector<double>& x, double fs, double freq) {
    double s = 0.0, c = 0.0, phase = 0.0;
    const double step = 2.0 * kPi * freq / fs;
    for (double v : x) {
        s += v * std::sin(phase);
        c += v * std::cos(phase);
        phase += step;
    }
    return 2.0 * std::sqrt(s * s + c * c) / static_cast<double>(x.size());
}

// Matriz de un paso RK4 del oscilador libre y'' + w²y = 0, vía los dos
// vectores base. Devuelve (amplitud/paso, error de fase en °/ciclo).
// Es la medida exacta del error del integrador, sin desenrollado de fase.
std::pair<double, double> rk4OscillatorStep(double omega, double h) {
    auto rk4Step = [&](double y, double v) {
        const auto f = [&](double yy, double vv, double& dy, double& dv) {
            dy = vv;
            dv = -omega * omega * yy;
        };
        double k1y, k1v, k2y, k2v, k3y, k3v, k4y, k4v;
        f(y, v, k1y, k1v);
        f(y + 0.5 * h * k1y, v + 0.5 * h * k1v, k2y, k2v);
        f(y + 0.5 * h * k2y, v + 0.5 * h * k2v, k3y, k3v);
        f(y + h * k3y, v + h * k3v, k4y, k4v);
        y += h / 6.0 * (k1y + 2.0 * k2y + 2.0 * k3y + k4y);
        v += h / 6.0 * (k1v + 2.0 * k2v + 2.0 * k3v + k4v);
        return std::pair<double, double>(y, v);
    };
    // Columnas: estado (1,0) y (0,w) -> base ortonormal en (y, v/w).
    const auto c1 = rk4Step(1.0, 0.0);
    const auto c2 = rk4Step(0.0, omega);
    const double a11 = c1.first, a21 = c1.second / omega;
    const double a12 = c2.first, a22 = c2.second / omega;
    // La matriz RK4 en (y, v/w) es [[Re R, Im R],[-Im R, Re R]] (el
    // oscilador gira en sentido HORARIO: y=cos wt, v/w=-sin wt). El
    // avance de fase por paso es alpha = -atan2(a21, a11), desenvuelto
    // CERCA de wh (no de 0): a 20 kHz el avance continuo es 218.6°, no
    // -141.4°. [D7] Desenvolver el ángulo mismo (regla "+2π si <0")
    // es incorrecto: a 16 kHz daba -345.6°/ciclo en vez de +14.4°.
    const double det = a11 * a22 - a12 * a21;
    const double exactPerStep = omega * h;
    double alpha = -std::atan2(a21, a11);  // avance por paso, (-pi, pi]
    while (alpha - exactPerStep > kPi) alpha -= 2.0 * kPi;
    while (alpha - exactPerStep < -kPi) alpha += 2.0 * kPi;
    const double ampPerStep = std::sqrt(det);
    const double cyclesPerStep = exactPerStep / (2.0 * kPi);
    // Positivo = RK4 ADELANTA (gira más por paso que el exacto).
    const double phaseErrDegPerCycle =
        (alpha - exactPerStep) / cyclesPerStep * 180.0 / kPi;
    return {ampPerStep, phaseErrDegPerCycle};
}

// Lo mismo para el integrador exponencial exacto (debe dar error 0).
std::pair<double, double> expOscillatorStep(double omega, double h) {
    ExpOscillator osc(omega, 0.0, h);
    double y1 = 1.0, v1 = 0.0;
    osc.step(y1, v1, 0.0);
    double y2 = 0.0, v2 = omega;
    osc.step(y2, v2, 0.0);
    const double a11 = y1, a21 = v1 / omega;
    const double a12 = y2, a22 = v2 / omega;
    const double tr = 0.5 * (a11 + a22);
    const double det = a11 * a22 - a12 * a21;
    const double im = std::sqrt(std::max(0.0, det - tr * tr));
    const double phasePerStep = std::atan2(im, tr);
    const double exactPerStep = omega * h;
    const double cyclesPerStep = exactPerStep / (2.0 * kPi);
    const double phaseErrDegPerCycle =
        (exactPerStep - phasePerStep) / cyclesPerStep * 180.0 / kPi;
    return {std::sqrt(det), phaseErrDegPerCycle};
}

const Vec3 kCorner{0.3, 0.3, 1.2};  // G1, F2 §1 P-A
const Vec3 kCenter{2.0, 3.5, 1.2};  // G2, F2 §1 P-A
}  // namespace

TEST(CoupledModel, RoomModesCountAndFrequencies) {
    const auto modes = roomModes();
    // Conteo rectangular EXACTO: 35 modos con f < 139.4 Hz [D4].
    // F2 §0 decía 37: es la estimación asintótica de Weyl, no el conteo.
    std::printf("MODOS: N(f<139.4 Hz) = %zu (Weyl decía 37)\n", modes.size());
    EXPECT_EQ(modes.size(), 35u);
    ASSERT_GE(modes.size(), 10u);
    const double first[] = {24.5, 42.9, 49.0, 49.4, 65.1, 68.6, 72.8};
    for (int i = 0; i < 7; ++i) {
        std::printf("  modo %d: %.1f Hz (esperado %.1f)\n", i, modes[i].freqHz,
                    first[i]);
        EXPECT_NEAR(modes[i].freqHz, first[i], 0.15);
    }
    // El modo (0,0,1) es 68.6 Hz y el (0,0,2) es 137.2 Hz (P-B2).
    for (const RoomMode& m : modes) {
        if (m.nx == 0 && m.ny == 0 && m.nz == 1) {
            EXPECT_NEAR(m.freqHz, 68.6, 0.05);
        }
        if (m.nx == 0 && m.ny == 0 && m.nz == 2) {
            EXPECT_NEAR(m.freqHz, 137.2, 0.05);
        }
    }
    // zeta desde T60: 6.9/(w T60).
    EXPECT_NEAR(modes[5].zeta, 6.9 / (2.0 * kPi * 68.6 * kT60), 1e-6);
    EXPECT_NEAR(modalTau1(), 0.0493, 0.0005);  // [s]
    EXPECT_NEAR(modalTau2(), 0.0246, 0.0005);  // [s] = tau_1/2
}

TEST(CoupledModel, GammaCorrectedAndRadiationImpedanceAtResonance) {
    const auto modes = roomModes();
    // Gamma_k en antinodo (phi = sqrt(2)): rho0 c0² sqrt(2)/V ≈ 2852.
    const RoomMode* m001 = nullptr;
    for (const RoomMode& m : modes)
        if (m.nx == 0 && m.ny == 0 && m.nz == 1) m001 = &m;
    ASSERT_NE(m001, nullptr);
    const Vec3 antinode{0.3, 0.3, 0.1};  // z=0.1: casi antinodo vertical
    const double gamma = m001->gammaAt(antinode);
    std::printf("GAMMA (0,0,1) en z=0.1 m: %.1f Pa/m^3 (canónico ~2852)\n",
                gamma);
    EXPECT_GT(gamma, 2000.0);
    EXPECT_LT(gamma, 2900.0);
    // |Z_rad| de UN solo modo en resonancia: 99.4 Pa·s/m³ (F2 (P-A.2)).
    // Se calcula con el modo aislado para no mezclar la suma completa.
    const double w = m001->omega;
    const double zeta = m001->zeta;
    const double phi = m001->phiAt(antinode);
    const std::complex<double> zSingle =
        gamma * phi * std::complex<double>(0.0, w) /
        std::complex<double>(0.0, 2.0 * zeta * w * w);
    std::printf("|Z_rad| un solo modo en resonancia: %.1f (F2: 99.4)\n",
                std::abs(zSingle));
    EXPECT_NEAR(std::abs(zSingle), 99.4, 5.0);
    // La suma completa es MAYOR (134.7 a 68.6 Hz en esquina z=1.2).
    const double zFull =
        std::abs(radiationImpedance(modes, 2.0 * kPi * 68.6, kCorner));
    std::printf("|Z_rad| suma completa 68.6 Hz esquina: %.1f\n", zFull);
    EXPECT_GT(zFull, 99.4);
}

TEST(CoupledModel, WesterveltSecondHarmonicLevel) {
    // Tabla F2 (P-B.1), convención P = presión RMS [D2].
    struct Row {
        double spl, freq, z, expectedDb;
    };
    const Row rows[] = {{90, 1000, 3.0, -76.6}, {100, 1000, 3.0, -66.6},
                        {110, 1000, 3.0, -56.6}, {110, 1000, 1.5, -62.6},
                        {110, 4000, 3.0, -44.6}, {100, 68.6, 3.0, -89.9}};
    for (const Row& r : rows) {
        const double got = westerveltRatioDb(kBetaAir, r.freq, r.z, r.spl);
        std::printf("WESTERVELT %.0f dB SPL / %.0f Hz / %.1f m: %.2f dB "
                    "(esperado %.1f)\n",
                    r.spl, r.freq, r.z, got, r.expectedDb);
        EXPECT_NEAR(got, r.expectedDb, 0.05);
    }
    // P-B.2: +6.02 dB por duplicación de z (límite onda plana).
    const double d1 = westerveltRatioDb(kBetaAir, 1000, 1.5, 110);
    const double d2 = westerveltRatioDb(kBetaAir, 1000, 3.0, 110);
    std::printf("WESTERVELT escala z: %.2f dB por duplicación (esperado "
                "+6.02, esférico +3..+6)\n",
                d2 - d1);
    EXPECT_NEAR(d2 - d1, 6.02, 0.05);
    // Fubini (serie exacta) vs Beyer-pico: divergencia registrada [D2].
    const double pRms = splDbToPressurePa(110.0);
    const double pPeak = pRms * std::sqrt(2.0);
    const double k = 2.0 * kPi * 1000.0 / kC0;
    const double sigma = kBetaAir * k * 3.0 * pPeak / (kRho0 * kC0 * kC0);
    const double fubiniDb = 20.0 * std::log10(fubiniSecondHarmonicRatio(sigma));
    const double beyerDb = westerveltRatioDbBeyerPeak(kBetaAir, 1000, 3.0, 110);
    std::printf("FUBINI sigma=%.5f: %.2f dB ; Beyer-pico: %.2f dB ; F2 "
                "(rms): %.2f dB ; gap F2-Beyer: %.2f dB\n",
                sigma, fubiniDb, beyerDb,
                westerveltRatioDb(kBetaAir, 1000, 3.0, 110),
                beyerDb - westerveltRatioDb(kBetaAir, 1000, 3.0, 110));
    EXPECT_NEAR(fubiniDb, beyerDb, 0.1);
    EXPECT_NEAR(beyerDb, -53.6, 0.05);
    EXPECT_NEAR(beyerDb - westerveltRatioDb(kBetaAir, 1000, 3.0, 110), 3.01,
                0.02);
    // P-B2: nivel a 68.6 Hz, 110 dB SPL: -79.9 dB.
    EXPECT_NEAR(westerveltRatioDb(kBetaAir, 68.6, 3.0, 110), -79.9, 0.05);
}

TEST(CoupledModel, PADeltaPhi) {
    const auto modes = roomModes();
    TransducerParams p;  // referencia: S_d=0.020, r_m=1.0
    double maxCorner = 0.0, maxFreq = 0.0, fracOver1 = 0.0;
    int over1 = 0, total = 0;
    for (int fi = 80; fi <= 280; ++fi) {
        const double f = fi * 0.5;
        const double d = std::abs(deltaPhiDeg(p, modes, 2.0 * kPi * f, kCorner));
        if (d > maxCorner) {
            maxCorner = d;
            maxFreq = f;
        }
        if (d > 1.0) ++over1;
        ++total;
    }
    fracOver1 = 100.0 * over1 / total;
    std::printf("P-A esquina: max|Delta phi| = %.2f deg @ %.1f Hz ; "
                "%.0f%% de frecuencias > 1 deg (medido 11%%)\n",
                maxCorner, maxFreq, fracOver1);
    EXPECT_GT(maxCorner, 1.5);
    EXPECT_LT(maxCorner, 5.0);
    EXPECT_GT(fracOver1, 5.0);
    // Rango F2 con S_d 0.013-0.030 y r_m 0.8-1.5: 0.6-6.4 deg.
    TransducerParams pLo;
    pLo.sd = 0.013;
    pLo.rM = 1.5;
    TransducerParams pHi;
    pHi.sd = 0.030;
    pHi.rM = 0.8;
    double maxLo = 0.0, maxHi = 0.0;
    for (int fi = 80; fi <= 280; ++fi) {
        const double f = fi * 0.5;
        maxLo = std::max(maxLo, std::abs(deltaPhiDeg(pLo, modes, 2.0 * kPi * f,
                                                     kCorner)));
        maxHi = std::max(maxHi, std::abs(deltaPhiDeg(pHi, modes, 2.0 * kPi * f,
                                                     kCorner)));
    }
    std::printf("P-A rango: min %.2f deg (S_d=0.013, r_m=1.5), max %.2f deg "
                "(S_d=0.030, r_m=0.8) ; F2: 0.6-6.4\n",
                maxLo, maxHi);
    EXPECT_GT(maxLo, 0.6);
    EXPECT_LT(maxHi, 6.4);
    // Centro: efecto mucho menor (nodo de los impares).
    double maxCenter = 0.0;
    for (int fi = 80; fi <= 280; ++fi) {
        const double f = fi * 0.5;
        maxCenter = std::max(
            maxCenter,
            std::abs(deltaPhiDeg(p, modes, 2.0 * kPi * f, kCenter)));
    }
    std::printf("P-A centro: max|Delta phi| = %.2f deg (< 1.0)\n", maxCenter);
    EXPECT_LT(maxCenter, 1.0);
}

TEST(CoupledModel, PAResonanceShiftModalOnlyIsBelowThreshold) {
    // HALLAZGO [D5]: la suma modal exacta da Delta f_s = 0.017 Hz, NO
    // 0.2-0.8 Hz (F2 (P-A.4)). La masa reactiva modal diferencial es
    // 1.8e-5 kg, no (2-5)e-4 kg. Este test CODIFICA el hallazgo: si un
    // día la suma diera 0.2-0.8 Hz, el test lo diría (fallaría el <0.05).
    // Consecuencia: R-A sobre Delta f_s (>0.3 Hz) no puede falsar el
    // modelo modal; la pata operativa de P-A es Delta phi.
    const auto modes = roomModes();
    const TransducerParams p;
    const double dfs = resonanceShiftHz(p, modes, kCorner, kCenter);
    const double w = 2.0 * kPi * p.fsHz;
    const double dm = p.sd * p.sd *
                      (radiationImpedance(modes, w, kCorner).imag() -
                       radiationImpedance(modes, w, kCenter).imag()) /
                      w;
    std::printf("P-A.4 modal: Delta f_s = %.3f Hz ; dm = %.2e kg (F2 "
                "suponía 0.2-0.8 Hz y (2-5)e-4 kg)\n",
                dfs, dm);
    EXPECT_LT(std::abs(dfs), 0.05);
    EXPECT_LT(std::abs(dm), 1.0e-4);
}

TEST(CoupledModel, PCVelocityNotch) {
    const auto modes = roomModes();
    TransducerParams p;
    double worst = 1.0;
    for (const RoomMode& m : modes) {
        const double r =
            velocityRatio(p, modes, m.omega, kCorner);
        worst = std::min(worst, r);
    }
    std::printf("P-C analítico esquina: min|v_c/v_0| = %.4f (F2: 0.96-0.98, "
                "medido 0.9838)\n",
                worst);
    EXPECT_GT(worst, 0.96);
    EXPECT_LT(worst, 0.995);
    // Extremo superior (S_d=0.030, r_m=0.8): >= 0.96.
    TransducerParams pHi;
    pHi.sd = 0.030;
    pHi.rM = 0.8;
    double worstHi = 1.0;
    for (const RoomMode& m : modes)
        worstHi = std::min(worstHi, velocityRatio(pHi, modes, m.omega, kCorner));
    std::printf("P-C extremo (S_d=0.030, r_m=0.8): min|v_c/v_0| = %.4f\n",
                worstHi);
    EXPECT_GT(worstHi, 0.96);
    // Dinámico: |v| con acoplo ON vs OFF a 49.0 Hz (muesca más fuerte).
    std::vector<double> vOn, vOff;
    for (int coupled = 1; coupled >= 0; --coupled) {
        CoupledSimulator::Config cfg;
        cfg.fs = 48000.0;
        cfg.westervelt = false;
        cfg.coupled = coupled != 0;
        cfg.rs = kCorner;
        CoupledSimulator sim(cfg);
        std::vector<double> vv;
        for (int i = 0; i < 96000; ++i) {
            sim.step(std::sin(2.0 * kPi * 49.0 * i / 48000.0));
            if (i >= 48000) vv.push_back(sim.v());
        }
        if (coupled) vOn = vv; else vOff = vv;
    }
    const double dyn = amplitudeAt(vOn, 48000.0, 49.0) /
                       amplitudeAt(vOff, 48000.0, 49.0);
    const double ana = velocityRatio(p, modes, 2.0 * kPi * 49.0, kCorner);
    std::printf("P-C dinámico 49 Hz: |v_on/v_off| = %.4f ; analítico %.4f\n",
                dyn, ana);
    EXPECT_NEAR(dyn, ana, 0.005);
}

TEST(CoupledModel, PB2GeneratedComponentDecaysAtHalfTau) {
    // Ráfaga 68.6 Hz, 500 ms, corte. La componente a 137.2 Hz de
    // Pi_2[p²](t) (lo GENERADO, no el repique del modo) decae con
    // tau_1/2 = 24.6 ms [D6]. Ventanas de 10 ms, ajuste log-lineal.
    CoupledSimulator::Config cfg;
    cfg.fs = 48000.0;
    cfg.westervelt = true;
    cfg.rs = kCorner;
    cfg.rl = kCenter;
    CoupledSimulator sim(cfg);
    std::size_t k2 = 0;
    for (std::size_t k = 0; k < sim.numModes(); ++k) {
        const auto& m = sim.modes()[k];
        if (m.nx == 0 && m.ny == 0 && m.nz == 2) k2 = k;
    }
    std::vector<double> pi2;
    const int nBurst = 24000, nTail = 48000;
    for (int i = 0; i < nBurst + nTail; ++i) {
        const double u =
            (i < nBurst) ? 8.0 * std::sin(2.0 * kPi * 68.6 * i / 48000.0) : 0.0;
        sim.step(u);
        if (i >= nBurst) pi2.push_back(sim.piP2(k2));
    }
    double amps[4], times[4];
    for (int w = 0; w < 4; ++w) {
        const int a = w * 480;
        std::vector<double> seg(pi2.begin() + a, pi2.begin() + a + 480);
        amps[w] = amplitudeAt(seg, 48000.0, 137.2);
        times[w] = (a + 240) / 48000.0;
        std::printf("P-B2 ventana %d (%.0f ms): amp137 = %.3e Pa^2\n", w,
                    times[w] * 1000.0, amps[w]);
    }
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int cnt = 0;
    for (int w = 0; w < 4; ++w) {
        if (amps[w] <= 0) continue;
        sx += times[w];
        sy += std::log(amps[w]);
        sxx += times[w] * times[w];
        sxy += times[w] * std::log(amps[w]);
        ++cnt;
    }
    ASSERT_GE(cnt, 3);
    const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    const double tauFit = -1.0 / slope;
    std::printf("P-B2: tau(comp. 137.2 Hz) = %.1f ms ; tau_1/2 = %.1f ms ; "
                "tau_1 = %.1f ms\n",
                tauFit * 1000.0, modalTau2() * 1000.0, modalTau1() * 1000.0);
    EXPECT_GT(tauFit, 0.012);             // claramente no instantáneo
    EXPECT_LT(tauFit, 0.035);             // claramente tau_1/2, no tau_1
    EXPECT_LT(tauFit, 0.75 * modalTau1());
}

TEST(CoupledModel, DiscreteEnergyBalancePerHop) {
    // F2 §2.2.4 (mejorado): balance promediado por hop, cuadratura RK4.
    // Lineal, 100 Hz, 1 V, 0.5 s a 192 kHz, hop 512 (2.67 ms).
    CoupledSimulator::Config cfg;
    cfg.fs = kFsOffline;
    cfg.westervelt = false;
    cfg.jilesAtherton = false;
    CoupledSimulator sim(cfg);
    const int n = static_cast<int>(0.5 * cfg.fs);
    std::vector<double> u(n);
    for (int i = 0; i < n; ++i)
        u[i] = std::sin(2.0 * kPi * 100.0 * i / cfg.fs);
    const double err = sim.maxHopBalanceError(u, 512);
    std::printf("BALANCE lineal: max err/hop = %.3e (criterio 1e-6)\n", err);
    EXPECT_LT(err, 1e-6);
    // Con Westervelt ON el balance también se mantiene (perturbativo).
    CoupledSimulator::Config cfgNl = cfg;
    cfgNl.westervelt = true;
    CoupledSimulator simNl(cfgNl);
    const double errNl = simNl.maxHopBalanceError(u, 512);
    std::printf("BALANCE westervelt: max err/hop = %.3e (criterio 1e-6)\n",
                errNl);
    EXPECT_LT(errNl, 1e-6);
}

TEST(CoupledModel, ExponentialIntegratorIsExactWhileRk4Fails) {
    // Auditoría F2 §3.1: RK4 a 48 kHz es estable pero inexacto > 4 kHz.
    // El integrador exponencial es exacto en fase a 16 y 20 kHz.
    // Valores EXACTOS de fase (arg R(iy), rama continua) [D7], con la
    // convención positivo = RK4 ADELANTA: la tabla de F2 usaba la
    // aproximación y^5/120, que a 16 kHz da +57.7° con el signo cambiado
    // (el valor exacto es +14.4°/ciclo, adelanto) y a 20 kHz 140.9° en
    // vez de +164.6°/ciclo. A 4-8 kHz RK4 retrasa (-0.2°/-2.3°).
    const double h = 1.0 / kFsAudio;
    const struct {
        double freq, rk4Amp, rk4PhaseDeg, tolDeg;
    } audit[] = {{4000, 0.9999, -0.2, 0.1}, {8000, 0.9921, -2.3, 0.5},
                 {16000, 0.6859, 14.4, 2.0}, {20000, 0.5995, 164.6, 10.0}};
    for (const auto& row : audit) {
        const double w = 2.0 * kPi * row.freq;
        const auto rk4 = rk4OscillatorStep(w, h);
        const auto exp = expOscillatorStep(w, h);
        std::printf("INTEGRADOR %.0f Hz: RK4 amp/paso %.4f (audit %.4f), "
                    "fase %+.1f deg/ciclo (exacto %+.1f) ; EXP amp %.12f, "
                    "fase %.2e deg/ciclo\n",
                    row.freq, rk4.first, row.rk4Amp, rk4.second,
                    row.rk4PhaseDeg, exp.first, exp.second);
        EXPECT_NEAR(rk4.first, row.rk4Amp, 0.01);
        EXPECT_NEAR(rk4.second, row.rk4PhaseDeg, row.tolDeg);
        EXPECT_NEAR(exp.first, 1.0, 1e-9);
        EXPECT_LT(std::abs(exp.second), 1e-6);
    }
    // Y el oscilador libre exacto conserva la fase 100 ciclos después.
    ExpOscillator osc(2.0 * kPi * 16000.0, 0.0, h);
    double y = 1.0, v = 0.0;
    const int steps = static_cast<int>(100.0 * 48000.0 / 16000.0);
    for (int i = 0; i < steps; ++i) osc.step(y, v, 0.0);
    std::printf("EXP-OSC 16 kHz, y tras 100 ciclos = %.12f (exacto 1.0)\n", y);
    EXPECT_NEAR(y, 1.0, 1e-9);
}

TEST(CoupledModel, DuffingAliasingDcAt48kCleanAt192k) {
    // F2 §3.3: el 3er armónico de 16 kHz (48 kHz) pliega a DC a 48 kHz.
    // A 192 kHz (Nyquist 96 kHz) no hay pliegue.
    double mean48 = 0.0, mean192 = 0.0;
    for (double fs : {48000.0, 192000.0}) {
        CochlearBand b;
        const double w = 2.0 * kPi * 16000.0;
        b.omega = w;
        b.zeta = 0.05;
        b.beta = 1.0;
        b.alpha = 0.3 * w * w;
        b.g = 0.0;
        b.tauA = 1.0;
        b.tauN = 1.0;
        const double h = 1.0 / fs;
        b.prepare(h);
        const double a = 2.0 * b.zeta * w * w;  // |y| ~ 1 en resonancia
        double mean = 0.0;
        int cnt = 0;
        const int n = static_cast<int>(fs);
        for (int i = 0; i < n; ++i) {
            b.step(a * std::sin(w * i / fs), h);
            if (i >= n / 2) {
                mean += b.y;
                ++cnt;
            }
        }
        mean /= cnt;
        if (fs == 48000.0) mean48 = mean; else mean192 = mean;
        std::printf("DUFFING fs=%.0f: media(y) 2a mitad = %.4e\n", fs, mean);
    }
    EXPECT_GT(std::abs(mean48), 1e-3);   // artefacto DC real a 48 kHz
    EXPECT_LT(std::abs(mean192), 1e-9);  // limpio a 192 kHz
}

TEST(CoupledModel, TripleCouplingsSparseAndExact) {
    const auto modes = roomModes();
    const auto couplings = tripleCouplings(modes);
    std::printf("C_ijk: %zu tripletes no nulos de %zu (%.1f%%; F2 §3.4 "
                "suponía O(K^3) denso)\n",
                couplings.size(), modes.size() * modes.size() * modes.size(),
                100.0 * couplings.size() /
                    (modes.size() * modes.size() * modes.size()));
    EXPECT_GT(couplings.size(), 1500u);
    EXPECT_LT(couplings.size(), 1700u);
    // Valor exacto: C[(0,0,1),(0,0,1),(0,0,2)] = 1/sqrt(2) ≈ 0.7071.
    std::size_t i1 = 0, k2 = 0;
    for (std::size_t k = 0; k < modes.size(); ++k) {
        if (modes[k].nx == 0 && modes[k].ny == 0 && modes[k].nz == 1) i1 = k;
        if (modes[k].nx == 0 && modes[k].ny == 0 && modes[k].nz == 2) k2 = k;
    }
    double c112 = 0.0;
    for (const TripleCoupling& c : couplings)
        if (c.i == i1 && c.j == i1 && c.k == k2) c112 = c.c;
    std::printf("C[(0,0,1),(0,0,1),(0,0,2)] = %.4f (exacto 1/sqrt(2) = "
                "0.7071)\n",
                c112);
    EXPECT_NEAR(c112, 1.0 / std::sqrt(2.0), 1e-9);
}

TEST(CoupledModel, JilesAthertonHistoryDependence) {
    // P-D a nivel de implementación: dos historias, mismo i final,
    // distinta M y distinta K_t. Rango F2: Delta F/F = 2-8%.
    JilesAtherton jPos, jNeg;
    const TransducerParams p;  // defaults de ferrita típica
    jPos.p = p;
    jNeg.p = p;
    double iPrev = 0.0;
    for (int s = 0; s <= 1000; ++s) {  // H+: 0 -> +2 A
        const double i = 2.0 * s / 1000.0;
        jPos.step(iPrev, i);
        iPrev = i;
    }
    iPrev = 0.0;
    for (int s = 0; s <= 1000; ++s) {  // H-: 0 -> -2 A
        const double i = -2.0 * s / 1000.0;
        jNeg.step(iPrev, i);
        iPrev = i;
    }
    for (int s = 0; s <= 100; ++s) {  // ambas convergen a +0.5 A
        const double ip = 2.0 - 1.5 * s / 100.0;
        const double in = -2.0 + 2.5 * s / 100.0;
        jPos.step(ip - 0.015, ip);
        jNeg.step(in - 0.025, in);
    }
    const double dKt = 100.0 * p.chiM * (jPos.m - jNeg.m) / p.ms;
    std::printf("J-A: M(H+) = %.1f A/m ; M(H-) = %.1f A/m ; Delta K_t/K_t = "
                "%.2f%% (F2: 2-8%%) ; rigidez acotada: %s\n",
                jPos.m, jNeg.m, dKt,
                (jPos.stiffnessClamped || jNeg.stiffnessClamped) ? "SI"
                                                                  : "no");
    EXPECT_GT(jPos.m, jNeg.m);
    EXPECT_GT(dKt, 1.0);
    EXPECT_LT(dKt, 10.0);
    EXPECT_FALSE(jPos.stiffnessClamped);
    EXPECT_FALSE(jNeg.stiffnessClamped);
}

TEST(CoupledModel, RealTimeCorrectionsBudgetAndFidelity) {
    // F2 §3.4: presupuesto útil < 1.33 ms/hop (hop 128 @ 48 kHz).
    const auto modes = roomModes();
    ModalFeedbackRt rt(modes, kCorner, TransducerParams{}, 10);
    WesterveltEstimatorRt west;
    JaTrimRt ja;
    std::vector<double> udot(128);
    for (int i = 0; i < 128; ++i) udot[i] = 0.01 * std::sin(i * 0.1);
    const int hops = 2000;
    double sink = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int hp = 0; hp < hops; ++hp) {
        for (int i = 0; i < 128; ++i) sink += rt.step(udot[i]);
        sink += west.predictL2fDb(6.325, 1000.0, 3.0);  // 1x por hop
        sink += ja.ktFactor();                           // 1x por hop
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double usPerHop =
        std::chrono::duration<double, std::micro>(t1 - t0).count() / hops;
    std::printf("RT: %.2f us/hop (presupuesto 1330 us) ; sink=%.3f\n",
                usPerHop, sink);
    EXPECT_LT(usPerHop, 500.0);
    // Fidelidad RT-1: |F_rad| con 10 modos vs modal completo a 68.6 Hz.
    ModalFeedbackRt rt10(modes, kCorner, TransducerParams{}, 10);
    const double w = 2.0 * kPi * 68.6;
    std::vector<double> fr;
    for (int i = 0; i < 48000; ++i) {
        const double uDot = 0.020 * w * 0.001 * std::cos(w * i / 48000.0);
        const double f = rt10.step(uDot);
        if (i >= 24000) fr.push_back(f);
    }
    const double aRt = amplitudeAt(fr, 48000.0, 68.6);
    const double zFull = std::abs(radiationImpedance(modes, w, kCorner));
    const double aFull = 0.020 * zFull * 0.020 * 0.001;
    std::printf("RT-1: |F_rad| 10 modos = %.3e N ; completo = %.3e N ; "
                "ratio %.3f\n",
                aRt, aFull, aRt / aFull);
    EXPECT_GT(aRt / aFull, 0.70);
    EXPECT_LT(aRt / aFull, 1.0);
    // RT-2: L_2f predicho a 110 dB SPL / 1 kHz / 3 m = 53.4 dB SPL.
    const double l2f = west.predictL2fDb(splDbToPressurePa(110.0), 1000.0, 3.0);
    std::printf("RT-2: L_2f predicho = %.2f dB SPL (esperado 53.4)\n", l2f);
    EXPECT_NEAR(l2f, 53.4, 0.1);
    // RT-3: trim con M/M_s = 0.4 y chi_M = 0.05 -> factor 1.02.
    ja.mOverMs = 0.4;
    std::printf("RT-3: factor K_t = %.4f (esperado 1.02)\n", ja.ktFactor());
    EXPECT_NEAR(ja.ktFactor(), 1.02, 1e-9);
}

TEST(CoupledModel, RirRoomModesAndSchroeder) {
    // rir_0000.wav: sala 5.964 x 3.080 x 9.500 m (ancho x alto x profundo).
    // Ejes del modelo: x=ancho, y=profundidad, z=alto. Primeros modos
    // verificados contra Python independiente (validate_rir.py).
    RoomGeometry g;
    g.lx = 5.963877437301715;
    g.ly = 9.499518940224963;
    g.lz = 3.080304828823752;
    // T60 MEDIDO (1.221 s, Schroeder T20x3), no el nominal (0.535 s) [R2].
    g.t60 = 1.221133;
    const auto modes = roomModesFor(g, g.schroederHz());
    const double first[] = {18.1, 28.8, 34.0, 36.1, 46.2, 54.2};
    ASSERT_GE(modes.size(), 6u);
    for (int i = 0; i < 6; ++i) {
        std::printf("RIR-0000 modo %d: %.1f Hz (esperado %.1f)\n", i,
                    modes[i].freqHz, first[i]);
        EXPECT_NEAR(modes[i].freqHz, first[i], 0.15);
    }
    // f_Sch con T60 medido: 167.3 Hz; con el nominal: 110.8 Hz.
    std::printf("RIR-0000 f_Sch(medido 1.221 s) = %.1f Hz ; f_Sch(nominal "
                "0.535 s) = %.1f Hz\n",
                g.schroederHz(),
                2000.0 * std::sqrt(0.5353670983310466 / g.volume()));
    EXPECT_NEAR(g.schroederHz(), 167.3, 1.0);
    EXPECT_NEAR(2000.0 * std::sqrt(0.5353670983310466 / g.volume()), 110.8,
                1.0);
    std::printf("RIR-0000 V=%.1f m3, S=%.1f m2, modos<f_Sch: %zu\n",
                g.volume(), g.surfaceArea(), modes.size());
    EXPECT_NEAR(g.volume(), 174.5, 0.5);
}

TEST(CoupledModel, SabineAbsorptionAndWallImpedanceFromRir) {
    // rir_0000: V=174.5 m3, S=208.5 m2. alpha nominal 0.252, medida 0.110
    // (validate_rir.py). Ambas físicas (0,1), sin outliers en las 200.
    const double v = 174.5, s = 208.5;
    const double aNom = sabineMeanAbsorption(0.5353670983310466, v, s);
    const double aMeas = sabineMeanAbsorption(1.221133, v, s);
    std::printf("SABINE rir_0000: alpha nominal = %.3f ; alpha medida = "
                "%.3f ; ratio = %.3f (2.284 en el dataset)\n",
                aNom, aMeas, aNom / aMeas);
    EXPECT_NEAR(aNom, 0.252, 0.005);
    EXPECT_NEAR(aMeas, 0.110, 0.005);
    EXPECT_GT(aNom, 0.0);
    EXPECT_LT(aNom, 1.0);
    // Z_w desde alpha medida: |Z| = rho0 c0 (1+sqrt(1-a))/(1-sqrt(1-a)).
    const double zw = wallImpedanceFromAbsorption(aMeas);
    std::printf("Z_w estimada (alpha=0.110) = %.0f Pa*s/m\n", zw);
    EXPECT_NEAR(zw, 14100.0, 1000.0);
    // Casos límite: alpha -> 0 es pared rígida (|Z| -> infinito);
    // alpha = 0.5 da |Z| = 411.6 * (1+0.7071)/(1-0.7071) ≈ 2400.
    EXPECT_NEAR(wallImpedanceFromAbsorption(0.5), 2400.0, 50.0);
}

TEST(CoupledModel, KemarTwoEarsAndMultiReceiverReadout) {
    // [R3] KEMAR estándar: oídos a ±0.0875 m del centro de la cabeza.
    // A 100 Hz (lambda 3.43 m >> 0.175 m) las dos lecturas deben
    // coincidir dentro del gradiente máximo del campo (~30%).
    // El estado es único: L, R, sub y multicanal leen el mismo X.
    CoupledSimulator::Config cfg;
    cfg.fs = 48000.0;
    cfg.westervelt = false;
    cfg.rl = {2.0, 3.5, 1.2};
    CoupledSimulator sim(cfg);
    std::vector<double> pL, pR, pC;
    for (int i = 0; i < 48000; ++i) {
        sim.step(std::sin(2.0 * kPi * 100.0 * i / 48000.0));
        if (i >= 36000) {
            pL.push_back(sim.pressureAt(
                {2.0 - kKemarEarOffsetM, 3.5, 1.2}));
            pR.push_back(sim.pressureAt(
                {2.0 + kKemarEarOffsetM, 3.5, 1.2}));
            pC.push_back(sim.pressureAtListener());
        }
    }
    const double aL = amplitudeAt(pL, 48000.0, 100.0);
    const double aR = amplitudeAt(pR, 48000.0, 100.0);
    const double aC = amplitudeAt(pC, 48000.0, 100.0);
    std::printf("KEMAR 100 Hz: |p_L|=%.4e Pa ; |p_R|=%.4e Pa ; "
                "|p_centro|=%.4e Pa ; ratio L/R=%.3f\n",
                aL, aR, aC, aL / aR);
    EXPECT_GT(aL, 0.0);
    EXPECT_GT(aR, 0.0);
    EXPECT_GT(aC, 0.0);
    EXPECT_GT(aL / aR, 0.7);
    EXPECT_LT(aL / aR, 1.4);
    EXPECT_GT(aC / (0.5 * (aL + aR)), 0.7);
    EXPECT_LT(aC / (0.5 * (aL + aR)), 1.4);
    // Punto de subwoofer (2.1): lectura finita del mismo estado.
    const double pSub = sim.pressureAt({1.0, 1.0, 0.5});
    std::printf("KEMAR 2.1: p_sub = %.4e Pa (finita)\n", pSub);
    EXPECT_TRUE(std::isfinite(pSub));
}

TEST(CoupledModel, KemarItdWoodworthRediscoveredLaw) {
    // Ley ITD redescubierta por PySR desde 710 posiciones KEMAR reales
    // (coordinador de élite 2026-11-26): ITD(θ) = -1409θ + 918.6 sinθ
    // + 144.8 θ³ [µs]; RMSE 5.52 µs, R² = 0.99987. CANDIDATA [R3].
    EXPECT_NEAR(kemarItdUs(0.0), 0.0, 1e-9);
    const double itd90 = kemarItdUs(kPi / 2.0);
    const double itd45 = kemarItdUs(kPi / 4.0);
    std::printf("ITD KEMAR: θ=45° -> %.1f µs ; θ=90° -> %.1f µs "
                "(Woodworth r=9.33 cm: %.1f / %.1f µs)\n",
                itd45, itd90, woodworthItdUs(kPi / 4.0, 0.0933),
                woodworthItdUs(kPi / 2.0, 0.0933));
    EXPECT_NEAR(itd90, -733.4, 5.0);
    EXPECT_NEAR(itd45, -386.8, 5.0);
    EXPECT_LT(itd90, itd45);  // |ITD| crece con |θ|
    EXPECT_LT(itd45, 0.0);
    // Acuerdo <6% con Woodworth teórico (r/c)(θ+sinθ), r = 9.33 cm.
    EXPECT_NEAR(std::abs(itd90),
                woodworthItdUs(kPi / 2.0, 0.0933), 0.06 * 733.4);
    EXPECT_NEAR(std::abs(itd45),
                woodworthItdUs(kPi / 4.0, 0.0933), 0.06 * 386.8);
}
