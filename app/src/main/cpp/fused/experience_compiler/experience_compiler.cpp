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

// Coeficientes de ganancia (dB por unidad de amount), MEDIDOS en la
// cadena real (fix #1432, 2026-11-25), NO por razonamiento. Método:
// tono dual-mono 1 kHz −3 dBFS, 2 s, HEADPHONES, setVolumetricEnabled
// ON (la configuración exacta del test #1432), régimen (2º segundo).
// Cada coeficiente es el marginal leave-one-out en la configuración
// ALL7 (quitar ese operador de los 7 a 1.0 y medir cuánto cambia la
// ganancia neta media L/R) — es la derivada parcial en el punto de
// operación all-1.0, no el marginal sobre ORBE (que sobreestima la
// pérdida porque NEURO/PULSO dependen del nivel: sobre ORBE NEURO
// pierde −14.38 dB, en ALL7 solo −11.15 dB, porque la señal ya llega
// atenuada). Suma de los 7 marginales leave-one-out: −9.766 dB; la
// pérdida total medida ALL7 es −14.182 dB (media L/R). La diferencia
// (−4.416 dB) es el término de interacción del modelo lineal (la
// cadena no es aditiva) y se carga a NEURO, el operador dominante y
// el responsable de la dependencia de nivel.
// PULSO — ABSORCIÓN DEL TRIM: el trim vive en la etapa tonal (5ª) y
// PULSO/dinámica (6ª, última) es nivel-dependiente: al subir el trim,
// su compresor empuja hacia atrás y absorbe parte de la compensación.
// Medido (probe-p8-trimsweep / probe-p8-pulso): sin PULSO el trim es
// 100% efectivo (trim +14.182 dB -> neto +0.009 dB); con PULSO a 1.0
// el trim +14.182 dB solo deja neto −2.668 dB y hacen falta +19.482 dB
// para neto ~0 dB. Por eso el coeficiente de PULSO NO es su ganancia
// standalone (−0.008 dB) sino su contribución EFECTIVA en all-1.0:
// −0.008 (ganancia) − 5.300 (absorción del trim) = −5.308 dB. La suma
// de los 7 coeficientes es −19.482 dB, así el trim P8 a 1.0 es
// +19.482 dB y la ganancia neta medida queda ~0 dB (peor canal
// ~0.08 dB por el desbalance residual −0.153 dB, < 0.10 dB del test).
// Son coeficientes EFECTIVOS calibrados en el punto all-1.0 (el modelo
// P8 es lineal y la cadena no lo es); en otros amounts son
// aproximados. Sonda y números en
// ~/workspace/corona-d10/logs/fix-1432/ (probe-p8-netgain2-after-centroid.log,
// probe-p8-trimsweep.log, probe-p8-pulso.log).
// El desbalance L/R residual de la cadena (−0.153 dB, ALL7) lo corrige
// el level-match por canal de OmniField (conservación del centroide);
// el trim escalar solo centra la media.
inline constexpr double kNominalGainDb[kNumOperators] = {
    +0.525,   // ORBE     (leave-one-out, media L/R)
    +0.057,   // HOLOS    (leave-one-out; el campo es ~neutro en ganancia
              //           tras la conservación de centroide de OmniField)
    -0.044,   // SALA VIVA (leave-one-out)
    +0.000,   // VOLTERRA (idéntico a la base: ganancia 0.000 dB)
    +0.858,   // ARMONÍA  (leave-one-out)
    -5.308,   // PULSO    (−0.008 ganancia − 5.300 absorción del trim;
              //           ver comentario de arriba)
    -15.570,  // NEURO    (−11.154 leave-one-out − 4.416 de interacción)
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
