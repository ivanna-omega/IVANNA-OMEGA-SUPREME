// test_closed_loop_adversarial.cpp — Banco adversarial host del control
// cerrado (plan del salto, Luna, sección 3 punto 10 y sección 4).
//
// Qué cubre (cada caso verifica fail-closed: acción cero / bypass /
// telemetría, nunca crash, nunca NaN propagado, nunca acción no acotada):
//   (1) NaN e Inf en cada entrada (AudioState, TwinConfidence, campo
//       perceptual, acciones, telemetría, gemelo, conservación).
//   (2) Clipping / valores extremos (+-1e30, +-1.0 en campos 0..1).
//   (3) Cambios de parámetros más rápidos que el límite 0.20/ciclo.
//   (4) Calibración vieja (age enorme) y routeId distinto.
//   (5) Divergencia gemelo/medición extrema.
//   (6) Confianza 0 y confianza NaN.
//   (7) Señales vacías / cero.
//   (8) Generación que salta (huecos) y generación que retrocede.
//
// Módulos probados contra IMPLEMENTACIÓN REAL (no solo headers):
//   M0 AudioStatePublisher, M3 TwinEstimator, M5 ActionSimulator,
//   M6 MpcShadow, M8 UncertaintySupervisor, telemetría, y el código
//   existente que el lazo usa: AcousticTwin, PerceptualConservation,
//   MpcController, OperatorSimulator, PerceptualVector/Action.
// Los .cpp hermanos se compilan por ruta en el target de CMake (ver
// CMakeLists.txt); si un .cpp hermano no existe, el target no enlaza y
// el fallo es visible en el build, no silencioso.
//
// DEFECTOS REALES PINNEADOS (tests KnownDefect_*): afirman el
// comportamiento defectuoso OBSERVADO para que el fallo quede
// reproducido y no se pierda; al corregir el módulo dueño, el test
// debe INVERTIRSE a la aserción correcta:
//   - KnownDefect_TwinRoom1e200NonFiniteRt60 (gemelo): estimate()
//     con sala 1e200 devuelve valid=true con rt60Sec NaN, volumen inf
//     y distancia crítica inf. M3 lo contiene (TwinEstimator lo marca
//     inválido), pero el gemelo promete "jamás NaN en la salida".
//   - KnownDefect_Distance1e308NonFiniteLatency (gemelo, misma clase):
//     distancia de escucha 1e308 m -> airLatencyMs y latencia por
//     banda en +inf con valid=true (a 1e305 m todo es finito).
//   - KnownDefect_MpcScoreNaNCurrentPropagatesNaN (MPC): score() con
//     el campo actual no finito devuelve NaN en vez de -inf, y step()
//     en modo seguro publica score/scoreZero NaN en la decisión y la
//     telemetría (la acción sí es cero: el fail-closed de la acción
//     funciona; lo que se propaga es el NaN en la puntuación).
//
// OBSERVACIONES DOCUMENTADAS (no defectos; el comportamiento es el del
// contrato y queda registrado para que nadie lo "arregle" por error):
//   - M0 es un tubo tonto: publica tal cual lo que se le da, incluido
//     un AudioState con confianza NaN; el fail-closed vive aguas abajo
//     (M8 rechaza el estado, M6 rechaza el gemelo). La generación que
//     retrocede o con huecos la NORMALIZA el publicador (nunca se
//     observa una generación que retroceda al leer).
//   - M5 solo rechaza inputs NO FINITOS: un campo finito pero absurdo
//     (claridad 1e30) pasa el gate con J finita; quien lo caza en la
//     cadena es M6 (clipping por audibilidad en el riel).
//   - La telemetría cuenta los frames perdidos por sobrescritura en
//     el DRENADO, no en el push: tras 300 pushes, dropped()==0 hasta
//     drenar; al drenar, dropped()==44 (los 44 más viejos).
//   - ConservationConfig::clamp() y el saneo interno mapean +Inf a 0
//     (el extremo inferior), no a 1: una energía medida +Inf se lee
//     como silencio y no limita. Solo alcanzable con muestras double
//     > ~1e154 (fuera del rango float32); con +-1e30 la salida sigue
//     siendo finita y acotada por la entrada.
//   - crossfadeEqualPower() sanea t (no finito -> sin cambio) pero NO
//     sanea a/b: con a NaN el resultado es NaN. El llamador debe
//     sanear las ganancias.
//   - TwinEstimator::canControlFine() mira el MÉTODO (kMeasured en
//     todas las variables) y el global, no el VALOR de cada confianza.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "fused/acoustic_twin/acoustic_twin.hpp"
#include "fused/closed_loop/action_simulator.hpp"
#include "fused/closed_loop/audio_state.hpp"
#include "fused/closed_loop/mpc_shadow.hpp"
#include "fused/closed_loop/telemetry.hpp"
#include "fused/closed_loop/twin_confidence.hpp"
#include "fused/closed_loop/twin_estimator.hpp"
#include "fused/closed_loop/uncertainty_supervisor.hpp"
#include "fused/perceptual_conservation.hpp"
#include "fused/perceptual_mpc/mpc_controller.hpp"
#include "fused/perceptual_mpc/operator_simulator.hpp"
#include "fused/perceptual_mpc/perceptual_vector.hpp"

namespace {

using namespace ivanna::closed_loop;
using namespace ivanna::perceptual_mpc;
using namespace ivanna::fused;
using namespace ivanna::fused::acoustic_twin;

constexpr double kNan = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();
constexpr float kNanF = std::numeric_limits<float>::quiet_NaN();
constexpr float kInfF = std::numeric_limits<float>::infinity();

// ── Constructores de fixtures ────────────────────────────────────────────

TwinConfidence HealthyTwin(float global) {
  TwinConfidence t;
  t.valid = true;
  t.global = global;
  return t;
}

AudioState HealthyState() {
  AudioState s;
  s.sampleTime = 48000;
  s.generation = 1;
  s.routeId = 7;
  s.calibrationId = 3;
  s.confidence = 0.9f;
  s.valid = true;
  return s;
}

// Campo que pide intervención: claridad/impacto bajos, confianza alta,
// fatiga baja, presupuesto libre, audibilidad a mitad (lejos del riel).
PerceptualField DemandingField() {
  PerceptualField f = PerceptualField::neutral();
  for (auto& b : f.bands) {
    b[Dim::kClarity] = 0.3;
    b[Dim::kImpact] = 0.3;
    b[Dim::kNaturalness] = 0.4;
    b[Dim::kSpatiality] = 0.4;
    b[Dim::kFatigue] = 0.1;
  }
  return f;
}

bool ActionFiniteAndBounded(const Action& a, double bound) {
  for (double x : a) {
    if (!std::isfinite(x)) return false;
    if (std::abs(x) > bound) return false;
  }
  return true;
}

bool EstimateAllFinite(const AcousticTwinEstimate& e) {
  if (!std::isfinite(e.overallConfidence) ||
      !std::isfinite(e.overallInterventionScale) ||
      !std::isfinite(e.roomVolumeM3) || !std::isfinite(e.roomSurfaceM2) ||
      !std::isfinite(e.schroederHz) || !std::isfinite(e.criticalDistanceM) ||
      !std::isfinite(e.bufferLatencyMs) || !std::isfinite(e.airLatencyMs) ||
      !std::isfinite(e.calibrationConfidence)) {
    return false;
  }
  for (const auto& b : e.band) {
    if (!std::isfinite(b.gainDb) || !std::isfinite(b.latencyMs) ||
        !std::isfinite(b.dynamicRangeDb) || !std::isfinite(b.thdPercent) ||
        !std::isfinite(b.noiseDbSpl) || !std::isfinite(b.rt60Sec) ||
        !std::isfinite(b.coherence) || !std::isfinite(b.compressionDb) ||
        !std::isfinite(b.maxSplDb) || !std::isfinite(b.confidence) ||
        !std::isfinite(b.interventionScale)) {
      return false;
    }
  }
  return true;
}

AcousticTwinEstimate CalibratedTwinEstimate() {
  return AcousticTwin::estimateCalibrated(
      AcousticTwinParams::esseGeneziCalibrated(),
      TwinCalibration::geneziPz1dEstimated());
}

// ── M0: AudioStatePublisher (contrato temporal único) ─────────────────────

TEST(M0Publisher, DefaultReadIsInvalidGenerationZero) {
  AudioStatePublisher pub;
  const AudioState r = pub.read();
  EXPECT_FALSE(r.valid);
  EXPECT_EQ(r.generation, 0u);
  EXPECT_EQ(r.routeId, 0u);
  EXPECT_EQ(r.calibrationId, 0u);
}

TEST(M0Publisher, RepeatedPublishAutoIncrementsGeneration) {
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.generation = 0;  // el llamador no avanza la generación: la pone el pub
  for (uint64_t expect = 1; expect <= 5; ++expect) {
    pub.publish(s);
    EXPECT_EQ(pub.read().generation, expect);
  }
}

TEST(M0Publisher, ExplicitGenerationRespected) {
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.generation = 100;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 100u);
  s.generation = 101;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 101u);
}

TEST(M0Publisher, GenerationGapIsPublishedAsIs) {
  // (8) generación que salta: el publicador respeta el salto que el
  // llamador fijó (el salto lo juzga el consumidor, M8/M6); lo que
  // nunca hace es inventar generaciones intermedias.
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.generation = 10;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 10u);
  s.generation = 5000;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 5000u);
}

TEST(M0Publisher, GenerationRegressionIsNormalizedForwardNeverBackwards) {
  // (8) generación que retrocede: publicar 50 tras 100 NO se observa
  // como 50 al leer; el publicador la normaliza a última+1 (101).
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.generation = 100;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 100u);
  s.generation = 50;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 101u);
  s.generation = 101;  // repetir la misma generación tampoco retrocede
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 102u);
}

TEST(M0Publisher, GenerationWrapsAtUint64MaxWithoutCrash) {
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.generation = UINT64_MAX;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, UINT64_MAX);
  s.generation = 0;
  pub.publish(s);  // última+1 con wrap: 0 (aritmética módulo 2^64)
  EXPECT_EQ(pub.read().generation, 0u);
  s.generation = 0;
  pub.publish(s);
  EXPECT_EQ(pub.read().generation, 1u);
}

TEST(M0Publisher, InvalidStatePassesThroughUnchanged) {
  // El publicador es un tubo tonto: NO sanea. Un estado inválido o con
  // confianza NaN se lee tal cual se publicó; el fail-closed vive aguas
  // abajo (M8 rechaza el estado, M6 rechaza el gemelo). Lo que se
  // verifica aquí es que el tubo no corrompe ni crashea.
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.valid = false;
  s.confidence = kNanF;
  s.generation = 42;
  pub.publish(s);
  const AudioState r = pub.read();
  EXPECT_EQ(r.generation, 42u);
  EXPECT_FALSE(r.valid);
  EXPECT_TRUE(std::isnan(r.confidence));
}

TEST(M0Publisher, RouteAndCalibrationSurvivePublish) {
  AudioStatePublisher pub;
  AudioState s = HealthyState();
  s.routeId = 0xDEADBEEFu;
  s.calibrationId = 0x12345678u;
  s.generation = 7;
  pub.publish(s);
  const AudioState r = pub.read();
  EXPECT_EQ(r.routeId, 0xDEADBEEFu);
  EXPECT_EQ(r.calibrationId, 0x12345678u);
}

// ── M5: ActionSimulator (restricciones duras + J) ─────────────────────────

TEST(M5Simulator, NanFieldRejectsEvenZeroAction) {
  // (1) La acción cero NO se acepta con telemetría corrupta: el chequeo
  // de finitud precede al atajo de acción cero.
  ActionSimulator sim;
  PerceptualField f = DemandingField();
  f.bands[0][Dim::kClarity] = kNan;
  const ActionScore r =
      sim.evaluate(f, zeroAction(), HealthyTwin(0.9f), -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.J, 0.0);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonNonFiniteInput);
}

TEST(M5Simulator, NanActionRejects) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[3] = kNan;
  const ActionScore r = sim.evaluate(DemandingField(), a, HealthyTwin(0.9f),
                                     -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.J, 0.0);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonNonFiniteInput);
}

TEST(M5Simulator, InfActionRejects) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = kInf;
  const ActionScore r = sim.evaluate(DemandingField(), a, HealthyTwin(0.9f),
                                     -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonNonFiniteInput);
}

TEST(M5Simulator, NanTwinGlobalRejects) {
  // (6) confianza NaN del gemelo: rechazo, J = 0.
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  const ActionScore r = sim.evaluate(DemandingField(), a, HealthyTwin(kNanF),
                                     -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.J, 0.0);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonNonFiniteInput);
}

TEST(M5Simulator, NanTruePeakThdNoiseLatencyEachRejects) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  const TwinConfidence twin = HealthyTwin(0.9f);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, twin, kNan, 0.0, 0.0, 0.0)
                   .reason,
               ActionSimulator::kReasonNonFiniteInput);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, twin, -3.0, kNan, 0.0, 0.0)
                   .reason,
               ActionSimulator::kReasonNonFiniteInput);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, twin, -3.0, 0.0, kInf, 0.0)
                   .reason,
               ActionSimulator::kReasonNonFiniteInput);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, twin, -3.0, 0.0, 0.0, kNan)
                   .reason,
               ActionSimulator::kReasonNonFiniteInput);
}

TEST(M5Simulator, ZeroActionAlwaysAllowedEvenWithNoTwinConfidence) {
  // (6) confianza 0: la acción cero sigue siendo allowed con J = 0.
  ActionSimulator sim;
  const ActionScore r = sim.evaluate(DemandingField(), zeroAction(),
                                     HealthyTwin(0.0f), -3.0, 0.0, 0.0, 0.0);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.J, 0.0);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonOk);
}

TEST(M5Simulator, InvalidTwinRejectsNonZeroAction) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  TwinConfidence twin = HealthyTwin(0.9f);
  twin.valid = false;
  const ActionScore r =
      sim.evaluate(DemandingField(), a, twin, -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_TRUE(std::isfinite(r.J));
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonTwinNoConfidence);
}

TEST(M5Simulator, TwinGlobalBoundary020) {
  // (6) 0.20 es el umbral inclusivo: 0.1999 rechaza, 0.20 permite.
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  EXPECT_FALSE(sim.evaluate(DemandingField(), a, HealthyTwin(0.1999f), -3.0,
                            0.0, 0.0, 0.0)
                   .allowed);
  EXPECT_TRUE(sim.evaluate(DemandingField(), a, HealthyTwin(0.20f), -3.0, 0.0,
                           0.0, 0.0)
                  .allowed);
}

TEST(M5Simulator, TruePeakBoundary) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  EXPECT_FALSE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -0.99, 0.0,
                            0.0, 0.0)
                   .allowed);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -0.99, 0.0,
                            0.0, 0.0)
                   .reason,
               ActionSimulator::kReasonTruePeak);
  EXPECT_TRUE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -1.0, 0.0,
                           0.0, 0.0)
                  .allowed);
}

TEST(M5Simulator, ThdAndNoiseBoundaries) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  EXPECT_FALSE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0,
                            1.0001, 0.0, 0.0)
                   .allowed);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0,
                            1.0001, 0.0, 0.0)
                   .reason,
               ActionSimulator::kReasonThd);
  EXPECT_TRUE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 1.0,
                           0.0, 0.0)
                  .allowed);
  EXPECT_FALSE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 0.0,
                            1.0001, 0.0)
                   .allowed);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 0.0,
                            1.0001, 0.0)
                   .reason,
               ActionSimulator::kReasonNoise);
  EXPECT_TRUE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 0.0,
                           1.0, 0.0)
                  .allowed);
}

TEST(M5Simulator, RateBoundary020PerCycle) {
  // (3) 0.20 por ciclo es inclusivo; 0.200001 rechaza.
  ActionSimulator sim;
  Action ok = zeroAction();
  ok[0] = 0.20;
  EXPECT_TRUE(sim.evaluate(DemandingField(), ok, HealthyTwin(0.9f), -3.0, 0.0,
                           0.0, 0.0)
                  .allowed);
  Action bad = zeroAction();
  bad[0] = 0.200001;
  const ActionScore r = sim.evaluate(DemandingField(), bad, HealthyTwin(0.9f),
                                     -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonRate);
  Action huge = zeroAction();
  huge[4] = 1e30;  // (2) delta absurdo: rate, no crash, J finita
  const ActionScore r2 = sim.evaluate(DemandingField(), huge, HealthyTwin(0.9f),
                                      -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r2.allowed);
  EXPECT_TRUE(std::isfinite(r2.J));
  EXPECT_STREQ(r2.reason, ActionSimulator::kReasonRate);
}

TEST(M5Simulator, RoomCutOnlyPositiveSalaVivaRejectsCutAllowed) {
  ActionSimulator sim;
  Action boost = zeroAction();
  boost[static_cast<std::size_t>(Operator::kSalaViva)] = 0.1;
  const ActionScore r = sim.evaluate(DemandingField(), boost, HealthyTwin(0.9f),
                                     -3.0, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonRoomGain);
  Action cut = zeroAction();
  cut[static_cast<std::size_t>(Operator::kSalaViva)] = -0.2;
  EXPECT_TRUE(sim.evaluate(DemandingField(), cut, HealthyTwin(0.9f), -3.0, 0.0,
                           0.0, 0.0)
                  .allowed);
  // Un boost diminuto también es ganancia neta > 0 dB: se rechaza.
  Action tiny = zeroAction();
  tiny[static_cast<std::size_t>(Operator::kSalaViva)] = 1e-9;
  EXPECT_FALSE(sim.evaluate(DemandingField(), tiny, HealthyTwin(0.9f), -3.0,
                            0.0, 0.0, 0.0)
                   .allowed);
}

TEST(M5Simulator, LatencyBoundary) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  EXPECT_FALSE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 0.0,
                            0.0, 1.0001)
                   .allowed);
  EXPECT_STREQ(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 0.0,
                            0.0, 1.0001)
                   .reason,
               ActionSimulator::kReasonLatency);
  EXPECT_TRUE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, 0.0,
                           0.0, 1.0)
                  .allowed);
  // Mejoras (incrementos negativos) no penalizan el gate.
  EXPECT_TRUE(sim.evaluate(DemandingField(), a, HealthyTwin(0.9f), -3.0, -5.0,
                           -5.0, -5.0)
                  .allowed);
}

TEST(M5Simulator, PrecedenceTruePeakBeforeRate) {
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.5;  // viola rate Y true peak: manda el primero del plan
  const ActionScore r = sim.evaluate(DemandingField(), a, HealthyTwin(0.9f),
                                     -0.5, 0.0, 0.0, 0.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_STREQ(r.reason, ActionSimulator::kReasonTruePeak);
}

TEST(M5Simulator, FiniteButAbsurdFieldPassesGateWithFiniteJ) {
  // (2) Documentado: M5 solo exige finitud del campo. Un campo finito
  // pero fuera de [0,1] pasa el gate (J finita, aunque absurda); quien
  // lo caza en la cadena es M6 (clipping). Ver EndToEnd.
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  PerceptualField huge = DemandingField();
  for (auto& b : huge.bands) b[Dim::kClarity] = 1e30;
  const ActionScore r =
      sim.evaluate(huge, a, HealthyTwin(0.9f), -3.0, 0.0, 0.0, 0.0);
  EXPECT_TRUE(r.allowed);
  EXPECT_TRUE(std::isfinite(r.J));
}

TEST(M5Simulator, CostIsFiniteForExtremeButFiniteInputs) {
  ActionSimulator sim;
  Action a = zeroAction();
  a.fill(0.2);
  const double j = sim.cost(DemandingField(), a, 1e30, 1e30, 1e30);
  EXPECT_TRUE(std::isfinite(j));
  // El helper de auditoría predictedDeltaP() NO sanea el campo actual:
  // con un campo NaN devuelve NaN (resta predicted - current). No es
  // un defecto de decisión: evaluate() rechaza el campo no finito
  // ANTES de llamar a cost(), así que ese NaN nunca llega a una
  // decisión. Queda registrado aquí para que nadie lo use sin guarda.
  PerceptualField f = DemandingField();
  f.bands[2][Dim::kImpact] = kNan;
  EXPECT_TRUE(std::isnan(sim.predictedDeltaP(f, a)));
}

TEST(M5Simulator, AllHardLimitsViolatedAtOnceStillRejectsWithReason) {
  ActionSimulator sim;
  Action a;
  a.fill(1.0);
  const ActionScore r = sim.evaluate(DemandingField(), a, HealthyTwin(0.9f),
                                     0.0, 100.0, 100.0, 100.0);
  EXPECT_FALSE(r.allowed);
  EXPECT_TRUE(std::isfinite(r.J));
  EXPECT_NE(r.reason, nullptr);
  EXPECT_NE(std::string(r.reason), "");
}

// ── M6: MpcShadow (MPC en sombra + rollback + crossfade) ──────────────────

TEST(M6Shadow, TwinInvalidGivesZeroTwinInvalid) {
  MpcShadow sh;
  TwinConfidence twin = HealthyTwin(0.9f);
  twin.valid = false;
  const MpcProposal p = sh.propose(DemandingField(), twin);
  EXPECT_TRUE(p.isZero);
  EXPECT_TRUE(ActionFiniteAndBounded(p.action, 0.0));
  EXPECT_EQ(p.J, 0.0);
  EXPECT_STREQ(p.reason, "twin-invalid");
}

TEST(M6Shadow, TwinNanGlobalGivesZeroTwinNan) {
  // (6) confianza NaN: cero con reason "twin-nan" (no "twin-invalid":
  // el chequeo de finitud va después del de validez).
  MpcShadow sh;
  const MpcProposal p = sh.propose(DemandingField(), HealthyTwin(kNanF));
  EXPECT_TRUE(p.isZero);
  EXPECT_EQ(p.J, 0.0);
  EXPECT_STREQ(p.reason, "twin-nan");
}

TEST(M6Shadow, TwinLowConfidenceGivesZero) {
  MpcShadow sh;
  EXPECT_STREQ(sh.propose(DemandingField(), HealthyTwin(0.19f)).reason,
               "twin-low-confidence");
  EXPECT_STREQ(sh.propose(DemandingField(), HealthyTwin(0.0f)).reason,
               "twin-low-confidence");
  // 0.20 es el umbral inclusivo: ya no es "low-confidence".
  EXPECT_STRNE(sh.propose(DemandingField(), HealthyTwin(0.20f)).reason,
               "twin-low-confidence");
}

TEST(M6Shadow, NanFieldGivesZeroNanField) {
  // (1) campo NaN: cero, y la acción previa se resetea (el ciclo
  // siguiente no hereda una acción de un estado inválido).
  MpcShadow sh;
  PerceptualField f = DemandingField();
  f.bands[4][Dim::kFatigue] = kNan;
  const MpcProposal p = sh.propose(f, HealthyTwin(0.9f));
  EXPECT_TRUE(p.isZero);
  EXPECT_EQ(p.J, 0.0);
  EXPECT_STREQ(p.reason, "nan-field");
  PerceptualField inf = DemandingField();
  inf.bands[0][Dim::kClarity] = kInf;
  EXPECT_STREQ(sh.propose(inf, HealthyTwin(0.9f)).reason, "nan-field");
}

TEST(M6Shadow, ClippingAudibilityAtRailGivesZero) {
  // (2) audibilidad clavada en 1.0 en cualquier banda = medidor en el
  // riel -> cero por clipping.
  MpcShadow sh;
  PerceptualField f = DemandingField();
  f.bands[7][Dim::kAudibility] = 1.0;
  const MpcProposal p = sh.propose(f, HealthyTwin(0.9f));
  EXPECT_TRUE(p.isZero);
  EXPECT_STREQ(p.reason, "clipping");
  PerceptualField over = DemandingField();
  over.bands[0][Dim::kAudibility] = 1e30;  // también >= 1.0
  EXPECT_STREQ(sh.propose(over, HealthyTwin(0.9f)).reason, "clipping");
}

TEST(M6Shadow, ClippingNoPowerHeadroomGivesZero) {
  MpcShadow sh;
  TwinConfidence twin = HealthyTwin(0.9f);
  twin.powerHeadroom.valid = true;
  twin.powerHeadroom.estimate = 0.0f;
  twin.powerHeadroom.confidence = 0.9f;
  EXPECT_STREQ(sh.propose(DemandingField(), twin).reason, "clipping");
  twin.powerHeadroom.estimate = -5.0f;
  EXPECT_STREQ(sh.propose(DemandingField(), twin).reason, "clipping");
  twin.powerHeadroom.estimate = 0.001f;  // margen positivo: ya no es clipping
  EXPECT_STRNE(sh.propose(DemandingField(), twin).reason, "clipping");
}

TEST(M6Shadow, BudgetExceededWhenNoCandidateFeasible) {
  MpcShadow sh;
  PerceptualField f = DemandingField();
  for (auto& b : f.bands) b[Dim::kBudget] = 0.99;
  const MpcProposal p = sh.propose(f, HealthyTwin(0.9f));
  EXPECT_TRUE(p.isZero);
  EXPECT_STREQ(p.reason, "budget-exceeded");
}

TEST(M6Shadow, DemandingFieldProposesBoundedFiniteAction) {
  MpcShadow sh;
  const MpcProposal p = sh.propose(DemandingField(), HealthyTwin(0.9f));
  EXPECT_STREQ(p.reason, "ok");
  EXPECT_TRUE(std::isfinite(p.J));
  EXPECT_TRUE(ActionFiniteAndBounded(p.action, 0.20));
  EXPECT_EQ(p.isZero, actionL1(p.action) == 0.0);
  EXPECT_GE(p.J, 0.0);  // el argmax incluye la acción cero (J=0)
}

TEST(M6Shadow, RapidFieldFlipsStayRateLimited) {
  // (3) campos que alternan entre extremos en cada ciclo: la acción
  // propuesta nunca salta más de 0.20 por operador entre ciclos.
  MpcShadow sh;
  Action prev = zeroAction();
  for (int t = 0; t < 40; ++t) {
    PerceptualField f = DemandingField();
    const double v = (t % 2 == 0) ? 0.0 : 1.0;
    for (auto& b : f.bands) {
      b[Dim::kClarity] = v;
      b[Dim::kImpact] = v;
    }
    const MpcProposal p = sh.propose(f, HealthyTwin(0.9f));
    ASSERT_TRUE(p.isZero || std::string(p.reason) == "ok")
        << "ciclo " << t << " reason=" << p.reason;
    for (std::size_t i = 0; i < kNumOperators; ++i) {
      EXPECT_LE(std::abs(p.action[i] - prev[i]), 0.20 + 1e-9)
          << "ciclo " << t << " operador " << i;
      EXPECT_TRUE(std::isfinite(p.action[i]));
    }
    EXPECT_TRUE(std::isfinite(p.J));
    prev = p.action;
  }
}

TEST(M6Shadow, RollbackAfterTwoContradictionsThenConsumedOnce) {
  MpcShadow sh;
  EXPECT_FALSE(sh.rollbackPending());
  sh.recordOutcome(true, false);
  EXPECT_FALSE(sh.rollbackPending());
  EXPECT_EQ(sh.consecutiveContradictions(), 1u);
  sh.recordOutcome(true, false);
  EXPECT_TRUE(sh.rollbackPending());
  EXPECT_EQ(sh.contradictionsTotal(), 2u);
  const MpcProposal p = sh.propose(DemandingField(), HealthyTwin(0.9f));
  EXPECT_TRUE(p.isZero);
  EXPECT_STREQ(p.reason, "rollback");
  EXPECT_EQ(sh.rollbacksTotal(), 1u);
  EXPECT_FALSE(sh.rollbackPending());  // consumido: el ciclo siguiente decide
  const MpcProposal p2 = sh.propose(DemandingField(), HealthyTwin(0.9f));
  EXPECT_STRNE(p2.reason, "rollback");
}

TEST(M6Shadow, GoodBlockResetsContradictionStreak) {
  MpcShadow sh;
  sh.recordOutcome(true, false);
  EXPECT_EQ(sh.consecutiveContradictions(), 1u);
  sh.recordOutcome(true, true);  // la medición confirmó: no contradice
  EXPECT_EQ(sh.consecutiveContradictions(), 0u);
  sh.recordOutcome(true, false);
  EXPECT_FALSE(sh.rollbackPending());
  sh.recordOutcome(false, false);  // no predijo mejora: no contradice
  EXPECT_EQ(sh.consecutiveContradictions(), 0u);
  EXPECT_FALSE(sh.rollbackPending());
}

TEST(M6Shadow, ResetClearsAllShadowAccounting) {
  MpcShadow sh;
  sh.recordOutcome(true, false);
  sh.recordOutcome(true, false);
  (void)sh.propose(DemandingField(), HealthyTwin(0.9f));
  EXPECT_GT(sh.proposalsTotal(), 0u);
  EXPECT_EQ(sh.rollbacksTotal(), 1u);
  sh.reset();
  EXPECT_EQ(sh.proposalsTotal(), 0u);
  EXPECT_EQ(sh.contradictionsTotal(), 0u);
  EXPECT_EQ(sh.rollbacksTotal(), 0u);
  EXPECT_FALSE(sh.rollbackPending());
}

TEST(M6Shadow, ProposeDoesNotModifyInputs) {
  MpcShadow sh;
  const PerceptualField f = DemandingField();
  const TwinConfidence t = HealthyTwin(0.9f);
  (void)sh.propose(f, t);
  const PerceptualField f2 = DemandingField();
  for (std::size_t b = 0; b < kNumBands; ++b) {
    for (std::size_t d = 0; d < kNumDims; ++d) {
      EXPECT_EQ(f.bands[b].v[d], f2.bands[b].v[d]);
    }
  }
  EXPECT_EQ(t.global, 0.9f);
  EXPECT_TRUE(t.valid);
}

TEST(M6Shadow, DegenerateConfigIsSanitized) {
  MpcShadowConfig bad;
  bad.horizon = 0;       // -> 1
  bad.stepMs = kNan;     // -> 50
  bad.numCandidates = 0; // -> 1
  bad.maxDeltaPerCycle = kNan;  // -> 0.20
  bad.minTwinConfidence = -1.0; // -> 0.20
  MpcShadow sh(bad);
  EXPECT_DOUBLE_EQ(sh.horizonMs(), 50.0);
  const MpcProposal p = sh.propose(DemandingField(), HealthyTwin(0.9f));
  EXPECT_TRUE(ActionFiniteAndBounded(p.action, 0.20));
  MpcShadowConfig huge;
  huge.horizon = 100;  // tope interno 16 -> 800 ms
  EXPECT_DOUBLE_EQ(MpcShadow(huge).horizonMs(), 800.0);
  MpcShadowConfig dflt;
  EXPECT_DOUBLE_EQ(MpcShadow(dflt).horizonMs(), 250.0);  // 5 x 50 ms
}

TEST(M6Shadow, CrossfadeNanTIsNoChange) {
  const CrossfadeGains g = crossfadeEqualPowerGains(kNan);
  EXPECT_DOUBLE_EQ(g.gainA, 1.0);
  EXPECT_DOUBLE_EQ(g.gainB, 0.0);
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(0.3, 0.9, kNan), 0.3);
  // +Inf también es no finito: sin cambio (toda la ganancia en `a`).
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(0.3, 0.9, kInf), 0.3);
  EXPECT_DOUBLE_EQ(crossfadeEqualPower(0.3, 0.9, -kInf), 0.3);
}

TEST(M6Shadow, CrossfadeBoundsAndEqualPower) {
  // cos(pi/2) en coma flotante es ~6.1e-17, no 0.0 exacto: comparar
  // con tolerancia, no con igualdad exacta.
  EXPECT_NEAR(crossfadeEqualPowerGains(-5.0).gainA, 1.0, 1e-12);
  EXPECT_NEAR(crossfadeEqualPowerGains(-5.0).gainB, 0.0, 1e-12);
  EXPECT_NEAR(crossfadeEqualPowerGains(5.0).gainA, 0.0, 1e-12);
  EXPECT_NEAR(crossfadeEqualPowerGains(5.0).gainB, 1.0, 1e-12);
  for (double t : {0.0, 0.25, 0.5, 0.75, 1.0}) {
    const CrossfadeGains g = crossfadeEqualPowerGains(t);
    EXPECT_NEAR(g.gainA * g.gainA + g.gainB * g.gainB, 1.0, 1e-12)
        << "t=" << t;
    EXPECT_GE(g.gainA, 0.0);
    EXPECT_GE(g.gainB, 0.0);
  }
  const CrossfadeGains mid = crossfadeEqualPowerGains(0.5);
  EXPECT_NEAR(mid.gainA, std::cos(3.14159265358979323846 / 4.0), 1e-12);
  EXPECT_NEAR(mid.gainB, std::sin(3.14159265358979323846 / 4.0), 1e-12);
}

TEST(M6Shadow, CrossfadeNanOperandPropagatesNan) {
  // Documentado: el helper solo sanea t; con a/b NaN el resultado es
  // NaN. El llamador debe sanear las ganancias antes de mezclar.
  EXPECT_TRUE(std::isnan(crossfadeEqualPower(kNan, 1.0, 0.5)));
  EXPECT_TRUE(std::isnan(crossfadeEqualPower(1.0, kNan, 0.5)));
}

// ── M8: UncertaintySupervisor (árbitro único + rollback) ───────────────────

TEST(M8Supervisor, NanGlobalIsNone) {
  UncertaintySupervisor sup;
  const Supervision s = sup.supervise(HealthyTwin(kNanF), HealthyState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_FALSE(s.allowControl);
  EXPECT_EQ(s.maxIntensity, 0.0f);
  EXPECT_NE(s.reason, nullptr);
}

TEST(M8Supervisor, GlobalOutOfRangeIsNone) {
  UncertaintySupervisor sup;
  EXPECT_EQ(sup.supervise(HealthyTwin(1.5f), HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  EXPECT_EQ(sup.supervise(HealthyTwin(-0.5f), HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  EXPECT_EQ(sup.supervise(HealthyTwin(kInfF), HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
}

TEST(M8Supervisor, NanDivergenceAndNegativeDivergenceAreNone) {
  // (5) divergencia NaN o negativa: fail-closed, no se controla.
  UncertaintySupervisor sup;
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), HealthyState(), kNanF).level,
            InterventionLevel::kNone);
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), HealthyState(), -0.1f).level,
            InterventionLevel::kNone);
  // Divergencia enorme pero finita: también acción cero (extrema).
  const Supervision s =
      sup.supervise(HealthyTwin(0.9f), HealthyState(), 1e30f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_EQ(s.maxIntensity, 0.0f);
}

TEST(M8Supervisor, InvalidTwinOrStateIsNone) {
  UncertaintySupervisor sup;
  TwinConfidence bad = HealthyTwin(0.9f);
  bad.valid = false;
  EXPECT_EQ(sup.supervise(bad, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  AudioState badState = HealthyState();
  badState.valid = false;
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), badState, 0.0f).level,
            InterventionLevel::kNone);
}

TEST(M8Supervisor, UnknownRouteIsNone) {
  UncertaintySupervisor sup;
  AudioState s = HealthyState();
  s.routeId = 0;
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), s, 0.0f).level,
            InterventionLevel::kNone);
}

TEST(M8Supervisor, RouteMismatchIsNone) {
  // (4) la calibración de una ruta nunca se reutiliza en otra.
  UncertaintySupervisor sup;
  const Supervision s =
      sup.supervise(HealthyTwin(0.9f), HealthyState(), 0.0f, 999u);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_FALSE(s.allowControl);
  // Misma ruta que la calibración: pasa el chequeo de ruta.
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), HealthyState(), 0.0f, 7u).level,
            InterventionLevel::kControl);
  // calibrationRouteId 0 = "sin dato de ruta": no hay mismatch posible.
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), HealthyState(), 0.0f, 0u).level,
            InterventionLevel::kControl);
}

TEST(M8Supervisor, StaleParameterExpiresCalibration) {
  // (4) cualquier parámetro válido con age > 300 s expira el gemelo.
  UncertaintySupervisor sup;
  TwinConfidence t = HealthyTwin(0.9f);
  t.h1[3].valid = true;
  t.h1[3].age = 301.0f;
  t.h1[3].confidence = 0.9f;
  t.h1[3].variance = 0.0f;
  t.h1[3].estimate = 0.0f;
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  // Age exactamente 300 s: todavía vigente (el tope es > 300).
  t.h1[3].age = 300.0f;
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kControl);
  // Age enorme (1e30 s): expirado, sin overflow ni crash.
  t.h1[3].age = 1e30f;
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  // Un parámetro INVÁLIDO con age enorme no expira nada: es "sin dato".
  TwinConfidence u = HealthyTwin(0.9f);
  u.h2.valid = false;
  u.h2.age = 1e30f;
  EXPECT_EQ(sup.supervise(u, HealthyState(), 0.0f).level,
            InterventionLevel::kControl);
}

TEST(M8Supervisor, InsaneParameterIsNone) {
  UncertaintySupervisor sup;
  TwinConfidence t = HealthyTwin(0.9f);
  t.latency.valid = true;
  t.latency.variance = -1.0f;  // varianza negativa: imposible
  t.latency.confidence = 0.9f;
  t.latency.age = 0.0f;
  t.latency.estimate = 1.0f;
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  t.latency.variance = 0.0f;
  t.latency.confidence = kNanF;
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  t.latency.confidence = 0.9f;
  t.latency.estimate = kInfF;
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
  t.latency.estimate = 1.0f;
  t.latency.confidence = 1.5f;  // confianza fuera de [0,1]
  EXPECT_EQ(sup.supervise(t, HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
}

TEST(M8Supervisor, ConfidenceBandsAndMaxIntensity) {
  // (6) 0.85 -> control; 0.70 -> solo limita (intensidad = confianza);
  // 0.30 -> solo telemetría; 0.19 -> acción cero.
  UncertaintySupervisor sup;
  const Supervision hi = sup.supervise(HealthyTwin(0.85f), HealthyState(), 0.0f);
  EXPECT_EQ(hi.level, InterventionLevel::kControl);
  EXPECT_TRUE(hi.allowControl);
  EXPECT_EQ(hi.maxIntensity, 1.0f);
  const Supervision mid = sup.supervise(HealthyTwin(0.70f), HealthyState(), 0.0f);
  EXPECT_EQ(mid.level, InterventionLevel::kLimitOnly);
  EXPECT_FALSE(mid.allowControl);
  EXPECT_EQ(mid.maxIntensity, 0.70f);
  const Supervision lo = sup.supervise(HealthyTwin(0.30f), HealthyState(), 0.0f);
  EXPECT_EQ(lo.level, InterventionLevel::kNone);
  EXPECT_FALSE(lo.allowControl);
  EXPECT_EQ(lo.maxIntensity, 0.0f);
  const Supervision zero = sup.supervise(HealthyTwin(0.19f), HealthyState(), 0.0f);
  EXPECT_EQ(zero.level, InterventionLevel::kNone);
  EXPECT_EQ(zero.maxIntensity, 0.0f);
  // Fronteras inclusivas: 0.80 controla, 0.50 limita, 0.20 telemetría.
  EXPECT_EQ(sup.supervise(HealthyTwin(0.80f), HealthyState(), 0.0f).level,
            InterventionLevel::kControl);
  EXPECT_EQ(sup.supervise(HealthyTwin(0.50f), HealthyState(), 0.0f).level,
            InterventionLevel::kLimitOnly);
  EXPECT_EQ(sup.supervise(HealthyTwin(0.20f), HealthyState(), 0.0f).level,
            InterventionLevel::kNone);
}

TEST(M8Supervisor, DivergenceDemotesOneLevel) {
  // (5) divergencia 0.26 (> 0.25): control -> solo limita, con la
  // confianza como intensidad máxima; 0.25 exacto no demueve.
  UncertaintySupervisor sup;
  const Supervision d = sup.supervise(HealthyTwin(0.9f), HealthyState(), 0.26f);
  EXPECT_EQ(d.level, InterventionLevel::kLimitOnly);
  EXPECT_FALSE(d.allowControl);
  EXPECT_EQ(d.maxIntensity, 0.9f);
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), HealthyState(), 0.25f).level,
            InterventionLevel::kControl);
  // Desde kLimitOnly, la divergencia baja a kNone.
  const Supervision d2 = sup.supervise(HealthyTwin(0.7f), HealthyState(), 0.5f);
  EXPECT_EQ(d2.level, InterventionLevel::kNone);
  EXPECT_EQ(d2.maxIntensity, 0.0f);
}

TEST(M8Supervisor, ExtremeDivergenceIsZeroActionEvenWithHighConfidence) {
  UncertaintySupervisor sup;
  const Supervision s = sup.supervise(HealthyTwin(0.95f), HealthyState(), 0.75f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_FALSE(s.allowControl);
  EXPECT_EQ(s.maxIntensity, 0.0f);
  EXPECT_EQ(sup.supervise(HealthyTwin(0.95f), HealthyState(), 0.7499f).level,
            InterventionLevel::kLimitOnly);
}

TEST(M8Supervisor, StateConfidenceOutOfRangeIsNone) {
  UncertaintySupervisor sup;
  AudioState s = HealthyState();
  s.confidence = 1.5f;
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), s, 0.0f).level,
            InterventionLevel::kNone);
  s.confidence = kNanF;
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), s, 0.0f).level,
            InterventionLevel::kNone);
  s.confidence = -0.1f;
  EXPECT_EQ(sup.supervise(HealthyTwin(0.9f), s, 0.0f).level,
            InterventionLevel::kNone);
}

TEST(M8Supervisor, RollbackNeedsTwoConsecutiveAnomalies) {
  UncertaintySupervisor sup;
  EXPECT_FALSE(sup.rollbackNeeded());
  sup.recordAnomaly();
  EXPECT_FALSE(sup.rollbackNeeded());
  EXPECT_EQ(sup.consecutiveAnomalies(), 1);
  sup.recordAnomaly();
  EXPECT_TRUE(sup.rollbackNeeded());
  EXPECT_TRUE(sup.rollbackNeeded());  // sigue true hasta resetRollback()
  sup.resetRollback();
  EXPECT_FALSE(sup.rollbackNeeded());
  EXPECT_EQ(sup.consecutiveAnomalies(), 0);
}

TEST(M8Supervisor, GoodBlockBreaksAnomalyStreak) {
  UncertaintySupervisor sup;
  sup.recordAnomaly();
  sup.recordGoodBlock();
  EXPECT_EQ(sup.consecutiveAnomalies(), 0);
  EXPECT_FALSE(sup.rollbackNeeded());
  sup.recordAnomaly();
  EXPECT_FALSE(sup.rollbackNeeded());
}

TEST(M8Supervisor, SuperviseIsPureAndRepeatable) {
  // supervise() es const y pura: dos llamadas idénticas dan el mismo
  // veredicto y no tocan el contador de rollback.
  UncertaintySupervisor sup;
  sup.recordAnomaly();
  const TwinConfidence t = HealthyTwin(0.85f);
  const AudioState s = HealthyState();
  const Supervision a = sup.supervise(t, s, 0.0f);
  const Supervision b = sup.supervise(t, s, 0.0f);
  EXPECT_EQ(a.level, b.level);
  EXPECT_EQ(a.maxIntensity, b.maxIntensity);
  EXPECT_STREQ(a.reason, b.reason);
  EXPECT_EQ(sup.consecutiveAnomalies(), 1);
}

// ── M3: TwinEstimator (gemelo -> confianza por variable) ───────────────────

TEST(M3Estimator, CalibratedP3GivesHonest070LimitOnlyNoFineControl) {
  TwinEstimator est;
  const TwinConfidence c = est.estimate(CalibratedTwinEstimate(),
                                        TwinCalibration::geneziPz1dEstimated());
  EXPECT_TRUE(c.valid);
  EXPECT_FLOAT_EQ(c.global, 0.70f);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.70f);
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kLimitOnly);
  // La calibración estimada NUNCA decide control fino.
  EXPECT_FALSE(est.canControlFine());
  EXPECT_FALSE(TwinEstimator::canControlFine(c));
  // h2/h3 sin dato (el barrido de respuesta no identifica Volterra).
  EXPECT_FALSE(c.h2.valid);
  EXPECT_FALSE(c.h3.valid);
  EXPECT_EQ(c.h2.method, ConfidenceMethod::kUnknown);
  // Todas las confianzas finitas en [0,1], varianzas >= 0.
  for (const auto& h : c.h1) {
    EXPECT_TRUE(std::isfinite(h.confidence));
    EXPECT_GE(h.confidence, 0.0f);
    EXPECT_LE(h.confidence, 1.0f);
    EXPECT_GE(h.variance, 0.0f);
    EXPECT_TRUE(std::isfinite(h.estimate));
    EXPECT_EQ(h.age, 0.0f);
  }
}

TEST(M3Estimator, UncalibratedEsseHasNoPowerHeadroomAndLowGlobal) {
  TwinEstimator est;
  const TwinConfidence c =
      est.estimate(AcousticTwin::estimate(AcousticTwinParams::esseGenezi()));
  EXPECT_TRUE(c.valid);
  EXPECT_FALSE(c.powerHeadroom.valid);  // potencia/sensibilidad desconocidas
  EXPECT_LT(c.global, 0.50f);          // sin potencia no gobierna nada
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kNone);
  EXPECT_FALSE(est.canControlFine());
}

TEST(M3Estimator, InvalidTwinEstimateGivesInvalidConfidence) {
  TwinEstimator est;
  AcousticTwinEstimate bad;
  bad.valid = false;
  const TwinConfidence c = est.estimate(bad);
  EXPECT_FALSE(c.valid);
  EXPECT_EQ(c.global, 0.0f);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  EXPECT_FALSE(est.canControlFine());
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kNone);
  bool allFinite = std::isfinite(c.global);
  for (const auto& h : c.h1) allFinite &= std::isfinite(h.confidence);
  EXPECT_TRUE(allFinite);
}

TEST(M3Estimator, NonFiniteTwinEstimateIsContained) {
  // (1)+(2) EL DEFECTO DEL GEMELO CONTENIDO: estimate() con sala 1e200
  // devuelve valid=true con rt60Sec NaN (ver KnownDefect_Twin*); el
  // estimador M3 lo detecta y devuelve confianza INVÁLIDA, finita.
  TwinEstimator est;
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.roomWidthM = 1e200;
  p.roomLengthM = 1e200;
  p.roomHeightM = 1e200;
  const AcousticTwinEstimate e = AcousticTwin::estimate(p);
  ASSERT_TRUE(e.valid);  // el gemelo cree que estimó (defecto pinneado abajo)
  const TwinConfidence c = est.estimate(e);
  EXPECT_FALSE(c.valid);
  EXPECT_EQ(c.global, 0.0f);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  EXPECT_FALSE(est.canControlFine());
}

TEST(M3Estimator, CalibrationMismatchIsRejected) {
  TwinEstimator est;
  TwinCalibration other = TwinCalibration::geneziPz1dEstimated();
  other.honestConfidence = 0.71;  // misma forma, otra confianza honesta
  const TwinConfidence c = est.estimate(CalibratedTwinEstimate(), other);
  EXPECT_FALSE(c.valid);
  EXPECT_EQ(c.global, 0.0f);
  TwinCalibration notValid = TwinCalibration::geneziPz1dEstimated();
  notValid.valid = false;
  EXPECT_FALSE(est.estimate(CalibratedTwinEstimate(), notValid).valid);
  // Y una calibración NaN tampoco pasa.
  TwinCalibration nanCal = TwinCalibration::geneziPz1dEstimated();
  nanCal.honestConfidence = static_cast<double>(kNanF);
  EXPECT_FALSE(nanCal.isValid());
  EXPECT_FALSE(est.estimate(CalibratedTwinEstimate(), nanCal).valid);
}

TEST(M3Estimator, MeasuredValuesHighConfidenceAllowFineControl) {
  TwinEstimator est;
  TwinMeasuredValues m;
  m.valid = true;
  m.h1Db.fill(0.0f);
  m.h1Confidence.fill(0.9f);
  m.h2Confidence = 0.9f;
  m.h3Confidence = 0.9f;
  m.latencyMs = 10.0f;
  m.latencyConfidence = 0.9f;
  m.roomRt60Sec = 0.34f;
  m.roomConfidence = 0.9f;
  m.coherence = 0.8f;
  m.coherenceConfidence = 0.9f;
  m.noiseFloorDbSpl = 30.0f;
  m.noiseConfidence = 0.9f;
  m.powerHeadroomDbSpl = 100.0f;
  m.powerConfidence = 0.9f;
  const TwinConfidence c = est.estimateMeasured(
      AcousticTwin::estimate(AcousticTwinParams::esseGenezi()), m);
  EXPECT_TRUE(c.valid);
  EXPECT_FLOAT_EQ(c.global, 0.9f);
  EXPECT_TRUE(est.canControlFine());
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kControl);
  for (const auto& h : c.h1) EXPECT_EQ(h.method, ConfidenceMethod::kMeasured);
  EXPECT_EQ(c.h2.method, ConfidenceMethod::kMeasured);
}

TEST(M3Estimator, MeasuredValuesInvalidRejects) {
  TwinEstimator est;
  const AcousticTwinEstimate e =
      AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
  TwinMeasuredValues m;
  m.valid = true;
  m.h1Db.fill(0.0f);
  m.h1Confidence.fill(0.9f);
  m.latencyMs = 10.0f;
  m.latencyConfidence = 0.9f;
  m.roomRt60Sec = 0.34f;
  m.roomConfidence = 0.9f;
  m.coherence = 0.8f;
  m.coherenceConfidence = 0.9f;
  m.noiseFloorDbSpl = 30.0f;
  m.noiseConfidence = 0.9f;
  m.powerHeadroomDbSpl = 100.0f;
  m.powerConfidence = 0.9f;
  m.h1Confidence[0] = kNanF;  // (1) un solo NaN invalida toda la medición
  EXPECT_FALSE(m.isValid());
  EXPECT_FALSE(est.estimateMeasured(e, m).valid);
  m.h1Confidence[0] = 0.9f;
  m.h1Db[5] = 1e30f;  // (2) fuera de rango cuerdo (±200 dB)
  EXPECT_FALSE(m.isValid());
  EXPECT_FALSE(est.estimateMeasured(e, m).valid);
  m.h1Db[5] = 0.0f;
  m.valid = false;  // sin flag valid, ni se mira el resto
  EXPECT_FALSE(m.isValid());
  EXPECT_FALSE(est.estimateMeasured(e, m).valid);
}

TEST(M3Estimator, StaticHelpersOnHandBuiltConfidence) {
  EXPECT_FLOAT_EQ(TwinEstimator::maxConservationIntensity(HealthyTwin(0.9f)),
                  0.9f);
  EXPECT_FLOAT_EQ(TwinEstimator::maxConservationIntensity(HealthyTwin(0.19f)),
                  0.0f);
  EXPECT_FLOAT_EQ(TwinEstimator::maxConservationIntensity(HealthyTwin(kNanF)),
                  0.0f);
  TwinConfidence over = HealthyTwin(1.5f);  // fuera de rango: se clampa a 1
  EXPECT_FLOAT_EQ(TwinEstimator::maxConservationIntensity(over), 1.0f);
  TwinConfidence inv = HealthyTwin(0.9f);
  inv.valid = false;
  EXPECT_FLOAT_EQ(TwinEstimator::maxConservationIntensity(inv), 0.0f);
  EXPECT_EQ(TwinEstimator::interventionLevel(HealthyTwin(0.85f)),
            InterventionLevel::kControl);
  EXPECT_EQ(TwinEstimator::interventionLevel(HealthyTwin(0.70f)),
            InterventionLevel::kLimitOnly);
  EXPECT_EQ(TwinEstimator::interventionLevel(HealthyTwin(0.30f)),
            InterventionLevel::kNone);
  EXPECT_EQ(TwinEstimator::interventionLevel(HealthyTwin(kNanF)),
            InterventionLevel::kNone);
  EXPECT_EQ(TwinEstimator::interventionLevel(inv), InterventionLevel::kNone);
}

TEST(M3Estimator, CanControlFineNeedsMeasuredEverywhereAndHighGlobal) {
  // Una sola variable estimada bloquea el control fino aunque el
  // global sea 0.95.
  TwinConfidence c = HealthyTwin(0.95f);
  for (auto& h : c.h1) {
    h.valid = true;
    h.method = ConfidenceMethod::kMeasured;
    h.confidence = 0.95f;
  }
  const ParameterConfidence* rest[7] = {&c.h2, &c.h3, &c.latency,
                                        &c.roomIr, &c.coherence,
                                        &c.noiseFloor, &c.powerHeadroom};
  (void)rest;
  c.h2.valid = true;
  c.h2.method = ConfidenceMethod::kMeasured;
  c.h3.valid = true;
  c.h3.method = ConfidenceMethod::kMeasured;
  c.latency.valid = true;
  c.latency.method = ConfidenceMethod::kMeasured;
  c.roomIr.valid = true;
  c.roomIr.method = ConfidenceMethod::kMeasured;
  c.coherence.valid = true;
  c.coherence.method = ConfidenceMethod::kMeasured;
  c.noiseFloor.valid = true;
  c.noiseFloor.method = ConfidenceMethod::kMeasured;
  c.powerHeadroom.valid = true;
  c.powerHeadroom.method = ConfidenceMethod::kMeasured;
  EXPECT_TRUE(TwinEstimator::canControlFine(c));
  c.h3.method = ConfidenceMethod::kDerived;  // una estimada lo bloquea
  EXPECT_FALSE(TwinEstimator::canControlFine(c));
  c.h3.method = ConfidenceMethod::kMeasured;
  c.global = 0.79f;  // global bajo el umbral 0.80 también lo bloquea
  EXPECT_FALSE(TwinEstimator::canControlFine(c));
}

TEST(M3Estimator, LastConfidenceTracksLatestEstimate) {
  TwinEstimator est;
  EXPECT_FALSE(est.lastConfidence().valid);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  (void)est.estimate(CalibratedTwinEstimate(),
                     TwinCalibration::geneziPz1dEstimated());
  EXPECT_TRUE(est.lastConfidence().valid);
  EXPECT_FLOAT_EQ(est.lastConfidence().global, 0.70f);
  AcousticTwinEstimate bad;
  bad.valid = false;
  (void)est.estimate(bad);
  EXPECT_FALSE(est.lastConfidence().valid);
}

// ── Telemetría (solo registra, nunca decide) ───────────────────────────────

TEST(Telemetry, ValidFrameRoundTripsUnchanged) {
  Telemetry tel;
  TelemetryFrame f;
  f.valid = true;
  f.sampleTime = 48000;
  f.generation = 9;
  f.confidence = 0.7f;
  for (int i = 0; i < 7; ++i) {
    f.chosenAction[i] = 0.1f * static_cast<float>(i);
    f.appliedAction[i] = 0.05f * static_cast<float>(i);
  }
  f.energyBeforeDb = -12.0f;
  f.energyAfterDb = -12.5f;
  f.truePeakDbTp = -1.5f;
  f.thdEstimateDb = -80.0f;
  f.latencyMs = 12.5f;
  f.xruns = 3;
  f.twinDivergence = 0.05f;
  tel.record(f);
  EXPECT_EQ(tel.pending(), 1u);
  TelemetryFrame out[1];
  EXPECT_EQ(tel.drain(out, 1), 1u);
  EXPECT_TRUE(out[0].valid);
  EXPECT_EQ(out[0].generation, 9u);
  EXPECT_FLOAT_EQ(out[0].confidence, 0.7f);
  EXPECT_FLOAT_EQ(out[0].energyAfterDb, -12.5f);
  EXPECT_EQ(out[0].xruns, 3u);
  EXPECT_EQ(tel.pending(), 0u);
}

TEST(Telemetry, NanFrameIsRecordedInvalidNotDroppedSilently) {
  // (1) un frame con NaN NO se descarta: se registra con valid=false
  // para que el fallo quede visible.
  Telemetry tel;
  TelemetryFrame f;
  f.valid = true;
  f.confidence = kNanF;
  tel.record(f);
  EXPECT_EQ(tel.pending(), 1u);
  TelemetryFrame out[1];
  EXPECT_EQ(tel.drain(out, 1), 1u);
  EXPECT_FALSE(out[0].valid);
  TelemetryFrame g;
  g.valid = true;
  g.chosenAction[2] = kInfF;
  tel.record(g);
  EXPECT_EQ(tel.drain(out, 1), 1u);
  EXPECT_FALSE(out[0].valid);
}

TEST(Telemetry, RecordFailureNullAndLongReason) {
  Telemetry tel;
  tel.recordFailure(nullptr);  // -> "unknown", ordinal 0
  FailureRecord out[1];
  EXPECT_EQ(tel.drainFailures(out, 1), 1u);
  EXPECT_STREQ(out[0].reason, "unknown");
  EXPECT_EQ(out[0].ordinal, 0u);
  const std::string longReason(100, 'x');
  tel.recordFailure(longReason.c_str());  // truncado a 63 + NUL
  EXPECT_EQ(tel.drainFailures(out, 1), 1u);
  EXPECT_EQ(std::strlen(out[0].reason), 63u);
  EXPECT_EQ(out[0].reason[63], '\0');
  EXPECT_EQ(out[0].ordinal, 1u);
}

TEST(Telemetry, DrainEdgeCasesDoNotCrash) {
  Telemetry tel;
  TelemetryFrame out[4];
  EXPECT_EQ(tel.drain(nullptr, 4), 0u);
  EXPECT_EQ(tel.drain(out, 0), 0u);
  EXPECT_EQ(tel.drain(out, 4), 0u);  // vacío
  FailureRecord fout[2];
  EXPECT_EQ(tel.drainFailures(nullptr, 2), 0u);
  EXPECT_EQ(tel.drainFailures(fout, 0), 0u);
  EXPECT_EQ(tel.drainFailures(fout, 2), 0u);
}

TEST(Telemetry, FrameOverflowKeepsMostRecentCountsLossOnDrain) {
  // Capacidad 256: 300 frames -> pending 256; al drenar, los 44 más
  // viejos se cuentan como perdidos y se entregan los 256 recientes.
  Telemetry tel;
  for (int i = 0; i < 300; ++i) {
    TelemetryFrame f;
    f.valid = true;
    f.generation = static_cast<uint64_t>(i);
    tel.record(f);
  }
  EXPECT_EQ(tel.pending(), 256u);
  EXPECT_EQ(tel.dropped(), 0u);  // la pérdida se cuenta en el drenado
  TelemetryFrame out[300];
  EXPECT_EQ(tel.drain(out, 300), 256u);
  EXPECT_EQ(out[0].generation, 44u);
  EXPECT_EQ(out[255].generation, 299u);
  EXPECT_EQ(tel.dropped(), 44u);
  EXPECT_EQ(tel.pending(), 0u);
}

TEST(Telemetry, FailureOverflowKeepsMostRecent) {
  Telemetry tel;
  for (int i = 0; i < 40; ++i) tel.recordFailure("f");
  EXPECT_EQ(tel.pendingFailures(), 32u);
  FailureRecord out[40];
  EXPECT_EQ(tel.drainFailures(out, 40), 32u);
  EXPECT_EQ(out[0].ordinal, 8u);
  EXPECT_EQ(out[31].ordinal, 39u);
  EXPECT_EQ(tel.droppedFailures(), 8u);
}

TEST(Telemetry, FailureOrdinalsAreMonotonic) {
  Telemetry tel;
  tel.recordFailure("a");
  tel.recordFailure("b");
  tel.recordFailure("c");
  FailureRecord out[3];
  EXPECT_EQ(tel.drainFailures(out, 3), 3u);
  EXPECT_EQ(out[0].ordinal, 0u);
  EXPECT_EQ(out[1].ordinal, 1u);
  EXPECT_EQ(out[2].ordinal, 2u);
  EXPECT_STREQ(out[0].reason, "a");
  EXPECT_STREQ(out[2].reason, "c");
}

TEST(Telemetry, GenerationGapsAreRecordedAsIsTelemetryDoesNotJudge) {
  // (8) la telemetría registra, no arbitra: generaciones con huecos o
  // que retroceden se guardan tal cual; M0/M8 son quienes las juzgan.
  Telemetry tel;
  for (uint64_t g : {1u, 2u, 5u, 4u, 100u}) {
    TelemetryFrame f;
    f.valid = true;
    f.generation = g;
    tel.record(f);
  }
  TelemetryFrame out[5];
  EXPECT_EQ(tel.drain(out, 5), 5u);
  EXPECT_EQ(out[0].generation, 1u);
  EXPECT_EQ(out[1].generation, 2u);
  EXPECT_EQ(out[2].generation, 5u);
  EXPECT_EQ(out[3].generation, 4u);
  EXPECT_EQ(out[4].generation, 100u);
}

TEST(Telemetry, ExtremeButFiniteValuesStayValid) {
  // (2) valores extremos pero finitos no invalidan el frame.
  Telemetry tel;
  TelemetryFrame f;
  f.valid = true;
  f.confidence = 1e30f;
  f.energyBeforeDb = -1e30f;
  f.truePeakDbTp = 1e30f;
  f.latencyMs = 1e30f;
  f.xruns = UINT32_MAX;
  tel.record(f);
  TelemetryFrame out[1];
  EXPECT_EQ(tel.drain(out, 1), 1u);
  EXPECT_TRUE(out[0].valid);
  EXPECT_EQ(out[0].xruns, UINT32_MAX);
}

// ── Código EXISTENTE que el lazo usa: gemelo ───────────────────────────────

TEST(TwinExisting, NanAndInfParamsGiveInvalidZeroConfidence) {
  // (1) entradas NaN/Inf se sanean a "desconocido" y no se estima nada.
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.ampMaxPowerW = kNan;
  p.sensitivityDb = kInf;
  p.roomRt60Sec = kNan;
  p.listenerDistanceM = -kInf;
  p.targetSplDb = kNan;
  const AcousticTwinEstimate e = AcousticTwin::estimate(p);
  EXPECT_FALSE(e.valid);
  EXPECT_EQ(e.overallConfidence, 0.0);
  EXPECT_EQ(e.overallInterventionScale, 0.0);
}

TEST(TwinExisting, ExtremeButFiniteParamsStayFiniteOrInvalid) {
  // (2) +-1e30 en potencia, sensibilidad, SPL y sala: o estimación
  // finita, o inválido. Nunca NaN, nunca crash.
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.ampMaxPowerW = 1e30;
  p.sensitivityDb = 1e30;
  AcousticTwinEstimate e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(EstimateAllFinite(e));
  p = AcousticTwinParams::esseGenezi();
  p.targetSplDb = 1e30;
  e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(EstimateAllFinite(e));
  p = AcousticTwinParams::esseGenezi();
  p.targetSplDb = -1e30;
  e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(EstimateAllFinite(e));
  p = AcousticTwinParams::esseGenezi();
  p.roomWidthM = 1e30;
  p.roomLengthM = 1e30;
  p.roomHeightM = 1e30;
  e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(EstimateAllFinite(e));
  p = AcousticTwinParams::esseGenezi();
  p.listenerDistanceM = 1e305;  // 1e308 desborda la latencia (ver pin abajo)
  e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(EstimateAllFinite(e));
  p = AcousticTwinParams::esseGenezi();
  p.dacBufferFrames = 2147483647;
  e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(std::isfinite(e.bufferLatencyMs));
}

TEST(TwinExisting, KnownDefect_Distance1e308NonFiniteLatency) {
  // DEFECTO REAL (pinneado, misma clase que la sala 1e200): con
  // distancia de escucha 1e308 m, la latencia por aire (d/343*1000)
  // desborda double a +inf y estimate() devuelve valid=true con
  // airLatencyMs y latencia por banda en +inf. A 1e305 m todo es
  // finito (ver test anterior). AL CORREGIRLO (acotar la distancia /
  // la latencia), este test debe INVERTIRSE a EstimateAllFinite(e).
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.listenerDistanceM = 1e308;
  const AcousticTwinEstimate e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_FALSE(EstimateAllFinite(e));
  EXPECT_TRUE(std::isinf(e.airLatencyMs));
  EXPECT_TRUE(std::isinf(e.band[0].latencyMs));
}

TEST(TwinExisting, KnownDefect_Room1e200NonFiniteRt60) {
  // DEFECTO REAL (pinneado): con dimensiones de sala ~1e200 m el
  // volumen desborda a inf y la Sabine hace inf/inf = NaN. estimate()
  // devuelve valid=true con rt60Sec NaN, roomVolumeM3 inf y
  // criticalDistanceM inf, contra su promesa de "jamás NaN en la
  // salida". M3 lo contiene (ver M3Estimator.NonFiniteTwinEstimateIsContained).
  // AL CORREGIRLO (sanear el volumen / acotar las dimensiones), este
  // test debe INVERTIRSE a: valid==false o EstimateAllFinite(e).
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.roomWidthM = 1e200;
  p.roomLengthM = 1e200;
  p.roomHeightM = 1e200;
  const AcousticTwinEstimate e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_FALSE(EstimateAllFinite(e));
  EXPECT_TRUE(std::isnan(e.band[0].rt60Sec));
  EXPECT_TRUE(std::isinf(e.roomVolumeM3));
  EXPECT_TRUE(std::isinf(e.criticalDistanceM));
}

TEST(TwinExisting, ImpossibleDacOrRoomIsInvalid) {
  AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  p.dacBits = 0;
  EXPECT_FALSE(AcousticTwin::estimate(p).valid);
  p = AcousticTwinParams::esseGenezi();
  p.dacSampleRateHz = -48000;
  EXPECT_FALSE(AcousticTwin::estimate(p).valid);
  p = AcousticTwinParams::esseGenezi();
  p.roomWidthM = 0.0;
  EXPECT_FALSE(AcousticTwin::estimate(p).valid);
  p = AcousticTwinParams::esseGenezi();
  p.roomRt60Sec = 0.0;
  EXPECT_FALSE(AcousticTwin::estimate(p).valid);
  p = AcousticTwinParams::esseGenezi();
  p.listenerDistanceM = -3.0;
  EXPECT_FALSE(AcousticTwin::estimate(p).valid);
}

TEST(TwinExisting, NanCalibrationFallsBackUncalibrated) {
  const AcousticTwinParams p = AcousticTwinParams::esseGeneziCalibrated();
  TwinCalibration cal = TwinCalibration::geneziPz1dEstimated();
  cal.honestConfidence = kNan;
  EXPECT_FALSE(cal.isValid());
  const AcousticTwinEstimate e = AcousticTwin::estimateCalibrated(p, cal);
  EXPECT_TRUE(e.valid);
  EXPECT_FALSE(e.calibrationApplied);
  TwinCalibration cal2 = TwinCalibration::geneziPz1dEstimated();
  cal2.responseDb[0] = kNan;
  EXPECT_FALSE(cal2.isValid());
  EXPECT_FALSE(AcousticTwin::estimateCalibrated(p, cal2).calibrationApplied);
}

TEST(TwinExisting, HeadphonesIgnoreNanRoom) {
  AcousticTwinParams p = AcousticTwinParams::esseHeadphones();
  p.roomWidthM = kNan;
  p.roomRt60Sec = -5.0;
  const AcousticTwinEstimate e = AcousticTwin::estimate(p);
  EXPECT_TRUE(e.valid);
  EXPECT_EQ(e.band[0].rt60Sec, 0.0);
  EXPECT_EQ(e.band[0].coherence, 1.0);
  EXPECT_TRUE(EstimateAllFinite(e));
}

TEST(TwinExisting, SplNanFallsBackTo85) {
  const AcousticTwinParams p = AcousticTwinParams::esseGenezi();
  const AcousticTwinEstimate e = AcousticTwin::estimateAtSpl(p, kNan);
  EXPECT_TRUE(e.valid);
  EXPECT_TRUE(EstimateAllFinite(e));
}

TEST(TwinExisting, InterventionScaleEqualsConfidence) {
  // A confianza baja se interviene MENOS (corrección del diseño
  // invertido de ivanna-free): interventionScale == confidence.
  const AcousticTwinEstimate e =
      AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
  EXPECT_TRUE(e.valid);
  EXPECT_DOUBLE_EQ(e.overallInterventionScale, e.overallConfidence);
  for (const auto& b : e.band) {
    EXPECT_DOUBLE_EQ(b.interventionScale, b.confidence);
    EXPECT_GE(b.interventionScale, 0.0);
    EXPECT_LE(b.interventionScale, 1.0);
  }
}

// ── Código EXISTENTE que el lazo usa: conservación ─────────────────────────

TEST(ConservationExisting, AmountZeroIsBitExactForExtremeInputs) {
  // (2)+(7) amount=0: bypass bit-exacto con silencio, seno, DC y +-1e30.
  PerceptualConservation c;
  c.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = 0.0f;
  c.setConfig(cfg);
  double l[128], r[128], l0[128], r0[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = l0[i] = 1e30 * std::sin(i * 0.3);
    r[i] = r0[i] = -1e30 * std::cos(i * 0.3);
  }
  c.process(l, r, 128, 0.0);
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(l[i], l0[i]);
    EXPECT_EQ(r[i], r0[i]);
  }
  for (int i = 0; i < 128; ++i) l[i] = r[i] = 0.0;
  c.process(l, r, 128, 0.00267);
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(l[i], 0.0);
    EXPECT_EQ(r[i], 0.0);
  }
  EXPECT_FLOAT_EQ(c.appliedGain(), 1.0f);
}

TEST(ConservationExisting, NanAmountIsClampedToBypass) {
  PerceptualConservation c;
  c.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = kNanF;
  c.setConfig(cfg);
  EXPECT_FLOAT_EQ(c.config().amount, 0.0f);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.5;
    r[i] = -0.5;
  }
  c.process(l, r, 128, 0.0);
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(l[i], 0.5);
    EXPECT_EQ(r[i], -0.5);
  }
  cfg.amount = kInfF;  // +Inf -> 0 (clampf mapea lo no finito al mínimo)
  c.setConfig(cfg);
  EXPECT_FLOAT_EQ(c.config().amount, 0.0f);
  cfg.amount = -1.0f;
  c.setConfig(cfg);
  EXPECT_FLOAT_EQ(c.config().amount, 0.0f);
}

TEST(ConservationExisting, AmountOneLimitsSustainedLoudNeverAmplifies) {
  PerceptualConservation c;
  c.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = 0.5f;
  c.setConfig(cfg);
  double l[128], r[128];
  double phase = 0.0;
  for (int blk = 0; blk < 300; ++blk) {
    for (int i = 0; i < 128; ++i) {
      l[i] = 0.9 * std::sin(phase);
      r[i] = l[i];
      phase += 0.131;
    }
    c.process(l, r, 128, blk * 0.00267);
    for (int i = 0; i < 128; ++i) {
      EXPECT_TRUE(std::isfinite(l[i]));
      EXPECT_LE(std::abs(l[i]), 0.9 + 1e-12);  // |salida| <= |entrada|
    }
  }
  EXPECT_LT(c.appliedGain(), 0.99f);  // el techo 0.5 acaba limitando
  EXPECT_GT(c.appliedGain(), 0.5f);
  EXPECT_TRUE(std::isfinite(c.appliedGain()));
  const ConservationFieldState st = c.fieldState();
  for (const auto& b : st.band) {
    EXPECT_TRUE(std::isfinite(b.energy));
    EXPECT_GE(b.energy, 0.0f);
    EXPECT_LE(b.energy, 1.0f);
  }
}

TEST(ConservationExisting, ExtremeSingleBlockStaysFiniteAndBounded) {
  // (2) un bloque a +-1e30 con amount=1: salida finita, |salida| <=
  // |entrada|, ganancia finita. (La energía medida se satura a 0 por
  // el cast a float, así que no limita: ver nota en la cabecera.)
  PerceptualConservation c;
  c.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = 0.5f;
  c.setConfig(cfg);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 1e30 * std::sin(i * 0.1);
    r[i] = l[i];
  }
  c.process(l, r, 128, 0.0);
  for (int i = 0; i < 128; ++i) {
    EXPECT_TRUE(std::isfinite(l[i]));
    EXPECT_LE(std::abs(l[i]), 1e30);
  }
  EXPECT_TRUE(std::isfinite(c.appliedGain()));
  EXPECT_GE(c.appliedGain(), 0.0f);
  EXPECT_LE(c.appliedGain(), 1.0f);
}

TEST(ConservationExisting, NanSamplesDoNotSpreadAndNextBlockRecovers) {
  // (1) una muestra NaN en la entrada: no contamina las demás, no
  // crashea, y el bloque siguiente limpio sale sin NaN.
  PerceptualConservation c;
  c.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = 0.5f;
  c.setConfig(cfg);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.5;
    r[i] = 0.5;
  }
  l[10] = std::numeric_limits<double>::quiet_NaN();
  r[64] = kInf;
  c.process(l, r, 128, 0.0);
  int nonFinite = 0;
  for (int i = 0; i < 128; ++i) {
    if (!std::isfinite(l[i])) ++nonFinite;
    if (!std::isfinite(r[i])) ++nonFinite;
  }
  EXPECT_EQ(nonFinite, 2);  // solo las muestras que ya venían no finitas
  EXPECT_TRUE(std::isfinite(c.appliedGain()));
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.5;
    r[i] = 0.5;
  }
  c.process(l, r, 128, 0.00267);
  for (int i = 0; i < 128; ++i) {
    EXPECT_TRUE(std::isfinite(l[i]));
    EXPECT_TRUE(std::isfinite(r[i]));
  }
}

TEST(ConservationExisting, EmptyAndNullInputsDoNotCrash) {
  // (7) frames 0 y punteros nulos: no-op, sin crash.
  PerceptualConservation c;
  c.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  c.setConfig(cfg);
  double l[4] = {0.1, 0.2, 0.3, 0.4};
  double r[4] = {0.1, 0.2, 0.3, 0.4};
  c.process(l, r, 0, 0.0);
  c.process(nullptr, r, 4, 0.0);
  c.process(l, nullptr, 4, 0.0);
  c.process(nullptr, nullptr, 0, 0.0);
  c.process(l, r, -5, 0.0);
  EXPECT_EQ(l[0], 0.1);
  EXPECT_EQ(r[3], 0.4);
}

TEST(ConservationExisting, DegenerateSampleRateIsSanitized) {
  PerceptualConservation c;
  c.prepare(kNan);
  c.prepare(0.0);
  c.prepare(1e30);
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  c.setConfig(cfg);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.5 * std::sin(i * 0.2);
    r[i] = l[i];
  }
  c.process(l, r, 128, 0.0);
  for (int i = 0; i < 128; ++i) EXPECT_TRUE(std::isfinite(l[i]));
}

TEST(ConservationExisting, SolverNullAndNanInputsDoNotCrash) {
  ConservationFieldState prev{};
  ConservationProposal props[1]{};
  float meas[8];
  for (auto& m : meas) m = 0.5f;
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = 0.5f;
  float gains[8];
  ConservationFieldState st{};
  ConservationSolver::solve(prev, nullptr, 0, meas, cfg, 0.0, 0.00267, gains,
                            &st);
  ConservationSolver::solve(prev, props, 1, nullptr, cfg, 0.0, 0.00267, gains,
                            &st);
  ConservationSolver::solve(prev, props, 1, meas, cfg, 0.0, 0.00267, nullptr,
                            &st);
  ConservationSolver::solve(prev, props, 1, meas, cfg, 0.0, 0.00267, gains,
                            nullptr);
  ConservationSolver::solve(prev, props, 1, meas, cfg, 0.0, kNan, gains, &st);
  SUCCEED();
}

TEST(ConservationExisting, SolverNanMeasuredEnergyGivesUnityGain) {
  ConservationFieldState prev{};
  ConservationProposal props[1]{};
  props[0].confidence = 1.0f;
  props[0].priority = 1.0f;
  props[0].energyMul.fill(1.0f);
  props[0].pressureMul.fill(1.0f);
  float meas[8];
  for (auto& m : meas) m = kNanF;  // (1) energía medida NaN
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = 0.5f;
  float gains[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  ConservationFieldState st{};
  ConservationSolver::solve(prev, props, 1, meas, cfg, 0.0, 0.00267, gains,
                            &st);
  for (int b = 0; b < 8; ++b) {
    EXPECT_TRUE(std::isfinite(gains[b]));
    EXPECT_FLOAT_EQ(gains[b], 1.0f);  // energía 0 medida -> no se atenúa
  }
}

TEST(ConservationExisting, SolverNanProposalGivesZeroGainFinite) {
  // (1) propuesta con energyMul NaN: el solver no propaga el NaN;
  // la ganancia cae a 0 (fail-closed: silencio antes que energía
  // inventada), finita y en [0,1].
  ConservationFieldState prev{};
  ConservationProposal props[1]{};
  props[0].confidence = 1.0f;
  props[0].priority = 1.0f;
  props[0].energyMul.fill(kNanF);
  props[0].pressureMul.fill(1.0f);
  float meas[8];
  for (auto& m : meas) m = 0.5f;
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = 0.5f;
  float gains[8];
  ConservationFieldState st{};
  ConservationSolver::solve(prev, props, 1, meas, cfg, 0.0, 0.00267, gains,
                            &st);
  for (int b = 0; b < 8; ++b) {
    EXPECT_TRUE(std::isfinite(gains[b]));
    EXPECT_GE(gains[b], 0.0f);
    EXPECT_LE(gains[b], 1.0f);
  }
  EXPECT_FLOAT_EQ(gains[0], 0.0f);
}

TEST(ConservationExisting, SolverNeverCreatesEnergyEvenWithHugeProposals) {
  // (2) multiplicadores de 1e30 y confianzas/prioridades absurdas:
  // la ganancia por banda sigue en [0,1]: nunca amplifica.
  ConservationFieldState prev{};
  ConservationProposal props[2]{};
  for (auto& pr : props) {
    pr.confidence = 1e30f;
    pr.priority = 1e30f;
    pr.energyMul.fill(1e30f);
    pr.pressureMul.fill(1e30f);
  }
  float meas[8];
  for (auto& m : meas) m = 0.5f;
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  cfg.energyCap = kNanF;  // techo NaN -> 0 por el saneo
  float gains[8];
  ConservationFieldState st{};
  ConservationSolver::solve(prev, props, 2, meas, cfg, 0.0, 0.00267, gains,
                            &st);
  for (int b = 0; b < 8; ++b) {
    EXPECT_TRUE(std::isfinite(gains[b]));
    EXPECT_GE(gains[b], 0.0f);
    EXPECT_LE(gains[b], 1.0f);
  }
  for (auto& m : meas) m = 0.0f;  // silencio: ganancia 1
  ConservationSolver::solve(prev, props, 2, meas, cfg, 0.0, 0.00267, gains,
                            &st);
  for (int b = 0; b < 8; ++b) EXPECT_FLOAT_EQ(gains[b], 1.0f);
}

TEST(ConservationExisting, TwinScaleSanitizedAndApplied) {
  PerceptualConservation c;
  c.prepare(48000.0);
  EXPECT_DOUBLE_EQ(c.twinInterventionScale(), 1.0);
  c.setTwinInterventionScale(kNan);  // NaN -> 1.0 (sin cambio)
  EXPECT_DOUBLE_EQ(c.twinInterventionScale(), 1.0);
  c.setTwinInterventionScale(2.0);
  EXPECT_DOUBLE_EQ(c.twinInterventionScale(), 1.0);
  c.setTwinInterventionScale(-1.0);
  EXPECT_DOUBLE_EQ(c.twinInterventionScale(), 0.0);
  // Gemelo inválido -> escala 1.0 (la conservación se comporta igual).
  AcousticTwinEstimate bad;
  bad.valid = false;
  applyTwinToConservation(c, bad);
  EXPECT_DOUBLE_EQ(c.twinInterventionScale(), 1.0);
  // Gemelo válido con intervención 0 -> la conservación se apaga.
  AcousticTwinEstimate zero = CalibratedTwinEstimate();
  zero.overallInterventionScale = 0.0;
  applyTwinToConservation(c, zero);
  EXPECT_DOUBLE_EQ(c.twinInterventionScale(), 0.0);
  // Gemelo calibrado 0.70 -> escala 0.70 (guardada como float).
  applyTwinToConservation(c, CalibratedTwinEstimate());
  EXPECT_FLOAT_EQ(static_cast<float>(c.twinInterventionScale()), 0.70f);
  // Y con escala 0 el process es bypass bit-exacto.
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  c.setConfig(cfg);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.5;
    r[i] = -0.5;
  }
  c.process(l, r, 128, 0.0);
  for (int i = 0; i < 128; ++i) {
    EXPECT_EQ(l[i], 0.5);
    EXPECT_EQ(r[i], -0.5);
  }
}

TEST(ConservationExisting, OperatorAmountsSanitized) {
  PerceptualConservation c;
  c.prepare(48000.0);
  c.setOperatorAmounts(nullptr, 0);  // no crash
  float amounts[7] = {kNanF, kInfF, -5.0f, 2.0f, 0.5f, 0.0f, 1.0f};
  c.setOperatorAmounts(amounts, 7);  // clamp a [0,1], sin crash
  ConservationConfig cfg;
  cfg.amount = 1.0f;
  c.setConfig(cfg);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.5 * std::sin(i * 0.2);
    r[i] = l[i];
  }
  c.process(l, r, 128, 0.0);
  for (int i = 0; i < 128; ++i) EXPECT_TRUE(std::isfinite(l[i]));
}

// ── Código EXISTENTE que el lazo usa: MPC en sombra ────────────────────────

TEST(MpcExisting, NanFieldIsSafeModeZeroAction) {
  MpcController mpc;
  PerceptualField f = PerceptualField::neutral();
  f.bands[0][Dim::kClarity] = kNan;
  const MpcDecision d = mpc.step(f);
  EXPECT_TRUE(d.safeMode);
  EXPECT_TRUE(ActionFiniteAndBounded(d.action, 0.0));
  EXPECT_EQ(mpc.decisionsLogged(), 1u);
}

TEST(MpcExisting, KnownDefect_ScoreNaNCurrentPropagatesNaN) {
  // DEFECTO REAL (pinneado): score() con el campo actual no finito
  // devuelve NaN en vez de -inf (lo que sí hace con la predicción no
  // finita o con el presupuesto excedido), y step() en modo seguro
  // publica ese NaN como score/scoreZero en la decisión y la
  // telemetría. La ACCIÓN sí es cero (el fail-closed funciona).
  // AL CORREGIRLO (score() -> -inf si current no es finito), este
  // test debe INVERTIRSE a EXPECT_TRUE(std::isfinite(...)) / -inf.
  MpcController mpc;
  PerceptualField f = PerceptualField::neutral();
  f.bands[0][Dim::kClarity] = kNan;
  EXPECT_TRUE(std::isnan(mpc.score(f, PerceptualField::neutral(),
                                   zeroAction())));
  const MpcDecision d = mpc.step(f);
  EXPECT_TRUE(d.safeMode);
  EXPECT_TRUE(ActionFiniteAndBounded(d.action, 0.0));
  EXPECT_TRUE(std::isnan(d.score));
  EXPECT_TRUE(std::isnan(d.scoreZero));
}

TEST(MpcExisting, ConfidenceZeroAndNanAreSafeMode) {
  // (6) confianza 0 en todo el campo, o NaN en una banda: acción cero.
  MpcController mpc;
  PerceptualField zero = PerceptualField::neutral();
  for (auto& b : zero.bands) b[Dim::kConfidence] = 0.0;
  EXPECT_TRUE(mpc.step(zero).safeMode);
  MpcController mpc2;
  PerceptualField nanConf = PerceptualField::neutral();
  nanConf.bands[3][Dim::kConfidence] = kNan;
  const MpcDecision d = mpc2.step(nanConf);
  EXPECT_TRUE(d.safeMode);
  EXPECT_TRUE(ActionFiniteAndBounded(d.action, 0.0));
}

TEST(MpcExisting, ConfidenceBoundary01999Vs020) {
  MpcController mpc;
  PerceptualField f = PerceptualField::neutral();
  for (auto& b : f.bands) b[Dim::kConfidence] = 0.1999;
  EXPECT_TRUE(mpc.step(f).safeMode);
  MpcController mpc2;
  PerceptualField f2 = PerceptualField::neutral();
  // 0.21, no 0.20 exacto: la media es una suma en coma flotante y el
  // borde exacto 0.20 es frágil al redondeo (8 x 0.2 sumado puede dar
  // 0.19999999999999998 < minConfidence). El umbral exacto 0.20 se
  // prueba sin suma en M5 (twin.global) y M8.
  for (auto& b : f2.bands) b[Dim::kConfidence] = 0.21;
  EXPECT_FALSE(mpc2.step(f2).safeMode);
}

TEST(MpcExisting, AbsurdFiniteFieldStillReturnsZeroActionFiniteScore) {
  // (2) campo en +-1e30 con confianza válida 0.9: el MPC no entra en
  // modo seguro (el campo ES finito), pero el argmax cae en la acción
  // cero (cualquier acción solo añade huella) y la puntuación es
  // finita. La acción nunca sale del límite de tasa.
  MpcController mpc;
  PerceptualField f;
  for (auto& b : f.bands) {
    b.v.fill(1e30);
    b[Dim::kConfidence] = 0.9;
    b[Dim::kBudget] = 0.0;
  }
  const MpcDecision d = mpc.step(f);
  EXPECT_TRUE(ActionFiniteAndBounded(d.action, 0.0));
  EXPECT_TRUE(std::isfinite(d.score));
  PerceptualField neg;
  for (auto& b : neg.bands) {
    b.v.fill(-1e30);
    b[Dim::kConfidence] = 0.9;
    b[Dim::kBudget] = 0.0;
  }
  const MpcDecision d2 = mpc.step(neg);
  EXPECT_TRUE(ActionFiniteAndBounded(d2.action, 0.0));
  EXPECT_TRUE(std::isfinite(d2.score));
}

TEST(MpcExisting, RapidAlternationRespectsRateLimit) {
  // (3) 50 ciclos alternando el campo entre extremos: ningún salto de
  // acción supera 0.20 por operador.
  MpcController mpc;
  Action prev = zeroAction();
  for (int t = 0; t < 50; ++t) {
    PerceptualField f = PerceptualField::neutral();
    const double v = (t % 2 == 0) ? 0.0 : 1.0;
    for (auto& b : f.bands) {
      b[Dim::kClarity] = v;
      b[Dim::kImpact] = v;
    }
    const MpcDecision d = mpc.step(f);
    for (std::size_t i = 0; i < kNumOperators; ++i) {
      EXPECT_LE(std::abs(d.action[i] - prev[i]), 0.20 + 1e-9);
      EXPECT_TRUE(std::isfinite(d.action[i]));
    }
    prev = d.action;
  }
}

TEST(MpcExisting, CandidatesWithNanOrExtremePreviousAreBounded) {
  MpcController mpc;
  Action nanA;
  nanA.fill(kNan);
  for (const auto& c : mpc.candidates(nanA)) {
    EXPECT_TRUE(ActionFiniteAndBounded(c, 0.20));
  }
  Action huge;
  huge.fill(1e30);
  for (const auto& c : mpc.candidates(huge)) {
    EXPECT_TRUE(ActionFiniteAndBounded(c, 1.0));
  }
  Action negHuge;
  negHuge.fill(-1e30);
  for (const auto& c : mpc.candidates(negHuge)) {
    EXPECT_TRUE(ActionFiniteAndBounded(c, 1.0));
  }
  EXPECT_EQ(mpc.candidates(zeroAction()).size(), 24u);
}

TEST(MpcExisting, DegenerateConfigIsSanitized) {
  MpcConfig bad;
  bad.maxDeltaPerCycle = kNan;  // -> 0.20
  bad.numCandidates = 0;        // -> 1
  bad.horizon = 0;              // -> 1
  bad.minConfidence = -1.0;     // -> 0.20
  bad.maxBudget = kInf;         // -> 0.90
  MpcController mpc(bad);
  const MpcDecision d = mpc.step(DemandingField());
  EXPECT_TRUE(ActionFiniteAndBounded(d.action, 0.20));
  EXPECT_TRUE(std::isfinite(d.score));
  MpcConfig huge;
  huge.numCandidates = 100000;  // tope interno 256
  MpcController mpc2(huge);
  EXPECT_LE(mpc2.candidates(zeroAction()).size(), 256u);
}

TEST(MpcExisting, HugeHorizonCompletesWithBoundedAction) {
  // El horizonte NO tiene tope superior en MpcConfig (observación:
  // horizon=SIZE_MAX colgaría el paso; aquí se prueba 5000, que
  // completa rápido y con acción acotada).
  MpcConfig cfg;
  cfg.horizon = 5000;
  MpcController mpc(cfg);
  const MpcDecision d = mpc.step(PerceptualField::neutral());
  EXPECT_TRUE(ActionFiniteAndBounded(d.action, 0.20));
  EXPECT_TRUE(std::isfinite(d.score));
}

TEST(MpcExisting, ProcessShadowNullZeroAndBitExact) {
  // (7) processShadow con punteros nulos / 0 frames no crashea y
  // devuelve una decisión; con audio, la copia es bit-exacta.
  MpcController mpc;
  const MpcDecision d0 =
      mpc.processShadow(nullptr, nullptr, 0, PerceptualField::neutral());
  EXPECT_TRUE(std::isfinite(d0.score));
  float in[4] = {1.0f, -1.0f, 0.5f, -0.5f};
  float out[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  (void)mpc.processShadow(in, out, 4, PerceptualField::neutral());
  for (int i = 0; i < 4; ++i) EXPECT_EQ(out[i], in[i]);
  float inout[4] = {1.0f, -1.0f, 0.5f, -0.5f};  // in-place (in == out)
  (void)mpc.processShadow(inout, inout, 4, PerceptualField::neutral());
  EXPECT_EQ(inout[0], 1.0f);
  EXPECT_EQ(inout[1], -1.0f);
  float extreme[3] = {std::numeric_limits<float>::max(),
                      -std::numeric_limits<float>::max(), 0.0f};
  float extOut[3] = {0.0f, 0.0f, 0.0f};
  (void)mpc.processShadow(extreme, extOut, 3, PerceptualField::neutral());
  for (int i = 0; i < 3; ++i) EXPECT_EQ(extOut[i], extreme[i]);
}

TEST(MpcExisting, ScoreRejectsNonFinitePredictionAndBudgetBlowout) {
  MpcController mpc;
  PerceptualField predNaN = PerceptualField::neutral();
  predNaN.bands[0][Dim::kClarity] = kNan;
  EXPECT_EQ(mpc.score(PerceptualField::neutral(), predNaN, zeroAction()),
            -kInf);
  PerceptualField predBudget = PerceptualField::neutral();
  for (auto& b : predBudget.bands) b[Dim::kBudget] = 0.99;
  EXPECT_EQ(mpc.score(PerceptualField::neutral(), predBudget, zeroAction()),
            -kInf);
  EXPECT_TRUE(std::isfinite(mpc.score(PerceptualField::neutral(),
                                      PerceptualField::neutral(),
                                      zeroAction())));
}

TEST(MpcExisting, StepIndexIsMonotonicAndResetRestarts) {
  // (8) el índice de paso del MPC es la "generación" de la sombra:
  // 1, 2, 3... y reset() lo reinicia a 1, sin huecos ni retrocesos.
  MpcController mpc;
  EXPECT_EQ(mpc.step(PerceptualField::neutral()).stepIndex, 1u);
  EXPECT_EQ(mpc.step(PerceptualField::neutral()).stepIndex, 2u);
  EXPECT_EQ(mpc.step(PerceptualField::neutral()).stepIndex, 3u);
  EXPECT_EQ(mpc.decisionsLogged(), 3u);
  mpc.reset();
  EXPECT_EQ(mpc.decisionsLogged(), 0u);
  EXPECT_EQ(mpc.step(PerceptualField::neutral()).stepIndex, 1u);
  EXPECT_TRUE(mpc.telemetryHistoryAt(99).action == zeroAction() ||
              mpc.telemetryHistoryCount() <= 1u);
}

// ── Vector perceptual / simulador (header-only + OperatorSimulator) ────────

TEST(VectorSim, ClampActionSanitizesNonFiniteAndExtreme) {
  Action a;
  a.fill(kNan);
  EXPECT_TRUE(ActionFiniteAndBounded(clampAction(a), 0.0));
  a.fill(kInf);
  EXPECT_TRUE(ActionFiniteAndBounded(clampAction(a), 0.0));
  a.fill(-kInf);
  EXPECT_TRUE(ActionFiniteAndBounded(clampAction(a), 0.0));
  a.fill(1e30);
  EXPECT_TRUE(ActionFiniteAndBounded(clampAction(a), 1.0));
  EXPECT_DOUBLE_EQ(clampAction(a)[0], 1.0);
  a.fill(-1e30);
  EXPECT_DOUBLE_EQ(clampAction(a)[0], -1.0);
  a.fill(0.5);
  EXPECT_DOUBLE_EQ(clampAction(a)[0], 0.5);
}

TEST(VectorSim, ClampInPlaceSanitizesField) {
  PerceptualVector v;
  v.v.fill(kNan);
  EXPECT_FALSE(v.isFinite());
  v.clampInPlace();
  EXPECT_TRUE(v.isFinite());
  for (double x : v.v) EXPECT_EQ(x, 0.0);
  v.v.fill(1e30);
  v.clampInPlace();
  for (double x : v.v) EXPECT_EQ(x, 1.0);
  v.v.fill(-1e30);
  v.clampInPlace();
  for (double x : v.v) EXPECT_EQ(x, 0.0);
  PerceptualField f;
  for (auto& b : f.bands) b.v.fill(kInf);
  EXPECT_FALSE(f.isFinite());
  f.clampInPlace();
  EXPECT_TRUE(f.isFinite());
}

TEST(VectorSim, SimulatorNanFieldAndActionStayFinite) {
  OperatorSimulator sim;
  PerceptualField f = PerceptualField::neutral();
  f.bands[3][Dim::kClarity] = kNan;
  EXPECT_TRUE(sim.simulateField(f, zeroAction()).isFinite());
  Action nanA;
  nanA.fill(kNan);
  const PerceptualField out =
      sim.simulateField(PerceptualField::neutral(), nanA);
  EXPECT_TRUE(out.isFinite());
  EXPECT_EQ(out.mean(Dim::kBudget), 0.0);  // NaN -> acción cero -> sin gasto
  Action infA;
  infA.fill(kInf);
  EXPECT_TRUE(sim.simulateField(PerceptualField::neutral(), infA).isFinite());
  Action huge;
  huge.fill(1e30);  // se clampa a +-1: gasto máximo, presupuesto acotado
  const PerceptualField outH =
      sim.simulateField(PerceptualField::neutral(), huge);
  EXPECT_TRUE(outH.isFinite());
  EXPECT_NEAR(outH.mean(Dim::kBudget), 0.14, 1e-12);
  EXPECT_LE(outH.mean(Dim::kBudget), 1.0);
}

TEST(VectorSim, SimulatorExtremeFieldStaysInRange) {
  OperatorSimulator sim;
  PerceptualField f;
  for (auto& b : f.bands) {
    b.v.fill(1e30);
    b[Dim::kConfidence] = 0.9;
  }
  const PerceptualField out = sim.simulateField(f, zeroAction());
  EXPECT_TRUE(out.isFinite());
  for (const auto& b : out.bands) {
    for (double x : b.v) {
      EXPECT_GE(x, 0.0);
      EXPECT_LE(x, 1.0);
    }
  }
}

TEST(VectorSim, SimulatorZeroActionAndEmptyField) {
  // (7) acción cero no toca el campo (salvo el presupuesto, que no se
  // mueve); campo vacío (todo cero) predice finito en [0,1].
  OperatorSimulator sim;
  const PerceptualField f = PerceptualField::neutral();
  const PerceptualField out = sim.simulateField(f, zeroAction());
  for (std::size_t b = 0; b < kNumBands; ++b) {
    for (std::size_t d = 0; d < kNumDims; ++d) {
      EXPECT_EQ(out.bands[b].v[d], f.bands[b].v[d]);
    }
  }
  PerceptualField empty;
  const PerceptualField outEmpty = sim.simulateField(empty, zeroAction());
  EXPECT_TRUE(outEmpty.isFinite());
  EXPECT_TRUE(sim.simulateHorizon(empty, zeroAction(), 0).isFinite());
  EXPECT_TRUE(sim.simulateHorizon(DemandingField(), zeroAction(), 5)
                  .isFinite());
}

TEST(VectorSim, ZeroActionAndActionNorms) {
  const Action z = zeroAction();
  for (double x : z) EXPECT_EQ(x, 0.0);
  EXPECT_EQ(actionL1(z), 0.0);
  EXPECT_EQ(actionL2(z), 0.0);
  Action a = zeroAction();
  a[0] = 0.2;
  EXPECT_NEAR(actionL1(a), 0.2, 1e-12);
  EXPECT_NEAR(actionL2(a), 0.2, 1e-12);
}

// ── End-to-end del lazo cerrado (fail-closed en cadena) ─────────────────────

TEST(EndToEnd, HealthyTwin070LimitsIntensityAndShadowProposesSafely) {
  // Cadena sana con la calibración estimada actual: M3 -> 0.70,
  // M8 -> kLimitOnly (maxIntensity 0.70), M6 propone acotado, M5
  // acepta una acción pequeña, la conservación se escala a 0.70.
  TwinEstimator est;
  const TwinConfidence conf = est.estimate(CalibratedTwinEstimate(),
                                           TwinCalibration::geneziPz1dEstimated());
  ASSERT_TRUE(conf.valid);
  UncertaintySupervisor sup;
  const Supervision s = sup.supervise(conf, HealthyState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kLimitOnly);
  EXPECT_FLOAT_EQ(s.maxIntensity, 0.70f);
  MpcShadow sh;
  const MpcProposal p = sh.propose(DemandingField(), conf);
  EXPECT_STREQ(p.reason, "ok");
  EXPECT_TRUE(ActionFiniteAndBounded(p.action, 0.20));
  ActionSimulator sim;
  Action small = zeroAction();
  small[0] = 0.1;
  EXPECT_TRUE(sim.evaluate(DemandingField(), small, conf, -3.0, 0.0, 0.0, 0.0)
                  .allowed);
  PerceptualConservation cons;
  cons.prepare(48000.0);
  applyTwinToConservation(cons, CalibratedTwinEstimate());
  EXPECT_FLOAT_EQ(static_cast<float>(cons.twinInterventionScale()), 0.70f);
}

TEST(EndToEnd, InvalidTwinShutsDownEveryStage) {
  // Gemelo inválido -> M3 inválido -> M8 kNone -> M6 twin-invalid ->
  // M5 rechaza -> conservación con escala 1.0 (sin gemelo, sin cambio)
  // y telemetría registra el fallo como visible.
  TwinEstimator est;
  AcousticTwinEstimate bad;
  bad.valid = false;
  const TwinConfidence conf = est.estimate(bad);
  EXPECT_FALSE(conf.valid);
  UncertaintySupervisor sup;
  const Supervision s = sup.supervise(conf, HealthyState(), 0.0f);
  EXPECT_EQ(s.level, InterventionLevel::kNone);
  EXPECT_EQ(s.maxIntensity, 0.0f);
  MpcShadow sh;
  EXPECT_STREQ(sh.propose(DemandingField(), conf).reason, "twin-invalid");
  ActionSimulator sim;
  Action a = zeroAction();
  a[0] = 0.1;
  EXPECT_FALSE(sim.evaluate(DemandingField(), a, conf, -3.0, 0.0, 0.0, 0.0)
                   .allowed);
  PerceptualConservation cons;
  cons.prepare(48000.0);
  applyTwinToConservation(cons, bad);
  EXPECT_DOUBLE_EQ(cons.twinInterventionScale(), 1.0);
  Telemetry tel;
  TelemetryFrame f;
  f.valid = false;  // el fallo queda registrado, no desaparece
  f.confidence = kNanF;
  tel.record(f);
  TelemetryFrame out[1];
  ASSERT_EQ(tel.drain(out, 1), 1u);
  EXPECT_FALSE(out[0].valid);
}

TEST(EndToEnd, NanFieldShutsDownControlButAudioPassesThrough) {
  // (1)+(7) campo NaN + conservación apagada: el control da cero por
  // todas partes y el audio pasa bit-exacto (bypass, no silencio).
  MpcShadow sh;
  PerceptualField f = DemandingField();
  f.bands[1][Dim::kImpact] = kNan;
  const MpcProposal p = sh.propose(f, HealthyTwin(0.9f));
  EXPECT_TRUE(p.isZero);
  EXPECT_STREQ(p.reason, "nan-field");
  MpcController mpc;
  EXPECT_TRUE(mpc.step(f).safeMode);
  PerceptualConservation cons;
  cons.prepare(48000.0);
  ConservationConfig cfg;
  cfg.amount = 0.0f;
  cons.setConfig(cfg);
  double l[128], r[128];
  for (int i = 0; i < 128; ++i) {
    l[i] = 0.25 * std::sin(i * 0.2);
    r[i] = l[i];
  }
  const double l0 = l[0];
  cons.process(l, r, 128, 0.0);
  EXPECT_EQ(l[0], l0);
}

}  // namespace
