// test_p2_snapshot_watchdog.cpp — P2 (tronidos + watchdog), 2026-11-26.
//
// REGRESIÓN del hueco de frescura: si el daemon deja de publicar, el
// efecto seguía eternamente con los controles viejos (readLatest() deja
// de adoptar y nada expiraba el snapshot). El watchdog
// (include/omega_snapshot_freshness.h) cae a defaults seguros con rampa
// de 100 ms tras 3000 ms sin snapshot nuevo, y recupera con la misma
// rampa cuando el daemon vuelve. Este test simula un daemon muerto en
// host (sin NDK, sin teléfono) con bloques de 40 ms (~1920 frames a
// 48 kHz, la cadencia medida en vivo) y fija:
//
//   1. Con adopciones periódicas (daemon vivo) el fade se queda en 1 y
//      nunca se pide re-aplicar (coste cero en caliente).
//   2. Daemon muerto: el fade se queda en 1 durante 3000 ms, luego cae
//      a 0 dentro de la rampa de 100 ms (+1 bloque de tolerancia) y se
//      queda en 0 mientras siga muerto.
//   3. El blend a fade 0 lleva los amounts a 0 y target_gain a 1.0, sin
//      tocar control (modo, sesión, generación, EQ, intensity, flags) y
//      sin silenciar: el snapshot sigue válido (CRC re-estampado) y la
//      ruta de audio no se toca (los operadores en bypass son bit-exactos).
//   4. Recuperación: al volver una adopción, el fade sube 0 -> 1 en la
//      rampa y se pide re-aplicar durante la subida.
//   5. Sin armar (ninguna adopción aún): update() siempre devuelve false
//      y el fade se queda en 1 (los defaults ya rigen sin control plane).
//   6. Un snapshot corrupto (readLatest false) se trata igual que un
//      daemon muerto: tras 3000 ms, fade a 0.
#include <gtest/gtest.h>

#include <cmath>

#include "../include/omega_control_bus.h"
#include "../include/omega_snapshot_freshness.h"

using ivanna::OmegaDspSnapshot;
using ivanna::SnapshotFreshnessState;
using ivanna::kSnapshotStaleAfterMs;
using ivanna::omega_snapshot_apply_stale_fade;
using ivanna::snapshot_freshness_update;

namespace {

// Bloque de 40 ms, como los medidos en vivo (~1920 frames a 48 kHz).
constexpr uint64_t kBlockMs = 40u;

// Avanza el watchdog N bloques sin adopción. Devuelve el fade final.
float runDeadBlocks(SnapshotFreshnessState& st, uint64_t& nowMs, int blocks,
                    int* reapplies = nullptr) {
    int n = 0;
    for (int i = 0; i < blocks; ++i) {
        nowMs += kBlockMs;
        if (snapshot_freshness_update(st, nowMs, false, kBlockMs)) ++n;
    }
    if (reapplies != nullptr) *reapplies = n;
    return st.fade;
}

OmegaDspSnapshot makeLiveSnapshot() {
    OmegaDspSnapshot s = OmegaDspSnapshot::makeNeutral();
    s.generation = 1696299ULL;
    s.discovered_session_id = 4057;
    s.orbe_amount = 1.0f;
    s.holos_amount = 0.5f;
    s.np_amount = 1.0f;
    s.hyper_amount = 0.5f;
    s.upmixing_immersivity = 0.8f;
    s.room_wet = 0.22f;
    s.comp_amount = 0.6f;
    s.exc_red = 0.3f;
    s.target_gain = 0.8f;
    s.intensity = 0.85f;
    s.temporal_field =
        ivanna::omega_output_mode_encode(ivanna::OmegaOutputMode::SPEAKERS);
    s.eq_gains[0] = 3.0f;
    s.stampCrc();
    return s;
}

}  // namespace

TEST(P2SnapshotWatchdog, LiveDaemonNeverFadesNorReapplies) {
    SnapshotFreshnessState st;
    uint64_t nowMs = 100000ULL;
    // 10 s de bloques con adopción en cada uno (daemon vivo).
    for (int i = 0; i < 250; ++i) {
        nowMs += kBlockMs;
        EXPECT_FALSE(snapshot_freshness_update(st, nowMs, true, kBlockMs));
    }
    EXPECT_TRUE(st.armed);
    EXPECT_FLOAT_EQ(st.fade, 1.0f);
    EXPECT_FLOAT_EQ(st.appliedFade, 1.0f);
}

TEST(P2SnapshotWatchdog, DeadDaemonFadesAfterThreeSeconds) {
    SnapshotFreshnessState st;
    uint64_t nowMs = 100000ULL;
    EXPECT_FALSE(snapshot_freshness_update(st, nowMs, true, kBlockMs));
    // 2999 ms sin adopción: fade intacto en 1.
    runDeadBlocks(st, nowMs, 74, nullptr);  // 74*40 = 2960 ms
    EXPECT_FLOAT_EQ(st.fade, 1.0f);
    // Un bloque más (3000 ms justos): aún no rancio (> 3000, no >=).
    nowMs += kBlockMs;
    EXPECT_FALSE(snapshot_freshness_update(st, nowMs, false, kBlockMs));
    EXPECT_FLOAT_EQ(st.fade, 1.0f);
    // A partir de aquí cae: en 100 ms de rampa + 1 bloque llega a 0.
    int reapplies = 0;
    const float faded = runDeadBlocks(st, nowMs, 4, &reapplies);
    EXPECT_FLOAT_EQ(faded, 0.0f);
    EXPECT_GT(reapplies, 0);
    // Sigue muerto 10 s más: se queda en 0 sin re-aplicar.
    reapplies = 0;
    runDeadBlocks(st, nowMs, 250, &reapplies);
    EXPECT_FLOAT_EQ(st.fade, 0.0f);
    EXPECT_EQ(reapplies, 0);
}

TEST(P2SnapshotWatchdog, StaleBlendIsSafeDefaultsNotSilence) {
    const OmegaDspSnapshot live = makeLiveSnapshot();
    OmegaDspSnapshot faded{};
    omega_snapshot_apply_stale_fade(faded, live, 0.0f);
    // Defaults seguros: amounts a 0, target_gain a 1.0.
    EXPECT_FLOAT_EQ(faded.orbe_amount, 0.0f);
    EXPECT_FLOAT_EQ(faded.holos_amount, 0.0f);
    EXPECT_FLOAT_EQ(faded.np_amount, 0.0f);
    EXPECT_FLOAT_EQ(faded.hyper_amount, 0.0f);
    EXPECT_FLOAT_EQ(faded.upmixing_immersivity, 0.0f);
    EXPECT_FLOAT_EQ(faded.room_wet, 0.0f);
    EXPECT_FLOAT_EQ(faded.comp_amount, 0.0f);
    EXPECT_FLOAT_EQ(faded.exc_red, 0.0f);
    EXPECT_FLOAT_EQ(faded.target_gain, 1.0f);
    // Control intacto: no se resucita ni se borra nada que no sea actuación.
    EXPECT_EQ(faded.generation, 1696299ULL);
    EXPECT_EQ(faded.discovered_session_id, 4057);
    EXPECT_FLOAT_EQ(faded.intensity, 0.85f);
    EXPECT_FLOAT_EQ(faded.eq_gains[0], 3.0f);
    EXPECT_EQ(ivanna::omega_output_mode_decode(faded.temporal_field),
              ivanna::OmegaOutputMode::SPEAKERS);
    EXPECT_TRUE(faded.isValid());  // CRC re-estampado: sigue válido.
    // El blend a fade 1 es identidad.
    OmegaDspSnapshot full{};
    omega_snapshot_apply_stale_fade(full, live, 1.0f);
    EXPECT_FLOAT_EQ(full.orbe_amount, 1.0f);
    EXPECT_FLOAT_EQ(full.target_gain, 0.8f);
    // El blend a fade 0.5 es el punto medio exacto.
    OmegaDspSnapshot half{};
    omega_snapshot_apply_stale_fade(half, live, 0.5f);
    EXPECT_FLOAT_EQ(half.orbe_amount, 0.5f);
    EXPECT_FLOAT_EQ(half.target_gain, 0.9f);
}

TEST(P2SnapshotWatchdog, RecoveryRampsBackUp) {
    SnapshotFreshnessState st;
    uint64_t nowMs = 100000ULL;
    EXPECT_FALSE(snapshot_freshness_update(st, nowMs, true, kBlockMs));
    runDeadBlocks(st, nowMs, 80, nullptr);  // 3200 ms -> fade 0
    EXPECT_FLOAT_EQ(st.fade, 0.0f);
    // Vuelve el daemon: una adopción por bloque, rampa de subida.
    int reapplies = 0;
    for (int i = 0; i < 4; ++i) {
        nowMs += kBlockMs;
        if (snapshot_freshness_update(st, nowMs, true, kBlockMs)) ++reapplies;
    }
    EXPECT_FLOAT_EQ(st.fade, 1.0f);
    EXPECT_GT(reapplies, 0);
    // Ya en 1, las adopciones siguientes no re-aplican.
    nowMs += kBlockMs;
    EXPECT_FALSE(snapshot_freshness_update(st, nowMs, true, kBlockMs));
}

TEST(P2SnapshotWatchdog, NeverArmedStaysIdle) {
    SnapshotFreshnessState st;
    uint64_t nowMs = 100000ULL;
    for (int i = 0; i < 250; ++i) {
        nowMs += kBlockMs;
        EXPECT_FALSE(snapshot_freshness_update(st, nowMs, false, kBlockMs));
    }
    EXPECT_FALSE(st.armed);
    EXPECT_FLOAT_EQ(st.fade, 1.0f);
}

TEST(P2SnapshotWatchdog, CorruptSnapshotBehavesLikeDeadDaemon) {
    // readLatest() false por CRC inválido: el efecto no adopta nada y el
    // watchdog debe expirar igual que con el daemon muerto.
    SnapshotFreshnessState st;
    uint64_t nowMs = 100000ULL;
    EXPECT_FALSE(snapshot_freshness_update(st, nowMs, true, kBlockMs));
    runDeadBlocks(st, nowMs, 80, nullptr);
    EXPECT_FLOAT_EQ(st.fade, 0.0f);
}
