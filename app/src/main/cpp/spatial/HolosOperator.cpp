// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#include "HolosOperator.hpp"

#include <algorithm>
#include <cstring>

#include "GammaComplex.hpp"

namespace ivanna {

namespace {
constexpr double kPiF = 3.14159265358979323846;

double erbRate(double fHz) noexcept {
    return 21.4 * std::log10(1.0 + 0.00437 * fHz);
}
double erbInverse(double rate) noexcept {
    return (std::pow(10.0, rate / 21.4) - 1.0) / 0.00437;
}
}  // namespace

// Ω (v2.5.444): la ganancia de firma por banda arranca en identidad (1.0).
HolosOperator::HolosOperator() {
    for (int b = 0; b < kMaxBands; ++b)
        omegaGainLin_[b].store(1.0f, std::memory_order_relaxed);
}

void HolosOperator::prepare(float sr) {
    const double s = std::isfinite(sr) ? std::clamp(static_cast<double>(sr), 8000.0, 384000.0) : 48000.0;
    sampleRate_ = s;
    fftSize_ = (s <= 48000.0) ? 512 : (s <= 96000.0) ? 1024 : (s <= 192000.0) ? 2048 : 4096;
    colaInv_ = 1.0 / ((double)(fftSize_ / kHop) * 0.375);
    const int n = fftSize_;
    fft_ = std::make_unique<FFTRadix2d>(n);
    window_.assign(n, 0.0);
    for (int i = 0; i < n; ++i)
        window_[i] = 0.5 * (1.0 - std::cos(2.0 * kPiF * (double)i / (double)n));
    ringL_.assign(n, 0.0);
    ringR_.assign(n, 0.0);
    pendL_.assign(kHop, 0.0);
    pendR_.assign(kHop, 0.0);
    olaL_.assign(n, 0.0);
    olaR_.assign(n, 0.0);
    qCap_ = 2 * n + 2 * kHop;
    wetQL_.assign(qCap_, 0.0);
    wetQR_.assign(qCap_, 0.0);
    dryQL_.assign(qCap_, 0.0);
    dryQR_.assign(qCap_, 0.0);
    reL_.assign(n, 0.0);
    imL_.assign(n, 0.0);
    reR_.assign(n, 0.0);
    imR_.assign(n, 0.0);
    tailRingL_.assign(n, 0.0);
    tailRingR_.assign(n, 0.0);
    tailReL_.assign(n, 0.0);
    tailImL_.assign(n, 0.0);
    tailReR_.assign(n, 0.0);
    tailImR_.assign(n, 0.0);
    midL_.assign(kHop, 0.0);
    midR_.assign(kHop, 0.0);
    rebuildBandTables();
    activeSet_ = &set24_;
    activeBands_ = 24;
    estimator_.prepare(s);
    estimator_.setBandTables(set24_.loBin, set24_.hiBin, set24_.centerHz, 24);
    kernel_.prepare(s);
    // Término difuso: una sola IR compartida (Master Studio sintetizada).
    // El acoplamiento se fija ANTES de sintetizar (se hornea en load()).
    rir_.setSampleRate((int)s);
    rir_.setLateDecorrelation(1.0);  // máxima difusión entre oídos
    rir_.setTrueStereoCrossGain(0.0);  // sin matriz true-stereo (FUSION §4.4.1)
    rir_.setWetDry(1.0);
    rir_.synthesizeMasterStudioBrir(roomRt60_.load(std::memory_order_relaxed), (int)s);
    // Warm-up: llevar wetNow a 1.0 alimentando silencio (prepare, no RT).
    // Sin esto, los primeros ~10 ms tras activar mezclarían seco en la
    // cola y la recuperación del wet (out−0.32·mid)/0.82 no sería exacta.
    {
        double zeroL[512] = {};
        double zeroR[512] = {};
        for (int done = 0; done < 48000; done += 512)
            rir_.process(zeroL, zeroR, 512);
    }
    prepared_ = true;
    reset();
}

void HolosOperator::rebuildBandTables() {
    const int n = fftSize_;
    const double sr = sampleRate_;
    const double fTop = std::min(16000.0, 0.45 * sr);
    double edgeHz[kMaxBands + 1];
    const double rLo = erbRate(60.0), rHi = erbRate(fTop);
    for (int i = 0; i <= kMaxBands; ++i)
        edgeHz[i] = erbInverse(rLo + (rHi - rLo) * (double)i / (double)kMaxBands);
    // Set de 24 y de 12 bandas (el tier cambia el set activo en caliente).
    for (int set = 0; set < 2; ++set) {
        BandSet& bs = (set == 0) ? set24_ : set12_;
        const int count = (set == 0) ? 24 : 12;
        const int merge = kMaxBands / count;
        bs.count = count;
        for (int j = 0; j < count; ++j) {
            const double lo = edgeHz[j * merge];
            const double hi = edgeHz[(j + 1) * merge];
            bs.centerHz[j] = std::sqrt(lo * hi);
            bs.loBin[j] = std::max(0, (int)std::ceil(lo * (double)n / sr));
            bs.hiBin[j] = std::min(n / 2, (int)std::floor(hi * (double)n / sr));
            if (bs.hiBin[j] <= bs.loBin[j]) bs.hiBin[j] = bs.loBin[j] + 1;
        }
        bs.bandOfBin.assign(n, 0);
        for (int k = 0; k <= n / 2; ++k) {
            const double f = (double)k * sr / (double)n;
            int b = count - 1;
            for (int j = 0; j < count; ++j) {
                if (f < edgeHz[(j + 1) * merge]) { b = j; break; }
            }
            bs.bandOfBin[k] = b;
            bs.bandOfBin[(n - k) % n] = b;
        }
    }
}

void HolosOperator::applyActiveBandSet() {
    const int tier = tier_.load(std::memory_order_relaxed);
    const int want = (tier >= (int)HolosThermalTier::Warm)
                         ? 12
                         : ((bandCount_.load(std::memory_order_relaxed) == 12) ? 12 : 24);
    if (want != activeBands_) {
        activeBands_ = want;
        activeSet_ = (want == 12) ? &set12_ : &set24_;
        estimator_.setBandTables(activeSet_->loBin, activeSet_->hiBin,
                                 activeSet_->centerHz, want);
    }
}

void HolosOperator::reset() noexcept {
    ringPos_ = 0;
    tailRingPos_ = 0;
    pendCount_ = 0;
    qHead_ = 0;
    qTail_ = 0;
    qCount_ = 0;
    frameIndex_ = 0;
    skipEmit_ = 0;
    totalIn_ = 0;
    totalOut_ = 0;
    wet_ = 0.0;
    blend_ = 0.0;
    flushPending_ = false;
    estimator_.reset();
    kernel_.reset();
    if (prepared_) {
        std::fill(ringL_.begin(), ringL_.end(), 0.0);
        std::fill(ringR_.begin(), ringR_.end(), 0.0);
        std::fill(pendL_.begin(), pendL_.end(), 0.0);
        std::fill(pendR_.begin(), pendR_.end(), 0.0);
        std::fill(olaL_.begin(), olaL_.end(), 0.0);
        std::fill(olaR_.begin(), olaR_.end(), 0.0);
        std::fill(wetQL_.begin(), wetQL_.end(), 0.0);
        std::fill(wetQR_.begin(), wetQR_.end(), 0.0);
        std::fill(dryQL_.begin(), dryQL_.end(), 0.0);
        std::fill(dryQR_.begin(), dryQR_.end(), 0.0);
        std::fill(tailRingL_.begin(), tailRingL_.end(), 0.0);
        std::fill(tailRingR_.begin(), tailRingR_.end(), 0.0);
        std::fill(tailReL_.begin(), tailReL_.end(), 0.0);
        std::fill(tailImL_.begin(), tailImL_.end(), 0.0);
        std::fill(tailReR_.begin(), tailReR_.end(), 0.0);
        std::fill(tailImR_.begin(), tailImR_.end(), 0.0);
        for (int b = 0; b < kMaxBands; ++b) {
            gammaLin_[b] = 1.0;
            gammaDb_[b] = 0.0;
            gammaRe_[b] = 1.0;  // Ley 1: identidad (|Γ|=1, φ=0)
            gammaIm_[b] = 0.0;
            widthSide_[b] = 1.0;
            widthNorm_[b] = 1.0;
            diffuseMix_[b] = 0.0;
            dynDbSmooth_[b] = 0.0;
        }
    }
}

void HolosOperator::setParams(const HolosParams& p) noexcept {
    amount_.store(std::isfinite(p.amount) ? std::clamp(p.amount, 0.0f, 1.0f) : 0.0f,
                  std::memory_order_relaxed);
    distanceM_.store(std::isfinite(p.distanceM) ? std::clamp(p.distanceM, 0.15f, 2.0f) : 0.35f,
                     std::memory_order_relaxed);
    spread_.store(std::isfinite(p.spread) ? std::clamp(p.spread, 0.0f, 1.5f) : 1.0f,
                  std::memory_order_relaxed);
    proximityDb_.store(std::isfinite(p.proximityDb) ? std::clamp(p.proximityDb, 0.0f, 6.0f) : 3.0f,
                       std::memory_order_relaxed);
    diffuse_.store(std::isfinite(p.diffuse) ? std::clamp(p.diffuse, 0.0f, 1.0f) : 0.5f,
                   std::memory_order_relaxed);
    harmonics_.store(std::isfinite(p.harmonics) ? std::clamp(p.harmonics, 0.0f, 1.0f) : 0.32f,
                     std::memory_order_relaxed);
    warmth_.store(std::isfinite(p.warmth) ? std::clamp(p.warmth, 0.0f, 1.0f) : 0.5f,
                  std::memory_order_relaxed);
    dynamics_.store(std::isfinite(p.dynamics) ? std::clamp(p.dynamics, 0.0f, 1.0f) : 1.0f,
                    std::memory_order_relaxed);
    width_.store(std::isfinite(p.width) ? std::clamp(p.width, 0.0f, 1.5f) : 1.0f,
                 std::memory_order_relaxed);
    gamma_.store(std::isfinite(p.gamma) ? std::clamp(p.gamma, 0.5f, 1.0f) : 1.0f,
                 std::memory_order_relaxed);
    roomRt60_.store(std::isfinite(p.roomRt60) ? std::clamp(p.roomRt60, 0.18f, 1.8f) : 0.34f,
                    std::memory_order_relaxed);
    headTracking_.store(p.headTracking, std::memory_order_relaxed);
    bandCount_.store((p.bandCount == 12) ? 12 : 24, std::memory_order_relaxed);
    eqLow_.store(std::isfinite(p.eqLow) ? std::clamp(p.eqLow, -18.0f, 18.0f) : 0.0f,
                 std::memory_order_relaxed);
    eqMid_.store(std::isfinite(p.eqMid) ? std::clamp(p.eqMid, -18.0f, 18.0f) : 0.0f,
                 std::memory_order_relaxed);
    eqHigh_.store(std::isfinite(p.eqHigh) ? std::clamp(p.eqHigh, -18.0f, 18.0f) : 0.0f,
                  std::memory_order_relaxed);
    eqPresence_.store(std::isfinite(p.eqPresence) ? std::clamp(p.eqPresence, -18.0f, 18.0f) : 0.0f,
                      std::memory_order_relaxed);
    eqTiltDb_.store(std::isfinite(p.eqTiltDb) ? std::clamp(p.eqTiltDb, -3.0f, 0.0f) : 0.0f,
                    std::memory_order_relaxed);
    eqFreq_.store(std::isfinite(p.eqFreq) ? std::clamp(p.eqFreq, 20.0f, 16000.0f) : 1000.0f,
                  std::memory_order_relaxed);
}

// Ω (v2.5.444): ganancia de firma por banda (lineal) desde el operador Ω.
// Hilo de control. nullptr o valor no finito ⇒ identidad (1.0) en esa
// banda. Se recorta a [0, 8] (≈ +18 dB): Ω nunca pide más que eso (el
// tope real lo pone C_max, ≤ +12 dB).
void HolosOperator::setOmegaBandGain(const float* gainsLin24) noexcept {
    for (int b = 0; b < kMaxBands; ++b) {
        const float g = (gainsLin24 != nullptr) ? gainsLin24[b] : 1.0f;
        omegaGainLin_[b].store(
            std::isfinite(g) ? std::clamp(g, 0.0f, 8.0f) : 1.0f,
            std::memory_order_relaxed);
    }
}

void HolosOperator::setAmount(float amount) noexcept {
    amount_.store(std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f,
                  std::memory_order_relaxed);
}

void HolosOperator::setHeadPose(float yawRad) noexcept {
    yawRad_.store(std::isfinite(yawRad)
                      ? static_cast<float>(std::clamp(static_cast<double>(yawRad), -kPiF, kPiF))
                      : 0.0f,
                  std::memory_order_relaxed);
}

void HolosOperator::setThermalTier(HolosThermalTier tier) noexcept {
    tier_.store((int)tier, std::memory_order_relaxed);
}

void HolosOperator::setThermalShed(bool shed) noexcept {
    tier_.store(shed ? (int)HolosThermalTier::Bypass : (int)HolosThermalTier::Normal,
                std::memory_order_relaxed);
}

void HolosOperator::setPinnaTable(const holos::PinnaTable& table) {
    pinna_ = table;  // hilo de control, nunca el de audio
}

int HolosOperator::loadVolterraKernel(const uint8_t* package, size_t packageSize) noexcept {
    return kernel_.loadKernelPackage(package, packageSize);
}

float HolosOperator::bandGammaDb(int b) const noexcept {
    if (b < 0 || b >= activeBands_) return 0.0f;
    return gammaDb_[b];
}
float HolosOperator::bandWidthGain(int b) const noexcept {
    if (b < 0 || b >= activeBands_) return 1.0f;
    return widthSide_[b] * widthNorm_[b];
}
float HolosOperator::bandDiffuseMix(int b) const noexcept {
    if (b < 0 || b >= activeBands_) return 0.0f;
    return diffuseMix_[b];
}

// Γ por banda, 1×/frame, desde s_b (FUSION §4.4.3).
void HolosOperator::computeBandGains() {
    const int tier = tier_.load(std::memory_order_relaxed);
    const double diffuseParam = diffuse_.load(std::memory_order_relaxed);
    const double effDiffuse =
        (tier >= (int)HolosThermalTier::Limited) ? 0.0
        : (tier == (int)HolosThermalTier::Warm) ? diffuseParam * 0.5
                                                : diffuseParam;
    const double dynamics = dynamics_.load(std::memory_order_relaxed);
    const double widthScale = width_.load(std::memory_order_relaxed);
    holos::EqParams eq;
    eq.low = eqLow_.load(std::memory_order_relaxed);
    eq.mid = eqMid_.load(std::memory_order_relaxed);
    eq.high = eqHigh_.load(std::memory_order_relaxed);
    eq.presence = eqPresence_.load(std::memory_order_relaxed);
    eq.tiltDb = eqTiltDb_.load(std::memory_order_relaxed);
    eq.freq = eqFreq_.load(std::memory_order_relaxed);
    const double sr = sampleRate_;
    // Fase B (ley de Esse): slew asimétrico por banda en dB/hop.
    const double hopSec = static_cast<double>(kHop) / static_cast<double>(std::max(sr, 8000.0));
    const double maxDownDbPerHop = 60.0 * hopSec;  // ~0.16 dB/hop @48k
    const double maxUpDbPerHop = 18.0 * hopSec;    // ~0.048 dB/hop @48k

    double rawDynDb[kMaxBands] = {};
    double coupledDynDb[kMaxBands] = {};
    for (int b = 0; b < activeBands_; ++b) {
        const holos::HolosBandState& s = estimator_.state(b);
        const double envDb = 20.0 * std::log10(std::max(static_cast<double>(s.env), 1.0e-6));
        rawDynDb[b] = holos::dynamicGainMasterDb(envDb, static_cast<double>(s.crestDb));
    }
    holos::coupleBandGainsDb(rawDynDb, coupledDynDb, activeBands_, 0.35, 0.15);
    // Fase B v2b: ancla de banda ancha — sin ella el escalón de Γ entre
    // bandas (2.2 dB) da AM a 375/125 Hz y THD+N -68.4 dB @1 kHz
    // (ver linkBandGainsDb). La detección sigue multibanda; la
    // aplicación es enlazada con <= 0.05 dB de desviación por banda.
    holos::linkBandGainsDb(coupledDynDb, coupledDynDb, activeBands_, 0.05);

    // Gamma estatico por banda (para la fase: solo el filtro estatico RBJ
    // tiene contraparte de fase; la dinamica y la Omega son moduladores).
    double gStaticArr[64];
    const int nStatic = std::min(activeBands_, 64);
    for (int b = 0; b < activeBands_; ++b) {
        const holos::HolosBandState& s = estimator_.state(b);
        const double fc = activeSet_->centerHz[b];
        // Γ_estático (magnitud RBJ del paramétrico en el centro de banda).
        const double gStatic = holos::staticGainLin(fc, eq, sr);
        if (b < nStatic) gStaticArr[b] = gStatic;
        // Γ_dinámico (ley maestra de Esse, cero makeup): acoplamiento
        // inter-banda + slew por banda en 64-bit + banda muerta.
        // Fase B v2 (ajuste 2 de Esse): suavizado exponencial (tau 40 ms)
        // + banda muerta ±0.05 dB en dynDbSmooth_. En régimen permanente
        // el objetivo cae dentro de la banda muerta y la ganancia se
        // CONGELA: Γ exactamente constante, cero AM a la tasa de hop
        // (375 Hz) -> THD+N de línea base. El slew (60/18 dB/s) sigue
        // siendo el límite duro de pendiente por hop.
        const double targetDb = coupledDynDb[b];
        const double curDb = dynDbSmooth_[b];
        const double deltaDb = targetDb - curDb;
        if (std::fabs(deltaDb) > 0.05) {
            const double dynAlpha = 1.0 - std::exp(-hopSec / 0.040);
            const double stepDb = std::clamp(deltaDb * dynAlpha,
                                             -maxDownDbPerHop, maxUpDbPerHop);
            dynDbSmooth_[b] = curDb + stepDb;
        }
        const double gDyn = std::pow(10.0, (static_cast<double>(dynamics) * dynDbSmooth_[b]) / 20.0);
        gammaLin_[b] = static_cast<double>(
            static_cast<double>(gStatic) * gDyn *
            static_cast<double>(omegaGainLin_[b].load(std::memory_order_relaxed)));
        gammaDb_[b] = 20.0 * std::log10(std::max(gammaLin_[b], 1e-6));
        // Γ_ancho (side desde θ/coh, con split de graves y norma).
        const double w = holos::widthFromState(s.coh, widthScale);
        const holos::WidthGains wg = holos::widthGains(w, fc);
        widthSide_[b] = wg.sideGain;
        widthNorm_[b] = wg.norm;
        // Mezcla directo/difuso de la banda.
        diffuseMix_[b] = holos::diffuseMix(s.coh, effDiffuse);
    }
    // Ley 1 — Gamma complejo de fase mixta. La FASE es la de Bode del filtro
    // ESTATICO (RBJ, fase minima real); la MAGNITUD es la TOTAL
    // (estatica x dinamica x Omega). La dinamica y la Omega son moduladores
    // (ganancia que varia en el tiempo), no filtros: su pendiente entre
    // bandas no tiene contraparte de fase, y darsela degrada el THD+N
    // (medido: -78.0 dB @1 kHz frente al umbral -80 dB). Tau0 = 0.
    // Con EQ plano -> phi = 0: camino real bit-identico.
    // F2/486: phaseFromMagnitude toma centros en double; centerHz de la
    // tabla de bandas sigue en float (API del estimador), se copia.
    double phiStatic[64];
    double fcD[64];
    for (int b = 0; b < activeBands_; ++b) fcD[b] = static_cast<double>(activeSet_->centerHz[b]);
    holos::phaseFromMagnitude(gStaticArr, fcD, activeBands_,
                              sr, 0.0, phiStatic);
    holos::complexGammaBandsFromPhase(gammaLin_, phiStatic, activeBands_,
                                      gammaRe_, gammaIm_);
}

void HolosOperator::renderBins(double* reL, double* imL, double* reR, double* imR,
                               const double* reTL, const double* imTL,
                               const double* reTR, const double* imTR) {
    const int n = fftSize_;
    const double sr = sampleRate_;
    const int tier = tier_.load(std::memory_order_relaxed);
    const double effGamma = (tier >= (int)HolosThermalTier::Limited)
                               ? 1.0
                               : gamma_.load(std::memory_order_relaxed);
    const double proxCap = proximityDb_.load(std::memory_order_relaxed);
    const double yaw = headTracking_.load(std::memory_order_relaxed)
                          ? yawRad_.load(std::memory_order_relaxed)
                          : 0.0;
    const std::vector<int>& bandOfBin = activeSet_->bandOfBin;
    // Reforma v2.5.429: G_b por banda (1×/banda/frame) + por bin.
    // Antes, greenGains() corría por BIN (512×) recalculando itdSeconds
    // (atan2/acos), el spreading y sin(θ) — todo independiente de f.
    // Ahora la base se precalcula por banda y el bin solo aplica lo
    // dependiente de f (greenGainsBin). Misma matemática, bit-idéntica.
    holos::GreenBandBase bandBase[kMaxBands];
    for (int b = 0; b < activeBands_; ++b) {
        const holos::HolosBandState& s = estimator_.state(b);
        const double theta = std::clamp(s.theta * s.coh - yaw, -kPiF, kPiF);
        bandBase[b] = holos::greenBandBase(theta, s.r, effGamma, proxCap);
    }
    // NEU-Omega Fase 0 parte 2 (v2.5.431): semiespectro — render solo los
    // 257 bins únicos (0..N/2) y espejar por simetría hermitiana. La
    // entrada (directa y cola) es hermitiana y el render la preserva
    // (ganancias reales + fase conjugada en N-k), así que la salida en
    // N-k es el conjugado de la salida en k. Salida idéntica salvo ~1 ulp.
    for (int k = 0; k <= n / 2; ++k) {
        const int b = bandOfBin[k];
        const double fSigned = (double)k * sr / (double)n;
        const double af = std::fabs(fSigned);
        // ── G_b: Green unificada (base de banda + bin) ──
        const double pinnaDb = pinna_.pinnaDb(bandBase[b].theta, af);
        const holos::GreenGains g = holos::greenGainsBin(bandBase[b], af, pinnaDb);
        // ── mid/side complejos por bin ──
        const double mRe = 0.5 * (reL[k] + reR[k]), mIm = 0.5 * (imL[k] + imR[k]);
        const double sRe = 0.5 * (reL[k] - reR[k]), sIm = 0.5 * (imL[k] - imR[k]);
        double phLRe = 1.0, phLIm = 0.0, phRRe = 1.0, phRIm = 0.0;
        if (k != 0 && k != n / 2) {  // bins DC/Nyquist son reales: sin rotación
            const double w = 2.0 * kPiF * fSigned;
            const double aL = -w * (g.itdSeconds * 0.5);
            // aR = −aL (simetría): cos(aR)=cos(aL), sin(aR)=−sin(aL).
            // Antes se calculaban 4 trig por bin; ahora 2 (v2.5.429).
            const double c = std::cos(aL), s_ = std::sin(aL);
            phLRe = c; phLIm = s_;
            phRRe = c; phRIm = -s_;
        }
        // ── Γ_b: una sola ganancia COMPLEJA por banda (Ley 1) ──
        // (gRe, gIm) = |Γ|·(cos φ, sin φ). DC/Nyquist son bins reales:
        // ahí gIm = 0 (rotarlos rompería la simetría hermitiana).
        const double gRe = gammaRe_[b];
        const double gIm = (k == 0 || k == n / 2) ? 0.0 : gammaIm_[b];
        const double wSide = widthSide_[b] * widthNorm_[b];
        // Al subir el difuso, el side directo baja (el campo va a la cola).
        const double sideDirect = 1.0 - diffuseMix_[b];
        const double dTail = diffuseMix_[b] * 0.82;  // nivel wet legacy del RIR
        // outL = (mid·gL·phL + side·w·norm·(1−dif)) · Γ ; outR simétrico.
        // La cola difusa se suma por bin con el mismo Γ (el compresor
        // legacy también actuaba sobre el wet de sala).
        const double midL = mRe * g.gL, midLi = mIm * g.gL;
        const double midR = mRe * g.gR, midRi = mIm * g.gR;
        const double dLRe = (midL * phLRe - midLi * phLIm) + sRe * wSide * sideDirect;
        const double dLIm = (midL * phLIm + midLi * phLRe) + sIm * wSide * sideDirect;
        const double dRRe = (midR * phRRe - midRi * phRIm) - sRe * wSide * sideDirect;
        const double dRIm = (midR * phRIm + midRi * phRRe) - sIm * wSide * sideDirect;
        // Multiplicación COMPLEJA por Γ (con φ=0 es bit-idéntica a la real:
        // x·1 − y·0 = x). La cola difusa lleva el mismo Γ que el directo.
        const double oLRe = (dLRe * gRe - dLIm * gIm) + (reTL[k] * gRe - imTL[k] * gIm) * dTail;
        const double oLIm = (dLRe * gIm + dLIm * gRe) + (reTL[k] * gIm + imTL[k] * gRe) * dTail;
        const double oRRe = (dRRe * gRe - dRIm * gIm) + (reTR[k] * gRe - imTR[k] * gIm) * dTail;
        const double oRIm = (dRRe * gIm + dRIm * gRe) + (reTR[k] * gIm + imTR[k] * gRe) * dTail;
        reL[k] = oLRe; imL[k] = oLIm;
        reR[k] = oRRe; imR[k] = oRIm;
        const int m = (n - k) % n;
        if (m != k) {  // espejo hermitiano (DC y Nyquist no se espejan)
            reL[m] = oLRe; imL[m] = -oLIm;
            reR[m] = oRRe; imR[m] = -oRIm;
        }
    }
}

// NEU·Ω Fase 6 parte 2: estimar sobre un espectro ya transformado
// (el del SpectralCore). Replica exacta de los pasos de estimación de
// runFrame() (setParams + estimate + computeBandGains), sin FFT ni
// render ni OLA. Solo lectura del espectro. Núcleo double (F2/486).
void HolosOperator::estimateSpectrum(const double* reL, const double* imL,
                                     const double* reR, const double* imR,
                                     int n) {
    estimator_.setParams(distanceM_.load(std::memory_order_relaxed),
                         spread_.load(std::memory_order_relaxed));
    estimator_.estimate(reL, imL, reR, imR, n);
    computeBandGains();
}

// Envoltorio float de estimateSpectrum (compat: SpectralCore legacy).
void HolosOperator::estimateSpectrum(const float* reL, const float* imL,
                                     const float* reR, const float* imR,
                                     int n) {
    if (!reL || !imL || !reR || !imR || n <= 0) return;
    const std::size_t un = static_cast<std::size_t>(n);
    if (wrapReL_.size() < un) {
        wrapReL_.resize(un); wrapImL_.resize(un);
        wrapReR_.resize(un); wrapImR_.resize(un);
    }
    for (int k = 0; k < n; ++k) {
        wrapReL_[k] = std::isfinite(reL[k]) ? static_cast<double>(reL[k]) : 0.0;
        wrapImL_[k] = std::isfinite(imL[k]) ? static_cast<double>(imL[k]) : 0.0;
        wrapReR_[k] = std::isfinite(reR[k]) ? static_cast<double>(reR[k]) : 0.0;
        wrapImR_[k] = std::isfinite(imR[k]) ? static_cast<double>(imR[k]) : 0.0;
    }
    estimateSpectrum(wrapReL_.data(), wrapImL_.data(), wrapReR_.data(), wrapImR_.data(), n);
}

void HolosOperator::runFrame() {
    const int n = fftSize_;
    // 1) Escribir el hop pendiente en el anillo (historia de N muestras).
    for (int i = 0; i < kHop; ++i) {
        ringL_[ringPos_] = pendL_[i];
        ringR_[ringPos_] = pendR_[i];
        ringPos_ = (ringPos_ + 1) % n;
    }
    // 1b) Término difuso: el mid del hop alimenta el RIR compartido; el
    // wet puro se recupera de su mezcla interna (out = 0.32·mid + 0.82·wet,
    // con wetNow=1 tras el warm-up de prepare()). La cola se analiza con
    // una segunda STFT (solo FFT) en el mismo reloj de hop.
    for (int i = 0; i < kHop; ++i) {
        const double m = 0.5 * (pendL_[i] + pendR_[i]);
        midL_[i] = m;
        midR_[i] = m;
    }
    rir_.process(midL_.data(), midR_.data(), kHop);
    for (int i = 0; i < kHop; ++i) {
        const double m = 0.5 * (pendL_[i] + pendR_[i]);
        midL_[i] = (midL_[i] - 0.32 * m) / 0.82;  // wet puro (canal L)
        midR_[i] = (midR_[i] - 0.32 * m) / 0.82;  // wet puro (canal R)
        tailRingL_[tailRingPos_] = midL_[i];
        tailRingR_[tailRingPos_] = midR_[i];
        tailRingPos_ = (tailRingPos_ + 1) % n;
    }
    // NEU-Omega Fase 0 (v2.5.430): dos FFT reales en una compleja (cola).
    // Empaquetar tailL + j*tailR y hacer UNA forward FFT (en vez de dos).
    // Mismo patrón que ORBE (v2.5.380). Ahorra una FFT por frame.
    for (int i = 0; i < n; ++i) {
        const int idx = (tailRingPos_ + i) % n;
        tailReL_[i] = tailRingL_[idx] * window_[i];  // parte real = L
        tailImL_[i] = tailRingR_[idx] * window_[i];  // parte imag = R
    }
    fft_->forward(tailReL_.data(), tailImL_.data());
    // Desempaquetar in situ (X_L, X_R desde Z = FFT(L + jR)).
    for (int k = 0; k <= n / 2; ++k) {
        const int m = (n - k) % n;
        const double zkr = tailReL_[k], zki = tailImL_[k];
        const double zmr = tailReL_[m], zmi = tailImL_[m];
        tailReL_[k] = 0.5 * (zkr + zmr);
        tailImL_[k] = 0.5 * (zki - zmi);
        tailReR_[k] = 0.5 * (zki + zmi);
        tailImR_[k] = 0.5 * (zmr - zkr);
        if (m != k) {
            tailReL_[m] = 0.5 * (zmr + zkr);
            tailImL_[m] = 0.5 * (zmi - zki);
            tailReR_[m] = 0.5 * (zmi + zki);
            tailImR_[m] = 0.5 * (zkr - zmr);
        }
    }
    // 2) Ventana principal = anillo en orden cronológico.
    // NEU-Omega Fase 0: mismo empaquetado L + jR (UNA forward FFT).
    for (int i = 0; i < n; ++i) {
        const int idx = (ringPos_ + i) % n;
        reL_[i] = ringL_[idx] * window_[i];  // parte real = L
        imL_[i] = ringR_[idx] * window_[i];  // parte imag = R
    }
    fft_->forward(reL_.data(), imL_.data());
    // Desempaquetar in situ.
    for (int k = 0; k <= n / 2; ++k) {
        const int m = (n - k) % n;
        const double zkr = reL_[k], zki = imL_[k];
        const double zmr = reL_[m], zmi = imL_[m];
        reL_[k] = 0.5 * (zkr + zmr);
        imL_[k] = 0.5 * (zki - zmi);
        reR_[k] = 0.5 * (zki + zmi);
        imR_[k] = 0.5 * (zmr - zkr);
        if (m != k) {
            reL_[m] = 0.5 * (zmr + zkr);
            imL_[m] = 0.5 * (zmi - zki);
            reR_[m] = 0.5 * (zmi + zki);
            imR_[m] = 0.5 * (zkr - zmr);
        }
    }
    estimator_.setParams(distanceM_.load(std::memory_order_relaxed),
                         spread_.load(std::memory_order_relaxed));
    estimator_.estimate(reL_.data(), imL_.data(), reR_.data(), imR_.data(), n);
    computeBandGains();
    renderBins(reL_.data(), imL_.data(), reR_.data(), imR_.data(),
               tailReL_.data(), tailImL_.data(), tailReR_.data(), tailImR_.data());
    // NEU-Omega Fase 0: dos IFFT reales en una compleja (UN solo IFFT).
    // Empaquetar Y_L + j*Y_R en reL/imL; tras el IFFT, reL_ = y_L e
    // imL_ = y_R. Ahorra una IFFT por frame. HOLOS pasa de 6 a 3 FFTs/hop.
    for (int i = 0; i < n; ++i) {
        const double yLre = reL_[i], yLim = imL_[i];
        const double yRre = reR_[i], yRim = imR_[i];
        reL_[i] = yLre - yRim;  // Re(Y_L + j*Y_R)
        imL_[i] = yLim + yRre;  // Im(Y_L + j*Y_R)
    }
    fft_->inverse(reL_.data(), imL_.data());
    // 3) Overlap-add con ventana de síntesis (reL_ = y_L, imL_ = y_R).
    for (int i = 0; i < n; ++i) {
        olaL_[i] += window_[i] * reL_[i];
        olaR_[i] += window_[i] * imL_[i];
    }
    // 4) Emitir un hop, salvo los primeros (N-hop)/hop frames (o los
    // primeros 3 tras un flush): misma regla que ORBE (ventanas con inicio
    // negativo desplazarían la salida; el seco es SIEMPRE el más antiguo).
    const int skipFrames = (n - kHop) / kHop;
    const bool emit = (skipEmit_ == 0) && (frameIndex_ >= skipFrames);
    if (skipEmit_ > 0) --skipEmit_;
    if (emit) {
        // K temporal + Γ_techo sobre el hop renderizado (post-OLA): el
        // polinomio genera armónicos en tiempo real y el softCeil acota
        // el pico verdadero (que solo existe en tiempo).
        for (int i = 0; i < kHop; ++i) {
            olaL_[i] *= colaInv_;
            olaR_[i] *= colaInv_;
        }
        {
            holos::VolterraKernelParams kp;
            kp.harmonics = harmonics_.load(std::memory_order_relaxed);
            kp.warmth = warmth_.load(std::memory_order_relaxed);
            kp.driveNorm = 0.45;  // drive por defecto del Exciter
            const int tier = tier_.load(std::memory_order_relaxed);
            kp.h3Enabled = (tier < (int)HolosThermalTier::Warm);
            kernel_.setParams(kp);
        }
        kernel_.process(olaL_.data(), olaR_.data(), kHop);
        for (int i = 0; i < kHop; ++i) {
            const double wL = holos::softCeil(olaL_[i]);
            const double wR = holos::softCeil(olaR_[i]);
            wetQL_[qTail_] = wL;
            wetQR_[qTail_] = wR;
            const int dIdx = (ringPos_ + i) % n;
            dryQL_[qTail_] = ringL_[dIdx];
            dryQR_[qTail_] = ringR_[dIdx];
            qTail_ = (qTail_ + 1) % qCap_;
        }
        qCount_ += kHop;
    }
    ++frameIndex_;
    std::memmove(olaL_.data(), olaL_.data() + kHop, (size_t)(n - kHop) * sizeof(double));
    std::memmove(olaR_.data(), olaR_.data() + kHop, (size_t)(n - kHop) * sizeof(double));
    std::fill(olaL_.begin() + (n - kHop), olaL_.end(), 0.0);
    std::fill(olaR_.begin() + (n - kHop), olaR_.end(), 0.0);
}

void HolosOperator::process(double* l, double* r, int n) noexcept {
    if (!prepared_ || l == nullptr || r == nullptr || n <= 0) return;
    applyActiveBandSet();
    const double sr = sampleRate_;
    const int tier = tier_.load(std::memory_order_relaxed);
    const double wetTarget =
        (tier >= (int)HolosThermalTier::Bypass || estimator_.meanCoherence() < 0.3)
            ? 0.0
            : amount_.load(std::memory_order_relaxed);
    const double blendTarget = (wetTarget > 0.0 || wet_ > 0.0) ? 1.0 : 0.0;
    // La puerta abierta resetea el sondeo (el próximo cierre empieza su
    // espera desde cero).
    if (wetTarget > 0.0) { cohBypassBlocks_ = 0; cohProbeBlocksRemaining_ = 0; }
    // Apagado completo y sin fades pendientes: passthrough bit-exacto, cero CPU.
    // PERO con trampa (v2.5.415, bandera 5 de la 414): el estimador de
    // coherencia solo se actualiza dentro de runFrame(), y runFrame() solo
    // corre cuando la etapa está activa — si la coherencia cae bajo 0.3 la
    // puerta se cierra, el estimador se congela bajo 0.3 y la puerta NO
    // puede reabrirse jamás (latch). En la 414 HOLOS medía ~1 µs en
    // captura por esto. Cuando SOLO la coherencia cierra la puerta, cada
    // ~1 s se sondean 8 bloques (~53 ms): se cae al camino normal, runFrame
    // re-sondea el estimador y la salida sigue en vivo (wetTarget=0)
    // hasta que la coherencia se recupere; si sigue baja, la puerta se
    // vuelve a cerrar y el sondeo se repite. Coste: ~8 bloques de ~2 ms
    // por segundo (~1.7 % de un core) solo mientras está en bypass por
    // coherencia. El cierre por tier térmico NO se sondea (es autoridad).
    if (wet_ == 0.0 && blend_ == 0.0 && wetTarget == 0.0 && blendTarget == 0.0) {
        const bool cohOnly = tier < (int)HolosThermalTier::Bypass &&
                             estimator_.meanCoherence() < 0.3;
        if (cohOnly) {
            if (cohProbeBlocksRemaining_ > 0) {
                --cohProbeBlocksRemaining_;
                // Cae al camino normal (sondeo): runFrame actualiza el
                // estimador; la salida sigue en vivo hasta que se recupere.
            } else if (++cohBypassBlocks_ >= kCohProbePeriodBlocks) {
                cohBypassBlocks_ = 0;
                cohProbeBlocksRemaining_ = kCohProbeBlocks - 1;
                // Cae al camino normal (primer bloque del sondeo).
            } else {
                flushPending_ = true;
                return;
            }
        } else {
            cohBypassBlocks_ = 0;
            cohProbeBlocksRemaining_ = 0;
            flushPending_ = true;
            return;
        }
    }
    const double slew = 1.0 / (0.015 * sr);  // rampa de 15 ms por muestra
    if (flushPending_) {
        flushPending_ = false;
        if (qCount_ > 0) {
            qCount_ = 0;
            qHead_ = 0;
            qTail_ = 0;
            totalOut_ = totalIn_;
            std::fill(olaL_.begin(), olaL_.end(), 0.0);
            std::fill(olaR_.begin(), olaR_.end(), 0.0);
        }
        std::fill(ringL_.begin(), ringL_.end(), 0.0);
        std::fill(ringR_.begin(), ringR_.end(), 0.0);
        std::fill(tailRingL_.begin(), tailRingL_.end(), 0.0);
        std::fill(tailRingR_.begin(), tailRingR_.end(), 0.0);
        ringPos_ = 0;
        tailRingPos_ = 0;
        skipEmit_ = 3;
        for (int b = 0; b < kMaxBands; ++b) dynDbSmooth_[b] = 0.0;
    }
    // Alimentar: acumular en pend y correr un frame por cada hop completo.
    for (int i = 0; i < n; ++i) {
        pendL_[pendCount_] = l[i];
        pendR_[pendCount_] = r[i];
        ++pendCount_;
        ++totalIn_;
        if (pendCount_ >= kHop) {
            runFrame();
            pendCount_ = 0;
        }
    }
    // Drenar: la salida retardada va fftSize muestras por detrás de la
    // entrada; lo que falta al inicio (priming) sale en vivo, sin huecos.
    const size_t targetOut = (totalIn_ > (size_t)fftSize_) ? (totalIn_ - (size_t)fftSize_) : 0;
    const size_t want = (targetOut > totalOut_) ? (targetOut - totalOut_) : 0;
    size_t drain = std::min({(size_t)n, want, (size_t)qCount_});
    size_t i = 0;
    for (; i < drain; ++i) {
        if (wet_ < wetTarget) wet_ = std::min(wet_ + slew, wetTarget);
        else if (wet_ > wetTarget) wet_ = std::max(wet_ - slew, wetTarget);
        if (blend_ < blendTarget) blend_ = std::min(blend_ + slew, blendTarget);
        else if (blend_ > blendTarget) blend_ = std::max(blend_ - slew, blendTarget);
        const double wetL = wetQL_[qHead_], wetR = wetQR_[qHead_];
        const double dryL = dryQL_[qHead_], dryR = dryQR_[qHead_];
        qHead_ = (qHead_ + 1) % qCap_;
        const double liveL = l[i], liveR = r[i];
        const double dryMixL = (1.0 - blend_) * liveL + blend_ * dryL;
        const double dryMixR = (1.0 - blend_) * liveR + blend_ * dryR;
        l[i] = (1.0 - wet_) * dryMixL + wet_ * wetL;
        r[i] = (1.0 - wet_) * dryMixR + wet_ * wetR;
    }
    qCount_ -= (int)drain;
    totalOut_ += drain;
    // El resto del bloque (si lo hay) queda en vivo: passthrough.
}

// Envoltorio float de process() (compat: FusedField legacy / JNI / tests).
void HolosOperator::process(float* l, float* r, int n) noexcept {
    if (!prepared_ || l == nullptr || r == nullptr || n <= 0) return;
    const std::size_t un = static_cast<std::size_t>(n);
    if (wrapL_.size() < un) { wrapL_.resize(un); wrapR_.resize(un); }
    for (int i = 0; i < n; ++i) {
        wrapL_[i] = std::isfinite(l[i]) ? static_cast<double>(l[i]) : 0.0;
        wrapR_[i] = std::isfinite(r[i]) ? static_cast<double>(r[i]) : 0.0;
    }
    process(wrapL_.data(), wrapR_.data(), n);
    for (int i = 0; i < n; ++i) {
        l[i] = std::isfinite(wrapL_[i]) ? static_cast<float>(wrapL_[i]) : 0.0f;
        r[i] = std::isfinite(wrapR_[i]) ? static_cast<float>(wrapR_[i]) : 0.0f;
    }
}

}  // namespace ivanna
