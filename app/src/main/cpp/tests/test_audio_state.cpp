// test_audio_state.cpp — M0: contrato temporal único (plan del salto, Luna).
//
// Verifica el seqlock de AudioStatePublisher por propiedades, no por
// números inventados: round-trip, generación monótona, coherencia bajo
// concurrencia, bypass con valid=false y detección de cambio de ruta.
//
// La coherencia se verifica haciendo que cada campo publicado sea función
// determinista de la generación: un lector que mezclara dos generaciones
// vería un campo que no corresponde a la generación leída.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "fused/closed_loop/audio_state.hpp"
#include "gtest/gtest.h"

namespace ivanna::closed_loop {
namespace {

AudioState MakeState(uint64_t sampleTime, uint64_t generation, uint32_t routeId,
                     uint32_t calibrationId, float confidence, bool valid) {
  AudioState s;
  s.sampleTime = sampleTime;
  s.generation = generation;
  s.routeId = routeId;
  s.calibrationId = calibrationId;
  s.confidence = confidence;
  s.valid = valid;
  return s;
}

// Estado coherente con su generación: cada campo es función determinista
// de la generación publicada.
AudioState StateForGeneration(uint64_t generation) {
  return MakeState(generation * 128u,  // un bloque de 128 muestras
                   generation,         // generación explícita
                   1000u + static_cast<uint32_t>(generation),
                   2000u + static_cast<uint32_t>(generation),
                   0.5f, true);
}

// true si todos los campos del estado corresponden a su generación.
bool MatchesGeneration(const AudioState& s) {
  return s.sampleTime == s.generation * 128u &&
         s.routeId == 1000u + static_cast<uint32_t>(s.generation) &&
         s.calibrationId == 2000u + static_cast<uint32_t>(s.generation) &&
         s.confidence == 0.5f && s.valid;
}

TEST(AudioState, RoundTripPreservesAllFields) {
  AudioStatePublisher pub;
  const AudioState in = MakeState(123456, 7, 42, 99, 0.7f, true);
  pub.publish(in);
  const AudioState out = pub.read();
  EXPECT_EQ(out.sampleTime, in.sampleTime);
  EXPECT_EQ(out.generation, in.generation);
  EXPECT_EQ(out.routeId, in.routeId);
  EXPECT_EQ(out.calibrationId, in.calibrationId);
  EXPECT_FLOAT_EQ(out.confidence, in.confidence);
  EXPECT_TRUE(out.valid);
}

TEST(AudioState, InitialStateIsInvalidBypass) {
  AudioStatePublisher pub;
  const AudioState out = pub.read();
  EXPECT_FALSE(out.valid);
  EXPECT_EQ(out.generation, 0u);
  EXPECT_EQ(out.sampleTime, 0u);
  EXPECT_EQ(out.routeId, 0u);
  EXPECT_EQ(out.calibrationId, 0u);
  EXPECT_FLOAT_EQ(out.confidence, 0.0f);
}

TEST(AudioState, GenerationIncrementsOnEachPublish) {
  AudioStatePublisher pub;
  const AudioState base = MakeState(0, 0, 1, 1, 0.5f, true);
  pub.publish(base);
  EXPECT_EQ(pub.read().generation, 1u);
  pub.publish(base);  // generation 0 otra vez: el llamador no avanzó
  EXPECT_EQ(pub.read().generation, 2u);
  pub.publish(base);
  EXPECT_EQ(pub.read().generation, 3u);

  // El llamador sí avanzó la generación: se respeta.
  pub.publish(MakeState(0, 10, 1, 1, 0.5f, true));
  EXPECT_EQ(pub.read().generation, 10u);
  // Y el auto-incremento continúa desde ahí.
  pub.publish(base);
  EXPECT_EQ(pub.read().generation, 11u);
  // Una generación explícita menor que la última no retrocede.
  pub.publish(MakeState(0, 5, 1, 1, 0.5f, true));
  EXPECT_EQ(pub.read().generation, 12u);
}

TEST(AudioState, InvalidStateReadsAsInvalid) {
  AudioStatePublisher pub;
  pub.publish(MakeState(1000, 0, 1, 1, 0.9f, true));
  EXPECT_TRUE(pub.read().valid);
  // El publicador marca el estado inválido: el consumidor debe hacer
  // bypass (pasar el audio sin tocarlo), no silenciarlo.
  pub.publish(MakeState(2000, 0, 1, 1, 0.0f, false));
  const AudioState out = pub.read();
  EXPECT_FALSE(out.valid);
  EXPECT_EQ(out.sampleTime, 2000u);
  EXPECT_FLOAT_EQ(out.confidence, 0.0f);
}

TEST(AudioState, RouteAndCalibrationChangeIsDetectable) {
  AudioStatePublisher pub;
  pub.publish(MakeState(0, 0, 0xA11CE, 0xCA11B, 0.8f, true));
  const AudioState before = pub.read();
  pub.publish(MakeState(128, 0, 0xB00B5, 0xDECA1, 0.8f, true));
  const AudioState after = pub.read();
  EXPECT_NE(after.routeId, before.routeId);
  EXPECT_NE(after.calibrationId, before.calibrationId);
  EXPECT_EQ(after.routeId, 0xB00B5u);
  EXPECT_EQ(after.calibrationId, 0xDECA1u);
  // La generación avanzó: el consumidor que guarda la generación anterior
  // detecta que el estado cambió.
  EXPECT_GT(after.generation, before.generation);
}

TEST(AudioState, ConcurrentReaderNeverSeesTornState) {
  AudioStatePublisher pub;
  constexpr int kPublishes = 20000;
  std::atomic<bool> stop{false};
  std::atomic<uint64_t> maxGenerationSeen{0};
  std::atomic<int> tornReads{0};

  // Los chequeos se acumulan en atómicos: gtest no es seguro para
  // EXPECT_* desde hilos que no son el principal.
  auto readerLoop = [&] {
    uint64_t lastGen = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const AudioState s = pub.read();
      // La generación nunca retrocede: solo fotos estables y monótonas.
      if (s.generation < lastGen) {
        tornReads.fetch_add(1, std::memory_order_relaxed);
      }
      lastGen = s.generation;
      // Generación 0 = estado inicial (inválido); a partir de la 1 cada
      // campo debe corresponder a la generación leída.
      if (s.generation > 0 && !MatchesGeneration(s)) {
        tornReads.fetch_add(1, std::memory_order_relaxed);
      }
      const uint64_t prev = maxGenerationSeen.load(std::memory_order_relaxed);
      if (s.generation > prev) {
        maxGenerationSeen.store(s.generation, std::memory_order_relaxed);
      }
    }
  };

  std::thread readerA(readerLoop);
  std::thread readerB(readerLoop);

  // Escritor: publica generaciones 1..kPublishes con auto-incremento
  // (generation 0 en cada llamada) y campos función de la generación.
  for (int i = 1; i <= kPublishes; ++i) {
    AudioState s = StateForGeneration(static_cast<uint64_t>(i));
    s.generation = 0;  // el publicador asigna gen = última + 1 == i
    pub.publish(s);
  }
  // Dar a los lectores la oportunidad de observar la generación final
  // antes de parar: sin esto, el escritor puede poner stop=true antes de
  // que ningún lector lea la última generación (maxGenerationSeen quedaría
  // en 19999, o en 0 si los lectores nunca fueron planificados en un
  // runner de 2 núcleos) y el test fallaría por una condición de carrera
  // del propio test, no del seqlock. Se espera con yield para no
  // acaparar el núcleo; el plazo es solo red de seguridad (en la
  // práctica los lectores la observan en milisegundos, porque el estado
  // final es estable).
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (maxGenerationSeen.load(std::memory_order_relaxed) <
             static_cast<uint64_t>(kPublishes) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  stop.store(true, std::memory_order_release);
  readerA.join();
  readerB.join();

  EXPECT_EQ(tornReads.load(), 0);
  EXPECT_EQ(maxGenerationSeen.load(), static_cast<uint64_t>(kPublishes));
  EXPECT_EQ(pub.read().generation, static_cast<uint64_t>(kPublishes));
  EXPECT_TRUE(MatchesGeneration(pub.read()));
}

}  // namespace
}  // namespace ivanna::closed_loop
