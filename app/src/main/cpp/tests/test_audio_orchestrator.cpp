// test_audio_orchestrator.cpp — Tests de host del orquestador maestro de audio
// (propuesta 0021, coordinador del Plan Omnipotencia; decisión del Consejo
// Supremo 2026-11-16, ronda 1 unánime: proposer gpt-oss-120b, reviewer luna,
// arbiter mini — candidato (e), tests para los 3 módulos sin tests).
//
// audio_orchestrator.cpp (19 KB) era uno de los 3 módulos top-level sin tests
// (audit_2026-11-16.md §5). Es el orquestador maestro: ganancia combinada
// (dialog × bass × master+eq) con slew por muestra, widener M/S mono-safe con
// soft-clip racional (knee 0.9, techo 1.0), sanitizador NaN/Inf, medidor LUFS
// BS.1770-4 real y telemetría (LUFS/pico). Compila en host con host_stubs
// (jni.h + android/log.h) y enlaza con sus dependencias reales:
// audio_control_plane.cpp (g_control_frame) + evolutionary_kernel_v2.cpp
// (evo_best_fitness / evo_get_best_genome / evo_get_generation).
//
// Lo que se verifica (comportamiento, no implementación):
// - Seguridad: buffer nulo / 0 muestras / tasa fuera de rango no cuelgan.
// - Sanitizador: NaN/+Inf/-Inf en entrada salen como 0 y nunca como NaN/Inf.
// - Silencio entra, silencio sale.
// - Ganancia master -6 dB aplica 0.501× exacto; +100 dB se clampa a +24 dB.
// - EQ +100 dB se clampa a +12 dB. NaN en un setter se ignora (no cambia nada).
// - Width/exciter/HRTF se clampan a [0,1] (observables en g_control_frame /
//   g_hrtf_wet_dry). Flush HRTF levanta el flag.
// - Widener con señal caliente nunca supera ±1.0 (soft-clip + clamp duro).
// - LUFS/pico BS.1770-4 finitos y en rango sano tras un seno conocido.
// - Mono funciona (sin widener, sin crash).
//
// Estado compartido: g_orch es un global estático; los tests corren en orden
// de definición en un solo proceso. Cada test sensible a ganancia llama a
// ResetGains() (vuelve a 0 dB y asienta gainSmoothed con un bloque) antes de
// medir. La LUFS integrada es con gating BS.1770 (los bloques de silencio no
// la mueven), así que no se afirma un valor exacto de LUFS, solo rango sano.
#include <atomic>
#include <cmath>
#include <cstddef>
#include <gtest/gtest.h>

#include "audio_orchestrator.h"
#include "audio_control_plane.hpp"

// Globales del orquestador (definidos en audio_orchestrator.cpp, no static):
// mezcla wet/dry HRTF y flag de flush que consume IvannaFusionCore.
extern std::atomic<float> g_hrtf_wet_dry;
extern std::atomic<bool> g_hrtf_flush_req;

namespace {

constexpr int kSampleRate = 48000;
constexpr int kFrames = 480;                 // 10 ms a 48 kHz
constexpr int kStereoSamples = kFrames * 2;  // muestras intercaladas L/R

// Seno 1 kHz de amplitud `amp` en estéreo (L=R).
void FillSine(float* buf, int frames, float amp) {
    for (int i = 0; i < frames; ++i) {
        const float v = amp * std::sin(2.0f * 3.14159265f * 1000.0f *
                                       static_cast<float>(i) / 48000.0f);
        buf[2 * i] = v;
        buf[2 * i + 1] = v;
    }
}

float Rms(const float* buf, int n) {
    double acc = 0.0;
    for (int i = 0; i < n; ++i) acc += static_cast<double>(buf[i]) * buf[i];
    return static_cast<float>(std::sqrt(acc / n));
}

float Peak(const float* buf, int n) {
    float p = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float a = std::fabs(buf[i]);
        if (a > p) p = a;
    }
    return p;
}

bool AllFinite(const float* buf, int n) {
    for (int i = 0; i < n; ++i)
        if (!std::isfinite(buf[i])) return false;
    return true;
}

// Vuelve todas las ganancias a neutro y asienta gainSmoothed a 1.0 con un
// bloque de seno (el slew llega al objetivo al final de cada bloque).
void ResetGains() {
    ivanna_set_master_gain(0.0f);
    ivanna_set_eq_gain(0.0f);
    ivanna_set_route_profile(0.0f, 0.0f, 0.0f);
    ivanna_set_stereo_width(0.0f);
    ivanna_set_manifold_enabled(false);
    float buf[kStereoSamples];
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
}

TEST(AudioOrchestratorTest, NullAndEmptyDoNotCrash) {
    float buf[kStereoSamples];
    FillSine(buf, kFrames, 0.5f);
    EXPECT_NO_FATAL_FAILURE(ivanna_orchestrate(nullptr, 0, 2, kSampleRate));
    EXPECT_NO_FATAL_FAILURE(ivanna_orchestrate(buf, 0, 2, kSampleRate));
    EXPECT_NO_FATAL_FAILURE(ivanna_orchestrate(nullptr, 100, 2, kSampleRate));
    EXPECT_NO_FATAL_FAILURE(ivanna_orchestrate(buf, -5, 2, kSampleRate));
}

TEST(AudioOrchestratorTest, SanitizesNaNAndInf) {
    ResetGains();
    float buf[kStereoSamples];
    FillSine(buf, kFrames, 0.5f);
    // Corromper muestras conocidas: NaN, +Inf, -Inf en ambos canales.
    buf[0] = NAN;
    buf[1] = INFINITY;
    buf[2] = -INFINITY;
    buf[3] = NAN;
    buf[100] = INFINITY;
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    EXPECT_TRUE(AllFinite(buf, kStereoSamples))
        << "la salida no debe contener NaN/Inf tras el sanitizador";
    EXPECT_EQ(buf[0], 0.0f);
    EXPECT_EQ(buf[1], 0.0f);
    EXPECT_EQ(buf[2], 0.0f);
    EXPECT_EQ(buf[3], 0.0f);
    EXPECT_EQ(buf[100], 0.0f);
}

TEST(AudioOrchestratorTest, SilenceStaysSilent) {
    ResetGains();
    float buf[kStereoSamples] = {};
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    for (int i = 0; i < kStereoSamples; ++i) EXPECT_EQ(buf[i], 0.0f);
}

TEST(AudioOrchestratorTest, MasterGainMinus6dBApplies) {
    ResetGains();
    float buf[kStereoSamples];
    // Bloque 1: rampa 1.0 -> 0.501. Bloque 2: ganancia ya asentada.
    ivanna_set_master_gain(-6.0f);
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    const float inRms = 0.5f / std::sqrt(2.0f);
    const float expect = inRms * 0.501187f;  // 10^(-6/20)
    EXPECT_NEAR(Rms(buf, kStereoSamples), expect, expect * 0.03f);
    ivanna_set_master_gain(0.0f);
}

TEST(AudioOrchestratorTest, MasterGainClampsToPlus24dB) {
    ResetGains();
    float buf[kStereoSamples];
    ivanna_set_master_gain(100.0f);  // fuera de rango: debe clampar a +24 dB
    FillSine(buf, kFrames, 0.01f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    FillSine(buf, kFrames, 0.01f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    // +24 dB = 15.849x. Sin clamp (+100 dB = 100000x) el pico sería ~1000.
    EXPECT_NEAR(Peak(buf, kStereoSamples), 0.01f * 15.8489f, 0.01f);
    EXPECT_LT(Peak(buf, kStereoSamples), 1.0f);
    ivanna_set_master_gain(0.0f);
}

TEST(AudioOrchestratorTest, EqGainClampsToPlus12dB) {
    ResetGains();
    float buf[kStereoSamples];
    ivanna_set_eq_gain(100.0f);  // fuera de rango: debe clampar a +12 dB
    FillSine(buf, kFrames, 0.01f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    FillSine(buf, kFrames, 0.01f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    // +12 dB = 3.981x. Sin clamp el pico sería ~1000.
    EXPECT_NEAR(Peak(buf, kStereoSamples), 0.01f * 3.9811f, 0.005f);
    EXPECT_LT(Peak(buf, kStereoSamples), 1.0f);
    ivanna_set_eq_gain(0.0f);
}

TEST(AudioOrchestratorTest, SettersIgnoreNaN) {
    ResetGains();
    float buf[kStereoSamples];
    ivanna_set_master_gain(-6.0f);
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    const float before = Rms(buf, kStereoSamples);
    // NaN debe ignorarse: la ganancia no cambia (si se aceptara, la salida
    // sería NaN y el RMS dejaría de ser finito).
    ivanna_set_master_gain(NAN);
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    EXPECT_TRUE(std::isfinite(Rms(buf, kStereoSamples)));
    EXPECT_NEAR(Rms(buf, kStereoSamples), before, before * 0.03f);
    ivanna_set_master_gain(0.0f);
}

TEST(AudioOrchestratorTest, AntiDolbyScoresStoredAndClamped) {
    ivanna_set_anti_dolby_scores(0.2f, 0.3f, 0.1f);
    EXPECT_NEAR(g_control_frame.yamnet_voice_score.load(), 0.2f, 1e-6f);
    EXPECT_NEAR(g_control_frame.yamnet_music_score.load(), 0.3f, 1e-6f);
    EXPECT_NEAR(g_control_frame.yamnet_bass_score.load(), 0.1f, 1e-6f);
    // silencio = 1 - (voz+música+graves) = 0.4
    EXPECT_NEAR(g_control_frame.yamnet_silence_score.load(), 0.4f, 1e-6f);
    // Fuera de [0,1] se clampa por componente.
    ivanna_set_anti_dolby_scores(2.0f, -1.0f, 0.5f);
    EXPECT_NEAR(g_control_frame.yamnet_voice_score.load(), 1.0f, 1e-6f);
    EXPECT_NEAR(g_control_frame.yamnet_music_score.load(), 0.0f, 1e-6f);
    EXPECT_NEAR(g_control_frame.yamnet_bass_score.load(), 0.5f, 1e-6f);
    // NaN se ignora: los valores no cambian.
    ivanna_set_anti_dolby_scores(NAN, 0.3f, 0.1f);
    EXPECT_NEAR(g_control_frame.yamnet_voice_score.load(), 1.0f, 1e-6f);
}

TEST(AudioOrchestratorTest, WidthExciterHrtfClampToUnit) {
    ivanna_set_stereo_width(5.0f);
    ivanna_set_exciter_amount(5.0f);
    ivanna_set_hrtf_wet_dry(5.0f);
    EXPECT_NEAR(g_control_frame.audio_engine_width.load(), 1.0f, 1e-6f);
    EXPECT_NEAR(g_control_frame.audio_engine_exciter.load(), 1.0f, 1e-6f);
    EXPECT_NEAR(g_hrtf_wet_dry.load(), 1.0f, 1e-6f);
    ivanna_set_stereo_width(-2.0f);
    ivanna_set_exciter_amount(-2.0f);
    ivanna_set_hrtf_wet_dry(-2.0f);
    EXPECT_NEAR(g_control_frame.audio_engine_width.load(), 0.0f, 1e-6f);
    EXPECT_NEAR(g_control_frame.audio_engine_exciter.load(), 0.0f, 1e-6f);
    EXPECT_NEAR(g_hrtf_wet_dry.load(), 0.0f, 1e-6f);
    ivanna_set_stereo_width(0.0f);
    ivanna_set_hrtf_wet_dry(1.0f);
}

TEST(AudioOrchestratorTest, FlushHrtfHistorySetsFlag) {
    g_hrtf_flush_req.store(false, std::memory_order_relaxed);
    ivanna_flush_hrtf_history();
    EXPECT_TRUE(g_hrtf_flush_req.load(std::memory_order_relaxed));
}

TEST(AudioOrchestratorTest, WidenerHotSignalNeverExceedsUnity) {
    ResetGains();
    ivanna_set_stereo_width(1.0f);  // widener a tope
    float buf[kStereoSamples];
    FillSine(buf, kFrames, 0.9f);
    ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    EXPECT_TRUE(AllFinite(buf, kStereoSamples));
    EXPECT_LE(Peak(buf, kStereoSamples), 1.0f)
        << "el soft-clip (knee 0.9, techo 1.0) + clamp duro deben acotar a ±1.0";
    ivanna_set_stereo_width(0.0f);
}

TEST(AudioOrchestratorTest, LufsAndPeakFiniteAndSane) {
    ResetGains();
    float buf[kStereoSamples];
    for (int k = 0; k < 6; ++k) {
        FillSine(buf, kFrames, 0.5f);
        ivanna_orchestrate(buf, kStereoSamples, 2, kSampleRate);
    }
    const float lufs = ivanna_get_lufs();
    const float peakDb = ivanna_get_peak_dbfs();
    EXPECT_TRUE(std::isfinite(lufs));
    EXPECT_TRUE(std::isfinite(peakDb));
    EXPECT_GT(lufs, -70.0f);
    EXPECT_LT(lufs, 0.0f);
    EXPECT_GT(peakDb, -70.0f);
    EXPECT_LE(peakDb, 0.0f);
    // Pico de sesión (máximo retenido por el medidor): el seno de amplitud
    // 0.5 tiene pico 20*log10(0.5) = -6.02 dBFS, así que el pico de sesión
    // debe ser al menos ese (otros tests previos pueden haberlo subido).
    EXPECT_GE(peakDb, -7.02f);
}

TEST(AudioOrchestratorTest, SampleRateOutOfRangeDefaultsSafely) {
    ResetGains();
    float buf[kStereoSamples];
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, 100);  // < 8000 -> 48000
    EXPECT_TRUE(AllFinite(buf, kStereoSamples));
    EXPECT_NEAR(Peak(buf, kStereoSamples), 0.5f, 0.05f);
    FillSine(buf, kFrames, 0.5f);
    ivanna_orchestrate(buf, kStereoSamples, 2, 1000000);  // > 768000 -> 48000
    EXPECT_TRUE(AllFinite(buf, kStereoSamples));
    EXPECT_NEAR(Peak(buf, kStereoSamples), 0.5f, 0.05f);
}

TEST(AudioOrchestratorTest, MonoWorks) {
    ResetGains();
    float mono[kFrames];
    for (int i = 0; i < kFrames; ++i)
        mono[i] = 0.5f * std::sin(2.0f * 3.14159265f * 1000.0f *
                                  static_cast<float>(i) / 48000.0f);
    ivanna_orchestrate(mono, kFrames, 1, kSampleRate);
    EXPECT_TRUE(AllFinite(mono, kFrames));
    EXPECT_NEAR(Rms(mono, kFrames), 0.5f / std::sqrt(2.0f), 0.01f);
}

}  // namespace
