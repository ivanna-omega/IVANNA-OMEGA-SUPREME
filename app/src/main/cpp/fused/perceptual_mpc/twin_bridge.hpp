// twin_bridge.hpp — Puente gemelo acústico → vector P[t] del MPC.
//
// Cierre 2026-11-21: alimenta la estimación del trayecto (parte 2 del plan
// de luna) al vector perceptual común P[t] del MPC (parte 3). La estimación
// del gemelo — interventionScale (confianza), coherencia y latencia — llega
// al MPC por dos canales fail-closed:
//   · Confianza: la confianza por banda de P[t] se multiplica por el
//     interventionScale del gemelo en esa banda. A confianza baja del
//     gemelo, el MPC ve poca confianza y entra en modo seguro (acción
//     cero) por su propio minConfidence. Es el freno principal.
//   · Presupuesto: a confianza baja del gemelo se consume presupuesto
//     extra (ser conservador). 0.25 es una constante NOMINAL (modelo, no
//     medición): a confianza 0 consume 25% extra; a confianza 1, nada.
//     El techo duro de presupuesto del MPC (maxBudget) sigue mandando.
//
// Fail-closed: si el gemelo NO es válido (valid == false), NO se toca el
// campo: el MPC se comporta exactamente como antes. Un gemelo ausente o
// roto no puede apagar ni desviar el MPC; solo un gemelo válido y con
// confianza alta lo deja actuar pleno.
//
// Mapeo de bandas: el gemelo tiene 9 bandas (63 Hz–16 kHz) y el MPC 8
// (63 Hz–8 kHz). Las 8 primeras coinciden en centro y se mapean 1:1; la
// 9ª (16 kHz) no tiene contraparte en el MPC y se ignora (documentado,
// no es un olvido). La latencia del gemelo (buffer + aire, ~11.41 ms en
// el trayecto de Esse) NO se cablea al MPC de horizonte 1: el MPC decide
// por bloque y su horizonte es 1 paso; la latencia queda expuesta en la
// estimación (AcousticTwinEstimate::airLatencyMs/bufferLatencyMs) para el
// trabajo futuro de horizonte extendido. La coherencia del gemelo ya está
// reflejada en su interventionScale (baja coherencia → baja confianza →
// baja escala), así que no se duplica en otro canal.
//
// Header-only y sin estado: una función pura. Sin malloc, sin I/O, sin
// tocar audio. Determinista.
#pragma once

#include <cstddef>

#include "../acoustic_twin/acoustic_twin.hpp"
#include "perceptual_vector.hpp"

namespace ivanna::perceptual_mpc {

// Consumo extra de presupuesto por falta de confianza del gemelo (0..1).
// Nominal, no medido: a interventionScale 0 consume 0.25 extra; a 1, 0.
inline constexpr double kTwinBudgetPenalty = 0.25;

// Aplica la estimación del gemelo al campo P[t] (in-place). Fail-closed:
// gemelo inválido -> campo intacto.
inline void applyTwinToField(
    PerceptualField& field,
    const ivanna::fused::acoustic_twin::AcousticTwinEstimate& twin) {
  if (!twin.valid) return;
  for (std::size_t i = 0; i < kNumBands; ++i) {
    const double scale =
        twin.band[i].interventionScale;  // 0..1 = confianza del gemelo
    field.bands[i][Dim::kConfidence] *= scale;
    field.bands[i][Dim::kBudget] += (1.0 - scale) * kTwinBudgetPenalty;
  }
  field.clampInPlace();
}

}  // namespace ivanna::perceptual_mpc
