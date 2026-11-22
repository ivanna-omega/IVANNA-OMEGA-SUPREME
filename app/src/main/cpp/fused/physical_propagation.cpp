// physical_propagation.cpp — ver physical_propagation.hpp para las
// ecuaciones, el mapa de reemplazo y el veredicto de honestidad.
// Funciones puras en doble precisión, sin I/O, sin estado, sin alloc.
#include "fused/physical_propagation.hpp"

#include <algorithm>

namespace ivanna::fused::propagation {
namespace {

// ── Utilidades fail-closed ─────────────────────────────────────────────────
bool finitePositive(double v) noexcept { return std::isfinite(v) && v > 0.0; }

// Pliega el azimut a [0, π/2] con su signo. Simetrías exactas del modelo
// de cabeza esférica (oídos sobre el eje x, fuente en el plano horizontal):
//   ITD(−θ) = −ITD(θ)                    (antisimetría izquierda/derecha)
//   ITD(θ)  = ITD(π − θ)  para θ > π/2   (espejo frontal/trasero: la
//     geometría solo ve el ángulo γ = π/2 − θ respecto al eje interaural,
//     y el ITD es par en γ).
// Devuelve (signo, θf). θ no finito → (1, 0).
void foldAzimuth(double azimuthRad, double& sign, double& thetaFolded) noexcept {
    if (!std::isfinite(azimuthRad)) {
        sign = 1.0;
        thetaFolded = 0.0;
        return;
    }
    double th = std::fmod(azimuthRad, 2.0 * kPi);
    sign = 1.0;
    if (th < 0.0) {
        sign = -1.0;
        th = -th;
    }
    if (th > kPi) th = 2.0 * kPi - th;
    if (th > kPi * 0.5) th = kPi - th;
    thetaFolded = th;
}

// Camino acústico exacto (forma cerrada, sin serie) desde una fuente a un
// oído sobre la esfera rígida — rayo directo si no lo bloquea la esfera,
// si no el mínimo de los dos caminos de borde (tangente + arco, onda
// reptante). Es la geometría de Kuhn / Duda & Martens, la misma que
// NearFieldHeadModel implementa en float; aquí en double y autocontenida
// (el módulo no depende de spatial/). earSign: +1 oído derecho (+a, 0),
// −1 oído izquierdo (−a, 0).
double pathLengthToEar(double thetaRad, double r, double earSign, double a) noexcept {
    const double rr = std::clamp(r, a + 0.01, 100.0);
    const double th = std::clamp(thetaRad, -kPi, kPi);
    const double sx = rr * std::sin(th);
    const double sy = rr * std::cos(th);
    const double ex = earSign * a;
    const double dx = sx - ex, dy = sy;
    const double direct = std::sqrt(dx * dx + dy * dy);
    const double segLen2 = dx * dx + dy * dy;
    bool blocked = false;
    if (segLen2 > 1e-12) {
        const double t = (sx * dx + sy * dy) / segLen2;
        if (t > 1e-6 && t < 1.0 - 1e-6) {
            const double cx = sx - t * dx, cy = sy - t * dy;
            blocked = (cx * cx + cy * cy) < (a * a * (1.0 - 1e-6));
        }
    }
    double best = blocked ? 1e30 : direct;
    const double tangLen = std::sqrt(rr * rr - a * a);
    const double gamma = std::atan2(sy, sx);
    const double beta = std::acos(a / rr);
    const double earAng = (earSign > 0.0) ? 0.0 : kPi;
    for (int s = -1; s <= 1; s += 2) {
        const double tangAng = gamma + static_cast<double>(s) * beta;
        double dAng = tangAng - earAng;
        while (dAng > kPi) dAng -= 2.0 * kPi;
        while (dAng < -kPi) dAng += 2.0 * kPi;
        const double total = tangLen + a * std::fabs(dAng);
        if (total < best) best = total;
    }
    return best;
}

double clampDistance(double distanceM, double headRadiusM) noexcept {
    if (!std::isfinite(distanceM)) return 1.0;
    return std::clamp(distanceM, headRadiusM + 0.01, 100.0);
}

}  // namespace

// ═══ PARTE I — PISO ═══════════════════════════════════════════════════════

double speedOfSound(double tempCelsius) noexcept {
    if (!std::isfinite(tempCelsius)) return kSpeedOfSoundMps;
    const double t = std::clamp(tempCelsius, -40.0, 60.0);
    return 331.3 * std::sqrt(1.0 + t / 273.15);
}

double sphericalSpreadingGain(double distanceM, double refDistanceM) noexcept {
    if (!finitePositive(distanceM) || !finitePositive(refDistanceM)) return 1.0;
    return refDistanceM / distanceM;
}

double sphericalSpreadingDb(double distanceM, double refDistanceM) noexcept {
    return 20.0 * std::log10(sphericalSpreadingGain(distanceM, refDistanceM));
}

double cylindricalSpreadingGain(double distanceM, double refDistanceM) noexcept {
    if (!finitePositive(distanceM) || !finitePositive(refDistanceM)) return 1.0;
    return std::sqrt(refDistanceM / distanceM);
}

double sceneDistanceGainPhysical(double distanceM, double minGain, double maxGainDb) noexcept {
    if (!finitePositive(distanceM)) return 1.0;
    const double lo = (std::isfinite(minGain) && minGain > 0.0) ? minGain : 0.5;
    const double hi = std::isfinite(maxGainDb)
                          ? std::pow(10.0, maxGainDb / 20.0)
                          : std::pow(10.0, 3.0 / 20.0);
    const double g = 1.0 / distanceM;
    return std::clamp(g, lo, hi);
}

double propagationDelaySeconds(double distanceM, double speedOfSound) noexcept {
    if (!finitePositive(distanceM) || !finitePositive(speedOfSound)) return 0.0;
    return distanceM / speedOfSound;
}

double propagationDelaySamples(double distanceM, double sampleRate,
                               double speedOfSound) noexcept {
    if (!finitePositive(sampleRate)) return 0.0;
    return propagationDelaySeconds(distanceM, speedOfSound) * sampleRate;
}

double airAbsorptionCoeffDbPerM(double freqHz, double tempCelsius, double humidityRel,
                                double pressurePa) noexcept {
    // ISO 9613-1:1993, §5 (fórmula, no tabla). f en Hz, T en K, p en Pa,
    // h = concentración molar de vapor de agua en %.
    if (!finitePositive(freqHz)) return 0.0;
    const double f = freqHz;
    const double tC = std::isfinite(tempCelsius) ? std::clamp(tempCelsius, -20.0, 50.0) : 20.0;
    const double hr = std::isfinite(humidityRel) ? std::clamp(humidityRel, 0.0, 1.0) : 0.70;
    const double pa = (std::isfinite(pressurePa) && pressurePa > 0.0) ? pressurePa : 101325.0;
    constexpr double kT0 = 293.15;    // 20 °C en K
    constexpr double kT01 = 273.16;   // punto triple del agua en K
    constexpr double kPr = 101325.0;  // presión de referencia
    const double tK = tC + 273.15;
    const double pSat = kPr * std::pow(10.0, -6.8346 * std::pow(kT01 / tK, 1.261) + 4.6151);
    const double h = hr * pSat / pa * 100.0;
    const double frO = (pa / kPr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
    const double frN = (pa / kPr) * std::sqrt(tK / kT0) *
                       (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(tK / kT0, -1.0 / 3.0) - 1.0)));
    const double alpha =
        8.686 * f * f *
        (1.84e-11 * (kPr / pa) * std::sqrt(tK / kT0) +
         std::pow(tK / kT0, -2.5) *
             (0.01275 * std::exp(-2239.1 / tK) * frO / (frO * frO + f * f) +
              0.1068 * std::exp(-3352.0 / tK) * frN / (frN * frN + f * f)));
    return (std::isfinite(alpha) && alpha > 0.0) ? alpha : 0.0;
}

double airAbsorptionDb(double freqHz, double distanceM, double tempCelsius,
                       double humidityRel, double pressurePa) noexcept {
    if (!finitePositive(distanceM)) return 0.0;
    return airAbsorptionCoeffDbPerM(freqHz, tempCelsius, humidityRel, pressurePa) * distanceM;
}

double dopplerFactor(double radialVelocityMps, double speedOfSound) noexcept {
    if (!std::isfinite(radialVelocityMps) || !finitePositive(speedOfSound)) return 1.0;
    if (std::fabs(radialVelocityMps) >= 0.9 * speedOfSound) return 1.0;
    return speedOfSound / (speedOfSound - radialVelocityMps);
}

double dopplerShift(double freqHz, double radialVelocityMps, double speedOfSound) noexcept {
    if (!finitePositive(freqHz)) return 0.0;
    return freqHz * dopplerFactor(radialVelocityMps, speedOfSound);
}

ConstantPowerMix constantPowerMix(double t) noexcept {
    const double tc = std::isfinite(t) ? std::clamp(t, 0.0, 1.0) : 0.0;
    const double ang = tc * (kPi * 0.5);
    return ConstantPowerMix{std::cos(ang), std::sin(ang)};
}

NearFieldPaths nearFieldParallax(double azimuthRad, double distanceM, double headRadiusM,
                                 double speedOfSound) noexcept {
    const double a = (std::isfinite(headRadiusM) && headRadiusM > 0.0)
                         ? std::clamp(headRadiusM, 0.05, 0.15)
                         : kHeadRadiusKemarM;
    const double r = clampDistance(distanceM, a);
    const double c = finitePositive(speedOfSound) ? speedOfSound : kSpeedOfSoundMps;
    NearFieldPaths out;
    out.pathLeftM = pathLengthToEar(azimuthRad, r, -1.0, a);
    out.pathRightM = pathLengthToEar(azimuthRad, r, +1.0, a);
    out.itdSeconds = (out.pathLeftM - out.pathRightM) / c;
    out.gainLeft = 1.0 / out.pathLeftM;
    out.gainRight = 1.0 / out.pathRightM;
    out.ildDivergenceDb = 20.0 * std::log10(out.pathLeftM / out.pathRightM);
    return out;
}

// ═══ PARTE II — LEY DE CURVATURA DE ORBE ═════════════════════════════════

double wavefrontCurvature(double distanceM) noexcept {
    if (!finitePositive(distanceM)) return 1.0;
    return 1.0 / distanceM;
}

double curvatureLawItdSeconds(double azimuthRad, double distanceM, double headRadiusM,
                              double speedOfSound) noexcept {
    const double a = (std::isfinite(headRadiusM) && headRadiusM > 0.0)
                         ? std::clamp(headRadiusM, 0.05, 0.15)
                         : kHeadRadiusKemarM;
    const double r = clampDistance(distanceM, a);
    const double c = finitePositive(speedOfSound) ? speedOfSound : kSpeedOfSoundMps;
    const double eps = a / r;  // ε = a·κ
    double sign = 1.0, th = 0.0;
    foldAzimuth(azimuthRad, sign, th);
    // Rama C (|θf| ≤ θ* = asin ε): ambos oídos por onda reptante; las
    // longitudes de tangente se cancelan y los arcos difieren en 2θf:
    // ITD = (a/c)·2θf EXACTO (sin serie). En θf = 0 da 0 exacto, como
    // exige la simetría — la serie de la rama D sola NO lo cumpliría.
    const double thetaStar = std::asin(std::min(eps, 1.0));
    if (th <= thetaStar) return sign * (a / c) * 2.0 * th;
    // Rama D: ipsilateral directo + contralateral reptante; serie en ε
    // hasta O(ε⁵) (coeficientes verificados con sympy; la rama C y la D
    // coinciden en θ* hasta O(ε⁵), ver test de continuidad).
    const double s = std::sin(th);
    const double s2 = s * s;
    const double e2 = eps * eps;
    const double e3 = e2 * eps;
    const double e4 = e3 * eps;
    const double e5 = e4 * eps;
    const double bracket = th + s + (eps * 0.5) * s2 - (e2 * 0.5) * s * (1.0 - s2) +
                           e3 * (1.0 / 6.0 + (5.0 / 8.0) * s2 * s2 - (3.0 / 4.0) * s2) +
                           e4 * ((7.0 / 8.0) * s2 * s2 * s - (5.0 / 4.0) * s2 * s +
                                 (3.0 / 8.0) * s) +
                           e5 * ((21.0 / 16.0) * s2 * s2 * s2 - (35.0 / 16.0) * s2 * s2 +
                                 (15.0 / 16.0) * s2 - 1.0 / 20.0);
    return sign * (a / c) * bracket;
}

double curvatureLawIldDivergenceDb(double azimuthRad, double distanceM, double headRadiusM) noexcept {
    const double a = (std::isfinite(headRadiusM) && headRadiusM > 0.0)
                         ? std::clamp(headRadiusM, 0.05, 0.15)
                         : kHeadRadiusKemarM;
    const double r = clampDistance(distanceM, a);
    // Misma geometría de rama que la ley de ITD, en forma cerrada exacta
    // (sin serie): ILD por divergencia = 20·log10(P_L/P_R).
    const double pL = pathLengthToEar(azimuthRad, r, -1.0, a);
    const double pR = pathLengthToEar(azimuthRad, r, +1.0, a);
    return 20.0 * std::log10(pL / pR);
}

double distanceFromItdExcess(double itdExcessSeconds, double headRadiusM,
                             double speedOfSound) noexcept {
    // A 90°: ITD = (a/c)(1 + π/2 + ε/2 + O(ε³)). Exceso E sobre Woodworth:
    // E ≈ (a/c)(ε/2)  →  ε ≈ 2·E·c/a  →  r = a/ε.
    // Solo el término líder (error < 3.5 % a 0.15 m, < 1 % a ≥ 0.35 m).
    if (!std::isfinite(itdExcessSeconds) || itdExcessSeconds <= 0.0) return -1.0;
    const double a = (std::isfinite(headRadiusM) && headRadiusM > 0.0)
                         ? std::clamp(headRadiusM, 0.05, 0.15)
                         : kHeadRadiusKemarM;
    const double c = finitePositive(speedOfSound) ? speedOfSound : kSpeedOfSoundMps;
    const double eps = 2.0 * itdExcessSeconds * c / a;
    if (!(eps > 0.0) || eps >= 1.0) return -1.0;
    return a / eps;
}

double curvatureRate(double distanceM, double radialVelocityMps) noexcept {
    if (!finitePositive(distanceM) || !std::isfinite(radialVelocityMps) ||
        radialVelocityMps <= 0.0)
        return 0.0;
    const double kappa = 1.0 / distanceM;
    return kappa * kappa * radialVelocityMps;
}

double timeToContact(double distanceM, double radialVelocityMps) noexcept {
    if (!finitePositive(distanceM) || !std::isfinite(radialVelocityMps) ||
        radialVelocityMps <= 0.0)
        return 1e30;
    return distanceM / radialVelocityMps;
}

}  // namespace ivanna::fused::propagation
