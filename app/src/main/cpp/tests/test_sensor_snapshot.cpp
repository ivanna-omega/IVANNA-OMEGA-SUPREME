// Pruebas del canal lateral del sensor (v2.5.506, cableado mic → organismo).
//
// Cubre el diseño del consejo (Luna/Haiku):
//   1. SensorSnapshot: validación de versión/fuente/secuencia/frescura/
//      rangos (rechazo sin tocar el estado) y constructor desde ERB[74].
//   2. SensorSnapshotChannel: latest value wins, nunca retrocede.
//   3. Puerta de ingestión kind 5 (AcousticMic) en
//      validateNeuroIngestArgs: rangos + frescura 1 s, kinds 0/1 fuera.
//   4. Rechazo sin tocar el estado: frames kind 5 inválidos no mueven
//      framesLearned ni la firma del ClosedLoopController.
//   5. Fail-closed: tras rechazar snapshots inválidos, el controlador
//      sigue en identidad (0 dB, wet 0) — sin mic/medida, P0 intacto.
//
// Header-only, sin Android ni JNI. Los fixtures son datos de prueba
// explícitos, no un sensor simulado en producción.
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "spatial/SensorSnapshot.hpp"
#include "../neuromorphic/neuro_acoustic_closed_loop.hpp"

using namespace ivanna::spatial;
using namespace ivanna::neuromorphic;

namespace {

constexpr std::uint64_t kNowUs = 5000000;  // "ahora" fijo en los tests

SensorFrame goodFrame() {
    SensorFrame f{};
    f.valid = true;
    f.frame_index = 7;
    f.level_dbfs = -30.0f;
    for (int b = 0; b < SensorFrame::kBands; ++b) {
        f.magnitude_db[b] = -40.0f;
        f.error_db[b] = 0.0f;
        f.coherence[b] = 0.9f;
    }
    return f;
}

SensorSnapshot goodSnapshot() {
    SensorSnapshot s{};
    s.version = kSensorSnapshotVersion;
    s.sourceId = kSensorSourceRouteA;
    s.sequence = 7;
    s.timestampUs = kNowUs;
    s.valid = true;
    s.frame = goodFrame();
    return s;
}

std::array<float, 74> goodErb74() {
    std::array<float, 74> f{};
    for (int b = 0; b < 24; ++b) {
        f[static_cast<std::size_t>(b)] = -40.0f;       // mag
        f[static_cast<std::size_t>(24 + b)] = 0.0f;    // err
        f[static_cast<std::size_t>(48 + b)] = 0.9f;    // coh
    }
    f[72] = -30.0f;  // level
    f[73] = 1.0f;    // valid
    return f;
}

}  // namespace

// ── 1. Validación del snapshot ──────────────────────────────────────────────
TEST(SensorSnapshot, ValidoPasa) {
    EXPECT_TRUE(validateSensorSnapshot(goodSnapshot(), kNowUs));
}

TEST(SensorSnapshot, RechazaVersionDesconocida) {
    auto s = goodSnapshot();
    s.version = 99;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
}

TEST(SensorSnapshot, RechazaFuenteDesconocida) {
    auto s = goodSnapshot();
    s.sourceId = 42;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s.sourceId = kSensorSourceRouteB;
    EXPECT_TRUE(validateSensorSnapshot(s, kNowUs));
}

TEST(SensorSnapshot, RechazaInvalido) {
    auto s = goodSnapshot();
    s.valid = false;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s = goodSnapshot();
    s.frame.valid = false;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
}

TEST(SensorSnapshot, RechazaSecuenciaCeroYDesalineada) {
    auto s = goodSnapshot();
    s.sequence = 0;
    s.frame.frame_index = 0;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s = goodSnapshot();
    s.sequence = 8;  // != frame_index (7)
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
}

TEST(SensorSnapshot, RechazaRancioYFuturo) {
    auto s = goodSnapshot();
    // 1 s + 1 µs viejo.
    s.timestampUs = kNowUs - kSensorSnapshotMaxAgeUs - 1;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    // Justo en el borde (1 s): pasa la puerta de entrada (la activación
    // estricta de 250 ms la aplica el controlador en compute()).
    s.timestampUs = kNowUs - kSensorSnapshotMaxAgeUs;
    EXPECT_TRUE(validateSensorSnapshot(s, kNowUs));
    // 100 ms + 1 µs en el futuro.
    s.timestampUs = kNowUs + kSensorSnapshotMaxFutureUs + 1;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s.timestampUs = 0;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
}

TEST(SensorSnapshot, RechazaRangosYNan) {
    auto s = goodSnapshot();
    s.frame.magnitude_db[3] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s = goodSnapshot();
    s.frame.error_db[3] = 61.0f;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s = goodSnapshot();
    s.frame.coherence[3] = 1.01f;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s = goodSnapshot();
    s.frame.level_dbfs = -121.0f;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
    s = goodSnapshot();
    s.frame.level_dbfs = 11.0f;
    EXPECT_FALSE(validateSensorSnapshot(s, kNowUs));
}

// ── 2. Constructor desde ERB[74] ───────────────────────────────────────────
TEST(SensorSnapshot, Erb74ConstruyeSnapshotValido) {
    const auto f = goodErb74();
    SensorSnapshot out{};
    ASSERT_TRUE(snapshotFromErb74(f.data(), kSensorSourceRouteA, 7, kNowUs, out));
    EXPECT_EQ(out.version, kSensorSnapshotVersion);
    EXPECT_EQ(out.sequence, 7u);
    EXPECT_EQ(out.frame.frame_index, 7u);
    EXPECT_FLOAT_EQ(out.frame.level_dbfs, -30.0f);
    EXPECT_FLOAT_EQ(out.frame.magnitude_db[0], -40.0f);
    EXPECT_FLOAT_EQ(out.frame.coherence[23], 0.9f);
    EXPECT_TRUE(validateSensorSnapshot(out, kNowUs));
}

TEST(SensorSnapshot, Erb74RechazaSinTocarOut) {
    const auto f = goodErb74();
    SensorSnapshot out{};
    out.sequence = 12345;  // testigo: no debe moverse
    // valid=0
    auto bad = f;
    bad[73] = 0.0f;
    EXPECT_FALSE(snapshotFromErb74(bad.data(), kSensorSourceRouteA, 7, kNowUs, out));
    EXPECT_EQ(out.sequence, 12345u);
    // NaN en una banda
    bad = f;
    bad[10] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(snapshotFromErb74(bad.data(), kSensorSourceRouteA, 7, kNowUs, out));
    EXPECT_EQ(out.sequence, 12345u);
    // Fuente desconocida / secuencia 0 / timestamp 0 / puntero nulo
    EXPECT_FALSE(snapshotFromErb74(f.data(), 9, 7, kNowUs, out));
    EXPECT_FALSE(snapshotFromErb74(f.data(), kSensorSourceRouteA, 0, kNowUs, out));
    EXPECT_FALSE(snapshotFromErb74(f.data(), kSensorSourceRouteA, 7, 0, out));
    EXPECT_FALSE(snapshotFromErb74(nullptr, kSensorSourceRouteA, 7, kNowUs, out));
    EXPECT_EQ(out.sequence, 12345u);
}

// ── 3. Canal latest-wins ───────────────────────────────────────────────────
TEST(SensorSnapshotChannel, LatestWinsYNuncaRetrocede) {
    SensorSnapshotChannel ch;
    SensorSnapshot out{};
    EXPECT_FALSE(ch.latest(out));
    EXPECT_FALSE(ch.hasSnapshot());
    auto s1 = goodSnapshot();
    s1.sequence = 1;
    s1.frame.frame_index = 1;
    auto s2 = goodSnapshot();
    s2.sequence = 2;
    s2.frame.frame_index = 2;
    ch.publish(s1);
    ch.publish(s2);
    ASSERT_TRUE(ch.latest(out));
    EXPECT_EQ(out.sequence, 2u);
    EXPECT_EQ(ch.sequence(), 2u);
    // Una secuencia vieja que llega tarde no pisa a la nueva.
    ch.publish(s1);
    ASSERT_TRUE(ch.latest(out));
    EXPECT_EQ(out.sequence, 2u);
    ch.reset();
    EXPECT_FALSE(ch.latest(out));
}

// ── 4. Puerta de ingestión kind 5 ──────────────────────────────────────────
TEST(NeuroIngestGate, Kind5ValidoPasa) {
    const float resp[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    const float coh[8] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    EXPECT_TRUE(validateNeuroIngestArgs(5, kNowUs, resp, coh, 0.9f, kNowUs));
    // Los kinds 2/3/4 siguen pasando.
    EXPECT_TRUE(validateNeuroIngestArgs(2, kNowUs, resp, coh, 0.9f, kNowUs));
    EXPECT_TRUE(validateNeuroIngestArgs(3, kNowUs, resp, coh, 0.9f, kNowUs));
    EXPECT_TRUE(validateNeuroIngestArgs(4, kNowUs, resp, coh, 0.9f, kNowUs));
}

TEST(NeuroIngestGate, RechazaKindsInvalidos) {
    const float resp[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    const float coh[8] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    EXPECT_FALSE(validateNeuroIngestArgs(0, kNowUs, resp, coh, 0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(1, kNowUs, resp, coh, 0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(6, kNowUs, resp, coh, 0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(-1, kNowUs, resp, coh, 0.9f, kNowUs));
}

TEST(NeuroIngestGate, RechazaRangosFrescuraYNulos) {
    const float resp[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    const float coh[8] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f};
    float badResp[8] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    badResp[2] = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs, badResp, coh, 0.9f, kNowUs));
    float badCoh[8] = {0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 1.5f};
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs, resp, badCoh, 0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs, resp, coh, 1.01f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs, resp, coh, -0.1f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs, nullptr, coh, 0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs, resp, nullptr, 0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(5, 0, resp, coh, 0.9f, kNowUs));
    // Rancio (>1 s) y futuro (>100 ms).
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs - kIngestMaxAgeUs - 1, resp, coh,
                                         0.9f, kNowUs));
    EXPECT_FALSE(validateNeuroIngestArgs(5, kNowUs + kIngestMaxFutureUs + 1, resp,
                                         coh, 0.9f, kNowUs));
}

// ── 5. Rechazo sin tocar el estado ─────────────────────────────────────────
TEST(NeuroIngestGate, RechazoNoTocaEstadoDelControlador) {
    ClosedLoopController ctl;
    ctl.prepare(48000.0f, 128);
    // Un frame kind 5 válido (vía el mapper mic) se aprende.
    const auto f74 = goodErb74();
    NeuroFrame mic{};
    ASSERT_TRUE(micErbToNeuroFrame(&f74[24], &f74[48], f74[72], true, 1000000,
                                   48000.0f, mic));
    EXPECT_EQ(mic.kind, NeuroSensorKind::AcousticMic);
    EXPECT_TRUE(ctl.ingest(mic));
    EXPECT_EQ(ctl.framesLearned(), 1u);
    const auto sigBefore = ctl.signature();
    const float confBefore = ctl.confidence();
    // Frame kind 5 con calidad baja: el controlador lo rechaza.
    NeuroFrame bad = mic;
    bad.timestampUs = 1010000;
    bad.quality = 0.1f;  // < kMinQuality (0.40)
    EXPECT_FALSE(ctl.ingest(bad));
    EXPECT_EQ(ctl.framesLearned(), 1u);
    const auto sigAfter = ctl.signature();
    for (int b = 0; b < kNeuroBands; ++b) {
        EXPECT_FLOAT_EQ(sigAfter.gainDb[static_cast<std::size_t>(b)],
                        sigBefore.gainDb[static_cast<std::size_t>(b)]);
    }
    EXPECT_FLOAT_EQ(ctl.confidence(), confBefore);
    // Frame kind 5 con coherencia baja: también se rechaza sin tocar nada.
    NeuroFrame badCoh = mic;
    badCoh.timestampUs = 1020000;
    for (int b = 0; b < kNeuroBands; ++b)
        badCoh.coherence[static_cast<std::size_t>(b)] = 0.05f;
    EXPECT_FALSE(ctl.ingest(badCoh));
    EXPECT_EQ(ctl.framesLearned(), 1u);
}

// ── 6. Fail-closed: sin medida válida, identidad ───────────────────────────
TEST(NeuroIngestGate, FailClosedSinMedidaValida) {
    ClosedLoopController ctl;
    ctl.prepare(48000.0f, 128);
    // Solo snapshots inválidos (rechazados por la puerta): el controlador
    // nunca se entera y compute() da identidad.
    auto bad = goodSnapshot();
    bad.valid = false;
    EXPECT_FALSE(validateSensorSnapshot(bad, kNowUs));
    const auto adj = ctl.compute(kNowUs);
    EXPECT_FALSE(adj.active);
    EXPECT_FLOAT_EQ(adj.wetScale, 0.0f);
    for (int b = 0; b < kNeuroBands; ++b) {
        EXPECT_FLOAT_EQ(adj.gainDb[static_cast<std::size_t>(b)], 0.0f);
        EXPECT_FLOAT_EQ(adj.phaseRad[static_cast<std::size_t>(b)], 0.0f);
    }
    EXPECT_EQ(ctl.framesLearned(), 0u);
}
