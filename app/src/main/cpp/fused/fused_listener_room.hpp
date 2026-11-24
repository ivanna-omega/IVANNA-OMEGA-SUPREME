#pragma once
// fused_listener_room.hpp — OPERADOR #3, MODELO OYENTE+SALA ("SALA VIVA").
// FUSIÓN MAGISTRAL (directiva de Esse 2026-11-18): los ~45 efectos del
// producto se fusionan en 7 operadores únicos; este es el #3.
//
// Nombre de producto aprobado por Esse (2026-11-18 ~07:06 CST): SALA VIVA.
// name() devuelve "SALA VIVA".
//
// Fusiona: RIR (RirConvolver) + SAF (modelo espacial/PCA/HRTF) + Hearing
// Adaptation + BioControl 5D + corrección de sala cut-only (Fase 4).
// Controles: SOLO ListenerRoomControls (fused_controls.hpp).
//
// MAPEO DE MOTORES (orden de proceso):
//   1. HearingAdaptationEngine — ENVUELTO. process() limpio, header-only,
//      compila en host. Perfil: FLAT=bypass (no se llama a process),
//      HARMAN=curva Harman aproximada con los dos únicos mandos del motor
//      (sellado 0.47 -> +2.9 dB de graves; pérdida 4/8 kHz +2 dB -> +1.0 dB
//      de agudos), CUSTOM=audiograma moderado fijo (sellado 0.70,
//      pérdida 4/8 kHz +4/+6 dB) hasta que la superficie de control tenga
//      entrada de audiograma real. El motor NO tiene curva Harman nativa;
//      el mapeo es una aproximación documentada, no una Harman medida.
//   2. RirConvolver — ENVUELTO. process(L,R,frames) + setSampleRate +
//      applySofaCoupling. SIN dataset RIR cargado el stage es BYPASS
//      fail-closed (isLoaded()==false -> no se toca la señal, roomWet se
//      trata como 0). La mezcla dry/wet es de POTENCIA CONSTANTE
//      (gDry^2+gWet^2=1) según roomWet EFECTIVO (limitado por la física
//      de la sala, ver RoomGeometryConfig). OJO: el convolver NO tiene
//      salida wet pura: su mezcla interna es out = (1-0.68*wn)*in +
//      0.82*wn*wetScale*wet (RirConvolver.cpp, mezcla wet/dry). Con wn
//      asentado a 1 (warm-up de ceros en setRir/prepare, pre-stream) la
//      salida es 0.32*in + 0.82*wetScale*wet y se recupera el wet con la
//      combinación lineal exacta a*dry + b*out, con b = gWet/(0.82*ws) y
//      a = gDry - 0.32*b. Si la IR se carga en caliente a mitad de stream
//      (sin pasar por setRir), wn sube por su one-pole anti-zipper
//      (~50 ms) y la mezcla es exacta una vez asentado.
//      rt60Sec/roomSizeM parametrizan la sala vía RoomGeometryConfig
//      (frecuencia de Schroeder + limitador físico del wet); la IR llega
//      por setRir() (dataset RIR del integrador) o por
//      synthesizeMasterStudioBrir() (la misma BRIR Master Studio que
//      HolosOperator se auto-sintetiza; es la vía que usa IvannaFusionCore
//      desde la corrección del FALLO 5, porque ahí no hay dataset RIR).
//   2b. CAMPO DIFUSO SDRF (2026-11-24, fusión homogénea): la red
//      unitaria SDRF (guías Weil/GUE + Householder + Gray-Markel + Kerr
//      + softReflect ≤0.95, sdrf_diffuse_field.hpp) vive DENTRO de este
//      operador, SIN controles nuevos: effectiveWet_ es la puerta y la
//      densidad ρ_E; rt60Sec es el decaimiento del lazo SOLO sin IR
//      cargada (con IR el campo es difusión pura, g=1). Sin IR cargada
//      el campo ES la sala (antes el stage era bypass); con IR cargada
//      difunde la cola RIR con espaciamiento GUE. La mezcla de potencia
//      constante no cambia (el wet llega con la misma forma). Con
//      effectiveWet_ = 0 el campo no corre y su estado se limpia.
//   3. RoomCorrectionEq — ENVUELTO, instancia PROPIA (el Balance Tonal
//      tiene la suya para el EQ de usuario). setCutOnly(true) + setTarget
//      (roomCorrDb[10] directo, ya clampado a -12..0 en los controles).
//      Cut-only Fase 4: solo recorta picos, nunca realza.
//   4. SAF (SafModelLoader / SafPcaDecoder / SafSpatialModifier) — NO
//      envueltos como etapa de audio: NINGUNO tiene process() de audio.
//      SafModelLoader::load(path) es I/O de fichero (sin modelo en host);
//      SafPcaDecoder::init(model)/decode(q) mapea latente->vector HRIR;
//      SafSpatialModifier::update(q, hrtf, azimuth) genera un HRIRPair
//      (sintético), no procesa muestras. Su función vive AQUÍ vía el
//      conocimiento maestro horneado: RirConvolver::applySofaCoupling(
//      kMasterSafGoldenQ) — el latente dorado SOFA (214 sujetos) parametriza
//      early-clarity, decorrelación tardía, delta de RT60 y escala del wet
//      del convolver (SofaSafRirMasterKnowledge.hpp, computeSofaRirCoupling).
//   5. BioControl5D — ENVUELTO como lazo, NO como etapa de audio: no
//      tiene process() (por diseño solo mueve Γ_b, r_ORBE y K; ver su
//      header §3.4). Sin sensor (Fase 0) es identidad exacta: update()
//      se llama por bloque con etaStar=eta0 y gammaB/orbeRadius devuelven
//      sus valores base. El control biológico 5D lo cubren, en audio, el
//      perfil auditivo + la sala.
//   6. RoomGeometryConfig — USADO como configuración: layout de referencia
//      escalado proporcionalmente con roomSizeM como profundidad,
//      frecuencia de Schroeder (diagnóstico) y
//      limitSyntheticReverbWetForRoomT60 (en sala viva, T60>=1.2 s, el wet
//      sintético es 0; en sala seca, T60<=0.5 s, pasa íntegro).
//
// Reglas duras (contrato fused_controls.hpp):
//  - Neutro = bit-transparente: process() retorna sin tocar el buffer.
//  - Matched-LUFS: ganancia neta 1.0 exacta en neutro.
//  - Sin malloc/locks/I-O en process(): scratch pre-reservado en
//    prepare() (kFusedMaxBlockFrames=8192); proceso interno en trozos de
//    128 frames (== Ivanna::BLOCK_SIZE, IvannaFusionCore.hpp:20).
//  - Fail-closed: cualquier muestra no finita -> 0.0.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "fused/fused_controls.hpp"
#include "fused/sdrf_diffuse_field.hpp"
#include "spatial/BioControl5D.hpp"
#include "spatial/HearingAdaptationEngine.hpp"
#include "spatial/RirConvolver.hpp"
#include "spatial/RoomCorrectionEq.hpp"
#include "spatial/RoomGeometryConfig.hpp"
#include "spatial/SofaSafRirMasterKnowledge.hpp"

namespace ivanna::fused {

class FusedListenerRoom {
public:
    // Trozo interno de proceso (== Ivanna::BLOCK_SIZE, IvannaFusionCore.hpp:20).
    static constexpr int kChunkFrames = 128;

    FusedListenerRoom();
    void prepare(double sampleRate) noexcept;
    void setControls(const ListenerRoomControls& c) noexcept;
    ListenerRoomControls controls() const noexcept { return controls_; }
    void process(double* left, double* right, int frames) noexcept;
    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "SALA VIVA"; }

    // Punto de inyección de la IR de sala (dataset RIR cargado por el
    // integrador, o IR sintetizada con RirConvolver::synthesizeMasterStudioBrir).
    // Sin IR cargada el stage RIR es bypass (fail-closed). Hilo de control.
    void setRir(const float* irL, const float* irR, int irLen) noexcept;

    // FALLO 5 (Fusión Magistral, 2026-11-18): sintetiza en el RirConvolver
    // propio la MISMA BRIR Master Studio determinista que HolosOperator
    // se auto-sintetiza en prepare() (mismos seeds/taps/decaimiento
    // exponencial en RirConvolver::synthesizeMasterStudioBrir): mismo
    // (rt60, sr) -> IR bit-idéntica. Es la vía por la que IvannaFusionCore
    // alimenta la sala fusionada — IvannaFusionCore NO carga ninguna RIR
    // de dataset, así que no hay bytes que enrutar por setRir(); la única
    // RIR que suena es esta BRIR sintetizada. Hilo de control (la síntesis
    // aloca y hace FFT en load()). No-op si esa (rt60, sr) ya está cargada.
    // Devuelve rirLoaded(). Tras setRir() con IR de dataset, prepare()
    // NO re-sintetiza (la IR de dataset manda).
    bool synthesizeMasterStudioBrir(float rt60S) noexcept;

    // Diagnóstico (hilo de control / tests).
    bool rirLoaded() const noexcept { return rir_.isLoaded(); }
    // Wet efectivo tras el limitador físico de sala (<= roomWet pedido).
    float effectiveRoomWet() const noexcept { return effectiveWet_; }
    float schroederFrequencyHz() const noexcept { return schroederHz_; }
    double bioEta() const noexcept { return bio_.eta(); }

private:
    void applyControls() noexcept;
    // F2/486: processChunk trabaja en double de extremo a extremo
    // (RIR double, EQ double); dryL/dryR son el dry post-hearing.
    void processChunk(double* l, double* r, const double* dryL, const double* dryR,
                      int n) noexcept;
    // CAMPO DIFUSO SDRF (2026-11-24): la red unitaria SDRF difunde el wet
    // de la sala. Sin IR cargada el campo ES la sala (decaimiento RT60
    // propio); con IR cargada difunde la cola RIR sin decaimiento añadido
    // (g=1, exactamente el kernel verificado). Ambas dejan l/r con la
    // forma del convolver (0.32*in + 0.82*ws*wet) para que la mezcla de
    // potencia constante de processChunk no cambie. In-place sobre l/r;
    // usan el scratch diffuseIn_/diffuseOut_ (kChunkFrames).
    void diffuseRirWet(double* l, double* r, const double* dryL,
                       const double* dryR, int n, double ws) noexcept;
    void diffuseFromDry(double* l, double* r, const double* dryL,
                        const double* dryR, int n) noexcept;
    void warmupRir() noexcept;

    ListenerRoomControls controls_{};
    double sampleRate_ = 48000.0;
    bool prepared_ = false;
    bool processed_ = false;  // ya corrió process() con audio real
    bool rirWarmed_ = false;  // wetNow_ del convolver asentado a 1.0
    // FALLO 5: la IR cargada es la BRIR Master Studio sintetizada (no de
    // dataset) y el RT60 con el que se sintetizó. prepare() la re-hornea
    // si cambia la SR (pre-stream); setRir() pone rirSynthesized_=false.
    bool rirSynthesized_ = false;
    float synthRt60_ = -1.0f;
    float effectiveWet_ = 0.0f;
    float schroederHz_ = 0.0f;

    ivanna::spatial::HearingAdaptationEngine hearing_;
    Ivanna::RirConvolver rir_;
    ivanna::dsp::RoomCorrectionEq roomEq_;
    Ivanna::BioControl5D bio_;
    ivanna::spatial::RoomGeometryConfig geometry_;

    // Campo difuso SDRF (2026-11-24): vive dentro de SALA VIVA, sin
    // controles nuevos (ver sdrf_diffuse_field.hpp). diffuseActive_
    // marca si el campo tiene estado que limpiar al apagarse.
    SdrfDiffuseField diffuse_;
    bool diffuseActive_ = false;

    // Scratch pre-reservado en prepare() (sin malloc en process()):
    // dry post-hearing para la mezcla RIR de potencia constante.
    // F2/486: double; el scratchL_/scratchR_ float de la conversión
    // double<->float se elimina (el operador trabaja directo sobre los
    // buffers double del llamador).
    std::vector<double> dryL_;
    std::vector<double> dryR_;
    // Scratch del campo difuso (trozos de kChunkFrames): entrada y salida
    // separadas (el campo necesita el seco mientras escribe el wet).
    std::vector<double> diffuseInL_;
    std::vector<double> diffuseInR_;
    std::vector<double> diffuseOutL_;
    std::vector<double> diffuseOutR_;
};

}  // namespace ivanna::fused
