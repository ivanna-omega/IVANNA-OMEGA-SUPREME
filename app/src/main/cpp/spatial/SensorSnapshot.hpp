// SensorSnapshot.hpp
// ===========================================================================
//  IVANNA — Snapshot versionado del sensor acústico (canal lateral, v2.5.506).
//
//  © 2026 Luis Uriel Pimentel Pérez · GORE TNS. All rights reserved.
//
//  EL HUECO DE RUTA B (consejo Luna/Haiku, 2026-11-26): el efecto (Ruta B)
//  y el hilo de audio (Ruta A) NUNCA deben llamar a HTTP ni a la app web,
//  y el audio NUNCA debe depender del envío. El dato sale por un canal
//  lateral explícito:
//
//    sensor (RealAcousticSensor, seqlock latestFrame, ~85 ms)
//      → SensorSnapshot { version, source_id, sequence, timestamp, valid, frame }
//      → SensorSnapshotChannel (latest value wins: si el consumidor se
//        retrasa, se descartan snapshots viejos, nunca se bloquea nadie)
//      → consumidor dedicado (hilo propio, fuera del hilo de audio)
//        → validateSensorSnapshot() → ingestHyperSensorFrame()
//
//  REGLAS (no negociables):
//    * Validar tamaño/versión/rangos/frescura ANTES de tocar el estado:
//      un snapshot incompleto o rancio se rechaza y el estado no se mueve.
//    * Aceptar un snapshot NO implica aplicar su salida al audio:
//      ingestión, aprendizaje y aplicación van separados (el operador Φ
//      y el bucle neuro deciden con sus propios topes y watchdog).
//    * Sin micrófono o con snapshot inválido, la contribución es
//      EXACTAMENTE cero: el bypass P0 queda bit-idéntico.
//    * El canal es un tubo tonto: publish() NO valida (el consumidor
//      valida); publish() NUNCA bloquea el hilo de audio (mutex corto,
//      solo lo llaman hilos de control).
//
//  DOMINIO DE TIEMPO: CLOCK_MONOTONIC en microsegundos, el mismo que
//  monotonicNowUs() (neuro_acoustic_closed_loop.hpp). En Kotlin eso es
//  SystemClock.uptimeMillis()*1000 — NO elapsedRealtime (CLOCK_BOOTTIME).
//  Un snapshot con timestamp de otro reloj se rechaza por frescura.
// ===========================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

#include "RealAcousticSensor.hpp"

namespace ivanna::spatial {

// ── Contrato del snapshot ───────────────────────────────────────────────────
inline constexpr std::uint32_t kSensorSnapshotVersion = 1;
// Fuentes conocidas. Cualquier otro source_id se rechaza.
inline constexpr std::uint32_t kSensorSourceRouteA = 1;  // app, AudioRecord
inline constexpr std::uint32_t kSensorSourceRouteB = 2;  // efecto audioserver
// Frescura: el consumidor debe entregar el snapshot dentro de 1 s de la
// medición (el frame del sensor llega cada ~85 ms; 1 s absorbe el
// scheduling del consumidor sin dejar entrar medidas rancias). El bucle
// neuro/Φ aplica además su propia ventana de 250 ms para ACTIVARSE
// (kStaleTimeoutUs): un snapshot aceptado pero viejo nunca mueve audio.
inline constexpr std::uint64_t kSensorSnapshotMaxAgeUs = 1000000;
inline constexpr std::uint64_t kSensorSnapshotMaxFutureUs = 100000;

struct SensorSnapshot {
    std::uint32_t version = kSensorSnapshotVersion;
    std::uint32_t sourceId = 0;      // 1 = Ruta A, 2 = Ruta B
    std::uint64_t sequence = 0;      // == frame.frame_index (monótona por fuente)
    std::uint64_t timestampUs = 0;   // CLOCK_MONOTONIC, µs
    bool valid = false;              // false = medición no confiable
    SensorFrame frame{};             // 24 bandas ERB (mag/err/coh, level)
};

// ── Validación pura (sin estado) ────────────────────────────────────────────
// Devuelve false sin tocar nada si el snapshot está incompleto, es de
// versión/fuente desconocida, trae valores fuera de rango o no finitos,
// la secuencia no coincide con el frame, o el timestamp es rancio o del
// futuro. Los rangos son LOS MISMOS que HyperStateOperator::setSensorFrame
// (mag [-120,30], err [-60,60], coh [0,1], level [-120,10]): un snapshot
// que pasa aquí no puede corromper al operador.
inline bool validateSensorSnapshot(const SensorSnapshot& s,
                                   std::uint64_t nowUs) noexcept {
    if (s.version != kSensorSnapshotVersion) return false;
    if (s.sourceId != kSensorSourceRouteA &&
        s.sourceId != kSensorSourceRouteB)
        return false;
    if (!s.valid || !s.frame.valid) return false;
    if (s.sequence == 0) return false;
    if (s.frame.frame_index != s.sequence) return false;
    if (s.timestampUs == 0) return false;
    if (nowUs != 0) {
        if (s.timestampUs > nowUs + kSensorSnapshotMaxFutureUs) return false;
        if (nowUs >= s.timestampUs &&
            nowUs - s.timestampUs > kSensorSnapshotMaxAgeUs)
            return false;
        if (s.timestampUs > nowUs &&
            s.timestampUs - nowUs > kSensorSnapshotMaxFutureUs)
            return false;
    }
    const SensorFrame& f = s.frame;
    if (!std::isfinite(f.level_dbfs) || f.level_dbfs < -120.0f ||
        f.level_dbfs > 10.0f)
        return false;
    for (int b = 0; b < SensorFrame::kBands; ++b) {
        const float m = f.magnitude_db[b];
        const float e = f.error_db[b];
        const float c = f.coherence[b];
        if (!std::isfinite(m) || !std::isfinite(e) || !std::isfinite(c))
            return false;
        if (m < -120.0f || m > 30.0f) return false;
        if (e < -60.0f || e > 60.0f) return false;
        if (c < 0.0f || c > 1.0f) return false;
    }
    return true;
}

// ── Constructor desde el layout ERB[74] de JNI ─────────────────────────────
// f74: mag[0..23], err[24..47], coh[48..71], level[72], valid[73] (1/0) —
// el mismo layout que nativeSensorLatestFrame. Devuelve false (sin tocar
// `out`) si el puntero es nulo, valid=0, o algún valor es no finito o
// fuera de los rangos del sensor. La secuencia es el frame_index del
// sensor; el llamador la pasa (nativeSensorFrameIndex).
inline bool snapshotFromErb74(const float* f74, std::uint32_t sourceId,
                              std::uint64_t sequence,
                              std::uint64_t timestampUs,
                              SensorSnapshot& out) noexcept {
    if (f74 == nullptr) return false;
    if (sourceId != kSensorSourceRouteA &&
        sourceId != kSensorSourceRouteB)
        return false;
    if (sequence == 0 || timestampUs == 0) return false;
    if (!(f74[73] >= 0.5f)) return false;  // valid=0
    SensorSnapshot s{};
    s.version = kSensorSnapshotVersion;
    s.sourceId = sourceId;
    s.sequence = sequence;
    s.timestampUs = timestampUs;
    s.valid = true;
    s.frame.valid = true;
    s.frame.frame_index = sequence;
    for (int b = 0; b < SensorFrame::kBands; ++b) {
        const float m = f74[b];
        const float e = f74[24 + b];
        const float c = f74[48 + b];
        if (!std::isfinite(m) || !std::isfinite(e) || !std::isfinite(c))
            return false;
        if (m < -120.0f || m > 30.0f) return false;
        if (e < -60.0f || e > 60.0f) return false;
        if (c < 0.0f || c > 1.0f) return false;
        s.frame.magnitude_db[b] = m;
        s.frame.error_db[b] = e;
        s.frame.coherence[b] = c;
    }
    const float level = f74[72];
    if (!std::isfinite(level) || level < -120.0f || level > 10.0f)
        return false;
    s.frame.level_dbfs = level;
    out = s;
    return true;
}

// ── Canal lateral: latest value wins ────────────────────────────────────────
// El productor publica; el consumidor lee el ÚLTIMO snapshot. Si el
// consumidor se retrasa, los viejos se descartan (nunca se encola, nunca
// se bloquea el productor). publish()/latest() solo los llaman hilos de
// control (captura/consumidor), NUNCA el hilo de audio: el mutex corto
// no puede entrar en el hot path.
class SensorSnapshotChannel {
public:
    void publish(const SensorSnapshot& s) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        // Latest wins, pero nunca retroceder: una secuencia vieja que
        // llega tarde no pisa a una nueva.
        if (has_ && s.sequence < sequence_) return;
        latest_ = s;
        sequence_ = s.sequence;
        has_ = true;
    }

    // false si aún no se publicó nada. Copia el último snapshot.
    bool latest(SensorSnapshot& out) const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!has_) return false;
        out = latest_;
        return true;
    }

    [[nodiscard]] std::uint64_t sequence() const noexcept {
        return sequence_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool hasSnapshot() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return has_;
    }

    void reset() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        latest_ = SensorSnapshot{};
        sequence_.store(0, std::memory_order_relaxed);
        has_ = false;
    }

private:
    mutable std::mutex mutex_;
    SensorSnapshot latest_{};
    std::atomic<std::uint64_t> sequence_{0};
    bool has_ = false;
};

}  // namespace ivanna::spatial
