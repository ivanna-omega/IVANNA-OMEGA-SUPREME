// test_unified_field_kernel.cpp — Operador Holográfico Único TΩ(ω) 2×2 en
// 64-bit double (IVANNA-APEX v2.6, 2026-11-18). Verifica el kernel en
// standalone ANTES de conectarlo a FusedChain: el kernel NO está conectado
// aún (la cadena que funciona no se toca); estos tests son la puerta de
// entrada para la integración (reemplazar 3 STFT + 30 IIRs + shuffler XTC
// por una sola matriz compleja 64-bit).
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/unified_field_kernel.hpp"
#include "../fused/physical_head_model.hpp"
#include "../fused/physical_propagation.hpp"

namespace {

using namespace ivanna::fused::apex;
using ivanna::physical::RigidSphereWorkspace;

constexpr double kSr = 48000.0;
constexpr int kNfft = 512;
constexpr int kHalf = kNfft / 2;
constexpr double kPiD = 3.14159265358979323846;

// Magnitud en dB por bin: Harman suave + corte de sala (-8 dB @120 Hz) +
// EQ de usuario (+3 dB @8 kHz). Curva suave en log-frecuencia (sin
// discontinuidades: el cepstrum no tolera escalones).
void buildTestMagnitude(double* magDb) {
    for (int k = 0; k <= kHalf; ++k) {
        const double f = (k == 0) ? 20.0 : static_cast<double>(k) * kSr / kNfft;
        const double lf = std::log10(std::max(f, 20.0));
        // Harman suave: +2 dB en graves que cae a 0 dB en 1 kHz, -1 dB en agudos.
        const double harman = 2.0 * std::exp(-std::pow((lf - std::log10(20.0)) / 1.2, 2.0)) -
                              1.0 * std::exp(-std::pow((lf - std::log10(20000.0)) / 0.8, 2.0));
        // Corte de sala: -8 dB gaussiano centrado en 120 Hz (σ 0.35 octavas).
        const double roomCut = -8.0 * std::exp(-std::pow((lf - std::log10(120.0)) / 0.15, 2.0));
        // EQ usuario: +3 dB gaussiano centrado en 8 kHz (σ 0.35 octavas).
        const double userEq = 3.0 * std::exp(-std::pow((lf - std::log10(8000.0)) / 0.15, 2.0));
        magDb[k] = harman + roomCut + userEq;
    }
}

// Planta acústica 2×2 altavoz→oído (replica la lógica de
// computeSpeakersMatrixBin): retardos 1/d + difracción de esfera rígida.
// Sirve para medir la separación REAL (composite A·W), no solo W.
struct PlantBin {
    std::complex<double> a00, a01, a10, a11;
};
PlantBin computePlantBin(double freqHz, const SpeakerPairPhysics& sp,
                         RigidSphereWorkspace& ws) {
    PlantBin A{};
    const double a = std::clamp(sp.headRadiusM, 0.05, 0.15);
    const double cSound = std::clamp(sp.speedOfSoundMps, 300.0, 380.0);
    const double halfSpan = std::clamp(sp.spanM, 0.15, 5.0) * 0.5;
    const double zDist = std::clamp(sp.distanceM, 0.25, 6.0);
    const double offset = std::clamp(sp.listenerOffsetM, -1.0, 1.0);
    const double dxL = -halfSpan - offset;
    const double dxR = +halfSpan - offset;
    const double dLL = std::hypot(dxL + a, zDist);
    const double dLR = std::hypot(dxL - a, zDist);
    const double dRL = std::hypot(dxR + a, zDist);
    const double dRR = std::hypot(dxR - a, zDist);
    const double rCenterL = std::hypot(dxL, zDist);
    const double rCenterR = std::hypot(dxR, zDist);
    const double cgLL = std::clamp(dxL / rCenterL, -1.0, 1.0);
    const double cgLR = std::clamp(-dxL / rCenterL, -1.0, 1.0);
    const double cgRL = std::clamp(dxR / rCenterR, -1.0, 1.0);
    const double cgRR = std::clamp(-dxR / rCenterR, -1.0, 1.0);
    const std::complex<double> sLL = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgLL, a, cSound, ws));
    const std::complex<double> sLR = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgLR, a, cSound, ws));
    const std::complex<double> sRL = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgRL, a, cSound, ws));
    const std::complex<double> sRR = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgRR, a, cSound, ws));
    const double dRef = 0.5 * (dLL + dRR);
    const double omega = 2.0 * kPiD * freqHz;
    auto path = [&](double d, const std::complex<double>& diff) {
        const double mag = dRef / std::max(d, 0.1);
        const double ph = -omega * (d - dRef) / cSound;
        return mag * std::exp(std::complex<double>(0.0, ph)) * diff;
    };
    A.a00 = path(dLL, sLL);
    A.a01 = path(dRL, sRL);
    A.a10 = path(dLR, sLR);
    A.a11 = path(dRR, sRR);
    return A;
}

double db20(double lin) { return 20.0 * std::log10(std::max(lin, 1e-300)); }

}  // namespace

// a) Fase mínima: magnitud exacta (<1e-11 dB) y cero pre-ringing (<-260 dB).
TEST(UnifiedFieldKernel, MinimumPhase) {
    UnifiedFieldWorkspace ws;
    std::array<double, kHalf + 1> magDb{};
    buildTestMagnitude(magDb.data());
    std::array<std::complex<double>, kHalf + 1> minPhase{};
    ASSERT_TRUE(buildMinimumPhaseSpectrum64(magDb.data(), kNfft, minPhase.data(), ws));

    // Magnitud exacta: |H_min[k]| debe ser 10^(dB/20) en cada bin.
    double maxErrDb = 0.0;
    for (int k = 0; k <= kHalf; ++k) {
        const double gotDb = db20(std::abs(minPhase[static_cast<std::size_t>(k)]));
        maxErrDb = std::max(maxErrDb, std::fabs(gotDb - magDb[static_cast<std::size_t>(k)]));
    }
    std::printf("[MinimumPhase] error máximo de magnitud: %.3e dB\n", maxErrDb);
    EXPECT_LT(maxErrDb, 1e-11);

    // Pre-ringing: IFFT del espectro de fase mínima → impulso causal.
    // El pico debe estar en n=0 (o casi) y la energía antes del pico
    // debe ser despreciable (<-260 dB del total).
    std::array<std::complex<double>, kNfft> full{};
    for (int k = 0; k <= kHalf; ++k) full[static_cast<std::size_t>(k)] = minPhase[static_cast<std::size_t>(k)];
    for (int k = 1; k < kHalf; ++k)
        full[static_cast<std::size_t>(kNfft - k)] = std::conj(minPhase[static_cast<std::size_t>(k)]);
    fftRadix2Double(full.data(), kNfft, /*inverse=*/true);
    double totalE = 0.0;
    int peakIdx = 0;
    double peakMag = 0.0;
    for (int n = 0; n < kNfft; ++n) {
        const double re = full[static_cast<std::size_t>(n)].real();
        totalE += re * re;
        if (std::fabs(re) > peakMag) { peakMag = std::fabs(re); peakIdx = n; }
    }
    double preE = 0.0;
    for (int n = 0; n < peakIdx; ++n) {
        const double re = full[static_cast<std::size_t>(n)].real();
        preE += re * re;
    }
    const double preDb = (preE <= 0.0) ? -1e9 : 10.0 * std::log10(preE / totalE);
    std::printf("[MinimumPhase] pico en n=%d, energía pre-pico: %.1f dB\n", peakIdx, preDb);
    EXPECT_LT(preDb, -260.0);
}

// b) Kirkeby XTC 2×2: elimina el peine de 2 kHz y preserva los subgraves.
TEST(UnifiedFieldKernel, KirkebyXtc2x2) {
    SpeakerPairPhysics sp;
    sp.spanM = 0.90;
    sp.distanceM = 0.85;
    RigidSphereWorkspace ws;

    // Subgraves: ganancia mono ~0 dB. F1/472: la renormalización del mono
    // en computeSpeakersMatrixBin (escalar por bin, taper 80→120 Hz)
    // corrige la caída LR4 (−1.113 dB a 80 Hz, −0.389 dB a 60 Hz) y el
    // criterio de la propuesta (±0.05 dB por debajo de 80 Hz) SE CUMPLE
    // ahora en los 4 puntos (antes: ±0.05/±0.10/±0.50/±1.50).
    const double subTol[4] = {0.05, 0.05, 0.05, 0.05};
    int idx = 0;
    for (double f : {20.0, 40.0, 60.0, 80.0}) {
        const ComplexMatrix2x2 W = computeSpeakersMatrixBin(f, sp, ws);
        const double monoL = db20(std::abs(W.m00 + W.m01));
        const double monoR = db20(std::abs(W.m10 + W.m11));
        std::printf("[Kirkeby] f=%.0f Hz monoL=%+.3f dB monoR=%+.3f dB (tol ±%.2f)\n",
                    f, monoL, monoR, subTol[idx]);
        EXPECT_NEAR(monoL, 0.0, subTol[idx]) << "f=" << f << " Hz (mono L)";
        EXPECT_NEAR(monoR, 0.0, subTol[idx]) << "f=" << f << " Hz (mono R)";
        ++idx;
    }

    // 300 Hz–5 kHz: separación interaural >15 dB (composite A·W: la señal
    // del oído deseado supera >15 dB a la del oído opuesto) y ganancia
    // mono ≤+3.0 dB (cota maxBoostDb).
    for (double f : {300.0, 500.0, 1000.0, 2000.0, 3000.0, 5000.0}) {
        const ComplexMatrix2x2 W = computeSpeakersMatrixBin(f, sp, ws);
        const PlantBin A = computePlantBin(f, sp, ws);
        // Composite C = A·W (oídos = A · (W · deseado)).
        const std::complex<double> c00 = A.a00 * W.m00 + A.a01 * W.m10;
        const std::complex<double> c01 = A.a00 * W.m01 + A.a01 * W.m11;
        const std::complex<double> c10 = A.a10 * W.m00 + A.a11 * W.m10;
        const std::complex<double> c11 = A.a10 * W.m01 + A.a11 * W.m11;
        const double sepL = db20(std::abs(c00) / std::max(std::abs(c10), 1e-300));
        const double sepR = db20(std::abs(c11) / std::max(std::abs(c01), 1e-300));
        const double monoL = db20(std::abs(W.m00 + W.m01));
        const double monoR = db20(std::abs(W.m10 + W.m11));
        std::printf("[Kirkeby] f=%.0f Hz sepL=%.1f dB sepR=%.1f dB monoL=%+.2f dB monoR=%+.2f dB\n",
                    f, sepL, sepR, monoL, monoR);
        EXPECT_GT(sepL, 15.0) << "f=" << f << " Hz (separación L)";
        EXPECT_GT(sepR, 15.0) << "f=" << f << " Hz (separación R)";
        EXPECT_LE(monoL, 3.0) << "f=" << f << " Hz (mono L ≤ +3 dB)";
        EXPECT_LE(monoR, 3.0) << "f=" << f << " Hz (mono R ≤ +3 dB)";
    }
}

// c) Simetría hermitiana: salida temporal exactamente real tras IFFT.
TEST(UnifiedFieldKernel, HermitianSymmetry) {
    UnifiedFieldWorkspace ws;
    SpeakerPairPhysics sp;
    sp.spanM = 0.90;
    sp.distanceM = 0.85;

    // Espectros de entrada hermitianos (señal real: tono 1 kHz + ruido).
    // DC (k=0) y Nyquist (k=half) DEBEN ser reales en una señal real.
    std::array<std::complex<double>, kNfft> specL{}, specR{};
    for (int k = 0; k <= kHalf; ++k) {
        const double f = static_cast<double>(k) * kSr / kNfft;
        const double mag = (std::fabs(f - 1000.0) < 200.0) ? 1.0 : 0.05;
        const double ph = 0.7 * static_cast<double>(k);
        std::complex<double> vL = mag * std::exp(std::complex<double>(0.0, ph));
        std::complex<double> vR = 0.8 * mag * std::exp(std::complex<double>(0.0, -ph));
        if (k == 0 || k == kHalf) { vL = {vL.real(), 0.0}; vR = {vR.real(), 0.0}; }
        specL[static_cast<std::size_t>(k)] = vL;
        specR[static_cast<std::size_t>(k)] = vR;
        if (k > 0 && k < kHalf) {
            specL[static_cast<std::size_t>(kNfft - k)] = std::conj(specL[static_cast<std::size_t>(k)]);
            specR[static_cast<std::size_t>(kNfft - k)] = std::conj(specR[static_cast<std::size_t>(k)]);
        }
    }
    // Matriz unificada por bin + fase mínima.
    std::array<double, kHalf + 1> magDb{};
    buildTestMagnitude(magDb.data());
    std::array<std::complex<double>, kHalf + 1> minPhase{};
    ASSERT_TRUE(buildMinimumPhaseSpectrum64(magDb.data(), kNfft, minPhase.data(), ws));
    for (int k = 0; k <= kHalf; ++k) {
        const double f = static_cast<double>(k) * kSr / kNfft;
        ws.outMatrix[static_cast<std::size_t>(k)] = computeSpeakersMatrixBin(f, sp, ws.sphereWs);
    }
    applyUnifiedMatrix2x2(specL.data(), specR.data(), kNfft, ws.outMatrix.data(),
                           minPhase.data());
    // IFFT: la salida temporal debe ser real (imaginario residual <1e-14).
    fftRadix2Double(specL.data(), kNfft, /*inverse=*/true);
    fftRadix2Double(specR.data(), kNfft, /*inverse=*/true);
    double maxIm = 0.0;
    double maxRe = 0.0;
    for (int n = 0; n < kNfft; ++n) {
        maxIm = std::max(maxIm, std::fabs(specL[static_cast<std::size_t>(n)].imag()));
        maxIm = std::max(maxIm, std::fabs(specR[static_cast<std::size_t>(n)].imag()));
        maxRe = std::max(maxRe, std::fabs(specL[static_cast<std::size_t>(n)].real()));
        maxRe = std::max(maxRe, std::fabs(specR[static_cast<std::size_t>(n)].real()));
    }
    std::printf("[Hermitian] imaginario residual máximo: %.3e (real máximo: %.3e)\n", maxIm, maxRe);
    EXPECT_LT(maxIm, 1e-14);
}

// d) Estimador de distancia: resuelve el ITD en graves donde el ILD es cero.
TEST(UnifiedFieldKernel, DistanceEstimator) {
    const double freqHz = 250.0;
    const double azimuthDeg = 85.0;
    const double azimuthRad = azimuthDeg * kPiD / 180.0;
    const double trueDistM = 0.35;
    const double headR = ivanna::physical::kHeadRadiusM;

    // ITD medido (sintético, exacto): ley de curvatura a la distancia real.
    const double measuredItd = ivanna::fused::propagation::curvatureLawItdSeconds(
        azimuthRad, trueDistM, headR);
    // Fallback ILD deliberadamente erróneo (1.0 m): en graves el ILD es
    // ~0, así que el estimador debe apoyarse en el ITD y recuperar 0.35 m.
    // F1/472: el blend confía al 100% en el ITD a |sin θ| ≥ 0.907 (w=1 a
    // 85°), así que la recuperación es exacta y el criterio de la
    // propuesta (0.35 ± 0.01 m) SE CUMPLE (antes: error 0.0113 m por la
    // mezcla del 1.4% hacia el fallback, tolerancia ±0.02 m).
    const double est = estimateDistanceFusedM(freqHz, azimuthRad, measuredItd, 1.0, headR);
    std::printf("[Distance] ITD medido=%.3e s, estimado=%.4f m (real %.2f m, error %.4f m)\n",
                measuredItd, est, trueDistM, std::fabs(est - trueDistM));
    EXPECT_NEAR(est, trueDistM, 0.01);
}
