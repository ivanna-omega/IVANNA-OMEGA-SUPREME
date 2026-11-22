// spatial/RirConvolver.cpp — Convolucionador RIR overlap-save
// Ver RirConvolver.hpp para la documentación completa.

#include "RirConvolver.hpp"
#include "SofaSafRirMasterKnowledge.hpp"
#include "SafmusLatentGuard.hpp"
#include <cmath>
#include <algorithm>

#if defined(__x86_64__) || defined(__i386__)
  #include <immintrin.h>
#endif
namespace {
// FTZ/DAZ anti-denormales: la convolucion RIR arrastra colas que decaen a
// subnormales y disparan microcode assists -> picos de CPU -> micro-cortes.
inline void enableRirDenormalGuard() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ volatile("msr fpcr, %0" :: "r"(fpcr));
#endif
}
} // namespace

namespace Ivanna {

// ── FFT keystone en double (F2/486) ─────────────────────────────────────────
// La FFT propia (fftReal + twiddles float) se elimina: todas las
// llamadas usan fft1024_ (FFTRadix2d, misma convención: forward
// exp(-2πi), inversa conjugada con escala 1/N). El fix histórico del
// error de fase por twiddle recursivo en float queda subsumido: la
// keystone precalcula sus tablas en double desde cos/sin exactos.
RirConvolver::RirConvolver() {
    std::memset(irReL_, 0, sizeof irReL_);
    std::memset(irImL_, 0, sizeof irImL_);
    std::memset(irReR_, 0, sizeof irReR_);
    std::memset(irImR_, 0, sizeof irImR_);
    std::memset(overlapL_, 0, sizeof overlapL_);
    std::memset(overlapR_, 0, sizeof overlapR_);
    const size_t totalFloats = static_cast<size_t>(TAIL_PARTS) * FFT_SIZE;
    tailIrReL_.assign(totalFloats, 0.);
    tailIrImL_.assign(totalFloats, 0.);
    tailIrReR_.assign(totalFloats, 0.);
    tailIrImR_.assign(totalFloats, 0.);
    pendTailIrReL_.assign(totalFloats, 0.);
    pendTailIrImL_.assign(totalFloats, 0.);
    pendTailIrReR_.assign(totalFloats, 0.);
    pendTailIrImR_.assign(totalFloats, 0.);
    fdlReL_.assign(totalFloats, 0.);
    fdlImL_.assign(totalFloats, 0.);
    fdlReR_.assign(totalFloats, 0.);
    fdlImR_.assign(totalFloats, 0.);
}

void RirConvolver::applySofaCoupling(const float q[7]) noexcept {
    // SAFMUS boundary: retain the previous safe room coupling on invalid input.
    if (!ivanna::safmus::isFiniteLatent(q)) return;
    float earlyBoost = 1.08f, lateDecorrel = 0.62f, rt60Delta = 0.0f, wetScale = 1.0f;
    ivanna::master::computeSofaRirCoupling(q, earlyBoost, lateDecorrel, rt60Delta, wetScale);
    earlyBoost_.store(earlyBoost, std::memory_order_relaxed);
    decorrel_.store(lateDecorrel, std::memory_order_relaxed);
    wetScale_.store(std::clamp(wetScale, 0.75f, 1.25f), std::memory_order_relaxed);
    rt60Delta_.store(std::clamp(rt60Delta, -0.15f, 0.15f), std::memory_order_relaxed);
}

bool RirConvolver::couplingStale() const noexcept {
    if (rawL_.empty() || bakedEarly_.load(std::memory_order_relaxed) < 0.f) return false;
    auto d = [](float a, float b) { return std::fabs(a - b); };
    return d(earlyBoost_.load(std::memory_order_relaxed), bakedEarly_.load(std::memory_order_relaxed)) > 0.01f ||
           d(decorrel_.load(std::memory_order_relaxed), bakedDecorrel_.load(std::memory_order_relaxed)) > 0.02f ||
           d(rt60Delta_.load(std::memory_order_relaxed), bakedRt60Delta_.load(std::memory_order_relaxed)) > 0.005f;
}

bool RirConvolver::relinkFromRaw() noexcept {
    if (rawL_.empty() || rawL_.size() != rawR_.size()) return false;
    std::vector<float> l = rawL_, r = rawR_;   // copia: load() reescribe raw
    load(l.data(), r.data(), static_cast<int>(l.size()));
    relinks_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

RirConvolver::Evidence RirConvolver::evidence() const noexcept {
    Evidence e;
    e.loads = loads_.load(std::memory_order_relaxed);
    e.relinks = relinks_.load(std::memory_order_relaxed);
    e.activeBlocks = activeBlocks_.load(std::memory_order_relaxed);
    e.inEnergy = inEnergy_.load(std::memory_order_relaxed);
    e.outEnergy = outEnergy_.load(std::memory_order_relaxed);
    e.bakedEarly = bakedEarly_.load(std::memory_order_relaxed);
    e.bakedDecorrel = bakedDecorrel_.load(std::memory_order_relaxed);
    e.bakedRt60Delta = bakedRt60Delta_.load(std::memory_order_relaxed);
    e.wetScale = wetScale_.load(std::memory_order_relaxed);
    return e;
}

void RirConvolver::synthesizeMasterStudioBrir(float rt60S, int sampleRate) noexcept {
    const int sr = (sampleRate > 8000) ? sampleRate : 48000;
    setSampleRate(sr);
    const float rt60 = std::clamp(rt60S, 0.18f, 1.80f);
    if (!needsMasterStudioBrirSynthesis(rt60, sr)) {
        return;
    }
    const int irLen = std::min(MAX_IR + 4 * BLOCK, static_cast<int>(rt60 * static_cast<float>(sr)));
    std::vector<float> irL(irLen, 0.0f), irR(irLen, 0.0f);

    // Reflexiones tempranas de sala de control ITU-R BS.1116
    // NOTA ACÚSTICA: El camino directo llega intacto vía dry * inL (latencia 0.00 ms).
    // Para evitar desfase o duplicación del diálogo (efecto slapback / comb-filter en películas),
    // el convolucionador de sala genera exclusivamente el campo reflejado y la cola difusa.
    const int erTap1 = std::min(irLen - 1, static_cast<int>(0.0042f * sr));
    const int erTap2 = std::min(irLen - 1, static_cast<int>(0.0079f * sr));
    const int erTap3 = std::min(irLen - 1, static_cast<int>(0.0134f * sr));
    irL[erTap1] += 0.22f; irR[erTap1] += 0.18f;
    irL[erTap2] -= 0.15f; irR[erTap2] += 0.19f;
    irL[erTap3] += 0.12f; irR[erTap3] -= 0.14f;

    // Cola difusa binaural descorrelacionada con decaimiento exponencial (-60 dB en rt60)
    const float decayRate = 6.907755f / (rt60 * static_cast<float>(sr));
    uint32_t seedL = 0x9E3779B9u;
    uint32_t seedR = 0x85EBCA6Bu;
    float normL = 0.0f, normR = 0.0f;
    for (int n = 0; n < irLen; ++n) {
        if (n > erTap1) {
            seedL = seedL * 1664525u + 1013904223u;
            seedR = seedR * 22695477u + 1u;
            const float nL = (static_cast<float>(static_cast<int32_t>(seedL)) / 2147483648.0f);
            const float nR = (static_cast<float>(static_cast<int32_t>(seedR)) / 2147483648.0f);
            const float env = 0.035f * std::exp(-decayRate * static_cast<float>(n));
            irL[n] += env * nL;
            irR[n] += env * nR;
        }
        normL += irL[n] * irL[n];
        normR += irR[n] * irR[n];
    }
    const float invNorm = 1.0f / std::sqrt(std::max(1e-6f, 0.5f * (normL + normR)));
    for (int n = 0; n < irLen; ++n) {
        irL[n] *= invNorm;
        irR[n] *= invNorm;
    }
    load(irL.data(), irR.data(), irLen);
    lastSynthRt60_ = rt60;
    lastSynthSr_   = sr;
}

void RirConvolver::load(const float* irL, const float* irR, int irLen) noexcept {
    if (!irL || !irR || irLen <= 0) return;
    const bool tailOverBudget = irLen > MAX_IR && !tailLatencyWithinBudget();
    tailLatencyBudgetExceeded_.store(tailOverBudget, std::memory_order_release);
    // The direct head is zero-latency. If one 512-frame tail block would exceed
    // SAFMUS's 20 ms budget, keep the safe head and omit only the delayed tail.
    if (tailOverBudget) irLen = MAX_IR;
    lastSynthRt60_ = -1.0;
    lastSynthSr_   = 0;

    // Snapshot del acoplamiento SAF que se hornea en esta carga + IR cruda para re-acoplar.
    const double snapEarly = earlyBoost_.load(std::memory_order_relaxed);
    const double snapDec   = decorrel_.load(std::memory_order_relaxed);
    const double snapRt    = rt60Delta_.load(std::memory_order_relaxed);
    if (irL != rawL_.data()) {
        const int keep = std::min(irLen, MAX_IR_TOTAL);
        rawL_.assign(irL, irL + keep);
        rawR_.assign(irR, irR + keep);
    }

    // ── FIX CRÍTICO: Eliminación de DC + Alineación Temporal Time-Of-Flight + Supresión de Eco Directo ──
    // En respuestas al impulso de sala (RIR) grabadas en disco, el tiempo de vuelo altavoz->micrófono
    // introduce un retraso acústico de 2 a 12 ms antes de la llegada directa. Si ese pico se convoluciona
    // junto con el camino directo (dry * in), se genera un eco slapback y un filtrado en peine destructivo
    // ("desfase y eco" en Amazon Prime Video / Netflix).
    // Detectamos el inicio directo en los primeros 1024 frames, alineamos al origen y suprimimos la réplica
    // directa redundante para que la convolución inyecte únicamente el campo reverberante limpio.
    int onsetIdx = 0;
    if (irLen >= 128) {
        double maxVal = 0.0;
        const int searchRange = std::min(irLen / 2, 1024);
        for (int i = 0; i < searchRange; ++i) {
            const double aL = std::fabs(irL[i]);
            const double aR = std::fabs(irR[i]);
            if (aL > maxVal) maxVal = aL;
            if (aR > maxVal) maxVal = aR;
        }
        if (maxVal > 1e-4) {
            const double thresh = maxVal * 0.35;
            for (int i = 0; i < searchRange; ++i) {
                if (std::fabs(irL[i]) >= thresh || std::fabs(irR[i]) >= thresh) {
                    onsetIdx = i;
                    break;
                }
            }
        }
    }

    const int availableLen = irLen - onsetIdx;
    const int safeLen = std::min(availableLen > 0 ? availableLen : irLen, MAX_IR_TOTAL);
    const float* srcL = (availableLen > 0) ? (irL + onsetIdx) : irL;
    const float* srcR = (availableLen > 0) ? (irR + onsetIdx) : irR;

    std::vector<double> normIrL(safeLen), normIrR(safeLen);
    double meanL = 0.0, meanR = 0.0;
    for (int i = 0; i < safeLen; ++i) {
        meanL += std::isfinite(srcL[i]) ? srcL[i] : 0.0;
        meanR += std::isfinite(srcR[i]) ? srcR[i] : 0.0;
    }
    meanL /= static_cast<double>(safeLen);
    meanR /= static_cast<double>(safeLen);

    double energySum = 0.0;
    const int fadeLen = std::min(64, safeLen / 4);
    for (int i = 0; i < safeLen; ++i) {
        double vL = (std::isfinite(srcL[i]) ? static_cast<double>(srcL[i]) : 0.0) - meanL;
        double vR = (std::isfinite(srcR[i]) ? static_cast<double>(srcR[i]) : 0.0) - meanR;
        // Supresión suave de espiga directa en origen (primeras 8 muestras) para evitar comb-filter con dry
        if (onsetIdx > 0 && i < 16) {
            const double directNotch = 0.15 + 0.85 * (static_cast<double>(i) / 16.0);
            vL *= directNotch;
            vR *= directNotch;
        }
        if (fadeLen > 0 && i >= safeLen - fadeLen) {
            const double t = static_cast<double>(safeLen - 1 - i) / static_cast<double>(fadeLen);
            const double w = 0.5 * (1.0 - std::cos(3.14159265 * t));
            vL *= w;
            vR *= w;
        }
        normIrL[i] = vL;
        normIrR[i] = vR;
        energySum += static_cast<double>(vL) * vL + static_cast<double>(vR) * vR;
    }
    const double rmsEnergy = static_cast<double>(std::sqrt(std::max(1e-9, 0.5 * energySum)));
    // Target L2 norm = 0.72 (-2.85 dB headroom para evitar saturación en transitorios densos)
    const double invL2 = 0.72 / rmsEnergy;
    for (int i = 0; i < safeLen; ++i) {
        normIrL[i] *= invL2;
        normIrR[i] *= invL2;
    }

    // SAF rt60Delta: envolvente exponencial que cambia la pendiente de decaimiento
    // (T' = T0*(1+delta)); 60 dB de caida = 3*ln10 por T. Solo desde 1 ms (cola/reflexiones).
    if (std::fabs(snapRt) > 1e-3) {
        const double sr = static_cast<double>(sampleRate_.load(std::memory_order_relaxed));
        const double T0 = 0.34, T1 = T0 * (1.0 + snapRt);
        const double k = 6.9077553 * (1.0 / T1 - 1.0 / T0) / sr;  // por muestra
        for (int i = 48; i < safeLen; ++i) {
            const double g = std::min(3.0, std::exp(-k * static_cast<double>(i - 48)));
            normIrL[i] *= g; normIrR[i] *= g;
        }
    }

    // ── Particionado no uniforme: head (latencia 0) + cola (overlap-save) ──
    const int tailLen = (safeLen > MAX_IR) ? (safeLen - MAX_IR) : 0;
    int newTailParts = tailLen > 0 ? (tailLen + BLOCK - 1) / BLOCK : 0;
    if (newTailParts > TAIL_PARTS) newTailParts = TAIL_PARTS;

    const size_t totalFloats = static_cast<size_t>(TAIL_PARTS) * FFT_SIZE;
    if (pendTailIrReL_.size() < totalFloats) {
        pendTailIrReL_.assign(totalFloats, 0.);
        pendTailIrImL_.assign(totalFloats, 0.);
        pendTailIrReR_.assign(totalFloats, 0.);
        pendTailIrImR_.assign(totalFloats, 0.);
    } else {
        std::fill(pendTailIrReL_.begin(), pendTailIrReL_.end(), 0.);
        std::fill(pendTailIrImL_.begin(), pendTailIrImL_.end(), 0.);
        std::fill(pendTailIrReR_.begin(), pendTailIrReR_.end(), 0.);
        std::fill(pendTailIrImR_.begin(), pendTailIrImR_.end(), 0.);
    }

    // Espectro de cada partición de cola en buffers PENDIENTES (sin data-race con el hilo RT)
    for (int p = 0; p < newTailParts; ++p) {
        const int off = MAX_IR + p * BLOCK;
        const int plen = (off + BLOCK <= safeLen) ? BLOCK : (safeLen - off);
        // Atenuación progresiva por partición para impedir acumulación resonante cuando se suben RT60 y Wet
        const double partDamp = 1.0 / std::sqrt(1.0 + 0.35 * static_cast<double>(p));
        double* tre = &pendTailIrReL_[(size_t)p * FFT_SIZE];
        double* tim = &pendTailIrImL_[(size_t)p * FFT_SIZE];
        for (int i = 0; i < plen; ++i) tre[i] = normIrL[off + i] * partDamp;
        fft1024_.forward(tre, tim);

        double* rre = &pendTailIrReR_[(size_t)p * FFT_SIZE];
        double* rim = &pendTailIrImR_[(size_t)p * FFT_SIZE];
        for (int i = 0; i < plen; ++i) rre[i] = normIrR[off + i] * partDamp;
        fft1024_.forward(rre, rim);

        // ── BRIR: decorrelar la cola R con red allpass en frecuencia ──
        if (decorrel_.load(std::memory_order_relaxed) > 0.001) {
            const double amt = decorrel_.load(std::memory_order_relaxed);
            const double a = 0.6 * amt;             // coeficiente allpass
            for (int k = 1; k < FFT_SIZE / 2; ++k) {
                const double w = 2.0 * 3.14159265358979 * (double)k / (double)FFT_SIZE;
                const double cw = std::cos(w), sw = std::sin(w);
                const double den = 1.0 + a*a - 2.0*a*cw;
                const double re_h = ((1.0+a*a)*cw - 2.0*a) / den;
                const double im_h = ((1.0-a*a)*sw) / den;
                const double re2 = rre[k]*re_h - rim[k]*im_h;
                const double im2 = rre[k]*im_h + rim[k]*re_h;
                rre[k] = re2; rim[k] = im2;
                rre[FFT_SIZE-k] = re2; rim[FFT_SIZE-k] = -im2;
            }
        }
    }
    pendTailPartsActive_ = newTailParts;

    const int len = std::min(safeLen, MAX_IR);
    const double eBoost = earlyBoost_.load(std::memory_order_relaxed);

    // Calcular FFT de la IR en los buffers pendientes (hilo de control)
    std::memset(pendIrReL_, 0, sizeof pendIrReL_);
    std::memset(pendIrImL_, 0, sizeof pendIrImL_);
    std::memset(pendIrReR_, 0, sizeof pendIrReR_);
    std::memset(pendIrImR_, 0, sizeof pendIrImR_);

    for (int i = 0; i < len; ++i) {
        // Aplicar realce de claridad directa/temprana acoplado al perfil SOFA-SAF
        const double gain = (i < 64) ? eBoost : 1.0;
        pendIrReL_[i] = normIrL[i] * gain;
        pendIrReR_[i] = normIrR[i] * gain;
    }

    // ── Binauralización SOFA-SAF de reflexiones tempranas (i >= 48, >1 ms @48k) ──
    if (len > 64 && decorrel_.load(std::memory_order_relaxed) > 0.05) {
        const double latScale = 0.06 * decorrel_.load(std::memory_order_relaxed);
        for (int i = len - 1; i >= 48; --i) {
            double accLat = 0.0;
            const int maxK = std::min(i - 48, 31);
            for (int k = 0; k <= maxK; ++k) {
                const double mid = 0.5 * (pendIrReL_[i - k] + pendIrReR_[i - k]);
                const double dip = (ivanna::master::kMasterSofaP0[k] - ivanna::master::kMasterSofaP0[128 + k]) * 8.0;
                accLat += mid * dip;
            }
            pendIrReL_[i] += latScale * accLat;
            pendIrReR_[i] -= latScale * accLat;
        }
    }
    fft1024_.forward(pendIrReL_, pendIrImL_);
    fft1024_.forward(pendIrReR_, pendIrImR_);

    // ── Normalización de Pico Espectral Global (Head + Tail) ──
    // Impide que modos resonantes de salas grandes acumulen > 0 dB al subir RT60 + Wet.
    double maxBinPower = 1e-6;
    for (int k = 0; k < FFT_SIZE; ++k) {
        double pL = pendIrReL_[k] * pendIrReL_[k] + pendIrImL_[k] * pendIrImL_[k];
        double pR = pendIrReR_[k] * pendIrReR_[k] + pendIrImR_[k] * pendIrImR_[k];
        for (int p = 0; p < newTailParts; ++p) {
            const double* tre = &pendTailIrReL_[(size_t)p * FFT_SIZE];
            const double* tim = &pendTailIrImL_[(size_t)p * FFT_SIZE];
            const double* rre = &pendTailIrReR_[(size_t)p * FFT_SIZE];
            const double* rim = &pendTailIrImR_[(size_t)p * FFT_SIZE];
            pL += tre[k] * tre[k] + tim[k] * tim[k];
            pR += rre[k] * rre[k] + rim[k] * rim[k];
        }
        const double pk = std::max(pL, pR);
        if (pk > maxBinPower) maxBinPower = pk;
    }
    const double maxBinMag = std::sqrt(maxBinPower);
    if (maxBinMag > 0.85) {
        const double specScale = 0.85 / maxBinMag;
        for (int k = 0; k < FFT_SIZE; ++k) {
            pendIrReL_[k] *= specScale;
            pendIrImL_[k] *= specScale;
            pendIrReR_[k] *= specScale;
            pendIrImR_[k] *= specScale;
        }
        for (int p = 0; p < newTailParts; ++p) {
            double* tre = &pendTailIrReL_[(size_t)p * FFT_SIZE];
            double* tim = &pendTailIrImL_[(size_t)p * FFT_SIZE];
            double* rre = &pendTailIrReR_[(size_t)p * FFT_SIZE];
            double* rim = &pendTailIrImR_[(size_t)p * FFT_SIZE];
            for (int k = 0; k < FFT_SIZE; ++k) {
                tre[k] *= specScale; tim[k] *= specScale;
                rre[k] *= specScale; rim[k] *= specScale;
            }
        }
    }

    pendOverlapLen_ = len - 1;
    bakedEarly_.store(snapEarly, std::memory_order_relaxed);
    bakedDecorrel_.store(snapDec, std::memory_order_relaxed);
    bakedRt60Delta_.store(snapRt, std::memory_order_relaxed);
    loads_.fetch_add(1, std::memory_order_relaxed);

    // Señalar al hilo de audio que hay una nueva IR lista
    pending_.store(true, std::memory_order_release);
    loaded_.store(true, std::memory_order_release);
}

void RirConvolver::unload() noexcept {
    wetDry_.store(0.0f, std::memory_order_relaxed);
    pending_.store(false, std::memory_order_relaxed);
    tailLatencyBudgetExceeded_.store(false, std::memory_order_release);
}

// Envoltorio float (F2/486): convierte por scratch y delega en el
// núcleo double. Lo usan JNI Ruta A (pdOutL/R float) y los tests.
// FIX RT 487: el scratch es fijo (kWrapCapacity); NUNCA se redimensiona
// en el hilo de audio. Bloques mayores se procesan por trozos — el núcleo
// double tiene estado, pero procesar por trozos es idéntico a llamarlo
// varias veces; en la práctica ningún bloque supera 8192 (kFusedMaxBlockFrames).
void RirConvolver::process(float* L, float* R, int frames) noexcept {
    if (!L || !R || frames <= 0) return;
    int offset = 0;
    int remaining = frames;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kWrapCapacity);
        for (int i = 0; i < chunk; ++i) {
            wrapL_[i] = std::isfinite(L[offset + i]) ? static_cast<double>(L[offset + i]) : 0.0;
            wrapR_[i] = std::isfinite(R[offset + i]) ? static_cast<double>(R[offset + i]) : 0.0;
        }
        process(wrapL_.data(), wrapR_.data(), chunk);
        for (int i = 0; i < chunk; ++i) {
            L[offset + i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
            R[offset + i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
        }
        offset += chunk;
        remaining -= chunk;
    }
}

// NÚCLEO double (F2/486).
void RirConvolver::process(double* L, double* R, int frames) noexcept {
    enableRirDenormalGuard();   // FTZ/DAZ en el hilo de audio
    const double wetTarget = std::clamp(static_cast<double>(wetDry_.load(std::memory_order_relaxed)), 0.0, 1.0);

    if (wetSmooth_ <= 0.) {
        wetSmooth_ = (double)std::exp(-1.0 / (48000.0 * 0.010));  // ~10 ms @48k
    }

    // Soft Suspension: cuando tanto el target como el suavizado están en 0,
    // preservamos las últimas `ol` muestras reales en overlapL_/overlapR_
    // sin ejecutar FFTs ni destruir la historia acústica.
    if (wetTarget < 1e-4 && wetNow_ < 1e-4) {
        wetNow_ = 0.0;
        const int ol = (overlapLen_ < MAX_IR) ? overlapLen_ : MAX_IR - 1;
        if (ol > 0 && L && R && frames > 0) {
            if (frames >= ol) {
                std::memcpy(overlapL_, L + (frames - ol), (size_t)ol * sizeof(double));
                std::memcpy(overlapR_, R + (frames - ol), (size_t)ol * sizeof(double));
            } else {
                const int keep = ol - frames;
                std::memmove(overlapL_, overlapL_ + frames, (size_t)keep * sizeof(double));
                std::memmove(overlapR_, overlapR_ + frames, (size_t)keep * sizeof(double));
                std::memcpy(overlapL_ + keep, L, (size_t)frames * sizeof(double));
                std::memcpy(overlapR_ + keep, R, (size_t)frames * sizeof(double));
            }
        }
        return;
    }
    if (!loaded_.load(std::memory_order_acquire)) return;

    // Aplicar IR pendiente si load() fue llamado desde el hilo de control.
    if (pending_.load(std::memory_order_acquire)) {
        const bool hadIr = (overlapLen_ > 0);
        if (hadIr && xfadeBlocks_ == 0) {
            std::memcpy(oldIrReL_, irReL_, sizeof oldIrReL_);
            std::memcpy(oldIrImL_, irImL_, sizeof oldIrImL_);
            std::memcpy(oldIrReR_, irReR_, sizeof oldIrReR_);
            std::memcpy(oldIrImR_, irImR_, sizeof oldIrImR_);
            xfadeBlocks_ = XFADE_BLOCKS;
        }
        overlapLen_ = pendOverlapLen_;
        std::memcpy(irReL_, pendIrReL_, sizeof irReL_);
        std::memcpy(irImL_, pendIrImL_, sizeof irImL_);
        std::memcpy(irReR_, pendIrReR_, sizeof irReR_);
        std::memcpy(irImR_, pendIrImR_, sizeof irImR_);
        const size_t totalFloats = static_cast<size_t>(TAIL_PARTS) * FFT_SIZE;
        if (pendTailIrReL_.size() >= totalFloats && tailIrReL_.size() >= totalFloats) {
            std::memcpy(tailIrReL_.data(), pendTailIrReL_.data(), totalFloats * sizeof(double));
            std::memcpy(tailIrImL_.data(), pendTailIrImL_.data(), totalFloats * sizeof(double));
            std::memcpy(tailIrReR_.data(), pendTailIrReR_.data(), totalFloats * sizeof(double));
            std::memcpy(tailIrImR_.data(), pendTailIrImR_.data(), totalFloats * sizeof(double));
        }
        tailPartsActive_ = pendTailPartsActive_;
        pending_.store(false, std::memory_order_release);
    }

    int remaining = frames;
    int offset    = 0;

    while (remaining > 0) {
        const int n = (remaining < BLOCK) ? remaining : BLOCK;

        // ── Crossfade de IR (monótono de oldIr -> pendIr ANTES de convolucionar) ──
        if (xfadeBlocks_ > 0) {
            --xfadeBlocks_;
            const double alpha = std::clamp(
                static_cast<double>(xfadeBlocks_) / static_cast<double>(XFADE_BLOCKS),
                0.0, 1.0);
            const double beta = 1.0 - alpha;
            for (int i = 0; i < FFT_SIZE; ++i) {
                irReL_[i] = alpha * oldIrReL_[i] + beta * pendIrReL_[i];
                irImL_[i] = alpha * oldIrImL_[i] + beta * pendIrImL_[i];
                irReR_[i] = alpha * oldIrReR_[i] + beta * pendIrReR_[i];
                irImR_[i] = alpha * oldIrImR_[i] + beta * pendIrImR_[i];
            }
            if (xfadeBlocks_ == 0) {
                overlapLen_ = pendOverlapLen_;
            }
        }

        const int ol = (overlapLen_ < MAX_IR) ? overlapLen_ : MAX_IR - 1;

        // ── Overlap-save Head L (latencia 0, soporta cualquier n <= BLOCK) ──
        std::memset(workRe_, 0, FFT_SIZE * sizeof(double));
        std::memset(workIm_, 0, FFT_SIZE * sizeof(double));
        if (ol > 0) {
            std::memcpy(workRe_, overlapL_, ol * sizeof(double));
        }
        for (int i = 0; i < n; ++i) workRe_[ol + i] = L[offset + i];
        // FIX CRÍTICO: conservar SIEMPRE las últimas `ol` muestras contiguas (workRe_ + n),
        // nunca `min(n, MAX_IR-1)` que dejaba congelado overlapL_[n..ol-1] cuando n=320 < 511.
        if (ol > 0) {
            std::memcpy(overlapL_, workRe_ + n, ol * sizeof(double));
        }
        fft1024_.forward(workRe_, workIm_);
        for (int i = 0; i < FFT_SIZE; ++i) {
            double yr = workRe_[i]*irReL_[i] - workIm_[i]*irImL_[i];
            double yi = workRe_[i]*irImL_[i] + workIm_[i]*irReL_[i];
            workRe_[i] = yr; workIm_[i] = yi;
        }
        fft1024_.inverse(workRe_, workIm_);
        double convL[BLOCK];
        for (int i = 0; i < n; ++i) convL[i] = workRe_[ol + i];

        // ── Overlap-save Head R (latencia 0, soporta cualquier n <= BLOCK) ──
        std::memset(workRe_, 0, FFT_SIZE * sizeof(double));
        std::memset(workIm_, 0, FFT_SIZE * sizeof(double));
        if (ol > 0) {
            std::memcpy(workRe_, overlapR_, ol * sizeof(double));
        }
        for (int i = 0; i < n; ++i) workRe_[ol + i] = R[offset + i];
        if (ol > 0) {
            std::memcpy(overlapR_, workRe_ + n, ol * sizeof(double));
        }
        fft1024_.forward(workRe_, workIm_);
        for (int i = 0; i < FFT_SIZE; ++i) {
            double yr = workRe_[i]*irReR_[i] - workIm_[i]*irImR_[i];
            double yi = workRe_[i]*irImR_[i] + workIm_[i]*irReR_[i];
            workRe_[i] = yr; workIm_[i] = yi;
        }
        fft1024_.inverse(workRe_, workIm_);
        double convR[BLOCK];
        for (int i = 0; i < n; ++i) convR[i] = workRe_[ol + i];

        // ── Cola particionada estéreo alineada exactamente a BLOCK=512 muestras ──
        // Acumula muestras de entrada en tailInL_/R_ y ejecuta el paso FDL de 1024 puntos
        // únicamente cuando se completan 512 muestras exactas, eliminando los huecos de ceros
        // y el zumbido periódico de 150 Hz cuando el caller entrega bloques de 256 o 320 frames.
        // Fase 3 (0028): si NEU·Ω lleva la cola en frecuencia, este
        // convolver salta su paso FDL de cola (solo head). El flag es
        // atómico y lo fija el integrador (JNI) al activar NEU.
        if (tailEnabled_.load(std::memory_order_relaxed) &&
            tailPartsActive_ > 0 && loaded_.load(std::memory_order_acquire) &&
            fdlReL_.size() >= (size_t)TAIL_PARTS * FFT_SIZE &&
            fdlReR_.size() >= (size_t)TAIL_PARTS * FFT_SIZE) {
            for (int i = 0; i < n; ++i) {
                convL[i] += tailOutL_[tailPos_];
                convR[i] += tailOutR_[tailPos_];
                tailInL_[tailPos_] = L[offset + i];
                tailInR_[tailPos_] = R[offset + i];
                if (++tailPos_ >= BLOCK) {
                    tailPos_ = 0;
                    // FFT L de ventana 1024 = [tailHistL_(512), tailInL_(512)]
                    std::memcpy(workRe_, tailHistL_, BLOCK * sizeof(double));
                    std::memcpy(workRe_ + BLOCK, tailInL_, BLOCK * sizeof(double));
                    std::memset(workIm_, 0, FFT_SIZE * sizeof(double));
                    std::memcpy(tailHistL_, tailInL_, BLOCK * sizeof(double));
                    fft1024_.forward(workRe_, workIm_);
                    std::memcpy(&fdlReL_[(size_t)fdlIndex_ * FFT_SIZE], workRe_, FFT_SIZE * sizeof(double));
                    std::memcpy(&fdlImL_[(size_t)fdlIndex_ * FFT_SIZE], workIm_, FFT_SIZE * sizeof(double));

                    // FFT R de ventana 1024 = [tailHistR_(512), tailInR_(512)]
                    std::memcpy(workRe_, tailHistR_, BLOCK * sizeof(double));
                    std::memcpy(workRe_ + BLOCK, tailInR_, BLOCK * sizeof(double));
                    std::memset(workIm_, 0, FFT_SIZE * sizeof(double));
                    std::memcpy(tailHistR_, tailInR_, BLOCK * sizeof(double));
                    fft1024_.forward(workRe_, workIm_);
                    std::memcpy(&fdlReR_[(size_t)fdlIndex_ * FFT_SIZE], workRe_, FFT_SIZE * sizeof(double));
                    std::memcpy(&fdlImR_[(size_t)fdlIndex_ * FFT_SIZE], workIm_, FFT_SIZE * sizeof(double));

                    // F2/486: la suma de las 32 particiones se acumula
                    // directamente en double en workRe_/workIm_ (que ya
                    // están libres: las FFT de entrada ya se copiaron al
                    // FDL) y se hace IFFT en double, canal L y luego R.
                    // El flag doubleAccum_ queda como compat sin efecto.
                    std::memset(workRe_, 0, FFT_SIZE * sizeof(double));
                    std::memset(workIm_, 0, FFT_SIZE * sizeof(double));
                    for (int p = 0; p < tailPartsActive_; ++p) {
                        const int src = (fdlIndex_ - p + TAIL_PARTS) % TAIL_PARTS;
                        const double* xrL = &fdlReL_[(size_t)src * FFT_SIZE];
                        const double* xiL = &fdlImL_[(size_t)src * FFT_SIZE];
                        const double* hrL = &tailIrReL_[(size_t)p * FFT_SIZE];
                        const double* hiL = &tailIrImL_[(size_t)p * FFT_SIZE];
                        for (int k = 0; k < FFT_SIZE; ++k) {
                            workRe_[k] += xrL[k] * hrL[k] - xiL[k] * hiL[k];
                            workIm_[k] += xrL[k] * hiL[k] + xiL[k] * hrL[k];
                        }
                    }
                    fft1024_.inverse(workRe_, workIm_);
                    std::memcpy(tailOutL_, workRe_ + BLOCK, BLOCK * sizeof(double));
                    std::memset(workRe_, 0, FFT_SIZE * sizeof(double));
                    std::memset(workIm_, 0, FFT_SIZE * sizeof(double));
                    for (int p = 0; p < tailPartsActive_; ++p) {
                        const int src = (fdlIndex_ - p + TAIL_PARTS) % TAIL_PARTS;
                        const double* xrR = &fdlReR_[(size_t)src * FFT_SIZE];
                        const double* xiR = &fdlImR_[(size_t)src * FFT_SIZE];
                        const double* hrR = &tailIrReR_[(size_t)p * FFT_SIZE];
                        const double* hiR = &tailIrImR_[(size_t)p * FFT_SIZE];
                        for (int k = 0; k < FFT_SIZE; ++k) {
                            workRe_[k] += xrR[k] * hrR[k] - xiR[k] * hiR[k];
                            workIm_[k] += xrR[k] * hiR[k] + xiR[k] * hrR[k];
                        }
                    }
                    fft1024_.inverse(workRe_, workIm_);
                    std::memcpy(tailOutR_, workRe_ + BLOCK, BLOCK * sizeof(double));
                    fdlIndex_ = (fdlIndex_ + 1) % TAIL_PARTS;
                }
            }
        }

        // ── Mezcla wet/dry + Matriz True-Stereo 4-Caminos + crossmix RIR experimental (runtime OFF) ──
        const double ws = wetSmooth_;
        double wn = wetNow_;
        const double tsCross = trueStereoCross_.load(std::memory_order_relaxed);
        const double xtcAmt  = xtcStrength_.load(std::memory_order_relaxed);
        const double safWetScale = wetScale_.load(std::memory_order_relaxed);
        double evIn = 0.0, evOut = 0.0;
        constexpr double kBassCoeff = 0.028; // ~220 Hz @ 48 kHz (preserva impacto en graves)
        constexpr double kShadowLp  = 0.36;  // Sombra acústica craneal de Woodworth (~3.2 kHz)
        constexpr double kDcPole    = 0.9993; // ~5 Hz DC blocker en la cola reverberante

        for (int i = 0; i < n; ++i) {
            wn = wetTarget + ws * (wn - wetTarget);
            // Ley de mezcla equa-potencia suave: evita que al subir el slider Wet al 100%
            // la cola reverberante sume +6 dB sobre la señal directa.
            const double dry = 1.0 - 0.68 * wn;
            const double wetGain = 0.82 * wn * safWetScale;
            const double inL = L[offset + i];
            const double inR = R[offset + i];

            // Bloqueo DC de primer orden sobre la salida convolucionada antes de entrar al historial
            const double rawConvL = std::isfinite(convL[i]) ? convL[i] : 0.0;
            const double rawConvR = std::isfinite(convR[i]) ? convR[i] : 0.0;
            const double cleanL = rawConvL - dcX1L_ + kDcPole * dcY1L_;
            dcX1L_ = rawConvL; dcY1L_ = cleanL;
            const double cleanR = rawConvR - dcX1R_ + kDcPole * dcY1R_;
            dcX1R_ = rawConvR; dcY1R_ = cleanR;

            // Actualizar línea de retardo circular de 32 taps para acoplamiento contralateral (LR/RL) y XTC
            crossWriteIdx_ = (crossWriteIdx_ - 1) & 31;
            crossHistL_[crossWriteIdx_] = cleanL;
            crossHistR_[crossWriteIdx_] = cleanR;

            double wetL = cleanL;
            double wetR = cleanR;

            // 1. Matriz True-Stereo 4-Caminos (LL + RL -> L, RR + LR -> R) con kernel SOFA de 32 taps
            if (tsCross > 0.001) {
                double crossFromR = 0.0;
                double crossFromL = 0.0;
                for (int k = 1; k < 32; ++k) {
                    const int idx = (crossWriteIdx_ + k) & 31;
                    const double tap = ivanna::master::kMasterTrueStereoCrossKernel[k];
                    crossFromR += crossHistR_[idx] * tap;
                    crossFromL += crossHistL_[idx] * tap;
                }
                wetL += tsCross * crossFromR;
                wetR += tsCross * crossFromL;
            }

            double outL = dry * inL + wetGain * wetL;
            double outR = dry * inR + wetGain * wetR;

            // Experimental RIR wet-tail crossmix only; production runtime keeps xtcAmt at zero.
            // This is not a measured 2x2 loudspeaker-to-ear inverse and must not be called BACCH XTC.
            if (xtcAmt > 0.001) {
                // Aislar graves (<220 Hz) para mantener pegada en fase intacta en los woofers
                xtcBassL_ += kBassCoeff * (outL - xtcBassL_);
                xtcBassR_ += kBassCoeff * (outR - xtcBassR_);
                const double hiL = outL - xtcBassL_;
                const double hiR = outR - xtcBassR_;

                // Retardo interaural Woodworth (~11 muestras @ 48 kHz = 229 us para altavoces a +-30 deg)
                const int delayIdx = (crossWriteIdx_ + 11) & 31;
                xtcLpL_ += kShadowLp * (crossHistL_[delayIdx] - xtcLpL_);
                xtcLpR_ += kShadowLp * (crossHistR_[delayIdx] - xtcLpR_);

                // Cancelación antisimétrica de diafonía transaural con compensación de energía controlada
                const double normGain = 1.0 + 0.10 * xtcAmt;
                outL = xtcBassL_ + normGain * (hiL - xtcAmt * 0.32 * xtcLpR_);
                outR = xtcBassR_ + normGain * (hiR - xtcAmt * 0.32 * xtcLpL_);
            }

            // Soft-clip racional de seguridad (> 0.96) para impedir espurios de convolución
            if (std::fabs(outL) > 0.96) {
                outL = std::copysign(0.96 + 0.04 * std::tanh((std::fabs(outL) - 0.96) * 8.0), outL);
            }
            if (std::fabs(outR) > 0.96) {
                outR = std::copysign(0.96 + 0.04 * std::tanh((std::fabs(outR) - 0.96) * 8.0), outR);
            }

            evIn  += static_cast<double>(inL) * inL + static_cast<double>(inR) * inR;
            evOut += static_cast<double>(outL) * outL + static_cast<double>(outR) * outR;
            L[offset + i] = outL;
            R[offset + i] = outR;
        }
        wetNow_ = wn;
        activeBlocks_.fetch_add(1, std::memory_order_relaxed);
        inEnergy_.store(inEnergy_.load(std::memory_order_relaxed) + evIn, std::memory_order_relaxed);
        outEnergy_.store(outEnergy_.load(std::memory_order_relaxed) + evOut, std::memory_order_relaxed);

        offset    += n;
        remaining -= n;
    }
}

} // namespace Ivanna
