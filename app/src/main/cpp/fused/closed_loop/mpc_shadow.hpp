// mpc_shadow.hpp — MPC perceptual EN SOMBRA (Luna M6, plan del salto).
//
// MpcShadow es la capa M6 sobre el MPC existente: observa el campo
// perceptual P[t] y la confianza del gemelo, propone UNA acción para el
// horizonte de 5 decisiones a 50 ms (250 ms), la registra, y NUNCA la
// aplica al audio. No hay salida de audio aquí, ni siquiera un buffer:
// propose() solo lee el campo y el gemelo, y devuelve la propuesta.
//
// Reutilización (no duplicación):
//   - OperatorSimulator: predice el campo futuro paso a paso.
//   - MpcController: aporta el conjunto determinista de candidatos
//     (candidates()) y la función objetivo J (score()). La J que se suma
//     en el horizonte es la MISMA fórmula que M5 define en el plan
//     (ganancia perceptual w_p·ΔP menos penalizaciones por huella de la
//     acción, fase, transitorios, ruido, distorsión y latencia, con los
//     pesos nominales del MpcConfig). Cuando M5 aterrice su
//     ActionSimulator, la integración consiste en sustituir las llamadas
//     a controller_.score() por ActionSimulator::evaluateJ() con los
//     mismos términos y pesos nominales; el roll-out del horizonte y las
//     guardas de esta clase NO cambian.
//
// Reglas duras (Luna M6):
//   - twin.global < 0.20, gemelo inválido, NaN, clipping o divergencia
//     -> acción cero (isZero = true).
//   - Si el modelo predice mejora pero la medición la contradice 2 veces
//     seguidas (recordOutcome) -> rollback: la siguiente propuesta es
//     acción cero y se registra el rollback.
//   - Crossfade equal-power de 20–50 ms al aplicar una acción: el helper
//     crossfadeEqualPower() SOLO se calcula aquí; la aplicación real es
//     trabajo futuro (integración), nunca en sombra.
//
// propose() es const por contrato: desde fuera, la sombra no tiene efecto
// observable (no modifica campo, gemelo ni audio). El estado interno de
// decisión (acción previa, rollback pendiente, contadores) vive en
// miembros mutable: es contabilidad de la sombra, no efecto sobre el
// sistema.

#pragma once

#include <cstddef>

#include "../perceptual_mpc/mpc_controller.hpp"
#include "../perceptual_mpc/operator_simulator.hpp"
#include "../perceptual_mpc/perceptual_vector.hpp"
#include "twin_confidence.hpp"

namespace ivanna::closed_loop {

// ── Crossfade equal-power (para cuando una acción se aplique) ─────────────
// Mezcla a -> b con t en [0, 1]: ganancia de a = cos(t·π/2), ganancia de
// b = sin(t·π/2). Energía de las ganancias constante: gA² + gB² == 1 para
// todo t (sin caída de 3 dB en el centro, como un crossfade lineal).
// La rampa real es de 20–50 ms (plan M6); aquí solo se calcula el
// coeficiente para un t dado. t fuera de [0, 1] se clamprea; t no finito
// es fail-closed (sin cambio: devuelve a).
struct CrossfadeGains {
  double gainA = 1.0;
  double gainB = 0.0;
};

CrossfadeGains crossfadeEqualPowerGains(double t);
double crossfadeEqualPower(double a, double b, double t);

struct MpcShadowConfig {
  std::size_t horizon = 5;      // decisiones por horizonte (Luna M6: 5)
  double stepMs = 50.0;         // ms por decisión -> horizonte de 250 ms
  std::size_t numCandidates = 24;  // candidatos por paso (como MpcConfig)
  double maxDeltaPerCycle = 0.20;  // |a[t]-a[t-1]| máximo por operador
  double improvementThreshold = 0.01;  // histéresis: mejora mínima p/cambiar
  double minTwinConfidence = kConfidenceZeroAction;  // 0.20: bajo esto, cero
};

// Propuesta de la sombra. `reason` apunta a un literal estático:
//   "ok"                   propuesta normal (acción o cero por argmax)
//   "rollback"             cero por rollback tras 2 contradicciones
//   "twin-invalid"         gemelo sin dato usable (twin.valid == false)
//   "twin-nan"             twin.global no finito
//   "twin-low-confidence"  twin.global < minTwinConfidence (0.20)
//   "nan-field"            el campo perceptual no es finito
//   "clipping"             saturación: audibilidad en el riel o sin
//                          margen de potencia en el gemelo
//   "divergence"           el roll-out del horizonte diverge (no finito)
//   "budget-exceeded"      ningún candidato es factible (presupuesto)
// `J` es la suma de J en el horizonte para la acción devuelta (0.0 en
// las propuestas cero por guarda/rollback).
struct MpcProposal {
  perceptual_mpc::Action action{};
  double J = 0.0;
  bool isZero = true;
  const char* reason = "ok";
};

class MpcShadow {
 public:
  explicit MpcShadow(MpcShadowConfig cfg = MpcShadowConfig{});

  // Propone la mejor acción para el horizonte. NUNCA aplica nada: no
  // modifica el campo, el gemelo ni ningún audio. Solo lee.
  MpcProposal propose(const perceptual_mpc::PerceptualField& field,
                      const TwinConfidence& twin) const;

  // Registra el resultado de una propuesta anterior: el modelo predijo
  // mejora (predictedImprovement) y la medición la confirmó o no
  // (measuredImprovement). Dos contradicciones seguidas (predice mejora
  // y la medición dice que no) arman el rollback: la siguiente
  // propose() devuelve acción cero con reason "rollback". Cualquier
  // resultado no contradictorio resetea la racha.
  void recordOutcome(bool predictedImprovement, bool measuredImprovement);

  // Telemetría de la sombra (solo lectura; la llena propose/recordOutcome).
  std::size_t proposalsTotal() const { return proposals_; }
  std::size_t contradictionsTotal() const { return contradictions_; }
  std::size_t consecutiveContradictions() const {
    return consecutiveContradictions_;
  }
  std::size_t rollbacksTotal() const { return rollbacks_; }
  bool rollbackPending() const { return rollbackPending_; }
  double horizonMs() const {
    return static_cast<double>(cfg_.horizon) * cfg_.stepMs;
  }

  void reset();

 private:
  MpcShadowConfig cfg_;
  // El controlador aporta candidatos deterministas + la función J
  // (score). El simulador predice el campo paso a paso. Ninguno se
  // duplica aquí: esta clase solo orquesta el roll-out del horizonte.
  perceptual_mpc::MpcController controller_;
  perceptual_mpc::OperatorSimulator sim_;

  // Estado interno de decisión (mutable: propose() es const por contrato).
  mutable perceptual_mpc::Action previous_{};
  mutable bool hasPrevious_ = false;
  mutable bool rollbackPending_ = false;
  mutable std::size_t proposals_ = 0;
  mutable std::size_t contradictions_ = 0;
  mutable std::size_t consecutiveContradictions_ = 0;
  mutable std::size_t rollbacks_ = 0;
};

}  // namespace ivanna::closed_loop
