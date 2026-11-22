// test_physical_propagation.cpp — Tests de fused/physical_propagation.hpp.
//
// PARTE I (piso): cada ley se verifica contra un valor CONOCIDO e
// independiente del código bajo test: −6.02 dB por duplicar distancia,
// r/343, tablas publicadas ISO 9613-1, Doppler analítico, Woodworth.
// PARTE II (ley de curvatura): la ley nueva se verifica contra DOS
// referencias independientes: el límite de campo lejano de Woodworth
// (κ → 0) y el modelo exacto de rayos de NearFieldHeadModel (forma
// cerrada min-directo/reptante, sin serie). Ninguna aserción compara
// la ley contra sí misma.
//
// Números de referencia (ver PHYSICAL_PROPAGATION_MAP.md §5):
//   Woodworth 90° = 655.8 µs;  ITD exacto 90°/0.15 m = 732.6 µs (+11.7 %)
//   ISO 9613-1 20 °C/70 % HR: α = 4.98 / 23.09 / 77.63 dB/km @ 1/4/8 kHz
//   ISO 9613-1 10 °C/70 % HR: α = 3.7 / 32.8 / 117 dB/km @ 1/4/8 kHz
#include <gtest/gtest.h>

#include <cmath>

#include "../fused/physical_propagation.hpp"
#include "../spatial/NearFieldHeadModel.hpp"

namespace {
using namespace ivanna::fused::propagation;
constexpr double kA = 0.0875;   // radio KEMAR (m)
constexpr double kC = 343.0;    // velocidad del sonido (m/s)
constexpr double kPiD = 3.14159265358979323846;

// Woodworth de campo lejano (referencia independiente, no usa la ley).
double woodworth(double thetaRad) {
    const double th = std::fabs(thetaRad);
    if (th <= kPiD * 0.5) return (kA / kC) * (th + std::sin(th));
    return (kA / kC) * (kPiD - th + std::sin(th));
}
}  // namespace

// ═══ PARTE I — PISO ═══════════════════════════════════════════════════════

TEST(PhysicalPropagation, SphericalSpreadingDoublesDistanceMinus6dB) {
    EXPECT_DOUBLE_EQ(sphericalSpreadingGain(1.0), 1.0);       // identidad en 1 m
    EXPECT_DOUBLE_EQ(sphericalSpreadingGain(2.0), 0.5);
    EXPECT_NEAR(sphericalSpreadingDb(2.0), -6.0206, 1e-4);    // −6.02 dB exactos
    EXPECT_NEAR(sphericalSpreadingDb(0.5), 6.0206, 1e-4);
    EXPECT_NEAR(sphericalSpreadingDb(4.0), -12.0412, 1e-4);   // −12.04 dB
    EXPECT_NEAR(sphericalSpreadingGain(0.25), 4.0, 1e-12);
    // Fail-closed: nunca ∞ ni NaN.
    EXPECT_DOUBLE_EQ(sphericalSpreadingGain(0.0), 1.0);
    EXPECT_DOUBLE_EQ(sphericalSpreadingGain(-1.0), 1.0);
    EXPECT_DOUBLE_EQ(sphericalSpreadingGain(NAN), 1.0);
    EXPECT_DOUBLE_EQ(sphericalSpreadingGain(2.0, 0.0), 1.0);
}

TEST(PhysicalPropagation, CylindricalSpreadingIsWfsRegime) {
    // WFS 2.5D usa 1/sqrt(d): a 4 m cae 6.02 dB, no 12.04 dB.
    EXPECT_DOUBLE_EQ(cylindricalSpreadingGain(1.0), 1.0);
    EXPECT_NEAR(cylindricalSpreadingGain(4.0), 0.5, 1e-12);
    EXPECT_NEAR(20.0 * std::log10(cylindricalSpreadingGain(2.0)), -3.0103, 1e-4);
}

TEST(PhysicalPropagation, SceneDistanceGainMatchesExistingClamps) {
    // Drop-in de HrirPoseSelect::sceneDistanceGain: mismos valores.
    EXPECT_FLOAT_EQ(static_cast<float>(sceneDistanceGainPhysical(1.0f)), 1.0f);
    EXPECT_NEAR(sceneDistanceGainPhysical(2.0), 0.5, 1e-12);
    EXPECT_NEAR(sceneDistanceGainPhysical(0.5), 1.41254, 1e-5);  // tope +3 dB
    EXPECT_NEAR(sceneDistanceGainPhysical(100.0), 0.5, 1e-12);   // tope −6 dB
    EXPECT_DOUBLE_EQ(sceneDistanceGainPhysical(NAN), 1.0);
    EXPECT_DOUBLE_EQ(sceneDistanceGainPhysical(-3.0), 1.0);
}

TEST(PhysicalPropagation, PropagationDelayIsDistanceOverC) {
    EXPECT_NEAR(propagationDelaySeconds(1.0), 1.0 / 343.0, 1e-12);  // 2.915 ms
    EXPECT_NEAR(propagationDelaySeconds(343.0), 1.0, 1e-12);
    EXPECT_NEAR(propagationDelaySeconds(0.5), 0.5 / 343.0, 1e-12);
    EXPECT_NEAR(propagationDelaySamples(1.0, 48000.0), 48000.0 / 343.0, 1e-9);
    EXPECT_DOUBLE_EQ(propagationDelaySeconds(0.0), 0.0);
    EXPECT_DOUBLE_EQ(propagationDelaySeconds(NAN), 0.0);
}

TEST(PhysicalPropagation, SpeedOfSound) {
    EXPECT_NEAR(speedOfSound(20.0), 343.21, 0.05);
    EXPECT_NEAR(speedOfSound(0.0), 331.3, 0.05);
    EXPECT_DOUBLE_EQ(speedOfSound(NAN), 343.0);
}

TEST(PhysicalPropagation, AirAbsorptionMatchesIso9613Tables20C70RH) {
    // Tablas publicadas ISO 9613-1 (20 °C, 70 % HR, 101.325 kPa):
    // 4.98 / 23.09 / 77.63 dB/km @ 1/4/8 kHz. Tolerancia ±10 % (la
    // exactitud declarada por la propia norma).
    EXPECT_NEAR(airAbsorptionCoeffDbPerM(1000.0, 20.0, 0.70) * 1000.0, 4.98, 0.50);
    EXPECT_NEAR(airAbsorptionCoeffDbPerM(4000.0, 20.0, 0.70) * 1000.0, 23.09, 2.31);
    EXPECT_NEAR(airAbsorptionCoeffDbPerM(8000.0, 20.0, 0.70) * 1000.0, 77.63, 7.76);
    // A distancias ORBE es despreciable; a 200 m domina en agudos
    // (tabla independiente acmap: 1.00 dB @ 1 kHz, 15.53 dB @ 8 kHz).
    EXPECT_NEAR(airAbsorptionDb(8000.0, 2.0, 20.0, 0.70), 0.1553, 0.016);
    EXPECT_NEAR(airAbsorptionDb(1000.0, 200.0, 20.0, 0.70), 1.00, 0.05);
    EXPECT_NEAR(airAbsorptionDb(8000.0, 200.0, 20.0, 0.70), 15.53, 0.78);
    // Monótona en frecuencia.
    EXPECT_LT(airAbsorptionCoeffDbPerM(1000.0), airAbsorptionCoeffDbPerM(4000.0));
    EXPECT_LT(airAbsorptionCoeffDbPerM(4000.0), airAbsorptionCoeffDbPerM(8000.0));
    EXPECT_DOUBLE_EQ(airAbsorptionCoeffDbPerM(0.0), 0.0);
    EXPECT_DOUBLE_EQ(airAbsorptionCoeffDbPerM(NAN), 0.0);
}

TEST(PhysicalPropagation, AirAbsorptionMatchesIso9613Tables10C70RH) {
    // Segunda tabla independiente (10 °C, 70 % HR): 3.7 / 32.8 / 117 dB/km.
    EXPECT_NEAR(airAbsorptionCoeffDbPerM(1000.0, 10.0, 0.70) * 1000.0, 3.7, 0.37);
    EXPECT_NEAR(airAbsorptionCoeffDbPerM(4000.0, 10.0, 0.70) * 1000.0, 32.8, 3.28);
    EXPECT_NEAR(airAbsorptionCoeffDbPerM(8000.0, 10.0, 0.70) * 1000.0, 117.0, 11.7);
}

TEST(PhysicalPropagation, DopplerShift) {
    // f' = f·c/(c−v_r): acercarse a 10 m/s sube 1 kHz a 1030.03 Hz.
    EXPECT_NEAR(dopplerFactor(10.0), 343.0 / 333.0, 1e-12);
    EXPECT_NEAR(dopplerShift(1000.0, 10.0), 1030.03, 0.01);
    EXPECT_NEAR(dopplerShift(1000.0, -10.0), 971.67, 0.01);
    EXPECT_DOUBLE_EQ(dopplerFactor(0.0), 1.0);
    EXPECT_DOUBLE_EQ(dopplerFactor(NAN), 1.0);
    EXPECT_DOUBLE_EQ(dopplerFactor(400.0), 1.0);  // fail-closed: |v| >= 0.9c
}

TEST(PhysicalPropagation, ConstantPowerMixConservesPower) {
    const ConstantPowerMix m0 = constantPowerMix(0.0);
    EXPECT_DOUBLE_EQ(m0.dry, 1.0);
    EXPECT_DOUBLE_EQ(m0.wet, 0.0);
    const ConstantPowerMix m1 = constantPowerMix(1.0);
    EXPECT_NEAR(m1.dry, 0.0, 1e-15);
    EXPECT_DOUBLE_EQ(m1.wet, 1.0);
    const ConstantPowerMix mh = constantPowerMix(0.5);
    EXPECT_NEAR(mh.dry, std::sqrt(0.5), 1e-15);
    EXPECT_NEAR(mh.wet, std::sqrt(0.5), 1e-15);
    for (double t = 0.0; t <= 1.0; t += 0.05) {
        const ConstantPowerMix m = constantPowerMix(t);
        EXPECT_NEAR(m.dry * m.dry + m.wet * m.wet, 1.0, 1e-12);  // dry²+wet²=1
    }
    EXPECT_DOUBLE_EQ(constantPowerMix(5.0).wet, 1.0);  // clamp a 1
}

TEST(PhysicalPropagation, NearFieldParallaxMatchesHeadModel) {
    // Doble precisión vs el modelo float existente: mismo algoritmo,
    // misma geometría — deben coincidir dentro de 2 µs en todo azimut.
    for (double r : {0.15, 0.35, 1.0, 2.0}) {
        for (double deg = -180.0; deg <= 180.0; deg += 15.0) {
            const double th = deg * kPiD / 180.0;
            const NearFieldPaths p = nearFieldParallax(th, r);
            const float ref = ivanna::nearfield::itdSeconds(static_cast<float>(th),
                                                           static_cast<float>(r));
            EXPECT_NEAR(p.itdSeconds, static_cast<double>(ref), 2e-6)
                << "r=" << r << " deg=" << deg;
        }
    }
    // Valores puntuales (modelo exacto de rayos).
    const NearFieldPaths p1 = nearFieldParallax(kPiD / 2.0, 1.0);
    EXPECT_NEAR(p1.itdSeconds * 1e6, 667.0, 3.0);
    EXPECT_NEAR(p1.ildDivergenceDb, 1.94, 0.05);
    const NearFieldPaths p015 = nearFieldParallax(kPiD / 2.0, 0.15);
    EXPECT_NEAR(p015.itdSeconds * 1e6, 732.6, 3.0);
    EXPECT_NEAR(p015.ildDivergenceDb, 14.01, 0.05);
    // Simetría: frontal 0 → ITD 0; izquierda = −derecha.
    EXPECT_DOUBLE_EQ(nearFieldParallax(0.0, 1.0).itdSeconds, 0.0);
    EXPECT_NEAR(nearFieldParallax(-kPiD / 2.0, 1.0).itdSeconds,
                -nearFieldParallax(kPiD / 2.0, 1.0).itdSeconds, 1e-12);
}

// ═══ PARTE II — LEY DE CURVATURA ══════════════════════════════════════════

TEST(CurvatureLaw, FarFieldRecoversWoodworthExactly) {
    // κ → 0: la ley DEBE colapsar a Woodworth. r = 100 m (ε = 0.000875).
    for (double deg : {0.0, 15.0, 30.0, 45.0, 60.0, 75.0, 90.0, 120.0, 180.0}) {
        const double th = deg * kPiD / 180.0;
        const double law = curvatureLawItdSeconds(th, 100.0);
        EXPECT_NEAR(law, woodworth(th), std::fabs(woodworth(th)) * 0.001 + 1e-9)
            << "deg=" << deg;
    }
    EXPECT_NEAR(curvatureLawItdSeconds(kPiD / 2.0, 100.0) * 1e6, 655.82, 0.7);
}

TEST(CurvatureLaw, NearFieldExcessAt90Deg015m) {
    // Predicción central de la ley: a 0.15 m / 90° el ITD supera a
    // Woodworth en +11.7 % (732.5 µs vs 655.8 µs). El encabezado de
    // NearFieldHeadModel documenta "~+12 %" — la ley lo DERIVA.
    const double law = curvatureLawItdSeconds(kPiD / 2.0, 0.15);
    EXPECT_NEAR(law * 1e6, 732.5, 2.0);
    EXPECT_NEAR((law - woodworth(kPiD / 2.0)) / woodworth(kPiD / 2.0), 0.117, 0.005);
    // Y la serie decae suavemente hacia Woodworth al alejar la fuente.
    EXPECT_NEAR(curvatureLawItdSeconds(kPiD / 2.0, 0.35) * 1e6, 687.9, 2.0);
    EXPECT_NEAR(curvatureLawItdSeconds(kPiD / 2.0, 1.0) * 1e6, 667.0, 2.0);
    EXPECT_NEAR(curvatureLawItdSeconds(kPiD / 2.0, 2.0) * 1e6, 661.4, 2.0);
}

TEST(CurvatureLaw, MatchesExactRayModelAllAzimuths) {
    // La ley (serie cerrada) contra el modelo exacto de rayos
    // (NearFieldHeadModel, sin serie): < 1 µs en TODO azimut, r ≥ 0.15 m.
    for (double r : {0.15, 0.35, 1.0}) {
        for (double deg = -180.0; deg <= 180.0; deg += 5.0) {
            const double th = deg * kPiD / 180.0;
            const double law = curvatureLawItdSeconds(th, r);
            const float ref = ivanna::nearfield::itdSeconds(static_cast<float>(th),
                                                           static_cast<float>(r));
            EXPECT_NEAR(law, static_cast<double>(ref), 1e-6)
                << "r=" << r << " deg=" << deg;
        }
    }
}

TEST(CurvatureLaw, BranchCIsExactAndSymmetric) {
    // Rama C (|θ| ≤ asin ε): ITD = (a/c)·2θ EXACTO. θ = 0 → 0 exacto.
    EXPECT_DOUBLE_EQ(curvatureLawItdSeconds(0.0, 0.15), 0.0);
    EXPECT_DOUBLE_EQ(curvatureLawItdSeconds(0.0, 1.0), 0.0);
    EXPECT_NEAR(curvatureLawItdSeconds(30.0 * kPiD / 180.0, 0.15) * 1e6, 267.1, 1.0);
    EXPECT_NEAR(curvatureLawItdSeconds(10.0 * kPiD / 180.0, 0.35) * 1e6, 89.06, 1.0);
    // Antisimetría y espejo frontal/trasero.
    EXPECT_NEAR(curvatureLawItdSeconds(-kPiD / 2.0, 0.15),
                -curvatureLawItdSeconds(kPiD / 2.0, 0.15), 1e-12);
    EXPECT_NEAR(curvatureLawItdSeconds(120.0 * kPiD / 180.0, 0.35),
                curvatureLawItdSeconds(60.0 * kPiD / 180.0, 0.35), 1e-12);
    EXPECT_NEAR(curvatureLawItdSeconds(kPiD, 0.35), 0.0, 1e-9);
}

TEST(CurvatureLaw, BranchesJoinContinuouslyAtThetaStar) {
    // θ* = asin(a/r): la rama C y la D deben coincidir ahí (teorema de
    // continuidad verificado con sympy hasta O(ε⁵)) y no haber salto.
    for (double r : {0.15, 0.35, 1.0}) {
        const double thStar = std::asin(kA / r);
        const double atStar = curvatureLawItdSeconds(thStar, r);
        EXPECT_NEAR(atStar, (kA / kC) * 2.0 * thStar, 1e-6) << "r=" << r;
        const double d = 1e-3;
        EXPECT_NEAR(curvatureLawItdSeconds(thStar + d, r),
                    curvatureLawItdSeconds(thStar - d, r), 2e-6)
            << "r=" << r;
    }
}

TEST(CurvatureLaw, IldDivergenceFromSameGeometry) {
    // ILD por divergencia (misma geometría de curvatura): 90°.
    EXPECT_NEAR(curvatureLawIldDivergenceDb(kPiD / 2.0, 0.15), 14.01, 0.1);
    EXPECT_NEAR(curvatureLawIldDivergenceDb(kPiD / 2.0, 0.35), 5.57, 0.1);
    EXPECT_NEAR(curvatureLawIldDivergenceDb(kPiD / 2.0, 1.0), 1.94, 0.05);
    EXPECT_NEAR(curvatureLawIldDivergenceDb(kPiD / 2.0, 100.0), 0.02, 0.02);
    EXPECT_DOUBLE_EQ(curvatureLawIldDivergenceDb(0.0, 1.0), 0.0);
}

TEST(CurvatureLaw, DistanceFromItdExcessInverts) {
    // El exceso de ITD sobre Woodworth estima la distancia SIN nivel.
    // Exceso medido con el modelo exacto (no con la ley: sin circularidad).
    for (double r : {0.35, 0.5, 1.0, 2.0}) {
        const double itd = static_cast<double>(
            ivanna::nearfield::itdSeconds(static_cast<float>(kPiD / 2.0),
                                          static_cast<float>(r)));
        const double est = distanceFromItdExcess(itd - woodworth(kPiD / 2.0));
        EXPECT_NEAR(est, r, r * 0.015) << "r=" << r;
    }
    // A 0.15 m el término líder erra 3.5 % (O(ε³) despreciado): 0.145 m.
    const double itd015 = static_cast<double>(
        ivanna::nearfield::itdSeconds(static_cast<float>(kPiD / 2.0), 0.15f));
    EXPECT_NEAR(distanceFromItdExcess(itd015 - woodworth(kPiD / 2.0)), 0.145, 0.005);
    // Sin exceso (o negativo) no hay estimación.
    EXPECT_DOUBLE_EQ(distanceFromItdExcess(0.0), -1.0);
    EXPECT_DOUBLE_EQ(distanceFromItdExcess(-1e-4), -1.0);
}

TEST(CurvatureLaw, LoomingCorollary) {
    EXPECT_DOUBLE_EQ(wavefrontCurvature(0.5), 2.0);
    EXPECT_DOUBLE_EQ(wavefrontCurvature(1.0), 1.0);
    EXPECT_DOUBLE_EQ(wavefrontCurvature(0.0), 1.0);  // fail-closed
    // κ̇ = κ²·v_r ; τ = 1/(κ·v_r): r = 0.5 m, v_r = 1 m/s → 4.0 /m/s, 0.50 s.
    EXPECT_DOUBLE_EQ(curvatureRate(0.5, 1.0), 4.0);
    EXPECT_DOUBLE_EQ(timeToContact(0.5, 1.0), 0.5);
    EXPECT_DOUBLE_EQ(curvatureRate(0.5, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(timeToContact(0.5, 0.0), 1e30);
    EXPECT_DOUBLE_EQ(timeToContact(0.5, -1.0), 1e30);
}
