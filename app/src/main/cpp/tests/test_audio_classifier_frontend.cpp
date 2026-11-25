// test_audio_classifier_frontend.cpp — CORONA Frente A, defecto 3.
//
// El clasificador NEURO (IvannaAudioClassifier) devolvía siempre MUSIC
// porque (1) el banco mel estaba a cero y nada lo llenaba nunca y (2)
// computeSTFT() no calculaba FFT (el "espectro" era |muestra| x 0.5).
// El fix implementa la FFT radix-2 real + el banco triangular real,
// detrás del flag setRealFrontendEnabled() (default ON, orden Esse
// 2026-11-25, todo activo en la 502).
//
// Qué afirma este test (host, modo síncrono = determinista):
//   1. LegacyFlagOffIsAlwaysMusic: con el flag OFF (explícito) el
//      comportamiento es EXACTAMENTE el de antes del fix (siempre MUSIC,
//      probabilidades idénticas para cualquier contenido) — el legacy
//      queda congelado como referencia.
//   2. RealFrontendMelBankIsNonZeroAndSelective: con el flag ON (default)
//      el banco mel ya no es cero y es selectivo en frecuencia (tono
//      grave → energía en bandas bajas; tono agudo → bandas altas).
//   3. RealFrontendOutputDependsOnContent: con el flag ON la salida del
//      clasificador depende del contenido (los vectores de probabilidad
//      difieren > 0.25 entre un tono grave y uno agudo; el silencio sigue
//      dando la salida legacy, porque no tiene contenido que discriminar).
//   4. FlagOffRestoresLegacyExactly: apagar el flag restaura el legacy
//      exacto (el toggle es reversible).
//
// LÍMITE HONESTO (pesos placeholder): los pesos INT8 son sintéticos
// ((f+k)%15-7 / (c+i)%11-5) salvo loadWeights(); ningún peso entrenado
// existe en el repo. Por eso el argmax sigue siendo MUSIC para todas las
// señales probadas con el flag ON: la distribución de probabilidad SÍ
// discrimina contenido (test 3), pero el cambio de clase dominante exige
// pesos entrenados (CRNN TFLite, trabajo de laboratorio aparte). Este test
// NO inventa pesos ni ajusta el test a un argmax: afirma lo que el
// frontend realmente entrega.
#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "../IvannaAudioClassifier.hpp"

using namespace Ivanna;

namespace {

// 1 s @48 kHz por señal, en bloques de 128 (el camino del hilo de audio).
void feedSignal(IvannaAudioClassifier& c, const std::vector<float>& mono) {
    std::vector<float> l(128), r(128);
    for (size_t off = 0; off < mono.size(); off += 128) {
        const size_t chunk = std::min<size_t>(128, mono.size() - off);
        for (size_t i = 0; i < chunk; ++i) l[i] = r[i] = mono[off + i];
        c.ingestAudioFrame(l.data(), r.data(), chunk);
        c.processPendingFrames();
    }
    c.processPendingFrames();
}

std::vector<float> makeTone(double freqHz, double amp = 0.5) {
    std::vector<float> s(48000);
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = static_cast<float>(amp * std::sin(2.0 * 3.14159265358979323846 *
                                                 freqHz * static_cast<double>(i) / 48000.0));
    }
    return s;
}

std::vector<float> makeNoise(unsigned seed = 0) {
    // Ruido blanco determinista (LCG).
    std::vector<float> s(48000);
    uint32_t st = 1234567u + seed;
    for (size_t i = 0; i < s.size(); ++i) {
        st = st * 1664525u + 1013904223u;
        s[i] = (static_cast<float>(st >> 8) / 16777216.0f - 0.5f);
    }
    return s;
}

AIModelOutput readOut(IvannaAudioClassifier& c) {
    AIModelOutput out{};
    EXPECT_TRUE(c.getModelOutput(out)) << "sin salida válida tras 1 s de señal";
    return out;
}

float maxProbDiff(const AIModelOutput& a, const AIModelOutput& b) {
    float m = 0.0f;
    for (size_t i = 0; i < NUM_CLASSES; ++i) {
        m = std::max(m, std::fabs(a.probabilities[i] - b.probabilities[i]));
    }
    return m;
}

size_t peakMelBand(const std::array<float, MEL_BANDS>& e) {
    size_t argmax = 0;
    for (size_t m = 1; m < e.size(); ++m) {
        if (e[m] > e[argmax]) argmax = m;
    }
    return argmax;
}

}  // namespace

TEST(AudioClassifierFrontend, DefaultIsRealFrontendOn) {
    // Default ON (orden Esse 2026-11-25, todo activo en la 502): el
    // clasificador nace con la FFT + banco mel reales, no con el legacy.
    IvannaAudioClassifier c;
    EXPECT_TRUE(c.isRealFrontendEnabled());
}

TEST(AudioClassifierFrontend, LegacyFlagOffIsAlwaysMusic) {
    IvannaAudioClassifier c;
    // Flag OFF explícito (el default es ON desde la orden 2026-11-25):
    // el legacy queda congelado aquí como referencia.
    c.setRealFrontendEnabled(false);
    EXPECT_FALSE(c.isRealFrontendEnabled());
    c.setSynchronous(true);
    // El banco se construye siempre (es solo una tabla); con el flag OFF
    // no se consulta, así que el legacy viene del gating, no de un banco
    // ausente. Antes del fix la suma era 0.0.
    EXPECT_GT(c.melFilterbankWeightSum(), 100.0f)
        << "el banco mel triangular debe estar construido (suma de pesos)";

    const std::vector<float> low = makeTone(200.0);
    const std::vector<float> high = makeTone(4000.0);
    const std::vector<float> noise = makeNoise();
    const std::vector<float> silence(48000, 0.0f);

    feedSignal(c, low);
    const AIModelOutput oLow = readOut(c);
    feedSignal(c, high);
    const AIModelOutput oHigh = readOut(c);
    feedSignal(c, noise);
    const AIModelOutput oNoise = readOut(c);
    feedSignal(c, silence);
    const AIModelOutput oSilence = readOut(c);

    // Medido pre-fix (sonda host): MUSIC, conf 0.4743, probs
    // [0.0012, 0.4743, 0.2603, 0.1428, 0.0784, 0.0430], idénticas para
    // las 4 señales. El legacy queda congelado aquí.
    for (const AIModelOutput* o : {&oLow, &oHigh, &oNoise, &oSilence}) {
        ASSERT_TRUE(o->is_valid);
        EXPECT_EQ(static_cast<int>(o->dominant_class),
                  static_cast<int>(AudioContextClass::MUSIC));
        EXPECT_NEAR(o->confidence, 0.4743f, 1e-3f);
    }
    EXPECT_LT(maxProbDiff(oLow, oHigh), 1e-6f)
        << "legacy: el contenido no debe mover las probabilidades";
    EXPECT_LT(maxProbDiff(oLow, oNoise), 1e-6f);
    EXPECT_LT(maxProbDiff(oLow, oSilence), 1e-6f);
}

TEST(AudioClassifierFrontend, RealFrontendMelBankIsNonZeroAndSelective) {
    IvannaAudioClassifier c;
    c.setSynchronous(true);
    c.setRealFrontendEnabled(true);
    ASSERT_TRUE(c.isRealFrontendEnabled());

    // El banco ya no es cero (medido: suma 250.20).
    EXPECT_GT(c.melFilterbankWeightSum(), 100.0f);

    feedSignal(c, makeTone(200.0));
    const auto eLow = c.logMelEnergies();
    const size_t bandLow = peakMelBand(eLow);
    // 200 Hz cae en la banda ~5 (medido: 5, energía 7.99; el piso de
    // log(1e-10) es -23.03).
    EXPECT_LE(bandLow, 10u) << "un tono grave debe concentrar energía en bandas bajas";
    EXPECT_GT(eLow[bandLow], 0.0f) << "energía real, no el piso log(1e-10)";

    feedSignal(c, makeTone(4000.0));
    const auto eHigh = c.logMelEnergies();
    const size_t bandHigh = peakMelBand(eHigh);
    // 4000 Hz cae en la banda ~48 (medido: 48, energía 8.33).
    EXPECT_GE(bandHigh, 40u) << "un tono agudo debe concentrar energía en bandas altas";
    EXPECT_GT(eHigh[bandHigh], 0.0f);
    EXPECT_GT(eHigh[bandHigh] - eHigh[bandLow], 20.0f)
        << "la banda del tono agudo debe estar muy por encima del piso en la banda grave";
}

TEST(AudioClassifierFrontend, RealFrontendOutputDependsOnContent) {
    IvannaAudioClassifier c;
    c.setSynchronous(true);
    c.setRealFrontendEnabled(true);

    feedSignal(c, makeTone(200.0));
    const AIModelOutput oLow = readOut(c);
    feedSignal(c, makeTone(4000.0));
    const AIModelOutput oHigh = readOut(c);
    feedSignal(c, makeNoise());
    const AIModelOutput oNoise = readOut(c);
    // Medido con el fix (sonda host): MUSIC conf 0.7902 (grave),
    // 0.3040 (agudo), 0.6855 (ruido); scene_energy 0.9145 / 1.2089 /
    // 0.1577. La salida depende del contenido: max|Δprob| grave-agudo
    // 0.486, agudo-ruido 0.382, grave-ruido 0.167.
    EXPECT_GT(maxProbDiff(oLow, oHigh), 0.25f)
        << "grave vs agudo deben dar distribuciones muy distintas";
    EXPECT_GT(maxProbDiff(oHigh, oNoise), 0.25f)
        << "agudo vs ruido deben dar distribuciones muy distintas";
    EXPECT_GT(maxProbDiff(oLow, oNoise), 0.10f)
        << "grave vs ruido deben dar distribuciones distintas";
    EXPECT_GT(std::fabs(oLow.scene_energy - oHigh.scene_energy), 0.1f);
    EXPECT_GT(std::fabs(oLow.scene_energy - oNoise.scene_energy), 0.1f);

    // El silencio no tiene contenido: con el frontend real da la misma
    // salida que el legacy (mel en el piso log(1e-10) en las 64 bandas).
    feedSignal(c, std::vector<float>(48000, 0.0f));
    const AIModelOutput oSilence = readOut(c);
    EXPECT_EQ(static_cast<int>(oSilence.dominant_class),
              static_cast<int>(AudioContextClass::MUSIC));
    EXPECT_NEAR(oSilence.confidence, 0.4743f, 1e-3f);
}

TEST(AudioClassifierFrontend, FlagOffRestoresLegacyExactly) {
    // Referencia legacy: clasificador con el flag OFF explícito, mismo tono.
    IvannaAudioClassifier legacy;
    legacy.setRealFrontendEnabled(false);
    legacy.setSynchronous(true);
    feedSignal(legacy, makeTone(200.0));
    const AIModelOutput oLegacy = readOut(legacy);

    // Clasificador A: flag ON, luego OFF.
    IvannaAudioClassifier a;
    a.setSynchronous(true);
    a.setRealFrontendEnabled(true);
    feedSignal(a, makeTone(200.0));
    const AIModelOutput oOn = readOut(a);
    EXPECT_GT(maxProbDiff(oOn, oLegacy), 0.25f)
        << "con el flag ON la salida debe diferir del legacy";
    a.setRealFrontendEnabled(false);
    EXPECT_FALSE(a.isRealFrontendEnabled());
    feedSignal(a, makeTone(200.0));
    const AIModelOutput oOff = readOut(a);

    EXPECT_EQ(static_cast<int>(oOff.dominant_class),
              static_cast<int>(AudioContextClass::MUSIC));
    EXPECT_LT(maxProbDiff(oOff, oLegacy), 1e-6f)
        << "apagar el flag debe restaurar el legacy exacto";
}
