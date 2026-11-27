// coupled_predict.cpp — HERRAMIENTA OFFLINE de predicción (Fase 3).
//
// El modelo acoplado completo como predictor/identificador OFFLINE (F2
// §3.4: no cabe en tiempo real). Uso:
//   coupled_predict                 predicciones P-A..P-D + balance
//   coupled_predict run F V S       simula S segundos de seno F Hz a V
//                                   voltios; vuelca p_l(t) a stdout (CSV)
//   coupled_predict rundec F V S    igual que run pero DESACOPLADO
//                                   (F_rad = 0): control para R-G
//   coupled_predict pa              curva Delta phi 40-140 Hz (CSV)
// Compilar (sin dependencias, C++17):
//   g++-14 -O2 -std=c++17 -o coupled_predict coupled_predict.cpp
// Ver README.md en este directorio. NO forma parte del build de Android:
// es herramienta de Fase 3 en el host (VM).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "coupled_model.hpp"

using namespace ivanna::fused::coupled;

namespace {
constexpr double kPi = 3.14159265358979323846;
const Vec3 kCorner{0.3, 0.3, 1.2};
const Vec3 kCenter{2.0, 3.5, 1.2};

void printPredictions() {
    const auto modes = roomModes();
    const TransducerParams p;
    std::printf("# MODELO ACOPLADO NO LINEAL — predicciones (F2, con "
                "correcciones Fase 3)\n");
    std::printf("# Sala 4.0x7.0x2.5 m, V=70 m3, T60=0.34 s, f_Sch=139.4 Hz, "
                "%zu modos\n",
                modes.size());
    std::printf("# Parámetros Genezi: SUPUESTOS (S_d=%.3f m2, r_m=%.1f, "
                "m=%.3f kg, f_s=%.0f Hz) — reemplazar por identificación\n",
                p.sd, p.rM, p.mMs, p.fsHz);
    std::printf("\n# P-A: Delta phi esquina (max) y centro\n");
    double mx = 0, mxf = 0, mxC = 0;
    for (int fi = 80; fi <= 280; ++fi) {
        const double f = fi * 0.5;
        const double d = std::abs(deltaPhiDeg(p, modes, 2 * kPi * f, kCorner));
        if (d > mx) { mx = d; mxf = f; }
        mxC = std::max(mxC, std::abs(deltaPhiDeg(p, modes, 2 * kPi * f, kCenter)));
    }
    std::printf("PA,max_dphi_corner_deg=%.2f @ %.1f Hz\n", mx, mxf);
    std::printf("PA,max_dphi_center_deg=%.2f\n", mxC);
    const double dfs = resonanceShiftHz(p, modes, kCorner, kCenter);
    std::printf("PA,delta_fs_modal_Hz=%.3f  # [D5] la suma modal exacta NO "
                "reproduce 0.2-0.8 Hz de F2\n",
                dfs);
    std::printf("\n# P-B: 2o armónico Westervelt (convención F2, P=RMS)\n");
    for (const auto& r : {std::make_tuple(90.0, 1000.0, 3.0),
                          std::make_tuple(100.0, 1000.0, 3.0),
                          std::make_tuple(110.0, 1000.0, 3.0),
                          std::make_tuple(110.0, 1000.0, 1.5),
                          std::make_tuple(110.0, 4000.0, 3.0),
                          std::make_tuple(100.0, 68.6, 3.0)}) {
        double spl, f, z;
        std::tie(spl, f, z) = r;
        std::printf("PB,L2f-Lf_dB=%.1f @ %.0f dB SPL / %.1f Hz / %.1f m "
                    "(Beyer-pico: %.1f)\n",
                    westerveltRatioDb(kBetaAir, f, z, spl), spl, f, z,
                    westerveltRatioDbBeyerPeak(kBetaAir, f, z, spl));
    }
    std::printf("\n# P-C: muesca de velocidad en resonancias modales\n");
    double worst = 1.0;
    for (const RoomMode& m : modes)
        worst = std::min(worst, velocityRatio(p, modes, m.omega, kCorner));
    std::printf("PC,min_vc_v0=%.4f  # F2: 0.96-0.98\n", worst);
    std::printf("\n# P-D: memoria magnética (orden de magnitud, chi_M "
                "supuesto 0.03-0.08)\n");
    std::printf("PD,deltaF_F_pct=2-8  # condicional a chi_M sin identificar; "
                "N>=20, test t\n");
    std::printf("\n# P-B2: cola modal 137.2 Hz\n");
    std::printf("PB2,L2f-Lf_dB=%.1f tau1_ms=%.1f tau2_ms=%.1f\n",
                westerveltRatioDb(kBetaAir, 68.6, 3.0, 110),
                modalTau1() * 1000.0, modalTau2() * 1000.0);
}

void printPaCurve() {
    const auto modes = roomModes();
    const TransducerParams p;
    std::printf("freq_Hz,delta_phi_corner_deg,delta_phi_center_deg,"
                "vc_v0_corner\n");
    for (int fi = 80; fi <= 280; ++fi) {
        const double f = fi * 0.5;
        const double w = 2 * kPi * f;
        std::printf("%.1f,%.3f,%.3f,%.4f\n", f, deltaPhiDeg(p, modes, w, kCorner),
                    deltaPhiDeg(p, modes, w, kCenter),
                    velocityRatio(p, modes, w, kCorner));
    }
}

int runSim(double freq, double volt, double seconds, bool coupled = true) {
    CoupledSimulator::Config cfg;
    cfg.fs = kFsOffline;
    cfg.westervelt = true;
    cfg.coupled = coupled;  // false = control desacoplado para R-G
    cfg.rs = kCorner;
    cfg.rl = kCenter;
    CoupledSimulator sim(cfg);
    const int n = static_cast<int>(seconds * cfg.fs);
    std::vector<double> u(n);
    for (int i = 0; i < n; ++i)
        u[i] = volt * std::sin(2 * kPi * freq * i / cfg.fs);
    // Balance en la misma corrida (hop 512 @192 kHz = 2.67 ms).
    const double err = sim.maxHopBalanceError(u, 512);
    std::fprintf(stderr, "# balance max err/hop = %.3e (criterio 1e-6)\n",
                 err);
    // Segunda corrida para volcar p_l(t) (el balance consume su propia
    // copia; esta es la corrida de volcado).
    CoupledSimulator sim2(cfg);
    std::printf("t_s,p_listener_Pa,v_m_s,i_A,E_J\n");
    for (int i = 0; i < n; ++i) {
        sim2.step(u[i]);
        if (i % 4 == 0)  // volcado a 48 kHz efectivos
            std::printf("%.6f,%.6e,%.6e,%.6e,%.6e\n", i / cfg.fs,
                        sim2.pressureAtListener(), sim2.v(), sim2.current(),
                        sim2.totalEnergy());
    }
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "pa") == 0) {
        printPaCurve();
        return 0;
    }
    if (argc >= 2 && std::strcmp(argv[1], "run") == 0) {
        const double freq = argc > 2 ? std::atof(argv[2]) : 100.0;
        const double volt = argc > 3 ? std::atof(argv[3]) : 1.0;
        const double sec = argc > 4 ? std::atof(argv[4]) : 1.0;
        return runSim(freq, volt, sec);
    }
    if (argc >= 2 && std::strcmp(argv[1], "rundec") == 0) {
        const double freq = argc > 2 ? std::atof(argv[2]) : 100.0;
        const double volt = argc > 3 ? std::atof(argv[3]) : 1.0;
        const double sec = argc > 4 ? std::atof(argv[4]) : 1.0;
        return runSim(freq, volt, sec, false);
    }
    printPredictions();
    return 0;
}
