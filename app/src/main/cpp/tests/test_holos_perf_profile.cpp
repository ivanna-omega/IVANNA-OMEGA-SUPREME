// test_holos_perf_profile.cpp — perfil de coste de HOLOS por componente.
//
// Propósito (F2 / v2.5.453): HOLOS mide 6–8.5 ms de media en vivo (Moto G85,
// 48 kHz, bloque 320 f) frente a un presupuesto de 6.67 ms, pero el conteo
// de flops sugiere ~1 ms. Este perfil corre HolosOperator::process() en
// host con bloques de 320 frames (el tamaño del bloque DSP vivo) y mide
// media/p99 por bloque con distintos parámetros, para aislar qué componente
// domina el coste (RIR/difuso, Volterra K, bandas, cola RT60). NO es un
// test de pass/fail de rendimiento: imprime la tabla y verifica solo que
// la salida es finita y no es silencio. Los tiempos en host (x86) no son
// los del teléfono (ARM little core), pero el coste RELATIVO por componente
// sí orienta dónde optimizar sin cambiar el sonido.
//
// HALLAZGO (perfil fino, 2026-11-15): el cuello real es el RIR
// (RirConvolver, ~610 µs/frame, ~85% de HOLOS); estimador+ganancias
// ~73 µs, render ~25 µs, STFT ~9 µs, IFFT+OLA+Volterra ~20 µs. La
// optimización debe atacar el RIR, no los trascendentales.
//
// TRAMPA DEL PERFIL: la entrada DEBE ser coherente entre L y R (misma
// señal, no ruido independiente por canal). Con ruido incorrelado, la
// puerta de coherencia (meanCoherence < 0.3) cierra HOLOS y process()
// hace passthrough — el coste medido sería ~0 y la comparación entre
// configs (p. ej. 12 vs 24 bandas) saldría falseada.
//
// Regla: la optimización que salga de aquí debe ser de implementación
// (mismo sonido), nunca de algoritmo sin verificar identidad.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>
#include <vector>

#include "../spatial/HolosOperator.hpp"

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

using namespace ivanna;

namespace {

constexpr int kBlock = 320;      // tamaño del bloque DSP vivo
constexpr int kBlocks = 300;     // ~2 s de audio por config
constexpr int kWarmup = 50;      // bloques de calentamiento (caché + RIR)

struct ProfileResult {
    const char* name;
    double meanUs;
    double p99Us;
};

std::vector<float> makeInput(int frames, uint32_t seed) {
    std::vector<float> v(frames);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> d(-0.7f, 0.7f);
    for (int i = 0; i < frames; ++i) {
        // Ruido + tono 440 Hz: excita todas las bandas y la RIR.
        const float t = (float)std::sin(2.0 * 3.141592653589793 * 440.0 * i / 48000.0);
        v[i] = 0.5f * d(rng) + 0.3f * t;
    }
    return v;
}

ProfileResult profileConfig(const char* name, const HolosParams& p,
                            const std::vector<float>& inL,
                            const std::vector<float>& inR) {
    HolosOperator op;
    op.prepare(48000.0f);
    op.setParams(p);
    std::vector<float> l = inL, r = inR;

    // Calentamiento: estabiliza caché y prima la RIR.
    for (int b = 0; b < kWarmup; ++b) {
        const int off = (b * kBlock) % ((int)l.size() - kBlock);
        op.process(l.data() + off, r.data() + off, kBlock);
    }

    std::vector<double> timesUs;
    timesUs.reserve(kBlocks);
    for (int b = 0; b < kBlocks; ++b) {
        const int off = (b * kBlock) % ((int)l.size() - kBlock);
        const auto t0 = std::chrono::steady_clock::now();
        op.process(l.data() + off, r.data() + off, kBlock);
        const auto t1 = std::chrono::steady_clock::now();
        timesUs.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    std::sort(timesUs.begin(), timesUs.end());
    double sum = 0.0;
    for (double t : timesUs) sum += t;
    const double mean = sum / (double)timesUs.size();
    const double p99 = timesUs[(size_t)(0.99 * (timesUs.size() - 1))];

    // Sanidad: la salida debe ser finita.
    for (float x : l) EXPECT_TRUE(std::isfinite(x));
    for (float x : r) EXPECT_TRUE(std::isfinite(x));

    std::printf("[HOLOS-PROFILE] %-12s mean=%8.1f us  p99=%8.1f us  (bloque %d f @48k, presupuesto 6670 us)\n",
                name, mean, p99, kBlock);
    std::fflush(stdout);
    return {name, mean, p99};
}

HolosParams liveParams() {
    HolosParams p;
    p.amount = 1.0f;       // vivo: HOLOS al 100% (amount 0.5 en app; 1.0 aquí = peor caso)
    p.diffuse = 0.5f;
    p.harmonics = 0.32f;
    p.roomRt60 = 0.34f;
    p.bandCount = 24;
    return p;
}

}  // namespace

TEST(HolosPerfProfile, CostByComponent) {
    const int totalFrames = kBlock * (kWarmup + kBlocks + 2);
    // Entrada COHERENTE entre L y R (misma señal base; R atenuada 0.9
    // para dar ILD sin romper la coherencia). Con ruido independiente
    // por canal la puerta de coherencia cierra HOLOS y el perfil mide
    // passthrough (~0 µs), falseando la comparación.
    const std::vector<float> base = makeInput(totalFrames, ivanna::test::IvannaTestSeed32(1));
    std::vector<float> inL = base;
    std::vector<float> inR(totalFrames);
    for (int i = 0; i < totalFrames; ++i) inR[i] = 0.9f * base[i];

    std::printf("[HOLOS-PROFILE] bloque=%d frames, %d bloques medidos, warmup=%d\n",
                kBlock, kBlocks, kWarmup);

    // 1. Config viva completa (peor caso, amount 1.0).
    const auto full = profileConfig("full", liveParams(), inL, inR);

    // 2. Sin difuso: aísla el coste de la cola difusa / RIR compartida.
    {
        HolosParams p = liveParams();
        p.diffuse = 0.0f;
        profileConfig("no_diffuse", p, inL, inR);
    }
    // 3. Sin armónicos: aísla el coste del kernel Volterra K.
    {
        HolosParams p = liveParams();
        p.harmonics = 0.0f;
        profileConfig("no_harmonics", p, inL, inR);
    }
    // 4. 12 bandas: coste del tier 1 (Warm) de SAFMUS.
    {
        HolosParams p = liveParams();
        p.bandCount = 12;
        profileConfig("bands12", p, inL, inR);
    }
    // 5. RT60 corto: aísla el coste de la longitud de la cola RIR.
    {
        HolosParams p = liveParams();
        p.roomRt60 = 0.18f;
        profileConfig("rt60_short", p, inL, inR);
    }
    // 6. Bypass: línea base (debe ser ~0).
    {
        HolosParams p = liveParams();
        p.amount = 0.0f;
        profileConfig("bypass", p, inL, inR);
    }

    std::printf("[HOLOS-PROFILE] full mean=%.1f us p99=%.1f us\n", full.meanUs, full.p99Us);
    std::fflush(stdout);
}
