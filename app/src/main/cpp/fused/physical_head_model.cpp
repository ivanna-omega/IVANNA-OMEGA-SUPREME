// physical_head_model.cpp — dispersión exacta por esfera rígida.
// Ver physical_head_model.hpp para la derivación y las convenciones.
//
// Implementación numérica (doble precisión):
//   jₙ : recurrencia DESCENDENTE de Miller (la ascendente es inestable para
//        n > ka). Se arranca en M = N + 25 con j_{M+1} = 0, j_M = 1, se baja
//        hasta 0 y se normaliza con j₀ = sin(ka)/ka (valor exacto).
//   yₙ : recurrencia ASCENDENTE (estable para y), desde
//        y₀ = −cos(ka)/ka, y₁ = −cos(ka)/ka² − sin(ka)/ka.
//   fₙ′ = f_{n−1} − (n+1)/ka · fₙ (n ≥ 1); f₀′ = −f₁.
//   Pₙ(cos γ) : recurrencia ascendente de Legendre.
//   iⁿ : ciclo (1, i, −1, −i), sin pow().
// La serie converge con N = ceil(ka) + 20 a < 1e-12 (test de convergencia).
#include "fused/physical_head_model.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::physical {
namespace {

constexpr double kPi = 3.14159265358979323846;

// FFT radix-2 iterativa (solo para synthesizeRigidSphereHrir, fuera del RT).
// inverse=true aplica 1/N al final.
void fftRadix2(std::complex<double>* x, std::size_t n, bool inverse) {
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        const double inv = 1.0 / static_cast<double>(n);
        for (std::size_t i = 0; i < n; ++i) x[i] *= inv;
    }
}

}  // namespace

int rigidSphereOrder(double ka) noexcept {
    if (!(ka > 0.0) || !std::isfinite(ka)) return 1;
    const int n = static_cast<int>(std::ceil(ka)) + 20;
    return std::clamp(n, 1, 200);
}

void RigidSphereWorkspace::ensure(std::size_t n) {
    if (jn.size() < n + 1) {
        jn.resize(n + 1);
        yn.resize(n + 1);
        jnDeriv.resize(n + 1);
        ynDeriv.resize(n + 1);
        legendre.resize(n + 1);
        jDown.resize(n + 1);
        hn.resize(n + 1);
    }
}

std::complex<double> rigidSphereTransfer(double freqHz, double cosGamma,
                                         double radiusM, double speedOfSoundMps,
                                         RigidSphereWorkspace& ws) noexcept {
    if (!(radiusM > 0.0) || !(speedOfSoundMps > 0.0)) return {0.0, 0.0};
    if (!(freqHz > 0.0)) return {1.0, 0.0};  // límite ka -> 0: sin dispersión
    const double cg = std::clamp(cosGamma, -1.0, 1.0);
    const double ka = 2.0 * kPi * freqHz * radiusM / speedOfSoundMps;
    const int nOrd = rigidSphereOrder(ka);
    const std::size_t n = static_cast<std::size_t>(nOrd);
    ws.ensure(n + 30);

    // ── jₙ por Miller descendente (scratch en el workspace, cero malloc) ──
    const std::size_t mTop = n + 25;
    ws.ensure(mTop + 1);
    std::vector<double>& jDown = ws.jDown;
    std::fill(jDown.begin(), jDown.end(), 0.0);
    jDown[mTop] = 1.0;
    jDown[mTop + 1] = 0.0;
    for (std::size_t k = mTop; k >= 1; --k) {
        // j_{k-1} = (2k+1)/ka · j_k − j_{k+1}
        jDown[k - 1] = (2.0 * static_cast<double>(k) + 1.0) / ka * jDown[k] - jDown[k + 1];
        if (k == 1) break;
    }
    const double j0exact = std::sin(ka) / ka;
    const double scale = (jDown[0] != 0.0) ? j0exact / jDown[0] : 0.0;
    for (std::size_t k = 0; k <= n; ++k) ws.jn[k] = jDown[k] * scale;

    // ── yₙ ascendente ──
    ws.yn[0] = -std::cos(ka) / ka;
    if (n >= 1) ws.yn[1] = -std::cos(ka) / (ka * ka) - std::sin(ka) / ka;
    for (std::size_t k = 1; k < n; ++k) {
        ws.yn[k + 1] = (2.0 * static_cast<double>(k) + 1.0) / ka * ws.yn[k] - ws.yn[k - 1];
    }

    // ── derivadas ──
    ws.jnDeriv[0] = -ws.jn[1];
    ws.ynDeriv[0] = -ws.yn[1];
    for (std::size_t k = 1; k <= n; ++k) {
        const double f = (static_cast<double>(k) + 1.0) / ka;
        ws.jnDeriv[k] = ws.jn[k - 1] - f * ws.jn[k];
        ws.ynDeriv[k] = ws.yn[k - 1] - f * ws.yn[k];
    }

    // ── Legendre Pₙ(cos γ) ──
    ws.legendre[0] = 1.0;
    if (n >= 1) ws.legendre[1] = cg;
    for (std::size_t k = 1; k < n; ++k) {
        ws.legendre[k + 1] = ((2.0 * static_cast<double>(k) + 1.0) * cg * ws.legendre[k] -
                              static_cast<double>(k) * ws.legendre[k - 1]) /
                             static_cast<double>(k + 1);
    }

    // ── serie ──
    std::complex<double> sum(0.0, 0.0);
    std::complex<double> iPow(1.0, 0.0);
    const std::complex<double> iUnit(0.0, 1.0);
    for (std::size_t k = 0; k <= n; ++k) {
        const std::complex<double> hn(ws.jn[k], ws.yn[k]);
        const std::complex<double> hnDeriv(ws.jnDeriv[k], ws.ynDeriv[k]);
        const std::complex<double> coef =
            std::complex<double>(ws.jn[k], 0.0) - (ws.jnDeriv[k] / hnDeriv) * hn;
        sum += (2.0 * static_cast<double>(k) + 1.0) * iPow * coef * ws.legendre[k];
        iPow *= iUnit;
    }
    return sum;
}

std::complex<double> rigidSphereTransfer(double freqHz, double cosGamma,
                                         double radiusM, double speedOfSoundMps) {
    RigidSphereWorkspace ws;
    return rigidSphereTransfer(freqHz, cosGamma, radiusM, speedOfSoundMps, ws);
}

EarTransfer rigidSphereEarTransfer(double freqHz, double azimuthRad,
                                   double radiusM, double speedOfSoundMps,
                                   RigidSphereWorkspace& ws) noexcept {
    const double s = std::sin(azimuthRad);
    EarTransfer out;
    out.left = rigidSphereTransfer(freqHz, -s, radiusM, speedOfSoundMps, ws);
    out.right = rigidSphereTransfer(freqHz, +s, radiusM, speedOfSoundMps, ws);
    return out;
}

EarTransfer rigidSphereEarTransfer(double freqHz, double azimuthRad,
                                   double radiusM, double speedOfSoundMps) {
    RigidSphereWorkspace ws;
    return rigidSphereEarTransfer(freqHz, azimuthRad, radiusM, speedOfSoundMps, ws);
}

double rigidSphereIldDb(double freqHz, double azimuthRad,
                        double radiusM, double speedOfSoundMps) noexcept {
    const EarTransfer e = rigidSphereEarTransfer(freqHz, azimuthRad, radiusM, speedOfSoundMps);
    const double ml = std::abs(e.left);
    const double mr = std::abs(e.right);
    if (!(ml > 0.0) || !(mr > 0.0)) return 0.0;
    return 20.0 * std::log10(ml / mr);
}

double rigidSphereItdMicroseconds(double azimuthRad, double fLoHz, double fHiHz,
                                  double radiusM, double speedOfSoundMps) noexcept {
    if (!(fHiHz > fLoHz) || !(fLoHz > 0.0)) return 0.0;
    // Muestreo de fase interaural en la banda; ajuste lineal (mínimos cuadrados).
    constexpr int kSamples = 64;
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    double prevPhase = 0.0;
    bool first = true;
    double unwrapped = 0.0;
    RigidSphereWorkspace ws;
    for (int i = 0; i < kSamples; ++i) {
        const double f = fLoHz + (fHiHz - fLoHz) * static_cast<double>(i) / (kSamples - 1);
        const EarTransfer e = rigidSphereEarTransfer(f, azimuthRad, radiusM, speedOfSoundMps, ws);
        double ph = std::arg(e.left / e.right);
        if (first) {
            unwrapped = ph;
            first = false;
        } else {
            double d = ph - prevPhase;
            while (d > kPi) d -= 2.0 * kPi;
            while (d < -kPi) d += 2.0 * kPi;
            unwrapped += d;
        }
        prevPhase = ph;
        sx += f;
        sy += unwrapped;
        sxx += f * f;
        sxy += f * unwrapped;
    }
    const double n = static_cast<double>(kSamples);
    const double denom = n * sxx - sx * sx;
    if (denom == 0.0) return 0.0;
    // Con convención e^{-iωt}, un retardo τ multiplica H por e^{+iωτ}: la
    // fase de H_L/H_R crece con pendiente +2π·ITD si L llega ANTES... salvo
    // que H ya es p_sup/p_centro: la fase interaural es arg(H_L/H_R) y el
    // ITD (llegada_R − llegada_L) = −pendiente/(2π). El signo se verificó
    // contra el SOFA MIT KEMAR (ITD medido +679 µs a 90°, modelo +759 µs).
    const double slope = (n * sxy - sx * sy) / denom;  // rad/Hz
    return -slope / (2.0 * kPi) * 1.0e6;
}

bool synthesizeRigidSphereHrir(double azimuthRad, double sampleRateHz, int numTaps,
                               double delaySeconds, double* outLeft, double* outRight,
                               double radiusM, double speedOfSoundMps) noexcept {
    if (outLeft == nullptr || outRight == nullptr) return false;
    if (!(sampleRateHz > 0.0) || numTaps <= 0 || !(delaySeconds >= 0.0)) return false;
    if (!(radiusM > 0.0) || !(speedOfSoundMps > 0.0)) return false;
    std::size_t nFft = 1;
    while (nFft < static_cast<std::size_t>(2 * numTaps)) nFft <<= 1;
    std::vector<std::complex<double>> specL(nFft), specR(nFft);
    RigidSphereWorkspace ws;
    const double s = std::sin(azimuthRad);
    for (std::size_t k = 0; k <= nFft / 2; ++k) {
        const double f = static_cast<double>(k) * sampleRateHz / static_cast<double>(nFft);
        // CONJUGACIÓN OBLIGATORIA: H está definida con convención e^{-iωt}
        // (p(t) = Re{H·e^{-iωt}}), pero la DFT inversa sintetiza e^{+iωt};
        // sin conjugar, la HRIR sale INVERTIDA EN TIEMPO (ITD con el signo
        // cambiado). El retardo común e^{-iωτ} sí es un retardo en la DFT.
        const std::complex<double> delayPhase =
            std::exp(std::complex<double>(0.0, -2.0 * kPi * f * delaySeconds));
        specL[k] = std::conj(rigidSphereTransfer(f, -s, radiusM, speedOfSoundMps, ws)) * delayPhase;
        specR[k] = std::conj(rigidSphereTransfer(f, +s, radiusM, speedOfSoundMps, ws)) * delayPhase;
    }
    for (std::size_t k = nFft / 2 + 1; k < nFft; ++k) {
        specL[k] = std::conj(specL[nFft - k]);
        specR[k] = std::conj(specR[nFft - k]);
    }
    fftRadix2(specL.data(), nFft, true);
    fftRadix2(specR.data(), nFft, true);
    for (int i = 0; i < numTaps; ++i) {
        outLeft[i] = specL[static_cast<std::size_t>(i)].real();
        outRight[i] = specR[static_cast<std::size_t>(i)].real();
    }
    return true;
}

}  // namespace ivanna::physical
