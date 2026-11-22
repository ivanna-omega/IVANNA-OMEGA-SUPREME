// test_rt_log_ring.cpp — Anillo lock-free de eventos RT (v2.5.446).
//
// Cubre include/rt_log_ring.hpp: push/drain sin alocación ni lock, orden de
// publicación (payload antes de seq), wrap con pérdida de los más viejos
// (nunca de audio) y productores concurrentes. El fix que esto protege:
// check_rt_safety.py pasaba de 6 violaciones (LOGI/LOGW/LOGE en
// omega_process, logPassthroughThrottled y logDspShedStateThrottled) a 0.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "../include/rt_log_ring.hpp"

using ivanna::rtlog::Event;
using ivanna::rtlog::Record;

namespace {

// Record lleva un std::atomic (no copiable): el test acumula snapshots planos.
struct Snapshot {
    uint64_t monoNs = 0;
    uint8_t code = 0;
    uint8_t reason = 0;
    float a = 0.f, b = 0.f, c = 0.f, d = 0.f;
};

std::vector<Snapshot> g_drained;
void collectRecord(const Record& r) {
    g_drained.push_back(Snapshot{r.monoNs, r.code, r.reason, r.a, r.b, r.c, r.d});
}

}  // namespace

TEST(RtLogRing, PushDrainRoundtrip) {
    ASSERT_EQ(ivanna::rtlog::pendingCount(), 0);
    ivanna::rtlog::push(Event::kPassthrough, 3, 1.5f, 2.5f, 3.5f, 4.5f);
    ivanna::rtlog::push(Event::kDspShedActive, 0, 10.f, 100.f, 500.f, 900.f);
    EXPECT_EQ(ivanna::rtlog::pendingCount(), 2);
    g_drained.clear();
    ivanna::rtlog::drain(&collectRecord);
    ASSERT_EQ(g_drained.size(), 2u);
    // Orden de slot, no cronológico garantizado: buscar por código.
    bool sawPassthrough = false, sawShed = false;
    for (const auto& r : g_drained) {
        if (r.code == static_cast<uint8_t>(Event::kPassthrough)) {
            sawPassthrough = true;
            EXPECT_EQ(r.reason, 3);
            EXPECT_FLOAT_EQ(r.a, 1.5f);
            EXPECT_FLOAT_EQ(r.d, 4.5f);
            EXPECT_GT(r.monoNs, 0u);
        }
        if (r.code == static_cast<uint8_t>(Event::kDspShedActive)) {
            sawShed = true;
            EXPECT_FLOAT_EQ(r.a, 10.f);
            EXPECT_FLOAT_EQ(r.b, 100.f);
            EXPECT_FLOAT_EQ(r.c, 500.f);
            EXPECT_FLOAT_EQ(r.d, 900.f);
        }
    }
    EXPECT_TRUE(sawPassthrough);
    EXPECT_TRUE(sawShed);
    // Segundo drenaje: vacío.
    EXPECT_EQ(ivanna::rtlog::pendingCount(), 0);
    g_drained.clear();
    ivanna::rtlog::drain(&collectRecord);
    EXPECT_TRUE(g_drained.empty());
}

TEST(RtLogRing, OverflowLosesOldestNeverCrashes) {
    // 200 eventos en un anillo de 64: quedan como mucho 64 pendientes y
    // el drenaje sigue funcionando. La pérdida es solo de diagnóstico.
    for (int i = 0; i < 200; ++i) {
        ivanna::rtlog::push(Event::kPassthrough, 0, static_cast<float>(i), 0.f, 0.f, 0.f);
    }
    EXPECT_LE(ivanna::rtlog::pendingCount(), ivanna::rtlog::kCapacity);
    EXPECT_GT(ivanna::rtlog::pendingCount(), 0);
    g_drained.clear();
    ivanna::rtlog::drain(&collectRecord);
    EXPECT_LE(g_drained.size(), static_cast<size_t>(ivanna::rtlog::kCapacity));
    EXPECT_EQ(ivanna::rtlog::pendingCount(), 0);
}

TEST(RtLogRing, ConcurrentProducersDoNotCorrupt) {
    constexpr int kThreads = 4;
    constexpr int kPushes = 1000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t] {
            for (int i = 0; i < kPushes; ++i) {
                ivanna::rtlog::push(Event::kAutoEnable, static_cast<uint8_t>(t),
                                    static_cast<float>(i), 0.f, 0.f, 0.f);
            }
        });
    }
    for (auto& t : ts) t.join();
    EXPECT_LE(ivanna::rtlog::pendingCount(), ivanna::rtlog::kCapacity);
    g_drained.clear();
    ivanna::rtlog::drain(&collectRecord);
    EXPECT_LE(g_drained.size(), static_cast<size_t>(ivanna::rtlog::kCapacity));
    // Todo lo drenado tiene código y motivo válidos (sin tearing salvaje).
    for (const auto& r : g_drained) {
        EXPECT_EQ(r.code, static_cast<uint8_t>(Event::kAutoEnable));
        EXPECT_LT(r.reason, kThreads);
    }
    EXPECT_EQ(ivanna::rtlog::pendingCount(), 0);
}

TEST(RtLogRing, PushIsFastEnoughForRt) {
    // Cota blanda de host: 10 000 pushes deben costar < 50 ms en total
    // (< 5 us por push, dos órdenes por debajo del periodo de bloque).
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10000; ++i) {
        ivanna::rtlog::push(Event::kPassthrough, 0, 0.f, 0.f, 0.f, 0.f);
    }
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("[rt_log_ring] 10000 pushes: %lld us (%.3f us/push)\n",
                static_cast<long long>(us), us / 10000.0);
    EXPECT_LT(us, 50000);
    g_drained.clear();
    ivanna::rtlog::drain(&collectRecord);
}
