#pragma once

#include <cstdint>
#include <cstddef>
#include <atomic>
#include <cstring>
#include "IvannaFusionCore.hpp"
#include "spatial/IntelligentUpmixer.hpp"
#include "spatial/HoaBinauralDecoder.hpp"
#include "spatial/HolosOperator.hpp"
#include "spatial/HyperStateOperator.hpp"
#include "spatial/SensorSnapshot.hpp"
#include "spatial/OmniField.hpp"
#include "spatial/OrbeBinaural.hpp"
#include "spatial/WfsRenderer.hpp"
#include "spatial/WfsSpreadGovernor.hpp"
#include "spatial/AuxFieldCoherence.hpp"
#include "spatial/TemporalSpatialDynamics.hpp"
#include "spatial/RoomCorrectionEq.hpp"
#include "neuromorphic/NpeStereoCore.hpp"
#include "neuromorphic/neuro_acoustic_closed_loop.hpp"
#include "spatial/CrosstalkCanceller.hpp"
#include "spatial/RoomGeometryConfig.hpp"
#include "supreme/SupremeTransitionEnvelope.hpp"
#include "fused/fused_chain.hpp"  // FUSIÓN MAGISTRAL (2026-11-18): núcleo compartido Ruta A/Ruta B

// ─────────────────────────────────────────────────────────────────────────────
// FIX (CI rojo — "namespace 'Ivanna' does not enclose namespace
// 'IvannaFusionEngine'"): la clase se declaraba en ámbito GLOBAL mientras
// IvannaFusionCore.cpp la define dentro de `namespace Ivanna {}`. Todas las
// clases colaboradoras (HrtfManager, EvolutionaryEQ, Psychoacoustics,
// IvannaAudioClassifier, IvannaVoiceProsodyEngine, IvannaSuperAgentMemory)
// también viven en namespace Ivanna — la clase se declara ahora en el mismo
// namespace, alineada con sus definiciones.
//
// Además se restaura la API de compatibilidad que omega_effect.cpp invoca
// (ctor con sampleRate, initSpatial, processStereo, loadCustomHrtf, setters
// del snapshot del Control Bus y getters de subsistemas), siguiendo el mismo
// contrato que documenta cpp_snapshot/IvannaFusionCore.hpp.
// ─────────────────────────────────────────────────────────────────────────────

namespace Ivanna {

// HrtfManager es un ALIAS (using HrtfManager = HrtfManagerT<float> en
// HrtfManager.hpp), no una clase: un forward `class HrtfManager;` choca
// con el alias (CI rojo, clang: "definition of type 'HrtfManager'
// conflicts with type alias"). Se anticipa la plantilla y el miembro
// m_hrtf usa el tipo explícito, idéntico al alias. SIN argumento por
// defecto aquí: la definición completa (con `= float`) ya llega por
// fused/fused_chain.hpp antes de esta línea, y repetir el default es
// error ("redefinition of default argument").
template <typename Scalar> class HrtfManagerT;
class EvolutionaryEQ;
class Psychoacoustics;
class IvannaAudioClassifier;
class IvannaVoiceProsodyEngine;
class IvannaSuperAgentMemory;

class IvannaFusionEngine {
public:

    IvannaFusionEngine();

    ~IvannaFusionEngine();

    // Constructor de compatibilidad: omega_effect.cpp crea la instancia con el
    // sampleRate capturado de AudioFlinger (ej. 48000 Hz).
    explicit IvannaFusionEngine(float sampleRate) : IvannaFusionEngine() {
        sampleRate_ = sampleRate;
        m_sampleRateF = sampleRate;
        m_aux.prepare(sampleRate);
        m_temporal.prepare(sampleRate);
        m_room.prepare(sampleRate);
        // Fase 4 (cut-only): la corrección de SALA solo recorta picos.
        m_room.setCutOnly(true);
        m_userEq.prepare(sampleRate);
        m_npe.prepare(sampleRate);
        m_xtc.prepare(sampleRate, 2048);
        m_upmixer.prepare(sampleRate);
        m_hoaDecoder.prepare(sampleRate, 8);
        m_orbe.prepare(sampleRate);
        m_holos.prepare(sampleRate);
        m_omni.prepare(sampleRate);
        m_hyper.prepare(sampleRate);
        m_neuroLoop_.prepare(sampleRate, static_cast<int>(Ivanna::BLOCK_SIZE));
        m_neuroBandEq_.prepare(sampleRate);
        // FUSIÓN MAGISTRAL: la cadena fusionada corre en Ruta B (efecto HAL).
        // Mismo núcleo que Ruta A (FusedChain); cada ruta con su instancia.
        fusedChain_.prepare(static_cast<double>(sampleRate));
        fusedChain_.setRouteTag("RutaB");
    }

    void process(Ivanna::AudioBuffer* buffer);

    // ── FUSIÓN MAGISTRAL (2026-11-18) — Ruta B ─────────────────────────────
    // La cadena fusionada (FusedChain, núcleo C++ compartido con Ruta A).
    // useFusedChain=true (default): processStereo() delega al FusedChain y
    // el camino legacy (FIFO + process()) queda como rollback. Los controles
    // se sincronizan desde los atomics del control plane en cada llamada.
    void setFusedControls(const ivanna::fused::FusedControls& c) noexcept {
        fusedChain_.setControls(c);
    }
    ivanna::fused::FusedControls fusedControls() const noexcept {
        return fusedChain_.controls();
    }
    void setUseFusedChain(bool use) noexcept { useFusedChain_ = use; }
    bool useFusedChain() const noexcept { return useFusedChain_; }
    // Modo de salida (puerta Genezi, Fix 1): lo pone omega_apply_snapshot
    // (omega_effect.cpp) al decodificar el modo del snapshot. El sync por
    // atomics (syncFusedControlsFromAtomics) parte de los controles actuales
    // y NO toca el modo, así que este valor persiste entre bloques.
    void setFusedOutputMode(ivanna::OmegaOutputMode mode) noexcept {
        auto c = fusedChain_.controls();
        c.spatial.mode = mode;
        fusedChain_.setControls(c);
    }
    ivanna::fused::FusedChain& fusedChain() noexcept { return fusedChain_; }
    const ivanna::fused::FusedChain& fusedChain() const noexcept { return fusedChain_; }
    // Telemetría para FotoHolo (ancho espacial + guardián + género).
    ivanna::fused::FusedTelemetry fusedTelemetry() const noexcept {
        return fusedChain_.telemetry();
    }

    // Bucle cerrado: 1× por bloque de audio, antes de HOLOS/ORBE.
    // Devuelve los ajustes (identidad si está apagado o inactivo) y
    // aplica la ganancia por banda con NeuroBandEq. Hilo de audio.
    ivanna::neuromorphic::NeuroAdjustments updateNeuroLoop(
        Ivanna::AudioBuffer* buffer) noexcept;

    void runAcousticProfiling();
    void setUpmixingEnabled(bool enable) { m_upmixer.setUpmixingEnabled(enable); }
    bool isUpmixingEnabled() const { return m_upmixer.isUpmixingEnabled(); }
    void setImmersivity(float value) { m_upmixer.setImmersivity(value); }
    float getImmersivity() const { return m_upmixer.getImmersivity(); }

    // Wave Field Synthesis (misión "cerrar WFS de extremo a extremo",
    // 2026-09-19). setWfsEnabled/setWfsSpread escriben en los MISMOS
    // atomics g_wfs_enabled/g_wfs_spread que process() ya lee cada bloque
    // (definidos en wfs_globals_effect.cpp) — sin crear un segundo estado
    // paralelo. setWfsSpeakerLayout configura la geometría 3D real del
    // WfsRenderer (llamado desde omega_apply_snapshot, hilo de control, NO
    // el hilo de audio — mismo criterio ya establecido para setObject()).
    /// Modo AUX/altavoces: escribe los mismos atomics que lee process() cada bloque.
    void setAuxField(bool enabled, float xoverHz, float coherence) noexcept;
    /// Ruta AUX Genezi / auto (v2.5.366): aplica el preset completo —
    /// geometría XTC del par de altavoces + campo AUX coherente. Hilo de
    /// control. Nota: un snapshot WFS posterior puede reescribir la
    /// geometría XTC (setWfsSpeakerLayout); el preset se vuelve a aplicar
    /// llamando de nuevo. HRTF wet queda en 0 en ruta AUX por diseño.
    void setAuxGeneziPreset(int preset) noexcept;
    void setTemporalField(bool enabled, float amount, bool autoMode = false) noexcept;
    void setRoomCorrection(const float* gainsDb10) noexcept;   // corrección acústica de sala (10 bandas de octava, dB)   // dinámica espacial temporal ("4D")
    void setWfsEnabled(bool enable) noexcept;
    bool isWfsEnabled() const noexcept;
    void setWfsSpread(float spread) noexcept;
    // ORBE — Binaural Esférico de Campo Cercano. Escriben los mismos
    // atomics g_orbe_* que process() lee cada bloque (definidos en
    // orbe_controls_bridge.cpp / orbe_globals_effect.cpp — mismo patrón
    // que g_wfs_*). ACTIVO por defecto (amount=1.0, directiva explícita
    // de Esse 2026-11-08; la puerta GREEN quedó anulada para este
    // release). amount=0 = bypass bit-exacto. Corre EN SERIE antes de
    // HOLOS (ORBE → HOLOS → IVANNA → Φ), no en su lugar.
    void setOrbeEnabled(bool enable) noexcept;
    void setOrbeParams(float amount, float distanceM, float spread, float proximityDb,
                       float diffuse, bool headTracking,
                       float headRadiusM = 0.0875f) noexcept;  // Ψ: radio KEMAR por defecto
    bool isOrbeEnabled() const noexcept;
    // HOLOS — Operador Único de Campo Acústico (FUSION.md). Corre EN
    // SERIE tras ORBE (ORBE → HOLOS → IVANNA → Φ): cuando HOLOS está
    // activo ORBE TAMBIÉN corre (el if/else que lo sustituía se eliminó
    // en v2.5.369; en serie el retardo algorítmico se suma: 512 muestras
    // de ORBE + 512 de HOLOS ≈ 21.3 ms @48 kHz). Mismo patrón de atomics
    // g_holos_* que ORBE (holos_controls_bridge.cpp). ACTIVO por defecto
    // (amount=0.5, harmonics=0.0 retuneado, directiva explícita de Esse
    // 2026-11-08; la puerta GREEN quedó anulada para este release).
    // amount=0 = bypass bit-exacto.
    void setHolosEnabled(bool enable) noexcept;
    void setHolosParams(float amount, float distanceM, float spread, float proximityDb,
                        float diffuse, float harmonics, float warmth, float dynamics,
                        float width, float gamma, float roomRt60, bool headTracking,
                        int bandCount) noexcept;
    void setHolosEq(float low, float mid, float high, float presence,
                    float tiltDb, float freq) noexcept;
    bool isHolosEnabled() const noexcept;
    // CONSERVACIÓN PERCEPTUAL — Árbitro de campo (2026-11-21). Escribe los
    // mismos atomics g_consv_* que syncFusedControlsFromAtomics() lee cada
    // bloque (definidos en conservation_controls_bridge.cpp /
    // conservation_globals_effect.cpp — mismo patrón que g_orbe_*/g_holos_*).
    // APAGADO por defecto (amount=0 = bypass bit-exacto). La Ruta B lo
    // recibe por snapshot ABI v17 (campos consv_*; omega_apply_snapshot
    // lo reenvía aquí). La Ruta A lo recibe por JNI (nativeSetFusedControls).
    void setConservationParams(float amount, float energyCap,
                               float phaseCoherence, float inertia) noexcept;
    bool isConservationEnabled() const noexcept;
    // IVANNA — Operador Unificado de Campo Espacial (v2.5.375). Escriben los
    // mismos atomics g_omni_* que process() lee cada bloque (definidos en
    // omni_controls_bridge.cpp / omni_globals_effect.cpp — mismo patrón que
    // g_orbe_*/g_holos_*). Apagado por defecto en Ruta B (amount=0) hasta
    // medir CPU en el G85; el shed térmico (setOmniThermalShed, LIMITED+)
    // lo apaga antes que ORBE/HOLOS por tiers.
    void setOmniEnabled(bool enable) noexcept;
    void setOmniParams(float amount, float scenePull, float distanceM,
                       bool headTracking) noexcept;
    bool isOmniEnabled() const noexcept;
    void setOmniThermalShed(bool shed) noexcept { m_omniShed_ = shed; }
    // Hiper-Operador de Estado Φ (v2.5.369). Entra tras la corrección de
    // sala (m_room) y antes del XTC (m_xtc), es decir, antes del
    // compresor/limitador y después de HOLOS/ORBE/AUX/NPE: una sola
    // ganancia unificada al final de la cadena. ACTIVO por defecto
    // (amount=0.5, target Harman 2018, directiva explícita de Esse
    // 2026-11-08; la puerta GREEN quedó anulada para este release).
    // amount=0 = bypass bit-exacto. Los parámetros viven en los atomics
    // g_hyper_* (hyper_controls_bridge.cpp / hyper_globals_effect.cpp).
    // En Ruta B los frames de sensor llegan por el canal lateral
    // (SensorSnapshot, spatial/SensorSnapshot.hpp): el efecto publica un
    // snapshot versionado y un consumidor dedicado lo ingiere con
    // ingestHyperSensorSnapshot, que valida versión/fuente/frescura/
    // rangos ANTES de tocar el estado. ingestHyperSensorFrame/
    // ingestHyperNeuroFrame son la API lista para ese transporte (mismo
    // patrón que ingestNeuroFrame). Ruta B es inicialmente SOLO
    // observacional: aceptar un frame no aplica su salida al audio.
    // ESTADO 2026-11-26 (consejo, defecto #10): Ruta B es "API LISTA",
    // sin productor en el proceso del efecto (audioserver) — ningún
    // llamador alimenta hoy este camino; la Ruta A viva es la JNI del
    // consumidor Kotlin (que solo PUBLICA en el canal, defecto #1: el
    // único escritor de stageFrame_ es el hilo de audio). Cuando se
    // cablee un productor en el efecto, DEBE ser el único escritor del
    // staging o el staging debe hacerse thread-safe; mientras tanto,
    // esta API se conserva y se testea, no se elimina.
    void setHyperEnabled(bool enable) noexcept;
    void setHyperAmount(float amount, int preset, bool xtcEnabled) noexcept;
    bool isHyperEnabled() const noexcept;
    bool ingestHyperSensorFrame(const ivanna::spatial::SensorFrame& frame) noexcept;
    // Canal lateral Ruta B: valida el snapshot y, solo si pasa, ingiere
    // su frame. Rechazo = false sin tocar el estado.
    bool ingestHyperSensorSnapshot(const ivanna::spatial::SensorSnapshot& snapshot) noexcept;
    bool ingestHyperNeuroFrame(int kind, std::uint64_t timestampUs,
                               const float* response8, const float* coherence8,
                               float quality, float abrWaveVLatencyMs,
                               float fnirsHbO) noexcept;
    // x/y/z: coordenadas de SALA absolutas (metros; x=ancho,y=altura,
    // z=profundidad), 7 altavoces, orden FL,FR,SL,SR,TL,TR,SW — mismo
    // formato que OmegaDspSnapshot::wfs_speaker_{x,y,z}. Se convierten
    // internamente a coordenadas relativas al oyente (listener fijo en
    // RoomGeometryConfig::defaultLayout()) antes de pasarlas a
    // WfsRenderer::setSpeakerLayout3D().
    void setWfsSpeakerLayout(const float x[7], const float y[7], const float z[7]) noexcept;


    void setGoldenEarMode(bool enable);

    // Reservar el slot no lineal mientras GoldenEar está solicitado o su envolvente
    // conserva cola audible. Se consulta desde el callback antes de processStereo().
    bool goldenEarNeedsNonlinearSlot() const noexcept {
        return m_goldenEarActive || m_harmSmoothed_ > 1.0e-5f;
    }

    void applyGoldenEarGAN(Ivanna::AudioBuffer* buffer);

    void setSafLatentParams(const float q[7]) noexcept;
    // Route B: wet/dry del HRTF con rampa interna de HrtfManager (0 = bypass seco).
    void setHrtfEnabled(bool enabled) noexcept {
        m_hrtfEnabled_.store(enabled, std::memory_order_release);
    }
    bool isHrtfEnabled() const noexcept {
        return m_hrtfEnabled_.load(std::memory_order_acquire);
    }
    // Evidencia medida del HRTF (contadores/energias del camino de audio real).
    struct HrtfEvidenceOut {
        uint64_t cpuBlocks=0, dspBlocks=0, safRefreshes=0, poseApplies=0;
        double inEnergy=0, outEnergy=0;
        float azimuthDeg=0, safBiasDeg=0, curvature=0;
        bool datasetLoaded=false;
    };
    bool hrtfEvidence(HrtfEvidenceOut& out) const noexcept;
    // Evidencia medida de IVANNA (contadores del operador m_omni en el
    // camino de audio real de Ruta B). Se publica por omega_process() en el
    // canal EffectEvidence cada 32 callbacks (mismo patrón que hrtfEvidence).
    struct OmniEvidenceOut {
        uint64_t processCalls=0, blocksRendered=0, framesRendered=0;
        float appliedAmount=0, convergenceR=0, sceneAzimuthDeg=0, diffuseness=0;
        bool fault=false;
    };
    bool omniEvidence(OmniEvidenceOut& out) const noexcept;

    // Pose de la escena (Ruta B): azimut/elevación en rad y distancia relativa (0.5..2). Devuelve true si el motor HRTF la aplicó.
    bool setSceneDistance(float distanceScale) noexcept;

    // ── Supremacía Acústica: TinyML Kernel, Volterra H2 & Hexagon cDSP ────────
    // Compatibilidad JNI/API histórica; el consumidor omega_effect gobierna el motor real ctx->volterraEngine.
    void setVolterraEnabled(bool enable) noexcept { m_volterraEnabled = enable; }
    bool isVolterraEnabled() const noexcept { return m_volterraEnabled; }
    void setFastRpcEnabled(bool enable) noexcept { m_fastRpcEnabled = enable; }
    bool isFastRpcEnabled() const noexcept { return m_fastRpcEnabled; }
    void setAtiEnabled(bool enable) noexcept { m_atiEnabled = enable; }
    bool isAtiEnabled() const noexcept { return m_atiEnabled; }

    // ── EQ evolutivo (Ruta B): interruptor de aplicación ─────────────────────
    // Paridad con Ruta A (JNI g_evoEqAEnabled). Antes Ruta B aplicaba el EvoEq
    // SIEMPRE una vez calibrado, sin interruptor: el toggle de la UI mentía
    // en Ruta B (decía OFF pero el efecto seguía aplicado). Ahora el engine
    // respeta este flag (default true = ON, como Ruta A). El cableado
    // Kotlin/JNI/SET_PARAM que lo gobierna desde el toggle es un pendiente
    // documentado (ver docs/EVOLUTIONARY_EQ.md § Integración); el hook C++
    // queda listo aquí. No afecta a m_ready: si no está calibrado sigue
    // siendo identidad aunque esté habilitado.
    void setEvoEqEnabled(bool enable) noexcept { m_evoEqEnabled.store(enable, std::memory_order_relaxed); }
    bool isEvoEqEnabled() const noexcept { return m_evoEqEnabled.load(std::memory_order_relaxed); }

    // ── API de compatibilidad con omega_effect.cpp / OmegaControlBus ─────────
    // Estos métodos reciben los parámetros del Control Plane (snapshot SHM) y
    // los enrutan a los subsistemas internos del engine.

    // Inicialización espacial tras SET_CONFIG de AudioFlinger.
    void initSpatial(float sr, int /*blockSize*/) noexcept {
        sampleRate_ = sr;
        m_sampleRateF = sr;
        m_upmixer.prepare(sr);
        m_hoaDecoder.prepare(sr, 8);
        m_orbe.prepare(sr);
        m_holos.prepare(sr);
        m_omni.prepare(sr);
        m_hyper.prepare(sr);
        m_neuroLoop_.prepare(sr, static_cast<int>(Ivanna::BLOCK_SIZE));
        m_neuroBandEq_.prepare(sr);
        m_wfs.init(sr, Ivanna::BLOCK_SIZE, 16);
        m_wfsInit = true;
        fusedChain_.prepare(static_cast<double>(sr));  // FUSIÓN MAGISTRAL (Ruta B)
        resetFifo();
        runAcousticProfiling();
    }

    void resetFifo() noexcept {
        m_inFifoCount = 0;
        m_inFifoReadPos = 0;
        m_inFifoWritePos = 0;
        m_outFifoCount = 0;
        m_outFifoReadPos = 0;
        m_outFifoWritePos = 0;
        if (!m_inFifoL.empty()) std::fill(m_inFifoL.begin(), m_inFifoL.end(), 0.0f);
        if (!m_inFifoR.empty()) std::fill(m_inFifoR.begin(), m_inFifoR.end(), 0.0f);
        if (!m_outFifoL.empty()) std::fill(m_outFifoL.begin(), m_outFifoL.end(), 0.0f);
        if (!m_outFifoR.empty()) std::fill(m_outFifoR.begin(), m_outFifoR.end(), 0.0f);
    }

    // Procesa N frames estéreo desinterleaved L/R en chunks de BLOCK_SIZE.
    // Llamado desde omega_process() en la ruta caliente de AudioFlinger.
    // Lock-free, zero-allocation carry-over ring buffer que garantiza que todo
    // bloque enviado a process(&buf) tenga EXACTAMENTE 128 muestras REALES contiguas,
    // con latencia CERO cuando frames es múltiplo de 128 y conservación exacta
    // (m_inFifoCount + m_outFifoCount == 128) para tamaños arbitrarios (96, 192, 240).
    void processStereo(float* left, float* right, size_t frames) noexcept {
        if (!left || !right || frames == 0) return;

        // FUSIÓN MAGISTRAL (Ruta B): la cadena fusionada procesa en double
        // (puente float exacto). El FIFO legacy y process() quedan solo como
        // rollback (useFusedChain=false).
        //
        // FALLO 1 (2026-11-18, HAL 192 frames — CRÍTICO): antes esta rama
        // llamaba fusedChain_.processFloat(left, right, frames) DIRECTO.
        // Con 192 frames (128+64) el resto de 64 llegaba a los motores
        // AudioBuffer de 128 (Psychoacoustics / EvolutionaryEQ en ARMONÍA,
        // HRTF en ORBE·Ω) y se rellenaba con 64 CEROS dentro del DSP,
        // metiendo ceros reales en historiales FIR y envolventes IIR →
        // ruido/modulación periódica a 250 Hz. Ahora el bloque pasa por
        // el MISMO Carry-over FIFO de 128 que el camino legacy: a
        // FusedChain SOLO llegan trozos exactos de 128 frames REALES y el
        // resto se lleva entre llamadas. Los pasos 0/1/3 son idénticos al
        // camino legacy a propósito (misma latencia y mismo cebado en
        // ambas rutas, para que alternar entre ellas no desalinee el
        // flujo): los ceros del cebado inicial (paso 0) solo rellenan la
        // SALIDA del primer bloque, jamás el estado DSP.
        if (useFusedChain_) {
            syncFusedControlsFromAtomics();

            // PARCHE v2.6 (punto 9) — FAST-PATH BIT-TRANSPARENTE: si neutro
            // y sin colas, no se toca nada: latencia cero añadida y consumo
            // mínimo (no se llama a fusedChain_.processFloat ni se copia
            // el FIFO). resetFifo() vacía el FIFO (el audio entrante pasa
            // directo, bit-exacto). Trade-off conocido: al pasar de
            // engaged→neutro con FIFO no vacío se descartan hasta 128
            // frames (2.67 ms, un clic único en la transición); a cambio
            // el neutro estable es latencia cero y CPU cero.
            if (fusedChain_.isNeutral() && !fusedChain_.spatial().hasActiveTail()) {
                resetFifo();
                return;
            }

            // PARCHE v2.6 (punto 9) — Si frames es múltiplo de 128 y el
            // FIFO está vacío: directo a processFloat (sin pasar por FIFO)
            // — el 99% de los frames de AudioFlinger (256, 512). El HAL
            // 192 (128+64) NO entra aquí: va al carry-over FIFO de abajo.
            if ((frames % Ivanna::BLOCK_SIZE) == 0 && m_inFifoCount == 0 &&
                m_outFifoCount == 0) {
                fusedChain_.processFloat(left, right,
                                         static_cast<int>(frames));
                return;
            }

            // 0. Cebado inicial (idéntico al legacy): garantiza el
            //    invariante m_inFifoCount + m_outFifoCount >= BLOCK_SIZE
            //    solo cuando frames no es múltiplo exacto de 128.
            if ((m_inFifoCount + m_outFifoCount < Ivanna::BLOCK_SIZE) &&
                ((frames % Ivanna::BLOCK_SIZE) != 0 || (m_inFifoCount + m_outFifoCount) > 0)) {
                const size_t need = Ivanna::BLOCK_SIZE - (m_inFifoCount + m_outFifoCount);
                for (size_t i = 0; i < need; ++i) {
                    m_outFifoL[m_outFifoWritePos] = 0.0f;
                    m_outFifoR[m_outFifoWritePos] = 0.0f;
                    m_outFifoWritePos = (m_outFifoWritePos + 1) % kFifoCapacity;
                }
                m_outFifoCount += need;
            }

            // 1. Ingreso de muestras crudas entrantes al FIFO de entrada.
            for (size_t i = 0; i < frames; ++i) {
                m_inFifoL[m_inFifoWritePos] = left[i];
                m_inFifoR[m_inFifoWritePos] = right[i];
                m_inFifoWritePos = (m_inFifoWritePos + 1) % kFifoCapacity;
            }
            m_inFifoCount += frames;

            // 2. Procesamiento exclusivo en múltiplos exactos de
            //    BLOCK_SIZE (128 frames REALES contiguos, jamás un resto
            //    parcial ni un solo cero de relleno en el estado DSP).
            while (m_inFifoCount >= Ivanna::BLOCK_SIZE) {
                float blkL[Ivanna::BLOCK_SIZE];
                float blkR[Ivanna::BLOCK_SIZE];
                for (size_t i = 0; i < Ivanna::BLOCK_SIZE; ++i) {
                    blkL[i] = m_inFifoL[m_inFifoReadPos];
                    blkR[i] = m_inFifoR[m_inFifoReadPos];
                    m_inFifoReadPos = (m_inFifoReadPos + 1) % kFifoCapacity;
                }
                m_inFifoCount -= Ivanna::BLOCK_SIZE;

                // Cadena fusionada (7 operadores, núcleo compartido Ruta A/B).
                fusedChain_.processFloat(blkL, blkR,
                                         static_cast<int>(Ivanna::BLOCK_SIZE));

                // Guardar salida procesada en FIFO de salida.
                for (size_t i = 0; i < Ivanna::BLOCK_SIZE; ++i) {
                    m_outFifoL[m_outFifoWritePos] = blkL[i];
                    m_outFifoR[m_outFifoWritePos] = blkR[i];
                    m_outFifoWritePos = (m_outFifoWritePos + 1) % kFifoCapacity;
                }
                m_outFifoCount += Ivanna::BLOCK_SIZE;
            }

            // 3. Extracción de exactamente N frames solicitados por el
            //    hardware/AudioFlinger (idéntico al legacy; el relleno a
            //    cero del shortfall solo ocurre en el cebado inicial).
            size_t available = (m_outFifoCount < frames) ? m_outFifoCount : frames;
            for (size_t i = 0; i < available; ++i) {
                left[i]  = m_outFifoL[m_outFifoReadPos];
                right[i] = m_outFifoR[m_outFifoReadPos];
                m_outFifoReadPos = (m_outFifoReadPos + 1) % kFifoCapacity;
            }
            for (size_t i = available; i < frames; ++i) {
                left[i]  = 0.0f;
                right[i] = 0.0f;
            }
            m_outFifoCount -= available;
            return;
        }

        // 0. Garantizar invariante m_inFifoCount + m_outFifoCount >= BLOCK_SIZE
        //    únicamente cuando frames no es múltiplo exacto de BLOCK_SIZE (ej. 96, 192, 240).
        if ((m_inFifoCount + m_outFifoCount < Ivanna::BLOCK_SIZE) &&
            ((frames % Ivanna::BLOCK_SIZE) != 0 || (m_inFifoCount + m_outFifoCount) > 0)) {
            const size_t need = Ivanna::BLOCK_SIZE - (m_inFifoCount + m_outFifoCount);
            for (size_t i = 0; i < need; ++i) {
                m_outFifoL[m_outFifoWritePos] = 0.0f;
                m_outFifoR[m_outFifoWritePos] = 0.0f;
                m_outFifoWritePos = (m_outFifoWritePos + 1) % kFifoCapacity;
            }
            m_outFifoCount += need;
        }

        // 1. Ingreso de muestras crudas entrantes al FIFO de entrada
        for (size_t i = 0; i < frames; ++i) {
            m_inFifoL[m_inFifoWritePos] = left[i];
            m_inFifoR[m_inFifoWritePos] = right[i];
            m_inFifoWritePos = (m_inFifoWritePos + 1) % kFifoCapacity;
        }
        m_inFifoCount += frames;

        // 2. Procesamiento exclusivo en múltiplos exactos de BLOCK_SIZE (128)
        while (m_inFifoCount >= Ivanna::BLOCK_SIZE) {
            Ivanna::AudioBuffer buf{};
            for (size_t i = 0; i < Ivanna::BLOCK_SIZE; ++i) {
                buf.left[i]  = m_inFifoL[m_inFifoReadPos];
                buf.right[i] = m_inFifoR[m_inFifoReadPos];
                m_inFifoReadPos = (m_inFifoReadPos + 1) % kFifoCapacity;
            }
            m_inFifoCount -= Ivanna::BLOCK_SIZE;

            // Pipeline DSP completo (EvoEQ, HRTF, WFS, Upmixer, TinyML AI)
            process(&buf);

            // Guardar salida procesada en FIFO de salida
            for (size_t i = 0; i < Ivanna::BLOCK_SIZE; ++i) {
                m_outFifoL[m_outFifoWritePos] = buf.left[i];
                m_outFifoR[m_outFifoWritePos] = buf.right[i];
                m_outFifoWritePos = (m_outFifoWritePos + 1) % kFifoCapacity;
            }
            m_outFifoCount += Ivanna::BLOCK_SIZE;
        }

        // 3. Extracción de exactamente N frames solicitados por el hardware/AudioFlinger
        size_t available = (m_outFifoCount < frames) ? m_outFifoCount : frames;
        for (size_t i = 0; i < available; ++i) {
            left[i]  = m_outFifoL[m_outFifoReadPos];
            right[i] = m_outFifoR[m_outFifoReadPos];
            m_outFifoReadPos = (m_outFifoReadPos + 1) % kFifoCapacity;
        }
        for (size_t i = available; i < frames; ++i) {
            left[i]  = 0.0f;
            right[i] = 0.0f;
        }
        m_outFifoCount -= available;
    }


    // Carga un dataset HRTF medido (formato IHR1) desde disco.
    // Devuelve false si el archivo no existe o la cabecera es inválida.
    bool loadCustomHrtf(const char* path) noexcept;

    // Actualiza la pose de cabeza para el render binaural.
    void updateHeadPose(float yaw, float pitch, float roll) noexcept;

    // Parámetros del snapshot OmegaDspSnapshot → subsistemas internos.
    // Stubs deliberados: permiten compilar el puente OmegaControlBus mientras
    // se cablea hacia StereoWidener / HarmonicExciter / compresor.
    void setSpatialWidth(float width) noexcept {
        if (std::isfinite(width)) {
            m_spatialWidthTarget_ = std::clamp(width, 0.0f, 3.0f);
        }
    }
    float getSpatialWidth() const noexcept { return m_spatialWidthTarget_; }
    // FIX (tronidos tipo metralleta al subir el slider al máximo, reporte
    // del propietario con captura, 2026-09-17): antes era un stub vacío —
    // el slider movía el snapshot SHM pero la ganancia armónica nunca
    // llegaba al DSP. Ahora se almacena como TARGET y process() la
    // integra con slew por muestra (ver m_harmSmoothed_): un salto duro
    // de ganancia de excitador es un escalón de amplitud audible = clic
    // por cada actualización del bus ("metralleta" = ráfaga de esos
    // clics al arrastrar rápido). El slew lo convierte en rampa suave.
    void setHarmonicGain(float gain) noexcept {
        if (std::isfinite(gain) && gain >= 0.0f && gain <= 4.0f) {
            m_harmGainTarget_ = gain;
            m_goldenEarActive = (gain > 0.001f);
        }
    }
    // v2.5.15: permiso de coordinación [0,1] sobre la ganancia armónica del usuario. Lo fijan
    // exc_red del motor adaptativo y el presupuesto armónico medido del Unity Engine; process()
    // lo integra con el mismo slew por muestra (sin escalones). 1.0 = sin recorte.
    void setHarmonicCoordination(float scale) noexcept {
        if (std::isfinite(scale)) m_harmCoordScale_ = std::clamp(scale, 0.0f, 1.0f);
    }
    float getHarmonicCoordination() const noexcept { return m_harmCoordScale_; }
    // v2.5.15: escalas de coordinación espacial [0.5,1] del arbitraje unificado del ADE (wfs_spread_scale,
    // upmix_immersivity: se calculaban y publicaban, nadie las aplicaba). Multiplican, en el punto de uso,
    // al valor del usuario (g_wfs_spread / g_upmixing_immersivity); 1.0 = sin recorte.
    void setSpatialCoordination(float wfsSpreadScale, float immersivityScale) noexcept {
        if (std::isfinite(wfsSpreadScale))   m_wfsCoordScale_ = std::clamp(wfsSpreadScale, 0.5f, 1.0f);
        if (std::isfinite(immersivityScale)) m_immCoordScale_ = std::clamp(immersivityScale, 0.5f, 1.0f);
    }
    void setCompressorParams(float thresholdDb, float ratio) noexcept {
        if (std::isfinite(thresholdDb) && std::isfinite(ratio)) {
            m_compThresholdDb_ = std::clamp(thresholdDb, -60.0f, 0.0f);
            m_compRatio_       = std::clamp(ratio, 1.0f, 20.0f);
        }
    }
    void setRouteProfile(float bassDb, float dialogDb, float widener) noexcept {
        if (std::isfinite(bassDb) && std::isfinite(dialogDb) && std::isfinite(widener)) {
            m_routeBassDb_      = std::clamp(bassDb, -12.0f, 12.0f);
            m_routeDialogDb_    = std::clamp(dialogDb, -12.0f, 12.0f);
            m_routeWidenerMult_ = std::clamp(widener, 0.0f, 2.0f);
        }
    }
    void setEqGains(const float* gains, int n,
                    float listenPhon, float refPhon) noexcept {
        if (!gains || n <= 0) return;
        // Antes: solo la MEDIA de las ganancias se aplicaba como trim (los sliders no moldeaban el espectro en
        // Ruta B). Ahora cada banda se aplica de verdad (biquads peaking, mismas frecuencias que la UI); el trim
        // conserva solo el ajuste por curva de loudness (phonDelta).
        for (int i = 0; i < 10; ++i)
            m_userEqDb_[i] = (i < n && std::isfinite(gains[i])) ? std::clamp(gains[i], -12.0f, 12.0f) : 0.0f;
        const float meanEqDb = 0.0f;
        const float phonDelta = (std::isfinite(listenPhon) && std::isfinite(refPhon) && refPhon > 0.0f)
            ? std::clamp((refPhon - listenPhon) * 0.05f, -3.0f, 3.0f)
            : 0.0f;
        m_eqTrimTarget_ = std::pow(10.0f, std::clamp(meanEqDb + phonDelta, -12.0f, 12.0f) / 20.0f);
    }
    /// NPE neuromórfico (solo Ruta B: omega_effect lo llama desde el snapshot v7; Ruta A usa IvannaNpeEngine en la app).
    void setNpe(bool on, float harmonic, float lateral, float ohc, float masterDb) noexcept {
        m_npeOn_ = on; m_npeHarm_ = harmonic; m_npeLat_ = lateral; m_npeOhc_ = ohc; m_npeMasterDb_ = masterDb;
    }
    /// Cancelación de diafonía para altavoces (Ruta B; la Ruta A la corre la cadena 4D de la app).
    void setCrosstalk(bool on) noexcept { m_xtc.setEngage(on); }
    /// Recorte por térmico/CPU (mismo criterio que Volterra): el NPE se desvanece (20 ms) y vuelve solo.
    void setNpeShed(bool shed) noexcept { m_npeShed_ = shed; }
    /// Recorte térmico/CPU de ORBE (mismo criterio que NPE/Volterra, tier >= LIMITED):
    /// el núcleo cae a bypass (12 bandas -> off) y vuelve solo al enfriarse.
    void setOrbeThermalShed(bool shed) noexcept { m_orbeShed_ = shed; }
    /// Recorte térmico/CPU de HOLOS: al activarse, HOLOS pasa a Bypass
    /// (tier 3 → cadena legacy bit-exacta). Los tiers intermedios
    /// (Warm/Limited) los gobierna el gestor térmico con setHolosThermalTier.
    void setHolosThermalShed(bool shed) noexcept { m_holosShed_ = shed; }
    void setHolosThermalTier(int tier) noexcept { m_holosTier_ = tier; }
    int npeSpikes() const noexcept { return m_npe.lastSpikes(); }
    void setIntensity(float intensity) noexcept {
        if (std::isfinite(intensity)) {
            m_intensityTarget_ = std::clamp(intensity, 0.0f, 1.0f);
        }
    }

    // ── Bucle cerrado neuro-acústico (v2.5.365, Ruta B) ─────────────────────
    // Apagado por defecto. Sin frames reales (EEG/ABR/fNIRS por JNI) el
    // bucle nunca se activa: compute() falla cerrado a identidad y
    // NeuroBandEq es bypass bit-exacto. ingestNeuroFrame() solo acepta
    // kinds reales (2=EEG, 3=ABR, 4=fNIRS); kind 0/1 se rechaza.
    // timestampUs debe ser CLOCK_MONOTONIC en microsegundos, el mismo
    // dominio que usa compute() (ver monotonicNowUs()): en Kotlin,
    // SystemClock.uptimeMillis() * 1000. Un timestamp en otro dominio
    // se ve como obsoleto/futuro y el bucle no aprende.
    void setNeuroLoopEnabled(bool enable) noexcept {
        m_neuroLoopEnabled_ = enable;
        if (!enable) {
            m_neuroLoop_.reset();
            const std::array<float, 8> zero{};
            m_neuroBandEq_.setGains(zero, m_sampleRateF);
        }
    }
    bool isNeuroLoopEnabled() const noexcept { return m_neuroLoopEnabled_; }
    bool ingestNeuroFrame(int kind, std::uint64_t timestampUs,
                          const float* response8, const float* coherence8,
                          float quality, float abrWaveVLatencyMs,
                          float fnirsHbO) noexcept {
        if (!m_neuroLoopEnabled_ || !response8 || !coherence8) return false;
        if (kind != 2 && kind != 3 && kind != 4) return false;
        ivanna::neuromorphic::NeuroFrame frame{};
        frame.kind = static_cast<ivanna::neuromorphic::NeuroSensorKind>(kind);
        frame.timestampUs = timestampUs;
        frame.quality = quality;
        frame.abrWaveVLatencyMs = abrWaveVLatencyMs;
        frame.fnirsHbO = fnirsHbO;
        for (int b = 0; b < 8; ++b) {
            frame.response[static_cast<std::size_t>(b)] = response8[b];
            frame.coherence[static_cast<std::size_t>(b)] = coherence8[b];
        }
        return m_neuroLoop_.ingest(frame);
    }
    // Telemetría del bucle (hilo de control; no toca el audio).
    void neuroLoopStatus(bool& active, float& confidence,
                         std::uint64_t& framesLearned, float& wet) const noexcept {
        active = m_neuroLoopEnabled_ &&
                 m_neuroLoop_.isActive(
                     ivanna::neuromorphic::monotonicNowUs());
        confidence = m_neuroLoop_.confidence();
        framesLearned = m_neuroLoop_.framesLearned();
        wet = m_neuroLoop_.lastWetScale();
    }

    IvannaAudioClassifier*     getClassifier()    const noexcept { return m_classifier; }
    IvannaVoiceProsodyEngine*  getProsodyEngine() const noexcept { return m_prosody; }
    const IntelligentUpmixer&  getUpmixer()       const noexcept { return m_upmixer; }
    IntelligentUpmixer&        getUpmixer()             noexcept { return m_upmixer; }

private:

    float sampleRate_ = 48000.0f;

    // FUSIÓN MAGISTRAL (2026-11-18, Ruta B): la cadena de los 7 operadores
    // (núcleo compartido con Ruta A). useFusedChain_=true por defecto;
    // false = rollback al camino legacy (FIFO + process()).
    ivanna::fused::FusedChain fusedChain_;
    bool useFusedChain_ = true;
    // Sincroniza los controles fusionados desde los atomics del control
    // plane (g_orbe_*, g_holos_*, g_hyper_*, g_wfs_*, ...). Definida en
    // IvannaFusionCore.cpp (ahí viven los extern de los atomics).
    void syncFusedControlsFromAtomics() noexcept;

    [[maybe_unused]] bool goldenEarMode_ = false;

    [[maybe_unused]] float safLatent_[7]{};


    HrtfManagerT<float>* m_hrtf = nullptr;
    std::atomic<bool> m_hrtfEnabled_{true};
    EvolutionaryEQ* m_evoEq = nullptr;
    // Interruptor Ruta B del EvoEq (paridad con Ruta A g_evoEqAEnabled).
    // 0038+0040 Parche C: neutro por defecto (false). Antes true forzaba la
    // curva Harman (audífonos) sobre la cadena DAC→Genezi aunque el snapshot
    // fuera neutro. Lo gobierna setEvoEqEnabled(); ver nota ahí.
    std::atomic<bool> m_evoEqEnabled{false};
    Psychoacoustics* m_psycho = nullptr;
    IvannaAudioClassifier* m_classifier = nullptr;
    IvannaVoiceProsodyEngine* m_prosody = nullptr;
    IvannaSuperAgentMemory* m_memory = nullptr;

    bool m_goldenEarActive = false;
    bool m_volterraEnabled = true; // estado de compatibilidad; no controla ctx->volterraEngine
    bool m_fastRpcEnabled = false;
    bool m_atiEnabled = true;

    IntelligentUpmixer m_upmixer;
    HoaBinauralDecoder m_hoaDecoder;
    // ORBE — Binaural Esférico de Campo Cercano (primera etapa espacial
    // en serie de process(): tras upmixer/HRTF y tras el blend WFS, antes
    // de HOLOS, IVANNA, Φ y del M/S — ORBE → HOLOS → IVANNA → Φ).
    ivanna::OrbeBinaural m_orbe;
    // HOLOS — Operador Único de Campo Acústico. Corre EN SERIE tras ORBE
    // (ver process()): ambos activos a la vez por directiva 2026-11-08;
    // ORBE ya no es un fallback legacy sino la etapa previa de HOLOS.
    ivanna::HolosOperator m_holos;
    // IVANNA — Operador Unificado de Campo Espacial (v2.5.375). Tras HOLOS
    // y antes de Φ (ver process()). amount=0 por defecto en Ruta B hasta
    // medir CPU en el G85 (directiva 2026-11-09); sin campos omni_* en el
    // snapshot (ABI v10) los parámetros viven en los atomics g_omni_* del
    // stub omni_globals_effect.cpp (mismo patrón que g_orbe_*/g_holos_*).
    ivanna::omnifield::OmniField m_omni;
    // Hiper-Operador de Estado Φ (v2.5.369): muestra a muestra, sin FFT.
    // Tras m_room, antes de m_xtc (ver process()).
    ivanna::spatial::HyperStateOperator m_hyper;

    // ── Wave Field Synthesis (2026-09-19) — ruta real de audio ──
    // El renderer sintetiza el campo de ondas de la señal estéreo como
    // dos fuentes primarias (L y R) sobre el array circular virtual y lo
    // mezcla binauralmente. La activación/desactivación es GLITCH-FREE:
    // crossfade smoothstep de 20 ms entre la rama seca (post-HOA/HRTF) y
    // la rama WFS, con estado atómico externo (g_wfs_enabled) — jamás un
    // switch duro en el callback. Cero allocs en el hot path: los buffers
    // se reservan en prepare()/init() y solo crecen si BLOCK_SIZE cambia.
    ivanna::spatial::WfsRenderer m_wfs;
    ivanna::spatial::CrosstalkCanceller m_xtc;        // diafonía para altavoces (apagado = identidad)
    ivanna::neuromorphic::NpeStereoCore m_npe;        // NPE neuromórfico en Ruta B (apagado = bit-exacto)
    // Bucle cerrado neuro-acústico (v2.5.365): controlador + EQ por banda.
    // Se prepara en el ctor / initSpatial con el sampleRate real; process()
    // lo actualiza 1× por bloque ANTES de HOLOS/ORBE (ver updateNeuroLoop
    // en IvannaFusionCore.cpp). Apagado por defecto.
    ivanna::neuromorphic::ClosedLoopController m_neuroLoop_;
    ivanna::neuromorphic::NeuroBandEq m_neuroBandEq_;
    bool m_neuroLoopEnabled_ = false;
    bool  m_npeOn_ = false, m_npeShed_ = false, m_orbeShed_ = false, m_holosShed_ = false, m_omniShed_ = false;
    int   m_holosTier_ = 0;  // HolosThermalTier (0 Normal … 3 Bypass)
    float m_npeHarm_ = 0.2f, m_npeLat_ = 0.2f, m_npeOhc_ = 0.3f, m_npeMasterDb_ = 0.0f;
    ivanna::dsp::RoomCorrectionEq m_userEq;           // EQ de 10 bandas del usuario (antes solo se aplicaba su media como trim)
    float m_userEqDb_[10] = {};
    ivanna::dsp::RoomCorrectionEq m_room;             // corrección de sala medida (RoomProbe) — bit-exacto con todo en 0
    ivanna::dsp::TemporalSpatialDynamics m_temporal;  // 4D: el lateral se recoge en cada golpe y se abre en lo sostenido
    ivanna::dsp::AuxFieldCoherence m_aux;   // modo AUX/altavoces: graves coherentes (ver AuxFieldCoherence.hpp)
    ivanna::supreme::SupremeTransitionEnvelope m_upmixEnv_{};
    bool  m_wfsInit      = false;
    float m_wfsFade      = 0.0f;    // 0 = bypass, 1 = WFS pleno
    std::vector<float> m_wfsInL, m_wfsInR;      // entradas mono por fuente
    std::vector<float> m_wfsOutL, m_wfsOutR;    // salida WFS del bloque
    float m_sampleRateF  = 48000.0f;
    // Buffer del campo HOA intermedio, reusado bloque a bloque (Upmixer lo
    // redimensiona solo si BLOCK_SIZE cambia — nunca malloc en el camino
    // caliente en régimen estable).
    std::vector<HoaVector> m_hoaField;

    struct FilterState {
        float x1 = 0.0f;
        float x2 = 0.0f;
        float y1 = 0.0f;
        float y2 = 0.0f;
    };

    FilterState m_chebLpfL;
    FilterState m_chebLpfR;
    float m_h2DcMeanL = 0.0f;
    float m_h2DcMeanR = 0.0f;

    // Slew-limiter de la ganancia armónica (slider UI → snapshot SHM → aquí).
    // m_harmSmoothed_ persigue a m_harmGainTarget_ a razón de kHarmSlew
    // por muestra (~0→2 en ≈167 ms a 48 kHz): imperceptible como retardo,
    // imposible como clic. Sin malloc ni locks en la ruta caliente.
    // 0038+0040 Parche C: neutro por defecto (0.0f). Antes 1.0f ejecutaba
    // applyGoldenEarGAN() aunque m_goldenEarActive fuera false; el snapshot
    // (harmonic_gain) lo sube cuando el usuario lo activa.
    float m_harmGainTarget_  = 0.0f;
    float m_harmSmoothed_    = 0.0f;
    float m_spatialWidthTarget_   = 1.0f;
    float m_spatialWidthSmoothed_ = 1.0f;
    float m_compThresholdDb_      = -12.0f;
    float m_compRatio_            = 1.0f;
    float m_compEnv_              = 0.0f;
    float m_harmCoordScale_       = 1.0f;
    float m_wfsCoordScale_        = 1.0f;
    // Apertura del campo WFS acondicionada: recorte al radio util del array,
    // suavizado de un polo (sin zipper al mover el slider) y defensa NaN.
    // Antes el valor se usaba crudo: spread > 1 sacaba las fuentes del array.
    ivanna::spatial::WfsSpreadGovernor m_wfsGov;
    float m_immCoordScale_        = 1.0f;
    float m_routeBassDb_          = 0.0f;
    float m_routeDialogDb_        = 0.0f;
    float m_routeWidenerMult_     = 1.0f;
    float m_eqTrimTarget_         = 1.0f;
    float m_eqTrimSmoothed_       = 1.0f;
    float m_intensityTarget_      = 1.0f;
    [[maybe_unused]] float m_intensitySmoothed_    = 1.0f;

    // Carry-over Ring FIFO para desinterleaving y tamaños arbitrarios de bloque
    static constexpr size_t kFifoCapacity = 16384;
    std::vector<float> m_inFifoL;
    std::vector<float> m_inFifoR;
    std::vector<float> m_outFifoL;
    std::vector<float> m_outFifoR;
    size_t m_inFifoCount = 0;
    size_t m_inFifoReadPos = 0;
    size_t m_inFifoWritePos = 0;
    size_t m_outFifoCount = 0;
    size_t m_outFifoReadPos = 0;
    size_t m_outFifoWritePos = 0;
};

} // namespace Ivanna

// ── Alias de compatibilidad — NAMESPACE GLOBAL ───────────────────────────────
// CRÍTICO: el alias debe estar en el namespace GLOBAL, no en Ivanna{}.
// omega_effect.cpp referencia el nombre histórico IvannaFusionCore sin
// cualificar (además de `using namespace Ivanna;`); sin este alias global el
// tipo no se resuelve en los TUs que solo incluyen este header.
using IvannaFusionCore = Ivanna::IvannaFusionEngine;
