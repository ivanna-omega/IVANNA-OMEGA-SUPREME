// action_simulator.hpp — Simulador de acciones M5 (plan del salto, Luna).
//
// Cada acción candidata se prueba contra el gemelo ANTES de aplicarse:
// se predice su efecto en el campo perceptual con OperatorSimulator
// (reutilizado, no duplicado) y se puntúa con la función de costo J de
// Luna:
//
//   J = w_p·ΔP − w_a·||Δa||² − w_φ·E_phase − w_t·E_transient
//           − w_n·E_noise − w_d·E_distortion − w_l·E_latency
//
// ΔP es la mejora predicha del campo (media por banda de la suma
// ponderada de las dimensiones; claridad, naturalidad, impacto,
// espacialidad y estabilidad suman; fatiga resta porque bajarla es
// mejora; audibilidad, confianza y presupuesto no cuentan como mejora:
// la audibilidad es huella, no ganancia, y el presupuesto ya lo cobra
// ||Δa||²). Los pesos de ΔP y los w_* son NOMINALES, no medidos:
// se calibran con ABX (Orden 6). Ningún número de este archivo es una
// medición del motor real.
//
// Términos de penalización (derivación honesta desde los inputs de
// evaluate()):
//   E_noise      = max(0, noiseIncreaseDb)   [dB]
//   E_distortion = max(0, thdIncreaseDb)     [dB]
//   E_latency    = max(0, latencyIncreaseMs) [ms]
//   E_transient  = max(0, −Δimpacto medio predicho) — pérdida de
//                  transitorio que predice el simulador. Entra dos
//                  veces por diseño de la fórmula (en ΔP y aquí): w_t
//                  es el peso EXTRA sobre el daño al transitorio.
//   E_phase      = 0. La API de evaluate() no recibe desviación de
//                  fase: no hay telemetría de fase hasta las Órdenes
//                  1–2, así que no se puede verificar aquí ni
//                  inventar un modelo. El límite nominal
//                  kMaxPhaseDeviationDeg queda declarado para M6, que
//                  debe aplicar el chequeo cuando la telemetría exista.
//
// Restricciones duras (Luna M5). Si alguna se viola, allowed=false:
//   true peak <= -1.0 dBTP · THD +<= 1 dB · ruido +<= 1 dB ·
//   rate <= 0.20 por ciclo de control · EQ boost <= 0 dB (sala) ·
//   ganancia neta de sala <= 0 dB · latencia +<= 1 ms ·
//   desviación de fase <= límite calibrado.
// Además: gemelo inválido o global < 0.20 → rechazado; cualquier input
// no finito → rechazado (fail-closed). La acción cero siempre es
// allowed con J = 0.
//
// Notas de implementación (contrato):
// - La acción ES el delta por ciclo de control (perceptual_vector.hpp:
//   "delta por operador en [-1, 1]"), así que el rate es max|a_op|.
// - Sala cut-only: sin mapa calibrado delta→dB (Orden 3 pendiente),
//   cualquier delta POSITIVO del operador SalaViva es ganancia neta
//   > 0 dB y se rechaza; los cortes (delta negativo) se permiten. Las
//   dos restricciones de sala colapsan en este único chequeo hasta que
//   exista la ganancia de sala por banda medida.
// - J es la puntuación SIN el gate: se calcula igual para una acción
//   rechazada (salvo input no finito, donde J = 0). `allowed` es el
//   gate; M6 debe respetar `allowed` y nunca hacer argmax solo con J.
// - Precedencia de los chequeos: (1) input no finito → rechazo;
//   (2) acción cero → allowed, J = 0; (3) gemelo sin confianza →
//   rechazo; (4) restricciones duras en el orden del plan; (5) resto
//   → allowed con su J.
// - Sin malloc, sin locks, sin I/O: evaluate() es pura y const; el
//   OperatorSimulator interno no tiene estado mutable.

#pragma once

#include <array>
#include <cstddef>

#include "../perceptual_mpc/operator_simulator.hpp"
#include "../perceptual_mpc/perceptual_vector.hpp"
#include "twin_confidence.hpp"

namespace ivanna::closed_loop {

struct ActionScore {
  double J = 0.0;         // coste sin el gate (ver nota en el header)
  bool allowed = false;   // false = la acción NO se aplica
  const char* reason = "";  // literal estático; "ok" si allowed
};

class ActionSimulator {
 public:
  // ── Reasons (literales estáticos; seguro devolverlos por valor) ────
  static constexpr const char* kReasonOk = "ok";
  static constexpr const char* kReasonNonFiniteInput = "non-finite input";
  static constexpr const char* kReasonTwinNoConfidence = "twin sin confianza";
  static constexpr const char* kReasonTruePeak = "true peak > -1.0 dBTP";
  static constexpr const char* kReasonThd = "THD increase > 1 dB";
  static constexpr const char* kReasonNoise = "noise increase > 1 dB";
  static constexpr const char* kReasonRate =
      "parameter rate > 0.20 per control cycle";
  static constexpr const char* kReasonRoomGain =
      "room gain > 0 dB (cut-only)";
  static constexpr const char* kReasonLatency = "latency increase > 1 ms";
  static constexpr const char* kReasonPhase =
      "phase deviation > calibrated limit";

  // ── Restricciones duras (Luna M5; límites inclusivos: == pasa) ──────
  static constexpr double kMaxTruePeakDbTp = -1.0;     // dBTP
  static constexpr double kMaxThdIncreaseDb = 1.0;     // dB
  static constexpr double kMaxNoiseIncreaseDb = 1.0;   // dB
  static constexpr double kMaxRatePerCycle = 0.20;     // |Δa| por operador
  static constexpr double kMaxRoomGainDb = 0.0;        // cut-only
  static constexpr double kMaxLatencyIncreaseMs = 1.0; // ms
  // NOMINAL, calibrar con medición (Órdenes 1–2). Sin entrada de fase en
  // evaluate() todavía: no verificable aquí (ver nota del header).
  static constexpr double kMaxPhaseDeviationDeg = 30.0;  // grados

  // ── Pesos de J — NOMINALES, calibrar con ABX (Orden 6) ──────────────
  // Escala: un ΔP típico por acción es O(0.01), así que las penalizaciones
  // son O(0.1) por unidad para que 1 dB de ruido/THD o 1 ms de latencia
  // pesen más que una ganancia predicha pequeña (seguridad antes que
  // ganancia). w_p domina (1.0); el resto frena, no manda.
  static constexpr double kWeightPerceptual = 1.0;  // w_p
  static constexpr double kWeightAction = 0.10;     // w_a, por ||Δa||²
  static constexpr double kWeightPhase = 0.10;      // w_phi
  static constexpr double kWeightTransient = 0.10;  // w_t
  static constexpr double kWeightNoise = 0.10;      // w_n, por dB
  static constexpr double kWeightDistortion = 0.10; // w_d, por dB
  static constexpr double kWeightLatency = 0.05;    // w_l, por ms

  // Ganancia de cada dimensión en ΔP (NOMINAL, calibrar con ABX,
  // Orden 6). Orden: audibilidad, claridad, naturalidad, impacto,
  // espacialidad, fatiga, estabilidad, confianza, presupuesto.
  // Fatiga −1: bajarla es mejora. Audibilidad/confianza/presupuesto 0:
  // no son mejora perceptual (ver nota del header).
  static constexpr std::array<double, perceptual_mpc::kNumDims> kDimGain = {
      0.0, 1.0, 1.0, 1.0, 1.0, -1.0, 1.0, 0.0, 0.0};

  // Evalúa una acción candidata. truePeakDbTp es el true peak PREDICHO
  // tras la acción (dBTP absoluto); thd/noise/latencyIncrease* son los
  // INCREMENTOS predichos que la acción causaría (pueden ser negativos
  // si la acción mejora ese aspecto).
  ActionScore evaluate(
      const perceptual_mpc::PerceptualField& field,
      const perceptual_mpc::Action& action, const TwinConfidence& twin,
      double truePeakDbTp, double thdIncreaseDb, double noiseIncreaseDb,
      double latencyIncreaseMs) const;

  // Mejora perceptual predicha ΔP (media por banda). Pública para
  // auditoría (M6/M8): es el término w_p·ΔP de J, sin penalizaciones.
  double predictedDeltaP(const perceptual_mpc::PerceptualField& field,
                         const perceptual_mpc::Action& action) const;

  // Coste J sin restricciones (la puntuación sin el gate). Público para
  // auditoría: evaluate() lo usa para rellenar ActionScore::J.
  double cost(const perceptual_mpc::PerceptualField& field,
              const perceptual_mpc::Action& action, double thdIncreaseDb,
              double noiseIncreaseDb, double latencyIncreaseMs) const;

 private:
  // ΔP entre un campo y su predicción (suma ponderada por banda).
  static double deltaP(const perceptual_mpc::PerceptualField& current,
                       const perceptual_mpc::PerceptualField& predicted);
  // Pérdida de transitorio predicha: max(0, −Δimpacto medio).
  static double transientLoss(
      const perceptual_mpc::PerceptualField& current,
      const perceptual_mpc::PerceptualField& predicted);
  static bool isZeroAction(const perceptual_mpc::Action& action);
  static bool isFiniteAction(const perceptual_mpc::Action& action);

  perceptual_mpc::OperatorSimulator sim_;
};

}  // namespace ivanna::closed_loop
