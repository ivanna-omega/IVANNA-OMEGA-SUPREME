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
// M0 es dueño de este header y de audio_state.cpp. Los demás módulos
// (M3, M5, M6, M8, telemetría) lo incluyen pero NO lo modifican.

#pragma once

#include <atomic>
#include <cstdint>

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
  // seq par = estable; impar = escritura en curso.
  mutable std::atomic<uint32_t> seq_{0};
  AudioState state_{};
};

}  // namespace ivanna::closed_loop
