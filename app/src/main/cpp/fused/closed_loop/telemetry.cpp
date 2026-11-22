// telemetry.cpp — Telemetría del control cerrado (plan del salto, Luna,
// sección 3 punto 14). SOLO registra, nunca decide nada.
//
// La mecánica lock-free vive en telemetry.hpp (detail::LockFreeRing). Aquí
// solo: validación de finitud (un frame no finito se registra con
// valid=false, no se descarta) y el registro de fallos con motivo truncado.

#include "telemetry.hpp"

#include <cmath>
#include <cstring>

namespace ivanna::closed_loop {
namespace {

// Todos los campos float del frame deben ser finitos para que el frame
// sea válido. Los enteros (sampleTime, generation, xruns) siempre lo son.
bool allFinite(const TelemetryFrame& f) {
  if (!std::isfinite(f.confidence)) return false;
  for (float x : f.chosenAction) {
    if (!std::isfinite(x)) return false;
  }
  for (float x : f.appliedAction) {
    if (!std::isfinite(x)) return false;
  }
  if (!std::isfinite(f.energyBeforeDb)) return false;
  if (!std::isfinite(f.energyAfterDb)) return false;
  if (!std::isfinite(f.truePeakDbTp)) return false;
  if (!std::isfinite(f.thdEstimateDb)) return false;
  if (!std::isfinite(f.latencyMs)) return false;
  if (!std::isfinite(f.twinDivergence)) return false;
  return true;
}

}  // namespace

void Telemetry::record(const TelemetryFrame& frame) {
  TelemetryFrame stored = frame;
  if (!allFinite(stored)) {
    // No se descarta en silencio: queda registrado como inválido para
    // que el fallo sea visible en la telemetría.
    stored.valid = false;
  }
  frames_.push(stored);
}

void Telemetry::recordFailure(const char* reason) {
  FailureRecord rec{};
  rec.ordinal = failureOrdinal_.fetch_add(1, std::memory_order_relaxed);
  if (reason == nullptr) reason = "unknown";
  std::strncpy(rec.reason, reason, sizeof(rec.reason) - 1);
  rec.reason[sizeof(rec.reason) - 1] = '\0';
  failures_.push(rec);
}

}  // namespace ivanna::closed_loop
