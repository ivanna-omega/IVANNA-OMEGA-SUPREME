// universal_hearing.hpp — Modelo Universal de Audición (P6, 2026-11-21).
//
// P6 de la visión de luna ("Majestuosidad Total", 2026-11-21): modelar la
// experiencia auditiva completa como
//   P(experiencia | x, H, R, L, C)
// donde x = señal, H = cadena/hardware, R = recinto, L = oyente,
// C = contexto y estado perceptual.
//
// Este módulo SOLO PREDICE. No procesa audio, no toca ningún buffer, no
// tiene estado global, no hace I/O: es una función pura (entrada →
// predicción). Por eso es seguro llamarlo desde cualquier hilo, incluido
// el de audio, y por eso su test puede probar que la señal no se modifica
// (no hay camino de audio que modificar).
//
// TODO LO QUE ES MODELO, NO MEDICIÓN, VA DOCUMENTADO COMO TAL. Cada
// constante del modelo es NOMINAL: calibración inicial por razonamiento,
// no medida del Genezi ni de la sala de Esse. Da órdenes de magnitud y
// tendencias (monotonía en la dirección correcta), no valores calibrados.
// Lo que falta medir está listado en el README.
//
// ENTRADAS EXPLÍCITAS, NADA INVENTADO. Cada struct de entrada trae sus
// campos con convención de "desconocido" documentada por campo:
//   · Un campo numérico en 0 (o -1) donde se marca "0 = desconocido"
//     significa "no se conoce": baja la confianza, NUNCA se inventa un
//     valor. Los campos donde 0 es un valor válido (p. ej. azimut 0 =
//     centrado, desbalance 0 = equilibrado) llevan un flag bool explícito
//     o no admiten desconocido.
//   · Un dato OPCIONAL desconocido aporta 0 a la pérdida que alimenta y
//     baja la confianza: la predicción es entonces un PISO, no un valor
//     central. Un dato REQUERIDO desconocido invalida la predicción
//     (valid=false, confianza 0): el consumidor DEBE checar valid.
// Requeridos: nivel de señal conocido, SPL objetivo conocido y, en
// altavoz, RT60 y volumen de sala conocidos y distancia del oyente > 0.
// En audífono la sala no se requiere (se ignora por completo).
//
// DERIVADOS, NO INVENTADOS. Si la distancia crítica o la coherencia no
// se dan pero sí se conocen volumen, RT60 y distancia, se DERIVAN con
// las mismas fórmulas del gemelo acústico (Dc = 0.057·√(V/RT60),
// coherencia = 1/(1+(d/Dc)²)). Un valor derivado baja la confianza
// (-0.05 cada uno) frente a uno medido: el módulo dice de dónde salió
// cada número.
//
// RELACIÓN CON P[t] (perceptual_mpc/perceptual_vector.hpp). El estado
// perceptual de este módulo tiene 16 dimensiones: las 9 primeras SON
// P[t] en EL MISMO ORDEN Y SEMÁNTICA (audibilidad, claridad,
// naturalidad, impacto, espacialidad, fatiga, estabilidad, confianza,
// presupuesto) y 7 nuevas de P6 (pérdida de profundidad, compresión
// espacial, enmascaramiento tímbrico, pérdida de inteligibilidad,
// desplazamiento de imagen, pérdida de microdinámica, pérdida de
// causalidad gesto-respuesta). La fatiga ya existe en P[t] y NO se
// duplica: la fatiga predicha aquí ES la dimensión 5 de P[t].
// Este módulo NO incluye headers del MPC ni del gemelo: misma semántica
// por construcción, para que un puente futuro (estilo twin_bridge.hpp)
// mapee 1:1. Las 8 primeras bandas coinciden con las del MPC; la 9ª
// (16 kHz) no tiene contraparte en el MPC y se ignora en ese mapeo
// (igual que hace twin_bridge con el gemelo).
//
// RELACIÓN CON EL GEMELO ACÚSTICO. El gemelo estima el trayecto físico;
// este módulo predice la experiencia a partir de entradas explícitas.
// Un puente futuro puede alimentar: ganancia/coherencia/latencia/THD/
// ruido del gemelo → SignalDesc/ChainDesc/RoomDesc de aquí. La
// correspondencia campo a campo está en el README; aquí NO hay acople.
//
// Parámetros del trayecto real de Esse (MEMORY.md): DAC USB JieLi
// 24-bit/48 kHz, Sony Genezi MHC-PZ1D (estéreo, flat, sub ON neutro,
// surround OFF), sala 4.0×7.0 m, escucha a 3 m, RT60 0.34 s, SPL
// objetivo 85 dB. La potencia del Genezi y la sensibilidad de sus
// bocinas NO son entradas de este módulo: la cadena se describe por
// THD y piso de ruido, que para Esse TAMPOCO están medidos (quedan en
// 0 = desconocidos) y la confianza lo refleja.
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ivanna::fused::universal_hearing {

// 9 bandas logarítmicas, 63 Hz .. 16 kHz (mismas que el gemelo acústico).
inline constexpr std::size_t kHearingBands = 9;
inline constexpr double kHearingBandHz[kHearingBands] = {
    63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

// 16 dimensiones: 9 de P[t] + 7 nuevas de P6. Todas en [0,1].
// En las 9 de P[t], 1 = "mejor" salvo fatiga y presupuesto (0 = mejor).
// En las 7 nuevas, 0 = sin pérdida (mejor) y 1 = pérdida total.
inline constexpr std::size_t kHearingDims = 16;

enum class Dim : std::size_t {
    // ── P[t] (mismo orden y semántica que perceptual_mpc) ────────────
    kAudibility = 0,   // cuánto se percibe la banda (0 inaudible, 1 pleno)
    kClarity = 1,      // inteligibilidad / separación (1 cristalina)
    kNaturalness = 2,  // ausencia de artefacto (1 natural)
    kImpact = 3,       // pegada del transitorio (1 máxima)
    kSpatiality = 4,   // anchura / profundidad percibida (1 envolvente)
    kFatigue = 5,      // aspereza acumulada (0 sin fatiga, 1 fatigante)
    kStability = 6,    // la imagen no se mueve (1 roca)
    kConfidence = 7,   // fiabilidad de la estimación (1 segura)
    kBudget = 8,       // presupuesto de intervención consumido (0 libre)
    // ── P6 (pérdidas: 0 sin pérdida, 1 pérdida total) ───────────────
    kDepthLoss = 9,            // pérdida de profundidad
    kSpatialCompression = 10,  // compresión espacial
    kTimbralMasking = 11,      // enmascaramiento tímbrico
    kIntelligibilityLoss = 12, // pérdida de inteligibilidad
    kImageShift = 13,          // desplazamiento de imagen (30° = 1.0)
    kMicrodynamicsLoss = 14,   // pérdida de microdinámica
    kCausalityLoss = 15,       // pérdida de causalidad gesto-respuesta
};

enum class ContentType : std::uint8_t {
    kMusic = 0,
    kVoice = 1,
    kMixed = 2,
};

enum class Transducer : std::uint8_t {
    kSpeaker = 0,    // Altavoz: hay distancia, aire y sala.
    kHeadphone = 1,  // Audífono: sin distancia, sin aire, sin sala.
};

// ── Señal (x) ────────────────────────────────────────────────────────
struct SignalDesc {
    // RMS por banda en dBFS. REQUERIDO: levelKnown debe ser true.
    // Rango válido [-120, 0].
    std::array<double, kHearingBands> rmsDbPerBand{};
    bool levelKnown{false};
    // Crest factor en dB. 0 = desconocido si !crestKnown.
    double crestFactorDb{0.0};
    bool crestKnown{false};
    // Densidad de transitorios 0..1. 0 = desconocido si !transientKnown.
    double transientDensity{0.0};
    bool transientKnown{false};
    // Fracción tonal 0..1 (1 = todo tonal/sostenido). 0 = desconocido
    // si !tonalKnown.
    double tonalFraction{0.0};
    bool tonalKnown{false};
    // Minutos de exposición al nivel actual. 0 = desconocido si
    // !exposureKnown.
    double exposureMinutes{0.0};
    bool exposureKnown{false};
};

// ── Cadena (H) ───────────────────────────────────────────────────────
struct ChainDesc {
    int dacBits{24};            // 16 / 24 / 32 (otro valor: penaliza confianza)
    int dacSampleRateHz{48000}; // 44100 / 48000 / 96000 / ...
    // THD en %. 0 = desconocido (nunca 0 real: ningún DAC tiene THD 0).
    double thdPercent{0.0};
    // Piso de ruido en dBFS. 0 = desconocido.
    double noiseFloorDb{0.0};
    // Latencia de buffer en ms (p. ej. 128/48000*1000 = 2.667 ms).
    double bufferLatencyMs{2.667};
    Transducer transducer{Transducer::kSpeaker};
    // Desbalance de canal en dB. 0 = equilibrado (valor válido).
    double channelImbalanceDb{0.0};
};

// ── Recinto (R) ──────────────────────────────────────────────────────
// En audífono TODA esta struct se ignora (no se valida ni se usa).
struct RoomDesc {
    // RT60 en s (500 Hz–1 kHz). 0 = desconocido. REQUERIDO en altavoz.
    double rt60Sec{0.0};
    // Volumen en m³. 0 = desconocido. REQUERIDO en altavoz.
    double roomVolumeM3{0.0};
    // Distancia crítica en m. 0 = desconocida (se deriva de V y RT60).
    double criticalDistanceM{0.0};
    // Coherencia directo/total 0..1. -1 = desconocida (se deriva de
    // distancia y distancia crítica). 0.0 es válido (todo reverberante).
    double coherence{-1.0};
    // Ruido ambiente en dB SPL. 0 = desconocido.
    double ambientNoiseDbSpl{0.0};
};

// ── Oyente (L) ───────────────────────────────────────────────────────
struct ListenerDesc {
    // Distancia a los altavoces en m. 0 = desconocida. REQUERIDA en
    // altavoz; en audífono se ignora.
    double distanceM{0.0};
    // Azimut en grados (-180..180). 0 = centrado (valor válido).
    double azimuthDeg{0.0};
    // Movimiento de cabeza 0..1 (0 quieto, 1 moviéndose mucho).
    double headMovement{0.0};
};

// ── Contexto (C) ─────────────────────────────────────────────────────
struct ContextDesc {
    ContentType content{ContentType::kMusic};
    // SPL objetivo en dB. 0 = desconocido. REQUERIDO (la fatiga y la
    // inteligibilidad dependen del nivel absoluto).
    double targetSplDb{0.0};
    // Fatiga previa acumulada 0..1 (0 = fresco).
    double priorFatigue{0.0};
    // Minutos de sesión (distinto de la exposición al nivel actual:
    // la sesión larga cansa aunque el nivel sea seguro).
    double sessionMinutes{0.0};
};

// Entrada completa del modelo: P(experiencia | x, H, R, L, C).
struct UniversalHearingInput {
    SignalDesc signal;
    ChainDesc chain;
    RoomDesc room;
    ListenerDesc listener;
    ContextDesc context;

    // Cadena real de Esse (MEMORY.md): DAC JieLi 24-bit/48 kHz →
    // Genezi (flat, sub ON neutro, surround OFF) → sala 4.0×7.0 m,
    // escucha a 3 m, RT60 0.34 s, SPL 85 dB. THD y piso de ruido NO
    // medidos (0 = desconocidos). Distancia crítica y coherencia no
    // medidas: se derivan (penalizan la confianza).
    static UniversalHearingInput esseGenezi() noexcept;
    // Mismo DAC y señal, audífono en vez de bocinas (sin sala, sin aire).
    static UniversalHearingInput esseHeadphones() noexcept;
};

// Estado perceptual por banda: 16 dimensiones en [0,1].
struct UniversalHearingState {
    std::array<double, kHearingDims> v{};

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

    // Estado neutro: las 9 dims de P[t] idénticas al neutral() del MPC
    // (todo a mitad salvo fatiga baja, confianza alta y presupuesto
    // libre); las 7 pérdidas de P6 en 0 (sin pérdida).
    static UniversalHearingState neutral() {
        UniversalHearingState s;
        s.v = {0.5, 0.5, 0.5, 0.5, 0.5, 0.2, 0.5, 0.9, 0.0,
               0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        return s;
    }
};

// Predicción completa del modelo.
struct UniversalHearingPrediction {
    std::array<UniversalHearingState, kHearingBands> band{};
    // Medias globales por dimensión (media aritmética de las 9 bandas).
    std::array<double, kHearingDims> globalMean{};
    double confidence{0.0};         // 0..1: confianza global (media de bandas)
    double interventionScale{0.0};  // = confidence: a confianza baja se
                                    // interviene MENOS (nunca al revés).
    double imageShiftDegrees{0.0};  // desplazamiento de imagen en grados
    double totalLatencyMs{0.0};     // buffer + aire (0 aire en audífono)
    double effectiveCriticalDistanceM{0.0};  // dada o derivada (0 audífono)
    double effectiveCoherence{0.0};         // dada o derivada (1 audífono)
    double schroederHz{0.0};                // 0 en audífono
    // CONTRATO FAIL-CLOSED: valid=false significa "sin predicción".
    // Entonces confidence = 0, interventionScale = 0 y TODAS las
    // predicciones valen 0.0 (definido, nunca NaN/inf). El consumidor
    // DEBE checar valid antes de usar cualquier otro campo: un 0.0 en
    // una predicción inválida significa "no sé", no "pérdida cero".
    bool valid{false};
};

// El Modelo Universal de Audición. Función pura: sin estado global, sin
// I/O, sin tocar audio. Determinista: misma entrada → misma salida,
// bit a bit.
class UniversalHearingModel {
public:
    static UniversalHearingPrediction predict(
        const UniversalHearingInput& input) noexcept;
};

}  // namespace ivanna::fused::universal_hearing
