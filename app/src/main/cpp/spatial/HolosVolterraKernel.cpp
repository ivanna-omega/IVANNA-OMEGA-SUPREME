// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#include "HolosVolterraKernel.hpp"

#include <algorithm>
#include <cstring>

#include "../neuromorphic/volterra_h2_symmetric.hpp"
#include "../neuromorphic/volterra_kernel_package.hpp"

namespace ivanna {
namespace holos {

namespace {
constexpr double kPiF = 3.14159265358979323846;
}  // namespace

struct HolosVolterraKernel::VolterraImpl {
    ivanna::dsp::VolterraH2Symmetric engine{64, 2};
};

HolosVolterraKernel::HolosVolterraKernel() = default;
HolosVolterraKernel::~HolosVolterraKernel() = default;

void HolosVolterraKernel::prepare(float sr) {
    const double s = std::isfinite(sr) ? std::clamp(static_cast<double>(sr), 8000.0, 384000.0) : 48000.0;
    sampleRate_ = s;
    fundCoef_ = std::exp(-1.0 / (s * 0.003));  // EMA 3 ms (regresión, a tasa base)
    for (int c = 0; c < 2; ++c) {
        Channel& ch = ch_[c];
        ch.hpf.setHighpass(3000.0, 0.707, (double)s);
        ch.lpf.setLowpass(8000.0, 0.707, (double)s);
        ch.lpfGold.setLowpass(8000.0, 0.707, (double)s);
        ch.dcR = std::exp(-2.0 * kPiF * 15.0 / s);  // DC-block 15 Hz
        // Envolvente OHC Heun (RK2): ataque 10 ms / release 80 ms, por muestra.
        ch.heunAtt = 1.0 - std::exp(-1.0 / (s * 0.010));
        ch.heunRel = 1.0 - std::exp(-1.0 / (s * 0.080));
    }
    if (!volterra_) volterra_ = std::make_unique<VolterraImpl>();
    interleave_.assign(2 * 512, 0.0);
    prepared_ = true;
    reset();
}

void HolosVolterraKernel::reset() noexcept {
    for (int c = 0; c < 2; ++c) {
        Channel& ch = ch_[c];
        ch.hpf.reset();
        ch.lpf.reset();
        ch.lpfGold.reset();
        ch.dcX = 0.0;
        ch.dcY = 0.0;
        ch.h2Mean = 0.0;
        ch.fundCross = 0.0;
        ch.fundPow = 0.0;
        ch.fundGain = 0.0;
        ch.heunEnv = 0.0;
    }
}

void HolosVolterraKernel::setParams(const VolterraKernelParams& p) noexcept {
    harmonics_.store(std::isfinite(p.harmonics) ? std::clamp(p.harmonics, 0.0f, 1.0f) : 0.0f,
                     std::memory_order_relaxed);
    warmth_.store(std::isfinite(p.warmth) ? std::clamp(p.warmth, 0.0f, 1.0f) : 0.5f,
                  std::memory_order_relaxed);
    driveNorm_.store(std::isfinite(p.driveNorm) ? std::clamp(p.driveNorm, 0.0f, 1.0f) : 0.45f,
                     std::memory_order_relaxed);
    h3Enabled_.store(p.h3Enabled, std::memory_order_relaxed);
}

bool HolosVolterraKernel::hasVolterraKernel() const noexcept {
    return volterra_ != nullptr && volterra_->engine.hasEffectiveKernel();
}

int HolosVolterraKernel::loadKernelPackage(const uint8_t* package, size_t packageSize) noexcept {
    if (!volterra_ || package == nullptr || packageSize == 0) return -1;
    ivanna::dsp::VolterraLoadPolicy policy;
    policy.expected_sample_rate_hz = (uint32_t)sampleRate_;
    // La puerta IVK1 exige motor deshabilitado + silencioso (EngineBusy si
    // no): deshabilitar antes de cargar. En éxito el wrapper RE-habilita
    // (el cargador en crudo no lo hace): con kernel cargado, el término con
    // memoria de K debe sonar (el envelope hace el fade-in). En error queda
    // en bypass (deshabilitado), como documenta el cargador. Llamar con el
    // motor en silencio (antes de procesar audio, o tras deshabilitar y
    // drenar hasta isSilent()).
    volterra_->engine.setEnabled(false);
    const auto st = volterra_->engine.loadKernelPackage(package, packageSize, policy);
    if (st == ivanna::dsp::VolterraLoadStatus::Ok) volterra_->engine.setEnabled(true);
    return (int)st;
}

float HolosVolterraKernel::heunEnvelope(int ch) const noexcept {
    if (ch < 0 || ch > 1) return 0.0f;
    return ch_[ch].heunEnv;
}

float HolosVolterraKernel::fundGain(int ch) const noexcept {
    if (ch < 0 || ch > 1) return 0.0f;
    return ch_[ch].fundGain;
}

double HolosVolterraKernel::tickChannel(double x, Channel& c) noexcept {
    if (!std::isfinite(x)) return 0.0;
    const double harm = harmonics_.load(std::memory_order_relaxed);
    const double warmth = warmth_.load(std::memory_order_relaxed);
    const double driveNorm = driveNorm_.load(std::memory_order_relaxed);
    const double driveLin = 1.0 + 3.0 * driveNorm;  // como el Exciter (drive_ = 1+3·d)

    // ── Envolvente OHC Heun (RK2) sobre |x|: gobierna el h2 ──
    const double ax = std::fabs(x);
    const double hc = (ax > c.heunEnv) ? c.heunAtt : c.heunRel;
    const double k1 = (ax - c.heunEnv) * hc;
    const double pred = c.heunEnv + k1;
    const double k2h = (ax - pred) * hc;
    c.heunEnv += 0.5 * (k1 + k2h);
    // nl_b = 0.085·env² + 0.05·env (forma funcional coclear, §1.7):
    // a más nivel, la OHC comprime → menos h2 (k2Scale < 1).
    const double nl = std::clamp(0.085 * c.heunEnv * c.heunEnv + 0.05 * c.heunEnv, 0.0, 0.45);
    const double k2Scale = 1.0 - 0.5 * nl;

    // ── Camino Exciter (>3 kHz, <8 kHz) ──
    const double h = c.hpf.process(c.lpf.process(x));
    // Cheb T2/T3 con el mismo polinomio que ChebHarmonicShaper::tick
    // (entrada escalada por driveLin, drive normalizado). H3 off en tier
    // térmico WARM: se anula el término cúbico (warmth→1 hace k3→0.063·dEff;
    // aquí se fuerza k3=0 pasando warmth=1 y escalando después — más limpio:
    // se calcula el polinomio y se resta la parte cúbica medida).
    double cheb;
    if (h3Enabled_.load(std::memory_order_relaxed)) {
        const double y = chebPolyRaw(h * driveLin, driveNorm, warmth, 0.0, k2Scale);
        // DC-block 15 Hz (misma fórmula que el shaper).
        const double out = y - c.dcX + c.dcR * c.dcY;
        c.dcX = y;
        c.dcY = std::isfinite(out) ? out : 0.0;
        cheb = c.dcY;
    } else {
        // Solo H2: el polinomio con warmth=1 da k3 = 0.42·0.15·dEff (no
        // cero exacto); se reconstruye H2 puro: y = (1−k2)·u + k2·u².
        const double dEff = driveNorm;
        const double k2 = 0.42 * 1.0 * dEff * k2Scale;
        double uu = h * driveLin;
        const double axu = std::fabs(uu);
        if (axu > 0.85) {
            const double ex = axu - 0.85;
            const double sat = 0.85 + 0.15 * (ex / (0.15 + ex));
            uu = (uu >= 0.0) ? sat : -sat;
        }
        const double y = (1.0 - k2) * uu + k2 * uu * uu;
        const double out = y - c.dcX + c.dcR * c.dcY;
        c.dcX = y;
        c.dcY = std::isfinite(out) ? out : 0.0;
        cheb = c.dcY;
    }
    // Padé [3/2] residual (misma racional que el Exciter, modo 1: 35%).
    // Es una no-linealidad IMPAR (su serie de Taylor es x + c3·x³ + …):
    // con H3 off (tier WARM) también se anula, o el tier no cumpliría su
    // promesa — el H3 residual del Padé era ~la mitad del H3 total.
    const double pade = h3Enabled_.load(std::memory_order_relaxed)
                             ? (softClipPade(h, driveLin) - h)
                             : 0.0;
    // ── Camino GoldenEar H2 (<8 kHz): 2·lf² con DC por EMA ──
    const double lf = c.lpfGold.process(x);
    const double rawH2 = 2.0 * lf * lf;
    c.h2Mean += 0.002 * (rawH2 - c.h2Mean);  // τ≈500 muestras ≈10.4 ms
    const double h2gold = rawH2 - c.h2Mean;

    // ── Una sola mezcla, una sola corrección de regresión ──
    double exc = 0.65 * cheb + 0.35 * pade + 0.12 * h2gold;
    {
        c.fundCross = fundCoef_ * c.fundCross + (1.0 - fundCoef_) * (exc * h);
        c.fundPow = fundCoef_ * c.fundPow + (1.0 - fundCoef_) * (h * h);
        if (c.fundPow > 1.0e-8)
            c.fundGain = std::clamp(c.fundCross / c.fundPow, -8.0, 8.0);
        exc -= c.fundGain * h;
    }
    double y = x + harm * exc * kExciterCeiling;
    return std::isfinite(y) ? y : 0.0;
}

// Envoltorio float de tickChannel (compat: process float).
float HolosVolterraKernel::tickChannel(float x, Channel& c) noexcept {
    const double y = tickChannel(static_cast<double>(x), c);
    return std::isfinite(y) ? static_cast<float>(y) : 0.0f;
}

void HolosVolterraKernel::process(double* l, double* r, int n) noexcept {
    if (!prepared_ || l == nullptr || r == nullptr || n <= 0) return;
    const bool hasKernel = hasVolterraKernel();
    const double harm = harmonics_.load(std::memory_order_relaxed);
    // Identidad exacta de coste cero: sin armónicos y sin kernel.
    if (harm <= 0.0 && !hasKernel) return;
    for (int i = 0; i < n; ++i) {
        l[i] = tickChannel(l[i], ch_[0]);
        r[i] = tickChannel(r[i], ch_[1]);
    }
    if (hasKernel) {
        // Memoria Volterra H2 (K=64, triangular): interleave → process → deinterleave.
        // Scratch preasignado (2×512); n mayores se procesan por trozos.
        int off = 0;
        while (off < n) {
            const int m = std::min(n - off, 512);
            for (int i = 0; i < m; ++i) {
                interleave_[2 * i] = l[off + i];
                interleave_[2 * i + 1] = r[off + i];
            }
            volterra_->engine.processInterleaved(interleave_.data(), interleave_.data(),
                                                 (uint32_t)m, 2);
            for (int i = 0; i < m; ++i) {
                l[off + i] = interleave_[2 * i];
                r[off + i] = interleave_[2 * i + 1];
            }
            off += m;
        }
    }
}

// Envoltorio float de process() (compat: HolosOperator legacy / tests).
void HolosVolterraKernel::process(float* l, float* r, int n) noexcept {
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

}  // namespace holos
}  // namespace ivanna
