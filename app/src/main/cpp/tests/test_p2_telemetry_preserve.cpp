// test_p2_telemetry_preserve.cpp — P2 (tronidos + watchdog), 2026-11-26.
//
// REGRESIÓN de la causa raíz del RMS=0 en vivo (2026-11-26 ~02:44 CST):
// Qobuz sonando, sesión 4057, daemon vivo, pero el snapshot de control
// mostraba raw_rms = 0.0 y raw_peak = 0.0. El efecto SÍ medía (bus local
// de telemetría: RMS 0.0234 / pico 0.0657 con música sonando; beacon a
// 47 979 frames/s), pero cada adopción (`ctx->pendingSnap = snap` en
// omega_process) sobrescribía la telemetría medida con los ceros del
// snapshot del daemon, que nunca escribe esos campos (makeDefault los
// pone en 0; ni applyPendingPerceptual ni applyFusionToSnapshot los
// tocan). omega_preserve_effect_telemetry() conserva los campos medidos
// al adoptar; este test fija ese contrato sin NDK ni teléfono:
//
//   1. Adoptar un snapshot nuevo conserva raw_rms/raw_peak/bandas/
//      effect_frames/supreme_* del pendingSnap viejo.
//   2. El control viene del snapshot nuevo (generación, sesión, amounts,
//      flags, modo, timestamp): la preservación no resucita control viejo.
//   3. effect_frames es monotónico tras adopciones repetidas (antes era
//      diente de sierra: volvía a 0 en cada generación).
//   4. Adoptar sobre un pendingSnap virgen (todo en 0) no inventa
//      telemetría: sigue en 0.
//   5. La preservación no toca generation/timestamp/crc del adoptado
//      (el bus los estampa al publicar; aquí solo se verifica que no se
//      copian los del viejo).
#include <gtest/gtest.h>

#include "../include/omega_control_bus.h"
#include "../include/omega_snapshot_telemetry.h"

using ivanna::OmegaDspSnapshot;
using ivanna::omega_preserve_effect_telemetry;

namespace {

OmegaDspSnapshot makeMeasured() {
    OmegaDspSnapshot old = OmegaDspSnapshot::makeNeutral();
    old.raw_rms = 0.0234f;
    old.raw_peak = 0.0657f;
    old.raw_band_low = 0.0185f;
    old.raw_band_mid = 0.0063f;
    old.raw_band_high = 0.0022f;
    old.raw_band_spread = 2140.8f;
    old.effect_frames = 15360ULL;
    old.supreme_declipped_peaks = 7u;
    old.supreme_tape_mag = 0.42f;
    old.supreme_snn_spikes = 11u;
    old.supreme_subsample_delay = 0.31f;
    return old;
}

OmegaDspSnapshot makeAdopted() {
    OmegaDspSnapshot snap = OmegaDspSnapshot::makeNeutral();
    snap.generation = 1696299ULL;
    snap.timestamp_ms = 34519526ULL;
    snap.discovered_session_id = 4057;
    snap.orbe_amount = 1.0f;
    snap.holos_amount = 0.0f;
    snap.temporal_field = ivanna::omega_output_mode_encode(ivanna::OmegaOutputMode::SPEAKERS);
    // Telemetría en 0, como la publica el daemon (nunca la escribe).
    snap.raw_rms = 0.0f;
    snap.raw_peak = 0.0f;
    snap.effect_frames = 0ULL;
    return snap;
}

}  // namespace

TEST(P2TelemetryPreserve, KeepsMeasuredLevelsOnAdopt) {
    const OmegaDspSnapshot measured = makeMeasured();
    OmegaDspSnapshot adopted = makeAdopted();
    omega_preserve_effect_telemetry(adopted, measured);
    EXPECT_FLOAT_EQ(adopted.raw_rms, 0.0234f);
    EXPECT_FLOAT_EQ(adopted.raw_peak, 0.0657f);
    EXPECT_FLOAT_EQ(adopted.raw_band_low, 0.0185f);
    EXPECT_FLOAT_EQ(adopted.raw_band_mid, 0.0063f);
    EXPECT_FLOAT_EQ(adopted.raw_band_high, 0.0022f);
    EXPECT_FLOAT_EQ(adopted.raw_band_spread, 2140.8f);
    EXPECT_EQ(adopted.effect_frames, 15360ULL);
    EXPECT_EQ(adopted.supreme_declipped_peaks, 7u);
    EXPECT_FLOAT_EQ(adopted.supreme_tape_mag, 0.42f);
    EXPECT_EQ(adopted.supreme_snn_spikes, 11u);
    EXPECT_FLOAT_EQ(adopted.supreme_subsample_delay, 0.31f);
}

TEST(P2TelemetryPreserve, ControlComesFromAdoptedSnapshot) {
    const OmegaDspSnapshot measured = makeMeasured();
    OmegaDspSnapshot adopted = makeAdopted();
    omega_preserve_effect_telemetry(adopted, measured);
    EXPECT_EQ(adopted.generation, 1696299ULL);
    EXPECT_EQ(adopted.timestamp_ms, 34519526ULL);
    EXPECT_EQ(adopted.discovered_session_id, 4057);
    EXPECT_FLOAT_EQ(adopted.orbe_amount, 1.0f);
    EXPECT_FLOAT_EQ(adopted.holos_amount, 0.0f);
    EXPECT_EQ(ivanna::omega_output_mode_decode(adopted.temporal_field),
              ivanna::OmegaOutputMode::SPEAKERS);
}

TEST(P2TelemetryPreserve, EffectFramesStaysMonotonicAcrossAdoptions) {
    // Antes del fix: cada adopción reseteaba effect_frames a 0 (diente
    // de sierra). Tres adopciones con 1000 frames procesados entre cada
    // una deben acumular 3000, no volver a 0.
    OmegaDspSnapshot pending = OmegaDspSnapshot::makeNeutral();
    uint64_t frames = 0;
    for (int i = 0; i < 3; ++i) {
        frames += 1000;
        pending.effect_frames = frames;
        pending.raw_rms = 0.01f * static_cast<float>(i + 1);
        OmegaDspSnapshot adopted = makeAdopted();
        adopted.generation = 100ULL + static_cast<uint64_t>(i);
        omega_preserve_effect_telemetry(adopted, pending);
        pending = adopted;
    }
    EXPECT_EQ(pending.effect_frames, 3000ULL);
    EXPECT_FLOAT_EQ(pending.raw_rms, 0.03f);
}

TEST(P2TelemetryPreserve, VirginPendingSnapStaysZero) {
    const OmegaDspSnapshot virgin = OmegaDspSnapshot::makeNeutral();
    OmegaDspSnapshot adopted = makeAdopted();
    omega_preserve_effect_telemetry(adopted, virgin);
    EXPECT_FLOAT_EQ(adopted.raw_rms, 0.0f);
    EXPECT_FLOAT_EQ(adopted.raw_peak, 0.0f);
    EXPECT_EQ(adopted.effect_frames, 0ULL);
}

TEST(P2TelemetryPreserve, DoesNotCopyGenerationOrTimestampFromOld) {
    OmegaDspSnapshot measured = makeMeasured();
    measured.generation = 42ULL;
    measured.timestamp_ms = 111ULL;
    OmegaDspSnapshot adopted = makeAdopted();
    omega_preserve_effect_telemetry(adopted, measured);
    EXPECT_EQ(adopted.generation, 1696299ULL);
    EXPECT_EQ(adopted.timestamp_ms, 34519526ULL);
}
