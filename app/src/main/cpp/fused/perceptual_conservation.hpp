// perceptual_conservation.hpp — Capa de conservación perceptual (2026-11-21).
//
// El "secreto" de luna (cxa/gpt-5.6-luna, 2026-11-21): la clave que falta no
// es otro operador, es un ÁRBITRO FÍSICO-PERCEPTUAL COMÚN — un estado de
// campo latente único por banda que los 7 operadores respeten, combinado por
// un solucionador central bajo restricciones de energía-fase-causalidad.
// Sin él, cada operador puede sonar excelente por separado y destruir la
// coherencia al encadenarse.
//
// Esta capa lo implementa como una ETAPA DE ARBITRAJE en FusedChain (tras
// los 7 operadores + transitorios APEX, antes de la matriz TΩ): mide el
// estado de campo por banda, combina las propuestas de los 7 operadores
// (derivadas de sus amounts efectivos) y aplica UNA ganancia de
// conservación suave que hace cumplir la ley: nunca crear energía
// (ganancia <= 1), nunca un salto brusco (slew limitado = coherencia de
// fase) y nunca mirar al futuro (estrictamente causal, solo presente/pasado).
//
// ADAPTACIÓN HONESTA del andamio de luna (/tmp/ivanna-perceptual, que era
// un esqueleto con contratos INVENTADOS y NO se usa a ciegas):
//   · El andamio definía su propio FusedChain abstracto — AQUÍ la capa vive
//     DENTRO del FusedChain real (fused_chain.hpp), no lo sustituye.
//   · El andamio usaba std::mutex en el hilo de audio — AQUÍ todo el estado
//     de control son std::atomic (sin locks, sin malloc, sin I/O en process).
//   · El andamio inventaba un protocolo binario para el daemon — AQUÍ la
//     Ruta B usa SET_PERCEPTUAL_STATE JSON por el socket real (ver
//     daemon_command_responder.h); este archivo no sabe nada del daemon.
//   · El andamio aplicaba una ganancia de campo arbitraria (0.75+0.25√e) —
//     AQUÍ la ganancia es la del solucionador (min por banda, <= 1, con
//     rampa por muestra) y con amount=0 NO SE TOCA el buffer (bit-exacto).
//
// ALCANCE DE ESTA FASE (F1): el estado de campo y el solucionador son POR
// BANDA (medición real con banco de 8 pasabanda), pero la corrección
// aplicada es UNA ganancia maestra suave (la banda más constreñida manda).
// La ganancia por banda con reconstrucción (banco de síntesis) es F2: sin
// ella no hay artefactos de reconstrucción y el bypass es trivialmente
// bit-exacto. El estado por banda SÍ se publica (telemetría) para que la
// UI y F2 lo usen.
//
// REGLAS: amount=0 (default) = bypass bit-exacto. Todo apagado por
// defecto. Sin malloc/locks en process(). Sin NaN (todo se clampa y se
// sanea). La velocidad z es 0: en estéreo no se mide altura (documentado,
// no inventado).
#pragma once

#include <array>
#include <atomic>
#include <cstdint>

#include "acoustic_twin/acoustic_twin.hpp"

namespace ivanna::fused {

// 8 bandas logarítmicas para el estado de campo.
inline constexpr int kConservationBands = 8;
// Centros de banda (Hz), logarítmicos de 62.5 Hz a 8 kHz.
inline constexpr float kConservationBandHz[kConservationBands] = {
    62.5f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f};
// Q del pasabanda de medición (RBJ, ganancia de pico 0 dB).
inline constexpr float kConservationBandQ = 1.0f;

struct ConservationVec3 {
    float x{};
    float y{};
    float z{};
};

// Estado de campo común por banda (el "x_b" del secreto de luna).
struct ConservationBandField {
    float pressure{};               // 0..1: amplitud RMS de la banda (raíz de la energía)
    ConservationVec3 velocity{};    // velocidad de partícula estimada: x = desbalance L/R [-1,1],
                                    // y = coherencia L/R [-1,1], z = 0 (sin medir en estéreo)
    float energy{};                 // 0..1: energía media de la banda (seno a fondo de escala = 1)
    double perceptualTime{};        // s, monotónico (tiempo del bloque)
    float curvature{};              // -1..1: curvatura espectral (2ª diferencia de log-energía)
};

struct ConservationFieldState {
    std::array<ConservationBandField, kConservationBands> band{};
    float masterGain{1.0f};         // ganancia de conservación aplicada (<= 1)
    std::uint64_t sequence{};
    double sampleTime{};
};

// Propuesta de un operador: multiplicadores por banda (energía/presión).
// confidence = actividad del operador (0 = inactivo, no propone nada);
// priority = peso relativo (1 = igual para todos por defecto).
struct ConservationProposal {
    std::array<float, kConservationBands> energyMul{};
    std::array<float, kConservationBands> pressureMul{};
    float confidence{};
    float priority{1.0f};
};

// Controles de la capa (UI). Defaults: APAGADO (amount 0, fail-closed).
// amount: 0..1, cuánto aplica la conservación (0 = bypass bit-exacto).
// energyCap: 0..1, techo de energía por banda (1 = sin techo).
// phaseCoherence: 0..1, cuánto se suaviza el cambio de ganancia
//   (0 = cambio rápido, 1 = cambio muy suave; evita discontinuidades).
// inertia: 0..0.99, continuidad temporal del estado de campo.
struct ConservationConfig {
    float amount{0.0f};
    float energyCap{1.0f};
    float phaseCoherence{0.5f};
    float inertia{0.82f};

    void clamp() noexcept;
    bool isNeutral() const noexcept { return amount <= 0.0f; }
};

// El solucionador central. Puro (sin estado propio salvo la config):
// combina las propuestas bajo las restricciones y devuelve las ganancias
// por banda (<= 1, nunca amplifica = nunca crea energía).
//   previous: estado de campo del bloque anterior (para la inercia).
//   proposals: una por operador (7); las inactivas llevan confidence 0.
//   measuredEnergy: energía medida por banda (0..1) en este bloque.
//   config: controles ya clampados. dt: duración del bloque en segundos.
//   outGains: ganancias por banda resultantes (0..1).
//   outState: estado de campo actualizado (para telemetría).
class ConservationSolver {
public:
    static void solve(const ConservationFieldState& previous,
                      const ConservationProposal* proposals, int numProposals,
                      const float* measuredEnergy, const ConservationConfig& config,
                      double nowSeconds, double dtSeconds,
                      float* outGains, ConservationFieldState* outState) noexcept;
};

// La capa de conservación (componente RT). Vive dentro de FusedChain.
// Config y amounts de operador en atomics (el hilo de UI/JNI escribe, el
// hilo de audio lee; sin locks). El banco de filtros y el estado de campo
// solo los toca el hilo de audio (prepare/process), sin sincronización.
class PerceptualConservation {
public:
    // Índices canónicos de operador (mismo orden que FusedChain::kNumOperators):
    // 0 NEURO, 1 ORBE, 2 HOLOS, 3 SALA VIVA, 4 VOLTERRA, 5 ARMONÍA, 6 PULSO.
    static constexpr int kNumOperators = 7;

    PerceptualConservation() = default;
    PerceptualConservation(const PerceptualConservation&) = delete;
    PerceptualConservation& operator=(const PerceptualConservation&) = delete;

    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    // Controles (RT-safe, atomics). amount=0 apaga la capa (bypass exacto).
    void setConfig(const ConservationConfig& c) noexcept;
    ConservationConfig config() const noexcept;
    // Amounts efectivos de los 7 operadores (0..1), en orden canónico.
    // Los fija FusedChain::pushEffectiveControls() cada bloque.
    void setOperatorAmounts(const float* amounts, int count) noexcept;

    // ── Escala de intervención del gemelo acústico (cierre 2026-11-21) ───
    // Multiplica el amount efectivo (0..1, default 1.0 = sin cambio). El
    // gemelo la baja cuando su confianza es baja (intervenir MENOS, no
    // más). Fail-closed: si el gemelo no está disponible, la escala queda
    // en 1.0 y la conservación se comporta como antes. RT-safe (atomic).
    void setTwinInterventionScale(double scale) noexcept;
    double twinInterventionScale() const noexcept;

    // Arbitraje del bloque. Si amount==0 o frames<=0 no toca el buffer.
    // Aplica la ganancia de conservación con rampa por muestra (sin clics).
    // nowSeconds: tiempo monotónico del bloque (para perceptualTime).
    void process(double* left, double* right, int frames, double nowSeconds) noexcept;

    // Telemetría (best-effort, el hilo de audio es el único escritor).
    ConservationFieldState fieldState() const noexcept { return fieldState_; }
    float appliedGain() const noexcept { return appliedGain_; }
    bool isActive() const noexcept {
        return configAmount_.load(std::memory_order_relaxed) > 0.0f;
    }

private:
    // Biquad pasabanda RBJ (medición, no síntesis).
    struct Biquad {
        double b0{}, b1{}, b2{}, a1{}, a2{};
        double x1{}, x2{}, y1{}, y2{};
        void setBandpass(double f0, double q, double fs) noexcept;
        double process(double x) noexcept;
        void reset() noexcept { x1 = x2 = y1 = y2 = 0.0; }
    };

    // Firma de campo de un operador: cuánto multiplica la energía de cada
    // banda por unidad de amount. Documentado en el .cpp (no es un ajuste
    // medido: es la "firma" declarada de cada operador; la energía neta
    // la acota el solver con el techo, así que un coeficiente impreciso
    // no puede crear energía).
    static float operatorEnergySignature(int opIndex, int band) noexcept;
    static float operatorPressureSignature(int opIndex, int band) noexcept;

    double sampleRate_ = 48000.0;
    // Banco de medición: 8 bandas × 2 canales (L y R por separado, para
    // desbalance y coherencia). Solo el hilo de audio los toca.
    Biquad bandL_[kConservationBands]{};
    Biquad bandR_[kConservationBands]{};
    // Envolventes de energía por banda y canal (leaky integrator ~50 ms).
    double envL_[kConservationBands]{};
    double envR_[kConservationBands]{};
    double envLR_[kConservationBands]{};  // sum(L*R) por banda (coherencia)
    ConservationFieldState fieldState_{};
    float appliedGain_ = 1.0f;            // ganancia aplicada (rampa suave)
    double blockTime_ = 0.0;              // tiempo acumulado (s) para perceptualTime

    // Control (atomics: UI/JNI escribe, audio lee).
    std::atomic<float> configAmount_{0.0f};
    std::atomic<float> configEnergyCap_{1.0f};
    std::atomic<float> configPhaseCoherence_{0.5f};
    std::atomic<float> configInertia_{0.82f};
    // Escala del gemelo acústico (cierre 2026-11-21): multiplica el amount
    // efectivo. Default 1.0 (sin gemelo = sin cambio, fail-closed).
    std::atomic<float> twinInterventionScale_{1.0f};
    std::array<std::atomic<float>, kNumOperators> operatorAmounts_{};
};

// Aplica la estimación del gemelo a la conservación (cierre 2026-11-21):
// fija la escala de intervención al overallInterventionScale del gemelo
// (0..1 = confianza). Fail-closed: gemelo inválido -> escala 1.0 (la
// conservación se comporta como antes). La coherencia del gemelo ya está
// reflejada en su interventionScale (baja coherencia -> baja confianza),
// así que no se duplica en otro canal. La latencia del gemelo no se
// cablea aquí (la conservación decide por bloque; queda expuesta en la
// estimación para trabajo futuro).
void applyTwinToConservation(
    PerceptualConservation& conservation,
    const acoustic_twin::AcousticTwinEstimate& twin) noexcept;

}  // namespace ivanna::fused
