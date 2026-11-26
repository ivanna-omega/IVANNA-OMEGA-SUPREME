// effect_sample_rate.hpp — Tasa de muestreo del FusionCore sin SET_CONFIG.
//
// EVOL-LIVEPATH (2026-11-26): en este HAL (Qualcomm) EFFECT_CMD_SET_CONFIG
// jamás llega al efecto, así que lastConfiguredSr es el default 48000
// aunque el hilo del mixer corra a otra tasa. El FusionCore nacía a
// 48000.0f (v2.5.486: a lastConfiguredSr, que sin SET_CONFIG sigue siendo
// 48000). La tasa REAL que ve el efecto es la cadencia de sus callbacks:
// el CallbackClockMonitor la mide en absoluto (frames / CLOCK_MONOTONIC,
// ventana >= 100 ms), sin depender del nominal. Estas funciones puras
// deciden (a) cuándo crear el núcleo y (b) a qué tasa, para que el motor
// no nazca desafinado en un hilo a 44.1k/96k.
//
// Medición (host, test_effect_sample_rate): alimentando el reloj de
// callback con 441 frames cada 10 ms, la tasa observada converge a 44100
// Hz; con 480 frames, a 48000 Hz. En el Moto G85 el hilo del mixer es 48
// kHz (HAL 48 kHz, USB_HEADSET): la tasa medida es 48000 y el núcleo nace
// igual que hoy — sin cambio de sonido. Una fuente a 44.1 kHz NO cambia
// nada: AudioFlinger la remuestrea a la tasa del mixer ANTES del efecto,
// así que el efecto nunca ve 44.1 kHz; lo que se "pierde" a 44.1 kHz es
// solo el remuestreo previo de AudioFlinger, fuera del alcance del motor.
#pragma once

#include <cmath>
#include <cstdint>

namespace ivanna {

// Espera máxima desde el primer bloque para que el reloj de callback
// valide la tasa medida antes de crear el núcleo (el arranque no se
// retrasa más: a los 200 ms se crea con el mejor dato disponible).
inline constexpr uint64_t kEffectCoreCreateWaitMs = 200u;

// ¿Crear ya el FusionCore? Con SET_CONFIG el HAL ya dijo la tasa: crear
// inmediato. Sin él, esperar a que el reloj de callback valide la tasa
// medida, o a que pasen 200 ms desde el primer bloque (fallback).
inline bool shouldCreateFusionCore(bool srFromSetConfig, bool clockValid,
                                   uint64_t nowMs, uint64_t firstBlockMs) noexcept {
    if (srFromSetConfig) return true;
    if (clockValid) return true;
    if (firstBlockMs == 0) return false;
    return (nowMs - firstBlockMs) >= kEffectCoreCreateWaitMs;
}

// Tasa del núcleo: manda SET_CONFIG si llegó; si no, la medida del reloj
// de callback; si no hay medida, lastConfiguredSr si es sana; si no, 48000.
inline uint32_t chooseEffectSampleRate(uint32_t lastConfiguredSr, bool srFromSetConfig,
                                       bool clockValid, double observedRateHz) noexcept {
    if (srFromSetConfig && lastConfiguredSr >= 8000u && lastConfiguredSr <= 768000u)
        return lastConfiguredSr;
    if (clockValid && std::isfinite(observedRateHz) && observedRateHz >= 8000.0 &&
        observedRateHz <= 768000.0)
        return static_cast<uint32_t>(std::llround(observedRateHz));
    if (lastConfiguredSr >= 8000u && lastConfiguredSr <= 768000u) return lastConfiguredSr;
    return 48000u;
}

}  // namespace ivanna
