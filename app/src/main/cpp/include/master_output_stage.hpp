#pragma once

#include "true_peak.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ivanna::master {

/**
 * Etapa final de consola maestro (64-bit double) tras SafetyLimiter:
 *   1) Guarda True-Peak 4x a -0.2 dBTP (0.9772372209558107) con convergencia
 *      suave antes del primer sobrepico (sin clic en frontera de bloque,
 *      sin latencia y fast-path bit-exacto cuando no hay exceso).
 *   2) Mezcla dry/wet en double (64-bit).
 *   3) TPDF Dither triangular exacto (+/-1 LSB) SOLO cuando el efecto está
 *      activo (masterEnv > 1e-6) y el formato destino es entero (16/24-bit).
 *      Con masterEnv == 0 (bypass), salida 100 % bit-exacta.
 *   4) Noise shaping por realimentación de error de 2º orden (Ley 3, Fase C):
 *      la salida se cuantiza a la rejilla real que usa el conversor aguas
 *      abajo (1/32767 a 16-bit, 1/8388607 a 24-bit — omega_pcm_format.h usa
 *      lrintf(x*32767) / lrintf(x*8388607)) y el error de cuantización se
 *      realimenta con NTF (1-z^-1)^2, de modo que el ruido de cuantización
 *      se desplaza fuera de la banda audible en vez de quedarse blanco.
 *      El TPDF sigue decorrelando (rompe armónicos de truncación); el
 *      shaping empuja el error residual a HF. Desviación máxima por muestra
 *      3 LSB (dither 1 + feedback 1.5 + redondeo 0.5).
 *
 * RT-safe: cero malloc, cero locks, estado por instancia de efecto.
 */
class MasterOutputStage {
public:
    static constexpr double kTargetTruePeakLin = 0.9772372209558107; // -0.2 dBTP
    static constexpr double kReleaseMs         = 60.0;

    void prepare(double sampleRate) noexcept {
        if (std::isfinite(sampleRate) && sampleRate >= 8000.0) {
            m_sampleRate = sampleRate;
        }
        m_releaseCoef = std::exp(-1.0 / (m_sampleRate * (kReleaseMs / 1000.0)));
    }

    void reset() noexcept {
        m_tpGain = 1.0;
        m_err1L = m_err2L = 0.0;
        m_err1R = m_err2R = 0.0;
        // Reinicio determinista del dither: dos pasadas sobre la misma
        // entrada tras reset() producen la misma salida (testeable en host).
        m_rngL = 0x9E3779B97F4A7C15ULL;
        m_rngR = 0xBF58476D1CE4E5B9ULL;
    }

    /**
     * Guarda True-Peak 4x en 64-bit justo después de SafetyLimiter::process().
     * Fast-path bit-exacto si truePeak4x <= -0.2 dBTP y sin release pendiente.
     */
    void processTruePeakGuard(float* L, float* R, int frames) noexcept {
        if (!L || !R || frames <= 0) return;
        if (m_releaseCoef <= 0.0) prepare(m_sampleRate);

        const float tpL = ivanna::truePeak4x(L, frames);
        const float tpR = ivanna::truePeak4x(R, frames);
        const double tp = std::max(
            std::isfinite(tpL) ? static_cast<double>(tpL) : 0.0,
            std::isfinite(tpR) ? static_cast<double>(tpR) : 0.0);

        if (tp <= kTargetTruePeakLin && m_tpGain >= 0.9999995) {
            m_tpGain = 1.0;
            return;
        }

        const double targetGain = (tp > kTargetTruePeakLin && tp > 1.0e-12)
            ? std::clamp(kTargetTruePeakLin / tp, 0.0, 1.0)
            : 1.0;

        // Buscar dónde empieza a subir sobre el umbral (para llegar suave antes del pico).
        int firstHot = frames - 1;
        for (int i = 0; i < frames; ++i) {
            const double al = std::fabs(static_cast<double>(L[i]));
            const double ar = std::fabs(static_cast<double>(R[i]));
            if (al > kTargetTruePeakLin * 0.94 || ar > kTargetTruePeakLin * 0.94) {
                firstHot = i;
                break;
            }
        }
        const int catchSamples = std::clamp(firstHot, 1, 16);
        const double startGain = m_tpGain;
        const bool needDown = (targetGain < startGain);
        const double downStep = needDown
            ? (targetGain - startGain) / static_cast<double>(catchSamples)
            : 0.0;

        double g = startGain;
        for (int i = 0; i < frames; ++i) {
            if (needDown && i < catchSamples) {
                g = startGain + downStep * static_cast<double>(i + 1);
                if (g < targetGain) g = targetGain;
            } else if (g > targetGain) {
                g = targetGain;
            } else if (g < 1.0) {
                g = 1.0 - m_releaseCoef * (1.0 - g);
                if (g > 0.9999999) g = 1.0;
            }

            const double l64 = (std::isfinite(L[i]) ? static_cast<double>(L[i]) : 0.0) * g;
            const double r64 = (std::isfinite(R[i]) ? static_cast<double>(R[i]) : 0.0) * g;
            L[i] = static_cast<float>(std::clamp(l64, -kTargetTruePeakLin, kTargetTruePeakLin));
            R[i] = static_cast<float>(std::clamp(r64, -kTargetTruePeakLin, kTargetTruePeakLin));
        }
        m_tpGain = g;
    }

    /**
     * Mezcla final en 64-bit + TPDF Dither + noise shaping según profundidad
     * de bits del destino.
     * bitDepth: 16 (PCM_16_BIT), 24 (PCM_24_BIT_PACKED), 0/32 (float32: sin dither).
     *
     * Noise shaping (2º orden, error-feedback): v = y + dither - (2*e1 - e2);
     * out = Q(v) a la rejilla real aguas abajo; e = out - v. El error de
     * cuantización queda conformado por NTF (1-z^-1)^2: casi nada de ruido
     * en graves/medios, el resto desplazado a HF donde el oído es menos
     * sensible. Estado por canal; se limpia en bypass y en reset().
     */
    inline void mixAndDitherSample(double dryL, double dryR,
                                   double wetL, double wetR,
                                   double masterEnv, int bitDepth,
                                   float& outL, float& outR) noexcept {
        const double env = std::clamp(masterEnv, 0.0, 1.0);
        if (env <= 1.0e-6) {
            // Bypass bit-exacto + reinicio limpio del shaper: al reactivar
            // no se inyecta error residual de la sesión anterior.
            m_err1L = m_err2L = 0.0;
            m_err1R = m_err2R = 0.0;
            outL = static_cast<float>(dryL);
            outR = static_cast<float>(dryR);
            return;
        }
        double yL = dryL + env * (wetL - dryL);
        double yR = dryR + env * (wetR - dryR);

        if (bitDepth == 16 || bitDepth == 24) {
            const double lsb = (bitDepth == 16) ? (1.0 / 32768.0) : (1.0 / 8388608.0);
            // Rejilla real del conversor aguas abajo (omega_pcm_format.h:
            // lrintf(x*32767) a 16-bit, lrintf(x*8388607) a 24-bit). Cuantizar
            // aquí a esa rejilla hace exacta la conversión posterior: no hay
            // segunda cuantización que añada error sin conformar.
            const double step = (bitDepth == 16) ? (1.0 / 32767.0) : (1.0 / 8388607.0);
            yL = shapeSample(yL + tpdfUnit(m_rngL) * lsb, step, m_err1L, m_err2L);
            yR = shapeSample(yR + tpdfUnit(m_rngR) * lsb, step, m_err1R, m_err2R);
        }

        outL = static_cast<float>(std::clamp(yL, -1.0, 1.0));
        outR = static_cast<float>(std::clamp(yR, -1.0, 1.0));
    }

    // Un paso del shaper: realimenta el error de cuantización con NTF
    // (1-z^-1)^2 y cuantiza a la rejilla `step`. e1/e2 son el error de la
    // muestra anterior y la anterior a esa (en unidades lineales, double).
    static inline double shapeSample(double v, double step,
                                     double& e1, double& e2) noexcept {
        double shaped = v - (2.0 * e1 - e2);
        shaped = std::clamp(shaped, -1.0, 1.0);
        const double q = std::round(shaped / step) * step;
        e2 = e1;
        e1 = q - shaped;
        return q;
    }

private:
    static inline double nextUniform01(uint64_t& state) noexcept {
        // xorshift64* (determinístico, sin estado global, sin locks)
        uint64_t x = state;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        state = x;
        const uint64_t r = x * 0x2545F4914F6CDD1DULL;
        return static_cast<double>(r >> 11) * (1.0 / 9007199254740992.0); // [0, 1)
    }

    static inline double tpdfUnit(uint64_t& state) noexcept {
        const double u1 = nextUniform01(state) - 0.5;
        const double u2 = nextUniform01(state) - 0.5;
        return u1 + u2; // Triangular en [-1.0, +1.0)
    }

    double m_sampleRate  = 48000.0;
    double m_releaseCoef = 0.0;
    double m_tpGain      = 1.0;
    // Estado del noise shaper (error de cuantización, por canal).
    double m_err1L = 0.0, m_err2L = 0.0;
    double m_err1R = 0.0, m_err2R = 0.0;
    uint64_t m_rngL      = 0x9E3779B97F4A7C15ULL;
    uint64_t m_rngR      = 0xBF58476D1CE4E5B9ULL;
};

} // namespace ivanna::master
