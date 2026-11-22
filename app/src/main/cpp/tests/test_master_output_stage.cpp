/**
 * test_master_output_stage.cpp
 *
 * Etapa final maestro 64-bit (v2.5.461): True-Peak 4x a -0.2 dBTP + TPDF
 * Dither + mezcla dry/wet en double, tras SafetyLimiter.
 *
 * 4 tests (especificación de Esse, 2026-11-17) + 3 tests Fase C Ley 3
 * (noise shaping, 2026-11-18):
 *   1) TruePeak4xGuard_CapsInterSampleOvershoot — pico inter-muestra +2.0 dBTP
 *      con picos individuales 0.95 -> truePeak4x(out) <= -0.2 dBTP, sin salto
 *      brusco al inicio del bloque.
 *   2) TruePeak4xGuard_BitExactFastPath — señal limpia -6 dBFS -> salida
 *      bit-exacta (memcmp == 0).
 *   3) MixAndDither_BypassBitExact — masterEnv == 0 -> out == dry bit a bit
 *      aun con bitDepth = 16.
 *   4) MixAndDither_Tpdf16BitBreaksQuantization — seno -90 dBFS a 16-bit:
 *      sin armónicos impares de truncación (vs redondeo sin dither) y
 *      desviación del dither dentro de ±3.0 LSB (Fase C: dither 1 + feedback
 *      1.5 + redondeo 0.5; antes era ±1.0 LSB sin shaping).
 *   5) MixAndDither_ShapedOutputOnDownstreamGrid — la salida cae en la
 *      rejilla real del conversor (1/32767 a 16-bit, 1/8388607 a 24-bit):
 *      la conversión posterior es exacta, sin segunda cuantización.
 *   6) MixAndDither_ShapingLowersInBandNoise — seno 100 Hz -60 dBFS, 4 s,
 *      16-bit: el ruido en banda (media móvil 32, ~<750 Hz) con shaping es
 *      < 0.85x del de un cuantizador TPDF-sin-shaping de referencia
 *      (teoría: 0.667x — el dither blanco queda, el error de cuantización
 *      se conforma fuera de banda), sin explotar el ruido total (< 4x).
 *   7) MixAndDither_DeterministicAfterReset — misma entrada, dos pasadas
 *      tras reset() -> salida idéntica bit a bit.
 *
 * Corre en host (x86_64 Linux), sin hardware Android.
 */

#include <gtest/gtest.h>
#include "SafetyLimiter.h"
#include "true_peak.hpp"
#include "master_output_stage.hpp"
#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>

static constexpr int   kSR     = 48000;
static constexpr int   kFrames = 512;
static constexpr float kTarget = 0.97723722f;  // -0.2 dBTP

// Magnitud DFT (Goertzel directo) en la frecuencia dada, normalizada para que
// un seno de amplitud A devuelva ~A. N debe contener ciclos enteros.
static double dftMagAt(const std::vector<float>& x, double freqHz, double sr) {
    const int n = static_cast<int>(x.size());
    double re = 0.0, im = 0.0;
    const double w = 2.0 * M_PI * freqHz / sr;
    for (int i = 0; i < n; ++i) {
        re += static_cast<double>(x[i]) * std::cos(w * i);
        im -= static_cast<double>(x[i]) * std::sin(w * i);
    }
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

static std::vector<float> quantize16(const std::vector<float>& x) {
    std::vector<float> q(x.size());
    for (size_t i = 0; i < x.size(); ++i)
        q[i] = static_cast<float>(std::lrintf(
            std::clamp(x[i], -1.0f, 1.0f) * 32767.0f)) / 32767.0f;
    return q;
}

// ── 1) Guarda True-Peak 4x ──────────────────────────────────────────────────
TEST(MasterOutputStage, TruePeak4xGuard_CapsInterSampleOvershoot) {
    // Seno 12 kHz, amplitud 1.259 (+2.0 dBTP de pico inter-muestra), fase 41°:
    // picos individuales ~0.95, pico inter-muestra ~1.259.
    const double amp = 1.258925;  // 10^(2.0/20)
    const double phase = 41.0 * M_PI / 180.0;
    std::vector<float> L(kFrames), R(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        const double v = amp * std::sin(2.0 * M_PI * 12000.0 * i / kSR + phase);
        L[i] = static_cast<float>(v);
        R[i] = static_cast<float>(v);
    }
    // Precondiciones: la configuración realmente tiene sobrepico inter-muestra.
    float samplePeak = 0.0f;
    for (int i = 0; i < kFrames; ++i)
        samplePeak = std::max(samplePeak, std::fabs(L[i]));
    ASSERT_LE(samplePeak, 0.96f) << "picos individuales deben ser ~0.95";
    ASSERT_GT(ivanna::truePeak4x(L.data(), kFrames), 1.0f)
        << "el pico inter-muestra debe superar 0 dBTP";

    // Cadena real: SafetyLimiter (como en omega_effect.cpp) y luego la guarda.
    ivanna::SafetyLimiter limiter;
    limiter.setParams(0.891251f, 0.98855f);
    limiter.setSampleRate(static_cast<float>(kSR));
    limiter.process(L.data(), R.data(), kFrames);

    ivanna::master::MasterOutputStage stage;
    stage.prepare(static_cast<double>(kSR));
    // Copia para medir la suavidad al inicio del bloque.
    const std::vector<float> inAfterLimiter = L;
    stage.processTruePeakGuard(L.data(), R.data(), kFrames);

    const float tpOut = std::max(ivanna::truePeak4x(L.data(), kFrames),
                                 ivanna::truePeak4x(R.data(), kFrames));
    EXPECT_LE(tpOut, kTarget + 1e-4f)
        << "la guarda debe dejar el pico inter-muestra en -0.2 dBTP o menos";

    // Sin discontinuidad brusca al inicio del bloque: el paso máximo entre
    // muestras consecutivas de salida no debe superar el de la entrada
    // (la ganancia solo reduce; el ramp de ataque es suave).
    float maxStepIn = 0.0f, maxStepOut = 0.0f;
    for (int i = 0; i + 1 < kFrames; ++i) {
        maxStepIn = std::max(maxStepIn, std::fabs(inAfterLimiter[i + 1] - inAfterLimiter[i]));
        maxStepOut = std::max(maxStepOut, std::fabs(L[i + 1] - L[i]));
    }
    EXPECT_LE(maxStepOut, maxStepIn * 1.05f + 1e-6f)
        << "la guarda no debe crear un salto brusco al inicio del bloque";
}

// ── 2) Fast-path bit-exacto ─────────────────────────────────────────────────
TEST(MasterOutputStage, TruePeak4xGuard_BitExactFastPath) {
    std::vector<float> L(kFrames), R(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        const float v = 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 1000.0f * i / kSR);
        L[i] = v;
        R[i] = -v;
    }
    const std::vector<float> inL = L, inR = R;
    ivanna::master::MasterOutputStage stage;
    stage.prepare(static_cast<double>(kSR));
    stage.processTruePeakGuard(L.data(), R.data(), kFrames);
    EXPECT_EQ(std::memcmp(L.data(), inL.data(), sizeof(float) * kFrames), 0)
        << "señal limpia -6 dBFS: cero alteración";
    EXPECT_EQ(std::memcmp(R.data(), inR.data(), sizeof(float) * kFrames), 0)
        << "señal limpia -6 dBFS: cero alteración (R)";
}

// ── 3) Bypass bit-exacto ────────────────────────────────────────────────────
TEST(MasterOutputStage, MixAndDither_BypassBitExact) {
    ivanna::master::MasterOutputStage stage;
    stage.prepare(static_cast<double>(kSR));
    const double dryVals[4] = {0.5, -0.25, 0.125, -0.75};
    for (int i = 0; i < 4; ++i) {
        float outL = 0.0f, outR = 0.0f;
        stage.mixAndDitherSample(dryVals[i], -dryVals[i], 0.9, -0.9,
                                 0.0, 16, outL, outR);
        EXPECT_EQ(outL, static_cast<float>(dryVals[i]))
            << "masterEnv==0: outL debe ser dryL bit a bit";
        EXPECT_EQ(outR, static_cast<float>(-dryVals[i]))
            << "masterEnv==0: outR debe ser dryR bit a bit";
    }
}

// ── 4) TPDF a 16-bit rompe la cuantización ──────────────────────────────────
// NOTA (2026-11-17): la especificación original pedía -72 dBFS, pero a ese
// nivel el redondeo al más cercano (lrintf, el que usa el pipeline real)
// YA produce un 3er armónico ~0 (6.5e-20 medido) — no hay distorsión que
// romper y el test no puede medir mejora. El beneficio del TPDF se mide
// donde la cuantización sí distorsiona: cerca de 1 LSB. A -90 dBFS
// (~1.04 LSB) el redondeo sin dither produce un 3er armónico fuerte
// (señal casi cuadrada) y el TPDF lo baja al piso de ruido.
TEST(MasterOutputStage, MixAndDither_Tpdf16BitBreaksQuantization) {
    // Seno 1 kHz a -90 dBFS (amplitud ~1.04 LSB), 100 ms = 100 ciclos exactos.
    const int n = 4800;
    const double amp = std::pow(10.0, -90.0 / 20.0);
    std::vector<float> dry(n);
    for (int i = 0; i < n; ++i)
        dry[i] = static_cast<float>(amp * std::sin(2.0 * M_PI * 1000.0 * i / kSR));

    // (a) Sin dither: redondeo directo a 16-bit.
    const std::vector<float> qPlain = quantize16(dry);

    // (b) Con TPDF: mixAndDitherSample (env=1, wet=dry) y luego cuantizar.
    ivanna::master::MasterOutputStage stage;
    stage.prepare(static_cast<double>(kSR));
    std::vector<float> dithered(n);
    const double lsb = 1.0 / 32768.0;
    for (int i = 0; i < n; ++i) {
        float outL = 0.0f, outR = 0.0f;
        stage.mixAndDitherSample(static_cast<double>(dry[i]), 0.0,
                                 static_cast<double>(dry[i]), 0.0,
                                 1.0, 16, outL, outR);
        dithered[i] = outL;
        // Desviación dentro de ±3.0 LSB (Fase C, Ley 3): dither TPDF ±1 LSB
        // + feedback del shaper ±1.5 LSB + redondeo a rejilla ±0.5 LSB.
        // El invariante que importa (3er armónico) se verifica abajo.
        EXPECT_LE(std::fabs(static_cast<double>(outL) - static_cast<double>(dry[i])),
                  lsb * 3.01)
            << "la salida con shaping no debe desviarse más de ±3 LSB";
    }
    const std::vector<float> qDithered = quantize16(dithered);

    // El 3er armónico (3 kHz) de truncación debe caer con el dither.
    const double h3Plain = dftMagAt(qPlain, 3000.0, kSR);
    const double h3Dithered = dftMagAt(qDithered, 3000.0, kSR);
    const double fund = dftMagAt(qPlain, 1000.0, kSR);
    ASSERT_GT(fund, 0.0) << "la fundamental debe medirse";
    EXPECT_LT(h3Dithered, h3Plain * 0.5)
        << "el TPDF debe bajar el 3er armónico al menos 6 dB (plain="
        << h3Plain << ", dithered=" << h3Dithered << ", fund=" << fund << ")";
}

// ── 5) Salida en la rejilla real del conversor ──────────────────────────────
// La salida con shaping debe ser múltiplo exacto del paso que usa
// omega_write_output_frame (1/32767 a 16-bit, 1/8388607 a 24-bit): la
// conversión posterior con lrintf() es entonces exacta y no añade un segundo
// error de cuantización sin conformar. Tolerancia 0.01 LSB a 16-bit;
// a 24-bit 0.6 LSB (límite de representación del propio float32).
TEST(MasterOutputStage, MixAndDither_ShapedOutputOnDownstreamGrid) {
    const int n = 4800;
    for (int bitDepth : {16, 24}) {
        const double step = (bitDepth == 16) ? (1.0 / 32767.0) : (1.0 / 8388607.0);
        ivanna::master::MasterOutputStage stage;
        stage.prepare(static_cast<double>(kSR));
        double worst = 0.0;
        for (int i = 0; i < n; ++i) {
            const double x = 0.7 * std::sin(2.0 * M_PI * 997.0 * i / kSR);
            float outL = 0.0f, outR = 0.0f;
            stage.mixAndDitherSample(x, -x, x, -x, 1.0, bitDepth, outL, outR);
            for (double o : {static_cast<double>(outL), static_cast<double>(outR)}) {
                const double k = o / step;
                worst = std::max(worst, std::fabs(k - std::round(k)));
            }
        }
        // Tolerancia por profundidad: a 16-bit la rejilla es exacta en
        // float32 (ulp << paso); a 24-bit el propio float32 no puede
        // representar la rejilla mejor que ~0.5 LSB (ulp ≈ 2^-24 ≈ paso/2),
        // así que ahí se exige 0.6 LSB: el error es de representación float,
        // no del shaper (el redondeo aguas abajo usa el mismo float).
        const double tol = (bitDepth == 16) ? 0.01 : 0.6;
        EXPECT_LT(worst, tol)
            << "bitDepth=" << bitDepth << ": salida fuera de la rejilla 1/"
            << (bitDepth == 16 ? 32767 : 8388607) << " (peor desviación "
            << worst << " LSB)";
    }
}

// ── 6) El shaping baja el ruido en banda ────────────────────────────────────
// Referencia TPDF-sin-shaping en el propio test (mismo xorshift64*, misma
// amplitud de dither, redondeo a la misma rejilla, sin realimentación).
namespace {
struct TestTpdfRef {
    uint64_t sL = 0x123456789ABCDEF0ULL, sR = 0x0FEDCBA987654321ULL;
    static double uni(uint64_t& s) {
        uint64_t x = s;
        x ^= x >> 12; x ^= x << 25; x ^= x >> 27; s = x;
        return static_cast<double>((x * 0x2545F4914F6CDD1DULL) >> 11) *
               (1.0 / 9007199254740992.0);
    }
    double quantize(double y, double step) {
        const double d = (uni(sL) - 0.5 + uni(sL) - 0.5) * (1.0 / 32768.0);
        return std::round(std::clamp(y + d, -1.0, 1.0) / step) * step;
    }
};
// Varianza del error tras media móvil de 32 muestras (~pasa-bajos <750 Hz):
// mide el ruido EN BANDA, no el total. Se descartan las primeras 32 muestras.
double inBandNoiseVar(const std::vector<double>& err) {
    const int kTaps = 32;
    const int n = static_cast<int>(err.size());
    double sum = 0.0;
    for (int i = 0; i < kTaps; ++i) sum += err[i];
    double mean = 0.0, m2 = 0.0;
    int count = 0;
    for (int i = kTaps; i < n; ++i) {
        sum += err[i] - err[i - kTaps];
        const double ma = sum / kTaps;
        ++count;
        const double delta = ma - mean;
        mean += delta / count;
        m2 += delta * (ma - mean);
    }
    return (count > 1) ? m2 / (count - 1) : 0.0;
}
}  // namespace

TEST(MasterOutputStage, MixAndDither_ShapingLowersInBandNoise) {
    // Seno 100 Hz a -60 dBFS, 4 s = 400 ciclos exactos a 48 kHz.
    const int n = 4 * kSR;
    const double amp = 0.001;
    std::vector<double> x(n);
    for (int i = 0; i < n; ++i)
        x[i] = amp * std::sin(2.0 * M_PI * 100.0 * i / kSR);

    // (a) Con shaping (el stage real).
    ivanna::master::MasterOutputStage stage;
    stage.prepare(static_cast<double>(kSR));
    std::vector<double> errShaped(n), outShaped(n);
    for (int i = 0; i < n; ++i) {
        float outL = 0.0f, outR = 0.0f;
        stage.mixAndDitherSample(x[i], 0.0, x[i], 0.0, 1.0, 16, outL, outR);
        outShaped[i] = static_cast<double>(outL);
        errShaped[i] = outShaped[i] - x[i];
    }

    // (b) Referencia TPDF sin shaping.
    TestTpdfRef ref;
    const double step = 1.0 / 32767.0;
    std::vector<double> errRef(n);
    for (int i = 0; i < n; ++i) {
        const double q = ref.quantize(x[i], step);
        errRef[i] = q - x[i];
    }

    const double varShaped = inBandNoiseVar(errShaped);
    const double varRef = inBandNoiseVar(errRef);
    ASSERT_GT(varRef, 0.0) << "la referencia debe tener ruido en banda";
    EXPECT_LT(varShaped, varRef * 0.85)
        << "el shaping debe bajar el ruido en banda al menos 15% (shaped="
        << varShaped << ", ref=" << varRef << ", ratio=" << varShaped / varRef
        << "; teoría ~0.667)";

    // El ruido TOTAL no debe explotar (el shaping lo desplaza, no lo crea).
    double totShaped = 0.0, totRef = 0.0;
    for (int i = 0; i < n; ++i) {
        totShaped += errShaped[i] * errShaped[i];
        totRef += errRef[i] * errRef[i];
    }
    EXPECT_LT(totShaped, totRef * 4.0)
        << "el ruido total con shaping no debe superar 4x la referencia ("
        << totShaped / totRef << "x)";
}

// ── 7) Determinismo tras reset() ────────────────────────────────────────────
TEST(MasterOutputStage, MixAndDither_DeterministicAfterReset) {
    const int n = 512;
    std::vector<double> x(n);
    for (int i = 0; i < n; ++i)
        x[i] = 0.5 * std::sin(2.0 * M_PI * 440.0 * i / kSR);

    auto run = [&](ivanna::master::MasterOutputStage& st) {
        std::vector<float> out(n);
        for (int i = 0; i < n; ++i) {
            float outL = 0.0f, outR = 0.0f;
            st.mixAndDitherSample(x[i], 0.0, x[i], 0.0, 1.0, 16, outL, outR);
            out[i] = outL;
        }
        return out;
    };

    ivanna::master::MasterOutputStage a, b;
    a.prepare(static_cast<double>(kSR));
    b.prepare(static_cast<double>(kSR));
    const std::vector<float> first = run(a);
    a.reset();
    const std::vector<float> afterReset = run(a);
    const std::vector<float> other = run(b);
    EXPECT_EQ(std::memcmp(first.data(), afterReset.data(), sizeof(float) * n), 0)
        << "tras reset(), la misma entrada debe dar la misma salida";
    EXPECT_EQ(std::memcmp(first.data(), other.data(), sizeof(float) * n), 0)
        << "dos instancias nuevas deben coincidir bit a bit";
}
