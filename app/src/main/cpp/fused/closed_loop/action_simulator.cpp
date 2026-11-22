// action_simulator.cpp — Simulador de acciones M5 (plan del salto, Luna).
//
// Implementación de ActionSimulator: predicción con OperatorSimulator +
// función de costo J + restricciones duras. Ver action_simulator.hpp
// para el contrato completo (fórmula, pesos NOMINALES, precedencia de
// chequeos y lo que esta API no puede verificar todavía: fase y mapa
// delta→dB de sala).

#include "action_simulator.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::closed_loop {
namespace {

// Máximo |a_op| de la acción: la acción ES el delta por ciclo de
// control, así que esto es el rate por operador.
double maxAbsAction(const perceptual_mpc::Action& action) {
  double m = 0.0;
  for (double x : action) m = std::max(m, std::abs(x));
  return m;
}

}  // namespace

double ActionSimulator::predictedDeltaP(
    const perceptual_mpc::PerceptualField& field,
    const perceptual_mpc::Action& action) const {
  return deltaP(field, sim_.simulateField(field, action));
}

double ActionSimulator::deltaP(
    const perceptual_mpc::PerceptualField& current,
    const perceptual_mpc::PerceptualField& predicted) {
  double sum = 0.0;
  for (std::size_t b = 0; b < perceptual_mpc::kNumBands; ++b) {
    double band = 0.0;
    for (std::size_t d = 0; d < perceptual_mpc::kNumDims; ++d) {
      band += kDimGain[d] * (predicted.bands[b].v[d] - current.bands[b].v[d]);
    }
    sum += band;
  }
  return sum / static_cast<double>(perceptual_mpc::kNumBands);
}

double ActionSimulator::transientLoss(
    const perceptual_mpc::PerceptualField& current,
    const perceptual_mpc::PerceptualField& predicted) {
  double sum = 0.0;
  for (std::size_t b = 0; b < perceptual_mpc::kNumBands; ++b) {
    sum += predicted.bands[b][perceptual_mpc::Dim::kImpact] -
           current.bands[b][perceptual_mpc::Dim::kImpact];
  }
  const double meanDeltaImpact =
      sum / static_cast<double>(perceptual_mpc::kNumBands);
  return std::max(0.0, -meanDeltaImpact);
}

double ActionSimulator::cost(const perceptual_mpc::PerceptualField& field,
                             const perceptual_mpc::Action& action,
                             double thdIncreaseDb, double noiseIncreaseDb,
                             double latencyIncreaseMs) const {
  const perceptual_mpc::PerceptualField predicted =
      sim_.simulateField(field, action);
  const double dP = deltaP(field, predicted);
  // E_phase = 0: sin entrada de fase en esta API (ver header). Se deja
  // el término explícito para que la fórmula coincida con la de Luna.
  constexpr double ePhase = 0.0;
  const double eTransient = transientLoss(field, predicted);
  const double eNoise = std::max(0.0, noiseIncreaseDb);
  const double eDistortion = std::max(0.0, thdIncreaseDb);
  const double eLatency = std::max(0.0, latencyIncreaseMs);
  double actionEnergy = 0.0;  // ||Δa||²
  for (double x : action) actionEnergy += x * x;

  return kWeightPerceptual * dP - kWeightAction * actionEnergy -
         kWeightPhase * ePhase - kWeightTransient * eTransient -
         kWeightNoise * eNoise - kWeightDistortion * eDistortion -
         kWeightLatency * eLatency;
}

bool ActionSimulator::isZeroAction(const perceptual_mpc::Action& action) {
  for (double x : action) {
    if (x != 0.0) return false;
  }
  return true;
}

bool ActionSimulator::isFiniteAction(const perceptual_mpc::Action& action) {
  for (double x : action) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}

ActionScore ActionSimulator::evaluate(
    const perceptual_mpc::PerceptualField& field,
    const perceptual_mpc::Action& action, const TwinConfidence& twin,
    double truePeakDbTp, double thdIncreaseDb, double noiseIncreaseDb,
    double latencyIncreaseMs) const {
  ActionScore out;

  // (1) Fail-closed: cualquier input no finito rechaza. Primero: ni
  // siquiera la acción cero se acepta con telemetría corrupta.
  if (!field.isFinite() || !isFiniteAction(action) ||
      !std::isfinite(static_cast<double>(twin.global)) ||
      !std::isfinite(truePeakDbTp) || !std::isfinite(thdIncreaseDb) ||
      !std::isfinite(noiseIncreaseDb) || !std::isfinite(latencyIncreaseMs)) {
    out.J = 0.0;
    out.allowed = false;
    out.reason = kReasonNonFiniteInput;
    return out;
  }

  // (2) La acción cero siempre es allowed, con J = 0. No hacer nada es
  // seguro aunque el gemelo no tenga confianza.
  if (isZeroAction(action)) {
    out.J = 0.0;
    out.allowed = true;
    out.reason = kReasonOk;
    return out;
  }

  // (3) Gemelo sin confianza: no se controla a ciegas (M6: acción cero).
  if (!twin.valid || twin.global < kConfidenceZeroAction) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonTwinNoConfidence;
    return out;
  }

  // (4) Restricciones duras, en el orden del plan. La primera que se
  // viola decide la reason.
  if (truePeakDbTp > kMaxTruePeakDbTp) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonTruePeak;
    return out;
  }
  if (thdIncreaseDb > kMaxThdIncreaseDb) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonThd;
    return out;
  }
  if (noiseIncreaseDb > kMaxNoiseIncreaseDb) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonNoise;
    return out;
  }
  if (maxAbsAction(action) > kMaxRatePerCycle) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonRate;
    return out;
  }
  // Sala cut-only: sin mapa calibrado delta→dB, cualquier delta positivo
  // de SalaViva es ganancia neta de sala > 0 dB (boost de un valle o de
  // un pico). Los cortes (delta negativo) se permiten.
  if (action[static_cast<std::size_t>(
          perceptual_mpc::Operator::kSalaViva)] > 0.0) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonRoomGain;
    return out;
  }
  if (latencyIncreaseMs > kMaxLatencyIncreaseMs) {
    out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
                 latencyIncreaseMs);
    out.allowed = false;
    out.reason = kReasonLatency;
    return out;
  }
  // Fase: sin entrada de fase en esta API no hay nada que chequear aquí;
  // kMaxPhaseDeviationDeg lo aplica M6 cuando exista telemetría de fase
  // (Órdenes 1–2). No se inventa un modelo delta→grados.

  // (5) Pasa el gate: J es la puntuación sin restricciones.
  out.J = cost(field, action, thdIncreaseDb, noiseIncreaseDb,
               latencyIncreaseMs);
  out.allowed = true;
  out.reason = kReasonOk;
  return out;
}

}  // namespace ivanna::closed_loop
