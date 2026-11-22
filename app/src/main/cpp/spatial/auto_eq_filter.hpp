#define _USE_MATH_DEFINES
#pragma once

#include <vector>
#include <cmath>
#include <atomic>
#include <array>
#include <algorithm>

namespace ivanna::spatial {

// Coeficientes en float64 (Fase 4, Ley 3): la acumulación en los estados
// IIR críticos (<120 Hz, crossovers, RoomCorrectionEq) es double. Los
// biquads peaking RBJ son de fase mínima por construcción (IIR); no se
// cambia el tipo de filtro, solo la precisión del estado. La API sigue
// siendo float (las muestras entran/salen como float).
struct BiquadCoeffs {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0;
    double a1 = 0.0, a2 = 0.0;
};

class BiquadFilter {
public:
    void setCoeffs(const BiquadCoeffs& c) noexcept {
        coeffs_ = c;
    }

    // Núcleo double 64-bit (F2/486): el cálculo YA era double; la I/O
    // era float. El overload float delega aquí (bit-idéntico).
    inline double process(double in) noexcept {
        const double out = coeffs_.b0 * in + coeffs_.b1 * z1_ + coeffs_.b2 * z2_
                                        - coeffs_.a1 * out1_ - coeffs_.a2 * out2_;
        z2_ = z1_; z1_ = in;
        out2_ = out1_; out1_ = out;
        return out;
    }

    inline float process(float in) noexcept {
        return static_cast<float>(process(static_cast<double>(in)));
    }

    void reset() noexcept {
        z1_ = z2_ = out1_ = out2_ = 0.0;
    }

private:
    BiquadCoeffs coeffs_;
    double z1_ = 0.0, z2_ = 0.0;
    double out1_ = 0.0, out2_ = 0.0;
};

class AutoEqFilter {
public:
    static constexpr int kMaxBands = 10;
    
    void init(float sampleRate) {
        sampleRate_ = sampleRate;
        for(int i=0; i<kMaxBands; ++i) {
            filtersL_[i].reset();
            filtersR_[i].reset();
        }
    }
    
    void setEnabled(bool enabled) {
        enabled_ = enabled;
    }
    
    // Configures a peaking EQ band. Q = center_freq / bandwidth
    // Coeficientes calculados en float64 (Fase 4, Ley 3).
    void setBand(int index, float freqHz, float gainDb, float Q) {
        if (index < 0 || index >= kMaxBands) return;

        const double A = std::pow(10.0, static_cast<double>(gainDb) / 40.0);
        const double w0 = 2.0 * M_PI * static_cast<double>(freqHz) / static_cast<double>(sampleRate_);
        const double alpha = std::sin(w0) / (2.0 * static_cast<double>(Q));

        BiquadCoeffs c;
        const double a0 = 1.0 + alpha / A;
        c.b0 = (1.0 + alpha * A) / a0;
        c.b1 = (-2.0 * std::cos(w0)) / a0;
        c.b2 = (1.0 - alpha * A) / a0;
        c.a1 = (-2.0 * std::cos(w0)) / a0;
        c.a2 = (1.0 - alpha / A) / a0;
        
        // Atomics or lock-free queue in real world, 
        // here we just directly set it (assume called safely or parameters cross-faded)
        filtersL_[index].setCoeffs(c);
        filtersR_[index].setCoeffs(c);
    }
    
    void process(double* left, double* right, int frames) noexcept {
        if (!enabled_) return;

        for (int i = 0; i < frames; ++i) {
            double sL = left[i];
            double sR = right[i];
            for (int b = 0; b < kMaxBands; ++b) {
                sL = filtersL_[b].process(sL);
                sR = filtersR_[b].process(sR);
            }
            left[i] = sL;
            right[i] = sR;
        }
    }

    void process(float* left, float* right, int frames) noexcept {
        if (!enabled_) return;

        for (int i = 0; i < frames; ++i) {
            float sL = left[i];
            float sR = right[i];
            for (int b = 0; b < kMaxBands; ++b) {
                sL = filtersL_[b].process(sL);
                sR = filtersR_[b].process(sR);
            }
            left[i] = sL;
            right[i] = sR;
        }
    }

private:
    float sampleRate_ = 48000.f;
    std::atomic<bool> enabled_{false};
    std::array<BiquadFilter, kMaxBands> filtersL_;
    std::array<BiquadFilter, kMaxBands> filtersR_;
};

} // namespace ivanna::spatial
