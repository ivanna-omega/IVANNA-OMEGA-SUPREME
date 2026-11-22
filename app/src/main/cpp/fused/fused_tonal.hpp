#pragma once
// fused_tonal.hpp — OPERADOR #5: BALANCE TONAL ÚNICO "ARMONÍA" (Fusión
// Magistral, 2026-11-18, directiva de Esse).
//
// Fusiona: EvolutionaryEQ (512 bandas) + Psychoacoustics (ISO 226) +
// Perceptual Loudness (LUFS/trim, Fix 5) + EliteTargetCurves (Harman 2018 +
// élite) + auto_eq_filter + EQ de usuario (10 bandas). Los cascarones y
// controles de los efectos fusionados quedan como LEGACY; la fuente de
// verdad es TonalControls (fused_controls.hpp).
//
// Orden de la cadena (fijo):
//   1. Psychoacoustics — SOLO cuando loudnessComp == true:
//      predictAndMitigateFatigue() + applyMaskingCompensation(). La fatiga
//      y el masking psicoacústico quedan cubiertos ADEMÁS por la
//      compensación de loudness ISO 226 del medidor (K-weighting BS.1770)
//      y por el EQ, así que este motor es un refuerzo, no la única vía.
//   2. EvolutionaryEQ — SOLO cuando curve != FLAT (0): FIR de 512 bandas
//      calibrado hacia Harman 2018 ×0.5 (su objetivo, fijado en
//      EvolutionaryEQ.cpp vía EliteTargetCurves; el módulo NO expone
//      setter de objetivo). Sin calibrar es identidad bit-exacta (gate de
//      seguridad del propio motor); prepare() calibra siempre.
//   3. EliteTargetCurves — fuente de las ganancias objetivo. curve 1 =
//      Harman 2018 (la aplica el EvoEQ ×0.5); curve 2 = élite-lean
//      (Harman −3 dB en graves <100 Hz): como el EvoEQ no tiene setter de
//      objetivo, el DELTA élite−Harman (×0.5, para sumar Harman×0.5 del
//      EvoEQ y dar élite-lean×0.5 en total) se pliega en la etapa 4.
//   4. RoomCorrectionEq (instancia PROPIA, distinta de la corrección de
//      sala del Modelo Oyente+Sala) — EQ de USUARIO de 10 bandas con el
//      filtro AutoEq (cascada de 10 biquads peaking RBJ) como
//      implementación. Mapeo: bandas 0-2 = lowDb, 3-6 = midDb, 7-9 =
//      highDb; modo completo (±12 dB, sin cut-only: el usuario sí puede
//      realzar). Sin ganancias es bypass bit-exacto.
//   5. Perceptual Loudness / trim LUFS — AL FINAL: el medidor K-weighted
//      (LoudnessMeter) mide cada trozo y el trim ESTÁTICO controls.trimDb
//      es la ÚNICA ganancia neta del operador (Matched-LUFS: neto 0.0 dB
//      cuando trimDb == 0).
//
// Decisión documentada sobre el trim AUTOMÁTICO (Fix 5):
//   LoudnessMeter::update_trim() normaliza hacia −14 LUFS con zona muerta
//   ±0.5 dB, pendiente máx 0.5 dB/s, freeze en silencio y tope ±6 dB. Ese
//   auto-trim NO se aplica aquí, a propósito: es una ganancia que depende
//   del programa y de la historia (tardaría 6 s en llegar a +3 dB y
//   perseguiría la dinámica dentro del tema), y el contrato de este
//   operador dice que trimDb es LA ÚNICA ganancia neta, determinista e
//   inmediata. El medidor SÍ corre (medición K-weighted real por trozo,
//   observable vía lufs()); si se quiere normalización automática, es un
//   control aparte, no trimDb.
//
// Reglas duras (contrato fused_controls.hpp):
//   - Neutro = bit-transparente: process() retorna sin tocar el buffer
//     cuando isNeutral() es true (curve 0 + trims 0; loudnessComp NO
//     forma parte de isNeutral() en TonalControls, así que tampoco aquí).
//   - Sin malloc / locks / I-O en process(). Scratch miembro
//     pre-reservado (kFusedMaxBlockFrames = 8192); bloques mayores se
//     procesan en trozos; dentro, en trozos de 128 frames
//     (= Ivanna::BLOCK_SIZE, lo que exigen Psychoacoustics y
//     EvolutionaryEQ::processNEON).
//   - Fail-closed: la entrada no finita se sanea a 0.0 ANTES de los
//     motores (para no envenenar su estado persistente) y cualquier
//     muestra no finita a la salida es 0.0; techo final ±1.5.

#include "fused/fused_controls.hpp"

#include "EvolutionaryEQ.hpp"
#include "IvannaFusionCore.hpp"
#include "Psychoacoustics.hpp"
#include "perceptual_loudness.hpp"
#include "spatial/EliteTargetCurves.hpp"
#include "spatial/RoomCorrectionEq.hpp"

#include <cmath>
#include <cstddef>

namespace ivanna::fused {

class FusedTonal {
public:
    // Trozo interno de proceso (= Ivanna::BLOCK_SIZE). Psychoacoustics y
    // EvolutionaryEQ operan sobre trozos planares double de 128 frames.
    static constexpr int kProcessChunkFrames = 128;
    // Techo final de seguridad a la salida (la cadena no debe entregar
    // más que esto aunque trim y EQ se sumen; el trim por sí solo llega
    // a 0.98 * 10^(6/20) ≈ 1.96 sin este techo — ver nota en .cpp).
    static constexpr double kOutputCeiling = 1.5;
    // El EvoEQ apunta a Harman ×0.5 (complementa Φ amount 0.5 en la
    // cadena completa); el delta élite-lean se aplica a la misma escala.
    static constexpr float kCurveTargetScale = 0.5f;

    FusedTonal() = default;
    FusedTonal(const FusedTonal&) = delete;
    FusedTonal& operator=(const FusedTonal&) = delete;

    void prepare(double sampleRate) noexcept;
    void setControls(const TonalControls& c) noexcept;
    TonalControls controls() const noexcept { return controls_; }
    void process(double* left, double* right, int frames) noexcept;
    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "ARMONÍA"; }

    // Observabilidad (no tocan el audio): LUFS K-weighted del último
    // trozo medido, si el EvoEQ quedó calibrado, índice de fatiga.
    float lufs() const noexcept { return static_cast<float>(meter_.lufs()); }
    bool evoReady() const noexcept { return evoEq_.isReady(); }
    float fatigueIndex() const noexcept { return psycho_.fatigueIndex(); }

private:
    // Reaplica trim + objetivos de 10 bandas a los motores (tras
    // prepare / setControls / reset; RoomCorrectionEq::prepare resetea
    // sus objetivos a 0, así que hay que reponerlos).
    void applyControls() noexcept;
    // Procesa work_ (UN trozo exacto de 128 frames reales) por la cadena
    // psico/evo → EQ → medidor → trim y vuelca el resultado al carry de
    // salida. Ver FALLO 1 en fused_tonal.cpp.
    void processBlock128() noexcept;

    TonalControls controls_{};
    double sampleRate_ = 48000.0;

    Ivanna::Psychoacoustics psycho_{};
    Ivanna::EvolutionaryEQ evoEq_{};
    ivanna::dsp::RoomCorrectionEq roomEq_{};
    ivanna::LoudnessMeterd meter_{};  // F2/486 — medidor K-weighted en double
    double trimGain_ = 1.0;

    // FALLO 1 (2026-11-18, HAL 192 frames — CRÍTICO): carry-over de 128
    // frames exactos para los motores de 128 fijos (Psychoacoustics /
    // EvolutionaryEQ, 128 fijos). El resto de un trozo no múltiplo de
    // 128 (p. ej. 64 de un bloque HAL de 192) JAMÁS se completa con
    // ceros: se acarrea como entrada real entre llamadas (carryIn_) y
    // la salida procesada se acarrea igual (carryOut_), así que el flujo
    // sale con un retardo constante ≤127 frames y un cebado inicial de
    // ceros SOLO a la salida, jamás al estado DSP. Con entrada ya
    // alineada a 128 el carry es cero y el retardo es 0.
    // Cotas: carryIn_ < 128 siempre; carryOut_ ≤ 127 pendientes + 128
    // del trozo recién procesado = 255 por trozo procesado dentro de
    // una llamada (capacidad = kFusedMaxBlockFrames + 128, que cubre
    // el peor trozo de 8192: 64 trozos + 127 pendientes).
    // F2/486 — MOTOR DOUBLE: carry y scratch en double 64-bit.
    double carryInL_[kProcessChunkFrames]{};
    double carryInR_[kProcessChunkFrames]{};
    int carryIn_ = 0;  // muestras reales sin procesar (< 128)
    double carryOutL_[kFusedMaxBlockFrames + kProcessChunkFrames]{};
    double carryOutR_[kFusedMaxBlockFrames + kProcessChunkFrames]{};
    int carryOut_ = 0;  // muestras procesadas esperando emisión

    // Buffer de trabajo de 128 frames para los motores (planar double).
    double workL_[kProcessChunkFrames]{};
    double workR_[kProcessChunkFrames]{};
    // Scratch miembro pre-reservado (sin malloc en process()).
    double scratchL_[kFusedMaxBlockFrames]{};
    double scratchR_[kFusedMaxBlockFrames]{};
};

}  // namespace ivanna::fused
