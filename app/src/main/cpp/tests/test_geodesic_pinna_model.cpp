// test_geodesic_pinna_model.cpp — Operador de Interferencia Geodésica.
//
// Verifica la Ley Geodésica del Notch contra la HRTF medida MIT KEMAR:
// la frecuencia del notch de cada familia, en cada elevación, debe caer
// dentro de la tolerancia LOO medida (baja ±400 Hz, alta ±1000 Hz) —
// predicción, no ajuste: las constantes se fijaron una vez con todas las
// elevaciones, pero el error leave-one-out es del mismo orden.
// Y verifica de extremo a extremo que el operador unificado (esfera x
// pinna) REPRODUCE el notch frontal de 8 kHz que la esfera sola NO tiene.
#include <gtest/gtest.h>

#include <cmath>
#include <complex>

#include "fused/geodesic_pinna_model.hpp"
#include "fused/physical_head_model.hpp"

namespace {
using ivanna::physical::headPlusPinnaEarTransfer;
using ivanna::physical::kPinnaHighFamily;
using ivanna::physical::kPinnaLowFamily;
using ivanna::physical::pinnaFactor;
using ivanna::physical::pinnaNotchFrequencyHz;
using ivanna::physical::pinnaPathDifferenceMm;
using ivanna::physical::rigidSphereEarTransfer;

constexpr double kPi = 3.14159265358979323846;
double deg2rad(double d) { return d * kPi / 180.0; }
double magDb(std::complex<double> z) { return 20.0 * std::log10(std::abs(z)); }
}  // namespace

// Familia BAJA medida (SOFA MIT KEMAR, az=0, oído L): {elev_deg, f_notch_Hz}
TEST(GeodesicPinnaModel, LowFamilyNotchMigratesWithElevationAsMeasured) {
    const double kElev[] = {-40, -30, -20, -10, 0, 10, 20, 30, 40, 50, 60, 70};
    const double kFreq[] = {6368, 6767, 6944, 7477, 7978, 8640, 9426, 9959, 10481, 10890, 11073, 11160};
    for (int i = 0; i < 12; ++i) {
        const double pred = pinnaNotchFrequencyHz(kPinnaLowFamily, deg2rad(kElev[i]));
        EXPECT_NEAR(pred, kFreq[i], 400.0) << "elev=" << kElev[i];
    }
    // El desvío es del orden del pabellón (15-27 mm), no un número mágico.
    EXPECT_GT(pinnaPathDifferenceMm(kPinnaLowFamily, 0.0), 15.0);
    EXPECT_LT(pinnaPathDifferenceMm(kPinnaLowFamily, 0.0), 30.0);
    // Monótono creciente con la elevación (el desvío se acorta).
    EXPECT_GT(pinnaNotchFrequencyHz(kPinnaLowFamily, deg2rad(-40.0)),
              5000.0);
    EXPECT_LT(pinnaNotchFrequencyHz(kPinnaLowFamily, deg2rad(-40.0)),
              pinnaNotchFrequencyHz(kPinnaLowFamily, deg2rad(70.0)));
}

// Familia ALTA medida: {elev_deg, f_notch_Hz}
TEST(GeodesicPinnaModel, HighFamilyNotchMigratesWithElevationAsMeasured) {
    const double kElev[] = {-40, -30, -20, 20, 30, 40, 50, 60, 70, 90};
    const double kFreq[] = {10115, 10971, 11192, 13388, 13367, 13291, 13302, 13431, 13528, 13544};
    for (int i = 0; i < 10; ++i) {
        const double pred = pinnaNotchFrequencyHz(kPinnaHighFamily, deg2rad(kElev[i]));
        EXPECT_NEAR(pred, kFreq[i], 1000.0) << "elev=" << kElev[i];
    }
}

// El factor del pabellón es 1 a baja frecuencia (no toca ITD/ILD) y está
// acotado; a f -> 0 es la identidad exacta.
TEST(GeodesicPinnaModel, PinnaFactorIsIdentityAtLowFrequencyAndBounded) {
    EXPECT_EQ(pinnaFactor(0.0, 0.0), std::complex<double>(1.0, 0.0));
    EXPECT_EQ(pinnaFactor(-100.0, 0.0), std::complex<double>(1.0, 0.0));
    const std::complex<double> f100 = pinnaFactor(100.0, 0.0);
    EXPECT_NEAR(std::abs(f100 - std::complex<double>(1.0, 0.0)), 0.0, 0.05);
    for (double f = 100.0; f <= 16000.0; f *= 1.3) {
        for (double el = -60.0; el <= 90.0; el += 30.0) {
            const std::complex<double> v = pinnaFactor(f, deg2rad(el));
            ASSERT_TRUE(std::isfinite(v.real()) && std::isfinite(v.imag()));
            EXPECT_LE(std::abs(v), 1.0 + 1e-12);
        }
    }
    // En el notch de la familia baja (elev 0) el factor cae (destructivo).
    const double fNotch = pinnaNotchFrequencyHz(kPinnaLowFamily, 0.0);
    EXPECT_LT(std::abs(pinnaFactor(fNotch, 0.0)), 0.6);
}

// Extremo a extremo: el operador unificado reproduce el notch frontal de
// ~8 kHz que la esfera sola NO tiene. La esfera es el piso; el notch es
// física nueva del operador geodésico.
TEST(GeodesicPinnaModel, UnifiedOperatorReproducesFrontal8kHzNotch) {
    std::complex<double> uL, uR;
    ASSERT_TRUE(headPlusPinnaEarTransfer(7978.0, 0.0, 0.0, uL, uR));
    // Mínimo local del módulo unificado en [7.5, 8.5] kHz y profundidad
    // contra el pico local fuera del notch.
    double notchDb = 1e9, peakDb = -1e9;
    for (double f = 6000.0; f <= 9500.0; f += 25.0) {
        std::complex<double> l, r;
        ASSERT_TRUE(headPlusPinnaEarTransfer(f, 0.0, 0.0, l, r));
        const double db = magDb(l);
        if (f >= 7500.0 && f <= 8500.0) notchDb = std::min(notchDb, db);
        if ((f < 7500.0 || f > 8500.0)) peakDb = std::max(peakDb, db);
    }
    EXPECT_GT(peakDb - notchDb, 8.0) << "el operador unificado debe abrir un notch >= 8 dB";
    // La esfera sola NO abre ese notch (profundidad < 3 dB en la misma banda).
    double sNotchDb = 1e9, sPeakDb = -1e9;
    for (double f = 6000.0; f <= 9500.0; f += 25.0) {
        const auto e = rigidSphereEarTransfer(f, 0.0);
        const double db = magDb(e.left);
        if (f >= 7500.0 && f <= 8500.0) sNotchDb = std::min(sNotchDb, db);
        if ((f < 7500.0 || f > 8500.0)) sPeakDb = std::max(sPeakDb, db);
    }
    EXPECT_LT(sPeakDb - sNotchDb, 3.0) << "la esfera sola es suave: sin notch";
    // Argumentos inválidos: fail-closed.
    EXPECT_FALSE(headPlusPinnaEarTransfer(0.0, 0.0, 0.0, uL, uR));
    EXPECT_FALSE(headPlusPinnaEarTransfer(-5.0, 0.0, 0.0, uL, uR));
    EXPECT_FALSE(headPlusPinnaEarTransfer(1000.0, 0.0, 0.0, uL, uR, 0.0));
}

// La corrección de azimut es de primer orden y aproximada: a az=0 es la
// ley exacta; a az=150° el notch bajo predicho cae dentro de ±600 Hz del
// medido (~10.0 kHz, elev 0). No se exige más: el azimut es trabajo abierto.
TEST(GeodesicPinnaModel, AzimuthCorrectionIsFirstOrderApproximate) {
    const double f0 = pinnaNotchFrequencyHz(kPinnaLowFamily, 0.0);
    EXPECT_NEAR(f0, 7978.0, 400.0);
    const double dl150 = pinnaPathDifferenceMm(kPinnaLowFamily, 0.0, deg2rad(150.0));
    const double f150 = 343.0 / (2.0 * dl150 * 1.0e-3);
    EXPECT_NEAR(f150, 10002.0, 600.0);
    // El desvío se acorta con el azimut pero sigue siendo físico (> 0).
    EXPECT_GT(dl150, 5.0);
    EXPECT_LT(dl150, pinnaPathDifferenceMm(kPinnaLowFamily, 0.0));
}
