// hardware_clock_from_muscle.hpp — Muestra de reloj hardware desde el músculo ASRC.
//
// EVOL-LIVEPATH (2026-11-26): hardware_rate_millihz del beacon estaba en 0
// aunque la sesión suena a 44.1/48 kHz. Causa raíz (leída en código, no
// adivinada): la cadena efecto <- snapshot HAL estaba rota en el PRIMER
// eslabón. El efecto SÍ tiene HardwareClockReader + HardwareClockTracker
// (omega_effect.cpp: el reader se abre en creación y SET_CONFIG, el tracker
// alimenta onHardwareClockEstimate -> beacon.hardware_rate_millihz), pero
// NADIE publica en /data/vendor/ivanna_audio/clock_snapshot.v2:
// HalClockProducer existe y está testeado, pero ningún código de producción
// lo instancia, y su config por defecto se niega a abrir un PCM por diseño
// ("daemon no abre PCM": sin dueño del stream no hay correlación válida).
// El adaptador HAL (audio_hardware_clock_hal_adapter.hpp) también solo lo
// usan los tests.
//
// La fuente viva que SÍ existe es el músculo ASRC v6: publica en el estado
// de fusión (IvannaFusionState) el par (timestamp, posición) del reloj real
// — clock_mmap_ns/clock_mmap_frames (HWSYNC del kernel) o, si el stream no
// es mmap, clock_write_ns/frames_written (write-timing) — más la tasa real
// del stream (stream_rate_hz: 48000, 44100, ...). El daemon lee ese estado
// cada 50 ms; este helper construye la HardwareClockSample que el daemon
// re-publica con HardwareClockPublisher para que el efecto la lea. El
// daemon es el único publicador (un solo proceso, sin competencia) y solo
// publica muestras del músculo: nunca inventa una tasa.
//
// Identidad del stream: streamId = sesión de música descubierta (la única
// identidad de stream que el daemon conoce); sin sesión confirmada todavía
// se usa 1 ("el stream de salida que el músculo mide") para no dejar la
// tasa en 0 los ~9 s que tarda el gate en confirmar. routeGeneration =
// generación de sesión (cambia al cambiar de sesión: el tracker re-ancla).
// La frescura del músculo (magic/version/salud/timestamp < 500 ms) la
// verifica el llamador (muscleFresh en ivanna_daemon.cpp), no este helper.
//
// Puro y testeable en host (sin NDK, sin teléfono, sin I/O).
#pragma once

#include <cstdint>

#include "../include/audio_hardware_clock.hpp"
#include "../include/ivanna_fusion_state.h"

namespace ivanna::daemon {

// Construye la muestra de reloj hardware desde el estado del músculo.
// Devuelve false (sin tocar `out`) si no hay muestra válida: sin par
// (timestamp, posición) del reloj, tasa fuera de [8000, 768000], o
// stream/route sin identidad. Fail-closed: el llamador no publica nada.
inline bool buildHardwareClockSample(const IvannaFusionState& st, int32_t sessionId,
                                     uint64_t sessionGen,
                                     ivanna::audio::HardwareClockSample& out) noexcept {
    if (st.stream_rate_hz < 8000 || st.stream_rate_hz > 768000) return false;
    ivanna::audio::HardwareClockSample s{};
    s.nominalSampleRateHz = st.stream_rate_hz;
    // Sin sesión confirmada: identidad sintética estable (1/1). El tracker
    // re-ancla por routeGeneration cuando la sesión se confirma (gen >= 1)
    // o cambia; un salto de timestamp > 2 s también re-ancla solo.
    s.streamId = (sessionId > 0) ? static_cast<uint64_t>(sessionId) : 1ULL;
    s.routeGeneration = (sessionGen > 0) ? sessionGen : 1ULL;
    if (st.clock_mmap_ns > 0) {
        // Par preferido: timestamp HWSYNC del kernel + posición mmap.
        s.hardwareTimestampNs = static_cast<uint64_t>(st.clock_mmap_ns);
        s.presentedFrames = static_cast<uint64_t>(st.clock_mmap_frames);
    } else if (st.clock_write_ns > 0) {
        // El stream no es mmap: par write-timing (también reloj HAL real).
        s.hardwareTimestampNs = static_cast<uint64_t>(st.clock_write_ns);
        s.presentedFrames = static_cast<uint64_t>(st.frames_written);
    } else {
        return false;  // sin reloj: no se publica nada
    }
    out = s;
    return true;
}

}  // namespace ivanna::daemon
