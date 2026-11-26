// test_p2_telemetry_mirror.cpp — P2 (tronidos + watchdog), 2026-11-26.
//
// REGRESIÓN de la causa raíz del RMS=0 en vivo (2026-11-26 ~02:44 CST):
// Qobuz sonando, sesión 4057, daemon vivo, pero el snapshot de control
// mostraba raw_rms = 0.0 / raw_peak = 0.0 / effect_frames = 0. El efecto
// SÍ medía (bus local: RMS 0.0234 / pico 0.0657; beacon a 47 979
// frames/s), pero el daemon nunca leía el bus local del efecto: lastSnap
// nace de makeDefault()/makeNeutral() (telemetría en 0) y ningún escritor
// del snapshot de control tocaba esos campos. EffectTelemetryMirror
// (daemon/control/effect_telemetry_mirror.h) hace que el daemon — único
// escritor del bus de control — refleje la telemetría del efecto antes
// de publicar. Este test usa el OmegaControlBus REAL sobre un archivo
// temporal en /tmp (mismo patrón que test_upmixing_control_plane) y fija:
//
//   1. Con audio vivo, el espejo copia RMS/pico/bandas/supreme_* del bus
//      local al snapshot de control y deja el control intacto (generación
//      propia, sesión, amounts, flags, modo).
//   2. effect_frames es un total MONOTÓNICO del daemon: avanza por deltas
//      y no se doble-cuenta si se sondea dos veces sin publicación nueva.
//   3. Si el contador local se resetea (efecto viejo / adopción vieja),
//      el total NO baja: el delta es el valor actual, nunca negativo.
//   4. Bus local rancio (> 2000 ms sin publicar = sin audio): los niveles
//      se publican en 0 (silencio honesto) y el total de frames se conserva.
//   5. Sin bus local nunca visto: todo en 0 (fail-closed, como hoy).
//   6. EVOL-LIVEPATH (consumer_generation muerto): el campo
//      consumer_generation (@32) nacía en 0 y NADIE lo escribía en
//      producción (solo los tests de ABI fijaban su offset). El espejo
//      refleja la generación aplicada del beacon (applied_control_
//      generation, lo que omega_effect confirmó aplicar en onBlock):
//      sin nota del beacon no se toca; con nota, avanza; con nota 0,
//      vuelve a 0 (el efecto no confirmó nada).
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "../daemon/control/effect_telemetry_mirror.h"
#include "../include/omega_control_bus.h"

using ivanna::OmegaControlBus;
using ivanna::OmegaDspSnapshot;
using ivanna::daemon::EffectTelemetryMirror;
using ivanna::daemon::kEffectTelemetryMaxAgeMs;

namespace {

uint64_t nowMs() {
    struct timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

std::string tmpPath(const char* name) {
    return std::string("/tmp/p2_mirror_") + name + ".bin";
}

struct LocalBus {
    std::string path;
    OmegaControlBus writer;
    OmegaControlBus reader;
    explicit LocalBus(const char* name) : path(tmpPath(name)) {
        std::remove(path.c_str());
    }
    ~LocalBus() { std::remove(path.c_str()); }
    bool open() {
        return writer.openWriter(path.c_str()) && reader.openReader(path.c_str());
    }
    // Publica un snapshot local como lo haría el efecto: telemetría medida
    // + resto copiado de un snapshot de control cualquiera.
    bool publish(float rms, float peak, uint64_t frames) {
        OmegaDspSnapshot s = OmegaDspSnapshot::makeNeutral();
        s.raw_rms = rms;
        s.raw_peak = peak;
        s.raw_band_low = rms * 0.8f;
        s.raw_band_mid = rms * 0.27f;
        s.raw_band_high = rms * 0.09f;
        s.raw_band_spread = 2140.8f;
        s.effect_frames = frames;
        s.supreme_declipped_peaks = 7u;
        s.supreme_tape_mag = 0.42f;
        s.supreme_snn_spikes = 11u;
        s.supreme_subsample_delay = 0.31f;
        return writer.publish(s);
    }
    uint64_t lastTimestampMs() {
        OmegaDspSnapshot s{};
        uint64_t seen = 0;
        if (!reader.readLatest(s, seen)) return 0;
        return s.timestamp_ms;
    }
};

OmegaDspSnapshot makeControl() {
    OmegaDspSnapshot s = OmegaDspSnapshot::makeNeutral();
    s.generation = 1696299ULL;
    s.discovered_session_id = 4057;
    s.orbe_amount = 1.0f;
    s.holos_amount = 0.0f;
    s.temporal_field =
        ivanna::omega_output_mode_encode(ivanna::OmegaOutputMode::SPEAKERS);
    s.flags = 0x8012u;
    return s;
}

}  // namespace

TEST(P2TelemetryMirror, MirrorsLiveLevelsAndKeepsControl) {
    LocalBus bus("live");
    ASSERT_TRUE(bus.open());
    ASSERT_TRUE(bus.publish(0.0234f, 0.0657f, 15360ULL));

    EffectTelemetryMirror mirror;
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));

    OmegaDspSnapshot control = makeControl();
    mirror.applyTo(control, true);
    EXPECT_FLOAT_EQ(control.raw_rms, 0.0234f);
    EXPECT_FLOAT_EQ(control.raw_peak, 0.0657f);
    EXPECT_FLOAT_EQ(control.raw_band_low, 0.0234f * 0.8f);
    EXPECT_FLOAT_EQ(control.raw_band_spread, 2140.8f);
    EXPECT_EQ(control.supreme_declipped_peaks, 7u);
    EXPECT_FLOAT_EQ(control.supreme_tape_mag, 0.42f);
    EXPECT_EQ(control.effect_frames, 15360ULL);
    // Control intacto: el espejo no toca nada que no sea telemetría.
    EXPECT_EQ(control.generation, 1696299ULL);
    EXPECT_EQ(control.discovered_session_id, 4057);
    EXPECT_FLOAT_EQ(control.orbe_amount, 1.0f);
    EXPECT_EQ(control.flags, 0x8012u);
    EXPECT_EQ(ivanna::omega_output_mode_decode(control.temporal_field),
              ivanna::OmegaOutputMode::SPEAKERS);
}

TEST(P2TelemetryMirror, FramesTotalIsMonotonicWithoutDoubleCount) {
    LocalBus bus("mono");
    ASSERT_TRUE(bus.open());
    EffectTelemetryMirror mirror;

    ASSERT_TRUE(bus.publish(0.01f, 0.02f, 1000ULL));
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    OmegaDspSnapshot c1 = makeControl();
    mirror.applyTo(c1, true);
    EXPECT_EQ(c1.effect_frames, 1000ULL);

    // Sondear dos veces sin publicación nueva no suma nada.
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    OmegaDspSnapshot c2 = makeControl();
    mirror.applyTo(c2, true);
    EXPECT_EQ(c2.effect_frames, 1000ULL);

    ASSERT_TRUE(bus.publish(0.02f, 0.03f, 2500ULL));
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    OmegaDspSnapshot c3 = makeControl();
    mirror.applyTo(c3, true);
    EXPECT_EQ(c3.effect_frames, 2500ULL);
}

TEST(P2TelemetryMirror, LocalResetDoesNotMoveTotalBackwards) {
    LocalBus bus("reset");
    ASSERT_TRUE(bus.open());
    EffectTelemetryMirror mirror;

    ASSERT_TRUE(bus.publish(0.01f, 0.02f, 10000ULL));
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    // El contador local vuelve a 5 (efecto viejo / adopción que resetea):
    // el total suma 5, no baja a 5 ni se vuelve negativo.
    ASSERT_TRUE(bus.publish(0.01f, 0.02f, 5ULL));
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    OmegaDspSnapshot c = makeControl();
    mirror.applyTo(c, true);
    EXPECT_EQ(c.effect_frames, 10005ULL);
}

TEST(P2TelemetryMirror, StaleLocalPublishesSilenceButKeepsFrames) {
    LocalBus bus("stale");
    ASSERT_TRUE(bus.open());
    EffectTelemetryMirror mirror;

    ASSERT_TRUE(bus.publish(0.0234f, 0.0657f, 15360ULL));
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    // La música paró: el último local tiene 5 s de edad (> 2000 ms).
    const uint64_t staleNow = bus.lastTimestampMs() + kEffectTelemetryMaxAgeMs + 3000ULL;
    EXPECT_FALSE(mirror.poll(bus.reader, staleNow));
    OmegaDspSnapshot c = makeControl();
    mirror.applyTo(c, false);
    EXPECT_FLOAT_EQ(c.raw_rms, 0.0f);
    EXPECT_FLOAT_EQ(c.raw_peak, 0.0f);
    EXPECT_FLOAT_EQ(c.raw_band_low, 0.0f);
    EXPECT_EQ(c.supreme_declipped_peaks, 0u);
    // El total de frames NO se borra con el silencio.
    EXPECT_EQ(c.effect_frames, 15360ULL);
}

TEST(P2TelemetryMirror, NeverSeenLocalIsFailClosedZero) {
    LocalBus bus("never");
    ASSERT_TRUE(bus.open());
    EffectTelemetryMirror mirror;
    EXPECT_FALSE(mirror.poll(bus.reader, nowMs()));
    OmegaDspSnapshot c = makeControl();
    mirror.applyTo(c, false);
    EXPECT_FLOAT_EQ(c.raw_rms, 0.0f);
    EXPECT_FLOAT_EQ(c.raw_peak, 0.0f);
    EXPECT_EQ(c.effect_frames, 0ULL);
    EXPECT_FLOAT_EQ(c.orbe_amount, 1.0f);
}

TEST(P2TelemetryMirror, ConsumerGenerationUntouchedWithoutBeaconNote) {
    LocalBus bus("cg-none");
    ASSERT_TRUE(bus.open());
    ASSERT_TRUE(bus.publish(0.0234f, 0.0657f, 15360ULL));

    EffectTelemetryMirror mirror;
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    // Sin nota del beacon, el espejo NO toca consumer_generation: el
    // valor que trae el snapshot de control se conserva (aquí 42).
    OmegaDspSnapshot c = makeControl();
    c.consumer_generation = 42ULL;
    mirror.applyTo(c, true);
    EXPECT_EQ(c.consumer_generation, 42ULL);
    EXPECT_FLOAT_EQ(c.raw_rms, 0.0234f);
}

TEST(P2TelemetryMirror, ConsumerGenerationMirrorsBeaconAppliedGen) {
    LocalBus bus("cg-live");
    ASSERT_TRUE(bus.open());
    ASSERT_TRUE(bus.publish(0.0234f, 0.0657f, 15360ULL));

    EffectTelemetryMirror mirror;
    EXPECT_TRUE(mirror.poll(bus.reader, nowMs()));
    // El efecto confirmó aplicar la generación 1696300 (beacon).
    mirror.noteBeaconAppliedGeneration(1696300ULL);
    OmegaDspSnapshot c = makeControl();
    mirror.applyTo(c, true);
    EXPECT_EQ(c.consumer_generation, 1696300ULL);
    // Es un acuse de control, no un nivel: también se refleja con el
    // bus local rancio (fresh=false), mientras los niveles van a 0.
    OmegaDspSnapshot cStale = makeControl();
    mirror.applyTo(cStale, false);
    EXPECT_EQ(cStale.consumer_generation, 1696300ULL);
    EXPECT_FLOAT_EQ(cStale.raw_rms, 0.0f);
    // Control intacto: la generación propia del snapshot no se toca.
    EXPECT_EQ(c.generation, 1696299ULL);
}

TEST(P2TelemetryMirror, ConsumerGenerationZeroIsHonest) {
    EffectTelemetryMirror mirror;
    // El efecto procesa pero no adoptó snapshot (applied 0): el campo
    // va a 0, no se queda con un valor viejo.
    mirror.noteBeaconAppliedGeneration(1696300ULL);
    OmegaDspSnapshot c1 = makeControl();
    mirror.applyTo(c1, false);
    EXPECT_EQ(c1.consumer_generation, 1696300ULL);
    mirror.noteBeaconAppliedGeneration(0ULL);
    OmegaDspSnapshot c2 = makeControl();
    c2.consumer_generation = 1696300ULL;
    mirror.applyTo(c2, false);
    EXPECT_EQ(c2.consumer_generation, 0ULL);
}
