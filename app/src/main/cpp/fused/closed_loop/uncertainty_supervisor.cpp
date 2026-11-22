// uncertainty_supervisor.cpp — M8: supervisor de incertidumbre + rollback
// (plan del salto, Luna). Ver uncertainty_supervisor.hpp para el contrato.
//
// Orden de chequeo (fail-closed, de lo más grave a lo menos grave):
//   1. inputs no finitos o fuera de rango -> kNone
//   2. gemelo o estado inválidos -> kNone
//   3. ruta desconocida o distinta a la calibración -> kNone
//   4. dato viejo (cualquier parámetro válido con age > kMaxAgeSeconds)
//      -> kNone
//   5. nivel base por confianza global (0.80 / 0.50 / 0.20)
//   6. discordancia gemelo/medición: baja un nivel (+ pide recalibración),
//      o acción cero si es extrema.

#include "uncertainty_supervisor.hpp"

#include <cmath>

namespace ivanna::closed_loop {
namespace {

// Literales estáticos: Supervision::reason nunca cuelga de un temporal.
constexpr const char* kReasonControl = "confianza alta: control permitido";
constexpr const char* kReasonLimitOnly =
    "confianza media: solo limita intensidad";
constexpr const char* kReasonTelemetryOnly = "confianza baja: solo telemetria";
constexpr const char* kReasonZeroAction =
    "accion cero: confianza bajo 0.20";
constexpr const char* kReasonNonFinite = "entrada no finita: fail-closed";
constexpr const char* kReasonInvalidParam =
    "parametro del gemelo invalido";
constexpr const char* kReasonTwinInvalid = "gemelo invalido";
constexpr const char* kReasonStateInvalid = "estado de audio invalido";
constexpr const char* kReasonRouteUnknown = "ruta desconocida";
constexpr const char* kReasonRouteMismatch =
    "ruta distinta a la calibracion: invalido";
constexpr const char* kReasonStale = "dato viejo: calibracion expirada";
constexpr const char* kReasonDivergence =
    "discordancia gemelo/medicion alta: baja un nivel, pedir recalibracion";
constexpr const char* kReasonDivergenceZero =
    "discordancia gemelo/medicion extrema: accion cero, pedir "
    "recalibracion";

// Un parámetro válido debe ser finito, con varianza >= 0 y confianza en
// [0, 1]. Un parámetro inválido (valid == false) no se chequea: es "sin
// dato", y eso ya lo cubre twin.valid / la confianza global.
bool parameterSane(const ParameterConfidence& p) {
  if (!p.valid) return true;
  if (!std::isfinite(p.estimate) || !std::isfinite(p.variance) ||
      !std::isfinite(p.age) || !std::isfinite(p.confidence)) {
    return false;
  }
  if (p.variance < 0.0f) return false;
  if (p.confidence < 0.0f || p.confidence > 1.0f) return false;
  return true;
}

// true si algún parámetro válido es más viejo que kMaxAgeSeconds.
bool anyParameterStale(const TwinConfidence& twin) {
  const auto stale = [](const ParameterConfidence& p) {
    return p.valid && p.age > kMaxAgeSeconds;
  };
  for (const auto& band : twin.h1) {
    if (stale(band)) return true;
  }
  return stale(twin.h2) || stale(twin.h3) || stale(twin.latency) ||
         stale(twin.roomIr) || stale(twin.coherence) ||
         stale(twin.noiseFloor) || stale(twin.powerHeadroom);
}

bool allParametersSane(const TwinConfidence& twin) {
  for (const auto& band : twin.h1) {
    if (!parameterSane(band)) return false;
  }
  return parameterSane(twin.h2) && parameterSane(twin.h3) &&
         parameterSane(twin.latency) && parameterSane(twin.roomIr) &&
         parameterSane(twin.coherence) && parameterSane(twin.noiseFloor) &&
         parameterSane(twin.powerHeadroom);
}

// Baja un nivel de intervención (kControl -> kLimitOnly -> kNone).
// Devuelve el nivel resultante.
InterventionLevel demoteOne(InterventionLevel level) {
  switch (level) {
    case InterventionLevel::kControl:
      return InterventionLevel::kLimitOnly;
    case InterventionLevel::kLimitOnly:
      return InterventionLevel::kNone;
    case InterventionLevel::kNone:
    default:
      return InterventionLevel::kNone;
  }
}

}  // namespace

Supervision UncertaintySupervisor::supervise(
    const TwinConfidence& twin, const AudioState& state,
    float twinMeasurementDivergence) const {
  // Sin routeId de calibración conocido no se puede checar el mismatch:
  // se delega con la ruta del propio estado (sin mismatch posible) y el
  // chequeo de ruta desconocida (routeId == 0) sigue activo.
  return supervise(twin, state, twinMeasurementDivergence, state.routeId);
}

Supervision UncertaintySupervisor::supervise(
    const TwinConfidence& twin, const AudioState& state,
    float twinMeasurementDivergence, uint32_t calibrationRouteId) const {
  Supervision out;

  // 1. Fail-closed ante lo no finito o fuera de rango.
  if (!std::isfinite(twin.global) || !std::isfinite(twinMeasurementDivergence) ||
      !std::isfinite(state.confidence)) {
    out.reason = kReasonNonFinite;
    return out;
  }
  if (twin.global < 0.0f || twin.global > 1.0f ||
      twinMeasurementDivergence < 0.0f || state.confidence < 0.0f ||
      state.confidence > 1.0f) {
    out.reason = kReasonNonFinite;
    return out;
  }
  if (!allParametersSane(twin)) {
    out.reason = kReasonInvalidParam;
    return out;
  }

  // 2. Gemelo o estado inválidos.
  if (!twin.valid) {
    out.reason = kReasonTwinInvalid;
    return out;
  }
  if (!state.valid) {
    out.reason = kReasonStateInvalid;
    return out;
  }

  // 3. Ruta: desconocida, o distinta a la de la calibración vigente.
  // Una calibración nunca se reutiliza en otra ruta (Luna M7/M8).
  if (state.routeId == 0) {
    out.reason = kReasonRouteUnknown;
    return out;
  }
  if (calibrationRouteId != 0 && state.routeId != calibrationRouteId) {
    out.reason = kReasonRouteMismatch;
    return out;
  }

  // 4. Dato viejo: cualquier parámetro válido más viejo que el tope.
  if (anyParameterStale(twin)) {
    out.reason = kReasonStale;
    return out;
  }

  // 5. Nivel base por confianza global (umbrales nominales del contrato).
  const float confidence = twin.global;
  if (confidence >= kConfidenceHigh) {
    out.level = InterventionLevel::kControl;
    out.allowControl = true;
    out.maxIntensity = 1.0f;
    out.reason = kReasonControl;
  } else if (confidence >= kConfidenceMedium) {
    out.level = InterventionLevel::kLimitOnly;
    out.allowControl = false;
    out.maxIntensity = confidence;
    out.reason = kReasonLimitOnly;
  } else if (confidence >= kConfidenceZeroAction) {
    out.level = InterventionLevel::kNone;
    out.allowControl = false;
    out.maxIntensity = 0.0f;
    out.reason = kReasonTelemetryOnly;
  } else {
    // < 0.20: acción cero (M6).
    out.level = InterventionLevel::kNone;
    out.allowControl = false;
    out.maxIntensity = 0.0f;
    out.reason = kReasonZeroAction;
    return out;
  }

  // 6. Discordancia gemelo/medición: baja un nivel y pide recalibración;
  // si es extrema, acción cero aunque la confianza sea alta (M6).
  if (twinMeasurementDivergence >= kDivergenceZeroAction) {
    out.level = InterventionLevel::kNone;
    out.allowControl = false;
    out.maxIntensity = 0.0f;
    out.reason = kReasonDivergenceZero;
    return out;
  }
  if (twinMeasurementDivergence > kMaxTwinMeasurementDivergence) {
    const InterventionLevel demoted = demoteOne(out.level);
    if (demoted != out.level) {
      out.level = demoted;
      out.allowControl = false;
      out.maxIntensity =
          (demoted == InterventionLevel::kLimitOnly) ? confidence : 0.0f;
      out.reason = kReasonDivergence;
    } else {
      // Ya estaba en kNone: no hay nivel que bajar, pero la
      // recalibración sigue pendiente.
      out.reason = kReasonDivergence;
    }
  }

  return out;
}

void UncertaintySupervisor::recordAnomaly() {
  ++consecutiveAnomalies_;
}

void UncertaintySupervisor::recordGoodBlock() {
  consecutiveAnomalies_ = 0;
}

bool UncertaintySupervisor::rollbackNeeded() const {
  return consecutiveAnomalies_ >= kRollbackAnomalyThreshold;
}

int UncertaintySupervisor::consecutiveAnomalies() const {
  return consecutiveAnomalies_;
}

void UncertaintySupervisor::resetRollback() {
  consecutiveAnomalies_ = 0;
}

}  // namespace ivanna::closed_loop
