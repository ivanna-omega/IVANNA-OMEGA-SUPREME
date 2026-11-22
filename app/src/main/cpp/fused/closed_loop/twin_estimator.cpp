// twin_estimator.cpp — M3: gemelo acústico como estimador puro con
// confianza por variable (plan del salto, Luna).
//
// Ver twin_estimator.hpp para el contrato, la ponderación NOMINAL del
// global y las reglas duras. Este archivo solo traduce
// AcousticTwinEstimate (+ calibración / valores medidos) a
// TwinConfidence, con saneo fail-closed: ningún valor no finito ni
// fuera de rango cuerdo llega a la salida.

#include "twin_estimator.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::closed_loop {
namespace {

using ivanna::fused::acoustic_twin::AcousticTwinEstimate;
using ivanna::fused::acoustic_twin::TwinCalibration;

// ── Saneo ────────────────────────────────────────────────────────────

float clampf(float x, float lo, float hi) noexcept {
  return std::max(lo, std::min(hi, x));
}

// Confianza saneada a [0,1]; NaN/inf -> 0 (nunca propaga no finitos).
float cleanConfidence(double c) noexcept {
  if (!std::isfinite(c)) return 0.0f;
  return clampf(static_cast<float>(c), 0.0f, 1.0f);
}

// Varianza NOMINAL (ver .hpp): (1 - confidence)^2, en las unidades al
// cuadrado del estimate. Finita siempre que la confianza lo sea.
float varianceNominal(float confidence) noexcept {
  const float d = 1.0f - clampf(confidence, 0.0f, 1.0f);
  return d * d;
}

ParameterConfidence makeParam(float estimate, float confidence,
                              ConfidenceMethod method, bool valid) noexcept {
  ParameterConfidence p;
  p.estimate = estimate;
  p.variance = valid ? varianceNominal(confidence) : 0.0f;
  p.age = 0.0f;  // la estimación se produce en el momento de la llamada
  p.method = method;
  p.confidence = valid ? clampf(confidence, 0.0f, 1.0f) : 0.0f;
  p.valid = valid;
  return p;
}

// Sin dato: estimate 0, varianza 0, confianza 0, method kUnknown.
// (No es "cero medido": es ausencia de dato; valid=false lo dice.)
ParameterConfidence noData() noexcept {
  return makeParam(0.0f, 0.0f, ConfidenceMethod::kUnknown, false);
}

// TwinConfidence inválido: todo a cero, todo finito.
TwinConfidence invalidConfidence() noexcept { return TwinConfidence{}; }

// ¿La estimación del gemelo trae solo valores finitos y en rango cuerdo
// en los campos que el estimador lee? Un solo NaN/inf o un valor
// imposible -> inválido (fail-closed, test 5).
bool estimateSane(const AcousticTwinEstimate& e) noexcept {
  if (!e.valid) return false;
  if (!std::isfinite(e.overallConfidence) ||
      !std::isfinite(e.calibrationConfidence) ||
      !std::isfinite(e.bufferLatencyMs) || !std::isfinite(e.airLatencyMs)) {
    return false;
  }
  if (e.overallConfidence < 0.0 || e.overallConfidence > 1.0) return false;
  if (e.calibrationConfidence < 0.0 || e.calibrationConfidence > 1.0) {
    return false;
  }
  // Latencias totales fuera de lo físico (buffer+aire < 5 s).
  if (e.bufferLatencyMs < 0.0 || e.bufferLatencyMs > 5000.0 ||
      e.airLatencyMs < 0.0 || e.airLatencyMs > 5000.0) {
    return false;
  }
  for (std::size_t i = 0; i < e.band.size(); ++i) {
    const auto& b = e.band[i];
    if (!std::isfinite(b.gainDb) || !std::isfinite(b.latencyMs) ||
        !std::isfinite(b.noiseDbSpl) || !std::isfinite(b.rt60Sec) ||
        !std::isfinite(b.coherence) || !std::isfinite(b.maxSplDb) ||
        !std::isfinite(b.confidence)) {
      return false;
    }
    // Rangos cuerdos (guardas de saneo, no física fina).
    if (b.gainDb < -200.0 || b.gainDb > 200.0) return false;
    if (b.latencyMs < 0.0 || b.latencyMs > 5000.0) return false;
    if (b.noiseDbSpl < -20.0 || b.noiseDbSpl > 140.0) return false;
    if (b.rt60Sec < 0.0 || b.rt60Sec > 30.0) return false;
    if (b.coherence < 0.0 || b.coherence > 1.0) return false;
    if (b.maxSplDb < 0.0 || b.maxSplDb > 200.0) return false;
    if (b.confidence < 0.0 || b.confidence > 1.0) return false;
  }
  return true;
}

// Global NOMINAL: media ponderada de las variables que la conservación
// necesita (pesos en twin_estimator.hpp). h2/h3 NO entran (ver .hpp).
// h1Scalar es la confianza de la variable H1 (base honesta / media de
// bandas); el resto son confianzas por variable (0 si la variable no
// tiene dato, p. ej. potencia desconocida: eso BAJA el global, como
// debe ser — sin potencia conocida la conservación se limita).
float globalWeighted(float h1Scalar, float latency, float roomIr,
                     float coherence, float noiseFloor,
                     float powerHeadroom) noexcept {
  const float g = kTwinWeightH1 * h1Scalar +
                  kTwinWeightLatency * latency +
                  kTwinWeightRoomIr * roomIr +
                  kTwinWeightCoherence * coherence +
                  kTwinWeightNoiseFloor * noiseFloor +
                  kTwinWeightPowerHeadroom * powerHeadroom;
  return clampf(g, 0.0f, 1.0f);
}

}  // namespace

// ── TwinMeasuredValues ───────────────────────────────────────────────

bool TwinMeasuredValues::isValid() const noexcept {
  if (!valid) return false;
  for (std::size_t i = 0; i < kTwinNumBands; ++i) {
    if (!std::isfinite(h1Db[i]) || !std::isfinite(h1Confidence[i])) {
      return false;
    }
    if (h1Db[i] < -200.0f || h1Db[i] > 200.0f) return false;
    if (h1Confidence[i] < 0.0f || h1Confidence[i] > 1.0f) return false;
  }
  const float confs[7] = {h2Confidence, h3Confidence, latencyConfidence,
                          roomConfidence, coherenceConfidence, noiseConfidence,
                          powerConfidence};
  for (float c : confs) {
    if (!std::isfinite(c) || c < 0.0f || c > 1.0f) return false;
  }
  if (!std::isfinite(h2) || !std::isfinite(h3) ||
      !std::isfinite(latencyMs) || !std::isfinite(roomRt60Sec) ||
      !std::isfinite(coherence) || !std::isfinite(noiseFloorDbSpl) ||
      !std::isfinite(powerHeadroomDbSpl)) {
    return false;
  }
  if (latencyMs < 0.0f || latencyMs > 5000.0f) return false;
  if (roomRt60Sec < 0.0f || roomRt60Sec > 30.0f) return false;
  if (coherence < 0.0f || coherence > 1.0f) return false;
  if (noiseFloorDbSpl < -20.0f || noiseFloorDbSpl > 140.0f) return false;
  if (powerHeadroomDbSpl < 0.0f || powerHeadroomDbSpl > 200.0f) return false;
  return true;
}

// ── TwinEstimator ────────────────────────────────────────────────────

TwinConfidence TwinEstimator::estimate(const AcousticTwinEstimate& e) {
  if (!estimateSane(e)) {
    last_ = invalidConfidence();
    return last_;
  }
  // Procedencia: la estimación trae calibración aplicada -> el gemelo
  // ya impuso su confianza honesta (0.70 en la P3 estimada); sin ella,
  // el modelo físico a secas. Esta sobrecarga no distingue una
  // calibración medida: para ella, usar estimate(e, cal).
  const ConfidenceMethod method = e.calibrationApplied
                                      ? ConfidenceMethod::kDatasheet
                                      : ConfidenceMethod::kDerived;
  const float base = e.calibrationApplied
                         ? cleanConfidence(e.calibrationConfidence)
                         : cleanConfidence(e.overallConfidence);

  TwinConfidence out;
  for (std::size_t i = 0; i < kTwinNumBands; ++i) {
    const auto& b = e.band[i];
    out.h1[i] = makeParam(static_cast<float>(b.gainDb),
                          cleanConfidence(b.confidence), method, true);
  }
  // h2/h3: el gemelo NO identifica kernels Volterra (Orden 5 pendiente).
  // Su THD% es un proxy de modelo, no un kernel: no se publica como h2.
  out.h2 = noData();
  out.h3 = noData();
  out.latency = makeParam(
      static_cast<float>(e.bufferLatencyMs + e.airLatencyMs), base, method,
      true);
  // RT60 de referencia a 1 kHz (banda 4). En audífono es 0: no hay sala,
  // la IR es trivialmente cero y el dato sigue siendo válido.
  out.roomIr = makeParam(static_cast<float>(e.band[4].rt60Sec), base, method,
                         true);
  double cohSum = 0.0, noiseSum = 0.0;
  for (const auto& b : e.band) {
    cohSum += b.coherence;
    noiseSum += b.noiseDbSpl;
  }
  const auto n = static_cast<double>(e.band.size());
  out.coherence = makeParam(static_cast<float>(cohSum / n), base, method,
                            true);
  out.noiseFloor = makeParam(static_cast<float>(noiseSum / n), base, method,
                             true);
  // Techo de potencia: SPL máximo de la banda más débil (la que clipea
  // primero limita). Sin potencia/sensibilidad conocidas (maxSplKnown
  // false en alguna banda) no hay dato: sin dato, no se controla.
  bool powerKnown = true;
  double minMaxSpl = 0.0;
  for (std::size_t i = 0; i < e.band.size(); ++i) {
    if (!e.band[i].maxSplKnown) {
      powerKnown = false;
      break;
    }
    if (i == 0 || e.band[i].maxSplDb < minMaxSpl) {
      minMaxSpl = e.band[i].maxSplDb;
    }
  }
  out.powerHeadroom = powerKnown
                          ? makeParam(static_cast<float>(minMaxSpl), base,
                                      method, true)
                          : noData();
  out.global = globalWeighted(base, base, base, base, base,
                              powerKnown ? base : 0.0f);
  out.valid = true;
  last_ = out;
  return out;
}

TwinConfidence TwinEstimator::estimate(const AcousticTwinEstimate& e,
                                       const TwinCalibration& cal) {
  if (!estimateSane(e)) {
    last_ = invalidConfidence();
    return last_;
  }
  // La calibración que se pasa debe ser LA QUE PRODUJO la estimación:
  // válida, aplicada, y con la misma confianza honesta. Mezclar una
  // estimación con otra calibración es un bug del llamador y se
  // rechaza (fail-closed). La validación profunda vive en
  // TwinCalibration::isValid() y ya la corrió AcousticTwin::
  // estimateCalibrated() antes de aplicar la calibración; aquí se
  // exige el flag valid + finitud de los campos usados.
  if (!cal.valid || !e.calibrationApplied ||
      !std::isfinite(cal.honestConfidence) || cal.honestConfidence < 0.0 ||
      cal.honestConfidence > 1.0 ||
      std::fabs(e.calibrationConfidence - cal.honestConfidence) > 1e-6) {
    last_ = invalidConfidence();
    return last_;
  }
  const ConfidenceMethod method = cal.isEstimated
                                      ? ConfidenceMethod::kDatasheet
                                      : ConfidenceMethod::kMeasured;
  const float base = cleanConfidence(cal.honestConfidence);

  TwinConfidence out;
  for (std::size_t i = 0; i < kTwinNumBands; ++i) {
    const auto& b = e.band[i];
    out.h1[i] = makeParam(static_cast<float>(b.gainDb),
                          cleanConfidence(b.confidence), method, true);
  }
  // Medida o no, la calibración de respuesta NO identifica kernels
  // Volterra (Orden 5): h2/h3 siguen sin dato.
  out.h2 = noData();
  out.h3 = noData();
  out.latency = makeParam(
      static_cast<float>(e.bufferLatencyMs + e.airLatencyMs), base, method,
      true);
  out.roomIr = makeParam(static_cast<float>(e.band[4].rt60Sec), base, method,
                         true);
  double cohSum = 0.0, noiseSum = 0.0;
  for (const auto& b : e.band) {
    cohSum += b.coherence;
    noiseSum += b.noiseDbSpl;
  }
  const auto n = static_cast<double>(e.band.size());
  out.coherence = makeParam(static_cast<float>(cohSum / n), base, method,
                            true);
  out.noiseFloor = makeParam(static_cast<float>(noiseSum / n), base, method,
                             true);
  bool powerKnown = true;
  double minMaxSpl = 0.0;
  for (std::size_t i = 0; i < e.band.size(); ++i) {
    if (!e.band[i].maxSplKnown) {
      powerKnown = false;
      break;
    }
    if (i == 0 || e.band[i].maxSplDb < minMaxSpl) {
      minMaxSpl = e.band[i].maxSplDb;
    }
  }
  out.powerHeadroom = powerKnown
                          ? makeParam(static_cast<float>(minMaxSpl), base,
                                      method, true)
                          : noData();
  out.global = globalWeighted(base, base, base, base, base,
                              powerKnown ? base : 0.0f);
  out.valid = true;
  last_ = out;
  return out;
}

TwinConfidence TwinEstimator::estimateMeasured(const AcousticTwinEstimate& e,
                                               const TwinMeasuredValues& m) {
  if (!estimateSane(e) || !m.isValid()) {
    last_ = invalidConfidence();
    return last_;
  }
  constexpr ConfidenceMethod kMethod = ConfidenceMethod::kMeasured;
  TwinConfidence out;
  float h1ConfSum = 0.0f;
  for (std::size_t i = 0; i < kTwinNumBands; ++i) {
    out.h1[i] = makeParam(m.h1Db[i], m.h1Confidence[i], kMethod, true);
    h1ConfSum += m.h1Confidence[i];
  }
  const float h1Scalar =
      h1ConfSum / static_cast<float>(kTwinNumBands);
  out.h2 = makeParam(m.h2, m.h2Confidence, kMethod, true);
  out.h3 = makeParam(m.h3, m.h3Confidence, kMethod, true);
  out.latency = makeParam(m.latencyMs, m.latencyConfidence, kMethod, true);
  out.roomIr = makeParam(m.roomRt60Sec, m.roomConfidence, kMethod, true);
  out.coherence = makeParam(m.coherence, m.coherenceConfidence, kMethod,
                            true);
  out.noiseFloor = makeParam(m.noiseFloorDbSpl, m.noiseConfidence, kMethod,
                             true);
  out.powerHeadroom = makeParam(m.powerHeadroomDbSpl, m.powerConfidence,
                                kMethod, true);
  out.global = globalWeighted(h1Scalar, m.latencyConfidence, m.roomConfidence,
                              m.coherenceConfidence, m.noiseConfidence,
                              m.powerConfidence);
  out.valid = true;
  last_ = out;
  return out;
}

// ── Intensidad / control fino / nivel ────────────────────────────────

float TwinEstimator::maxConservationIntensity(const TwinConfidence& conf) {
  if (!conf.valid) return 0.0f;
  if (!std::isfinite(conf.global) || conf.global < kConfidenceZeroAction) {
    return 0.0f;
  }
  return clampf(conf.global, 0.0f, 1.0f);
}

bool TwinEstimator::canControlFine(const TwinConfidence& conf) {
  if (!conf.valid) return false;
  if (!std::isfinite(conf.global) || conf.global < kConfidenceHigh) {
    return false;
  }
  // Control fino exige MEDICIÓN REAL en todas las variables que lo
  // necesitan: H1 por banda, h2, h3, latencia, sala, coherencia, ruido
  // y potencia. Una sola variable estimada lo bloquea.
  for (const auto& h1 : conf.h1) {
    if (!h1.valid || h1.method != ConfidenceMethod::kMeasured) return false;
  }
  const ParameterConfidence* rest[7] = {&conf.h2, &conf.h3, &conf.latency,
                                        &conf.roomIr, &conf.coherence,
                                        &conf.noiseFloor,
                                        &conf.powerHeadroom};
  for (const ParameterConfidence* p : rest) {
    if (!p->valid || p->method != ConfidenceMethod::kMeasured) return false;
  }
  return true;
}

InterventionLevel TwinEstimator::interventionLevel(const TwinConfidence& conf) {
  if (!conf.valid || !std::isfinite(conf.global)) {
    return InterventionLevel::kNone;
  }
  if (conf.global >= kConfidenceHigh) return InterventionLevel::kControl;
  if (conf.global >= kConfidenceMedium) return InterventionLevel::kLimitOnly;
  return InterventionLevel::kNone;
}

float TwinEstimator::maxConservationIntensity() const {
  return maxConservationIntensity(last_);
}

bool TwinEstimator::canControlFine() const { return canControlFine(last_); }

InterventionLevel TwinEstimator::interventionLevel() const {
  return interventionLevel(last_);
}

}  // namespace ivanna::closed_loop
