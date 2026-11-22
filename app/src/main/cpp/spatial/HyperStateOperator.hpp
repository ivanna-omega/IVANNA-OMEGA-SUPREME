// =====================================================================
//  IVANNA — Hiper-Operador de Estado Φ (Fusión Homogénea) v2.5.369
//  HyperStateOperator.hpp — header-only, C++20/26, cero malloc en audio.
//
//  © 2026 Luis Uriel Pimentel Pérez · GORE TNS. All rights reserved.
//
//  ALCANCE: IVANNA es para CUALQUIER Android 13 en adelante. El Moto G85
//  es el dispositivo de desarrollo/prueba, no el único. Este operador no
//  tiene ningún supuesto específico de un modelo de teléfono hardcodeado:
//  geometrías genéricas de smartphone/sala/auto, sample rates 8–192 kHz,
//  y todo el sensado por micrófono estándar (AudioRecord) o por interfaz
//  (V/I, EMF, EEG) cuando el hardware la provea.
//
//  Qué es: UN solo operador de estado continuo, muestra a muestra, SIN
//  FFT por bloques y SIN STFT, que fusiona los tres vectores de estado
//  en una sola ganancia unificada. Es la evolución del operador HOLOS
//  (Y_b = G_b ⊛ [K_b·X_b]·Γ_b, por banda STFT) al dominio temporal:
//
//      Y(t) = x(t) + wet · [ Φ(s_neuro, s_emf, s_field) · x(t) − x(t) ]
//
//  con un solo wet (amount), una sola ganancia unificada y una sola
//  guarda de pico. amount=0 es identidad bit-exacta.
//
//  LOS TRES VECTORES (estado honesto, v2.5.369):
//    S_neuro(t) — NPE ON-DEVICE (modelo coclear + LIF corriendo en el
//      procesador del teléfono sobre el audio REAL) + interfaz EEG para
//      hardware real (setNeuroFrame, formato NeuroFrame de
//      neuro_acoustic_closed_loop.hpp). El NPE es un MODELO de la
//      cóclea, no una medición del cerebro: no se presenta como tal.
//      Sin frames EEG, el NPE es el estado neuro vivo. Con frames EEG
//      reales y frescos, el controlador de bucle cerrado añade trims
//      por banda a Γ. Sin NPE no hay modulación neuro (no se inventa).
//    Z_emf(t) — SOLO medición real: V/I reales (setMeasuredVI, observador
//      EmfEstimator Thiele-Small) o back-EMF real (setMeasuredEmf).
//      Sin fuente real, el término EMF es identidad: NINGÚN modelo
//      sustituye a la medición. En un teléfono Android NO hay vía
//      eléctrica (el Aux 3.5 mm es solo salida, sin retorno): el altavoz
//      se observa ACÚSTICAMENTE a través del micrófono (SensorFrame),
//      no eléctricamente. La interfaz queda para hardware con sensado
//      de corriente.
//    W_acústico(r,θ) — campo medido por el micrófono del teléfono
//      (RealAcousticSensor, otro componente v2.5.369): SensorFrame con
//      24 bandas ERB (magnitude_db, error_db, coherence, level_dbfs).
//      error_db gobierna la corrección Γ; coherence es el peso de
//      confianza por banda; magnitude_db es el campo medido.
//
//  DOS MODOS (explícitos en HyperStatus::mode, nunca confundidos):
//    MEDIDO (2): hay SensorFrame válido y fresco (< 500 ms). Γ viene
//      del error medido por el micrófono (corrección de sala/altavoz
//      real, ±9 dB por banda). XTC con wet ∝ coherencia media.
//    APROXIMADO (1): NO hay SensorFrame todavía (el sensor de micrófono
//      se está cableando; AudioRecord aún no alimenta frames). Γ es una
//      corrección APROXIMADA de balance espectral: mide el espectro de
//      la propia señal con el banco ERB (envolventes por banda, reales,
//      sobre audio real) y lo lleva hacia plano respecto al nivel de
//      banda ancha de la entrada, ±3 dB, lento (200 ms). NO es una
//      medición de sala: es un ecualizador adaptativo sobre la señal.
//      Suena real y es audible en Ruta A HOY; cuando lleguen frames del
//      micrófono, el modo MEDIDO lo reemplaza automáticamente. XTC a wet
//      completo (la geometría es declarada, no medida).
//    Apagado (0): amount=0 o sin preparar → identidad bit-exacta.
//
//  FAIL-CLOSED (lo que NUNCA pasa):
//    - Sin amount (>0) no hay proceso: identidad bit-exacta.
//    - Frames inválidos (valid=false, NaN, fuera de rango, duplicados)
//      se rechazan sin tocar el estado.
//    - Datos no finitos en la entrada se ponen a 0; la salida nunca
//      es NaN/Inf (si lo fuera, se devuelve la entrada).
//    - El pico de salida NUNCA supera el pico de la entrada (guarda de
//      pico contra el pico corriente de la entrada).
//    - La ganancia global NUNCA sube: todas las modulaciones son
//      atenuación o mezcla wet/dry; el limiter NO se toca (el operador
//      corre ANTES del compresor/limitador de la cadena).
//    - Volterra solo con coeficientes MEDIDOS (puerta: setVolterraCoefficients
//      valida finitud y |c| ≤ 8; sin coeficientes, identidad).
//
//  TIEMPO REAL: prepare() asigna (hilo de control); process() no asigna,
//  no bloquea, no hace I/O. Los frames llegan por staging + flag atómico
//  (hilo del sensor → hilo de audio, sin locks). El controlador EEG se
//  actualiza 1× cada 128 muestras con reloj monotónico real.
// =====================================================================
#pragma once

#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../neuromorphic/biquad_envelope_bank.hpp"
#include "../neuromorphic/diagonal_volterra.hpp"
#include "../neuromorphic/lif_neuron_pool.hpp"
#include "../neuromorphic/neuro_acoustic_closed_loop.hpp"
#include "AuxGeneziRoute.hpp"
#include "CrosstalkCanceller.hpp"
#include "EliteTargetCurves.hpp"
#include "EmfEstimator.hpp"
#include "RealAcousticSensor.hpp"

namespace ivanna::spatial {

// ── Preset de ruta (geometría del par de altavoces para XTC) ─────────────
//  PhoneSpeaker: altavoces estéreo del propio teléfono (span ~0.15 m,
//    oyente a ~0.30 m; valores típicos de un smartphone, no de un modelo
//    concreto). APROXIMADO (la distancia depende de cómo se sostiene el
//    teléfono), documentado como tal.
//  GeneziRoom / CarCabin: presets de AuxGeneziRoute (v2.5.366).
enum class HyperRoutePreset : int {
    PhoneSpeaker = 0,
    GeneziRoom = 1,
    CarCabin = 2,
};

// ── Modo del operador (visible en status; nunca confundir) ───────────────
enum class HyperMode : int {
    Off = 0,          // amount=0 o sin preparar
    Approximate = 1,  // sin SensorFrame: Γ aproximada por balance espectral
    Measured = 2,     // SensorFrame válido y fresco: Γ por error medido
    Calibrated = 3,   // v2.5.451: perfil MEDIDO guardado (barrido de sala) aplicado
                      // cuando el DAC USB está conectado y no hay micrófono vivo.
                      // Γ por error guardado, coherencia 1.0 (medición de
                      // confianza). Precede a Measured: el perfil es la versión
                      // limpia y verificada de la misma medición que el mic
                      // haría en vivo, sin ruido ni latencia.
};

struct HyperParams {
    float amount = 0.0f;  // 0..1 (0 = apagado, identidad bit-exacta)
    HyperRoutePreset preset = HyperRoutePreset::PhoneSpeaker;
    bool xtcEnabled = true;  // XTC: solo altavoces; apagar en auriculares
    float maxCorrectionDb = 9.0f;  // tope por banda de Γ medida [1..12]
    std::uint32_t volterraKernelLength = 64;  // 1..4096 (64 = viable en RT)
};

struct HyperStatus {
    bool enabled = false;
    HyperMode mode = HyperMode::Off;
    bool sensorFresh = false;   // SensorFrame válido y fresco (< 500 ms)
    bool neuroActive = false;   // EEG fresco y activo (extensión futura)
    float neuroWet = 0.0f;      // wet del bucle EEG [0..1]
    bool emfFresh = false;      // V/I o EMF real fresco (< 100 ms)
    float meanCoherence = 0.0f;
    float spikeRate = 0.0f;     // NPE on-device: tasa de disparo LIF [0..1]
    std::uint64_t framesReceived = 0;
    float levelDbfs = -120.0f;  // último nivel medido por el micrófono
    float emfVelocityMps = 0.0f;
    float emfDisplacementMm = 0.0f;
};

class HyperStateOperator {
public:
    static constexpr int kBands = 24;  // bandas ERB (mismas que HOLOS/sensor)
    static constexpr std::uint32_t kMaxKernelLength = 4096;

    void prepare(float sampleRate) noexcept {
        sampleRate_ = (std::isfinite(sampleRate) && sampleRate >= 8000.0f &&
                       sampleRate <= 192000.0f)
                          ? sampleRate
                          : 48000.0f;
        const float sr = sampleRate_;
        const double srD = static_cast<double>(sr);
        // Bandas ERB (idénticas a HOLOS y al sensor: 60 Hz–16 kHz).
        const double fTop = std::min(16000.0, 0.45 * srD);
        const double rLo = sensorErbRate(60.0f);
        const double rHi = sensorErbRate(static_cast<float>(fTop));
        double edgeHz[kBands + 1];
        for (int i = 0; i <= kBands; ++i)
            edgeHz[i] = sensorErbInverse(static_cast<float>(rLo + (rHi - rLo) * static_cast<double>(i) / static_cast<double>(kBands)));
        for (int b = 0; b < kBands; ++b) {
            const double lo = edgeHz[b], hi = edgeHz[b + 1];
            bandCenter_[b] = std::sqrt(lo * hi);
            const double q = bandCenter_[b] / erbWidth(bandCenter_[b]);
            const double qc = std::clamp(q, 0.7, 10.0);
            gammaL_[b].set_bandpass(bandCenter_[b], srD, qc);
            gammaR_[b].set_bandpass(bandCenter_[b], sr, qc);
            // BandBiquad::set_bandpass es la variante RBJ de "skirt gain
            // constante": su ganancia de pico es Q, no 1. El banco Γ
            // necesita ganancia unidad en el centro (si no, el término
            // (g−1)·BP se amplifica Q veces y la corrección explota).
            // Se normaliza por 1/Q al leer el tick (también para las
            // envolventes, que si no medirían Q veces el nivel real).
            invQ_[b] = 1.0 / qc;
            gammaGain_[b] = 1.0;
            gammaTarget_[b] = 1.0;
            gammaEnv_[b] = 0.0;
            // Target por defecto: Harman AE/OE 2018 (EliteTargetCurves).
            // El modo APROXIMADO doma el espectro hacia esta curva; el
            // modo MEDIDO usa el target del sensor (misma curva).
            // Se construye tras el bucle (buildTargetCurve).
            targetDb_[b] = 0.0f;
            targetStaged_[b] = 0.0f;
            // Banda Greenwood más cercana (para los trims EEG, 8 bandas).
            greenwoodIdx_[b] = nearestGreenwood(bandCenter_[b]);
        }
        // Curva objetivo por defecto (Harman 2018 + bass trim): se aplica
        // directamente aquí (prepare corre fuera del hilo de audio).
        buildTargetCurve(targetDb_);
        for (int b = 0; b < kBands; ++b) targetStaged_[b] = targetDb_[b];
        // Constantes de tiempo.
        wetStep_ = 1.0 / (0.005 * srD);                    // 5 ms
        peakDecay_ = std::exp(-1.0 / (0.100 * srD));       // 100 ms
        spikeAlpha_ = 1.0 - std::exp(-1.0 / (0.050 * srD));  // 50 ms
        gammaSlew_ = 1.0 - std::exp(-1.0 / (0.050 * srD));   // 50 ms
        envAtt_ = 1.0 - std::exp(-1.0 / (0.010 * srD));      // 10 ms
        envRel_ = 1.0 - std::exp(-1.0 / (0.200 * srD));      // 200 ms
        rmsAlpha_ = 1.0 - std::exp(-1.0 / (0.100 * srD));    // 100 ms
        sensorStaleSamples_ = static_cast<std::uint64_t>(0.5f * sr);  // 500 ms
        emfStaleSamples_ = static_cast<std::uint64_t>(0.1f * sr);     // 100 ms

        envBank_.init(static_cast<std::uint32_t>(sr));
        ivannanpe::LIFParams lp{};
        lp.sample_rate = sr;
        lif_.reset(lp);
        neuroCtrl_.prepare(sr, 128);
        kernelLength_ = std::clamp(params_.volterraKernelLength, 1u, kMaxKernelLength);
        volterra_.prepare(sr, kernelLength_);
        volterraBypass_ = volterra_.isBypass();
        baseH1_.assign(kernelLength_, 0.0);
        baseH2d_.assign(kernelLength_, 0.0);
        scaledH2d_.assign(kernelLength_, 0.0);
        stageH1_.assign(kernelLength_, 0.0);
        stageH2d_.assign(kernelLength_, 0.0);
        if (!baseH1_.empty()) baseH1_[0] = 1.0;
        emf_.prepare(sr, emfParams_);
        xtc_.prepare(sr, 2048);
        // Tope de realce mono del XTC a +1 dB (el default del cancelador
        // es +4 dB): con contenido centrado/mono, el realce del shuffler
        // taparía la corrección Γ y empujaría el pico contra la guarda
        // (clipping). Con +1 dB el XTC sigue separando en estéreo sin
        // pelearse con Γ. Medido en test (1 kHz mono: sin tope, el corte
        // de −6 dB quedaba enmascarado por el realce).
        xtc_.setMaxMonoBoostDb(1.0f);
        applyPresetGeometry();
        reset();
        prepared_ = true;
    }

    void reset() noexcept {
        for (int b = 0; b < kBands; ++b) {
            gammaL_[b].reset();
            gammaR_[b].reset();
            gammaGain_[b] = 1.0;
            gammaTarget_[b] = 1.0;
            gammaEnv_[b] = 0.0;
            eegTrim24_[b] = 0.0;
        }
        envBank_.reset();
        ivannanpe::LIFParams lp{};
        lp.sample_rate = sampleRate_;
        lif_.reset(lp);
        neuroCtrl_.reset();
        lastNeuroAdj_ = ivanna::neuromorphic::NeuroAdjustments{};
        neuroActive_ = false;
        volterra_.reset();
        emf_.reset();
        xtc_.reset();
        wet_ = 0.0;
        peakIn_ = 0.0;
        rmsEnv_ = 0.0;
        spikeRate_ = 0.0;
        prevXL_ = 0.0;
        prevXR_ = 0.0;
        emfVel_ = 0.0;
        emfDisp_ = 0.0;
        emfFresh_ = false;
        lastEmfDriveApplied_ = -1.0;
        // El frame del sensor NO se borra: es estado vivo; su frescura
        // se evalúa por contador de muestras (tras reset, expira solo).
    }

    // ── Parámetros (hilo de control; amount/preset/xtc son atómicos) ────
    void setAmount(float amount) noexcept {
        amount_.store(std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f,
                      std::memory_order_relaxed);
    }
    void setRoutePreset(HyperRoutePreset preset) noexcept {
        preset_.store(static_cast<int>(preset), std::memory_order_relaxed);
        presetDirty_.store(true, std::memory_order_relaxed);
    }
    void setXtcEnabled(bool on) noexcept {
        xtcEnabled_.store(on, std::memory_order_relaxed);
        xtc_.setEngage(on);
    }
    void setParams(const HyperParams& p) noexcept {
        setAmount(p.amount);
        setRoutePreset(p.preset);
        setXtcEnabled(p.xtcEnabled);
        if (std::isfinite(p.maxCorrectionDb))
            maxCorrectionDb_.store(std::clamp(p.maxCorrectionDb, 1.0f, 12.0f),
                                   std::memory_order_relaxed);
        // volterraKernelLength se guarda y se aplica en el próximo
        // prepare() (realoca las líneas de retardo).
        if (p.volterraKernelLength >= 1 && p.volterraKernelLength <= kMaxKernelLength)
            params_.volterraKernelLength = p.volterraKernelLength;
    }
    [[nodiscard]] float amount() const noexcept {
        return amount_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] HyperRoutePreset preset() const noexcept {
        return static_cast<HyperRoutePreset>(
            std::clamp(preset_.load(std::memory_order_relaxed), 0, 2));
    }

    // ── Vector W: SensorFrame del micrófono (hilo del sensor) ────────────
    // Valida y pone el frame en staging; el hilo de audio lo recoge sin
    // locks. Devuelve false si se rechaza (no toca nada).
    bool setSensorFrame(const SensorFrame& f) noexcept {
        if (!f.valid) return false;
        if (!std::isfinite(f.level_dbfs) || f.level_dbfs < -120.0f ||
            f.level_dbfs > 10.0f)
            return false;
        for (int b = 0; b < SensorFrame::kBands; ++b) {
            const float m = f.magnitude_db[b];
            const float e = f.error_db[b];
            const float c = f.coherence[b];
            if (!std::isfinite(m) || !std::isfinite(e) || !std::isfinite(c))
                return false;
            if (m < -120.0f || m > 30.0f) return false;
            if (e < -60.0f || e > 60.0f) return false;
            if (c < 0.0f || c > 1.0f) return false;
        }
        if (hasFrame_ && f.frame_index == lastFrameIndex_) return false;  // duplicado
        stageFrame_ = f;
        framePending_.store(true, std::memory_order_release);
        return true;
    }

    // ── Perfil CALIBRADO (v2.5.451): error por banda MEDIDO y guardado ────
    // El DAC USB no tiene micrófono: cuando está conectado, Φ usa el perfil
    // de la medición de sala (barrido, RoomMeasurement) en vez de frames
    // vivos. El perfil se pone en staging (igual que SensorFrame) y el hilo
    // de audio lo aplica sin locks. Devuelve false si se rechaza.
    // errorDb[b] = (magnitud medida − target) por banda ERB, en dB; Γ = −error.
    bool setCalibratedProfile(const float errDb[kBands]) noexcept {
        for (int b = 0; b < kBands; ++b) {
            const float e = errDb[b];
            if (!std::isfinite(e) || e < -60.0f || e > 60.0f) return false;
        }
        for (int b = 0; b < kBands; ++b) calibStage_[b] = errDb[b];
        calibWantActive_.store(true, std::memory_order_relaxed);
        calibPending_.store(true, std::memory_order_release);
        return true;
    }

    // Quita el perfil calibrado: Φ vuelve a MEDIDO (si hay frame fresco) o
    // a APROXIMADO. Se llama al desconectar el DAC.
    void clearCalibratedProfile() noexcept {
        calibWantActive_.store(false, std::memory_order_relaxed);
        calibPending_.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool calibratedActive() const noexcept { return calibActive_; }

    // ── Vector S_neuro (extensión EEG): frame REAL de sensor ─────────────
    // Formato NeuroFrame (neuro_acoustic_closed_loop.hpp): kind 2=EEG en
    // auricular, 3=ABR, 4=fNIRS; response/coherence[8] en [0..1] por banda
    // Greenwood; quality [0..1]; timestampUs en CLOCK_MONOTONIC (µs).
    // Delega en ClosedLoopController::ingest (valida y rechaza lo demás).
    // Sin frames, el NPE on-device es el estado neuro vivo.
    bool setNeuroFrame(const ivanna::neuromorphic::NeuroFrame& frame) noexcept {
        return neuroCtrl_.ingest(frame);
    }

    // ── Vector Z_emf: medición REAL (hilo de medición / nativo) ──────────
    // V/I reales en bornes del altavoz (amperio con sensado de corriente
    // o interfaz USB con shunt). El observador EmfEstimator corre en el
    // hilo de audio con estos valores. Sin fuente real, identidad.
    void setMeasuredVI(float voltage, float current) noexcept {
        if (!std::isfinite(voltage) || !std::isfinite(current)) return;
        emfV_.store(std::clamp(voltage, -100.0f, 100.0f), std::memory_order_relaxed);
        emfI_.store(std::clamp(current, -100.0f, 100.0f), std::memory_order_relaxed);
        emfViPending_.store(true, std::memory_order_release);
    }
    // setTargetDb: curva objetivo ÉLITE por banda (dB, relativa a
    // plano). 24 valores, uno por banda ERB (mismos centros que
    // bandCenterHz). Es la referencia de corrección del operador para el
    // modo APROXIMADO (sin micrófono): la corrección doma el espectro
    // hacia refDb + targetDb_b en vez de hacia plano. Ejemplos: la curva
    // objetivo Harman (grave elevado, agudo en pendiente) o la firma
    // electroacústica medida de un altavoz/auricular insignia.
    //   REGLA DE PROPIETARIO ÚNICO: en modo MEDIDO el target vive en el
    // SENSOR (RealAcousticSensor::setTarget / nativeSensorSetTarget) y
    // su error_db YA lo incluye — el operador NO lo reaplica ahí (si no,
    // la corrección se duplicaría). Este target solo gobierna el modo
    // APROXIMADO. Default: todo 0 (plano).
    // Valores no finitos se ignoran; fuera de ±12 dB se recortan.
    // El staging es por bandera atómica (hilo de control → audio).
    void setTargetDb(const float* targetDb24) noexcept {
        if (targetDb24 == nullptr) return;
        for (int b = 0; b < kBands; ++b) {
            const float t = targetDb24[b];
            if (std::isfinite(t)) {
                targetStaged_[b] = std::clamp(t, -12.0f, 12.0f);
            }
        }
        targetPending_.store(true, std::memory_order_release);
    }
    float targetDb(int band) const noexcept {
        if (band < 0 || band >= kBands) return 0.0f;
        return targetDb_[band];
    }

    // setEliteTarget: elige la curva objetivo por preset + control de
    // graves. kind: 0 Harman 2018 (default), 1 Harman élite-lean, 2 campo
    // difuso H&M, 3 altavoces en sala. bassTrimDb: ±3 dB (estante suave
    // <100 Hz, preferencia individual de graves, Olive 2025). Construye
    // la curva de 24 bandas y la aplica vía setTargetDb (staging).
    // El JNI (nativeSetHyperTarget) llama a esto Y aplica la misma curva
    // al sensor, para que ambos modos corrijan hacia la misma referencia.
    void setEliteTarget(EliteTargetKind kind, float bassTrimDb) noexcept {
        const int k = static_cast<int>(kind);
        targetKind_ = (k >= 0 && k < kEliteTargetKindCount)
                          ? kind
                          : EliteTargetKind::Harman2018;
        bassTrimDb_ = std::isfinite(bassTrimDb)
                          ? std::clamp(bassTrimDb, -3.0f, 3.0f)
                          : 0.0f;
        float curve[kBands];
        buildTargetCurve(curve);
        setTargetDb(curve);
    }
    void setBassTrimDb(float bassTrimDb) noexcept {
        setEliteTarget(targetKind_, bassTrimDb);
    }
    EliteTargetKind targetKind() const noexcept { return targetKind_; }
    float bassTrimDb() const noexcept { return bassTrimDb_; }

    // Back-EMF medida directamente (voltios). v = emf / Bl.
    void setMeasuredEmf(float emfVolts) noexcept {
        if (!std::isfinite(emfVolts)) return;
        emfDirect_.store(std::clamp(emfVolts, -100.0f, 100.0f),
                         std::memory_order_relaxed);
        emfDirectPending_.store(true, std::memory_order_release);
    }

    // ── Volterra: coeficientes MEDIDOS (hilo de control) ──────────────────
    // Solo se aceptan coeficientes medidos (protocolo GENEZY). Sin ellos,
    // el motor es identidad. La carga real ocurre en el hilo de audio
    // (staging) para no tocar los vectores del motor a mitad de bloque.
    // Núcleo double (F2/486): el staging de coeficientes es double.
    bool setVolterraCoefficients(const double* h1, const double* h2d,
                                 std::uint32_t length) noexcept {
        if (!prepared_ || h1 == nullptr || h2d == nullptr) return false;
        if (length != kernelLength_) return false;
        for (std::uint32_t k = 0; k < length; ++k) {
            if (!std::isfinite(h1[k]) || !std::isfinite(h2d[k])) return false;
            if (std::fabs(h1[k]) > static_cast<double>(kMaxDiagonalCoeff) ||
                std::fabs(h2d[k]) > static_cast<double>(kMaxDiagonalCoeff))
                return false;
        }
        for (std::uint32_t k = 0; k < length; ++k) {
            stageH1_[k] = h1[k];
            stageH2d_[k] = h2d[k];
        }
        stageLen_.store(length, std::memory_order_relaxed);
        stagePending_.store(true, std::memory_order_release);
        return true;
    }

    // Envoltorio float (compat legacy): cast exacto a double.
    bool setVolterraCoefficients(const float* h1, const float* h2d,
                                 std::uint32_t length) noexcept {
        if (!prepared_ || h1 == nullptr || h2d == nullptr) return false;
        if (length != kernelLength_) return false;
        if (wrapH1_.size() < length) { wrapH1_.resize(length); wrapH2_.resize(length); }
        for (std::uint32_t k = 0; k < length; ++k) {
            wrapH1_[k] = static_cast<double>(h1[k]);
            wrapH2_[k] = static_cast<double>(h2d[k]);
        }
        return setVolterraCoefficients(wrapH1_.data(), wrapH2_.data(), length);
    }

    [[nodiscard]] bool isVolterraBypass() const noexcept { return volterraBypass_; }
    [[nodiscard]] float bandCenterHz(int b) const noexcept {
        if (b < 0 || b >= kBands) return 0.0f;
        return static_cast<float>(bandCenter_[b]);
    }
    // Ganancia Γ aplicada ahora en la banda b (dB, telemetría).
    [[nodiscard]] float gammaGainDb(int b) const noexcept {
        if (b < 0 || b >= kBands) return 0.0f;
        const double g = gammaGain_[b];
        return (g > 0.0) ? static_cast<float>(20.0 * std::log10(g)) : -120.0f;
    }

    [[nodiscard]] HyperStatus status() const noexcept {
        HyperStatus st{};
        st.enabled = prepared_ && amount() > 0.0f;
        // v2.5.451: Calibrated precede a Measured — el perfil guardado es la
        // versión limpia y verificada de la medición que el mic haría en vivo.
        st.mode = !st.enabled ? HyperMode::Off
                  : calibActive_ ? HyperMode::Calibrated
                  : sensorFresh_ ? HyperMode::Measured
                                 : HyperMode::Approximate;
        st.sensorFresh = sensorFresh_;
        st.neuroActive = neuroActive_;
        st.neuroWet = lastNeuroAdj_.wetScale;
        st.emfFresh = emfFresh_;
        // En Calibrated la coherencia es 1.0 por definición: el perfil es una
        // medición guardada de confianza, no una estimación viva ruidosa.
        st.meanCoherence = (st.mode == HyperMode::Calibrated)
                               ? 1.0f
                               : meanCoherence_.load(std::memory_order_relaxed);
        st.spikeRate = static_cast<float>(spikeRate_);
        st.framesReceived = framesReceived_.load(std::memory_order_relaxed);
        st.levelDbfs = hasFrame_ ? activeFrame_.level_dbfs : -120.0f;
        st.emfVelocityMps = static_cast<float>(emfVel_);
        st.emfDisplacementMm = static_cast<float>(emfDisp_ * 1000.0);
        return st;
    }

    // ── Procesado muestra a muestra (hilo de audio) ───────────────────────
    // Núcleo double (F2/486): el plano de señal (Γ, Volterra, XTC, guarda
    // de pico) corre en 64-bit. El NPE (envBank/LIF) y el controlador EEG
    // son plano de control y quedan float, como el clasificador TinyML.
    void process(double& L, double& R) noexcept {
        const double xL = sanitize(L);
        const double xR = sanitize(R);
        const double inst = std::max(std::fabs(xL), std::fabs(xR));
        peakIn_ = (inst > peakIn_) ? inst : peakIn_ * peakDecay_;
        rmsEnv_ += rmsAlpha_ * (0.5 * (std::fabs(xL) + std::fabs(xR)) - rmsEnv_);
        ++sampleCounter_;
        if (!prepared_) {
            L = xL;
            R = xR;
            return;
        }
        if (framePending_.load(std::memory_order_acquire)) applyStagedFrame();
        if (calibPending_.load(std::memory_order_acquire)) applyStagedCalib();
        applyStagedEmf();
        sensorFresh_ = hasFrame_ &&
                       (sampleCounter_ - lastFrameSample_) <= sensorStaleSamples_;
        const bool emfFreshNow =
            emfFresh_ && (sampleCounter_ - emfLastSample_) <= emfStaleSamples_;
        if (!emfFreshNow) {
            emfVel_ = 0.0f;
            emfDisp_ = 0.0f;
        }
        const double wetTarget = amount_.load(std::memory_order_relaxed);
        wet_ += std::clamp(wetTarget - wet_, -wetStep_, wetStep_);
        if (wet_ <= 0.0) {
            L = xL;
            R = xR;
            return;  // identidad bit-exacta (entrada finita)
        }
        if ((sampleCounter_ & 127u) == 0u) blockUpdate(emfFreshNow);

        // ── (a) S_neuro: NPE on-device sobre el audio REAL ──────────────
        // Envolventes cocleares (8 bandas) + LIF (32 neuronas). El mismo
        // estado que corre NpeStereoCore, sin su cadena de ganancia/AGC
        // (esa cadena subiría el nivel; aquí el NPE es ESTADO, no proceso).
        const ivanna::PerceptualCues cues =
            envBank_.extract(static_cast<float>(xL), static_cast<float>(xR));
        std::array<float, 32> currents{};
        for (int b = 0; b < ivanna::BEB_BANDS; ++b) {
            const float iL = envBank_.envL[b] * 0.05f;
            const float iR = envBank_.envR[b] * 0.05f;
            currents[static_cast<std::size_t>(b * 4 + 0)] = iL;
            currents[static_cast<std::size_t>(b * 4 + 1)] = iR;
            currents[static_cast<std::size_t>(b * 4 + 2)] = iL * 0.1f;
            currents[static_cast<std::size_t>(b * 4 + 3)] = iR * 0.1f;
        }
        const int spikes = lif_.tick(currents);
        spikeRate_ += spikeAlpha_ * ((static_cast<double>(spikes) / 32.0) - spikeRate_);
        const float modelDrive = std::clamp(
            0.6f * static_cast<float>(spikeRate_) + 0.4f * std::clamp(cues.T, 0.0f, 1.0f), 0.0f, 1.0f);

        // ── (c/d) Γ: banco ERB paralelo (identidad exacta si g_b = 1) ────
        // y = x + Σ (g_b − 1)·BP_b(x). Las envolventes miden el espectro
        // de la ENTRADA (feedforward: sin realimentación, estable).
        double yL = xL, yR = xR;
        for (int b = 0; b < kBands; ++b) {
            gammaGain_[b] += gammaSlew_ * (gammaTarget_[b] - gammaGain_[b]);
            const double bpL = gammaL_[b].tick(xL) * invQ_[b];
            const double bpR = gammaR_[b].tick(xR) * invQ_[b];
            const double envIn = 0.5 * (std::fabs(bpL) + std::fabs(bpR));
            gammaEnv_[b] += ((envIn > gammaEnv_[b]) ? envAtt_ : envRel_) *
                            (envIn - gammaEnv_[b]);
            const double g = gammaGain_[b];
            if (g != 1.0) {
                yL += (g - 1.0) * bpL;
                yR += (g - 1.0) * bpR;
            }
        }

        // ── (c) Volterra diagonal (solo kernel medido; identidad si no) ──
        if (!volterraBypass_) {
            volterra_.process(&yL, &yR, 1);
        }

        // ── (d) XTC: cancelación de diafonía (geometría del preset) ──────
        // En modo MEDIDO, wet ∝ coherencia media (confianza). En modo
        // APROXIMADO, wet completo (la geometría es declarada, no medida).
        if (xtcEnabled_.load(std::memory_order_relaxed)) {
            double xl = yL, xr = yR;
            xtc_.processBlock(&xl, &xr, 1);
            // v2.5.451: en CALIBRADO el wet es completo (perfil de confianza,
            // coherencia 1.0); en MEDIDO escala con la coherencia viva.
            const double xtcWet =
                wet_ * ((sensorFresh_ && !calibActive_)
                            ? static_cast<double>(meanCoherence_.load(std::memory_order_relaxed))
                            : 1.0);
            yL += xtcWet * (xl - yL);
            yR += xtcWet * (xr - yR);
        }

        // ── (e) Ganancia neuro-acoplada (NPE): ancho + micro-fase ────────
        // Ancho M/S que respira con el drive neural (±10%); micro-fase
        // por primera diferencia (tilt sutil); atenuación diminuta.
        // NUNCA sube el nivel: w se compensa con la guarda de pico.
        const double neuroDepth = wet_ * static_cast<double>(modelDrive);
        {
            const double mid = 0.5 * (yL + yR);
            const double side = 0.5 * (yL - yR);
            const double w = 1.0 + 0.10 * (static_cast<double>(modelDrive) - 0.5) * 2.0 * wet_;
            yL = mid + side * w;
            yR = mid - side * w;
        }
        yL += 0.03 * neuroDepth * (xL - prevXL_);
        yR += 0.03 * neuroDepth * (xR - prevXR_);
        prevXL_ = xL;
        prevXR_ = xR;

        // ── (b) Z_emf: protección de excursión (solo atenuación) ─────────
        const double emfDrive =
            emfFreshNow ? std::clamp(std::fabs(emfDisp_) / static_cast<double>(kEmfXMaxM), 0.0, 1.0)
                        : 0.0;
        const double gEmf = 1.0 - 0.15 * wet_ * emfDrive;
        yL *= gEmf;
        yR *= gEmf;
        const double gNeuro = 1.0 - 0.02 * neuroDepth;
        yL *= gNeuro;
        yR *= gNeuro;

        // ── (f) Guarda de pico + ganancia unificada ───────────────────────
        // La guarda garantiza: |Y| ≤ pico de la entrada (siempre).
        if (peakIn_ > 0.0) {
            yL = std::clamp(yL, -peakIn_, peakIn_);
            yR = std::clamp(yR, -peakIn_, peakIn_);
        }
        L = xL + wet_ * (yL - xL);
        R = xR + wet_ * (yR - xR);
        if (!std::isfinite(L)) L = xL;
        if (!std::isfinite(R)) R = xR;
    }

    // Envoltorio float (compat legacy / JNI Ruta A): delega en el núcleo double.
    void process(float& L, float& R) noexcept {
        double l = std::isfinite(L) ? static_cast<double>(L) : 0.0;
        double r = std::isfinite(R) ? static_cast<double>(R) : 0.0;
        process(l, r);
        L = std::isfinite(l) ? static_cast<float>(l) : 0.0f;
        R = std::isfinite(r) ? static_cast<float>(r) : 0.0f;
    }

    void processBlock(double* L, double* R, int n) noexcept {
        if (L == nullptr || R == nullptr || n <= 0) return;
        for (int i = 0; i < n; ++i) process(L[i], R[i]);
    }

    // Envoltorio float (compat legacy / JNI Ruta A): convierte por scratch
    // y delega en el núcleo double.
    void processBlock(float* L, float* R, int n) noexcept {
        if (L == nullptr || R == nullptr || n <= 0) return;
        const std::size_t un = static_cast<std::size_t>(n);
        if (wrapL_.size() < un) { wrapL_.resize(un); wrapR_.resize(un); }
        for (int i = 0; i < n; ++i) {
            wrapL_[i] = std::isfinite(L[i]) ? static_cast<double>(L[i]) : 0.0;
            wrapR_[i] = std::isfinite(R[i]) ? static_cast<double>(R[i]) : 0.0;
        }
        processBlock(wrapL_.data(), wrapR_.data(), n);
        for (int i = 0; i < n; ++i) {
            L[i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
            R[i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
        }
    }

private:
    static constexpr float kEmfXMaxM = 0.002f;  // 2 mm de excursión de ref.
    static constexpr float kMaxDiagonalCoeff = 8.0f;

    static inline float sanitize(float v) noexcept {
        return std::isfinite(v) ? v : 0.0f;
    }
    static inline double sanitize(double v) noexcept {
        return std::isfinite(v) ? v : 0.0;
    }
    static inline double erbWidth(double fHz) noexcept {
        return 24.7 * (4.37 * fHz / 1000.0 + 1.0);
    }
    static int nearestGreenwood(float fHz) noexcept {
        int best = 0;
        float bestD = 1e30f;
        const float lf = std::log(fHz);
        for (int b = 0; b < ivanna::neuromorphic::kNeuroBands; ++b) {
            const float d =
                std::fabs(lf - std::log(ivanna::neuromorphic::kNeuroCenterHz[b]));
            if (d < bestD) {
                bestD = d;
                best = b;
            }
        }
        return best;
    }

    void applyPresetGeometry() noexcept {
        ivanna::spatial::AuxRouteParams rp{};
        switch (preset()) {
            case HyperRoutePreset::CarCabin:
                rp = auxPreset(AuxPresetKind::CarCabin);
                break;
            case HyperRoutePreset::GeneziRoom:
                rp = auxPreset(AuxPresetKind::GeneziRoom);
                break;
            case HyperRoutePreset::PhoneSpeaker:
            default: {
                rp.xtc.spanMeters = 0.15f;     // altavoces del teléfono
                rp.xtc.distanceMeters = 0.30f;  // teléfono en la mano
                rp.xtc.listenerOffsetM = 0.0f;
                rp.auxXoverHz = 120.0f;
                rp.auxCoherence = 1.0f;
                break;
            }
        }
        xtc_.setGeometry(rp.xtc);
    }

    // Hilo de audio: recoge el frame puesto en staging por el sensor.
    void applyStagedFrame() noexcept {
        framePending_.store(false, std::memory_order_release);
        activeFrame_ = stageFrame_;
        hasFrame_ = true;
        lastFrameIndex_ = activeFrame_.frame_index;
        lastFrameSample_ = sampleCounter_;
        framesReceived_.fetch_add(1, std::memory_order_relaxed);
        float cohSum = 0.0f;
        for (int b = 0; b < SensorFrame::kBands; ++b)
            cohSum += activeFrame_.coherence[b];
        meanCoherence_.store(cohSum / (float)SensorFrame::kBands,
                             std::memory_order_relaxed);
    }

    // Hilo de audio: consume el perfil calibrado puesto en staging (v2.5.451).
    // Se aplica en el mismo punto que el frame del sensor, sin locks.
    void applyStagedCalib() noexcept {
        calibPending_.store(false, std::memory_order_release);
        for (int b = 0; b < kBands; ++b) calibErrorDb_[b] = calibStage_[b];
        calibActive_ = calibWantActive_.load(std::memory_order_relaxed);
    }

    // Hilo de audio: consume V/I o EMF real puestos en staging.
    void applyStagedEmf() noexcept {
        if (emfViPending_.load(std::memory_order_acquire)) {
            emfViPending_.store(false, std::memory_order_release);
            const double v = emfV_.load(std::memory_order_relaxed);
            const double i = emfI_.load(std::memory_order_relaxed);
            double vel = 0.0, disp = 0.0;
            emf_.process(&v, &i, 1, &vel, &disp);
            emfVel_ = vel;
            emfDisp_ = disp;
            emfFresh_ = true;
            emfLastSample_ = sampleCounter_;
        }
        if (emfDirectPending_.load(std::memory_order_acquire)) {
            emfDirectPending_.store(false, std::memory_order_release);
            const double emf = emfDirect_.load(std::memory_order_relaxed);
            const double bl = static_cast<double>(emf_.params().Bl);
            emfVel_ = (bl > 0.0) ? emf / bl : 0.0;
            emfDisp_ = static_cast<double>(emf_.params().leak) * emfDisp_ +
                       emfVel_ / static_cast<double>(sampleRate_);
            if (!std::isfinite(emfDisp_)) emfDisp_ = 0.0;
            emfFresh_ = true;
            emfLastSample_ = sampleCounter_;
        }
    }

    // 1× cada 128 muestras: EEG, coeficientes Volterra, objetivos Γ.
    void blockUpdate(bool emfFreshNow) noexcept {
        if (presetDirty_.load(std::memory_order_relaxed)) {
            presetDirty_.store(false, std::memory_order_relaxed);
            applyPresetGeometry();
        }
        if (targetPending_.load(std::memory_order_acquire)) {
            targetPending_.store(false, std::memory_order_release);
            for (int b = 0; b < kBands; ++b) targetDb_[b] = targetStaged_[b];
        }
        // EEG real: reloj monotónico (mismo dominio que los timestamps
        // de los frames; ver neuro_acoustic_closed_loop.hpp).
        const std::uint64_t nowUs = ivanna::neuromorphic::monotonicNowUs();
        lastNeuroAdj_ = neuroCtrl_.compute(nowUs);
        neuroActive_ = lastNeuroAdj_.active;
        // Trims EEG por banda (Greenwood 8 → ERB 24), ya con wetScale.
        for (int b = 0; b < kBands; ++b) {
            const double trim =
                static_cast<double>(lastNeuroAdj_.gainDb[static_cast<std::size_t>(greenwoodIdx_[b])]) *
                static_cast<double>(lastNeuroAdj_.wetScale);
            eegTrim24_[b] = std::clamp(trim, -6.0, 6.0);
        }
        // Coeficientes Volterra puestos en staging (hilo de control).
        if (stagePending_.load(std::memory_order_acquire)) {
            stagePending_.store(false, std::memory_order_release);
            const std::uint32_t len = stageLen_.load(std::memory_order_relaxed);
            if (len == kernelLength_ && len >= 1 && len <= kMaxKernelLength) {
                for (std::uint32_t k = 0; k < len; ++k) {
                    baseH1_[k] = stageH1_[k];
                    baseH2d_[k] = stageH2d_[k];
                }
                if (volterra_.setCoefficients(baseH1_.data(), baseH2d_.data(), len))
                    volterraBypass_ = volterra_.isBypass();
            }
        }
        // Gobernanza Volterra por s_emf: el término cuadrático crece con
        // la excursión medida (más no-linealidad → más corrección).
        // Solo con kernel medido cargado y EMF real fresco.
        if (!volterraBypass_ && emfFreshNow) {
            const double emfDrive =
                std::clamp(std::fabs(emfDisp_) / static_cast<double>(kEmfXMaxM), 0.0, 1.0);
            if (std::fabs(emfDrive - lastEmfDriveApplied_) > 0.05) {
                lastEmfDriveApplied_ = emfDrive;
                const double scale = 1.0 + emfDrive;
                const std::uint32_t len = kernelLength_;
                for (std::uint32_t k = 0; k < len; ++k)
                    scaledH2d_[k] =
                        std::clamp(baseH2d_[k] * scale,
                                   -static_cast<double>(kMaxDiagonalCoeff),
                                   static_cast<double>(kMaxDiagonalCoeff));
                volterra_.setCoefficients(baseH1_.data(), scaledH2d_.data(), len);
            }
        }
        recomputeGammaTargets();
    }

    // Objetivos Γ (dB → lineal). MEDIDO: error del micrófono × coherencia.
    // APROXIMADO: balance espectral de la propia señal hacia plano.
    void recomputeGammaTargets() noexcept {
        const double maxDb = maxCorrectionDb_.load(std::memory_order_relaxed);
        if (calibActive_) {
            // v2.5.451 CALIBRADO: Γ por el error MEDIDO guardado (barrido de
            // sala). Coherencia 1.0: el perfil es una medición de confianza,
            // no una estimación viva. Misma fórmula que MEDIDO, otra fuente.
            for (int b = 0; b < kBands; ++b) {
                double db = -static_cast<double>(calibErrorDb_[b]);
                db += eegTrim24_[b];
                db = std::clamp(db, -maxDb, maxDb);
                gammaTarget_[b] = std::pow(10.0, db * 0.05);
            }
        } else if (sensorFresh_) {
            for (int b = 0; b < kBands; ++b) {
                double db = -static_cast<double>(activeFrame_.error_db[b]) *
                            static_cast<double>(activeFrame_.coherence[b]);
                db += eegTrim24_[b];
                db = std::clamp(db, -maxDb, maxDb);
                gammaTarget_[b] = std::pow(10.0, db * 0.05);
            }
        } else {
            // Referencia = potencia MEDIA por banda (espectro plano =
            // cada banda a 1/24 de la potencia total). Comparar el nivel
            // por banda contra la suma de banda ancha es un error de
            // calibración: un tono solo nunca iguala la suma de todos y
            // la banda más fuerte leería "por debajo" y no se cortaría.
            // La referencia es autoconsistente (bandas contra bandas) y
            // no depende del volumen: un espectro plano no se corrige.
            // El objetivo por banda es refDb + targetDb_b: con la curva
            // élite (Harman / firma insignia) la corrección doma el
            // espectro HACIA ESA CURVA, no hacia plano. En modo MEDIDO
            // el target vive en el sensor y aquí no se aplica (regla de
            // propietario único, ver setTargetDb).
            // SOLO CORTE [−3, 0] dB, nunca refuerzo: en un banco paralelo
            // las faldas de las bandas vecinas refuerzan lo que la banda
            // directa corta (leakage), y un refuerzo aproximado subiría
            // el piso/ruido. Domar los picos es el efecto real y seguro;
            // el refuerzo queda para el modo MEDIDO (respuesta de sala
            // suave, donde las bandas vecinas coinciden).
            double meanPow = 0.0;
            for (int b = 0; b < kBands; ++b) {
                meanPow += gammaEnv_[b] * gammaEnv_[b];
            }
            meanPow /= static_cast<double>(kBands);
            const double refDb = 10.0 * std::log10(meanPow + 1e-18);
            for (int b = 0; b < kBands; ++b) {
                const double levelDb = 20.0 * std::log10(gammaEnv_[b] + 1e-9);
                const double desiredDb = refDb + static_cast<double>(targetDb_[b]);
                double db = -(levelDb - desiredDb);
                db = std::clamp(db, -3.0, 0.0);
                db += eegTrim24_[b];
                db = std::clamp(db, -maxDb, maxDb);
                gammaTarget_[b] = std::pow(10.0, db * 0.05);
            }
        }
    }

    // ── Estado ───────────────────────────────────────────────────────────
    bool prepared_ = false;
    float sampleRate_ = 48000.0f;
    std::uint64_t sampleCounter_ = 0;
    HyperParams params_{};
    std::atomic<float> amount_{0.0f};
    std::atomic<int> preset_{0};
    std::atomic<bool> xtcEnabled_{true};
    std::atomic<bool> presetDirty_{false};
    std::atomic<float> maxCorrectionDb_{9.0f};
    std::uint32_t kernelLength_ = 64;

    double wet_ = 0.0;
    double wetStep_ = 1.0 / 240.0;
    double peakIn_ = 0.0;
    double peakDecay_ = 0.9998;
    double rmsEnv_ = 0.0;
    double rmsAlpha_ = 0.0002;
    double spikeAlpha_ = 0.0004;
    double gammaSlew_ = 0.0004;
    double envAtt_ = 0.002;
    double envRel_ = 0.0001;
    std::uint64_t sensorStaleSamples_ = 24000;
    std::uint64_t emfStaleSamples_ = 4800;

    // S_neuro: NPE on-device (envolventes + LIF) + controlador EEG.
    ivanna::BiquadEnvelopeBank envBank_{};
    ivannanpe::LIFPool32 lif_{};
    double spikeRate_ = 0.0;
    ivanna::neuromorphic::ClosedLoopController neuroCtrl_{};
    ivanna::neuromorphic::NeuroAdjustments lastNeuroAdj_{};
    bool neuroActive_ = false;
    double eegTrim24_[kBands] = {};
    int greenwoodIdx_[kBands] = {};

    // Γ: banco ERB paralelo (24 bandas × 2 canales), en double (F2/486).
    ivanna::BandBiquadD gammaL_[kBands]{};
    ivanna::BandBiquadD gammaR_[kBands]{};
    double gammaGain_[kBands] = {};
    double gammaTarget_[kBands] = {};
    double gammaEnv_[kBands] = {};
    double bandCenter_[kBands] = {};
    double invQ_[kBands] = {};
    // Curva objetivo élite por banda (dB, relativa a plano). Solo
    // gobierna el modo APROXIMADO (en MEDIDO el target vive en el
    // sensor). Staging por bandera atómica (control → audio).
    float targetDb_[kBands] = {};
    float targetStaged_[kBands] = {};
    std::atomic<bool> targetPending_{false};
    EliteTargetKind targetKind_ = EliteTargetKind::Harman2018;
    float bassTrimDb_ = 0.0f;

    // Construye la curva de 24 bandas para (targetKind_, bassTrimDb_)
    // sobre los centros de banda actuales.
    void buildTargetCurve(float* out24) const noexcept {
        if (out24 == nullptr) return;
        // buildEliteTarget24/applyBassTrimDb son control (curva dB) y
        // toman centros float: el cast es exacto a efectos de la curva.
        float centersF[kBands];
        for (int b = 0; b < kBands; ++b)
            centersF[b] = static_cast<float>(bandCenter_[b]);
        buildEliteTarget24(targetKind_, centersF, out24);
        applyBassTrimDb(out24, centersF, bassTrimDb_);
    }

    // Volterra diagonal (kernel medido; identidad si no hay).
    ivanna::dsp::DiagonalVolterra volterra_{};
    bool volterraBypass_ = true;
    std::vector<double> baseH1_{};
    std::vector<double> baseH2d_{};
    std::vector<double> scaledH2d_{};
    std::vector<double> stageH1_{};
    std::vector<double> stageH2d_{};
    std::atomic<std::uint32_t> stageLen_{0};
    std::atomic<bool> stagePending_{false};
    double lastEmfDriveApplied_ = -1.0;
    // Scratch de los envoltorios float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapH1_{}, wrapH2_{};
    std::vector<double> wrapL_{}, wrapR_{};

    // Z_emf: observador Thiele-Small (solo con V/I o EMF real).
    ivanna::spatial::EmfEstimator emf_{};
    ivanna::spatial::TsParams emfParams_{};
    double emfVel_ = 0.0;
    double emfDisp_ = 0.0;
    bool emfFresh_ = false;
    std::uint64_t emfLastSample_ = 0;
    std::atomic<float> emfV_{0.0f};
    std::atomic<float> emfI_{0.0f};
    std::atomic<bool> emfViPending_{false};
    std::atomic<float> emfDirect_{0.0f};
    std::atomic<bool> emfDirectPending_{false};

    // W: XTC (geometría del preset) + SensorFrame del micrófono.
    ivanna::spatial::CrosstalkCanceller xtc_{};
    SensorFrame stageFrame_{};
    std::atomic<bool> framePending_{false};
    SensorFrame activeFrame_{};
    bool hasFrame_ = false;
    bool sensorFresh_ = false;
    std::uint64_t lastFrameSample_ = 0;
    std::uint64_t lastFrameIndex_ = 0;
    std::atomic<std::uint64_t> framesReceived_{0};
    std::atomic<float> meanCoherence_{0.0f};

    // Perfil CALIBRADO (v2.5.451): error MEDIDO guardado por banda ERB para
    // el DAC sin micrófono. Staging igual que SensorFrame: el hilo JNI
    // escribe calibStage_ + calibPending_; el hilo de audio lo aplica en
    // applyStagedCalib() y solo él toca calibErrorDb_/calibActive_.
    float calibStage_[kBands] = {};
    float calibErrorDb_[kBands] = {};
    std::atomic<bool> calibPending_{false};
    std::atomic<bool> calibWantActive_{false};
    bool calibActive_ = false;

    double prevXL_ = 0.0;
    double prevXR_ = 0.0;
};

}  // namespace ivanna::spatial
