// experience_compiler.cpp — P8.1 + P8.2 + P8.3 (ver header).
#include "experience_compiler.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace ivanna::fused::experience_compiler {

namespace {

// Mapa diagonal NOMINAL E → operador (ver header): el componente i
// alimenta el operador i. Documentado como punto de partida por
// razonamiento, no como calibración.
inline constexpr std::size_t kComponentToOperator[kNumComponents] = {
    0,  // I imagen        -> ORBE
    1,  // D profundidad   -> HOLOS
    2,  // S escala        -> SALA VIVA
    3,  // T textura       -> VOLTERRA
    4,  // M microdinámica -> ARMONÍA
    5,  // F fidelidad temporal -> PULSO
    6,  // A atención/fatiga    -> NEURO
};

// Coeficientes NOMINALES de ganancia (dB por unidad de amount). Por
// razonamiento, NO medidos en el Genezi ni en la sala. El trim maestro
// los compensa exactamente, así que la ganancia neta es 0.0 DENTRO de
// este modelo nominal. La ganancia real de cada operador se mide en
// P2/P3; cuando exista, estos coeficientes se sustituyen por los medidos.
inline constexpr double kNominalGainDb[kNumOperators] = {
    +0.10,  // ORBE
    -0.05,  // HOLOS
    +0.08,  // SALA VIVA
    +0.12,  // VOLTERRA
    +0.06,  // ARMONÍA
    -0.04,  // PULSO
    +0.02,  // NEURO
};

double clamp01(double x) noexcept {
    if (!std::isfinite(x)) return 0.0;
    if (x < 0.0) return 0.0;
    if (x > 1.0) return 1.0;
    return x;
}

}  // namespace

bool ExperienceVector::isValid() const noexcept {
    for (std::size_t i = 0; i < kNumComponents; ++i) {
        if (!std::isfinite(v[i]) || v[i] < 0.0 || v[i] > 1.0) return false;
        if (!std::isfinite(uncertainty[i]) || uncertainty[i] < 0.0 ||
            uncertainty[i] > 1.0) {
            return false;
        }
    }
    return true;
}

void ExperienceVector::clampInPlace() noexcept {
    for (std::size_t i = 0; i < kNumComponents; ++i) {
        if (!std::isfinite(v[i])) {
            v[i] = 0.0;
            uncertainty[i] = 1.0;
            continue;
        }
        if (v[i] < 0.0) v[i] = 0.0;
        if (v[i] > 1.0) v[i] = 1.0;
        if (!std::isfinite(uncertainty[i])) {
            uncertainty[i] = 1.0;
            continue;
        }
        if (uncertainty[i] < 0.0) uncertainty[i] = 0.0;
        if (uncertainty[i] > 1.0) uncertainty[i] = 1.0;
    }
}

double distance(const ExperienceVector& a, const ExperienceVector& b) noexcept {
    double sum = 0.0;
    for (std::size_t i = 0; i < kNumComponents; ++i) {
        if (!std::isfinite(a.v[i]) || !std::isfinite(b.v[i])) {
            return std::numeric_limits<double>::infinity();
        }
        double d = a.v[i] - b.v[i];
        sum += d * d;
    }
    return std::sqrt(sum);
}

std::string serialize(const ExperienceVector& e) {
    char buf[512];
    std::snprintf(
        buf, sizeof(buf),
        "E=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f;"
        "U=%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f",
        e.v[0], e.v[1], e.v[2], e.v[3], e.v[4], e.v[5], e.v[6],
        e.uncertainty[0], e.uncertainty[1], e.uncertainty[2],
        e.uncertainty[3], e.uncertainty[4], e.uncertainty[5],
        e.uncertainty[6]);
    return std::string(buf);
}

bool parse(const std::string& text, ExperienceVector* out) noexcept {
    if (out == nullptr) return false;
    ExperienceVector tmp;
    double vals[14];
    int n = std::sscanf(
        text.c_str(),
        "E=%lf,%lf,%lf,%lf,%lf,%lf,%lf;U=%lf,%lf,%lf,%lf,%lf,%lf,%lf",
        &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5],
        &vals[6], &vals[7], &vals[8], &vals[9], &vals[10], &vals[11],
        &vals[12], &vals[13]);
    if (n != 14) return false;
    for (std::size_t i = 0; i < kNumComponents; ++i) {
        tmp.v[i] = vals[i];
        tmp.uncertainty[i] = vals[7 + i];
    }
    if (!tmp.isValid()) return false;
    *out = tmp;
    return true;
}

Reachability classify(const ExperienceVector& target,
                      const ReachableRegion& region) noexcept {
    bool anyUnmeasured = false;
    for (std::size_t i = 0; i < kNumComponents; ++i) {
        double x = target.v[i];
        if (!std::isfinite(x) || x < 0.0 || x > 1.0) {
            return Reachability::kUnreachable;
        }
        if (!region.measured[i]) {
            anyUnmeasured = true;
            continue;
        }
        if (x < region.min[i] || x > region.max[i]) {
            return Reachability::kUnreachable;
        }
    }
    return anyUnmeasured ? Reachability::kUnknown
                         : Reachability::kAchievable;
}

double nominalGainDbPerUnit(std::size_t operatorIndex) noexcept {
    if (operatorIndex >= kNumOperators) return 0.0;
    return kNominalGainDb[operatorIndex];
}

CompileResult compile(const ExperienceVector& target,
                      const ReachableRegion& region) noexcept {
    CompileResult result;
    // Punto más cercano: el objetivo clampado a [0,1] (y, donde el
    // rango esté medido, clampado además a [min,max]).
    ExperienceVector nearest = target;
    nearest.clampInPlace();
    for (std::size_t i = 0; i < kNumComponents; ++i) {
        if (region.measured[i]) {
            if (nearest.v[i] < region.min[i]) nearest.v[i] = region.min[i];
            if (nearest.v[i] > region.max[i]) nearest.v[i] = region.max[i];
        }
    }
    result.nearestPoint = nearest;
    result.distanceToTarget = distance(target, nearest);
    result.reachability = classify(target, region);
    // Si el objetivo tiene algún NaN, distance es +inf y el punto más
    // cercano es el clamp (NaN → 0). Se compila el punto más cercano,
    // nunca el NaN: fail-closed.
    if (!std::isfinite(result.distanceToTarget)) {
        result.distanceToTarget = 0.0;
        for (std::size_t i = 0; i < kNumComponents; ++i) {
            if (!std::isfinite(target.v[i])) result.distanceToTarget += 1.0;
        }
        result.reachability = Reachability::kUnreachable;
    }
    // Mapa diagonal nominal E → amounts, desde el punto más cercano.
    result.amounts.fill(0.0);
    for (std::size_t c = 0; c < kNumComponents; ++c) {
        result.amounts[kComponentToOperator[c]] = clamp01(nearest.v[c]);
    }
    // Restricción dura: ganancia neta 0.0 dB. Trim = −Σ g_i·amount_i.
    double gain = 0.0;
    for (std::size_t o = 0; o < kNumOperators; ++o) {
        gain += kNominalGainDb[o] * result.amounts[o];
    }
    result.trimDb = -gain;
    result.netGainDb = gain + result.trimDb;  // 0.0 por construcción
    result.ok = true;
    return result;
}

const char* componentName(Component c) noexcept {
    switch (c) {
        case Component::kImage: return "imagen";
        case Component::kDepth: return "profundidad";
        case Component::kScale: return "escala";
        case Component::kTexture: return "textura";
        case Component::kMicrodynamics: return "microdinamica";
        case Component::kTemporalFidelity: return "fidelidad-temporal";
        case Component::kAttention: return "atencion";
    }
    return "desconocida";
}

}  // namespace ivanna::fused::experience_compiler
