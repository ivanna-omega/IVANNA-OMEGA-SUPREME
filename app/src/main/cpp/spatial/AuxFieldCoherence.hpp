// AuxFieldCoherence.hpp
// ============================================================================
// IVANNA — Campo de presión coherente para salida por ALTAVOCES / AUX
// ============================================================================
// El WFS/HRTF del motor renderiza binauralmente (pensado para audífonos). Por
// AUX hacia un amplificador con woofers, ese renderizado no aplica: lo que
// importa es que los graves y sus transitorios lleguen EN FASE y con la misma
// presión desde ambos lados, para que la sala se excite como un campo de
// presión coherente y no como "dos bocinas".
//
// Técnica (estándar, verificable): crossover Linkwitz-Riley de 4º orden.
//   LP(f) + HP(f) suma plana en magnitud y alineada en fase (all-pass), así que
//   la banda alta queda INTACTA (estéreo, imagen y transitorios agudos) y solo
//   la banda baja se trata:
//       baja'_c = (1-k)·baja_c + k·0.5·(baja_L + baja_R)        k = coherencia 0..1
//   Con k=1 los graves son idénticos en ambos canales (presión uniforme, sin
//   cancelaciones de fase entre woofers); con k=0 es solo el all-pass LR4.
//
// Lo que NO hace (a propósito): no mide la sala ni compensa su respuesta — eso
// es corrección acústica (RIR/SAF/AutoEQ), otro módulo.
//
// RT-safe: cero asignaciones, cero locks, estado fijo. Activación sin clic:
// fundido de 20 ms entre la señal original y la tratada; con el modo apagado y
// el fundido en 0 el audio pasa BIT-EXACTO (no se toca ni una muestra).
//
// F2/486 (labor grande, motor double): plantilla con default float +
// alias. AuxFieldCoherence = float histórico bit-idéntico;
// AuxFieldCoherenceD = núcleo double (lo usa FusedSpatial). Los parámetros
// (xover, coherencia, enabled) siguen siendo float = control.
// ============================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace ivanna::dsp {

template <typename Scalar = float>
class AuxFieldCoherenceT {
public:
    static constexpr Scalar kMinXoverHz = Scalar(40.0);
    static constexpr Scalar kMaxXoverHz = Scalar(250.0);

    void prepare(float sampleRate) noexcept {
        sr_ = static_cast<Scalar>((std::isfinite(sampleRate) && sampleRate > 8000.0f) ? sampleRate : 48000.0f);
        fadeStep_ = Scalar(1.0) / (Scalar(0.020) * sr_);
        coefXover_ = Scalar(-1.0);            // fuerza recálculo
        reset();
        updateCoefs(xoverHz_);
    }

    /// Hilo de audio, al inicio de cada bloque (valores ya validados aquí).
    void setParams(bool enabled, float xoverHz, float coherence) noexcept {
        enabled_ = enabled;
        if (std::isfinite(xoverHz)) xoverHz_ = std::clamp(static_cast<Scalar>(xoverHz), kMinXoverHz, kMaxXoverHz);
        if (std::isfinite(coherence)) coh_ = std::clamp(static_cast<Scalar>(coherence), Scalar(0.0), Scalar(1.0));
    }

    bool isActive() const noexcept { return enabled_ || fade_ > Scalar(0.0); }
    float fade() const noexcept { return static_cast<float>(fade_); }

    void reset() noexcept {
        for (auto& s : st_) s = Bq{};
        fade_ = Scalar(0.0);
    }

    void process(Scalar* L, Scalar* R, std::size_t n) noexcept {
        if (!L || !R || n == 0) return;
        if (!enabled_ && fade_ <= Scalar(0.0)) return;            // bypass bit-exacto
        if (std::fabs(xoverHz_ - coefXover_) > Scalar(0.25)) updateCoefs(xoverHz_);
        const Scalar target = enabled_ ? Scalar(1.0) : Scalar(0.0);
        const Scalar k = coh_;
        for (std::size_t i = 0; i < n; ++i) {
            const Scalar dl = L[i], dr = R[i];
            const Scalar lowL  = run(st_[0], run(st_[1], dl, lp_), lp_);
            const Scalar lowR  = run(st_[2], run(st_[3], dr, lp_), lp_);
            const Scalar highL = run(st_[4], run(st_[5], dl, hp_), hp_);
            const Scalar highR = run(st_[6], run(st_[7], dr, hp_), hp_);
            const Scalar mono = Scalar(0.5) * (lowL + lowR);
            const Scalar pl = highL + (Scalar(1.0) - k) * lowL + k * mono;
            const Scalar pr = highR + (Scalar(1.0) - k) * lowR + k * mono;
            if (fade_ < target)      fade_ = std::min(target, fade_ + fadeStep_);
            else if (fade_ > target) fade_ = std::max(target, fade_ - fadeStep_);
            const Scalar f = fade_;
            L[i] = dl + f * (pl - dl);
            R[i] = dr + f * (pr - dr);
        }
        if (!enabled_ && fade_ <= Scalar(0.0)) reset();            // estado limpio para la próxima activación
    }

private:
    struct Coef { Scalar b0 = Scalar(1), b1 = Scalar(0), b2 = Scalar(0), a1 = Scalar(0), a2 = Scalar(0); };
    struct Bq { Scalar x1 = Scalar(0), x2 = Scalar(0), y1 = Scalar(0), y2 = Scalar(0); };

    static Scalar run(Bq& s, Scalar x, const Coef& c) noexcept {
        const Scalar y = c.b0 * x + c.b1 * s.x1 + c.b2 * s.x2 - c.a1 * s.y1 - c.a2 * s.y2;
        s.x2 = s.x1; s.x1 = x; s.y2 = s.y1; s.y1 = y;
        return y;
    }

    void updateCoefs(Scalar fc) noexcept {
        coefXover_ = fc;
        // Butterworth 2º orden (Q = 1/√2) en cascada x2 = Linkwitz-Riley 4º orden.
        const Scalar w0 = Scalar(6.2831853071795864769) * fc / sr_;
        const Scalar cw = std::cos(w0), sw = std::sin(w0);
        const Scalar alpha = sw / (Scalar(2.0) * Scalar(0.70710678118654752440));
        const Scalar a0 = Scalar(1.0) + alpha;
        const Scalar inv = Scalar(1.0) / a0;
        lp_.b0 = (Scalar(1.0) - cw) * Scalar(0.5) * inv; lp_.b1 = (Scalar(1.0) - cw) * inv; lp_.b2 = lp_.b0;
        hp_.b0 = (Scalar(1.0) + cw) * Scalar(0.5) * inv; hp_.b1 = -(Scalar(1.0) + cw) * inv; hp_.b2 = hp_.b0;
        lp_.a1 = hp_.a1 = Scalar(-2.0) * cw * inv;
        lp_.a2 = hp_.a2 = (Scalar(1.0) - alpha) * inv;
    }

    Scalar sr_ = Scalar(48000.0), fadeStep_ = Scalar(1.0) / (Scalar(0.020) * Scalar(48000.0));
    bool  enabled_ = false;
    Scalar xoverHz_ = Scalar(120.0), coh_ = Scalar(1.0), coefXover_ = Scalar(-1.0), fade_ = Scalar(0.0);
    Coef lp_, hp_;
    Bq st_[8];
};

// Alias de compat: el float histórico, bit-idéntico (lo usan los tests y
// la ruta legacy); el double es el núcleo de la labor grande.
using AuxFieldCoherence = AuxFieldCoherenceT<float>;
using AuxFieldCoherenceD = AuxFieldCoherenceT<double>;

} // namespace ivanna::dsp
