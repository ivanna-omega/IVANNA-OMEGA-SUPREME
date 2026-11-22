// test_saf_jni_bridge.cpp — Tests de host del bridge JNI SAF
// (propuesta 0017, coordinador del Plan Omnipotencia).
//
// SaFJniBridge.cpp (130 líneas) estaba entre los 19 módulos sin tests:
// bridge JNI de SaFBridge (Kotlin) ↔ SaFOptimizer. El test usa el stub
// JNI de host (tests/host_stubs/jni.h, que implementa NewFloatArray /
// SetFloatArrayRegion / GetStringUTFChars) y un stub de
// ivanna_saf_apply_latent que captura el vector q propagado al renderer
// (el cable SAF → ObjectRenderer: sin él, el optimizador convergía pero
// el audio nunca cambiaba).
//
// Lo que se verifica (comportamiento, no implementación):
// - nativeSaFReset pone q=0, iteración=0 y PROPAGA q al renderer (el
//   stub registra la llamada con q=0).
// - nativeSaFFeedback cambia q (gradiente) y PROPAGA el q actualizado.
// - nativeSaFGetStatus devuelve 8 floats: q[0..6] + flag modelo (0 sin
//   init); nativeSaFGetParams devuelve los mismos 7 q.
// - nativeSaFGetIteration crece con el feedback; nativeSaFGetError y
//   nativeSaFIsConverged devuelven valores finitos / booleanos sanos.
// - nativeSaFInit con ruta inexistente devuelve false (sin crash) y el
//   flag de modelo sigue en 0.
//
// Nota de orden: el optimizador es un singleton global (getGlobalSaF).
// Cada test que necesita un estado conocido llama nativeSaFReset primero.
#include <cmath>
#include <cstring>
#include <gtest/gtest.h>
#include <jni.h>

namespace {
// Stub de ivanna_saf_apply_latent (definida en omega_effect.cpp en
// producción): captura el último q propagado y cuenta las llamadas.
float g_lastLatent[7] = {};
int g_applyCount = 0;
}  // namespace

extern "C" void ivanna_saf_apply_latent(const float q[7]) {
    std::memcpy(g_lastLatent, q, sizeof(g_lastLatent));
    ++g_applyCount;
}

extern "C" {
JNIEXPORT jboolean JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFInit(JNIEnv*, jobject, jstring);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFFeedback(JNIEnv*, jobject, jint, jboolean);
JNIEXPORT jfloatArray JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetStatus(JNIEnv*, jobject);
JNIEXPORT jfloatArray JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetParams(JNIEnv*, jobject);
JNIEXPORT jint JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetIteration(JNIEnv*, jobject);
JNIEXPORT void JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(JNIEnv*, jobject);
JNIEXPORT jboolean JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFIsConverged(JNIEnv*, jobject);
JNIEXPORT jfloat JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetError(JNIEnv*, jobject);
JNIEXPORT jboolean JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFSaveState(JNIEnv*, jobject, jstring);
JNIEXPORT jboolean JNICALL
Java_com_ivanna_omega_saf_SaFBridge_nativeSaFLoadState(JNIEnv*, jobject, jstring);
}

namespace {

void ReadParams(JNIEnv* env, float out[7]) {
    jfloatArray arr =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetParams(env, nullptr);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(env->GetArrayLength(arr), 7);
    jfloat* data = env->GetFloatArrayElements(arr, nullptr);
    ASSERT_NE(data, nullptr);
    std::memcpy(out, data, sizeof(float) * 7);
}

bool AllFinite(const float* v, int n) {
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(v[i])) return false;
    }
    return true;
}

}  // namespace

TEST(SaFJniBridgeTest, ResetPropagatesZeroLatent) {
    JNIEnv env{};
    g_applyCount = 0;
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(&env, nullptr);
    EXPECT_GE(g_applyCount, 1);
    for (int i = 0; i < 7; ++i) {
        EXPECT_FLOAT_EQ(g_lastLatent[i], 0.0f);
    }
    EXPECT_EQ(
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetIteration(&env, nullptr), 0);
}

TEST(SaFJniBridgeTest, FeedbackChangesParamsAndPropagates) {
    JNIEnv env{};
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(&env, nullptr);
    g_applyCount = 0;
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFFeedback(&env, nullptr, 0, JNI_FALSE);
    EXPECT_GE(g_applyCount, 1);
    float q[7];
    ReadParams(&env, q);
    EXPECT_TRUE(AllFinite(q, 7));
    bool changed = false;
    for (int i = 0; i < 7; ++i) {
        if (q[i] != 0.0f) changed = true;
        EXPECT_FLOAT_EQ(g_lastLatent[i], q[i]);
    }
    EXPECT_TRUE(changed);
    EXPECT_GT(
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetIteration(&env, nullptr), 0);
}

TEST(SaFJniBridgeTest, StatusMatchesParamsPlusModelFlag) {
    JNIEnv env{};
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(&env, nullptr);
    jfloatArray st =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetStatus(&env, nullptr);
    ASSERT_NE(st, nullptr);
    EXPECT_EQ(env.GetArrayLength(st), 8);
    jfloat* data = env.GetFloatArrayElements(st, nullptr);
    ASSERT_NE(data, nullptr);
    EXPECT_TRUE(AllFinite(data, 8));
    EXPECT_FLOAT_EQ(data[7], 0.0f);  // sin init: modelo no cargado
    float q[7];
    ReadParams(&env, q);
    for (int i = 0; i < 7; ++i) {
        EXPECT_FLOAT_EQ(data[i], q[i]);
    }
}

TEST(SaFJniBridgeTest, InitWithBadPathReturnsFalse) {
    JNIEnv env{};
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(&env, nullptr);
    jstring bad = env.NewStringUTF("/no/existe/SAF_model.json");
    const jboolean ok =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFInit(&env, nullptr, bad);
    EXPECT_EQ(ok, JNI_FALSE);
    jfloatArray st =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetStatus(&env, nullptr);
    ASSERT_NE(st, nullptr);
    jfloat* data = env.GetFloatArrayElements(st, nullptr);
    ASSERT_NE(data, nullptr);
    EXPECT_FLOAT_EQ(data[7], 0.0f);  // modelo sigue sin cargar
}

TEST(SaFJniBridgeTest, ErrorAndConvergedAreSane) {
    JNIEnv env{};
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(&env, nullptr);
    const float err =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetError(&env, nullptr);
    EXPECT_TRUE(std::isfinite(err));
    EXPECT_GE(err, 0.0f);
    const jboolean conv =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFIsConverged(&env, nullptr);
    EXPECT_TRUE(conv == JNI_TRUE || conv == JNI_FALSE);
}

TEST(SaFJniBridgeTest, InvalidFeedbackDirectionIsIgnored) {
    JNIEnv env{};
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFReset(&env, nullptr);
    const int iterBefore =
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetIteration(&env, nullptr);
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFFeedback(&env, nullptr, -1, JNI_TRUE);
    Java_com_ivanna_omega_saf_SaFBridge_nativeSaFFeedback(&env, nullptr, 99, JNI_TRUE);
    EXPECT_EQ(
        Java_com_ivanna_omega_saf_SaFBridge_nativeSaFGetIteration(&env, nullptr),
        iterBefore);
}
