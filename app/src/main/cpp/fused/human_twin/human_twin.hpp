// human_twin.hpp — P10 Gemelo Humano (Frente E, 2026-11-24).
//
// P10 del plan PLAN_P7_P10.md §5: el modelo pasa de representar el
// sistema a representar la interacción sistema–persona: qué alteraciones
// detecta Esse realmente, cuáles le molestan, qué prefiere aunque no sea
// neutral, cuándo su confianza cae, cuándo una mejora objetiva no produce
// mejora subjetiva.
//
// ALCANCE DE ESTE MÓDULO (P10.1, código sin teléfono):
//   · Banco de pruebas (módulo standalone `fused/human_twin/`): las 8
//     pruebas breves de calibración del plan (§5.2) como tipos, cada una
//     con SU eje (nunca mezclados, §5.1).
//   · Estructura de registro por prueba: estímulo, respuesta, tiempo,
//     eje. Serialización ida y vuelta del registro.
//   · Modelo de sensibilidad: estima P(Esse detecta) y P(le molesta).
//     SIN BASELINE está en prior de azar (0.5) y declara
//     calibrated=false: no finge conocer a Esse.
//   · P10.2 / P10.3 / P10.4 quedan BLOQUEADOS en Esse (sus sesiones) y
//     en P7/P8/P9 — ver README.
//
// SEPARACIÓN DE EJES (§5.1). Cada prueba pertenece a exactamente UN eje:
//   1. Conservación de la obra — ¿llega lo que la mezcla contiene?
//   2. Exactitud física — ¿el sistema reproduce lo que se le pide?
//   3. Preferencia del oyente — ¿qué le gusta a Esse?
//   4. Adaptación contextual — ¿qué conviene en este momento/contenido?
// Una prueba que mezcla ejes queda invalidada: el registro rechaza
// (append devuelve false) cualquier TrialRecord cuyo eje no coincida con
// el eje de su tipo de prueba.
//
// MAPA PRUEBA → EJE (documentado, fijo):
//   ABX adaptativo        → Conservación (detecta diferencias entre
//                            procesamientos: ¿llega la obra?)
//   Detección sí/no       → Conservación (sensibilidad por banda/tipo)
//   Comparación profundidad → Conservación (sensibilidad a D de E)
//   Percepción de textura → Conservación (sensibilidad a T de E)
//   Sensibilidad a fatiga → Adaptación contextual (caída con el tiempo
//                            de escucha: ¿qué conviene en este momento?)
//   Preferencia tímbrica  → Preferencia (qué balance prefiere, separado
//                            de lo neutral)
//   Tolerancia a latencia → Exactitud física (umbral en que la latencia
//                            se vuelve perceptible/molesta)
//   Memoria de referencia → Preferencia (estabilidad de su referencia
//                            interna entre sesiones)
//
// El módulo NO procesa audio, NO toca buffers, NO tiene estado global y
// NO hace I/O: estructuras de datos + funciones puras. La UI de pruebas
// en la app (P10.1) NO se construye aquí: exige tocar la app y queda
// fuera de este frente (sin cablear nada en la app).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ivanna::fused::human_twin {

// 4 ejes, nunca mezclados (plan §5.1).
enum class Axis : std::uint8_t {
    kConservation = 0,       // conservación de la obra
    kPhysicalAccuracy = 1,   // exactitud física
    kPreference = 2,         // preferencia del oyente
    kContextual = 3,         // adaptación contextual
};

// 8 pruebas breves de calibración (plan §5.2).
enum class TestType : std::uint8_t {
    kAbxAdaptive = 0,        // ABX adaptativo, ~15 min
    kDetection = 1,          // detección sí/no, ~10 min
    kDepthComparison = 2,    // comparación de profundidad, ~10 min
    kTexturePerception = 3,  // percepción de textura, ~10 min
    kFatigueSensitivity = 4, // sensibilidad a fatiga, ~15 min
    kTimbralPreference = 5,  // preferencia tímbrica, ~10 min
    kLatencyTolerance = 6,   // tolerancia a latencia, ~10 min
    kReferenceMemory = 7,    // memoria de referencia, ~5 min
};
inline constexpr std::size_t kNumTestTypes = 8;

// Eje al que pertenece cada prueba (fijo, ver header).
Axis testAxis(TestType type) noexcept;
const char* testName(TestType type) noexcept;
const char* axisName(Axis axis) noexcept;
// Duración nominal por sesión en minutos (plan §5.2).
int testDurationMin(TestType type) noexcept;

// Un ensayo registrado: estímulo, respuesta, tiempo, eje.
struct TrialRecord {
    TestType type = TestType::kAbxAdaptive;
    Axis axis = Axis::kConservation;  // debe ser testAxis(type)
    std::uint32_t stimulusId = 0;     // id del estímulo presentado
    std::int32_t response = 0;        // respuesta de Esse (código por prueba)
    double responseTimeMs = 0.0;      // tiempo de respuesta (>= 0)
    bool correct = false;             // true si la respuesta fue correcta
    std::uint64_t timestampMs = 0;    // marca temporal de la sesión
};

// true si el registro es coherente: eje == testAxis(type), tiempo finito
// y >= 0, estímulo identificado.
bool isValidRecord(const TrialRecord& r) noexcept;

// Registro de una sesión (o de varias): solo acepta registros válidos.
// append devuelve false y no guarda nada si el registro es inválido
// (p. ej. eje mezclado: la prueba queda invalidada, no promediada).
class TrialLog {
public:
    bool append(const TrialRecord& r) noexcept;
    std::size_t size() const noexcept { return trials_.size(); }
    const TrialRecord& at(std::size_t i) const { return trials_.at(i); }
    std::size_t countByTest(TestType type) const noexcept;
    std::size_t countByAxis(Axis axis) const noexcept;
    // Serialización estable, una línea por ensayo. parse() devuelve false
    // si alguna línea es inválida; en ese caso no modifica el log.
    std::string serialize() const;
    bool parse(const std::string& text) noexcept;

private:
    std::vector<TrialRecord> trials_;
};

// Modelo de sensibilidad (P10.3, estructura + prior).
// Sin baseline (P10.2) el modelo está en prior de azar y lo declara.
struct SensitivityEstimate {
    double pDetect = 0.5;   // P(Esse detecta la alteración)
    double pBothers = 0.5;  // P(le molesta)
    double confidence = 0.0;  // 0 mientras no esté calibrado
    bool calibrated = false;
};

class SensitivityModel {
public:
    // Predicción para una alteración dada (en dB). Sin calibrar devuelve
    // el prior de azar (0.5 / 0.5, confianza 0): no finge conocer a Esse.
    SensitivityEstimate predict(TestType type, double alterationDb) const noexcept;
    bool isCalibrated() const noexcept { return calibrated_; }
    // Ajuste con ensayos reales (P10.2). Exige >= 20 ensayos del mismo
    // tipo con respuestas correctas/incorrectas; si no, devuelve false y
    // el modelo SIGUE sin calibrar (fail-closed). Con suficientes datos,
    // pDetect = tasa empírica de acierto y la confianza crece con el
    // número de ensayos (máx. 0.9). Estimador crudo y documentado como
    // tal: no es un ajuste psicométrico completo.
    bool fit(TestType type, const TrialLog& log) noexcept;

private:
    bool calibrated_ = false;
    TestType fittedType_ = TestType::kAbxAdaptive;
    double pDetect_ = 0.5;
    double pBothers_ = 0.5;
    double confidence_ = 0.0;
};

// Mínimo de ensayos para calibrar el modelo con un tipo de prueba.
inline constexpr std::size_t kMinTrialsToCalibrate = 20;

}  // namespace ivanna::fused::human_twin
