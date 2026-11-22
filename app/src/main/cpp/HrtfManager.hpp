#pragma once
#include "IvannaFusionCore.hpp"
#include "spatial/SafmusLatentGuard.hpp"
#include "HRTFBinLoader.hpp"
#include "SofaHRTFLoader.hpp"
#include "hexagon/ivanna_fastrpc_client.hpp"
#include "supreme/SupremeTransitionEnvelope.hpp"
#include <algorithm>  // FIX: std::clamp usado en setSafLatentQ() sin este include
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Ivanna {
#ifndef IVANNA_HRTF_TAPS_DEFINED
#define IVANNA_HRTF_TAPS_DEFINED
constexpr size_t HRTF_TAPS = 128;
#endif
constexpr int XFADE_FRAMES = 128;

// F2/486 (labor grande, motor double): plantilla con default float +
// alias. HrtfManager = float histórico bit-idéntico;
// HrtfManagerD = núcleo double (lo usa FusedSpatial).
// El plano de SEÑAL (bancos HRIR, historial overlap-save, ganancia de
// distancia, crossfade) es Scalar; los datasets (IHR1/SOFA/PCA), la
// síntesis analítica, el SAF latente y la envolvente wet/dry
// (SupremeTransitionEnvelope, float) quedan float = control/datos.
// La ruta Hexagon/DSP (FastRPC, float por hardware) solo existe en la
// instancia float; el núcleo double convoluciona en CPU.
template <typename Scalar = float>
class HrtfManagerT {
public:
    HrtfManagerT();

    // FALLO 6 (2026-11-18): el ctor fija la envolvente wet/dry (m_wetEnv) a
    // 48000 Hz porque al construir aún no se conoce la SR real. prepare()
    // la reconfigura con la SR de la sesión (la misma que recibe
    // FusedSpatial::prepare / FusedChain::prepare). Sin esta llamada, a
    // 96/192 kHz las rampas de wet/dry duran la mitad / cuarta parte de lo
    // diseñado en ms (SupremeTransitionEnvelope calcula las muestras como
    // duración_ms * SR). SR fuera de 8000..384000 Hz o no finita → 48000.
    // No toca currentGain/step: una rampa en vuelo conserva su paso y la
    // siguiente transición ya usa la SR nueva. Llamar entre bloques (o
    // antes del primer bloque), nunca realloc: solo reescribe floats.
    // PARCHE v2.6: firma float (como pide el parche) + setImmediate
    // (currentGain) tras configure() para fijar la envolvente en su valor
    // actual con la SR nueva. PENDIENTE (fuera de este fix):
    // cfg.sample_rate_in = 48000 del cliente FastRPC en el ctor (ruta DSP
    // Hexagon) sigue fijo a 48 kHz.
    void prepare(float sampleRate) noexcept;

    // Carga un banco de HRIRs medidos desde un archivo IHR1 o IVHRTF01.
    // Cuando está disponible, setHeadPose() selecciona la posición más
    // cercana del dataset en lugar de sintetizar analíticamente.
    // Retorna true si el dataset se cargó correctamente.
    bool loadFromDataset(const char* path);

    // ── SOFA medido (v2.5.472 Fase 2 ítem D) ─────────────────────────────
    // Carga un SOFA real (Data.IR leído de verdad por SofaHRTFLoader) y lo
    // instala como fuente de HRIR por pose (tiene precedencia sobre el
    // dataset IHR1 y sobre la síntesis analítica). Solo acepta archivos
    // con Status::PARSED_MEASURED: un SOFA que no se puede parsear NO
    // desplaza el dataset vigente (fail-closed, retorna false).
    // Retorna true si el SOFA medido quedó activo.
    bool loadSofaDataset(const char* path);
    bool hasSofaDataset() const noexcept { return m_sofaLoaded; }

    // Selector de sala firma del catálogo SOFA medido (SignatureRooms.kt).
    // Lee ListenerRoomControls::signatureRoom (0=ninguna, 1..3=salas) desde
    // el llamador (JNI): 1 = TU-Berlin QU KEMAR (0.5 m), 2 = MIT KEMAR
    // normal pinna (1.4 m), 3 = THK KU100 (3.25 m). Con 0 se desactiva el
    // SOFA y vuelve el dataset IHR1 o la síntesis. Busca el .sofa en
    // m_sofaSearchDir (si se fijó) y en las rutas del dispositivo donde la
    // APK/el módulo extraen los assets. Retorna false si el id es inválido
    // o el archivo no se pudo cargar como medido (sin tocar el estado).
    bool setSignatureRoom(int id);
    int signatureRoom() const noexcept { return m_signatureRoom; }
    // Directorio que se prueba PRIMERO al buscar los .sofa del catálogo
    // (tests: IVANNA_SOFA_ASSET_DIR; app: dir de assets extraídos).
    void setSofaSearchDir(const std::string& dir) { m_sofaSearchDir = dir; }

    // ── Slot para RIR medida (medición con micrófono: OTRO paso, necesita
    // al dueño; NO implementada aquí) ─────────────────────────────────────
    // Acepta una RIR estéreo medida y la guarda como sala instalada, lista
    // para que el operador de sala (RirConvolver / fused_listener_room) la
    // consuma cuando se cablee. Hoy es solo staging: NADA la convoluciona
    // aún (la medición con micrófono y su cableado quedan pendientes).
    // taps 1..1M, SR 1000..384000 Hz, muestras finitas; si no, false.
    bool installMeasuredRoomRir(const float* left, const float* right, size_t taps,
                                float sampleRateHz);
    void clearMeasuredRoomRir() noexcept;
    bool hasMeasuredRoomRir() const noexcept { return !m_roomRirL.empty(); }
    const std::vector<float>& measuredRoomRirLeft() const noexcept { return m_roomRirL; }
    const std::vector<float>& measuredRoomRirRight() const noexcept { return m_roomRirR; }
    float measuredRoomRirSampleRate() const noexcept { return m_roomRirSampleRate; }

    bool hasDataset() const noexcept { return m_datasetLoaded; }

    void processBinauralScene(Ivanna::AudioBuffer* buffer);
    // Entrada double para la cadena double (FusedSpatial): procesa n
    // muestras en bloques de BLOCK_SIZE (128); si n no es múltiplo, el
    // resto pasa en seco (fail-safe, sin silencio). La instancia float
    // expone la misma sobrecarga en float.
    void processBinauralScene(Scalar* left, Scalar* right, int n);
    void setHeadPose(float yaw, float pitch, float roll);

    // Ruta B (omega_effect): distancia de la ESCENA publicada por el daemon (omega_control_bus.h, npe_ctl bits 16..31).
    // Aplica la ley 1/d de nivel (HrirPoseSelect.hpp::sceneDistanceGain) con rampa sin clic; el azimut/elevación llegan por
    // setHeadPose() (head_yaw/pitch_rad). Rechaza (false) lo no finito o fuera de 0.5..2 sin tocar el estado.
    bool setSceneDistance(float distanceScale) noexcept;
    float getSceneDistanceGainTarget() const noexcept { return m_distGainTarget.load(std::memory_order_relaxed); }

    // FIX (cableado binaural, FASE 8): pose de cabeza actual expuesta como
    // grados azimuth/elevation — mismo valor que setHeadPose() ya calcula
    // internamente para el dataset medido (yaw*(180/pi) + safBias), pero
    // aquí queda accesible para cualquier consumidor que necesite la
    // posición real vigente (p.ej. el path FastRPC/Hexagon DSP).
    float getCurrentAzimuthDeg() const noexcept {
        return m_currentAzimuthDeg.load(std::memory_order_relaxed);
    }
    float getCurrentElevationDeg() const noexcept {
        return m_currentElevationDeg.load(std::memory_order_relaxed);
    }
    void setRiemannianCurvature(float curvature) {
        m_intrinsicCurvature.store(curvature, std::memory_order_relaxed);
    }

    // FIX (DAC USB-C): wet/dry blend — permite bypassear parcial o totalmente
    // la convolución HRTF. Útil al conectar un DAC USB-C: llamar setWetDry(0)
    // + flushHistory() elimina el ruido de "canal sin señal" producido por
    // muestras residuales en el historial cuando cambia la ruta de audio.
    // wet=0 → señal seca estéreo pura; wet=1 → binaural completo (default).
    void setWetDry(float wet) noexcept {
        const float w = std::clamp(wet, 0.f, 1.f);
        m_wetDry.store(w, std::memory_order_relaxed);
        if (m_wetEnv.renderedBlocks == 0u) {
            m_wetEnv.setImmediate(w);
        }
    }
    float getWetDry() const noexcept {
        return m_wetDry.load(std::memory_order_relaxed);
    }

    // LGN — LEY GEODÉSICA DEL NOTCH (física nueva, cableada 2026-11-18):
    // el pabellón se aplica como FIR de 3 taps al HRIR base en
    // synthesizeHrtf() (ver HrtfManager.cpp). Por defecto ACTIVO: la HRTF
    // analítica lleva los notches de pinna medidos en KEMAR. Desactivarlo
    // (false) deja la síntesis como antes: esfera/PCA sola, sin pabellón
    // (fallback). Solo afecta a la PRÓXIMA síntesis (setHeadPose o ctor);
    // no re-sintetiza el banco vigente. Sin cambio de ABI.
    void setPinnaEnabled(bool enabled) noexcept {
        m_pinnaEnabled.store(enabled, std::memory_order_relaxed);
    }
    bool pinnaEnabled() const noexcept {
        return m_pinnaEnabled.load(std::memory_order_relaxed);
    }

    // Limpia el historial de convolución overlap-save.
    // DEBE llamarse tras setWetDry(0) cuando el DAC USB-C se conecta/desconecta
    // para evitar que muestras cacheadas corrompan el siguiente bloque.
    void flushHistory() noexcept;

    // FIX: punto de entrada del vector latente SAF q[7] en el motor HRTF.
    // q[0] modula la curvatura Riemanniana intrínseca (personalización espacial).
    // q[1..2] ajustan el azimut efectivo del banco activo.
    // q[3..6] quedan reservados para morph fino de lóbulos en datasets medidos.
    // Rango esperado: [-1.0, +1.0] por dimensión (salida normalizada del SAF optimizer).
    void setSafLatentQ(const float* q, int size) noexcept {
        // SAFMUS: validate the complete latent before mutating any HRTF parameter.
        if (size < 7 || !ivanna::safmus::isFiniteLatent(q)) return;
        // q[0] → curvatura Riemanniana base ±0.15 alrededor del valor por defecto
        const float cur = 0.15f + std::clamp(q[0], -1.f, 1.f) * 0.15f;
        const float bias = (size >= 2) ? q[1] * 10.f : 0.f;
        // Cambio material => el banco HRTF vigente quedo desfasado del SAF: se marca para
        // re-sintetizar en el hilo de audio (antes solo ocurria en el proximo setHeadPose()).
        if (std::fabs(cur - m_intrinsicCurvature.load(std::memory_order_relaxed)) > 0.002f ||
            std::fabs(bias - m_safAzimuthBias.load(std::memory_order_relaxed)) > 0.05f)
            m_safDirty.store(true, std::memory_order_release);
        m_intrinsicCurvature.store(cur, std::memory_order_relaxed);
        m_safAzimuthBias.store(bias, std::memory_order_relaxed);
        for (int i = 0; i < 7 && i < size; ++i)
            m_safQ[i] = q[i];
        m_safQValid.store(true, std::memory_order_release);
    }

    // Evidencia MEDIDA en el camino de audio (no indicadores de UI).
    struct Evidence {
        uint64_t cpuBlocks = 0;      // bloques convolucionados por CPU con wet>0
        uint64_t dspBlocks = 0;      // bloques delegados al Hexagon (sin medicion propia)
        uint64_t safRefreshes = 0;   // re-sintesis de banco forzadas por cambio de SAF q
        uint64_t poseApplies = 0;    // setHeadPose efectivos
        double inEnergy = 0.0, outEnergy = 0.0;
        float azimuthDeg = 0.f, curvature = 0.f, safBiasDeg = 0.f;
        bool datasetLoaded = false;   // false => sintesis analitica (Rayleigh), no HRIR medidas
    };
    Evidence evidence() const noexcept {
        Evidence e;
        e.cpuBlocks = m_evCpu.load(std::memory_order_relaxed);
        e.dspBlocks = m_evDsp.load(std::memory_order_relaxed);
        e.safRefreshes = m_evSafRefresh.load(std::memory_order_relaxed);
        e.poseApplies = m_evPose.load(std::memory_order_relaxed);
        e.inEnergy = m_evIn.load(std::memory_order_relaxed);
        e.outEnergy = m_evOut.load(std::memory_order_relaxed);
        e.azimuthDeg = m_currentAzimuthDeg.load(std::memory_order_relaxed);
        e.curvature = m_intrinsicCurvature.load(std::memory_order_relaxed);
        e.safBiasDeg = m_safAzimuthBias.load(std::memory_order_relaxed);
        e.datasetLoaded = m_datasetLoaded || m_sofaLoaded;
        return e;
    }

private:
    std::atomic<bool> m_safDirty{false};
    std::atomic<float> m_lastYaw{0.f}, m_lastPitch{0.f}, m_lastRoll{0.f};
    std::atomic<uint64_t> m_evCpu{0}, m_evDsp{0}, m_evSafRefresh{0}, m_evPose{0};
    std::atomic<double> m_evIn{0.0}, m_evOut{0.0};
    // Síntesis analítica (Rayleigh esférico) — fallback cuando no hay dataset
    void synthesizeHrtf(float yaw, float pitch, float roll, int bank);
    void processBinauralSceneImpl(Scalar* left, Scalar* right);
    void applyDistanceGain(Scalar* left, Scalar* right, size_t n) noexcept;
    std::atomic<float> m_distGainTarget{1.0f};
    Scalar             m_distGainCur = Scalar(1.0);

    // Carga los filtros de la posición del dataset más cercana al azimut dado
    void loadFromDatasetAtPose(float azimuthDeg, float elevationDeg, int bank);

    // Carga en el banco el HRIR SOFA medido de la pose más cercana, con
    // remuestreo lineal a la SR de la sesión (m_sampleRate) y truncado a
    // HRTF_TAPS. Sin efecto si no hay SOFA cargado o la pose no es finita
    // (el banco queda intacto: fail-closed). El HRIR medido ya lleva
    // pinna/ITD/ILD: se guarda tal cual, sin pabellón LGN ni crossfeed
    // añadido, en el mismo orden invertido que loadFromDatasetAtPose.
    void loadSofaAtPose(float azimuthDeg, float elevationDeg, int bank);

    std::atomic<float> m_wetDry{1.0f};   // FIX DAC USB-C: 0=dry, 1=full HRTF
    ivanna::supreme::SupremeTransitionEnvelope m_wetEnv{};
    std::atomic<int>   m_activeBank{0};
    std::atomic<int>   m_pendingBank{0};
    int                m_pendingBankLocal = 0;

    std::atomic<bool>  m_xfadeTrigger{false};
    bool               m_xfading = false;
    size_t             m_xfadePos = 0;

    std::atomic<float> m_intrinsicCurvature{0.15f};
    // LGN (2026-11-18): SR real de la sesión (la fija prepare(); el ctor
    // asume 48 kHz). synthesizeHrtf() la usa para convertir los retardos
    // del pabellón (segundos) a muestras. Sin ella los retardos del
    // pabellón no se pueden calcular: el resto de la síntesis (ITD 18
    // muestras, crossfeed 14) sigue asumiendo 48 kHz como antes.
    std::atomic<float> m_sampleRate{48000.0f};
    // LGN (2026-11-18): true = pabellón activo (default); false = esfera
    // sola (fallback). Ver setPinnaEnabled().
    std::atomic<bool>  m_pinnaEnabled{true};
    // Ley de Curvatura (2026-11-18): distancia de la escena en metros
    // (la fija setSceneDistance(); default 1.0). synthesizeHrtf() la usa
    // para el ITD en forma cerrada (curvatureLawItdSeconds). Solo se lee
    // en la síntesis (setHeadPose), no en el camino caliente.
    std::atomic<float> m_sceneDistance{1.0f};
    std::atomic<float> m_safAzimuthBias{0.f};   // sesgo de azimut fino desde q[1]
    std::atomic<bool>  m_safQValid{false};
    float              m_safQ[7]{};               // vector latente completo
    bool               m_datasetLoaded = false;

    // FIX (cableado binaural, FASE 8): última pose de cabeza real recibida
    // por setHeadPose(), en grados — la misma conversión que ya se aplica
    // para posicionar el banco HRTF sintetizado/medido, pero conservada
    // aquí para que processBinauralScene() (path FastRPC/Hexagon) pueda
    // leer la posición real en vez de un valor fijo en cero.
    std::atomic<float> m_currentAzimuthDeg{0.f};
    std::atomic<float> m_currentElevationDeg{0.f};

    ALIGN_NEON Scalar m_hrtfLL[2][HRTF_TAPS]; // L → oído izquierdo
    ALIGN_NEON Scalar m_hrtfLR[2][HRTF_TAPS]; // L → oído derecho (crosstalk)
    ALIGN_NEON Scalar m_hrtfRR[2][HRTF_TAPS]; // R → oído derecho
    ALIGN_NEON Scalar m_hrtfRL[2][HRTF_TAPS]; // R → oído izquierdo (crosstalk)

    ALIGN_NEON Scalar m_histL[BLOCK_SIZE + HRTF_TAPS];
    ALIGN_NEON Scalar m_histR[BLOCK_SIZE + HRTF_TAPS];

    // Distinct, preallocated FastRPC input staging prevents input/output aliasing
    // and lets CPU convolution restore the original dry block if Hexagon rejects it.
    alignas(64) float m_dspDryL[BLOCK_SIZE];
    alignas(64) float m_dspDryR[BLOCK_SIZE];

    // Dataset cargado (HRIRs medidos de hasta 1250 posiciones)
    HRTFBinLoader m_loader;
    ivanna::dsp::IvannaFastRpcClient m_fastRpcClient;

    // ── SOFA medido (v2.5.472 Fase 2 ítem D) ─────────────────────────────
    // Respaldo de la API pública declarada arriba. D dejó las declaraciones
    // sin estos miembros y el header NO compilaba (m_sofaLoaded,
    // m_signatureRoom, m_sofaSearchDir, m_roomRirL/R y m_roomRirSampleRate
    // sin declarar): se añaden aquí sin cambiar ninguna firma pública.
    SofaHRTFLoader m_sofaLoader;
    bool           m_sofaLoaded = false;
    int            m_signatureRoom = 0;
    std::string    m_sofaSearchDir;
    std::vector<float> m_roomRirL, m_roomRirR;
    float          m_roomRirSampleRate = 0.0f;
};
// Alias de compat: el float histórico, bit-idéntico; el double es el
// núcleo de la labor grande.
using HrtfManager = HrtfManagerT<float>;
using HrtfManagerD = HrtfManagerT<double>;

} // namespace Ivanna
