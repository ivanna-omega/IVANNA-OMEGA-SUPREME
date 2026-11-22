// test_bridgeplayer_lufs_trim.cpp
//
// FIX 5 (v2.5.465) — Reproductor IVANNA tronando + volumen sube/baja (Ruta A).
//
// CAUSA CONCRETA (verificada en código, perceptual_loudness.hpp +
// ivanna_omega_jni.cpp nativeProcess, vía DSPBridge.process del
// BridgePlayer):
//   El trim LUFS automático se movía hasta 0.1 dB POR BLOQUE sin zona
//   muerta, sobre LUFS momentáneo (EMA 400 ms, sin gating), y se aplicaba
//   con un suavizado rápido (0.001/muestra ≈ 10–21 ms) en serie con el
//   runtimeMul del ADE (target_gain 0.5–1.0), el compresor multibanda y
//   el SafetyLimiter. En el BridgePlayer (bloques de 2048 frames) eso es
//   hasta ~4.7 dB/s a 96 kHz / ~2.3 dB/s a 48 kHz de ganancia persiguiendo
//   la dinámica propia de la música: estrofa → estribillo → el trim baja;
//   intro queda → el trim sube. Ese es el "volumen sube y baja" con el
//   módulo desinstalado (Ruta A pura, sin Ruta B).
//
// FIX (perceptual_loudness.hpp::update_trim, sin tocar HolosOperator ni
// los 5 archivos Fase B):
//   - Paso máximo 0.5 dB POR SEGUNDO de audio (no por bloque): la
//     velocidad de corrección ya no depende del tamaño de bloque ni de
//     la tasa (captura 320f/bloque = 150 bloques/s, reproductor
//     2048f/bloque = 23 bloques/s).
//   - Zona muerta ±0.5 dB alrededor del target (por debajo del JND
//     musical: el trim no persigue el residuo del EMA).
//   - Congelado en silencio (LUFS ≤ -70, puerta absoluta BS.1770):
//     sin esto el silencio entre temas lo empujaba al tope y el tema
//     siguiente arrancaba 6 dB bajo.
//   - Tope total ±6 dB (antes ±12 dB).
//   Ganancia neta en régimen: SIN CAMBIO (mismo target -14 LUFS;
//   Matched-LUFS 0.0 dB) — solo deja de cabalgar la música dentro del
//   tema. Estos tests fijan la dinámica nueva con señales sintéticas
//   (seno 1 kHz); el LUFS absoluto no se predice: se lee del propio
//   medidor tras asentarse y se comprueba el trim relativo a él.

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "../perceptual_loudness.hpp"

namespace {

constexpr float kSampleRateHz = 48000.0f;
constexpr int kBlockFrames = 2048;
// Paso máximo por llamada a 2048 frames @ 48 kHz: 0.5 dB/s * 2048/48000.
constexpr float kMaxStepPerCall =
    ivanna::LoudnessMeter::kTrimMaxSlewDbPerSec * kBlockFrames / kSampleRateHz;

// Seno 1 kHz estéreo, amplitud `amp`, `seconds` segundos, medido en
// bloques de kBlockFrames. Devuelve el LUFS medido tras asentarse.
float settleSine(ivanna::LoudnessMeter& meter, float amp, float seconds) {
    const int totalFrames = static_cast<int>(seconds * kSampleRateHz);
    std::vector<float> left(kBlockFrames), right(kBlockFrames);
    int done = 0;
    float lufs = -70.0f;
    while (done < totalFrames) {
        const int n = std::min(kBlockFrames, totalFrames - done);
        for (int i = 0; i < n; ++i) {
            const float ph = 2.0f * static_cast<float>(M_PI) * 1000.0f *
                             static_cast<float>(done + i) / kSampleRateHz;
            left[i] = amp * std::sin(ph);
            right[i] = amp * std::sin(ph);
        }
        lufs = meter.measure_block(left.data(), right.data(), n);
        done += n;
    }
    return lufs;
}

// Silencio (ceros) medido en bloques; devuelve el LUFS resultante.
float settleSilence(ivanna::LoudnessMeter& meter, float seconds) {
    const int totalFrames = static_cast<int>(seconds * kSampleRateHz);
    std::vector<float> zeros(kBlockFrames, 0.0f);
    int done = 0;
    float lufs = -70.0f;
    while (done < totalFrames) {
        const int n = std::min(kBlockFrames, totalFrames - done);
        lufs = meter.measure_block(zeros.data(), zeros.data(), n);
        done += n;
    }
    return lufs;
}

}  // namespace

// ── Zona muerta: error ≤ ±0.5 dB no mueve el trim ───────────────────────────
TEST(BridgePlayerLufsTrim, DeadbandHoldsTrimAtZero) {
    ivanna::LoudnessMeter meter;
    meter.init(kSampleRateHz);
    const float lufs = settleSine(meter, 0.5f, 3.0f);
    // Target dentro de la zona muerta por ambos lados: 50 llamadas no
    // deben mover el trim ni un ápice (antes se movía 0.05 dB/bloque
    // persiguiendo el residuo del EMA).
    for (int i = 0; i < 50; ++i) {
        EXPECT_FLOAT_EQ(meter.update_trim(lufs + 0.2f, kBlockFrames, kSampleRateHz), 0.0f);
        EXPECT_FLOAT_EQ(meter.update_trim(lufs - 0.2f, kBlockFrames, kSampleRateHz), 0.0f);
    }
    // Target exacto: tampoco se mueve (ganancia neta 0.0 dB en régimen).
    for (int i = 0; i < 50; ++i) {
        EXPECT_FLOAT_EQ(meter.update_trim(lufs, kBlockFrames, kSampleRateHz), 0.0f);
    }
}

// ── Pendiente acotada por segundo, no por bloque ────────────────────────────
// Con un error de -10 dB (señal 10 dB por encima del target), la ley vieja
// daba 0.1 dB/bloque = 2.3 dB/s a 48 kHz (4.7 dB/s a 96 kHz). La nueva da
// como mucho 0.5 dB/s: 100 bloques (4.27 s) mueven ~2.13 dB, no 10 dB.
TEST(BridgePlayerLufsTrim, SlewBoundedAtHalfDbPerSecond) {
    ivanna::LoudnessMeter meter;
    meter.init(kSampleRateHz);
    const float lufs = settleSine(meter, 0.5f, 3.0f);
    const float target = lufs - 10.0f;  // señal muy por encima del target
    float prev = 0.0f;
    for (int i = 0; i < 100; ++i) {
        const float trim = meter.update_trim(target, kBlockFrames, kSampleRateHz);
        EXPECT_LE(prev - trim, kMaxStepPerCall + 1e-6f)
            << "el trim no debe bajar más de 0.5 dB/s por llamada";
        EXPECT_LT(trim, prev + 1e-6f) << "con señal alta el trim debe bajar";
        prev = trim;
    }
    // 100 bloques * 0.0213 dB/bloque ≈ -2.13 dB (corrección lenta, no riding).
    EXPECT_NEAR(prev, -100.0f * kMaxStepPerCall, 0.05f);
}

// El mismo límite vale para bloques pequeños (captura, 320 frames): el
// paso por llamada escala con la duración del bloque, así que 150
// bloques/s ya no dan 15 dB/s.
TEST(BridgePlayerLufsTrim, SlewScalesWithBlockDuration) {
    ivanna::LoudnessMeter meter;
    meter.init(kSampleRateHz);
    const float lufs = settleSine(meter, 0.5f, 3.0f);
    const float target = lufs - 10.0f;
    const float trim = meter.update_trim(target, 320, kSampleRateHz);
    const float maxStep320 = ivanna::LoudnessMeter::kTrimMaxSlewDbPerSec * 320.0f / kSampleRateHz;
    EXPECT_LE(-trim, maxStep320 + 1e-6f);
    EXPECT_GT(-trim, 0.0f);
}

// ── Silencio: el trim se congela ────────────────────────────────────────────
// Sin el freeze, el silencio entre temas (LUFS ≈ -90) empujaba el trim
// al tope y el tema siguiente arrancaba 6 dB bajo.
TEST(BridgePlayerLufsTrim, SilenceFreezesTrim) {
    ivanna::LoudnessMeter meter;
    meter.init(kSampleRateHz);
    const float lufs = settleSilence(meter, 3.0f);
    EXPECT_LE(lufs, -70.0f) << "el silencio debe medir bajo la puerta absoluta";
    for (int i = 0; i < 100; ++i) {
        EXPECT_FLOAT_EQ(meter.update_trim(-14.0f, kBlockFrames, kSampleRateHz), 0.0f);
    }
}

// ── Tope total ±6 dB ────────────────────────────────────────────────────────
TEST(BridgePlayerLufsTrim, TotalTrimClampedAtSixDb) {
    ivanna::LoudnessMeter meter;
    meter.init(kSampleRateHz);
    const float lufs = settleSine(meter, 0.5f, 3.0f);
    float trim = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        trim = meter.update_trim(lufs - 30.0f, kBlockFrames, kSampleRateHz);
    }
    EXPECT_FLOAT_EQ(trim, -6.0f);
    // Y por arriba igual.
    ivanna::LoudnessMeter meter2;
    meter2.init(kSampleRateHz);
    const float lufs2 = settleSine(meter2, 0.05f, 3.0f);
    float trim2 = 0.0f;
    for (int i = 0; i < 2000; ++i) {
        trim2 = meter2.update_trim(lufs2 + 30.0f, kBlockFrames, kSampleRateHz);
    }
    EXPECT_FLOAT_EQ(trim2, 6.0f);
}

// ── Convergencia lenta al target (régimen sin cambio de ganancia neta) ─────
// Lazo cerrado como en vivo: el trim se aplica a la señal que vuelve a
// medirse (measure_block tras cada update_trim, con la ganancia del trim).
// Con error sostenido de -3 dB el trim converge hacia -3 dB a ≤0.5 dB/s:
// la ley vieja llegaba en ~0.7 s cabalgando la música; la nueva tarda
// ~10 s y se queda quieta al llegar (error dentro de la zona muerta de
// ±0.5 dB: el trim final queda a ≤0.5 dB del objetivo, inaudible, y
// ya no se mueve — esa es la zona muerta trabajando, no un error).
TEST(BridgePlayerLufsTrim, ConvergesSlowlyToSustainedError) {
    ivanna::LoudnessMeter meter;
    meter.init(kSampleRateHz);
    const float lufs0 = settleSine(meter, 0.5f, 3.0f);
    const float target = lufs0 - 3.0f;
    std::vector<float> left(kBlockFrames), right(kBlockFrames);
    float trim = 0.0f;
    for (int b = 0; b < 400; ++b) {
        const float g = meter.trimLinear();
        for (int i = 0; i < kBlockFrames; ++i) {
            const float ph = 2.0f * static_cast<float>(M_PI) * 1000.0f *
                             static_cast<float>(b * kBlockFrames + i) / kSampleRateHz;
            left[i] = 0.5f * g * std::sin(ph);
            right[i] = 0.5f * g * std::sin(ph);
        }
        meter.measure_block(left.data(), right.data(), kBlockFrames);
        trim = meter.update_trim(target, kBlockFrames, kSampleRateHz);
    }
    EXPECT_NEAR(trim, -3.0f, 0.5f);
    // Y una vez en el target (error ≈ 0, dentro de la zona muerta), se queda.
    const float frozen = trim;
    for (int i = 0; i < 50; ++i) {
        EXPECT_FLOAT_EQ(meter.update_trim(target, kBlockFrames, kSampleRateHz), frozen);
    }
}
