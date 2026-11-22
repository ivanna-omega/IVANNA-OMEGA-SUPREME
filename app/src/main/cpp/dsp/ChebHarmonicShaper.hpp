// ChebHarmonicShaper.hpp — Excitador Armónico por Polinomios de Chebyshev (T2 Par + T3 Impar)
// con Control Anti-IMD por Planicidad Espectral y Bloqueador DC de 1er Orden.
// (c) 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
//
// Especificación Matemática M9 / Sección 4.5 / Prueba T7:
//   - Polinomio de Chebyshev de 2º orden centrado (armónico par 2*f0, calidez de triodo):
//       T2*(u) = 2*u^2 - 1 + (1 - u^2) = u^2
//   - Polinomio de Chebyshev de 3er orden (armónico impar 3*f0, presencia/ataque de cinta):
//       T3*(u) = -1/3 * (4*u^3 - 3*u) = u - (4/3)*u^3
//   - Pesos armónicos gobernados por warmth in [0, 1], drive in [0, 1] y flatness1m (f8):
//       d_eff = drive * (1 - 0.5 * f8)
//       k2 = 0.42 * warmth * d_eff
//       k3 = 0.42 * (1 - 0.85 * warmth) * d_eff
//   - Bloqueador DC de 1er orden a fc = 15 Hz: H_DC(z) = (1 - z^-1) / (1 - R*z^-1)
//     garantizando |DC| < 1e-4 en estado estacionario.
//
// F2/486 (2026-11-20) — MOTOR DOUBLE: plantilla sobre el tipo escalar.
// `ChebHarmonicShaper` sigue siendo la instancia float (llamadores
// existentes bit-idénticos) y `ChebHarmonicShaperd` es la instancia
// double 64-bit para el camino de señal de la FusedChain.
#pragma once

#include <algorithm>
#include <cmath>

namespace ivanna { namespace dsp {

template <typename T = float>
class ChebHarmonicShaperT {
public:
    void prepare(T sr) noexcept {
        const T safeSr = (sr > T(8000)) ? sr : T(48000);
        R_ = std::exp(-T(6.283185307179586) * T(15) / safeSr);
        reset();
    }

    void reset() noexcept {
        dcX_ = T(0);
        dcY_ = T(0);
    }

    // x: muestra de entrada
    // drive: [0..1], warmth: [0..1], flatness1m (f8): [0..1]
    T tick(T x, T drive, T warmth, T flatness1m = T(0)) noexcept {
        if (!std::isfinite(x)) return T(0);
        const T d  = std::clamp(drive,      T(0), T(1));
        const T w  = std::clamp(warmth,     T(0), T(1));
        const T f8 = std::clamp(flatness1m, T(0), T(1));

        // Atenuación Anti-IMD en pasajes armónicamente densos (M9)
        const T dEff = d * (T(1) - T(0.5) * f8);

        // Compresor suave de envolvente C^1 para mantener u en (-1, 1) sin generar
        // distorsión impar parásita en niveles nominales (|x| <= 0.85).
        T u = x;
        const T ax = std::fabs(x);
        if (ax > T(0.85)) {
            const T ex = ax - T(0.85);
            const T sat = T(0.85) + T(0.15) * (ex / (T(0.15) + ex));
            u = (x >= T(0)) ? sat : -sat;
        }

        const T k2 = T(0.42) * w * dEff;
        const T k3 = T(0.42) * (T(1) - T(0.85) * w) * dEff;

        // T2*(u) = 2*u^2 - 1 + (1 - u^2) = u^2 (2º armónico par puro)
        const T t2 = T(2) * u * u - T(1) + (T(1) - u * u);
        // T3(u) = 4*u^3 - 3*u (3er armónico impar puro)
        const T t3 = T(4) * u * u * u - T(3) * u;

        const T y = (T(1) - k2 - k3) * u + k2 * t2 + k3 * (t3 * T(-0.33333333));

        // Bloqueador DC de 15 Hz: H(z) = (1 - z^-1) / (1 - R*z^-1)
        const T out = y - dcX_ + R_ * dcY_;
        dcX_ = y;
        dcY_ = std::isfinite(out) ? out : T(0);
        return dcY_;
    }

private:
    T R_   = T(0.99804);
    T dcX_ = T(0);
    T dcY_ = T(0);
};

// Instancia float (histórica) / instancia double 64-bit (F2/486).
using ChebHarmonicShaper = ChebHarmonicShaperT<float>;
using ChebHarmonicShaperd = ChebHarmonicShaperT<double>;

}} // namespace ivanna::dsp
