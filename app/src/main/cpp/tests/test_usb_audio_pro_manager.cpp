// test_usb_audio_pro_manager.cpp — Tests de host del gestor USB Pro
// (propuesta 0021, coordinador del Plan Omnipotencia; decisión del Consejo
// Supremo 2026-11-16, ronda 1 unánime: proposer gpt-oss-120b, reviewer luna,
// arbiter mini — candidato (e), tests para los 3 módulos sin tests).
//
// usb_audio_pro_manager.cpp (43 KB) era uno de los 3 módulos top-level sin
// tests (audit_2026-11-16.md §5). Es el motor USB asíncrono: endpoint
// isócrono + ASRC polifásico + feedback UAC, con JNI hacia Kotlin. Su núcleo
// algorítmico (usb_asrc_kernel / usb_asrc_clock_servo / usb_asrc_cutoff) YA
// tenía tests (test_usb_asrc_cutoff, test_usb_asrc_spectral_response,
// test_usb_asrc_clock_servo); lo que faltaba era el PROPIO gestor (.cpp).
//
// Para compilarlo en host hizo falta extender el shim JNI de host_stubs
// (jni.h): el gestor usa NewIntArray/SetIntArrayRegion (nativeGetFeedbackInfo
// y nativeGetEngineStats devuelven jintArray) y el shim solo tenía las
// variantes float. La extensión espeja el patrón float con estáticos
// thread_local separados.
//
// Lo que se verifica (comportamiento fail-closed SIN dispositivo USB):
// - Inactivo por defecto: ivanna_usb_pro_active() == false.
// - Escribir sin motor (nativeWriteFrames / ivanna_usb_pro_write) devuelve 0.
// - Telemetría sin motor: stats a cero, feedback ausente, no isócrono.
// - Arrancar con fd inválido (-1) no arranca el motor. Parar sin motor es seguro.
// - Configurar el endpoint fija el estado de feedback (observable) y el
//   centinela feedbackEpAddress<=0 lo apaga (comportamiento pre-cambio).
//
// NO se arranca el motor en estos tests (sin DAC USB real, el hilo haría
// ioctls USB sobre un fd que no es USB). El motor vivo con hardware real se
// verifica en dispositivo, no en host.
//
// Estado compartido: g_engine es un global estático; los tests corren en
// orden de definición en un solo proceso. ConfigureEndpoint cambia el estado
// global (feedback); cada test que lo necesita lo fija explícitamente antes
// de medir. El shim JNI usa un único array estático por tipo: hay que leer
// los valores de un jintArray ANTES de pedir el siguiente.
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <jni.h>

// API C del gestor (sin pasar por Kotlin).
extern "C" int ivanna_usb_pro_write(const int32_t* frames, int count);
extern "C" bool ivanna_usb_pro_active();

// JNI del gestor (declaradas aquí: no hay header público para ellas).
extern "C" JNIEXPORT void JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeStartAsyncEngine(
    JNIEnv* env, jobject thiz, jlong handle, jint fd);
extern "C" JNIEXPORT void JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeStopAsyncEngine(
    JNIEnv* env, jobject thiz, jlong handle);
extern "C" JNIEXPORT void JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeConfigureEndpoint(
    JNIEnv* env, jobject thiz, jint epAddress, jint maxPacketSize,
    jint interval, jint sampleRate, jint sourceSampleRate, jint channels,
    jint bitDepth, jint feedbackEpAddress, jint feedbackMaxPacketSize,
    jint feedbackInterval);
extern "C" JNIEXPORT jintArray JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeGetFeedbackInfo(
    JNIEnv* env, jobject thiz);
extern "C" JNIEXPORT jint JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeWriteFrames(
    JNIEnv* env, jobject thiz, jfloatArray samples, jint frames);
extern "C" JNIEXPORT jintArray JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeGetEngineStats(
    JNIEnv* env, jobject thiz);
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ivanna_omega_audio_UsbAudioProManager_nativeIsIsochronous(
    JNIEnv* env, jobject thiz);

namespace {

// Lee los 6 stats del motor en un array local (hay que copiar ANTES de
// pedir otro jintArray: el shim comparte un único buffer por tipo).
void ReadEngineStats(JNIEnv* env, jint out[6]) {
    jintArray arr = Java_com_ivanna_omega_audio_UsbAudioProManager_nativeGetEngineStats(
        env, nullptr);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(env->GetArrayLength(arr), 6);
    jint* data = env->GetIntArrayElements(arr, nullptr);
    ASSERT_NE(data, nullptr);
    std::memcpy(out, data, sizeof(jint) * 6);
    env->ReleaseIntArrayElements(arr, data, JNI_ABORT);
}

void ReadFeedbackInfo(JNIEnv* env, jint out[4]) {
    jintArray arr = Java_com_ivanna_omega_audio_UsbAudioProManager_nativeGetFeedbackInfo(
        env, nullptr);
    ASSERT_NE(arr, nullptr);
    EXPECT_EQ(env->GetArrayLength(arr), 4);
    jint* data = env->GetIntArrayElements(arr, nullptr);
    ASSERT_NE(data, nullptr);
    std::memcpy(out, data, sizeof(jint) * 4);
    env->ReleaseIntArrayElements(arr, data, JNI_ABORT);
}

TEST(UsbAudioProManagerTest, InitiallyInactive) {
    EXPECT_FALSE(ivanna_usb_pro_active());
}

TEST(UsbAudioProManagerTest, WriteFramesWhenInactiveReturnsZero) {
    JNIEnv env;
    jfloatArray samples = env.NewFloatArray(96);
    ASSERT_NE(samples, nullptr);
    EXPECT_EQ(Java_com_ivanna_omega_audio_UsbAudioProManager_nativeWriteFrames(
                  &env, nullptr, samples, 48),
              0);
    // Array nulo y 0 frames también devuelven 0 sin colgar.
    EXPECT_EQ(Java_com_ivanna_omega_audio_UsbAudioProManager_nativeWriteFrames(
                  &env, nullptr, nullptr, 48),
              0);
    EXPECT_EQ(Java_com_ivanna_omega_audio_UsbAudioProManager_nativeWriteFrames(
                  &env, nullptr, samples, 0),
              0);
}

TEST(UsbAudioProManagerTest, UsbProWriteWhenInactiveReturnsZero) {
    const int32_t frames[8] = {0};
    EXPECT_EQ(ivanna_usb_pro_write(frames, 8), 0);
    EXPECT_EQ(ivanna_usb_pro_write(nullptr, 8), 0);
}

TEST(UsbAudioProManagerTest, EngineStatsSaneWhenInactive) {
    JNIEnv env;
    jint stats[6] = {-1, -1, -1, -1, -1, -1};
    ReadEngineStats(&env, stats);
    // [0]=submitted [1]=completed [2]=errors [3]=underruns
    // [4]=framesAvailable [5]=isoMode. Sin motor, todo a cero.
    EXPECT_EQ(stats[0], 0);
    EXPECT_EQ(stats[1], 0);
    EXPECT_EQ(stats[2], 0);
    EXPECT_GE(stats[3], 0);
    EXPECT_GE(stats[4], 0);
    EXPECT_EQ(stats[5], 0);
}

TEST(UsbAudioProManagerTest, FeedbackInfoAbsentWhenInactive) {
    JNIEnv env;
    jint info[4] = {-1, -1, -1, -1};
    ReadFeedbackInfo(&env, info);
    // [0]=hasFeedback [1]=paquetes [2]=último crudo [3]=endpoint.
    EXPECT_EQ(info[0], 0);
    EXPECT_EQ(info[1], 0);
    EXPECT_EQ(info[2], 0);
}

TEST(UsbAudioProManagerTest, IsIsochronousFalseInitially) {
    JNIEnv env;
    EXPECT_EQ(Java_com_ivanna_omega_audio_UsbAudioProManager_nativeIsIsochronous(
                  &env, nullptr),
              JNI_FALSE);
}

TEST(UsbAudioProManagerTest, StartWithInvalidFdDoesNotStart) {
    JNIEnv env;
    Java_com_ivanna_omega_audio_UsbAudioProManager_nativeStartAsyncEngine(
        &env, nullptr, 12345, -1);
    EXPECT_FALSE(ivanna_usb_pro_active());
}

TEST(UsbAudioProManagerTest, StopWhenNotRunningIsSafe) {
    JNIEnv env;
    EXPECT_NO_FATAL_FAILURE(
        Java_com_ivanna_omega_audio_UsbAudioProManager_nativeStopAsyncEngine(
            &env, nullptr, 12345));
    EXPECT_FALSE(ivanna_usb_pro_active());
}

TEST(UsbAudioProManagerTest, ConfigureEndpointSetsFeedbackState) {
    JNIEnv env;
    // Con endpoint de feedback válido: hasFeedback=1 y la dirección queda.
    Java_com_ivanna_omega_audio_UsbAudioProManager_nativeConfigureEndpoint(
        &env, nullptr, 0x01, 192, 1, 48000, 48000, 2, 32, 0x81, 4, 1);
    jint info[4] = {-1, -1, -1, -1};
    ReadFeedbackInfo(&env, info);
    EXPECT_EQ(info[0], 1);
    EXPECT_EQ(info[3], 0x81);
    // Centinela feedbackEpAddress<=0 (el DAC no expone endpoint): sin feedback.
    Java_com_ivanna_omega_audio_UsbAudioProManager_nativeConfigureEndpoint(
        &env, nullptr, 0x01, 192, 1, 48000, 48000, 2, 32, -1, 0, 1);
    ReadFeedbackInfo(&env, info);
    EXPECT_EQ(info[0], 0);
}

}  // namespace
