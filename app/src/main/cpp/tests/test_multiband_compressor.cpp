// test_multiband_compressor.cpp — Regresión 0027 (elevación omnipotente del DSP).
//
// El compresor de Ruta A (g_comp) es MONOBANDA: un solo detector y un
// solo threshold/ratio para todo el espectro. El MultibandCompressor
// (3 bandas, crossovers Linkwitz-Riley 4º orden) lo sustituye cuando
// está activo, con la misma interfaz de runtime (setRuntimeAmount).
//
// Exigencias del revisor del consejo (ronda 1, 0027):
//   - módulo multibanda real con 3 bandas y crossovers LR 4º orden;
//   - sustitución en Ruta A cuando esté activo, manteniendo setRuntimeAmount;
//   - monobanda conservado solo como fallback real;
//   - tests host: reconstrucción, independencia por banda, bypass
//     bit-exacto, estabilidad y rendimiento.
//
// Notas de medida:
// - La suma LR4 es plana en MAGNITUD (all-pass): la reconstrucción se
//   mide en magnitud por frecuencia (DFT), no muestra a muestra (la fase
//   del all-pass desplaza la señal sin cambiar su nivel).
// - El Compressor tiene makeup acoplado a la reducción real (hasta
//   ~4.5 dB con threshold −12 / ratio 4): una señal apenas por encima
//   del threshold se compensa casi entera. Para medir compresión neta
//   hace falta una señal MUY por encima del threshold (0.9 ≈ −0.9 dBFS
//   frente a threshold −12 dB: reducción neta ~3.75 dB tras makeup).
// - El fundido de activación (20 ms) es un crossfade suave: no hay clic
//   (pendiente acotada), aunque en la frecuencia de cruce haya una caída
//   momentánea de magnitud (el all-pass está a 180° ahí). El test de
//   clic mide DISCONTINUIDAD (pendiente), no caída de nivel.
#include <gtest/gtest.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>
#include "dsp/MultibandCompressor.hpp"

using ivanna::MultibandCompressor;

namespace {

constexpr float kSr = 48000.0f;
constexpr int kBlock = 1024;

// Magnitud DFT (un solo bin) de `buf` a `freq` Hz, normalizada para que
// un seno de amplitud A devuelva ~A. Ventana = todo el buffer (señal
// estacionaria; se mide solo la segunda mitad del tono).
float magAt(float freq, const std::vector<float>& buf, float sr) {
    const int n = static_cast<int>(buf.size());
    double re = 0.0, im = 0.0;
    const double w = 2.0 * M_PI * static_cast<double>(freq) / static_cast<double>(sr);
    for (int i = 0; i < n; ++i) {
        const double ph = w * static_cast<double>(i);
        re += static_cast<double>(buf[i]) * std::cos(ph);
        im -= static_cast<double>(buf[i]) * std::sin(ph);
    }
    return static_cast<float>(2.0 * std::sqrt(re * re + im * im) / static_cast<double>(n));
}

void fillSine(std::vector<float>& buf, float freq, float amp, float sr) {
    for (size_t i = 0; i < buf.size(); ++i) {
        buf[i] = amp * std::sin(2.0f * static_cast<float>(M_PI) * freq *
                                static_cast<float>(i) / sr);
    }
}

MultibandCompressor makeMb() {
    MultibandCompressor mb;
    ivanna::DSPParams p;
    p.sampleRate = 48000;
    mb.setParams(p);
    mb.setThreshold(-12.0f);
    mb.setRatio(4.0f);
    mb.setRuntimeAmount(0.0f);
    mb.setEnabled(true);
    return mb;
}

// Procesa `in` en bloques de kBlock y devuelve la salida (mismo tamaño).
// `in` es mono; se duplica a L/R.
std::vector<float> processMono(MultibandCompressor& mb, const std::vector<float>& in) {
    std::vector<float> out(in.size());
    std::vector<float> l(kBlock), r(kBlock);
    size_t pos = 0;
    while (pos < in.size()) {
        const int n = static_cast<int>(std::min<size_t>(kBlock, in.size() - pos));
        for (int i = 0; i < n; ++i) { l[i] = in[pos + i]; r[i] = in[pos + i]; }
        mb.process(l.data(), r.data(), n);
        for (int i = 0; i < n; ++i) out[pos + i] = l[i];
        pos += static_cast<size_t>(n);
    }
    return out;
}

}  // namespace

TEST(MultibandCompressor, FlatSumReconstruction) {
    // Sin compresión (señal por debajo del threshold), la suma de las
    // 3 bandas LR4 es plana en magnitud: ±0.5 dB en todo el espectro.
    const float freqs[] = {50.f, 100.f, 200.f, 500.f, 1000.f, 2000.f, 4000.f, 8000.f, 12000.f};
    for (float f : freqs) {
        auto mb = makeMb();
        std::vector<float> in(48000);
        fillSine(in, f, 0.1f, kSr);  // −20 dBFS, por debajo del threshold
        auto out = processMono(mb, in);
        // Régimen estacionario: segunda mitad del tono.
        std::vector<float> tail(out.begin() + 24000, out.end());
        const float mag = magAt(f, tail, kSr);
        const float db = 20.0f * std::log10(mag / 0.1f);
        EXPECT_NEAR(db, 0.0f, 0.5f) << "frecuencia " << f << " Hz, mag=" << mag;
    }
}

TEST(MultibandCompressor, BandIndependence) {
    // Un grave FUERTE comprime su banda sin tocar el agudo, y el agudo
    // no toca el grave. Grave 100 Hz a 0.9 (−0.9 dBFS); agudo 8 kHz a
    // 0.1 (−20 dBFS).
    //
    // Threshold −24 dB (no −12): el Compressor tiene makeup acoplado a
    // la reducción real (techo ~4.5 dB con threshold −12 / ratio 4) y
    // un HPF de sidechain a 120 Hz que atenúa el tono de 100 Hz (~0.57x).
    // Con threshold −12, un tono a 0.9 queda ~6 dB por encima y el
    // makeup lo cancela casi entero (reducción neta ~0 dB). Con
    // threshold −24 el tono queda ~18 dB por encima: reducción −13.65 dB,
    // makeup +9 dB (techo), neta −4.65 dB — medible. El agudo a 0.1
    // queda 4 dB por encima de −24: reducción −3 dB, makeup +3 dB,
    // neta 0 dB (su nivel no cambia; solo se mide su nivel, no su
    // dinámica). La segunda mitad del tono se mide en régimen (el
    // threshold suavizado ya convergió a −24).
    auto mbLow = makeMb();
    mbLow.setThreshold(-24.0f);
    std::vector<float> lowOnly(48000);
    fillSine(lowOnly, 100.f, 0.9f, kSr);
    auto outLow = processMono(mbLow, lowOnly);
    std::vector<float> tailLow(outLow.begin() + 24000, outLow.end());
    const float magLowAlone = magAt(100.f, tailLow, kSr);
    // El grave SÍ se comprime (reducción neta >1 dB tras el makeup).
    EXPECT_LT(magLowAlone, 0.9f * 0.891f) << "el grave no se comprimió";
    EXPECT_GT(magLowAlone, 0.3f) << "el grave se aplastó demasiado";

    auto mbHigh = makeMb();
    mbHigh.setThreshold(-24.0f);
    std::vector<float> highOnly(48000);
    fillSine(highOnly, 8000.f, 0.1f, kSr);
    auto outHigh = processMono(mbHigh, highOnly);
    std::vector<float> tailHigh(outHigh.begin() + 24000, outHigh.end());
    const float magHighAlone = magAt(8000.f, tailHigh, kSr);

    auto mbMix = makeMb();
    mbMix.setThreshold(-24.0f);
    std::vector<float> mix(48000);
    for (size_t i = 0; i < mix.size(); ++i) {
        mix[i] = 0.9f * std::sin(2.0f * static_cast<float>(M_PI) * 100.f *
                                 static_cast<float>(i) / kSr) +
                 0.1f * std::sin(2.0f * static_cast<float>(M_PI) * 8000.f *
                                 static_cast<float>(i) / kSr);
    }
    auto outMix = processMono(mbMix, mix);
    std::vector<float> tailMix(outMix.begin() + 24000, outMix.end());
    const float magHighMix = magAt(8000.f, tailMix, kSr);
    const float magLowMix = magAt(100.f, tailMix, kSr);
    // El agudo cambia <1 dB aunque el grave esté comprimiendo a tope.
    const float dbHigh = 20.0f * std::log10(magHighMix / magHighAlone);
    EXPECT_NEAR(dbHigh, 0.0f, 1.0f) << "el grave contaminó el agudo";
    // El grave se comprime igual con el agudo presente.
    EXPECT_NEAR(magLowMix, magLowAlone, magLowAlone * 0.05f)
        << "el agudo contaminó el grave";
}

TEST(MultibandCompressor, BypassBitExact) {
    // Desactivado = el buffer no se toca (bit-exacto). El fundido tarda
    // ~14 bloques en llegar a 0 exacto; se procesan 20 y se comprueba
    // el bloque siguiente.
    MultibandCompressor mb = makeMb();
    mb.setEnabled(false);
    std::vector<float> l(kBlock), r(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        l[i] = 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 440.f *
                               static_cast<float>(i) / kSr);
        r[i] = l[i];
    }
    for (int b = 0; b < 20; ++b) mb.process(l.data(), r.data(), kBlock);
    std::vector<float> lIn = l, rIn = r;
    mb.process(l.data(), r.data(), kBlock);
    EXPECT_EQ(0, std::memcmp(l.data(), lIn.data(), kBlock * sizeof(float)));
    EXPECT_EQ(0, std::memcmp(r.data(), rIn.data(), kBlock * sizeof(float)));
}

TEST(MultibandCompressor, FiniteAndBoundedHotInput) {
    auto mb = makeMb();
    std::vector<float> l(kBlock), r(kBlock);
    for (int b = 0; b < 10; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            // Entrada caliente: ±1.5 alternando + ruido.
            const float s = (i % 2 == 0) ? 1.5f : -1.5f;
            l[i] = s + 0.1f * std::sin(static_cast<float>(i) * 0.7f);
            r[i] = -s;
        }
        mb.process(l.data(), r.data(), kBlock);
        for (int i = 0; i < kBlock; ++i) {
            ASSERT_TRUE(std::isfinite(l[i])) << "NaN/Inf en L, bloque " << b;
            ASSERT_TRUE(std::isfinite(r[i])) << "NaN/Inf en R, bloque " << b;
            EXPECT_LT(std::fabs(l[i]), 4.0f) << "L fuera de rango";
            EXPECT_LT(std::fabs(r[i]), 4.0f) << "R fuera de rango";
        }
    }
}

TEST(MultibandCompressor, ClickFreeOnEnable) {
    // Sin clic al activar: la pendiente máxima muestra a muestra
    // durante la transición no supera 2x la pendiente del régimen.
    MultibandCompressor mb = makeMb();
    mb.setEnabled(false);
    std::vector<float> l(kBlock), r(kBlock);
    auto fill = [&](std::vector<float>& v, int blockIdx) {
        for (int i = 0; i < kBlock; ++i) {
            const int n = blockIdx * kBlock + i;
            v[i] = 0.5f * std::sin(2.0f * static_cast<float>(M_PI) * 1000.f *
                                   static_cast<float>(n) / kSr);
        }
    };
    // Asentar el bypass (mix = 0 exacto).
    for (int b = 0; b < 20; ++b) { fill(l, b); fill(r, b); mb.process(l.data(), r.data(), kBlock); }
    // Pendiente del régimen (bypass = la señal misma).
    float baseSlope = 0.0f;
    {
        fill(l, 20); fill(r, 20);
        mb.process(l.data(), r.data(), kBlock);
        for (int i = 1; i < kBlock; ++i)
            baseSlope = std::max(baseSlope, std::fabs(l[i] - l[i - 1]));
    }
    ASSERT_GT(baseSlope, 0.0f);
    // Activar y medir la pendiente durante la transición (5 bloques ≈ 107 ms).
    mb.setEnabled(true);
    float transSlope = 0.0f;
    float prev = l[kBlock - 1];
    for (int b = 21; b < 26; ++b) {
        fill(l, b); fill(r, b);
        mb.process(l.data(), r.data(), kBlock);
        transSlope = std::max(transSlope, std::fabs(l[0] - prev));
        for (int i = 1; i < kBlock; ++i)
            transSlope = std::max(transSlope, std::fabs(l[i] - l[i - 1]));
        prev = l[kBlock - 1];
    }
    EXPECT_LE(transSlope, 2.0f * baseSlope)
        << "discontinuidad al activar (clic): base=" << baseSlope
        << " transición=" << transSlope;
}

TEST(MultibandCompressor, CostWithinBudget) {
    auto mb = makeMb();
    std::vector<float> l(kBlock), r(kBlock);
    for (int i = 0; i < kBlock; ++i) {
        l[i] = 0.3f * std::sin(static_cast<float>(i) * 0.05f);
        r[i] = l[i];
    }
    // Calentamiento.
    for (int b = 0; b < 5; ++b) mb.process(l.data(), r.data(), kBlock);
    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kIters = 100;
    for (int b = 0; b < kIters; ++b) mb.process(l.data(), r.data(), kBlock);
    const auto t1 = std::chrono::steady_clock::now();
    const double usPerBlock =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() /
        1000.0 / static_cast<double>(kIters);
    // Bloque de 1024 muestras = 21.33 ms de audio: 2 ms es <10% del tiempo real.
    EXPECT_LT(usPerBlock, 2000.0) << "us/bloque=" << usPerBlock;
}
