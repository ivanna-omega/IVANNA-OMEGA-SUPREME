// test_route_a_asrc_bypass.cpp
// ============================================================================
// 0033 — Bypass nativo del ASRC (decisión de Esse 2026-11-16)
// ----------------------------------------------------------------------------
// El ASRC no va en el Snapdragon: se bypassa y el reloj lo maneja el
// AudioTrack nativo. El código NO se borra (regla); se bypassa con el
// flag RouteAAsrc::setEnabled (conmutable en runtime).
//
// Telemetría 459 (vc 750) que respalda la decisión: PLL saturado en el
// riel −500 ppm (demanda −635 ppm, más allá del riel), buffer
// BridgePlayer sobrelleno (pll_occ 96694 > cap 92256), coste DSP 16–21
// ms/bloque con picos de hasta 109 ms. El ASRC (FIR 319 taps + doble
// precisión) es un coste grande y su PLL no corrige en este chip.
//
// Tests:
//  1. EnabledByDefault — una instancia nueva tiene enabled()==true
//     (comportamiento existente preservado; el bypass lo impone la capa
//     Kotlin con asrcEnabled=false).
//  2. BypassIsBitExact — con setEnabled(false), process() devuelve la
//     entrada SIN TOCARLA (memcmp exacto) y produced == inFrames.
//  3. BypassRespectsOutCap — con outCap < inFrames, solo se copian
//     outCap frames (nunca se escribe fuera del buffer).
//  4. ToggleReactivates — tras re-habilitar, el ASRC vuelve a remuestrear
//     (la salida ya NO es bit-idéntica a la entrada: el FIR 319 taps la
//     transforma; y es finita y acotada).
// ============================================================================
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "../ivanna_route_a_asrc.h"

namespace {

using ivanna::routea::RouteAAsrc;

constexpr int kSr = 48000;
constexpr double kPi = 3.14159265358979323846;

// Señal de prueba con contenido variado: seno + ruido determinista +
// offset DC + extremos (para que un passthrough roto se note).
std::vector<float> MixedStereo(int frames) {
    std::vector<float> in((size_t)frames * 2);
    uint32_t seed = 0x12345678u;
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kSr;
        const double sine = 0.5 * std::sin(2.0 * kPi * 1000.0 * t);
        seed = seed * 1664525u + 1013904223u;
        const double noise = ((seed >> 16) & 0x7FFF) / 32767.0 - 0.5;
        in[(size_t)i * 2] = static_cast<float>(sine + 0.1 * noise + 0.05);
        in[(size_t)i * 2 + 1] = static_cast<float>(-sine + 0.1 * noise - 0.05);
    }
    // Extremos en los bordes (el passthrough no debe tocarlos).
    if (frames > 0) {
        in[0] = 1.0f; in[1] = -1.0f;
        in[(size_t)(frames - 1) * 2] = 0.999f;
        in[(size_t)(frames - 1) * 2 + 1] = -0.999f;
    }
    return in;
}

TEST(RouteAAsrcBypass, EnabledByDefault) {
    RouteAAsrc asrc;
    EXPECT_TRUE(asrc.enabled())
        << "Una instancia nueva debe nacer habilitada (comportamiento "
           "existente); el bypass lo impone la capa Kotlin";
}

TEST(RouteAAsrcBypass, BypassIsBitExact) {
    RouteAAsrc asrc;
    asrc.setEnabled(false);
    EXPECT_FALSE(asrc.enabled());
    const int frames = 2048;
    const std::vector<float> in = MixedStereo(frames);
    std::vector<float> out((size_t)frames * 2, 0.0f);
    const int produced = asrc.process(in.data(), frames, out.data(), frames);
    EXPECT_EQ(produced, frames)
        << "En bypass, produced debe ser inFrames (sin remuestreo)";
    EXPECT_EQ(std::memcmp(out.data(), in.data(), (size_t)frames * 2 * sizeof(float)), 0)
        << "En bypass, la salida debe ser BIT-EXACTA a la entrada";
    // Y repetido en un segundo bloque (el bypass no acumula estado).
    std::vector<float> out2((size_t)frames * 2, 0.0f);
    const int produced2 = asrc.process(in.data(), frames, out2.data(), frames);
    EXPECT_EQ(produced2, frames);
    EXPECT_EQ(std::memcmp(out2.data(), in.data(), (size_t)frames * 2 * sizeof(float)), 0);
}

TEST(RouteAAsrcBypass, BypassRespectsOutCap) {
    RouteAAsrc asrc;
    asrc.setEnabled(false);
    const int frames = 2048;
    const int cap = 512;
    const std::vector<float> in = MixedStereo(frames);
    // Centinela después de la zona escribible: no debe tocarse.
    std::vector<float> out((size_t)frames * 2, 7.0f);
    const int produced = asrc.process(in.data(), frames, out.data(), cap);
    EXPECT_EQ(produced, cap);
    EXPECT_EQ(std::memcmp(out.data(), in.data(), (size_t)cap * 2 * sizeof(float)), 0);
    for (int i = cap * 2; i < frames * 2; ++i) {
        EXPECT_FLOAT_EQ(out[(size_t)i], 7.0f)
            << "El bypass no debe escribir más allá de outCap";
    }
}

TEST(RouteAAsrcBypass, ToggleReactivates) {
    RouteAAsrc asrc;
    asrc.setEnabled(false);
    const int frames = 4096;
    const std::vector<float> in = MixedStereo(frames);
    std::vector<float> outBypass((size_t)frames * 2, 0.0f);
    EXPECT_EQ(asrc.process(in.data(), frames, outBypass.data(), frames), frames);
    EXPECT_EQ(std::memcmp(outBypass.data(), in.data(),
                          (size_t)frames * 2 * sizeof(float)),
              0);
    // Re-habilitar: el ASRC vuelve a remuestrear (la salida ya NO es
    // bit-idéntica: el FIR 319 taps la transforma; y es finita).
    asrc.setEnabled(true);
    EXPECT_TRUE(asrc.enabled());
    std::vector<float> outActive((size_t)frames * 2 + 64, 0.0f);
    const int produced =
        asrc.process(in.data(), frames, outActive.data(), frames + 32);
    EXPECT_GT(produced, 0);
    bool identical = (produced == frames) &&
                     (std::memcmp(outActive.data(), in.data(),
                                  (size_t)frames * 2 * sizeof(float)) == 0);
    EXPECT_FALSE(identical)
        << "Con el ASRC activo, la salida debe diferir de la entrada "
           "(el FIR la transforma); si es idéntica, el bypass sigue puesto";
    for (int i = 0; i < produced * 2; ++i) {
        EXPECT_TRUE(std::isfinite(outActive[(size_t)i]));
    }
}

}  // namespace
