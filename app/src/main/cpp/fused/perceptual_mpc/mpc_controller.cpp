// mpc_controller.cpp — Controlador predictivo perceptual (MPC) en MODO SOMBRA.
//
// El MPC nunca modifica audio. Solo decide. La decisión se registra en
// last_() y se cuenta en decisionsLogged(); nada aquí escribe en un buffer
// de audio salvo processShadow(), que es un memcpy bit-exacto.

#include "mpc_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace ivanna::perceptual_mpc {
namespace {

constexpr double kNegInf = -std::numeric_limits<double>::infinity();

// Limita `a` en tasa respecto a `prev`: |a-prev| <= maxDelta por operador,
// y además a [-1, 1].
Action rateLimit(const Action& a, const Action& prev, double maxDelta) {
  Action out;
  for (std::size_t i = 0; i < kNumOperators; ++i) {
    double lo = std::max(-1.0, prev[i] - maxDelta);
    double hi = std::min(1.0, prev[i] + maxDelta);
    double x = a[i];
    if (!std::isfinite(x)) x = 0.0;
    if (x < lo) x = lo;
    if (x > hi) x = hi;
    out[i] = x;
  }
  return out;
}

double stddev(const std::array<double, kNumBands>& xs, double mean) {
  double s = 0.0;
  for (double x : xs) {
    const double d = x - mean;
    s += d * d;
  }
  return std::sqrt(s / static_cast<double>(kNumBands));
}

}  // namespace

MpcController::MpcController(MpcConfig cfg) : cfg_(cfg) {
  // Fail-closed: una configuración inválida se recorta a valores sanos en
  // vez de crashear o de comportarse de forma indefinida.
  if (!std::isfinite(cfg_.maxDeltaPerCycle) || cfg_.maxDeltaPerCycle <= 0.0) {
    cfg_.maxDeltaPerCycle = 0.20;
  }
  if (cfg_.maxDeltaPerCycle > 1.0) cfg_.maxDeltaPerCycle = 1.0;
  if (!std::isfinite(cfg_.improvementThreshold) || cfg_.improvementThreshold < 0.0) {
    cfg_.improvementThreshold = 0.01;
  }
  if (cfg_.numCandidates == 0) cfg_.numCandidates = 1;
  if (cfg_.numCandidates > 256) cfg_.numCandidates = 256;
  if (cfg_.horizon == 0) cfg_.horizon = 1;
  if (!std::isfinite(cfg_.minConfidence) || cfg_.minConfidence < 0.0) {
    cfg_.minConfidence = 0.20;
  }
  if (cfg_.minConfidence > 1.0) cfg_.minConfidence = 1.0;
  if (!std::isfinite(cfg_.maxBudget) || cfg_.maxBudget <= 0.0) {
    cfg_.maxBudget = 0.90;
  }
  if (cfg_.maxBudget > 1.0) cfg_.maxBudget = 1.0;
  previous_.fill(0.0);
}

double MpcController::score(const PerceptualField& current,
                            const PerceptualField& predicted,
                            const Action& action) const {
  if (!predicted.isFinite()) return kNegInf;
  const double meanBudget = predicted.mean(Dim::kBudget);
  if (meanBudget > cfg_.maxBudget) return kNegInf;  // techo duro

  // Ganancia por banda (mejora respecto al estado actual) y penalización
  // por banda; luego se agregan y se penaliza la dispersión entre bandas.
  std::array<double, kNumBands> perBand{};
  for (std::size_t b = 0; b < kNumBands; ++b) {
    const PerceptualVector& cur = current.bands[b];
    const PerceptualVector& pre = predicted.bands[b];
    const double gain =
        cfg_.wClarity * (pre[Dim::kClarity] - cur[Dim::kClarity]) +
        cfg_.wNaturalness * (pre[Dim::kNaturalness] - cur[Dim::kNaturalness]) +
        cfg_.wImpact * (pre[Dim::kImpact] - cur[Dim::kImpact]) +
        cfg_.wSpatiality * (pre[Dim::kSpatiality] - cur[Dim::kSpatiality]) +
        cfg_.wStability * (pre[Dim::kStability] - cur[Dim::kStability]);
    // Fatiga con signo: subirla penaliza, bajarla premia (es el trabajo de
    // NEURO). Audibilidad: cualquier cambio de sonoridad es huella.
    // Confianza: solo se penaliza la caída.
    const double penalty =
        cfg_.wFatigue * (pre[Dim::kFatigue] - cur[Dim::kFatigue]) +
        cfg_.wAudibility *
            std::abs(pre[Dim::kAudibility] - cur[Dim::kAudibility]) +
        cfg_.wConfidence *
            std::max(0.0, cur[Dim::kConfidence] - pre[Dim::kConfidence]);
    perBand[b] = gain - penalty;
  }
  double meanScore = 0.0;
  for (double s : perBand) meanScore += s;
  meanScore /= static_cast<double>(kNumBands);
  const double incoherence = stddev(perBand, meanScore);

  // Huella de la intervención: magnitud de la acción (proxy de alteración
  // de fase / pre-post-ring / modulación / pérdida de transitorios) y
  // presupuesto consumido en la predicción.
  return meanScore - cfg_.wIncoherence * incoherence -
         cfg_.wMovement * actionL2(action) -
         cfg_.wLatency * actionL1(action) - cfg_.wBudget * meanBudget;
}

std::vector<Action> MpcController::candidates(const Action& previous) const {
  const Action prev = clampAction(previous);
  std::vector<Action> out;
  out.reserve(cfg_.numCandidates);

  // Candidatos estructurados (siempre en el mismo orden).
  std::vector<Action> structured;
  structured.push_back(zeroAction());
  for (std::size_t op = 0; op < kNumOperators; ++op) {
    Action up = zeroAction();
    up[op] = cfg_.maxDeltaPerCycle;
    structured.push_back(up);
    Action down = zeroAction();
    down[op] = -cfg_.maxDeltaPerCycle;
    structured.push_back(down);
  }
  Action allUp;
  allUp.fill(cfg_.maxDeltaPerCycle);
  structured.push_back(allUp);
  Action allDown;
  allDown.fill(-cfg_.maxDeltaPerCycle);
  structured.push_back(allDown);

  // Resto: pseudo-aleatorios deterministas (semilla fija del config).
  SplitMix64 rng(cfg_.seed);
  while (structured.size() < cfg_.numCandidates) {
    Action a;
    for (double& x : a) {
      x = rng.uniform(-cfg_.maxDeltaPerCycle, cfg_.maxDeltaPerCycle);
    }
    structured.push_back(a);
  }

  // Todos pasan por el límite de tasa respecto a `previous`.
  for (const Action& a : structured) {
    if (out.size() >= cfg_.numCandidates) break;
    out.push_back(rateLimit(a, prev, cfg_.maxDeltaPerCycle));
  }
  return out;
}

MpcDecision MpcController::step(const PerceptualField& field) {
  ++stepIndex_;
  MpcDecision d;
  d.stepIndex = stepIndex_;

  // Confianza media del campo (para telemetría y modo seguro).
  const double meanConfidence =
      field.isFinite() ? field.mean(Dim::kConfidence) : 0.0;

  // Modo seguro: campo no finito o confianza baja -> acción cero.
  const bool lowConfidence =
      !field.isFinite() || meanConfidence < cfg_.minConfidence;
  const Action prev = hasPrevious_ ? previous_ : zeroAction();
  const PerceptualField predZero = sim_.simulateHorizon(field, zeroAction(), cfg_.horizon);
  d.scoreZero = score(field, predZero, zeroAction());

  if (lowConfidence) {
    d.action = zeroAction();
    d.score = d.scoreZero;
    d.scoreKept = d.scoreZero;
    d.safeMode = true;
    d.keptPrevious = false;
    previous_ = d.action;
    hasPrevious_ = true;
    last_ = d;
    ++logged_;
    pushTelemetry(d, meanConfidence);
    return last_;
  }

  // Evaluar candidatos y quedarse con el mejor.
  const std::vector<Action> cands = candidates(prev);
  double bestScore = kNegInf;
  Action best = zeroAction();
  for (const Action& c : cands) {
    const PerceptualField pred = sim_.simulateHorizon(field, c, cfg_.horizon);
    const double s = score(field, pred, c);
    if (s > bestScore) {
      bestScore = s;
      best = c;
    }
  }

  // Histéresis: mantener la acción anterior si la mejora no supera el umbral.
  const PerceptualField predKept =
      sim_.simulateHorizon(field, prev, cfg_.horizon);
  d.scoreKept = score(field, predKept, prev);
  if (hasPrevious_ && (bestScore - d.scoreKept) < cfg_.improvementThreshold) {
    d.action = prev;
    d.keptPrevious = true;
    d.score = d.scoreKept;
  } else {
    d.action = best;
    d.keptPrevious = false;
    d.score = bestScore;
  }

  previous_ = d.action;
  hasPrevious_ = true;
  last_ = d;
  ++logged_;
  pushTelemetry(d, meanConfidence);
  return last_;
}

MpcTelemetry MpcController::telemetry() const {
  MpcTelemetry t;
  t.stepIndex = stepIndex_;
  t.decisionsLogged = logged_;
  t.lastAction = last_.action;
  t.lastScore = last_.score;
  t.lastScoreZero = last_.scoreZero;
  t.lastSafeMode = last_.safeMode;
  t.lastKeptPrevious = last_.keptPrevious;
  t.lastMeanConfidence = lastMeanConfidence_;
  t.historyCount = historyCount_;
  return t;
}

MpcDecision MpcController::telemetryHistoryAt(std::size_t index) const {
  if (index >= historyCount_) return MpcDecision{};
  // index 0 = más reciente. El anillo escribe en historyHead_ (próximo
  // slot); la más reciente está en (head - 1) mod size.
  const std::size_t pos =
      (historyHead_ + kHistorySize - 1 - index) % kHistorySize;
  return history_[pos];
}

void MpcController::pushTelemetry(const MpcDecision& d, double meanConfidence) {
  lastMeanConfidence_ = meanConfidence;
  history_[historyHead_] = d;
  historyHead_ = (historyHead_ + 1) % kHistorySize;
  if (historyCount_ < kHistorySize) ++historyCount_;
}

MpcDecision MpcController::processShadow(const float* in, float* out,
                                         std::size_t frames,
                                         const PerceptualField& field) {
  // Copia bit-exacta. memcpy con in == out es válido (misma región).
  if (frames > 0 && in != nullptr && out != nullptr) {
    std::memcpy(out, in, frames * sizeof(float));
  }
  // La decisión depende solo del campo, nunca del audio.
  return step(field);
}

void MpcController::reset() {
  previous_.fill(0.0);
  hasPrevious_ = false;
  last_ = MpcDecision{};
  stepIndex_ = 0;
  logged_ = 0;
  lastMeanConfidence_ = 0.0;
  history_.fill(MpcDecision{});
  historyCount_ = 0;
  historyHead_ = 0;
}

}  // namespace ivanna::perceptual_mpc
