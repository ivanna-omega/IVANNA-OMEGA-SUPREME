// test_twin_estimator.cpp — M3: gemelo acústico como estimador puro con
// confianza por variable (plan del salto, Luna).
//
// Verifica lo que el módulo promete y nada más:
//   1. Calibración estimada 0.70 -> method datasheet, global 0.70,
//      intensidad de conservación parcial (< 1) y canControlFine()==false.
//   2. Confianza por variable: cada campo tiene su confidence y su method
//      (h1 por banda, h2/h3 sin dato, latencia, sala, coherencia, ruido,
//      potencia).
//   3. Sin calibración / gemelo inválido -> TwinConfidence inválido
//      (valid=false) e intensidad 0.
//   4. Calibración medida simulada (TwinMeasuredValues, todo kMeasured,
//      confianza alta) -> canControlFine()==true.
//   5. Valores no finitos en la entrada no producen confianzas no finitas
//      (fail-closed).
//   6. Medido con confianza media (0.60) -> sin control fino, intensidad
//      parcial.
//   7. Sin calibración -> method kDerived, global por debajo de 0.50
//      (la potencia desconocida lo baja; el gemelo a secas da ~0.46).
//
// Sin fabricar métricas: los valores esperados son los del gemelo real
// (AcousticTwin::estimateCalibrated con la P3 estimada) y del modelo P3
// (confianza honesta 0.70, documentada en genezi_pz1d_estimado_datasheet
// .json y transcrita en TwinCalibration::geneziPz1dEstimated). Las
// tolerancias de ganancia/latencia/RT60 son las del propio test del
// gemelo (cálculo independiente a mano).

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "fused/acoustic_twin/acoustic_twin.hpp"
#include "fused/closed_loop/twin_confidence.hpp"
#include "fused/closed_loop/twin_estimator.hpp"

namespace {

using namespace ivanna::closed_loop;
using namespace ivanna::fused::acoustic_twin;

// Estimación real del trayecto de Esse con la calibración P3 estimada.
AcousticTwinEstimate calibratedEstimate() {
  const TwinCalibration cal = TwinCalibration::geneziPz1dEstimated();
  return AcousticTwin::estimateCalibrated(
      AcousticTwinParams::esseGeneziCalibrated(), cal);
}

// ¿Toda la confianza de salida es finita y en [0,1]? (test 5).
bool allConfidencesFiniteAndBounded(const TwinConfidence& c) {
  auto paramOk = [](const ParameterConfidence& p) {
    return std::isfinite(p.estimate) && std::isfinite(p.variance) &&
           std::isfinite(p.age) && std::isfinite(p.confidence) &&
           p.confidence >= 0.0f && p.confidence <= 1.0f &&
           p.variance >= 0.0f;
  };
  for (const auto& h1 : c.h1) {
    if (!paramOk(h1)) return false;
  }
  const ParameterConfidence* rest[7] = {&c.h2, &c.h3, &c.latency, &c.roomIr,
                                        &c.coherence, &c.noiseFloor,
                                        &c.powerHeadroom};
  for (const ParameterConfidence* p : rest) {
    if (!paramOk(*p)) return false;
  }
  return std::isfinite(c.global) && c.global >= 0.0f && c.global <= 1.0f;
}

// Valores medidos simulados (Ordenes 1-5) con confianza alta: cubren
// TODAS las variables, h2/h3 incluidos. Las estimaciones se toman del
// gemelo real (ganancia/latencia/RT60/coherencia/ruido/potencia); lo
// simulado es SOLO el hecho de que fueron medidas y su confianza.
TwinMeasuredValues measuredHigh() {
  const AcousticTwinEstimate e = calibratedEstimate();
  TwinMeasuredValues m;
  double cohSum = 0.0, noiseSum = 0.0, minMaxSpl = 0.0;
  for (std::size_t i = 0; i < kTwinNumBands; ++i) {
    m.h1Db[i] = static_cast<float>(e.band[i].gainDb);
    m.h1Confidence[i] = 0.95f;
    cohSum += e.band[i].coherence;
    noiseSum += e.band[i].noiseDbSpl;
    if (i == 0 || e.band[i].maxSplDb < minMaxSpl) {
      minMaxSpl = e.band[i].maxSplDb;
    }
  }
  m.h2 = 0.05f;  // kernel de 2º orden identificado (valor simulado)
  m.h2Confidence = 0.90f;
  m.h3 = 0.01f;  // kernel de 3er orden identificado (valor simulado)
  m.h3Confidence = 0.90f;
  m.latencyMs = static_cast<float>(e.bufferLatencyMs + e.airLatencyMs);
  m.latencyConfidence = 0.95f;
  m.roomRt60Sec = static_cast<float>(e.band[4].rt60Sec);
  m.roomConfidence = 0.95f;
  m.coherence = static_cast<float>(cohSum / kTwinNumBands);
  m.coherenceConfidence = 0.95f;
  m.noiseFloorDbSpl = static_cast<float>(noiseSum / kTwinNumBands);
  m.noiseConfidence = 0.95f;
  m.powerHeadroomDbSpl = static_cast<float>(minMaxSpl);
  m.powerConfidence = 0.95f;
  m.valid = true;
  return m;
}

}  // namespace

// ── 1. Calibración estimada 0.70 ─────────────────────────────────────
TEST(TwinEstimator, EstimatedCalibrationLimitsConservationOnly) {
  const AcousticTwinEstimate e = calibratedEstimate();
  ASSERT_TRUE(e.valid);
  ASSERT_TRUE(e.calibrationApplied);
  ASSERT_NEAR(e.calibrationConfidence, 0.70, 0.001);

  TwinEstimator est;
  const TwinConfidence c = est.estimate(e, TwinCalibration::geneziPz1dEstimated());
  ASSERT_TRUE(c.valid);
  // El global es la confianza honesta de la calibración (0.70).
  EXPECT_NEAR(c.global, 0.70, 1e-5);
  EXPECT_EQ(c.h1[0].method, ConfidenceMethod::kDatasheet);
  EXPECT_EQ(c.latency.method, ConfidenceMethod::kDatasheet);
  EXPECT_EQ(c.roomIr.method, ConfidenceMethod::kDatasheet);
  EXPECT_EQ(c.powerHeadroom.method, ConfidenceMethod::kDatasheet);
  // La misma sobrecarga de un argumento no distingue la calibración:
  // con calibración aplicada etiqueta kDatasheet y mismo global.
  const TwinConfidence c1 = est.estimate(e);
  EXPECT_TRUE(c1.valid);
  EXPECT_NEAR(c1.global, 0.70, 1e-5);
  EXPECT_EQ(c1.h1[0].method, ConfidenceMethod::kDatasheet);

  // Intensidad de conservación PARCIAL: 0.70, nunca 1.0 con estimado.
  EXPECT_NEAR(est.maxConservationIntensity(), 0.70, 1e-5);
  EXPECT_GT(est.maxConservationIntensity(), 0.0f);
  EXPECT_LT(est.maxConservationIntensity(), 1.0f);
  EXPECT_NEAR(TwinEstimator::maxConservationIntensity(c), 0.70, 1e-5);
  // Nivel de intervención: media -> solo limita intensidad.
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kLimitOnly);
  EXPECT_EQ(TwinEstimator::interventionLevel(c),
            InterventionLevel::kLimitOnly);
  // REGLA DURA: estimado NUNCA decide control fino.
  EXPECT_FALSE(est.canControlFine());
  EXPECT_FALSE(TwinEstimator::canControlFine(c));
  EXPECT_TRUE(allConfidencesFiniteAndBounded(c));
}

// ── 2. Confianza por variable ────────────────────────────────────────
TEST(TwinEstimator, PerVariableConfidence) {
  const AcousticTwinEstimate e = calibratedEstimate();
  TwinEstimator est;
  const TwinConfidence c =
      est.estimate(e, TwinCalibration::geneziPz1dEstimated());
  ASSERT_TRUE(c.valid);

  // H1 por banda (9 bandas 63 Hz–16 kHz): la confianza por banda es la
  // del gemelo calibrado (graves 0.65 / medios 0.70 / agudos 0.60).
  for (std::size_t i = 0; i < kTwinNumBands; ++i) {
    EXPECT_TRUE(c.h1[i].valid) << "banda " << i;
    EXPECT_EQ(c.h1[i].method, ConfidenceMethod::kDatasheet) << "banda " << i;
    EXPECT_GT(c.h1[i].confidence, 0.0f) << "banda " << i;
    EXPECT_NEAR(c.h1[i].estimate, e.band[i].gainDb, 1e-5) << "banda " << i;
    EXPECT_FLOAT_EQ(c.h1[i].age, 0.0f) << "banda " << i;
    EXPECT_GE(c.h1[i].variance, 0.0f) << "banda " << i;
  }
  EXPECT_NEAR(c.h1[0].confidence, 0.65, 1e-5);  // 63 Hz: graves
  EXPECT_NEAR(c.h1[1].confidence, 0.65, 1e-5);  // 125 Hz: graves
  EXPECT_NEAR(c.h1[4].confidence, 0.70, 1e-5);  // 1 kHz: medios
  EXPECT_NEAR(c.h1[8].confidence, 0.60, 1e-5);  // 16 kHz: agudos
  // Ganancia a 1 kHz: ancla del gemelo, -6.35 dB (cálculo a mano).
  EXPECT_NEAR(c.h1[4].estimate, -6.35, 0.5);

  // h2/h3: el gemelo NO identifica kernels Volterra (Orden 5 pendiente):
  // sin dato, no un cero fingido.
  EXPECT_FALSE(c.h2.valid);
  EXPECT_FLOAT_EQ(c.h2.confidence, 0.0f);
  EXPECT_EQ(c.h2.method, ConfidenceMethod::kUnknown);
  EXPECT_FLOAT_EQ(c.h2.estimate, 0.0f);
  EXPECT_FALSE(c.h3.valid);
  EXPECT_FLOAT_EQ(c.h3.confidence, 0.0f);
  EXPECT_EQ(c.h3.method, ConfidenceMethod::kUnknown);

  // Latencia total: buffer 2.667 ms + aire 8.746 ms = ~11.41 ms.
  EXPECT_TRUE(c.latency.valid);
  EXPECT_NEAR(c.latency.estimate, 11.41, 0.5);
  EXPECT_NEAR(c.latency.confidence, 0.70, 1e-5);
  EXPECT_EQ(c.latency.method, ConfidenceMethod::kDatasheet);

  // Sala: RT60 de referencia 0.34 s a 1 kHz.
  EXPECT_TRUE(c.roomIr.valid);
  EXPECT_NEAR(c.roomIr.estimate, 0.34, 0.05);
  EXPECT_NEAR(c.roomIr.confidence, 0.70, 1e-5);
  EXPECT_EQ(c.roomIr.method, ConfidenceMethod::kDatasheet);

  // Coherencia baja a 3 m (domina la reverberación, ~0.069 a 1 kHz).
  EXPECT_TRUE(c.coherence.valid);
  EXPECT_GT(c.coherence.estimate, 0.0f);
  EXPECT_LT(c.coherence.estimate, 0.5f);
  EXPECT_NEAR(c.coherence.confidence, 0.70, 1e-5);

  // Piso de ruido: ambiente 30 dB SPL con forma espectral del gemelo.
  EXPECT_TRUE(c.noiseFloor.valid);
  EXPECT_GT(c.noiseFloor.estimate, 20.0f);
  EXPECT_LT(c.noiseFloor.estimate, 40.0f);
  EXPECT_NEAR(c.noiseFloor.confidence, 0.70, 1e-5);

  // Potencia: con calibración el SPL máximo SÍ se conoce (135 W, 88 dB);
  // el techo es el de la banda más débil (63 Hz, ~99.7 dB SPL).
  EXPECT_TRUE(c.powerHeadroom.valid);
  EXPECT_GT(c.powerHeadroom.estimate, 90.0f);
  EXPECT_LT(c.powerHeadroom.estimate, 115.0f);
  EXPECT_NEAR(c.powerHeadroom.confidence, 0.70, 1e-5);
  EXPECT_EQ(c.powerHeadroom.method, ConfidenceMethod::kDatasheet);
}

// ── 3. Sin calibración / gemelo inválido -> inválido, intensidad 0 ───
TEST(TwinEstimator, InvalidTwinYieldsInvalidConfidence) {
  TwinEstimator est;
  // Estimador fresco, sin ninguna estimación: nada que controlar.
  EXPECT_FALSE(est.lastConfidence().valid);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  EXPECT_FALSE(est.canControlFine());
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kNone);

  // Parámetros en cero: el gemelo no puede estimar nada. OJO:
  // AcousticTwinParams{} NO es inválido (trae defaults válidos en el
  // struct: DAC 24-bit/48 kHz, sala 4×7×2.5, RT60 0.34); hay que
  // poner los campos en cero explícitamente, como hace el test del
  // gemelo.
  AcousticTwinParams zero{};
  zero.dacBits = 0;
  zero.dacSampleRateHz = 0;
  zero.dacBufferFrames = 0;
  zero.roomWidthM = 0.0;
  zero.roomLengthM = 0.0;
  zero.roomHeightM = 0.0;
  zero.roomRt60Sec = 0.0;
  zero.listenerDistanceM = 0.0;
  const AcousticTwinEstimate e0 = AcousticTwin::estimate(zero);
  EXPECT_FALSE(e0.valid);
  const TwinConfidence c0 = est.estimate(e0);
  EXPECT_FALSE(c0.valid);
  EXPECT_FLOAT_EQ(c0.global, 0.0f);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  EXPECT_FALSE(est.canControlFine());
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kNone);
  EXPECT_TRUE(allConfidencesFiniteAndBounded(c0));

  // Estimación vacía (valid=false por defecto): igual de inválida.
  const AcousticTwinEstimate empty{};
  const TwinConfidence cEmpty = est.estimate(empty);
  EXPECT_FALSE(cEmpty.valid);
  EXPECT_FLOAT_EQ(cEmpty.global, 0.0f);
  EXPECT_FLOAT_EQ(TwinEstimator::maxConservationIntensity(cEmpty), 0.0f);
  EXPECT_FALSE(TwinEstimator::canControlFine(cEmpty));

  // Calibración inválida con estimación válida: fail-closed.
  TwinCalibration bad = TwinCalibration::geneziPz1dEstimated();
  bad.honestConfidence = 1.5;  // fuera de [0,1]
  ASSERT_FALSE(bad.isValid());
  const TwinConfidence cBad = est.estimate(calibratedEstimate(), bad);
  EXPECT_FALSE(cBad.valid);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);

  // Calibración válida pero inconsistente con la estimación (la
  // estimación no la aplicó): fail-closed.
  const AcousticTwinEstimate plain =
      AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
  ASSERT_TRUE(plain.valid);
  ASSERT_FALSE(plain.calibrationApplied);
  const TwinConfidence cMismatch =
      est.estimate(plain, TwinCalibration::geneziPz1dEstimated());
  EXPECT_FALSE(cMismatch.valid);
}

// ── 4. Calibración medida simulada -> control fino ───────────────────
TEST(TwinEstimator, MeasuredCalibrationEnablesFineControl) {
  const AcousticTwinEstimate e = calibratedEstimate();
  const TwinMeasuredValues m = measuredHigh();
  ASSERT_TRUE(m.isValid());

  TwinEstimator est;
  const TwinConfidence c = est.estimateMeasured(e, m);
  ASSERT_TRUE(c.valid);
  // Todo medido, confianza alta: global ~0.95.
  EXPECT_NEAR(c.global, 0.95, 0.01);
  for (const auto& h1 : c.h1) {
    EXPECT_EQ(h1.method, ConfidenceMethod::kMeasured);
    EXPECT_NEAR(h1.confidence, 0.95, 1e-5);
  }
  EXPECT_EQ(c.h2.method, ConfidenceMethod::kMeasured);
  EXPECT_TRUE(c.h2.valid);
  EXPECT_EQ(c.h3.method, ConfidenceMethod::kMeasured);
  EXPECT_TRUE(c.h3.valid);
  EXPECT_EQ(c.latency.method, ConfidenceMethod::kMeasured);
  EXPECT_EQ(c.roomIr.method, ConfidenceMethod::kMeasured);
  EXPECT_EQ(c.coherence.method, ConfidenceMethod::kMeasured);
  EXPECT_EQ(c.noiseFloor.method, ConfidenceMethod::kMeasured);
  EXPECT_EQ(c.powerHeadroom.method, ConfidenceMethod::kMeasured);
  EXPECT_TRUE(est.canControlFine());
  EXPECT_TRUE(TwinEstimator::canControlFine(c));
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kControl);
  EXPECT_NEAR(est.maxConservationIntensity(), 0.95, 0.01);
  EXPECT_TRUE(allConfidencesFiniteAndBounded(c));

  // Medido pero SIN marcar valid: fail-closed, no controla nada.
  TwinMeasuredValues notValid = m;
  notValid.valid = false;
  ASSERT_FALSE(notValid.isValid());
  const TwinConfidence cNv = est.estimateMeasured(e, notValid);
  EXPECT_FALSE(cNv.valid);
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  EXPECT_FALSE(est.canControlFine());

  // Una calibración MEDIDA (isEstimated=false) sube a kMeasured las
  // variables que cubre, pero h2/h3 siguen sin dato (el barrido de
  // respuesta no identifica Volterra): el control fino sigue bloqueado.
  TwinCalibration measuredCal = TwinCalibration::geneziPz1dEstimated();
  measuredCal.isEstimated = false;
  measuredCal.honestConfidence = 0.95;
  measuredCal.directPathConfidence = 0.95;
  measuredCal.bassConfidence = 0.95;
  measuredCal.midConfidence = 0.95;
  measuredCal.trebleConfidence = 0.95;
  measuredCal.roomConfidence = 0.95;
  measuredCal.valid = measuredCal.isValid();
  ASSERT_TRUE(measuredCal.valid);
  const AcousticTwinEstimate eMeasured =
      AcousticTwin::estimateCalibrated(
          AcousticTwinParams::esseGeneziCalibrated(), measuredCal);
  ASSERT_TRUE(eMeasured.calibrationApplied);
  const TwinConfidence cMeasured = est.estimate(eMeasured, measuredCal);
  ASSERT_TRUE(cMeasured.valid);
  EXPECT_EQ(cMeasured.h1[4].method, ConfidenceMethod::kMeasured);
  EXPECT_NEAR(cMeasured.global, 0.95, 1e-5);
  EXPECT_FALSE(cMeasured.h2.valid);  // Volterra exige Orden 5
  EXPECT_FALSE(est.canControlFine());
}

// ── 5. Valores no finitos -> fail-closed, confianzas finitas ─────────
TEST(TwinEstimator, NonFiniteInputsStayFinite) {
  TwinEstimator est;

  // Ganancia NaN en una banda.
  AcousticTwinEstimate eNan = calibratedEstimate();
  eNan.band[4].gainDb = std::numeric_limits<double>::quiet_NaN();
  const TwinConfidence cNan = est.estimate(eNan);
  EXPECT_FALSE(cNan.valid);
  EXPECT_FLOAT_EQ(cNan.global, 0.0f);
  EXPECT_TRUE(allConfidencesFiniteAndBounded(cNan));
  EXPECT_FLOAT_EQ(est.maxConservationIntensity(), 0.0f);
  EXPECT_FALSE(est.canControlFine());

  // Confianza global infinita.
  AcousticTwinEstimate eInf = calibratedEstimate();
  eInf.overallConfidence = std::numeric_limits<double>::infinity();
  const TwinConfidence cInf = est.estimate(eInf);
  EXPECT_FALSE(cInf.valid);
  EXPECT_TRUE(allConfidencesFiniteAndBounded(cInf));

  // Latencia NaN.
  AcousticTwinEstimate eLat = calibratedEstimate();
  eLat.airLatencyMs = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(est.estimate(eLat).valid);

  // Coherencia NaN.
  AcousticTwinEstimate eCoh = calibratedEstimate();
  eCoh.band[0].coherence = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(est.estimate(eCoh).valid);

  // Valores medidos con NaN: isValid() false -> estimado inválido.
  TwinMeasuredValues mNan = measuredHigh();
  mNan.h1Db[0] = std::numeric_limits<float>::quiet_NaN();
  ASSERT_FALSE(mNan.isValid());
  const TwinConfidence cMNan =
      est.estimateMeasured(calibratedEstimate(), mNan);
  EXPECT_FALSE(cMNan.valid);
  EXPECT_TRUE(allConfidencesFiniteAndBounded(cMNan));

  TwinMeasuredValues mInf = measuredHigh();
  mInf.latencyConfidence = std::numeric_limits<float>::infinity();
  ASSERT_FALSE(mInf.isValid());
  EXPECT_FALSE(est.estimateMeasured(calibratedEstimate(), mInf).valid);
}

// ── 6. Medido con confianza media: sin control fino, parcial ─────────
TEST(TwinEstimator, MeasuredMediumConfidenceNoFineControl) {
  TwinMeasuredValues m = measuredHigh();
  for (auto& c : m.h1Confidence) c = 0.60f;
  m.h2Confidence = 0.60f;
  m.h3Confidence = 0.60f;
  m.latencyConfidence = 0.60f;
  m.roomConfidence = 0.60f;
  m.coherenceConfidence = 0.60f;
  m.noiseConfidence = 0.60f;
  m.powerConfidence = 0.60f;
  ASSERT_TRUE(m.isValid());

  TwinEstimator est;
  const TwinConfidence c = est.estimateMeasured(calibratedEstimate(), m);
  ASSERT_TRUE(c.valid);
  EXPECT_NEAR(c.global, 0.60, 1e-5);
  // Medido pero por debajo de 0.80: no decide control fino.
  EXPECT_FALSE(est.canControlFine());
  EXPECT_EQ(est.interventionLevel(), InterventionLevel::kLimitOnly);
  // Pero sí limita la conservación a 0.60 (parcial).
  EXPECT_NEAR(est.maxConservationIntensity(), 0.60, 1e-5);
}

// ── 7. Sin calibración: derivado del modelo, global < 0.50 ───────────
TEST(TwinEstimator, UncalibratedIsDerivedAndLimited) {
  // Trayecto sin calibrar: potencia y sensibilidad desconocidas.
  const AcousticTwinEstimate e =
      AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
  ASSERT_TRUE(e.valid);
  ASSERT_FALSE(e.calibrationApplied);
  ASSERT_FALSE(e.band[4].maxSplKnown);

  TwinEstimator est;
  const TwinConfidence c = est.estimate(e);
  ASSERT_TRUE(c.valid);
  EXPECT_EQ(c.h1[0].method, ConfidenceMethod::kDerived);
  EXPECT_EQ(c.latency.method, ConfidenceMethod::kDerived);
  // Potencia desconocida: sin dato, y eso baja el global por debajo
  // del ~0.46 del gemelo a secas (la conservación necesita potencia).
  EXPECT_FALSE(c.powerHeadroom.valid);
  EXPECT_FLOAT_EQ(c.powerHeadroom.confidence, 0.0f);
  EXPECT_LT(c.global, 0.50f);
  EXPECT_GT(c.global, 0.0f);
  EXPECT_NEAR(est.maxConservationIntensity(), c.global, 1e-6);
  EXPECT_FALSE(est.canControlFine());
  // (La intensidad se comprueba aquí: el bloque de audífonos de abajo
  // sobrescribe el último resultado del estimador.)
  // Audífono: sin sala, la IR es trivialmente cero y válida.
  const AcousticTwinEstimate eHp =
      AcousticTwin::estimate(AcousticTwinParams::esseHeadphones());
  ASSERT_TRUE(eHp.valid);
  const TwinConfidence cHp = est.estimate(eHp);
  ASSERT_TRUE(cHp.valid);
  EXPECT_TRUE(cHp.roomIr.valid);
  EXPECT_FLOAT_EQ(cHp.roomIr.estimate, 0.0f);
  EXPECT_TRUE(cHp.coherence.valid);
  EXPECT_NEAR(cHp.coherence.estimate, 1.0, 1e-5);
}
