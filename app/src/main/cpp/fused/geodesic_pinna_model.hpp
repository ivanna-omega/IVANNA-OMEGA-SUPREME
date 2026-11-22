// geodesic_pinna_model.hpp — FÍSICA NUEVA: OPERADOR DE INTERFERENCIA
// GEODÉSICA (pabellón) — 2026-11-18, Trabajador A.
//
// La esfera rígida (physical_head_model.hpp) es física EXISTENTE y NO
// explica los cues espectrales del pabellón. Este módulo formula un modelo
// NUEVO — la Ley Geodésica del Notch (LGN) — que deriva los notches de
// pinna de la HRTF desde UN solo principio, en vez de leerlos de una tabla
// medida:
//
// PRINCIPIO (interferencia de dos caminos geodésicos): la presión en el
// oído es la suma de la onda que entra directa al canal y las ondas que
// llegan tras desviarse por los bordes del pabellón (hélix / antihélix).
// Cada borde es un dispersor puntual fijo en la cabeza; la diferencia de
// camino entre el camino directo y el camino por el borde j es la
// proyección del vector fijo canal→borde sobre la dirección de incidencia:
//
//   ΔL_j(θ, φ) = M_j − B_j · sin(φ − φ0_j)  (+ corrección de azimut, ver abajo)
//
// (M_j = desvío medio, B_j = |vector canal→borde| proyectado, φ0_j = su
// inclinación; todo en mm). La interferencia destructiva (ωτ_j = π) da el
// notch de orden cero de la familia j:
//
//   f_N,j(φ) = c / (2 · ΔL_j(φ))
//
// DOS FAMILIAS medidas en el SOFA MIT KEMAR (az = 0, oído L = R por
// simetría; 12 y 10 elevaciones, −40°…+90°):
//   familia BAJA: M = 23.90 mm, B = 8.70 mm, φ0 = −18.0°
//   familia ALTA: M = 16.00 mm, B = 3.48 mm, φ0 = −31.0°
// Verificación (leave-one-out, predicción de elevaciones NO usadas en el
// ajuste): familia BAJA RMS 147 Hz, máx 278 Hz (nulo f=cte: RMS 1718 Hz);
// familia ALTA RMS 445 Hz, máx 904 Hz (nulo: 1242 Hz). Ajuste completo:
// BAJA RMS 104 Hz / máx 174 Hz; ALTA RMS 273 Hz / máx 487 Hz.
// El modelo predice la MIGRACIÓN del notch con la elevación (6.37 kHz a
// −40° → 11.16 kHz a +70°, familia baja) — algo que la esfera NO puede
// hacer en absoluto (su |H| es suave y sin notches).
//
// OPERADOR UNIFICADO (cabeza + pabellón en una sola ecuación):
//   H_ear(f, θ, φ) = H_esfera(f, θ) · F_pinna(f, φ)
//   F_pinna = [1 + ρ1·e^{iωτ1} + ρ2·e^{iωτ2}] / (1 + ρ1 + ρ2),
//   τ_j = ΔL_j/c, ρ1 = 0.70, ρ2 = 0.55 (de la profundidad del notch a
//   elev 0; la profundidad varía con la elevación, el modelo la aproxima).
// A baja frecuencia F_pinna → 1 (el pabellón no toca ITD/ILD); a alta
// frecuencia genera los notches y su migración.
//
// ALCANCE HONESTO (límites medidos, no excusas):
//  - La ley es INTERPOLATIVA en elevación: el ajuste con elev ≤ 10° NO
//    extrapola a elev ≥ 20° (error hasta +3.4 kHz) — los 3 parámetros no
//    se identifican desde un rango corto. Dentro del rango medido predice.
//  - Azimut (elev 0): el notch bajo migra 8.0 → 10.0 kHz de az 0° a 150°.
//    La corrección implementada (término −D_az·(1−cosθ)/2, D_az = 4.4 mm)
//    es de PRIMER ORDEN y aproximada (residuos ±400 Hz, 9 % a 180°).
//    NO está verificada al nivel de la ley de elevación.
//  - La profundidad del notch se aproxima (ρ constantes); la medida varía
//    6–35 dB con la dirección. El modelo acierta la POSICIÓN del notch,
//    no su profundidad exacta.
//  - La estructura fina (peine ~480 Hz, mínimos múltiples) NO la explica
//    este operador de 2 caminos: hacen falta más dispersores o la
//    resonancia del conducto. El operador captura las DOS familias
//    principales, no todo el espectro.
#pragma once

#include <complex>

namespace ivanna::physical {

// Constantes de la Ley Geodésica del Notch (KEMAR, pabellón normal).
// M, B en mm; phi0 en radianes. Derivadas del SOFA MIT KEMAR medido
// (ver cabecera); son geometría del pabellón, independientes de c.
struct PinnaNotchFamily {
    double meanPathMm;   // M_j: desvío medio canal→borde (mm)
    double amplitudeMm;  // B_j: proyección del vector canal→borde (mm)
    double phaseRad;     // phi0_j: inclinación del vector (rad)
    double rho;          // amplitud relativa del camino por el borde
};
inline constexpr PinnaNotchFamily kPinnaLowFamily{23.90, 8.70, -0.3141592653589793, 0.70};
inline constexpr PinnaNotchFamily kPinnaHighFamily{16.00, 3.48, -0.5410520681188612, 0.55};
// Corrección de azimut de primer orden (familia baja, elev 0): el notch
// migra 8.0 -> 10.0 kHz de az 0 a 150 deg. APROXIMADA (ver cabecera).
inline constexpr double kPinnaAzimuthDropMm = 4.4;

// Diferencia de camino geodésica (mm) del borde j a una elevación dada.
//   elevationRad : elevación de la fuente (0 = horizontal, + = arriba).
double pinnaPathDifferenceMm(const PinnaNotchFamily& family, double elevationRad) noexcept;

// Diferencia de camino con corrección de azimut de primer orden.
//   azimuthRad : 0 = frontal. A az = 0 coincide con la ley de elevación.
double pinnaPathDifferenceMm(const PinnaNotchFamily& family, double elevationRad,
                             double azimuthRad) noexcept;

// Frecuencia del notch (Hz) de la familia j: c / (2·ΔL).
// Devuelve 0 si ΔL <= 0 (fail-closed).
double pinnaNotchFrequencyHz(const PinnaNotchFamily& family, double elevationRad,
                             double speedOfSoundMps = 343.0) noexcept;

// Factor del pabellón F_pinna(f): 1 a baja frecuencia; genera los notches
// y su migración con la elevación. |F| <= 1. f <= 0 devuelve 1.
std::complex<double> pinnaFactor(double freqHz, double elevationRad, double azimuthRad = 0.0,
                                 double speedOfSoundMps = 343.0) noexcept;

// Operador unificado cabeza + pabellón (un solo principio, dos escalas):
// H = H_esfera(f, azimut) · F_pinna(f, elevación, azimut), por oído.
// El factor de pabellón es el mismo para ambos oídos en esta versión
// (la ley de elevación es simétrica L/R a az = 0; el refinamiento
// por-oído del azimut es trabajo abierto, ver cabecera).
// Devuelve false con argumentos inválidos (sin tocar las salidas).
bool headPlusPinnaEarTransfer(double freqHz, double azimuthRad, double elevationRad,
                              std::complex<double>& outLeft, std::complex<double>& outRight,
                              double speedOfSoundMps = 343.0) noexcept;

}  // namespace ivanna::physical
