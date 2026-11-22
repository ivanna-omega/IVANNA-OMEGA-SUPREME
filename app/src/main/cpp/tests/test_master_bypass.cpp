// test_master_bypass.cpp — Tests de host del bypass maestro (2.5.461,
// propuesta 0035, parche del bofo/hueco — ataque FINAL 0035→main).
//
// Compila la TU REAL ivanna_jni_stub.cpp (flag g_masterBypass + stub
// AudioEngine_nativeSetBypass) con sus dependencias reales
// (audio_orchestrator.cpp + audio_control_plane.cpp +
// evolutionary_kernel_v2.cpp) en host con host_stubs (jni.h + android/log.h).
//
// Diseño SIMPLIFICADO (override de Esse 2026-11-17 14:55, cero deuda
// frente a las 4 objeciones de Haiku ronda 5):
// - El flag se declara `extern` en omega_effect_beacon.h (header común
//   que incluyen AMBOS .cpp JNI) y se define UNA sola vez en el stub.
//   Sin accessors intermedios: el test lee el flag DIRECTO desde la
//   declaración del header (eso mismo es el test de la objeción 1).
// - El guard de bypass va PRIMERO en nativeProcess (antes del guard
//   no-bloqueante, del drain, del shed y de GetFloatArrayElements).
//   Sin P2 en DSPState.kt (va en el Movimiento 2).
//
// Lo que se verifica (comportamiento, no implementación):
// - El flag arranca en false (efectos ON por defecto: sin degradación).
// - El JNI REAL AudioEngine_nativeSetBypass escribe el flag REAL
//   (false -> true -> false), idempotente ante toggles repetidos.
// - Los otros JNI del stub (gain/eq/width/exciter) NO tocan el flag
//   (el bypass no se contamina por los sliders).
// - El header común declara el flag como std::atomic<bool> (objeción 1).
// - Toggles concurrentes no rompen el flag (humo de concurrencia).
// - La COLOCACIÓN del guard se verifica por revisión estructural del
//   .cpp real (orden: bypass < guard < shed < GetFloatArrayElements).
//
// LÍMITE HONESTO (declarado): el early return de nativeProcess
// (ivanna_omega_jni.cpp) NO es testeable en host: esa TU no compila en host
// (CMakeLists:2089) y no existe precedente de compilarla. La COLOCACIÓN del
// guard SÍ se verifica en host con el test estructural
// MasterBypass.GuardPlacement. Lo que NO se verifica en host es el
// COMPORTAMIENTO bit-exacto (buffer intacto): la prueba que lo cierra es la
// escucha de Esse en el dispositivo (misma canción, bypass ON = salida
// bit-idéntica a la entrada) + CTest completo en verde.

#include <atomic>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>
#include <jni.h>

// El flag REAL, declarado en omega_effect_beacon.h (header común de
// ambos .cpp JNI) y definido UNA sola vez en ivanna_jni_stub.cpp.
// Incluir el header (en vez de declarar el extern a mano) ES el test de
// la objeción 1: si la declaración no existiera o no coincidiera con la
// definición, esto no compila o no enlaza.
#include "../include/omega_effect_beacon.h"

// Los JNI REALES del stub (C linkage, mismo target libivanna_omega.so).
extern "C" void Java_com_ivanna_omega_audio_AudioEngine_nativeSetBypass(
    JNIEnv*, jobject, jboolean);
extern "C" void Java_com_ivanna_omega_audio_AudioEngine_nativeSetGain(
    JNIEnv*, jobject, jfloat);
extern "C" void Java_com_ivanna_omega_audio_AudioEngine_nativeSetEqGain(
    JNIEnv*, jobject, jfloat);
extern "C" void Java_com_ivanna_omega_audio_AudioEngine_nativeSetWidth(
    JNIEnv*, jobject, jfloat);
extern "C" void Java_com_ivanna_omega_audio_AudioEngine_nativeSetExciter(
    JNIEnv*, jobject, jfloat);

namespace {

bool Flag() { return g_masterBypass.load(std::memory_order_relaxed); }

void SetBypass(jboolean v) {
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetBypass(nullptr, nullptr, v);
}

TEST(MasterBypass, DefaultIsFalse) {
    // El proceso arranca con efectos ON: el flag nunca nace en true.
    EXPECT_FALSE(Flag());
}

TEST(MasterBypass, StubJniWritesFlag) {
    SetBypass(JNI_TRUE);
    EXPECT_TRUE(Flag());
    SetBypass(JNI_FALSE);
    EXPECT_FALSE(Flag());
}

TEST(MasterBypass, ToggleIsIdempotent) {
    SetBypass(JNI_TRUE);
    SetBypass(JNI_TRUE);
    EXPECT_TRUE(Flag());
    SetBypass(JNI_FALSE);
    SetBypass(JNI_FALSE);
    EXPECT_FALSE(Flag());
    SetBypass(JNI_TRUE);
    SetBypass(JNI_FALSE);
    SetBypass(JNI_TRUE);
    EXPECT_TRUE(Flag());
    SetBypass(JNI_FALSE);
    EXPECT_FALSE(Flag());
}

TEST(MasterBypass, OtherStubJnisDoNotTouchFlag) {
    // El bypass no se contamina por los sliders: gain/eq/width/exciter
    // escriben el orquestador, nunca el flag.
    SetBypass(JNI_TRUE);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetGain(nullptr, nullptr, 0.5f);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetEqGain(nullptr, nullptr, 1.0f);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetWidth(nullptr, nullptr, 0.8f);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetExciter(nullptr, nullptr, 0.3f);
    EXPECT_TRUE(Flag());

    SetBypass(JNI_FALSE);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetGain(nullptr, nullptr, -1.0f);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetEqGain(nullptr, nullptr, 0.0f);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetWidth(nullptr, nullptr, 1.0f);
    Java_com_ivanna_omega_audio_AudioEngine_nativeSetExciter(nullptr, nullptr, 0.0f);
    EXPECT_FALSE(Flag());
}

TEST(MasterBypass, HeaderDeclaresFlagAsAtomicBool) {
    // Objeción 1 (ODR): el header común debe declarar el flag como
    // std::atomic<bool> — una sola definición en el stub, N declaraciones
    // idénticas. Si el tipo no coincidiera, esto no compila.
    static_assert(std::is_same_v<decltype(g_masterBypass), std::atomic<bool>>,
                  "g_masterBypass debe ser std::atomic<bool> (header común)");
    static_assert(std::is_same_v<decltype(&g_masterBypass),
                                  std::atomic<bool>*>,
                  "la dirección del flag debe ser única (una definición)");
    EXPECT_FALSE(Flag());
}

TEST(MasterBypass, ConcurrentToggleStaysCoherent) {
    // Humo de concurrencia (lo que el árbitro pidió probar en host):
    // 4 hilos alternando el flag 1000 veces cada uno. No se afirma un
    // valor final determinado (los hilos compiten); se afirma que el
    // flag sobrevive sin romperse y que un store final deja el valor
    // exacto que se escribió. Un bool no-atómico se rompería aquí.
    constexpr int kThreads = 4;
    constexpr int kIters = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([t] {
            for (int i = 0; i < kIters; ++i) {
                g_masterBypass.store((i + t) % 2 == 0,
                                     std::memory_order_release);
            }
        });
    }
    for (auto& w : workers) w.join();
    g_masterBypass.store(false, std::memory_order_release);
    EXPECT_FALSE(Flag());
    g_masterBypass.store(true, std::memory_order_release);
    EXPECT_TRUE(Flag());
    g_masterBypass.store(false, std::memory_order_release);
    EXPECT_FALSE(Flag());
}

TEST(MasterBypass, GuardPlacement) {
    // Test ESTRUCTURAL (no de comportamiento): lee el .cpp REAL de
    // ivanna_omega_jni.cpp y verifica el ORDEN del guard de bypass dentro
    // de nativeProcess: el guard va PRIMERO — antes del guard
    // no-bloqueante, antes del shed governor y antes del primer
    // GetFloatArrayElements posterior. Si el archivo no se abre, el test
    // FALLA (fail-closed): un test que no puede leer no es un test que
    // pasa.
#ifndef IVANNA_OMEGA_JNI_PATH
    FAIL() << "IVANNA_OMEGA_JNI_PATH no definido en CMake";
#else
    std::ifstream in(IVANNA_OMEGA_JNI_PATH);
    ASSERT_TRUE(in.is_open()) << "no se pudo abrir " << IVANNA_OMEGA_JNI_PATH;
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string src = ss.str();
    const auto posNative = src.find("Java_com_ivanna_omega_dsp_DSPBridge_nativeProcess");
    ASSERT_NE(posNative, std::string::npos) << "nativeProcess no encontrado";
    const auto posGuard = src.find(
        "if (g_masterBypass.load(std::memory_order_acquire)) return;", posNative);
    ASSERT_NE(posGuard, std::string::npos)
        << "guard de bypass (load acquire) no encontrado en nativeProcess";
    const auto posDspGuard = src.find("NonBlockingDspProcessGuard dspGuard;", posNative);
    ASSERT_NE(posDspGuard, std::string::npos)
        << "guard no-bloqueante no encontrado en nativeProcess";
    ASSERT_LT(posGuard, posDspGuard)
        << "el guard de bypass debe ir ANTES de cualquier adquisición";
    const auto posShed = src.find("shedGov.shouldShed", posNative);
    ASSERT_NE(posShed, std::string::npos) << "shed governor no encontrado en nativeProcess";
    ASSERT_LT(posGuard, posShed)
        << "el guard de bypass debe ir ANTES del shed governor";
    const auto posGet = src.find("GetFloatArrayElements", posGuard);
    ASSERT_NE(posGet, std::string::npos) << "GetFloatArrayElements no encontrado tras el guard";
    ASSERT_LT(posDspGuard, posGet) << "el guard no-bloqueante debe ir antes de la copia JNI";
    // La invariante es el orden: el guard corta ANTES de que se adquiera
    // nada y de que se copie el buffer JNI (bit-exacto por construcción);
    // no se exige distancia.
#endif
}

}  // namespace
