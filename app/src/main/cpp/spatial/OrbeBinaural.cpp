// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#include "OrbeBinaural.hpp"

#include <algorithm>
#include <cstring>

#include "OmegaOperator.hpp"  // Ω (v2.5.444): pre-compensación de diafonía

namespace ivanna {

namespace {
// Ventana Hann periódica. Σ w² sobre los frames solapados = (N/hop)·0.375
// (media de w² = 0.375): con hop = N/4 es 1.5, pero el hop es FIJO (128)
// mientras N escala con la SR, así que la constante COLA escala con N
// (3.0 @1024, 6.0 @2048, 12.0 @4096). Dividir por 1.5 fijo inflaba la
// salida N/512 (medido: pico 2× @96 kHz, 4× @192 kHz, 8× @384 kHz).
// La constante real se calcula en prepare() (miembro colaInv_).
constexpr float kPiF = 3.14159265358979323846f;

float erbRate(float fHz) noexcept {
    return 21.4f * std::log10(1.0f + 0.00437f * fHz);
}
float erbInverse(float rate) noexcept {
    return (std::pow(10.0f, rate / 21.4f) - 1.0f) / 0.00437f;
}
}  // namespace

template <typename Scalar>
void OrbeBinauralT<Scalar>::prepare(float sr) {
    const float s = std::isfinite(sr) ? std::clamp(sr, 8000.0f, 384000.0f) : 48000.0f;
    sampleRate_ = s;
    fftSize_ = (s <= 48000.0f) ? 512 : (s <= 96000.0f) ? 1024 : (s <= 192000.0f) ? 2048 : 4096;
    // Constante COLA real para este N con hop fijo: (N/hop)·0.375.
    colaInv_ = Scalar(1.0) / (static_cast<Scalar>(fftSize_ / kHop) * Scalar(0.375));
    const int n = fftSize_;
    fft_ = std::make_unique<FFTRadix2T<Scalar>>(n);
    window_.assign(n, Scalar(0.0));
    for (int i = 0; i < n; ++i)
        window_[i] = Scalar(0.5) * (Scalar(1.0) - std::cos(Scalar(2.0) * static_cast<Scalar>(kPiF) * static_cast<Scalar>(i) / static_cast<Scalar>(n)));
    ringL_.assign(n, Scalar(0.0));
    ringR_.assign(n, Scalar(0.0));
    pendL_.assign(kHop, Scalar(0.0));
    pendR_.assign(kHop, Scalar(0.0));
    olaL_.assign(n, Scalar(0.0));
    olaR_.assign(n, Scalar(0.0));
    qCap_ = 2 * n + 2 * kHop;
    wetQL_.assign(qCap_, Scalar(0.0));
    wetQR_.assign(qCap_, Scalar(0.0));
    dryQL_.assign(qCap_, Scalar(0.0));
    dryQR_.assign(qCap_, Scalar(0.0));
    reL_.assign(n, Scalar(0.0));
    imL_.assign(n, Scalar(0.0));
    reR_.assign(n, Scalar(0.0));
    imR_.assign(n, Scalar(0.0));
    activeBands_ = (bandCount_.load(std::memory_order_relaxed) == 12) ? 12 : 24;
    rebuildBandTables();
    // Fase 1 (L1): el conservador trabaja sobre las bandas ERB activas.
    if constexpr (kOrbePowerConservation) nfCons_.prepare(activeBands_, (float)kHop / s);
    prepared_ = true;
    reset();
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::rebuildBandTables() {
    const int n = fftSize_;
    const float sr = sampleRate_;
    const float fTop = std::min(16000.0f, 0.45f * sr);
    // Bordes ERB: 24 bandas entre 60 Hz y fTop.
    float edgeHz[kMaxBands + 1];
    const float rLo = erbRate(60.0f), rHi = erbRate(fTop);
    for (int i = 0; i <= kMaxBands; ++i)
        edgeHz[i] = erbInverse(rLo + (rHi - rLo) * (float)i / (float)kMaxBands);
    bands_.assign(activeBands_, Band{});
    const int merge = kMaxBands / activeBands_;  // 1 (24 bandas) o 2 (12)
    for (int j = 0; j < activeBands_; ++j) {
        const float lo = edgeHz[j * merge];
        const float hi = edgeHz[(j + 1) * merge];
        bands_[j].centerHz = std::sqrt(lo * hi);
        bands_[j].loBin = std::max(0, (int)std::ceil(lo * (float)n / sr));
        bands_[j].hiBin = std::min(n / 2, (int)std::floor(hi * (float)n / sr));
        if (bands_[j].hiBin <= bands_[j].loBin) bands_[j].hiBin = bands_[j].loBin + 1;
    }
    bandOfBin_.assign(n, 0);
    for (int k = 0; k <= n / 2; ++k) {
        const float f = (float)k * sr / (float)n;
        int b = activeBands_ - 1;
        for (int j = 0; j < activeBands_; ++j) {
            if (f < edgeHz[(j + 1) * merge]) { b = j; break; }
        }
        bandOfBin_[k] = b;
        bandOfBin_[(n - k) % n] = b;
    }
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::reset() noexcept {
    ringPos_ = 0;
    pendCount_ = 0;
    qHead_ = 0;
    qTail_ = 0;
    qCount_ = 0;
    frameIndex_ = 0;
    skipEmit_ = 0;
    totalIn_ = 0;
    totalOut_ = 0;
    wet_ = Scalar(0.0);
    blend_ = Scalar(0.0);
    flushPending_ = false;
    meanCoherence_ = 1.0f;
    // Fase 1 (L1/L2): el conservador y el suavizador vuelven a su estado
    // inicial (ganancias 1.0, Cantidad suavizada 0).
    if constexpr (kOrbePowerConservation) { nfCons_.reset(); nfAmt_.v = 0.0f; }
    const float d0 = distanceM_.load(std::memory_order_relaxed);
    for (int b = 0; b < kMaxBands; ++b) {
        azimuth_[b] = 0.0f;
        distance_[b] = d0;
        coherence_[b] = 1.0f;
    }
    if (prepared_) {
        std::fill(ringL_.begin(), ringL_.end(), Scalar(0.0));
        std::fill(ringR_.begin(), ringR_.end(), Scalar(0.0));
        std::fill(pendL_.begin(), pendL_.end(), Scalar(0.0));
        std::fill(pendR_.begin(), pendR_.end(), Scalar(0.0));
        std::fill(olaL_.begin(), olaL_.end(), Scalar(0.0));
        std::fill(olaR_.begin(), olaR_.end(), Scalar(0.0));
        std::fill(wetQL_.begin(), wetQL_.end(), Scalar(0.0));
        std::fill(wetQR_.begin(), wetQR_.end(), Scalar(0.0));
        std::fill(dryQL_.begin(), dryQL_.end(), Scalar(0.0));
        std::fill(dryQR_.begin(), dryQR_.end(), Scalar(0.0));
    }
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::setParams(const OrbeParams& p) noexcept {
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
    headRadiusM_.store(std::isfinite(p.headRadiusM)
                           ? std::clamp(p.headRadiusM, 0.05f, 0.15f)
                           : nearfield::kHeadRadiusM,
                       std::memory_order_relaxed);
    headTracking_.store(p.headTracking, std::memory_order_relaxed);
    // bandCount solo se aplica en el próximo prepare(): reconstruir las
    // tablas de bandas en caliente correría contra process().
    bandCount_.store((p.bandCount == 12) ? 12 : 24, std::memory_order_relaxed);
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::setAmount(float amount) noexcept {
    amount_.store(std::isfinite(amount) ? std::clamp(amount, 0.0f, 1.0f) : 0.0f,
                  std::memory_order_relaxed);
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::setHeadPose(float yawRad) noexcept {
    yawRad_.store(std::isfinite(yawRad) ? std::clamp(yawRad, -kPiF, kPiF) : 0.0f,
                  std::memory_order_relaxed);
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::setHeadRadiusM(float radiusM) noexcept {
    headRadiusM_.store(std::isfinite(radiusM) ? std::clamp(radiusM, 0.05f, 0.15f)
                                             : nearfield::kHeadRadiusM,
                       std::memory_order_relaxed);
}

template <typename Scalar>
float OrbeBinauralT<Scalar>::headRadiusM() const noexcept {
    return headRadiusM_.load(std::memory_order_relaxed);
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::setThermalShed(bool shed) noexcept {
    thermalShed_.store(shed, std::memory_order_relaxed);
}

// Ω (v2.5.444): diafonía κ de la vía (0..1). κ=0 ⇒ sin pre-compensación.
// κ=1 (mono) ⇒ el ILD percibido es 0; la inversa lo marca y el render
// queda en el techo (0 dB): una bocina mono no produce ILD (se declara).
template <typename Scalar>
void OrbeBinauralT<Scalar>::setCrosstalkKappa(float kappa) noexcept {
    crosstalkKappa_.store(
        std::isfinite(kappa) ? std::clamp(kappa, 0.0f, 1.0f) : 0.0f,
        std::memory_order_relaxed);
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::setCrosstalkMaxIldDb(float maxDb) noexcept {
    crosstalkMaxIldDb_.store(
        std::isfinite(maxDb) ? std::clamp(maxDb, 0.0f, 36.0f) : 12.0f,
        std::memory_order_relaxed);
}

template <typename Scalar>
float OrbeBinauralT<Scalar>::bandAzimuth(int b) const noexcept {
    if (b < 0 || b >= activeBands_) return 0.0f;
    return azimuth_[b];
}
template <typename Scalar>
float OrbeBinauralT<Scalar>::bandDistance(int b) const noexcept {
    if (b < 0 || b >= activeBands_) return 0.0f;
    return distance_[b];
}
template <typename Scalar>
float OrbeBinauralT<Scalar>::bandCoherence(int b) const noexcept {
    if (b < 0 || b >= activeBands_) return 0.0f;
    return coherence_[b];
}

template <typename Scalar>
float OrbeBinauralT<Scalar>::thetaFromItd(float tauSeconds, float r) const noexcept {
    const float a = std::fabs(tauSeconds);
    if (a <= 1e-9f) return 0.0f;
    const float sign = (tauSeconds > 0.0f) ? 1.0f : -1.0f;
    const float hi = kPiF * 0.5f;
    if (nearfield::itdSeconds(hi, r, headRadiusM()) <= a) return sign * hi;
    float lo = 0.0f, hiB = hi;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hiB);
        if (nearfield::itdSeconds(mid, r, headRadiusM()) < a) lo = mid; else hiB = mid;
    }
    return sign * 0.5f * (lo + hiB);
}

template <typename Scalar>
float OrbeBinauralT<Scalar>::thetaFromIld(float ildDbMeas, float r, float freqHz) const noexcept {
    const float a = std::fabs(ildDbMeas);
    if (a <= 1e-6f) return 0.0f;
    const float sign = (ildDbMeas > 0.0f) ? 1.0f : -1.0f;
    const float hi = kPiF * 0.5f;
    if (nearfield::ildDb(hi, r, freqHz, headRadiusM()) <= a) return sign * hi;
    float lo = 0.0f, hiB = hi;
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hiB);
        if (nearfield::ildDb(mid, r, freqHz, headRadiusM()) < a) lo = mid; else hiB = mid;
    }
    return sign * 0.5f * (lo + hiB);
}

template <typename Scalar>
float OrbeBinauralT<Scalar>::distanceFromIld(float ildDbMeas, float theta, float freqHz) const noexcept {
    const float a = std::fabs(ildDbMeas);
    const float th = std::fabs(theta);
    if (th < 0.05f) return distanceM_.load(std::memory_order_relaxed);  // sin info de ILD en el centro
    if (a <= nearfield::ildDb(th, 2.0f, freqHz, headRadiusM())) return 2.0f;
    if (a >= nearfield::ildDb(th, 0.15f, freqHz, headRadiusM())) return 0.15f;
    float lo = 0.15f, hi = 2.0f;  // ildDb decrece con r
    for (int i = 0; i < 24; ++i) {
        const float mid = 0.5f * (lo + hi);
        if (nearfield::ildDb(th, mid, freqHz, headRadiusM()) > a) lo = mid; else hi = mid;
    }
    return 0.5f * (lo + hi);
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::estimateBands(const Scalar* reL, const Scalar* imL,
                                 const Scalar* reR, const Scalar* imR) {
    const float sr = sampleRate_;
    const float alpha = 1.0f - std::exp(-((float)kHop / sr) / 0.020f);  // suavizado ~20 ms
    const float spread = spread_.load(std::memory_order_relaxed);
    const float distParam = distanceM_.load(std::memory_order_relaxed);
    float cohSum = 0.0f;
    for (int b = 0; b < activeBands_; ++b) {
        const Band& band = bands_[b];
        double pL = 0.0, pR = 0.0, xRe = 0.0, xIm = 0.0;
        for (int k = band.loBin; k < band.hiBin; ++k) {
            const double lRe = reL[k], lIm = imL[k], rRe = reR[k], rIm = imR[k];
            pL += lRe * lRe + lIm * lIm;
            pR += rRe * rRe + rIm * rIm;
            // cross = conj(L) * R
            xRe += lRe * rRe + lIm * rIm;
            xIm += lRe * rIm - lIm * rRe;
        }
        const float fc = band.centerHz;
        float coh = coherence_[b];
        if (pL + pR > 1e-10) {
            const double denom = std::sqrt(pL * pR);
            coh = denom > 1e-12 ? (float)(std::sqrt(xRe * xRe + xIm * xIm) / denom) : 0.0f;
            coh = std::clamp(coh, 0.0f, 1.0f);
            const float ildMeas = 10.0f * (float)std::log10((pR + 1e-12) / (pL + 1e-12));
            const float ipd = (float)std::atan2(xIm, xRe);
            const float itdMeas = std::clamp(ipd / (2.0f * kPiF * fc), -0.0008f, 0.0008f);
            const float rCur = distance_[b];
            float thetaEst;
            if (fc < 700.0f) {
                thetaEst = thetaFromItd(itdMeas, rCur);
            } else {
                thetaEst = thetaFromIld(ildMeas, rCur, fc);
            }
            thetaEst = std::clamp(thetaEst * spread, -kPiF * 0.5f, kPiF * 0.5f);
            azimuth_[b] += alpha * (thetaEst - azimuth_[b]);
            if (fc < 800.0f && coh > 0.5f) {
                const float rEst = distanceFromIld(ildMeas, thetaEst, fc);
                distance_[b] += alpha * (rEst - distance_[b]);
            } else {
                // Sin evidencia de cercanía, la distancia vuelve despacio al parámetro base.
                distance_[b] += alpha * (distParam - distance_[b]);
            }
        }
        coherence_[b] += alpha * (coh - coherence_[b]);
        cohSum += coherence_[b];
    }
    meanCoherence_ = activeBands_ > 0 ? cohSum / (float)activeBands_ : 1.0f;
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::renderBins(Scalar* reL, Scalar* imL, Scalar* reR, Scalar* imR) {
    const int n = fftSize_;
    const float sr = sampleRate_;
    const float diffuse = diffuse_.load(std::memory_order_relaxed);
    const float proxCap = proximityDb_.load(std::memory_order_relaxed);
    const float proxScale = proxCap / 6.0f;
    // Fase 1 (L2): la Cantidad escala la señal física (ITD/ILD/estante),
    // no la mezcla. aLaw suaviza el amount (tau 25 ms) para no hacer
    // zipper al mover el slider. Con el flag OFF, aLaw = 1.0f (sin cambio).
    float aLaw = 1.0f;
    if constexpr (kOrbePowerConservation) {
        aLaw = nfAmt_.step(amount_.load(std::memory_order_relaxed), (float)kHop / sr);
    }
    const float yaw = headTracking_.load(std::memory_order_relaxed)
                          ? yawRad_.load(std::memory_order_relaxed) : 0.0f;
    // Ω (v2.5.444): diafonía κ de la vía y ILD máximo radiable. Se cargan
    // una vez por frame (no por bin). κ=0 ⇒ el `if` del bucle no se toma.
    const float xtalkKappa = crosstalkKappa_.load(std::memory_order_relaxed);
    const float xtalkMaxIld = crosstalkMaxIldDb_.load(std::memory_order_relaxed);
    // v2.5.380 — invariantes por banda fuera del bucle por bin: theta, r,
    // itd y sin(theta) dependen solo de la banda (24 valores), no del bin
    // (512). Antes se recalculaban por bin, incluido itdSeconds() — el
    // modelo esférico (atan2/acos por camino) — 512 veces por frame en vez
    // de 24. Los valores son bit-idénticos (misma expresión, mismos
    // operandos); solo se evalúan una vez por banda.
    float thetaB[kMaxBands], rB[kMaxBands], itdB[kMaxBands];
    float wLB[kMaxBands], wRB[kMaxBands];
    for (int b = 0; b < activeBands_; ++b) {
        const float th = std::clamp(azimuth_[b] * coherence_[b] - yaw, -kPiF, kPiF);
        thetaB[b] = th;
        rB[b] = distance_[b];
        // Fase 1 (L2): con el flag ON, la Cantidad escala el ITD.
        float itdVal = nearfield::itdSeconds(th, rB[b], headRadiusM());
        if constexpr (kOrbePowerConservation) itdVal *= aLaw;
        itdB[b] = itdVal;
        const float wR = 0.5f * (1.0f + std::sin(th));
        wRB[b] = wR;
        wLB[b] = 1.0f - wR;
    }
    // NEU-Omega Fase 0 parte 2 (v2.5.431): semiespectro — render solo los
    // 257 bins únicos (0..N/2) y espejar por simetría hermitiana. La
    // entrada es hermitiana (señal real) y el render la preserva
    // (ganancias reales + fase conjugada en N-k), así que la salida en
    // N-k es el conjugado de la salida en k. Salida idéntica salvo ~1 ulp.
    // El bucle por bin pasa de 512 a 257 iteraciones por frame.
    for (int k = 0; k <= n / 2; ++k) {
        const int b = bandOfBin_[k];
        const float theta = thetaB[b];
        const float r = rB[b];
        const float fSigned = (float)k * sr / (float)n;
        const float af = std::fabs(fSigned);
        const float itd = itdB[b];
        float ild = nearfield::ildDb(theta, r, af, headRadiusM());
        // Fase 1 (L2): con el flag ON, la Cantidad escala el ILD.
        if constexpr (kOrbePowerConservation) ild *= aLaw;
        // Ω (v2.5.444) — pre-compensación de diafonía (G^Ω, §5.2): el ILD
        // radiado se expande por la inversa de la Ec. 7 para que el
        // percibido iguale al de diseño, hasta el techo D_max(κ). κ=0
        // (audífonos) ⇒ sin cambio. La Green de HOLOS no se pre-compensa
        // en v1 (ORBE es el render binaural primario; ver OmegaOperator).
        if (xtalkKappa > 0.0f) {
            ild = ivanna::spatial::OmegaOperator::radiatedIldClippedDb(
                ild, xtalkKappa, xtalkMaxIld);
        }
        float shelf = nearfield::proximityShelfDb(r, af) * proxScale;
        // Fase 1 (L2): con el flag ON, la Cantidad escala el estante.
        if constexpr (kOrbePowerConservation) shelf *= aLaw;
        // v2.5.380 — 2 pow en vez de 4: pow(10,a)·pow(10,b) = pow(10,a+b)
        // salvo ≤1 ulp; el cambio en THD+N/IMD es despreciable (el test
        // lo verifica: THD+N −139.3x dB, IMD ≈ −157 dB, como antes).
        const float wR = wRB[b];
        const float wL = wLB[b];
        Scalar gL = std::pow(Scalar(10.0), -static_cast<Scalar>(ild) / Scalar(40.0) + static_cast<Scalar>(shelf) * static_cast<Scalar>(wL) / Scalar(20.0));
        Scalar gR = std::pow(Scalar(10.0), +static_cast<Scalar>(ild) / Scalar(40.0) + static_cast<Scalar>(shelf) * static_cast<Scalar>(wR) / Scalar(20.0));
        // Estante de proximidad: se reparte entre oídos por lateralización
        // (wR = 0.5·(1+sin θ)). En el centro (θ=0) ambos oídos reciben la
        // mitad: una fuente centrada NO debe lateralizarse por el estante.
        // La parte común la cancela la normalización de potencia constante
        // de abajo: el estante es un componente de ILD, no una ganancia
        // global (la ganancia global la acota el SafetyLimiter aguas abajo).
        const Scalar norm = std::sqrt(Scalar(2.0) / (gL * gL + gR * gR));
        gL *= norm;
        gR *= norm;
        // mid/side complejos por bin.
        const Scalar mRe = Scalar(0.5) * (reL[k] + reR[k]), mIm = Scalar(0.5) * (imL[k] + imR[k]);
        const Scalar sRe = Scalar(0.5) * (reL[k] - reR[k]), sIm = Scalar(0.5) * (imL[k] - imR[k]);
        Scalar phLRe = Scalar(1.0), phLIm = Scalar(0.0), phRRe = Scalar(1.0), phRIm = Scalar(0.0);
        if (k != 0 && k != n / 2) {  // bins DC/Nyquist son reales: sin rotación de fase
            const Scalar w = Scalar(2.0) * static_cast<Scalar>(kPiF) * static_cast<Scalar>(fSigned);
            const Scalar aL = -w * (static_cast<Scalar>(itd) * Scalar(0.5));
            const Scalar aR = -w * (-static_cast<Scalar>(itd) * Scalar(0.5));
            phLRe = std::cos(aL); phLIm = std::sin(aL);
            phRRe = std::cos(aR); phRIm = std::sin(aR);
        }
        // outL = mid*gL*phL + side*sideGain ; outR = mid*gR*phR - side*sideGain
        // Fase 1 (L2): con el flag ON, S nunca se atenúa (kSideGain = 1.0f,
        // fix del defecto M1); con el flag OFF, el diffuse de siempre
        // (0.5 por defecto).
        Scalar sideGain = static_cast<Scalar>(diffuse);
        if constexpr (kOrbePowerConservation) sideGain = static_cast<Scalar>(ivanna::nf::kSideGain);
        const Scalar oLRe = (mRe * gL * phLRe - mIm * gL * phLIm) + sRe * sideGain;
        const Scalar oLIm = (mRe * gL * phLIm + mIm * gL * phLRe) + sIm * sideGain;
        const Scalar oRRe = (mRe * gR * phRRe - mIm * gR * phRIm) - sRe * sideGain;
        const Scalar oRIm = (mRe * gR * phRIm + mIm * gR * phRRe) - sIm * sideGain;
        reL[k] = oLRe; imL[k] = oLIm;
        reR[k] = oRRe; imR[k] = oRIm;
        const int m = (n - k) % n;
        if (m != k) {  // espejo hermitiano (DC y Nyquist no se espejan)
            reL[m] = oLRe; imL[m] = -oLIm;
            reR[m] = oRRe; imR[m] = -oRIm;
        }
    }
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::runFrame() {
    const int n = fftSize_;
    // 1) Escribir el hop pendiente en el anillo (historia de N muestras).
    for (int i = 0; i < kHop; ++i) {
        ringL_[ringPos_] = pendL_[i];
        ringR_[ringPos_] = pendR_[i];
        ringPos_ = (ringPos_ + 1) % n;
    }
    // 2) Ventana = anillo en orden cronológico (ringPos_ = muestra más antigua).
    // v2.5.380 — el anillo es circular: copiar en dos tramos contiguos en
    // vez de un % por muestra (512 mods por canal y frame). Mismos valores,
    // mismo orden de escritura; solo cambia la aritmética de índices.
    // v2.5.380 — dos FFT reales en una compleja: empaquetar L + i·R en un
    // solo buffer y hacer UNA forward FFT (en vez de dos). Desempaquetar
    // con X[k] = (Z[k] + conj(Z[N-k]))/2, Y[k] = (Z[k] − conj(Z[N-k]))/(2i)
    // (la FFT es lineal: Z = FFT(L) + i·FFT(R)). El resultado es el mismo
    // salvo ~1 ulp de redondeo: THD+N/IMD se mueven ≤0.2 dB / ~1.5 dB en
    // el piso de redondeo (todos a mejor) y el test lo verifica.
    // Ahorra una FFT completa por frame.
    {
        const int first = n - ringPos_;  // ringPos_..n-1, luego 0..ringPos_-1
        for (int i = 0; i < first; ++i) {
            reL_[i] = ringL_[ringPos_ + i] * window_[i];  // parte real = L
            imL_[i] = ringR_[ringPos_ + i] * window_[i];  // parte imag = R
        }
        for (int i = first; i < n; ++i) {
            const int idx = i - first;
            reL_[i] = ringL_[idx] * window_[i];
            imL_[i] = ringR_[idx] * window_[i];
        }
    }
    fft_->forward(reL_.data(), imL_.data());
    // Desempaquetar in situ: se leen Z[k] y Z[N-k] antes de escribir.
    for (int k = 0; k <= n / 2; ++k) {
        const int m = (n - k) % n;
        const Scalar zkr = reL_[k], zki = imL_[k];
        const Scalar zmr = reL_[m], zmi = imL_[m];
        reL_[k] = Scalar(0.5) * (zkr + zmr);
        imL_[k] = Scalar(0.5) * (zki - zmi);
        reR_[k] = Scalar(0.5) * (zki + zmi);
        imR_[k] = Scalar(0.5) * (zmr - zkr);
        if (m != k) {
            reL_[m] = Scalar(0.5) * (zmr + zkr);
            imL_[m] = Scalar(0.5) * (zmi - zki);
            reR_[m] = Scalar(0.5) * (zmi + zki);
            imR_[m] = Scalar(0.5) * (zkr - zmr);
        }
    }
    estimateBands(reL_.data(), imL_.data(), reR_.data(), imR_.data());
    // Fase 1 (L1): envolver el render con el conservador de potencia
    // por banda. addIn mide la potencia de entrada por banda ERB,
    // addOut la de salida, commit() calcula g_b = sqrt(E_in/E_out) y
    // la ganancia se aplica al espectro de salida. Con el flag OFF,
    // este bloque no existe (if constexpr) y el camino es el de siempre.
    if constexpr (kOrbePowerConservation) {
        nfCons_.beginFrame();
        for (int k = 0; k <= n / 2; ++k)
            nfCons_.addIn(bandOfBin_[k], (double)reL_[k] * reL_[k] + (double)imL_[k] * imL_[k] + (double)reR_[k] * reR_[k] + (double)imR_[k] * imR_[k]);
    }
    renderBins(reL_.data(), imL_.data(), reR_.data(), imR_.data());
    if constexpr (kOrbePowerConservation) {
        for (int k = 0; k <= n / 2; ++k)
            nfCons_.addOut(bandOfBin_[k], (double)reL_[k] * reL_[k] + (double)imL_[k] * imL_[k] + (double)reR_[k] * reR_[k] + (double)imR_[k] * imR_[k]);
        nfCons_.commit();
        for (int k = 0; k < n; ++k) {
            const Scalar g = static_cast<Scalar>(nfCons_.gain[bandOfBin_[k]]);
            reL_[k] *= g; imL_[k] *= g; reR_[k] *= g; imR_[k] *= g;
        }
    }
    // NEU-Omega Fase 0 (v2.5.430): dos IFFT reales en una compleja.
    // Empaquetar Y_L + j*Y_R en reL/imL y hacer UN solo IFFT (en vez de
    // dos). Por linealidad: IFFT(Y_L + j*Y_R) = y_L + j*y_R, así que tras
    // el IFFT reL_ = y_L (tiempo) e imL_ = y_R (tiempo). El resultado es
    // el mismo salvo ~1 ulp. Ahorra una IFFT completa por frame.
    // (El forward ya estaba empaquetado desde la v2.5.380; ORBE pasa de
    // 3 a 2 FFTs por hop.)
    for (int i = 0; i < n; ++i) {
        const Scalar yLre = reL_[i], yLim = imL_[i];
        const Scalar yRre = reR_[i], yRim = imR_[i];
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
    // primeros 3 tras un flush): sus ventanas empiezan en posiciones
    // NEGATIVAS (antes del inicio del stream) y desplazarían toda la
    // salida (N-hop) muestras. El hop seco es el MÁS ANTIGUO del anillo
    // (el que alinea con el hop OLA emitido); el más nuevo está (N-hop)
    // muestras por delante y desalineaba seco/wet. El seco es SIEMPRE
    // el más antiguo: usar el nuevo como seco y cambiar a mitad de
    // stream provocaba un salto atrás de (N-hop) muestras en el seco
    // (clic medido de 0.12 a 100 Hz). Tras un flush el anillo se vacía,
    // así que los primeros hops secos son cero y la salida hace un
    // hueco suave de ~8 ms durante el fundido de activación — sin clic,
    // sin repetición y sin contenido viejo.
    const int skipFrames = (n - kHop) / kHop;
    const bool emit = (skipEmit_ == 0) && (frameIndex_ >= skipFrames);
    if (skipEmit_ > 0) --skipEmit_;
    if (emit) {
        for (int i = 0; i < kHop; ++i) {
            wetQL_[qTail_] = olaL_[i] * colaInv_;
            wetQR_[qTail_] = olaR_[i] * colaInv_;
            const int dIdx = (ringPos_ + i) % n;
            dryQL_[qTail_] = ringL_[dIdx];
            dryQR_[qTail_] = ringR_[dIdx];
            qTail_ = (qTail_ + 1) % qCap_;
        }
        qCount_ += kHop;
    }
    ++frameIndex_;
    std::memmove(olaL_.data(), olaL_.data() + kHop, (size_t)(n - kHop) * sizeof(Scalar));
    std::memmove(olaR_.data(), olaR_.data() + kHop, (size_t)(n - kHop) * sizeof(Scalar));
    std::fill(olaL_.begin() + (n - kHop), olaL_.end(), Scalar(0.0));
    std::fill(olaR_.begin() + (n - kHop), olaR_.end(), Scalar(0.0));
}

template <typename Scalar>
void OrbeBinauralT<Scalar>::process(Scalar* l, Scalar* r, int n) noexcept {
    if (!prepared_ || l == nullptr || r == nullptr || n <= 0) return;
    const float sr = sampleRate_;
    // Fase 1 (L2, flag ON): la Cantidad escala la señal física (aLaw),
    // no la mezcla: el wet es binario (1 si amount>0, 0 si amount=0).
    // Con el flag OFF, el wetTarget continuo de siempre.
    const Scalar wetTarget =
        (thermalShed_.load(std::memory_order_relaxed) || meanCoherence_ < 0.3f)
            ? Scalar(0.0)
            : (kOrbePowerConservation
                   ? (amount_.load(std::memory_order_relaxed) > 0.0f ? Scalar(1.0)
                                                                     : Scalar(0.0))
                   : static_cast<Scalar>(amount_.load(std::memory_order_relaxed)));
    const Scalar blendTarget = (wetTarget > Scalar(0.0) || wet_ > Scalar(0.0)) ? Scalar(1.0) : Scalar(0.0);
    // La puerta abierta resetea el sondeo (el próximo cierre empieza su
    // espera desde cero).
    if (wetTarget > Scalar(0.0)) { cohBypassBlocks_ = 0; cohProbeBlocksRemaining_ = 0; }
    // Apagado completo y sin fades pendientes: passthrough bit-exacto, cero CPU.
    // Se marca flushPending_: al reactivar, las colas guardan hops viejos.
    // PERO con trampa (v2.5.415, bandera 5 de la 414): el estimador de
    // coherencia solo se actualiza dentro de runFrame(), y runFrame() solo
    // corre cuando la etapa está activa — si la coherencia cae bajo 0.3 la
    // puerta se cierra, el estimador se congela bajo 0.3 y la puerta NO
    // puede reabrirse jamás (latch). En la 414 ORBE medía ~1 µs en
    // captura por esto (además del shed térmico, que es por diseño y NO se
    // sondea). Cuando SOLO la coherencia cierra la puerta, cada ~1 s se
    // sondean 8 bloques (~53 ms): se cae al camino normal, runFrame
    // re-sondea el estimador y la salida sigue en vivo (wetTarget=0)
    // hasta que la coherencia se recupere; si sigue baja, la puerta se
    // vuelve a cerrar y el sondeo se repite.
    if (wet_ == Scalar(0.0) && blend_ == Scalar(0.0) && wetTarget == Scalar(0.0) && blendTarget == Scalar(0.0)) {
        const bool cohOnly = !thermalShed_.load(std::memory_order_relaxed) &&
                             meanCoherence_ < 0.3f;
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
    const Scalar slew = Scalar(1.0) / (Scalar(0.015) * static_cast<Scalar>(sr));  // rampa de 15 ms por muestra
    // Reactivación tras estar apagado (flag puesto por el fast-path):
    // las colas guardan hops viejos. Vaciarlas UNA vez y reiniciar el
    // contador retardado: los primeros 512 muestras salen en vivo (con
    // el fundido) y el stream retardado arranca limpio con el audio
    // nuevo. Sin esto, el primer hop drenado sería audio viejo.
    if (flushPending_) {
        flushPending_ = false;
        if (qCount_ > 0) {
            qCount_ = 0;
            qHead_ = 0;
            qTail_ = 0;
            totalOut_ = totalIn_;
            std::fill(olaL_.begin(), olaL_.end(), Scalar(0.0));
            std::fill(olaR_.begin(), olaR_.end(), Scalar(0.0));
        }
        // Vaciar el anillo: el audio viejo no debe reaparecer como seco
        // tras la pausa. Los primeros 3 frames no emiten (skipEmit_) y
        // los primeros hops secos son cero: hueco suave de ~8 ms durante
        // el fundido, sin clic ni repetición (ver runFrame paso 4).
        std::fill(ringL_.begin(), ringL_.end(), Scalar(0.0));
        std::fill(ringR_.begin(), ringR_.end(), Scalar(0.0));
        ringPos_ = 0;
        skipEmit_ = 3;
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
        const Scalar wetL = wetQL_[qHead_], wetR = wetQR_[qHead_];
        const Scalar dryL = dryQL_[qHead_], dryR = dryQR_[qHead_];
        qHead_ = (qHead_ + 1) % qCap_;
        const Scalar liveL = l[i], liveR = r[i];
        const Scalar dryMixL = (Scalar(1.0) - blend_) * liveL + blend_ * dryL;
        const Scalar dryMixR = (Scalar(1.0) - blend_) * liveR + blend_ * dryR;
        l[i] = (Scalar(1.0) - wet_) * dryMixL + wet_ * wetL;
        r[i] = (Scalar(1.0) - wet_) * dryMixR + wet_ * wetR;
    }
    qCount_ -= (int)drain;
    totalOut_ += drain;
    // El resto del bloque (si lo hay) queda en vivo: passthrough.
}

}  // namespace ivanna

// Instanciaciones explícitas (las definiciones viven en este .cpp):
// float = ruta legacy bit-idéntica; double = núcleo de la labor grande.
template class ivanna::OrbeBinauralT<float>;
template class ivanna::OrbeBinauralT<double>;
