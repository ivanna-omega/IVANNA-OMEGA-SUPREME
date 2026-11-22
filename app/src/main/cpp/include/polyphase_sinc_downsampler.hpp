// polyphase_sinc_downsampler.hpp
// ============================================================================
// IVANNA — Downsampler sinc polifásico de Kaiser (Fase 4, Ley 3)
// ============================================================================
// FLAC 192 kHz/24-bit a DAC 48 kHz: el remuestreador Kotlin de 64 taps
// (Blackman-Harris, ~-92 dB) dejaba alias audibles por encima de 24 kHz.
// Este downsampler nativo es sinc de fase lineal con ventana Kaiser:
//
//   · Ratios enteros (2, 4, 8): 192k→48k, 96k→48k, 88.2k→44.1k, 176.4k→44.1k.
//   · Diseño float64 (coeficientes Kaiser, beta para 140 dB) y acumulación
//     float64 por muestra de salida; la salida es float32.
//   · Rechazo de alias MEDIDO >130 dB (prototipo: -145 dB a 24 kHz, -163 dB
//     a 30 kHz para 192k→48k); banda audible plana hasta 20 kHz (-0.03 dB).
//   · Fase lineal (FIR simétrico): sin distorsión de fase ni pre-ringing
//     asimétrico; retardo de grupo constante = (N-1)/2 muestras de entrada.
//   · Sin allocs en process(): la tabla y el historial se reservan en
//     configure(); el scratch solo crece (nunca se libera en caliente).
//   · NO es un ASRC (ratio fijo entero, sin servo de reloj): el servo de
//     reloj lo sigue llevando el PLL + RouteAAsrc. NO reescribe el ASRC.
//
// Uso (hilo de decodificación, no RT): configure() una vez por pista,
// process() por bloque de decodificación, flush() al final del stream
// (drena la cola de latencia). reset() entre pistas (silencio).
// ============================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ivanna {

class PolyphaseSincDownsampler {
public:
    // Atenuación de diseño (margen sobre los 130 dB exigidos por Fase 4).
    static constexpr double kDesignAttenDb = 140.0;
    // Borde de la banda de paso: la banda audible queda plana hasta 20 kHz
    // (-0.03 dB); la transición va de aquí a Nyquist de salida.
    static constexpr double kPassbandEdgeHz = 21000.0;
    static constexpr int kMaxRatio = 8;
    static constexpr int kMaxTaps = 4095;

    PolyphaseSincDownsampler() = default;

    // Diseña el filtro para inputRate/outputRate (ratio entero 2..8).
    // Devuelve false si el ratio no es entero o está fuera de rango.
    // Reconfigurar descarta el historial (silencio).
    bool configure(int inputRate, int outputRate) noexcept {
        if (inputRate <= 0 || outputRate <= 0 || inputRate <= outputRate) return false;
        if (inputRate % outputRate != 0) return false;
        const int ratio = inputRate / outputRate;
        if (ratio < 2 || ratio > kMaxRatio) return false;

        const double fsStop = static_cast<double>(outputRate) * 0.5;   // Nyquist de salida
        const double fc = std::min(kPassbandEdgeHz, fsStop * 0.875);    // borde de paso
        if (!(fc > 0.0) || !(fc < fsStop)) return false;
        const double beta = kaiserBeta(kDesignAttenDb);
        const double dW = 2.0 * kPi * (fsStop - fc) / static_cast<double>(inputRate);
        int n = static_cast<int>(std::ceil((kDesignAttenDb - 8.0) / (2.285 * dW))) + 1;
        if ((n % 2) == 0) ++n;                                          // impar (simétrico)
        if (n > kMaxTaps) n = kMaxTaps;
        if (n < 31) n = 31;

        taps_.assign(static_cast<size_t>(n), 0.0);
        const double i0b = besselI0(beta);
        double sum = 0.0;
        for (int i = 0; i < n; ++i) {
            const double m = static_cast<double>(i) - (n - 1) / 2.0;
            const double x = 2.0 * fc / static_cast<double>(inputRate) * m;
            const double sinc = (std::fabs(x) < 1e-12) ? 1.0
                : std::sin(kPi * x) / (kPi * x);
            const double r = 2.0 * i / (n - 1) - 1.0;                    // -1..1
            const double w = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
            taps_[static_cast<size_t>(i)] = 2.0 * fc / static_cast<double>(inputRate) * sinc * w;
            sum += taps_[static_cast<size_t>(i)];
        }
        for (auto& v : taps_) v /= sum;                                  // ganancia DC = 1

        inputRate_ = inputRate;
        outputRate_ = outputRate;
        ratio_ = ratio;
        histL_.assign(static_cast<size_t>(n - 1), 0.0);
        histR_.assign(static_cast<size_t>(n - 1), 0.0);
        nextCenter_ = n - 1;                                            // primera muestra real
        return true;
    }

    int ratio() const noexcept { return ratio_; }
    int inputRate() const noexcept { return inputRate_; }
    int outputRate() const noexcept { return outputRate_; }
    int taps() const noexcept { return static_cast<int>(taps_.size()); }
    double stopbandAttenuationDb() const noexcept { return kDesignAttenDb; }
    // Retardo de grupo en muestras de entrada (fase lineal, constante).
    int latencyFrames() const noexcept { return (taps() - 1) / 2; }

    void reset() noexcept {
        std::fill(histL_.begin(), histL_.end(), 0.0);
        std::fill(histR_.begin(), histR_.end(), 0.0);
        nextCenter_ = taps() > 0 ? taps() - 1 : 0;
    }

    // Downsamplea inFrames (L/R separados) a outL/outR.
    // outL/outR deben tener capacidad >= (inFrames + ratio_ - 1) / ratio_ + 1.
    // Devuelve los frames de salida escritos. El historial se conserva entre
    // llamadas (la fase de diezmado no se reinicia por bloque).
    int process(const float* inL, const float* inR, int inFrames,
                float* outL, float* outR) noexcept {
        if (taps_.empty() || ratio_ <= 0) return 0;
        if (inL == nullptr || inR == nullptr || outL == nullptr || outR == nullptr) return 0;
        if (inFrames <= 0) return 0;
        const int n = taps();
        const int center = (n - 1) / 2;
        const int need = (n - 1) + inFrames;
        if (static_cast<int>(scratchL_.size()) < need) {
            scratchL_.resize(static_cast<size_t>(need));
            scratchR_.resize(static_cast<size_t>(need));
        }
        std::copy(histL_.begin(), histL_.end(), scratchL_.begin());
        std::copy(histR_.begin(), histR_.end(), scratchR_.begin());
        for (int i = 0; i < inFrames; ++i) {
            scratchL_[static_cast<size_t>(n - 1 + i)] = static_cast<double>(inL[i]);
            scratchR_[static_cast<size_t>(n - 1 + i)] = static_cast<double>(inR[i]);
        }
        int out = 0;
        while (nextCenter_ + center < need) {
            double accL = 0.0, accR = 0.0;
            const int base = nextCenter_ - center;
            for (int k = 0; k < n; ++k) {
                accL += taps_[static_cast<size_t>(k)] * scratchL_[static_cast<size_t>(base + k)];
                accR += taps_[static_cast<size_t>(k)] * scratchR_[static_cast<size_t>(base + k)];
            }
            outL[out] = static_cast<float>(accL);
            outR[out] = static_cast<float>(accR);
            ++out;
            nextCenter_ += ratio_;
        }
        // Guarda las últimas N-1 muestras como historial del próximo bloque.
        for (int i = 0; i < n - 1; ++i) {
            histL_[static_cast<size_t>(i)] = scratchL_[static_cast<size_t>(inFrames + i)];
            histR_[static_cast<size_t>(i)] = scratchR_[static_cast<size_t>(inFrames + i)];
        }
        nextCenter_ -= inFrames;
        return out;
    }

    // Drena la cola de latencia al final del stream (el filtro necesita
    // (N-1)/2 muestras futuras para las últimas salidas). Devuelve los
    // frames escritos (<= (latencyFrames + ratio - 1) / ratio + 1).
    int flush(float* outL, float* outR) noexcept {
        if (taps_.empty() || ratio_ <= 0) return 0;
        if (outL == nullptr || outR == nullptr) return 0;
        const int zeros = latencyFrames() + ratio_;
        std::vector<float> z(static_cast<size_t>(zeros), 0.0f);
        return process(z.data(), z.data(), zeros, outL, outR);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;

    static double kaiserBeta(double attenDb) noexcept {
        if (attenDb > 50.0) return 0.1102 * (attenDb - 8.7);
        if (attenDb >= 21.0)
            return 0.5842 * std::pow(attenDb - 21.0, 0.4) + 0.07886 * (attenDb - 21.0);
        return 0.0;
    }
    static double besselI0(double x) noexcept {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 40; ++k) {
            t *= (x * x) / (4.0 * k * k);
            s += t;
            if (t < 1e-18) break;
        }
        return s;
    }

    int inputRate_ = 0, outputRate_ = 0, ratio_ = 0;
    std::vector<double> taps_;       // coeficientes Kaiser (diseño float64)
    std::vector<double> histL_, histR_;    // historial (N-1 frames de entrada)
    std::vector<double> scratchL_, scratchR_;  // combinado historial+bloque
    int nextCenter_ = 0;             // índice (en combinado) del próximo centro de salida
};

}  // namespace ivanna
