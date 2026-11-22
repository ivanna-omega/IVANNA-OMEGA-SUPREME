// geodesic_pinna_model.cpp — Operador de Interferencia Geodésica.
// Ver geodesic_pinna_model.hpp para el principio, las constantes medidas y
// los límites honestos.
#include "fused/geodesic_pinna_model.hpp"

#include "fused/physical_head_model.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::physical {
namespace {
constexpr double kPi = 3.14159265358979323846;
}

double pinnaPathDifferenceMm(const PinnaNotchFamily& family, double elevationRad) noexcept {
    if (!std::isfinite(elevationRad)) return family.meanPathMm;
    return family.meanPathMm - family.amplitudeMm * std::sin(elevationRad - family.phaseRad);
}

double pinnaPathDifferenceMm(const PinnaNotchFamily& family, double elevationRad,
                             double azimuthRad) noexcept {
    double dl = pinnaPathDifferenceMm(family, elevationRad);
    if (!std::isfinite(azimuthRad)) return dl;
    // Corrección de azimut de primer orden (APROXIMADA, ver cabecera):
    // el desvío se acorta al alejarse del frontal.
    dl -= kPinnaAzimuthDropMm * 0.5 * (1.0 - std::cos(azimuthRad));
    return dl;
}

double pinnaNotchFrequencyHz(const PinnaNotchFamily& family, double elevationRad,
                             double speedOfSoundMps) noexcept {
    if (!(speedOfSoundMps > 0.0)) return 0.0;
    const double dlMm = pinnaPathDifferenceMm(family, elevationRad);
    if (!(dlMm > 0.0)) return 0.0;
    return speedOfSoundMps / (2.0 * dlMm * 1.0e-3);
}

std::complex<double> pinnaFactor(double freqHz, double elevationRad, double azimuthRad,
                                 double speedOfSoundMps) noexcept {
    if (!(freqHz > 0.0) || !(speedOfSoundMps > 0.0)) return {1.0, 0.0};
    const double rho1 = std::clamp(kPinnaLowFamily.rho, 0.0, 1.0);
    const double rho2 = std::clamp(kPinnaHighFamily.rho, 0.0, 1.0);
    const double norm = 1.0 + rho1 + rho2;
    const double tau1 = pinnaPathDifferenceMm(kPinnaLowFamily, elevationRad, azimuthRad) * 1.0e-3 /
                        speedOfSoundMps;
    const double tau2 = pinnaPathDifferenceMm(kPinnaHighFamily, elevationRad, azimuthRad) * 1.0e-3 /
                        speedOfSoundMps;
    // Convención e^{-iwt}: un retardo tau multiplica por e^{+i·w·tau}.
    const std::complex<double> e1 =
        std::exp(std::complex<double>(0.0, 2.0 * kPi * freqHz * tau1));
    const std::complex<double> e2 =
        std::exp(std::complex<double>(0.0, 2.0 * kPi * freqHz * tau2));
    return (std::complex<double>(1.0, 0.0) + rho1 * e1 + rho2 * e2) / norm;
}

bool headPlusPinnaEarTransfer(double freqHz, double azimuthRad, double elevationRad,
                              std::complex<double>& outLeft, std::complex<double>& outRight,
                              double speedOfSoundMps) noexcept {
    if (!(freqHz > 0.0) || !(speedOfSoundMps > 0.0) || !std::isfinite(azimuthRad) ||
        !std::isfinite(elevationRad)) {
        return false;
    }
    const EarTransfer sphere = rigidSphereEarTransfer(freqHz, azimuthRad, kHeadRadiusM,
                                                      speedOfSoundMps);
    const std::complex<double> fPinna =
        pinnaFactor(freqHz, elevationRad, azimuthRad, speedOfSoundMps);
    outLeft = sphere.left * fPinna;
    outRight = sphere.right * fPinna;
    return true;
}

}  // namespace ivanna::physical
