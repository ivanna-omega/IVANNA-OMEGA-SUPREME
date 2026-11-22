// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once

/*
 * ============================================================
 * FFT radix-2 in-place — Cooley-Tukey, header-only.
 *
 * Sin dependencias externas (no requiere FFTW/KissFFT en el NDK).
 * Tamaño debe ser potencia de 2. Pensado para N pequeño (256-1024)
 * usado por HRTFConvolver — no optimizado para señales largas.
 *
 * F2/486 (2026-11-20) — MOTOR DOUBLE: la clase es ahora una plantilla
 * sobre el tipo escalar. `FFTRadix2` sigue siendo la instancia float
 * (todos los llamadores existentes compilan bit-idénticos, sin tocarlos)
 * y `FFTRadix2d` es la instancia double 64-bit para el camino de señal
 * de la FusedChain. Las tablas twiddle ya se calculaban en double y se
 * truncaban a float; en la instancia double ya no hay truncado.
 * ============================================================
 */

#include <cmath>
#include <cstdint>
#include <vector>

namespace ivanna {

template <typename Scalar = float>
class FFTRadix2T {
public:
    // Precalcula tablas de twiddle/bit-reversal para un tamaño fijo N.
    explicit FFTRadix2T(int N) : N_(N), log2N_(0) {
        int n = N;
        while (n > 1) { n >>= 1; ++log2N_; }
        bitrev_.resize(N_);
        for (int i = 0; i < N_; ++i) bitrev_[i] = reverse_bits(i, log2N_);

        cosTab_.resize(N_ / 2);
        sinTab_.resize(N_ / 2);
        for (int i = 0; i < N_ / 2; ++i) {
            const double ang = -2.0 * M_PI * (double)i / (double)N_;
            cosTab_[i] = static_cast<Scalar>(std::cos(ang));
            sinTab_[i] = static_cast<Scalar>(std::sin(ang));
        }
    }

    int size() const noexcept { return N_; }

    // Transformada in-place. sign=+1 forward, sign=-1 inverse (sin normalizar;
    // el llamador debe dividir por N tras la inversa).
    void transform(Scalar* re, Scalar* im, int sign) const noexcept {
        // Bit-reversal permutation
        for (int i = 0; i < N_; ++i) {
            const int j = bitrev_[i];
            if (j > i) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
        }
        // Butterfly stages
        for (int stage = 1; stage <= log2N_; ++stage) {
            const int m  = 1 << stage;
            const int m2 = m >> 1;
            const int tabStep = N_ / m;
            for (int k = 0; k < N_; k += m) {
                for (int j = 0; j < m2; ++j) {
                    const int tabIdx = j * tabStep;
                    Scalar wr = cosTab_[tabIdx];
                    Scalar wi = sign > 0 ? sinTab_[tabIdx] : -sinTab_[tabIdx];

                    const int idxA = k + j;
                    const int idxB = idxA + m2;
                    const Scalar tr = re[idxB] * wr - im[idxB] * wi;
                    const Scalar ti = re[idxB] * wi + im[idxB] * wr;
                    re[idxB] = re[idxA] - tr;
                    im[idxB] = im[idxA] - ti;
                    re[idxA] += tr;
                    im[idxA] += ti;
                }
            }
        }
    }

    void forward(Scalar* re, Scalar* im) const noexcept { transform(re, im, +1); }

    void inverse(Scalar* re, Scalar* im) const noexcept {
        transform(re, im, -1);
        const Scalar invN = Scalar(1) / static_cast<Scalar>(N_);
        for (int i = 0; i < N_; ++i) { re[i] *= invN; im[i] *= invN; }
    }

private:
    static int reverse_bits(int x, int bits) noexcept {
        int r = 0;
        for (int i = 0; i < bits; ++i) { r = (r << 1) | (x & 1); x >>= 1; }
        return r;
    }

    int N_;
    int log2N_;
    std::vector<int>    bitrev_;
    std::vector<Scalar> cosTab_;
    std::vector<Scalar> sinTab_;
};

// Instancia float (histórica): todos los llamadores existentes la usan
// sin cambio y sin cambio de comportamiento.
using FFTRadix2 = FFTRadix2T<float>;
// Instancia double 64-bit (F2/486): camino de señal de la FusedChain.
using FFTRadix2d = FFTRadix2T<double>;

} // namespace ivanna
