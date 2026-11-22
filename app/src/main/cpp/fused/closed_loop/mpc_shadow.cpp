// mpc_shadow.cpp — MPC perceptual EN SOMBRA (Luna M6, plan del salto).
//
// a* = argmax de la suma de J en el horizonte (5 decisiones a 50 ms):
// para cada candidato determinista del MpcController se rueda el
// OperatorSimulator paso a paso y se acumula J = controller_.score()
// en cada paso. score() ES la función J de M5 (misma fórmula, mismos
// términos y pesos nominales del plan); cuando M5 aterrice su
// ActionSimulator, la integración es sustituir controller_.score() por
// ActionSimulator::evaluateJ() — el roll-out y las guardas no cambian.
//
// La sombra NUNCA aplica la acción: propose() no escribe en el campo,
// el gemelo, ningún buffer de audio ni ningún estado externo. Solo
// actualiza su contabilidad interna (acción previa, rollback, contadores).

#include "mpc_shadow.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace ivanna::closed_loop {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNegInf = -std::numeric_limits<double>::infinity();

using namespace ivanna::perceptual_mpc;

// Clipping: el campo perceptual vive en [0, 1] por contrato; una
// audibilidad clavada en 1.0 es el medidor saturado en el riel. Además,
// si el gemelo conoce el margen de potencia y es <= 0, la cadena está
// en su límite: cualquier acción que pida más energía es clipping.
bool isClipping(const PerceptualField& field, const TwinConfidence& twin) {
  for (const auto& b : field.bands) {
    if (b[Dim::kAudibility] >= 1.0) return true;
  }
  if (twin.powerHeadroom.valid && twin.powerHeadroom.estimate <= 0.0f) {
    return true;
  }
  return false;
}

}  // namespace

CrossfadeGains crossfadeEqualPowerGains(double t) {
  // Fail-closed: t no finito -> sin cambio (toda la ganancia en `a`).
  if (!std::isfinite(t)) return CrossfadeGains{1.0, 0.0};
  if (t < 0.0) t = 0.0;
  if (t > 1.0) t = 1.0;
  const double theta = t * (kPi / 2.0);
  return CrossfadeGains{std::cos(theta), std::sin(theta)};
}

double crossfadeEqualPower(double a, double b, double t) {
  const CrossfadeGains g = crossfadeEqualPowerGains(t);
  return a * g.gainA + b * g.gainB;
}

MpcShadow::MpcShadow(MpcShadowConfig cfg) : cfg_(cfg) {
  // Fail-closed: una configuración inválida se recorta a valores sanos.
  if (cfg_.horizon == 0) cfg_.horizon = 1;
  if (cfg_.horizon > 16) cfg_.horizon = 16;
  if (!std::isfinite(cfg_.stepMs) || cfg_.stepMs <= 0.0) cfg_.stepMs = 50.0;
  if (cfg_.numCandidates == 0) cfg_.numCandidates = 1;
  if (cfg_.numCandidates > 256) cfg_.numCandidates = 256;
  if (!std::isfinite(cfg_.maxDeltaPerCycle) || cfg_.maxDeltaPerCycle <= 0.0) {
    cfg_.maxDeltaPerCycle = 0.20;
  }
  if (cfg_.maxDeltaPerCycle > 1.0) cfg_.maxDeltaPerCycle = 1.0;
  if (!std::isfinite(cfg_.improvementThreshold) ||
      cfg_.improvementThreshold < 0.0) {
    cfg_.improvementThreshold = 0.01;
  }
  if (!std::isfinite(cfg_.minTwinConfidence) || cfg_.minTwinConfidence < 0.0) {
    cfg_.minTwinConfidence = kConfidenceZeroAction;
  }
  if (cfg_.minTwinConfidence > 1.0) cfg_.minTwinConfidence = 1.0;

  // El controlador se configura con el mismo horizonte y los mismos
  // límites: sus candidatos ya vienen limitados en tasa y su score() es
  // la J que se suma. step() no se usa: M6 necesita el argmax sobre la
  // SUMA de J en el horizonte, que es el roll-out de propose().
  MpcConfig mpcCfg;
  mpcCfg.horizon = cfg_.horizon;
  mpcCfg.numCandidates = cfg_.numCandidates;
  mpcCfg.maxDeltaPerCycle = cfg_.maxDeltaPerCycle;
  mpcCfg.improvementThreshold = cfg_.improvementThreshold;
  mpcCfg.minConfidence = cfg_.minTwinConfidence;
  controller_ = MpcController(mpcCfg);

  previous_.fill(0.0);
}

MpcProposal MpcShadow::propose(const PerceptualField& field,
                               const TwinConfidence& twin) const {
  ++proposals_;
  MpcProposal p;
  p.action = zeroAction();
  p.J = 0.0;
  p.isZero = true;

  // Propuesta cero por guarda: la acción previa se resetea a cero (como
  // hace MpcController::step() en modo seguro), para que el siguiente
  // ciclo no herede una acción de un estado que ya no es válido.
  const auto zeroWith = [&](const char* reason) {
    p.reason = reason;
    previous_ = zeroAction();
    hasPrevious_ = true;
    return p;
  };

  // Rollback pendiente (2 contradicciones seguidas): la siguiente
  // propuesta es acción cero y el rollback queda registrado. Se consume
  // en esta propuesta; el ciclo siguiente vuelve a decidir normal.
  if (rollbackPending_) {
    rollbackPending_ = false;
    ++rollbacks_;
    return zeroWith("rollback");
  }

  // Guardas duras M6, en orden de chequeo (gemelo -> campo -> señal).
  if (!twin.valid) return zeroWith("twin-invalid");
  if (!std::isfinite(twin.global)) return zeroWith("twin-nan");
  if (twin.global < static_cast<float>(cfg_.minTwinConfidence)) {
    return zeroWith("twin-low-confidence");
  }
  if (!field.isFinite()) return zeroWith("nan-field");
  if (isClipping(field, twin)) return zeroWith("clipping");

  // ── Argmax sobre la suma de J en el horizonte ──────────────────────────
  // Para cada candidato: rodar el simulador paso a paso (misma acción
  // repetida, como hace simulateHorizon) y acumular J en cada paso con
  // la fórmula de M5 (controller_.score). Un candidato es infactible si
  // su predicción no es finita (divergencia) o su J es -inf (techo de
  // presupuesto de score()): se descarta, no se le inventa puntuación.
  const Action prev = hasPrevious_ ? previous_ : zeroAction();
  const std::vector<Action> cands = controller_.candidates(prev);

  const auto horizonSumJ = [&](const Action& a, double& outJ) {
    // Devuelve false si el roll-out diverge o es infactible.
    PerceptualField f = field;
    double sum = 0.0;
    for (std::size_t h = 0; h < cfg_.horizon; ++h) {
      const PerceptualField pred = sim_.simulateField(f, a);
      if (!pred.isFinite()) return false;
      const double j = controller_.score(f, pred, a);
      if (!std::isfinite(j)) return false;  // -inf: techo de presupuesto
      sum += j;
      f = pred;
    }
    outJ = sum;
    return true;
  };

  double bestJ = kNegInf;
  Action best = zeroAction();
  bool anyFeasible = false;
  for (const Action& c : cands) {
    double jSum = 0.0;
    // horizonSumJ descarta el candidato si su roll-out diverge o su J es
    // -inf (techo de presupuesto de score()): no se le inventa puntuación.
    if (!horizonSumJ(c, jSum)) continue;
    anyFeasible = true;
    if (jSum > bestJ) {
      bestJ = jSum;
      best = c;
    }
  }
  if (!anyFeasible) {
    // Sin candidatos factibles: si el presupuesto del campo ya supera el
    // techo, hasta la acción cero es infactible -> budget-exceeded; en
    // cualquier otro caso, divergencia del modelo. El 0.90 es el techo
    // duro MpcConfig::maxBudget (default, no sobrescrito en MpcShadow).
    const bool budgetBlown = field.mean(Dim::kBudget) > 0.90;
    return zeroWith(budgetBlown ? "budget-exceeded" : "divergence");
  }

  // Histéresis: retener la acción anterior si la mejora del mejor
  // candidato no supera el umbral (evita chattering entre operadores).
  if (hasPrevious_) {
    double keptJ = 0.0;
    if (horizonSumJ(prev, keptJ) &&
        (bestJ - keptJ) < cfg_.improvementThreshold) {
      best = prev;
      bestJ = keptJ;
    }
  }

  p.action = clampAction(best);
  p.J = bestJ;
  p.isZero = (actionL1(p.action) == 0.0);
  p.reason = "ok";
  previous_ = p.action;
  hasPrevious_ = true;
  return p;
}

void MpcShadow::recordOutcome(bool predictedImprovement,
                              bool measuredImprovement) {
  // Solo contradice si el modelo predijo mejora y la medición dice que
  // no la hubo. Un acierto (o no haber predicho mejora) resetea la racha.
  if (predictedImprovement && !measuredImprovement) {
    ++contradictions_;
    ++consecutiveContradictions_;
    if (consecutiveContradictions_ >= 2) {
      rollbackPending_ = true;
      consecutiveContradictions_ = 0;
    }
  } else {
    consecutiveContradictions_ = 0;
  }
}

void MpcShadow::reset() {
  previous_.fill(0.0);
  hasPrevious_ = false;
  rollbackPending_ = false;
  proposals_ = 0;
  contradictions_ = 0;
  consecutiveContradictions_ = 0;
  rollbacks_ = 0;
}

}  // namespace ivanna::closed_loop
