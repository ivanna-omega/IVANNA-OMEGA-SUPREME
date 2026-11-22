// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// OmniFieldRender.hpp — Diseñador FIR + renderer por muestra de OMNI-FIELD.
//
// Por cada fuente virtual (stem) y oído:
//   FIR de fase mínima de 64 taps, diseñado del campo SIREN
//   (OmniFieldSiren.hpp) por cepstro real -> línea de retardo
//   fraccionaria (interpolación cúbica Catmull-Rom de 4 puntos).
// La FFT solo se usa para DISEÑAR el filtro (cepstro) a tasa de control;
// el camino de audio es por muestra, sin bloques ni FFT.
//
// Diferencias deliberadas respecto al INAF original (ver reporte):
//  - 64 taps en vez de 96 (coste por muestra ~1/3 menor; la energía de
//    la HRTF de fase mínima más allá del tap 64 es despreciable y la
//    ventana Hann del último cuarto ya la atenúa en el diseño de 96).
//  - Retardo fraccionario por interpolación cúbica de 4 puntos en vez
//    de sinc·Kaiser de 32 puntos con tabla polifásica: ~8× menos
//    multiplicaciones por muestra y sin tabla; a cambio, atenuación
//    leve en alta frecuencia del camino retardado (el FIR ya es de
//    fase mínima y el retardo es sub-muestra en su mayor parte).
//  - Los coeficientes se guardan en orden natural h[0..63] (h[0] = tap
//    actual) en vez de invertidos para producto escalar; la convolución
//    es matemáticamente la misma.
//  - Suavizado de 3 puntos en log-f por encima de 8 kHz (ver design()).
//  - BinauralFir::delayL/R son retardos RELATIVOS al centro de la
//    cabeza (pueden ser negativos, como en el INAF); StemRenderer añade
//    una latencia base fija ceil(a/c·sr)+2 muestras (la misma fórmula
//    que el dInt_ del INAF) para que el retardo efectivo nunca sea
//    negativo. El ITD (diferencia L−R) se conserva exacto.
//
// Contrato: header-only, C++17, noexcept en el camino de audio, cero
// allocations en design() ni en process() (todo el scratch son arrays
// miembro; la FFT se (re)construye solo en prepare()).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "OmniFieldSiren.hpp"
#include "fft_radix2.hpp"

namespace ivanna {
namespace omnifield {

// FIR binaural de una fuente virtual: 64 taps de fase mínima por oído
// en orden natural, más el retardo relativo de cada oído en muestras
// (SirenField::delaysSec × sampleRate; puede ser negativo).
struct BinauralFir {
    static constexpr int kTaps = 64;
    // Taps y retardos en double (F2/486): son coeficientes de señal.
    double l[kTaps];
    double r[kTaps];
    double delayL = 0.0;
    double delayR = 0.0;
};

struct FirDesigner {
    static constexpr int kGrid = 32;      // puntos log-f del campo
    static constexpr int kFftMax = 1024;  // NFFT máxima (sr > 50 kHz)

    void prepare(float sampleRate) noexcept {
        sr_ = (std::isfinite(sampleRate) && sampleRate >= 8000.0f && sampleRate <= 192000.0f)
                  ? sampleRate
                  : 48000.0f;
        nfft_ = (sr_ > 50000.0f) ? 1024 : 512;
        fft_ = FFTRadix2d(nfft_);  // única allocation: en prepare(), nunca en design()
        prepared_ = true;
    }

    // Diseña el FIR binaural para (dir, rM). Devuelve false si algo no
    // es finito (el llamador hace fail-closed y conserva el FIR anterior).
    bool design(const float dir[3], float rM, BinauralFir& out) noexcept {
        if (!prepared_ || dir == nullptr) return false;
        for (int c = 0; c < 3; ++c) {
            if (!std::isfinite(dir[c])) return false;
        }
        if (!std::isfinite(rM)) return false;

        // 1) Muestreo del campo en K=32 puntos log-f (200 Hz–16 kHz).
        for (int k = 0; k < kGrid; ++k) freqs_[k] = detail::gridFreqHz(k, kGrid);
        SirenField::evalDbGrid(dir, rM, freqs_, kGrid, dbL_, dbR_);
        for (int k = 0; k < kGrid; ++k) {
            if (!std::isfinite(dbL_[k]) || !std::isfinite(dbR_[k])) return false;
        }

        // 2) REFINAMIENTO OBLIGATORIO: suavizado de 3 puntos en log-f por
        //    encima de 8 kHz. N1 pierde contra IDW-3 con 2.17 dB en
        //    8–16 kHz (rugo de alta frecuencia del SIREN); el promedio
        //    (k−1, k, k+1) sobre las muestras ORIGINALES (sin cascada)
        //    lo mitiga sin tocar la banda grave/media. El último punto
        //    (k=31, sin vecino superior) usa promedio de 2 puntos.
        //    La rejilla SIREN es float (pesos float del .inc); el cast a
        //    double es exacto y el cepstro corre en double (F2/486).
        for (int k = 0; k < kGrid; ++k) {
            smL_[k] = static_cast<double>(dbL_[k]);
            smR_[k] = static_cast<double>(dbR_[k]);
            if (freqs_[k] > 8000.0f && k >= 1) {
                if (k + 1 < kGrid) {
                    smL_[k] = (static_cast<double>(dbL_[k - 1]) + static_cast<double>(dbL_[k]) + static_cast<double>(dbL_[k + 1])) * (1.0 / 3.0);
                    smR_[k] = (static_cast<double>(dbR_[k - 1]) + static_cast<double>(dbR_[k]) + static_cast<double>(dbR_[k + 1])) * (1.0 / 3.0);
                } else {
                    smL_[k] = (static_cast<double>(dbL_[k - 1]) + static_cast<double>(dbL_[k])) * 0.5;
                    smR_[k] = (static_cast<double>(dbR_[k - 1]) + static_cast<double>(dbR_[k])) * 0.5;
                }
            }
        }

        // 3) Mapa bin FFT -> punto de rejilla (interpolación lineal en
        //    log f), igual que el INAF (K=64 allí, K=32 aquí).
        const int H = nfft_ / 2;
        const float lf0 = std::log(inaf_weights::kFMinHz);
        const float lf1 = std::log(inaf_weights::kFMaxHz);
        for (int j = 0; j <= H; ++j) {
            const float f = std::max((float)j * sr_ / (float)nfft_, inaf_weights::kFMinHz);
            const float pos = detail::clampf(
                (std::log(std::min(f, inaf_weights::kFMaxHz)) - lf0) / (lf1 - lf0), 0.0f, 1.0f) *
                (float)(kGrid - 1);
            const int i = std::min((int)pos, kGrid - 2);
            binIdx_[j] = i;
            binW_[j] = static_cast<double>(pos - static_cast<float>(i));
        }

        // 4) Cepstro real -> fase mínima -> 64 taps + Hann último cuarto.
        if (!designEar(smL_, out.l)) return false;
        if (!designEar(smR_, out.r)) return false;

        // 5) Retardos relativos en muestras.
        float tauL = 0.0f, tauR = 0.0f;
        SirenField::delaysSec(dir, rM, tauL, tauR);
        out.delayL = static_cast<double>(tauL) * sr_;
        out.delayR = static_cast<double>(tauR) * sr_;
        if (!std::isfinite(out.delayL) || !std::isfinite(out.delayR)) return false;
        return true;
    }

private:
    // Un oído: dB de rejilla -> FIR de fase mínima de 64 taps (orden
    // natural). Replica designEar del INAF (cepstro real con la misma
    // secuencia de signos FFT -1/+1/-1), con L=64 en vez de 96.
    // Cepstro real -> fase mínima en double (F2/486; la FFT es FFTRadix2d).
    bool designEar(const double* db, double* hrOut) noexcept {
        constexpr double kLn10Over20 = 0.11512925464970229;
        constexpr double kPi = 3.14159265358979323846;
        const int N = nfft_, H = N / 2;
        for (int j = 0; j <= H; ++j) {
            const double v = db[binIdx_[j]] * (1.0 - binW_[j]) + db[binIdx_[j] + 1] * binW_[j];
            re_[j] = v * kLn10Over20;
            im_[j] = 0.0;
        }
        for (int j = 1; j < H; ++j) { re_[N - j] = re_[j]; im_[N - j] = 0.0; }
        fft_.transform(re_, im_, -1);                  // cepstro real (sin normalizar)
        const double inv = 1.0 / static_cast<double>(N);
        re2_[0] = re_[0] * inv; im2_[0] = 0.0;
        for (int n = 1; n < H; ++n) { re2_[n] = 2.0 * re_[n] * inv; im2_[n] = 0.0; }
        re2_[H] = re_[H] * inv; im2_[H] = 0.0;
        for (int n = H + 1; n < N; ++n) { re2_[n] = 0.0; im2_[n] = 0.0; }
        fft_.transform(re2_, im2_, +1);                // log H_min (complejo)
        for (int j = 0; j < N; ++j) {
            const double m = std::exp(re2_[j]), ph = im2_[j];
            re_[j] = m * std::cos(ph);
            im_[j] = m * std::sin(ph);
        }
        fft_.transform(re_, im_, -1);
        const int L = BinauralFir::kTaps, tail = L / 4, t0 = L - tail;
        for (int k = 0; k < L; ++k) {
            double h = re_[k] * inv;
            if (k >= t0) h *= 0.5 * (1.0 + std::cos(kPi * static_cast<double>(k - t0) / static_cast<double>(tail)));
            if (!std::isfinite(h)) return false;
            hrOut[k] = h;                              // orden natural (INAF: invertido)
        }
        return true;
    }

    bool prepared_ = false;
    float sr_ = 48000.0f;
    int nfft_ = 512;
    FFTRadix2d fft_{512};
    // Scratch miembro (nada de new/malloc en design()).
    // freqs_/dbL_/dbR_ son float: la rejilla SIREN es float (pesos del .inc).
    float freqs_[kGrid] = {};
    float dbL_[kGrid] = {};
    float dbR_[kGrid] = {};
    double smL_[kGrid] = {};
    double smR_[kGrid] = {};
    double re_[kFftMax] = {};
    double im_[kFftMax] = {};
    double re2_[kFftMax] = {};
    double im2_[kFftMax] = {};
    int binIdx_[kFftMax / 2 + 1] = {};
    double binW_[kFftMax / 2 + 1] = {};
};

// Renderer por muestra de un stem mono -> binaural.
// FIR 64 taps por oído + retardo fraccionario (Catmull-Rom de 4 puntos)
// sobre anillo prealocado de 512 floats/oído. Cero alloc en process().
class StemRenderer {
public:
    void prepare(float sr) noexcept {
        sr_ = (std::isfinite(sr) && sr >= 8000.0f && sr <= 192000.0f) ? sr : 48000.0f;
        // Latencia base: el retardo relativo mínimo es -a/c (la fuente no
        // puede estar más cerca del oído que r-a), así que con esta base
        // el retardo efectivo nunca es negativo. Misma fórmula que el
        // dInt_ del INAF: ceil(a/c·sr)+2 (15 muestras @48 kHz).
        baseDelay_ = (int)std::ceil(nearfield::kHeadRadiusM / nearfield::kSoundSpeedMps * sr_) + 2;
        reset();
    }

    // Limpia solo el historial (anillos y posiciones); conserva el FIR.
    void reset() noexcept {
        for (int i = 0; i < BinauralFir::kTaps; ++i) xRing_[i] = 0.0;
        for (int i = 0; i < kDelayRing; ++i) { dlL_[i] = 0.0; dlR_[i] = 0.0; }
        xPos_ = 0;
        dlPos_ = 0;
    }

    // Copia el FIR; si algún coeficiente o retardo no es finito, el FIR
    // se pone a cero (fail-closed: silencio en vez de NaN).
    void setFir(const BinauralFir& fir) noexcept {
        bool ok = std::isfinite(fir.delayL) && std::isfinite(fir.delayR);
        for (int k = 0; ok && k < BinauralFir::kTaps; ++k) {
            ok = std::isfinite(fir.l[k]) && std::isfinite(fir.r[k]);
        }
        if (!ok) {
            for (int k = 0; k < BinauralFir::kTaps; ++k) { fir_.l[k] = 0.0; fir_.r[k] = 0.0; }
            fir_.delayL = 0.0;
            fir_.delayR = 0.0;
            return;
        }
        fir_ = fir;
    }

    // Acepta cualquier n > 0; no-ops con punteros nulos o n <= 0.
    // Muestras de entrada no finitas se tratan como 0 (no envenenan
    // los anillos). Latencia total: baseDelay_ + retardo relativo.
    // Núcleo double (F2/486): convolución FIR + retardo fraccionario.
    void processMono(const double* in, double* outL, double* outR, int n) noexcept {
        if (in == nullptr || outL == nullptr || outR == nullptr || n <= 0) return;
        constexpr int L = BinauralFir::kTaps;
        for (int i = 0; i < n; ++i) {
            double x = in[i];
            if (!std::isfinite(x)) x = 0.0;
            xRing_[xPos_] = x;
            double yL = 0.0, yR = 0.0;
            for (int m = 0; m < L; ++m) {
                int idx = xPos_ - m;
                if (idx < 0) idx += L;
                const double xm = xRing_[idx];
                yL += fir_.l[m] * xm;
                yR += fir_.r[m] * xm;
            }
            xPos_ = (xPos_ + 1 >= L) ? 0 : xPos_ + 1;

            dlL_[dlPos_] = yL;
            dlR_[dlPos_] = yR;
            outL[i] = fracDelay(dlL_, static_cast<double>(baseDelay_) + fir_.delayL);
            outR[i] = fracDelay(dlR_, static_cast<double>(baseDelay_) + fir_.delayR);
            dlPos_ = (dlPos_ + 1) & (kDelayRing - 1);
        }
    }

    // Envoltorio float (compat legacy): convierte por scratch y delega
    // en el núcleo double.
    void processMono(const float* in, float* outL, float* outR, int n) noexcept {
        if (in == nullptr || outL == nullptr || outR == nullptr || n <= 0) return;
        const std::size_t un = static_cast<std::size_t>(n);
        if (wrapIn_.size() < un) { wrapIn_.resize(un); wrapL_.resize(un); wrapR_.resize(un); }
        for (int i = 0; i < n; ++i)
            wrapIn_[i] = std::isfinite(in[i]) ? static_cast<double>(in[i]) : 0.0;
        processMono(wrapIn_.data(), wrapL_.data(), wrapR_.data(), n);
        for (int i = 0; i < n; ++i) {
            outL[i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
            outR[i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
        }
    }

private:
    static constexpr int kDelayRing = 512;  // potencia de 2 (máscara)

    // Retardo fraccionario D (muestras, ya con base) por Catmull-Rom de
    // 4 puntos sobre el anillo `dl`, cuya muestra actual está en dlPos_.
    // D se clampa a [2, 400]: >= 2 garantiza que el punto p0 (una
    // muestra más nueva que p1) ya existe en el anillo.
    double fracDelay(const double* dl, double d) const noexcept {
        const double dc = std::clamp(d, 2.0, 400.0);
        const int di = static_cast<int>(dc);  // parte entera (>= 2)
        const double mu = dc - static_cast<double>(di);  // parte fraccionaria [0, 1)
        const int p = static_cast<int>(dlPos_);
        const double p0 = dl[(p - di + 1 + kDelayRing * 2) & (kDelayRing - 1)];
        const double p1 = dl[(p - di + kDelayRing * 2) & (kDelayRing - 1)];
        const double p2 = dl[(p - di - 1 + kDelayRing * 2) & (kDelayRing - 1)];
        const double p3 = dl[(p - di - 2 + kDelayRing * 2) & (kDelayRing - 1)];
        const double mu2 = mu * mu, mu3 = mu2 * mu;
        double v = 0.5 * ((2.0 * p1) + (-p0 + p2) * mu +
                          (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * mu2 +
                          (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * mu3);
        return std::isfinite(v) ? v : 0.0;
    }

    float sr_ = 48000.0f;
    int baseDelay_ = 15;
    BinauralFir fir_{};
    double xRing_[BinauralFir::kTaps] = {};
    int xPos_ = 0;
    double dlL_[kDelayRing] = {};
    double dlR_[kDelayRing] = {};
    uint32_t dlPos_ = 0;
    // Scratch del envoltorio float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapIn_{}, wrapL_{}, wrapR_{};
};

}  // namespace omnifield
}  // namespace ivanna
