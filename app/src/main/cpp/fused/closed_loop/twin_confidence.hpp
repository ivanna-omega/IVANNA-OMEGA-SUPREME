// twin_confidence.hpp — Confianza por variable del gemelo (M3/M8).
//
// El gemelo no entrega solo una estimación: entrega cada parámetro con su
// incertidumbre (Luna, plan del salto, M3 y M8). Sin esto, un controlador
// que confía en un gemelo con 30% de error puede ser peor que un sistema
// más simple.
//
// Regla dura: el gemelo con confianza estimada 0.70 SOLO puede gobernar
// la intensidad máxima de conservación. NUNCA decide EQ fino, fase,
// Volterra ni compensación de sala. Eso exige medición real (Órdenes 1-5).
//
// Bandas de confianza (NOMINALES, no medidas — se calibran con ABX,
// Orden 6). El 0.70 actual cae en "media": solo limita intensidad, lo
// que es exactamente la regla del plan.
//   >= kConfidenceHigh (0.80): puede controlar.
//   >= kConfidenceMedium (0.50): solo limita intensidad.
//   <  kConfidenceMedium: solo telemetría.
//   <  kConfidenceZeroAction (0.20): acción cero (M6).
//
// M3 es dueño de llenar estas estructuras desde el gemelo (acoustic_twin).
// M5, M6 y M8 las leen. Nadie más las modifica.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ivanna::closed_loop {

// Bandas del gemelo: 9 bandas, 63 Hz – 16 kHz (igual que acoustic_twin).
inline constexpr std::size_t kTwinNumBands = 9;

// Umbrales de confianza (NOMINALES, no medidos).
inline constexpr float kConfidenceHigh = 0.80f;       // puede controlar
inline constexpr float kConfidenceMedium = 0.50f;     // solo limita intensidad
inline constexpr float kConfidenceZeroAction = 0.20f; // acción cero

// Método por el que se obtuvo un parámetro.
enum class ConfidenceMethod : uint8_t {
  kUnknown = 0,   // sin dato
  kDatasheet = 1, // derivado del datasheet (estimado, no medido)
  kDerived = 2,   // derivado por modelo físico (estimado)
  kMeasured = 3,  // medido (barrido / IR / loopback)
};

// Un parámetro del gemelo con su incertidumbre (Luna M8).
struct ParameterConfidence {
  float estimate = 0.0f;  // valor estimado
  float variance = 0.0f;  // varianza (>= 0)
  float age = 0.0f;       // segundos desde la medición/estimación
  ConfidenceMethod method = ConfidenceMethod::kUnknown;
  float confidence = 0.0f;  // 0..1
  bool valid = false;       // false = sin dato usable
};

// Confianza por variable del gemelo (Luna M3).
struct TwinConfidence {
  // H1: respuesta lineal por banda (9 bandas 63 Hz–16 kHz).
  std::array<ParameterConfidence, kTwinNumBands> h1{};
  ParameterConfidence h2{};            // no linealidad 2º orden
  ParameterConfidence h3{};            // no linealidad 3er orden
  ParameterConfidence latency{};       // latencia total D
  ParameterConfidence roomIr{};        // IR de sala R
  ParameterConfidence coherence{};     // coherencia L/R C
  ParameterConfidence noiseFloor{};    // piso de ruido N(f)
  ParameterConfidence powerHeadroom{}; // margen de potencia Pmax(f)
  // Confianza global 0..1 (el mínimo ponderado de las variables que el
  // control necesita; la define M3). 0.70 = calibración estimada actual.
  float global = 0.0f;
  bool valid = false;
};

// Nivel de intervención que la confianza permite (Luna M8).
enum class InterventionLevel : uint8_t {
  kNone = 0,      // solo telemetría (confianza baja)
  kLimitOnly = 1, // solo limita intensidad (confianza media)
  kControl = 2,   // puede controlar (confianza alta)
};

}  // namespace ivanna::closed_loop
