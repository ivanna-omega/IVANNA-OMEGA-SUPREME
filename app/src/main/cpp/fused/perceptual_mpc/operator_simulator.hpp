// operator_simulator.hpp — Simulador lineal y auditable de los 7 operadores.
//
// Modelo: P'[dim] = clamp(P[dim] + sum_op a_op * W[op][dim], 0, 1).
// El presupuesto se actualiza aparte: P'[presupuesto] =
// clamp(P[presupuesto] + kBudgetPerActionL1 * ||a||_1, 0, 1).
//
// Los pesos W son CALIBRACIÓN INICIAL por razonamiento (no valores medidos):
// cada operador empuja las dimensiones que por diseño debe mover. No son
// una medición del motor real; la calibración contra mediciones es trabajo
// pendiente (ver README). La matriz es fija y auditable: cualquiera puede
// leerla y verificar qué predice el simulador. Sin red neuronal: el modelo
// es una matriz lineal, no una autoridad opaca.

#pragma once

#include <array>
#include <cstddef>

#include "perceptual_vector.hpp"

namespace ivanna::perceptual_mpc {

class OperatorSimulator {
 public:
  // Presupuesto consumido por unidad de ||acción||_1.
  static constexpr double kBudgetPerActionL1 = 0.02;

  // W[operador][dimensión]. Fila = operador, columna = dimensión.
  static const std::array<std::array<double, kNumDims>, kNumOperators>& weights();

  // Un paso de predicción para un vector.
  PerceptualVector simulateStep(const PerceptualVector& p, const Action& a) const;
  // Un paso para todo el campo (misma acción en todas las bandas).
  PerceptualField simulateField(const PerceptualField& f, const Action& a) const;
  // Horizonte de N pasos con la misma acción repetida.
  PerceptualField simulateHorizon(const PerceptualField& f, const Action& a,
                                  std::size_t steps) const;
};

}  // namespace ivanna::perceptual_mpc
