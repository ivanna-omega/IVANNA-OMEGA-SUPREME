/**
 * genre_smooth_state.hpp — Estabilizador de género + mix (0046).
 * © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
 *
 * DISEÑO DE ESSE (verbatim, 2026-11-17 22:41 CST) — dos funciones:
 *   1. update_genre_classifier() — SOLO en la inferencia nueva del
 *      clasificador (~1 Hz): EMA + histéresis + dwell de 3 hits.
 *      Escribe target_mix (low_mix=0.45 / high_mix=1.00).
 *   2. step_genre_mix_rt() — en el hilo RT de audio: suavizado
 *      exponencial tau 0.60 s (transición completa ~1.8 s al 95 %).
 *
 * ADAPTACIONES DOCUMENTADAS (no rediseño — los números de Esse son
 * intocables: alpha 0.30, umbrales 0.75/0.40, dwell 3, tau 0.60 s):
 *   (a) target_mix y current_mix son std::atomic<float>: el escritor es
 *       el hilo NPE (evaluateAndDrive / processBlock) y los lectores son
 *       hilos JNI/telemetría. Una lectura rasgada sería benigna aquí
 *       (un valor transitorio de mix no rompe audio), pero el atómico es
 *       gratis y elimina la carrera por completo. Los demás campos solo
 *       los toca el hilo lento → plain.
 *   (b) Siembra en frío en la primera inferencia: active_genre se fija
 *       desde la primera probabilidad (≥0.75 → 1, si no → 0). Sin ella,
 *       el gate del cerebro bloquearía la primera etiqueta de la familia
 *       1 durante ~3 s. El régimen permanente (histéresis + dwell) no
 *       cambia: la siembra solo corre en la primera llamada.
 *   (c) Las constantes kTHigh/kTLow/kDwell se definen arriba de la
 *       comprobación de inicialización (mismo valores, solo reorden).
 *
 * CONTEXTO (verificado en código, REPORT.md 0046):
 *   - El clasificador vive en neuromorphic/autonomous_brain.hpp:375-385:
 *     umbrales duros instantáneos sobre cf_db + ratios, sin suavizado.
 *     Ventana 4096 muestras (~85 ms) → la etiqueta podía cambiar hasta
 *     ~11.7×/s; en vivo brincaba Hip-Hop/EDM ↔ Electrónica ↔ Pop cada
 *     1–2 s en la misma canción.
 *   - El cerebro llama a update decimado a ~1 Hz (12 ventanas): el
 *     diseño asume una inferencia por segundo (alpha 0.30 → tau ~2.8 s).
 *     Sin decimado el tau caería a ~0.28 s y los brincos pasarían igual.
 *   - El mix que cicla 0.45↔1.00 es el blend de re-inyección de captura
 *     (PlaybackCaptureService.kt:1328-1331): 0.45 con módulo, userMixGain
 *     (hasta 1.00) sin él. NINGÚN camino de código conecta el género con
 *     ese target — el ciclo lo mueve el flapping de sesión/beacon
 *     (causa raíz: 0043 + 0044). Este header estabiliza el género y
 *     expone el mix suavizado; el escalón del mix de captura se suaviza
 *     en Kotlin con la MISMA fórmula (step exponencial tau 0.60 s).
 *
 * RT-safe: sin heap, sin bloqueos (relaxed atomics), sin excepciones.
 */
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ivanna::genre {

struct GenreSmoothState {
    float smoothed_prob = 0.0f;
    bool  initialized   = false;
    int   consecutive_hits = 0;
    int   active_genre  = 0;
    std::atomic<float> target_mix{0.45f}; // escrito por el hilo/tick lento del clasificador
    std::atomic<float> current_mix{0.45f}; // suavizado en el hilo RT de audio
};

// 1) Llamar SOLO cuando hay nueva inferencia del clasificador (ej. cada 1.0 s).
//    NO corre en el loop de muestras de audio.
inline float update_genre_classifier(float raw_prob,
                                     float low_mix,
                                     float high_mix,
                                     GenreSmoothState& st) noexcept {
    constexpr float kTHigh = 0.75f;
    constexpr float kTLow  = 0.40f;
    constexpr int   kDwell = 3;

    const float p = std::isfinite(raw_prob) ? std::clamp(raw_prob, 0.0f, 1.0f) : 0.0f;
    if (!st.initialized) {
        st.smoothed_prob = p; // evita arranque lento en frío
        st.initialized = true;
        // 0046 (b) — siembra en frío de la familia activa (ver cabecera).
        st.active_genre = (p >= kTHigh) ? 1 : 0;
        st.consecutive_hits = 0;
    } else {
        constexpr float alpha = 0.30f; // tau ~2.8 s @ 1 Hz
        st.smoothed_prob += alpha * (p - st.smoothed_prob);
    }

    if (st.smoothed_prob >= kTHigh) {
        if (st.active_genre == 0) {
            if (++st.consecutive_hits >= kDwell) {
                st.active_genre = 1;
                st.consecutive_hits = 0;
            }
        } else {
            st.consecutive_hits = 0;
        }
    } else if (st.smoothed_prob <= kTLow) {
        st.consecutive_hits = 0;
        st.active_genre = 0;
    } else {
        // Zona muerta (0.40..0.75): reinicia el conteo para exigir racha ininterrumpida
        st.consecutive_hits = 0;
    }

    st.target_mix.store((st.active_genre == 1) ? high_mix : low_mix,
                        std::memory_order_relaxed);
    return st.target_mix.load(std::memory_order_relaxed);
}

// 2) Llamar en el hilo RT de audio (por muestra o por bloque con dt real en segundos).
//    Para transición completa de ~1.8 s (95%): tau = 0.60 s (3 * tau = 1.8 s).
inline float step_genre_mix_rt(GenreSmoothState& st, float dt_sec) noexcept {
    constexpr float kTauSec = 0.60f;
    const float coef = 1.0f - std::exp(-std::max(dt_sec, 0.0f) / kTauSec);
    const float cur = st.current_mix.load(std::memory_order_relaxed);
    const float tgt = st.target_mix.load(std::memory_order_relaxed);
    const float next = cur + coef * (tgt - cur);
    st.current_mix.store(next, std::memory_order_relaxed);
    return next;
}

} // namespace ivanna::genre
