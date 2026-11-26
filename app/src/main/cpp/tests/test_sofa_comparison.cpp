// test_sofa_comparison.cpp — Arnes de comparacion SOFA (Trabajador C).
//
// Compara en las MISMAS metricas (sofa_comparison_metrics.hpp):
//   (i)   HRTF MEDIDA: kemar.ihr1 = conversion offline de
//         MIT_KEMAR_normal_pinna.sofa (710 pos, 512 taps, 48 kHz).
//         Validado en Python contra el SOFA: |dITD| max 2.5 us,
//         |dILD| max 0.01 dB, |dILD banda| max 0.05 dB.
//   (ii)  ORBE·Ω ACTUAL: CUES DE SALIDA de FusedSpatial ante ruido blanco
//         diotico estacionario (ver orbeOutputCues) — HEADPHONES, amount=1,
//         distancia 1.4 m = r del SOFA, hrtfWet=1, upmix off, WFS off.
//         ORBE es adaptativo (estima azimut/distancia desde la propia
//         senal), asi que NO tiene funcion de transferencia: ni el impulso
//         (latencia de bloque ~590 muestras) ni Welch H(f)=Sxy/Sxx valen
//         (medido: ITD Welch -1121 us a az=0). Solo valen los cues de
//         salida. En host NO hay dataset HRTF cargado:
//         hasHrtfDataset()==false -> HRTF analitica, no KEMAR medido.
//   (iii) SofaHRTFLoader (v2.5.472 Fase 2 ítem D): lee Data.IR + SourcePosition
//         + Data.SamplingRate DE VERDAD (lector HDF5 mínimo, Status::
//         PARSED_MEASURED) y hrtfAt() selecciona por azimut/elevación. El
//         par reconstruido SOFA-PCA (128 taps) solo queda como fallback
//         cuando el archivo no se puede parsear (VALID_NOT_PARSED /
//         UNSUPPORTED): nunca se presenta como medido.
//   (iv)  TEORIA NUEVA (A/B): medida con el MISMO arnes — RigidSphere-
//         VsMeasured (esfera rigida vs MIT KEMAR), CurvatureLawVsMeasured
//         (ley de curvatura vs MIT r=1.4 m y TU-Berlin r=0.5 m; la prueba
//         que cierra el mapa de B) y PinnaNotchVsMeasured (Ley Geodesica
//         del Notch vs extraccion PROPIA de C del SOFA + operador unificado
//         esfera x pinna vs medido; LOO refitado por C en Python).
//         Tabla final de 3 columnas en ComparisonTableForNewPhysics.
//         Criterio "fisica nueva verificada" en SOFA_COMPARISON_METRICS.md
//         y en sofa_comparison_metrics.hpp.
//
// Las aserciones son GUARDAS de cordura (finitud, rangos fisicos amplios),
// no umbrales de "fisica correcta": el valor de este test es la TABLA
// impresa (stdout), que es la evidencia numerica del reporte.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include "HRTFBinLoader.hpp"
#include "HrtfManager.hpp"
#include "SofaHRTFLoader.hpp"
#include "fused/fused_spatial.hpp"
#include "fused/geodesic_pinna_model.hpp"
#include "fused/physical_head_model.hpp"
#include "fused/physical_propagation.hpp"
#include "sofa_comparison_metrics.hpp"

#ifndef IVANNA_HRTF_ASSET_DIR
#error "IVANNA_HRTF_ASSET_DIR debe definirse desde tests/CMakeLists.txt"
#endif
#ifndef IVANNA_SOFA_ASSET_DIR
#error "IVANNA_SOFA_ASSET_DIR debe definirse desde tests/CMakeLists.txt"
#endif

namespace {

using namespace ivanna::sofa_compare;
using ivanna::fused::FusedSpatial;
using ivanna::fused::SpatialControls;
using ivanna::OmegaOutputMode;

constexpr double kSr = 48000.0;
constexpr int kIrLen = 4096;       // ventana de metricas sobre la IR efectiva
constexpr int kCaptureFrames = 48000;  // 1 s de captura tras el impulso

std::string ihr1Path() {
    return (std::filesystem::path(IVANNA_HRTF_ASSET_DIR) / "kemar.ihr1").string();
}
std::string sofaPath() {
    return (std::filesystem::path(IVANNA_SOFA_ASSET_DIR) / "MIT_KEMAR_normal_pinna.sofa").string();
}

// Entrada horizontal (el=0) del .ihr1 con ese azimut SOFA (0..355).
const Ivanna::HRTFEntry* findAzimuth(const Ivanna::HRTFBinLoader& db, double azSofa) {
    for (std::size_t i = 0; i < db.size(); ++i) {
        const auto& e = db.entry(i);
        if (std::abs(e.elevationDeg) < 0.01 && std::abs(e.azimuthDeg - azSofa) < 0.01) return &e;
    }
    return nullptr;
}

// ITD (us) por xcorr de las HRIRs filtradas a [fLo, fHi] Hz (FFT 4096,
// bins fuera de banda a cero, IFFT, xcorr parabolica +-1 ms).
// Mismo signo que itdXcorrUs: positivo = izquierda lidera.
double lowBandItdUs(const float* l, const float* r, std::size_t n, double sr,
                    double fLo, double fHi) {
    constexpr std::size_t kFft = 4096;
    std::vector<std::complex<double>> L(kFft, {0.0, 0.0}), R(kFft, {0.0, 0.0});
    for (std::size_t i = 0; i < n && i < kFft; ++i) {
        L[i] = {static_cast<double>(l[i]), 0.0};
        R[i] = {static_cast<double>(r[i]), 0.0};
    }
    fft4096(L);
    fft4096(R);
    auto bandpass = [&](std::vector<std::complex<double>>& X) {
        for (std::size_t k = 0; k < kFft; ++k) {
            const double f =
                static_cast<double>(k <= kFft / 2 ? k : kFft - k) * sr / static_cast<double>(kFft);
            if (f < fLo || f > fHi) X[k] = {0.0, 0.0};
        }
        for (auto& v : X) v = std::conj(v);
        fft4096(X);
        for (auto& v : X) v = std::conj(v) / static_cast<double>(kFft);
    };
    bandpass(L);
    bandpass(R);
    std::vector<float> fl(kFft), fr(kFft);
    for (std::size_t i = 0; i < kFft; ++i) {
        fl[i] = static_cast<float>(L[i].real());
        fr[i] = static_cast<float>(R[i].real());
    }
    return itdXcorrUs(fl.data(), fr.data(), fl.size(), sr);
}


// ── TRABAJADOR A: esfera rigida (fused/physical_head_model.hpp) ──────────
// La esfera es fisica CLASICA (Rayleigh 1907, serie Bessel/Hankel): exacta
// como solucion de la ecuacion de onda, pero SIN pinna. Se mide con el
// MISMO arnes: sus HRIRs sintetizadas vs la HRTF medida MIT KEMAR.
TEST(SofaComparison, RigidSphereVsMeasured) {
    Ivanna::HRTFBinLoader db;
    ASSERT_TRUE(db.load(ihr1Path().c_str()));
    const double azimuths[] = {0.0, 30.0, 60.0, 90.0, 120.0, 180.0};
    std::printf("[sofa_compare] ESFERA RIGIDA (A) vs MEDIDO (MIT KEMAR, 48 kHz, 512 taps)\n");
    std::printf("[sofa_compare] %5s | %7s %7s %6s | %6s %6s %6s | %5s %5s | %5s %5s | %5s %5s | %7s %7s\n",
                "az", "ITDmed", "ITDesf", "err", "ILDmed", "ILD esf", "err", "SDL", "SDLsh",
                "SDR", "SDRsh", "xcL", "xcR", "ITDlbMed", "ITDlbEsf");
    double sumItdErr = 0.0, sumIldErr = 0.0, sumSd = 0.0;
    int n = 0;
    for (double az : azimuths) {
        const Ivanna::HRTFEntry* e = findAzimuth(db, az);
        ASSERT_NE(e, nullptr);
        const HrirMetrics meas =
            computeMetrics(e->left.data(), e->right.data(), e->left.size(), kSr);
        ASSERT_TRUE(meas.valid);
        std::vector<double> hL(512), hR(512);
        ASSERT_TRUE(ivanna::physical::synthesizeRigidSphereHrir(
            az * M_PI / 180.0, kSr, 512, 0.002, hL.data(), hR.data()))
            << "az=" << az;
        std::vector<float> fL(hL.begin(), hL.end()), fR(hR.begin(), hR.end());
        const HrirMetrics sph = computeMetrics(fL.data(), fR.data(), fL.size(), kSr);
        ASSERT_TRUE(sph.valid) << "az=" << az;
        const double itdErr = sph.itdOnsetUs - meas.itdOnsetUs;
        const double ildErr = sph.ildBroadbandDb - meas.ildBroadbandDb;
        const double sdL = spectralDistortionDb(sph.magL, meas.magL, false);
        const double sdLsh = spectralDistortionDb(sph.magL, meas.magL, true);
        const double sdR = spectralDistortionDb(sph.magR, meas.magR, false);
        const double sdRsh = spectralDistortionDb(sph.magR, meas.magR, true);
        const double xcL = maxNormXcorr(fL.data(), e->left.data(), 512, kSr);
        const double xcR = maxNormXcorr(fR.data(), e->right.data(), 512, kSr);
        const double itdLbMed = lowBandItdUs(e->left.data(), e->right.data(), 512, kSr, 150, 500);
        const double itdLbEsf = lowBandItdUs(fL.data(), fR.data(), 512, kSr, 150, 500);
        std::printf("[sofa_compare] %5.0f | %7.1f %7.1f %+6.1f | %+6.2f %+6.2f %+6.2f | "
                    "%5.2f %5.2f | %5.2f %5.2f | %5.2f %5.2f | %7.1f %7.1f\n",
                    az, meas.itdOnsetUs, sph.itdOnsetUs, itdErr, meas.ildBroadbandDb,
                    sph.ildBroadbandDb, ildErr, sdL, sdLsh, sdR, sdRsh, xcL, xcR, itdLbMed,
                    itdLbEsf);
        EXPECT_LT(std::abs(itdErr), 1000.0) << "az=" << az;
        EXPECT_LT(std::abs(ildErr), 30.0) << "az=" << az;
        sumItdErr += std::abs(itdErr);
        sumIldErr += std::abs(ildErr);
        sumSd += (sdL + sdR) / 2.0;
        ++n;
    }
    std::printf("[sofa_compare] ESFERA RIGIDA: |err ITD onset| medio %.1f us, |err ILD| medio "
                "%.2f dB, SD media %.2f dB\n",
                sumItdErr / n, sumIldErr / n, sumSd / n);
    // ILD por banda a 90 grados: la esfera subestima el ILD medido por el
    // bright spot en el oido contralateral (ver physical_head_model.hpp).
    const Ivanna::HRTFEntry* e90 = findAzimuth(db, 90.0);
    ASSERT_NE(e90, nullptr);
    const HrirMetrics m90 = computeMetrics(e90->left.data(), e90->right.data(), 512, kSr);
    std::vector<double> hL(512), hR(512);
    ASSERT_TRUE(ivanna::physical::synthesizeRigidSphereHrir(M_PI / 2.0, kSr, 512, 0.002,
                                                            hL.data(), hR.data()));
    std::vector<float> fL(hL.begin(), hL.end()), fR(hR.begin(), hR.end());
    const HrirMetrics s90 = computeMetrics(fL.data(), fR.data(), 512, kSr);
    std::printf("[sofa_compare] ILD por banda a 90 grados (centro Hz: 250 500 1k 2k 4k 8k 16k)\n");
    std::printf("[sofa_compare]   medido:");
    for (int b = 0; b < kNumBands; ++b) std::printf(" %+5.1f", m90.ildBandDb[b]);
    std::printf("\n[sofa_compare]   esfera:");
    for (int b = 0; b < kNumBands; ++b) std::printf(" %+5.1f", s90.ildBandDb[b]);
    std::printf("\n");
}

// ── TRABAJADOR B: Ley de Curvatura (fused/physical_propagation.hpp) ───────
// La prueba que cierra el mapa de B (\u00a73.5): la ley predice el ITD de
// campo cercano contra SOFA MEDIDO. MIT KEMAR r=1.4 m y TU-Berlin QU KEMAR
// r=0.5 m (tu_berlin_kemar.ihr1, 360 pos, el=0). Comparador: ITD de banda
// baja (150-500 Hz, retardo de grupo), el que la ley modela (rayos +
// reptante, sin difraccion de onda). La inversion distancia-desde-exceso
// se alimenta con el exceso MEDIDO (no con el de la ley): si el exceso
// medido no es de curvatura, la inversion lo delata.
TEST(SofaComparison, CurvatureLawVsMeasured) {
    using namespace ivanna::fused::propagation;
    Ivanna::HRTFBinLoader mit, tub;
    ASSERT_TRUE(mit.load(ihr1Path().c_str()));
    const std::string tubPath =
        (std::filesystem::path(IVANNA_HRTF_ASSET_DIR) / "tu_berlin_kemar.ihr1").string();
    ASSERT_TRUE(tub.load(tubPath.c_str())) << tubPath;
    ASSERT_EQ(tub.size(), 360u);

    const double kWoodworth90 = 0.0875 / 343.0 * (M_PI / 2.0 + 1.0) * 1e6;  // 655.8 us
    std::printf("[sofa_compare] LEY DE CURVATURA (B) vs MEDIDO: ITD banda baja 150-500 Hz, az=90\n");
    std::printf("[sofa_compare] Woodworth 90 grados = %.1f us\n", kWoodworth90);
    for (const auto& ds : {std::make_pair(&mit, 1.4), std::make_pair(&tub, 0.5)}) {
        const Ivanna::HRTFEntry* e = findAzimuth(*ds.first, 90.0);
        ASSERT_NE(e, nullptr);
        const double itdLb = lowBandItdUs(e->left.data(), e->right.data(), 512, kSr, 150, 500);
        const double law = curvatureLawItdSeconds(M_PI / 2.0, ds.second) * 1e6;
        const double excess = itdLb - kWoodworth90;
        const double rEst = distanceFromItdExcess(excess * 1e-6);
        const HrirMetrics m = computeMetrics(e->left.data(), e->right.data(), 512, kSr);
        const double ildDiv = curvatureLawIldDivergenceDb(M_PI / 2.0, ds.second);
        std::printf("[sofa_compare] r=%.1f m: ITD medido LB %.1f us | ley %.1f us (err %+.1f) | "
                    "exceso medido %+.1f us | r estimada desde exceso %.3f m (real %.1f m) | "
                    "ILD medido %.2f dB vs divergencia ley %.2f dB\n",
                    ds.second, itdLb, law, law - itdLb, excess, rEst, ds.second,
                    m.ildBroadbandDb, ildDiv);
        EXPECT_GT(itdLb, 500.0);
        EXPECT_LT(itdLb, 900.0);
        EXPECT_GT(law, 600.0);
        EXPECT_LT(law, 750.0);
    }
    // Tabla por azimut a r=1.4 m (MIT): la ley es simetrica y monotona.
    std::printf("[sofa_compare] r=1.4 m, ITD banda baja por azimut (medido vs ley)\n");
    for (double az : {0.0, 30.0, 60.0, 90.0, 120.0, 180.0}) {
        const Ivanna::HRTFEntry* e = findAzimuth(mit, az);
        ASSERT_NE(e, nullptr);
        const double itdLb = lowBandItdUs(e->left.data(), e->right.data(), 512, kSr, 150, 500);
        const double law = curvatureLawItdSeconds(az * M_PI / 180.0, 1.4) * 1e6;
        std::printf("[sofa_compare] az=%5.0f: medido LB %7.1f us | ley %7.1f us | err %+6.1f us\n",
                    az, itdLb, law, law - itdLb);
    }
}

// ── TRABAJADOR A (2): Ley Geodesica del Notch (fused/geodesic_pinna_model.hpp)
// FISICA NUEVA candidata: el pabellon como interferometro de dos caminos
// geodesicos. La ley predice la MIGRACION del notch con la elevacion
// (6.4 kHz a -40° -> 11.2 kHz a +70°, familia baja) — algo que la esfera
// rigida NO puede hacer en absoluto (su |H| es suave, sin notches).
// Verificacion INDEPENDIENTE de C (no la tabla de A):
//  (a) las frecuencias medidas se extrajeron yo mismo del SOFA con h5py
//      (FFT 8192 + media movil, minimos de |H_L| a az=0): la familia baja
//      migra 6503 -> 11213 Hz de -20° a +50° en MI extraccion (la de A:
//      6944 -> 10890 Hz; diferencia maxima 441 Hz, por suavizado distinto);
//      la familia alta a -40°/-30° da 9981/10933 Hz (A: 10115/10971).
//      La ley se comprueba contra MI extraccion, tolerancia +-650 Hz.
//  (b) leave-one-out refitado por C en Python (scipy least_squares sobre
//      la tabla medida de A): BAJA RMS 119 Hz / max 185 Hz (A afirma
//      147/278); ALTA RMS 464 Hz / max 990 Hz (A afirma 445/904); nulo
//      (f=cte) RMS 1718 Hz (baja) / 1242 Hz (alta). La ley predice
//      elevaciones NO usadas en el ajuste ~15x (baja) / ~2.7x (alta)
//      mejor que el nulo.
//  (c) extremo a extremo: HRIRs del operador unificado (esfera x pinna)
//      sintetizadas por muestreo en frecuencia + IFFT, contra la HRTF
//      medida MIT KEMAR a az=0 en varias elevaciones, con el MISMO arnes
//      (SD de forma por banda). La sintesis se autovalida primero contra
//      synthesizeRigidSphereHrir (mismo ITD/ILD dentro de tolerancia).
TEST(SofaComparison, PinnaNotchVsMeasured) {
    using ivanna::physical::kPinnaHighFamily;
    using ivanna::physical::kPinnaLowFamily;
    using ivanna::physical::pinnaFactor;
    using ivanna::physical::pinnaNotchFrequencyHz;
    using ivanna::physical::rigidSphereEarTransfer;

    // (a) Ley vs MI extraccion independiente del SOFA (az=0, oido L).
    const double kLowEl[] = {-20, -10, 0, 10, 20, 30, 40, 50};
    const double kLowMine[] = {6503, 7106, 8145, 8753, 9394, 9857, 10627, 11213};
    std::printf("[sofa_compare] LEY GEODESICA DEL NOTCH (A) vs EXTRACCION PROPIA (C)\n");
    for (int i = 0; i < 8; ++i) {
        const double pred = pinnaNotchFrequencyHz(kPinnaLowFamily, kLowEl[i] * M_PI / 180.0);
        std::printf("[sofa_compare]   baja el=%+3.0f: pred %7.0f Hz | extraido C %7.0f Hz | "
                    "err %+6.0f Hz\n",
                    kLowEl[i], pred, kLowMine[i], pred - kLowMine[i]);
        EXPECT_NEAR(pred, kLowMine[i], 650.0) << "elev=" << kLowEl[i];
    }
    const double kHighEl[] = {-40, -30};
    const double kHighMine[] = {9981, 10933};
    for (int i = 0; i < 2; ++i) {
        const double pred = pinnaNotchFrequencyHz(kPinnaHighFamily, kHighEl[i] * M_PI / 180.0);
        std::printf("[sofa_compare]   alta el=%+3.0f: pred %7.0f Hz | extraido C %7.0f Hz | "
                    "err %+6.0f Hz\n",
                    kHighEl[i], pred, kHighMine[i], pred - kHighMine[i]);
        EXPECT_NEAR(pred, kHighMine[i], 700.0) << "elev=" << kHighEl[i];
    }

    // (c) Sintesis por muestreo en frecuencia (N=512, 48 kHz) + IFFT ingenua.
    // H(f) = H_esfera(f, az) * F_pinna(f, el, az); simetria hermitiana; retardo
    // comun de 2 ms (igual que synthesizeRigidSphereHrir: la HRIR de la
    // esfera es no-causal y sin el la IFFT circular la envuelve).
    auto synthUnified = [](double azDeg, double elDeg, bool withPinna,
                           std::vector<float>& outL, std::vector<float>& outR) {
        constexpr std::size_t N = 512;
        const double az = azDeg * M_PI / 180.0, el = elDeg * M_PI / 180.0;
        std::vector<std::complex<double>> HL(N), HR(N);
        for (std::size_t k = 0; k <= N / 2; ++k) {
            const double f = static_cast<double>(k) * kSr / static_cast<double>(N);
            const auto e = rigidSphereEarTransfer(f, az);
            std::complex<double> p(1.0, 0.0);
            if (withPinna) p = pinnaFactor(f, el, az);
            HL[k] = e.left * p;
            HR[k] = e.right * p;
        }
        for (std::size_t k = 1; k < N / 2; ++k) {
            HL[N - k] = std::conj(HL[k]);
            HR[N - k] = std::conj(HR[k]);
        }
        HL[N / 2] = {HL[N / 2].real(), 0.0};
        HR[N / 2] = {HR[N / 2].real(), 0.0};
        std::vector<double> xL(N, 0.0), xR(N, 0.0);
        for (std::size_t n = 0; n < N; ++n) {
            std::complex<double> sL(0, 0), sR(0, 0);
            for (std::size_t k = 0; k < N; ++k) {
                // Kernel e^{-i2pikn/N}: rigidSphereEarTransfer usa la
                // convencion fisica e^{-iwt} (ver physical_head_model.hpp:
                // "conjugacion obligatoria e^{-iwt}<->DFT"); con el kernel
                // +i la HRIR sale invertida en el tiempo (ITD con el signo
                // cambiado: medido -623 us vs +680 us de referencia).
                const double ang = -2.0 * M_PI * static_cast<double>(k * n) / static_cast<double>(N);
                const std::complex<double> w(std::cos(ang), std::sin(ang));
                sL += HL[k] * w;
                sR += HR[k] * w;
            }
            xL[n] = sL.real() / static_cast<double>(N);
            xR[n] = sR.real() / static_cast<double>(N);
        }
        const std::size_t shift = static_cast<std::size_t>(std::lround(0.002 * kSr));
        outL.assign(N, 0.0f);
        outR.assign(N, 0.0f);
        for (std::size_t n = 0; n < N; ++n) {
            outL[(n + shift) % N] = static_cast<float>(xL[n]);
            outR[(n + shift) % N] = static_cast<float>(xR[n]);
        }
    };

    // Autovalidacion de la sintesis: esfera sola por mi muestreo vs
    // synthesizeRigidSphereHrir (mismo ITD onset e ILD).
    std::vector<float> myL, myR;
    synthUnified(90.0, 0.0, false, myL, myR);
    std::vector<double> hL(512), hR(512);
    ASSERT_TRUE(ivanna::physical::synthesizeRigidSphereHrir(M_PI / 2.0, kSr, 512, 0.002,
                                                            hL.data(), hR.data()));
    std::vector<float> refL(hL.begin(), hL.end()), refR(hR.begin(), hR.end());
    const HrirMetrics myM = computeMetrics(myL.data(), myR.data(), 512, kSr);
    const HrirMetrics refM = computeMetrics(refL.data(), refR.data(), 512, kSr);
    ASSERT_TRUE(myM.valid && refM.valid);
    std::printf("[sofa_compare] autovalidacion sintesis az=90: ITD onset mio %.1f vs ref %.1f us | "
                "ILD mio %.2f vs ref %.2f dB\n",
                myM.itdOnsetUs, refM.itdOnsetUs, myM.ildBroadbandDb, refM.ildBroadbandDb);
    EXPECT_NEAR(myM.itdOnsetUs, refM.itdOnsetUs, 30.0);
    EXPECT_NEAR(myM.ildBroadbandDb, refM.ildBroadbandDb, 0.5);

    // (c) Operador unificado vs medido a az=0, varias elevaciones.
    Ivanna::HRTFBinLoader db;
    ASSERT_TRUE(db.load(ihr1Path().c_str()));
    auto findAzEl = [](const Ivanna::HRTFBinLoader& d, double az, double el) {
        for (std::size_t i = 0; i < d.size(); ++i) {
            const auto& e = d.entry(i);
            if (std::abs(e.azimuthDeg - az) < 0.01 && std::abs(e.elevationDeg - el) < 0.01) return &e;
        }
        return static_cast<const Ivanna::HRTFEntry*>(nullptr);
    };
    std::printf("[sofa_compare] OPERADOR UNIFICADO (esfera x pinna) vs MEDIDO, az=0\n");
    std::printf("[sofa_compare] %5s | %5s %5s | %5s %5s | %5s %5s\n", "elev",
                "SDshEsfL", "SDshEsfR", "SDshUniL", "SDshUniR", "dipUni", "dipMed");
    for (double el : {0.0, 30.0, -20.0}) {
        const Ivanna::HRTFEntry* e = findAzEl(db, 0.0, el);
        ASSERT_NE(e, nullptr) << "elev=" << el;
        const HrirMetrics meas = computeMetrics(e->left.data(), e->right.data(), 512, kSr);
        ASSERT_TRUE(meas.valid);
        std::vector<float> sL, sR, uL, uR;
        synthUnified(0.0, el, false, sL, sR);
        synthUnified(0.0, el, true, uL, uR);
        const HrirMetrics sph = computeMetrics(sL.data(), sR.data(), 512, kSr);
        const HrirMetrics uni = computeMetrics(uL.data(), uR.data(), 512, kSr);
        ASSERT_TRUE(sph.valid && uni.valid);
        const double sdEsfL = spectralDistortionDb(sph.magL, meas.magL, true);
        const double sdEsfR = spectralDistortionDb(sph.magR, meas.magR, true);
        const double sdUniL = spectralDistortionDb(uni.magL, meas.magL, true);
        const double sdUniR = spectralDistortionDb(uni.magR, meas.magR, true);
        // Profundidad del notch (dB) en 7.5-8.5 kHz vs pico local fuera:
        // espectro fino (FFT 4096) de la HRIR unificada y de la medida.
        auto notchDipDb = [](const float* x, std::size_t n, double sr) {
            constexpr std::size_t kFft = 4096;
            std::vector<std::complex<double>> a(kFft, {0.0, 0.0});
            for (std::size_t i = 0; i < n && i < kFft; ++i) a[i] = {x[i], 0.0};
            fft4096(a);
            double dip = 1e9, peak = -1e9;
            for (std::size_t k = 0; k <= kFft / 2; ++k) {
                const double f = static_cast<double>(k) * sr / static_cast<double>(kFft);
                const double db = 20.0 * std::log10(std::abs(a[k]) + 1e-30);
                if (f >= 7500.0 && f <= 8500.0) dip = std::min(dip, db);
                if ((f >= 6000.0 && f < 7500.0) || (f > 8500.0 && f <= 9500.0))
                    peak = std::max(peak, db);
            }
            return peak - dip;
        };
        const double dipUni = notchDipDb(uL.data(), 512, kSr);
        const double dipMed = notchDipDb(e->left.data(), 512, kSr);
        std::printf("[sofa_compare] %5.0f | %5.2f %5.2f | %5.2f %5.2f | %5.1f %5.1f\n", el, sdEsfL,
                    sdEsfR, sdUniL, sdUniR, dipUni, dipMed);
        // El operador unificado abre el notch frontal que la esfera no tiene.
        if (el == 0.0) EXPECT_GT(dipUni, 6.0) << "el notch unificado debe ser >= 6 dB a elev 0";
        // La SD de forma por banda de octava NO mejora con el pinna (medido:
        // 5.73 -> 6.74 dB a elev 0): el notch es estrecho dentro de la banda
        // de 8 kHz y el rizado de banda ancha de F_pinna (dos caminos con
        // retardo) anade error de forma. La evidencia del notch es el dip
        // (11.0 dB unificado vs 30.1 dB medido a elev 0: posicion correcta,
        // profundidad subestimada, como admite A con rho constantes), no la
        // SD por octavas. Guarda blanda: que no empeore mas de 2 dB.
        EXPECT_LT((sdUniL + sdUniR) / 2.0, (sdEsfL + sdEsfR) / 2.0 + 2.0) << "elev=" << el;
    }
}

// ── Metodo PRINCIPAL: cues de SALIDA con ruido estacionario ─────────────────
// La cadena ORBE·Ω NO es LTI y NO tiene funcion de transferencia:
//   · OrbeBinaural ESTIMA azimut/distancia por banda DESDE la propia
//     senal de entrada (estimateBands: ITD/ILD/IPD/coherencia medidos) y
//     re-renderiza con esos estimados (renderBins). Con ruido diotico
//     estima centro; tras la HRTF estima el azimut que la HRTF impuso.
//   · Por eso el metodo Welch H(f)=Sxy/Sxx da basura (medido: ITD de
//     -1121 us a az=0, +3312 us a 60/120, xcorr ~0.01 con la HRIR): no
//     hay H(f) que estimar en un sistema adaptativo.
//   · El metodo del impulso TAMPOCO vale: la cadena es STFT por bloques
//     (FFT 512, hop 128) y su respuesta al impulso esta dominada por la
//     latencia de bloque (~590 muestras) y el esparcido STFT.
// La medida VALIDA de un renderizador adaptativo es la de sus CUES DE
// SALIDA ante una entrada estacionaria con cues conocidas (aqui: ruido
// blanco diotico, fuente al centro): ITD de salida por correlacion
// cruzada yL vs yR, ILD por banda por cociente de PSD (Welch), y
// magnitud efectiva por banda = PSD_salida/PSD_entrada. Eso es lo que
// un oyente recibiria: si ORBE coloca la fuente en az, la salida debe
// llevar los cues de az. Estimulo: ruido blanco diotico determinista
// (xorshift64), amp 0.5. 1 s de asentamiento (crossfade de pose 341 ms
// + estimador ORBE ~20 ms) + 2 s de medida.
struct OrbeOutputCues {
    double itdXcorrUs = std::nan("");  // ITD de la SALIDA (xcorr yL vs yR)
    double itdLowBandUs = std::nan("");  // ITD de la SALIDA, banda 150-500 Hz
    double ildBroadbandDb = std::nan("");
    double magL[kNumBands] = {};       // magnitud efectiva dB/banda (PSDout/PSD_in)
    double magR[kNumBands] = {};
    double ildBandDb[kNumBands] = {};
    double peakXcorr = std::nan("");   // coherencia interaural (pico xcorr norm.)
    bool valid = false;
};

OrbeOutputCues orbeOutputCues(double azSofa, float orbeAmount, bool* hasDataset) {
    double az = azSofa;
    if (az > 180.0) az -= 360.0;  // SOFA 0..355 -> ORBE -180..180
    SpatialControls c;
    c.amount = orbeAmount;
    c.azimuthDeg = static_cast<float>(az);
    c.distanceM = 1.4f;  // r del SOFA MIT KEMAR
    c.width = 1.0f;
    c.mode = OmegaOutputMode::HEADPHONES;
    c.wfsSpread = 0.0f;
    c.hrtfWet = 1.0f;
    c.headTracking = false;
    c.xtcEnabled = false;
    c.upmixEnabled = false;
    c.upmixImmersivity = 1.25f;
    c.clamp();

    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(c);
    if (hasDataset) *hasDataset = s.hasHrtfDataset();

    auto makeNoise = [](std::size_t n) {
        std::vector<double> x(n);
        std::uint64_t st = 0x9E3779B97F4A7C15ull;
        for (auto& v : x) {
            st ^= st << 13;
            st ^= st >> 7;
            st ^= st << 17;
            const double u = static_cast<double>(st >> 11) / 9007199254740992.0;
            v = (2.0 * u - 1.0) * 0.5;
        }
        return x;
    };
    std::vector<double> warm = makeNoise(static_cast<std::size_t>(kCaptureFrames));
    s.process(warm.data(), warm.data(), kCaptureFrames);  // asentar pose + estimador
    constexpr int kMeasureFrames = 96000;                 // 2 s de medida
    std::vector<double> x = makeNoise(static_cast<std::size_t>(kMeasureFrames));
    std::vector<double> yL = x, yR = x;
    s.process(yL.data(), yR.data(), kMeasureFrames);

    OrbeOutputCues out;
    // ITD de salida: xcorr yL vs yR sobre la serie temporal (mismo
    // estimador que itdXcorrUs, pero sobre la SALIDA estacionaria).
    std::vector<float> fL(yL.begin(), yL.end()), fR(yR.begin(), yR.end());
    out.itdXcorrUs = itdXcorrUs(fL.data(), fR.data(), fL.size(), kSr);
    // ITD de banda baja (150-500 Hz) de la salida: mismo estimador que
    // usan la esfera rigida y la Ley de Curvatura para la comparacion.
    out.itdLowBandUs = lowBandItdUs(fL.data(), fR.data(), fL.size(), kSr, 150.0, 500.0);
    // Pico de xcorr normalizada = coherencia interaural de la salida.
    {
        double eL = 0.0, eR = 0.0;
        for (std::size_t i = 0; i < fL.size(); ++i) {
            eL += (double)fL[i] * fL[i];
            eR += (double)fR[i] * fR[i];
        }
        const int maxLag = static_cast<int>(0.001 * kSr);
        double best = -1.0;
        for (int lag = -maxLag; lag <= maxLag; ++lag) {
            double cc = 0.0;
            for (std::size_t i = 0; i < fL.size(); ++i) {
                const long j = static_cast<long>(i) + lag;
                if (j >= 0 && j < static_cast<long>(fL.size()))
                    cc += (double)fL[i] * fR[j];
            }
            best = std::max(best, cc / std::sqrt(eL * eR));
        }
        out.peakXcorr = best;
    }
    // PSD por banda (Welch, Hann 4096, hop 2048) de entrada y salidas.
    constexpr std::size_t kFft = 4096, kHop = 2048;
    std::vector<double> psdX(kFft, 0.0), psdL(kFft, 0.0), psdR(kFft, 0.0);
    std::vector<double> win(kFft);
    for (std::size_t i = 0; i < kFft; ++i)
        win[i] = 0.5 * (1.0 - std::cos(2.0 * M_PI * static_cast<double>(i) /
                                       static_cast<double>(kFft)));
    int nBlocks = 0;
    for (std::size_t off = 0; off + kFft <= x.size(); off += kHop) {
        std::vector<std::complex<double>> X(kFft), YL(kFft), YR(kFft);
        for (std::size_t i = 0; i < kFft; ++i) {
            X[i] = {x[off + i] * win[i], 0.0};
            YL[i] = {yL[off + i] * win[i], 0.0};
            YR[i] = {yR[off + i] * win[i], 0.0};
        }
        fft4096(X);
        fft4096(YL);
        fft4096(YR);
        for (std::size_t k = 0; k <= kFft / 2; ++k) {
            psdX[k] += std::norm(X[k]);
            psdL[k] += std::norm(YL[k]);
            psdR[k] += std::norm(YR[k]);
        }
        ++nBlocks;
    }
    double totL = 0.0, totR = 0.0;
    for (std::size_t k = 0; k <= kFft / 2; ++k) {
        totL += psdL[k];
        totR += psdR[k];
    }
    out.ildBroadbandDb = 10.0 * std::log10(totL / totR);
    for (int b = 0; b < kNumBands; ++b) {
        double pX = 0.0, pL = 0.0, pR = 0.0;
        for (std::size_t k = 0; k <= kFft / 2; ++k) {
            const double f = static_cast<double>(k) * kSr / static_cast<double>(kFft);
            if (f >= kBandLo[b] && f < kBandHi[b]) {
                pX += psdX[k];
                pL += psdL[k];
                pR += psdR[k];
            }
        }
        out.magL[b] = 10.0 * std::log10(pL / pX + 1e-30);
        out.magR[b] = 10.0 * std::log10(pR / pX + 1e-30);
        out.ildBandDb[b] = out.magL[b] - out.magR[b];
    }
    std::printf("[sofa_compare] az=%5.0f amount=%.1f Welch PSD: %d bloques\n", azSofa,
                static_cast<double>(orbeAmount), nBlocks);
    out.valid = std::isfinite(out.itdXcorrUs) && std::isfinite(out.itdLowBandUs) &&
                std::isfinite(out.ildBroadbandDb) && std::isfinite(out.peakXcorr);
    return out;
}

// IR efectiva de ORBE·Ω a un azimut: impulso diotico + 1 s de ceros.
// ANTES del impulso se alimenta 1 s de silencio: setHeadPose dispara un
// crossfade de 128 bloques (~341 ms) al banco del nuevo azimut, y el
// impulso en el frame 0 mediria el banco VIEJO (az 0) — con el warm-up el
// impulso sondea el banco ya asentado en el azimut pedido.
HrirMetrics orbeEffectiveIr(double azSofa, std::vector<float>* outL, std::vector<float>* outR,
                            bool* hasDataset) {
    double az = azSofa;
    if (az > 180.0) az -= 360.0;  // SOFA 0..355 -> ORBE -180..180
    SpatialControls c;
    c.amount = 1.0f;
    c.azimuthDeg = static_cast<float>(az);
    c.distanceM = 1.4f;  // r del SOFA MIT KEMAR
    c.width = 1.0f;
    c.mode = OmegaOutputMode::HEADPHONES;
    c.wfsSpread = 0.0f;
    c.hrtfWet = 1.0f;
    c.headTracking = false;
    c.xtcEnabled = false;
    c.upmixEnabled = false;
    c.upmixImmersivity = 1.25f;
    c.clamp();

    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(c);
    if (hasDataset) *hasDataset = s.hasHrtfDataset();
    std::vector<double> warm(static_cast<std::size_t>(kCaptureFrames), 0.0);
    s.process(warm.data(), warm.data(), kCaptureFrames);  // asentar crossfade
    std::vector<double> l(static_cast<std::size_t>(kCaptureFrames), 0.0),
        r(static_cast<std::size_t>(kCaptureFrames), 0.0);
    l[0] = 1.0;
    r[0] = 1.0;
    s.process(l.data(), r.data(), kCaptureFrames);
    outL->assign(l.begin(), l.begin() + kIrLen);
    outR->assign(r.begin(), r.begin() + kIrLen);
    // Diagnostico de forma de la IR (solo az 0 y 90): el STFT de ORBE
    // esparce el impulso y el onset a 5% puede caer en un artefacto.
    if (azSofa == 0.0 || azSofa == 90.0) {
        auto peakIdx = [](const std::vector<double>& v) {
            std::size_t bi = 0;
            double bp = 0.0;
            for (std::size_t i = 0; i < v.size(); ++i)
                if (std::abs(v[i]) > bp) { bp = std::abs(v[i]); bi = i; }
            return bi;
        };
        std::printf("[sofa_compare] DBG az=%5.0f IR: onsetL=%.1f onsetR=%.1f "
                    "peakL@%zu=%.4f peakR@%zu=%.4f E(0..127)=%.3g E(128..4095)=%.3g\n",
                    azSofa, onsetSamples(outL->data(), outL->size()),
                    onsetSamples(outR->data(), outR->size()), peakIdx(l), l[peakIdx(l)],
                    peakIdx(r), r[peakIdx(r)],
                    [&] { double e = 0; for (int i = 0; i < 128; ++i) e += l[i]*l[i]+r[i]*r[i]; return e; }(),
                    [&] { double e = 0; for (int i = 128; i < kIrLen; ++i) e += l[i]*l[i]+r[i]*r[i]; return e; }());
    }
    // Energia de cola mas alla de la ventana (diagnostico: la IR efectiva
    // deberia estar contenida en 4096 muestras; si no, la ventana miente).
    double tail = 0.0, tot = 0.0;
    for (int i = 0; i < kCaptureFrames; ++i) {
        const double e = l[i] * l[i] + r[i] * r[i];
        tot += e;
        if (i >= kIrLen) tail += e;
    }
    std::printf("[sofa_compare] az=%5.0f cola>4096: %.2f%% de la energia\n", azSofa,
                tot > 0.0 ? 100.0 * tail / tot : 0.0);
    return computeMetrics(outL->data(), outR->data(), static_cast<std::size_t>(kIrLen), kSr);
}

}  // namespace

TEST(SofaComparison, MetricsSelfCheck) {
    // Retardo conocido: R retrasada 10 muestras a 48 kHz -> +208.33 us.
    std::vector<float> l(512, 0.0f), r(512, 0.0f);
    l[100] = 1.0f;
    r[110] = 1.0f;
    EXPECT_NEAR(itdOnsetUs(l.data(), r.data(), l.size(), kSr), 10.0 / 48000.0 * 1e6, 25.0);
    EXPECT_NEAR(itdXcorrUs(l.data(), r.data(), l.size(), kSr), 10.0 / 48000.0 * 1e6, 25.0);
    // Ganancia conocida: R a la mitad -> ILD +6.02 dB.
    std::fill(l.begin(), l.end(), 0.0f);
    std::fill(r.begin(), r.end(), 0.0f);
    for (int i = 0; i < 512; ++i) {
        l[i] = static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * i / 48000.0));
        r[i] = 0.5f * l[i];
    }
    const HrirMetrics m = computeMetrics(l.data(), r.data(), l.size(), kSr);
    EXPECT_NEAR(m.ildBroadbandDb, 6.0206, 0.05);
    EXPECT_NEAR(m.itdOnsetUs, 0.0, 25.0);
    // Espectros identicos -> SD = 0 (raw y shape).
    EXPECT_NEAR(spectralDistortionDb(m.magL, m.magL, false), 0.0, 1e-9);
    EXPECT_NEAR(spectralDistortionDb(m.magL, m.magL, true), 0.0, 1e-9);
    // Correlacion de una IR consigo misma = 1.
    EXPECT_NEAR(maxNormXcorr(l.data(), l.data(), l.size(), kSr), 1.0, 1e-6);
}

TEST(SofaComparison, MeasuredReferenceSanity) {
    Ivanna::HRTFBinLoader db;
    ASSERT_TRUE(db.load(ihr1Path().c_str())) << ihr1Path();
    ASSERT_TRUE(db.isIHR1Format());
    EXPECT_EQ(db.size(), 710u);
    EXPECT_FLOAT_EQ(db.header().sampleRate, 48000.0f);
    EXPECT_EQ(db.header().taps, 512u);

    int nHoriz = 0;
    double maxItd = 0.0, maxIld = 0.0;
    for (std::size_t i = 0; i < db.size(); ++i) {
        const auto& e = db.entry(i);
        if (std::abs(e.elevationDeg) >= 0.01) continue;
        ++nHoriz;
        const HrirMetrics m = computeMetrics(e.left.data(), e.right.data(), e.left.size(), kSr);
        ASSERT_TRUE(m.valid) << "az=" << e.azimuthDeg;
        maxItd = std::max(maxItd, std::abs(m.itdOnsetUs));
        maxIld = std::max(maxIld, std::abs(m.ildBroadbandDb));
        if (std::abs(e.azimuthDeg) < 0.01) {
            EXPECT_NEAR(m.itdOnsetUs, 0.0, 5.0) << "frontal: ITD debe ser ~0";
            EXPECT_NEAR(m.ildBroadbandDb, 0.0, 0.05) << "frontal: ILD debe ser ~0";
        }
        if (std::abs(e.azimuthDeg - 90.0) < 0.01) {
            EXPECT_GT(m.itdOnsetUs, 500.0);
            EXPECT_LT(m.itdOnsetUs, 750.0);
            std::printf("[sofa_compare] MEDIDO az=90: ITD onset %.1f us (xcorr %.1f), ILD %.2f dB\n",
                        m.itdOnsetUs, m.itdXcorrUs, m.ildBroadbandDb);
        }
    }
    EXPECT_EQ(nHoriz, 72);
    EXPECT_GT(maxItd, 550.0);
    EXPECT_LT(maxItd, 700.0);
    EXPECT_GT(maxIld, 15.0);
    EXPECT_LT(maxIld, 20.0);
    std::printf("[sofa_compare] MEDIDO (kemar.ihr1): max|ITD onset| %.1f us, max|ILD| %.2f dB\n",
                maxItd, maxIld);
}

TEST(SofaComparison, SofaLoaderIsMeasuredNotReconstruction) {
    // v2.5.472 Fase 2 ítem D: el lector HDF5 mínimo lee Data.IR de verdad.
    // El SOFA MIT KEMAR real (710 pos, 512 taps, 44.1 kHz) se parsea como
    // PARSED_MEASURED; hrtf() es el par frontal MEDIDO (no el par PCA de
    // 128 taps) y hrtfAt() varía con el azimut. La reconstrucción PCA solo
    // queda para archivos no parseables (ver test_sofa_hrtf_loader.cpp).
    Ivanna::SofaHRTFLoader loader;
    ASSERT_TRUE(loader.load(sofaPath())) << sofaPath();
    EXPECT_EQ(loader.lastStatus(), Ivanna::SofaHRTFLoader::Status::PARSED_MEASURED);
    EXPECT_TRUE(loader.isMeasured());
    EXPECT_FALSE(loader.isReconstructedFromMasterBasis());
    EXPECT_EQ(loader.numPositions(), 710u);
    EXPECT_EQ(loader.tapsPerEar(), 512u);
    EXPECT_FLOAT_EQ(loader.sampleRateHz(), 44100.0f);
    const auto& h = loader.hrtf();
    ASSERT_EQ(h.left.size(), h.right.size());
    EXPECT_EQ(h.left.size(), 512u);
    EXPECT_FLOAT_EQ(h.sampleRate, 44100.0f);
    Ivanna::HRTFIR front;
    ASSERT_TRUE(loader.hrtfAt(0.0f, 0.0f, front));
    EXPECT_EQ(h.left, front.left);
    EXPECT_EQ(h.right, front.right);
    Ivanna::HRTFIR side;
    ASSERT_TRUE(loader.hrtfAt(90.0f, 0.0f, side));
    EXPECT_NE(front.left, side.left) << "hrtfAt no varía con el azimut";
    const HrirMetrics m = computeMetrics(h.left.data(), h.right.data(), h.left.size(), 44100.0);
    EXPECT_TRUE(m.valid);
    std::printf("[sofa_compare] LOADER medido (MIT KEMAR, 710 pos, 512 taps, 44.1 kHz): "
                "ITD onset %.1f us, ITD xcorr %.1f us, ILD %.2f dB, pico %.4f\n",
                m.itdOnsetUs, m.itdXcorrUs, m.ildBroadbandDb, m.peakAbs);
}

TEST(SofaComparison, OrbeEffectiveIrVsMeasured) {
    Ivanna::HRTFBinLoader db;
    ASSERT_TRUE(db.load(ihr1Path().c_str()));

    const double azimuths[] = {0.0, 30.0, 60.0, 90.0, 120.0, 180.0};
    std::printf("[sofa_compare] ORBE·Ω actual (CUES DE SALIDA, ruido blanco diotico "
                "estacionario, HEADPHONES amount=1 d=1.4m hrtfWet=1 upmix off WFS off) "
                "vs MEDIDO (misma sonda: ITD por xcorr, ILD por PSD)\n");
    std::printf("[sofa_compare] %5s | %7s %7s %6s | %6s %6s %6s | %5s %5s | %5s %5s | %5s\n",
                "az", "ITDmed", "ITDorbe", "err", "ILDmed", "ILDorbe", "err", "SDL", "SDLsh",
                "SDR", "SDRsh", "coh");
    double sumItdErr = 0.0, sumIldErr = 0.0, sumSd = 0.0;
    int n = 0;
    bool hasDataset = false;
    for (double az : azimuths) {
        const Ivanna::HRTFEntry* e = findAzimuth(db, az);
        ASSERT_NE(e, nullptr) << "az=" << az;
        const HrirMetrics meas =
            computeMetrics(e->left.data(), e->right.data(), e->left.size(), kSr);
        ASSERT_TRUE(meas.valid);
        const OrbeOutputCues orbe = orbeOutputCues(az, 1.0f, &hasDataset);
        ASSERT_TRUE(orbe.valid) << "az=" << az;
        // ITD: mismo estimador en ambos lados (xcorr). El onset medido se
        // reporta en la referencia; aqui el error es xcorr-xcorr.
        const double itdErr = orbe.itdXcorrUs - meas.itdXcorrUs;
        const double ildErr = orbe.ildBroadbandDb - meas.ildBroadbandDb;
        const double sdL = spectralDistortionDb(orbe.magL, meas.magL, false);
        const double sdLsh = spectralDistortionDb(orbe.magL, meas.magL, true);
        const double sdR = spectralDistortionDb(orbe.magR, meas.magR, false);
        const double sdRsh = spectralDistortionDb(orbe.magR, meas.magR, true);
        std::printf("[sofa_compare] %5.0f | %7.1f %7.1f %+6.1f | %+6.2f %+6.2f %+6.2f | "
                    "%5.2f %5.2f | %5.2f %5.2f | %5.2f\n",
                    az, meas.itdXcorrUs, orbe.itdXcorrUs, itdErr, meas.ildBroadbandDb,
                    orbe.ildBroadbandDb, ildErr, sdL, sdLsh, sdR, sdRsh, orbe.peakXcorr);
        // Guardas de cordura (amplias): el valor del test es la tabla.
        EXPECT_LT(std::abs(itdErr), 2000.0) << "az=" << az;
        EXPECT_LT(std::abs(ildErr), 30.0) << "az=" << az;
        sumItdErr += std::abs(itdErr);
        sumIldErr += std::abs(ildErr);
        sumSd += (sdL + sdR) / 2.0;
        ++n;
    }
    std::printf("[sofa_compare] ORBE·Ω actual: |err ITD xcorr| medio %.1f us, |err ILD| medio "
                "%.2f dB, SD media %.2f dB, hasHrtfDataset=%d (0=analitica, no KEMAR medido)\n",
                sumItdErr / n, sumIldErr / n, sumSd / n, static_cast<int>(hasDataset));
    // En host ORBE usa HRTF analitica (sin dataset cargado). Si esto cambia,
    // la linea base cambia con ello: el test lo senala para re-medicion.
    EXPECT_FALSE(hasDataset) << "dataset HRTF cargado en host: re-medir la linea base";
}

// Descomposicion: solo-HRTF (ORBE amount=0, passthrough) vs cadena completa.
// Dice cuanto del error viene de la HRTF analitica y cuanto anade ORBE.
TEST(SofaComparison, HrtfOnlyVsFullChain) {
    Ivanna::HRTFBinLoader db;
    ASSERT_TRUE(db.load(ihr1Path().c_str()));
    const double azimuths[] = {0.0, 30.0, 60.0, 90.0, 120.0, 180.0};
    std::printf("[sofa_compare] %5s | %7s %7s | %6s %6s | %5s %5s\n", "az", "ITDhrt",
                "ITDfull", "ILDhrt", "ILDfull", "SDLhrt", "SDLful");
    for (double az : azimuths) {
        const Ivanna::HRTFEntry* e = findAzimuth(db, az);
        ASSERT_NE(e, nullptr);
        const HrirMetrics meas =
            computeMetrics(e->left.data(), e->right.data(), e->left.size(), kSr);
        const OrbeOutputCues hrtf = orbeOutputCues(az, 0.0f, nullptr);
        const OrbeOutputCues full = orbeOutputCues(az, 1.0f, nullptr);
        ASSERT_TRUE(hrtf.valid && full.valid);
        const double sdH = spectralDistortionDb(hrtf.magL, meas.magL, true);
        const double sdF = spectralDistortionDb(full.magL, meas.magL, true);
        std::printf("[sofa_compare] %5.0f | %7.1f %7.1f | %+6.2f %+6.2f | %5.2f %5.2f "
                    "(medido ITD xc %.1f, ILD %.2f)\n",
                    az, hrtf.itdXcorrUs, full.itdXcorrUs, hrtf.ildBroadbandDb,
                    full.ildBroadbandDb, sdH, sdF, meas.itdXcorrUs, meas.ildBroadbandDb);
    }
}

// El metodo del impulso NO es valido para esta cadena (diagnostico, no
// medida): la cadena es STFT por bloques y su respuesta al impulso esta
// dominada por latencia de bloque + esparcido, no por la HRTF. Medido:
// energia cero en las primeras 128 muestras, onset ~440-500, pico ~590
// (~12 ms). Con el impulso, az=0 daba un ITD espurio de -983 us y xcorr
// ~0.00 con la HRIR medida. Este test fija el artefacto para que nadie
// vuelva a "medir" ORBE con un impulso y reporte esos numeros como si
// fueran la respuesta binaural.
TEST(SofaComparison, ImpulseMethodIsInvalidForThisChain) {
    std::vector<float> oL, oR;
    const HrirMetrics m0 = orbeEffectiveIr(0.0, &oL, &oR, nullptr);
    ASSERT_TRUE(m0.valid);
    double eEarly = 0.0;
    for (int i = 0; i < 128; ++i) eEarly += oL[static_cast<std::size_t>(i)] * oL[static_cast<std::size_t>(i)] +
                                           oR[static_cast<std::size_t>(i)] * oR[static_cast<std::size_t>(i)];
    EXPECT_EQ(eEarly, 0.0) << "la cadena no responde en las primeras 128 muestras (latencia de bloque)";
    EXPECT_GT(onsetSamples(oL.data(), oL.size()), 128.0);
    EXPECT_GT(onsetSamples(oR.data(), oR.size()), 128.0);
    std::printf("[sofa_compare] IMPULSO (diagnostico, NO usar como medida): az=0 onsetL=%.1f "
                "onsetR=%.1f muestras; ITD espurio %.1f us\n",
                onsetSamples(oL.data(), oL.size()), onsetSamples(oR.data(), oR.size()),
                m0.itdOnsetUs);
}


TEST(SofaComparison, ComparisonTableForNewPhysics) {
    // Tabla final de TRES columnas (directiva de Esse 2026-11-18):
    //   (i) fisica existente re-derivada = ESFERA RIGIDA (A): solucion
    //       exacta de la ecuacion de onda, fisica clasica (Rayleigh 1907).
    //   (ii) teoria nueva propuesta = LEY DE CURVATURA (B): formulacion
    //       cerrada en kappa=1/r (ver PHYSICAL_PROPAGATION_MAP.md).
    //   (iii) HRTF medida (MIT KEMAR r=1.4 m) / ORBE·Ω actual.
    // ITD en banda baja (150-500 Hz) para medido/esfera/ley: mismo
    // estimador, mismo azimut. ORBE lleva su ITD por xcorr de banda ancha:
    // la xcorr de banda baja de una salida ESTOCASTICA no tiene pico
    // definido (satura en el borde de la ventana +-1 ms: 1000.0 us), asi
    // que no es comparable; su ITD comparable es la xcorr de banda ancha
    // (ver OrbeEffectiveIrVsMeasured, xcorr contra xcorr). ILD: banda
    // ancha para medido/esfera/ORBE; la ley solo da ILD por divergencia
    // (parcial, sin difraccion). SD (forma): esfera y ORBE vs medido;
    // la ley no produce espectro.
    using namespace ivanna::fused::propagation;
    Ivanna::HRTFBinLoader db;
    ASSERT_TRUE(db.load(ihr1Path().c_str()));
    const double azimuths[] = {0.0, 30.0, 60.0, 90.0, 120.0, 180.0};
    std::printf("[sofa_compare] TABLA 3 COLUMNAS (ITD LB 150-500 Hz, us; ORBE: xcorr banda ancha)\n");
    std::printf("[sofa_compare] %5s | %7s %7s %7s %7s | %6s %6s %6s | %5s %5s\n", "az",
                "ITDmed", "ITDesf", "ITDley", "ITDorb*", "ILDmed", "ILDesf", "ILDorbe",
                "SDshE", "SDshO");
    double sItdEsf = 0, sItdLey = 0, sItdOrbe = 0, sIldEsf = 0, sIldOrbe = 0, sSdE = 0, sSdO = 0;
    int n = 0;
    for (double az : azimuths) {
        const Ivanna::HRTFEntry* e = findAzimuth(db, az);
        ASSERT_NE(e, nullptr);
        const HrirMetrics meas =
            computeMetrics(e->left.data(), e->right.data(), e->left.size(), kSr);
        const double itdLbMed = lowBandItdUs(e->left.data(), e->right.data(), 512, kSr, 150, 500);
        std::vector<double> hL(512), hR(512);
        ASSERT_TRUE(ivanna::physical::synthesizeRigidSphereHrir(
            az * M_PI / 180.0, kSr, 512, 0.002, hL.data(), hR.data()));
        std::vector<float> fL(hL.begin(), hL.end()), fR(hR.begin(), hR.end());
        const HrirMetrics sph = computeMetrics(fL.data(), fR.data(), fL.size(), kSr);
        const double itdLbEsf = lowBandItdUs(fL.data(), fR.data(), 512, kSr, 150, 500);
        const double itdLey = curvatureLawItdSeconds(az * M_PI / 180.0, 1.4) * 1e6;
        const OrbeOutputCues orbe = orbeOutputCues(az, 1.0f, nullptr);
        ASSERT_TRUE(meas.valid && sph.valid && orbe.valid);
        const double sdE = spectralDistortionDb(sph.magL, meas.magL, true);
        const double sdO = spectralDistortionDb(orbe.magL, meas.magL, true);
        std::printf("[sofa_compare] %5.0f | %7.1f %7.1f %7.1f %7.1f | %+6.2f %+6.2f %+6.2f | "
                    "%5.2f %5.2f\n",
                    az, itdLbMed, itdLbEsf, itdLey, orbe.itdXcorrUs, meas.ildBroadbandDb,
                    sph.ildBroadbandDb, orbe.ildBroadbandDb, sdE, sdO);
        sItdEsf += std::abs(itdLbEsf - itdLbMed);
        sItdLey += std::abs(itdLey - itdLbMed);
        sItdOrbe += std::abs(orbe.itdXcorrUs - meas.itdXcorrUs);
        sIldEsf += std::abs(sph.ildBroadbandDb - meas.ildBroadbandDb);
        sIldOrbe += std::abs(orbe.ildBroadbandDb - meas.ildBroadbandDb);
        sSdE += sdE;
        sSdO += sdO;
        ++n;
    }
    std::printf("[sofa_compare] ERRORES MEDIOS vs MEDIDO (r=1.4 m, 6 azimuts):\n");
    std::printf("[sofa_compare]   esfera (fisica existente): |err ITD LB| %.1f us, "
                "|err ILD| %.2f dB, SD forma %.2f dB\n",
                sItdEsf / n, sIldEsf / n, sSdE / n);
    std::printf("[sofa_compare]   ley curvatura (teoria nueva): |err ITD LB| %.1f us "
                "(ILD divergencia: solo parte; sin espectro)\n",
                sItdLey / n);
    std::printf("[sofa_compare]   ORBE actual: |err ITD xcorr| %.1f us, |err ILD| %.2f dB, "
                "SD forma %.2f dB (*ITD ORBE por xcorr banda ancha, no LB)\n",
                sItdOrbe / n, sIldOrbe / n, sSdO / n);
    std::printf("[sofa_compare]   ley geodesica del notch (teoria nueva, A): predice la "
                "migracion del notch con la elevacion, que la esfera NO puede hacer; "
                "LOO refitado por C: RMS 119 Hz / max 185 Hz (baja), 464 Hz / max 990 Hz "
                "(alta); nulo 1718/1242 Hz. Operador unificado: dip 11.0 dB a elev 0 "
                "(medido 30.1 dB), SD forma 6.74 vs 5.73 dB esfera (ver PinnaNotchVsMeasured)\n");
    std::printf("[sofa_compare] CRITERIO (a)+(b)+(c) en SOFA_COMPARISON_METRICS.md. "
                "Veredicto en el informe del trabajador C.\n");
}

// ── HrtfManager + SOFA medido (v2.5.472 Fase 2 ítem D) ─────────────────────
// Cableado: SofaHRTFLoader (Data.IR leído de verdad) -> HrtfManager
// (loadSofaDataset / setSignatureRoom / loadSofaAtPose con remuestreo) ->
// banco HRTF por pose. La evidencia es la RESPUESTA AL IMPULSO efectiva
// del convolucionador (mismo método que test_hrtf_pinna_lgn.cpp): con
// wet=1 la salida es la convolución pura, así que la IR medida ES el banco
// instalado. Neutro: sin SOFA, signatureRoom=0, comportamiento anterior.
namespace {

constexpr float kSofaTestSr = 48000.0f;
constexpr int kSofaSettleBlocks = 140;  // > XFADE_FRAMES (128): el banco nuevo manda

std::unique_ptr<Ivanna::HrtfManager> mkSofaHrtf() {
    auto h = std::make_unique<Ivanna::HrtfManager>();
    h->prepare(kSofaTestSr);
    h->setSofaSearchDir(IVANNA_SOFA_ASSET_DIR);
    h->setWetDry(1.0f);
    return h;
}

// Respuesta al impulso del canal L tras asentar el crossfade: impulso 1.0
// en la muestra 0 del canal L, resto 0; outL[i] = hrir[i] (128 taps).
std::vector<float> sofaImpulseResponseL(Ivanna::HrtfManager& h) {
    Ivanna::AudioBuffer silence{};
    std::memset(&silence, 0, sizeof(silence));
    for (int b = 0; b < kSofaSettleBlocks; ++b) h.processBinauralScene(&silence);
    std::vector<float> out;
    for (int b = 0; b < 4; ++b) {
        Ivanna::AudioBuffer buf{};
        std::memset(&buf, 0, sizeof(buf));
        if (b == 0) buf.left[0] = 1.0f;
        h.processBinauralScene(&buf);
        out.insert(out.end(), buf.left, buf.left + Ivanna::BLOCK_SIZE);
    }
    return out;
}

double sofaEnergy(const std::vector<float>& x, size_t n) {
    double e = 0.0;
    for (size_t i = 0; i < n && i < x.size(); ++i) e += (double)x[i] * x[i];
    return e;
}

size_t sofaPeakIndex(const std::vector<float>& x, size_t n) {
    size_t best = 0;
    float m = 0.0f;
    for (size_t i = 0; i < n && i < x.size(); ++i) {
        if (std::fabs(x[i]) > m) {
            m = std::fabs(x[i]);
            best = i;
        }
    }
    return best;
}

double sofaMaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) m = std::max(m, std::fabs((double)a[i] - b[i]));
    return m;
}

} // namespace

TEST(SofaHrtfWiring, LoadSofaDatasetMeasured) {
    auto h = mkSofaHrtf();
    EXPECT_FALSE(h->hasSofaDataset());
    EXPECT_EQ(h->signatureRoom(), 0);
    ASSERT_TRUE(h->loadSofaDataset(sofaPath().c_str())) << sofaPath();
    EXPECT_TRUE(h->hasSofaDataset());
    // Carga directa != sala del catálogo: el id queda en 0.
    EXPECT_EQ(h->signatureRoom(), 0);
    // Rutas inválidas: false, sin tocar el SOFA vigente (fail-closed).
    EXPECT_FALSE(h->loadSofaDataset(nullptr));
    EXPECT_FALSE(h->loadSofaDataset(""));
    EXPECT_FALSE(h->loadSofaDataset("/no/existe/kemar.sofa"));
    EXPECT_TRUE(h->hasSofaDataset());
}

TEST(SofaHrtfWiring, SignatureRoomSelectsCatalog) {
    auto h = mkSofaHrtf();
    const std::filesystem::path dir(IVANNA_SOFA_ASSET_DIR);
    const char* files[3] = {"TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa",
                            "MIT_KEMAR_normal_pinna.sofa",
                            "demo_FreeFieldHRTF_1_IR.sofa"};
    for (int id = 1; id <= 3; ++id) {
        ASSERT_TRUE(std::filesystem::exists(dir / files[id - 1])) << files[id - 1];
        ASSERT_TRUE(h->setSignatureRoom(id)) << "id " << id;
        EXPECT_EQ(h->signatureRoom(), id);
        EXPECT_TRUE(h->hasSofaDataset());
    }
    // 0 desactiva el SOFA y vuelve al IHR1/síntesis.
    ASSERT_TRUE(h->setSignatureRoom(0));
    EXPECT_EQ(h->signatureRoom(), 0);
    EXPECT_FALSE(h->hasSofaDataset());
    // Id inválido: false, sin tocar el estado.
    EXPECT_FALSE(h->setSignatureRoom(4));
    EXPECT_FALSE(h->setSignatureRoom(-1));
    EXPECT_FALSE(h->setSignatureRoom(99));
    EXPECT_EQ(h->signatureRoom(), 0);
    EXPECT_FALSE(h->hasSofaDataset());
}

TEST(SofaHrtfWiring, UnparseableSofaDoesNotDisplace) {
    auto h = mkSofaHrtf();
    ASSERT_TRUE(h->setSignatureRoom(2));  // MIT KEMAR vigente
    EXPECT_EQ(h->signatureRoom(), 2);
    // Archivo con solo la firma HDF5 (no parseable como medido).
    const std::filesystem::path fake =
        std::filesystem::temp_directory_path() /
        ("ivanna_sofa_fake_" + std::to_string(::getpid()) + ".sofa");
    {
        std::vector<uint8_t> buf(2048, 0x00);
        const uint8_t sig[8] = {0x89, 'H', 'D', 'F', 0x0D, 0x0A, 0x1A, 0x0A};
        for (int i = 0; i < 8; ++i) buf[i] = sig[i];
        std::ofstream f(fake, std::ios::binary);
        f.write(reinterpret_cast<const char*>(buf.data()), (std::streamsize)buf.size());
    }
    EXPECT_FALSE(h->loadSofaDataset(fake.string().c_str()));
    EXPECT_TRUE(h->hasSofaDataset()) << "un SOFA no medido desplazó al vigente";
    EXPECT_EQ(h->signatureRoom(), 2);
    EXPECT_FALSE(h->setSignatureRoom(99));
    EXPECT_EQ(h->signatureRoom(), 2);
    std::filesystem::remove(fake);
}

TEST(SofaHrtfWiring, AzimuthChangesEffectiveHrir) {
    auto h = mkSofaHrtf();
    ASSERT_TRUE(h->loadSofaDataset(sofaPath().c_str()));  // MIT KEMAR, 44.1 kHz
    h->setHeadPose(0.0f, 0.0f, 0.0f);
    const std::vector<float> ir0 = sofaImpulseResponseL(*h);
    ASSERT_EQ(ir0.size(), 4u * Ivanna::BLOCK_SIZE);
    // El azimut 90 usa OTRA medición del archivo (no el mismo banco).
    h->setHeadPose(1.5707963f, 0.0f, 0.0f);  // yaw +90°
    const std::vector<float> ir90 = sofaImpulseResponseL(*h);
    EXPECT_GT(sofaMaxAbsDiff(ir0, ir90), 1.0e-3)
        << "el banco no cambió con el azimut: el SOFA no se usa por pose";
    // Determinista: volver a az 0 reproduce la misma IR bit-exacta.
    h->setHeadPose(0.0f, 0.0f, 0.0f);
    const std::vector<float> ir0b = sofaImpulseResponseL(*h);
    EXPECT_EQ(sofaMaxAbsDiff(ir0, ir0b), 0.0);
    // La IR efectiva ES el HRIR medido remuestreado (44.1 -> 48 kHz):
    // mismo pico (escala de SR) y energía comparable en los 128 taps.
    Ivanna::SofaHRTFLoader loader;
    ASSERT_TRUE(loader.load(sofaPath()));
    Ivanna::HRTFIR native;
    ASSERT_TRUE(loader.hrtfAt(0.0f, 0.0f, native));
    const double peakScale = 48000.0 / 44100.0;
    const size_t nativePeak = sofaPeakIndex(native.left, native.left.size());
    const size_t effPeak = sofaPeakIndex(ir0, Ivanna::HRTF_TAPS);
    EXPECT_LE(std::abs((long)effPeak - (long)std::lround(nativePeak * peakScale)), 2L)
        << "pico efectivo " << effPeak << " vs nativo " << nativePeak;
    const double eEff = sofaEnergy(ir0, Ivanna::HRTF_TAPS);
    const double eNat = sofaEnergy(native.left, (size_t)std::lround(128 / peakScale));
    EXPECT_GT(eEff, 0.0);
    EXPECT_GT(eNat, 0.0);
    const double ratio = eEff / eNat;
    EXPECT_GT(ratio, 0.5);
    EXPECT_LT(ratio, 2.0) << "energía efectiva/nativa fuera de rango: " << ratio;
    // Y NO es la síntesis analítica: sin SOFA el banco es otro.
    auto synth = mkSofaHrtf();
    synth->setHeadPose(0.0f, 0.0f, 0.0f);
    const std::vector<float> irSynth = sofaImpulseResponseL(*synth);
    EXPECT_GT(sofaMaxAbsDiff(ir0, irSynth), 1.0e-3)
        << "la IR con SOFA es idéntica a la síntesis: el SOFA no se instaló";
    std::printf("[sofa_compare] HRTF efectiva SOFA az0: pico tap %zu, E=%.4f; "
                "az90 dif máx %.4f; síntesis dif máx %.4f\n",
                effPeak, eEff, sofaMaxAbsDiff(ir0, ir90), sofaMaxAbsDiff(ir0, irSynth));
}

TEST(SofaHrtfWiring, MeasuredRoomRirSlot) {
    auto h = mkSofaHrtf();
    EXPECT_FALSE(h->hasMeasuredRoomRir());
    const float l[4] = {1.0f, 0.5f, 0.25f, 0.125f};
    const float r[4] = {0.9f, 0.4f, 0.2f, 0.1f};
    ASSERT_TRUE(h->installMeasuredRoomRir(l, r, 4, 48000.0f));
    EXPECT_TRUE(h->hasMeasuredRoomRir());
    EXPECT_EQ(h->measuredRoomRirLeft().size(), 4u);
    EXPECT_EQ(h->measuredRoomRirRight().size(), 4u);
    EXPECT_FLOAT_EQ(h->measuredRoomRirLeft()[0], 1.0f);
    EXPECT_FLOAT_EQ(h->measuredRoomRirRight()[3], 0.1f);
    EXPECT_FLOAT_EQ(h->measuredRoomRirSampleRate(), 48000.0f);
    // Inválidos: false, sin tocar la RIR instalada (fail-closed).
    const float bad[2] = {1.0f, std::nanf("")};
    EXPECT_FALSE(h->installMeasuredRoomRir(nullptr, r, 4, 48000.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, nullptr, 4, 48000.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, r, 0, 48000.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, r, 1000001, 48000.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, r, 4, 100.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, r, 4, 500000.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, r, 4, std::nan("")));
    EXPECT_FALSE(h->installMeasuredRoomRir(bad, r, 2, 48000.0f));
    EXPECT_FALSE(h->installMeasuredRoomRir(l, bad, 2, 48000.0f));
    EXPECT_TRUE(h->hasMeasuredRoomRir());
    EXPECT_EQ(h->measuredRoomRirLeft().size(), 4u);
    h->clearMeasuredRoomRir();
    EXPECT_FALSE(h->hasMeasuredRoomRir());
    EXPECT_FLOAT_EQ(h->measuredRoomRirSampleRate(), 0.0f);
}

TEST(SofaHrtfWiring, NeutralWithoutSofa) {
    auto h = mkSofaHrtf();
    EXPECT_FALSE(h->hasSofaDataset());
    EXPECT_EQ(h->signatureRoom(), 0);
    EXPECT_FALSE(h->evidence().datasetLoaded);  // sin IHR1 ni SOFA en host
    ASSERT_TRUE(h->setSignatureRoom(0));        // no-op, sin sala firma
    EXPECT_FALSE(h->hasSofaDataset());
    EXPECT_EQ(h->signatureRoom(), 0);
}

// ── ITD efectivo a azimut 0° (evol-hrtf, bloque HRTF + ITD) ────────────────
// El defecto: con PCA (sin SOFA) el ITD a azimut 0° era −396 µs (fuente al
// frente desplazada). La causa NO es la Ley de Curvatura (a 0° da 0 exacto)
// sino el PEDESTAL del par base PCA (p0 + V·q): medido en el par crudo,
// xcorr −417 µs, centroide −102 µs, retardo de grupo 150–500 Hz +545 µs
// (el par es difuso: rho_xcorr = 0.137 — no hay un ITD bien definido).
// Método: respuesta al impulso de los 4 caminos (mismo que
// sofaImpulseResponseL, pero con impulso en L y en R) y pico de
// correlación cruzada hLL vs hRR con interpolación parabólica.
// Convención de signo (la de la Ley de Curvatura): ITD > 0 = el oído
// DERECHO va antes = fuente a la derecha; ITD < 0 = fuente a la izquierda.
// A azimut 0° el ITD debe ser ~0: tolerancia ±50 µs (la del barrido Green:
// 1 muestra @48 kHz = 20.83 µs; ±50 µs ≈ ±2.4 muestras — un frontal
// simétrico no debe desplazarse más de eso).
namespace {

struct SofaIr4 {
    std::vector<float> ll, rl, lr, rr;  // directos ll/rr + crossfeed rl/lr
};

SofaIr4 sofaImpulseResponse4(Ivanna::HrtfManager& h) {
    Ivanna::AudioBuffer silence{};
    std::memset(&silence, 0, sizeof(silence));
    for (int b = 0; b < kSofaSettleBlocks; ++b) h.processBinauralScene(&silence);
    SofaIr4 ir;
    for (int ch = 0; ch < 2; ++ch) {  // 0 = impulso en L, 1 = impulso en R
        for (int b = 0; b < 4; ++b) {
            Ivanna::AudioBuffer buf{};
            std::memset(&buf, 0, sizeof(buf));
            if (b == 0) {
                if (ch == 0) buf.left[0] = 1.0f;
                else buf.right[0] = 1.0f;
            }
            h.processBinauralScene(&buf);
            if (ch == 0) {
                ir.ll.insert(ir.ll.end(), buf.left, buf.left + Ivanna::BLOCK_SIZE);
                ir.rl.insert(ir.rl.end(), buf.right, buf.right + Ivanna::BLOCK_SIZE);
            } else {
                ir.lr.insert(ir.lr.end(), buf.left, buf.left + Ivanna::BLOCK_SIZE);
                ir.rr.insert(ir.rr.end(), buf.right, buf.right + Ivanna::BLOCK_SIZE);
            }
        }
    }
    return ir;
}

// Pico de xcorr entre a y b con interpolación parabólica. Devuelve el lag
// en muestras: b retrasado `lag` muestras respecto a a. (Si b va antes,
// lag < 0.)
double sofaXcorrLagSamples(const std::vector<float>& a, const std::vector<float>& b,
                           std::size_t n) {
    const int maxLag = static_cast<int>(n) - 1;
    double best = -1e30;
    int bestLag = 0;
    std::vector<double> c(2 * static_cast<std::size_t>(maxLag) + 1, 0.0);
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double s = 0.0;
        const int i0 = std::max(0, -lag);
        const int i1 = static_cast<int>(std::min(n, n - static_cast<std::size_t>(lag)));
        for (int i = i0; i < i1; ++i)
            s += static_cast<double>(a[static_cast<std::size_t>(i)]) *
                 b[static_cast<std::size_t>(i + lag)];
        c[static_cast<std::size_t>(lag + maxLag)] = s;
        if (s > best) {
            best = s;
            bestLag = lag;
        }
    }
    // Interpolación parabólica alrededor del pico.
    double frac = 0.0;
    if (bestLag > -maxLag && bestLag < maxLag) {
        const double cm = c[static_cast<std::size_t>(bestLag - 1 + maxLag)];
        const double c0 = c[static_cast<std::size_t>(bestLag + maxLag)];
        const double cp = c[static_cast<std::size_t>(bestLag + 1 + maxLag)];
        const double den = cm - 2.0 * c0 + cp;
        if (std::fabs(den) > 1e-30) frac = 0.5 * (cm - cp) / den;
    }
    return static_cast<double>(bestLag) + frac;
}

double sofaCentroid(const std::vector<float>& x, std::size_t n) {
    double num = 0.0, den = 0.0;
    for (std::size_t i = 0; i < n && i < x.size(); ++i) {
        const double e = static_cast<double>(x[i]) * x[i];
        num += static_cast<double>(i) * e;
        den += e;
    }
    return den > 0.0 ? num / den : 0.0;
}

double sofaIldDb(const std::vector<float>& l, const std::vector<float>& r, std::size_t n) {
    const double el = sofaEnergy(l, n), er = sofaEnergy(r, n);
    if (el <= 0.0 || er <= 0.0) return 0.0;
    return 10.0 * std::log10(el / er);
}

// ITD efectivo (µs, signo Ley de Curvatura: + = oído derecho antes) por
// xcorr hLL vs hRR, con centroide e ILD como segundo y tercer estimador.
struct SofaItd {
    double xcorrUs = 0.0;
    double centroidUs = 0.0;
    double ildDb = 0.0;
};

SofaItd sofaMeasureItd(const SofaIr4& ir) {
    SofaItd m;
    const double lag = sofaXcorrLagSamples(ir.ll, ir.rr, Ivanna::HRTF_TAPS);
    m.xcorrUs = -lag * 1e6 / 48000.0;  // lag<0 (R antes) -> ITD>0
    m.centroidUs = (sofaCentroid(ir.ll, Ivanna::HRTF_TAPS) -
                    sofaCentroid(ir.rr, Ivanna::HRTF_TAPS)) * 1e6 / 48000.0;
    m.ildDb = sofaIldDb(ir.ll, ir.rr, Ivanna::HRTF_TAPS);
    return m;
}

void sofaPrintItd(const char* tag, float yawDeg, const SofaItd& m) {
    std::printf("[sofa_itd] %s yaw %+6.1f°: ITD xcorr %+8.1f µs | centroide "
                "%+8.1f µs | ILD %+6.2f dB\\n",
                tag, yawDeg, m.xcorrUs, m.centroidUs, m.ildDb);
}

}  // namespace

TEST(SofaHrtfWiring, PcaPathItdAtZeroAzimuth) {
    auto h = mkSofaHrtf();  // sin SOFA: síntesis PCA (comportamiento por defecto)
    ASSERT_FALSE(h->hasSofaDataset());
    // yaw en radianes; theta (Ley de Curvatura) = -yaw. Se barre theta
    // 0, ±30°, ±90°: a 0° el ITD debe ser ~0; lateral, el signo debe seguir
    // a theta y la magnitud debe crecer con |theta| (Woodworth: ~261 µs
    // a 30°, ~656 µs a 90° a 1 m).
    const float thetas[5] = {0.0f, 30.0f, -30.0f, 90.0f, -90.0f};
    SofaItd m0, m30, m90;
    for (float thetaDeg : thetas) {
        const float yaw = -thetaDeg * 3.14159265f / 180.0f;
        h->setHeadPose(yaw, 0.0f, 0.0f);
        const SofaIr4 ir = sofaImpulseResponse4(*h);
        const SofaItd m = sofaMeasureItd(ir);
        sofaPrintItd("PCA", thetaDeg, m);
        if (thetaDeg == 0.0f) m0 = m;
        if (thetaDeg == 30.0f) m30 = m;
        if (thetaDeg == 90.0f) m90 = m;
        if (thetaDeg == -30.0f)
            EXPECT_LT(m.xcorrUs, 0.0) << "ITD a theta -30° debe ser negativo (fuente a la izquierda)";
        if (thetaDeg == -90.0f)
            EXPECT_LT(m.xcorrUs, 0.0) << "ITD a theta -90° debe ser negativo";
    }
    EXPECT_LE(std::fabs(m0.xcorrUs), 50.0)
        << "ITD a azimut 0° con PCA: la fuente al frente no debe desplazarse";
    EXPECT_GT(m30.xcorrUs, 0.0) << "ITD a theta +30° debe ser positivo (fuente a la derecha)";
    EXPECT_GT(m90.xcorrUs, 0.0) << "ITD a theta +90° debe ser positivo";
    EXPECT_GT(std::fabs(m90.xcorrUs), std::fabs(m30.xcorrUs))
        << "el ITD debe crecer con el azimut";
    EXPECT_GT(std::fabs(m30.xcorrUs), 150.0) << "ITD a 30° demasiado pequeño (Woodworth ~261 µs)";
    EXPECT_LT(std::fabs(m30.xcorrUs), 400.0) << "ITD a 30° demasiado grande (Woodworth ~261 µs)";
    EXPECT_GT(std::fabs(m90.xcorrUs), 500.0) << "ITD a 90° demasiado pequeño (Woodworth ~656 µs)";
    EXPECT_LT(std::fabs(m90.xcorrUs), 800.0) << "ITD a 90° demasiado grande (Woodworth ~656 µs, máx ley 733 µs + escala)";
}

TEST(SofaHrtfWiring, MeasuredSofaItdAtZeroAzimuth) {
    auto h = mkSofaHrtf();
    ASSERT_TRUE(h->loadSofaDataset(sofaPath().c_str()));  // MIT KEMAR medido (assets v2)
    ASSERT_TRUE(h->hasSofaDataset());
    // yaw en radianes; az SOFA = +yaw (grados). El frontal medido es
    // simétrico (L≡R en MIT KEMAR a 0°): ITD 0 por construcción.
    const float yaws[5] = {0.0f, 30.0f, -30.0f, 90.0f, -90.0f};
    SofaItd m0, m30, m90;
    for (float yawDeg : yaws) {
        const float yaw = yawDeg * 3.14159265f / 180.0f;
        h->setHeadPose(yaw, 0.0f, 0.0f);
        const SofaIr4 ir = sofaImpulseResponse4(*h);
        const SofaItd m = sofaMeasureItd(ir);
        sofaPrintItd("MIT", yawDeg, m);
        if (yawDeg == 0.0f) m0 = m;
        if (yawDeg == 30.0f) m30 = m;
        if (yawDeg == 90.0f) m90 = m;
        if (yawDeg == -30.0f)
            EXPECT_GT(m.xcorrUs, 0.0) << "ITD medido a az +(-30°) debe ser positivo (SOFA: az+ = izquierda)";
        if (yawDeg == -90.0f)
            EXPECT_GT(m.xcorrUs, 0.0) << "ITD medido a az -90° debe ser positivo";
    }
    EXPECT_LE(std::fabs(m0.xcorrUs), 50.0)
        << "ITD a azimut 0° con SOFA medido: el frontal de MIT KEMAR es simétrico";
    EXPECT_LT(m30.xcorrUs, 0.0) << "ITD medido a az +30° debe ser negativo (SOFA: az+ = izquierda)";
    EXPECT_LT(m90.xcorrUs, 0.0) << "ITD medido a az +90° debe ser negativo";
    EXPECT_GT(std::fabs(m90.xcorrUs), std::fabs(m30.xcorrUs))
        << "el ITD medido debe crecer con el azimut";
    EXPECT_GT(std::fabs(m30.xcorrUs), 150.0) << "ITD medido a 30° demasiado pequeño";
    EXPECT_LT(std::fabs(m30.xcorrUs), 400.0) << "ITD medido a 30° demasiado grande";
    EXPECT_GT(std::fabs(m90.xcorrUs), 500.0) << "ITD medido a 90° demasiado pequeño";
    EXPECT_LT(std::fabs(m90.xcorrUs), 800.0) << "ITD medido a 90° demasiado grande";
}

TEST(SofaHrtfWiring, ModuleV0SofaLoadsInHrtfManager) {
    // Escenario exacto del teléfono: los SOFA del módulo son v0
    // (re-codificados con h5py). Antes del lector v0, setSignatureRoom(2)
    // fallaba en silencio (loadSofaDataset exigía isMeasured() y el v0 no
    // parseaba): la sala firma no sonaba. Ahora carga el MIT KEMAR v0
    // del módulo y el frontal medido da ITD ~0 por la vía completa
    // (loader v0 -> HrtfManager -> banco -> convolución).
    std::string dir;
    {
        std::filesystem::path cur = std::filesystem::current_path();
        for (int i = 0; i < 10 && !cur.empty(); ++i) {
            const std::filesystem::path cand =
                cur / "magisk_module/system/etc/ivanna_omega/sofa";
            if (std::filesystem::is_directory(cand)) {
                dir = cand.string();
                break;
            }
            if (cur == cur.parent_path()) break;
            cur = cur.parent_path();
        }
    }
    if (dir.empty())
        GTEST_SKIP() << "sin dir de SOFA del módulo (repo no a la vista)";
    auto h = mkSofaHrtf();
    h->setSofaSearchDir(dir.c_str());
    ASSERT_TRUE(h->setSignatureRoom(2)) << "MIT KEMAR v0 del módulo no cargó";
    EXPECT_EQ(h->signatureRoom(), 2);
    EXPECT_TRUE(h->hasSofaDataset());
    h->setHeadPose(0.0f, 0.0f, 0.0f);
    const SofaIr4 ir = sofaImpulseResponse4(*h);
    const SofaItd m = sofaMeasureItd(ir);
    sofaPrintItd("MIT-v0-modulo", 0.0f, m);
    EXPECT_LE(std::fabs(m.xcorrUs), 50.0)
        << "ITD a 0° con el SOFA v0 del módulo: el frontal medido es simétrico";
    EXPECT_LE(std::fabs(m.centroidUs), 50.0)
        << "ITD por centroide a 0° con el SOFA v0 del módulo";
}
