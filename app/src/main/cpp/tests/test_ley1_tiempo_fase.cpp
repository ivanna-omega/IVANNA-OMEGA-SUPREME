// test_ley1_tiempo_fase.cpp — Ley 1: Dominio Total del Tiempo y la Fase.
//
// Tres suites:
//  1. GammaComplex: Γ complejo de fase mixta (Bode + lineal).
//  2. ListenerGeometry + SupremeMsoFarrowArbitrator: alineación entera +
//     fraccional desde la geometría (el ITD completo se honra).
//  3. ImpulseInverse: FIR inverso de fase mínima desde una magnitud medida.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "../spatial/GammaComplex.hpp"
#include "../spatial/ImpulseInverse.hpp"
#include "../spatial/fft_radix2.hpp"
#include "../supreme/ListenerGeometry.hpp"
#include "../supreme/ShmPipelineArbitrator.hpp"

using namespace ivanna::holos;
using namespace ivanna::supreme;

namespace {

// Centros ERB de ejemplo (24 bandas, 60 Hz .. 16 kHz, ~log).
void erbCenters(float* f, int n) {
    const double f0 = 60.0, f1 = 16000.0;
    for (int i = 0; i < n; ++i) {
        const double t = (n > 1) ? (double)i / (double)(n - 1) : 0.0;
        f[i] = (float)(f0 * std::pow(f1 / f0, t));
    }
}

int argmaxAbs(const float* x, int n) {
    int best = 0;
    float bestV = -1.0f;
    for (int i = 0; i < n; ++i) {
        const float v = std::fabs(x[i]);
        if (v > bestV) { bestV = v; best = i; }
    }
    return best;
}

}  // namespace

// ── 1. Γ complejo ───────────────────────────────────────────────────────────
TEST(Ley1Gamma, FlatMagnitudeIsIdentity) {
    // Magnitud plana → (re, im) = (1, 0) exacto: camino real bit-idéntico.
    constexpr int kBands = 24;
    float mag[kBands];
    float fc[kBands];
    float re[kBands], im[kBands];
    for (int b = 0; b < kBands; ++b) mag[b] = 1.0f;
    erbCenters(fc, kBands);
    complexGammaBands(mag, fc, kBands, 48000.0f, 0.0f, re, im);
    for (int b = 0; b < kBands; ++b) {
        EXPECT_FLOAT_EQ(re[b], 1.0f);
        EXPECT_FLOAT_EQ(im[b], 0.0f);
    }
}

TEST(Ley1Gamma, RisingMagnitudeHasPositivePhase) {
    // Relación de Bode: magnitud que SUBE con f → fase POSITIVA (adelanto,
    // como un diferenciador jω). Magnitud que BAJA → fase negativa.
    constexpr int kBands = 24;
    float fc[kBands];
    erbCenters(fc, kBands);
    // +6 dB/oct en todo el rango: mag ∝ f.
    float mag[kBands];
    float re[kBands], im[kBands];
    for (int b = 0; b < kBands; ++b) mag[b] = fc[b] / fc[0];
    complexGammaBands(mag, fc, kBands, 48000.0f, 0.0f, re, im);
    // En el interior (no en los bordes, donde la pendiente es unilateral),
    // la fase debe ser positiva y cercana a +0.2609·6 ≈ +1.57 (tope 1.2).
    for (int b = 2; b < kBands - 2; ++b) {
        EXPECT_GT(im[b], 0.0f) << "banda " << b << " debería tener fase positiva";
        // |Γ|·sin φ = im; con mag grande la im es grande: verificar el
        // SIGNO y que |φ| no se pase del tope.
        const float phi = std::atan2(im[b], re[b]);
        EXPECT_GT(phi, 0.5f);
        EXPECT_LE(phi, 1.21f);
    }
}

TEST(Ley1Gamma, SharpResonanceGetsMinPhaseSmoothTonalDoesNot) {
    // Un pico AGUDO (resonancia) → fase de Bode concentrada en sus
    // flancos: la fase es ~0 EN el pico (la pendiente es 0 en un máximo),
    // positiva en el flanco de subida y negativa en el de bajada.
    constexpr int kBands = 24;
    float fc[kBands];
    erbCenters(fc, kBands);
    // Pico agudo de +12 dB en la banda 12 (una sola banda alta).
    float mag[kBands];
    for (int b = 0; b < kBands; ++b) mag[b] = 1.0f;
    mag[12] = 3.9810717f;  // +12 dB
    float re[kBands], im[kBands];
    complexGammaBands(mag, fc, kBands, 48000.0f, 0.0f, re, im);
    // Flancos del pico: fase no nula, con signos opuestos.
    EXPECT_GT(im[11], 0.0f);   // flanco de subida: fase positiva
    EXPECT_LT(im[13], 0.0f);   // flanco de bajada: fase negativa
    // En el pico mismo la pendiente es 0 → fase ~0; lejos del pico,
    // la magnitud es plana → fase ~0.
    EXPECT_NEAR(im[12], 0.0f, 1e-6f);
    EXPECT_NEAR(im[0], 0.0f, 1e-6f);
    EXPECT_NEAR(im[kBands - 1], 0.0f, 1e-6f);
    // La magnitud se conserva: |Γ| = mag.
    for (int b = 0; b < kBands; ++b) {
        const float got = std::sqrt(re[b] * re[b] + im[b] * im[b]);
        EXPECT_NEAR(got, mag[b], mag[b] * 1e-5f + 1e-6f);
    }
}

TEST(Ley1Gamma, MinPhaseFromSlopeSign) {
    // Casos unitarios de la relación de Bode.
    EXPECT_NEAR(minPhaseFromSlopeDbPerOct(6.0f), 1.2f, 1e-6f);   // +6 dB/oct → tope
    EXPECT_NEAR(minPhaseFromSlopeDbPerOct(-6.0f), -1.2f, 1e-6f);  // −6 dB/oct → tope
    EXPECT_FLOAT_EQ(minPhaseFromSlopeDbPerOct(0.0f), 0.0f);
    EXPECT_GT(minPhaseFromSlopeDbPerOct(2.0f), 0.0f);
    EXPECT_LT(minPhaseFromSlopeDbPerOct(-2.0f), 0.0f);
}

// ── 2. Geometría → ITD ──────────────────────────────────────────────────────
TEST(Ley1Align, SymmetricGeometryHasZeroItd) {
    ListenerGeometry g;
    g.listenerOffsetM = 0.0f;
    const ItdSplit s = geometryToItd(g);
    EXPECT_FLOAT_EQ(s.itdSeconds, 0.0f);
    EXPECT_EQ(s.intDelayL, 0);
    EXPECT_EQ(s.intDelayR, 0);
    EXPECT_FLOAT_EQ(s.fracNs, 0.0f);
}

TEST(Ley1Align, OffsetRightDelaysRightChannel) {
    // Oyente a la derecha (+0.20 m): el altavoz R está más cerca, su sonido
    // llega ANTES → hay que retrasar R. El ITD debe ser positivo y el
    // retardo entero debe ir a R, no a L.
    ListenerGeometry g;
    g.spanMeters = 0.90f;
    g.distanceMeters = 0.85f;
    g.listenerOffsetM = 0.20f;
    g.speedOfSound = 343.0f;
    g.sampleRate = 48000.0f;
    const ItdSplit s = geometryToItd(g);
    EXPECT_GT(s.itdSeconds, 0.0f);
    EXPECT_EQ(s.intDelayL, 0);
    EXPECT_GT(s.intDelayR, 0);
    // Coherencia: entero + fracción = ITD total (en muestras).
    const float total = (float)(s.intDelayR - s.intDelayL) + s.fracNs * 1.0e-9f * g.sampleRate;
    const float expected = s.itdSeconds * g.sampleRate;
    EXPECT_NEAR(total, expected, 1e-3f);
    // Orden de magnitud real: ~536 µs ≈ 25.7 muestras @48 kHz.
    EXPECT_GT(expected, 20.0f);
    EXPECT_LT(expected, 32.0f);
}

TEST(Ley1Align, NsSplitHonorsFullItd) {
    // 240 µs @48 kHz = 11.52 muestras: entero 12 + fracción −0.48.
    // Antes esto saturaba el Farrow en ±1 muestra.
    const ItdSplit s = nsToItdSplit(240000.0f, 48000.0f);
    EXPECT_EQ(s.intDelayL, 0);
    EXPECT_EQ(s.intDelayR, 12);
    const float total = (float)(s.intDelayR - s.intDelayL) + s.fracNs * 1.0e-9f * 48000.0f;
    EXPECT_NEAR(total, 11.52f, 1e-3f);
    // Negativo: se retrasa L.
    const ItdSplit sn = nsToItdSplit(-240000.0f, 48000.0f);
    EXPECT_EQ(sn.intDelayR, 0);
    EXPECT_EQ(sn.intDelayL, 12);
}

TEST(Ley1Align, ArbitratorHonorsFullItdEndToEnd) {
    // Prueba decisiva: un impulso por el arbitrador con ITD 240 µs debe
    // salir con el canal R ~11.5 muestras DESPUÉS que L (no saturado en ±1).
    SupremeMsoFarrowArbitrator mso;
    mso.prepare(48000.0f);
    mso.setMsoItdNanoseconds(240000.0f);
    mso.setEnabled(true);
    constexpr int kBlock = 256;
    std::vector<float> L(kBlock, 0.0f), R(kBlock, 0.0f);
    // Converger: envolvente a 1.0, ITD suavizado, slew entero a 12.
    for (int i = 0; i < 40; ++i) {
        std::fill(L.begin(), L.end(), 0.0f);
        std::fill(R.begin(), R.end(), 0.0f);
        mso.process(L.data(), R.data(), kBlock, 48000.0f);
    }
    EXPECT_EQ(mso.intDelaySamplesR(), 12);
    EXPECT_EQ(mso.intDelaySamplesL(), 0);
    // Impulso en el centro del bloque, en ambos canales.
    std::fill(L.begin(), L.end(), 0.0f);
    std::fill(R.begin(), R.end(), 0.0f);
    L[128] = 1.0f;
    R[128] = 1.0f;
    mso.process(L.data(), R.data(), kBlock, 48000.0f);
    const int peakL = argmaxAbs(L.data(), kBlock);
    const int peakR = argmaxAbs(R.data(), kBlock);
    // El pico debe ser detectable (no enterrado por la envolvente).
    EXPECT_GT(std::fabs(L[peakL]), 0.2f);
    EXPECT_GT(std::fabs(R[peakR]), 0.2f);
    // Diferencial ≈ 11.52 muestras (12 entero − 0.48 fracción).
    const int diff = peakR - peakL;
    EXPECT_GE(diff, 10);
    EXPECT_LE(diff, 13);
}

// ── 3. Filtro inverso ───────────────────────────────────────────────────────
TEST(Ley1Inverse, FlatMagnitudeIsIdentity) {
    // |H| = 1 → FIR = [1, 0, 0, ...].
    constexpr int kBins = 257;  // N = 512
    std::vector<float> mag(kBins, 1.0f);
    float fir[128];
    const int n = inverseFirFromMagnitude(mag.data(), kBins, fir, 128);
    EXPECT_GT(n, 0);
    EXPECT_NEAR(fir[0], 1.0f, 0.05f);
    for (int i = 1; i < n; ++i) EXPECT_NEAR(fir[i], 0.0f, 0.05f);
}

TEST(Ley1Inverse, ResonanceIsFlattenedAndCompacted) {
    // IR resonante (pico a 200 Hz, decaimiento exp): su magnitud tiene un
    // pico agudo. El inverso debe aplanarla y compactar el pulso.
    constexpr float kSr = 48000.0f;
    constexpr int kN = 512;
    constexpr float kF0 = 200.0f;
    // IR original: resonancia amortiguada.
    std::vector<float> h(kN, 0.0f);
    for (int i = 0; i < kN; ++i) {
        const float t = (float)i / kSr;
        h[i] = std::exp(-(float)i / 40.0f) * std::cos(2.0f * 3.14159265358979323846f * kF0 * t);
    }
    // Magnitud |H| por FFT.
    ivanna::FFTRadix2 fft(kN);
    std::vector<float> re(h.begin(), h.end()), im(kN, 0.0f);
    fft.transform(re.data(), im.data(), +1);
    std::vector<float> mag(kN / 2 + 1);
    for (int k = 0; k <= kN / 2; ++k) {
        mag[k] = std::sqrt(re[k] * re[k] + im[k] * im[k]);
    }
    // Debe haber un pico real en 200 Hz (si no, el test no prueba nada).
    const int kPeak = (int)std::lround(kF0 * kN / kSr);
    EXPECT_GT(mag[kPeak], 2.0f * mag[kN / 4]);
    // Inverso.
    float fir[128];
    const int nTaps = inverseFirFromMagnitude(mag.data(), kN / 2 + 1, fir, 128);
    EXPECT_GT(nTaps, 0);
    // Convolución y = h * fir: debe ser un pulso compacto.
    std::vector<float> y(kN + nTaps, 0.0f);
    for (int i = 0; i < kN; ++i) {
        for (int j = 0; j < nTaps; ++j) y[i + j] += h[i] * fir[j];
    }
    const int pk = argmaxAbs(y.data(), (int)y.size());
    // El pico está cerca del inicio (el inverso es causal y compacto).
    EXPECT_LT(pk, 32);
    // Energía concentrada: ≥60% en los primeros 16 muestras tras el pico.
    float eTot = 0.0f, eHead = 0.0f;
    for (size_t i = 0; i < y.size(); ++i) eTot += y[i] * y[i];
    for (int i = pk; i < std::min(pk + 16, (int)y.size()); ++i) eHead += y[i] * y[i];
    EXPECT_GT(eHead / std::max(eTot, 1e-12f), 0.6f);
    // Y la magnitud resultante es plana (el pico de 200 Hz se aplana).
    std::vector<float> yr(y.begin(), y.begin() + kN), yi(kN, 0.0f);
    fft.transform(yr.data(), yi.data(), +1);
    float maxDb = -1e9f, minDb = 1e9f;
    for (int k = 2; k <= kN / 4; ++k) {  // banda con energía (sin DC)
        const float m = std::sqrt(yr[k] * yr[k] + yi[k] * yi[k]);
        if (m <= 1e-9f) continue;
        const float db = 20.0f * std::log10(m);
        maxDb = std::max(maxDb, db);
        minDb = std::min(minDb, db);
    }
    EXPECT_LT(maxDb - minDb, 8.0f);  // aplanado (el original tenía >20 dB de pico)
}

TEST(Ley1Inverse, FirStageAppliesTaps) {
    InverseFirStage stage;
    const float taps[2] = {1.0f, 0.5f};
    stage.setTaps(taps, 2);
    EXPECT_TRUE(stage.active());
    EXPECT_EQ(stage.numTaps(), 2);
    float l[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    float r[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    stage.process(l, r, 4);
    EXPECT_FLOAT_EQ(l[0], 1.0f);
    EXPECT_FLOAT_EQ(l[1], 0.5f);
    EXPECT_FLOAT_EQ(l[2], 0.0f);
    // Bypass: sin taps, identidad.
    stage.setTaps(nullptr, 0);
    EXPECT_FALSE(stage.active());
    float l2[2] = {0.7f, -0.3f};
    float r2[2] = {0.2f, 0.9f};
    stage.process(l2, r2, 2);
    EXPECT_FLOAT_EQ(l2[0], 0.7f);
    EXPECT_FLOAT_EQ(r2[1], 0.9f);
}
