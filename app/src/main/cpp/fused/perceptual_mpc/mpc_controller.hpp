// mpc_controller.hpp — Controlador predictivo perceptual (MPC) en MODO SOMBRA.
//
// El MPC observa el campo P[t], genera acciones candidatas, simula el
// futuro con OperatorSimulator, puntúa cada futuro con la función objetivo
// y devuelve la mejor acción. NO la aplica: la decisión solo se registra
// (modo sombra). El audio nunca pasa por aquí; processShadow() copia el
// buffer bit-exacto y corre un paso del MPC al lado, para demostrar que la
// sombra no modifica la señal.
//
// Límites duros (nunca se violan, aunque el objetivo lo pida):
//   - Tasa de cambio: |a[t]-a[t-1]| <= maxDeltaPerCycle por operador.
//   - Techo de presupuesto: la predicción no puede superar maxBudget.
//   - Histéresis: no se cambia de acción si la mejora es menor que
//     improvementThreshold (evita el chattering entre operadores).
//   - Modo seguro: confianza media < minConfidence, o campo no finito,
//     o candidato inválido -> acción cero, determinista.
//
// La generación de candidatos es determinista (PRNG splitmix64 con semilla
// fija + candidatos estructurados): el mismo estado previo produce siempre
// los mismos candidatos, así que un test puede reproducir el argmax.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "operator_simulator.hpp"
#include "perceptual_vector.hpp"

namespace ivanna::perceptual_mpc {

struct MpcConfig {
  double maxDeltaPerCycle = 0.20;      // |a[t]-a[t-1]| máximo por operador
  double improvementThreshold = 0.01;  // histéresis: mejora mínima para cambiar
  std::size_t numCandidates = 24;      // candidatos por paso (tope interno 256)
  std::size_t horizon = 1;             // pasos de predicción
  double minConfidence = 0.20;         // bajo esto -> modo seguro
  double maxBudget = 0.90;             // techo duro de presupuesto
  std::uint64_t seed = 0x9E3779B97F4A7C15ULL;
  // Pesos del objetivo (ganancia perceptual).
  double wClarity = 1.5;
  double wNaturalness = 1.2;
  double wImpact = 1.0;
  double wSpatiality = 1.0;
  double wStability = 0.8;
  // Pesos de las penalizaciones (huella audible). wMovement/wLatency son
  // desempates (la guarda fuerte es el límite de tasa + el techo de
  // presupuesto + la histéresis), no un freno dominante.
  double wFatigue = 0.6;     // subida de fatiga (con signo: bajarla premia)
  double wAudibility = 0.4;  // cualquier cambio de sonoridad es huella
  double wConfidence = 0.5;  // caída de confianza
  double wBudget = 0.4;      // presupuesto ya consumido en la predicción
  double wMovement = 0.05;   // ||acción||_2: alteración de fase / ring
  double wIncoherence = 0.8; // dispersión entre bandas de la ganancia
  double wLatency = 0.02;    // ||acción||_1: coste de intervención
};

struct MpcDecision {
  Action action{};
  double score = 0.0;       // puntuación de la acción devuelta
  double scoreZero = 0.0;   // puntuación de no hacer nada
  double scoreKept = 0.0;   // puntuación de mantener la acción anterior
  bool safeMode = false;    // true si entró en modo seguro
  bool keptPrevious = false;  // true si la histéresis retuvo la acción anterior
  std::uint64_t stepIndex = 0;
};

// Telemetría de la sombra (cierre 2026-11-21): instantánea de la última
// decisión para observarla en vivo sin tocar el audio. Solo lectura: el
// hilo de audio es el único escritor (step/processShadow), el lector (UI
// o log) solo lee. Sin malloc, sin I/O.
struct MpcTelemetry {
  std::uint64_t stepIndex = 0;        // pasos sombra ejecutados
  std::size_t decisionsLogged = 0;    // decisiones registradas
  Action lastAction{};                // última acción decidida
  double lastScore = 0.0;             // puntuación de la última acción
  double lastScoreZero = 0.0;         // puntuación de no hacer nada
  bool lastSafeMode = false;          // última decisión en modo seguro
  bool lastKeptPrevious = false;      // histéresis retuvo la acción anterior
  double lastMeanConfidence = 0.0;    // confianza media del campo (último paso)
  std::size_t historyCount = 0;       // decisiones guardadas en el historial
};

class MpcController {
 public:
  explicit MpcController(MpcConfig cfg = MpcConfig{});

  // Paso sombra: observa el campo y devuelve la decisión. No modifica audio.
  MpcDecision step(const PerceptualField& field);

  // Función objetivo (mayor = mejor). Pública para auditoría y tests:
  // -inf si la predicción viola el techo de presupuesto o no es finita.
  double score(const PerceptualField& current,
               const PerceptualField& predicted, const Action& action) const;

  // Candidatos deterministas que step() evalúa, ya limitados en tasa
  // respecto a `previous`. El mismo `previous` da siempre la misma lista.
  std::vector<Action> candidates(const Action& previous) const;

  // Passthrough sombra: copia in -> out bit-exacto y corre un paso del MPC.
  // La decisión NO depende del audio (solo del campo). Fail-closed ante
  // punteros nulos o frames == 0: no crashea, corre el paso igual.
  MpcDecision processShadow(const float* in, float* out, std::size_t frames,
                            const PerceptualField& field);

  const MpcDecision& lastDecision() const { return last_; }
  std::size_t decisionsLogged() const { return logged_; }

  // ── Telemetría de la sombra (cierre 2026-11-21) ───────────────────────
  // Instantánea de la última decisión + historial corto (anillo de 32, sin
  // alocación) para observar las decisiones en vivo sin tocar el audio.
  // Solo lectura desde fuera; el hilo de audio es el único escritor.
  MpcTelemetry telemetry() const;
  // Decisiones guardadas en el historial (0 = más reciente).
  std::size_t telemetryHistoryCount() const { return historyCount_; }
  MpcDecision telemetryHistoryAt(std::size_t index) const;

  void reset();

 private:
  // PRNG determinista splitmix64 (sin std::random_device: la decisión debe
  // ser reproducible).
  struct SplitMix64 {
    std::uint64_t state;
    explicit SplitMix64(std::uint64_t seed) : state(seed) {}
    std::uint64_t next() {
      std::uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
      z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
      z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
      return z ^ (z >> 31);
    }
    double uniform(double lo, double hi) {
      const double u = static_cast<double>(next() >> 11) *
                       (1.0 / 9007199254740992.0);  // 53 bits
      return lo + (hi - lo) * u;
    }
  };

  MpcConfig cfg_;
  OperatorSimulator sim_;
  Action previous_{};
  bool hasPrevious_ = false;
  MpcDecision last_{};
  std::uint64_t stepIndex_ = 0;
  std::size_t logged_ = 0;
  // Telemetría: confianza media del campo en el último paso + historial
  // corto de decisiones (anillo fijo, sin alocación en el hilo de audio).
  double lastMeanConfidence_ = 0.0;
  static constexpr std::size_t kHistorySize = 32;
  std::array<MpcDecision, kHistorySize> history_{};
  std::size_t historyCount_ = 0;
  std::size_t historyHead_ = 0;  // próximo slot a escribir

  // Guarda la decisión en el historial (anillo) y actualiza la confianza
  // media del paso. La llama step() en sus dos salidas.
  void pushTelemetry(const MpcDecision& d, double meanConfidence);
};

}  // namespace ivanna::perceptual_mpc
