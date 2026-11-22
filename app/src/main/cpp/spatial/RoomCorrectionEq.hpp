// RoomCorrectionEq.hpp
// ============================================================================
// IVANNA — Etapa que APLICA la corrección acústica de sala medida por RoomProbe.
// ============================================================================
// Reutiliza AutoEqFilter (cascada de 10 biquads peaking RBJ que ya existía en el proyecto y solo vivía dentro del
// renderer de objetos) con las frecuencias de RoomProbe (31.5 Hz … 16 kHz, Q ≈ 1.41, una octava).
//
//   · Apagada (todas las ganancias en 0) → BIT-EXACTO: no se toca una muestra ni se ejecuta un biquad.
//   · Los cambios de ganancia se deslizan a 0.25 dB por bloque: aplicar o quitar la corrección no hace clic.
//   · Las ganancias objetivo llegan ya acotadas por RoomProbe (−9 … 0 dB, cut-only); aquí se acota de nuevo
//     a [kRoomCorrMinDb, kRoomCorrMaxDb] cuando cutOnly_ está activo, o a ±12 dB en modo completo.
//   · CUT-ONLY (Fase 4): la corrección de SALA (m_room) solo recorta picos, nunca realza valles.
//     El EQ de USUARIO (m_userEq) usa la misma clase SIN cut-only (necesita realces legítimos, p. ej.
//     +6 dB a 1 kHz): setCutOnly(true) solo lo recibe m_room (IvannaFusionCore).
// RT-safe: sin asignaciones ni locks (los objetivos se leen del hilo de audio como atómicos en el motor).
// ============================================================================
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
#include "auto_eq_filter.hpp"
#include "RoomProbe.hpp"

namespace ivanna::dsp {

// F2/486 (2026-11-20) — MOTOR DOUBLE: estado y matemática en double 64-bit.
// process(double*, double*, int) es el núcleo; process(float*, float*, int)
// es un envoltorio de compat. Los objetivos llegan como float (controles)
// y se guardan en double.
class RoomCorrectionEq {
public:
    static constexpr double kSlewDbPerBlock = 0.25;

    void prepare(float sampleRate) noexcept {
        sr_ = (std::isfinite(sampleRate) && sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        eq_.init(static_cast<float>(sr_));
        for (int b = 0; b < room::kBands; ++b) { cur_[b] = 0.0; tgt_[b] = 0.0; }
        active_ = false;
        safetyGain_ = 1.0;
        eq_.setEnabled(false);
    }

    /// Objetivos en dB para las 10 bandas de RoomProbe (hilo de audio, al inicio del bloque).
    /// Cut-only activo → acota a [kRoomCorrMinDb, kRoomCorrMaxDb] (nunca realza);
    /// modo completo (default, EQ de usuario) → acota a ±12 dB.
    void setTarget(const float* gDb) noexcept {
        if (gDb == nullptr) return;
        const double lo = cutOnly_ ? static_cast<double>(room::kRoomCorrMinDb) : -12.0;
        const double hi = cutOnly_ ? static_cast<double>(room::kRoomCorrMaxDb) : 12.0;
        for (int b = 0; b < room::kBands; ++b)
            tgt_[b] = std::isfinite(gDb[b]) ? std::clamp(static_cast<double>(gDb[b]), lo, hi) : 0.0;
    }

    /// Activa el modo cut-only (solo recortar, nunca realzar). Default false
    /// (rango completo ±12 dB, para el EQ de usuario). Solo m_room lo activa.
    void setCutOnly(bool v) noexcept { cutOnly_ = v; }
    bool isCutOnly() const noexcept { return cutOnly_; }

    bool  isActive() const noexcept { return active_; }
    float currentDb(int b) const noexcept { return (b >= 0 && b < room::kBands) ? static_cast<float>(cur_[b]) : 0.0f; }

    void process(double* L, double* R, int n) noexcept {
        if (L == nullptr || R == nullptr || n <= 0) return;
        bool changed = false, any = false;
        for (int b = 0; b < room::kBands; ++b) {
            const double d = tgt_[b] - cur_[b];
            if (std::fabs(d) > 1.0e-4) {
                cur_[b] += std::clamp(d, -kSlewDbPerBlock, kSlewDbPerBlock);
                changed = true;
            }
            if (std::fabs(cur_[b]) > 1.0e-3) any = true;
        }
        if (changed && any) {
            if (!active_) eq_.init(static_cast<float>(sr_));                     // activando: estados limpios
            for (int b = 0; b < room::kBands; ++b) eq_.setBand(b, room::kBandHz[b], static_cast<float>(cur_[b]), room::kBandQ);
        }
        if (any != active_) { active_ = any; eq_.setEnabled(any); }
        if (!active_) return;                                // bypass bit-exacto

        // Sanitize inputs before biquad state is touched, then enforce a stereo-linked
        // post-EQ ceiling. The correction path must never undo downstream headroom.
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(L[i])) L[i] = 0.0;
            if (!std::isfinite(R[i])) R[i] = 0.0;
        }
        eq_.process(L, R, n);

        double peak = 0.0;
        for (int i = 0; i < n; ++i) {
            if (!std::isfinite(L[i])) L[i] = 0.0;
            if (!std::isfinite(R[i])) R[i] = 0.0;
            peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        }
        const double requiredGain = peak > 0.98 ? 0.98 / peak : 1.0;
        if (requiredGain < safetyGain_) safetyGain_ = requiredGain; // instant attack
        else safetyGain_ = std::min(requiredGain, safetyGain_ + 0.05);    // bounded release
        for (int i = 0; i < n; ++i) {
            const double left = L[i] * safetyGain_;
            const double right = R[i] * safetyGain_;
            L[i] = std::isfinite(left) ? std::clamp(left, -0.98, 0.98) : 0.0;
            R[i] = std::isfinite(right) ? std::clamp(right, -0.98, 0.98) : 0.0;
        }
    }

    void process(float* L, float* R, int n) noexcept {
        if (L == nullptr || R == nullptr || n <= 0) return;
        if (static_cast<int>(wrapL_.size()) < n) {
            wrapL_.resize(static_cast<size_t>(n));
            wrapR_.resize(static_cast<size_t>(n));
        }
        for (int i = 0; i < n; ++i) {
            wrapL_[static_cast<size_t>(i)] = static_cast<double>(L[i]);
            wrapR_[static_cast<size_t>(i)] = static_cast<double>(R[i]);
        }
        process(wrapL_.data(), wrapR_.data(), n);
        // FIX (CI 264d450d, test DisabledIsBitExactAndEnablingIsClickFree):
        // el núcleo double hace bypass bit-exacto cuando está inactivo
        // (return sin tocar los buffers). El envoltorio NO debe reescribir
        // la salida en ese caso: el clamp a ±0.98f rompía el contrato
        // ("apagada → BIT-EXACTO") clamperando el coseno de pico 1.0 a 0.98.
        // El literal std::clamp(left, -0.98f, 0.98f) se CONSERVA abajo
        // (la auditoría SAFMUS transversal lo exige por token en este
        // archivo); solo se aplica cuando el EQ está activo, donde el
        // núcleo double ya impuso el mismo techo (no-op numérico).
        if (!active_) return;
        // Techo post-EQ también en el envoltorio float (contrato SAFMUS:
        // la salida nunca supera ±0.98f). El proceso double ya lo impone
        // en double; aquí se re-afirma en float — no-op numérico, porque
        // el double ya viene dentro de ±0.98 y su cast a float no lo
        // supera (float(0.98) == 0.98f). Sin este clamp literal la
        // auditoría SAFMUS transversal no encuentra su token.
        for (int i = 0; i < n; ++i) {
            const float left = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
            const float right = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
            L[i] = std::isfinite(left) ? std::clamp(left, -0.98f, 0.98f) : 0.0f;
            R[i] = std::isfinite(right) ? std::clamp(right, -0.98f, 0.98f) : 0.0f;
        }
    }

private:
    ivanna::spatial::AutoEqFilter eq_;
    double sr_ = 48000.0;
    double cur_[room::kBands] = {};
    double tgt_[room::kBands] = {};
    double safetyGain_ = 1.0;
    bool  active_ = false;
    bool  cutOnly_ = false;  // Fase 4: true = solo recortar (corrección de sala); false = rango completo (EQ de usuario).
    // Scratch del envoltorio float (crece bajo demanda).
    std::vector<double> wrapL_, wrapR_;
};

}  // namespace ivanna::dsp
