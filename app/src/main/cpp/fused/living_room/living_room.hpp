// living_room.hpp — P9 Sala Viva (Frente E, 2026-11-24).
//
// P9 del plan PLAN_P7_P10.md §4: la sala deja de ser una condición fija y
// se convierte en un instrumento con función objetivo perceptual.
//
// ALCANCE DE ESTE MÓDULO (simulador en host, código sin teléfono):
//   · Modelo de sala del gemelo: modos axiales por posición, frecuencia
//     de Schroeder, distancia crítica, Δsala por distancia del oyente.
//   · Gate del plan (§4.3): el simulador predice el cambio de Δsala al
//     mover al oyente 1 m.
//   · Los 7 modos de la tabla del plan como DATOS (nombre, función
//     objetivo, intervención), no como efectos activos: este módulo NO
//     procesa audio y NO aplica ningún modo.
//   · Escucha real en 3 posiciones por modo: BLOQUEADA en Esse.
//
// DATOS DE LA SALA (plan §4.2 + gemelo acústico):
//   · Sala 4.0 × 7.0 m, escucha a 3 m, RT60 ~0.34 s, Schroeder ~139 Hz,
//     distancia crítica ~0.82 m. Estos valores vienen del gemelo y del
//     plan; NO son una medición nueva.
//   · ALTURA NOMINAL 2.5 m: la altura de la sala NO está medida. El
//     volumen (70 m³) y todo lo que deriva de él (Schroeder, Dc, modos
//     verticales) usan 2.5 m como supuesto nominal documentado, no como
//     medición. Cuando P3 mida la sala, la altura se sustituye.
//   · Verificación de consistencia: con V=70 m³ y RT60=0.34 s,
//     Schroeder = 2000·√(0.34/70) ≈ 139.4 Hz y
//     Dc = 0.057·√(70/0.34) ≈ 0.82 m: coinciden con los valores del plan.
//
// LÍMITE HONESTO (plan §4.3): con un solo par de bocinas y un solo
// micrófono de teléfono, la Sala Viva es control de UN punto de escucha,
// no control de toda la sala. Este simulador predice en puntos, no en
// mapas: no existe mapa acústico multipunto con este hardware.
//
// El módulo NO procesa audio, NO toca buffers, NO tiene estado global y
// NO hace I/O: funciones puras (geometría + posición → predicción).
#pragma once

#include <array>
#include <cstddef>

namespace ivanna::fused::living_room {

// Geometría de la sala de Esse. La altura es NOMINAL (no medida).
struct RoomGeometry {
    double lengthM = 7.0;   // largo (eje de escucha)
    double widthM = 4.0;    // ancho
    double heightM = 2.5;   // alto — NOMINAL, no medido
    double rt60Sec = 0.34;  // RT60 ~0.34 s (referencia del gemelo/plan)
    double speedOfSoundMps = 343.0;
};

inline constexpr std::size_t kNumModes = 7;

// 7 modos de la tabla del plan (§4.1). Solo datos: ningún modo se aplica
// en este módulo.
enum class RoomMode : std::size_t {
    kConservation = 0,  // mínima intervención
    kReference = 1,     // máxima neutralidad verificable
    kMonumental = 2,    // escala y energía controladas
    kIntimate = 3,      // proximidad y baja dispersión
    kHistorical = 4,    // recreación de una firma de época
    kDirector = 5,      // punto de escucha adaptable (sigue al oyente)
    kBlind = 6,         // sin pistas visuales ni expectativas (protocolo)
};

struct ModeInfo {
    const char* name = "";
    const char* objective = "";     // función objetivo
    const char* intervention = "";  // intervención que describe el modo
    // true si el modo se puede probar con el hardware actual (G85 + JieLi
    // + Genezi + sala 4×7). Todos son probables por escucha; ninguno exige
    // hardware que Esse no tenga.
    bool testableWithCurrentHardware = true;
};

ModeInfo modeInfo(RoomMode mode) noexcept;

// Volumen de la sala en m³ (con altura nominal).
double roomVolumeM3(const RoomGeometry& room) noexcept;
// Frecuencia de Schroeder en Hz: 2000·√(RT60/V).
double schroederHz(const RoomGeometry& room) noexcept;
// Distancia crítica en m: 0.057·√(V/RT60).
double criticalDistanceM(const RoomGeometry& room) noexcept;

// Frecuencia del modo axial n (1, 2, 3, ...) en el eje dado.
// axis: 0 = largo, 1 = ancho, 2 = alto. n < 1 → 0.0 (inválido).
double axialModeHz(const RoomGeometry& room, int axis, int n) noexcept;

// Δsala en dB a una distancia d del oyente: energía reverberante
// relativa al sonido directo, 10·log10(1 + (d/Dc)²). d debe ser > 0 y
// finito; si no, devuelve 0.0 y valid=false vía roomDelta().
struct RoomDeltaResult {
    double deltaDb = 0.0;
    bool valid = false;
};

RoomDeltaResult roomDeltaDb(const RoomGeometry& room,
                            double listenerDistanceM) noexcept;

// Gate P9 (§4.3): cambio predicho de Δsala al mover al oyente de fromM
// a toM (p. ej. 1 m). valid=false si alguna distancia es inválida.
struct DeltaChangeResult {
    double changeDb = 0.0;  // roomDelta(to) − roomDelta(from)
    bool valid = false;
};

DeltaChangeResult predictDeltaChange(const RoomGeometry& room, double fromM,
                                     double toM) noexcept;

// Posiciones de escucha del plan: referencia 3.0 m; segunda posición
// 1.5 m y 4.5 m.
inline constexpr double kReferenceListenerM = 3.0;
inline constexpr double kNearListenerM = 1.5;
inline constexpr double kFarListenerM = 4.5;

}  // namespace ivanna::fused::living_room
