// test_polyphase_sinc_downsampler.cpp — Fase 4: downsampler sinc de Kaiser.
// Verifica con MEDICIÓN (no con el diseño): banda de paso plana, rechazo de
// alias >130 dB, ganancia DC = 1 y continuidad entre bloques.
// Compila en host sin NDK ni root.
#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "include/polyphase_sinc_downsampler.hpp"

using ivanna::PolyphaseSincDownsampler;

namespace {

// Amplitud (lineal, pico) de la componente a `freqHz` en x[0..n) por Goertzel
// (DFT de un solo bin, acumulación float64). Robusto para medir aliases
// de -130 dB: con n grande el suelo de ruido del estimador es << -140 dB.
double goertzelAmp(const float* x, int n, double sampleRate, double freqHz) {
    const double w = 2.0 * M_PI * freqHz / sampleRate;
    const double cw = std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = 0; i < n; ++i) {
        const double s0 = static_cast<double>(x[i]) + 2.0 * cw * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return 2.0 * std::sqrt(re * re + im * im) / n;
}

std::vector<float> sine(int n, double sampleRate, double freqHz, double amp) {
    std::vector<float> x(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i)
        x[static_cast<size_t>(i)] = static_cast<float>(amp * std::sin(2.0 * M_PI * freqHz * i / sampleRate));
    return x;
}

// Downsamplea un stream completo en bloques (como el reproductor) y devuelve
// la salida concatenada (con flush al final).
std::vector<float> downsampleBlocks(PolyphaseSincDownsampler& ds,
                                    const std::vector<float>& in, int blockFrames) {
    std::vector<float> out;
    std::vector<float> tmp(static_cast<size_t>(blockFrames + 16));
    for (size_t off = 0; off < in.size(); off += static_cast<size_t>(blockFrames)) {
        const int n = static_cast<int>(std::min<size_t>(static_cast<size_t>(blockFrames), in.size() - off));
        const int w = ds.process(in.data() + off, in.data() + off, n, tmp.data(), tmp.data());
        out.insert(out.end(), tmp.begin(), tmp.begin() + w);
    }
    std::vector<float> tail(static_cast<size_t>(1024));
    const int w = ds.flush(tail.data(), tail.data());
    out.insert(out.end(), tail.begin(), tail.begin() + w);
    return out;
}

}  // namespace

TEST(PolyphaseSincDownsampler, ConfigureRejectsInvalidRatios) {
    PolyphaseSincDownsampler ds;
    EXPECT_FALSE(ds.configure(48000, 48000));   // ratio 1: no es downsampling
    EXPECT_FALSE(ds.configure(44100, 48000));   // subida de tasa
    EXPECT_FALSE(ds.configure(96000, 44100));   // ratio no entero (2.176...)
    EXPECT_FALSE(ds.configure(384000, 8000));   // ratio 48 > kMaxRatio
    EXPECT_FALSE(ds.configure(0, 48000));
    EXPECT_FALSE(ds.configure(192000, 0));
    EXPECT_TRUE(ds.configure(192000, 48000));   // 4:1
    EXPECT_EQ(ds.ratio(), 4);
    EXPECT_TRUE(ds.configure(96000, 48000));    // 2:1
    EXPECT_EQ(ds.ratio(), 2);
    EXPECT_TRUE(ds.configure(88200, 44100));    // 2:1
    EXPECT_TRUE(ds.configure(176400, 44100));   // 4:1
}

TEST(PolyphaseSincDownsampler, PassbandIsFlatTo20kHz) {
    PolyphaseSincDownsampler ds;
    ASSERT_TRUE(ds.configure(192000, 48000));
    for (double f : {1000.0, 10000.0, 20000.0}) {
        const auto in = sine(192000, 192000.0, f, 0.5);  // 1 s
        const auto out = downsampleBlocks(ds, in, 4096);
        ds.reset();
        // Mide en régimen permanente (descarta latencia + transitorio).
        const int skip = ds.latencyFrames() / 4 + 4800;
        const double a = goertzelAmp(out.data() + skip, 24000, 48000.0, f);
        const double db = 20.0 * std::log10(std::max(a / 0.5, 1e-12));
        EXPECT_NEAR(db, 0.0, 0.5) << "banda de paso a " << f << " Hz debe ser plana (medido " << db << " dB)";
    }
}

TEST(PolyphaseSincDownsampler, AliasRejectionExceeds130dB) {
    // Tono a 30 kHz (amplitud 0.9) a 192 kHz: al bajar a 48 kHz aliasa a
    // 18 kHz (48000-30000). El filtro debe dejarlo por debajo de -130 dB.
    PolyphaseSincDownsampler ds;
    ASSERT_TRUE(ds.configure(192000, 48000));
    const auto in = sine(192000, 192000.0, 30000.0, 0.9);  // 1 s
    const auto out = downsampleBlocks(ds, in, 4096);
    const int skip = ds.latencyFrames() / 4 + 4800;
    const double alias = goertzelAmp(out.data() + skip, 24000, 48000.0, 18000.0);
    const double aliasDb = 20.0 * std::log10(std::max(alias / 0.9, 1e-16));
    EXPECT_LT(aliasDb, -130.0) << "alias de 30 kHz a 18 kHz debe estar bajo -130 dB (medido " << aliasDb << " dB)";

    // Segundo alias: 90 kHz -> 6 kHz (96000-90000 = 6000).
    ds.reset();
    const auto in2 = sine(192000, 192000.0, 90000.0, 0.9);
    const auto out2 = downsampleBlocks(ds, in2, 4096);
    const double alias2 = goertzelAmp(out2.data() + skip, 24000, 48000.0, 6000.0);
    const double alias2Db = 20.0 * std::log10(std::max(alias2 / 0.9, 1e-16));
    EXPECT_LT(alias2Db, -130.0) << "alias de 90 kHz a 6 kHz debe estar bajo -130 dB (medido " << alias2Db << " dB)";
}

TEST(PolyphaseSincDownsampler, DcGainIsOneAndBlocksAreContinuous) {
    PolyphaseSincDownsampler ds;
    ASSERT_TRUE(ds.configure(192000, 48000));
    // DC 0.5: tras el transitorio, la salida debe ser 0.5 (ganancia DC = 1).
    std::vector<float> dc(192000, 0.5f);
    const auto out = downsampleBlocks(ds, dc, 4096);
    double mean = 0.0;
    for (size_t i = 24000; i < out.size(); ++i) mean += out[i];
    mean /= static_cast<double>(out.size() - 24000);
    EXPECT_NEAR(mean, 0.5, 0.001) << "ganancia DC debe ser 1";

    // Continuidad: procesar en bloques de 100 frames da lo mismo que en un
    // solo bloque (la fase de diezmado y el historial no se reinician).
    ds.reset();
    const auto in = sine(96000, 192000.0, 5000.0, 0.7);
    const auto outA = downsampleBlocks(ds, in, 100);
    ds.reset();
    const auto outB = downsampleBlocks(ds, in, 96000);
    ASSERT_EQ(outA.size(), outB.size());
    double maxDiff = 0.0;
    for (size_t i = 0; i < outA.size(); ++i)
        maxDiff = std::max(maxDiff, std::fabs(static_cast<double>(outA[i]) - outB[i]));
    EXPECT_LT(maxDiff, 1e-5) << "procesar por bloques debe dar lo mismo que de una vez";
}
