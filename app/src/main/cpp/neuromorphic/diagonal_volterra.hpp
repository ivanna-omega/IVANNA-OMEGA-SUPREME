// =====================================================================
//  IVANNA — Volterra Diagonal (polinomio de memoria) v2.5.368
//  diagonal_volterra.hpp — header-only, C++20/26.
//
//  © 2026 Luis Uriel Pimentel Pérez · GORE TNS. All rights reserved.
//
//  El camino real hacia "K=4096" (pilar 1, docs/AUX_GENEZI_ROUTE.md §1).
//
//  El Volterra H2 COMPLETO es O(K^2) por muestra: K=4096 -> 8.4 M
//  coeficientes y ~806 GMAC/s. Inviable. Pero la literatura de
//  pre-distorsión (DPD) lleva décadas resolviendo exactamente este
//  problema podando el kernel por la física del dispositivo:
//
//    * Polinomio de memoria (memory polynomial, Ding et al.): solo
//      los términos DIAGONALES h[k]·x[n-k]·|x[n-k]|^(p-1). Es el
//      modelo estándar de DPD en RF y audio.
//    * Dynamic Deviation Reduction (DDR, Ghannouchi/Hammi): se podan
//      los términos cruzados lejanos y se conservan los cercanos a
//      la diagonal. Un DDR de 2º orden usa 93 coeficientes frente a
//      1287 del Volterra completo (5º orden, 8 taps).
//
//  Este módulo es el caso orden 2, diagonal puro:
//
//      y[n] = Σ_k h1[k]·x[n-k] + Σ_k h2d[k]·x[n-k]²
//
//  Coste O(K) por muestra: K=4096 -> 8192 MACs/muestra/canal ->
//  ~0.79 GMAC/s en estéreo @48 kHz. Viable con NEON en un teléfono.
//
//  Qué captura y qué NO captura (honesto):
//    SÍ: no-linealidad estática (compresión, asimetría) + memoria
//      lineal larga (resonancias de caja, modos del habitáculo hasta
//      4096 muestras = 85 ms @48 kHz).
//    NO: los términos cruzados h2[k,l] con k≠l (interacción entre
//      muestras de distinto retardo). Para esos sigue existiendo el
//      VolterraH2Symmetric K=64 (2,080 coef.), que es el complemento:
//      H2 completo para memoria corta + diagonal para memoria larga.
//      La suma de ambos es el modelo DDR de 2º orden del repo.
//
//  Los coeficientes SOLO vienen de medición (protocolo
//  GENEZY_CHARACTERIZATION_PROTOCOL.md). Sin coeficientes medidos, el
//  módulo es identidad. setCoefficients() valida (finitos, |c| ≤ 8,
//  K en [1..4096]) y rechaza lo demás sin tocar el estado.
//
//  Tiempo real: prepare() asigna las líneas de retardo (hilo de
//  control); process() no asigna, no bloquea. Bypass bit-exacto
//  cuando h1 es identidad y h2d es cero.
// =====================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ivanna::dsp {

// K máximo del modelo diagonal (memoria larga). 4096 muestras = 85 ms.
inline constexpr std::uint32_t kMaxDiagonalKernelLength = 4096;
// Cota de coeficiente (misma que la política IVK1 de producción).
inline constexpr float kMaxDiagonalCoeff = 8.0f;

struct DiagonalVolterraCost {
    std::uint64_t coeffs = 0;              // 2K (h1 + h2d)
    std::uint64_t macsPerSample = 0;       // 2K por canal
    double gmacPerSecondStereo48k = 0.0;   // 2 canales @48 kHz
};

inline DiagonalVolterraCost diagonalVolterraCost(std::uint32_t k) noexcept {
    DiagonalVolterraCost c{};
    c.coeffs = 2ull * k;
    c.macsPerSample = 2ull * k;
    c.gmacPerSecondStereo48k =
        static_cast<double>(c.macsPerSample) * 2.0 * 48000.0 / 1.0e9;
    return c;
}

class DiagonalVolterra {
public:
    // Hilo de control. K en [1..4096]; fuera de rango -> K=64.
    // F2/486: coeficientes e historial en double; la API de carga
    // sigue aceptando float (datos de medición) con cast exacto.
    void prepare(float sampleRate, std::uint32_t kernelLength) noexcept {
        sampleRate_ = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        kernelLength_ = (kernelLength >= 1 && kernelLength <= kMaxDiagonalKernelLength)
                            ? kernelLength
                            : 64;
        h1_.assign(kernelLength_, 0.0);
        h2d_.assign(kernelLength_, 0.0);
        h1_[0] = 1.0;  // identidad por defecto
        delayL_.assign(kernelLength_, 0.0);
        delayR_.assign(kernelLength_, 0.0);
        write_ = 0;
        prepared_ = true;
    }

    [[nodiscard]] std::uint32_t kernelLength() const noexcept {
        return kernelLength_;
    }

    // Hilo de control. Devuelve false si rechaza (no toca nada):
    // punteros nulos, K distinto del preparado, coeficiente no finito
    // o |c| > 8. Los coeficientes vienen de medición, nunca inventados.
    bool setCoefficients(const double* h1, const double* h2d,
                         std::uint32_t length) noexcept {
        if (!prepared_ || !h1 || !h2d) return false;
        if (length != kernelLength_) return false;
        for (std::uint32_t k = 0; k < length; ++k) {
            const double a = h1[k], b = h2d[k];
            if (!std::isfinite(a) || !std::isfinite(b)) return false;
            if (std::fabs(a) > static_cast<double>(kMaxDiagonalCoeff) ||
                std::fabs(b) > static_cast<double>(kMaxDiagonalCoeff)) {
                return false;
            }
        }
        std::copy(h1, h1 + length, h1_.begin());
        std::copy(h2d, h2d + length, h2d_.begin());
        return true;
    }

    // Overload float (compat legacy): cast exacto a double.
    bool setCoefficients(const float* h1, const float* h2d,
                         std::uint32_t length) noexcept {
        if (!prepared_ || !h1 || !h2d) return false;
        if (length != kernelLength_) return false;
        if (wrapH1_.size() < length) { wrapH1_.resize(length); wrapH2_.resize(length); }
        for (std::uint32_t k = 0; k < length; ++k) {
            wrapH1_[k] = static_cast<double>(h1[k]);
            wrapH2_[k] = static_cast<double>(h2d[k]);
        }
        return setCoefficients(wrapH1_.data(), wrapH2_.data(), length);
    }

    [[nodiscard]] bool isBypass() const noexcept {
        if (!prepared_) return true;
        if (h1_[0] != 1.0) return false;
        for (std::uint32_t k = 1; k < kernelLength_; ++k) {
            if (h1_[k] != 0.0) return false;
        }
        for (std::uint32_t k = 0; k < kernelLength_; ++k) {
            if (h2d_[k] != 0.0) return false;
        }
        return true;
    }

    // Hilo de audio. y[n] = Σ h1[k]·x[n-k] + Σ h2d[k]·x[n-k]².
    // Cero malloc. Si es bypass, no toca los buffers (bit-exacto).
    // Núcleo double (F2/486).
    void process(double* left, double* right, int n) noexcept {
        if (!prepared_ || !left || !right || n <= 0) return;
        if (isBypass()) return;
        const std::size_t kLen = kernelLength_;
        for (int i = 0; i < n; ++i) {
            delayL_[write_] = left[i];
            delayR_[write_] = right[i];
            double yL = 0.0, yR = 0.0;
            std::size_t idx = write_;
            for (std::size_t k = 0; k < kLen; ++k) {
                const double xL = delayL_[idx];
                const double xR = delayR_[idx];
                yL += h1_[k] * xL + h2d_[k] * xL * xL;
                yR += h1_[k] * xR + h2d_[k] * xR * xR;
                idx = (idx == 0) ? kLen - 1 : idx - 1;
            }
            left[i] = std::isfinite(yL) ? yL : 0.0;
            right[i] = std::isfinite(yR) ? yR : 0.0;
            write_ = (write_ + 1 == kLen) ? 0 : write_ + 1;
        }
    }

    // Envoltorio float (compat legacy): convierte por scratch y delega
    // en el núcleo double.
    void process(float* left, float* right, int n) noexcept {
        if (!prepared_ || !left || !right || n <= 0) return;
        if (isBypass()) return;
        const std::size_t un = static_cast<std::size_t>(n);
        if (wrapL_.size() < un) { wrapL_.resize(un); wrapR_.resize(un); }
        for (int i = 0; i < n; ++i) {
            wrapL_[i] = std::isfinite(left[i]) ? static_cast<double>(left[i]) : 0.0;
            wrapR_[i] = std::isfinite(right[i]) ? static_cast<double>(right[i]) : 0.0;
        }
        process(wrapL_.data(), wrapR_.data(), n);
        for (int i = 0; i < n; ++i) {
            left[i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
            right[i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
        }
    }

    void reset() noexcept {
        std::fill(delayL_.begin(), delayL_.end(), 0.0);
        std::fill(delayR_.begin(), delayR_.end(), 0.0);
        write_ = 0;
    }

private:
    double sampleRate_ = 48000.0;
    std::uint32_t kernelLength_ = 64;
    bool prepared_ = false;
    std::vector<double> h1_{};
    std::vector<double> h2d_{};
    std::vector<double> delayL_{};
    std::vector<double> delayR_{};
    std::size_t write_ = 0;
    // Scratch de los envoltorios float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapL_{};
    std::vector<double> wrapR_{};
    std::vector<double> wrapH1_{};
    std::vector<double> wrapH2_{};
};

}  // namespace ivanna::dsp
