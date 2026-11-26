// test_hardware_clock_from_muscle.cpp — EVOL-LIVEPATH (2026-11-26).
//
// REGRESIÓN de la causa raíz de hardware_rate_millihz = 0 en vivo: la
// cadena efecto <- snapshot HAL estaba rota en el primer eslabón. El
// efecto SÍ lee /data/vendor/ivanna_audio/clock_snapshot.v2
// (HardwareClockReader + tracker -> onHardwareClockEstimate -> beacon),
// pero NADIE lo publicaba: HalClockProducer nunca se instancia en
// producción y su config por defecto se niega a abrir un PCM por diseño.
// La fuente viva es el músculo ASRC: el estado de fusión ya trae el par
// (timestamp, posición) del reloj real + la tasa del stream. El daemon
// re-publica esa muestra (daemon/hardware_clock_from_muscle.hpp) y este
// test fija el contrato del helper + el extremo a extremo con el tracker
// del efecto:
//
//   1. Par mmap (HWSYNC) preferido; par write-timing como fallback.
//   2. Rechazos fail-closed: sin reloj, tasa fuera de rango.
//   3. Sin sesión confirmada: identidad sintética 1/1 (no 0: el tracker
//      exige streamId y routeGeneration ambos cero o ambos no-cero).
//   4. Extremo a extremo: dos muestras a 1 s con 48000 frames -> el
//      tracker del efecto estima 48000 Hz, deriva ~0 ppm (el beacon
//      publicaría hardware_rate_millihz = 48000000). Igual a 44100 Hz:
//      la tasa REAL del stream, no el 48000 por defecto.
//   5. Cambio de sesión (routeGeneration) re-ancla el tracker; muestra
//      congelada (músculo parado) no valida.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "../daemon/hardware_clock_from_muscle.hpp"
#include "../include/audio_hardware_clock.hpp"

using ivanna::audio::HardwareClockSample;
using ivanna::audio::HardwareClockTracker;
using ivanna::daemon::buildHardwareClockSample;

namespace {

IvannaFusionState makeMuscle(uint32_t rateHz, int64_t mmapNs, uint32_t mmapFrames,
                             int64_t writeNs = 0, uint32_t written = 0) {
    IvannaFusionState st{};
    st.magic = IVANNA_FUSION_MAGIC;
    st.version = IVANNA_FUSION_VERSION;
    st.stream_rate_hz = rateHz;
    st.clock_mmap_ns = mmapNs;
    st.clock_mmap_frames = mmapFrames;
    st.clock_write_ns = writeNs;
    st.frames_written = written;
    st.muscle_health = 1;
    return st;
}

}  // namespace

TEST(HardwareClockFromMuscle, PrefersMmapPair) {
    const IvannaFusionState st = makeMuscle(48000, 1'700'000'000'000LL, 123456);
    HardwareClockSample out{};
    ASSERT_TRUE(buildHardwareClockSample(st, 2297, 7, out));
    EXPECT_EQ(out.nominalSampleRateHz, 48000u);
    EXPECT_EQ(out.hardwareTimestampNs, 1'700'000'000'000ULL);
    EXPECT_EQ(out.presentedFrames, 123456ULL);
    EXPECT_EQ(out.streamId, 2297ULL);
    EXPECT_EQ(out.routeGeneration, 7ULL);
}

TEST(HardwareClockFromMuscle, FallsBackToWriteTimingPair) {
    // Stream no-mmap: timestamp mmap en 0, par write-timing válido.
    const IvannaFusionState st = makeMuscle(44100, 0, 0, 1'700'000'000'000LL, 777);
    HardwareClockSample out{};
    ASSERT_TRUE(buildHardwareClockSample(st, 2297, 7, out));
    EXPECT_EQ(out.nominalSampleRateHz, 44100u);
    EXPECT_EQ(out.hardwareTimestampNs, 1'700'000'000'000ULL);
    EXPECT_EQ(out.presentedFrames, 777ULL);
}

TEST(HardwareClockFromMuscle, RejectsWithoutClockOrRate) {
    HardwareClockSample out{};
    // Sin ningún timestamp: no hay reloj que publicar.
    EXPECT_FALSE(buildHardwareClockSample(makeMuscle(48000, 0, 0), 2297, 7, out));
    // Tasa fuera de rango: no se publica (ni 0 ni 7999 ni 768001).
    EXPECT_FALSE(buildHardwareClockSample(makeMuscle(0, 1'000'000LL, 10), 2297, 7, out));
    EXPECT_FALSE(buildHardwareClockSample(makeMuscle(7999, 1'000'000LL, 10), 2297, 7, out));
    EXPECT_FALSE(buildHardwareClockSample(makeMuscle(768001, 1'000'000LL, 10), 2297, 7, out));
    // Tasa en los bordes sí.
    EXPECT_TRUE(buildHardwareClockSample(makeMuscle(8000, 1'000'000LL, 10), 2297, 7, out));
    EXPECT_TRUE(buildHardwareClockSample(makeMuscle(768000, 1'000'000LL, 10), 2297, 7, out));
}

TEST(HardwareClockFromMuscle, NoSessionUsesSyntheticIdentity) {
    const IvannaFusionState st = makeMuscle(48000, 1'000'000LL, 10);
    HardwareClockSample out{};
    ASSERT_TRUE(buildHardwareClockSample(st, 0, 0, out));
    EXPECT_EQ(out.streamId, 1ULL);
    EXPECT_EQ(out.routeGeneration, 1ULL);
}

TEST(HardwareClockFromMuscle, TrackerMeasures48000FromMuscleSamples) {
    // ANTES: sin publicador, el tracker nunca recibía muestras y el beacon
    // quedaba en hardware_rate_millihz = 0. DESPUÉS: dos muestras del
    // músculo a 1 s de distancia con 48000 frames -> 48000 Hz medidos.
    HardwareClockTracker tracker;
    HardwareClockSample s1{}, s2{};
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 1'000'000'000LL, 100000), 2297, 7, s1));
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 2'000'000'000LL, 148000), 2297, 7, s2));
    EXPECT_FALSE(tracker.observe(s1).valid);  // primera muestra: ancla
    const auto st2 = tracker.observe(s2);
    ASSERT_TRUE(st2.valid);
    EXPECT_NEAR(st2.measuredSampleRateHz, 48000.0, 0.001);
    EXPECT_NEAR(st2.driftPpm, 0.0, 0.001);
    EXPECT_EQ(st2.frameDelta, 48000ULL);
    EXPECT_EQ(st2.elapsedNs, 1'000'000'000ULL);
}

TEST(HardwareClockFromMuscle, TrackerMeasures44100NotDefault48000) {
    // La sesión es a 44.1 kHz: el tracker debe medir 44100, no el 48000
    // por defecto del núcleo. 44100 frames en 1 s exacto.
    HardwareClockTracker tracker;
    HardwareClockSample s1{}, s2{};
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(44100, 1'000'000'000LL, 5000), 2297, 3, s1));
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(44100, 2'000'000'000LL, 49100), 2297, 3, s2));
    EXPECT_FALSE(tracker.observe(s1).valid);
    const auto st2 = tracker.observe(s2);
    ASSERT_TRUE(st2.valid);
    EXPECT_NEAR(st2.measuredSampleRateHz, 44100.0, 0.001);
    EXPECT_NEAR(st2.driftPpm, 0.0, 0.001);
}

TEST(HardwareClockFromMuscle, TrackerMeasuresDrift) {
    // 48002 frames en 1 s a nominal 48000 -> +41.667 ppm.
    HardwareClockTracker tracker;
    HardwareClockSample s1{}, s2{};
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 1'000'000'000LL, 0), 2297, 1, s1));
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 2'000'000'000LL, 48002), 2297, 1, s2));
    EXPECT_FALSE(tracker.observe(s1).valid);
    const auto st2 = tracker.observe(s2);
    ASSERT_TRUE(st2.valid);
    EXPECT_NEAR(st2.measuredSampleRateHz, 48002.0, 0.001);
    EXPECT_NEAR(st2.driftPpm, 41.6667, 0.001);
}

TEST(HardwareClockFromMuscle, RouteChangeReanchorsTracker) {
    HardwareClockTracker tracker;
    HardwareClockSample s1{}, s2{}, s3{};
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 1'000'000'000LL, 0), 2297, 1, s1));
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 2'000'000'000LL, 48000), 2297, 1, s2));
    EXPECT_FALSE(tracker.observe(s1).valid);
    ASSERT_TRUE(tracker.observe(s2).valid);
    // Cambia la sesión (routeGeneration 1 -> 2): la muestra ancla de
    // nuevo en vez de medir contra la ruta vieja.
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 3'000'000'000LL, 96000), 4057, 2, s3));
    EXPECT_FALSE(tracker.observe(s3).valid);
    HardwareClockSample s4{};
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 4'000'000'000LL, 144000), 4057, 2, s4));
    const auto st4 = tracker.observe(s4);
    ASSERT_TRUE(st4.valid);
    EXPECT_NEAR(st4.measuredSampleRateHz, 48000.0, 0.001);
}

TEST(HardwareClockFromMuscle, FrozenMuscleDoesNotValidate) {
    // Músculo parado: misma muestra dos veces -> el tracker no valida
    // (no sostiene una tasa rancia como si fuera viva).
    HardwareClockTracker tracker;
    HardwareClockSample s1{}, s2{};
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 1'000'000'000LL, 48000), 2297, 1, s1));
    ASSERT_TRUE(buildHardwareClockSample(makeMuscle(48000, 1'000'000'000LL, 48000), 2297, 1, s2));
    EXPECT_FALSE(tracker.observe(s1).valid);
    EXPECT_FALSE(tracker.observe(s2).valid);
}
