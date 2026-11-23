// ── ESFERA DE POINCARÉ BINAURAL v1 — CÓDIGO DE PRODUCCIÓN ──────────────────
// Origen: ~/workspace/user/files/ivanna_poincare_binaural_1_bjya.hpp
// (Esfera de Poincaré Binaural v1, 2026-11-22), copiada VERBATIM salvo
// este bloque de procedencia. Antes vivía solo en tests/
// (poincare_binaural_v1.hpp, comparación vs ORBE 2026-11-22); desde
// 2026-11-23 la usa FusedSpatial como pan de campo cercano detrás del
// flag kPoincarePanEnabled (OFF por defecto, NUNCA por defecto sin ABX
// en el teléfono). Verificación (g++ 13, C++17): selfTest PASS, cue
// exacto a 6 decimales, error de energía por bin 1.1e-15, identidad a=0
// bit-exacta con retardo real 511 muestras (NO 512 como dice
// latencySamples()).
//
// PROHIBIDO: las funciones de ley de curvatura de
// fused/physical_propagation.hpp (Parte II: wavefrontCurvature,
// curvatureLawItdSeconds, curvatureLawIldDivergenceDb, distanceFromItdExcess,
// curvatureRate, timeToContact) están REFUTADAS y NUNCA se llaman desde
// este operador ni desde su cableado. El cue de este operador viene
// SOLO de ivanna::fused::propagation::nearFieldParallax (modelo de
// rayos, Parte I) vía Operator::setScene.
// IVANNA-OMEGA-SUPREME — FÍSICA NUEVA: ESFERA DE POINCARÉ BINAURAL (v1, 2026-11-22)
// Header único, C++17, sin heap en process(), sin locks, sin I/O.
//
// POSTULADO
//   Cada bin espectral del par de oídos es un vector de Jones J = [L, R]ᵀ.
//   Su estado de "polarización" es el vector de Stokes
//       s0 = |L|²+|R|²        energía        (sonoridad)
//       s1 = |L|²−|R|²        ILD            (dónde: lateralidad)
//       s2 = 2·Re(L̄R)         en fase        (centro)
//       s3 = 2·Im(L̄R)         IPD            (dónde: retardo)
//   El render espacial NO es una ganancia ni una mezcla: es un elemento de SU(2),
//       U = cos(δ/2)·I − i·sin(δ/2)·(k₁σz + k₂σx + k₃σy),
//   que rota (s1,s2,s3) sobre la esfera de radio s0 y deja s0 intacto.
//
// INVARIANTES (exactos por construcción, verificados en selfTest y en host)
//   I1  |Y_L|²+|Y_R|² = |L|²+|R|²  en CADA bin y CADA frame  ⇒  LUFS idéntico
//       para toda ponderación K, sin auto-gain, sin conservador de banda.
//   I2  Imposible cavar una muesca: la energía por bin no puede bajar (I1).
//   I3  Direcciones, anchura y energía son coordenadas independientes:
//       dirección  = ILD/ITD            → pan(α, β)
//       anchura    = ángulo a s2-eje    → contracción geodésica por bin
//       energía    = s0                 → intocable
//   I4  Cantidad a ∈ [0,1] es el parámetro del grupo: U(a) = Rσz(aβ)·Rσy(aα),
//       unitario para todo a; a=0 identidad exacta; continuo y monótono en el cue.
//
// QUÉ NO ES: no está validado perceptualmente (ABX pendiente, ver PRUEBAS) y no
// afirmo novedad frente a la literatura (SU(2)/Jones/Poincaré es matemática clásica;
// su aplicación a ILD/ITD por bin con estas invariantes no la encontré verificada).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <vector>

namespace ivanna::poincare {

using Cx = std::complex<double>;
inline constexpr double kPi = 3.14159265358979323846;

struct Jones { Cx l{0, 0}, r{0, 0}; };
struct Stokes { double s0, s1, s2, s3; };

inline Stokes stokes(const Jones& j) noexcept {
    const Cx c = std::conj(j.l) * j.r;
    return {std::norm(j.l) + std::norm(j.r), std::norm(j.l) - std::norm(j.r), 2.0 * c.real(), 2.0 * c.imag()};
}

struct SU2 { Cx a00{1, 0}, a01{0, 0}, a10{0, 0}, a11{1, 0}; };

// Rotación de ángulo δ alrededor del eje unitario k=(k1,k2,k3) en índices de Stokes
// (k1↔σz↔s1, k2↔σx↔s2, k3↔σy↔s3; terna (z,x,y) cíclica ⇒ diestra).
inline SU2 rotation(double k1, double k2, double k3, double delta) noexcept {
    const double c = std::cos(0.5 * delta), s = std::sin(0.5 * delta);
    SU2 u;
    u.a00 = Cx(c, -s * k1);
    u.a01 = Cx(-s * k3, -s * k2);
    u.a10 = Cx(+s * k3, -s * k2);
    u.a11 = Cx(c, +s * k1);
    return u;
}
inline Jones apply(const SU2& u, const Jones& x) noexcept {
    return {u.a00 * x.l + u.a01 * x.r, u.a10 * x.l + u.a11 * x.r};
}
inline SU2 mul(const SU2& A, const SU2& B) noexcept {
    SU2 C;
    C.a00 = A.a00 * B.a00 + A.a01 * B.a10;  C.a01 = A.a00 * B.a01 + A.a01 * B.a11;
    C.a10 = A.a10 * B.a00 + A.a11 * B.a10;  C.a11 = A.a10 * B.a01 + A.a11 * B.a11;
    return C;
}

// PAN (dirección): ILD por rotación real (mezcla de potencia constante L↔R) y IPD por
// rotación de fase simétrica ±β/2 (misma asignación de fases que ORBE: L −β/2, R +β/2).
//   ildDb > 0 ⇒ oído derecho más intenso;  itdSec > 0 ⇒ oído izquierdo llega más tarde.
//   Para entrada mono centrada da exactamente gR/gL = 10^(ild/20), fase R−L = ω·itd.
inline SU2 pan(double ildDb, double itdSec, double omega, double a) noexcept {
    a = std::min(1.0, std::max(0.0, a));
    const double u = std::atan(std::pow(10.0, ildDb / 20.0)) - 0.25 * kPi;  // medio ángulo ILD
    const SU2 ry = rotation(0.0, 0.0, 1.0, 2.0 * a * u);
    const SU2 rz = rotation(1.0, 0.0, 0.0, a * omega * itdSec);
    return mul(rz, ry);
}

// ANCHURA: contracción/expansión GEODÉSICA del ángulo γ entre s y el eje central s2.
// w<1 colapsa hacia el centro, w>1 ensancha; rotación unitaria ⇒ energía exacta.
inline Jones width(const Jones& x, double w) noexcept {
    const Stokes s = stokes(x);
    if (!(s.s0 > 1e-30) || !std::isfinite(s.s0)) return x;
    const double n1 = s.s1 / s.s0, n2 = std::min(1.0, std::max(-1.0, s.s2 / s.s0)), n3 = s.s3 / s.s0;
    const double sinG = std::sqrt(std::max(0.0, 1.0 - n2 * n2));
    if (sinG < 1e-9) return x;
    const double g = std::acos(n2);
    const double gt = std::min(kPi, std::max(0.0, w * g));
    // eje = e2 × n / |e2 × n| = (n3, 0, −n1)/sinG ; +δ aleja de e2.
    return apply(rotation(n3 / sinG, 0.0, -n1 / sinG, gt - g), x);
}

struct AmountSmoother {
    float v = 0.0f;
    float step(float target, float hopSec, float tauSec = 0.025f) noexcept {
        const float k = 1.0f - std::exp(-hopSec / tauSec);
        v += k * ((std::isfinite(target) ? target : 0.0f) - v);
        return v;
    }
};

// Operador de banda completa: STFT Hann N=512/hop=128 (COLA 1/1.5), mismo marco que ORBE/TΩ.
// Latencia N muestras. Cue físico inyectado por el llamador (p. ej. nearfield::ildDb/itdSeconds).
class Operator {
public:
    static constexpr int kN = 512, kHop = 128, kBins = kN / 2 + 1;

    void prepare(double fs) noexcept {
        fs_ = (fs > 8000.0 && fs < 384000.0) ? fs : 48000.0;
        for (int i = 0; i < kN; ++i) win_[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / kN);
        hist_.assign(2 * kN, 0.0);
        ola_.assign(2 * kN, 0.0);
        out_.assign(2 * (kN + 8192), 0.0);
        reset();
        for (int k = 0; k < kBins; ++k) { ild_[k] = 0.0; itd_[k] = 0.0; }
        rebuild(0.0f);
    }
    void reset() noexcept {
        std::fill(hist_.begin(), hist_.end(), 0.0);
        std::fill(ola_.begin(), ola_.end(), 0.0);
        std::fill(out_.begin(), out_.end(), 0.0);
        histWp_ = 0; inTotal_ = 0; next_ = kN; outWp_ = outRp_ = outLen_ = 0;
        amt_.v = 0.0f; wid_ = 1.0f;
    }
    // Escena: tablas de ILD/ITD por bin (hilo de control; CueFn(theta,r,f,&ildDb,&itdSec)).
    template <class CueFn>
    void setScene(double azimuthRad, double distanceM, CueFn&& cue) noexcept {
        for (int k = 0; k < kBins; ++k) {
            double ild = 0.0, itd = 0.0;
            cue(azimuthRad, distanceM, k * fs_ / kN, ild, itd);
            ild_[k] = std::isfinite(ild) ? ild : 0.0;
            itd_[k] = std::isfinite(itd) ? itd : 0.0;
        }
        rebuild(amt_.v);
    }
    void setAmount(float a) noexcept { amountTarget_ = std::min(1.0f, std::max(0.0f, a)); }
    void setWidth(float w) noexcept { widthTarget_ = std::min(2.0f, std::max(0.0f, w)); }
    int latencySamples() const noexcept { return kN; }

    // In-place float estéreo. Cualquier n ≥ 1.
    void process(float* l, float* r, int n) noexcept {
        if (!l || !r || n <= 0) return;
        const int fifo = static_cast<int>(out_.size() / 2);
        for (int i = 0; i < n; ++i) {
            hist_[2 * histWp_] = std::isfinite(l[i]) ? l[i] : 0.0;
            hist_[2 * histWp_ + 1] = std::isfinite(r[i]) ? r[i] : 0.0;
            histWp_ = (histWp_ + 1) % kN;
            if (++inTotal_ == next_) { next_ += kHop; frame(fifo); }
            if (outLen_ > 0) {
                l[i] = static_cast<float>(out_[2 * outRp_]);
                r[i] = static_cast<float>(out_[2 * outRp_ + 1]);
                outRp_ = (outRp_ + 1) % fifo; --outLen_;
            } else { l[i] = 0.0f; r[i] = 0.0f; }
        }
    }
    // Auditoría: peor error relativo de energía por bin visto en frames (I1).
    double worstBinEnergyError() const noexcept { return worstErr_; }

private:
    void rebuild(float a) noexcept {
        for (int k = 0; k < kBins; ++k) {
            const double w = 2.0 * kPi * k * fs_ / kN;
            const bool edge = (k == 0 || k == kN / 2);        // DC/Nyquist reales: solo ILD (real)
            u_[k] = pan(ild_[k], edge ? 0.0 : itd_[k], w, a);
        }
        builtA_ = a;
    }
    static void fft(Cx* x, bool inv) noexcept {
        for (int i = 1, j = 0; i < kN; ++i) {
            int bit = kN >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (int len = 2; len <= kN; len <<= 1) {
            const double ang = (inv ? 2.0 : -2.0) * kPi / len;
            const Cx wl(std::cos(ang), std::sin(ang));
            for (int i = 0; i < kN; i += len) {
                Cx w(1, 0);
                for (int k = 0; k < len / 2; ++k) {
                    const Cx u = x[i + k], v = x[i + k + len / 2] * w;
                    x[i + k] = u + v; x[i + k + len / 2] = u - v; w *= wl;
                }
            }
        }
        if (inv) for (int i = 0; i < kN; ++i) x[i] /= kN;
    }
    void frame(int fifo) noexcept {
        const double hopSec = static_cast<double>(kHop) / fs_;
        const float a = amt_.step(amountTarget_, static_cast<float>(hopSec));
        wid_ += 0.2f * (widthTarget_ - wid_);
        if (std::fabs(a - builtA_) > 1e-5f) rebuild(a);
        for (int i = 0; i < kN; ++i) {
            const int idx = (histWp_ + i) % kN;
            xl_[i] = Cx(hist_[2 * idx] * win_[i], 0.0);
            xr_[i] = Cx(hist_[2 * idx + 1] * win_[i], 0.0);
        }
        fft(xl_.data(), false); fft(xr_.data(), false);
        for (int k = 0; k < kBins; ++k) {
            Jones x{xl_[k], xr_[k]};
            const bool edge = (k == 0 || k == kN / 2);
            const double ein = std::norm(x.l) + std::norm(x.r);
            if (!edge && std::fabs(wid_ - 1.0f) > 1e-4f) x = width(x, wid_);
            const Jones y = apply(u_[k], x);
            if (ein > 1e-20) worstErr_ = std::max(worstErr_, std::fabs((std::norm(y.l) + std::norm(y.r)) / ein - 1.0));
            xl_[k] = y.l; xr_[k] = y.r;
            if (k > 0 && k < kN / 2) { xl_[kN - k] = std::conj(y.l); xr_[kN - k] = std::conj(y.r); }
        }
        xl_[0] = Cx(xl_[0].real(), 0.0); xr_[0] = Cx(xr_[0].real(), 0.0);
        xl_[kN / 2] = Cx(xl_[kN / 2].real(), 0.0); xr_[kN / 2] = Cx(xr_[kN / 2].real(), 0.0);
        fft(xl_.data(), true); fft(xr_.data(), true);
        for (int i = 0; i < kN; ++i) {
            ola_[2 * i] += xl_[i].real() * win_[i];
            ola_[2 * i + 1] += xr_[i].real() * win_[i];
        }
        constexpr double kCola = 1.0 / 1.5;   // Σw² = (N/hop)·0.375 = 1.5
        for (int i = 0; i < kHop; ++i) {
            if (outLen_ < fifo) {
                out_[2 * outWp_] = ola_[2 * i] * kCola;
                out_[2 * outWp_ + 1] = ola_[2 * i + 1] * kCola;
                outWp_ = (outWp_ + 1) % fifo; ++outLen_;
            }
        }
        for (int i = 0; i < 2 * (kN - kHop); ++i) ola_[i] = ola_[i + 2 * kHop];
        for (int i = 2 * (kN - kHop); i < 2 * kN; ++i) ola_[i] = 0.0;
    }

    double fs_ = 48000.0;
    std::array<double, kN> win_{};
    std::array<double, kBins> ild_{}, itd_{};
    std::array<SU2, kBins> u_{};
    std::array<Cx, kN> xl_{}, xr_{};
    std::vector<double> hist_, ola_, out_;
    int histWp_ = 0, outWp_ = 0, outRp_ = 0, outLen_ = 0;
    long long inTotal_ = 0, next_ = kN;
    AmountSmoother amt_;
    float amountTarget_ = 0.0f, widthTarget_ = 1.0f, wid_ = 1.0f, builtA_ = -1.0f;
    double worstErr_ = 0.0;
};

// Auditoría analítica de los invariantes (sin audio). true = OK.
inline bool selfTest() noexcept {
    double worst = 0.0, cueErr = 0.0;
    for (int t = 0; t < 2000; ++t) {
        const double p = t * 0.7311;
        Jones x{Cx(std::cos(p), std::sin(1.7 * p)), Cx(std::sin(2.3 * p), std::cos(0.9 * p))};
        const double e0 = std::norm(x.l) + std::norm(x.r);
        const double w = 0.1 + 1.9 * (0.5 + 0.5 * std::sin(0.37 * p));
        const double a = 0.5 + 0.5 * std::sin(0.11 * p);
        Jones y = apply(pan(12.0 * std::sin(p), 6e-4 * std::cos(1.3 * p), 2 * kPi * (100 + 9000 * (0.5 + 0.5 * std::sin(p))), a), width(x, w));
        worst = std::max(worst, std::fabs((std::norm(y.l) + std::norm(y.r)) / e0 - 1.0));
    }
    // Cue: mono centrado → ILD y fase exactos.
    const Jones c{Cx(1 / std::sqrt(2.0), 0), Cx(1 / std::sqrt(2.0), 0)};
    const double ild = 7.3, itd = 4.1e-4, om = 2 * kPi * 900.0;
    const Jones y = apply(pan(ild, itd, om, 1.0), c);
    cueErr = std::fabs(20 * std::log10(std::abs(y.r) / std::abs(y.l)) - ild) +
             std::fabs(std::arg(y.r / y.l) - om * itd);
    // Anchura: w=0 colapsa a s2-eje (s1=s3=0), energía intacta.
    const Jones z = width(Jones{Cx(0.8, 0.1), Cx(-0.3, 0.5)}, 0.0);
    const Stokes sz = stokes(z);
    return worst < 1e-12 && cueErr < 1e-9 && std::fabs(sz.s1) < 1e-9 * sz.s0 && std::fabs(sz.s3) < 1e-9 * sz.s0;
}

}  // namespace ivanna::poincare
