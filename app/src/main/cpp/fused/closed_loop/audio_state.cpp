// audio_state.cpp — M0: contrato temporal único (plan del salto, Luna).
//
// Seqlock de un solo escritor / varios lectores. Sin malloc, sin locks,
// sin I/O: seguro para publicar desde el hilo de control y leer desde el
// hilo de audio.
//
// Protocolo:
//   - seq_ par  = foto estable; seq_ impar = escritura en curso.
//   - publish(): seq_ -> impar, barrera release, copia del estado,
//     barrera release, seq_ -> par. La generación se auto-incrementa
//     cuando el llamador no la avanzó respecto a la última publicada:
//     si state.generation <= última, se publica última + 1; si el
//     llamador fijó una generación mayor, se respeta.
//   - read(): lee seq_ (s1); si es impar, reintenta; copia el estado;
//     lee seq_ (s2); si s1 != s2, la copia puede mezclar dos
//     generaciones y se descarta. Solo una foto con s1 == s2 par es
//     una generación coherente.
//
// Nota sobre la copia no atómica de state_ en read(): es el riesgo
// deliberado y acotado de todo seqlock — la copia puede leer una
// escritura a medias, pero la comprobación s1 == s2 la detecta y la
// descarta antes de devolverla. El consumidor nunca ve una mezcla.
// (El mismo patrón ya se usa en omega_control_bus.cpp / control_frame.hpp.)

#include "fused/closed_loop/audio_state.hpp"

namespace ivanna::closed_loop {

void AudioStatePublisher::publish(const AudioState& state) {
  AudioState next = state;
  // El escritor es único: leer state_.generation es estable aquí (ninguna
  // escritura en curso; los lectores solo leen).
  const uint64_t lastGeneration = state_.generation;
  if (next.generation <= lastGeneration) {
    next.generation = lastGeneration + 1;
  }

  // Abrir la ventana de escritura: seq_ impar.
  seq_.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  state_ = next;
  std::atomic_thread_fence(std::memory_order_release);
  // Cerrar la ventana: seq_ par, foto estable.
  seq_.fetch_add(1, std::memory_order_release);
}

AudioState AudioStatePublisher::read() const {
  for (;;) {
    const uint32_t seqBefore = seq_.load(std::memory_order_acquire);
    if ((seqBefore & 1u) != 0u) {
      continue;  // Escritura en curso: la foto no es estable.
    }
    const AudioState snapshot = state_;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t seqAfter = seq_.load(std::memory_order_acquire);
    if (seqBefore == seqAfter) {
      return snapshot;  // Foto estable: una sola generación.
    }
    // seq cambió durante la copia: descartar y reintentar.
  }
}

}  // namespace ivanna::closed_loop
