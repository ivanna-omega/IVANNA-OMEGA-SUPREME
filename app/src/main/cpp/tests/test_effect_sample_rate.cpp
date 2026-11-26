// test_effect_sample_rate.cpp — EVOL-LIVEPATH (2026-11-26).
//
// El FusionCore nacía a 48000.0f aunque la sesión fuera a otra tasa:
// SET_CONFIG jamás llega en este HAL, así que lastConfiguredSr es el
// default 48000. La tasa REAL que ve el efecto es la cadencia de sus
// callbacks (frames / CLOCK_MONOTONIC). Este test fija el contrato de
// include/effect_sample_rate.hpp:
//
//   1. chooseEffectSampleRate: manda SET_CONFIG si llegó; si no, la tasa
//      MEDIDA del reloj de callback; si no hay medida, lastConfiguredSr
//      si es sana; si no, 48000. Medidas absurdas/NaN no se aceptan.
//   2. shouldCreateFusionCore: con SET_CONFIG se crea inmediato; sin él,
//      se espera a que el reloj valide la tasa medida o a 200 ms desde el
//      primer bloque (el arranque no se retrasa más).
//   3. MEDICIÓN (el reloj distingue tasas): alimentando el
//      CallbackClockMonitor con 441 frames cada 10 ms, la tasa observada
//      converge a 44100 Hz; con 480 frames, a 48000 Hz; con 960, a 96000.
//      En el Moto G85 el hilo del mixer es 48 kHz: la tasa medida es
//      48000 y el núcleo nace igual que hoy (sin cambio de sonido).
//      Una fuente a 44.1 kHz no cambia nada: AudioFlinger la remuestrea
//      a la tasa del mixer ANTES del efecto.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>

#include "../include/audio_callback_clock.hpp"
#include "../include/effect_sample_rate.hpp"

using ivanna::audio::CallbackClockMonitor;
using ivanna::chooseEffectSampleRate;
using ivanna::kEffectCoreCreateWaitMs;
using ivanna::shouldCreateFusionCore;

TEST(EffectSampleRate, SetConfigWinsOverMeasuredClock) {
    // El HAL dijo 96000 por SET_CONFIG: manda aunque el reloj mida 44100.
    EXPECT_EQ(chooseEffectSampleRate(96000, true, true, 44100.0), 96000u);
    EXPECT_EQ(chooseEffectSampleRate(96000, true, false, 0.0), 96000u);
}

TEST(EffectSampleRate, MeasuredClockWinsWithoutSetConfig) {
    // Sin SET_CONFIG, la tasa medida (44.1k) manda sobre el default 48000.
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, 44100.0), 44100u);
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, 96000.0), 96000u);
    // Redondeo: 44099.6 -> 44100.
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, 44099.6), 44100u);
}

TEST(EffectSampleRate, FallsBackToConfiguredThenDefault) {
    // Sin medida: lastConfiguredSr si es sana...
    EXPECT_EQ(chooseEffectSampleRate(48000, false, false, 0.0), 48000u);
    EXPECT_EQ(chooseEffectSampleRate(96000, false, false, 0.0), 96000u);
    // ...si no, 48000 (lastConfiguredSr 0 o absurdo no se propaga).
    EXPECT_EQ(chooseEffectSampleRate(0, false, false, 0.0), 48000u);
    EXPECT_EQ(chooseEffectSampleRate(100, false, false, 0.0), 48000u);
    EXPECT_EQ(chooseEffectSampleRate(2000000, false, false, 0.0), 48000u);
}

TEST(EffectSampleRate, AbsurdMeasuredRatesRejected) {
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, 100.0), 48000u);
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, 2000000.0), 48000u);
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, NAN), 48000u);
    EXPECT_EQ(chooseEffectSampleRate(48000, false, true, INFINITY), 48000u);
}

TEST(EffectSampleRate, CreateImmediatelyWithSetConfigOrClock) {
    EXPECT_TRUE(shouldCreateFusionCore(true, false, 1000, 0));
    EXPECT_TRUE(shouldCreateFusionCore(false, true, 1000, 1000));
    // Sin SET_CONFIG ni reloj: no se crea en el primer bloque.
    EXPECT_FALSE(shouldCreateFusionCore(false, false, 1000, 1000));
    EXPECT_FALSE(shouldCreateFusionCore(false, false, 1000, 0));
}

TEST(EffectSampleRate, CreateAfterWaitWithoutClock) {
    EXPECT_EQ(kEffectCoreCreateWaitMs, 200u);
    // 199 ms no bastan; a los 200 ms se crea con el default.
    EXPECT_FALSE(shouldCreateFusionCore(false, false, 1199, 1000));
    EXPECT_TRUE(shouldCreateFusionCore(false, false, 1200, 1000));
    EXPECT_TRUE(shouldCreateFusionCore(false, false, 5000, 1000));
}

namespace {

// Alimenta el reloj con bloques de `framesPerBlock` cada 10 ms durante
// `blocks` bloques y devuelve la última tasa observada válida (0 si
// nunca validó). El monitor ancla en el primer observe() y valida a
// partir de 100 ms de ventana.
double observeRate(uint32_t framesPerBlock, int blocks, bool& validOut) {
    CallbackClockMonitor clock;
    uint64_t nowNs = 1'000'000'000ULL;
    double lastRate = 0.0;
    validOut = false;
    for (int i = 0; i < blocks; ++i) {
        const auto st = clock.observe(nowNs, framesPerBlock, 48000);
        if (st.valid) {
            lastRate = st.observedSampleRateHz;
            validOut = true;
        }
        nowNs += 10'000'000ULL;  // 10 ms por bloque
    }
    return lastRate;
}

}  // namespace

TEST(EffectSampleRate, CallbackClockMeasures44100) {
    bool valid = false;
    const double rate = observeRate(441, 30, valid);
    ASSERT_TRUE(valid);
    EXPECT_NEAR(rate, 44100.0, 1.0);
}

TEST(EffectSampleRate, CallbackClockMeasures48000) {
    bool valid = false;
    const double rate = observeRate(480, 30, valid);
    ASSERT_TRUE(valid);
    EXPECT_NEAR(rate, 48000.0, 1.0);
}

TEST(EffectSampleRate, CallbackClockMeasures96000) {
    bool valid = false;
    const double rate = observeRate(960, 30, valid);
    ASSERT_TRUE(valid);
    EXPECT_NEAR(rate, 96000.0, 1.0);
}

TEST(EffectSampleRate, CallbackClockNeeds100msWindow) {
    // 5 bloques (50 ms): la ventana es corta, el reloj no valida nada.
    bool valid = false;
    const double rate = observeRate(480, 5, valid);
    EXPECT_FALSE(valid);
    EXPECT_EQ(rate, 0.0);
}
