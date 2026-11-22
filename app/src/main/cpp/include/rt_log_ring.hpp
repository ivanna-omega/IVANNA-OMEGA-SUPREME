// rt_log_ring.hpp — Anillo de eventos de diagnóstico lock-free para el hilo
// de audio en tiempo real (RT).
//
// © 2026 Luis Uriel Pimentel Pérez — IVANNA OMEGA SUPREME
//
// PROBLEMA (check_rt_safety.py, 6 violaciones, 2026-11-14): omega_process,
// logPassthroughThrottled y logDspShedStateThrottled llamaban a
// __android_log_print (LOGI/LOGW/LOGE) DESDE el callback de audio.
// __android_log_print hace I/O y puede tomar locks internos: en el hilo RT
// es una fuente de jitter y de bloqueos no acotados. El throttle de 5 s
// reducía la frecuencia pero no eliminaba el riesgo: una sola llamada en
// el momento equivocado basta.
//
// SOLUCIÓN: el hilo RT ya no loguea. Solo hace push() a este anillo:
//   - Sin alocaciones, sin locks, sin I/O, sin std::string, sin excepciones.
//   - Un único atomic fetch_add reserva el slot; el payload se escribe y
//     la secuencia se publica con release (el drenador lee con acquire).
//   - Capacidad fija 64 eventos; si el drenador no corre, los eventos más
//     viejos se sobrescriben (pérdida de diagnóstico, nunca de audio).
//   - clock_gettime(CLOCK_MONOTONIC) es vDSO en Android: sin syscall ni lock.
//   - FIX (TSan, v2.5.457): el payload TAMBIÉN es atómico. La reserva por
//     fetch_add es única por secuencia, pero los slots se REUSAN cada 64
//     pushes: dos push() concurrentes con secuencias a 64 de distancia
//     escriben el MISMO slot, y el drenador lee mientras otro push
//     escribe — con campos planos eso es data race (UB) y TSan lo caza.
//     Con campos atómicos (relaxed; la publicación sigue siendo seq con
//     release/acquire) cada campo leído es un valor completo escrito por
//     algún push, sin tearing, y push() sigue siendo lock-free y
//     wait-free. El coste es nulo en x86_64/ARM64 (atómicos alineados
//     de 1/4/8 bytes compilan a loads/stores planos).
//
// DRENAJE (hilo NO-RT, nunca en el callback):
//   - Ruta B: omega_command() en omega_effect.cpp llama a
//     ivanna::rtlog::drain() con un callback que sí puede usar LOGI/LOGW.
//   - Ruta A: nativeGetDspShed() en ivanna_omega_jni.cpp (poller 1 Hz)
//     drena los eventos de shed con el mismo mecanismo.
//   - drain() es el ÚNICO consumidor: dos drenadores concurrentes podrían
//     duplicar o perder un evento (el CAS de limpieza lo hace at-most-once
//     por slot, pero dos drenadores pueden repartirse los slots). En la
//     práctica cada proceso tiene un solo drenador (audioserver y app son
//     procesos distintos, cada uno con su propia copia del anillo).
//
// El payload lleva los números ya formateables (contadores, EMA, pico):
// el drenador no necesita leer ningún estado del DSP, solo el evento.
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>

namespace ivanna::rtlog {

// Código de evento. El drenador traduce a texto; el hilo RT solo publica
// el código + 4 floats + un motivo numérico (sin strings en RT).
enum class Event : uint8_t {
    kPassthrough = 0,      // omega_process: passthrough; reason = PassthroughReason; a = frames
    kAutoEnable,           // omega_process: auto-enable (EFFECT_CMD_ENABLE no llegó)
    kFusionCreated,        // omega_process: FusionEngine lazy creado en 1er bloque
    kFusionCreateFailed,   // omega_process: FusionEngine lazy falló (nothrow null)
    kDspShedActive,        // nativeProcess: shed ACTIVO; a=sheds b=total c=emaUs d=peakUs
    kDspShedEnded,         // nativeProcess: shed terminado; a=sheds b=total c=emaUs d=peakUs
    kOutputMode,           // omega_process: cambio de modo de salida; reason = 1 SPEAKERS, 0 HEADPHONES/UNKNOWN; a = generation (float, exacta < 2^24)
    kCount,
};

// Motivos de passthrough (espejo de PassthroughReason en omega_effect.cpp;
// el valor numérico es el contrato: no reordenar sin actualizar el drenador).
enum class PassthroughReason : uint8_t {
    kNotEnabledSilent = 0,
    kNotStereo,
    kIntegrityFail,
    kAntitamperLockdown,
    kFusionCoreNull,
    kBypassBeginBlockFail,
    kRtBuffersNull,
    kCount,
};

struct Record {
    std::atomic<uint64_t> seq{0};  // 0 = vacío; >0 = evento publicado
    // Payload atómico (ver nota TSan arriba): productores concurrentes y
    // el drenador pueden tocar el mismo slot a la vez; cada campo es un
    // valor completo, nunca tearing. `pad` no se lee ni se escribe jamás.
    std::atomic<uint64_t> monoNs{0};
    std::atomic<uint8_t> code{0};
    std::atomic<uint8_t> reason{0};
    uint16_t pad{0};
    std::atomic<float> a{0.f}, b{0.f}, c{0.f}, d{0.f};
};

// El anillo vive en el hilo RT: los atómicos DEBEN ser lock-free aquí
// (un atomic con lock interno en push() sería jitter no acotado).
static_assert(std::atomic<uint64_t>::is_always_lock_free, "rtlog: atomic<uint64_t> debe ser lock-free");
static_assert(std::atomic<uint8_t>::is_always_lock_free, "rtlog: atomic<uint8_t> debe ser lock-free");
static_assert(std::atomic<float>::is_always_lock_free, "rtlog: atomic<float> debe ser lock-free");

inline constexpr int kCapacity = 64;

inline Record g_slots[kCapacity]{};
inline std::atomic<uint32_t> g_nextSeq{1};

// Publica un evento desde el hilo RT. noexcept, sin alocación ni lock.
// El orden de escritura importa: payload primero (relaxed — la
// publicación es seq con release al final), seq con release al final.
inline void push(Event code, uint8_t reason, float a, float b, float c, float d) noexcept {
    const uint32_t seq = g_nextSeq.fetch_add(1, std::memory_order_relaxed);
    Record& slot = g_slots[(seq - 1u) % static_cast<uint32_t>(kCapacity)];
    struct timespec ts {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    slot.monoNs.store(static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
                      static_cast<uint64_t>(ts.tv_nsec), std::memory_order_relaxed);
    slot.code.store(static_cast<uint8_t>(code), std::memory_order_relaxed);
    slot.reason.store(reason, std::memory_order_relaxed);
    slot.a.store(a, std::memory_order_relaxed);
    slot.b.store(b, std::memory_order_relaxed);
    slot.c.store(c, std::memory_order_relaxed);
    slot.d.store(d, std::memory_order_relaxed);
    slot.seq.store(static_cast<uint64_t>(seq), std::memory_order_release);
}

// Número de eventos pendientes de drenar (solo diagnóstico, aproximado).
inline int pendingCount() noexcept {
    int n = 0;
    for (int i = 0; i < kCapacity; ++i) {
        if (g_slots[i].seq.load(std::memory_order_acquire) != 0) ++n;
    }
    return n;
}

// Drena todos los eventos pendientes llamando a fn por cada uno, en orden
// de slot (no garantizado cronológico tras wrap: el drenador es 1 Hz y el
// anillo es de 64, el wrap solo ocurre si >64 eventos en <1 s, es decir,
// solo con passthrough sostenido, donde el orden exacto no importa).
// SOLO desde hilo no-RT. fn no debe tocar el hilo de audio.
using DrainFn = void (*)(const Record&);
inline void drain(DrainFn fn) noexcept {
    if (!fn) return;
    for (int i = 0; i < kCapacity; ++i) {
        Record& slot = g_slots[i];
        const uint64_t seq = slot.seq.load(std::memory_order_acquire);
        if (seq == 0) continue;
        Record copy;
        // Payload con loads relaxed: el acquire ya se hizo al leer seq;
        // cada campo es un valor completo (ver nota TSan en el header).
        copy.monoNs.store(slot.monoNs.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.code.store(slot.code.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.reason.store(slot.reason.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.a.store(slot.a.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.b.store(slot.b.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.c.store(slot.c.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.d.store(slot.d.load(std::memory_order_relaxed), std::memory_order_relaxed);
        copy.seq.store(seq, std::memory_order_relaxed);
        // Limpia solo si nadie reescribió el slot mientras copiábamos:
        // si el CAS falla, el evento nuevo queda pendiente para el próximo drenaje.
        uint64_t expected = seq;
        if (slot.seq.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) {
            fn(copy);
        }
    }
}

}  // namespace ivanna::rtlog
