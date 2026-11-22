#include "../include/SafetyLimiter.h"
#include "../include/true_peak.hpp"
#include <algorithm>
#include <cmath>  // std::max — el NDK (libc++) no lo incluye transitivamente
#include <cstring>

namespace ivanna {

// ── Constantes del limiter (TAREA 5) ────────────────────────────────────────
namespace {
    constexpr double kReleaseMs   = 50.0;   // release suave
    constexpr double kAttackMs    = 1.5;    // ataque corto pero continuo
    constexpr double kKneeRatio   = 0.1;    // soft-knee 10:1 sobre threshold

// Saturacion suave hacia el techo: identidad por debajo del 90 % del ceiling,
// curva racional (continua en valor y pendiente) por encima, y nunca supera
// el ceiling. Sustituye al recorte duro del final de process().
//
// v2.5.199 (distorsion armonica al subir volumen, medida en host): el codo estaba al 90 % del techo
// (0.8897) pero la curva estatica de computeGainForPeak() deja los picos de un master caliente entre
// threshold (-1 dBFS) y el techo (0.89..0.99). Todo pico estable caia, pues, DENTRO del tramo
// saturado -> la cresta del seno se aplanaba de forma permanente: THD 0.87 % a 0 dBFS y 1.98 % a
// +3/+6 dBFS (seno 100 Hz y 1 kHz, 48 kHz, bloques de 256). Con el codo al 97 % del techo el
// regimen estable queda lineal (THD 0.00 % a 0 dBFS, 0.50 % a +3/+6 dBFS) sin cambiar el nivel,
// la reduccion de ganancia ni el techo (la saturacion sigue acotada por ceil_). La saturacion queda
// reservada a lo que si es excepcional: el sobrepico durante la rampa de ataque de 1.5 ms.
inline double softCeil(double x, double ceil_) {
    const double knee = ceil_ * 0.97;
    const double ax = x < 0.0 ? -x : x;
    if (ax <= knee) return x;
    const double range = ceil_ - knee;          // > 0
    const double over  = (ax - knee) / range;   // >= 0
    const double y = knee + range * (over / (1.0 + over));
    return x < 0.0 ? -y : y;
}
} // namespace

void SafetyLimiter::setParams(float threshold, float ceiling) {
    if (!std::isfinite(threshold) || threshold <= 0.0f)
        threshold = 0.630957f; // -4 dBFS aprox

    if (!std::isfinite(ceiling) || ceiling <= 0.0f)
        ceiling = 0.988553f; // -0.1 dBFS aprox

    m_threshold = std::min(static_cast<double>(threshold), static_cast<double>(ceiling));
    m_ceiling   = static_cast<double>(ceiling);
}

void SafetyLimiter::setSampleRate(float sampleRate) {
    if (sampleRate > 8000.f) m_sampleRate = static_cast<double>(sampleRate);
    // Coeficiente de release (decaimiento exponencial por muestra).
    const double sr64 = m_sampleRate;
    m_releaseCoef = std::exp(-1.0 / (sr64 * kReleaseMs / 1000.0));
    m_attackCoef  = std::exp(-1.0 / (sr64 * kAttackMs  / 1000.0));
}

// Ganancia objetivo (<=1) para un peak lineal dado. Soft-knee: por encima del
// threshold se reduce a ratio 10:1 hasta el ceiling; por debajo, ganancia 1.
double SafetyLimiter::computeGainForPeak(double peakLin) const {
    if (peakLin <= m_threshold) return 1.0;
    const double excess = peakLin - m_threshold;

    // Curva suave hacia ceiling para reducir THD.
    // Evita que el knee genere una discontinuidad fuerte.
    double limited = m_threshold +
                    excess / (1.0 + excess * 8.0);

    if (limited > m_ceiling)
        limited = m_ceiling;

    return (peakLin > 1e-9) ? (limited / peakLin) : 1.0;
}

double SafetyLimiter::limitSample(double x) {
    // Camino escalar de respaldo (tail del proceso de bloque). El comportamiento
    // audible vive en process() (lookahead + release); esto solo garantiza que
    // una muestra aislada nunca salga del rango [-m_ceiling, m_ceiling].
    //
    // FIX: el clamp final era std::clamp(x, -1.0f, 1.0f) en vez de ±m_ceiling.
    // m_ceiling default = 0.98855 ≈ -0.1 dBFS. La inconsistencia de 0.1 dB
    // entre process() (que satura con softCeil al ceiling) y limitSample()
    // (que cortaba a 1.0, 0.1dB más alto) significaba que el path escalar
    // podía dejar 0.1dB más de señal que el path de bloque — audible como
    // diferencia de nivel en material con muchos transientes (fade-in, etc.).
    if (!std::isfinite(x)) {
        m_clipCount.fetch_add(1, std::memory_order_relaxed);
        return 0.0;
    }
    const double ax  = std::fabs(x);
    // Retorno directo (sin saturación) para señal dentro del threshold.
    // El clamp a ±1.0 anterior aquí era innecesario — si ax ≤ threshold
    // (≤ 0.63096 por defecto) no puede exceder ±1.0 de todos modos.
    if (ax <= m_threshold) return x;

    const double sign  = x < 0.0 ? -1.0 : 1.0;
    const double excess = ax - m_threshold;
    double limited = m_threshold + excess / (1.0 + excess * 8.0);

    if (limited > m_ceiling) {
        limited = m_ceiling;
        m_clipCount.fetch_add(1, std::memory_order_relaxed);
    }
    // FIX: clamp con m_ceiling, no 1.0
    const double out = sign * limited;
    if (out >  m_ceiling) return  m_ceiling;
    if (out < -m_ceiling) return -m_ceiling;
    return out;
}

void SafetyLimiter::process(double* L, double* R, int frames) {
    if (m_bypass || frames <= 0) return;

    // Sanitización de entrada: el limiter es la última barrera del DSP.
    // Ningún NaN/Inf puede entrar al cálculo de peak ni contaminar estados.
    int clips = 0;
    bool nonFinite = false;
    for (int i = 0; i < frames; ++i) {
        if (!std::isfinite(L[i])) { L[i] = 0.0; nonFinite = true; ++clips; }
        if (!std::isfinite(R[i])) { R[i] = 0.0; nonFinite = true; ++clips; }
    }

    // Lazy-init del coeficiente de release si nunca se llamo setSampleRate().
    if (m_releaseCoef <= 0.0) setSampleRate(static_cast<float>(m_sampleRate));

    // ── Peak del bloque (escalar double; el camino NEON float histórico
    // queda sustituido: el limiter corre una vez por bloque) ───────────────
    // El bloque ES el lookahead: la ganancia se decide con el peak de TODO el
    // bloque antes de escribir una sola muestra, asi que ningun pico sale sin
    // su reduccion aplicada — sin delay line y sin latencia añadida.
    double peak  = 0.0;
    const double ceil_ = m_ceiling;

    for (int i = 0; i < frames; ++i) {
        const double al = std::fabs(L[i]);
        const double ar = std::fabs(R[i]);
        if (!std::isfinite(al) || !std::isfinite(ar)) { nonFinite = true; continue; }
        peak = std::max(peak, std::max(al, ar));
    }

    // v2.5.36 — PICO REAL: el DAC reconstruye la onda entre muestras, así que el pico que importa es el
    // inter-muestra (interpolación 4x). Antes solo se miraba |muestra| y picos de hasta +3 dB sobre el techo
    // llegaban a la salida de 3.5 mm. truePeak4x() omite los intervalos silenciosos: coste despreciable.
    if (!nonFinite) {
        peak = std::max(peak, std::max(truePeak4x(L, frames), truePeak4x(R, frames)));
        if (!std::isfinite(peak)) { nonFinite = true; peak = 0.0; }
    }

    // Transparencia absoluta: material limpio sin reduccion residual sale
    // bit-exacto — y con la MISMA latencia (cero) que cuando el limiter actua.
    // FIX (fast-path fallaba tras release largo): >= 0.99999f es demasiado
    // estricto — un release de 50 ms desde 0.5 tarda ~15 bloques en cruzar
    // ese umbral, dejando el material posterior con una ganancia de 0.99999x
    // aplicada muestra a muestra (no bit-exacto). Ampliamos la banda muerta
    // a 0.9995 (~-0.004 dB, inaudible) y snap explicito a 1.0 para que el
    // siguiente bloque limpio se vaya por la salida rapida.
    if (peak <= m_threshold && m_gainNow >= 0.9995 && !nonFinite) {
        m_gainNow = 1.0;
        m_peakBefore.store(static_cast<float>(peak), std::memory_order_relaxed);
        m_gainReduction.store(0.0f, std::memory_order_relaxed);
        return;
    }

    // Ganancia del bloque: objetivo derivado del peak del bloque, ataque
    // continuo de ~1.5 ms y release exponencial de 50 ms entre picos. La
    // ganancia NO sigue la forma de onda muestra a muestra -> sin modulacion
    // de amplitud (que es distorsion armonica) ni pumping.
    // FIX (chasquidos / distorsion armonica): el ataque era instantaneo en la
    // PRIMERA muestra del bloque (gain = min(gainNow, blockGain)). Ese escalon
    // de ganancia cae siempre en la frontera de bloque, donde la senal no tiene
    // por que estar en un cruce por cero -> discontinuidad periodica a razon de
    // un evento por buffer = chasquido/tronido audible, y espectralmente un
    // peine de armonicos a la frecuencia de bloque. Ahora el ataque converge
    // por muestra (~1.5 ms), asi que la envolvente de ganancia es continua.
    // El residuo que pueda asomar durante la rampa lo recoge la saturacion
    // suave de abajo (antes era un recorte duro = onda cuadrada).
    const double blockGainRaw = computeGainForPeak(peak);
    // FIX (envenamiento de gain): computeGainForPeak() devuelve limited/peakLin;
    // con entradas patologicas (peak ~ 1e-9, denormals, NaN reciclado del bloque
    // anterior) el cociente puede quedar fuera de [0, 1] o no finito. Un solo
    // frame con gain no finito propaga NaN a TODO el resto del bloque via el
    // filtro one-pole. Clamp defensivo antes de entrar al loop caliente.
    const double blockGain = std::isfinite(blockGainRaw)
        ? std::max(0.0, std::min(1.0, blockGainRaw))
        : 1.0;
    if (m_attackCoef <= 0.0) {
        m_attackCoef = std::exp(-1.0 / (m_sampleRate * kAttackMs / 1000.0));
    }
    // Igual defensa sobre m_gainNow (por si un bloque previo con guard
    // parcial dejo residuo no finito). Sin esto el fix del blockGain seria
    // inutil: la contaminacion vendria via el estado, no via la entrada.
    double gain = std::isfinite(m_gainNow) ? m_gainNow : 1.0;

    for (int i = 0; i < frames; ++i) {
        if (blockGain < gain) {
            gain = m_attackCoef * gain + (1.0 - m_attackCoef) * blockGain;
            if (gain < blockGain) gain = blockGain;
        } else if (gain < blockGain) {
            gain = m_releaseCoef * gain + (1.0 - m_releaseCoef) * blockGain;
            if (gain > blockGain) gain = blockGain;
        }
        if (!std::isfinite(gain)) gain = 1.0;
        if (gain > 1.0) gain = 1.0;
        if (gain < 0.0) gain = 0.0;

        // Entrada defensiva: nunca permitir que NaN/Inf llegue al
        // multiplicador ni al estado audible. El limiter es la última barrera.
        const double inL = std::isfinite(L[i]) ? L[i] : 0.0;
        const double inR = std::isfinite(R[i]) ? R[i] : 0.0;

        double outL = inL * gain;
        double outR = inR * gain;

        // FIX (falso positivo de 9912 clips/ciclo): antes se contaban como
        // "clips" todas las muestras de ENTRADA que superaban ceil_ ANTES de
        // aplicar la reducción de ganancia lineal por bloque (inL * gain).
        // Un limitador con lookahead que atenúa limpiamente la señal con su
        // envolvente de ganancia NO está clipeando; el recorte/clipping real
        // solo ocurre si, tras aplicar `gain` (p. ej. durante los primeros
        // samples de la rampa de ataque de 1.5 ms en un transitorio abrupto),
        // la señal aún supera `ceil_` y obliga a actuar al saturador de techo.
        if (!std::isfinite(outL) || !std::isfinite(outR)) {
            ++clips;
            outL = 0.0;
            outR = 0.0;
        } else if (std::fabs(outL) > 1.05 || std::fabs(outR) > 1.05) {
            ++clips;
        }
        outL = softCeil(outL, ceil_);
        outR = softCeil(outR, ceil_);

        L[i] = outL;
        R[i] = outR;
    }

    m_gainNow = gain;

    if (clips > 0) m_clipCount.fetch_add(clips, std::memory_order_relaxed);
    m_peakBefore.store(static_cast<float>(peak), std::memory_order_relaxed);

    // Telemetría de atenuación objetivo en dB, basada en la ganancia que
    // realmente calcula el limiter. El cálculo anterior solo reportaba GR
    // cuando peak > ceiling; entre threshold (-4 dBFS) y ceiling (-0.1 dBFS)
    // el limiter ya atenuaba, pero publicaba 0 dB y cegaba al controlador
    // adaptativo. blockGain está acotada a (0,1], así que esta conversión es
    // finita y no confunde "pico sobre ceiling" con "ganancia aplicada".
    float reduction_db = 0.0f;
    if (blockGain > 1e-9 && blockGain < 1.0)
        reduction_db = static_cast<float>(-20.0 * std::log10(blockGain));
    if (!std::isfinite(reduction_db) || reduction_db < 0.0f)
        reduction_db = 0.0f;
    m_gainReduction.store(reduction_db, std::memory_order_relaxed);
}

void SafetyLimiter::process(float* L, float* R, int frames) {
    if (m_bypass || frames <= 0) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(L[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(R[i]);
    }
    process(wrapL_.data(), wrapR_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        L[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        R[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}

void SafetyLimiter::reset() {
    m_peakBefore.store(0.0f, std::memory_order_relaxed);
    m_gainReduction.store(0.0f, std::memory_order_relaxed);
    m_gainNow = 1.0;
    resetClipCount();
}

void SafetyLimiter::bypass(bool enabled) {
    m_bypass = enabled;
}

float SafetyLimiter::getPeakBeforeLimit() const {
    return m_peakBefore.load(std::memory_order_relaxed);
}

float SafetyLimiter::getGainReduction() const {
    return m_gainReduction.load(std::memory_order_relaxed);
}

int SafetyLimiter::getClipCount() const {
    return m_clipCount.load(std::memory_order_relaxed);
}

void SafetyLimiter::resetClipCount() {
    m_clipCount.store(0, std::memory_order_relaxed);
}

} // namespace ivanna
