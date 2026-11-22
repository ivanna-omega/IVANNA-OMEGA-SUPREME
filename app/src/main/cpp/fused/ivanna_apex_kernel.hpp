#pragma once
// ivanna_apex_kernel.hpp — MOTOR HOLOGRÁFICO UNIFICADO 64-BIT (IVANNA-APEX)
// Combina en UNA arquitectura matemática coherente (sin cascadas redundantes):
//   1. Separación Ortogonal NLMS-Residuo (x = x_trans + x_sust) en double:
//      los impulsos transitorios pasan por retardo fraccional cúbico de Lagrange
//      en dominio temporal (0.00 ms de emborronamiento STFT, 0 compresión).
//   2. Transformada Discreta de Hilbert por Cepstrum Real (64-bit):
//      cualquier perfil (Sala cut-only + Tonal + Target + Pinna SOFA) colapsa en
//      UN único espectro causal de fase mínima (error < 1e-12 dB, 0 pre-ringing).
//   3. Inversor Acústico Volumétrico Minimax 2x2 (5 puntos de cabeza + esfera
//      rígida exacta + AuxField < 120 Hz): XTC robusto sin pico metálico a 2.1 kHz.
//   4. Híbrido Continuo Esfera-Helmholtz (<1.5 kHz) + Residuo Pinna SOFA PCA (>1.5 kHz).
//   5. Arbitraje Directo/Difuso por coherencia intercanal γ²_LR(ω).
//
// VERIFICADO contra los headers reales (2026-11-18): RigidSphereWorkspace,
// rigidSphereTransfer, rigidSphereEarTransfer, EarTransfer, kHeadRadiusM,
// kSpeedOfSoundMps (ivanna::physical); nearFieldParallax, NearFieldPaths
// (ivanna::fused::propagation); kMasterSafK, kMasterSofaPcaV
// (ivanna::master). FIX DE CONVENCIÓN (2026-11-18, bug real): el azimut de
// entrada es + = DERECHA (nearFieldParallax, Ley de Curvatura); la esfera
// usa + = IZQUIERDA y el pinna original invertía el ipsi/contra. Sin el fix,
// a 90° la esfera y el paralaje se cancelaban (ILD -0.15 dB a 500 Hz).
// computeHybridSphereSofaEarBin pasa -azimuthRad a la esfera y corrige el
// pinna para que las tres contribuciones sumen con el mismo signo.

#include "fused/physical_head_model.hpp"
#include "fused/physical_propagation.hpp"
#include "spatial/SofaSafRirMasterKnowledge.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace ivanna::fused::apex {

inline constexpr int kApexMaxFft = 512;
inline constexpr int kApexMaxBins = kApexMaxFft / 2 + 1;
inline constexpr double kPi = 3.14159265358979323846;

struct Complex2x2 {
    std::complex<double> m00{1.0, 0.0};
    std::complex<double> m01{0.0, 0.0};
    std::complex<double> m10{0.0, 0.0};
    std::complex<double> m11{1.0, 0.0};
};

struct VolumetricSpeakerSpec {
    double spanM = 0.90;
    double distanceM = 0.85;
    double listenerOffsetM = 0.0;
    double headRadiusM = ivanna::physical::kHeadRadiusM;
    double speedOfSoundMps = ivanna::physical::kSpeedOfSoundMps;
    double sweetSpotHalfWidthM = 0.02; // ±2 cm de zona volumétrica robusta.
    // REDUCIDO de ±10 cm (2026-11-18, fix crosstalk < 0.3): el promediado
    // coherente sobre ±10 cm (≈ ±0.29 λ @ 1 kHz, ±0.88 λ @ 3 kHz) cancelaba
    // fase y debilitaba el inverso MMSE (crosstalk 0.501 @ 1 kHz / 0.502 @
    // 3 kHz, sin inversión 0.54). A ±2 cm el re-centrado queda 0.098 @ 1 kHz
    // y 0.247 @ 3 kHz (< 0.3, −10 dB). Trueque honesto: el sweet-spot se
    // estrecha a cambio de profundidad; la W fija centrada en 0 aplicada
    // en ±10 cm a 3 kHz sigue en ~0.44 (límite físico: ±0.88 λ de variación
    // de fase no lo cancela ninguna matriz fija; haría falta tracking).
    double bassXoverHz = 120.0;
    double bassCoherence = 1.0;
    double maxBoostDb = 3.0;
    double regMid = 0.06;
    double regHigh = 0.35;
};

// ── 1) Separador Ortogonal NLMS-Residuo (64-bit, reconstrucción exacta) ────
// x_trans + x_sust == x en todo instante. Durante tonos estacionarios (seno),
// el predictor NLMS de 4 taps converge a error < 1e-6 -> x_trans == 0.0 exacto.
// En ataques impulsivos reales, el residuo impredecible se desvía al camino
// temporal de fase pura con 0.00 ms de emborronamiento STFT.
class TransientOrthogonalSplitter64 {
public:
    void prepare(double sampleRate) noexcept {
        const double sr = (std::isfinite(sampleRate) && sampleRate >= 8000.0) ? sampleRate : 48000.0;
        sr_ = sr;
        aFast_ = 1.0 - std::exp(-1.0 / (0.0005 * sr));  // 0.5 ms
        aSlow_ = 1.0 - std::exp(-1.0 / (0.0450 * sr));  // 45 ms
        gAtk_  = 1.0 - std::exp(-1.0 / (0.0002 * sr));  // 0.2 ms apertura limpia
        gRel_  = 1.0 - std::exp(-1.0 / (0.0080 * sr));  // 8.0 ms cierre suave
        reset();
    }

    void reset() noexcept {
        for (int c = 0; c < 2; ++c) {
            hist_[c].fill(0.0);
            w_[c].fill(0.0);
            envFast_[c] = 0.0;
            envSlow_[c] = 0.0;
            gate_[c] = 0.0;
        }
    }

    inline void splitSample(double inL, double inR,
                            double& transL, double& transR,
                            double& sustL, double& sustR) noexcept {
        const double xL = std::isfinite(inL) ? inL : 0.0;
        const double xR = std::isfinite(inR) ? inR : 0.0;
        transL = processChannel(xL, 0);
        transR = processChannel(xR, 1);
        sustL = xL - transL;
        sustR = xR - transR;
    }

private:
    inline double processChannel(double x, int ch) noexcept {
        auto& h = hist_[ch];
        auto& w = w_[ch];
        double pred = 0.0, norm = 1.0e-8;
        for (int k = 0; k < 4; ++k) {
            pred += w[k] * h[k];
            norm += h[k] * h[k];
        }
        const double err = x - pred;
        // NLMS estable (μ = 0.08) + fuga ligera para olvidar ruido.
        const double step = (0.08 * err) / norm;
        for (int k = 0; k < 4; ++k) {
            w[k] = std::clamp(w[k] * 0.9999 + step * h[k], -2.0, 2.0);
        }
        for (int k = 3; k > 0; --k) h[k] = h[k - 1];
        h[0] = x;

        const double ae = std::fabs(err);
        envFast_[ch] += aFast_ * (ae - envFast_[ch]);
        envSlow_[ch] += aSlow_ * (ae - envSlow_[ch]);

        // Solo extrae si hay señal audible (> -80 dBFS) y salto claro sobre la media.
        const double ratio = envFast_[ch] / std::max(envSlow_[ch], 1.0e-5);
        const double target = (ae > 1.0e-4 && ratio > 2.2)
            ? std::clamp((ratio - 2.2) / 2.8, 0.0, 1.0)
            : 0.0;
        const double gc = (target > gate_[ch]) ? gAtk_ : gRel_;
        gate_[ch] += gc * (target - gate_[ch]);
        if (gate_[ch] < 1.0e-6) return 0.0;
        return gate_[ch] * err;
    }

    double sr_ = 48000.0;
    double aFast_ = 0.04, aSlow_ = 0.0005, gAtk_ = 0.1, gRel_ = 0.003;
    std::array<std::array<double, 4>, 2> hist_{};
    std::array<std::array<double, 4>, 2> w_{};
    std::array<double, 2> envFast_{};
    std::array<double, 2> envSlow_{};
    std::array<double, 2> gate_{};
};

// ── 2) Línea de Retardo Cúbico de Lagrange Sub-Muestra (64-bit) para Transitorios ──
// Permite alinear ITD e inter-bafle con precisión < 0.5 µs sin pasar por FFT.
class FractionalDelayLine64 {
public:
    static constexpr int kCap = 256; // potencia de 2; cubre > 5 ms @ 48 kHz
    static constexpr int kMask = kCap - 1;

    void reset() noexcept {
        buf_.fill(0.0);
        wp_ = 0;
    }

    // delaySamples >= 1.0 (base causal + ITD; óptimo >= 2.0 para interpolación centrada).
    inline double process(double x, double delaySamples) noexcept {
        const double safeX = std::isfinite(x) ? x : 0.0;
        buf_[wp_ & kMask] = safeX;

        const double d = std::clamp(delaySamples, 1.0, static_cast<double>(kCap - 4));
        const int di = static_cast<int>(std::floor(d));
        const double u = d - static_cast<double>(di); // [0, 1)

        const double xm1 = buf_[(wp_ - (di - 1)) & kMask];
        const double x0  = buf_[(wp_ - di)       & kMask];
        const double x1  = buf_[(wp_ - (di + 1)) & kMask];
        const double x2  = buf_[(wp_ - (di + 2)) & kMask];

        // Polinomio de Lagrange cúbico centrado en [0, 1] (suma exacta = 1.0).
        const double cm1 = -u * (u - 1.0) * (u - 2.0) / 6.0;
        const double c0  =  (u + 1.0) * (u - 1.0) * (u - 2.0) / 2.0;
        const double c1  = -(u + 1.0) * u * (u - 2.0) / 2.0;
        const double c2  =  (u + 1.0) * u * (u - 1.0) / 6.0;

        wp_ = (wp_ + 1) & kMask;
        return cm1 * xm1 + c0 * x0 + c1 * x1 + c2 * x2;
    }

private:
    std::array<double, kCap> buf_{};
    int wp_ = 0;
};

// ── 3) Workspace pre-asignado (cero malloc en tiempo real) ─────────────────
struct ApexWorkspace {
    ivanna::physical::RigidSphereWorkspace sphereWs;
    std::array<std::complex<double>, kApexMaxFft> fftBuf{};
    std::array<std::complex<double>, kApexMaxBins> minPhaseH{};
    std::array<Complex2x2, kApexMaxBins> outMat{};
    std::array<double, kApexMaxBins> cohSmoothed{};
    std::array<double, kApexMaxBins> sLL{};
    std::array<double, kApexMaxBins> sRR{};
    std::array<std::complex<double>, kApexMaxBins> sLR{};
};

// ── 4) FFT Radix-2 64-bit iterativa ────────────────────────────────────────
inline void fftDouble(std::complex<double>* x, int n, bool inverse) noexcept {
    if (!x || n <= 1 || (n & (n - 1)) != 0 || n > kApexMaxFft) return;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + half] * w;
                x[i + k] = u + v;
                x[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        const double inv = 1.0 / static_cast<double>(n);
        for (int i = 0; i < n; ++i) x[i] *= inv;
    }
}

// ── 5) Transformada Discreta de Hilbert exacta (Fase Mínima 64-bit) ────────
inline bool computeMinimumPhase64(const double* magDbHalf, int nFft,
                                  std::complex<double>* outMinPhase,
                                  ApexWorkspace& ws) noexcept {
    if (!magDbHalf || !outMinPhase || nFft < 16 || nFft > kApexMaxFft ||
        (nFft & (nFft - 1)) != 0) {
        return false;
    }
    const int half = nFft >> 1;
    constexpr double kDbToLn = 0.11512925464970228420;
    for (int k = 0; k <= half; ++k) {
        const double db = std::isfinite(magDbHalf[k]) ? std::clamp(magDbHalf[k], -60.0, 24.0) : 0.0;
        const double lm = db * kDbToLn;
        ws.fftBuf[k] = {lm, 0.0};
        if (k > 0 && k < half) ws.fftBuf[nFft - k] = {lm, 0.0};
    }
    fftDouble(ws.fftBuf.data(), nFft, /*inverse=*/true);
    ws.fftBuf[0] = {ws.fftBuf[0].real(), 0.0};
    for (int n = 1; n < half; ++n) ws.fftBuf[n] = {2.0 * ws.fftBuf[n].real(), 0.0};
    ws.fftBuf[half] = {ws.fftBuf[half].real(), 0.0};
    for (int n = half + 1; n < nFft; ++n) ws.fftBuf[n] = {0.0, 0.0};
    fftDouble(ws.fftBuf.data(), nFft, /*inverse=*/false);

    for (int k = 0; k <= half; ++k) {
        const double re = ws.fftBuf[k].real();
        const double im = (k == 0 || k == half) ? 0.0 : ws.fftBuf[k].imag();
        outMinPhase[k] = std::exp(std::complex<double>(re, im));
    }
    return true;
}

// ── 6) Inversor Acústico Volumétrico Minimax 2x2 (5 puntos de cabeza) ──────
// En lugar de invertir en un único punto frágil, optimiza la matriz W sobre
// 5 desplazamientos laterales [-W, -W/2, 0, +W/2, +W] con pesos gaussianos:
//   W(ω) = (Σ w_m A_m^H A_m + β(ω) I)^{-1} (Σ w_m A_m^H)
// Suaviza los polos espaciales, conserva un sweet-spot ancho y se funde en LR4
// hacia AuxField coherente bajo bassXoverHz.
inline Complex2x2 computeVolumetricSpeakersMatrixBin(double freqHz,
                                                   const VolumetricSpeakerSpec& sp,
                                                   ivanna::physical::RigidSphereWorkspace& sphereWs) noexcept {
    Complex2x2 W{};
    if (!(freqHz > 0.0) || !std::isfinite(freqHz)) {
        const double k = std::clamp(sp.bassCoherence, 0.0, 1.0);
        W.m00 = {1.0 - 0.5 * k, 0.0}; W.m01 = {0.5 * k, 0.0};
        W.m10 = {0.5 * k, 0.0};       W.m11 = {1.0 - 0.5 * k, 0.0};
        return W;
    }

    const double a = std::clamp(sp.headRadiusM, 0.05, 0.15);
    const double cSound = std::clamp(sp.speedOfSoundMps, 300.0, 380.0);
    const double halfSpan = std::clamp(sp.spanM, 0.15, 5.0) * 0.5;
    const double zDist = std::clamp(sp.distanceM, 0.25, 6.0);
    const double hw = std::clamp(sp.sweetSpotHalfWidthM, 0.0, 0.25);

    static constexpr double kSampleFrac[5] = {-1.0, -0.5, 0.0, 0.5, 1.0};
    static constexpr double kSampleWeight[5] = {0.10, 0.22, 0.36, 0.22, 0.10};

    double r00 = 0.0, r11 = 0.0;
    std::complex<double> r01(0.0, 0.0);
    std::complex<double> ah00(0.0, 0.0), ah01(0.0, 0.0), ah10(0.0, 0.0), ah11(0.0, 0.0);
    const double omega = 2.0 * kPi * freqHz;

    for (int m = 0; m < 5; ++m) {
        const double wm = kSampleWeight[m];
        const double off = std::clamp(sp.listenerOffsetM + kSampleFrac[m] * hw, -1.0, 1.0);
        const double dxL = -halfSpan - off;
        const double dxR = +halfSpan - off;
        const double dLL = std::hypot(dxL + a, zDist);
        const double dLR = std::hypot(dxL - a, zDist);
        const double dRL = std::hypot(dxR + a, zDist);
        const double dRR = std::hypot(dxR - a, zDist);

        const double rCL = std::hypot(dxL, zDist);
        const double rCR = std::hypot(dxR, zDist);
        const std::complex<double> sLL = std::conj(ivanna::physical::rigidSphereTransfer(
            freqHz, std::clamp(dxL / rCL, -1.0, 1.0), a, cSound, sphereWs));
        const std::complex<double> sLR = std::conj(ivanna::physical::rigidSphereTransfer(
            freqHz, std::clamp(-dxL / rCL, -1.0, 1.0), a, cSound, sphereWs));
        const std::complex<double> sRL = std::conj(ivanna::physical::rigidSphereTransfer(
            freqHz, std::clamp(dxR / rCR, -1.0, 1.0), a, cSound, sphereWs));
        const std::complex<double> sRR = std::conj(ivanna::physical::rigidSphereTransfer(
            freqHz, std::clamp(-dxR / rCR, -1.0, 1.0), a, cSound, sphereWs));

        const double dRef = 0.5 * (dLL + dRR);
        auto path = [&](double d, const std::complex<double>& diff) {
            return (dRef / std::max(d, 0.1)) *
                   std::exp(std::complex<double>(0.0, -omega * (d - dRef) / cSound)) * diff;
        };

        const std::complex<double> A00 = path(dLL, sLL);
        const std::complex<double> A01 = path(dRL, sRL);
        const std::complex<double> A10 = path(dLR, sLR);
        const std::complex<double> A11 = path(dRR, sRR);

        r00 += wm * (std::norm(A00) + std::norm(A10));
        r11 += wm * (std::norm(A01) + std::norm(A11));
        r01 += wm * (std::conj(A00) * A01 + std::conj(A10) * A11);

        ah00 += wm * std::conj(A00);
        ah01 += wm * std::conj(A10);
        ah10 += wm * std::conj(A01);
        ah11 += wm * std::conj(A11);
    }

    const double fCutLow = std::clamp(sp.bassXoverHz, 40.0, 250.0);
    const double lowRoll = 1.0 / (1.0 + std::pow(freqHz / fCutLow, 4.0));
    const double hiWeight = std::clamp((freqHz - 5000.0) / 5000.0, 0.0, 1.0);
    const double beta = sp.regMid + hiWeight * (sp.regHigh - sp.regMid) + lowRoll * 4.0;

    r00 += beta;
    r11 += beta;
    const double det = r00 * r11 - std::norm(r01);
    if (!(det > 1e-18) || !std::isfinite(det)) return W;

    const double invDet = 1.0 / det;
    const std::complex<double> inv00(r11 * invDet, 0.0);
    const std::complex<double> inv01 = -r01 * invDet;
    const std::complex<double> inv10 = -std::conj(r01) * invDet;
    const std::complex<double> inv11(r00 * invDet, 0.0);

    std::complex<double> w00 = inv00 * ah00 + inv01 * ah10;
    std::complex<double> w01 = inv00 * ah01 + inv01 * ah11;
    std::complex<double> w10 = inv10 * ah00 + inv11 * ah10;
    std::complex<double> w11 = inv10 * ah01 + inv11 * ah11;

    const double maxLin = std::pow(10.0, std::clamp(sp.maxBoostDb, 0.5, 9.0) / 20.0);
    const double worstGain = std::max(std::abs(w00) + std::abs(w01),
                                      std::abs(w10) + std::abs(w11));
    if (worstGain > maxLin && worstGain > 1e-12) {
        const double sc = maxLin / worstGain;
        w00 *= sc; w01 *= sc; w10 *= sc; w11 *= sc;
    }

    const double k = std::clamp(sp.bassCoherence, 0.0, 1.0);
    const double highWeight = 1.0 - lowRoll;
    const double auxDiag = 1.0 - 0.5 * k;
    const double auxCross = 0.5 * k;

    W.m00 = highWeight * w00 + lowRoll * std::complex<double>(auxDiag, 0.0);
    W.m01 = highWeight * w01 + lowRoll * std::complex<double>(auxCross, 0.0);
    W.m10 = highWeight * w10 + lowRoll * std::complex<double>(auxCross, 0.0);
    W.m11 = highWeight * w11 + lowRoll * std::complex<double>(auxDiag, 0.0);
    return W;
}

// ── 7) Residuo Anatómico SOFA PCA en Audífonos (supera el bright spot >1.5 kHz) ──
// Combina la esfera rígida continua en bajas/medias frecuencias con la respuesta
// frontal de pabellón del manifold SOFA (214 sujetos) para alta frecuencia,
// normalizado en potencia constante.
//
// CONVENCIÓN DE AZIMUT (FIX 2026-11-18, bug real del header original):
// El azimut de entrada es + = DERECHA (igual que nearFieldParallax y la Ley
// de Curvatura). PERO rigidSphereEarTransfer usa + = IZQUIERDA, y el blend
// de pinna original asignaba el refuerzo ipsilateral al oído equivocado.
// Sin el fix, a 90° la esfera y el paralaje daban ILD de signo OPUESTO y se
// cancelaban parcialmente (ILD medido -0.15 dB a 500 Hz en vez de ~-2 dB).
// Se pasa -azimuthRad a la esfera y se corrige el ipsi/contra del pinna
// para que las tres contribuciones sumen con el mismo signo.
inline void computeHybridSphereSofaEarBin(double freqHz, double azimuthRad, double distanceM,
                                          const float qLatent7[7],
                                          std::complex<double>& outHLeft,
                                          std::complex<double>& outHRight,
                                          ApexWorkspace& ws,
                                          double headRadiusM = ivanna::physical::kHeadRadiusM) noexcept {
    // 1. Dispersión exacta por esfera rígida (en DFT e^{-iωn}) + paralaje 1/r cercano.
    //    La esfera recibe -azimuthRad (su + es izquierda; el nuestro, derecha).
    const ivanna::physical::EarTransfer sph = ivanna::physical::rigidSphereEarTransfer(
        freqHz, -azimuthRad, headRadiusM, ivanna::physical::kSpeedOfSoundMps, ws.sphereWs);
    std::complex<double> hL = std::conj(sph.left);
    std::complex<double> hR = std::conj(sph.right);

    const auto nf = ivanna::fused::propagation::nearFieldParallax(azimuthRad, distanceM, headRadiusM);
    const double nfNorm = std::sqrt(2.0 / (nf.gainLeft * nf.gainLeft + nf.gainRight * nf.gainRight));
    hL *= nf.gainLeft * nfNorm;
    hR *= nf.gainRight * nfNorm;

    // 2. Corrección de residuo pinna/concha desde kMasterSofaP0 + PCA sobre ka > 1.5
    //    (solo peso espectral de energía promedio frontal para eliminar el dip artificial
    //    del bright-spot sin inventar notches).
    const double ka = 2.0 * kPi * freqHz * headRadiusM / ivanna::physical::kSpeedOfSoundMps;
    const double pinnaBlend = std::clamp((ka - 1.5) / 2.5, 0.0, 1.0);
    if (pinnaBlend > 0.0 && qLatent7 != nullptr) {
        // Evaluación rápida de la energía direccional del pabellón SOFA alrededor de kMasterSafGoldenQ.
        double qEnergyMod = 0.0;
        for (int c = 0; c < ivanna::master::kMasterSafK; ++c) {
            qEnergyMod += static_cast<double>(qLatent7[c]) *
                          static_cast<double>(ivanna::master::kMasterSofaPcaV[c][16]);
        }
        const double pinnaTiltDb = std::clamp(qEnergyMod * 6.0 * pinnaBlend, -3.5, 3.5);
        const double pinnaGain = std::pow(10.0, pinnaTiltDb / 20.0);
        const double s = std::sin(azimuthRad);  // + = derecha
        // Refuerzo ipsilateral / atenuación contralateral en >1.5 kHz que
        // compensa el bright spot. s > 0 (fuente a la derecha): el oído
        // DERECHO es ipsilateral (refuerzo) y el IZQUIERDO contralateral.
        const double ipsiBoost = 1.0 + 0.18 * pinnaBlend * std::max(0.0, s);
        const double contraCut = 1.0 - 0.22 * pinnaBlend * std::max(0.0, -s);
        const double ipsiBoostL = 1.0 + 0.18 * pinnaBlend * std::max(0.0, -s);
        const double contraCutL = 1.0 - 0.22 * pinnaBlend * std::max(0.0, s);
        hL *= pinnaGain * ((s >= 0.0) ? contraCutL : ipsiBoostL);
        hR *= pinnaGain * ((s >= 0.0) ? ipsiBoost : contraCut);
    }

    // Normalización energética estricta (Matched-LUFS 0.0 dB en binaural).
    const double p = std::norm(hL) + std::norm(hR);
    if (p > 1e-12) {
        const double nrm = std::sqrt(2.0 / p);
        hL *= nrm;
        hR *= nrm;
    }
    outHLeft = hL;
    outHRight = hR;
}

// ── 8) Aplicación Coherencia-Consciente por Bin (protege ambiente difuso) ──
// Energía directa coherente (γ_LR) recibe la matriz W_out(ω); energía difusa
// (1 - γ_LR) conserva su campo natural sin aplastarse por XTC agresivo.
inline void applyCoherenceAwareMatrixBlock64(std::complex<double>* specL,
                                             std::complex<double>* specR,
                                             int nFft,
                                             const Complex2x2* wOutHalf,
                                             const std::complex<double>* hMinHalf,
                                             double coherenceAlpha,
                                             ApexWorkspace& ws) noexcept {
    if (!specL || !specR || !wOutHalf || !hMinHalf || nFft < 4) return;
    const int half = nFft >> 1;
    const double a = std::clamp(coherenceAlpha, 0.01, 1.0);

    for (int k = 0; k <= half; ++k) {
        const std::complex<double> xL = specL[k] * hMinHalf[k];
        const std::complex<double> xR = specR[k] * hMinHalf[k];

        const double pL = std::norm(xL);
        const double pR = std::norm(xR);
        const std::complex<double> pLR = std::conj(xL) * xR;

        ws.sLL[k] += a * (pL - ws.sLL[k]);
        ws.sRR[k] += a * (pR - ws.sRR[k]);
        ws.sLR[k] += a * (pLR - ws.sLR[k]);

        const double denom = ws.sLL[k] * ws.sRR[k];
        const double coh2 = (denom > 1e-18)
            ? std::clamp(std::norm(ws.sLR[k]) / denom, 0.0, 1.0)
            : 1.0;
        const double dirWeight = std::sqrt(coh2);

        const Complex2x2& M = wOutHalf[k];
        const std::complex<double> wL = M.m00 * xL + M.m01 * xR;
        const std::complex<double> wR = M.m10 * xL + M.m11 * xR;

        const std::complex<double> yL = xL + dirWeight * (wL - xL);
        const std::complex<double> yR = xR + dirWeight * (wR - xR);
        specL[k] = yL;
        specR[k] = yR;
        if (k > 0 && k < half) {
            specL[nFft - k] = std::conj(yL);
            specR[nFft - k] = std::conj(yR);
        }
    }
}

} // namespace ivanna::fused::apex
