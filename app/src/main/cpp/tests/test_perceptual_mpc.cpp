// test_perceptual_mpc.cpp — MPC perceptual en modo sombra.
//
// Verifica lo que el módulo promete y nada más:
//   - la sombra NO modifica la señal (bit-exacto);
//   - elige la acción de mejor puntuación (argmax reproducible);
//   - respeta el límite de tasa de cambio;
//   - la histéresis retiene la acción anterior;
//   - el modo seguro (confianza baja / NaN) devuelve acción cero;
//   - el techo de presupuesto no se viola.
// Nada aquí afirma mejora audible: el módulo está en sombra y los pesos del
// simulador son calibración inicial, no mediciones.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#include "fused/perceptual_mpc/mpc_controller.hpp"
#include "fused/perceptual_mpc/operator_simulator.hpp"
#include "fused/perceptual_mpc/perceptual_vector.hpp"
#include "fused/perceptual_mpc/twin_bridge.hpp"
#include "fused/acoustic_twin/acoustic_twin.hpp"

namespace {

using namespace ivanna::perceptual_mpc;

// Estado que pide intervención: claridad/impacto bajos, confianza alta,
// fatiga baja, presupuesto libre.
PerceptualField demandingField() {
  PerceptualField f = PerceptualField::neutral();
  for (auto& b : f.bands) {
    b[Dim::kClarity] = 0.3;
    b[Dim::kImpact] = 0.3;
    b[Dim::kNaturalness] = 0.4;
    b[Dim::kSpatiality] = 0.4;
    b[Dim::kFatigue] = 0.1;
    b[Dim::kConfidence] = 0.9;
    b[Dim::kBudget] = 0.0;
  }
  return f;
}

bool isZero(const Action& a) {
  for (double x : a) {
    if (x != 0.0) return false;
  }
  return true;
}

bool actionsEqual(const Action& a, const Action& b) {
  for (std::size_t i = 0; i < kNumOperators; ++i) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// PRNG determinista para rellenar buffers de test (splitmix64).
std::uint64_t splitmixNext(std::uint64_t& s) {
  std::uint64_t z = (s += 0x9E3779B97F4A7C15ULL);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
  return z ^ (z >> 31);
}

}  // namespace

// ── 1. La sombra no modifica la señal (bit-exacto) ──────────────────────────
TEST(MpcShadow, BitExactPassthrough) {
  MpcController mpc;
  constexpr std::size_t kFrames = 4096;
  std::vector<float> in(kFrames), out(kFrames, 12345.0f);
  std::uint64_t s = 0x123456789ABCDEF0ULL;
  for (std::size_t i = 0; i < kFrames; ++i) {
    const std::uint64_t r = splitmixNext(s);
    // Valores variados: ruido determinista en [-1, 1].
    in[i] = static_cast<float>((static_cast<double>(r >> 11) /
                                9007199254740992.0) *
                                   2.0 -
                               1.0);
  }
  // Casos borde: denormal, -0.0, extremos.
  in[0] = 1e-40f;
  in[1] = -0.0f;
  in[2] = 1.0f;
  in[3] = -1.0f;
  in[4] = 0.0f;

  const MpcDecision d = mpc.processShadow(in.data(), out.data(), kFrames, demandingField());
  EXPECT_EQ(std::memcmp(in.data(), out.data(), kFrames * sizeof(float)), 0);
  EXPECT_EQ(mpc.decisionsLogged(), 1u);
  EXPECT_EQ(d.stepIndex, 1u);
  EXPECT_FALSE(d.safeMode);
}

// ── 2. Elige la acción de mejor puntuación (argmax reproducible) ───────────
TEST(MpcShadow, ChoosesBestScore) {
  MpcConfig cfg;
  cfg.improvementThreshold = 0.0;  // sin histéresis: debe devolver el argmax
  cfg.numCandidates = 24;
  cfg.horizon = 1;
  MpcController mpc(cfg);
  const PerceptualField field = demandingField();

  const MpcDecision d = mpc.step(field);

  // Reproducir el argmax con la misma lista determinista de candidatos.
  const OperatorSimulator sim;
  double bestScore = -std::numeric_limits<double>::infinity();
  for (const Action& c : mpc.candidates(zeroAction())) {
    const PerceptualField pred = sim.simulateHorizon(field, c, cfg.horizon);
    bestScore = std::max(bestScore, mpc.score(field, pred, c));
  }
  EXPECT_DOUBLE_EQ(d.score, bestScore);
  EXPECT_GT(d.score, d.scoreZero);  // la mejor acción supera a no hacer nada
  EXPECT_FALSE(isZero(d.action));   // y no es la acción cero
}

// ── 3. Respeta el límite de tasa de cambio ─────────────────────────────────
TEST(MpcShadow, RateLimitRespected) {
  MpcConfig cfg;
  cfg.maxDeltaPerCycle = 0.20;
  cfg.improvementThreshold = 0.0;
  MpcController mpc(cfg);
  const PerceptualField field = demandingField();

  Action prev = zeroAction();
  for (int t = 0; t < 200; ++t) {
    const MpcDecision d = mpc.step(field);
    for (std::size_t op = 0; op < kNumOperators; ++op) {
      EXPECT_LE(std::abs(d.action[op] - prev[op]),
                cfg.maxDeltaPerCycle + 1e-12);
      EXPECT_LE(std::abs(d.action[op]), 1.0 + 1e-12);
    }
    prev = d.action;
  }
}

// ── 4. La histéresis retiene la acción anterior ────────────────────────────
TEST(MpcShadow, HysteresisHolds) {
  MpcConfig cfg;
  cfg.improvementThreshold = 100.0;  // umbral imposible de superar
  MpcController mpc(cfg);
  const PerceptualField field = demandingField();

  const MpcDecision d1 = mpc.step(field);
  const MpcDecision d2 = mpc.step(field);
  EXPECT_TRUE(actionsEqual(d2.action, d1.action));
  EXPECT_TRUE(d2.keptPrevious);
}

// ── 5. Modo seguro: confianza baja -> acción cero ──────────────────────────
TEST(MpcShadow, SafeModeLowConfidence) {
  MpcController mpc;
  PerceptualField lowConf = PerceptualField::neutral();
  for (auto& b : lowConf.bands) b[Dim::kConfidence] = 0.0;

  const MpcDecision d = mpc.step(lowConf);
  EXPECT_TRUE(isZero(d.action));
  EXPECT_TRUE(d.safeMode);

  // Recuperación: con confianza alta vuelve a decidir normal.
  const MpcDecision d2 = mpc.step(demandingField());
  EXPECT_FALSE(d2.safeMode);
}

// ── 6. Modo seguro: campo no finito (NaN) -> acción cero ───────────────────
TEST(MpcShadow, SafeModeNonFinite) {
  MpcController mpc;
  PerceptualField bad = PerceptualField::neutral();
  bad.bands[3][Dim::kClarity] = std::numeric_limits<double>::quiet_NaN();

  const MpcDecision d = mpc.step(bad);
  EXPECT_TRUE(isZero(d.action));
  EXPECT_TRUE(d.safeMode);
}

// ── 7. El techo de presupuesto no se viola ─────────────────────────────────
TEST(MpcShadow, BudgetCapRespected) {
  MpcConfig cfg;
  cfg.improvementThreshold = 0.0;
  MpcController mpc(cfg);
  PerceptualField field = demandingField();
  for (auto& b : field.bands) b[Dim::kBudget] = 0.89;  // casi al techo 0.90

  const MpcDecision d = mpc.step(field);
  EXPECT_FALSE(d.safeMode);

  const OperatorSimulator sim;
  const PerceptualField pred = sim.simulateHorizon(field, d.action, cfg.horizon);
  EXPECT_LE(pred.mean(Dim::kBudget), cfg.maxBudget + 1e-12);
  // Con presupuesto 0.89 y techo 0.90, solo cabe ||a||_1 <= 0.5
  // (0.02 por unidad de L1). La acción elegida debe respetar ese margen.
  EXPECT_LE(actionL1(d.action), 0.5 + 1e-12);
}

// ── 8. El simulador con acción cero es la identidad ────────────────────────
TEST(Simulator, ZeroActionIsIdentity) {
  const OperatorSimulator sim;
  PerceptualVector p = PerceptualVector::neutral();
  p[Dim::kClarity] = 0.0;
  p[Dim::kImpact] = 1.0;
  p[Dim::kBudget] = 0.73;
  const PerceptualVector out = sim.simulateStep(p, zeroAction());
  for (std::size_t i = 0; i < kNumDims; ++i) {
    EXPECT_DOUBLE_EQ(out.v[i], p.v[i]);
  }
}

// ── 9. Una acción unitaria coincide con la fila de pesos ───────────────────
TEST(Simulator, UnitActionMatchesWeightRow) {
  const OperatorSimulator sim;
  const PerceptualVector p = PerceptualVector::neutral();
  Action a = zeroAction();
  a[static_cast<std::size_t>(Operator::kVolterra)] = 1.0;
  const PerceptualVector out = sim.simulateStep(p, a);
  const auto& w = OperatorSimulator::weights();
  const auto& row = w[static_cast<std::size_t>(Operator::kVolterra)];
  for (std::size_t d = 0; d < kNumDims; ++d) {
    if (d == static_cast<std::size_t>(Dim::kBudget)) {
      // El presupuesto no va en la matriz: 0.02 * ||a||_1.
      EXPECT_DOUBLE_EQ(out.v[d], p.v[d] + 0.02);
    } else {
      double expected = p.v[d] + row[d];
      if (expected < 0.0) expected = 0.0;
      if (expected > 1.0) expected = 1.0;
      EXPECT_NEAR(out.v[d], expected, 1e-12);
    }
  }
}

// ── 10. El audio sigue intacto tras muchos pasos ───────────────────────────
TEST(MpcShadow, AudioUntouchedOverManySteps) {
  MpcController mpc;
  constexpr std::size_t kFrames = 1024;
  std::vector<float> in(kFrames), out(kFrames);
  std::uint64_t s = 0xFEDCBA9876543210ULL;
  for (std::size_t i = 0; i < kFrames; ++i) {
    in[i] = static_cast<float>((static_cast<double>(splitmixNext(s) >> 11) /
                                9007199254740992.0) *
                                   2.0 -
                               1.0);
  }
  const std::vector<float> ref = in;
  for (int t = 0; t < 50; ++t) {
    const PerceptualField f = (t % 2 == 0) ? demandingField()
                                           : PerceptualField::neutral();
    mpc.processShadow(in.data(), out.data(), kFrames, f);
    EXPECT_EQ(std::memcmp(in.data(), out.data(), kFrames * sizeof(float)), 0);
  }
  EXPECT_EQ(mpc.decisionsLogged(), 50u);
  EXPECT_EQ(std::memcmp(in.data(), ref.data(), kFrames * sizeof(float)), 0);
}

// ── 11. Determinismo: misma semilla y misma secuencia -> mismas decisiones ─
TEST(MpcShadow, DeterministicDecisions) {
  MpcConfig cfg;
  cfg.seed = 42;
  MpcController a(cfg), b(cfg);
  const PerceptualField f1 = demandingField();
  const PerceptualField f2 = PerceptualField::neutral();
  for (int t = 0; t < 20; ++t) {
    const PerceptualField& f = (t % 3 == 0) ? f2 : f1;
    const MpcDecision da = a.step(f);
    const MpcDecision db = b.step(f);
    EXPECT_TRUE(actionsEqual(da.action, db.action));
    EXPECT_DOUBLE_EQ(da.score, db.score);
  }
}

// ── 12. Puente gemelo→MPC: gemelo válido y con confianza alta ────────────
// El gemelo de Esse (Genezi) es válido; su interventionScale por banda es
// > 0 (confianza ~0.46 global), así que la confianza de P[t] se conserva
// en su mayor parte y el MPC puede decidir (no entra en modo seguro por
// el gemelo). Se verifica que la confianza se escala por el factor del
// gemelo, no que se pone a 1 ni a 0.
TEST(TwinBridge, ValidHighConfidencePreservesField) {
  using namespace ivanna::fused::acoustic_twin;
  const AcousticTwinEstimate twin =
      AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
  ASSERT_TRUE(twin.valid);
  PerceptualField f = demandingField();
  const double confBefore = f.mean(Dim::kConfidence);
  applyTwinToField(f, twin);
  // La confianza se multiplica por interventionScale (0..1): no sube.
  EXPECT_LE(f.mean(Dim::kConfidence), confBefore);
  EXPECT_GT(f.mean(Dim::kConfidence), 0.0);
  // Con confianza aún > minConfidence (0.20), el MPC decide (no safe mode
  // forzado por el gemelo). La decisión puede ser cero o no; lo que se
  // verifica es que NO entra en modo seguro por falta de confianza.
  MpcController mpc;
  const MpcDecision d = mpc.step(f);
  EXPECT_FALSE(d.safeMode);
}

// ── 13. Puente gemelo→MPC: gemelo válido pero con confianza baja ──────────
// Si el gemelo tiene confianza ~0 (p. ej. parámetros inconsistentes), la
// confianza de P[t] cae y el MPC entra en modo seguro (acción cero).
// Fail-closed: la incertidumbre del trayecto apaga la intervención.
TEST(TwinBridge, ValidLowConfidenceForcesSafeMode) {
  using namespace ivanna::fused::acoustic_twin;
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.roomRt60Sec = 5.0;  // inconsistente: sala imposible -> confianza baja
  p.listenerDistanceM = 50.0;
  const AcousticTwinEstimate twin = AcousticTwin::estimate(p);
  ASSERT_TRUE(twin.valid);
  ASSERT_LT(twin.overallInterventionScale, 0.3);
  PerceptualField f = demandingField();
  applyTwinToField(f, twin);
  EXPECT_LT(f.mean(Dim::kConfidence), 0.20);
  MpcController mpc;
  const MpcDecision d = mpc.step(f);
  EXPECT_TRUE(d.safeMode);
  EXPECT_TRUE(isZero(d.action));
}

// ── 14. Puente gemelo→MPC: gemelo inválido no toca el campo ───────────────
// Fail-closed: si el gemelo no puede estimar (valid == false), el campo
// queda intacto y el MPC se comporta exactamente como antes.
TEST(TwinBridge, InvalidTwinLeavesFieldUntouched) {
  using namespace ivanna::fused::acoustic_twin;
  AcousticTwinEstimate twin;
  twin.valid = false;
  PerceptualField f = demandingField();
  const PerceptualField before = f;
  applyTwinToField(f, twin);
  for (std::size_t b = 0; b < kNumBands; ++b) {
    for (std::size_t d = 0; d < kNumDims; ++d) {
      EXPECT_DOUBLE_EQ(f.bands[b].v[d], before.bands[b].v[d]);
    }
  }
  // El MPC decide igual que sin gemelo.
  MpcController a, b;
  const MpcDecision da = a.step(f);
  const MpcDecision db = b.step(before);
  EXPECT_TRUE(actionsEqual(da.action, db.action));
  EXPECT_DOUBLE_EQ(da.score, db.score);
}

// ── 15. Puente gemelo→MPC: baja confianza consume presupuesto ─────────────
// A confianza baja del gemelo, el presupuesto de P[t] sube (ser
// conservador). A confianza 1, el presupuesto no cambia.
TEST(TwinBridge, LowConfidenceConsumesBudget) {
  using namespace ivanna::fused::acoustic_twin;
  AcousticTwinEstimate twin;
  twin.valid = true;
  for (auto& b : twin.band) {
    b.interventionScale = 0.0;  // confianza cero en todas las bandas
    b.confidence = 0.0;
  }
  twin.overallInterventionScale = 0.0;
  PerceptualField f = demandingField();
  const double budgetBefore = f.mean(Dim::kBudget);
  applyTwinToField(f, twin);
  EXPECT_GT(f.mean(Dim::kBudget), budgetBefore);
  // Con confianza 1, el presupuesto no se mueve.
  AcousticTwinEstimate twinFull;
  twinFull.valid = true;
  for (auto& b : twinFull.band) {
    b.interventionScale = 1.0;
    b.confidence = 1.0;
  }
  twinFull.overallInterventionScale = 1.0;
  PerceptualField f2 = demandingField();
  const double budgetBefore2 = f2.mean(Dim::kBudget);
  applyTwinToField(f2, twinFull);
  EXPECT_DOUBLE_EQ(f2.mean(Dim::kBudget), budgetBefore2);
}

// ── 16. Telemetría de la sombra: instantánea + historial ─────────────────
// step() registra la decisión: telemetry() refleja la última, el
// historial guarda las últimas 32 (0 = más reciente) y reset() lo limpia.
TEST(MpcTelemetry, SnapshotAndHistory) {
  MpcController mpc;
  EXPECT_EQ(mpc.telemetry().decisionsLogged, 0u);
  EXPECT_EQ(mpc.telemetryHistoryCount(), 0u);
  const PerceptualField f = demandingField();
  for (int t = 0; t < 5; ++t) mpc.step(f);
  const MpcTelemetry tel = mpc.telemetry();
  EXPECT_EQ(tel.stepIndex, 5u);
  EXPECT_EQ(tel.decisionsLogged, 5u);
  EXPECT_EQ(tel.historyCount, 5u);
  EXPECT_EQ(mpc.telemetryHistoryCount(), 5u);
  // La más reciente (índice 0) es la última decisión.
  EXPECT_EQ(mpc.telemetryHistoryAt(0).stepIndex, 5u);
  EXPECT_EQ(mpc.telemetryHistoryAt(4).stepIndex, 1u);
  // Fuera de rango devuelve decisión neutra (sin crashear).
  EXPECT_EQ(mpc.telemetryHistoryAt(99).stepIndex, 0u);
  // La instantánea refleja la última decisión.
  EXPECT_DOUBLE_EQ(tel.lastScore, mpc.lastDecision().score);
  EXPECT_EQ(tel.lastSafeMode, mpc.lastDecision().safeMode);
  // reset() limpia todo.
  mpc.reset();
  EXPECT_EQ(mpc.telemetry().decisionsLogged, 0u);
  EXPECT_EQ(mpc.telemetryHistoryCount(), 0u);
  EXPECT_EQ(mpc.telemetry().stepIndex, 0u);
}

// ── 17. Telemetría + sombra: el audio sigue bit-exacto ────────────────────
// processShadow() copia el audio bit-exacto Y registra la telemetría.
// La observación en vivo no modifica la señal.
TEST(MpcTelemetry, ShadowStillBitExact) {
  MpcController mpc;
  constexpr std::size_t kFrames = 512;
  std::vector<float> in(kFrames), out(kFrames);
  std::uint64_t s = 0x123456789ABCDEF0ULL;
  for (std::size_t i = 0; i < kFrames; ++i) {
    in[i] = static_cast<float>((static_cast<double>(splitmixNext(s) >> 11) /
                                9007199254740992.0) * 2.0 - 1.0);
  }
  const PerceptualField f = demandingField();
  mpc.processShadow(in.data(), out.data(), kFrames, f);
  EXPECT_EQ(std::memcmp(in.data(), out.data(), kFrames * sizeof(float)), 0);
  EXPECT_EQ(mpc.telemetry().decisionsLogged, 1u);
  EXPECT_EQ(mpc.telemetryHistoryCount(), 1u);
  EXPECT_GT(mpc.telemetry().lastMeanConfidence, 0.0);
}
