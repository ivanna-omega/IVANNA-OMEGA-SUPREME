// audio_state.hpp — M0: contrato temporal único (plan del salto, Luna).
//
// Cada bloque de audio lleva un AudioState. Ruta A y Ruta B consumen el
// MISMO estado: no se permite que una ruta use parámetros antiguos y la
// otra nuevos. Si generation, routeId o calibrationId no coinciden, el
// controlador entra en bypass seguro.
//
// Publicación por seqlock (sin locks en el hilo de audio): el lector
// reintenta mientras el seq sea impar o cambie entre las dos lecturas.
// Un AudioState con valid == false significa bypass seguro, no silencio:
// la cadena pasa el audio sin tocarlo.
//
// CORRECCIÓN (2026-11-22, Luna / v2.5.496): el seqlock anterior guardaba
// el estado en un AudioState plano (no atómico) y el lector lo copiaba
// con una lectura plana mientras el escritor lo escribía. Aunque el seq
// detectaba la mezcla y la descartaba, en C++ eso es data race y
// comportamiento indefinido: el compilador puede reordenar o romper la
// comprobación, y ThreadSanitizer lo marca. Por eso el test
// ConcurrentReaderNeverSeesTornState fallaba en la Puerta (flake).
// Ahora el payload vive en palabras atómicas (4 x uint64_t = 32 B):
// TODOS los accesos al estado compartido son atómicos, así que no hay
// data race; el seqlock solo detecta la mezcla de generaciones, que es
// su trabajo legítimo. Sin malloc, sin locks, sin I/O: igual de seguro
// para el hilo de audio.
//
// M0 es dueño de este header y de audio_state.cpp. Los demás módulos
// (M3, M5, M6, M8, telemetría) lo incluyen pero NO lo modifican.

#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <type_traits>

namespace ivanna::closed_loop {

// Estado temporal que acompaña a cada bloque de audio.
struct AudioState {
  uint64_t sampleTime = 0;     // muestras desde el inicio (48 kHz)
  uint64_t generation = 0;     // incrementa en cada republicación
  uint32_t routeId = 0;        // firma de ruta (M7); 0 = desconocida
  uint32_t calibrationId = 0;  // id de la calibración vigente; 0 = ninguna
  float confidence = 0.0f;     // confianza global 0..1
  bool valid = false;          // false = bypass seguro
};

// Publicador de AudioState con seqlock. Sin malloc, sin locks, sin I/O.
// Seguro para publicar desde un hilo y leer desde el hilo de audio.
class AudioStatePublisher {
 public:
  AudioStatePublisher() = default;
  AudioStatePublisher(const AudioStatePublisher&) = delete;
  AudioStatePublisher& operator=(const AudioStatePublisher&) = delete;

  // Publica un estado nuevo (incrementa generation si el llamador no lo
  // hizo). El escritor es único; los lectores pueden ser varios.
  void publish(const AudioState& state);

  // Lectura consistente: si el seqlock detecta una escritura en curso,
  // reintenta hasta obtener una foto estable.
  AudioState read() const;

 private:
  // El estado se guarda como palabras atómicas (ver nota arriba).
  // sizeof(AudioState) == 32 -> 4 palabras de 64 bits.
  static constexpr size_t kWordCount = sizeof(AudioState) / sizeof(uint64_t);
  static_assert(sizeof(AudioState) % sizeof(uint64_t) == 0,
                "AudioState debe ser multiplo de 8 bytes para el seqlock atomico");
  static_assert(std::is_trivially_copyable_v<AudioState>,
                "AudioState debe ser trivially copyable para memcpy entre palabras");
  // Palabra 1 (indice 1) es generation (sampleTime ocupa la palabra 0).
  static constexpr size_t kGenerationWord = 1;

  static void ToWords(const AudioState& state, uint64_t* words) {
    const auto packed = std::bit_cast<std::array<uint64_t, kWordCount>>(state);
    for (size_t i = 0; i < kWordCount; ++i) words[i] = packed[i];
  }
  static AudioState FromWords(const uint64_t* words) {
    std::array<uint64_t, kWordCount> packed{};
    for (size_t i = 0; i < kWordCount; ++i) packed[i] = words[i];
    return std::bit_cast<AudioState>(packed);
  }

  // seq par = estable; impar = escritura en curso.
  mutable std::atomic<uint32_t> seq_{0};
  // Payload: palabras atómicas (value-initialized a cero = estado inválido).
  std::atomic<uint64_t> words_[kWordCount]{};
};

}  // namespace ivanna::closed_loop
