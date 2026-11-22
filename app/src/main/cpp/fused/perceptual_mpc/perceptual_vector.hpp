// perceptual_vector.hpp — Vector perceptual común P[t] por banda (MPC sombra).
//
// Parte del módulo perceptual_mpc (próximo salto magistral, 2026-11-21).
// Módulo nuevo e independiente: solo decide; nunca toca audio, FusedChain,
// JNI, UI ni daemon.
//
// P[t] es un vector de 9 dimensiones normalizadas en [0,1] por banda de
// frecuencia (8 bandas nominales, centros aprox. ERB — no medidos).
//   Audibilidad:  cuánto se percibe la banda (0 = inaudible, 1 = pleno).
//   Claridad:     inteligibilidad / separación (1 = cristalina).
//   Naturalidad:  ausencia de artefacto de procesado (1 = natural).
//   Impacto:      pegada / presencia del transitorio (1 = máxima).
//   Espacialidad: anchura / profundidad percibida (1 = envolvente).
//   Fatiga:       aspereza / dureza acumulada (0 = sin fatiga, 1 = fatigante).
//   Estabilidad:  la imagen no se mueve ni respira (1 = roca).
//   Confianza:    fiabilidad de la estimación (0 = no fiarse, 1 = segura).
//   Presupuesto:  fracción del presupuesto de intervención ya consumida
//                 (0 = libre, 1 = agotado).
//
// Todas las dimensiones viven en [0,1]; clampInPlace() las fuerza a ese
// rango. Un valor no finito invalida el vector (isFinite() == false) y el
// MPC entra en modo seguro (acción cero).

#pragma once

#include <array>
#include <cmath>
#include <cstddef>

namespace ivanna::perceptual_mpc {

inline constexpr std::size_t kNumDims = 9;
inline constexpr std::size_t kNumBands = 8;
inline constexpr std::size_t kNumOperators = 7;

enum class Dim : std::size_t {
  kAudibility = 0,
  kClarity = 1,
  kNaturalness = 2,
  kImpact = 3,
  kSpatiality = 4,
  kFatigue = 5,
  kStability = 6,
  kConfidence = 7,
  kBudget = 8,
};

enum class Operator : std::size_t {
  kOrbe = 0,
  kHolos = 1,
  kSalaViva = 2,
  kVolterra = 3,
  kArmonia = 4,
  kPulso = 5,
  kNeuro = 6,
};

// Acción candidata: delta por operador en [-1, 1]. 0 = no tocar.
using Action = std::array<double, kNumOperators>;

struct PerceptualVector {
  std::array<double, kNumDims> v{};

  double& operator[](Dim d) { return v[static_cast<std::size_t>(d)]; }
  double operator[](Dim d) const { return v[static_cast<std::size_t>(d)]; }

  bool isFinite() const {
    for (double x : v) {
      if (!std::isfinite(x)) return false;
    }
    return true;
  }

  void clampInPlace() {
    for (double& x : v) {
      if (!std::isfinite(x)) {
        x = 0.0;
        continue;
      }
      if (x < 0.0) x = 0.0;
      if (x > 1.0) x = 1.0;
    }
  }

  // Estado neutro: todo a mitad salvo fatiga baja, confianza alta y
  // presupuesto libre.
  static PerceptualVector neutral() {
    PerceptualVector p;
    p.v = {0.5, 0.5, 0.5, 0.5, 0.5, 0.2, 0.5, 0.9, 0.0};
    return p;
  }
};

struct PerceptualField {
  std::array<PerceptualVector, kNumBands> bands{};

  bool isFinite() const {
    for (const auto& b : bands) {
      if (!b.isFinite()) return false;
    }
    return true;
  }

  void clampInPlace() {
    for (auto& b : bands) b.clampInPlace();
  }

  double mean(Dim d) const {
    double s = 0.0;
    for (const auto& b : bands) s += b[d];
    return s / static_cast<double>(kNumBands);
  }

  static PerceptualField neutral() {
    PerceptualField f;
    for (auto& b : f.bands) b = PerceptualVector::neutral();
    return f;
  }

  // Centros nominales de banda (Hz, escala aprox. ERB). Nominales, no
  // medidos: el MPC no depende de sus valores exactos.
  static const std::array<double, kNumBands>& bandCenterHz() {
    static const std::array<double, kNumBands> kCenters = {
        63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0};
    return kCenters;
  }
};

// Utilidades de acción.
inline Action clampAction(const Action& a) {
  Action out = a;
  for (double& x : out) {
    if (!std::isfinite(x)) x = 0.0;
    if (x < -1.0) x = -1.0;
    if (x > 1.0) x = 1.0;
  }
  return out;
}

inline double actionL1(const Action& a) {
  double s = 0.0;
  for (double x : a) s += std::abs(x);
  return s;
}

inline double actionL2(const Action& a) {
  double s = 0.0;
  for (double x : a) s += x * x;
  return std::sqrt(s);
}

inline Action zeroAction() {
  Action a{};
  a.fill(0.0);
  return a;
}

}  // namespace ivanna::perceptual_mpc
