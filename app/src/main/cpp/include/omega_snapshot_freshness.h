// omega_snapshot_freshness.h — Watchdog de frescura del snapshot (P2, 2026-11-26).
//
// HUECO MEDIDO (vivo 2026-11-26): el efecto adopta un snapshot nuevo solo
// cuando readLatest() ve una generación posterior. Si el daemon deja de
// publicar (muere, socket caído, SHM corrupta, keepalive roto), readLatest()
// devuelve false para siempre y omega_process() seguiría eternamente con
// los controles VIEJOS: ORBE/HOLOS/conservación congelados en el último
// valor, sin que nada los expire. El doctor no tenía forma de distinguir
// "daemon vivo con controles estables" de "daemon muerto con controles
// rancios" (la generación del snapshot de control SÍ avanza ~1/s cuando
// el daemon vive; cuando muere, se congela — pero el efecto no lo miraba).
//
// El watchdog vive en omega_process(), por bloque, y es puro + RT-safe
// (aritmética entera/flotante, sin alloc/lock/I/O/log):
//
//   - Se arma con la PRIMERA adopción. Antes de ella no hay nada que
//     expirar: sin control plane el efecto ya corre con sus defaults.
//   - Si pasan > 3000 ms sin adoptar un snapshot nuevo (3× el keepalive
//     de 1 s del daemon, con margen), el fade cae 1 -> 0 con rampa de
//     100 ms y el snapshot aplicado se mezcla hacia sus DEFAULTS SEGUROS:
//     amounts (ORBE/HOLOS/NP/Φ) a 0, room_wet/upmix a 0, comp/exc a 0,
//     target_gain a 1.0. La cadena pasa el audio con los operadores en
//     bypass bit-exacto: NUNCA es silencio ni se tocan master gain,
//     flags, EQ, modo ni sesión.
//   - Cuando el daemon vuelve a publicar, la adopción rearma el watchdog
//     y el fade sube 0 -> 1 con la misma rampa de 100 ms (recuperación
//     suave, sin escalón).
//   - El snapshot (re-)aplicado solo cambia cuando el fade cambia: en
//     régimen no se re-aplica nada por bloque (coste cero en caliente).
//
// Observabilidad: el beacon congela applied_control_generation cuando no
// hay adopciones (es la señal de "daemon muerto" que el doctor ya lee);
// el fade vigente vive en el contexto del efecto (snapFresh).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "omega_control_bus.h"

namespace ivanna {

// Edad máxima sin snapshot nuevo antes de considerar rancio el control.
// El daemon publica por keepalive cada 1000 ms; 3000 ms = 3 keepalives
// perdidos (muerte del daemon, no jitter de scheduling).
inline constexpr uint64_t kSnapshotStaleAfterMs = 3000u;
// Duración de la rampa de caída a defaults (y de vuelta) en ms. 100 ms
// es ~2-3 bloques a 48 kHz y está por encima del umbral de tronido de
// cualquier escalón de ganancia; por debajo, el oído no lo nota.
inline constexpr uint64_t kSnapshotStaleRampMs = 100u;

// Mezcla un snapshot hacia sus defaults seguros según fade:
// fade 1 = snapshot tal cual; fade 0 = amounts 0, target_gain 1.0,
// exc_red/comp_amount/room_wet/upmix a 0. NO toca: intensity, EQ,
// compresor (threshold), flags (Volterra/HRTF), corrección de sala,
// NPE, SAF, modo de salida, sesión ni generación. Con fade 0 la cadena
// pasa el audio con los operadores en bypass bit-exacto (no silencio).
// Re-estampa el CRC para que la copia siga siendo un snapshot válido.
inline void omega_snapshot_apply_stale_fade(OmegaDspSnapshot& dst,
                                            const OmegaDspSnapshot& src,
                                            float fade) noexcept {
    dst = src;
    const float f = std::clamp(fade, 0.0f, 1.0f);
    dst.orbe_amount = src.orbe_amount * f;
    dst.holos_amount = src.holos_amount * f;
    dst.np_amount = src.np_amount * f;
    dst.hyper_amount = src.hyper_amount * f;
    dst.upmixing_immersivity = src.upmixing_immersivity * f;
    dst.room_wet = src.room_wet * f;
    dst.comp_amount = src.comp_amount * f;
    dst.exc_red = src.exc_red * f;
    dst.target_gain = 1.0f + (src.target_gain - 1.0f) * f;
    dst.stampCrc();
}

// Mueve fade hacia target a velocidad de rampa (kSnapshotStaleRampMs).
// dtMs = duración del bloque actual en ms.
inline float omega_snapshot_fade_step(float fade, float target, uint64_t dtMs) noexcept {
    if (fade == target) return fade;
    const float step =
        static_cast<float>(dtMs) / static_cast<float>(kSnapshotStaleRampMs);
    if (target > fade) return std::min(target, fade + step);
    return std::max(target, fade - step);
}

// Estado del watchdog, por instancia de efecto (miembro del contexto).
// El contexto se construye con placement-new, así que los inicializadores
// (fade 1.0 = control vigente) sí corren; no depende de calloc.
struct SnapshotFreshnessState {
    uint64_t lastAdoptMs = 0;  // CLOCK_MONOTONIC ms de la última adopción
    bool armed = false;        // true tras la primera adopción
    float fade = 1.0f;         // 1 = control vigente, 0 = defaults seguros
    float appliedFade = 1.0f;  // último fade (re-)aplicado al motor
};

// Decisión por bloque de audio. `adopted` = este bloque adoptó un snapshot
// nuevo (readLatest true). `blockMs` = duración del bloque en ms.
// Devuelve true si el motor debe (re-)aplicar el snapshot con el fade
// vigente (faded = pendingSnap mezclado por appliedFade). En régimen
// (fade estable) devuelve false: coste cero en caliente.
inline bool snapshot_freshness_update(SnapshotFreshnessState& st, uint64_t nowMs,
                                      bool adopted, uint64_t blockMs) noexcept {
    if (adopted) {
        st.lastAdoptMs = nowMs;
        st.armed = true;
    }
    if (!st.armed) return false;
    const bool stale =
        !adopted && (nowMs - st.lastAdoptMs) > kSnapshotStaleAfterMs;
    const float target = stale ? 0.0f : 1.0f;
    st.fade = omega_snapshot_fade_step(st.fade, target, blockMs);
    if (std::fabs(st.fade - st.appliedFade) > 0.001f) {
        st.appliedFade = st.fade;
        return true;
    }
    return false;
}

}  // namespace ivanna
