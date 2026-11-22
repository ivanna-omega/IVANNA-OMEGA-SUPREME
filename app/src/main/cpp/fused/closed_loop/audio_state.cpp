// audio_state.cpp — M0: contrato temporal único (plan del salto, Luna).
//
// Seqlock de un solo escritor / varios lectores. Sin malloc, sin locks,
// sin I/O: seguro para publicar desde el hilo de control y leer desde el
// hilo de audio.
//
// Protocolo:
//   - seq_ par  = foto estable; seq_ impar = escritura en curso.
//   - publish(): seq_ -> impar, barrera release, escritura de las
//     palabras atómicas, barrera release, seq_ -> par. La generación se
//     auto-incrementa cuando el llamador no la avanzó respecto a la
//     última publicada: si state.generation <= última, se publica
//     última + 1; si el llamador fijó una generación mayor, se respeta.
//   - read(): lee seq_ (s1); si es impar, reintenta; copia las palabras
//     atómicas; lee seq_ (s2); si s1 != s2, la copia puede mezclar dos
//     generaciones y se descarta. Solo una foto con s1 == s2 par es
//     una generación coherente.
//
// CORRECCIÓN (2026-11-22, Luna / v2.5.496): antes el payload era un
// AudioState plano y la copia del lector era una lectura plana
// concurrente con la escritura del publicador: data race y
// comportamiento indefinido en C++, aunque el seq lo detectara.
// Ahora cada palabra del payload es std::atomic<uint64_t>: no hay
// ningún acceso no atómico al estado compartido, así que no hay data
// race y ThreadSanitizer queda limpio. El seqlock conserva su único
// trabajo legítimo: descartar la mezcla de dos generaciones.

#include "fused/closed_loop/audio_state.hpp"

namespace ivanna::closed_loop {

void AudioStatePublisher::publish(const AudioState& state) {
  AudioState next = state;
  // El escritor es único: leer la generación publicada es estable aquí
  // (ninguna escritura en curso; los lectores solo leen).
  const uint64_t lastGeneration =
      words_[kGenerationWord].load(std::memory_order_acquire);
  if (next.generation <= lastGeneration) {
    next.generation = lastGeneration + 1;
  }

  uint64_t words[kWordCount];
  ToWords(next, words);

  // Abrir la ventana de escritura: seq_ impar.
  seq_.fetch_add(1, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);
  for (size_t i = 0; i < kWordCount; ++i) {
    words_[i].store(words[i], std::memory_order_release);
  }
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
    uint64_t words[kWordCount];
    for (size_t i = 0; i < kWordCount; ++i) {
      words[i] = words_[i].load(std::memory_order_acquire);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t seqAfter = seq_.load(std::memory_order_acquire);
    if (seqBefore == seqAfter) {
      return FromWords(words);  // Foto estable: una sola generación.
    }
    // seq cambió durante la copia: descartar y reintentar.
  }
}

}  // namespace ivanna::closed_loop
