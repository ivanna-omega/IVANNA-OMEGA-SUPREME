// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// ImpulseInverse — motor de filtro inverso (Ley 1, estilo Trinnov).
//
// Toma una respuesta en MAGNITUD medida (|H| por bin) y produce un FIR
// INVERSO compacto (fase + magnitud) que convierte la respuesta al
// impulso en un pulso limpio:
//
//   H_inv(f) = (1/|H(f)|) · e^{−j·φ_min(f)}
//
// donde φ_min es la fase MÍNIMA de |H| (contraparte de Bode, método
// cepstral): el inverso es causal y de fase mínima — sin pre-ringing,
// toda la energía después de t=0. El inverso NUNCA levanta un valle
// más de maxBoostDb (por defecto +12 dB): una cancelación profunda es
// un nulo físico, no algo que se corrija con ganancia.
//
// Método cepstral (usa FFTRadix2, ya en el repo):
//   1. c = IFFT(ln|H|)                      (cepstrum real)
//   2. cMin = causalizar(c)                 (0, 2·c, 0: fase mínima)
//   3. ln H_min = FFT(cMin)                 (re = ln|H|, im = φ_min)
//   4. H_inv = (1/|H|)·e^{−jφ_min}, con tope de realce
//   5. fir = IFFT(H_inv), ventana Hann a maxTaps
//
// La MEDICIÓN en vivo (RealAcousticSensor/RoomProbe) es de la Fase 4;
// este frente entrega el MOTOR + la etapa FIR que lo aplica. La función
// de cálculo aloca (hilo de CONTROL, al instalar una medición nueva);
// process() es RT-seguro (cero alloc, cero locks).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "fft_radix2.hpp"

namespace ivanna {
namespace holos {

namespace detail {
// ¿N es potencia de 2 (y ≥ 8)?
inline bool isPow2Ge8(int n) noexcept {
    return n >= 8 && (n & (n - 1)) == 0;
}
}  // namespace detail

// FIR inverso de fase mínima desde una magnitud medida.
// magLin: |H| lineal por bin, k = 0..nBins-1, nBins = N/2+1 (N potencia de 2).
// firOut: salida, capacidad maxTaps. maxBoostDb: tope de realce (+12 dB).
// Devuelve el número de taps escritos (0 si la entrada es inválida).
// Con |H| = 1 en todas partes → FIR = [1, 0, 0, ...] (identidad).
inline int inverseFirFromMagnitude(const float* magLin, int nBins,
                                   float* firOut, int maxTaps,
                                   float maxBoostDb = 12.0f) noexcept {
    if (!magLin || !firOut || nBins < 5 || maxTaps <= 0) return 0;
    const int n = 2 * (nBins - 1);  // tamaño FFT
    if (!detail::isPow2Ge8(n)) return 0;
    const int taps = std::min(maxTaps, n);
    const float floorMag = 1e-6f;  // −120 dB: nunca dividir por cero
    const float maxBoost = std::pow(10.0f, std::clamp(maxBoostDb, 0.0f, 24.0f) / 20.0f);
    FFTRadix2 fft(n);
    std::vector<float> re(n), im(n);
    // 1. Cepstrum real: IFFT de ln|H| (espectro real y par).
    for (int k = 0; k <= n / 2; ++k) {
        const float m = std::isfinite(magLin[k]) ? std::max(magLin[k], floorMag) : 1.0f;
        re[k] = std::log(m);
        if (k > 0 && k < n / 2) re[n - k] = re[k];
        im[k] = 0.0f;
        if (k > 0 && k < n / 2) im[n - k] = 0.0f;
    }
    fft.transform(re.data(), im.data(), -1);  // inversa, sin normalizar
    const float invN = 1.0f / (float)n;
    for (int i = 0; i < n; ++i) re[i] *= invN;  // c[n] (cepstrum real)
    // 2. Causalizar (fase mínima): 0, 2·c, 0.
    std::vector<float> cMin(n, 0.0f);
    cMin[0] = re[0];
    for (int i = 1; i < n / 2; ++i) cMin[i] = 2.0f * re[i];
    cMin[n / 2] = re[n / 2];
    // 3. FFT → ln H_min (re = ln|H|, im = φ_min).
    std::fill(im.begin(), im.end(), 0.0f);
    fft.transform(cMin.data(), im.data(), +1);
    // cMin ahora es re; im es φ_min.
    // 4. Espectro inverso (primera mitad) + espejo hermitiano.
    std::vector<float> invRe(n), invIm(n);
    for (int k = 0; k <= n / 2; ++k) {
        const float m = std::isfinite(magLin[k]) ? std::max(magLin[k], floorMag) : 1.0f;
        float invMag = 1.0f / m;
        if (invMag > maxBoost) invMag = maxBoost;
        const float phi = im[k];  // φ_min (de la FFT de cMin)
        invRe[k] = invMag * std::cos(phi);
        invIm[k] = -invMag * std::sin(phi);
        if (k > 0 && k < n / 2) {
            invRe[n - k] = invRe[k];
            invIm[n - k] = -invIm[k];
        }
    }
    // 5. IFFT → FIR temporal (parte real), ventana Hann a `taps`.
    fft.transform(invRe.data(), invIm.data(), -1);
    for (int i = 0; i < n; ++i) invRe[i] *= invN;
    for (int i = 0; i < taps; ++i) {
        // Ventana medio-coseno: 1.0 en i=0 (se preserva el tap directo),
        // decayendo a ~0 en i=taps-1 (compacta sin recorte brusco).
        const float win = 0.5f + 0.5f * std::cos(3.14159265358979323846f * (float)i / (float)taps);
        firOut[i] = invRe[i] * win;
    }
    return taps;
}

// Etapa FIR compacta (la que aplica el inverso en el pipeline).
// setTaps() aloca (hilo de CONTROL); process() es RT-seguro.
class InverseFirStage {
public:
    InverseFirStage() = default;
    InverseFirStage(const InverseFirStage&) = delete;
    InverseFirStage& operator=(const InverseFirStage&) = delete;

    // Instala un FIR (copia). taps==0 o null → bypass (identidad).
    void setTaps(const float* taps, int nTaps) noexcept {
        if (!taps || nTaps <= 0) {
            taps_.clear();
            dl_.clear();
            dr_.clear();
            pos_ = 0;
            return;
        }
        const int n = std::min(nTaps, 1024);
        taps_.assign(taps, taps + n);
        dl_.assign((size_t)n, 0.0f);
        dr_.assign((size_t)n, 0.0f);
        pos_ = 0;
    }
    bool active() const noexcept { return !taps_.empty(); }
    int numTaps() const noexcept { return (int)taps_.size(); }

    void reset() noexcept {
        std::fill(dl_.begin(), dl_.end(), 0.0f);
        std::fill(dr_.begin(), dr_.end(), 0.0f);
        pos_ = 0;
    }

    // Convolución directa (misma FIR en L y R). Sin alloc, sin locks.
    void process(float* l, float* r, int n) noexcept {
        if (!l || !r || n <= 0) return;
        if (taps_.empty()) return;  // bypass: identidad
        const int t = (int)taps_.size();
        for (int i = 0; i < n; ++i) {
            dl_[pos_] = std::isfinite(l[i]) ? l[i] : 0.0f;
            dr_[pos_] = std::isfinite(r[i]) ? r[i] : 0.0f;
            float accL = 0.0f, accR = 0.0f;
            int idx = pos_;
            for (int k = 0; k < t; ++k) {
                accL += taps_[k] * dl_[idx];
                accR += taps_[k] * dr_[idx];
                if (--idx < 0) idx = t - 1;
            }
            l[i] = accL;
            r[i] = accR;
            if (++pos_ >= t) pos_ = 0;
        }
    }

private:
    std::vector<float> taps_;
    std::vector<float> dl_, dr_;
    int pos_ = 0;
};

}  // namespace holos
}  // namespace ivanna
