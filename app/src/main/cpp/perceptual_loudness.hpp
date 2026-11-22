#pragma once
// perceptual_loudness.hpp — Perceptual Optimizer (fase 1): medidor de
// loudness K-weighted real (ITU-R BS.1770) + normalización automática.
//
// POR QUÉ EXISTE ESTE ARCHIVO:
//   `audio_control_plane.hpp` ya declaraba `output_lufs` (atomic<float>,
//   default -23.f) pero NADIE lo escribía nunca — quedaba fijo en su
//   default para siempre. Lo que el kernel evolutivo llamaba "loudness"
//   (`PerceptualCues::L` en biquad_envelope_bank.hpp) es en realidad una
//   suma de envolventes multibanda rectificadas — una métrica de energía
//   razonable para transientes/GA, pero no loudness perceptual real (sin
//   K-weighting, sin gating, sin escala LUFS). Ninguna de las dos cosas
//   es "falsa" — pero ninguna es lo que el nombre "Perceptual Optimizer"
//   promete. Este archivo construye la pieza que faltaba: un medidor de
//   loudness K-weighted real, y lo usa para normalizar el volumen de
//   salida hacia un target (-14 LUFS, el estándar de streaming), en vez
//   de dejar que el volumen percibido dependa del mastering original de
//   cada archivo.
//
// ALCANCE v1:
//   - K-weighting real (RBJ biquad cookbook, coeficientes derivados de
//     las frecuencias/ganancias/Q documentadas en BS.1770-4: high-shelf
//     ~1681.9Hz +4dB, high-pass ~38.13Hz Q=0.5003) — funcionalmente
//     correcto, no bit-exacto con el filtro de referencia oficial.
//   - Integración de 400ms (loudness "momentary" de la spec) vía media
//     móvil exponencial equivalente, no ventana rectangular exacta.
//   - Sin gating de silencio (el gating de -70/-10 LUFS de la spec
//     completa queda fuera de v1 — no cambia el resultado en música con
//     contenido continuo, que es el caso de uso real aquí).
//   - Trim de ganancia automático: se mueve como máximo 0.5 dB POR
//     SEGUNDO (no por bloque: a 150 bloques/s un paso "pequeño" por
//     bloque sigue siendo 15 dB/s) y solo fuera de una zona muerta de
//     ±0.5 dB alrededor del target, para que nunca "bombee" ni se note
//     como un efecto, solo como una corrección lenta de volumen
//     percibido entre temas. FIX 5 (v2.5.465, reproductor tronando +
//     volumen sube/baja, 2026-11-18): la ley vieja daba hasta 0.1 dB
//     POR BLOQUE sin zona muerta, sobre LUFS momentáneo (EMA 400 ms,
//     sin gating): en el BridgePlayer (bloques de 2048 frames) eso es
//     hasta ~4.7 dB/s a 96 kHz / ~2.3 dB/s a 48 kHz de ganancia
//     persiguiendo la dinámica propia de la música (estrofa→estribillo
//     → el trim baja; intro queda → el trim sube), aplicada además
//     con un suavizado rápido (0.001/muestra ≈ 10–21 ms) en serie con
//     el runtimeMul del ADE (target_gain 0.5–1.0), el compresor
//     multibanda y el SafetyLimiter. Esa es la causa del "volumen
//     sube y baja" con el módulo desinstalado (Ruta A pura). El trim
//     sigue convergiendo al mismo target (-14 LUFS) en régimen — el
//     arreglo no añade ganancia neta (Matched-LUFS 0.0 dB) — solo
//     deja de cabalgar la música dentro del tema.

#include <cmath>
#include <algorithm>

namespace ivanna {

// F2/486 (2026-11-20) — MOTOR DOUBLE: plantillas sobre el tipo escalar.
// `Biquad2` / `LoudnessMeter` siguen siendo las instancias float
// (llamadores existentes sin cambio) y `Biquad2d` / `LoudnessMeterd` son
// las instancias double 64-bit para el camino de señal de la FusedChain.
template <typename T = float>
struct Biquad2T {
    T b0 = T(1), b1 = T(0), b2 = T(0), a1 = T(0), a2 = T(0);
    T z1 = T(0), z2 = T(0);

    inline T tick(T x) noexcept {
        const T y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
    inline void reset() noexcept { z1 = z2 = T(0); }
};
using Biquad2 = Biquad2T<float>;
using Biquad2d = Biquad2T<double>;

// RBJ cookbook high-shelf (para la etapa 1 de K-weighting, "pre-filter")
template <typename T = float>
inline Biquad2T<T> make_high_shelfT(T fs, T f0, T gainDb, T Q) noexcept {
    const T A = std::pow(T(10), gainDb / T(40));
    const T w0 = T(2) * static_cast<T>(M_PI) * f0 / fs;
    const T cosw0 = std::cos(w0), sinw0 = std::sin(w0);
    const T alpha = sinw0 / (T(2) * Q);
    const T sqrtA = std::sqrt(A);

    const T b0 =    A * ((A + 1) + (A - 1) * cosw0 + T(2) * sqrtA * alpha);
    const T b1 = -T(2) * A * ((A - 1) + (A + 1) * cosw0);
    const T b2 =    A * ((A + 1) + (A - 1) * cosw0 - T(2) * sqrtA * alpha);
    const T a0 =        (A + 1) - (A - 1) * cosw0 + T(2) * sqrtA * alpha;
    const T a1 =    T(2) * ((A - 1) - (A + 1) * cosw0);
    const T a2 =        (A + 1) - (A - 1) * cosw0 - T(2) * sqrtA * alpha;

    Biquad2T<T> bq;
    bq.b0 = b0 / a0; bq.b1 = b1 / a0; bq.b2 = b2 / a0;
    bq.a1 = a1 / a0; bq.a2 = a2 / a0;
    return bq;
}

inline Biquad2 make_high_shelf(float fs, float f0, float gainDb, float Q) noexcept {
    return make_high_shelfT<float>(fs, f0, gainDb, Q);
}

// RBJ cookbook high-pass (para la etapa 2 de K-weighting, "RLB weighting")
template <typename T = float>
inline Biquad2T<T> make_highpassT(T fs, T f0, T Q) noexcept {
    const T w0 = T(2) * static_cast<T>(M_PI) * f0 / fs;
    const T cosw0 = std::cos(w0), sinw0 = std::sin(w0);
    const T alpha = sinw0 / (T(2) * Q);

    const T b0 = (T(1) + cosw0) / T(2);
    const T b1 = -(T(1) + cosw0);
    const T b2 = (T(1) + cosw0) / T(2);
    const T a0 = T(1) + alpha;
    const T a1 = -T(2) * cosw0;
    const T a2 = T(1) - alpha;

    Biquad2T<T> bq;
    bq.b0 = b0 / a0; bq.b1 = b1 / a0; bq.b2 = b2 / a0;
    bq.a1 = a1 / a0; bq.a2 = a2 / a0;
    return bq;
}

inline Biquad2 make_highpass(float fs, float f0, float Q) noexcept {
    return make_highpassT<float>(fs, f0, Q);
}

template <typename T = float>
class LoudnessMeterT {
public:
    void init(T sampleRate) noexcept {
        sr_ = sampleRate;
        shelfL_ = make_high_shelfT<T>(sr_, T(1681.9), T(4.0), T(0.7071));
        shelfR_ = shelfL_;
        hpL_    = make_highpassT<T>(sr_, T(38.13), T(0.5003));
        hpR_    = hpL_;
        // Constante de tiempo ~400ms (loudness "momentary" de BS.1770-4),
        // vía EMA equivalente: alpha = 1 - exp(-1 / (fs * tau))
        emaAlpha_ = T(1) - std::exp(T(-1) / (sr_ * T(0.4)));
        meanSquare_ = T(1e-9);
        currentTrimDb_ = T(0);
        reset();
    }

    void reset() noexcept {
        shelfL_.reset(); shelfR_.reset();
        hpL_.reset(); hpR_.reset();
    }

    /** Mide LUFS del bloque (K-weighted) y actualiza el estado interno. */
    T measure_block(const T* inL, const T* inR, int n) noexcept {
        for (int i = 0; i < n; ++i) {
            T l = shelfL_.tick(inL[i]);
            l = hpL_.tick(l);
            T r = shelfR_.tick(inR[i]);
            r = hpR_.tick(r);
            // BS.1770: suma de canales (mono/stereo sin weighting extra
            // de canal — surround usa G_i por canal, no aplica acá).
            const T ms = T(0.5) * (l * l + r * r);
            meanSquare_ += emaAlpha_ * (ms - meanSquare_);
        }
        meanSquare_ = std::max(meanSquare_, T(1e-9));
        lastLufs_ = T(-0.691) + T(10) * std::log10(meanSquare_);
        return lastLufs_;
    }

    T lufs() const noexcept { return lastLufs_; }

    // FIX 5 (v2.5.465): el trim ya no se mueve por bloque sino por
    // segundo de audio. `blockFrames`/`sampleRateHz` son del bloque que
    // se acaba de medir (nativeProcess los conoce); con ellos el paso
    // máximo por llamada es kTrimMaxSlewDbPerSec * duración del bloque,
    // así que la velocidad de corrección es la misma a 320 frames/bloque
    // (captura, 150 bloques/s) que a 2048 (reproductor, 23 bloques/s).
    // Zona muerta ±0.5 dB (por debajo del JND musical: el trim no
    // persigue el residuo del EMA) y tope total ±6 dB (antes ±12 dB:
    // un máster a -26 LUFS ya no empuja la cadena 12 dB contra el
    // limiter). Ganancia neta en régimen: sin cambio (mismo target).
    static constexpr T kTrimDeadbandDb = T(0.5);
    static constexpr T kTrimMaxSlewDbPerSec = T(0.5);
    static constexpr T kTrimMaxDb = T(6.0);
    static constexpr T kTrimProportional = T(0.02);

    /**
     * Trim de ganancia (dB) hacia targetLufs, actualizado lentamente
     * (máx 0.5 dB/s, solo fuera de la zona muerta de ±0.5 dB) para no
     * bombear ni notarse como efecto. Se llama una vez por bloque,
     * junto con measure_block().
     */
    T update_trim(T targetLufs, int blockFrames, T sampleRateHz) noexcept {
        const T errorDb = targetLufs - lastLufs_;
        // Silencio (puerta absoluta BS.1770, -70 LUFS): el trim se
        // CONGELA. Sin esto, el silencio entre temas (LUFS ≈ -90) lo
        // empujaba al tope (-6 dB) y el tema siguiente arrancaba 6 dB
        // bajo, recuperando a 0.5 dB/s — otro sube/baja al cambiar de
        // canción. El medidor no tiene gating; el trim sí lo tiene aquí.
        if (lastLufs_ <= T(-70.0)) return currentTrimDb_;
        if (std::fabs(errorDb) <= kTrimDeadbandDb) return currentTrimDb_;
        const T blockSec = (blockFrames > 0 && sampleRateHz > T(0))
            ? static_cast<T>(blockFrames) / sampleRateHz : T(0);
        const T maxStepDb = kTrimMaxSlewDbPerSec * blockSec;
        const T step = std::clamp(errorDb * kTrimProportional, -maxStepDb, maxStepDb);
        currentTrimDb_ = std::clamp(currentTrimDb_ + step, -kTrimMaxDb, kTrimMaxDb);
        return currentTrimDb_;
    }

    T trimLinear() const noexcept {
        return std::pow(T(10), currentTrimDb_ / T(20));
    }

private:
    T sr_ = T(96000);
    Biquad2T<T> shelfL_, shelfR_, hpL_, hpR_;
    T emaAlpha_ = T(0.01);
    T meanSquare_ = T(1e-9);
    T lastLufs_ = T(-70);
    T currentTrimDb_ = T(0);
};

// Instancia float (histórica) / instancia double 64-bit (F2/486).
using LoudnessMeter = LoudnessMeterT<float>;
using LoudnessMeterd = LoudnessMeterT<double>;

}  // namespace ivanna
