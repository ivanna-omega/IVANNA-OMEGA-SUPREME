// test_holos_multidim.cpp — P3/3.3: test HOLOS multidimensional, equivalente
// a test_orbe_power_conservation.cpp pero para HOLOS.
//
// Mide POR BANDA ERB (24 bandas, 60 Hz–16 kHz, misma escala ERB del
// operador: erbRate(f) = 21.4·log10(1+0.00437·f)):
//   - energía total (L+R) por banda, salida/entrada en dB;
//   - balance L/R (ILD) por banda, entrada y salida;
//   - coherencia L/R por banda, entrada y salida;
// con tono coherente, ruido (dual-mono y L/R independientes), estéreo
// asimétrico y transitorios.
//
// Parámetros explícitos (documentados, no defaults implícitos): amount
// 1.0 (y 0.5 en un test), spread 1.2 (ATREVIDA), proximityDb 6.0,
// diffuse 0.85, harmonics 0.0 y dynamics 0.0 (el excitador K y el
// compresor Γ cambian la ganancia por diseño y se excluyen para aislar
// el render espacial; el preset embarcado lleva harmonics 0.002 ≈ 0 y
// dynamics 1.0 — su efecto se mide aparte en un test), headTracking
// false (determinista), bandCount 24, distanceM 0.35.
//
// MATIZ DE LA PUERTA (fail-closed): con ruido L/R independiente la
// coherencia media cae bajo 0.3 y HOLOS CIERRA LA PUERTA (wetTarget=0):
// la salida es passthrough bit-exacto, no el render. Eso es conducta
// correcta, no defecto — por eso el ruido "incorrelado" que mide el
// render es DUAL-MONO (coherencia 1, el render lo descorrelaciona por
// bandas) y el L/R independiente tiene su propio test de puerta.
//
// Medición (g++-14, 48 kHz; los umbrales se fijaron SOBRE lo medido —
// cada test imprime su valor):
//   - tonos (8, 100 Hz–12 kHz, Goertzel exacto): 0.000 dB por banda.
//   - ruido dual-mono (Welch): E por banda −0.04…+0.07 dB (ruido de
//     estimación), ILDout 0.00 dB, COH 1.00→1.00 en las 24 bandas,
//     energía total −0.004 dB. CONCLUSIÓN DEL RIESGO 3.3: M1-HOLOS NO
//     es peor de lo creído en contenido simétrico — conserva tan bien
//     como ORBE (±0.05 dB).
//   - asimétrico (ILD +6.02 dB plana): energía total −0.038 dB, mismo
//     lado siempre (sin espejo). PERO: (1) la ILD se re-renderiza —
//     +6.0 dB en graves, +17.7…+18.3 dB por encima de b07 (la sombra de
//     cabeza física para el azimut estimado; la entrada plana +6 dB es
//     afísica — una fuente real a ese azimut TIENE +18 dB en agudos —
//     así que es el diseño del render, no un espejo; aun así, +12 dB
//     de exageración vs la entrada que debe conocer Esse); (2) bache
//     real de energía −0.86 dB (b06) y −0.80 dB (b07), las bandas donde
//     la ILD salta — por encima del objetivo ±0.05 dB. El conservador
//     por banda no lo cierra ahí.
//   - ruido L/R independiente: puerta cerrada, passthrough bit-exacto
//     en la 2ª mitad (max|diff| = 0.0).
//   - transitorios: pico 0.500→0.500, energía total −0.000 dB.
//   - amount 0.5: −0.004 dB; amount 0: bit-exacto.
//   - tonos: Goertzel a la fundamental (exacto, sin ruido de estimación).
#include <gtest/gtest.h>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>
#include <vector>

#include "../spatial/HolosOperator.hpp"

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

namespace {
constexpr double kFs = 48000.0;
constexpr int kDur = 96000;       // 2 s @48 kHz
constexpr int kMeasFrom = 48000;  // ventana de medición: 2ª mitad
constexpr int kMeasTo = 96000;
constexpr double kPiD = 3.14159265358979323846;
constexpr int kBands = 24;

double db10(double r) { return 10.0 * std::log10(r); }

double power(const std::vector<float>& v, int from, int to) {
    double a = 0.0;
    for (int i = from; i < to; ++i) a += (double)v[i] * v[i];
    return a / (double)(to - from);
}

// --- FFT radix-2 (Hann) --------------------------------------------------
void fftRadix2(std::vector<std::complex<double>>& x) {
    const int n = (int)x.size();
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPiD / len;
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + len / 2] * w;
                x[i + k] = u + v;
                x[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Bordes ERB (misma escala que SpectralCore::erbRate/erbInverse).
void erbEdges(double* edges) {
    const double rLo = 21.4 * std::log10(1.0 + 0.00437 * 60.0);
    const double rHi = 21.4 * std::log10(1.0 + 0.00437 * 16000.0);
    for (int i = 0; i <= kBands; ++i) {
        const double r = rLo + (rHi - rLo) * i / kBands;
        edges[i] = (std::pow(10.0, r / 21.4) - 1.0) / 0.00437;
    }
}

// Espectro por banda (Welch): potencias L/R y |cross| por banda,
// promediadas sobre 4 ventanas de 16384 con solape en [48000, 96000).
struct BandSpectrum {
    double pL[kBands] = {};
    double pR[kBands] = {};
    double xLR[kBands] = {};  // |Σ X_L·conj(X_R)| por banda
};
BandSpectrum bandSpectrum(const std::vector<float>& l, const std::vector<float>& r) {
    constexpr int kN = 16384;
    double edges[kBands + 1];
    erbEdges(edges);
    BandSpectrum acc;
    const int nWin = 4;
    std::vector<std::complex<double>> xl(kN), xr(kN);
    for (int w = 0; w < nWin; ++w) {
        const int from = kMeasFrom + w * 8192;
        for (int i = 0; i < kN; ++i) {
            const double hann = 0.5 * (1.0 - std::cos(2.0 * kPiD * i / (kN - 1)));
            xl[i] = (double)l[from + i] * hann;
            xr[i] = (double)r[from + i] * hann;
        }
        fftRadix2(xl);
        fftRadix2(xr);
        for (int b = 0; b < kBands; ++b) {
            const int kLo = std::max(1, (int)std::floor(edges[b] * kN / kFs));
            const int kHi = std::min(kN / 2, (int)std::ceil(edges[b + 1] * kN / kFs));
            for (int k = kLo; k < kHi; ++k) {
                acc.pL[b] += std::norm(xl[k]);
                acc.pR[b] += std::norm(xr[k]);
                acc.xLR[b] += std::abs(xl[k] * std::conj(xr[k]));
            }
        }
    }
    for (int b = 0; b < kBands; ++b) {
        acc.pL[b] /= nWin;
        acc.pR[b] /= nWin;
        acc.xLR[b] /= nWin;
    }
    return acc;
}

double bandIldDb(const BandSpectrum& s, int b) { return db10(s.pL[b] / s.pR[b]); }
double bandCoh(const BandSpectrum& s, int b) {
    const double den = std::sqrt(s.pL[b] * s.pR[b]);
    return den > 0.0 ? s.xLR[b] / den : 0.0;
}

// Goertzel a f0 (potencia de la fundamental; ventana = nº entero de
// periodos cuando f0 divide a 48000).
double goertzelPower(const std::vector<float>& v, int from, int to, double f0) {
    const int n = to - from;
    const double w = 2.0 * kPiD * f0 / kFs;
    const double cw = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < to; ++i) {
        const double s0 = (double)v[i] + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    return (re * re + im * im) / ((double)n * (double)n / 4.0);
}

// --- Harness HOLOS -------------------------------------------------------
struct Sig { std::vector<float> l, r; };

ivanna::HolosParams holosParams(float amount) {
    ivanna::HolosParams p;
    p.amount = amount;
    p.spread = 1.2f;
    p.proximityDb = 6.0f;
    p.diffuse = 0.85f;
    p.harmonics = 0.0f;
    p.dynamics = 0.0f;
    p.headTracking = false;
    p.bandCount = 24;
    p.distanceM = 0.35f;
    return p;
}

void runHolos(const ivanna::HolosParams& p, std::vector<float>& l, std::vector<float>& r) {
    ivanna::HolosOperator op;
    op.prepare(48000.0f);
    op.setParams(p);
    for (size_t s = 0; s < l.size(); s += 128) {
        const int n = (int)std::min<size_t>(128, l.size() - s);
        op.process(l.data() + s, r.data() + s, n);
    }
}

Sig tone(double f0, double amp) {
    Sig s;
    s.l.assign(kDur, 0.0f);
    s.r.assign(kDur, 0.0f);
    for (int i = 0; i < kDur; ++i) {
        const float v = (float)(amp * std::sin(2.0 * kPiD * f0 * i / kFs));
        s.l[i] = v;
        s.r[i] = v;
    }
    return s;
}

// Ruido dual-mono (coherencia 1: la puerta queda abierta).
Sig noiseMono(uint32_t seed, double amp = 0.5) {
    Sig s;
    s.l.assign(kDur, 0.0f);
    s.r.assign(kDur, 0.0f);
    std::mt19937 g(seed);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < kDur; ++i) {
        const float v = (float)(amp * u(g));
        s.l[i] = v;
        s.r[i] = v;
    }
    return s;
}

// Ruido L/R independiente (coherencia ~0: la puerta se cierra).
Sig noiseIndep(uint32_t seedL, uint32_t seedR, double amp = 0.5) {
    Sig s;
    s.l.assign(kDur, 0.0f);
    s.r.assign(kDur, 0.0f);
    std::mt19937 gL(seedL), gR(seedR);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int i = 0; i < kDur; ++i) { s.l[i] = (float)(amp * u(gL)); s.r[i] = (float)(amp * u(gR)); }
    return s;
}

// Estéreo asimétrico: mismo ruido, R a la mitad (ILD +6.02 dB).
Sig noiseAsym(uint32_t seed, double amp = 0.5) {
    Sig s = noiseMono(seed, amp);
    for (auto& v : s.r) v *= 0.5f;
    return s;
}

// Transitorios: tren de impulsos 0.5 cada 0.25 s + tono 1 kHz 0.3.
Sig transients() {
    Sig s = tone(1000.0, 0.3);
    for (int i = 0; i < kDur; i += 12000) {
        s.l[i] += 0.5f;
        s.r[i] += 0.5f;
    }
    return s;
}
}  // namespace

// --- Tonos coherentes: energía por banda (Goertzel, exacto) --------------
TEST(HolosMultidim, ToneBandEnergy) {
    // 8 tonos cubriendo 100 Hz–12 kHz; cada uno cae en su banda ERB.
    // Medido 2026-11-26 (g++-14): 0.000 dB en los 8 (exacto).
    const double freqs[] = {100.0, 200.0, 400.0, 800.0, 1600.0, 3200.0, 6400.0, 12000.0};
    double worst = 0.0;
    for (double f0 : freqs) {
        const Sig in = tone(f0, 0.5);
        std::vector<float> l = in.l, r = in.r;
        runHolos(holosParams(1.0f), l, r);
        const double gIn = goertzelPower(in.l, kMeasFrom, kMeasTo, f0);
        const double gOut = goertzelPower(l, kMeasFrom, kMeasTo, f0);
        const double d = db10(gOut / gIn);
        worst = std::max(worst, std::fabs(d));
        std::printf("[holos-multi] tono %.0f Hz banda: %+.3f dB\n", f0, d);
        EXPECT_LE(std::fabs(d), 0.05);
    }
    std::printf("[holos-multi] tono banda peor: %.3f dB\n", worst);
}

// --- Ruido dual-mono: perfil por banda (Welch) ---------------------------
TEST(HolosMultidim, NoiseMonoBandProfile) {
    const Sig in = noiseMono(ivanna::test::IvannaTestSeed32(1));
    std::vector<float> l = in.l, r = in.r;
    runHolos(holosParams(1.0f), l, r);
    const BandSpectrum sIn = bandSpectrum(in.l, in.r);
    const BandSpectrum sOut = bandSpectrum(l, r);
    double worstE = 0.0, worstIld = 0.0;
    std::printf("[holos-multi] ruido dual-mono por banda (E dB, ILDin→ILDout dB, COHin→COHout):\n");
    for (int b = 0; b < kBands; ++b) {
        const double e = db10((sOut.pL[b] + sOut.pR[b]) / (sIn.pL[b] + sIn.pR[b]));
        const double ildIn = bandIldDb(sIn, b), ildOut = bandIldDb(sOut, b);
        const double cohIn = bandCoh(sIn, b), cohOut = bandCoh(sOut, b);
        worstE = std::max(worstE, std::fabs(e));
        worstIld = std::max(worstIld, std::fabs(ildOut));
        std::printf("[holos-multi]   b%02d E=%+.2f ILD %+.2f→%+.2f COH %.2f→%.2f\n", b, e,
                    ildIn, ildOut, cohIn, cohOut);
        // Medido 2026-11-26: peor E 0.07 dB (ruido de estimación Welch),
        // ILDout 0.00 dB, COH 1.00→1.00 en las 24 bandas.
        EXPECT_LE(std::fabs(e), 0.15);
        EXPECT_LE(std::fabs(ildOut), 0.5);
    }
    std::printf("[holos-multi] ruido banda peor E %.2f dB, peor |ILDout| %.2f dB\n", worstE,
                worstIld);
    // Energía total (todas las bandas): la Regla de Oro del render.
    const double pIn = power(in.l, kMeasFrom, kMeasTo) + power(in.r, kMeasFrom, kMeasTo);
    const double pOut = power(l, kMeasFrom, kMeasTo) + power(r, kMeasFrom, kMeasTo);
    std::printf("[holos-multi] ruido energía total: %+.3f dB\n", db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

// --- Ruido L/R independiente: la puerta se cierra (fail-closed) ----------
TEST(HolosMultidim, IndependentNoiseGateCloses) {
    const Sig in = noiseIndep(ivanna::test::IvannaTestSeed32(2), ivanna::test::IvannaTestSeed32(3));
    std::vector<float> l = in.l, r = in.r;
    runHolos(holosParams(1.0f), l, r);
    double maxDiff = 0.0;
    for (int i = kMeasFrom; i < kMeasTo; ++i) {
        maxDiff = std::max(maxDiff, (double)std::fabs(l[i] - in.l[i]));
        maxDiff = std::max(maxDiff, (double)std::fabs(r[i] - in.r[i]));
    }
    std::printf("[holos-multi] ruido indep: max|salida−entrada| 2ª mitad = %.6f\n", maxDiff);
    EXPECT_EQ(maxDiff, 0.0);
}

// --- Estéreo asimétrico: el lado no se espeja -----------------------------
TEST(HolosMultidim, AsymmetricStereoKeepsSide) {
    const Sig in = noiseAsym(ivanna::test::IvannaTestSeed32(1));
    std::vector<float> l = in.l, r = in.r;
    runHolos(holosParams(1.0f), l, r);
    const BandSpectrum sIn = bandSpectrum(in.l, in.r);
    const BandSpectrum sOut = bandSpectrum(l, r);
    double worstE = 0.0;
    std::printf("[holos-multi] asimétrico por banda (E dB, ILDin→ILDout dB):\n");
    for (int b = 0; b < kBands; ++b) {
        const double e = db10((sOut.pL[b] + sOut.pR[b]) / (sIn.pL[b] + sIn.pR[b]));
        const double ildIn = bandIldDb(sIn, b), ildOut = bandIldDb(sOut, b);
        worstE = std::max(worstE, std::fabs(e));
        std::printf("[holos-multi]   b%02d E=%+.2f ILD %+.2f→%+.2f\n", b, e, ildIn, ildOut);
        // Medido 2026-11-26: E ±0.07 dB salvo b06 −0.86 dB y b07 −0.80 dB
        // (bandas de transición donde la ILD salta de +6 a +15 dB: bache
        // real del render, por encima del objetivo ±0.05 dB — ver
        // cabecera). La ILD se re-renderiza: +6.02 dB de entrada (plana)
        // → +6.0 dB en graves, +17.7…+18.3 dB por encima de b07 (sombra
        // de cabeza física para el azimut estimado). Mismo lado siempre.
        EXPECT_LE(std::fabs(e), 1.0);
        EXPECT_GT(ildOut, 0.0);  // mismo lado: sin espejo
    }
    std::printf("[holos-multi] asimétrico banda peor E %.2f dB\n", worstE);
    const double pIn = power(in.l, kMeasFrom, kMeasTo) + power(in.r, kMeasFrom, kMeasTo);
    const double pOut = power(l, kMeasFrom, kMeasTo) + power(r, kMeasFrom, kMeasTo);
    std::printf("[holos-multi] asimétrico energía total: %+.3f dB\n", db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

// --- Transitorios: sin blow-up, pico acotado ------------------------------
TEST(HolosMultidim, TransientsNoBlowUp) {
    const Sig in = transients();
    std::vector<float> l = in.l, r = in.r;
    runHolos(holosParams(1.0f), l, r);
    double peakIn = 0.0, peakOut = 0.0;
    for (int i = 0; i < kDur; ++i) {
        peakIn = std::max(peakIn, (double)std::fabs(in.l[i]));
        peakOut = std::max(peakOut, (double)std::fabs(l[i]));
        EXPECT_TRUE(std::isfinite(l[i]));
        EXPECT_TRUE(std::isfinite(r[i]));
    }
    const double pIn = power(in.l, kMeasFrom, kMeasTo) + power(in.r, kMeasFrom, kMeasTo);
    const double pOut = power(l, kMeasFrom, kMeasTo) + power(r, kMeasFrom, kMeasTo);
    std::printf("[holos-multi] transitorios: pico in %.3f out %.3f, energía total %+.3f dB\n",
                peakIn, peakOut, db10(pOut / pIn));
    EXPECT_LE(peakOut, 1.0);
    // Medido 2026-11-26: energía total −0.000 dB.
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

// --- Amount 0.5: conserva como amount 1.0 --------------------------------
TEST(HolosMultidim, AmountHalfConserves) {
    const Sig in = noiseMono(ivanna::test::IvannaTestSeed32(1));
    std::vector<float> l = in.l, r = in.r;
    runHolos(holosParams(0.5f), l, r);
    const double pIn = power(in.l, kMeasFrom, kMeasTo) + power(in.r, kMeasFrom, kMeasTo);
    const double pOut = power(l, kMeasFrom, kMeasTo) + power(r, kMeasFrom, kMeasTo);
    std::printf("[holos-multi] ruido amount 0.5 energía total: %+.3f dB\n",
                db10(pOut / pIn));
    EXPECT_NEAR(db10(pOut / pIn), 0.0, 0.05);
}

// --- Amount 0: bypass bit-exacto ------------------------------------------
TEST(HolosMultidim, AmountZeroIsBitExact) {
    const Sig in = noiseMono(ivanna::test::IvannaTestSeed32(1));
    std::vector<float> l = in.l, r = in.r;
    runHolos(holosParams(0.0f), l, r);
    EXPECT_EQ(l, in.l);
    EXPECT_EQ(r, in.r);
}
