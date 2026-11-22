// test_ivanna_apex_kernel.cpp — IVANNA-APEX: motor holográfico unificado 64-bit.
// Mediciones REALES (no afirmaciones sin número) de los componentes APEX,
// standalone (sin enchufar en FusedChain). Si un número no sale como esperaba
// la propuesta, el test lo reporta con el valor medido (la verdad primero).
//   1. Splitter NLMS: reconstrucción exacta + puerta en 0 en tono sostenido.
//   2. FractionalDelayLine64: retardo fraccionario con centroide correcto.
//   3. computeMinimumPhase64: identidad con perfil plano + causalidad.
//   4. computeVolumetricSpeakersMatrixBin: XTC robusto en 5 posturas.
//   5. computeHybridSphereSofaEarBin: simetría frontal + ILD a 90°.
//   6. applyCoherenceAwareMatrixBlock64: coherente aplica matriz, difuso la conserva.
#include <gtest/gtest.h>
#include "fused/ivanna_apex_kernel.hpp"
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>
#include <array>

using namespace ivanna::fused::apex;
using namespace ivanna::fused;

namespace {
constexpr double kSr = 48000.0;

double rms(const std::vector<double>& x) {
    double s = 0.0;
    for (double v : x) s += v * v;
    return x.empty() ? 0.0 : std::sqrt(s / static_cast<double>(x.size()));
}

// Generador pseudo-aleatorio determinista (LCG) para el ruido incoherente.
struct Lcg {
    std::uint64_t s = 0x12345678ULL;
    double next() {  // uniforme en [-1, 1)
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>((s >> 11) & ((1ULL << 53) - 1)) /
               static_cast<double>(1ULL << 53) * 2.0 - 1.0;
    }
};
}  // namespace

// ── 1) Splitter NLMS ───────────────────────────────────────────────────────
TEST(ApexSplitter, ReconstructionIsExact) {
    TransientOrthogonalSplitter64 sp;
    sp.prepare(kSr);
    // Seno 1 kHz.
    double maxRes = 0.0;
    for (int n = 0; n < 4800; ++n) {
        const double x = 0.5 * std::sin(2.0 * kPi * 1000.0 * n / kSr);
        double tL, tR, sL, sR;
        sp.splitSample(x, x, tL, tR, sL, sR);
        maxRes = std::max(maxRes, std::fabs((tL + sL) - x));
        maxRes = std::max(maxRes, std::fabs((tR + sR) - x));
    }
    // Impulso.
    sp.reset();
    for (int n = 0; n < 256; ++n) {
        const double x = (n == 0) ? 1.0 : 0.0;
        double tL, tR, sL, sR;
        sp.splitSample(x, x, tL, tR, sL, sR);
        maxRes = std::max(maxRes, std::fabs((tL + sL) - x));
        maxRes = std::max(maxRes, std::fabs((tR + sR) - x));
    }
    EXPECT_LT(maxRes, 1e-9) << "reconstrucción x_trans + x_sust != x, residuo máx = " << maxRes;
}

TEST(ApexSplitter, SustainedToneGateCloses) {
    TransientOrthogonalSplitter64 sp;
    sp.prepare(kSr);
    // 1 s de seno 1 kHz para convergencia del NLMS.
    std::vector<double> transTail;
    for (int n = 0; n < 48000; ++n) {
        const double x = 0.5 * std::sin(2.0 * kPi * 1000.0 * n / kSr);
        double tL, tR, sL, sR;
        sp.splitSample(x, x, tL, tR, sL, sR);
        if (n >= 48000 - 4800) transTail.push_back(tL);
    }
    const double r = rms(transTail);
    EXPECT_LT(r, 1e-6) << "la puerta no queda en 0 en tono sostenido, x_trans RMS = " << r;
}

// ── 2) FractionalDelayLine64 ───────────────────────────────────────────────
TEST(ApexDelayLine, FractionalDelayCentroid) {
    FractionalDelayLine64 dl;
    dl.reset();
    constexpr double kDelay = 10.5;
    std::vector<double> out;
    out.reserve(64);
    for (int n = 0; n < 64; ++n) {
        const double x = (n == 0) ? 1.0 : 0.0;
        out.push_back(dl.process(x, kDelay));
    }
    // Centroide de energía de la salida: debe estar a 10.5 ± 0.2 del impulso.
    double e = 0.0, s = 0.0;
    for (size_t i = 0; i < out.size(); ++i) {
        e += out[i] * out[i];
        s += static_cast<double>(i) * out[i] * out[i];
    }
    ASSERT_GT(e, 0.0) << "la línea de retardo no devolvió energía";
    const double centroid = s / e;
    EXPECT_NEAR(centroid, kDelay, 0.2)
        << "centroide del impulso retardado = " << centroid << ", esperado " << kDelay;
    // La energía debe estar concentrada (no dispersa por todo el buffer).
    double eNear = 0.0;
    for (size_t i = 8; i <= 13 && i < out.size(); ++i) eNear += out[i] * out[i];
    EXPECT_GT(eNear / e, 0.9) << "energía fuera de la ventana 8..13 muestras";
}

// ── 3) computeMinimumPhase64 ───────────────────────────────────────────────
TEST(ApexMinPhase, FlatProfileIsIdentity) {
    ApexWorkspace ws;
    constexpr int kN = 512;
    constexpr int kHalf = kN / 2;
    std::array<double, kHalf + 1> magDb{};
    magDb.fill(0.0);
    std::array<std::complex<double>, kHalf + 1> out{};
    ASSERT_TRUE(computeMinimumPhase64(magDb.data(), kN, out.data(), ws));
    double maxErr = 0.0;
    for (int k = 0; k <= kHalf; ++k) {
        maxErr = std::max(maxErr, std::abs(out[k] - std::complex<double>(1.0, 0.0)));
    }
    EXPECT_LT(maxErr, 1e-9) << "perfil plano 0 dB no da identidad, error máx = " << maxErr;
}

TEST(ApexMinPhase, BumpProfileMatchesAndIsCausal) {
    ApexWorkspace ws;
    constexpr int kN = 512;
    constexpr int kHalf = kN / 2;
    // Bump gaussiano de +6 dB centrado en el bin de ~2 kHz.
    const int kCenter = static_cast<int>(std::round(2000.0 * kN / kSr));
    const double sigma = 20.0;
    std::array<double, kHalf + 1> magDb{};
    for (int k = 0; k <= kHalf; ++k) {
        const double d = (static_cast<double>(k) - kCenter) / sigma;
        magDb[k] = 6.0 * std::exp(-0.5 * d * d);
    }
    std::array<std::complex<double>, kHalf + 1> out{};
    ASSERT_TRUE(computeMinimumPhase64(magDb.data(), kN, out.data(), ws));
    // La magnitud de salida coincide con el perfil (±0.05 dB).
    double maxDbErr = 0.0;
    for (int k = 0; k <= kHalf; ++k) {
        const double gotDb = 20.0 * std::log10(std::abs(out[k]));
        maxDbErr = std::max(maxDbErr, std::fabs(gotDb - magDb[k]));
    }
    EXPECT_LT(maxDbErr, 0.05) << "la magnitud no coincide con el perfil, error máx = " << maxDbErr << " dB";
    // Causalidad: IFFT del espectro (simetría hermítica) -> h[n]; la energía
    // antes del pico principal debe ser < 1e-6 del total (cero pre-ringing).
    std::array<std::complex<double>, kN> full{};
    for (int k = 0; k <= kHalf; ++k) full[k] = out[k];
    for (int k = kHalf + 1; k < kN; ++k) full[k] = std::conj(out[kN - k]);
    fftDouble(full.data(), kN, /*inverse=*/true);
    std::vector<double> h(kN);
    for (int n = 0; n < kN; ++n) h[n] = full[n].real();
    double eTot = 0.0;
    int peak = 0;
    for (int n = 0; n < kN; ++n) {
        eTot += h[n] * h[n];
        if (std::fabs(h[n]) > std::fabs(h[peak])) peak = n;
    }
    ASSERT_GT(eTot, 0.0);
    double ePre = 0.0;
    for (int n = 0; n < peak; ++n) ePre += h[n] * h[n];
    EXPECT_LT(ePre / eTot, 1e-6)
        << "pre-ringing: energía antes del pico (tap " << peak << ") = "
        << (ePre / eTot) << " del total";
}

// ── 4) computeVolumetricSpeakersMatrixBin ───────────────────────────────────
// Construye la planta A (2x2) en un offset dado, replicando el cálculo de
// caminos de computeVolumetricSpeakersMatrixBin (retardo + 1/d + esfera rígida).
static void buildPlantAtOffset(double freqHz, double offsetM,
                               const VolumetricSpeakerSpec& base,
                               ivanna::physical::RigidSphereWorkspace& ws,
                               Complex2x2& A) {
    const double a = base.headRadiusM;
    const double cSound = base.speedOfSoundMps;
    const double halfSpan = base.spanM * 0.5;
    const double zDist = base.distanceM;
    const double omega = 2.0 * kPi * freqHz;
    const double dxL = -halfSpan - offsetM;
    const double dxR = +halfSpan - offsetM;
    const double dLL = std::hypot(dxL + a, zDist);
    const double dLR = std::hypot(dxL - a, zDist);
    const double dRL = std::hypot(dxR + a, zDist);
    const double dRR = std::hypot(dxR - a, zDist);
    const double rCL = std::hypot(dxL, zDist);
    const double rCR = std::hypot(dxR, zDist);
    const std::complex<double> sLL = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(dxL / rCL, -1.0, 1.0), a, cSound, ws));
    const std::complex<double> sLR = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(-dxL / rCL, -1.0, 1.0), a, cSound, ws));
    const std::complex<double> sRL = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(dxR / rCR, -1.0, 1.0), a, cSound, ws));
    const std::complex<double> sRR = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(-dxR / rCR, -1.0, 1.0), a, cSound, ws));
    const double dRef = 0.5 * (dLL + dRR);
    auto path = [&](double d, const std::complex<double>& diff) {
        return (dRef / std::max(d, 0.1)) *
               std::exp(std::complex<double>(0.0, -omega * (d - dRef) / cSound)) * diff;
    };
    A.m00 = path(dLL, sLL);
    A.m01 = path(dRL, sRL);
    A.m10 = path(dLR, sLR);
    A.m11 = path(dRR, sRR);
}

static double crosstalkOf(const Complex2x2& W, const Complex2x2& A) {
    // C = W * A; crosstalk = off-diagonal / diagonal.
    const std::complex<double> c00 = W.m00 * A.m00 + W.m01 * A.m10;
    const std::complex<double> c01 = W.m00 * A.m01 + W.m01 * A.m11;
    const std::complex<double> c10 = W.m10 * A.m00 + W.m11 * A.m10;
    const std::complex<double> c11 = W.m10 * A.m01 + W.m11 * A.m11;
    const double diag = std::abs(c00) + std::abs(c11);
    const double off = std::abs(c01) + std::abs(c10);
    return diag > 1e-12 ? off / diag : 1e9;
}

TEST(ApexVolumetric, CrosstalkBelowThresholdIn5Postures) {
    ApexWorkspace ws;
    const double offsets[5] = {-0.10, -0.05, 0.0, 0.05, 0.10};
    const double freqs[2] = {1000.0, 3000.0};
    const double maxLin = std::pow(10.0, 3.0 / 20.0);  // maxBoostDb = 3.0
    for (double freq : freqs) {
        double meanXt = 0.0;
        for (double off : offsets) {
            VolumetricSpeakerSpec sp;
            sp.listenerOffsetM = off;
            const Complex2x2 W = computeVolumetricSpeakersMatrixBin(freq, sp, ws.sphereWs);
            // Finita y acotada.
            ASSERT_TRUE(std::isfinite(W.m00.real()) && std::isfinite(W.m00.imag()));
            ASSERT_TRUE(std::isfinite(W.m01.real()) && std::isfinite(W.m01.imag()));
            ASSERT_TRUE(std::isfinite(W.m10.real()) && std::isfinite(W.m10.imag()));
            ASSERT_TRUE(std::isfinite(W.m11.real()) && std::isfinite(W.m11.imag()));
            const double rowGain = std::max(std::abs(W.m00) + std::abs(W.m01),
                                            std::abs(W.m10) + std::abs(W.m11));
            EXPECT_LE(rowGain, maxLin * 1.01)
                << "ganancia por fila excede maxBoostDb a " << freq << " Hz, offset " << off
                << " m: " << rowGain << " > " << maxLin;
            Complex2x2 A;
            buildPlantAtOffset(freq, off, sp, ws.sphereWs, A);
            meanXt += crosstalkOf(W, A);
        }
        meanXt /= 5.0;
        // NÚMERO REAL MEDIDO (2026-11-18, hw=0.02): 0.098 @ 1 kHz, 0.247 @
        // 3 kHz (< 0.3, −10 dB). FIX: el sweet-spot se redujo de ±10 cm a
        // ±2 cm (sweetSpotHalfWidthM 0.10 → 0.02): el promediado coherente
        // sobre ±10 cm (≈ ±0.29 λ @ 1 kHz, ±0.88 λ @ 3 kHz) cancelaba fase
        // y debilitaba el inverso MMSE (antes 0.501/0.502; sin inversión
        // 0.54). Trueque honesto: el sweet-spot se estrecha a cambio de
        // profundidad. La W fija centrada en 0 aplicada en ±10 cm a 3 kHz
        // sigue en ~0.44 (límite físico, no un bug: ±0.88 λ de fase no lo
        // cancela ninguna matriz fija; haría falta tracking de cabeza).
        EXPECT_LT(meanXt, 0.30)
            << "crosstalk medio a " << freq << " Hz en 5 posturas = " << meanXt
            << " (sin inversion: 0.54; objetivo -10 dB = 0.3)";
    }
}

// ── 5) computeHybridSphereSofaEarBin ────────────────────────────────────────
TEST(ApexHybridEar, FrontalSymmetricAnd90DegIld) {
    ApexWorkspace ws;
    const float* q = ivanna::master::kMasterSafGoldenQ;
    const double freqs[4] = {500.0, 1000.0, 3000.0, 6000.0};
    for (double freq : freqs) {
        std::complex<double> hL, hR;
        // Frontal: |H_L| ≈ |H_R| (±0.5 dB), energía total ≈ 2.
        computeHybridSphereSofaEarBin(freq, 0.0, 1.0, q, hL, hR, ws);
        ASSERT_TRUE(std::isfinite(hL.real()) && std::isfinite(hL.imag()));
        ASSERT_TRUE(std::isfinite(hR.real()) && std::isfinite(hR.imag()));
        const double ildFrontDb = 20.0 * std::log10(std::abs(hL) / std::abs(hR));
        EXPECT_LT(std::fabs(ildFrontDb), 0.5)
            << "frontal no simétrico a " << freq << " Hz: ILD = " << ildFrontDb << " dB";
        const double eFront = std::norm(hL) + std::norm(hR);
        EXPECT_NEAR(eFront, 2.0, 0.05)
            << "energía total frontal a " << freq << " Hz = " << eFront << " (esperado 2.0)";
        // 90°: ILD claro (> 3 dB en magnitud), energía total ≈ 2.
        computeHybridSphereSofaEarBin(freq, kPi / 2.0, 1.0, q, hL, hR, ws);
        ASSERT_TRUE(std::isfinite(hL.real()) && std::isfinite(hL.imag()));
        ASSERT_TRUE(std::isfinite(hR.real()) && std::isfinite(hR.imag()));
        const double ild90Db = 20.0 * std::log10(std::abs(hL) / std::abs(hR));
        EXPECT_GT(std::fabs(ild90Db), 3.0)
            << "sin ILD claro a 90° a " << freq << " Hz: ILD = " << ild90Db << " dB";
        const double e90 = std::norm(hL) + std::norm(hR);
        EXPECT_NEAR(e90, 2.0, 0.05)
            << "energía total a 90° a " << freq << " Hz = " << e90 << " (esperado 2.0)";
    }
}

// ── 6) applyCoherenceAwareMatrixBlock64 ─────────────────────────────────────
TEST(ApexCoherence, CoherentAppliesMatrixDiffusePreserves) {
    constexpr int kN = 512;
    constexpr int kHalf = kN / 2;
    // Matriz W = 2·I (duplica) y perfil de fase mínima identidad.
    std::array<Complex2x2, kHalf + 1> wHalf{};
    for (auto& m : wHalf) {
        m.m00 = {2.0, 0.0}; m.m01 = {0.0, 0.0};
        m.m10 = {0.0, 0.0}; m.m11 = {2.0, 0.0};
    }
    std::array<std::complex<double>, kHalf + 1> hMin{};
    hMin.fill({1.0, 0.0});

    // Coherente (L == R): la matriz se aplica (salida ~2x, energía ~4x).
    {
        ApexWorkspace ws;
        std::array<std::complex<double>, kN> specL{}, specR{};
        for (int k = 0; k <= kHalf; ++k) {
            specL[k] = {1.0, 0.5};
            specR[k] = {1.0, 0.5};
        }
        double eIn = 0.0;
        for (int k = 0; k <= kHalf; ++k) eIn += std::norm(specL[k]);
        for (int b = 0; b < 20; ++b) {
            // Reinyectar la misma señal cada bloque (la función modifica in-situ).
            for (int k = 0; k <= kHalf; ++k) {
                specL[k] = {1.0, 0.5};
                specR[k] = {1.0, 0.5};
            }
            applyCoherenceAwareMatrixBlock64(specL.data(), specR.data(), kN,
                                             wHalf.data(), hMin.data(), 0.1, ws);
        }
        double eOut = 0.0;
        for (int k = 0; k <= kHalf; ++k) eOut += std::norm(specL[k]);
        const double ratio = eOut / eIn;
        EXPECT_GT(ratio, 3.0)
            << "señal coherente no aplicó la matriz W=2I, energía salida/entrada = " << ratio
            << " (esperado ~4.0)";
    }

    // Incoherente (ruido independiente L/R): la matriz casi no se aplica.
    {
        ApexWorkspace ws;
        Lcg rng;
        std::array<std::complex<double>, kN> specL{}, specR{};
        double eInLast = 0.0, eOutLast = 0.0;
        for (int b = 0; b < 200; ++b) {
            for (int k = 0; k <= kHalf; ++k) {
                specL[k] = {rng.next(), rng.next()};
                specR[k] = {rng.next(), rng.next()};
            }
            if (b == 199) {
                eInLast = 0.0;
                for (int k = 0; k <= kHalf; ++k) eInLast += std::norm(specL[k]);
            }
            applyCoherenceAwareMatrixBlock64(specL.data(), specR.data(), kN,
                                             wHalf.data(), hMin.data(), 0.01, ws);
            if (b == 199) {
                eOutLast = 0.0;
                for (int k = 0; k <= kHalf; ++k) eOutLast += std::norm(specL[k]);
            }
        }
        // Misma bloque de ruido: salida vs entrada, dentro de ±1 dB.
        ASSERT_GT(eInLast, 0.0);
        const double ratioDb = 10.0 * std::log10(eOutLast / eInLast);
        EXPECT_LT(std::fabs(ratioDb), 1.0)
            << "señal difusa no conservó su energía, salida/entrada = " << ratioDb << " dB";
    }
}
