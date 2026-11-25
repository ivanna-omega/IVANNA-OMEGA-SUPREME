// experience_compiler.hpp — P8 Compilador de Experiencia (Frente E, 2026-11-24).
//
// P8 del plan PLAN_P7_P10.md §3: IVANNA recibe una intención musical y la
// compila hacia el sistema físico real, encontrando la mejor solución
// dentro de lo que el hardware puede hacer — sin fingir capacidades.
//
// ALCANCE DE ESTE MÓDULO (P8.1 + P8.3, código sin teléfono):
//   · P8.1 — ExperienceVector E = [I, D, S, T, M, F, A] (imagen,
//     profundidad, escala, textura, microdinámica, fidelidad temporal,
//     atención/fatiga), 7 componentes en [0,1] + incertidumbre por
//     componente. Serialización, límites, distancia.
//   · P8.2 — ReachableRegion: rango alcanzable por componente, con flag
//     "medido" por componente. Estado 2026-11-24: NINGÚN rango está
//     medido (P3 sin calibrar, P6 nominal, P4 pesos sin calibrar), así
//     que todos los componentes quedan "sin medir" y el compilador
//     declara Unknown en vez de fingir alcanzabilidad.
//   · P8.3 — Compilador intención → parámetros: optimizador con ganancia
//     neta 0.0 dB como RESTRICCIÓN DURA. Fuera de la región se declara
//     "no alcanzable" y se devuelve el punto más cercano.
//   · P8.4 / P8.5 quedan BLOQUEADOS en Esse (escucha) — ver README.
//
// MAPA E → OPERADORES (NOMINAL, NO MEDIDO). El compilador usa un mapa
// diagonal documentado: I→ORBE, D→HOLOS, S→SALA_VIVA, T→VOLTERRA,
// M→ARMONÍA, F→PULSO, A→NEURO. Es un mapa de partida por razonamiento,
// no una calibración: los amounts que salen son una propuesta dentro de
// [0,1], no una predicción validada. Cuando P3/P4 calibren, el mapa se
// sustituye por la tabla medida; la interfaz no cambia.
//
// GANANCIA NETA 0.0 dB (RESTRICCIÓN DURA). Cada operador tiene un
// coeficiente NOMINAL de ganancia g_i (dB por unidad de amount), también
// por razonamiento y documentado como tal. El compilador aplica un trim
// maestro trimDb = −Σ g_i·amount_i, de modo que la ganancia neta es 0.0
// por construcción. El test host lo verifica en 50 intenciones: si
// |neta| > 1e-9 dB, el test falla. Los coeficientes nominales NO son la
// ganancia real medida de cada operador (esa medición es P2/P3); son el
// modelo del compilador, y el trim garantiza 0.0 dB DENTRO de ese modelo.
//
// El módulo NO procesa audio, NO toca buffers, NO tiene estado global y
// NO hace I/O: funciones puras (intención → parámetros).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ivanna::fused::experience_compiler {

// 7 componentes de E (plan §3.1), en este orden canónico.
enum class Component : std::size_t {
    kImage = 0,       // I imagen
    kDepth = 1,       // D profundidad
    kScale = 2,       // S escala
    kTexture = 3,     // T textura
    kMicrodynamics = 4,  // M microdinámica
    kTemporalFidelity = 5,  // F fidelidad temporal
    kAttention = 6,   // A atención/fatiga
};
inline constexpr std::size_t kNumComponents = 7;

// 7 operadores, mismo orden canónico que perceptual_mpc (ORBE, HOLOS,
// SALA VIVA, VOLTERRA, ARMONÍA, PULSO, NEURO).
inline constexpr std::size_t kNumOperators = 7;

// Vector de experiencia E: 7 componentes en [0,1] + incertidumbre por
// componente en [0,1] (0 = seguro, 1 = totalmente incierto).
struct ExperienceVector {
    std::array<double, kNumComponents> v{};
    std::array<double, kNumComponents> uncertainty{};
    // Por defecto la incertidumbre es 1.0 (máxima): sin datos, E es una
    // intención declarada, no una medición.
    ExperienceVector() { uncertainty.fill(1.0); }

    double& operator[](Component c) {
        return v[static_cast<std::size_t>(c)];
    }
    double operator[](Component c) const {
        return v[static_cast<std::size_t>(c)];
    }
    // true si las 7 componentes y las 7 incertidumbres son finitas y
    // están en [0,1].
    bool isValid() const noexcept;
    // Clamp a [0,1]; NaN → 0.0 con incertidumbre 1.0 (fail-closed: un
    // componente desconocido no se inventa, se marca incierto).
    void clampInPlace() noexcept;
};

// Distancia euclídea entre dos vectores E (0 = idénticos). NaN → +inf.
double distance(const ExperienceVector& a, const ExperienceVector& b) noexcept;

// Serialización estable "E=a,b,c,d,e,f,g;U=a,b,c,d,e,f,g" con 6 decimales.
// parse() devuelve false si el formato es inválido o hay valores fuera
// de [0,1]; en ese caso no modifica el destino.
std::string serialize(const ExperienceVector& e);
bool parse(const std::string& text, ExperienceVector* out) noexcept;

// Región alcanzable por componente (P8.2). min/max en [0,1] + flag
// "medido": true solo si el rango salió de mediciones (P3/P6). Hoy
// todos los flags son false: ningún rango está medido.
struct ReachableRegion {
    std::array<double, kNumComponents> min{};
    std::array<double, kNumComponents> max{};
    std::array<bool, kNumComponents> measured{};
    ReachableRegion() { max.fill(1.0); }  // [0,1] completo, sin medir
};

enum class Reachability : std::uint8_t {
    kAchievable = 0,  // dentro de rangos medidos
    kUnknown = 1,     // dentro de [0,1] pero con rangos sin medir:
                      // no se puede probar alcanzabilidad ni negarla
    kUnreachable = 2,  // fuera de [0,1] o fuera de un rango medido
};

// Clasifica un objetivo E contra la región. Un objetivo fuera de [0,1]
// o con NaN es Unreachable; dentro de [0,1] con algún rango sin medir
// es Unknown; solo con todos los rangos medidos y dentro de ellos es
// Achievable.
Reachability classify(const ExperienceVector& target,
                      const ReachableRegion& region) noexcept;

// Resultado de compilar una intención (P8.3).
struct CompileResult {
    // Amounts por operador en [0,1] (orden canónico).
    std::array<double, kNumOperators> amounts{};
    // Trim maestro en dB que compensa la ganancia nominal de los
    // operadores. Ganancia neta = Σ g_i·amount_i + trimDb = 0.0.
    double trimDb = 0.0;
    // Ganancia neta resultante (dB). Debe ser 0.0 (tolerancia 1e-9).
    double netGainDb = 0.0;
    Reachability reachability = Reachability::kUnknown;
    // Punto más cercano al objetivo dentro de la región alcanzable
    // (el objetivo clampado a [0,1] cuando es inalcanzable).
    ExperienceVector nearestPoint;
    // Distancia del objetivo a nearestPoint (0 si alcanzable/desconocido
    // dentro de [0,1]).
    double distanceToTarget = 0.0;
    bool ok = false;  // false solo con entrada no compilable (NaN total)
};

// Compila una intención E objetivo a amounts por operador + trim, con
// ganancia neta 0.0 dB como restricción dura. Si el objetivo está fuera
// de la región, devuelve el punto más cercano y declara la distancia;
// nunca fuerza lo inalcanzable.
CompileResult compile(const ExperienceVector& target,
                      const ReachableRegion& region) noexcept;

// Coeficientes NOMINALES de ganancia por operador (dB por unidad de
// amount). Por razonamiento, NO medidos. El trim los compensa exactamente.
double nominalGainDbPerUnit(std::size_t operatorIndex) noexcept;

const char* componentName(Component c) noexcept;

}  // namespace ivanna::fused::experience_compiler
