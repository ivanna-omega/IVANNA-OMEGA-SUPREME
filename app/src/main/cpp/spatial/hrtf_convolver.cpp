/*
 * ============================================================================
 * IVANNA-OMEGA-SUPREME — Motor de Audio Holográfico de Bajo Nivel
 * ============================================================================
 * Autoría Exclusiva y Propiedad Absoluta:
 * Luis Uriel Pimentel Pérez (alias Gore TNS)
 *
 * Todos los modelos matemáticos, arquitecturas de sistema e implementaciones
 * de código contenidos en este archivo son propiedad intelectual exclusiva
 * del autor citado. Queda estrictamente prohibida la reproducción, distribución,
 * modificación o uso comercial no autorizado.
 *
 * Este software NO se distribuye bajo licencia CC0 ni dominio público.
 * Todos los derechos reservados. © 2026 Luis Uriel Pimentel Pérez.
 * ============================================================================
 */

#include "hrtf_convolver.hpp"

// ── Anti-denormales (estado del arte DSP en tiempo real) ──────────────────
// Las colas IIR/FIR del convolver decaen a valores subnormales (~1e-38), donde
// la CPU degrada 10-100x por microcode assist. Se activa FTZ/DAZ por hilo.
#if defined(__x86_64__) || defined(__i386__)
  #include <immintrin.h>
  static inline void enableDenormalGuard() noexcept {
      _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
      _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
  }
#elif defined(__aarch64__) || defined(__arm__)
  #include <cstdint>
  static inline void enableDenormalGuard() noexcept {
#if defined(__aarch64__)
      uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
      fpcr |= (1ULL << 24); // FZ flush-to-zero
      __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
#elif defined(__arm__) || defined(__ARM_ARCH_7A__)
      uint32_t fpscr; __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
      fpscr |= (1U << 24); // FZ
      __asm__ volatile("vmsr fpscr, %0" :: "r"(fpscr));
#endif
  }
#else
  static inline void enableDenormalGuard() noexcept {}

#endif
#if defined(__aarch64__) || defined(__arm__)
#include <arm_neon.h>
#endif
#include "fft_radix2.hpp"
#include "../include/audio_thread_priority.h"
#include "../SafHRTFDatasetBridge.hpp"
#include <cstring>
#include <cmath>
#include <type_traits>
#include <algorithm>
#include <android/log.h>

#define HRTF_LOG_TAG "IVANNA-HRTF"

namespace ivanna {

// Multiplicación compleja espectro × HRTF (pasos 2d y 2e del process):
// NEON float en ARM para Scalar=float (bit-idéntico al histórico);
// escalar double en el núcleo double (el camino NEON float queda solo
// para Scalar=float). La rama NEON descartada por if constexpr no se
// instancia en double.
template <typename Scalar>
static void multiplySpectra(const Scalar* mRe, const Scalar* mIm,
                            const Scalar* hRe, const Scalar* hIm,
                            Scalar* yRe, Scalar* yIm, int n) noexcept {
#if defined(__aarch64__)
    if constexpr (std::is_same_v<Scalar, float>) {
        int i = 0;
        for (; i <= n - 4; i += 4) {
            float32x4_t mRe4 = vld1q_f32(mRe + i);
            float32x4_t mIm4 = vld1q_f32(mIm + i);
            float32x4_t hRe4 = vld1q_f32(hRe + i);
            float32x4_t hIm4 = vld1q_f32(hIm + i);
            vst1q_f32(yRe + i, vsubq_f32(vmulq_f32(mRe4, hRe4), vmulq_f32(mIm4, hIm4)));
            vst1q_f32(yIm + i, vaddq_f32(vmulq_f32(mRe4, hIm4), vmulq_f32(mIm4, hRe4)));
        }
        for (; i < n; ++i) {
            yRe[i] = mRe[i] * hRe[i] - mIm[i] * hIm[i];
            yIm[i] = mRe[i] * hIm[i] + mIm[i] * hRe[i];
        }
        return;
    }
#endif
    for (int i = 0; i < n; ++i) {
        yRe[i] = mRe[i] * hRe[i] - mIm[i] * hIm[i];
        yIm[i] = mRe[i] * hIm[i] + mIm[i] * hRe[i];
    }
}

// FIX (ver comentario en hrtf_convolver.hpp): definiciones reales de
// constructor y destructor acá, donde fft_radix2.hpp (incluido arriba)
// da el tipo completo de FFTRadix2 que std::unique_ptr<FFTRadix2>
// necesita. '= default' alcanza para los dos — el compilador genera la
// construcción/destrucción correctas de fft_ automáticamente, solo
// necesitaban verse desde un lugar con el tipo completo visible.
template <typename Scalar>
HRTFConvolverT<Scalar>::HRTFConvolverT() = default;
template <typename Scalar>
HRTFConvolverT<Scalar>::~HRTFConvolverT() = default;

// -----------------------------------------------------------------------------
template <typename Scalar>
uint32_t HRTFConvolverT<Scalar>::next_pow2(uint32_t v) {
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    return ++v;
}

// -----------------------------------------------------------------------------
template <typename Scalar>
void HRTFConvolverT<Scalar>::init(uint32_t sampleRate) {
    sampleRateF_ = static_cast<Scalar>(sampleRate);
    itdLineL_.assign(256, Scalar(0.0));
    itdLineR_.assign(256, Scalar(0.0));
    itdWrite_ = 0; itdSmoothed_ = Scalar(0.0);
    sr_ = sampleRate;
    hrtf_.init(sampleRate, IR_LEN);

    // Carga HRTF medido. FIX (2026-09-10, verificado por grep contra
    // magisk_module/customize.sh): la ruta anterior
    // (/data/adb/ivanna_omega/hrtf_database.bin) NUNCA existio en el
    // dispositivo — customize.sh despliega los sujetos IHR1 en
    // /data/adb/ivanna_omega/hrtf/*.ihr1 con indice SHA-256, y no hay
    // ningun .bin en el modulo. Este camino caia SIEMPRE al sintetico:
    // el HRTF medido (la promesa central de espacializacion) nunca
    // llegaba al audio por este convolver. Se adopta la misma cadena de
    // busqueda que IvannaFusionCore.cpp:62 ya usa con exito: dataset
    // personalizado del usuario primero, KEMAR medido del modulo despues.
    // El bridge (ya endurecido) valida cabecera/tamano antes de reservar.
    //
    // FIX (2026-09-10, frente DSP): confirmado con `find` sobre los
    // assets reales que van 12 sujetos .ihr1 empaquetados — 7 son
    // sujetos humanos CIPIC medidos de verdad (cipic_003..cipic_165),
    // no solo KEMAR. No reordeno cual va PRIMERO — KEMAR como default
    // es una decision de producto razonable (referencia estandar de la
    // industria, no un bug), y no es mi llamada decidir que sujeto
    // "suena mejor". Pero si KEMAR llegara a faltar o venir corrupto en
    // algun build, antes esto caia derecho a sintetico dejando 10
    // sujetos reales sin tocar — ahora se agregan como fallback
    // adicional, mismo patron, sin tocar el orden de prioridad ya
    // establecido para el caso normal.
    const char* hrtfCandidates[] = {
        "/data/adb/ivanna_omega/hrtf_dataset.ihr1",                        // custom del usuario (root)
        "/system/etc/ivanna_omega/hrtf/kemar.ihr1",                        // Magisk mount (root)
        "/data/adb/ivanna_omega/hrtf/kemar.ihr1",                          // KEMAR medido del modulo (root)
        "/data/data/com.ivanna.omega/files/ivanna_omega/hrtf/kemar.ihr1",  // No-Root APK assets extraídos
        "/data/user/0/com.ivanna.omega/files/ivanna_omega/hrtf/kemar.ihr1",// No-Root multi-user path
        "/data/adb/ivanna_omega/hrtf/cipic_003.ihr1",                      // fallback: sujeto humano medido real
        "/data/data/com.ivanna.omega/files/ivanna_omega/hrtf/cipic_003.ihr1",
        "/data/adb/ivanna_omega/hrtf/cipic_165.ihr1",                      // fallback: otro sujeto humano medido real
        "/data/data/com.ivanna.omega/files/ivanna_omega/hrtf/cipic_165.ihr1",
        "/data/adb/ivanna_omega/hrtf/kemar_large.ihr1",                    // fallback: variante KEMAR
        "/data/data/com.ivanna.omega/files/ivanna_omega/hrtf/kemar_large.ihr1"
    };

    bool hrtfLoaded = false;
    const char* hrtfLoadedFrom = nullptr;
    for (const char* candidate : hrtfCandidates) {
        if (Ivanna::SafHRTFDatasetBridge::load(hrtf_, candidate, sampleRate)) {
            hrtfLoaded = true;
            hrtfLoadedFrom = candidate;
            break;
        }
    }

    measuredDatasetLoaded_ = hrtfLoaded;

    // Cargar también pca_basis.bin si está en disco (Root o No-Root);
    // si no está, SyntheticHRTF::init() ya precargó la base maestra SOFA-PCA desde ROM constexpr.
    const char* pcaCandidates[] = {
        "/system/etc/ivanna_omega/pca_basis.bin",
        "/data/adb/ivanna_omega/pca_basis.bin",
        "/data/data/com.ivanna.omega/files/ivanna_omega/pca_basis.bin",
        "/data/user/0/com.ivanna.omega/files/ivanna_omega/pca_basis.bin"
    };
    for (const char* pcaPath : pcaCandidates) {
        if (hrtf_.loadPcaBasis(pcaPath)) {
            break;
        }
    }

    // Inicializar SafSpatialModifier con el modelo maestro SOFA-SAF
    Ivanna::SAFModel masterModel;
    masterModel.p0.assign(
        ivanna::master::kMasterSofaP0,
        ivanna::master::kMasterSofaP0 + ivanna::master::kMasterHrirVecLen
    );
    masterModel.V.resize(ivanna::master::kMasterSafK);
    for (int k = 0; k < ivanna::master::kMasterSafK; ++k) {
        masterModel.V[k].assign(
            ivanna::master::kMasterSofaPcaV[k],
            ivanna::master::kMasterSofaPcaV[k] + ivanna::master::kMasterHrirVecLen
        );
    }
    safModifier_.init(masterModel);
    hrtf_.setLatentParams(ivanna::master::kMasterSafGoldenQ);

    if (hrtfLoaded) {
        __android_log_print(
                ANDROID_LOG_INFO,
                "IVANNA_HRTF",
                "HRTF: custom dataset loaded from %s",
                hrtfLoadedFrom
            );
    } else {
        __android_log_print(
                ANDROID_LOG_WARN,
                "IVANNA_HRTF",
                "HRTF: synthetic fallback"
            );
    }

    ivanna::audio::enableAudioThreadFastMathOnce();   // Si existe en tu código
    fftSize_ = next_pow2(BLOCK + IR_LEN - 1);
    fft_ = std::make_unique<FFTRadix2T<Scalar>>(fftSize_);

    histL_.assign(fftSize_, Scalar(0.0));
    histR_.assign(fftSize_, Scalar(0.0));
    pendingIn_L_.assign(RING_BUFFER_SIZE, Scalar(0.0));
    pendingIn_R_.assign(RING_BUFFER_SIZE, Scalar(0.0));
    outQueue_L_.assign(RING_BUFFER_SIZE, Scalar(0.0));
    outQueue_R_.assign(RING_BUFFER_SIZE, Scalar(0.0));

    inReadPtr_ = 0; inWritePtr_ = 0; inCount_ = 0;
    outReadPtr_ = 0; outWritePtr_ = 0; outCount_ = 0;

    reL_.assign(fftSize_, Scalar(0.0));   imL_.assign(fftSize_, Scalar(0.0));
    reR_.assign(fftSize_, Scalar(0.0));   imR_.assign(fftSize_, Scalar(0.0));
    monoRe_.assign(fftSize_, Scalar(0.0)); monoIm_.assign(fftSize_, Scalar(0.0));
    yReL_.assign(fftSize_, Scalar(0.0));  yImL_.assign(fftSize_, Scalar(0.0));
    yReR_.assign(fftSize_, Scalar(0.0));  yImR_.assign(fftSize_, Scalar(0.0));

    hrir_L_current_.assign(fftSize_, Scalar(0.0)); hrir_R_current_.assign(fftSize_, Scalar(0.0));
    hrir_L_target_.assign(fftSize_, Scalar(0.0));  hrir_R_target_.assign(fftSize_, Scalar(0.0));
    H_ReL_curr_.assign(fftSize_, Scalar(0.0));     H_ImL_curr_.assign(fftSize_, Scalar(0.0));
    H_ReR_curr_.assign(fftSize_, Scalar(0.0));     H_ImR_curr_.assign(fftSize_, Scalar(0.0));
    H_ReL_targ_.assign(fftSize_, Scalar(0.0));     H_ImL_targ_.assign(fftSize_, Scalar(0.0));
    H_ReR_targ_.assign(fftSize_, Scalar(0.0));     H_ImR_targ_.assign(fftSize_, Scalar(0.0));

    currentAzimuth_       = 0.0f;
    currentAggressiveness_= 0.5f;
    targetAzimuth_.store(0.0f, std::memory_order_relaxed);
    targetAggressiveness_.store(0.5f, std::memory_order_relaxed);
    newTargetPending_.store(false, std::memory_order_relaxed);

    updateFilterResponses(0.0f, 0.5f, true);
    xfadeSamplesRemaining_.store(0, std::memory_order_relaxed);
    filterInitialized_ = true;
}

// -----------------------------------------------------------------------------
// Reinicia el estado dinámico (buffers, historial, crossfade) preservando la
// configuración ya calculada por init() (sr_, fftSize_, fft_). No reasigna
// FFTRadix2 ni reinicia SyntheticHRTF (no tienen estado dependiente de la
// posición). Usado por ObjectRenderer::reset() al soltar un virtual speaker.
template <typename Scalar>
void HRTFConvolverT<Scalar>::reset() noexcept {
    std::fill(itdLineL_.begin(), itdLineL_.end(), Scalar(0.0));
    std::fill(itdLineR_.begin(), itdLineR_.end(), Scalar(0.0));
    itdWrite_ = 0; itdSmoothed_ = Scalar(0.0);
    if (!filterInitialized_) return;

    std::fill(histL_.begin(), histL_.end(), Scalar(0.0));
    std::fill(histR_.begin(), histR_.end(), Scalar(0.0));
    std::fill(pendingIn_L_.begin(), pendingIn_L_.end(), Scalar(0.0));
    std::fill(pendingIn_R_.begin(), pendingIn_R_.end(), Scalar(0.0));
    std::fill(outQueue_L_.begin(), outQueue_L_.end(), Scalar(0.0));
    std::fill(outQueue_R_.begin(), outQueue_R_.end(), Scalar(0.0));

    inReadPtr_ = 0; inWritePtr_ = 0; inCount_ = 0;
    outReadPtr_ = 0; outWritePtr_ = 0; outCount_ = 0;

    currentAzimuth_        = 0.0f;
    currentAggressiveness_ = 0.5f;
    targetAzimuth_.store(0.0f, std::memory_order_relaxed);
    targetAggressiveness_.store(0.5f, std::memory_order_relaxed);
    newTargetPending_.store(false, std::memory_order_relaxed);
    xfadeSamplesRemaining_.store(0, std::memory_order_relaxed);

    updateFilterResponses(0.0f, 0.5f, true);
}

// -----------------------------------------------------------------------------
template <typename Scalar>
void HRTFConvolverT<Scalar>::set_position(float azimuthDeg, float aggressiveness) noexcept {
    while (azimuthDeg < -180.0f) azimuthDeg += 360.0f;
    while (azimuthDeg > 180.0f)  azimuthDeg -= 360.0f;
    aggressiveness = std::clamp(aggressiveness, 0.0f, 1.0f);

    float oldAz  = targetAzimuth_.load(std::memory_order_relaxed);
    float oldAgg = targetAggressiveness_.load(std::memory_order_relaxed);

    if (std::abs(azimuthDeg - oldAz) > 0.1f || std::abs(aggressiveness - oldAgg) > 0.01f) {
        targetAzimuth_.store(azimuthDeg, std::memory_order_release);
        targetAggressiveness_.store(aggressiveness, std::memory_order_release);
        newTargetPending_.store(true, std::memory_order_release);
    }
}

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// AUDIT FIX (SOFA nunca llegaba al audio): los 5 archivos .sofa de
// assets/sofa/ (sujetos NH del dataset LISTEN) tenían cargador
// (SofaHRTFLoader) compilado pero sin un solo call-site — la HRTF siempre
// salía del dataset IHR1 o del modelo sintético. Esta entrada inyecta el
// IR medido directo en los búferes del convolver. Se ejecuta en el hilo de
// control (resize fuera del hot path); el hilo de audio solo lee los
// búferes ya fijos dentro de updateFilterResponses(), protegido por el
// seqlock de newTargetPending_ igual que cualquier cambio de azimut.
template <typename Scalar>
bool HRTFConvolverT<Scalar>::loadCustomHrir(const float* irL, const float* irR, size_t len) noexcept {
    if (!irL || !irR || len == 0) return false;
    const size_t n = len < static_cast<size_t>(IR_LEN) ? len : static_cast<size_t>(IR_LEN);
    if (customIrL_.size() != static_cast<size_t>(IR_LEN)) {
        customIrL_.assign(IR_LEN, Scalar(0.0));
        customIrR_.assign(IR_LEN, Scalar(0.0));
    }
    std::fill(customIrL_.begin(), customIrL_.end(), Scalar(0.0));
    std::fill(customIrR_.begin(), customIrR_.end(), Scalar(0.0));
    std::copy(irL, irL + n, customIrL_.begin());
    std::copy(irR, irR + n, customIrR_.begin());
    customHrirActive_.store(true, std::memory_order_release);
    newTargetPending_.store(true, std::memory_order_release);
    return true;
}

// -----------------------------------------------------------------------------
template <typename Scalar>
void HRTFConvolverT<Scalar>::updateFilterResponses(float azimuthDeg, float aggressiveness, bool immediate) noexcept {
    // Fuente del IR: HRIR medido (SOFA) si está activo, si no el sintético.
    // FIX RT (auditoría 2026-09-22, REVERTIDO por accidente en cc23618c y
    // restaurado 2026-09-24): antes se copiaba el resultado de generate() a
    // un HRIRPair LOCAL (synthHolder), fresco en cada llamada -> malloc de
    // sus dos std::vector en cada cambio de azimut/agresividad, en pleno
    // hilo de audio SCHED_FIFO. generate() ahora devuelve una referencia a
    // un scratch persistente de SyntheticHRTF (preasignado, ver
    // synthetic_hrtf.hpp): se apunta directo a ese scratch, sin copia local.
    // Es seguro leerlo aquí mismo (un solo hilo llama a este método por
    // instancia, y el scratch se consume vía std::copy más abajo, antes de
    // cualquier llamada subsiguiente a generate()).
    // F2/486: el IR medido vive ya en Scalar (customIrL_/R_); el sintético
    // sigue siendo float (SyntheticHRTF) y se castea al copiar. Por eso
    // las dos fuentes no comparten un solo puntero: cada rama copia
    // desde su propio vector.
    const bool useCustom = customHrirActive_.load(std::memory_order_acquire);
    const HRIRPair* synth = nullptr;
    if (!useCustom) {
        synth = &hrtf_.generate(azimuthDeg, aggressiveness);
    }

    const size_t copyL = std::min(useCustom ? customIrL_.size() : synth->L.size(),
                                  static_cast<size_t>(IR_LEN));
    const size_t copyR = std::min(useCustom ? customIrR_.size() : synth->R.size(),
                                  static_cast<size_t>(IR_LEN));

    if (immediate) {
        std::fill(H_ReL_curr_.begin(), H_ReL_curr_.end(), Scalar(0.0));
        std::fill(H_ImL_curr_.begin(), H_ImL_curr_.end(), Scalar(0.0));
        std::fill(H_ReR_curr_.begin(), H_ReR_curr_.end(), Scalar(0.0));
        std::fill(H_ImR_curr_.begin(), H_ImR_curr_.end(), Scalar(0.0));
        if (copyL > 0) {
            if (useCustom) std::copy(customIrL_.begin(), customIrL_.begin() + copyL, H_ReL_curr_.begin());
            else std::copy(synth->L.begin(), synth->L.begin() + copyL, H_ReL_curr_.begin());
        }
        if (copyR > 0) {
            if (useCustom) std::copy(customIrR_.begin(), customIrR_.begin() + copyR, H_ReR_curr_.begin());
            else std::copy(synth->R.begin(), synth->R.begin() + copyR, H_ReR_curr_.begin());
        }
        fft_->forward(H_ReL_curr_.data(), H_ImL_curr_.data());
        fft_->forward(H_ReR_curr_.data(), H_ImR_curr_.data());

        currentAzimuth_        = azimuthDeg;
        currentAggressiveness_ = aggressiveness;
        xfadeSamplesRemaining_.store(0, std::memory_order_relaxed);
    } else {
        std::fill(H_ReL_targ_.begin(), H_ReL_targ_.end(), Scalar(0.0));
        std::fill(H_ImL_targ_.begin(), H_ImL_targ_.end(), Scalar(0.0));
        std::fill(H_ReR_targ_.begin(), H_ReR_targ_.end(), Scalar(0.0));
        std::fill(H_ImR_targ_.begin(), H_ImR_targ_.end(), Scalar(0.0));
        if (copyL > 0) {
            if (useCustom) std::copy(customIrL_.begin(), customIrL_.begin() + copyL, H_ReL_targ_.begin());
            else std::copy(synth->L.begin(), synth->L.begin() + copyL, H_ReL_targ_.begin());
        }
        if (copyR > 0) {
            if (useCustom) std::copy(customIrR_.begin(), customIrR_.begin() + copyR, H_ReR_targ_.begin());
            else std::copy(synth->R.begin(), synth->R.begin() + copyR, H_ReR_targ_.begin());
        }
        fft_->forward(H_ReL_targ_.data(), H_ImL_targ_.data());
        fft_->forward(H_ReR_targ_.data(), H_ImR_targ_.data());
    }
}


// -----------------------------------------------------------------------------
template <typename Scalar>
Scalar HRTFConvolverT<Scalar>::computeItdSamples(float azimuthDeg) const noexcept {
    if (sampleRateF_ <= Scalar(0.0)) return Scalar(0.0);
    // Woodworth (cabeza esferica): ITD = (r/c) * (sin(theta) + theta),
    // theta en radianes limitado al hemisferio frontal |theta| <= pi/2.
    // Constantes en double exacto: en float dan los literales f históricos.
    const Scalar theta = std::clamp(static_cast<Scalar>(azimuthDeg) * Scalar(0.017453292519943295), Scalar(-1.5707963267948966), Scalar(1.5707963267948966));
    const Scalar itdSec = (Scalar(0.0875) / Scalar(343.0)) * (std::sin(theta) + theta);
    return std::clamp(itdSec * sampleRateF_, -static_cast<Scalar>(kMaxItdSamples), static_cast<Scalar>(kMaxItdSamples));
}

template <typename Scalar>
void HRTFConvolverT<Scalar>::applyItd(Scalar* outputL, Scalar* outputR, uint32_t n) noexcept {
    if (sampleRateF_ <= Scalar(0.0) || itdLineL_.empty()) return;
    const Scalar target = computeItdSamples(targetAzimuth_.load(std::memory_order_relaxed));
    // One-pole ~1.5 ms: los movimientos de cabeza/fuente no producen zipper noise
    const Scalar kSmooth = Scalar(1.0) - std::exp(-Scalar(1.0) / (Scalar(0.0015) * sampleRateF_));
    const uint32_t N = static_cast<uint32_t>(itdLineL_.size());
    for (uint32_t i = 0; i < n; ++i) {
        itdSmoothed_ += (target - itdSmoothed_) * kSmooth;
        const Scalar dL = itdSmoothed_ > Scalar(0.0) ?  itdSmoothed_ : Scalar(0.0);
        const Scalar dR = itdSmoothed_ < Scalar(0.0) ? -itdSmoothed_ : Scalar(0.0);
        itdLineL_[itdWrite_] = outputL[i];
        itdLineR_[itdWrite_] = outputR[i];
        const int32_t w = static_cast<int32_t>(itdWrite_);
        auto readFrac = [&](const std::vector<Scalar>& line, Scalar d) -> Scalar {
            if (d < Scalar(0.001)) return line[itdWrite_]; // camino rapido: sin delay
            const Scalar rp = static_cast<Scalar>(w) - d;
            int32_t i0 = static_cast<int32_t>(std::floor(rp));
            const Scalar frac = rp - static_cast<Scalar>(i0);
            i0 = ((i0 % static_cast<int32_t>(N)) + N) % N;
            const int32_t i1 = (i0 + 1) % static_cast<int32_t>(N);
            return line[i0] * (Scalar(1.0) - frac) + line[i1] * frac;
        };
        outputL[i] = readFrac(itdLineL_, dL);
        outputR[i] = readFrac(itdLineR_, dR);
        itdWrite_ = (itdWrite_ + 1) % N;
    }
}

// -----------------------------------------------------------------------------
template <typename Scalar>
void HRTFConvolverT<Scalar>::process(const Scalar* inputL, const Scalar* inputR,
                            Scalar* outputL, Scalar* outputR,
                            uint32_t numSamples) noexcept {
    enableDenormalGuard();
    if (!filterInitialized_ || !inputL || !inputR || !outputL || !outputR || numSamples == 0) {
        // FIX defensivo: antes se hacía memcpy incondicional aquí, pero si la
        // razón de entrar a este bypass era justamente inputL/inputR nulos
        // (una de las condiciones de arriba), el memcpy siguiente crasheaba
        // por leer de un puntero nulo. Ahora solo copia si los punteros de
        // entrada Y salida son válidos.
        if (inputL && outputL && outputL != inputL) std::memcpy(outputL, inputL, numSamples * sizeof(Scalar));
        if (inputR && outputR && outputR != inputR) std::memcpy(outputR, inputR, numSamples * sizeof(Scalar));
        return;
    }

    // 0. Garantizar invariante de conservación de muestras (inCount_ + outCount_ >= BLOCK)
    //    cuando numSamples no es múltiplo exacto de BLOCK (ej. 128, 192 o 240 en AudioFlinger).
    //    Cuando numSamples es múltiplo de BLOCK (256, 512), projectedOut >= numSamples desde
    //    el primer bloque y no se añade ninguna muestra de cebado.
    const uint32_t projectedOut = outCount_ + ((inCount_ + numSamples) / static_cast<uint32_t>(BLOCK)) * static_cast<uint32_t>(BLOCK);
    if (projectedOut < numSamples && (inCount_ + outCount_) < static_cast<uint32_t>(BLOCK)) {
        const uint32_t needPrime = static_cast<uint32_t>(BLOCK) - (inCount_ + outCount_);
        for (uint32_t p = 0; p < needPrime && outCount_ < RING_BUFFER_SIZE; ++p) {
            outQueue_L_[outWritePtr_] = Scalar(0.0);
            outQueue_R_[outWritePtr_] = Scalar(0.0);
            outWritePtr_ = (outWritePtr_ + 1) % RING_BUFFER_SIZE;
            ++outCount_;
        }
    }

    // 1. Inserción al búfer circular de entrada
    for (uint32_t i = 0; i < numSamples; ++i) {
        if (inCount_ < RING_BUFFER_SIZE) {
            const Scalar sL = std::isfinite(inputL[i]) ? inputL[i] : Scalar(0.0);
            const Scalar sR = std::isfinite(inputR[i]) ? inputR[i] : Scalar(0.0);
            pendingIn_L_[inWritePtr_] = sL;
            pendingIn_R_[inWritePtr_] = sR;
            inWritePtr_ = (inWritePtr_ + 1) % RING_BUFFER_SIZE;
            ++inCount_;
        }
    }

    // 2. Procesar bloques completos (Overlap-Save)
    while (inCount_ >= static_cast<uint32_t>(BLOCK)) {
        // --- 2a. Verificar si hay que iniciar un crossfade ---
        bool pending = newTargetPending_.load(std::memory_order_acquire);
        int xfadeRemaining = xfadeSamplesRemaining_.load(std::memory_order_relaxed);
        if (pending && xfadeRemaining == 0) {
            float tAz = targetAzimuth_.load(std::memory_order_relaxed);
            float tAgg = targetAggressiveness_.load(std::memory_order_relaxed);
            newTargetPending_.store(false, std::memory_order_relaxed);

            if (std::abs(tAz - currentAzimuth_) > 0.1f ||
                std::abs(tAgg - currentAggressiveness_) > 0.01f ||
                customHrirActive_.load(std::memory_order_relaxed)) {
                updateFilterResponses(tAz, tAgg, false);
                xfadeRemaining = XFADE_DURATION_SAMPLES;
                xfadeSamplesRemaining_.store(xfadeRemaining, std::memory_order_relaxed);
            }
        }

        // 2b. Mover historial y leer nuevo bloque
        int overlapSize = fftSize_ - BLOCK;
        std::memmove(histL_.data(), histL_.data() + BLOCK, overlapSize * sizeof(Scalar));
        std::memmove(histR_.data(), histR_.data() + BLOCK, overlapSize * sizeof(Scalar));
        for (int i = 0; i < BLOCK; ++i) {
            histL_[overlapSize + i] = pendingIn_L_[inReadPtr_];
            histR_[overlapSize + i] = pendingIn_R_[inReadPtr_];
            inReadPtr_ = (inReadPtr_ + 1) % RING_BUFFER_SIZE;
        }
        inCount_ -= BLOCK;

        // 2c. Preparar mono y FFT
        for (int i = 0; i < fftSize_; ++i) {
            Scalar s = histL_[i] + histR_[i];
            if (!std::isfinite(s)) {
                std::fill(histL_.begin(), histL_.end(), Scalar(0.0));
                std::fill(histR_.begin(), histR_.end(), Scalar(0.0));
                s = Scalar(0.0);
            }
            monoRe_[i] = s * Scalar(0.5);
            monoIm_[i] = Scalar(0.0);
            if (std::abs(monoRe_[i]) < Scalar(1e-30)) monoRe_[i] = Scalar(0.0);
        }
        fft_->forward(monoRe_.data(), monoIm_.data());

        // 2d. Convolución con filtro actual (A): NEON float en ARM para
        // Scalar=float (bit-idéntico); escalar double en el núcleo double.
        multiplySpectra<Scalar>(monoRe_.data(), monoIm_.data(), H_ReL_curr_.data(), H_ImL_curr_.data(), yReL_.data(), yImL_.data(), fftSize_);
        multiplySpectra<Scalar>(monoRe_.data(), monoIm_.data(), H_ReR_curr_.data(), H_ImR_curr_.data(), yReR_.data(), yImR_.data(), fftSize_);
        fft_->inverse(yReL_.data(), yImL_.data());
        fft_->inverse(yReR_.data(), yImR_.data());
        const Scalar scale = Scalar(1.0) / static_cast<Scalar>(fftSize_);

        // 2e. Salida con o sin crossfade (SIEMPRE produce exactamente BLOCK muestras)
        xfadeRemaining = xfadeSamplesRemaining_.load(std::memory_order_relaxed);
        if (xfadeRemaining > 0) {
            // Convolución con filtro destino (B): igual que 2d.
            multiplySpectra<Scalar>(monoRe_.data(), monoIm_.data(), H_ReL_targ_.data(), H_ImL_targ_.data(), reL_.data(), imL_.data(), fftSize_);
            multiplySpectra<Scalar>(monoRe_.data(), monoIm_.data(), H_ReR_targ_.data(), H_ImR_targ_.data(), reR_.data(), imR_.data(), fftSize_);
            fft_->inverse(reL_.data(), imL_.data());
            fft_->inverse(reR_.data(), imR_.data());

            // FIX CRÍTICO (eliminación del sonido de hélice / pérdida de muestras):
            // Jamás hacer `break` cuando `xfadeRemaining` llega a 0 a mitad de bloque,
            // porque `inCount_` ya consumió las `BLOCK` muestras. Si `xfadeRemaining`
            // llega a 0 dentro del bloque, el resto del bloque se emite directamente con
            // el filtro destino (progress = 1.0), conservando 1:1 el flujo de muestras.
            for (int i = 0; i < BLOCK; ++i) {
                int outIdx = overlapSize + i;
                Scalar currL = yReL_[outIdx] * scale;
                Scalar currR = yReR_[outIdx] * scale;
                Scalar targL = reL_[outIdx] * scale;
                Scalar targR = reR_[outIdx] * scale;

                Scalar finalL = targL;
                Scalar finalR = targR;
                if (xfadeRemaining > 0) {
                    Scalar progress = Scalar(1.0) - (static_cast<Scalar>(xfadeRemaining) / static_cast<Scalar>(XFADE_DURATION_SAMPLES));
                    progress = std::clamp(progress, Scalar(0.0), Scalar(1.0));
                    const Scalar wCurr = std::sqrt(Scalar(1.0) - progress);
                    const Scalar wTarg = std::sqrt(progress);
                    finalL = wCurr * currL + wTarg * targL;
                    finalR = wCurr * currR + wTarg * targR;
                    --xfadeRemaining;
                }

                if (outCount_ < RING_BUFFER_SIZE) {
                    outQueue_L_[outWritePtr_] = finalL;
                    outQueue_R_[outWritePtr_] = finalR;
                    outWritePtr_ = (outWritePtr_ + 1) % RING_BUFFER_SIZE;
                    ++outCount_;
                }
            }

            xfadeSamplesRemaining_.store(xfadeRemaining, std::memory_order_relaxed);

            // Si se consumió todo el crossfade, promover el filtro destino a actual
            if (xfadeRemaining <= 0) {
                std::swap(H_ReL_curr_, H_ReL_targ_);
                std::swap(H_ImL_curr_, H_ImL_targ_);
                std::swap(H_ReR_curr_, H_ReR_targ_);
                std::swap(H_ImR_curr_, H_ImR_targ_);
                currentAzimuth_  = targetAzimuth_.load(std::memory_order_relaxed);
                currentAggressiveness_ = targetAggressiveness_.load(std::memory_order_relaxed);
            }
        } else {
            // Sin crossfade: salida directa
            for (int i = 0; i < BLOCK; ++i) {
                int outIdx = overlapSize + i;
                if (outCount_ < RING_BUFFER_SIZE) {
                    outQueue_L_[outWritePtr_] = yReL_[outIdx] * scale;
                    outQueue_R_[outWritePtr_] = yReR_[outIdx] * scale;
                    outWritePtr_ = (outWritePtr_ + 1) % RING_BUFFER_SIZE;
                    ++outCount_;
                }
            }
        }
    }

    // 3. Entrega de muestras al buffer de salida del sistema
    uint32_t samplesToDeliver = std::min(numSamples, outCount_);
    Scalar lastDeliveredL = Scalar(0.0);
    Scalar lastDeliveredR = Scalar(0.0);
    for (uint32_t i = 0; i < samplesToDeliver; ++i) {
        lastDeliveredL = outQueue_L_[outReadPtr_];
        lastDeliveredR = outQueue_R_[outReadPtr_];
        outputL[i] = lastDeliveredL;
        outputR[i] = lastDeliveredR;
        outReadPtr_ = (outReadPtr_ + 1) % RING_BUFFER_SIZE;
        --outCount_;
    }

    if (samplesToDeliver < numSamples) {
        for (uint32_t i = samplesToDeliver; i < numSamples; ++i) {
            lastDeliveredL *= Scalar(0.95);
            lastDeliveredR *= Scalar(0.95);
            outputL[i] = lastDeliveredL;
            outputR[i] = lastDeliveredR;
        }
    }

    // 4. ITD interaural: delay fraccional por oido (post-convolucion)
    applyItd(outputL, outputR, numSamples);
}

} // namespace ivanna


template <typename Scalar>
void ivanna::HRTFConvolverT<Scalar>::updateSafField(
    const std::array<float,7>& q,
    float azimuth
)
{
    hrtf_.setLatentParams(q.data());

    if(!safModifier_.update(
        q,
        hrtf_,
        azimuth))
    {
        newTargetPending_.store(true, std::memory_order_release);
        return;
    }

    const HRIRPair& h = safModifier_.current();
    // Publicar el HRIR SAF mediante la cola lock-free loadCustomHrir / newTargetPending_
    // para que el FFT se ejecute exclusivamente en el hilo de audio sin carrera de datos
    // sobre H_ReL_targ_ / fft_ durante process().
    const size_t len = std::min(h.L.size(), h.R.size());
    if (len > 0) {
        loadCustomHrir(h.L.data(), h.R.data(), len);
    } else {
        newTargetPending_.store(true, std::memory_order_release);
    }
}

// Instanciaciones explícitas (las definiciones viven en este .cpp):
// float = ruta legacy bit-idéntica; double = núcleo de la labor grande.
template class ivanna::HRTFConvolverT<float>;
template class ivanna::HRTFConvolverT<double>;
