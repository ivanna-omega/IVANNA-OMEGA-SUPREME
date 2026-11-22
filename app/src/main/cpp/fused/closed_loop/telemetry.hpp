// telemetry.hpp — Telemetría del control cerrado (plan del salto, Luna,
// sección 3 punto 14).
//
// La telemetría SOLO registra, nunca decide nada. A 10 Hz el control
// publica un TelemetryFrame (confianza, acción elegida, acción aplicada,
// energía antes/después, true peak, THD estimado, latencia, xruns,
// divergencia gemelo/medición); los eventos de fallo se registran de
// inmediato con recordFailure(), sin esperar al drenado de 10 Hz.
//
// Diseño (hilo de audio: sin malloc, sin locks, sin I/O):
//   - Dos anillos lock-free independientes (frames y fallos), de capacidad
//     potencia de dos, con almacenamiento estático (std::array). El
//     productor (hilo de audio / control) solo hace fetch_add + CAS sobre
//     atómicos; el consumidor (que vuelca a log/archivo) corre fuera del
//     hilo de audio y drena con drain()/drainFailures().
//   - Cada slot lleva un número de secuencia: 0 = nunca escrito,
//     kWriting = un productor está escribiendo, pos+1 = frame publicado.
//     El consumidor copia el payload y re-verifica la secuencia: si cambió
//     durante la copia, el frame se cuenta como perdido (dropped()) y
//     NUNCA se entrega un frame roto/mezclado.
//   - Un productor reclama su slot con CAS (secuencia previa -> kWriting):
//     dos productores que caen en el mismo slot se serializan; si el slot
//     ya lo publicó un ciclo posterior, el frame se pierde (dropped()) en
//     vez de corromper nada. El reclamo con espera está acotado (4096
//     vueltas): el hilo de audio nunca se bloquea indefinidamente.
//   - POLÍTICA DE BUFFER LLENO (documentada): al llenarse, el anillo
//     SOBRESCRIBE EL MÁS VIEJO y conserva el más reciente. El frame más
//     reciente es el más valioso para diagnosticar el estado actual; un
//     consumidor lento pierde historia, nunca el presente. Los frames
//     perdidos se cuentan en dropped()/droppedFailures().
//   - Un frame con algún campo no finito (NaN/Inf) NO se descarta en
//     silencio: se registra con valid=false, para que el fallo quede
//     visible en la telemetría en vez de desaparecer.
//   - Consumidor único: drain()/drainFailures() deben llamarse desde un
//     solo hilo (el que vuelca). Los productores pueden ser varios.
//
// Contratos compartidos (audio_state.hpp, twin_confidence.hpp): se leen,
// no se modifican. Los 7 operadores de chosenAction/appliedAction son los
// de perceptual_mpc (kNumOperators = 7): ORBE, HOLOS, SALA VIVA, Volterra,
// ARMONÍA, PULSO, NEURO·Ω.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace ivanna::closed_loop {

// Un frame de telemetría a 10 Hz (Luna, sección 3 punto 14).
struct TelemetryFrame {
  uint64_t sampleTime = 0;      // muestras desde el inicio (48 kHz)
  uint64_t generation = 0;      // generation del AudioState vigente
  float confidence = 0.0f;      // confianza global del gemelo 0..1
  float chosenAction[7] = {};   // acción elegida por el control (7 ops)
  float appliedAction[7] = {};  // acción realmente aplicada (tras límites)
  float energyBeforeDb = 0.0f;  // energía antes del control (dB)
  float energyAfterDb = 0.0f;   // energía después del control (dB)
  float truePeakDbTp = 0.0f;    // true peak (dBTP)
  float thdEstimateDb = 0.0f;   // THD estimado (dB)
  float latencyMs = 0.0f;       // latencia total (ms)
  uint32_t xruns = 0;           // xruns acumulados
  float twinDivergence = 0.0f;  // divergencia gemelo/medición
  bool valid = false;           // false = frame inválido (p. ej. no finito)
};

// Un evento de fallo: disponible de inmediato, sin esperar al drenado.
struct FailureRecord {
  char reason[64] = {};    // motivo, truncado a 63 chars + NUL
  uint64_t ordinal = 0;    // contador monótono de fallos
};

namespace detail {

// Anillo lock-free MPMC con sobrescritura del más viejo (ver cabecera).
// N debe ser potencia de dos. T debe ser copiable por asignación.
template <typename T, std::size_t N>
class LockFreeRing {
  static_assert(N > 0 && (N & (N - 1)) == 0, "capacity must be a power of two");

 public:
  LockFreeRing() = default;
  LockFreeRing(const LockFreeRing&) = delete;
  LockFreeRing& operator=(const LockFreeRing&) = delete;

  // Publica un valor. Nunca bloquea indefinidamente, no hace malloc,
  // no toma locks ni hace I/O. Si el slot ya lo publicó un ciclo
  // posterior, el valor se pierde y se cuenta en dropped().
  void push(const T& value) {
    const uint64_t pos = writePos_.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = slots_[pos & kMask];
    // Secuencia que el ciclo anterior dejó en este slot (0 si nunca se
    // escribió). Un valor menor es un ciclo anterior cuyo productor lo
    // perdió sin publicar; un valor mayor es un ciclo posterior que ya
    // publicó: en ese caso estamos tarde y el frame se pierde.
    const uint64_t prev = (pos >= N) ? (pos - N + 1) : 0;
    uint64_t seq = slot.seq.load(std::memory_order_acquire);
    // Espera acotada a que otro productor termine de escribir este slot.
    // El escritor solo hace una copia de ~100 bytes, así que 4096 vueltas
    // es holgado; si expira, se pierde el frame antes que bloquear audio.
    int spins = 0;
    while (seq == kWriting) {
      if (++spins > 4096) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      seq = slot.seq.load(std::memory_order_acquire);
    }
    if (seq > prev) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    // Reclamar el slot: secuencia -> kWriting. Si otro productor nos
    // ganó la carrera, perder el frame antes que corromperlo.
    uint64_t expected = seq;
    if (!slot.seq.compare_exchange_strong(expected, kWriting,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    slot.payload = value;
    slot.seq.store(pos + 1, std::memory_order_release);
  }

  // Frames pendientes de drenar (instantánea; acotado a la capacidad).
  std::size_t pending() const {
    const uint64_t w = writePos_.load(std::memory_order_relaxed);
    const uint64_t r = readPos_.load(std::memory_order_relaxed);
    const uint64_t n = (w > r) ? (w - r) : 0;
    return (n > N) ? N : static_cast<std::size_t>(n);
  }

  std::size_t dropped() const {
    return static_cast<std::size_t>(dropped_.load(std::memory_order_relaxed));
  }

  // Drena hasta max valores en orden FIFO. Devuelve cuántos entregó (0 si
  // vacío o si el siguiente frame aún lo está escribiendo un productor:
  // se reintenta en el próximo drenado). Nunca entrega un frame roto: si
  // el slot se sobrescribió durante la copia, ese frame se cuenta como
  // perdido y se sigue con el siguiente. Solo un hilo consumidor.
  std::size_t drain(T* out, std::size_t max) {
    if (out == nullptr || max == 0) return 0;
    std::size_t count = 0;
    while (count < max) {
      uint64_t r = readPos_.load(std::memory_order_relaxed);
      const uint64_t w = writePos_.load(std::memory_order_acquire);
      if (r >= w) break;  // vacío
      if (w - r > N) {
        // El productor nos dio la vuelta: los más viejos ya no existen.
        const uint64_t jump = w - N;
        dropped_.fetch_add(jump - r, std::memory_order_relaxed);
        readPos_.store(jump, std::memory_order_relaxed);
        r = jump;
      }
      Slot& slot = slots_[r & kMask];
      const uint64_t seq = slot.seq.load(std::memory_order_acquire);
      if (seq != r + 1) {
        if (seq == kWriting || seq < r + 1) {
          break;  // productor a medio escribir; reintentar luego
        }
        // seq > r+1: el slot ya lleva un ciclo posterior; este frame se
        // perdió por sobrescritura.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        readPos_.store(r + 1, std::memory_order_relaxed);
        continue;
      }
      T tmp = slot.payload;  // copia
      if (slot.seq.load(std::memory_order_acquire) != r + 1) {
        // Sobrescrito durante la copia: descartar, nunca entregar roto.
        dropped_.fetch_add(1, std::memory_order_relaxed);
        readPos_.store(r + 1, std::memory_order_relaxed);
        continue;
      }
      out[count++] = tmp;
      readPos_.store(r + 1, std::memory_order_relaxed);
    }
    return count;
  }

 private:
  static constexpr uint64_t kWriting = UINT64_MAX;
  static constexpr std::size_t kMask = N - 1;

  struct Slot {
    std::atomic<uint64_t> seq{0};
    T payload{};
  };

  std::array<Slot, N> slots_{};
  alignas(64) std::atomic<uint64_t> writePos_{0};
  alignas(64) std::atomic<uint64_t> readPos_{0};
  alignas(64) std::atomic<uint64_t> dropped_{0};
};

}  // namespace detail

// Telemetría del control cerrado: SOLO registra, nunca decide nada.
class Telemetry {
 public:
  // 256 frames a 10 Hz = 25.6 s de historia para el consumidor.
  static constexpr std::size_t kCapacity = 256;
  // 32 fallos encolados bastan: el consumidor los vuelca de inmediato.
  static constexpr std::size_t kFailureCapacity = 32;

  Telemetry() = default;
  Telemetry(const Telemetry&) = delete;
  Telemetry& operator=(const Telemetry&) = delete;

  // Registra un frame (10 Hz). Camino de audio: sin malloc, sin locks,
  // sin I/O. Un frame no finito se registra con valid=false.
  void record(const TelemetryFrame& frame);

  // Registra un evento de fallo de inmediato (no espera al drenado de
  // 10 Hz). reason == nullptr se registra como "unknown".
  void recordFailure(const char* reason);

  std::size_t pending() const { return frames_.pending(); }
  std::size_t pendingFailures() const { return failures_.pending(); }
  // Frames/fallos perdidos por sobrescritura (consumidor lento).
  std::size_t dropped() const { return frames_.dropped(); }
  std::size_t droppedFailures() const { return failures_.dropped(); }

  // Drena hasta max frames pendientes en orden FIFO. Devuelve cuántos
  // entregó. Solo un hilo consumidor.
  std::size_t drain(TelemetryFrame* out, std::size_t max) {
    return frames_.drain(out, max);
  }
  // Drena los fallos pendientes en orden FIFO. Independiente de drain():
  // un fallo no espera a que se drenen los frames.
  std::size_t drainFailures(FailureRecord* out, std::size_t max) {
    return failures_.drain(out, max);
  }

 private:
  detail::LockFreeRing<TelemetryFrame, kCapacity> frames_{};
  detail::LockFreeRing<FailureRecord, kFailureCapacity> failures_{};
  std::atomic<uint64_t> failureOrdinal_{0};
};

}  // namespace ivanna::closed_loop
