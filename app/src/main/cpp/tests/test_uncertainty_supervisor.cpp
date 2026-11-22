// test_uncertainty_supervisor.cpp — M8: supervisor de incertidumbre +
// rollback (plan del salto, Luna).
//
// Los tests verifican las reglas con los umbrales NOMINALES documentados
// en twin_confidence.hpp (0.80 / 0.50 / 0.20) y en
// uncertainty_supervisor.hpp (kMaxAgeSeconds = 300 s, divergencia alta
// 0.25, divergencia extrema 0.75). Ningún número aquí es una métrica
// medida: son los umbrales del contrato, y si el contrato cambia, estos
// tests deben cambiar con él.

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <limits>

#include "fused/closed_loop/uncertainty_supervisor.hpp"

namespace ivanna::closed_loop {
namespace {

constexpr uint32_t kRouteGenezi = 0xA11CE001u;
constexpr uint32_t kRouteOther = 0xBEEF0002u;

TwinConfidence makeTwin(float global) {
  TwinConfidence twin;
  twin.global = global;
  twin.valid = true;
  return twin;
}

AudioState makeState(uint32_t routeId = kRouteGenezi) {
  AudioState state;
  state.sampleTime = 48000;
  state.generation = 1;
  state.routeId = routeId;
  state.calibrationId = 7;
  state.confidence = 0.9f;
  state.valid = true;
  return state;
}

bool reasonContains(const Supervision& s, const char* needle) {
  return s.reason != nullptr &&
         std::strstr(s.reason, needle) != nullptr;
}

}  // namespace

TEST(UncertaintySupervisor, HighConfidenceAllowsControl) {
  UncertaintySupervisor sup;
  const Supervision s =
      sup.supervise(makeTwin(0.9f), makeState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kControl);
  EXPECT_TRUE(s.allowControl);
  EXPECT_FLOAT_EQ(s.maxIntensity, 1.0f);
  EXPECT_NE(s.reason, nullptr);
}

TEST(UncertaintySupervisor, EstimatedCalibrationLimitsIntensityOnly) {
  // El 0.70 estimado actual cae en "media": solo limita intensidad, con
  // maxIntensity = la propia confianza. NUNCA controla EQ fino, fase,
  // Volterra ni sala.
  UncertaintySupervisor sup;
  const Supervision s =
      sup.supervise(makeTwin(0.70f), makeState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kLimitOnly);
  EXPECT_FALSE(s.allowControl);
  EXPECT_FLOAT_EQ(s.maxIntensity, 0.70f);
}

TEST(UncertaintySupervisor, LowConfidenceIsTelemetryOnly) {
  UncertaintySupervisor sup;
  const Supervision s =
      sup.supervise(makeTwin(0.3f), makeState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_FALSE(s.allowControl);
  EXPECT_FLOAT_EQ(s.maxIntensity, 0.0f);
}

TEST(UncertaintySupervisor, VeryLowConfidenceIsZeroAction) {
  UncertaintySupervisor sup;
  const Supervision s =
      sup.supervise(makeTwin(0.1f), makeState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_FALSE(s.allowControl);
  EXPECT_FLOAT_EQ(s.maxIntensity, 0.0f);
  EXPECT_TRUE(reasonContains(s, "cero"));
}

TEST(UncertaintySupervisor, InvalidTwinOrStateIsNone) {
  UncertaintySupervisor sup;
  TwinConfidence invalidTwin = makeTwin(0.9f);
  invalidTwin.valid = false;
  const Supervision a = sup.supervise(invalidTwin, makeState(), 0.0f);
  EXPECT_EQ(a.level, InterventionLevel::kNone);
  EXPECT_FALSE(a.allowControl);
  EXPECT_FLOAT_EQ(a.maxIntensity, 0.0f);

  AudioState invalidState = makeState();
  invalidState.valid = false;
  const Supervision b = sup.supervise(makeTwin(0.9f), invalidState, 0.0f);
  EXPECT_EQ(b.level, InterventionLevel::kNone);
  EXPECT_FALSE(b.allowControl);
  EXPECT_FLOAT_EQ(b.maxIntensity, 0.0f);
}

TEST(UncertaintySupervisor, StaleDataIsNone) {
  UncertaintySupervisor sup;
  TwinConfidence stale = makeTwin(0.9f);
  stale.h1[0].valid = true;
  stale.h1[0].age = kMaxAgeSeconds + 1.0f;  // 301 s: expirado
  const Supervision s = sup.supervise(stale, makeState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_FALSE(s.allowControl);
  EXPECT_FLOAT_EQ(s.maxIntensity, 0.0f);

  // Borde: justo en el tope sigue siendo válido.
  TwinConfidence fresh = makeTwin(0.9f);
  fresh.h1[0].valid = true;
  fresh.h1[0].age = kMaxAgeSeconds;
  const Supervision ok = sup.supervise(fresh, makeState(), 0.0f);
  EXPECT_EQ(ok.level, InterventionLevel::kControl);
}

TEST(UncertaintySupervisor, HighDivergenceDemotesOneLevel) {
  UncertaintySupervisor sup;
  // 0.9 con divergencia 0.5: kControl -> kLimitOnly, y pide recalibración.
  const Supervision a =
      sup.supervise(makeTwin(0.9f), makeState(), 0.5f);
  EXPECT_EQ(a.level, InterventionLevel::kLimitOnly);
  EXPECT_FALSE(a.allowControl);
  EXPECT_FLOAT_EQ(a.maxIntensity, 0.9f);
  EXPECT_TRUE(reasonContains(a, "recalibracion"));

  // 0.70 con divergencia 0.5: kLimitOnly -> kNone.
  const Supervision b =
      sup.supervise(makeTwin(0.70f), makeState(), 0.5f);
  EXPECT_EQ(b.level, InterventionLevel::kNone);
  EXPECT_FLOAT_EQ(b.maxIntensity, 0.0f);
  EXPECT_TRUE(reasonContains(b, "recalibracion"));

  // Divergencia pequeña no degrada.
  const Supervision c =
      sup.supervise(makeTwin(0.9f), makeState(), 0.1f);
  EXPECT_EQ(c.level, InterventionLevel::kControl);

  // Divergencia extrema: acción cero aunque la confianza sea alta (M6).
  const Supervision d =
      sup.supervise(makeTwin(0.9f), makeState(), 0.9f);
  EXPECT_EQ(d.level, InterventionLevel::kNone);
  EXPECT_FLOAT_EQ(d.maxIntensity, 0.0f);
  EXPECT_TRUE(reasonContains(d, "recalibracion"));
}

TEST(UncertaintySupervisor, NonFiniteInputIsNone) {
  UncertaintySupervisor sup;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float inf = std::numeric_limits<float>::infinity();

  EXPECT_EQ(sup.supervise(makeTwin(nan), makeState(), 0.0f).level,
            InterventionLevel::kNone);
  EXPECT_EQ(sup.supervise(makeTwin(inf), makeState(), 0.0f).level,
            InterventionLevel::kNone);
  EXPECT_EQ(sup.supervise(makeTwin(0.9f), makeState(), nan).level,
            InterventionLevel::kNone);
  EXPECT_EQ(sup.supervise(makeTwin(0.9f), makeState(), inf).level,
            InterventionLevel::kNone);

  TwinConfidence nanParam = makeTwin(0.9f);
  nanParam.latency.valid = true;
  nanParam.latency.confidence = nan;
  EXPECT_EQ(sup.supervise(nanParam, makeState(), 0.0f).level,
            InterventionLevel::kNone);

  AudioState nanState = makeState();
  nanState.confidence = nan;
  EXPECT_EQ(sup.supervise(makeTwin(0.9f), nanState, 0.0f).level,
            InterventionLevel::kNone);
}

TEST(UncertaintySupervisor, RollbackAfterTwoConsecutiveAnomalies) {
  UncertaintySupervisor sup;
  EXPECT_FALSE(sup.rollbackNeeded());

  sup.recordAnomaly();
  EXPECT_FALSE(sup.rollbackNeeded()) << "una sola anomalia no dispara";
  EXPECT_EQ(sup.consecutiveAnomalies(), 1);

  sup.recordAnomaly();
  EXPECT_TRUE(sup.rollbackNeeded()) << "dos anomalias seguidas disparan";
  EXPECT_EQ(sup.consecutiveAnomalies(), 2);

  sup.resetRollback();
  EXPECT_FALSE(sup.rollbackNeeded());
  EXPECT_EQ(sup.consecutiveAnomalies(), 0);

  // Un bloque bueno corta la racha: anomalia, bueno, anomalia -> false.
  sup.recordAnomaly();
  sup.recordGoodBlock();
  EXPECT_EQ(sup.consecutiveAnomalies(), 0);
  sup.recordAnomaly();
  EXPECT_FALSE(sup.rollbackNeeded());
  sup.recordAnomaly();
  EXPECT_TRUE(sup.rollbackNeeded());
}

TEST(UncertaintySupervisor, RouteMismatchIsNone) {
  UncertaintySupervisor sup;
  // Misma ruta que la calibración: controla.
  const Supervision ok = sup.supervise(makeTwin(0.9f), makeState(),
                                        0.0f, kRouteGenezi);
  EXPECT_EQ(ok.level, InterventionLevel::kControl);

  // Ruta distinta a la de la calibración: inválido, nunca se reutiliza
  // una calibración en otra ruta (M7/M8).
  const Supervision bad = sup.supervise(makeTwin(0.9f), makeState(),
                                         0.0f, kRouteOther);
  EXPECT_EQ(bad.level, InterventionLevel::kNone);
  EXPECT_FALSE(bad.allowControl);
  EXPECT_FLOAT_EQ(bad.maxIntensity, 0.0f);

  // Ruta desconocida (routeId == 0): inválido.
  const Supervision unknown =
      sup.supervise(makeTwin(0.9f), makeState(0), 0.0f);
  EXPECT_EQ(unknown.level, InterventionLevel::kNone);
}

TEST(UncertaintySupervisor, ThresholdBoundaries) {
  UncertaintySupervisor sup;
  // Justo en 0.80: controla. Justo debajo: solo limita.
  EXPECT_EQ(sup.supervise(makeTwin(0.80f), makeState(), 0.0f).level,
            InterventionLevel::kControl);
  EXPECT_EQ(sup.supervise(makeTwin(0.79f), makeState(), 0.0f).level,
            InterventionLevel::kLimitOnly);
  // Justo en 0.50: limita. Justo debajo: solo telemetría.
  EXPECT_EQ(sup.supervise(makeTwin(0.50f), makeState(), 0.0f).level,
            InterventionLevel::kLimitOnly);
  EXPECT_EQ(sup.supervise(makeTwin(0.49f), makeState(), 0.0f).level,
            InterventionLevel::kNone);
  // Justo en 0.20: telemetría (no acción cero). Justo debajo: cero.
  EXPECT_EQ(sup.supervise(makeTwin(0.20f), makeState(), 0.0f).level,
            InterventionLevel::kNone);
  EXPECT_TRUE(reasonContains(sup.supervise(makeTwin(0.20f), makeState(),
                                            0.0f),
                             "telemetria"));
  EXPECT_TRUE(reasonContains(sup.supervise(makeTwin(0.19f), makeState(),
                                            0.0f),
                             "cero"));
}

}  // namespace ivanna::closed_loop
