// TemporalSpatialDynamics.hpp
// ============================================================================
// IVANNA — Dinámica espacial en el TIEMPO ("4D": la imagen respira con la música)
// ============================================================================
// Las tres dimensiones espaciales ya las cubre el motor (HRTF/HOA/WFS, ancho, profundidad). Lo que ninguna etapa
// hacía era mover el espacio EN EL TIEMPO ligado a la música: el ancho era un valor fijo. Esta etapa lo hace con
// una técnica clásica y verificable:
//
//   · En cada TRANSITORIO (golpe, ataque de caja, pluck) las partes laterales se recogen hacia el centro
//     (la imagen se "enfoca" → el impacto suena más compacto y con más pegada).
//   · En lo SOSTENIDO (colas, cuerdas, ambiente) las partes laterales se abren ("bloom" → más envolvente).
//
// El detector es el TransientDetector que ya existía en el proyecto (comparación de dos envolventes, sin IA);
// aquí solo se reutiliza. El gain lateral se suaviza: ataque 3 ms, relajación ~140 ms, con 25 ms de retención
// tras un golpe para que no tiemble.
//
// LEY ADAPTATIVA (modo auto, opcional): la profundidad efectiva escala con la DENSIDAD de golpes medida en la
// propia música, no con un valor fijo:
//     r  = golpes por segundo (flancos de subida del detector, media móvil de 2 s)
//     s  = 1 - exp(-r / 3)                       (0 sin golpes → ~0.63 a 3/s → ~0.86 a 6/s)
//     a_ef = a_usuario · (0.4 + 0.6·s)           (el slider es el TECHO; nunca se supera)
// Música densa en ataques (electrónica, rock) recibe todo el enfoque; ambiente/cuerdas sostenidas, un 40 % (el
// bloom sigue ahí pero sin pegar cada nota). Es una heurística de diseño con coeficientes elegidos a mano: acotada y
// probada, NO validada con escucha — por eso se puede apagar y el slider manda.
//
// Propiedades garantizadas (y probadas en tests/test_temporal_spatial.cpp):
//   · El canal CENTRAL (M = (L+R)/2) no se toca nunca: L'+R' == L+R. Compatible en mono y no cambia el centro.
//   · Solo se modifica el lateral POR ENCIMA de ~250 Hz (dos pasa-altos de 1er orden): los graves, que el modo AUX
//     deja coherentes, no se vuelven a abrir.
//   · Apagado y con el fundido en 0 el audio pasa BIT-EXACTO. Activación sin clic (fundido de 20 ms).
//   · RT-safe: sin asignaciones ni locks.
//
// Lo que NO es: no es espacialización binaural ni mide la sala. Es modulación temporal del ancho lateral.
// ============================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>
#include "TransientDetector.hpp"

namespace ivanna::dsp {

// F2/486 (labor grande, double interno): el estado de señal (gains,
// envolventes, filtros laterales) vive en double; process(double) es
// el núcleo y process(float) delega por scratch. Los setters siguen
// en float (son sliders de control); la matemática es double.
class TemporalSpatialDynamics {
public:
    static constexpr std::size_t kMaxChunk = 1024;
    static constexpr float kTightPerAmount = 0.60f;  // a amount=1, el lateral cae a 40 % en un golpe
    static constexpr float kBloomPerAmount = 0.35f;  // a amount=1, el lateral sube a 135 % en lo sostenido
    static constexpr float kSideCornerHz   = 250.0f;

    void prepare(float sampleRate) noexcept {
        sr_ = (std::isfinite(sampleRate) && sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        detector_.prepare(sampleRate);
        atk_      = 1.0 - std::exp(-1.0 / (0.003 * sr_));
        rel_      = 1.0 - std::exp(-1.0 / (0.140 * sr_));
        holdLen_  = static_cast<int>(0.025 * sr_);
        fadeStep_ = 1.0 / (0.020 * sr_);
        lpA_      = 1.0 - std::exp(-2.0 * 3.14159265358979 * static_cast<double>(kSideCornerHz) / sr_);
        reset();
    }

    /// Hilo de audio, al inicio de cada bloque.
    void setParams(bool enabled, float amount) noexcept {
        enabled_ = enabled;
        if (std::isfinite(amount)) amount_ = std::clamp(static_cast<double>(amount), 0.0, 1.0);
    }

    /// ARCO NARRATIVO 4D (opcional): la cadena 4D mide la forma de la canción a escala de segundos. `tension` (0..1, el
    /// hueco previo al drop) abre la imagen más allá del bloom normal: la sala "se infla" antes del golpe;
    /// `impact` (0..1, envolvente del drop) recoge el lateral más fuerte en los golpes: el impacto entra compacto.
    /// Ambos en 0 = comportamiento idéntico al de siempre. Topes duros: bloom <= +60 %, recogida <= -85 %.
    void setArc(float tension, float impact) noexcept {
        tension_ = std::isfinite(tension) ? std::clamp(static_cast<double>(tension), 0.0, 1.0) : 0.0;
        impact_  = std::isfinite(impact)  ? std::clamp(static_cast<double>(impact),  0.0, 1.0) : 0.0;
    }

    /// Modo adaptativo: la profundidad efectiva sigue la densidad de golpes (ver ley arriba).
    void setAuto(bool on) noexcept { auto_ = on; }
    /// Profundidad efectiva ahora (= amount si auto está apagado) — telemetría / tests.
    float effectiveAmount() const noexcept { return static_cast<float>(auto_ ? amount_ * autoGain_ : amount_); }
    float hitRate() const noexcept { return static_cast<float>(rate_); }

    bool  isActive() const noexcept { return enabled_ || fade_ > 0.0; }
    float fade()     const noexcept { return static_cast<float>(fade_); }
    /// Ganancia lateral actual (1 = sin cambio) — telemetría / tests.
    float sideGain() const noexcept { return static_cast<float>(g_); }

    void reset() noexcept {
        detector_.reset();
        g_ = 1.0; hold_ = 0; fade_ = 0.0;
        rate_ = 0.0; autoGain_ = 0.4; prevHit_ = false;
        lp1_ = lp2_ = 0.0;
    }

    // Núcleo double (F2/486).
    void process(double* L, double* R, std::size_t n) noexcept {
        if (L == nullptr || R == nullptr || n == 0) return;
        if (!enabled_ && fade_ <= 0.0) {      // bypass bit-exacto
            if (g_ != 1.0) { g_ = 1.0; hold_ = 0; lp1_ = lp2_ = 0.0; detector_.reset(); }
            return;
        }
        std::size_t done = 0;
        while (done < n) {
            const std::size_t c = std::min(n - done, kMaxChunk);
            for (std::size_t i = 0; i < c; ++i) mid_[i] = 0.5 * (L[done + i] + R[done + i]);
            const bool hit = detector_.processBlock(mid_, c);
            if (hit) hold_ = holdLen_;
            {   // densidad de golpes: flancos de subida, media móvil de 2 s
                const double dt = static_cast<double>(c) / sr_;
                const double inst = (hit && !prevHit_) ? (1.0 / dt) : 0.0;
                rate_ += (inst - rate_) * std::min(1.0, dt / 2.0);
                prevHit_ = hit;
                autoGain_ = 0.4 + 0.6 * (1.0 - std::exp(-rate_ / 3.0));
            }
            const double amt = auto_ ? amount_ * autoGain_ : amount_;
            const double gTight = 1.0 - std::min(0.85, static_cast<double>(kTightPerAmount) * amt * (1.0 + 0.5 * impact_));
            const double gBloom = 1.0 + std::min(0.60, static_cast<double>(kBloomPerAmount) * amt * (1.0 + 0.6 * tension_));
            for (std::size_t i = 0; i < c; ++i) {
                const double target = (hold_ > 0) ? gTight : gBloom;
                if (hold_ > 0) --hold_;
                g_ += (target - g_) * (target < g_ ? atk_ : rel_);

                if (enabled_) fade_ = std::min(1.0, fade_ + fadeStep_);
                else          fade_ = std::max(0.0, fade_ - fadeStep_);

                const double l = L[done + i], r = R[done + i];
                const double s = 0.5 * (l - r);
                lp1_ += (s - lp1_) * lpA_;
                const double h1 = s - lp1_;
                lp2_ += (h1 - lp2_) * lpA_;
                const double hp = h1 - lp2_;                 // lateral > ~250 Hz (12 dB/oct)
                const double s2 = s + fade_ * (g_ - 1.0) * hp;
                const double m  = 0.5 * (l + r);
                L[done + i] = m + s2;
                R[done + i] = m - s2;
            }
            done += c;
        }
    }

    // Envoltorio float (compat legacy): convierte por scratch y delega
    // en el núcleo double.
    void process(float* L, float* R, std::size_t n) noexcept {
        if (L == nullptr || R == nullptr || n == 0) return;
        if (wrapL_.size() < n) { wrapL_.resize(n); wrapR_.resize(n); }
        for (std::size_t i = 0; i < n; ++i) {
            wrapL_[i] = std::isfinite(L[i]) ? static_cast<double>(L[i]) : 0.0;
            wrapR_[i] = std::isfinite(R[i]) ? static_cast<double>(R[i]) : 0.0;
        }
        process(wrapL_.data(), wrapR_.data(), n);
        for (std::size_t i = 0; i < n; ++i) {
            L[i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
            R[i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
        }
    }

private:
    TransientDetector detector_;
    double sr_ = 48000.0, atk_ = 0.0, rel_ = 0.0, fadeStep_ = 0.0, lpA_ = 0.0;
    int   holdLen_ = 0;
    bool  enabled_ = false;
    double amount_ = 0.6;
    bool  auto_ = false;
    double rate_ = 0.0, autoGain_ = 0.4;
    double tension_ = 0.0, impact_ = 0.0;
    bool  prevHit_ = false;
    double g_ = 1.0, fade_ = 0.0, lp1_ = 0.0, lp2_ = 0.0;
    int   hold_ = 0;
    double mid_[kMaxChunk] = {};
    // Scratch del envoltorio float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapL_{};
    std::vector<double> wrapR_{};
};

}  // namespace ivanna::dsp
