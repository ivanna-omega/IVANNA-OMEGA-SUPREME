// ivanna_sinc_downsampler_jni.cpp — Puente JNI del downsampler sinc (Fase 4).
//
// Clase Kotlin: com.ivanna.omega.audio.NativeSincDownsampler (una instancia
// por reproducción). Vive en libivanna_omega.so (target `ivanna_omega`).
//
// El nativo (PolyphaseSincDownsampler) trabaja con canales L/R separados;
// el puente convierte desde/a PCM estéreo intercalado (el formato del
// reproductor). La conversión usa scratch reutilizable (solo crece).
//
// Tiempo real: process() usa GetPrimitiveArrayCritical (cero copias, sin
// interacción con el GC) y solo hace cómputo puro dentro de la sección
// crítica (sin llamadas JNI, sin bloqueos, sin allocs tras el warmup).
//
// Fail-closed: si el handle es 0 o el nativo no está, las llamadas no hacen
// nada (el lado Kotlin detecta isAvailable=false y usa el remuestreador
// Kotlin como fallback; el audio nunca se rompe).
#include <jni.h>

#include <cstdint>
#include <new>      // std::nothrow
#include <vector>

#include "include/polyphase_sinc_downsampler.hpp"

using ivanna::PolyphaseSincDownsampler;

namespace {

PolyphaseSincDownsampler* fromHandle(jlong handle) {
    if (handle == 0) return nullptr;
    return reinterpret_cast<PolyphaseSincDownsampler*>(static_cast<uintptr_t>(handle));
}

// Scratch por hilo de llamada (el reproductor llama desde un solo hilo de
// decodificación; el scratch solo crece, nunca se libera en caliente).
thread_local std::vector<float> g_scratchL;
thread_local std::vector<float> g_scratchR;

bool ensureScratch(int frames) {
    if (frames <= 0) return false;
    if (static_cast<int>(g_scratchL.size()) < frames) {
        g_scratchL.resize(static_cast<size_t>(frames));
        g_scratchR.resize(static_cast<size_t>(frames));
    }
    return true;
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeCreate(JNIEnv* env, jobject /*thiz*/) {
    (void)env;
    auto* ds = new (std::nothrow) PolyphaseSincDownsampler();
    if (!ds) return 0;
    return static_cast<jlong>(reinterpret_cast<uintptr_t>(ds));
}

JNIEXPORT void JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeDestroy(JNIEnv* env, jobject /*thiz*/,
                                                                jlong handle) {
    (void)env;
    delete fromHandle(handle);
}

JNIEXPORT jboolean JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeConfigure(JNIEnv* env, jobject /*thiz*/,
                                                                  jlong handle, jint inputRate,
                                                                  jint outputRate) {
    (void)env;
    PolyphaseSincDownsampler* ds = fromHandle(handle);
    if (!ds) return JNI_FALSE;
    return ds->configure(static_cast<int>(inputRate), static_cast<int>(outputRate)) ? JNI_TRUE
                                                                                    : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeProcess(JNIEnv* env, jobject /*thiz*/,
                                                                jlong handle, jfloatArray in,
                                                                jint inFrames, jfloatArray out,
                                                                jint outCap) {
    PolyphaseSincDownsampler* ds = fromHandle(handle);
    if (!ds || !in || !out || inFrames <= 0 || outCap <= 0) return 0;
    if (!ensureScratch(static_cast<int>(inFrames))) return 0;
    jfloat* inPtr = static_cast<jfloat*>(env->GetPrimitiveArrayCritical(in, nullptr));
    if (!inPtr) return 0;
    jfloat* outPtr = static_cast<jfloat*>(env->GetPrimitiveArrayCritical(out, nullptr));
    if (!outPtr) {
        env->ReleasePrimitiveArrayCritical(in, inPtr, JNI_ABORT);
        return 0;
    }
    // Intercalado -> L/R separados (scratch).
    for (int i = 0; i < inFrames; ++i) {
        g_scratchL[static_cast<size_t>(i)] = inPtr[2 * i];
        g_scratchR[static_cast<size_t>(i)] = inPtr[2 * i + 1];
    }
    const int produced = ds->process(g_scratchL.data(), g_scratchR.data(),
                                     static_cast<int>(inFrames),
                                     g_scratchL.data(), g_scratchR.data());
    // L/R separados -> intercalado (reutiliza el mismo scratch).
    for (int i = 0; i < produced; ++i) {
        outPtr[2 * i] = g_scratchL[static_cast<size_t>(i)];
        outPtr[2 * i + 1] = g_scratchR[static_cast<size_t>(i)];
    }
    env->ReleasePrimitiveArrayCritical(out, outPtr, 0);
    env->ReleasePrimitiveArrayCritical(in, inPtr, JNI_ABORT);
    return static_cast<jint>(produced);
}

JNIEXPORT jint JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeFlush(JNIEnv* env, jobject /*thiz*/,
                                                              jlong handle, jfloatArray out,
                                                              jint outCap) {
    PolyphaseSincDownsampler* ds = fromHandle(handle);
    if (!ds || !out || outCap <= 0) return 0;
    if (!ensureScratch(static_cast<int>(outCap))) return 0;
    jfloat* outPtr = static_cast<jfloat*>(env->GetPrimitiveArrayCritical(out, nullptr));
    if (!outPtr) return 0;
    const int produced = ds->flush(g_scratchL.data(), g_scratchR.data());
    for (int i = 0; i < produced; ++i) {
        outPtr[2 * i] = g_scratchL[static_cast<size_t>(i)];
        outPtr[2 * i + 1] = g_scratchR[static_cast<size_t>(i)];
    }
    env->ReleasePrimitiveArrayCritical(out, outPtr, 0);
    return static_cast<jint>(produced);
}

JNIEXPORT void JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeReset(JNIEnv* env, jobject /*thiz*/,
                                                              jlong handle) {
    (void)env;
    PolyphaseSincDownsampler* ds = fromHandle(handle);
    if (!ds) return;
    ds->reset();
}

JNIEXPORT jint JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeTaps(JNIEnv* env, jobject /*thiz*/,
                                                             jlong handle) {
    (void)env;
    PolyphaseSincDownsampler* ds = fromHandle(handle);
    if (!ds) return 0;
    return static_cast<jint>(ds->taps());
}

JNIEXPORT jint JNICALL
Java_com_ivanna_omega_audio_NativeSincDownsampler_nativeLatencyFrames(JNIEnv* env, jobject /*thiz*/,
                                                                     jlong handle) {
    (void)env;
    PolyphaseSincDownsampler* ds = fromHandle(handle);
    if (!ds) return 0;
    return static_cast<jint>(ds->latencyFrames());
}

}  // extern "C"
