// twin_estimator.hpp — M3: gemelo acústico como estimador puro con
// confianza por variable (plan del salto, Luna).
//
// El estimador llena TwinConfidence desde el gemelo existente
// (acoustic_twin): NO reestima el trayecto, NO procesa audio, NO toca
// ningún buffer. Es una función pura sobre AcousticTwinEstimate (+ la
// calibración que la produjo, o valores medidos inyectados).
//
// REGLA DURA (Luna, M3): la calibración actual es ESTIMADA por
// datasheet (confianza honesta 0.70, method kDatasheet/kDerived). Esa
// confianza SOLO limita la intensidad máxima de conservación
// (maxConservationIntensity). NUNCA decide EQ fino, fase, Volterra ni
// sala: canControlFine() devuelve false mientras no haya medición real
// (method != kMeasured en TODAS las variables que el control fino
// necesita, h2/h3 incluidos). El camino medido existe
// (estimateMeasured + TwinMeasuredValues) pero queda fail-closed: sin
// datos medidos, su confianza es 0 y no controla nada.
//
// GLOBAL — mínimo ponderado NOMINAL (no medido; se calibra con ABX,
// Orden 6). El global es la media ponderada de las variables que la
// CONSERVACIÓN necesita; h2/h3 NO entran: el control fino (Volterra)
// exige medición de todos modos (canControlFine), así que la
// incertidumbre de los kernels no debe bloquear la conservación, y su
// confianza se lee por variable (hoy 0: no identificados, Orden 5).
// Pesos NOMINALES (suman 1.00):
//   h1 (respuesta por banda)     0.30  — la conservación decide por
//                                        energía por banda.
//   powerHeadroom (techo SPL)    0.20  — sin potencia conocida no se
//                                        sabe cuánto se puede pedir.
//   roomIr (RT60 de referencia)  0.15  — energía reverberante.
//   latency (buffer + aire)      0.10
//   coherence (directo/reverb)   0.10
//   noiseFloor (piso de ruido)   0.15
// La confianza de la variable H1 es la confianza BASE (honesta de la
// calibración, o media de bandas sin calibración); h1[i] detalla la
// incertidumbre por banda (graves 0.65 / medios 0.70 / agudos 0.60 en
// la P3 estimada). Que la media de las bandas (0.656) quede por debajo
// del 0.70 global es intencionado: cada banda individual carga
// incertidumbre modal adicional, mientras la FORMA global de la
// respuesta se afirma a la confianza honesta 0.70 del JSON.
//
// VARIANZA — nominal, no medida: variance = (1 - confidence)^2, en
// las unidades al cuadrado del estimate (dB^2, ms^2, s^2...). A
// confianza 0.70 la desviación típica nominal es 0.3 unidades.
// Cuando la calibración no distingue una variable (el JSON P3 solo
// distingue por región de frecuencia, no por variable), la variable
// hereda la confianza honesta global 0.70 — no se inventa un reparto
// por variable que el JSON no afirma.
//
// MÉTODO — procedencia:
//   kDerived   sin calibración: modelo físico del gemelo.
//   kDatasheet con calibración estimada (isEstimated == true).
//   kMeasured  con calibración medida (isEstimated == false) o con
//              TwinMeasuredValues. h2/h3 NUNCA salen de la calibración:
//              el barrido de respuesta no identifica kernels Volterra
//              (Orden 5); solo TwinMeasuredValues los cubre.
//   kUnknown   sin dato (h2/h3 estimados, powerHeadroom sin potencia).
//
// EDAD — la estimación se produce en el momento de la llamada:
// age = 0.0 siempre. La expiración de una calibración vieja o el
// cambio de ruta los juzga M8 (uncertainty_supervisor), no este
// estimador: aquí no se inventa un TTL.
//
// Fail-closed: estimación inválida, calibración inválida o inconsistente
// con la estimación, valores medidos inválidos, o cualquier valor no
// finito en la entrada -> TwinConfidence inválido (valid=false,
// global=0, todas las confianzas 0 y finitas), intensidad 0, sin
// control fino.

#pragma once

#include <array>
#include <cstddef>

#include "../acoustic_twin/acoustic_twin.hpp"
#include "twin_confidence.hpp"

namespace ivanna::closed_loop {

// Pesos NOMINALES del global (ver cabecera del archivo). Suman 1.00.
inline constexpr float kTwinWeightH1 = 0.30f;
inline constexpr float kTwinWeightPowerHeadroom = 0.20f;
inline constexpr float kTwinWeightRoomIr = 0.15f;
inline constexpr float kTwinWeightLatency = 0.10f;
inline constexpr float kTwinWeightCoherence = 0.10f;
inline constexpr float kTwinWeightNoiseFloor = 0.15f;

// Valores medidos del trayecto (Órdenes 1–5: loopback, barrido del
// transductor, IR de sala, potencia, no linealidad). Los puebla quien
// carga la medición real (la app, o el test que la simula); el
// estimador NO mide nada. Sin estos datos, estimateMeasured() es
// fail-closed (confianza 0, no controla nada).
struct TwinMeasuredValues {
  // H1 medido por banda (dB) + confianza por banda (0..1).
  std::array<float, kTwinNumBands> h1Db{};
  std::array<float, kTwinNumBands> h1Confidence{};
  // Kernels no lineales identificados por armónicos (Orden 5).
  // h2/h3 son los coeficientes identificados, no el THD total.
  float h2 = 0.0f;
  float h2Confidence = 0.0f;
  float h3 = 0.0f;
  float h3Confidence = 0.0f;
  // Latencia total medida D (ms).
  float latencyMs = 0.0f;
  float latencyConfidence = 0.0f;
  // RT60 medido de referencia, 500 Hz–1 kHz (s).
  float roomRt60Sec = 0.0f;
  float roomConfidence = 0.0f;
  // Coherencia L/R medida (0..1).
  float coherence = 0.0f;
  float coherenceConfidence = 0.0f;
  // Piso de ruido medido (dB SPL).
  float noiseFloorDbSpl = 0.0f;
  float noiseConfidence = 0.0f;
  // Techo de potencia medido: SPL máximo de la banda más débil (dB SPL).
  float powerHeadroomDbSpl = 0.0f;
  float powerConfidence = 0.0f;
  // true = estos valores son mediciones reales (no estimaciones).
  bool valid = false;

  // Validación fail-closed: finitos, confianzas en [0,1], rangos cuerdos.
  bool isValid() const noexcept;
};

// Estimador puro del gemelo: AcousticTwinEstimate -> TwinConfidence.
// Sin estado global, sin I/O, sin tocar audio. Guarda solo el último
// TwinConfidence producido (para maxConservationIntensity() /
// canControlFine() sin argumentos); una instancia NO es segura para
// compartir entre hilos sin sincronización externa.
class TwinEstimator {
 public:
  TwinEstimator() = default;

  // Estima desde el gemelo. Procedencia: kDatasheet si la estimación
  // trae calibración aplicada, kDerived si no. Si la calibración
  // aplicada era MEDIDA, usar la sobrecarga con TwinCalibration: esta
  // sobrecarga no puede distinguirla y la etiquetaría kDatasheet.
  TwinConfidence estimate(
      const ivanna::fused::acoustic_twin::AcousticTwinEstimate& estimate);

  // Estima con la calibración que produjo la estimación. La
  // procedencia sale de cal.isEstimated (true -> kDatasheet,
  // false -> kMeasured). Fail-closed: calibración inválida, o
  // inconsistente con la estimación (no aplicada, o confianza honesta
  // distinta), -> TwinConfidence inválido. Una calibración medida
  // sube a kMeasured las variables que cubre (H1, latencia, sala,
  // potencia...), pero h2/h3 SIGUEN sin dato: el barrido de respuesta
  // no identifica kernels Volterra (Orden 5), así que canControlFine()
  // sigue en false hasta tener TwinMeasuredValues con h2/h3.
  TwinConfidence estimate(
      const ivanna::fused::acoustic_twin::AcousticTwinEstimate& estimate,
      const ivanna::fused::acoustic_twin::TwinCalibration& calibration);

  // Estima desde valores MEDIDOS (Órdenes 1–5). Todas las variables
  // salen con method kMeasured y las confianzas inyectadas. Fail-closed:
  // estimación inválida o valores medidos inválidos -> TwinConfidence
  // inválido. La discordancia gemelo/medición NO se juzga aquí: la
  // publica M8 (uncertainty_supervisor) a partir de este resultado.
  TwinConfidence estimateMeasured(
      const ivanna::fused::acoustic_twin::AcousticTwinEstimate& estimate,
      const TwinMeasuredValues& measured);

  // Última confianza producida (inválida hasta la primera estimate*).
  TwinConfidence lastConfidence() const { return last_; }

  // Intensidad máxima de conservación que la última confianza permite:
  // 0 si inválida o global < kConfidenceZeroAction (0.20); en otro
  // caso el global (0..1). A 0.70 estimado -> 0.70, parcial (< 1).
  // Coherente con AcousticTwin::interventionScale (= confianza).
  float maxConservationIntensity() const;
  // ¿Puede la última confianza decidir control FINO (EQ fino, fase,
  // Volterra, sala)? Solo con medición real en TODAS las variables y
  // global >= kConfidenceHigh (0.80).
  bool canControlFine() const;
  // Nivel de intervención de la última confianza (umbrales nominales
  // 0.80 / 0.50 de twin_confidence.hpp).
  InterventionLevel interventionLevel() const;

  // Versiones puras sobre un TwinConfidence dado (para M5/M6/M8, que
  // no necesitan una instancia del estimador).
  static float maxConservationIntensity(const TwinConfidence& conf);
  static bool canControlFine(const TwinConfidence& conf);
  static InterventionLevel interventionLevel(const TwinConfidence& conf);

 private:
  TwinConfidence last_{};
};

}  // namespace ivanna::closed_loop
