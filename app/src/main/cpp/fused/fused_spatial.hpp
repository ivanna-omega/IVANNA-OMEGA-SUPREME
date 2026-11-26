#pragma once
// fused_spatial.hpp — OPERADOR ESPACIAL ÚNICO (ORBE·Ω) de la Fusión Magistral
// (2026-11-18, directiva de Esse; nombre aprobado ~07:06 CST).
//
// Fusiona en UNA cadena, en este orden:
//   1. IntelligentUpmixer (estéreo → campo HOA orden ≤2) — solo si upmixEnabled.
//   2. HoaBinauralDecoder (campo HOA → binaural, 8 altavoces virtuales).
//   3. HrtfManager (HRTF SOFA/KEMAR medido, o síntesis analítica Rayleigh si
//      no hay dataset; wet/dry por hrtfWet).
//   4. WfsRenderer (2 objetos L/R a ±apertura, mezcla de potencia constante
//      según wfsSpread; la apertura la modula el análisis de escena del
//      StereoObjectDecomposer). Con kPoincarePanEnabled (OFF por defecto)
//      el spread WFS lo sustituye el pan geodésico de Poincaré
//      (spatial/poincare_binaural.hpp), con el mismo wet/dry wfsMix_.
//   5. OrbeBinaural (ORBE, campo cercano esférico por bandas ERB).
//   6. CrosstalkCanceller (solo SPEAKERS / Genezi).
//   7. AuxFieldCoherence (solo SPEAKERS / Genezi; preset AuxGeneziRoute
//      GeneziRoom: xover 120 Hz, coherencia 1.0).
//
// PUERTA GENEZI (Fix 1, v2.5.465): en modo SPEAKERS la cadena de audífonos
// se neutraliza DENTRO del operador (ORBE amount→0 efectivo, HRTF wet→0 y
// sin llamada) y se activa la ruta de altavoz (XTC + AuxField). En modo
// HEADPHONES, cadena binaural normal.
//
// ARBITRAJE DE UNA SOLA ETAPA BINAURAL (Fallo 3d): las cuatro etapas
// binaurales — upmix HOA + decoder, HRTF, WFS y ORBE — corrían en serie
// sobre el mismo audio (cuádruple binaural). Ahora solo UNA renderiza por
// trozo, por prioridad HOA > ORBE > HRTF > WFS; las demás se saltan salvo
// su fade de salida (ORBE 15 ms, WFS rampa a 0), que es un fundido, no un
// segundo render. En SPEAKERS ninguna etapa binaural corre (tampoco el
// decoder HOA: su salida es binaural).
//
// TROZOS EXACTOS DE 128 (Fallo 3d): HrtfManager solo acepta AudioBuffer
// de 128 fijos y el código viejo rellenaba la cola con ceros cuando el
// trozo era menor (un breve silencio inyectado en la historia del HRTF).
// process() acumula ahora la entrada en inPend_ y solo llama a
// processChunk() con 128 exactos; la salida sale de la cola outPend_
// (retraso ≤127 muestras; el primer trozo parcial sale en seco hasta que
// se acumula el primer 128). reset()/prepare() vacían los pendientes
// (frontera de stream: se pierden ≤127 muestras, como un reinicio).
//
// Head tracking SO(3): HeadTracker (pose cruda del sensor) + HeadTrackerFusion
// (fusión + predicción al instante del frame). La pose llega por
// feedRotationVector() desde la app (IvannaHeadTracker, Kotlin); sin
// alimentación el yaw es 0 y el parallax no se mueve — no se inventa pose.
// El yaw fusionado alimenta ORBE/HRTF (setHeadPose) y el offset del XTC.
//
// NO envueltos en la ruta de audio (documentado, no omitido):
//   - SceneGeometryRenderer: renderiza geometría a imagen PPM en disco;
//     no tiene process() de audio. Su función la cubre el preset de sala
//     (AuxGeneziRoute) y la geometría WFS.
//   - DopPacketizer: empaqueta bytes DSD a PCM DoP (formato de salida,
//     no procesado espacial); no consume PCM.
//   - AuxGeneziRoute (clase): solo presets/parámetros y la puerta Volterra
//     (AuxVolterraGate, que pertenece al Operador Transductor VOLTERRA·Ω);
//     sus valores GeneziRoom SÍ se usan (xover/coherencia del AuxField).
//   - StereoObjectDecomposer: SÍ envuelto como análisis de escena (sus 4
//     objetos no alimentan WFS: la prescripción de la fusión es 2 objetos
//     L/R; el descomponedor mide el ancho real (sideRatio) y modula la
//     apertura WFS).
//
// Reglas duras (fused_controls.hpp): neutro = bit-transparente (process()
// retorna sin tocar el buffer), ganancia neta 0.0 dB en neutro, todo
// clampado, sin malloc/locks/I-O en process(), fail-closed (no finito → 0).
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <vector>

#include "fused/fused_controls.hpp"
#include "spatial/IntelligentUpmixer.hpp"
#include "spatial/HoaBinauralDecoder.hpp"
#include "HrtfManager.hpp"
#include "spatial/WfsRenderer.hpp"
#include "spatial/OrbeBinaural.hpp"
#include "spatial/poincare_binaural.hpp"
#include "spatial/CrosstalkCanceller.hpp"
#include "spatial/AuxFieldCoherence.hpp"
#include "spatial/StereoObjectDecomposer.hpp"
#include "spatial/ivanna_head_tracker.hpp"
#include "spatial/HeadTrackerFusion.hpp"
#include "spatial/AuxGeneziRoute.hpp"
#include "IvannaFusionCore.hpp"  // Ivanna::AudioBuffer, Ivanna::BLOCK_SIZE

namespace ivanna::fused {

// Pan Poincaré de campo cercano (2026-11-23): sustituye el spread WFS
// por el pan geodésico de Poincaré (cue exacto por nearFieldParallax),
// detrás de flag y OFF por defecto. NUNCA por defecto sin ABX en el
// teléfono (orden 8 de Luna). El flag es compile-time (patrón
// kEmergentToneEnabled en spatial/OmniField.hpp): el target
// test_poincare_pan compila con -DIVANNA_POINCARE_PAN_ENABLED=1 para
// ejercitar el camino ON; en producción y en el resto de la suite el
// flag es false y el camino WFS es el de siempre, bit-idéntico.
#ifndef IVANNA_POINCARE_PAN_ENABLED
#define IVANNA_POINCARE_PAN_ENABLED 0
#endif

class FusedSpatial {
public:
    // Pan Poincaré de campo cercano: false = spread WFS (comportamiento
    // actual); true = pan geodésico en lugar del WFS (solo en el target
    // de test dedicado, ver arriba). Nunca true en producción sin ABX.
    static constexpr bool kPoincarePanEnabled = (IVANNA_POINCARE_PAN_ENABLED != 0);

    FusedSpatial();
    ~FusedSpatial() = default;
    FusedSpatial(const FusedSpatial&) = delete;
    FusedSpatial& operator=(const FusedSpatial&) = delete;

    void prepare(double sampleRate) noexcept;
    void setControls(const SpatialControls& c) noexcept;
    SpatialControls controls() const noexcept { return controls_; }
    void process(double* left, double* right, int frames) noexcept;
    bool isNeutral() const noexcept { return controls_.isNeutral(); }
    void reset() noexcept;
    static const char* name() noexcept { return "ORBE·Ω"; }

    // Alimentación del sensor de cabeza (hilo de control): vector de
    // rotación Android (Sensor.TYPE_ROTATION_VECTOR, [x,y,z,w]) + timestamp
    // en ms. Sin ella el yaw fusionado es 0 (sin parallax inventado).
    void feedRotationVector(const float rv[4], float timestampMs) noexcept;
    // Yaw fusionado vigente, en grados (telemetría; 0 sin sensor).
    float trackerYawDeg() const noexcept;
    // Pitch / roll fusionados vigentes, en grados (SO(3) completo, v2.5.472).
    float trackerPitchDeg() const noexcept;
    float trackerRollDeg() const noexcept;
    // Sensores sueltos para la fusión gyro+RV (hilo de sensor). Pendiente
    // el alimentador Kotlin; el RV solo ya da yaw+pitch+roll completos.
    void feedGyro(const float gyro[3], float timestampMs) noexcept;
    void feedAccel(const float acc[3], float timestampMs) noexcept;
    void feedMagnetometer(const float mag[3], float timestampMs) noexcept;
    // Traslación del oyente (m) para el parallax por traslación (ARCore
    // donde llegue; sin ella, (0,0,0)).
    void setListenerTranslation(float xM, float yM, float zM) noexcept;
    void listenerTranslation(float out[3]) const noexcept;
    // Latencia de seguimiento (F0, v2.5.471): out[0]=end-to-end
    // sensor→bloque renderizado (ms), out[1]=sensor→JNI (ms), out[2]=yaw
    // aplicado (deg), out[3]=timestamp del sensor (ms BOOTTIME),
    // out[4]=timestamp aplicado (ms BOOTTIME). -1 = sin medición todavía.
    // Solo instrumentación: medir, no cambiar el camino de render.
    void trackingTelemetry(float out[5]) const noexcept;
    // ¿Hay dataset HRTF medido cargado? false = síntesis analítica.
    bool hasHrtfDataset() const noexcept { return hrtf_.hasDataset(); }
    // PARCHE v2.6 (Fallo 4 / punto 3): ¿queda alguna cola de fundido
    // activa? (ORBE 15 ms, XTC 20 ms, AuxField 20 ms, WFS). process()
    // solo retorna temprano (bit-transparente) si isNeutral() Y no hay
    // cola activa; si hay cola, processChunk() la drena hasta 0.
    bool hasActiveTail() const noexcept {
        return orbe_.isActive() || xtc_.isActive() || auxField_.isActive() ||
               wfsMix_ > 0.0f;
    }

private:
    static constexpr int kChunk = 128;  // == Ivanna::BLOCK_SIZE
    static constexpr float kHeadRadiusM = 0.0875f;  // KEMAR
    static constexpr float kWfsObjectY = 1.5f;      // distancia objetos WFS (m)
    static constexpr float kWfsMaxApertureM = 1.5f; // apertura a wfsSpread=1

    float effectiveOrbeAmount() const noexcept {
        // Solo bocina plana apaga ORBE: en GENEZI el amount del usuario
        // pasa (tercer modo, v2.5.483 / evol-holos).
        return controls_.mode == OmegaOutputMode::SPEAKERS ? 0.0f : controls_.amount;
    }
    float effectiveHrtfWet() const noexcept {
        // HRTF binaural PROHIBIDO en Genezi (TransducerProfile): en
        // SPEAKERS y en GENEZI el wet es 0; solo audífonos lo usan.
        return ivanna::omega_output_is_speakers_like(controls_.mode) ? 0.0f : controls_.hrtfWet;
    }
    bool isSpeakers() const noexcept {
        return controls_.mode == OmegaOutputMode::SPEAKERS;
    }
    // Altavoz a efectos de XTC/AuxField/pose: bocina plana o Genezi
    // (el trayecto Genezi lleva XTC + AuxField activos por diseño).
    bool isSpeakersLike() const noexcept {
        return ivanna::omega_output_is_speakers_like(controls_.mode);
    }

    void applyControls() noexcept;             // empuja controls_ a los motores
    void applyPose(bool force) noexcept;       // azimut + yaw tracker → ORBE/HRTF/XTC
    void updatePoincareScene() noexcept;       // azimut + yaw tracker + distancia → setScene (flag ON)
    // Una sola etapa binaural por trozo (Fallo 3d). HOA = upmixer + decoder
    // (un render completo); ORBE = campo cercano; HRTF = SOFA/síntesis;
    // WFS = 2 objetos a ±apertura. None = SPEAKERS o nada activo.
    enum class BinauralStage : std::uint8_t { None, Hoa, Orbe, Hrtf, Wfs };
    BinauralStage selectBinauralStage() const noexcept;
    void processChunk(double* l, double* r, int n) noexcept;  // n SIEMPRE == kChunk

    SpatialControls controls_{};
    double sampleRate_ = 48000.0;
    bool prepared_ = false;

    // Motores (orden de process()) — todos en double (F2/486). El XTC
    // ya era dual float/double por sobrecarga; el resto son las versiones
    // D de cada plantilla (el alias float queda para los llamadores viejos).
    Ivanna::IntelligentUpmixerD upmixer_;
    Ivanna::HoaBinauralDecoderD hoaDecoder_;
    Ivanna::HrtfManagerD hrtf_;
    ivanna::spatial::WfsRendererD wfs_;
    ivanna::OrbeBinauralD orbe_;
    ivanna::poincare::Operator poincare_;  // pan de campo cercano (flag ON; ver kPoincarePanEnabled)
    ivanna::spatial::CrosstalkCanceller xtc_;
    ivanna::dsp::AuxFieldCoherenceD auxField_;
    // Análisis de escena + tracking (envueltos en su etapa).
    ivanna::spatial::StereoObjectDecomposerD decomposer_;
    ivanna::spatial::HeadTracker headTracker_;
    ivanna::spatial::HeadTrackerFusion trackerFusion_;

    // Scratch pre-reservado (cero malloc en process()).
    std::vector<Ivanna::HoaVectorD> field_;  // campo HOA por trozo (128)
    // Acumulador de entrada (trozos exactos de 128, Fallo 3d): la entrada
    // se acumula aquí hasta completar 128; processChunk() NUNCA ve un
    // trozo parcial, así que el HRTF jamás se rellena con ceros.
    std::array<double, kChunk> inPendL_{};
    std::array<double, kChunk> inPendR_{};
    int inPendCount_ = 0;  // muestras pendientes en inPend_ (0..127)
    // Cola de salida del acumulador (anillo de 256: un trozo procesado
    // (128) más el retraso máximo pendiente (127) caben siempre).
    std::array<double, 2 * kChunk> outPendL_{};
    std::array<double, 2 * kChunk> outPendR_{};
    int outPendHead_ = 0;   // índice de lectura del anillo
    int outPendCount_ = 0;  // muestras en cola (0..255)
    std::array<double, kChunk> hoaL_{};
    std::array<double, kChunk> hoaR_{};
    std::array<double, kChunk> wfsL_{};
    std::array<double, kChunk> wfsR_{};
    // Scratch float del pan Poincaré (el operador es float nativo; la
    // cadena es double: ida y vuelta por trozo, solo con flag ON).
    std::array<float, kChunk> poinL_{};
    std::array<float, kChunk> poinR_{};
    std::array<double, kChunk> objBuf_[4]{};

    // Estado de rampas / cachés (evitan re-síntesis y clics).
    double wfsMix_ = 0.0;            // mezcla WFS vigente (0 seco .. 1 WFS)
    float lastAperture_ = -1.0f;     // última apertura WFS aplicada (m)
    // Escena Poincaré vigente (azimut efectivo + distancia): setScene
    // solo se llama cuando cambian (umbral 0.25° / 1 mm, como applyPose).
    float lastPoincareAzimuthDeg_ = 1.0e9f;
    float lastPoincareDistanceM_ = -1.0f;
    float lastPoseYawDeg_ = 1.0e9f;  // último yaw aplicado a ORBE/HRTF
    float lastPosePitchDeg_ = 1.0e9f;  // último pitch aplicado a ORBE/HRTF
    float lastPoseRollDeg_ = 1.0e9f;  // último roll aplicado a ORBE/HRTF
    float lastXtcYawDeg_ = 1.0e9f;   // último yaw aplicado al XTC
    // F0 (v2.5.471): timestamps de seguimiento (ms, base CLOCK_BOOTTIME,
    // igual que SensorEvent.timestamp). Atómicos: feedRotationVector corre
    // en el hilo del sensor y applyPose en el hilo de audio.
    std::atomic<float> poseSensorTsMs_{-1.0e9f};
    std::atomic<float> poseJniTsMs_{-1.0e9f};
    std::atomic<float> poseAppliedTsMs_{-1.0e9f};
    std::atomic<float> poseLatencyMs_{-1.0f};     // aplicado - sensor
    std::atomic<float> poseJniLatencyMs_{-1.0f};  // JNI - sensor
    std::atomic<float> poseAppliedYawDeg_{0.0f};
};

}  // namespace ivanna::fused
