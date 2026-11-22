// operator_simulator.cpp — Simulador lineal y auditable de los 7 operadores.
//
// La matriz de pesos es CALIBRACIÓN INICIAL por razonamiento, NO valores
// medidos del motor real (ver README.md del módulo). Cada fila es un
// operador; cada columna, una dimensión perceptual (orden: Audibilidad,
// Claridad, Naturalidad, Impacto, Espacialidad, Fatiga, Estabilidad,
// Confianza, Presupuesto). Un peso positivo empuja la dimensión hacia
// arriba; uno negativo, hacia abajo. La columna de Confianza es cero en
// todas las filas: la acción no cambia la confianza de la estimación.
// La columna de Presupuesto también es cero: el presupuesto lo consume la
// energía de intervención (||a||_1), no la matriz.

#include "operator_simulator.hpp"

namespace ivanna::perceptual_mpc {
namespace {

// Calibración inicial por razonamiento — NO valores medidos.
const std::array<std::array<double, kNumDims>, kNumOperators> kWeights = {{
    // ORBE (espacial): abre el campo, sube espacialidad y claridad.
    {{0.01, 0.10, 0.05, 0.02, 0.12, 0.02, 0.01, 0.00, 0.00}},
    // HOLOS (campo): cohesión del campo, impacto y estabilidad.
    {{0.01, 0.04, 0.03, 0.05, 0.04, 0.01, 0.02, 0.00, 0.00}},
    // SALA VIVA (sala): naturalidad y espacialidad de la sala.
    {{0.00, 0.02, 0.06, 0.01, 0.08, 0.02, 0.02, 0.00, 0.00}},
    // VOLTERRA (transductor): pegada y claridad, con algo de fatiga.
    {{0.02, 0.03, 0.01, 0.10, 0.00, 0.03, 0.00, 0.00, 0.00}},
    // ARMONÍA (tonal): claridad y naturalidad del balance tonal.
    {{0.00, 0.08, 0.07, 0.02, 0.01, 0.01, 0.03, 0.00, 0.00}},
    // PULSO (dinámica): impacto y estabilidad del transitorio.
    {{0.01, 0.02, 0.00, 0.06, 0.00, 0.02, 0.01, 0.00, 0.00}},
    // NEURO (cerebro): baja la fatiga y estabiliza la imagen.
    {{0.00, 0.01, 0.01, 0.00, 0.00, -0.05, 0.04, 0.00, 0.00}},
}};

}  // namespace

const std::array<std::array<double, kNumDims>, kNumOperators>&
OperatorSimulator::weights() {
  return kWeights;
}

PerceptualVector OperatorSimulator::simulateStep(const PerceptualVector& p,
                                                 const Action& a) const {
  const Action ac = clampAction(a);
  PerceptualVector out = p;
  const auto& w = weights();
  for (std::size_t op = 0; op < kNumOperators; ++op) {
    if (ac[op] == 0.0) continue;
    for (std::size_t d = 0; d < kNumDims; ++d) {
      out.v[d] += ac[op] * w[op][d];
    }
  }
  // El presupuesto no va en la matriz: lo consume la energía de intervención.
  out[Dim::kBudget] =
      p[Dim::kBudget] + kBudgetPerActionL1 * actionL1(ac);
  out.clampInPlace();
  return out;
}

PerceptualField OperatorSimulator::simulateField(const PerceptualField& f,
                                                 const Action& a) const {
  PerceptualField out;
  for (std::size_t b = 0; b < kNumBands; ++b) {
    out.bands[b] = simulateStep(f.bands[b], a);
  }
  return out;
}

PerceptualField OperatorSimulator::simulateHorizon(const PerceptualField& f,
                                                   const Action& a,
                                                   std::size_t steps) const {
  PerceptualField out = f;
  for (std::size_t s = 0; s < steps; ++s) {
    out = simulateField(out, a);
  }
  return out;
}

}  // namespace ivanna::perceptual_mpc
