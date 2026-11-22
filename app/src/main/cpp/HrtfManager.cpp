#include "HrtfManager.hpp"
#include "fused/geodesic_pinna_model.hpp"
#include "fused/physical_propagation.hpp"
#include "spatial/SofaSafRirMasterKnowledge.hpp"
#include "spatial/HrirPoseSelect.hpp"
#include <cmath>
#include <cstring>
#include <type_traits>
#include <algorithm>

namespace Ivanna {
namespace {
// ── LGN — LEY GEODÉSICA DEL NOTCH (física nueva, cableada 2026-11-18) ────
// El factor de pabellón F_pinna(f) = [1 + ρ1·e^{iωτ1} + ρ2·e^{iωτ2}] /
// (1+ρ1+ρ2) es, en el dominio del tiempo, un FIR de 3 taps: el camino
// directo (peso 1) + los 2 caminos desviados por hélix/antihélix (pesos
// ρ1, ρ2) con retardos τ1, τ2 = ΔL/c. Ver geodesic_pinna_model.hpp: la ley
// da ΔL_j(φ, θ) = M_j − B_j·sin(φ − φ0_j) (+ corrección de azimut).
// Los retardos son fraccionarios (interpolación lineal); la salida se
// alarga kPinnaExtraTaps muestras (el llamador reserva len + eso).
// Devuelve false (sin tocar dst) si algún parámetro no es válido
// (fail-closed: el llamador usa el HRIR base sin pabellón = esfera sola).
// dst debe tener al menos len + kPinnaExtraTaps muestras; se suma sobre
// su contenido (el llamador lo inicializa a 0).
constexpr int kPinnaExtraTaps = 40;  // cubre τ1 máx (~95 µs) hasta 384 kHz
constexpr double kPinnaSpeedOfSound = 343.0;
bool applyPinnaFir(const float* src, float* dst, int len, double elevationRad,
                   double azimuthRad, double sampleRate) noexcept {
    if (!src || !dst || len <= 0 || !(sampleRate > 0.0) || !std::isfinite(sampleRate))
        return false;
    const double dl1 = ivanna::physical::pinnaPathDifferenceMm(
        ivanna::physical::kPinnaLowFamily, elevationRad, azimuthRad);
    const double dl2 = ivanna::physical::pinnaPathDifferenceMm(
        ivanna::physical::kPinnaHighFamily, elevationRad, azimuthRad);
    if (!std::isfinite(dl1) || !std::isfinite(dl2) || dl1 <= 0.0 || dl2 <= 0.0)
        return false;
    const double d1 = dl1 * 1.0e-3 / kPinnaSpeedOfSound * sampleRate;
    const double d2 = dl2 * 1.0e-3 / kPinnaSpeedOfSound * sampleRate;
    // d <= kPinnaExtraTaps-1: el tap interpolado i0+1 no sale del buffer.
    if (!std::isfinite(d1) || !std::isfinite(d2) || d1 < 0.0 || d2 < 0.0 ||
        d1 > static_cast<double>(kPinnaExtraTaps - 1) ||
        d2 > static_cast<double>(kPinnaExtraTaps - 1))
        return false;
    const double rho1 = ivanna::physical::kPinnaLowFamily.rho;
    const double rho2 = ivanna::physical::kPinnaHighFamily.rho;
    const double norm = 1.0 + rho1 + rho2;
    const double w0 = 1.0 / norm, w1 = rho1 / norm, w2 = rho2 / norm;
    auto addTap = [&](double delay, double weight, int n, float x) {
        const double pos = static_cast<double>(n) + delay;
        const int i0 = static_cast<int>(std::floor(pos));
        const double frac = pos - static_cast<double>(i0);
        dst[i0] += static_cast<float>(x * weight * (1.0 - frac));
        dst[i0 + 1] += static_cast<float>(x * weight * frac);
    };
    for (int n = 0; n < len; ++n) {
        const float x = src[n];
        if (x == 0.0f) continue;
        addTap(0.0, w0, n, x);
        addTap(d1, w1, n, x);
        addTap(d2, w2, n, x);
    }
    return true;
}

}  // namespace

// ── Catálogo de salas firma (espejo exacto de SignatureRooms.kt) ───────────
// Archivos .sofa del catálogo SOFA medido; el id es el de
// ListenerRoomControls::signatureRoom (1..3). La APK/el módulo extraen los
// assets a /system/etc/ivanna_omega/sofa/ (magisk_module/system/etc/...);
// m_sofaSearchDir se prueba PRIMERO (tests y assets extraídos por la app).
namespace {
constexpr const char* kSignatureSofaFiles[3] = {
    "TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa",  // id 1
    "MIT_KEMAR_normal_pinna.sofa",                    // id 2
    "demo_FreeFieldHRTF_1_IR.sofa",                   // id 3
};
constexpr const char* kSofaDeviceDirs[] = {
    "/data/adb/ivanna_omega/sofa",
    "/data/data/com.ivanna.omega/files/ivanna_omega/sofa",
    "/data/user/0/com.ivanna.omega/files/ivanna_omega/sofa",
    "/system/etc/ivanna_omega/sofa",
};
}  // namespace

template <typename Scalar>
HrtfManagerT<Scalar>::HrtfManagerT() {
    const char* bootCandidates[] = {
        "/data/adb/ivanna_omega/hrtf_dataset.ihr1",
        "/system/etc/ivanna_omega/hrtf/kemar.ihr1",
        "/data/adb/ivanna_omega/hrtf/kemar.ihr1",
        "/data/data/com.ivanna.omega/files/ivanna_omega/hrtf/kemar.ihr1",
        "/data/user/0/com.ivanna.omega/files/ivanna_omega/hrtf/kemar.ihr1"
    };
    for (const char* p : bootCandidates) {
        if (loadFromDataset(p)) break;
    }
    if (!m_datasetLoaded) {
        synthesizeHrtf(0.0f, 0.0f, 0.0f, 0);
        synthesizeHrtf(0.0f, 0.0f, 0.0f, 1);
    }

    for (size_t i = 0; i < BLOCK_SIZE + HRTF_TAPS; ++i) {
        m_histL[i] = Scalar(0.0);
        m_histR[i] = Scalar(0.0);
    }

    // Inicializar coeficientes de crossfade
    m_xfadePos  = 0;
    m_xfading   = false;
    // FALLO 6 (2026-11-18): 48000.0f es SOLO el valor por defecto del ctor —
    // aquí aún no se conoce la SR de la sesión. Quien conozca la SR real
    // debe llamar prepare(sampleRate) (p.ej. desde FusedSpatial::prepare);
    // hasta entonces la envolvente asume 48 kHz.
    m_wetEnv.configure(48000.0f, 8.0f, 18.0f, 35.0f);
    m_wetEnv.setImmediate(1.0f);
    ivanna::dsp::HrtfConvolutionConfig cfg;
    cfg.sample_rate_in = 48000;
    cfg.sample_rate_out = 768000;
    cfg.hrtf_filter_length = 512;
    cfg.block_size = BLOCK_SIZE;
    cfg.num_azimuth_bins = 360;
    cfg.num_elevation_bins = 180;
    cfg.use_fft_convolution = true;
    m_fastRpcClient.initialize(cfg);
}

// FALLO 6 (2026-11-18): reconfigura la envolvente wet/dry con la SR real
// de la sesión. Ver HrtfManager.hpp::prepare() para el porqué y los límites.
// PARCHE v2.6: firma float + setImmediate(currentGain) tras configure().
template <typename Scalar>
void HrtfManagerT<Scalar>::prepare(float sampleRate) noexcept {
    const float sr = (std::isfinite(sampleRate) && sampleRate > 8000.0f &&
                      sampleRate <= 384000.0f)
                         ? sampleRate
                         : 48000.0f;
    // LGN (2026-11-18): la SR real de la sesión; synthesizeHrtf() la usa
    // para los retardos del pabellón (segundos → muestras).
    m_sampleRate.store(sr, std::memory_order_relaxed);
    // Solo la envolvente: sus tiempos (8/18/35 ms) se convierten a muestras
    // con sampleRate en SupremeTransitionEnvelope::setTarget(), así que con
    // esto las rampas duran lo diseñado en ms a 96/192 kHz. configure() no
    // toca currentGain/step/remainingSamples (rampa en vuelo intacta);
    // setImmediate(currentGain) fija la envolvente en su valor actual con
    // la SR nueva (no-op sobre el valor, solo re-afirma el estado).
    m_wetEnv.configure(sr, 8.0f, 18.0f, 35.0f);
    m_wetEnv.setImmediate(m_wetEnv.currentGain);
}

template <typename Scalar>
void HrtfManagerT<Scalar>::synthesizeHrtf(float yaw, float pitch, float roll, int bank) {
    (void)roll;
    const float eff_azimuth = -yaw;
    const float theta = eff_azimuth;
    const float phi   = pitch;
    const float geodesic_dist = std::acos(
        std::max(-1.0f, std::min(1.0f, std::cos(phi) * std::cos(theta)))
    );
    const float riemannian_scale =
        1.0f + m_intrinsicCurvature.load(std::memory_order_relaxed) * std::sin(geodesic_dist);

    const float ild = std::clamp(std::sin(theta) * riemannian_scale * 0.25f, -0.45f, 0.45f);
    // ── ITD POR LA LEY DE CURVATURA (física nueva, cableada 2026-11-18) ──
    // Forma cerrada curvatureLawItdSeconds(θ, r): recupera Woodworth exacto
    // (a/c)(θ+sinθ) en campo lejano y añade el exceso de campo cercano.
    // Reemplaza la aproximación sin(θ)*18 muestras, que daba 375 µs máx a
    // 90° frente a los 656 µs reales de Woodworth (y se sumaba al pedestal
    // de 416 µs). La distancia es la de escena (setSceneDistance, 0.5..2 m).
    // La escala Riemanniana (SAF) se conserva como personalización sobre el
    // ITD físico. Se limita a ±800 µs (cubre el máx de la ley, 733 µs
    // @0.15 m); el bucle de almacenamiento trunca la cola en HRTF_TAPS.
    const double itdSec =
        ivanna::fused::propagation::curvatureLawItdSeconds(
            static_cast<double>(theta),
            static_cast<double>(m_sceneDistance.load(std::memory_order_relaxed))) *
        static_cast<double>(riemannian_scale);
    const double itdSr = static_cast<double>(m_sampleRate.load(std::memory_order_relaxed));
    const int itdMaxSamples = static_cast<int>(std::round(0.0008 * itdSr));
    const int itdSamples = std::clamp(
        static_cast<int>(std::round(itdSec * itdSr)),
        -itdMaxSamples, itdMaxSamples
    );

    std::memset(m_hrtfLL[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfRR[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfLR[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfRL[bank], 0, HRTF_TAPS * sizeof(Scalar));

    // Construir HRIR causal de 128 taps desde la variedad maestra SOFA + SAF (p0 + V * q_master)
    constexpr int kSofaLen = ivanna::master::kMasterHrirLen;
    float baseL[kSofaLen]{};
    float baseR[kSofaLen]{};
    for (int n = 0; n < kSofaLen; ++n) {
        float l = ivanna::master::kMasterSofaP0[n];
        float r = ivanna::master::kMasterSofaP0[kSofaLen + n];
        for (int k = 0; k < ivanna::master::kMasterSafK; ++k) {
            const float qk = ivanna::master::kMasterSafGoldenQ[k];
            l += qk * ivanna::master::kMasterSofaPcaV[k][n];
            r += qk * ivanna::master::kMasterSofaPcaV[k][kSofaLen + n];
        }
        baseL[n] = l;
        baseR[n] = r;
    }

    // NOTA PEDESTAL DE ITD (2026-11-18): el par base (p0 + V·q) es una
    // reconstrucción PCA difusa y asimétrica; no hay un ITD bien definido
    // para alinearlo (xcorr dice +416 µs, centroide dice -102 µs, y
    // alinear por centroide EMPEORA el ITD sintetizado a -13 muestras).
    // El fix correcto es un HRIR frontal real (SOFA medido a 0°), no un
    // alineado del par PCA. Queda pendiente (SofaHRTFLoader no lee el
    // SOFA). El ITD de azimut (Ley de Curvatura, abajo) sí es correcto y
    // se suma sobre el par tal cual.

    // ── LGN — LEY GEODÉSICA DEL NOTCH (física nueva, cableada 2026-11-18) ──
    // El pabellón se aplica como FIR de 3 taps al HRIR base, ANTES de
    // ILD/ITD; el mismo factor para ambos oídos (la ley es simétrica L/R
    // en esta versión). La salida se alarga kPinnaExtraTaps muestras; el
    // bucle de almacenamiento trunca en HRTF_TAPS como ya hacía con
    // ITD/crossfeed. La energía (y con ella la normalización ILD) se mide
    // sobre el HRIR ya con pabellón, así el ILD se aplica al filtro final.
    // Fallback (esfera sola): si está desactivado con setPinnaEnabled(false)
    // o applyPinnaFir falla (parámetros no válidos), se usa el HRIR base
    // sin pabellón, exactamente como antes. Sin cambio de ABI.
    // Elevación = pitch (phi), azimut = theta (ver arriba).
    float pinnaL[kSofaLen + kPinnaExtraTaps]{};
    float pinnaR[kSofaLen + kPinnaExtraTaps]{};
    const float* hrirL = baseL;
    const float* hrirR = baseR;
    int hrirLen = kSofaLen;
    if (m_pinnaEnabled.load(std::memory_order_relaxed)) {
        const double sr = static_cast<double>(m_sampleRate.load(std::memory_order_relaxed));
        const bool okL = applyPinnaFir(baseL, pinnaL, kSofaLen,
                                       static_cast<double>(phi), static_cast<double>(theta), sr);
        const bool okR = applyPinnaFir(baseR, pinnaR, kSofaLen,
                                       static_cast<double>(phi), static_cast<double>(theta), sr);
        if (okL && okR) {
            hrirL = pinnaL;
            hrirR = pinnaR;
            hrirLen = kSofaLen + kPinnaExtraTaps;
        }
    }

    float energyL = 1e-8f, energyR = 1e-8f;
    for (int n = 0; n < hrirLen; ++n) {
        energyL += hrirL[n] * hrirL[n];
        energyR += hrirR[n] * hrirR[n];
    }
    const float normL = (1.0f - ild) / std::sqrt(energyL);
    const float normR = (1.0f + ild) / std::sqrt(energyR);
    const int delayL = std::max(0, itdSamples);
    const int delayR = std::max(0, -itdSamples);

    // Almacenar en orden causal invertido [HRTF_TAPS - 1 - tap] porque processBinauralScene
    // indexa m_histL[i + t] donde t = HRTF_TAPS - 1 es la muestra actual x[n].
    for (int n = 0; n < hrirLen; ++n) {
        const int tapL = n + delayL;
        const int tapR = n + delayR;
        if (tapL < static_cast<int>(HRTF_TAPS)) {
            m_hrtfLL[bank][HRTF_TAPS - 1 - tapL] = hrirL[n] * normL;
        }
        if (tapR < static_cast<int>(HRTF_TAPS)) {
            m_hrtfRR[bank][HRTF_TAPS - 1 - tapR] = hrirR[n] * normR;
        }
        // Crossfeed acústico natural de sombra cefálica (~14 muestras = 290 us)
        const int crossTapL = tapL + 14;
        const int crossTapR = tapR + 14;
        if (crossTapL < static_cast<int>(HRTF_TAPS)) {
            m_hrtfRL[bank][HRTF_TAPS - 1 - crossTapL] = hrirL[n] * normL * 0.12f;
        }
        if (crossTapR < static_cast<int>(HRTF_TAPS)) {
            m_hrtfLR[bank][HRTF_TAPS - 1 - crossTapR] = hrirR[n] * normR * 0.12f;
        }
    }
}

template <typename Scalar>
void HrtfManagerT<Scalar>::setHeadPose(float yaw, float pitch, float roll) {
    // FIX (cableado binaural, FASE 8): persistir la pose real en grados,
    // incondicionalmente — antes solo se calculaba azDeg dentro del branch
    // m_datasetLoaded para posicionar el dataset medido, y se perdía justo
    // después de usarse. Cualquier consumidor de la posición real vigente
    // (p.ej. el path FastRPC/Hexagon en processBinauralScene) necesita
    // poder leerla sin importar si hay dataset cargado o síntesis analítica.
    constexpr float kRadToDeg = 180.f / 3.14159265f;
    m_lastYaw.store(yaw, std::memory_order_relaxed);
    m_lastPitch.store(pitch, std::memory_order_relaxed);
    m_lastRoll.store(roll, std::memory_order_relaxed);
    m_evPose.fetch_add(1, std::memory_order_relaxed);
    const float safBias = m_safAzimuthBias.load(std::memory_order_relaxed);
    m_currentAzimuthDeg.store((yaw * kRadToDeg) + safBias, std::memory_order_relaxed);
    m_currentElevationDeg.store(pitch * kRadToDeg, std::memory_order_relaxed);

    // FIX (clic en banco-switch): prepara el banco inactivo y activa crossfade.
    // processBinauralScene mezcla los dos bancos durante XFADE_FRAMES bloques.
    const int inactive = 1 - m_activeBank.load(std::memory_order_relaxed);
    if (m_sofaLoaded || m_datasetLoaded) {
        const float azDeg = (yaw * kRadToDeg) + safBias;
        loadFromDatasetAtPose(azDeg, pitch * kRadToDeg, inactive);
    } else {
        // El sesgo SAF (q[1], +-10 grados) tambien debe llegar a la sintesis analitica;
        // antes solo se aplicaba con dataset medido y q[1] no tenia efecto sin dataset.
        synthesizeHrtf(yaw + safBias * (3.14159265f / 180.f), pitch, roll, inactive);
    }
    // Señalizar crossfade — processBinauralScene lo lee de forma lock-free
    m_pendingBank.store(inactive, std::memory_order_release);
    m_xfadeTrigger.store(true,  std::memory_order_release);
}

template <typename Scalar>
bool HrtfManagerT<Scalar>::setSceneDistance(float distanceScale) noexcept {
    if (!std::isfinite(distanceScale) || distanceScale < 0.5f - 1e-3f || distanceScale > 2.f + 1e-3f) return false;
    m_distGainTarget.store(ivanna::spatial::sceneDistanceGain(distanceScale), std::memory_order_relaxed);   // idempotente
    // Ley de Curvatura (2026-11-18): la distancia de escena para el ITD
    // en forma cerrada (synthesizeHrtf). Se lee en la próxima síntesis.
    m_sceneDistance.store(distanceScale, std::memory_order_relaxed);
    return true;
}

// Rampa lineal por bloque hacia la ganancia de distancia (sin clic). Identidad exacta mientras el objetivo es 1.
template <typename Scalar>
void HrtfManagerT<Scalar>::applyDistanceGain(Scalar* left, Scalar* right, size_t n) noexcept {
    const Scalar target = static_cast<Scalar>(m_distGainTarget.load(std::memory_order_relaxed));
    if (m_distGainCur == Scalar(1.0) && target == Scalar(1.0)) return;
    const Scalar step = (target - m_distGainCur) / static_cast<Scalar>(n == 0 ? 1 : n);
    Scalar g = m_distGainCur;
    for (size_t i = 0; i < n; ++i) {
        g += step;
        left[i]  *= g;
        right[i] *= g;
    }
    m_distGainCur = (std::fabs(target - Scalar(1.0)) < Scalar(1e-6)) ? Scalar(1.0) : target;
}

template <typename Scalar>
void HrtfManagerT<Scalar>::processBinauralScene(Ivanna::AudioBuffer* buffer) {
    if constexpr (std::is_same_v<Scalar, float>) {
        // Instancia float: punteros directos, bit-idéntico al histórico.
        processBinauralSceneImpl(buffer->left, buffer->right);
        applyDistanceGain(buffer->left, buffer->right, BLOCK_SIZE);
    } else {
        // Instancia double: staging en pila (sin malloc en RT) float->double
        // y vuelta. El llamador double usa la sobrecarga (double*, double*, int).
        Scalar tmpL[BLOCK_SIZE], tmpR[BLOCK_SIZE];
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            tmpL[i] = static_cast<Scalar>(buffer->left[i]);
            tmpR[i] = static_cast<Scalar>(buffer->right[i]);
        }
        processBinauralSceneImpl(tmpL, tmpR);
        applyDistanceGain(tmpL, tmpR, BLOCK_SIZE);
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            buffer->left[i] = static_cast<float>(tmpL[i]);
            buffer->right[i] = static_cast<float>(tmpR[i]);
        }
    }
}

// Entrada double para la cadena double (FusedSpatial): bloques de
// BLOCK_SIZE; el resto no múltiplo pasa en seco (fail-safe).
template <typename Scalar>
void HrtfManagerT<Scalar>::processBinauralScene(Scalar* left, Scalar* right, int n) {
    if (!left || !right || n <= 0) return;
    int done = 0;
    while (done + static_cast<int>(BLOCK_SIZE) <= n) {
        processBinauralSceneImpl(left + done, right + done);
        applyDistanceGain(left + done, right + done, BLOCK_SIZE);
        done += static_cast<int>(BLOCK_SIZE);
    }
}

template <typename Scalar>
void HrtfManagerT<Scalar>::processBinauralSceneImpl(Scalar* left, Scalar* right) {
    // Ruta Hexagon/DSP (FastRPC, float por hardware): solo en la instancia
    // float. El núcleo double convoluciona en CPU (el DSP es float).
    if constexpr (std::is_same_v<Scalar, float>) {
    if (m_fastRpcClient.isDSPReady() &&
        m_wetDry.load(std::memory_order_relaxed) >= 0.999f &&
        m_wetEnv.currentGain >= 0.999f && !m_wetEnv.isTransitioning()) {
        // The QAIC client does not promise in-place operation. Preserve the
        // source block in preallocated per-instance staging and only count/return
        // when the DSP explicitly confirms success. A rejected/failed RPC restores
        // the dry block and falls through to the CPU convolver below.
        std::memcpy(m_dspDryL, left, sizeof(m_dspDryL));
        std::memcpy(m_dspDryR, right, sizeof(m_dspDryR));
        ivanna::dsp::SpatialPosition pos;
        pos.azimuth = m_currentAzimuthDeg.load(std::memory_order_relaxed);
        pos.elevation = m_currentElevationDeg.load(std::memory_order_relaxed);
        pos.distance = 1.0f;
        const bool dspApplied = m_fastRpcClient.delegateBinauralConvolution(
            m_dspDryL, m_dspDryR,
            left, right,
            pos, BLOCK_SIZE
        );
        if (dspApplied) {
            m_evDsp.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::memcpy(left, m_dspDryL, sizeof(m_dspDryL));
        std::memcpy(right, m_dspDryR, sizeof(m_dspDryR));
    }
    }  // if constexpr (float: DSP)
    // ── Crossfade lock-free ──────────────────────────────────────────────────
    // Si hay un banco pendiente: inicia crossfade suave sin clic.
    // XFADE_FRAMES bloques de fundido cruzado (coseno)
    // SAF vivo: si q cambio y no hay crossfade en curso, re-sintetiza el banco inactivo con
    // la ultima pose (curvatura/sesgo nuevos) y dispara el crossfade, sin esperar a un
    // setHeadPose() que puede no llegar (p.ej. sin sensores de cabeza).
    if (m_safDirty.load(std::memory_order_acquire) && !m_xfading &&
        !m_xfadeTrigger.load(std::memory_order_acquire)) {
        m_safDirty.store(false, std::memory_order_relaxed);
        m_evSafRefresh.fetch_add(1, std::memory_order_relaxed);
        setHeadPose(m_lastYaw.load(std::memory_order_relaxed),
                    m_lastPitch.load(std::memory_order_relaxed),
                    m_lastRoll.load(std::memory_order_relaxed));
    }
    if (m_xfadeTrigger.exchange(false, std::memory_order_acq_rel)) {
        m_pendingBankLocal = m_pendingBank.load(std::memory_order_acquire);
        m_xfading  = true;
        m_xfadePos = 0;
    }

    const int bankA = m_activeBank.load(std::memory_order_relaxed);
    const int bankB = m_pendingBankLocal;

    // Peso de crossfade [0..1]: 0 = solo bankA, 1 = solo bankB
    Scalar xfadeW = Scalar(0.0);
    bool  doXfade = m_xfading;
    if (doXfade) {
        xfadeW = static_cast<Scalar>(m_xfadePos + 1) / static_cast<Scalar>(XFADE_FRAMES);
        // Curva coseno para evitar bump de energía
        xfadeW = Scalar(0.5) * (Scalar(1.0) - std::cos(xfadeW * Scalar(3.141592653589793)));
        m_xfadePos++;
        if (m_xfadePos >= XFADE_FRAMES) {
            m_activeBank.store(bankB, std::memory_order_release);
            m_xfading  = false;
            m_xfadePos = 0;
            xfadeW     = Scalar(1.0);
            doXfade    = false;
        }
    }

    // FIX DAC USB-C: leer targetWet una sola vez por bloque y aplicar rampa por muestra
    // con SupremeTransitionEnvelope para evitar clics cuando wet salta entre 0 y 1.
    const float targetWet = std::clamp(m_wetDry.load(std::memory_order_relaxed), 0.0f, 1.0f);

    // ── Ingresar muestras al historial ───────────────────────────────────────
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        m_histL[HRTF_TAPS - 1 + i] = left[i];
        m_histR[HRTF_TAPS - 1 + i] = right[i];
    }

    // Bypass completo únicamente cuando la envolvente de transición ha llegado a cero
    if (!m_wetEnv.beginBlock(targetWet)) {
        std::memmove(m_histL, m_histL + BLOCK_SIZE, (HRTF_TAPS - 1) * sizeof(Scalar));
        std::memmove(m_histR, m_histR + BLOCK_SIZE, (HRTF_TAPS - 1) * sizeof(Scalar));
        return;
    }

    // ── Convolución HRTF ─────────────────────────────────────────────────────
    double evIn = 0.0;
    for (size_t i = 0; i < BLOCK_SIZE; ++i)
        evIn += static_cast<double>(m_histL[HRTF_TAPS - 1 + i]) * m_histL[HRTF_TAPS - 1 + i]
              + static_cast<double>(m_histR[HRTF_TAPS - 1 + i]) * m_histR[HRTF_TAPS - 1 + i];
    // Bucle escalar en Scalar (una sola copia): en host corre para
    // float y double; en ARM, la instancia float usa NEON (bit-idéntico)
    // y la instancia double usa este bucle escalar.
    auto scalarLoop = [&]() {
        for (size_t i = 0; i < BLOCK_SIZE; ++i) {
            const Scalar wet = static_cast<Scalar>(m_wetEnv.nextSample());
            const Scalar dry = Scalar(1.0) - wet;
            Scalar outL = Scalar(0.0), outR = Scalar(0.0);
            for (size_t t = 0; t < HRTF_TAPS; ++t) {
                const Scalar xL = m_histL[i + t];
                const Scalar xR = m_histR[i + t];
                outL += xL * m_hrtfLL[bankA][t] + xR * m_hrtfRL[bankA][t];
                outR += xR * m_hrtfRR[bankA][t] + xL * m_hrtfLR[bankA][t];
            }
            if (doXfade) {
                Scalar bL = Scalar(0.0), bR = Scalar(0.0);
                for (size_t t = 0; t < HRTF_TAPS; ++t) {
                    const Scalar xL = m_histL[i + t];
                    const Scalar xR = m_histR[i + t];
                    bL += xL * m_hrtfLL[bankB][t] + xR * m_hrtfRL[bankB][t];
                    bR += xR * m_hrtfRR[bankB][t] + xL * m_hrtfLR[bankB][t];
                }
                outL = outL * (Scalar(1.0) - xfadeW) + bL * xfadeW;
                outR = outR * (Scalar(1.0) - xfadeW) + bR * xfadeW;
            }
            // FIX DAC USB-C: mismo blend wet/dry que el path NEON
            const Scalar dryL = m_histL[HRTF_TAPS - 1 + i];
            const Scalar dryR = m_histR[HRTF_TAPS - 1 + i];
            left[i]  = wet * outL + dry * dryL;
            right[i] = wet * outR + dry * dryR;
        }
    };
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
    if constexpr (std::is_same_v<Scalar, float>) {
    for (size_t i = 0; i < BLOCK_SIZE; ++i) {
        const float wet = m_wetEnv.nextSample();
        const float dry = 1.0f - wet;
        float32x4_t aLL = vdupq_n_f32(0.f), aLR = vdupq_n_f32(0.f);
        float32x4_t aRR = vdupq_n_f32(0.f), aRL = vdupq_n_f32(0.f);

        for (size_t t = 0; t < HRTF_TAPS; t += 4) {
            const float32x4_t xL  = vld1q_f32(&m_histL[i + t]);
            const float32x4_t xR  = vld1q_f32(&m_histR[i + t]);
            aLL = vmlaq_f32(aLL, vld1q_f32(&m_hrtfLL[bankA][t]), xL);
            aLR = vmlaq_f32(aLR, vld1q_f32(&m_hrtfLR[bankA][t]), xL);
            aRR = vmlaq_f32(aRR, vld1q_f32(&m_hrtfRR[bankA][t]), xR);
            aRL = vmlaq_f32(aRL, vld1q_f32(&m_hrtfRL[bankA][t]), xR);
        }

        auto hsum = [](float32x4_t v) {
            return vgetq_lane_f32(v,0)+vgetq_lane_f32(v,1)+
                   vgetq_lane_f32(v,2)+vgetq_lane_f32(v,3);
        };

        float outL = hsum(aLL) + hsum(aRL);
        float outR = hsum(aRR) + hsum(aLR);

        if (doXfade) {
            float32x4_t bLL = vdupq_n_f32(0.f), bLR = vdupq_n_f32(0.f);
            float32x4_t bRR = vdupq_n_f32(0.f), bRL = vdupq_n_f32(0.f);
            for (size_t t = 0; t < HRTF_TAPS; t += 4) {
                const float32x4_t xL = vld1q_f32(&m_histL[i + t]);
                const float32x4_t xR = vld1q_f32(&m_histR[i + t]);
                bLL = vmlaq_f32(bLL, vld1q_f32(&m_hrtfLL[bankB][t]), xL);
                bLR = vmlaq_f32(bLR, vld1q_f32(&m_hrtfLR[bankB][t]), xL);
                bRR = vmlaq_f32(bRR, vld1q_f32(&m_hrtfRR[bankB][t]), xR);
                bRL = vmlaq_f32(bRL, vld1q_f32(&m_hrtfRL[bankB][t]), xR);
            }
            const float bL = hsum(bLL) + hsum(bRL);
            const float bR = hsum(bRR) + hsum(bLR);
            outL = outL * (1.0f - xfadeW) + bL * xfadeW;
            outR = outR * (1.0f - xfadeW) + bR * xfadeW;
        }

        // FIX DAC USB-C: blend wet (HRTF) + dry (original) — preserva la señal
        // estéreo pura cuando wet < 1, evitando artefactos al cambiar ruta de audio.
        const float dryL = m_histL[HRTF_TAPS - 1 + i];
        const float dryR = m_histR[HRTF_TAPS - 1 + i];
        left[i]  = wet * outL + dry * dryL;
        right[i] = wet * outR + dry * dryR;
    }    } else {
        scalarLoop();
    }
#else
    scalarLoop();
#endif
    {
        double evOut = 0.0;
        for (size_t i = 0; i < BLOCK_SIZE; ++i)
            evOut += static_cast<double>(left[i]) * left[i]
                   + static_cast<double>(right[i]) * right[i];
        m_evCpu.fetch_add(1, std::memory_order_relaxed);
        m_evIn.store(m_evIn.load(std::memory_order_relaxed) + evIn, std::memory_order_relaxed);
        m_evOut.store(m_evOut.load(std::memory_order_relaxed) + evOut, std::memory_order_relaxed);
    }

    // ── Shift de historial: mover las últimas HRTF_TAPS-1 muestras al inicio
    // FIX (bug de offset): el shift anterior movía HRTF_TAPS-1 elementos desde
    // el índice BLOCK_SIZE, que no coincidía con el inicio del overlap correcto.
    // El overlap-save correcto: copiar los últimos (HRTF_TAPS-1) samples del
    // bloque actual al inicio del buffer de historia.
    std::memmove(m_histL, m_histL + BLOCK_SIZE, (HRTF_TAPS - 1) * sizeof(Scalar));
    std::memmove(m_histR, m_histR + BLOCK_SIZE, (HRTF_TAPS - 1) * sizeof(Scalar));
}

template <typename Scalar>
void HrtfManagerT<Scalar>::flushHistory() noexcept {
    // FIX DAC USB-C: limpia el historial overlap-save. Llamar inmediatamente
    // después de setWetDry(0.f) cuando el sistema rotea al DAC USB-C.
    // Sin esto, las muestras cacheadas en m_histL/m_histR producen ruido
    // de correlación (sonido de "canal de TV sin señal") en el primer
    // bloque procesado por el nuevo dispositivo de salida.
    std::memset(m_histL, 0, sizeof(m_histL));
    std::memset(m_histR, 0, sizeof(m_histR));
}

template <typename Scalar>
bool HrtfManagerT<Scalar>::loadFromDataset(const char* path) {
    if (!m_loader.load(path)) return false;
    m_datasetLoaded = true;
    loadFromDatasetAtPose(0.f, 0.f, 0);
    loadFromDatasetAtPose(0.f, 0.f, 1);
    return true;
}

// ── SOFA medido (v2.5.472 Fase 2 ítem D) ────────────────────────────────────
// Solo se instala un SOFA con Status::PARSED_MEASURED (Data.IR leído de
// verdad por SofaHRTFLoader). La carga se hace en una instancia temporal:
// un archivo que no se puede parsear como medido NO desplaza el dataset
// vigente (fail-closed, retorna false, estado intacto). Al instalarse, el
// SOFA tiene precedencia sobre el dataset IHR1 y sobre la síntesis
// analítica (ver loadFromDatasetAtPose) y ambos bancos se re-poseen con la
// pose vigente para que actúe de inmediato.
template <typename Scalar>
bool HrtfManagerT<Scalar>::loadSofaDataset(const char* path) {
    if (!path || !*path) return false;
    SofaHRTFLoader tmp;
    if (!tmp.load(path) || !tmp.isMeasured()) return false;
    m_sofaLoader = std::move(tmp);
    m_sofaLoaded = true;
    const float az = m_currentAzimuthDeg.load(std::memory_order_relaxed);
    const float el = m_currentElevationDeg.load(std::memory_order_relaxed);
    loadFromDatasetAtPose(az, el, 0);
    loadFromDatasetAtPose(az, el, 1);
    return true;
}

// Selector de sala firma del catálogo SOFA medido (SignatureRooms.kt).
// 0 = ninguna (desactiva el SOFA y vuelve el IHR1 o la síntesis); 1..3 =
// salas. Busca el .sofa en m_sofaSearchDir primero y luego en las rutas del
// dispositivo. Id inválido o archivo no cargable como medido -> false SIN
// tocar el estado (la sala vigente sigue activa).
template <typename Scalar>
bool HrtfManagerT<Scalar>::setSignatureRoom(int id) {
    if (id < 0 || id > 3) return false;
    if (id == 0) {
        m_sofaLoaded = false;
        m_signatureRoom = 0;
        const float az = m_currentAzimuthDeg.load(std::memory_order_relaxed);
        const float el = m_currentElevationDeg.load(std::memory_order_relaxed);
        loadFromDatasetAtPose(az, el, 0);
        loadFromDatasetAtPose(az, el, 1);
        return true;
    }
    const char* file = kSignatureSofaFiles[id - 1];
    auto tryDir = [&](const std::string& dir) -> bool {
        if (dir.empty()) return false;
        std::string p = dir;
        if (p.back() != '/') p += '/';
        p += file;
        return loadSofaDataset(p.c_str());
    };
    if (tryDir(m_sofaSearchDir)) {
        m_signatureRoom = id;
        return true;
    }
    for (const char* d : kSofaDeviceDirs) {
        if (tryDir(d)) {
            m_signatureRoom = id;
            return true;
        }
    }
    return false;
}

// ── Slot para RIR medida (medición con micrófono: OTRO paso, necesita al
// dueño) ────────────────────────────────────────────────────────────────────
// Solo staging: guarda la RIR estéreo medida para que el operador de sala
// (RirConvolver / fused_listener_room) la consuma cuando se cablee. Hoy
// NADA la convoluciona. Validación fail-closed: punteros nulos, taps fuera
// de 1..1M, SR fuera de 1000..384000 Hz o muestras no finitas -> false sin
// tocar la RIR instalada.
template <typename Scalar>
bool HrtfManagerT<Scalar>::installMeasuredRoomRir(const float* left, const float* right,
                                         size_t taps, float sampleRateHz) {
    if (!left || !right || taps == 0 || taps > 1000000 ||
        !std::isfinite(sampleRateHz) || sampleRateHz < 1000.0f ||
        sampleRateHz > 384000.0f)
        return false;
    for (size_t i = 0; i < taps; ++i)
        if (!std::isfinite(left[i]) || !std::isfinite(right[i])) return false;
    m_roomRirL.assign(left, left + taps);
    m_roomRirR.assign(right, right + taps);
    m_roomRirSampleRate = sampleRateHz;
    return true;
}

template <typename Scalar>
void HrtfManagerT<Scalar>::clearMeasuredRoomRir() noexcept {
    m_roomRirL.clear();
    m_roomRirR.clear();
    m_roomRirSampleRate = 0.0f;
}

template <typename Scalar>
void HrtfManagerT<Scalar>::loadSofaAtPose(float azimuthDeg, float elevationDeg, int bank) {
    if (!m_sofaLoaded || bank < 0 || bank > 1) return;
    if (!std::isfinite(azimuthDeg) || !std::isfinite(elevationDeg)) return;
    HRTFIR pair;
    if (!m_sofaLoader.hrtfAt(azimuthDeg, elevationDeg, pair)) return;  // banco intacto
    const float srcSr = pair.sampleRate;
    const float dstSr = m_sampleRate.load(std::memory_order_relaxed);
    if (!(srcSr > 0.0f) || !(dstSr > 0.0f)) return;
    const size_t srcLen = pair.left.size();
    if (srcLen == 0 || pair.right.size() != srcLen) return;
    std::memset(m_hrtfLL[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfRR[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfLR[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfRL[bank], 0, HRTF_TAPS * sizeof(Scalar));
    // Remuestreo lineal a la SR de la sesión, truncado a HRTF_TAPS. Con
    // SR iguales es copia exacta (ratio 1, frac 0). La cola fuera del
    // archivo repite el último valor (sin extrapolación).
    const double ratio = static_cast<double>(srcSr) / static_cast<double>(dstSr);
    auto sample = [&](const std::vector<float>& v, size_t k) -> float {
        const double pos = static_cast<double>(k) * ratio;
        const size_t i0 = static_cast<size_t>(std::floor(pos));
        const double frac = pos - static_cast<double>(i0);
        if (i0 >= srcLen) return v.back();
        const float a = v[i0];
        const float b = (i0 + 1 < srcLen) ? v[i0 + 1] : v.back();
        return static_cast<float>(a * (1.0 - frac) + b * frac);
    };
    for (size_t k = 0; k < HRTF_TAPS; ++k) {
        m_hrtfLL[bank][HRTF_TAPS - 1 - k] = sample(pair.left, k);
        m_hrtfRR[bank][HRTF_TAPS - 1 - k] = sample(pair.right, k);
    }
}

template <typename Scalar>
void HrtfManagerT<Scalar>::loadFromDatasetAtPose(float azimuthDeg, float elevationDeg, int bank) {
    // SOFA medido tiene precedencia sobre el dataset IHR1 (v2.5.472 D).
    if (m_sofaLoaded) {
        loadSofaAtPose(azimuthDeg, elevationDeg, bank);
        return;
    }
    if (!m_datasetLoaded || m_loader.size() == 0) return;
    const size_t n = m_loader.size();
    // Distancia angular sobre la esfera (envuelve el azimut y usa la elevación): ver HrirPoseSelect.hpp.
    const size_t bestIdx = ivanna::spatial::nearestHrirIndex(n,
        [this](size_t i) { return m_loader.entry(i).azimuthDeg; },
        [this](size_t i) { return m_loader.entry(i).elevationDeg; },
        azimuthDeg, elevationDeg);
    const auto& e = m_loader.entry(bestIdx);
    const size_t cL = std::min(static_cast<size_t>(HRTF_TAPS), e.left.size());
    const size_t cR = std::min(static_cast<size_t>(HRTF_TAPS), e.right.size());

    std::memset(m_hrtfLL[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfRR[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfLR[bank], 0, HRTF_TAPS * sizeof(Scalar));
    std::memset(m_hrtfRL[bank], 0, HRTF_TAPS * sizeof(Scalar));

    constexpr size_t kSofaLen = static_cast<size_t>(ivanna::master::kMasterHrirLen);
    for (size_t k = 0; k < cL; ++k) {
        float val = e.left[k];
        if (k < kSofaLen) {
            for (int c = 0; c < ivanna::master::kMasterSafK; ++c) {
                val += ivanna::master::kMasterSafGoldenQ[c] * ivanna::master::kMasterSofaPcaV[c][k];
            }
        }
        m_hrtfLL[bank][HRTF_TAPS - 1 - k] = val;
    }
    for (size_t k = 0; k < cR; ++k) {
        float val = e.right[k];
        if (k < kSofaLen) {
            for (int c = 0; c < ivanna::master::kMasterSafK; ++c) {
                val += ivanna::master::kMasterSafGoldenQ[c] * ivanna::master::kMasterSofaPcaV[c][kSofaLen + k];
            }
        }
        m_hrtfRR[bank][HRTF_TAPS - 1 - k] = val;
    }
}

} // namespace Ivanna

// Instanciaciones explícitas (las definiciones viven en este .cpp):
// float = ruta legacy bit-idéntica; double = núcleo de la labor grande.
template class Ivanna::HrtfManagerT<float>;
template class Ivanna::HrtfManagerT<double>;
