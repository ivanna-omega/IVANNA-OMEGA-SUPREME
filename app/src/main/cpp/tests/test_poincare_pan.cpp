// test_poincare_pan.cpp — Pan Poincaré de campo cercano cableado en
// FusedSpatial (sustituye el spread WFS) detrás del flag
// FusedSpatial::kPoincarePanEnabled.
//
// ESTE TARGET COMPILA CON EL FLAG ON (-DIVANNA_POINCARE_PAN_ENABLED=1,
// ver CMakeLists.txt): es el único que ejercita el camino ON. El resto
// de la suite corre con el flag OFF (default) y el test
// WfsStageGoldenWithPoincareOff (test_fused_spatial.cpp) clava ese
// camino. Si este target se compila sin el define, el test FlagIsOn
// cae a propósito: no se puede "probar el ON" con el flag apagado.
//
// El cue es SOLO nearFieldParallax (rayos, Parte I de
// physical_propagation.hpp). NUNCA las funciones de ley de curvatura
// (Parte II, REFUTADAS). La medición es la de la comparación
// test_poincare_vs_orbe.cpp (proyección LS con suma de Kahan,
// convención de rayos: ITD > 0 ⇔ L más tarde ⇔ derecha).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_spatial.hpp"
#include "../fused/physical_propagation.hpp"
#include "../spatial/poincare_binaural.hpp"

using ivanna::fused::FusedSpatial;
using ivanna::fused::SpatialControls;
using ivanna::fused::propagation::nearFieldParallax;
using ivanna::OmegaOutputMode;

namespace {

constexpr double kSr = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 6.28318530717958647692;
constexpr int kHop = 128;

// ── Proyección LS sobre sin/cos a f (ciclos enteros ⇒ exacta) ─────────────
// Devuelve (a, b) con x(t) ≈ a·sin(ωt) + b·cos(ωt); |C| = √(a²+b²),
// fase φ con a = A·cosφ, b = A·sinφ. Mismo método que P0 y que la
// comparación Poincaré vs ORBE.
void projectSinCos(const double* x, size_t n, double freqHz, double& a, double& b) {
    double s = 0.0, c = 0.0, cs = 0.0, cc = 0.0;
    double phase = 0.0;
    const double step = kTwoPi * freqHz / kSr;
    for (size_t i = 0; i < n; ++i) {
        const double v = x[i];
        const double yS = v * std::sin(phase) - cs;
        const double tS = s + yS; cs = (tS - s) - yS; s = tS;
        const double yC = v * std::cos(phase) - cc;
        const double tC = c + yC; cc = (tC - c) - yC; c = tC;
        phase += step;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
    a = 2.0 * s / static_cast<double>(n);
    b = 2.0 * c / static_cast<double>(n);
}

// ILD (dB) e ITD (s) entre dos canales a frecuencia f, por proyección LS.
// ITD > 0 ⇔ el oído IZQUIERDO llega más tarde ⇔ fuente a la DERECHA.
// |2πf·itd| debe ser < π (f ≤ 500 Hz con itd ≤ 0.73 ms).
void measureIldItd(const double* l, const double* r, size_t n, double f,
                   double& ildDb, double& itdSec) {
    double aL = 0, bL = 0, aR = 0, bR = 0;
    projectSinCos(l, n, f, aL, bL);
    projectSinCos(r, n, f, aR, bR);
    const double ampL = std::sqrt(aL * aL + bL * bL);
    const double ampR = std::sqrt(aR * aR + bR * bR);
    ildDb = 20.0 * std::log10(std::max(ampR, 1e-30) / std::max(ampL, 1e-30));
    const double phiL = std::atan2(bL, aL), phiR = std::atan2(bR, aR);
    double dphi = phiR - phiL;
    while (dphi > kPi) dphi -= kTwoPi;
    while (dphi <= -kPi) dphi += kTwoPi;
    itdSec = dphi / (kTwoPi * f);
}

// Multi-tono seco (centrado, L=R): 5 tonos con ciclos enteros en 1 s.
std::vector<double> makeDryMultiTone(size_t n, double amp = 0.15) {
    static const double kFreqs[] = {250, 500, 1000, 2000, 4000};
    std::vector<double> x(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(i) / kSr;
        double v = 0.0;
        for (double f : kFreqs) v += amp * std::sin(kTwoPi * f * t);
        x[i] = v;
    }
    return x;
}

// Ruido blanco determinista (xorshift64, semilla fija) en [-amp, +amp].
std::vector<double> makeNoise(size_t n, double amp, std::uint64_t seed) {
    std::vector<double> x(n);
    std::uint64_t s = seed;
    for (auto& v : x) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        const double u = static_cast<double>(s >> 11) / 9007199254740992.0;
        v = (2.0 * u - 1.0) * amp;
    }
    return x;
}

// Configuración que selecciona la etapa Wfs (el slot que el pan Poincaré
// sustituye con flag ON): ORBE 0, HRTF 0, upmix off, wfsSpread 1.
SpatialControls wfsStageControls(float azimuthDeg, float distanceM) {
    SpatialControls c;
    c.amount = 0.0f;
    c.azimuthDeg = azimuthDeg;
    c.distanceM = distanceM;
    c.width = 1.0f;
    c.mode = OmegaOutputMode::HEADPHONES;
    c.wfsSpread = 1.0f;
    c.hrtfWet = 0.0f;
    c.headTracking = false;
    c.xtcEnabled = false;
    c.upmixEnabled = false;
    c.clamp();
    return c;
}

double totalEnergy(const std::vector<double>& l, const std::vector<double>& r,
                   size_t from, size_t to) {
    double e = 0.0;
    for (size_t i = from; i < to; ++i) e += l[i] * l[i] + r[i] * r[i];
    return e;
}

}  // namespace

// Guarda: este target DEBE compilar con el flag ON. Si alguien lo
// registra sin -DIVANNA_POINCARE_PAN_ENABLED=1, este test cae en vez de
// dar un falso verde sobre el camino WFS.
TEST(PoincarePan, FlagIsOn) {
    EXPECT_TRUE(FusedSpatial::kPoincarePanEnabled);
}

// Auditoría analítica de los invariantes del operador (sin audio).
TEST(PoincarePan, OperatorSelfTest) {
    EXPECT_TRUE(ivanna::poincare::selfTest());
    ivanna::poincare::Operator op;
    op.prepare(kSr);
    const auto cue = nearFieldParallax(30.0 * kPi / 180.0, 1.0);
    op.setScene(30.0 * kPi / 180.0, 1.0,
                [&](double, double, double, double& ild, double& itd) {
                    ild = cue.ildDivergenceDb;
                    itd = cue.itdSeconds;
                });
    op.setAmount(1.0f);
    op.setWidth(1.0f);
    std::vector<float> l(48000, 0.0f), r(48000, 0.0f);
    for (size_t i = 0; i < l.size(); ++i) {
        const double t = static_cast<double>(i) / kSr;
        l[i] = r[i] = static_cast<float>(0.5 * std::sin(kTwoPi * 1000.0 * t));
    }
    for (size_t off = 0; off < l.size(); off += kHop)
        op.process(l.data() + off, r.data() + off, kHop);
    // Energía por bin exacta (invariante I1): error < 1e-12.
    EXPECT_LT(op.worstBinEnergyError(), 1e-12);
}

// Cue exacto por el camino cableado (FusedSpatial con flag ON) en la
// rejilla de la comparación: θ ∈ {0,30,60,90}° × r ∈ {0.15,0.5,1,2} m.
// ILD ≤ 0.01 dB e ITD ≤ 1 µs (la comparación standalone medía 0.0001 dB
// y 0.10 µs; el cableado añade solo la ida y vuelta float y la rampa
// wfsMix_, ambas asentadas en el segundo 2, que es la ventana medida).
TEST(PoincarePan, CueAccuracyWired) {
    const double kThetas[] = {0, 30, 60, 90};
    const double kDists[] = {0.15, 0.5, 1.0, 2.0};
    const double kFreqs[] = {250, 500, 1000, 2000, 4000};
    const size_t kN = 96000;               // 2 s
    const size_t kMeasA = 48000, kMeasN = 48000;  // segundo 2 (régimen)
    double maxIldErr = 0.0, maxItdErrUs = 0.0;
    for (double thetaDeg : kThetas) {
        for (double distM : kDists) {
            const double theta = thetaDeg * kPi / 180.0;
            const auto cue = nearFieldParallax(theta, distM);
            const std::vector<double> dry = makeDryMultiTone(kN);
            FusedSpatial s;
            s.prepare(kSr);
            s.setControls(wfsStageControls(static_cast<float>(thetaDeg),
                                           static_cast<float>(distM)));
            std::vector<double> outL = dry, outR = dry;
            s.process(outL.data(), outR.data(), static_cast<int>(kN));
            for (double f : kFreqs) {
                double ild = 0.0, itd = 0.0;
                measureIldItd(outL.data() + kMeasA, outR.data() + kMeasA, kMeasN, f,
                              ild, itd);
                const double ildErr = std::fabs(ild - cue.ildDivergenceDb);
                maxIldErr = std::max(maxIldErr, ildErr);
                EXPECT_LE(ildErr, 0.01)
                    << "ILD θ=" << thetaDeg << " r=" << distM << " f=" << f;
                if (f <= 500.0) {
                    const double itdErrUs =
                        std::fabs((itd - cue.itdSeconds) * 1e6);
                    maxItdErrUs = std::max(maxItdErrUs, itdErrUs);
                    EXPECT_LE(itdErrUs, 1.0)
                        << "ITD θ=" << thetaDeg << " r=" << distM << " f=" << f;
                }
            }
        }
    }
    std::printf("[poincare_pan] cue cableado: max ILD err %.4f dB, max ITD err %.2f µs\n",
                maxIldErr, maxItdErrUs);
}

// Ganancia neta ≈ 0 dB (el operador es unitario por bin) y silencio
// digital con flag ON: ruido blanco mono → ΔE < 0.1 dB; ceros → ceros
// exactos.
TEST(PoincarePan, NetGainAndSilence) {
    const size_t kN = 96000;  // 2 s
    const size_t kMeasA = 48000, kMeasN = 48000;
    const double kRms = 0.125893;  // -18 dBFS RMS
    // Ruido mono (L=R): el pan lo rota sin tocar la energía total.
    const std::vector<double> in = makeNoise(kN, kRms * 1.7320508075688772, 0x12345678ULL);
    FusedSpatial s;
    s.prepare(kSr);
    s.setControls(wfsStageControls(30.0f, 1.0f));
    std::vector<double> outL = in, outR = in;
    s.process(outL.data(), outR.data(), static_cast<int>(kN));
    const double eIn = totalEnergy(in, in, kMeasA, kMeasA + kMeasN);
    const double eOut = totalEnergy(outL, outR, kMeasA, kMeasA + kMeasN);
    const double dDb = 10.0 * std::log10(eOut / eIn);
    std::printf("[poincare_pan] ganancia neta ruido mono 30°/1 m: %+.4f dB\n", dDb);
    EXPECT_LE(std::fabs(dDb), 0.1);

    // Silencio digital: ceros → ceros exactos (sin DC ni cola).
    const std::vector<double> zeros(kN, 0.0);
    FusedSpatial s2;
    s2.prepare(kSr);
    s2.setControls(wfsStageControls(60.0f, 0.5f));
    std::vector<double> zL = zeros, zR = zeros;
    s2.process(zL.data(), zR.data(), static_cast<int>(kN));
    double peak = 0.0;
    for (size_t i = 0; i < kN; ++i)
        peak = std::max(peak, std::max(std::fabs(zL[i]), std::fabs(zR[i])));
    EXPECT_EQ(peak, 0.0);
}
