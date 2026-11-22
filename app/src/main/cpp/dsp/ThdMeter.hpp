#pragma once
// ThdMeter.hpp — Medidor de THD+N, niveles y clipping (Punto 3 de la crítica
// externa a IVANNA, 2026-11-16: "no existen mediciones físicas en hardware
// real"). Propuesta 0022, coordinador del Plan Omnipotencia; decisión del
// Consejo Supremo 2026-11-16, ronda 1 unánime: proposer gpt-oss-120b,
// reviewer luna, arbiter mini — punto (A), mediciones físicas.
//
// Header-only, sin dependencias de Android: la misma matemática corre en
// host (CTest) y en el análisis de las grabaciones del teléfono. Método:
// ajuste por mínimos cuadrados de un seno a la frecuencia fundamental
// (para un número entero de ciclos en el buffer, seno y coseno son
// ortogonales y las amplitudes salen directas). La potencia de la
// fundamental es A^2/2; la potencia total es la media de x^2; el residuo
// (armónicos + ruido) es total − fundamental. THD+N = residuo/fundamental.
// No hay FFT ni ventanas: es exacto para tonos con ciclos enteros y robusto
// (el residuo incluye TODO lo que no es la fundamental, sin leakage).
//
// Uso: medir la distorsión real de la cadena (host o dispositivo). Un seno
// puro da THD+N ~ −140 dB (límite de float); un seno con 2º armónico a
// −40 dB da THD+N = −40 dB; un seno recortado (clipping) da THD alto y
// clippedSamples > 0. Ver test_thd_meter.cpp.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ivanna::measure {

struct ThdResult {
    double thdPlusNoiseDb = -200.0;  // 10*log10(residuo/fundamental); -200 = sin residuo
    double thdPercent = 0.0;         // 100 * sqrt(residuo/fundamental)
    double fundamentalDbfs = -200.0; // 20*log10(A / 1.0), A = amplitud de la fundamental
    double totalDbfs = -200.0;       // 20*log10(RMS total)
    double peakDbfs = -200.0;        // 20*log10(pico)
    double rmsDbfs = -200.0;         // 20*log10(RMS)
    std::uint32_t clippedSamples = 0;  // |x| >= 0.999f (a un LSB de full-scale)
    bool valid = false;              // false si no hay fundamental medible
};

namespace detail {
// Amplitud de la fundamental por mínimos cuadrados (seno + coseno).
// Requiere un número entero de ciclos en el buffer para que seno y coseno
// sean ortogonales; si no, el resultado sigue siendo válido pero con
// leakage (se documenta en el llamador).
inline double fundamentalAmplitude(const float* x, std::size_t n, double phaseStep,
                                   double* outSin, double* outCos) noexcept {
    double s = 0.0, c = 0.0;
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(x[i]);
        s += v * std::sin(phase);
        c += v * std::cos(phase);
        phase += phaseStep;
        if (phase >= 6.28318530717958647692) phase -= 6.28318530717958647692;
    }
    // Para ciclos enteros: a = 2/N * Σx·sin, b = 2/N * Σx·cos.
    const double a = 2.0 * s / static_cast<double>(n);
    const double b = 2.0 * c / static_cast<double>(n);
    if (outSin) *outSin = a;
    if (outCos) *outCos = b;
    return std::sqrt(a * a + b * b);
}
}  // namespace detail

// Mide THD+N y niveles de un buffer. `fundamentalHz` es la frecuencia del
// tono de prueba; `sampleRateHz` la tasa. El buffer debe contener un número
// entero de ciclos de la fundamental para una medida exacta (p. ej. 4800
// muestras a 48 kHz = 100 ciclos de 1 kHz). Si la fundamental es demasiado
// pequeña (< −120 dBFS) o el buffer es vacío, valid=false (no hay tono que
// medir; no es un error, es "sin señal").
inline ThdResult measureThdPlusNoise(const float* x, std::size_t n, double sampleRateHz,
                                     double fundamentalHz) noexcept {
    ThdResult r;
    if (!x || n == 0 || !(sampleRateHz > 0.0) || !(fundamentalHz > 0.0) ||
        fundamentalHz >= sampleRateHz * 0.5) {
        return r;
    }
    const double phaseStep = 6.28318530717958647692 * fundamentalHz / sampleRateHz;
    double totalPower = 0.0;
    double peak = 0.0;
    std::uint32_t clipped = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = static_cast<double>(x[i]);
        totalPower += v * v;
        const double a = v < 0.0 ? -v : v;
        if (a > peak) peak = a;
        if (a >= 0.999) ++clipped;
    }
    totalPower /= static_cast<double>(n);
    const double rms = std::sqrt(totalPower);
    r.totalDbfs = 20.0 * std::log10(rms + 1e-20);
    r.rmsDbfs = r.totalDbfs;
    r.peakDbfs = 20.0 * std::log10(peak + 1e-20);
    r.clippedSamples = clipped;
    const double amp = detail::fundamentalAmplitude(x, n, phaseStep, nullptr, nullptr);
    if (!(amp > 1e-6)) return r;  // sin fundamental medible (< −120 dBFS)
    r.fundamentalDbfs = 20.0 * std::log10(amp + 1e-20);
    const double fundPower = 0.5 * amp * amp;
    double residPower = totalPower - fundPower;
    if (residPower < 0.0) residPower = 0.0;  // redondeo: total >= fundamental
    const double ratio = residPower / fundPower;
    r.thdPlusNoiseDb = 10.0 * std::log10(ratio + 1e-20);
    r.thdPercent = 100.0 * std::sqrt(ratio);
    r.valid = true;
    return r;
}

// Generador de tono de prueba (para tests y para el protocolo de medición
// en dispositivo): seno puro de amplitud `amp` a `freqHz`, con un número
// entero de ciclos en `frames` (sin leakage). Si `harmonic2Db` es finito,
// añade un 2º armónico a ese nivel en dB relativos a la fundamental
// (para validar que el medidor lo detecta al nivel correcto).
inline void fillTestTone(float* x, std::size_t frames, double sampleRateHz, double freqHz,
                         float amp, double harmonic2Db = -1e9) noexcept {
    if (!x || frames == 0) return;
    const double step = 6.28318530717958647692 * freqHz / sampleRateHz;
    const double h2amp = std::isfinite(harmonic2Db)
        ? static_cast<double>(amp) * std::pow(10.0, harmonic2Db / 20.0) : 0.0;
    double phase = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        double v = static_cast<double>(amp) * std::sin(phase);
        if (h2amp != 0.0) v += h2amp * std::sin(2.0 * phase);
        x[i] = static_cast<float>(v);
        phase += step;
        if (phase >= 6.28318530717958647692) phase -= 6.28318530717958647692;
    }
}

}  // namespace ivanna::measure
