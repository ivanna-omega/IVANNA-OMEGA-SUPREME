// uncertainty_supervisor.hpp — M8: supervisor de incertidumbre + rollback
// (plan del salto, Luna).
//
// El supervisor es el ÁRBITRO ÚNICO de intervención: decide qué nivel de
// intervención permite la confianza del gemelo. Ningún otro módulo (M5, M6,
// conservación) decide por sí mismo si puede controlar: todos consultan
// aquí primero.
//
// Reglas (Luna M8; umbrales NOMINALES del contrato twin_confidence.hpp,
// no medidos — se calibran con ABX, Orden 6):
//   confianza global >= 0.80 -> kControl    (puede controlar)
//   confianza global >= 0.50 -> kLimitOnly  (solo limita intensidad,
//                                            maxIntensity = la confianza)
//   confianza global <  0.50 -> kNone       (solo telemetría)
//   confianza global <  0.20 -> acción cero (M6: kNone, maxIntensity 0)
//   dato viejo (age > kMaxAgeSeconds) o ruta distinta a la de la
//     calibración -> inválido (kNone)
//   discordancia gemelo/medición alta -> baja UN nivel de intervención
//     y pide recalibración (reason); discordancia extrema -> acción cero
//   cualquier input no finito -> kNone fail-closed
//
// Rollback (Luna M6/M8): si el modelo predice mejora pero la medición lo
// contradice dos bloques seguidos, hay que volver atrás. El llamador marca
// cada bloque con recordAnomaly() / recordGoodBlock(); tras 2 anomalías
// seguidas sin un bloque bueno, rollbackNeeded() == true. resetRollback()
// limpia el estado tras aplicar el rollback.
//
// Hilo de audio: supervise() es const y pura (sin malloc, sin locks, sin
// I/O, sin estado mutable): segura para llamar desde el callback. El
// contador de anomalías vive fuera del camino caliente (lo tocan
// recordAnomaly/recordGoodBlock/resetRollback desde el hilo de control).

#pragma once

#include <cstdint>

#include "audio_state.hpp"
#include "twin_confidence.hpp"

namespace ivanna::closed_loop {

// Edad máxima de un dato del gemelo, en segundos (NOMINAL, no medido).
// Justificación: 300 s (5 min) es el punto en el que una estimación de
// sala/posición/transductor sin ningún refresco deja de poder gobernar el
// control — el oyente puede haberse movido, la sala cambiado, el DAC
// cambiado. El plan prevé reidentificación al iniciar, al cambiar de
// ruta y cuando la confianza cae, más microsondas inaudibles (§1): un
// gemelo vivo se mantiene fresco y nunca llega a este tope. Un gemelo
// estimado que jamás se refresca (el 0.70 actual) envejecerá y el
// supervisor lo degradará a kNone: ese es el comportamiento fail-closed
// buscado hasta que el barrido Genezi lo sustituya por medición real.
// Se recalibra con datos de las Órdenes 0–3.
inline constexpr float kMaxAgeSeconds = 300.0f;

// Discordancia gemelo/medición (0..1, p. ej. error relativo de predicción)
// por encima de la cual se baja un nivel de intervención (NOMINAL).
inline constexpr float kMaxTwinMeasurementDivergence = 0.25f;

// Discordancia a partir de la cual el gemelo contradice de plano a la
// medición: acción cero (M6), aunque la confianza sea alta (NOMINAL).
inline constexpr float kDivergenceZeroAction = 0.75f;

// Anomalías consecutivas que disparan el rollback (Luna M6/M8: dos
// bloques anómalos seguidos).
inline constexpr int kRollbackAnomalyThreshold = 2;

// Veredicto del supervisor para un bloque.
struct Supervision {
  InterventionLevel level = InterventionLevel::kNone;
  bool allowControl = false;  // true solo en kControl
  float maxIntensity = 0.0f;  // 1.0 en kControl, confianza en kLimitOnly,
                              // 0.0 en kNone / acción cero
  const char* reason = "invalido";  // literal estático, nunca nullptr
};

class UncertaintySupervisor {
 public:
  UncertaintySupervisor() = default;

  // Arbitra un bloque. twinMeasurementDivergence es 0..1 (0 = el gemelo
  // predice exactamente lo medido). La sobrecarga sin calibrationRouteId
  // solo puede invalidar por ruta desconocida (routeId == 0); para el
  // chequeo de ruta distinta hay que pasar el routeId de la calibración
  // vigente (o el que el llamador conozca) en la sobrecarga de 4 args.
  Supervision supervise(const TwinConfidence& twin, const AudioState& state,
                        float twinMeasurementDivergence) const;
  Supervision supervise(const TwinConfidence& twin, const AudioState& state,
                        float twinMeasurementDivergence,
                        uint32_t calibrationRouteId) const;

  // Marca un bloque anómalo (la medición contradijo al modelo) o bueno.
  // Un bloque bueno corta la racha de anomalías.
  void recordAnomaly();
  void recordGoodBlock();

  // true tras kRollbackAnomalyThreshold anomalías seguidas sin bloque
  // bueno. Sigue true hasta resetRollback().
  bool rollbackNeeded() const;
  int consecutiveAnomalies() const;

  // Limpia el estado de rollback (llamar tras aplicar el rollback).
  void resetRollback();

 private:
  int consecutiveAnomalies_ = 0;
};

}  // namespace ivanna::closed_loop
