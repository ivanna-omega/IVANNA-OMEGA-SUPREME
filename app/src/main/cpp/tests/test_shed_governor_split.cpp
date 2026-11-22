// test_shed_governor_split.cpp — 0031: un gobernador de shed POR RUTA.
//
// Bug de la 458 (tronidos continuos "de inicio a fin", 2026-11-16): UN SOLO
// DspLoadShedGovernor global servía a los dos llamadores de nativeProcess
// (DSPBridge.process), que tienen periodos opuestos:
//   - BridgePlayer (MÚSICA): ~1984 frames @48 kHz = 41.3 ms, presupuesto
//     85% = 35.1 ms, coste DSP ~6 ms (MUY por debajo).
//   - Captura: BLOCK_FRAMES=320 @48 kHz = 6.67 ms, presupuesto 5.67 ms,
//     coste DSP ~8 ms (POR ENCIMA).
// La captura generaba deuda crónica y esa deuda hacía soltar ~20% de los
// bloques al BridgePlayer aunque su propio coste estaba muy por debajo de
// su presupuesto. Cada cambio procesado->seco es un tronido
// (DspLoadTierPolicy.kt, 2026-11-12): a ~5 transitorios/s el tronido es
// continuo, "de inicio a fin". La 0025 no tocó el gobernador: por eso
// persistió en la 458 aunque la captura estuviera sana en telemetría.
//
// Fix: RouteShedGovernors (dsp/dsp_load_shed_governor.hpp) — un gobernador
// por ruta, enrutado por el PERIODO del bloque (umbral 20 ms, entre
// 6.67 ms y 41.3 ms). Cada ruta suelta solo por SU propia deuda.
//
// Estos tests simulan el tráfico entrelazado de ambas rutas bloque a
// bloque (captura cada 6.67 ms, BridgePlayer cada 41.3 ms) con los costes
// medidos en vivo, y verifican (a) que el modelo COMPARTIDO reproduce el
// bug (el BridgePlayer suelta bloques sin estar sobrecargado) y (b) que
// el modelo SEPARADO lo elimina (el BridgePlayer no suelta nunca) sin
// desactivar el shed donde sí hace falta (captura sobrecargada y
// BridgePlayer genuinamente sobrecargado siguen soltando).

#include <gtest/gtest.h>

#include <cstdint>
#include <queue>
#include <utility>
#include <vector>

#include "../dsp/dsp_load_shed_governor.hpp"

namespace {

constexpr int64_t kCapturePeriodUs = 6667;    // 320 frames @ 48 kHz.
constexpr int64_t kCaptureCostUs = 8000;      // Medido en vivo (dsp_us ~8 ms).
constexpr int64_t kBridgePeriodUs = 41333;    // ~1984 frames @ 48 kHz.
constexpr int64_t kBridgeCostUs = 6000;       // Medido en vivo (dsp_us 4.6-7.9 ms).

// Un evento de tráfico: (tiempo_us, es_captura). Min-heap por tiempo.
using Event = std::pair<int64_t, bool>;
struct EventLater {
    bool operator()(const Event& a, const Event& b) const { return a.first > b.first; }
};

// Corre el tráfico entrelazado 10 s y devuelve (sheds_captura, total_captura,
// sheds_bridge, total_bridge). `route` decide a qué gobernador va cada
// bloque: para el modelo compartido siempre el mismo; para el separado,
// RouteShedGovernors::forPeriod.
template <typename Router>
void runInterleaved(Router& route, int64_t simUs, uint64_t& capSheds,
                    uint64_t& capTotal, uint64_t& bpSheds, uint64_t& bpTotal) {
    capSheds = capTotal = bpSheds = bpTotal = 0;
    std::priority_queue<Event, std::vector<Event>, EventLater> pq;
    pq.emplace(0, true);   // captura
    pq.emplace(0, false);  // BridgePlayer
    while (!pq.empty()) {
        const auto [t, isCap] = pq.top();
        pq.pop();
        if (t > simUs) break;
        const int64_t period = isCap ? kCapturePeriodUs : kBridgePeriodUs;
        const int64_t cost = isCap ? kCaptureCostUs : kBridgeCostUs;
        auto& gov = route(period);
        if (isCap) {
            ++capTotal;
            if (gov.shouldShed(period)) {
                gov.observeShed(period);
                ++capSheds;
            } else {
                gov.observeRun(cost, period);
            }
            pq.emplace(t + kCapturePeriodUs, true);
        } else {
            ++bpTotal;
            if (gov.shouldShed(period)) {
                gov.observeShed(period);
                ++bpSheds;
            } else {
                gov.observeRun(cost, period);
            }
            pq.emplace(t + kBridgePeriodUs, false);
        }
    }
}

}  // namespace

TEST(ShedGovernorSplit, SharedGovernorLeaksCaptureDebtIntoBridgePlayer) {
    // REPRODUCCIÓN DEL BUG (0031): con UN SOLO gobernador global, el
    // BridgePlayer suelta bloques aunque su coste (6 ms) está muy por
    // debajo de su presupuesto (35.1 ms). La deuda la pone la captura.
    ivanna::dsp::DspLoadShedGovernor shared;
    auto route = [&shared](int64_t) -> ivanna::dsp::DspLoadShedGovernor& {
        return shared;
    };
    uint64_t capSheds = 0, capTotal = 0, bpSheds = 0, bpTotal = 0;
    runInterleaved(route, 10'000'000, capSheds, capTotal, bpSheds, bpTotal);
    ASSERT_GT(capTotal, 1000u);
    ASSERT_GT(bpTotal, 100u);
    // El BridgePlayer suelta una fracción SUSTANCIAL de sus bloques sin
    // estar sobrecargado: esa es la contaminación por la deuda ajena.
    const double bpFrac =
        static_cast<double>(bpSheds) / static_cast<double>(bpTotal);
    EXPECT_GT(bpSheds, 0u);
    EXPECT_GT(bpFrac, 0.05);
    // Y la captura también suelta (su sobrecarga es real).
    EXPECT_GT(capSheds, 0u);
}

TEST(ShedGovernorSplit, SplitGovernorsIsolateBridgePlayerFromCaptureDebt) {
    // EL FIX (0031): con un gobernador por ruta, el BridgePlayer NO suelta
    // NUNCA (su coste 6 ms << presupuesto 35.1 ms) aunque la captura siga
    // sobrecargada y soltando su fracción natural.
    ivanna::dsp::RouteShedGovernors govs;
    auto route = [&govs](int64_t period) -> ivanna::dsp::DspLoadShedGovernor& {
        return govs.forPeriod(period);
    };
    uint64_t capSheds = 0, capTotal = 0, bpSheds = 0, bpTotal = 0;
    runInterleaved(route, 10'000'000, capSheds, capTotal, bpSheds, bpTotal);
    ASSERT_GT(capTotal, 1000u);
    ASSERT_GT(bpTotal, 100u);
    // BridgePlayer: cero sheds. Ni uno solo: su deuda nunca es positiva.
    EXPECT_EQ(bpSheds, 0u);
    EXPECT_EQ(govs.large().shedFraction(), 0.0);
    // Captura: sigue soltando su fracción natural (protección diseñada
    // contra el desborde del RecordThread). Coste 8 ms > presupuesto
    // 5.67 ms: ~1 de cada 3-4 bloques.
    const double capFrac =
        static_cast<double>(capSheds) / static_cast<double>(capTotal);
    EXPECT_GT(capSheds, 0u);
    EXPECT_GT(capFrac, 0.10);
    EXPECT_LT(capFrac, 0.45);
    // Cada EMA mide la DEMANDA de su propia ruta, sin mezclarse.
    EXPECT_NEAR(govs.large().emaUs(), kBridgeCostUs, 200);
    EXPECT_NEAR(govs.small().emaUs(), kCaptureCostUs, 200);
}

TEST(ShedGovernorSplit, SplitGovernorsRouteByPeriodBoundary) {
    // El umbral (20 ms) cae entre 6.67 ms (captura) y 41.3 ms (BridgePlayer).
    ivanna::dsp::RouteShedGovernors govs;
    EXPECT_EQ(&govs.forPeriod(6667), &govs.small());
    EXPECT_EQ(&govs.forPeriod(20000), &govs.small());
    EXPECT_EQ(&govs.forPeriod(20001), &govs.large());
    EXPECT_EQ(&govs.forPeriod(41333), &govs.large());
    // Y el enrutado es coherente con el presupuesto: un bloque grande
    // procesado no deja deuda en el gobernador pequeño ni viceversa.
    govs.forPeriod(41333).observeRun(6000, 41333);
    EXPECT_EQ(govs.small().totalBlocks(), 0u);
    EXPECT_EQ(govs.large().totalBlocks(), 1u);
    govs.forPeriod(6667).observeRun(8000, 6667);
    EXPECT_EQ(govs.small().totalBlocks(), 1u);
    EXPECT_EQ(govs.large().totalBlocks(), 1u);
}

TEST(ShedGovernorSplit, GenuinelyOverloadedBridgePlayerStillSheds) {
    // El fix NO desactiva el shed: si el BridgePlayer mismo supera su
    // presupuesto (p. ej. 40 ms > 35.1 ms en un core lento), su gobernador
    // sigue soltando. Solo se elimina la contaminación ENTRE rutas.
    ivanna::dsp::RouteShedGovernors govs;
    auto& large = govs.large();
    for (int i = 0; i < 50; ++i) {
        if (large.shouldShed(kBridgePeriodUs)) {
            large.observeShed(kBridgePeriodUs);
        } else {
            large.observeRun(40000, kBridgePeriodUs);
        }
    }
    EXPECT_GT(large.shedBlocks(), 0u);
    // Y la captura no se entera: su gobernador sigue en cero.
    EXPECT_EQ(govs.small().totalBlocks(), 0u);
    EXPECT_EQ(govs.small().shedBlocks(), 0u);
}

TEST(ShedGovernorSplit, ResetClearsBothGovernors) {
    ivanna::dsp::RouteShedGovernors govs;
    govs.small().observeRun(8000, kCapturePeriodUs);
    govs.small().observeRun(8000, kCapturePeriodUs);
    govs.small().observeRun(8000, kCapturePeriodUs);
    govs.large().observeRun(6000, kBridgePeriodUs);
    ASSERT_GT(govs.small().totalBlocks(), 0u);
    ASSERT_GT(govs.large().totalBlocks(), 0u);
    govs.reset();
    EXPECT_EQ(govs.small().totalBlocks(), 0u);
    EXPECT_EQ(govs.small().shedBlocks(), 0u);
    EXPECT_EQ(govs.small().debtUs(), 0);
    EXPECT_EQ(govs.large().totalBlocks(), 0u);
    EXPECT_EQ(govs.large().shedBlocks(), 0u);
    EXPECT_EQ(govs.large().debtUs(), 0);
}
