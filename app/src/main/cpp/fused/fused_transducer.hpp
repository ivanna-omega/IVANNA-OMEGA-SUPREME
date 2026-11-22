#pragma once
// fused_transducer.hpp — OPERADOR #4: CORRECTOR DE TRANSDUCTOR
// Fusión Magistral (2026-11-18, directiva de Esse).
//
// Fusiona: Volterra (H2) + EmfEstimator (Back-EMF) + TransducerOperator +
// TemporalSpatialDynamics + TransientDetector.
// Controles (solo estos, ver fused_controls.hpp::TransducerControls):
// correction 0..1, temporalAmount 0..1, emfEnabled.
//
// MAPEO DE MOTORES (honesto, verificado contra cada header):
//
//  1. TransientDetector (spatial/TransientDetector.hpp, ivanna::TransientDetector):
//     SENSOR, no procesa audio. El operador lo corre por trozo de 128
//     frames sobre el mid (L+R)/2. Un transiente detectado reduce a la
//     mitad la corrección Volterra de ese trozo (la corrección no lineal
//     sobre un ataque puede emborronarlo); en lo sostenido la corrección
//     va completa. TemporalSpatialDynamics lleva SU PROPIO detector
//     interno por diseño (lo reutiliza para su ley 4D); el de aquí
//     gobierna solo el stage Volterra, no se duplica su efecto.
//  2. Volterra H2 (neuromorphic/volterra_h2_symmetric.hpp, K=64, 2 ch):
//     la corrección no lineal del transductor. processInterleaved()
//     con buffers separados permite el dry/wet del operador:
//     out = dry + effCorrection * (wet - dry), donde effCorrection =
//     correction * emfGovernor * (transiente ? 0.5 : 1.0).
//     SIN KERNEL CARGADO (host: hasEffectiveKernel()==false) el motor
//     es bypass fail-closed: processInterleaved() hace memcpy
//     (conserva el historial para una calibración posterior) y NO
//     silencia ni inventa coeficientes. La cantidad correction solo
//     escala lo que el kernel haría; sin kernel no hay nada que escalar.
//     Complemento: DiagonalVolterra (neuromorphic/diagonal_volterra.hpp,
//     K=64, memoria larga) corre tras el H2 completo; sin coeficientes
//     medidos es identidad (h1[0]=1, h2d=0 -> isBypass(), no toca nada).
//     Los coeficientes SOLO vienen de medición (protocolo de
//     caracterización del Genezi); este operador no inventa coeficientes.
//     La carga del paquete .ivk1 medido ES parte de su contrato desde la
//     corrección del FALLO 5 (Fusión Magistral, 2026-11-18):
//     loadKernelPackage/replaceKernelPackage delegan en
//     VolterraH2Symmetric con la política de producción
//     (MOTO_G85_GENEZY / MOTO_G85_PZ1D, K=64, fs 48 kHz) y aplican el
//     .ivk1 dentro de volterra_. Hilo de CONTROL, nunca el de audio
//     (el motor puede llamar memalign/free y replaceKernelPackage puede
//     dormir hasta ~300 ms esperando quiescencia). Contrato del motor:
//     setVolterraEnabled(false) antes de loadKernelPackage; en éxito
//     restaurar setVolterraEnabled() según la preferencia del usuario
//     (cargar != habilitar). Sin kernel cargado (host:
//     hasEffectiveKernel()==false) el motor es bypass fail-closed.
//  3. EmfEstimator (spatial/EmfEstimator.hpp, ivanna::spatial):
//     SENSOR, no procesa audio directo: process(V, I, n, vOut, xOut)
//     estima velocidad y desplazamiento del cono por Thiele-Small.
//     NO HAY fuente real de V/I en este camino (el DAC USB-C por Aux
//     no tiene ADC de retorno — lo dice el propio header): el operador
//     lo alimenta con V = señal (mid, proxy de tensión, escala unidad)
//     e I = V/Re (proxy óhmico, carga resistiva). Con ese proxy el
//     Back-EMF estimado es -Le*dI/dt y el gobernador es suave:
//     emfGovernor = clamp(1/(1+rms(velocidad)), 0.5, 1.0) — típico
//     ~0.95 con un seno 1 kHz a 0.7, 1.0 en silencio/DC. El gobernador
//     solo ATENÚA la corrección con velocidad de cono alta, nunca la
//     crea ni silencia. Con emfEnabled=false el gobernador es 1.0
//     (la corrección usa su modelo interno, sin sensor).
//  4. TransducerOperator (spatial/TransducerOperator.hpp, ivanna::psi):
//     DISEÑO, no procesador de audio: designTransducerCompensation()
//     diseña ganancias por banda (dB) a partir de una respuesta MEDIDA
//     vs objetivo, con límite WNG. SIN MEDIDA no hay nada que
//     compensar: con medida plana (0 dB) y objetivo plano devuelve
//     ganancias 0 dB (no-op), y el propio header lo dice ("con medida
//     plana devuelve ganancia 0"). El operador calcula ese diseño en
//     prepare() (4 bandas, plano/plano, WNG +3 dB) y lo guarda como
//     estado (transducerDesignValid()/transducerDesignMeanGainDb());
//     la actuación en audio es el stage Volterra, gobernado por
//     correction y por el EMF. Cuando exista la medición real del
//     Genezi (Fase VII), el diseño dejará de ser plano y sus ganancias
//     se aplicarán con un banco por banda — eso exige la medida, no se
//     inventa aquí.
//  5. TemporalSpatialDynamics (spatial/TemporalSpatialDynamics.hpp):
//     dinámica espacial temporal 4D, AL FINAL. Mapeo:
//     setParams(enabled = temporalAmount > 0, amount = temporalAmount),
//     setAuto(false), setArc(0, 0). Apagada es bit-exacta; encendida
//     solo toca el lateral por encima de ~250 Hz y nunca el centro
//     (L+R se conserva). Es el único stage con efecto audible en host
//     sin kernel Volterra ni medida de transductor.
//
// Reglas de la cadena fusionada: neutro = retorno sin tocar el buffer
// (bit-transparente, ganancia exactamente 1.0); scratch pre-reservado
// en prepare() (máx kFusedMaxBlockFrames=8192, trozos mayores se
// procesan en bloques); proceso interno en trozos de 128 frames
// (== Ivanna::BLOCK_SIZE, IvannaFusionCore.hpp:20); sin malloc, locks
// ni I/O en process(); muestra no finita -> 0.0 (fail-closed).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "fused/fused_controls.hpp"
#include "neuromorphic/diagonal_volterra.hpp"
#include "neuromorphic/volterra_h2_symmetric.hpp"
#include "spatial/EmfEstimator.hpp"
#include "spatial/TemporalSpatialDynamics.hpp"
#include "spatial/TransducerOperator.hpp"
#include "spatial/TransientDetector.hpp"

namespace ivanna::fused {

class FusedTransducer {
public:
    // Trozo interno de proceso == Ivanna::BLOCK_SIZE (128 frames).
    static constexpr int kChunkFrames = 128;
    // Longitud de kernel Volterra: 64 taps == K del kernel PZ1D/Genezi.
    static constexpr std::uint32_t kVolterraKernelLength = 64;

    FusedTransducer();
    void prepare(double sampleRate) noexcept;
    void setControls(const TransducerControls& c) noexcept;
    TransducerControls controls() const noexcept { return controls_; }
    void process(double* left, double* right, int frames) noexcept;
    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "VOLTERRA·Ω"; }

    // Telemetría de solo lectura (último trozo procesado / diseño vigente).
    float lastEmfGovernor() const noexcept { return emfGovernor_; }
    bool lastTransientHit() const noexcept { return transientHit_; }
    bool transducerDesignValid() const noexcept { return designValid_; }
    float transducerDesignMeanGainDb() const noexcept { return designMeanGainDb_; }
    bool volterraHasEffectiveKernel() const noexcept {
        return volterra_.hasEffectiveKernel();
    }

    // Carga de kernel IVK1 medido (FALLO 5, Fusión Magistral 2026-11-18).
    // Delegan en VolterraH2Symmetric::loadKernelPackage/replaceKernelPackage
    // con la política de producción por defecto (MOTO_G85_GENEZY /
    // MOTO_G85_PZ1D, K=64, fs 48 kHz): el .ivk1 (moto_g85_pz1d.ivk1 /
    // genezy_mhc_pz1d.ivk1) se aplica dentro de volterra_. Hilo de CONTROL,
    // nunca el de audio. Devuelven el VolterraLoadStatus (0 = Ok).
    // setVolterraEnabled/isVolterraEnabled gobiernan el envelope del motor
    // (contrato: deshabilitar antes de loadKernelPackage; en éxito restaurar
    // según la preferencia del usuario; cargar != habilitar).
    ivanna::dsp::VolterraLoadStatus loadKernelPackage(
        const std::uint8_t* package, std::size_t packageSize,
        const ivanna::dsp::VolterraLoadPolicy& policy =
            ivanna::dsp::VolterraLoadPolicy{}) noexcept;
    ivanna::dsp::VolterraLoadStatus replaceKernelPackage(
        const std::uint8_t* package, std::size_t packageSize,
        const ivanna::dsp::VolterraLoadPolicy& policy =
            ivanna::dsp::VolterraLoadPolicy{}) noexcept;
    void setVolterraEnabled(bool enabled) noexcept { volterra_.setEnabled(enabled); }
    bool isVolterraEnabled() const noexcept { return volterra_.isEnabled(); }
    const char* volterraActiveKernelDeviceId() const noexcept {
        return volterra_.activeKernelDeviceId();
    }

private:
    void applyDesign() noexcept;
    // Procesa un sub-trozo [0, n) en double (n <= kChunkFrames).
    // F2/486: el operador es double de extremo a extremo; ya no hay
    // conversión double<->float por bloque.
    void processSubchunk(double* l, double* r, int n) noexcept;

    TransducerControls controls_{};
    double sampleRate_ = 48000.0;
    bool prepared_ = false;

    // Motores envueltos (orden de proceso: 1 sensor -> 2 Volterra ->
    // 3 sensor EMF (gobierna) -> 4 diseño (estado) -> 5 temporal).
    ivanna::TransientDetector transient_;
    ivanna::dsp::VolterraH2Symmetric volterra_{kVolterraKernelLength, 2};
    ivanna::dsp::DiagonalVolterra diagonal_;
    ivanna::spatial::EmfEstimator emf_;
    ivanna::psi::TransducerDesign design_{};
    bool designValid_ = false;
    float designMeanGainDb_ = 0.0f;
    ivanna::dsp::TemporalSpatialDynamics temporal_;

    // Gobernanza del último trozo (telemetría).
    float emfGovernor_ = 1.0f;
    bool transientHit_ = false;

    // Scratch pre-reservado en prepare() (miembros, nunca en process()).
    // Trozo de 128 frames: seco, húmedo, mid y buses del estimador EMF —
    // todo double (F2/486).
    double dryL_[kChunkFrames] = {};
    double dryR_[kChunkFrames] = {};
    double wetL_[kChunkFrames] = {};
    double wetR_[kChunkFrames] = {};
    double mid_[kChunkFrames] = {};
    double interIn_[2 * kChunkFrames] = {};
    double interOut_[2 * kChunkFrames] = {};
    double emfCurrent_[kChunkFrames] = {};
    double emfVel_[kChunkFrames] = {};
    double emfDisp_[kChunkFrames] = {};
};

}  // namespace ivanna::fused
