#pragma once
// fused_measure.hpp — Medición THD+N en DOBLE PRECISIÓN para la Fusión
// Magistral (2026-11-18).
//
// Método: el mismo que dsp/ThdMeter.hpp (ajuste por mínimos cuadrados de un
// seno a la fundamental; THD+N = (potencia total − fundamental) /
// fundamental; exacto para un número entero de ciclos, sin FFT ni ventanas),
// pero TODO en double (entrada, acumulación y resultado). El medidor float
// tiene su piso en ~−140 dB (límite de float32); este medidor baja hasta el
// piso de double (~−300 dB), que es lo que exige la meta de −200 dB.
//
// Además: prueba de nulidad (null test) — resta la entrada de la salida y
// mide el residuo. En neutro la cadena fusionada es bit-transparente, así
// que el residuo es EXACTAMENTE 0 (bitExact=true). Cualquier residuo > 0
// es una desviación real y se reporta con su número, sin fabricar nada.
//
// Uso: tono de 1 kHz a −3 dBFS, 48 kHz, número entero de ciclos
// (p. ej. 48000 muestras = 1000 ciclos). Medir por operador (neutro) y la
// cadena completa (neutro y con procesado activo a punto de operación
// definido). El número "engaged" (p. ej. HOLOS comprimiendo) lo domina el
// procesado intencional, no la aritmética — se reporta por separado y la
// Fase B −102.79 dB es el ancla que no se puede degradar.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ivanna::fused::measure {

struct ThdDoubleResult {
    double thdPlusNoiseDb = -320.0;  // 10*log10(residuo/fundamental); -320 = sin residuo medible
    double thdPercent = 0.0;
    double fundamentalDbfs = -320.0;
    double totalDbfs = -320.0;
    double peakDbfs = -320.0;
    double rmsDbfs = -320.0;
    std::uint32_t clippedSamples = 0;
    bool valid = false;
};

struct NullResult {
    double residualDb = -320.0;  // 20*log10(RMS residuo / RMS señal); -320 = residuo cero
    double residualRms = 0.0;
    double signalRms = 0.0;
    double maxAbsDiff = 0.0;
    bool bitExact = false;  // residuo exactamente 0.0
    bool valid = false;
};

namespace detail {
inline double fundamentalAmplitudeD(const double* x, std::size_t n, double phaseStep) noexcept {
    double s = 0.0, c = 0.0, phase = 0.0;
    constexpr double kTwoPi = 6.28318530717958647692;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[i];
        s += v * std::sin(phase);
        c += v * std::cos(phase);
        phase += phaseStep;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
    const double a = 2.0 * s / static_cast<double>(n);
    const double b = 2.0 * c / static_cast<double>(n);
    return std::sqrt(a * a + b * b);
}
}  // namespace detail

// Generador de tono en double (ciclos enteros si frames = k * sr / freq).
inline void fillToneDouble(double* x, std::size_t frames, double sampleRateHz,
                           double freqHz, double amp, double harmonic2Db = -1e9) noexcept {
    if (!x || frames == 0) return;
    constexpr double kTwoPi = 6.28318530717958647692;
    const double step = kTwoPi * freqHz / sampleRateHz;
    const double h2amp = std::isfinite(harmonic2Db)
        ? amp * std::pow(10.0, harmonic2Db / 20.0) : 0.0;
    double phase = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        double v = amp * std::sin(phase);
        if (h2amp != 0.0) v += h2amp * std::sin(2.0 * phase);
        x[i] = v;
        phase += step;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
}

// THD+N en double. El buffer debe contener un número entero de ciclos.
// Método (v2, 2026-11-18): ajuste por mínimos cuadrados de la fundamental
// (seno+coseno, suma compensada de Kahan) y luego residuo DIRECTO por
// muestra (x − fundamental ajustada), NO la diferencia (total −
// fundamental). La diferencia de dos potencias grandes casi iguales
// cancela catastróficamente y dejaba el piso del medidor en ~−130 dB a
// 48000 muestras; el residuo directo baja el piso hasta el piso de double
// (~−310 dB), que es lo que exige la meta de −200 dB.
inline ThdDoubleResult measureThdDouble(const double* x, std::size_t n, double sampleRateHz,
                                        double fundamentalHz) noexcept {
    ThdDoubleResult r;
    if (!x || n == 0 || !(sampleRateHz > 0.0) || !(fundamentalHz > 0.0) ||
        fundamentalHz >= sampleRateHz * 0.5) {
        return r;
    }
    constexpr double kTwoPi = 6.28318530717958647692;
    constexpr double kFloor = 1e-32;  // piso de medida en double (~−320 dB)
    const double phaseStep = kTwoPi * fundamentalHz / sampleRateHz;
    // Pase 1: ajuste de la fundamental + potencia total (suma de Kahan).
    double s = 0.0, c = 0.0, cs = 0.0, cc = 0.0;
    double totalPower = 0.0, cTot = 0.0, peak = 0.0;
    std::uint32_t clipped = 0;
    double phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[i];
        const double yS = v * std::sin(phase) - cs;
        const double tS = s + yS; cs = (tS - s) - yS; s = tS;
        const double yC = v * std::cos(phase) - cc;
        const double tC = c + yC; cc = (tC - c) - yC; c = tC;
        const double yT = v * v - cTot;
        const double tT = totalPower + yT; cTot = (tT - totalPower) - yT; totalPower = tT;
        const double a = v < 0.0 ? -v : v;
        if (a > peak) peak = a;
        if (a >= 0.999999) ++clipped;
        phase += phaseStep;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
    totalPower /= static_cast<double>(n);
    const double rms = std::sqrt(totalPower);
    r.totalDbfs = 20.0 * std::log10(rms + 1e-32);
    r.rmsDbfs = r.totalDbfs;
    r.peakDbfs = 20.0 * std::log10(peak + 1e-32);
    r.clippedSamples = clipped;
    const double aSin = 2.0 * s / static_cast<double>(n);
    const double bCos = 2.0 * c / static_cast<double>(n);
    const double amp = std::sqrt(aSin * aSin + bCos * bCos);
    if (!(amp > 1e-9)) return r;  // sin fundamental medible
    r.fundamentalDbfs = 20.0 * std::log10(amp + 1e-32);
    const double fundPower = 0.5 * amp * amp;
    // Pase 2: residuo directo por muestra.
    double residPower = 0.0, cRes = 0.0;
    phase = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double fund = aSin * std::sin(phase) + bCos * std::cos(phase);
        const double d = x[i] - fund;
        const double yR = d * d - cRes;
        const double tR = residPower + yR; cRes = (tR - residPower) - yR; residPower = tR;
        phase += phaseStep;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
    residPower /= static_cast<double>(n);
    const double ratio = residPower / fundPower;
    r.thdPlusNoiseDb = 10.0 * std::log10(ratio + kFloor);
    r.thdPercent = 100.0 * std::sqrt(ratio);
    r.valid = true;
    return r;
}

// Prueba de nulidad: residuo = salida − entrada. En neutro debe ser 0.
inline NullResult nullTest(const double* in, const double* out, std::size_t n) noexcept {
    NullResult r;
    if (!in || !out || n == 0) return r;
    double sigPower = 0.0, resPower = 0.0, maxDiff = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = out[i] - in[i];
        const double a = d < 0.0 ? -d : d;
        if (a > maxDiff) maxDiff = a;
        sigPower += in[i] * in[i];
        resPower += d * d;
    }
    sigPower /= static_cast<double>(n);
    resPower /= static_cast<double>(n);
    r.signalRms = std::sqrt(sigPower);
    r.residualRms = std::sqrt(resPower);
    r.maxAbsDiff = maxDiff;
    r.bitExact = (resPower == 0.0);
    if (r.bitExact) {
        r.residualDb = -320.0;
    } else if (r.signalRms > 0.0) {
        r.residualDb = 20.0 * std::log10(r.residualRms / r.signalRms + 1e-32);
    } else {
        r.residualDb = 20.0 * std::log10(r.residualRms + 1e-32);
    }
    r.valid = true;
    return r;
}

inline double rmsDb(const double* x, std::size_t n) noexcept {
    if (!x || n == 0) return -320.0;
    double p = 0.0;
    for (std::size_t i = 0; i < n; ++i) p += x[i] * x[i];
    p /= static_cast<double>(n);
    return 20.0 * std::log10(std::sqrt(p) + 1e-32);
}

inline double peakAbs(const double* x, std::size_t n) noexcept {
    if (!x || n == 0) return 0.0;
    double m = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double a = x[i] < 0.0 ? -x[i] : x[i];
        if (a > m) m = a;
    }
    return m;
}

inline bool isFiniteBuffer(const double* x, std::size_t n) noexcept {
    if (!x) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (!std::isfinite(x[i])) return false;
    return true;
}

}  // namespace ivanna::fused::measure
