// effect_telemetry_mirror.h — P2 (2026-11-26): el daemon refleja la
// telemetría medida por el efecto en el snapshot de control que publica.
//
// CAUSA RAÍZ MEDIDA (vivo 2026-11-26, Qobuz sonando, sesión 4057):
// el snapshot de control mostraba raw_rms = 0.0 / raw_peak = 0.0 /
// effect_frames = 0 aunque la música sonaba. El efecto SÍ mide por
// bloque (omega_effect.cpp: mide RMS/pico/bandas y los publica en su bus
// LOCAL, /data/adb/ivanna_omega/omega_effect_ctrl_local, con RMS 0.0234
// y pico 0.0657 en vivo), pero el snapshot de control — el que lee el
// doctor con `od` — lo publica SOLO el daemon, y el daemon jamás leía
// el bus local: lastSnap nace de makeDefault()/makeNeutral() (RMS 0) y
// ni applyPendingPerceptual ni applyFusionToSnapshot tocan esos campos.
//
// El fix NO es que el efecto escriba en el bus de control: el seqlock
// es de escritor único (el daemon) y un segundo escritor rompería el
// protocolo. El daemon sigue siendo el único escritor del bus de
// control; aquí solo LEE el bus local del efecto (reader) y COPIA los
// campos de telemetría en lastSnap antes de publicar.
//
//   - Niveles (raw_rms/raw_peak/bandas/supreme_*): se copian del último
//     snapshot local FRESCO (timestamp_ms con edad <= 2000 ms). Si el
//     bus local está rancio (sin audio: el efecto no publica sin
//     bloques), se publican CEROS: el silencio es la verdad cuando no
//     hay bloques, y un nivel congelado mentiría igual que el 0 de hoy.
//   - effect_frames: el local se reseteaba en cada adopción del efecto
//     (roto 1, ya fijado por omega_preserve_effect_telemetry), y el
//     archivo local puede venir de un efecto viejo. Por eso el daemon
//     reconstruye un total MONOTÓNICO acumulando deltas, con detección
//     de reseteo (si el local baja, el delta es el valor actual, no un
//     negativo). El total nunca baja y nunca se pone en 0 por silencio.
//   - Control intacto: el espejo NO toca generación, timestamp (los
//     estampa publish()), sesión, amounts, flags, modo ni ningún campo
//     de control. Si el bus local no existe o no abre, el espejo se
//     salta (fail-closed: el snapshot queda como hoy, en ceros).
//
// Puro y testeable en host (sin NDK, sin teléfono): poll() necesita un
// OmegaControlBus real, que en el test se abre sobre un archivo temporal
// en /tmp. Sin alloc/lock/I/O propio: solo el readLatest() lock-free del
// bus. El bucle del daemon lo llama ~1 vez/s (en cada publicación,
// ivanna_daemon.cpp), no en el hot path de fusión.
#pragma once

#include <cstdint>

#include "include/omega_control_bus.h"

namespace ivanna::daemon {

// Edad máxima del snapshot local para considerarlo señal viva. El efecto
// publica en su bus local en CADA bloque (~40 ms con música); 2000 ms es
// 50 bloques de margen y cae a 0 en ~2-3 s tras una pausa (el keepalive
// del daemon publica 1 vez/s, así que el 0 llega como mucho 2-3 s tarde).
inline constexpr uint64_t kEffectTelemetryMaxAgeMs = 2000u;

// Espejo de telemetría efecto -> snapshot de control. Estado persistente
// del bucle del daemon (una instancia por proceso, nunca se copia).
struct EffectTelemetryMirror {
    // Última generación local vista (para readLatest) y último conteo de
    // frames local (para el delta del total monotónico).
    uint64_t lastLocalGen = 0;
    uint64_t lastLocalFrames = 0;
    // Total de frames procesados por el efecto desde que el daemon
    // arranca (monotónico; sobrevive a reseteos del contador local).
    uint64_t totalFrames = 0;
    // EVOL-LIVEPATH (consumer_generation muerto): última generación de
    // control que el efecto confirmó aplicar (applied_control_generation
    // del beacon, que omega_effect.cpp publica en onBlock() con la
    // generación del snapshot que adoptó). El campo consumer_generation
    // del snapshot (@32) nacía en 0 y NADIE lo escribía: el daemon es el
    // único escritor del bus de control y el efecto no puede escribirlo
    // (seqlock de escritor único). El espejo lo copia aquí, en el mismo
    // publish() que los niveles: 0 = el efecto aún no confirmó nada.
    uint64_t lastBeaconAppliedGen = 0;
    bool haveBeaconGen = false;
    // Anota la generación aplicada leída del beacon (una vez por
    // publicación del daemon; el bucle la lee con readEffectBeacon()).
    // Un gen 0 es válido (efecto procesando sin snapshot adoptado) y se
    // anota igual: haveBeaconGen distingue "nunca leído" de "leído en 0".
    void noteBeaconAppliedGeneration(uint64_t gen) noexcept {
        lastBeaconAppliedGen = gen;
        haveBeaconGen = true;
    }
    // Último snapshot local visto (solo importan sus campos de
    // telemetría y su timestamp_ms para la frescura).
    OmegaDspSnapshot lastLocal{};
    bool haveLocal = false;

    // Lee el bus local del efecto (si hay una generación nueva) y decide
    // si la telemetría es fresca. Devuelve true si el último snapshot
    // local tiene edad <= kEffectTelemetryMaxAgeMs respecto a nowMs
    // (CLOCK_MONOTONIC, el mismo reloj que estampa publish()).
    // Llamar una vez por publicación del daemon; el coste es un
    // readLatest() lock-free (casi gratis si no hay generación nueva).
    bool poll(const OmegaControlBus& bus, uint64_t nowMs) noexcept {
        OmegaDspSnapshot cur{};
        if (bus.readLatest(cur, lastLocalGen)) {
            // Delta con detección de reseteo del contador local: si el
            // local baja (adopción vieja, reinicio del efecto), el delta
            // es el valor actual, nunca un negativo ni un salto atrás.
            totalFrames += (cur.effect_frames >= lastLocalFrames)
                               ? (cur.effect_frames - lastLocalFrames)
                               : cur.effect_frames;
            lastLocalFrames = cur.effect_frames;
            lastLocal = cur;
            haveLocal = true;
        }
        if (!haveLocal) return false;
        return (nowMs - lastLocal.timestamp_ms) <= kEffectTelemetryMaxAgeMs;
    }

    // Aplica la telemetría a `dst` (el lastSnap del daemon, antes de
    // publish()). Con fresh=true copia los niveles del último snapshot
    // local; con fresh=false los pone en 0 (silencio honesto: sin
    // bloques no hay nivel que medir). effect_frames siempre lleva el
    // total monotónico. NO toca ningún campo de control.
    void applyTo(OmegaDspSnapshot& dst, bool fresh) const noexcept {
        if (fresh) {
            dst.raw_rms = lastLocal.raw_rms;
            dst.raw_peak = lastLocal.raw_peak;
            dst.raw_band_low = lastLocal.raw_band_low;
            dst.raw_band_mid = lastLocal.raw_band_mid;
            dst.raw_band_high = lastLocal.raw_band_high;
            dst.raw_band_spread = lastLocal.raw_band_spread;
            dst.supreme_declipped_peaks = lastLocal.supreme_declipped_peaks;
            dst.supreme_tape_mag = lastLocal.supreme_tape_mag;
            dst.supreme_snn_spikes = lastLocal.supreme_snn_spikes;
            dst.supreme_subsample_delay = lastLocal.supreme_subsample_delay;
        } else {
            dst.raw_rms = 0.0f;
            dst.raw_peak = 0.0f;
            dst.raw_band_low = 0.0f;
            dst.raw_band_mid = 0.0f;
            dst.raw_band_high = 0.0f;
            dst.raw_band_spread = 0.0f;
            dst.supreme_declipped_peaks = 0u;
            dst.supreme_tape_mag = 0.0f;
            dst.supreme_snn_spikes = 0u;
            dst.supreme_subsample_delay = 0.0f;
        }
        dst.effect_frames = totalFrames;
        // EVOL-LIVEPATH: la generación que el efecto confirmó aplicar. Se
        // copia siempre que el beacon se haya leído alguna vez (también
        // con fresh=false: es un acuse de control, no un nivel; si el
        // efecto murió, el último valor confirmado sigue siendo verdad
        // histórica y su frescura la dicen effect_frames/timestamp).
        if (haveBeaconGen) dst.consumer_generation = lastBeaconAppliedGen;
    }
};

}  // namespace ivanna::daemon
