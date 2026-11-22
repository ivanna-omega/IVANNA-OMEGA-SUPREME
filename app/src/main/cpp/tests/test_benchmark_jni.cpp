// test_benchmark_jni.cpp — Tests de host del benchmark JNI de latencia
// (propuesta 0016, coordinador del Plan Omnipotencia).
//
// ivanna_benchmark_jni.cpp (72 líneas) estaba entre los 19 módulos sin
// tests: una sola función JNI, nativeMeasureRoundTripLatencyUs, que mide
// clock_gettime(CLOCK_MONOTONIC) alrededor de la cadena DSP real de la
// Ruta A in-process (ParametricEQ → Compressor → HarmonicExciter →
// StereoWidener → GainStage → SafetyLimiter) con un pulso unitario.
// Es la métrica honesta de la Ruta A: mide el código que el usuario
// oye cuando el motor corre in-process.
//
// Lo que se verifica:
// - La función corre en host con el stub JNI (JNIEnv{} vacío) y no
//   crashea.
// - Devuelve una latencia positiva y razonable: (0, 10000] µs para
//   256 frames. Medido en host 2026-11-16: ~94–120 µs. El umbral
//   10000 µs es 100× el valor medido: es un guard de cordura, no un
//   benchmark de rendimiento (un test de timing ajustado fallaría bajo
//   ctest -j4 por contención de CPU — lección de test_route_a_asrc).
// - Dos llamadas consecutivas devuelven valores en rango (la cadena
//   corre de forma repetible, sin estado corrupto entre llamadas).
#include <gtest/gtest.h>
#include <jni.h>

extern "C" {
JNIEXPORT jlong JNICALL
Java_com_ivanna_omega_core_IvannaNativeLib_nativeMeasureRoundTripLatencyUs(
    JNIEnv*, jclass);
}

TEST(BenchmarkJniTest, RoundTripLatencyIsPositiveAndSane) {
    JNIEnv env{};
    const jlong us =
        Java_com_ivanna_omega_core_IvannaNativeLib_nativeMeasureRoundTripLatencyUs(
            &env, nullptr);
    // 256 frames por 6 módulos DSP: ~100 µs en host. El umbral 10 ms
    // es 100× el valor medido (guard de cordura, no benchmark).
    EXPECT_GT(us, 0);
    EXPECT_LE(us, 10000);
}

TEST(BenchmarkJniTest, RepeatedCallsAreRepeatable) {
    JNIEnv env{};
    const jlong us1 =
        Java_com_ivanna_omega_core_IvannaNativeLib_nativeMeasureRoundTripLatencyUs(
            &env, nullptr);
    const jlong us2 =
        Java_com_ivanna_omega_core_IvannaNativeLib_nativeMeasureRoundTripLatencyUs(
            &env, nullptr);
    EXPECT_GT(us1, 0);
    EXPECT_LE(us1, 10000);
    EXPECT_GT(us2, 0);
    EXPECT_LE(us2, 10000);
}
