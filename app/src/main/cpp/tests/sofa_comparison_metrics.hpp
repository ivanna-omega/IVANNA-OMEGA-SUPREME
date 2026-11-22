// sofa_comparison_metrics.hpp — Metricas COMUNES de comparacion SOFA
// (Trabajador C, 2026-11-18). Las usan TODOS los trabajadores (A: modelo
// fisico de cabeza; B: propagacion de onda; C: verificacion) para reportar
// en el mismo formato. Si cambias una definicion aqui, cambia para todos:
// actualiza tambien SOFA_COMPARISON_METRICS.md y el script de referencia
// ~/workspace/analysis-ivanna/sofa_reference_kemar.py.
//
// REFERENCIA MEDIDA: MIT_KEMAR_normal_pinna.sofa (710 pos, 512 taps,
// 44.1 kHz, r=1.4 m) y su conversion fiel kemar.ihr1 (710 pos, 512 taps,
// 48 kHz). Validado en Python: |dITD| max 2.5 us, |dILD| max 0.01 dB,
// |dILD por banda| max 0.05 dB entre SOFA e IHR1. El test C++ carga el
// .ihr1 con HRTFBinLoader (sin HDF5 en host).
//
// CONVENCION: oido izquierdo = receiver 0 (y=+0.09 m, SOFA ListenerView
// (1,0,0), Up (0,0,1)). ITD = t_derecho - t_izquierdo, en microsegundos:
// positivo = la fuente esta a la izquierda (az SOFA +90 = izquierda).
// Azimut SOFA 0..355 se mapea a -180..180 para ORBE (az>180 -> az-360).
//
// METRICAS (todas sobre la HRIR / IR efectiva, por azimut, plano el=0):
//   ITD onset (PRIMARIO): primer cruce del 5% del pico, con interpolacion
//     lineal sub-muestra. Mide el frente de onda directo. La correlacion
//     cruzada plana SALTA de pico en MIT KEMAR (az=115: 814 us por xcorr
//     vs 492 us por onset; el onset es suave 612->591->569->544->517->492
//     us de 90 a 115 grados) por los reflejos de pinna; por eso xcorr es
//     solo SECUNDARIA (diagnostico).
//   ILD banda ancha (dB): 10*log10(E_L/E_R), energia total de la HRIR.
//   ILD por banda (dB): diferencia L-R de energia por banda de octava
//     (7 bandas, centros 250..16000 Hz), via FFT 4096.
//   Espectro de magnitud (dB por banda, por oido): 10*log10(energia de
//     banda). Es la H(f) del modelo vs la medida, por banda.
//   Distorsion espectral SD (dB): raiz de la media de
//     (20*log10|H_modelo| - 20*log10|H_medida|)^2 por banda, por oido.
//     SD_raw incluye el offset de ganancia global; SD_shape lo quita
//     (resta la media por oido/azimut) y mide solo la FORMA espectral.
//   Correlacion: maxima correlacion cruzada normalizada entre las IRs
//     del modelo y la medida (mismo oido), lag +-1 ms. 1.0 = identicas.
//
// CRITERIO "FISICA NUEVA VERIFICADA" (directiva de Esse 2026-11-18):
//   Una teoria es fisica nueva verificable solo si cumple (a)+(b)+(c):
//   (a) ecuaciones propias ESCRITAS (no solo descripcion verbal);
//   (b) al menos UNA prediccion cuantitativa DISTINTA de la fisica
//       existente (p. ej. un notch de pinna que la esfera rigida no
//       predice, o parallax de campo cercano + ITD de campo lejano con
//       la misma ecuacion), con su valor numerico;
//   (c) esa prediccion se COMPRUEBA contra SOFA o contra ORBE con un
//       error reportado en estas metricas.
//   Si no cumple (a)+(b)+(c): veredicto "no es fisica nueva
//   verificable, es re-derivacion". Si el equipo solo re-deriva la
//   fisica existente, el reporte final debe decirlo asi: "no se logro
//   fisica nueva, solo se re-derivo la existente". Sin inflar numeros.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace ivanna::sofa_compare {

// 7 bandas de octava [lo, hi) Hz, centros para el reporte.
inline constexpr int kNumBands = 7;
inline constexpr double kBandLo[kNumBands] = {125, 250, 500, 1000, 2000, 4000, 8000};
inline constexpr double kBandHi[kNumBands] = {250, 500, 1000, 2000, 4000, 8000, 16000};
inline constexpr int kBandCenterHz[kNumBands] = {250, 500, 1000, 2000, 4000, 8000, 16000};

// FFT radix-2 iterativa (N potencia de 2), in-place, para bandLevels().
inline void fft4096(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * M_PI / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// Energia por banda de octava (dB re 1.0), FFT 4096 con zero-padding.
inline void bandLevels(const float* x, std::size_t n, double sr, double out[kNumBands]) {
    constexpr std::size_t kFft = 4096;
    std::vector<std::complex<double>> a(kFft, {0.0, 0.0});
    for (std::size_t i = 0; i < n && i < kFft; ++i) a[i] = {static_cast<double>(x[i]), 0.0};
    fft4096(a);
    for (int b = 0; b < kNumBands; ++b) {
        double p = 0.0;
        for (std::size_t k = 0; k <= kFft / 2; ++k) {
            const double f = static_cast<double>(k) * sr / static_cast<double>(kFft);
            if (f >= kBandLo[b] && f < kBandHi[b]) p += std::norm(a[k]);
        }
        out[b] = 10.0 * std::log10(p + 1e-30);
    }
}

// Onset: primer cruce del 5% del pico, interpolacion lineal sub-muestra.
// Devuelve el instante en muestras (double). NaN si no hay pico.
inline double onsetSamples(const float* x, std::size_t n) {
    double peak = 0.0;
    for (std::size_t i = 0; i < n; ++i) peak = std::max(peak, std::abs(static_cast<double>(x[i])));
    if (peak <= 0.0 || !std::isfinite(peak)) return std::nan("");
    const double thr = 0.05 * peak;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = std::abs(static_cast<double>(x[i]));
        if (v >= thr) {
            if (i == 0) return 0.0;
            const double prev = std::abs(static_cast<double>(x[i - 1]));
            const double frac = (thr - prev) / (v - prev + 1e-30);
            return static_cast<double>(i - 1) + std::min(1.0, std::max(0.0, frac));
        }
    }
    return std::nan("");
}

// ITD (us) = t_R - t_L por onset. Positivo = izquierda lidera.
inline double itdOnsetUs(const float* l, const float* r, std::size_t n, double sr) {
    const double ol = onsetSamples(l, n);
    const double orr = onsetSamples(r, n);
    if (!std::isfinite(ol) || !std::isfinite(orr)) return std::nan("");
    return (orr - ol) / sr * 1e6;
}

// ITD (us) por correlacion cruzada plana, lag +-1 ms, pico parabolico.
// SECUNDARIA: salta de pico por reflejos de pinna (ver cabecera).
inline double itdXcorrUs(const float* l, const float* r, std::size_t n, double sr) {
    const int maxLag = static_cast<int>(0.001 * sr);
    double best = -1.0;
    int bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double c = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const long j = static_cast<long>(i) + lag;
            if (j >= 0 && j < static_cast<long>(n))
                c += static_cast<double>(r[j]) * static_cast<double>(l[i]);
        }
        if (c > best) { best = c; bestLag = lag; }
    }
    // Refinado parabolico con los vecinos (si existen).
    auto xcorrAt = [&](int lag) {
        double c = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const long j = static_cast<long>(i) + lag;
            if (j >= 0 && j < static_cast<long>(n))
                c += static_cast<double>(r[j]) * static_cast<double>(l[i]);
        }
        return c;
    };
    double lag = static_cast<double>(bestLag);
    if (bestLag > -maxLag && bestLag < maxLag) {
        const double a = xcorrAt(bestLag - 1), b = best, c0 = xcorrAt(bestLag + 1);
        const double den = a - 2.0 * b + c0;
        if (den != 0.0) lag += std::min(1.0, std::max(-1.0, 0.5 * (a - c0) / den));
    }
    return lag / sr * 1e6;
}

struct HrirMetrics {
    double itdOnsetUs = std::nan("");
    double itdXcorrUs = std::nan("");
    double ildBroadbandDb = std::nan("");
    double magL[kNumBands] = {};
    double magR[kNumBands] = {};
    double ildBandDb[kNumBands] = {};
    double peakAbs = 0.0;
    double energyL = 0.0, energyR = 0.0;
    bool valid = false;
};

// Todas las metricas de un par de IRs (mismo sr, misma longitud).
inline HrirMetrics computeMetrics(const float* l, const float* r, std::size_t n, double sr) {
    HrirMetrics m;
    m.itdOnsetUs = itdOnsetUs(l, r, n, sr);
    m.itdXcorrUs = itdXcorrUs(l, r, n, sr);
    double eL = 0.0, eR = 0.0, pk = 0.0;
    bool finite = true;
    for (std::size_t i = 0; i < n; ++i) {
        const double dl = l[i], dr = r[i];
        if (!std::isfinite(dl) || !std::isfinite(dr)) finite = false;
        eL += dl * dl;
        eR += dr * dr;
        pk = std::max(pk, std::max(std::abs(dl), std::abs(dr)));
    }
    m.energyL = eL;
    m.energyR = eR;
    m.peakAbs = pk;
    m.ildBroadbandDb = (eR > 0.0) ? 10.0 * std::log10(eL / eR) : std::nan("");
    bandLevels(l, n, sr, m.magL);
    bandLevels(r, n, sr, m.magR);
    for (int b = 0; b < kNumBands; ++b) m.ildBandDb[b] = m.magL[b] - m.magR[b];
    m.valid = finite && std::isfinite(m.itdOnsetUs) && std::isfinite(m.ildBroadbandDb) && pk > 0.0;
    return m;
}

// Distorsion espectral SD (dB) entre dos espectros por banda.
// meanRemove=true -> solo forma (quita el offset de ganancia global).
inline double spectralDistortionDb(const double a[kNumBands], const double b[kNumBands],
                                   bool meanRemove) {
    double ma = 0.0, mb = 0.0;
    if (meanRemove) {
        for (int k = 0; k < kNumBands; ++k) { ma += a[k]; mb += b[k]; }
        ma /= kNumBands;
        mb /= kNumBands;
    }
    double s = 0.0;
    for (int k = 0; k < kNumBands; ++k) {
        const double d = (a[k] - ma) - (b[k] - mb);
        s += d * d;
    }
    return std::sqrt(s / kNumBands);
}

// Maxima correlacion cruzada normalizada entre dos IRs (mismo oido),
// lag +-1 ms. 1.0 = identicas salvo escala y retardo.
inline double maxNormXcorr(const float* a, const float* b, std::size_t n, double sr) {
    double ea = 0.0, eb = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        ea += static_cast<double>(a[i]) * a[i];
        eb += static_cast<double>(b[i]) * b[i];
    }
    if (ea <= 0.0 || eb <= 0.0) return std::nan("");
    const int maxLag = static_cast<int>(0.001 * sr);
    double best = -1.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        double c = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const long j = static_cast<long>(i) + lag;
            if (j >= 0 && j < static_cast<long>(n))
                c += static_cast<double>(a[i]) * static_cast<double>(b[j]);
        }
        best = std::max(best, c / std::sqrt(ea * eb));
    }
    return best;
}

}  // namespace ivanna::sofa_compare
