// ─────────────────────────────────────────────────────────────────────────────
// geodesic_pinna_3d.hpp — Física 1: LGN 3D exacta (ΔL_j = L_base,j − d_j·û)
// + mitigación de bright-spot contralateral + eco de torso/hombro.
// Paquete FISICA_NUEVA_CONSOLIDADA (Esse, 2026-11-18). Extiende la LGN
// verificada en KEMAR (RMS 104 Hz) a 3D exacto + factor de escala por radio
// de cabeza personal. Header-only, RT-safe (sin malloc/locks/IO).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace ivanna::newphys {

struct PinnaVector3D {
    double baseLenM;
    double dx, dy, dz; // canal -> borde (oído izquierdo, metros)
    double rho;        // profundidad relativa del dispersor
};

class GeodesicPinna3D {
public:
    static constexpr double kSoundSpeed = 343.0;
    static constexpr double kKemarRadius = 0.0875;

    // Vectores 3D derivados exactamente de los parámetros medidos en PHYSICAL_HEAD_MODEL:
    // Baja: M=23.90 mm, B=8.70 mm, phi0=-18° -> dx = -B*sin(-18°)=+2.688 mm, dz = B*cos(-18°)=+8.274 mm
    // Alta: M=16.00 mm, B=3.48 mm, phi0=-31° -> dx = -B*sin(-31°)=+1.792 mm, dz = B*cos(-31°)=+2.983 mm
    // dy inicial estimado por anchura lateral de concha/hélix (ajustable contra 710 pos SOFA)
    std::array<PinnaVector3D, 2> families{{
        {0.02390, 0.002688, 0.00440, 0.008274, 0.70},
        {0.01600, 0.001792, 0.00220, 0.002983, 0.55}
    }};

    // Evalúa factor complejo H_ear(f) / H_esfera_base para oído L o R
    std::complex<double> evalTransfer(double freqHz, double azRad, double elRad,
                                      bool isRightEar, double headRadiusM,
                                      double pinnaMix, double brightSpotDamp,
                                      double torsoEchoMix) const noexcept {
        if (freqHz <= 1.0) return {1.0, 0.0};

        const double ce = std::cos(elRad);
        const double ux = ce * std::cos(azRad);
        const double uy = ce * std::sin(azRad); // +y = izquierda
        const double uz = std::sin(elRad);
        const double ySign = isRightEar ? -1.0 : 1.0;

        // Escalado anatómico proporcional si se calibró radio de cabeza personal
        const double scale = std::clamp(headRadiusM / kKemarRadius, 0.75, 1.30);
        const double omega = 2.0 * M_PI * freqHz;

        // 1) Interferencia Geodésica 3D (dos familias de bordes)
        std::complex<double> pinnaSum(1.0, 0.0);
        double norm = 1.0;
        for (const auto& f : families) {
            const double dot = (f.dx * ux + (f.dy * ySign) * uy + f.dz * uz) * scale;
            const double deltaL = std::max(0.001, f.baseLenM * scale - dot);
            const double tau = deltaL / kSoundSpeed;
            // Notch destructivo cuando omega * tau = pi
            pinnaSum += f.rho * std::polar(1.0, omega * tau);
            norm += f.rho;
        }
        const std::complex<double> H_pinna =
            (1.0 - pinnaMix) + pinnaMix * (pinnaSum / norm);

        // 2) Mitigación física del Bright Spot contralateral (γ -> 0 en esfera rígida)
        // Dirección del oído: izquierdo en (0, +1, 0), derecho en (0, -1, 0)
        const double earY = isRightEar ? -1.0 : 1.0;
        const double cosAlpha = std::clamp(uy * earY, -1.0, 1.0); // +1 = ipsilateral, -1 = contralateral
        // En esfera, antípoda del oído (gamma = acos(-cosAlpha) -> 0) tiene bright spot
        const double gamma = std::acos(std::clamp(-cosAlpha, -1.0, 1.0));
        const double ka = omega * headRadiusM / kSoundSpeed;
        double bsGain = 1.0;
        if (ka > 1.5 && brightSpotDamp > 0.0) {
            const double sigKa = 1.0 / (1.0 + std::exp(-(ka - 2.2)));
            const double angExp = std::exp(-(gamma * gamma) / (2.0 * 0.45 * 0.45));
            bsGain = 1.0 - brightSpotDamp * sigKa * angExp;
        }

        // 3) Eco de torso/hombro (< 3 kHz, clave física de elevación en graves)
        std::complex<double> H_torso(1.0, 0.0);
        if (torsoEchoMix > 1e-5) {
            const double tauSh = (0.22 / kSoundSpeed) * (1.0 - uz);
            const std::complex<double> lp =
                torsoEchoMix / std::complex<double>(1.0, freqHz / 1500.0);
            H_torso = (1.0 + lp * std::polar(1.0, -omega * tauSh)) / (1.0 + torsoEchoMix);
        }

        return H_pinna * bsGain * H_torso;
    }

    // Frecuencia predicha del notch principal (para telemetría y test)
    double predictNotchHz(int familyIdx, double azRad, double elRad,
                          bool isRightEar, double headRadiusM) const noexcept {
        const auto& f = families[std::clamp(familyIdx, 0, 1)];
        const double ce = std::cos(elRad);
        const double ux = ce * std::cos(azRad);
        const double uy = ce * std::sin(azRad);
        const double uz = std::sin(elRad);
        const double ySign = isRightEar ? -1.0 : 1.0;
        const double scale = std::clamp(headRadiusM / kKemarRadius, 0.75, 1.30);
        const double dot = (f.dx * ux + (f.dy * ySign) * uy + f.dz * uz) * scale;
        const double deltaL = std::max(0.001, f.baseLenM * scale - dot);
        return kSoundSpeed / (2.0 * deltaL);
    }
};

} // namespace ivanna::newphys
