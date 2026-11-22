// test_action_simulator.cpp — Simulador de acciones M5 (plan del salto, Luna).
//
// Verifica lo que el módulo promete y nada más:
//   - la acción cero es allowed con J == 0 (incluso sin gemelo);
//   - una acción pequeña con campo neutro y gemelo válido es allowed
//     con J finito;
//   - cada restricción dura violada por separado rechaza con su reason;
//   - gemelo inválido o global < 0.20 rechaza;
//   - input no finito rechaza (fail-closed);
//   - una acción que mejora ΔP puntúa más que una que lo empeora.
// Los tests verifican reglas y orden, no valores medidos: los pesos de J
// son NOMINALES (calibrar con ABX, Orden 6) y ningún test afirma mejora
// audible.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "fused/closed_loop/action_simulator.hpp"
#include "fused/closed_loop/twin_confidence.hpp"
#include "fused/perceptual_mpc/perceptual_vector.hpp"

namespace {

using namespace ivanna::closed_loop;
using namespace ivanna::perceptual_mpc;

// Gemelo válido con la calibración estimada actual (0.70).
TwinConfidence validTwin(float global = 0.70f) {
  TwinConfidence t;
  t.valid = true;
  t.global = global;
  return t;
}

// Acción con un solo operador en delta.
Action singleOp(Operator op, double delta) {
  Action a = zeroAction();
  a[static_cast<std::size_t>(op)] = delta;
  return a;
}

// Evaluar con telemetría nominal segura: true peak -3 dBTP, sin
// aumentos de THD/ruido/latencia.
ActionScore evalNominal(const ActionSimulator& sim,
                        const PerceptualField& field, const Action& action,
                        const TwinConfidence& twin) {
  return sim.evaluate(field, action, twin, -3.0, 0.0, 0.0, 0.0);
}

}  // namespace

// ── 1. Acción cero: allowed, J == 0 ─────────────────────────────────────────
TEST(ActionSimulator, ZeroActionIsAllowedWithZeroJ) {
  ActionSimulator sim;
  const ActionScore s =
      evalNominal(sim, PerceptualField::neutral(), zeroAction(), validTwin());
  EXPECT_TRUE(s.allowed);
  EXPECT_EQ(s.J, 0.0);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonOk);
}

// La acción cero es allowed incluso sin gemelo: no hacer nada es seguro.
TEST(ActionSimulator, ZeroActionAllowedEvenWithInvalidTwin) {
  ActionSimulator sim;
  const TwinConfidence invalid;  // valid=false, global=0
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    zeroAction(), invalid);
  EXPECT_TRUE(s.allowed);
  EXPECT_EQ(s.J, 0.0);
}

// ── 2. Acción pequeña, campo neutro, gemelo válido ──────────────────────────
TEST(ActionSimulator, SmallActionNeutralFieldAllowedFiniteJ) {
  ActionSimulator sim;
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kNeuro, 0.1),
                                    validTwin());
  EXPECT_TRUE(s.allowed);
  EXPECT_TRUE(std::isfinite(s.J));
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonOk);
}

// Un corte de sala (delta negativo de SalaViva) se permite: cut-only.
TEST(ActionSimulator, RoomCutIsAllowed) {
  ActionSimulator sim;
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kSalaViva, -0.1),
                                    validTwin());
  EXPECT_TRUE(s.allowed);
  EXPECT_TRUE(std::isfinite(s.J));
}

// ── 3. Cada restricción dura violada por separado ───────────────────────────
TEST(ActionSimulator, TruePeakViolationRejected) {
  ActionSimulator sim;
  const ActionScore s = sim.evaluate(
      PerceptualField::neutral(), singleOp(Operator::kNeuro, 0.1),
      validTwin(), -0.5 /* > -1.0 dBTP */, 0.0, 0.0, 0.0);
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonTruePeak);
}

TEST(ActionSimulator, ThdViolationRejected) {
  ActionSimulator sim;
  const ActionScore s = sim.evaluate(PerceptualField::neutral(),
                                     singleOp(Operator::kNeuro, 0.1),
                                     validTwin(), -3.0, 1.5, 0.0, 0.0);
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonThd);
}

TEST(ActionSimulator, NoiseViolationRejected) {
  ActionSimulator sim;
  const ActionScore s = sim.evaluate(PerceptualField::neutral(),
                                     singleOp(Operator::kNeuro, 0.1),
                                     validTwin(), -3.0, 0.0, 2.0, 0.0);
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonNoise);
}

TEST(ActionSimulator, RateViolationRejected) {
  ActionSimulator sim;
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kOrbe, 0.21),
                                    validTwin());
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonRate);
}

TEST(ActionSimulator, RoomBoostViolationRejected) {
  ActionSimulator sim;
  // Delta positivo de SalaViva = ganancia neta de sala > 0 dB (boost).
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kSalaViva, 0.1),
                                    validTwin());
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonRoomGain);
}

TEST(ActionSimulator, LatencyViolationRejected) {
  ActionSimulator sim;
  const ActionScore s = sim.evaluate(PerceptualField::neutral(),
                                     singleOp(Operator::kNeuro, 0.1),
                                     validTwin(), -3.0, 0.0, 0.0, 1.5);
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonLatency);
}

// Los límites son inclusivos: el valor exacto del límite pasa.
TEST(ActionSimulator, LimitsAreInclusive) {
  ActionSimulator sim;
  const ActionScore s = sim.evaluate(
      PerceptualField::neutral(), singleOp(Operator::kOrbe, 0.20),
      validTwin(), -1.0, 1.0, 1.0, 1.0);
  EXPECT_TRUE(s.allowed);
  EXPECT_TRUE(std::isfinite(s.J));
}

// ── 4. Gemelo sin confianza ─────────────────────────────────────────────────
TEST(ActionSimulator, InvalidTwinRejected) {
  ActionSimulator sim;
  const TwinConfidence invalid;  // valid=false, global=0
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kNeuro, 0.1), invalid);
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonTwinNoConfidence);
}

TEST(ActionSimulator, LowConfidenceTwinRejected) {
  ActionSimulator sim;
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kNeuro, 0.1),
                                    validTwin(0.19f));
  EXPECT_FALSE(s.allowed);
  EXPECT_STREQ(s.reason, ActionSimulator::kReasonTwinNoConfidence);
}

// El umbral 0.20 es inclusivo: global == 0.20 controla.
TEST(ActionSimulator, TwinConfidenceBoundaryAllowed) {
  ActionSimulator sim;
  const ActionScore s = evalNominal(sim, PerceptualField::neutral(),
                                    singleOp(Operator::kNeuro, 0.1),
                                    validTwin(0.20f));
  EXPECT_TRUE(s.allowed);
}

// ── 5. Input no finito: fail-closed ─────────────────────────────────────────
TEST(ActionSimulator, NonFiniteInputsRejected) {
  ActionSimulator sim;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  // Campo con NaN.
  PerceptualField badField = PerceptualField::neutral();
  badField.bands[0][Dim::kClarity] = nan;
  EXPECT_FALSE(evalNominal(sim, badField, singleOp(Operator::kNeuro, 0.1),
                           validTwin())
                   .allowed);

  // Acción con NaN.
  Action badAction = singleOp(Operator::kNeuro, 0.1);
  badAction[0] = nan;
  EXPECT_FALSE(evalNominal(sim, PerceptualField::neutral(), badAction,
                           validTwin())
                   .allowed);

  // Acción con Inf (también viola el rate, pero el fail-closed manda).
  Action infAction = singleOp(Operator::kNeuro, inf);
  const ActionScore sInf = evalNominal(sim, PerceptualField::neutral(),
                                       infAction, validTwin());
  EXPECT_FALSE(sInf.allowed);
  EXPECT_STREQ(sInf.reason, ActionSimulator::kReasonNonFiniteInput);

  // Escalar NaN: true peak.
  const ActionScore sTp = sim.evaluate(PerceptualField::neutral(),
                                       singleOp(Operator::kNeuro, 0.1),
                                       validTwin(), nan, 0.0, 0.0, 0.0);
  EXPECT_FALSE(sTp.allowed);
  EXPECT_STREQ(sTp.reason, ActionSimulator::kReasonNonFiniteInput);

  // Escalar Inf: latencia.
  const ActionScore sLat = sim.evaluate(PerceptualField::neutral(),
                                        singleOp(Operator::kNeuro, 0.1),
                                        validTwin(), -3.0, 0.0, 0.0, inf);
  EXPECT_FALSE(sLat.allowed);
  EXPECT_STREQ(sLat.reason, ActionSimulator::kReasonNonFiniteInput);

  // Gemelo con global NaN.
  TwinConfidence nanTwin = validTwin();
  nanTwin.global = static_cast<float>(nan);
  EXPECT_FALSE(evalNominal(sim, PerceptualField::neutral(),
                           singleOp(Operator::kNeuro, 0.1), nanTwin)
                   .allowed);

  // Acción cero con telemetría NaN también rechaza: fail-closed manda
  // sobre el atajo de la acción cero.
  const ActionScore sZeroNan =
      sim.evaluate(PerceptualField::neutral(), zeroAction(), validTwin(),
                   nan, 0.0, 0.0, 0.0);
  EXPECT_FALSE(sZeroNan.allowed);
  EXPECT_STREQ(sZeroNan.reason, ActionSimulator::kReasonNonFiniteInput);
}

// ── 6. Orden: mejorar ΔP puntúa más que empeorarlo ──────────────────────────
// NEURO +0.1 baja la fatiga y sube claridad/naturalidad/estabilidad
// (ΔP > 0); NEURO −0.1 hace lo contrario (ΔP < 0). Ambas acciones son
// allowed; solo cambia el signo de la mejora predicha.
TEST(ActionSimulator, ImprovingActionScoresHigherThanWorsening) {
  ActionSimulator sim;
  const ActionScore good = evalNominal(sim, PerceptualField::neutral(),
                                       singleOp(Operator::kNeuro, 0.1),
                                       validTwin());
  const ActionScore bad = evalNominal(sim, PerceptualField::neutral(),
                                      singleOp(Operator::kNeuro, -0.1),
                                      validTwin());
  EXPECT_TRUE(good.allowed);
  EXPECT_TRUE(bad.allowed);
  EXPECT_TRUE(std::isfinite(good.J));
  EXPECT_TRUE(std::isfinite(bad.J));
  EXPECT_GT(good.J, bad.J);
  // Coherencia con el simulador: ΔP tiene el signo esperado.
  EXPECT_GT(sim.predictedDeltaP(PerceptualField::neutral(),
                                singleOp(Operator::kNeuro, 0.1)),
            0.0);
  EXPECT_LT(sim.predictedDeltaP(PerceptualField::neutral(),
                                singleOp(Operator::kNeuro, -0.1)),
            0.0);
}
