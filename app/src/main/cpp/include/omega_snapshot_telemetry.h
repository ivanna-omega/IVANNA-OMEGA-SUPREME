// omega_snapshot_telemetry.h — Telemetría medida por el efecto (P2, 2026-11-26).
//
// CAUSA RAÍZ MEDIDA (vivo 2026-11-26, Qobuz sonando, sesión 4057):
// el snapshot de control (/data/adb/ivanna_omega/omega_control_snapshot)
// mostraba raw_rms = 0.0 y raw_peak = 0.0 aunque la música sonaba, y el
// beacon probaba que el efecto sí procesaba (frames a 47 979/s, un stream
// a 48 kHz; bus local de telemetría con RMS 0.0234 / pico 0.0657).
//
// La cadena de la telemetría tiene DOS rotos, y este header fija el primero:
//
//   ROTO 1 (aquí): omega_process() mide RMS/pico/bandas por bloque y los
//   escribe en ctx->pendingSnap (omega_effect.cpp), pero al adoptar un
//   snapshot nuevo del daemon hace `ctx->pendingSnap = snap` — y el
//   snapshot del daemon trae raw_rms/raw_peak/effect_frames en 0 SIEMPRE
//   (el daemon no los escribe: makeDefault() los pone en 0 y ni
//   applyPendingPerceptual ni applyFusionToSnapshot los tocan). Cada
//   adopción borraba la telemetría medida hasta el bloque siguiente, y
//   effect_frames volvía a 0 en cada generación (diente de sierra en vez
//   de contador monotónico).
//
//   ROTO 2 (effect_telemetry_mirror.h, lado daemon): el daemon nunca
//   reflejaba el bus local del efecto en el snapshot que publica, así que
//   el snapshot de control — el que lee el doctor con `od` — no podía
//   llevar RMS distinto de 0 por construcción.
//
// Este helper es puro y RT-safe (copias de floats, sin alloc/lock/I/O):
// se llama en omega_process() justo al adoptar, antes de sobrescribir
// pendingSnap, y conserva los campos medidos por el efecto. El snapshot
// de control (generación, sesión, amounts, flags, modo) viene del daemon;
// la telemetría (RMS/pico/bandas/frames/supreme) viene del efecto. Los
// dos planos no se mezclan: el writer del bus de control sigue siendo
// solo el daemon (seqlock de escritor único intacto).
#pragma once

#include <cstdint>

#include "omega_control_bus.h"

namespace ivanna {

// Conserva la telemetría medida por el efecto al adoptar un snapshot
// nuevo del daemon: copia SOLO los campos que el efecto mide por bloque
// desde `measured` (el pendingSnap viejo) a `dst` (el snapshot recién
// adoptado). Todo lo demás (control, sesión, generación, modo, amounts)
// queda como lo publicó el daemon.
//   - raw_rms / raw_peak: nivel del último bloque procesado.
//   - raw_band_low/mid/high/spread: bandas espectrales del último bloque.
//   - effect_frames: total de frames procesados (monotónico tras este
//     fix: antes se reseteaba a 0 en cada adopción).
//   - supreme_declipped_peaks / supreme_tape_mag / supreme_snn_spikes /
//     supreme_subsample_delay: telemetría de los Ejes Supremos.
// No toca generation/timestamp/crc: el bus los estampa al publicar.
inline void omega_preserve_effect_telemetry(OmegaDspSnapshot& dst,
                                            const OmegaDspSnapshot& measured) noexcept {
    dst.raw_rms = measured.raw_rms;
    dst.raw_peak = measured.raw_peak;
    dst.raw_band_low = measured.raw_band_low;
    dst.raw_band_mid = measured.raw_band_mid;
    dst.raw_band_high = measured.raw_band_high;
    dst.raw_band_spread = measured.raw_band_spread;
    dst.effect_frames = measured.effect_frames;
    dst.supreme_declipped_peaks = measured.supreme_declipped_peaks;
    dst.supreme_tape_mag = measured.supreme_tape_mag;
    dst.supreme_snn_spikes = measured.supreme_snn_spikes;
    dst.supreme_subsample_delay = measured.supreme_subsample_delay;
}

}  // namespace ivanna
