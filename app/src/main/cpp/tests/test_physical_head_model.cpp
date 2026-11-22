// test_physical_head_model.cpp — esfera rígida: física exacta, no heurística.
//
// Verifica la solución de dispersión de physical_head_model.hpp contra:
//   (a) límites analíticos (ka->0, presión doble frontal, bright spot);
//   (b) una implementación INDEPENDIENTE en scipy (spherical_jn/spherical_yn,
//       /tmp/sphere_ref.py): 20 valores complejos con tolerancia 1e-9;
//   (c) propiedades físicas (simetría, ILD/ITD en rango conocido).
// El test NO compara contra la HRTF medida: esa comparación vive en el
// informe analysis-ivanna/PHYSICAL_HEAD_MODEL_2026-11-18.md (la esfera
// NO reproduce los notches de pinna; veredicto honesto allí).
#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <vector>

#include "fused/physical_head_model.hpp"

namespace {
using ivanna::physical::EarTransfer;
using ivanna::physical::RigidSphereWorkspace;
using ivanna::physical::rigidSphereEarTransfer;
using ivanna::physical::rigidSphereIldDb;
using ivanna::physical::rigidSphereItdMicroseconds;
using ivanna::physical::rigidSphereOrder;
using ivanna::physical::rigidSphereTransfer;
using ivanna::physical::synthesizeRigidSphereHrir;
using ivanna::physical::kHeadRadiusM;

constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }
double magDb(std::complex<double> z) { return 20.0 * std::log10(std::abs(z)); }

// Tabla de referencia: scipy (spherical_jn/yn + lpmv), doble precisión.
// {f_Hz, az_deg, Re(HL), Im(HL), Re(HR), Im(HR)} — ver /tmp/sphere_ref.py.
struct RefRow { double f, az, reL, imL, reR, imR; };
const RefRow kRef[] = {
  {250, 0, 9.724580632760e-01, -1.817867177981e-02, 9.724580632760e-01, -1.817867177981e-02},
  {250, 45, 9.100928055389e-01, -4.431329943799e-01, 9.014676310730e-01, 4.067559759752e-01},
  {250, 90, 8.461608025726e-01, -6.098824445233e-01, 8.339629782396e-01, 5.734857512777e-01},
  {250, 180, 9.724580632760e-01, -1.817867177981e-02, 9.724580632760e-01, -1.817867177981e-02},
  {1000, 0, 1.094673360429e+00, -2.381194402045e-01, 1.094673360429e+00, -2.381194402045e-01},
  {1000, 45, 2.413029766284e-01, -1.424708690817e+00, -5.906483703576e-02, 7.795205261474e-01},
  {1000, 90, -4.853314652727e-01, -1.459339794420e+00, -9.004985826879e-01, 6.453901735510e-01},
  {1000, 180, 1.094673360429e+00, -2.381194402045e-01, 1.094673360429e+00, -2.381194402045e-01},
  {4000, 0, 1.278082562991e+00, -1.167693716594e-01, 1.278082562991e+00, -1.167693716594e-01},
  {4000, 45, -2.235903316221e-02, 1.807990627116e+00, 4.015536732574e-01, -4.200411728748e-01},
  {4000, 90, 1.861195925883e+00, -4.988522928908e-01, -6.363165284121e-01, -9.245756744406e-01},
  {4000, 180, 1.278082562991e+00, -1.167693716594e-01, 1.278082562991e+00, -1.167693716594e-01},
  {8000, 0, 1.322804549728e+00, -9.614089021042e-02, 1.322804549728e+00, -9.614089021042e-02},
  {8000, 45, -1.844081226631e+00, -4.652702199007e-01, -1.251009990374e-01, -5.193196623114e-01},
  {8000, 90, 1.869696325003e+00, -6.420703896066e-01, -3.251153297675e-01, 9.265005188464e-01},
  {8000, 180, 1.322804549728e+00, -9.614089021042e-02, 1.322804549728e+00, -9.614089021042e-02},
  {16000, 0, 1.357258070405e+00, -6.108089933690e-02, 1.357258070405e+00, -6.108089933690e-02},
  {16000, 45, 1.560564151736e+00, 1.182073694026e+00, -2.091808803879e-01, 3.105052600411e-01},
  {16000, 90, 1.697300571777e+00, -1.044473379546e+00, -6.713192427719e-01, -3.220660838693e-01},
  {16000, 180, 1.357258070405e+00, -6.108089933690e-02, 1.357258070405e+00, -6.108089933690e-02},
};
}  // namespace

TEST(PhysicalHeadModel, MatchesIndependentScipyReference) {
    for (const RefRow& r : kRef) {
        const EarTransfer e = rigidSphereEarTransfer(r.f, deg2rad(r.az));
        EXPECT_NEAR(e.left.real(), r.reL, 1e-9) << "f=" << r.f << " az=" << r.az << " ReL";
        EXPECT_NEAR(e.left.imag(), r.imL, 1e-9) << "f=" << r.f << " az=" << r.az << " ImL";
        EXPECT_NEAR(e.right.real(), r.reR, 1e-9) << "f=" << r.f << " az=" << r.az << " ReR";
        EXPECT_NEAR(e.right.imag(), r.imR, 1e-9) << "f=" << r.f << " az=" << r.az << " ImR";
    }
}

TEST(PhysicalHeadModel, LowFrequencyLimitIsUnity) {
    for (double az : {0.0, 30.0, 90.0, 180.0, 270.0}) {
        const EarTransfer e = rigidSphereEarTransfer(50.0, deg2rad(az));
        EXPECT_NEAR(std::abs(e.left), 1.0, 0.02) << "az=" << az;
        EXPECT_NEAR(std::abs(e.right), 1.0, 0.02) << "az=" << az;
        EXPECT_NEAR(std::arg(e.left), 0.0, 0.15) << "az=" << az;
    }
    // f = 0 exacto: identidad.
    EXPECT_EQ(rigidSphereTransfer(0.0, 0.5), std::complex<double>(1.0, 0.0));
    EXPECT_EQ(rigidSphereTransfer(-100.0, 0.5), std::complex<double>(1.0, 0.0));
}

TEST(PhysicalHeadModel, Symmetries) {
    for (double f : {250.0, 1000.0, 4000.0, 12000.0}) {
        // Frontal y trasero: ambos oídos iguales.
        EXPECT_NEAR(std::abs(rigidSphereEarTransfer(f, 0.0).left -
                             rigidSphereEarTransfer(f, 0.0).right), 0.0, 1e-12);
        EXPECT_NEAR(std::abs(rigidSphereEarTransfer(f, deg2rad(180.0)).left -
                             rigidSphereEarTransfer(f, deg2rad(180.0)).right), 0.0, 1e-12);
        // Espejo: H_L(θ) = H_R(−θ); periodicidad 360°.
        const EarTransfer a = rigidSphereEarTransfer(f, deg2rad(60.0));
        const EarTransfer b = rigidSphereEarTransfer(f, deg2rad(-60.0));
        EXPECT_NEAR(std::abs(a.left - b.right), 0.0, 1e-12);
        EXPECT_NEAR(std::abs(a.right - b.left), 0.0, 1e-12);
        const EarTransfer c = rigidSphereEarTransfer(f, deg2rad(60.0 + 360.0));
        EXPECT_NEAR(std::abs(a.left - c.left), 0.0, 1e-12);
    }
}

TEST(PhysicalHeadModel, FrontalPressureDoublingAndAntipodeBrightSpot) {
    // Punto frontal (γ=π, de cara a la fuente): |H| -> 2 a ka grande.
    const double frontal = std::abs(rigidSphereTransfer(16000.0, -1.0));
    EXPECT_GT(frontal, 1.90);
    EXPECT_LT(frontal, 2.05);
    // Antípoda (γ=0): bright spot, |H| ~ 0.74 — NO hay sombra profunda justo
    // detrás. Si un día esto da ~0.05, la serie está mal (o la convención).
    const double antipode = std::abs(rigidSphereTransfer(16000.0, 1.0));
    EXPECT_GT(antipode, 0.60);
    EXPECT_LT(antipode, 0.90);
    // El bright spot es ESTRECHO (γ ≲ 10°): justo fuera de él, en
    // γ ≈ 10–40°, la sombra sí es profunda (−8…−16 dB a 8 kHz). A 90° de
    // azimut el oído contralateral cae EXACTAMENTE en el bright spot, y por
    // eso la esfera subestima el ILD medido (ver informe). Si un día el
    // mínimo en γ=20° da ~0 dB, la serie está mal (o la convención).
    const double shadow = magDb(rigidSphereTransfer(8000.0, std::cos(deg2rad(20.0))));
    EXPECT_LT(shadow, -6.0);
    EXPECT_GT(shadow, -20.0);
}

TEST(PhysicalHeadModel, IldGrowsWithFrequencyAndAzimuth) {
    // A 90°, el ILD crece con f (sombra cefálica real, no clamp heurístico).
    const double ild500 = rigidSphereIldDb(500.0, deg2rad(90.0));
    const double ild4000 = rigidSphereIldDb(4000.0, deg2rad(90.0));
    const double ild8000 = rigidSphereIldDb(8000.0, deg2rad(90.0));
    EXPECT_GT(ild500, 0.5);
    EXPECT_LT(ild500, 4.0);
    EXPECT_GT(ild4000, ild500);
    EXPECT_GT(ild8000, ild4000);
    EXPECT_LT(ild8000, 12.0);  // la esfera NO llega a los ~16-23 dB medidos
    EXPECT_NEAR(rigidSphereIldDb(1000.0, 0.0), 0.0, 1e-9);
    EXPECT_GT(rigidSphereIldDb(4000.0, deg2rad(90.0)), 0.0);
    EXPECT_LT(rigidSphereIldDb(4000.0, deg2rad(-90.0)), 0.0);  // signo espejo
}

TEST(PhysicalHeadModel, ItdAt90DegreesMatchesWoodworthRange) {
    // ITD máximo (grupo 150-500 Hz) ~ a/c·(π/2+1) ≈ 670 µs para a=0.0875.
    const double itd90 = rigidSphereItdMicroseconds(deg2rad(90.0), 150.0, 500.0);
    EXPECT_GT(itd90, 550.0);
    EXPECT_LT(itd90, 800.0);
    EXPECT_NEAR(rigidSphereItdMicroseconds(0.0, 150.0, 500.0), 0.0, 5.0);
    EXPECT_NEAR(rigidSphereItdMicroseconds(deg2rad(180.0), 150.0, 500.0), 0.0, 5.0);
    const double itd30 = rigidSphereItdMicroseconds(deg2rad(30.0), 150.0, 500.0);
    EXPECT_GT(itd30, 150.0);
    EXPECT_LT(itd30, itd90);
}

TEST(PhysicalHeadModel, SeriesConvergesWithOrder) {
    // N+20 órdenes extra no deben mover |H| más de 1e-9.
    RigidSphereWorkspace ws;
    const double ka = 2.0 * kPi * 16000.0 * kHeadRadiusM / 343.0;
    EXPECT_GT(rigidSphereOrder(ka), 40);
    EXPECT_LT(rigidSphereOrder(ka), 60);
    const std::complex<double> h1 = rigidSphereTransfer(16000.0, 0.3, kHeadRadiusM, 343.0, ws);
    // Re-llamar con el mismo workspace (reutilización sin malloc) da lo mismo.
    const std::complex<double> h2 = rigidSphereTransfer(16000.0, 0.3, kHeadRadiusM, 343.0, ws);
    EXPECT_NEAR(std::abs(h1 - h2), 0.0, 1e-15);
    EXPECT_TRUE(std::isfinite(h1.real()) && std::isfinite(h1.imag()));
}

TEST(PhysicalHeadModel, FiniteEverywhereUpTo16kHz) {
    RigidSphereWorkspace ws;
    for (double f = 20.0; f <= 16000.0; f *= 1.5) {
        for (double az = 0.0; az < 360.0; az += 15.0) {
            const EarTransfer e = rigidSphereEarTransfer(f, deg2rad(az), kHeadRadiusM, 343.0, ws);
            ASSERT_TRUE(std::isfinite(e.left.real()) && std::isfinite(e.left.imag()))
                << "f=" << f << " az=" << az;
            ASSERT_TRUE(std::isfinite(e.right.real()) && std::isfinite(e.right.imag()))
                << "f=" << f << " az=" << az;
            EXPECT_GT(std::abs(e.left), 0.05);
            EXPECT_LT(std::abs(e.left), 2.5);
        }
    }
    // Argumentos inválidos: fail-closed, sin NaN.
    EXPECT_EQ(rigidSphereTransfer(1000.0, 0.0, 0.0), std::complex<double>(0.0, 0.0));
    EXPECT_EQ(rigidSphereTransfer(1000.0, 0.0, -1.0), std::complex<double>(0.0, 0.0));
    EXPECT_EQ(rigidSphereTransfer(1000.0, 0.0, kHeadRadiusM, 0.0),
              std::complex<double>(0.0, 0.0));
}

TEST(PhysicalHeadModel, HrirSynthesisIsFiniteAndItdConsistent) {
    constexpr int kTaps = 512;
    std::vector<double> hL(kTaps), hR(kTaps);
    ASSERT_TRUE(synthesizeRigidSphereHrir(deg2rad(90.0), 44100.0, kTaps, 0.002, hL.data(), hR.data()));
    double eL = 0.0, eR = 0.0;
    int peakL = 0, peakR = 0;
    for (int i = 0; i < kTaps; ++i) {
        ASSERT_TRUE(std::isfinite(hL[i]) && std::isfinite(hR[i])) << "tap " << i;
        eL += hL[i] * hL[i];
        eR += hR[i] * hR[i];
        if (std::abs(hL[i]) > std::abs(hL[peakL])) peakL = i;
        if (std::abs(hR[i]) > std::abs(hR[peakR])) peakR = i;
    }
    EXPECT_GT(eL, 1e-6);
    EXPECT_GT(eR, 1e-6);
    // El pico ipsilateral llega antes que el contralateral. El retardo de
    // pico de banda ancha (~658 µs, dominado por alta frecuencia) es MENOR
    // que el ITD de baja frecuencia (~777 µs): a baja frecuencia la
    // difracción alarga el ITD por encima del valor geométrico de Woodworth
    // (~670 µs). Ambos valores son física, no un error.
    const double peakDelayUs = static_cast<double>(peakR - peakL) / 44100.0 * 1e6;
    EXPECT_GT(peakDelayUs, 400.0);
    EXPECT_LT(peakDelayUs, 900.0);
    // Argumentos inválidos -> false sin tocar buffers.
    EXPECT_FALSE(synthesizeRigidSphereHrir(0.0, 44100.0, kTaps, 0.002, nullptr, hR.data()));
    EXPECT_FALSE(synthesizeRigidSphereHrir(0.0, 0.0, kTaps, 0.002, hL.data(), hR.data()));
    EXPECT_FALSE(synthesizeRigidSphereHrir(0.0, 44100.0, 0, 0.002, hL.data(), hR.data()));
}
