#pragma once

#include <cstddef>
#include <random>
#include "IvannaFusionCore.hpp"

namespace Ivanna {

/**
 * EvolutionaryEQ — ecualizador FIR evolutivo (genoma espectral de 512 bandas,
 * optimización LM-CMA-ES).
 *
 * HISTORIA (auditoría dc2ea33f, 2026-09-13): este módulo estaba "genuinamente
 * incompleto" en tres frentes: (1) el optimizador no tenía llamador en
 * producción, (2) calculateFitness() NO medía audio real —solo penalizaba la
 * varianza interna del genoma—, y (3) m_meanGenome nunca llegaba a
 * m_firCoeffsL/m_firCoeffsR, así que el FIR aplicado era siempre identidad.
 * Esa revisión completó los tres (fitness real vs curva objetivo,
 * rebuildFilterFromGenome, gate tras calibrate()).
 *
 * REPARACIÓN v2.5.369 (auditoría 2026-11-08, ver docs/EVOLUTIONARY_EQ.md):
 * la revisión anterior dejó cinco defectos reales, medidos en host:
 *   1. Calibración NO determinista: el RNG era thread_local con semilla fija
 *      que AVANZABA entre instancias → dos calibraciones daban FIR distintos
 *      (max|ΔFIR| = 0.016, fitness −4.76 vs −5.12) y Ruta A ≠ Ruta B.
 *      Ahora calibrate() resiembra el RNG y parte del genoma neutral:
 *      determinista e idempotente (mismo FIR siempre).
 *   2. Curva objetivo incoherente: 6 puntos paramétricos ("presencia") que
 *      CONTRADICEN el target élite del producto (Harman 2018) y no medían
 *      nada por debajo de 458 Hz (los graves quedaban en −2.6 dB por arrastre
 *      del regularizador, sin objetivo). Ahora el objetivo ES Harman 2018
 *      (13 anclajes, 20 Hz–16 kHz, vía EliteTargetCurves, única fuente de
 *      verdad) escalado ×0.5 para complementar el amount 0.5 de Φ: ambas
 *      etapas suman Harman completo en defaults.
 *   3. Convergencia pobre: fitness −20.25 → −4.20 en 48 generaciones (sin
 *      converger) y los objetivos de agudos NO se alcanzaban (13 kHz:
 *      error −1.46 dB; 20.6 kHz: −1.37 dB). Causa: paso fijo sin adaptación
 *      (el evolution path acumulaba la MEDIA —no el paso— y nunca se leía).
 *      Ahora hay adaptación de paso por éxito y 96 generaciones.
 *      (La adaptación CSA se probó y se descartó por MEDICIÓN: el paso
 *      medio es ~10× menor que una muestra N(0,1) por la recombinación 0.5
 *      y la mutación uniforme, así que ||p|| << χ y CSA colapsaba σ a 0.05
 *      en 8 generaciones y la respuesta quedaba casi plana. La adaptación
 *      por éxito no depende de esa distribución.)
 *   4. Calibración lenta (~967 ms en host): el fitness re-diseñaba el FIR
 *      con cos() por cada candidato (132k trascendentales/eval). Ahora el
 *      fitness usa una matriz precomputada (linealidad de la DTFT) y el FIR
 *      solo se materializa al final: ~10 ms.
 *   5. Normalización por pico (+3 dB) incompatible con el pico Harman
 *      (+8.6 dB): habría escalado todo −5.6 dB. Ahora se ancla en |H(1 kHz)|
 *      = 1.0 (referencia Harman) con topes de seguridad (maxMag ≤ 2.0,
 *      pico de tap ≤ 1.0).
 * La aplicación al audio sigue GATEADA tras calibrate(): sin calibración
 * verificada, processNEON() es identidad bit-exacta (misma doctrina que
 * SaFStimulusRenderer: nunca activar un filtro sin verificación real).
 *
 * S14 — AUTO-CALIBRACIÓN RE-OBJETIVADA, Fase 6 «510 — IVANNA a la corona»
 * (2026-11-28): calibrateToTarget(erbError[24]) construye el objetivo como
 * Harman 2018 ×0.5 − error regional ERB (ver calibrateToTarget): la misma
 * base Harman ×0.5 de calibrate() —con error cero el objetivo degenera en
 * el de calibrate()—, re-objetivada por la medición. Primera pasada
 * CUT-ONLY (como Fase 4): el error solo puede recortar el objetivo, nunca
 * elevarlo por encima de Harman; los refuerzos solo tras A/B.
 *
 * GATING (decisión de Esse, recomendación del documento: SERVIDOR PRIMERO,
 * local después, NUNCA ambas a la vez): este punto de entrada está APAGADO
 * por defecto —no tiene ningún llamador en producción—. Sin una llamada
 * explícita a calibrateToTarget(), el módulo se comporta exactamente como
 * antes (calibrate() hacia Harman ×0.5, o identidad bit-exacta sin
 * calibrar). La activación de la vía local frente a la vía servidor es
 * decisión explícita de Esse; este cambio no la toma ni la presupone.
 *
 * INTERACCIÓN CON LA CADENA (no cambiar el orden): el EvoEQ vive en
 * ARMONÍA ANTES del EQ de usuario de 10 bandas (fused_tonal.hpp: orden
 * fijo Psicoacústica → EvolutionaryEQ → RoomCorrectionEq → trim LUFS).
 * Su corrección y la del `.ivanna.json` (que alimenta el EQ de 10 bandas)
 * se SUMAN en dB. Por tanto el analizador que produce erbError[24] debe
 * contabilizar AMBAS correcciones (medir con las dos activas y repartir,
 * o medir con una sola activa) o desactivar una de las dos; aplicar el
 * error medido con el EQ de 10 bandas activo como si fuera solo del EvoEQ
 * es doble corrección. Este módulo no cambia el orden de la cadena ni
 * toca el árbitro ni el kernel D10.
 */
class EvolutionaryEQ {
public:
    EvolutionaryEQ();
    ~EvolutionaryEQ() = default;

    void processNEON(Ivanna::AudioBuffer* buffer);
    /** n frames planares, n arbitrario (Ruta A). Equivalente a processNEON cuando n == BLOCK_SIZE. */
    void processPlanar(float* left, float* right, size_t n);
    // F2/486 — MOTOR DOUBLE: misma convolución FIR en double 64-bit.
    void processPlanar(double* left, double* right, size_t n);
    void updateLM_CMA_ES();

    /**
     * Ejecuta una optimización ACOTADA (kCalibrationGenerations) y habilita
     * la aplicación del FIR si —y solo si— la auto-verificación pasa: FIR
     * finito y pico <= 1.0 (post-normalización). Punto de entrada explícito
     * para producción; NO se llama a ciegas desde el constructor.
     *
     * DETERMINISTA e idempotente: resiembra el RNG (1337) y parte del genoma
     * neutral en cada llamada, así que dos calibraciones producen el MISMO
     * FIR (Ruta A == Ruta B, reproducible en cualquier teléfono).
     */
    void calibrate(float sampleRateHz);

    /**
     * S14 — Auto-calibración RE-OBJETIVADA (Fase 6, 510). Punto de entrada
     * APAGADO por defecto: sin llamador en producción (la activación
     * local-vs-servidor es decisión de Esse; servidor primero, local
     * después, nunca ambas a la vez). Sin llamarla, el comportamiento es
     * idéntico al actual.
     *
     * `erbError[24]`: error regional medido en dB por banda ERB (centros
     * eliteErbCenters24(48 kHz), 60 Hz–16 kHz), definido como respuesta
     * medida MENOS Harman 2018 en esa banda: positivo = exceso sobre
     * Harman, negativo = hueco por debajo de Harman. Entradas no finitas
     * se tratan como 0 dB (sin corrección en esa banda).
     *
     * Objetivo construido (512 bandas, interpolación lineal en
     * log-frecuencia entre centros ERB; fuera de rango se clampa al
     * valor del borde):
     *   target[b] = max(Harman2018(f_b) × 0.5 − max(erbError_interp, 0),
     *                   −2.0 dB)
     * La base es la MISMA Harman ×0.5 de calibrate() (complementa Φ
     * amount 0.5; con error cero el objetivo degenera en el de
     * calibrate()); el piso −2.0 dB es el existente del módulo. CUT-ONLY
     * en la primera pasada: el término de error se recorta a ≥ 0, así
     * que target ≤ Harman ×0.5 en todas las bandas — un hueco medido
     * (error negativo) NO se refuerza; los refuerzos solo tras A/B.
     *
     * Ejecución: genoma inicial NEUTRO (como calibrate()) y luego las
     * mismas kCalibrationGenerations de updateLM_CMA_ES() con el fitness
     * re-objetivado (13 anclajes muestreados del objetivo), y el mismo
     * gate que calibrate(): rebuildFilterFromGenome() (ancla
     * |H(1 kHz)| = 1.0, maxMag ≤ 2.0) + auto-verificación (FIR finito,
     * pico de tap ≤ 1.5). El genoma NO se inicializa en el objetivo:
     * medido en host, iniciar ahí deja maxMag ≈ 2.75 y el tope escala
     * todo ×0.73 (−2.8 dB global, ancla rota), porque el fitness no ve
     * el escalado del rebuild; desde neutro converge a la cuenca de
     * calibrate() sin tope activo. El objetivo 512 bandas queda expuesto
     * en targetGenome() para observabilidad y tests.
     * «pico de tap ≤ 1.0, como ya hace rebuildFilterFromGenome», pero el
     * tope real del código es 1.5 (el FIR Harman natural tiene pico
     * ~1.05; caparlo a 1.0 hundiría el nivel ~0.45 dB y rompería el
     * ancla de 1 kHz — ver comentario en rebuildFilterFromGenome). No
     * se cambia el tope aquí: la vía S14 hereda el 1.5 existente y los
     * tests lo verifican como tal.
     *
     * El ancla de 1 kHz es de NIVEL: un corte centrado en 1 kHz se
     * realiza como hueco RELATIVO a 1 kHz (el FIR entero se escala para
     * que |H(1 kHz)| = 1.0), no como bajada absoluta.
     *
     * DETERMINISTA e idempotente como calibrate(): resiembra el RNG
     * (1337) y parte del objetivo construido, así que dos llamadas con
     * el mismo error dan el MISMO FIR. Llamar a calibrate() después
     * borra el objetivo personalizado y vuelve a Harman ×0.5. Si
     * `erbError` es nullptr, no hace nada (fail-closed, estado intacto).
     */
    void calibrateToTarget(const float erbError[24]);

    /** Objetivo 512 bandas construido por el último calibrateToTarget()
     *  (dB por banda, mismo orden que genome()); todo ceros hasta la
     *  primera llamada. */
    const float* targetGenome() const noexcept { return m_targetGenome; }
    /** True si hay un objetivo personalizado activo (puesto por
     *  calibrateToTarget() y aún no borrado por calibrate()). */
    bool hasCustomTarget() const noexcept { return m_hasCustomTarget; }

    /** Vacía el historial FIR (ceros). La Ruta A lo llama al reactivar el
     *  EQ tras estar apagado, para no convolver con historial obsoleto. */
    void resetHistory() noexcept;

    // ── Observabilidad (usada por la barrera de regresión real) ──────────────
    void         rebuildFilterFromGenome();
    bool         isReady()       const noexcept { return m_ready; }
    const float* genome()        const noexcept { return m_meanGenome; }
    const float* firLeft()       const noexcept { return m_firCoeffsL; }
    const double* firLeftD()     const noexcept { return m_firCoeffsLd; }  // F2/486
    float        normalization() const noexcept { return m_normalization; }
    /** Paso de mutación actual (adaptado por éxito; 0.5 al inicio). */
    float        stepSize()      const noexcept { return m_stepSize; }

    /** Fitness actual del genoma en el criterio REAL (respuesta en magnitud). */
    float currentFitness() const;

    static constexpr int kCalibrationGenerations = 150;

private:
    alignas(16) float m_firCoeffsL[FIR_TAPS];
    alignas(16) float m_firCoeffsR[FIR_TAPS];
    // F2/486 — MOTOR DOUBLE: coeficientes e historial en double 64-bit
    // (cast exacto desde el diseño float en rebuildFilterFromGenome).
    alignas(16) double m_firCoeffsLd[FIR_TAPS];
    alignas(16) double m_firCoeffsRd[FIR_TAPS];

    alignas(16) float m_histL[BLOCK_SIZE + FIR_TAPS];
    alignas(16) float m_histR[BLOCK_SIZE + FIR_TAPS];
    alignas(16) double m_histLd[BLOCK_SIZE + FIR_TAPS];
    alignas(16) double m_histRd[BLOCK_SIZE + FIR_TAPS];

    float m_meanGenome[BANDS_512];
    float m_stepSize{0.5f}; // mutacion suficiente para converger al perfil espectral objetivo
    // Mejor fitness del genoma medio en la generación anterior (para la
    // adaptación de paso por éxito). Se resetea en calibrate().
    float m_prevMeanFitness{-1e30f};

    float m_normalization{1.0f};
    bool  m_ready{false};

    // S14 — objetivo personalizado (calibrateToTarget). 13 anclajes de
    // fitness muestreados del objetivo 512 bandas + el objetivo mismo.
    // m_hasCustomTarget=false ⇒ calculateFitness() usa la tabla Harman
    // ×0.5 estática (comportamiento de calibrate(), sin cambios).
    static constexpr int kTargetAnchorCount = 13;
    float m_targetDb[kTargetAnchorCount]{};
    float m_targetGenome[BANDS_512]{};
    bool  m_hasCustomTarget{false};

    // Gate compartido por calibrate() y calibrateToTarget(): habilita la
    // aplicación solo si el FIR reconstruido es finito y de pico ≤ 1.5
    // (tope de rebuildFilterFromGenome). Un solo sitio, una sola regla.
    void armIfVerified();

    // RNG PROPIO por instancia, resembrado en calibrate(). Antes era un
    // thread_local compartido que avanzaba entre instancias y hacía la
    // calibración no determinista (defecto #1).
    std::mt19937 m_rng{1337};
    float fastRand();

    // Criterio de fitness REAL: desviación entre |H(ω)| del FIR derivado de
    // `genome` y la curva objetivo Harman 2018 ×0.5 (ver EvolutionaryEQ.cpp).
    float calculateFitness(const float* genome);

    // Respuesta en magnitud |H(ω)| en frecuencia normalizada wn ∈ (0, π).
    float magnitudeAt(const float* coeffs, float wn) const;
};

} // namespace Ivanna
