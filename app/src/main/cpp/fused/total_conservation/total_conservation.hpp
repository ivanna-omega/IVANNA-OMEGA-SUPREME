// total_conservation.hpp — P7 Conservación Total (Frente E, 2026-11-24).
//
// P7 del plan PLAN_P7_P10.md §2: la conservación deja de ser una auditoría
// final y se convierte en una propiedad verificable de toda la cadena. Cada
// reproducción lleva un certificado: qué se perdió, dónde, cuánto, y con
// qué confianza.
//
// ALCANCE DE ESTE MÓDULO (P7.1 + P7.2, código sin teléfono):
//   · P7.1 — cada una de las 9 etapas expone su Δ medible vía estimateDelta().
//   · P7.2 — estructura de datos del certificado por reproducción.
//   · P7.3 / P7.4 / P7.5-UI quedan BLOQUEADOS en Esse (escucha) — ver README.
//
// HONESTIDAD DE LOS DATOS — REGLA DURA DEL MÓDULO. Un término NO MEDIDO se
// marca "sin estimar" (estimated=false, deltaDb=0.0, confidence=0.0) y
// NUNCA se inventa ni se suma al Δtotal. El certificado declara
// totalValid=false mientras falte un solo término. Estado 2026-11-24:
//   · Fuente: WAV/FLAC sin pérdida → Δ=0 medido (por definición del
//     formato). Códecs con pérdida → sin estimar (la pérdida existe pero
//     su magnitud en esta cadena no está medida).
//   · Decodificación: bit-exacto (sin resampling) → Δ=0. Con resampling
//     o cambio de sample rate → sin estimar (medible en host, pendiente
//     de medición, no inventado).
//   · DSP: amounts de los 7 operadores en 0 → Δ=0. Con amount > 0 → sin
//     estimar: la alteración por operador debe salir del banco
//     adversarial (P1) y hoy no existe esa tabla medida. P2 audible NO
//     hecho: no hay nada que totalizar aún.
//   · Conversión / Amplificación / Transductor / Sala / Posición → sin
//     estimar: salen de la calibración P3 y el gemelo está en confianza
//     ~0.46 (< 0.80, NO controla audio). La potencia del Genezi y la
//     sensibilidad de sus bocinas están SIN MEDIR.
//   · Percepción estimada → sin estimar: la aporta P10 cuando exista.
//     Antes de P10 este término NO se inventa.
//
// El módulo NO procesa audio, NO toca buffers, NO tiene estado global y
// NO hace I/O: funciones puras (entrada → Δ / certificado). Por eso es
// seguro llamarlo desde cualquier hilo y su test puede probar que la
// señal no se modifica (no hay camino de audio que modificar).
//
// Regla de Oro Matched-LUFS: este módulo no aplica ganancia de ningún
// tipo; solo estima y certifica. Ganancia neta 0.0 dB por construcción.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace ivanna::fused::total_conservation {

// 9 etapas de la cadena (plan §2.1), en orden de recorrido de la señal.
enum class Stage : std::size_t {
    kSource = 0,         // fuente (formato/códec)
    kDecode = 1,         // decodificación (resampling / cambio de sample rate)
    kDsp = 2,            // DSP (ORBE/HOLOS/SALA VIVA/VOLTERRA/ARMONÍA/PULSO/NEURO)
    kConversion = 3,     // conversión (DAC JieLi 24-bit/48 kHz)
    kAmplification = 4,  // amplificación (Genezi)
    kTransducer = 5,     // transductor (bocinas del Genezi)
    kRoom = 6,           // sala (4.0×7.0 m, RT60 ~0.34 s)
    kPosition = 7,       // posición (distancia 3 m, desplazamiento del oyente)
    kPerception = 8,     // percepción estimada (fatiga, atención — P10)
};
inline constexpr std::size_t kNumStages = 9;

// 6 familias de alteración (plan §2.1): Δtotal es su suma por etapa.
enum class Family : std::size_t {
    kTimbre = 0,
    kTime = 1,
    kSpace = 2,
    kDynamics = 3,
    kTexture = 4,
    kAttention = 5,
};
inline constexpr std::size_t kNumFamilies = 6;

// 7 operadores DSP, mismo orden canónico que perceptual_mpc (ORBE, HOLOS,
// SALA VIVA, VOLTERRA, ARMONÍA, PULSO, NEURO).
inline constexpr std::size_t kNumOperators = 7;

enum class Codec : std::uint8_t {
    kWav = 0,      // sin pérdida
    kFlac = 1,     // sin pérdida
    kMp3 = 2,      // con pérdida
    kAac = 3,      // con pérdida
    kOpus = 4,     // con pérdida
    kUnknown = 5,  // formato no identificado
};

// Entrada de una reproducción para la estimación por etapa.
struct StageInput {
    Codec codec = Codec::kUnknown;
    // true si la decodificación es bit-exacta (sin resampling ni cambio
    // de sample rate). false = hubo resampling o no se sabe.
    bool decodeBitExact = false;
    // Amounts efectivos de los 7 operadores DSP en [0,1].
    std::array<double, kNumOperators> operatorAmounts{};
    // true si la conservación (P2) está activa con amount > 0.
    bool conservationActive = false;
};

// Δ de UNA etapa, en dB de pérdida (magnitud, >= 0).
// estimated=false significa "sin estimar": el término NO se conoce y NO
// se suma al Δtotal. deltaDb queda en 0.0 y confidence en 0.0 en ese caso.
struct StageDelta {
    bool estimated = false;
    double deltaDb = 0.0;    // magnitud de la pérdida en dB (>= 0, finita)
    double confidence = 0.0; // confianza en [0,1]; 0 si no estimado
    // Razón legible del estado: "medido", o por qué está sin estimar.
    const char* reason = "sin estimar";
};

// Estima el Δ de una etapa. Función pura. Devuelve un número (o el flag
// "sin estimar"), nunca un texto sin número: el test host lo exige.
StageDelta estimateDelta(Stage stage, const StageInput& input) noexcept;

// Certificado por reproducción (P7.2, plan §2.2 paso 2). Persistido en
// log, no en audio. NO altera el audio: buildCertificate es pura.
struct PlaybackCertificate {
    std::array<StageDelta, kNumStages> stages{};
    // Δtotal = suma de los Δ estimados. VÁLIDO solo si totalValid.
    double totalDb = 0.0;
    // true solo si las 9 etapas están estimadas. Mientras falte una, el
    // certificado lo declara: totalDb es entonces un PISO, no un total.
    bool totalValid = false;
    // Confianza del certificado: mínimo de las confianzas estimadas;
    // 0.0 si alguna etapa está sin estimar.
    double confidence = 0.0;
    // Número de etapas estimadas (0..9).
    std::size_t estimatedStages = 0;
    // ── Ξ_10 — ACCIÓN D10 (CORONA D10, 2026-11-25) ─────────────────────
    // Campo MEDIDO, fuera de las 9 etapas: NO entra en totalDb ni en
    // totalValid (la acción hamiltoniana es adimensional, no una pérdida
    // en dB, y no es una etapa del recorrido de la señal). Lo puebla
    // FusedChain::totalConservationCertificate() desde la telemetría del
    // motor D10 (telemetry().hamiltonianAction del último bloque
    // arbitrado). Regla dura del módulo: si el D10 está deshabilitado o
    // aún no ha arbitrado ningún bloque, el campo queda SIN ESTIMAR
    // (hasTenthDimension=false, acción NaN) — NUNCA inventado.
    bool hasTenthDimension = false;
    double tenthDimensionAction =
        std::numeric_limits<double>::quiet_NaN();  // acción hamiltoniana D10
    std::uint64_t trackIndex = 0;  // índice de la pista en la sesión host
    std::uint32_t ivannaVersionCode = 0;  // p. ej. 501
};

PlaybackCertificate buildCertificate(const StageInput& input,
                                     std::uint64_t trackIndex,
                                     std::uint32_t ivannaVersionCode) noexcept;

// Nombre corto de la etapa, para el log del certificado.
const char* stageName(Stage stage) noexcept;
const char* familyName(Family family) noexcept;

}  // namespace ivanna::fused::total_conservation
