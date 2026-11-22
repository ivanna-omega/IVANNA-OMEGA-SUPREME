// physical_propagation.hpp — Propagación física de la onda para ORBE·Ω.
//
// DOS PARTES, CON HONESTIDAD EXPLÍCITA SOBRE QUÉ ES CADA UNA:
//
// ── PARTE I — PISO: re-derivación de física existente ─────────────────────
// Leyes conocidas (ecuación de onda, ISO 9613-1, Huygens/WFS, Woodworth)
// re-escritas como funciones puras en doble precisión para reemplazar las
// heurísticas de fused_spatial.cpp. NADA de esto es física nueva: es la
// física estándar, puesta en forma utilizable por el camino caliente.
// Ver PHYSICAL_PROPAGATION_MAP.md ("mapa de reemplazo").
//
// ── PARTE II — LEY DE CURVATURA DE ORBE (nueva formulación) ───────────────
// Una sola ecuación, parametrizada por la CURVATURA LOCAL DEL FRENTE DE
// ONDA κ = 1/r, que da los cues binaurales en campo cercano Y campo
// lejano sin cambio de régimen. Ecuaciones explícitas, predicciones
// verificables y el veredicto de honestidad completo en
// PHYSICAL_PROPAGATION_MAP.md §3–§5. Resumen:
//
//   ε = a·κ = a/r  (radio de cabeza × curvatura; ε → 0 = campo lejano)
//   θf = azimut plegado a [0, π/2] (simetría frontal/trasera + antisimetría)
//   θ* = asin(ε)   (ángulo de cambio de rama: el rayo directo ipsilateral
//                    deja de estar bloqueado por la esfera)
//
//   Rama C (|θf| ≤ θ*, AMBOS oídos por onda reptante — EXACTA, no serie):
//     ITD = (a/c) · 2·θf
//
//   Rama D (θ* < |θf| ≤ π/2, ipsilateral directo + contralateral reptante):
//     ITD = (a/c)·[ θf + sinθf
//                  + (ε/2)·sin²θf
//                  − (ε²/2)·sinθf·cos²θf
//                  + ε³·(1/6 + (5/8)sin⁴θf − (3/4)sin²θf)
//                  + ε⁴·((7/8)sin⁵θf − (5/4)sin³θf + (3/8)sinθf)
//                  + ε⁵·((21/16)sin⁶θf − (35/16)sin⁴θf + (15/16)sin²θf − 1/20)
//                  + O(ε⁶) ]
//
//   Límite κ → 0 (ε → 0): ITD → (a/c)(θf + sinθf) = Woodworth exacto.
//   Exceso de campo cercano a 90°: ΔITD ≈ (a/c)(ε/2) — el exceso de ITD
//   sobre Woodworth ES en sí mismo un cue de distancia (inversible en
//   forma cerrada: distanceFromItdExcess). Corolario de acercamiento
//   (looming): κ̇ = κ²·v_r, tiempo de contacto τ = 1/(κ·v_r).
//
// Funciones puras, sin estado, sin malloc/locks/I-O: seguras en el hilo
// de audio y testeables sin audio. Fail-closed: entradas no finitas o
// fuera de rango devuelven el valor neutro documentado en cada función
// (nunca NaN, nunca infinito).
//
// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once

#include <cmath>

namespace ivanna::fused::propagation {

// ── Constantes ─────────────────────────────────────────────────────────────
inline constexpr double kSpeedOfSoundMps = 343.0;    // c, aire ~20 °C (la que
                                                      // ya usan WFS/ORBE)
inline constexpr double kReferenceDistanceM = 1.0;    // referencia 0 dB
inline constexpr double kHeadRadiusKemarM = 0.0875;   // a, cabeza KEMAR
inline constexpr double kPi = 3.14159265358979323846;

// ═══════════════════════════════════════════════════════════════════════════
// PARTE I — PISO (física existente, re-derivada)
// ═══════════════════════════════════════════════════════════════════════════

// Velocidad del sonido en aire seco, c(T) = 331.3·sqrt(1 + T/273.15).
// c(20 °C) = 343.21 m/s — por eso el código usa 343.0 (≈19.5 °C).
// Entrada no finita → 343.0. T fuera de [−40, 60] °C → clamp al borde.
double speedOfSound(double tempCelsius) noexcept;

// Divergencia esférica (onda puntual, solución de la ecuación de onda):
// presión ∝ 1/r. Ganancia lineal RELATIVA a rRef (1.0 = identidad en rRef).
// sphericalSpreadingGain(2, 1) = 0.5 (−6.02 dB por duplicar distancia).
// r <= 0 o no finito → 1.0 (fail-closed, nunca ∞).
double sphericalSpreadingGain(double distanceM, double refDistanceM = kReferenceDistanceM) noexcept;
double sphericalSpreadingDb(double distanceM, double refDistanceM = kReferenceDistanceM) noexcept;

// Divergencia cilíndrica (onda 2.5D / array lineal — la que USA WFS:
// WfsRenderer::computeTapTargets aplica 1/sqrt(d), NO 1/d; ambas son
// física, en regímenes distintos: fuente puntual esférica 1/r, fuente
// lineal / WFS 2.5D 1/sqrt(r)). Ganancia relativa a rRef.
double cylindricalSpreadingGain(double distanceM, double refDistanceM = kReferenceDistanceM) noexcept;

// Ganancia de distancia de escena CON los topes de seguridad que ya usa
// HrirPoseSelect::sceneDistanceGain: 1/r clampado a [minGain,
// 10^(maxGainDb/20)]. Defaults = comportamiento actual (tope +3 dB al
// acercarse, −6 dB a 2 m). Los topes NO son física: son seguridad
// (evitan recorte cerca y −∞ lejos). La ley física pura es
// sphericalSpreadingGain(); esta función es el reemplazo drop-in.
double sceneDistanceGainPhysical(double distanceM, double minGain = 0.5,
                                 double maxGainDb = 3.0) noexcept;

// Retardo de propagación: τ = r/c. delay(1 m) = 2.915 ms; en muestras a
// 48 kHz = 139.94. r <= 0 o no finito → 0.
double propagationDelaySeconds(double distanceM, double speedOfSound = kSpeedOfSoundMps) noexcept;
double propagationDelaySamples(double distanceM, double sampleRate,
                               double speedOfSound = kSpeedOfSoundMps) noexcept;

// Absorción del aire, ISO 9613-1:1993 (fórmula completa, no tabla):
// α(f, T, HR, p) en dB/m, con frecuencias de relajación del oxígeno y
// del nitrógeno. Verificado contra las tablas publicadas (§ test):
// 20 °C / 70 % HR → 1 kHz: 0.00498 dB/m; 4 kHz: 0.02309; 8 kHz: 0.07763.
// A distancias ORBE (0.15–2 m) es despreciable (8 kHz a 2 m: 0.16 dB);
// a 200 m es el término dominante en agudos (8 kHz: 15.5 dB).
// f <= 0 o no finita → 0. HR fuera de [0, 1] → clamp. T fuera de
// [−20, 50] °C → clamp al borde (rango de validez de la norma).
double airAbsorptionCoeffDbPerM(double freqHz, double tempCelsius = 20.0,
                                double humidityRel = 0.70,
                                double pressurePa = 101325.0) noexcept;
double airAbsorptionDb(double freqHz, double distanceM, double tempCelsius = 20.0,
                       double humidityRel = 0.70, double pressurePa = 101325.0) noexcept;

// Doppler (fuente en movimiento, observador fijo):
//   f' = f · c/(c − v_r),  v_r > 0 = fuente que SE ACERCA (radial).
// dopplerFactor(10 m/s) = 1.03003 (+3.0 % a 1 kHz: +30.0 Hz).
// |v_r| >= 0.9·c o no finito → factor 1.0 (fail-closed).
double dopplerFactor(double radialVelocityMps, double speedOfSound = kSpeedOfSoundMps) noexcept;
double dopplerShift(double freqHz, double radialVelocityMps,
                    double speedOfSound = kSpeedOfSoundMps) noexcept;

// Mezcla de potencia constante (la que usa fused_spatial.cpp para el
// wet WFS): dry = cos(t·π/2), wet = sin(t·π/2), dry² + wet² = 1.
// Es conservación de POTENCIA — válida para señales no correlacionadas;
// seco y WFS provienen de la MISMA señal (parcialmente correladas), así
// que aquí es convención de mezcla, no ley de la onda. t fuera de
// [0, 1] → clamp.
struct ConstantPowerMix {
    double dry = 1.0;
    double wet = 0.0;
};
ConstantPowerMix constantPowerMix(double t) noexcept;

// Parallax de campo cercano por geometría exacta (rayos + onda reptante
// sobre la esfera, modelo de Kuhn / Duda & Martens — el mismo que
// NearFieldHeadModel implementa en float; aquí en double, autocontenido):
// caminos acústicos a cada oído, ITD = (P_L − P_R)/c, ganancias 1/P.
// Convención: azimut 0 = frontal, + = derecha; ITD > 0 = fuente a la
// derecha (oído izquierdo más tarde); ildDb > 0 = oído derecho más
// intenso. A 90° / 1 m: ITD = 667.0 µs, ILD por divergencia = 1.94 dB.
// A 90° / 0.15 m: ITD = 732.6 µs, ILD por divergencia = 14.01 dB
// (el resto del ILD medido en graves es sombra por difracción, que el
// modelo de rayos NO captura: ver mapa, honestidad).
struct NearFieldPaths {
    double pathLeftM = 1.0;    // camino acústico al oído izquierdo (m)
    double pathRightM = 1.0;   // camino acústico al oído derecho (m)
    double itdSeconds = 0.0;   // (P_L − P_R)/c
    double gainLeft = 1.0;     // 1/P_L (divergencia esférica por oído)
    double gainRight = 1.0;    // 1/P_R
    double ildDivergenceDb = 0.0;  // 20·log10(P_L/P_R): solo divergencia
};
NearFieldPaths nearFieldParallax(double azimuthRad, double distanceM,
                                 double headRadiusM = kHeadRadiusKemarM,
                                 double speedOfSound = kSpeedOfSoundMps) noexcept;

// ═══════════════════════════════════════════════════════════════════════════
// PARTE II — LEY DE CURVATURA DE ORBE (nueva formulación)
// ═══════════════════════════════════════════════════════════════════════════

// Curvatura local del frente de onda en el centro de la cabeza: κ = 1/r.
// κ = 0 → onda plana (campo lejano); κ > 0 → onda esférica (campo
// cercano). Parámetro adimensional: ε = a·κ = a/r.
double wavefrontCurvature(double distanceM) noexcept;

// ITD por la LEY DE CURVATURA (una sola ecuación, dos ramas que se unen
// continuamente en θ* = asin(ε); ver el encabezado de este archivo).
//   κ → 0: recupera Woodworth EXACTO, (a/c)(θf + sinθf).
//   r = 0.15 m, θ = 90°: 732.5 µs (+11.7 % sobre Woodworth 655.8 µs).
//   Error vs el modelo exacto de rayos (NearFieldHeadModel): < 0.6 µs
//   en todo azimut para r ≥ 0.15 m (ver test_physical_propagation).
// Azimut en radianes, cualquier valor (se pliega por simetría);
// distancia <= a+0.01 o no finita → clamp a a+0.01 / 1.0.
double curvatureLawItdSeconds(double azimuthRad, double distanceM,
                              double headRadiusM = kHeadRadiusKemarM,
                              double speedOfSound = kSpeedOfSoundMps) noexcept;

// ILD por divergencia esférica de la MISMA geometría de curvatura:
// 20·log10(P_L/P_R) con P los caminos de rama (exactos, forma cerrada).
// 90°: 14.01 dB @ 0.15 m; 5.57 dB @ 0.35 m; 1.94 dB @ 1 m; 0.02 dB @ 100 m.
// NO incluye la sombra por difracción (término de alta frecuencia del
// ILD real): es la parte de divergencia, exacta en graves.
double curvatureLawIldDivergenceDb(double azimuthRad, double distanceM,
                                   double headRadiusM = kHeadRadiusKemarM) noexcept;

// INVERSIÓN — la predicción usable de la ley: el exceso de ITD sobre
// Woodworth mide la distancia SIN usar nivel (a diferencia del ILD,
// que necesita calibración de nivel y modelo de sombra). A 90°:
//   ε ≈ 2·(ITD·c/a − 1 − π/2)  →  r = a/ε.
// Verificado: r real 0.15 → estimado 0.145 m; 0.35 → 0.348; 0.5 → 0.499;
// 1.0 → 0.999; 2.0 → 2.000. Solo válido si |ITD| > Woodworth (exceso
// positivo) y |θf| cerca de 90° (la fórmula usa θf = 90°); fuera de
// eso devuelve −1.0 (sin estimación).
// itdExcessSeconds = ITD_medido − ITD_Woodworth(θ).
double distanceFromItdExcess(double itdExcessSeconds, double headRadiusM = kHeadRadiusKemarM,
                             double speedOfSound = kSpeedOfSoundMps) noexcept;

// Corolario de acercamiento (looming acústico): para una fuente que se
// acerca con velocidad radial v_r > 0, la curvatura crece como
//   κ̇ = κ² · v_r.
// Tiempo de contacto (tau óptico, aquí acústico): τ = r/v_r = 1/(κ·v_r).
// Ej.: r = 0.5 m, v_r = 1 m/s → κ = 2.0 /m, κ̇ = 4.0 /m/s, τ = 0.50 s.
// v_r <= 0 o no finito → κ̇ = 0, τ = +∞ (devuelto como 1e30).
double curvatureRate(double distanceM, double radialVelocityMps) noexcept;
double timeToContact(double distanceM, double radialVelocityMps) noexcept;

}  // namespace ivanna::fused::propagation
