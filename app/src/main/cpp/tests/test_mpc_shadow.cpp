// test_mpc_shadow.cpp — MPC perceptual en sombra (Luna M6, plan del salto).
//
// Verifica reglas y propiedades, no valores medidos (los pesos del
// simulador son calibración inicial, no mediciones; nada aquí afirma
// mejora audible):
//   1. campo neutro + gemelo válido -> propuesta finita (acción y J);
//   2. twin.global < 0.20 -> acción cero (y el umbral 0.20 no dispara);
//   3. campo con NaN -> acción cero;
//   4. dos contradicciones seguidas -> rollback (siguiente propuesta cero);
//   5. una contradicción sola NO dispara rollback;
//   6. el crossfade equal-power conserva energía (gA²+gB² == 1);
//   7. propose() no modifica el campo de entrada (la sombra no toca nada);
//   8. clipping (audibilidad en el riel / sin margen de potencia) -> cero;
//   9. presupuesto agotado -> cero por budget-exceeded.
// Además: gemelo inválido -> cero, y tras el rollback la sombra vuelve a
// decidir normal (el rollback se consume en una propuesta).

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "fused/closed_loop/mpc_shadow.hpp"
#include "fused/closed_loop/twin_confidence.hpp"
#include "fused/perceptual_mpc/perceptual_vector.hpp"

namespace {

using namespace ivanna::closed_loop;
using namespace ivanna::perceptual_mpc;

// Gemelo válido con la calibración estimada actual (0.70).
TwinConfidence validTwin(float global = 0.70f) {
  TwinConfidence t;
  t.global = global;
  t.valid = true;
  return t;
}

// Estado que pide intervención: claridad/impacto bajos, confianza alta,
// fatiga baja, presupuesto libre (mismo patrón que test_perceptual_mpc).
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

bool isZeroAction(const Action& a) { return actionL1(a) == 0.0; }

bool actionFinite(const Action& a) {
  for (double x : a) {
    if (!std::isfinite(x)) return false;
  }
  return true;
}

bool fieldsEqual(const PerceptualField& a, const PerceptualField& b) {
  for (std::size_t band = 0; band < kNumBands; ++band) {
    for (std::size_t d = 0; d < kNumDims; ++d) {
      const double x = a.bands[band].v[d];
      const double y = b.bands[band].v[d];
      if (x == y) continue;
      // NaN != NaN: dos NaN en la misma posición cuentan como "sin cambio".
      if (std::isnan(x) && std::isnan(y)) continue;
      return false;
    }
  }
  return true;
}

}  // namespace

// ── 1. Campo neutro + gemelo válido -> propuesta finita ─────────────────────
TEST(MpcShadow, NeutralFieldValidTwinReturnsFiniteProposal) {
  MpcShadow shadow;
  const PerceptualField field = PerceptualField::neutral();
  const MpcProposal p = shadow.propose(field, validTwin());
  EXPECT_TRUE(actionFinite(p.action));
  EXPECT_TRUE(std::isfinite(p.J));
  EXPECT_NE(p.reason, nullptr);
  EXPECT_EQ(shadow.proposalsTotal(), 1u);
  // El horizonte es 5 decisiones a 50 ms = 250 ms (Luna M6).
  EXPECT_DOUBLE_EQ(shadow.horizonMs(), 250.0);
}

// ── 2. twin.global < 0.20 -> acción cero; 0.20 exacto NO dispara ────────────
TEST(MpcShadow, TwinBelowThresholdReturnsZeroAction) {
  MpcShadow shadow;
  const PerceptualField field = demandingField();

  const MpcProposal low = shadow.propose(field, validTwin(0.19f));
  EXPECT_TRUE(low.isZero);
  EXPECT_TRUE(isZeroAction(low.action));
  EXPECT_STREQ(low.reason, "twin-low-confidence");

  // El umbral es < 0.20: con 0.20 exacto la sombra decide normal.
  const MpcProposal at = shadow.propose(field, validTwin(0.20f));
  EXPECT_STRNE(at.reason, "twin-low-confidence");
  EXPECT_FALSE(at.isZero);

  // Gemelo sin dato usable -> cero, aunque global diga otra cosa.
  TwinConfidence invalid = validTwin(0.70f);
  invalid.valid = false;
  const MpcProposal noTwin = shadow.propose(field, invalid);
  EXPECT_TRUE(noTwin.isZero);
  EXPECT_STREQ(noTwin.reason, "twin-invalid");

  // twin.global NaN -> cero.
  const MpcProposal nanTwin =
      shadow.propose(field, validTwin(std::numeric_limits<float>::quiet_NaN()));
  EXPECT_TRUE(nanTwin.isZero);
  EXPECT_STREQ(nanTwin.reason, "twin-nan");
}

// ── 3. Campo con NaN -> acción cero ─────────────────────────────────────────
TEST(MpcShadow, NanFieldReturnsZeroAction) {
  MpcShadow shadow;
  PerceptualField field = demandingField();
  field.bands[3][Dim::kClarity] = std::numeric_limits<double>::quiet_NaN();
  const MpcProposal p = shadow.propose(field, validTwin());
  EXPECT_TRUE(p.isZero);
  EXPECT_TRUE(isZeroAction(p.action));
  EXPECT_TRUE(std::isfinite(p.J));
  EXPECT_STREQ(p.reason, "nan-field");
}

// ── 4. Dos contradicciones seguidas -> rollback ─────────────────────────────
TEST(MpcShadow, TwoConsecutiveContradictionsTriggerRollback) {
  MpcShadow shadow;
  const PerceptualField field = demandingField();
  const TwinConfidence twin = validTwin();

  // La sombra propone algo no-cero con un campo que pide intervención.
  const MpcProposal first = shadow.propose(field, twin);
  EXPECT_FALSE(first.isZero);

  // El modelo predijo mejora y la medición contradice, dos veces seguidas.
  shadow.recordOutcome(/*predictedImprovement=*/true,
                       /*measuredImprovement=*/false);
  shadow.recordOutcome(/*predictedImprovement=*/true,
                       /*measuredImprovement=*/false);
  EXPECT_TRUE(shadow.rollbackPending());
  EXPECT_EQ(shadow.contradictionsTotal(), 2u);

  // La siguiente propuesta es acción cero por rollback, y queda registrada.
  const MpcProposal rolled = shadow.propose(field, twin);
  EXPECT_TRUE(rolled.isZero);
  EXPECT_TRUE(isZeroAction(rolled.action));
  EXPECT_STREQ(rolled.reason, "rollback");
  EXPECT_EQ(shadow.rollbacksTotal(), 1u);
  EXPECT_FALSE(shadow.rollbackPending());

  // El rollback se consume en una propuesta: la siguiente vuelve a
  // decidir normal (no queda clavada en cero).
  const MpcProposal after = shadow.propose(field, twin);
  EXPECT_STRNE(after.reason, "rollback");
  EXPECT_FALSE(after.isZero);
}

// ── 5. Una contradicción sola NO dispara rollback ───────────────────────────
TEST(MpcShadow, SingleContradictionDoesNotTriggerRollback) {
  MpcShadow shadow;
  const PerceptualField field = demandingField();
  const TwinConfidence twin = validTwin();

  const MpcProposal first = shadow.propose(field, twin);
  EXPECT_FALSE(first.isZero);

  shadow.recordOutcome(/*predictedImprovement=*/true,
                       /*measuredImprovement=*/false);
  EXPECT_FALSE(shadow.rollbackPending());
  EXPECT_EQ(shadow.consecutiveContradictions(), 1u);

  const MpcProposal next = shadow.propose(field, twin);
  EXPECT_STRNE(next.reason, "rollback");
  EXPECT_FALSE(next.isZero);

  // Y una medición que confirma resetea la racha: otra contradicción
  // aislada después tampoco dispara rollback.
  shadow.recordOutcome(/*predictedImprovement=*/true,
                       /*measuredImprovement=*/true);
  EXPECT_EQ(shadow.consecutiveContradictions(), 0u);
  shadow.recordOutcome(/*predictedImprovement=*/true,
                       /*measuredImprovement=*/false);
  EXPECT_FALSE(shadow.rollbackPending());
  const MpcProposal again = shadow.propose(field, twin);
  EXPECT_STRNE(again.reason, "rollback");
}

// ── 6. Crossfade equal-power conserva energía ───────────────────────────────
TEST(MpcShadow, CrossfadeEqualPowerConservesEnergy) {
  // gA² + gB² == 1 en todo t (energía de las ganancias constante).
  const double ts[] = {0.0, 0.25, 0.5, 0.75, 1.0};
  for (double t : ts) {
    const CrossfadeGains g = crossfadeEqualPowerGains(t);
    EXPECT_NEAR(g.gainA * g.gainA + g.gainB * g.gainB, 1.0, 1e-12)
        << "t=" << t;
  }
  // Extremos: en t=0 todo `a`, en t=1 todo `b` (cos(π/2) es ~6e-17, no
  // 0.0 exacto: NEAR, no DOUBLE_EQ).
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(1.0, 0.0, 0.0), 1.0);
  EXPECT_NEAR(crossfadeEqualPower(1.0, 0.0, 1.0), 0.0, 1e-12);
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(0.0, 1.0, 0.0), 0.0);
  EXPECT_NEAR(crossfadeEqualPower(0.0, 1.0, 1.0), 1.0, 1e-12);
  // Centro: dos señales iguales a 1.0 suman sqrt(2) (potencia, no
  // amplitud: 2·(1/√2)² = 1 por señal, sin caída de 3 dB).
  EXPECT_NEAR(crossfadeEqualPower(1.0, 1.0, 0.5), std::sqrt(2.0), 1e-12);
  // Coherencia: la mezcla es a·gA + b·gB.
  const CrossfadeGains g = crossfadeEqualPowerGains(0.25);
  EXPECT_NEAR(crossfadeEqualPower(0.3, -0.7, 0.25), 0.3 * g.gainA - 0.7 * g.gainB,
              1e-12);
  // t fuera de rango se clamprea; t no finito es fail-closed (sin cambio).
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(0.3, 0.9, -2.0),
                   crossfadeEqualPower(0.3, 0.9, 0.0));
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(0.3, 0.9, 5.0),
                   crossfadeEqualPower(0.3, 0.9, 1.0));
  EXPECT_DOUBLE_EQ(
      crossfadeEqualPower(0.3, 0.9, std::numeric_limits<double>::quiet_NaN()),
      0.3);
}

// ── 7. La sombra nunca aplica nada: el campo queda intacto ──────────────────
TEST(MpcShadow, ProposeDoesNotModifyInputField) {
  MpcShadow shadow;
  const PerceptualField field = demandingField();
  const PerceptualField before = field;
  const TwinConfidence twin = validTwin();
  const float globalBefore = twin.global;

  const MpcProposal p = shadow.propose(field, twin);
  EXPECT_FALSE(p.isZero);  // decide algo, pero sin tocar la entrada
  EXPECT_TRUE(fieldsEqual(field, before));
  EXPECT_FLOAT_EQ(twin.global, globalBefore);

  // Tampoco tras una propuesta cero por guarda.
  PerceptualField nanField = demandingField();
  nanField.bands[0][Dim::kFatigue] =
      std::numeric_limits<double>::quiet_NaN();
  const PerceptualField nanBefore = nanField;
  const MpcProposal zero = shadow.propose(nanField, twin);
  EXPECT_TRUE(zero.isZero);
  EXPECT_TRUE(fieldsEqual(nanField, nanBefore));
}

// ── 8. Clipping -> acción cero ──────────────────────────────────────────────
TEST(MpcShadow, ClippingReturnsZeroAction) {
  MpcShadow shadow;
  // Audibilidad clavada en el riel (1.0) en una banda: medidor saturado.
  PerceptualField field = demandingField();
  field.bands[5][Dim::kAudibility] = 1.0;
  const MpcProposal p = shadow.propose(field, validTwin());
  EXPECT_TRUE(p.isZero);
  EXPECT_STREQ(p.reason, "clipping");

  // Sin margen de potencia en el gemelo: la cadena está en su límite.
  TwinConfidence noHeadroom = validTwin();
  noHeadroom.powerHeadroom.valid = true;
  noHeadroom.powerHeadroom.estimate = 0.0f;
  const MpcProposal q = shadow.propose(demandingField(), noHeadroom);
  EXPECT_TRUE(q.isZero);
  EXPECT_STREQ(q.reason, "clipping");
}

// ── 9. Presupuesto agotado -> cero por budget-exceeded ──────────────────────
TEST(MpcShadow, BudgetExhaustedReturnsZeroAction) {
  MpcShadow shadow;
  PerceptualField field = demandingField();
  for (auto& b : field.bands) b[Dim::kBudget] = 0.95;
  const MpcProposal p = shadow.propose(field, validTwin());
  EXPECT_TRUE(p.isZero);
  EXPECT_TRUE(isZeroAction(p.action));
  EXPECT_STREQ(p.reason, "budget-exceeded");
}
