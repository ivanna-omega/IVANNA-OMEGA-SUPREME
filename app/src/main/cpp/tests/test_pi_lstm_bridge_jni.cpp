// test_pi_lstm_bridge_jni.cpp — Tests de host del bridge JNI Pi-LSTM
// (propuesta 0015, coordinador del Plan Omnipotencia).
//
// pi_lstm_bridge_jni.cpp (278 líneas) estaba entre los 19 módulos sin
// tests: es el bridge JNI de PiLstmBridge (Kotlin) con su propia
// instancia PILSTMMilenioEngine. El test usa el stub JNI de host
// (tests/host_stubs/jni.h) y llama las funciones JNI directamente con
// un JNIEnv{} vacío, patrón de test_phase2_jni_natives.
//
// Lo que se verifica (comportamiento, no implementación):
// - Antes de nativeInit los getters devuelven 0 y los setters son no-op
//   (el engine no arranca corrupto por un set previo al init).
// - Tras nativeInit + tick, la telemetría (np_sat, error) es finita y
//   está en [0, 1].
// - nativeSetNPMax fluye al engine: con NP_max=0.1 la saturación es
//   mayor que con NP_max=10 (mismo input, mismo reset) — el setter
//   llega al LSTM y no se queda en el mailbox.
// - Los setters clampan NaN / fuera de rango (alpha NaN, beta 100,
//   eta -5, np_max 0, harmonic_gain 5): tras el tick la telemetría
//   sigue finita y en rango, sin corrupción.
// - Tick con punteros nulos o 0 frames es no-op (no crashea).
// - nativeResetTelemetry no crashea y la telemetría sigue finita.
//
// Nota de orden: los tests comparten el estado global del bridge
// (static). gtest corre en orden de definición; el test NotReady va
// primero (antes de cualquier nativeInit) y cada test que necesita un
// estado limpio llama nativeInit (que pide reset) antes de tickear.
#include <cmath>
#include <gtest/gtest.h>
#include <jni.h>

extern "C" {
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(JNIEnv*, jobject);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetAlpha(JNIEnv*, jobject, jfloat);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetBeta(JNIEnv*, jobject, jfloat);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetGamma(JNIEnv*, jobject, jfloat);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetEta(JNIEnv*, jobject, jfloat);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetNPMax(JNIEnv*, jobject, jfloat);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetHarmonicGain(JNIEnv*, jobject, jfloat);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetHrtfEnabled(JNIEnv*, jobject, jboolean);
JNIEXPORT jfloat JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(JNIEnv*, jobject);
JNIEXPORT jfloat JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetError(JNIEnv*, jobject);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeResetTelemetry(JNIEnv*, jobject);

void ivanna_pilstm_bridge_tick_block(const float* inL, const float* inR,
                                     int frames, float sampleRate) noexcept;
}

namespace {

constexpr float kSr = 48000.0f;
constexpr int kFrames = 256;

void FillSine(float* l, float* r, int n, float freq = 440.0f) {
    for (int i = 0; i < n; ++i) {
        const float ph = 2.0f * 3.14159265f * freq * static_cast<float>(i) / kSr;
        l[i] = 0.5f * std::sin(ph);
        r[i] = 0.5f * std::cos(ph);
    }
}

void TickSine(JNIEnv* env, int blocks = 4) {
    float l[kFrames], r[kFrames];
    FillSine(l, r, kFrames);
    for (int b = 0; b < blocks; ++b) {
        ivanna_pilstm_bridge_tick_block(l, r, kFrames, kSr);
    }
    (void)env;
}

}  // namespace

TEST(PiLstmBridgeJniTest, NotReadyGettersReturnZero) {
    // Debe correr ANTES de cualquier nativeInit (orden de definición).
    JNIEnv env{};
    EXPECT_FLOAT_EQ(
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr),
        0.0f);
    EXPECT_FLOAT_EQ(
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetError(&env, nullptr),
        0.0f);
    // Setters antes de init: no-op, no crashean.
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetAlpha(&env, nullptr, 5.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetNPMax(&env, nullptr, 0.1f);
}

TEST(PiLstmBridgeJniTest, InitAndTickPublishesFiniteTelemetry) {
    JNIEnv env{};
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(&env, nullptr);
    TickSine(&env);
    const float npSat =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    const float err =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetError(&env, nullptr);
    EXPECT_TRUE(std::isfinite(npSat));
    EXPECT_TRUE(std::isfinite(err));
    EXPECT_GE(npSat, 0.0f);
    EXPECT_LE(npSat, 1.0f);
    EXPECT_GE(err, 0.0f);
    EXPECT_LE(err, 1.0f);
}

TEST(PiLstmBridgeJniTest, NpMaxSetterFlowsToEngine) {
    JNIEnv env{};
    // NP_max pequeño -> h se clamp a ±0.1 y np_sat = |h|/0.1 satura alto.
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(&env, nullptr);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetNPMax(&env, nullptr, 0.1f);
    TickSine(&env);
    const float satSmall =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    // NP_max grande -> |h|/10 queda bajo.
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(&env, nullptr);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetNPMax(&env, nullptr, 10.0f);
    TickSine(&env);
    const float satLarge =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    EXPECT_TRUE(std::isfinite(satSmall));
    EXPECT_TRUE(std::isfinite(satLarge));
    EXPECT_GT(satSmall, satLarge);
}

TEST(PiLstmBridgeJniTest, SettersClampNaNAndOutOfRange) {
    JNIEnv env{};
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(&env, nullptr);
    const float nan = std::nanf("");
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetAlpha(&env, nullptr, nan);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetBeta(&env, nullptr, 100.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetGamma(&env, nullptr, -3.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetEta(&env, nullptr, -5.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetNPMax(&env, nullptr, 0.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetHarmonicGain(&env, nullptr, 5.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetHrtfEnabled(&env, nullptr, JNI_FALSE);
    TickSine(&env);
    const float npSat =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    const float err =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetError(&env, nullptr);
    EXPECT_TRUE(std::isfinite(npSat));
    EXPECT_TRUE(std::isfinite(err));
    EXPECT_GE(npSat, 0.0f);
    EXPECT_LE(npSat, 1.0f);
    EXPECT_GE(err, 0.0f);
    EXPECT_LE(err, 1.0f);
    // Restaurar defaults para los tests que siguen.
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetAlpha(&env, nullptr, 1.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetBeta(&env, nullptr, 1.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetGamma(&env, nullptr, 1.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetEta(&env, nullptr, 1.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetNPMax(&env, nullptr, 1.0f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetHarmonicGain(&env, nullptr, 0.3f);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeSetHrtfEnabled(&env, nullptr, JNI_TRUE);
}

TEST(PiLstmBridgeJniTest, NullAndZeroTickAreNoOps) {
    JNIEnv env{};
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(&env, nullptr);
    TickSine(&env);
    const float before =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    ivanna_pilstm_bridge_tick_block(nullptr, nullptr, kFrames, kSr);
    ivanna_pilstm_bridge_tick_block(nullptr, nullptr, 0, kSr);
    float l[kFrames], r[kFrames];
    FillSine(l, r, kFrames);
    ivanna_pilstm_bridge_tick_block(l, r, 0, kSr);
    ivanna_pilstm_bridge_tick_block(l, r, -4, kSr);
    const float after =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    EXPECT_TRUE(std::isfinite(after));
    EXPECT_FLOAT_EQ(before, after);
}

TEST(PiLstmBridgeJniTest, ResetTelemetryKeepsTelemetryFinite) {
    JNIEnv env{};
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeInit(&env, nullptr);
    TickSine(&env, 8);
    Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeResetTelemetry(&env, nullptr);
    TickSine(&env);
    const float npSat =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetNpSat(&env, nullptr);
    const float err =
        Java_com_ivanna_omega_neuromorphic_PiLstmBridge_nativeGetError(&env, nullptr);
    EXPECT_TRUE(std::isfinite(npSat));
    EXPECT_TRUE(std::isfinite(err));
    EXPECT_GE(npSat, 0.0f);
    EXPECT_LE(npSat, 1.0f);
    EXPECT_GE(err, 0.0f);
    EXPECT_LE(err, 1.0f);
}
