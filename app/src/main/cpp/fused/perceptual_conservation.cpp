// perceptual_conservation.cpp — Capa de conservación perceptual.
// Ver perceptual_conservation.hpp para el diseño y las adaptaciones honestas
// del andamio de luna.
#include "fused/perceptual_conservation.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::fused {
namespace {

// Sanea un float: NaN/inf -> default (nunca propaga no-finitos al audio).
inline float sane(float v, float dflt) noexcept {
    return std::isfinite(v) ? v : dflt;
}
inline float clampf(float v, float lo, float hi) noexcept {
    v = sane(v, lo);
    return v < lo ? lo : (v > hi ? hi : v);
}
// Normalización de energía: un seno a fondo de escala (amplitud 1.0) tiene
// media cuadrática 0.5; energía 1.0 = ese nivel. Música típica: 0.01..0.3.
inline constexpr double kEnergyRef = 0.25;  // 0.5 (MS de seno FS) / 2
// Constante de tiempo del seguidor de envolvente de energía (~50 ms).
inline constexpr double kEnvTauSec = 0.05;

}  // namespace

void ConservationConfig::clamp() noexcept {
    amount = clampf(amount, 0.0f, 1.0f);
    energyCap = clampf(energyCap, 0.0f, 1.0f);
    phaseCoherence = clampf(phaseCoherence, 0.0f, 1.0f);
    inertia = clampf(inertia, 0.0f, 0.99f);
}

// ── Biquad pasabanda RBJ (ganancia de pico 0 dB) ────────────────────────────
void PerceptualConservation::Biquad::setBandpass(double f0, double q, double fs) noexcept {
    if (!(f0 > 0.0) || !(fs > 0.0) || !(q > 0.0)) {
        b0 = b1 = b2 = a1 = a2 = 0.0;
        return;
    }
    const double w0 = 2.0 * 3.14159265358979323846 * f0 / fs;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double cosw = std::cos(w0);
    const double a0 = 1.0 + alpha;
    b0 = alpha / a0;
    b1 = 0.0;
    b2 = -alpha / a0;
    a1 = -2.0 * cosw / a0;
    a2 = (1.0 - alpha) / a0;
    reset();
}

double PerceptualConservation::Biquad::process(double x) noexcept {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
}

// ── Firmas de campo por operador ────────────────────────────────────────────
// Multiplicador de energía (y presión) que cada operador PROPONE por unidad
// de amount. Declarado, no medido: es la "firma" de campo de cada operador.
//   NEURO: el guardián atempera (-5% energía) — estabilidad ante todo.
//   ORBE: redistribuye el espacio, no cambia la energía neta (1.0).
//   HOLOS: el campo añade presencia (+10% energía, +5% presión).
//   SALA VIVA: la sala añade energía de cola (+5%).
//   VOLTERRA: los armónicos añaden energía (+8%), más en agudos.
//   ARMONÍA: reequilibra el tono, energía neta neutra (1.0).
//   PULSO: el golpe añade energía transitoria (+5%).
// El solver promedia estas propuestas ponderadas por la actividad del
// operador y ACOTA el resultado con el techo de energía; además la ganancia
// final es <= 1 (nunca amplifica), así que una firma imprecisa no puede
// crear energía: como mucho deja de reducir.
float PerceptualConservation::operatorEnergySignature(int opIndex, int band) noexcept {
    float base = 1.0f;
    switch (opIndex) {
        case 0: base = 0.95f; break;  // NEURO
        case 1: base = 1.00f; break;  // ORBE
        case 2: base = 1.10f; break;  // HOLOS
        case 3: base = 1.05f; break;  // SALA VIVA
        case 4: base = 1.08f; break;  // VOLTERRA
        case 5: base = 1.00f; break;  // ARMONÍA
        case 6: base = 1.05f; break;  // PULSO
        default: base = 1.00f; break;
    }
    // VOLTERRA: los armónicos viven en agudos (bandas >= 2 kHz).
    if (opIndex == 4 && band >= 5) base *= 1.05f;
    return base;
}

float PerceptualConservation::operatorPressureSignature(int opIndex, int /*band*/) noexcept {
    switch (opIndex) {
        case 0: return 0.98f;  // NEURO
        case 1: return 1.00f;  // ORBE
        case 2: return 1.05f;  // HOLOS
        case 3: return 1.02f;  // SALA VIVA
        case 4: return 1.03f;  // VOLTERRA
        case 5: return 1.00f;  // ARMONÍA
        case 6: return 1.02f;  // PULSO
        default: return 1.00f;
    }
}

// ── Solucionador central ────────────────────────────────────────────────────
void ConservationSolver::solve(const ConservationFieldState& previous,
                               const ConservationProposal* proposals, int numProposals,
                               const float* measuredEnergy, const ConservationConfig& config,
                               double nowSeconds, double dtSeconds,
                               float* outGains, ConservationFieldState* outState) noexcept {
    if (!proposals || numProposals <= 0 || !measuredEnergy || !outGains || !outState) return;
    // dt válido: un bloque puede durar hasta segundos (el test llama
    // process() una vez con 1 s). Solo se rechazan valores no finitos o
    // absurdos; el fallback es un hop de 128 muestras @ 48 kHz.
    const double dt = (dtSeconds > 0.0 && dtSeconds <= 10.0 && std::isfinite(dtSeconds))
        ? dtSeconds
        : 0.00267;
    // Factor de inercia por bloque: cuánto se mueve el estado hacia el objetivo.
    const float inertia = clampf(config.inertia, 0.0f, 0.99f);
    const float alpha = clampf(1.0f - static_cast<float>(std::pow(static_cast<double>(inertia), dt * 60.0)), 0.0f, 1.0f);
    // El slew limit (coherencia de fase) se aplica en process() sobre la
    // ganancia maestra; el solver devuelve las ganancias sin slew.

    // Curvatura espectral: 2ª diferencia de log-energía entre bandas.
    float logE[kConservationBands]{};
    for (int b = 0; b < kConservationBands; ++b) {
        const float e = clampf(measuredEnergy[b], 0.0f, 1.0f);
        logE[b] = std::log(1e-6f + e);
    }

    ConservationFieldState out = previous;
    out.sequence = previous.sequence + 1;
    out.sampleTime = nowSeconds;
    for (int b = 0; b < kConservationBands; ++b) {
        // Promedio ponderado de las propuestas (peso = confianza × prioridad).
        float wSum = 0.0f;
        float eMul = 0.0f;
        float pMul = 0.0f;
        for (int i = 0; i < numProposals; ++i) {
            const float w = clampf(proposals[i].confidence, 0.0f, 1.0f) *
                            clampf(proposals[i].priority, 0.0f, 1.0f);
            if (w <= 0.0f) continue;
            eMul += proposals[i].energyMul[static_cast<std::size_t>(b)] * w;
            pMul += proposals[i].pressureMul[static_cast<std::size_t>(b)] * w;
            wSum += w;
        }
        if (wSum > 1e-6f) {
            eMul /= wSum;
            pMul /= wSum;
        } else {
            eMul = 1.0f;
            pMul = 1.0f;
        }
        const float measured = clampf(measuredEnergy[b], 0.0f, 1.0f);
        // Energía objetivo: medida × propuesta, ACOTADA por el techo.
        // El techo es la ley de conservación: ninguna banda supera energyCap.
        float targetEnergy = clampf(measured * eMul, 0.0f, clampf(config.energyCap, 0.0f, 1.0f));
        const float targetPressure = clampf(std::sqrt(targetEnergy) * pMul, 0.0f, 1.0f);
        // Ganancia por banda: <= 1 (nunca amplifica = nunca crea energía).
        // En silencio (medida ~0) la ganancia es 1 (no se amplifica el silencio).
        float gain = 1.0f;
        if (measured > 1e-6f && targetEnergy < measured) {
            gain = clampf(std::sqrt(targetEnergy / measured), 0.0f, 1.0f);
        }
        // Slew limit (coherencia de fase): la ganancia no salta de golpe.
        // Se aplica en process() sobre la ganancia maestra.
        outGains[b] = gain;

        // Estado de campo (suavizado por inercia hacia el objetivo).
        auto& o = out.band[static_cast<std::size_t>(b)];
        const auto& old = previous.band[static_cast<std::size_t>(b)];
        o.energy = old.energy + (targetEnergy - old.energy) * alpha;
        o.pressure = old.pressure + (targetPressure - old.pressure) * alpha;
        // La velocidad y la curvatura las fija process() (medición); aquí se
        // conservan (el solver no las inventa).
        o.velocity = old.velocity;
        o.perceptualTime = nowSeconds;
        // Curvatura espectral (2ª diferencia de log-energía, bordes a un lado).
        float curv = 0.0f;
        if (b == 0) {
            curv = logE[0] - logE[1];
        } else if (b == kConservationBands - 1) {
            curv = logE[b] - logE[b - 1];
        } else {
            curv = logE[static_cast<std::size_t>(b - 1)] - 2.0f * logE[static_cast<std::size_t>(b)] +
                   logE[static_cast<std::size_t>(b + 1)];
        }
        o.curvature = old.curvature + (clampf(curv, -1.0f, 1.0f) - old.curvature) * alpha;
    }
    // La ganancia maestra (min por banda) la calcula process(); aquí se deja
    // en 1.0 (el llamador la sobrescribe). outState lleva el campo.
    out.masterGain = 1.0f;
    *outState = out;
}

// ── PerceptualConservation ──────────────────────────────────────────────────
void PerceptualConservation::prepare(double sampleRate) noexcept {
    sampleRate_ = (sampleRate > 8000.0 && std::isfinite(sampleRate)) ? sampleRate : 48000.0;
    for (int b = 0; b < kConservationBands; ++b) {
        const double f0 = static_cast<double>(kConservationBandHz[b]);
        bandL_[b].setBandpass(f0, static_cast<double>(kConservationBandQ), sampleRate_);
        bandR_[b].setBandpass(f0, static_cast<double>(kConservationBandQ), sampleRate_);
        envL_[b] = envR_[b] = envLR_[b] = 0.0;
    }
    reset();
}

void PerceptualConservation::reset() noexcept {
    for (int b = 0; b < kConservationBands; ++b) {
        bandL_[b].reset();
        bandR_[b].reset();
        envL_[b] = envR_[b] = envLR_[b] = 0.0;
    }
    fieldState_ = ConservationFieldState{};
    appliedGain_ = 1.0f;
    blockTime_ = 0.0;
}

void PerceptualConservation::setConfig(const ConservationConfig& c) noexcept {
    ConservationConfig cc = c;
    cc.clamp();
    configAmount_.store(cc.amount, std::memory_order_relaxed);
    configEnergyCap_.store(cc.energyCap, std::memory_order_relaxed);
    configPhaseCoherence_.store(cc.phaseCoherence, std::memory_order_relaxed);
    configInertia_.store(cc.inertia, std::memory_order_relaxed);
}

ConservationConfig PerceptualConservation::config() const noexcept {
    ConservationConfig c;
    c.amount = configAmount_.load(std::memory_order_relaxed);
    c.energyCap = configEnergyCap_.load(std::memory_order_relaxed);
    c.phaseCoherence = configPhaseCoherence_.load(std::memory_order_relaxed);
    c.inertia = configInertia_.load(std::memory_order_relaxed);
    return c;
}

void PerceptualConservation::setOperatorAmounts(const float* amounts, int count) noexcept {
    if (!amounts || count <= 0) return;
    const int n = count < kNumOperators ? count : kNumOperators;
    for (int i = 0; i < n; ++i) {
        operatorAmounts_[static_cast<std::size_t>(i)].store(clampf(amounts[i], 0.0f, 1.0f),
                                                            std::memory_order_relaxed);
    }
}

void PerceptualConservation::setTwinInterventionScale(double scale) noexcept {
    if (!std::isfinite(scale)) scale = 1.0;
    twinInterventionScale_.store(clampf(static_cast<float>(scale), 0.0f, 1.0f),
                                 std::memory_order_relaxed);
}

double PerceptualConservation::twinInterventionScale() const noexcept {
    return static_cast<double>(twinInterventionScale_.load(std::memory_order_relaxed));
}

void applyTwinToConservation(
    PerceptualConservation& conservation,
    const acoustic_twin::AcousticTwinEstimate& twin) noexcept {
    // Fail-closed: gemelo inválido -> escala 1.0 (sin cambio).
    if (!twin.valid) {
        conservation.setTwinInterventionScale(1.0);
        return;
    }
    conservation.setTwinInterventionScale(twin.overallInterventionScale);
}

void PerceptualConservation::process(double* left, double* right, int frames,
                                     double nowSeconds) noexcept {
    if (!left || !right || frames <= 0) return;
    const ConservationConfig cfg = config();
    // Amount efectivo: el gemelo acústico lo escala (cierre 2026-11-21).
    // A confianza baja del gemelo, la conservación interviene menos
    // (fail-closed). Sin gemelo la escala es 1.0 y nada cambia.
    const float effectiveAmount = clampf(
        cfg.amount * static_cast<float>(twinInterventionScale()), 0.0f, 1.0f);
    const double dt = static_cast<double>(frames) / sampleRate_;
    blockTime_ += dt;

    // ── Apagado: rampa suave de vuelta a 1.0 y luego bypass bit-exacto ────
    // Si la capa se apaga con la ganancia reducida, volver a 1.0 de golpe
    // sería un clic. Se rampa en este bloque y a partir del siguiente (ya en
    // 1.0) no se toca el buffer.
    if (effectiveAmount <= 0.0f) {
        if (appliedGain_ >= 0.999999f) {
            appliedGain_ = 1.0f;
            return;  // bypass bit-exacto
        }
        const double start = appliedGain_;
        for (int i = 0; i < frames; ++i) {
            const double t = static_cast<double>(i + 1) / static_cast<double>(frames);
            const double g = start + (1.0 - start) * t;
            left[i] *= g;
            right[i] *= g;
        }
        appliedGain_ = 1.0f;
        return;
    }

    // ── Medición: banco de 8 pasabanda × L/R + acumuladores ───────────────
    // Se mide la SEÑAL DE ENTRADA (salida de los 7 operadores) antes de
    // aplicar la ganancia: feedforward causal (solo el bloque actual).
    double accL[kConservationBands]{};
    double accR[kConservationBands]{};
    double accLR[kConservationBands]{};
    for (int i = 0; i < frames; ++i) {
        const double l = left[i];
        const double r = right[i];
        if (!std::isfinite(l) || !std::isfinite(r)) continue;
        for (int b = 0; b < kConservationBands; ++b) {
            const double bl = bandL_[b].process(l);
            const double br = bandR_[b].process(r);
            accL[b] += bl * bl;
            accR[b] += br * br;
            accLR[b] += bl * br;
        }
    }
    const double n = static_cast<double>(frames);
    const double envAlpha = 1.0 - std::exp(-dt / kEnvTauSec);
    float measuredEnergy[kConservationBands]{};
    for (int b = 0; b < kConservationBands; ++b) {
        // Envolvente (leaky integrator) de la energía por banda y canal.
        envL_[b] += (accL[b] / n - envL_[b]) * envAlpha;
        envR_[b] += (accR[b] / n - envR_[b]) * envAlpha;
        envLR_[b] += (accLR[b] / n - envLR_[b]) * envAlpha;
        const double e = (envL_[b] + envR_[b]) * 0.5 / kEnergyRef;
        measuredEnergy[b] = clampf(static_cast<float>(e), 0.0f, 1.0f);
    }

    // ── Propuestas de los 7 operadores (de sus amounts efectivos) ─────────
    ConservationProposal proposals[kNumOperators]{};
    for (int i = 0; i < kNumOperators; ++i) {
        const float a = operatorAmounts_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        proposals[i].confidence = clampf(a, 0.0f, 1.0f);
        proposals[i].priority = 1.0f;
        for (int b = 0; b < kConservationBands; ++b) {
            proposals[i].energyMul[static_cast<std::size_t>(b)] = operatorEnergySignature(i, b);
            proposals[i].pressureMul[static_cast<std::size_t>(b)] = operatorPressureSignature(i, b);
        }
    }

    // ── Solucionador: ganancias por banda + estado de campo ───────────────
    float gains[kConservationBands]{};
    ConservationFieldState nextState{};
    ConservationSolver::solve(fieldState_, proposals, kNumOperators, measuredEnergy, cfg,
                              nowSeconds, dt, gains, &nextState);
    // Velocidad por banda (medición, no propuesta): x = desbalance L/R,
    // y = coherencia L/R, z = 0 (sin medir en estéreo).
    for (int b = 0; b < kConservationBands; ++b) {
        const double sum = envL_[b] + envR_[b];
        const double diff = envL_[b] - envR_[b];
        float vx = 0.0f;
        float vy = 0.0f;
        if (sum > 1e-12) {
            vx = clampf(static_cast<float>(diff / sum), -1.0f, 1.0f);
            const double denom = std::sqrt(std::max(1e-12, envL_[b] * envR_[b]));
            vy = clampf(static_cast<float>(envLR_[b] / denom), -1.0f, 1.0f);
        }
        auto& o = nextState.band[static_cast<std::size_t>(b)];
        const auto& old = fieldState_.band[static_cast<std::size_t>(b)];
        const float alpha = clampf(1.0f - static_cast<float>(std::pow(static_cast<double>(cfg.inertia), dt * 60.0)), 0.0f, 1.0f);
        o.velocity.x = old.velocity.x + (vx - old.velocity.x) * alpha;
        o.velocity.y = old.velocity.y + (vy - old.velocity.y) * alpha;
        o.velocity.z = 0.0f;
    }

    // ── Ganancia maestra: la banda más constreñida manda ──────────────────
    float masterGain = 1.0f;
    for (int b = 0; b < kConservationBands; ++b) masterGain = std::min(masterGain, gains[b]);
    // Mezcla por amount efectivo (gemelo incluido): 0 = sin conservación
    // (ganancia 1), 1 = plena.
    float target = 1.0f + effectiveAmount * (masterGain - 1.0f);
    target = clampf(target, 0.0f, 1.0f);
    // Slew limit (coherencia de fase) sobre la ganancia aplicada.
    const float maxSlewPerSec = 4.0f * (1.0f - cfg.phaseCoherence) + 0.1f;
    const float maxStep = maxSlewPerSec * static_cast<float>(dt);
    const float prevGain = appliedGain_;
    float newGain = prevGain;
    if (target > prevGain + maxStep) newGain = prevGain + maxStep;
    else if (target < prevGain - maxStep) newGain = prevGain - maxStep;
    else newGain = target;
    newGain = clampf(newGain, 0.0f, 1.0f);

    // ── Aplicación: rampa por muestra (sin clics) ─────────────────────────
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i + 1) / static_cast<double>(frames);
        const double g = static_cast<double>(prevGain) + (static_cast<double>(newGain) - static_cast<double>(prevGain)) * t;
        left[i] *= g;
        right[i] *= g;
    }
    appliedGain_ = newGain;
    nextState.masterGain = newGain;
    fieldState_ = nextState;
}

}  // namespace ivanna::fused
