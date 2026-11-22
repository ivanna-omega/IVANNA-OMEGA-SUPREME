// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// fused_field.hpp — OPERADOR DE CAMPO UNIFICADO (#2 de la Fusión Magistral).
// Nombre de producto aprobado por Esse (2026-11-18): HOLOS·Ω.
// (name() devuelve "HOLOS·Ω".)
//
// Fusiona: HOLOS + Phi (Φ) + Gamma (Ley 1, campo Γ) + SpectralCore +
// NEU·Ω (compilador espectral holográfico) + OmniField (IVANNA).
// Controles: SOLO FieldControls (fused_controls.hpp). Cascarones y
// controles de los efectos fusionados quedan como LEGACY (nunca se
// borra código); la fuente de verdad es FusedControls.
//
// CADENA (en serie, mismo orden que la cadena viva ORBE → HOLOS → OMNI → Φ):
//   HOLOS (HolosOperator) → OmniField → Phi (HyperStateOperator)
// El XTC interno de Phi queda APAGADO: la cancelación de diafonía vive
// en el Operador Espacial (#1), no aquí.
//
// MAPEO DE CONTROLES (FieldControls → motores):
//   amount    → HolosParams.amount  Y OmniParams.amount (mismo amount
//               de campo para ambos: el campo es uno solo).
//   phiAmount → HyperStateOperator::setAmount.
//   phiPreset → HyperStateOperator::setRoutePreset (0 teléfono,
//               1 sala/Genezi, 2 auto; clampado a 0..2).
//   pumping   → HolosParams.dynamics = 1.0f − pumping.
//               pumping=0 → dynamics=1.0 (el compresor dinámico de HOLOS
//               como hoy, SIN compresión extra y SIN bombeo añadido);
//               pumping=1 → dynamics=0 (Γ dinámico apagado).
//               El bombeo es CERO por diseño: ningún motor añade una
//               modulación de ganancia periódica; la única dinámica es
//               la Γ_dinámica propia de HOLOS, gobernada por este control.
//   harmonics → HolosParams.harmonics, re-clampado a
//               kHolosHarmonicsMaxValidated = 0.002 (la guarda validada;
//               FieldControls::clamp() ya lo limita a 0..0.002).
//   gamma     → HolosParams.gamma (exponente de divergencia del campo Γ).
//   roomRt60  → HolosParams.roomRt60 (RT60 del término difuso).
//   warmth    → HolosParams.warmth (balance par/impar del término K).
//   coherence → SIN entrada en HolosParams: HolosOperator ESTIMA la
//               coherencia desde la señal (HolosStateEstimator;
//               léase con meanCoherence()). El control se conserva en
//               la superficie fusionada y no se fuerza dentro de
//               `diffuse` (ese es el control de la cola difusa, otra
//               cosa); `diffuse` queda en su default 0.5.
//   dynamics  → NO se mapea directo: el control de dinámica del campo
//               ES `pumping` (dynamics = 1 − pumping, arriba). El campo
//               `dynamics` de FieldControls queda reservado para el
//               gobierno de la cadena y no alimenta un segundo compresor.
//
// FASE B −102.79 dB (ancla linkBandGainsDb): SE PRESERVA. HolosOperator
// se envuelve SIN TOCARLO (ni su .hpp ni su .cpp): el operador, su
// HolosGainField y su ancla quedan bit-idénticos; solo se le pasan
// parámetros por su API pública (setParams).
//
// Γ COMPLEJO (Ley 1, GammaComplex.hpp): NO se envuelve por separado.
// El campo Γ vive DENTRO de HolosOperator (HolosOperator.cpp incluye
// GammaComplex.hpp y aplica gammaRe_/gammaIm_ por banda en renderBins).
// Envolverlo aparte duplicaría la fase de Bode sobre la misma magnitud.
//
// SPECTRALCORE + NEUCOMPILER (NEU·Ω) — NO ENVUELTOS COMO PRE-ETAPA,
// por diseño, no por falta de API:
//   - SpectralCore SÍ tiene un process() limpio estéreo in/out
//     (inL/inR → outL/outR) y compila en host; NeuCompiler NO tiene
//     process(): es un proveedor de matrices 2×2 por (banda, frecuencia)
//     para el SpectralCore (NeuCompiler::provider + setMatrixProvider).
//   - PERO su función es el PASO ESPECTRAL ÚNICO COMPARTIDO: con M=I
//     el SpectralCore es un STFT/OLA identidad (FFT 512 / hop 128, la
//     MISMA rejilla que HOLOS) con 512 muestras de latencia. Ponerlo en
//     serie ANTES de HOLOS duplicaría el STFT (doble latencia, doble
//     coste) para una matriz identidad: cero función nueva.
//   - Y con la matriz HOLOS compilada (NeuCompiler::holosMatrix) NO se
//     puede poner en serie con HolosOperator::process(): aplicaría HOLOS
//     DOS VECES (una por matriz, una por el operador serial). La vía
//     correcta es la integración a nivel de cadena (Fase 6): UN solo
//     SpectralCore cuyo pre-render llama a
//     HolosOperator::estimateSpectrum() y cuyo proveedor lee los estados
//     por banda ya estimados, REEMPLAZANDO el process() serial de HOLOS
//     (no sumándose a él). Esa integración vive en la cadena, no en este
//     operador; además el camino por matriz solo cubre la parte DIRECTA
//     (la cola difusa es Fase 3 y el K temporal post-OLA no está en la
//     matriz), así que reemplazar el operador serial pondría en riesgo
//     el ancla de Fase B (−102.79 dB).
//   - Por tanto: la compilación espectral la realiza el núcleo STFT de
//     HOLOS, misma función, una sola implementación. SpectralCore y
//     NeuCompiler quedan documentados aquí como la vía de integración
//     futura a nivel de cadena, no como etapa serial de este operador.
//
// NEUTRO = BIT-TRANSPARENTE: si isNeutral() (amount<=0 y phiAmount<=0),
// process() retorna sin tocar el buffer (ganancia exactamente 1.0,
// Matched-LUFS). Fuera de neutro, la única ganancia es la que dictan
// los controles. Sin malloc/locks/I-O en process(): el scratch se
// pre-reserva en prepare() (kFusedMaxBlockFrames = 8192) y se procesa
// en trozos de 128 frames (= Ivanna::BLOCK_SIZE, como la cadena viva).
// Fail-closed: muestra no finita a la salida → 0.0; sin prepare() no
// se toca el buffer.
#include <algorithm>
#include <cmath>
#include <cstddef>

#include "fused/fused_controls.hpp"
#include "spatial/HolosOperator.hpp"
#include "spatial/HyperStateOperator.hpp"
#include "spatial/OmniField.hpp"

namespace ivanna::fused {

class FusedField {
public:
    // Trozo interno de procesado (= Ivanna::BLOCK_SIZE, como la cadena viva).
    static constexpr int kChunkFrames = 128;

    FusedField() = default;
    FusedField(const FusedField&) = delete;
    FusedField& operator=(const FusedField&) = delete;

    // Prepara los tres motores y pre-reserva el scratch (puede allocar;
    // hilo de control, nunca el de audio). Idempotente.
    void prepare(double sampleRate) noexcept;
    // Clampa y guarda; empuja el mapeo a los tres motores.
    void setControls(const FieldControls& c) noexcept;
    FieldControls controls() const noexcept { return controls_; }
    // Puerta Genezi (Fallo 3a, PARCHE v2.6): el modo de salida vive en
    // FieldControls.mode (fused_controls.hpp) y lo pone FusedChain desde
    // spatial.mode. En SPEAKERS el render binaural de HOLOS (GreenKernel
    // ITD/ILD + cola difusa rir_ Master Studio) se apaga: HolosParams.amount
    // → 0 y diffuse → 0. Phi y OmniField siguen corriendo (Phi lleva el
    // preset de sala/Genezi; su XTC interno ya estaba apagado).
    // setOutputMode() se conserva por compatibilidad: escribe en
    // controls_.mode (la única fuente de verdad ahora).
    void setOutputMode(OmegaOutputMode mode) noexcept;
    OmegaOutputMode outputMode() const noexcept { return controls_.mode; }
    bool isSpeakers() const noexcept { return controls_.mode == OmegaOutputMode::SPEAKERS; }
    // Estéreo double in-place. Neutro → no toca el buffer.
    void process(double* left, double* right, int frames) noexcept;
    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "HOLOS·Ω"; }

    // Telemetría: coherencia media estimada por HOLOS (0..1). Medida,
    // no control: véase el mapeo de `coherence` arriba.
    float meanCoherence() const noexcept { return holos_.meanCoherence(); }
    float appliedHolosAmount() const noexcept { return holos_.appliedAmount(); }

private:
    void pushParamsToEngines() noexcept;

    FieldControls controls_{};
    // Modo de salida (Fallo 3a, PARCHE v2.6): ahora vive en
    // FieldControls.mode; outputMode_ se conserva solo como espejo para
    // no romper llamadas externas a outputMode(). La fuente de verdad es
    // controls_.mode (isSpeakers() lo lee de ahí).
    OmegaOutputMode outputMode_ = OmegaOutputMode::HEADPHONES;
    double sampleRate_ = 48000.0;
    bool prepared_ = false;

    // Núcleo: HOLOS (sin tocar) → OmniField → Phi.
    HolosOperator holos_;
    ivanna::omnifield::OmniField omni_;
    ivanna::spatial::HyperStateOperator phi_;

};

}  // namespace ivanna::fused
