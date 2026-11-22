// npe_live_vs_native.cpp — Medición 0024: NPE a parámetros NATIVOS
// (0.2/0.2/0.3/0.0) vs VIVOS (0.60/0.50/0.40/+2.5 dB).
// Compilar: g++-14 -O2 -std=c++17 -I app/src/main/cpp -I app/src/main/cpp/include
//   tools/measure/npe_live_vs_native.cpp app/src/main/cpp/phase_oracle.cpp -o /tmp/npe_measure
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>
#include "neuromorphic/NpeStereoCore.hpp"
#include "dsp/ThdMeter.hpp"

using ivanna::neuromorphic::NpeStereoCore;
using ivanna::measure::measureThdPlusNoise;
using ivanna::measure::fillTestTone;

static double bandRms(NpeStereoCore& c, double freqHz, float amp) {
    std::vector<float> L(4800), R(4800);
    fillTestTone(L.data(), L.size(), 48000.0, freqHz, amp);
    R = L;
    c.process(L.data(), R.data(), 4800);
    double s = 0.0;
    for (size_t i = 2400; i < L.size(); ++i) s += double(L[i]) * double(L[i]);
    return std::sqrt(s / double(L.size() - 2400));
}
static void measure(const char* name, bool enabled, float harm, float lat, float ohc, float masterDb) {
    NpeStereoCore c; c.prepare(48000.f);
    std::vector<float> L(48000), R(48000);
    fillTestTone(L.data(), L.size(), 48000.0, 1000.0, 0.5f);
    R = L;
    c.setParams(enabled, harm, lat, ohc, masterDb);
    for (size_t k = 0; k + 1024 <= L.size(); k += 1024)
        c.process(L.data() + k, R.data() + k, 1024);
    const auto r = measureThdPlusNoise(L.data() + 24000, 24000, 48000.0, 1000.0);
    std::printf("%-12s THD+N=%7.2f dB  fund=%7.2f dBFS  peak=%7.2f dBFS  clip=%u\n",
                name, r.thdPlusNoiseDb, r.fundamentalDbfs, r.peakDbfs, r.clippedSamples);
    const double freqs[3] = {110.0, 1000.0, 8000.0};
    for (double f : freqs) {
        NpeStereoCore co; co.prepare(48000.f);
        NpeStereoCore cf; cf.prepare(48000.f);
        co.setParams(true, harm, lat, ohc, masterDb);
        cf.setParams(false, harm, lat, ohc, masterDb);
        const double on = bandRms(co, f, 0.5f);
        const double off = bandRms(cf, f, 0.5f);
        const double db = 20.0 * std::log10((on + 1e-9) / (off + 1e-9));
        std::printf("  %6.0f Hz: %+6.2f dB\n", f, db);
    }
    // Coste.
    std::vector<float> B(1024), BR(1024);
    for (size_t i = 0; i < B.size(); ++i) {
        B[i] = 0.3f * std::sin(2.f * 3.14159265f * 440.f * float(i) / 48000.f);
        BR[i] = B[i];
    }
    for (int i = 0; i < 20; ++i) c.process(B.data(), BR.data(), 1024);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 200; ++i) c.process(B.data(), BR.data(), 1024);
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 200.0;
    std::printf("  coste: %.1f us/bloque-1024\n", us);
}
int main() {
    std::printf("NPE: bypass vs nativo vs vivo (seno 1 kHz, 0.5, 48 kHz)\n");
    measure("BYPASS", false, 0.0f, 0.0f, 0.0f, 0.0f);
    measure("NATIVO", true, 0.2f, 0.2f, 0.3f, 0.0f);
    measure("VIVO", true, 0.60f, 0.50f, 0.40f, 2.5f);
    measure("VIVO-0.3", true, 0.30f, 0.30f, 0.30f, 0.0f);
    return 0;
}
