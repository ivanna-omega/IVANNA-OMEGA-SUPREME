/**
 * test_genre_smooth_state.cpp — Estabilizador de género + mix (0046).
 *
 * Diseño de Esse (verbatim en include/genre_smooth_state.hpp): EMA
 * alpha 0.30 + histéresis (alto 0.75 / bajo 0.40) + dwell de 3 hits en
 * la inferencia lenta (~1 Hz), y paso exponencial tau 0.60 s en el hilo
 * RT. Cubre el defecto diagnosticado en vivo 2026-11-17 21:14 CST: el
 * clasificador NPE brincaba Hip-Hop/EDM ↔ Electrónica ↔ Pop cada 1–2 s
 * en la misma canción (umbrales duros instantáneos en
 * autonomous_brain.hpp:375-385) y el mix de captura cicla 0.45↔1.00.
 *
 * Los 4 tests exigidos por el frente + 2 de integración con el cerebro:
 *   (a) probabilidad cruda oscilando → el género activo NO cambia.
 *   (b) probabilidad sostenida ≥0.75 → el género SÍ cambia (tras dwell).
 *   (c) el mix RT converge al target en ~1.8–2.0 s sin saltos.
 *   (d) la zona muerta (0.40..0.75) no cambia el género.
 *   (e) el cerebro sigue clasificando tras las primeras ventanas.
 *   (f) el cerebro NO cambia de familia con CF alternando cada 1 s.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "include/genre_smooth_state.hpp"
#include "neuromorphic/autonomous_brain.hpp"
#include "neuromorphic/npe_analysis_tap.hpp"

using ivanna::genre::GenreSmoothState;
using ivanna::genre::step_genre_mix_rt;
using ivanna::genre::update_genre_classifier;
using ivanna::acoustic::AutonomousBrain;
using ivanna::acoustic::Synthesizer;
using ivanna::acoustic::npeAnalyzeStereo;

namespace {

constexpr float kLowMix = 0.45f;
constexpr float kHighMix = 1.00f;
constexpr float kSr = 48000.f;
const char* kNone = "\xe2\x80\x94";  // "—"

// Familia 1 (comprimida) según el gate del cerebro (autonomous_brain.hpp).
bool isFamily1(const std::string& g) {
    return g == "Hip-Hop / EDM" || g == "Pop" || g == "Reggaet\xc3\xb3n" ||
           g == "Electr\xc3\xb3nica";
}
bool isFamily0(const std::string& g) {
    return g == "Rock / Metal" || g == "Jazz / Cl\xc3\xa1sica" ||
           g == "Ac\xc3\xbastica" || g == "Mixto";
}

// Señal tipo música comprimida (CF bajo): graves con ataques + medios +
// agudos, continua. CF ≈ 5 dB → familia comprimida.
void compressedLike(std::vector<float>& l, std::vector<float>& r, int n, float amp) {
    l.resize(n); r.resize(n);
    for (int i = 0; i < n; ++i) {
        const double t = i / (double)kSr;
        const double kick = std::sin(2 * 3.14159265 * 55.0 * t) * (0.5 + 0.5 * std::sin(2 * 3.14159265 * 2.0 * t));
        const double v = 0.5 * kick + 0.25 * std::sin(2 * 3.14159265 * 1800.0 * t) + 0.15 * std::sin(2 * 3.14159265 * 7000.0 * t);
        l[i] = (float)(amp * v);
        r[i] = (float)(amp * v * 0.98);
    }
}

// Señal dinámica (CF alto): ráfagas de 5 ms a 55 Hz cada 80 ms, silencio
// entre medias. Pico 0.9, RMS ≈ 0.16 → CF ≈ 15 dB → familia dinámica.
// El periodo (80 ms) es MENOR que la ventana del cerebro (85 ms) a
// propósito: cada ventana contiene al menos una ráfaga, así ninguna
// ventana es silencio (en silencio evaluateAndDrive retorna antes y el
// contador de decimado no avanza, lo que desregulariza la cadencia ~1 Hz
// y dos muestras dinámicas seguidas tumbarían la familia — artefacto del
// test, no del diseño).
void dynamicLike(std::vector<float>& l, std::vector<float>& r, int n, float amp) {
    l.resize(n); r.resize(n);
    for (int i = 0; i < n; ++i) {
        const double t = i / (double)kSr;
        const double phase = std::fmod(t, 0.080);
        const double burst = (phase < 0.005)
            ? std::sin(2 * 3.14159265 * 55.0 * t) : 0.0;
        l[i] = (float)(amp * burst);
        r[i] = (float)(amp * burst * 0.98);
    }
}

std::string feedSeconds(AutonomousBrain& b, Synthesizer& s,
                        const std::vector<float>& l, const std::vector<float>& r,
                        int seconds) {
    const int n = static_cast<int>(kSr) * seconds;
    for (int off = 0; off + 480 <= n; off += 480)
        npeAnalyzeStereo(b, s, l.data() + off, r.data() + off, 480, kSr);
    return b.getLastGenre();
}

} // namespace

// ── (a) La probabilidad cruda oscilando NO cambia el género activo ────────
TEST(GenreSmoothState, OscillatingProbabilityDoesNotFlipHigh) {
    GenreSmoothState st;
    // Primera inferencia 0.9 → siembra en frío, familia 1.
    EXPECT_FLOAT_EQ(update_genre_classifier(0.9f, kLowMix, kHighMix, st), kHighMix);
    EXPECT_EQ(st.active_genre, 1);
    // Brinca 0.3/0.8 cada segundo: la EMA (~2.8 s) nunca sale de la zona
    // muerta con racha suficiente para el dwell.
    for (int i = 0; i < 6; ++i) {
        update_genre_classifier(0.3f, kLowMix, kHighMix, st);
        EXPECT_EQ(st.active_genre, 1) << "flip a 0 en la iteración " << i << " (p=0.3)";
        EXPECT_FLOAT_EQ(st.target_mix.load(), kHighMix);
        update_genre_classifier(0.8f, kLowMix, kHighMix, st);
        EXPECT_EQ(st.active_genre, 1) << "flip a 0 en la iteración " << i << " (p=0.8)";
        EXPECT_FLOAT_EQ(st.target_mix.load(), kHighMix);
    }
}

TEST(GenreSmoothState, OscillatingProbabilityDoesNotFlipLow) {
    GenreSmoothState st;
    EXPECT_FLOAT_EQ(update_genre_classifier(0.2f, kLowMix, kHighMix, st), kLowMix);
    EXPECT_EQ(st.active_genre, 0);
    for (int i = 0; i < 6; ++i) {
        update_genre_classifier(0.7f, kLowMix, kHighMix, st);
        EXPECT_EQ(st.active_genre, 0) << "flip a 1 en la iteración " << i << " (p=0.7)";
        EXPECT_FLOAT_EQ(st.target_mix.load(), kLowMix);
        update_genre_classifier(0.1f, kLowMix, kHighMix, st);
        EXPECT_EQ(st.active_genre, 0) << "flip a 1 en la iteración " << i << " (p=0.1)";
        EXPECT_FLOAT_EQ(st.target_mix.load(), kLowMix);
    }
}

// ── (b) Probabilidad sostenida ≥0.75 SÍ cambia el género (tras dwell) ─────
TEST(GenreSmoothState, SustainedHighSwitchesAfterDwell) {
    GenreSmoothState st;
    update_genre_classifier(0.2f, kLowMix, kHighMix, st);
    update_genre_classifier(0.2f, kLowMix, kHighMix, st);
    EXPECT_EQ(st.active_genre, 0);
    // Dos inferencias sostenidas en 0.9 NO bastan: la EMA aún sube por la
    // zona muerta (0.41, 0.56) y el dwell exige 3 hits POR ENCIMA de 0.75.
    update_genre_classifier(0.9f, kLowMix, kHighMix, st);
    update_genre_classifier(0.9f, kLowMix, kHighMix, st);
    EXPECT_EQ(st.active_genre, 0) << "cambió demasiado pronto (sin dwell real)";
    // Seis más: la EMA cruza 0.75 y acumula los 3 hits consecutivos.
    for (int i = 0; i < 6; ++i)
        update_genre_classifier(0.9f, kLowMix, kHighMix, st);
    EXPECT_EQ(st.active_genre, 1);
    EXPECT_FLOAT_EQ(st.target_mix.load(), kHighMix);
    // Y de vuelta: al bajar NO hay dwell, pero la EMA tiene que cruzar
    // 0.40 primero (desde ~0.84: 0.62, 0.48, 0.37 → tres sostenidos en
    // 0.1). Un solo 0.1 deja la EMA en zona muerta y NO cambia nada.
    update_genre_classifier(0.1f, kLowMix, kHighMix, st);
    EXPECT_EQ(st.active_genre, 1) << "un solo 0.1 no debe bajar la familia";
    update_genre_classifier(0.1f, kLowMix, kHighMix, st);
    update_genre_classifier(0.1f, kLowMix, kHighMix, st);
    EXPECT_EQ(st.active_genre, 0);
    EXPECT_FLOAT_EQ(st.target_mix.load(), kLowMix);
}

// ── (c) El mix RT converge al target en ~2 s sin saltos ───────────────────
TEST(GenreSmoothState, RtMixConvergesSmoothly) {
    GenreSmoothState st;
    update_genre_classifier(0.9f, kLowMix, kHighMix, st);  // target = 1.00
    st.current_mix.store(kLowMix, std::memory_order_relaxed);
    constexpr float kDt = 1.0f / 150.0f;  // bloque 320 muestras @ 48 kHz
    float prev = kLowMix;
    float maxStep = 0.f;
    for (int i = 0; i < 300; ++i) {  // 2.0 s
        const float cur = step_genre_mix_rt(st, kDt);
        EXPECT_GE(cur, prev - 1e-7f) << "el mix bajó en el paso " << i;
        maxStep = std::max(maxStep, cur - prev);
        prev = cur;
    }
    EXPECT_GE(prev, 0.97f) << "tras 2.0 s no llegó al 95% del recorrido";
    EXPECT_LT(maxStep, 0.02f) << "salto por bloque demasiado grande (zipper)";
    // Y de vuelta, simétrico: tres 0.1 sostenidos para que la EMA cruce
    // 0.40 y el target pase a 0.45 (un solo 0.1 queda en zona muerta).
    update_genre_classifier(0.1f, kLowMix, kHighMix, st);
    update_genre_classifier(0.1f, kLowMix, kHighMix, st);
    update_genre_classifier(0.1f, kLowMix, kHighMix, st);
    EXPECT_FLOAT_EQ(st.target_mix.load(), kLowMix);
    for (int i = 0; i < 300; ++i) prev = step_genre_mix_rt(st, kDt);
    EXPECT_LE(prev, 0.48f);
}

// ── (d) La zona muerta no cambia el género ────────────────────────────────
TEST(GenreSmoothState, DeadZoneHoldsGenre) {
    GenreSmoothState st;
    update_genre_classifier(0.6f, kLowMix, kHighMix, st);  // siembra en 0
    EXPECT_EQ(st.active_genre, 0);
    for (int i = 0; i < 6; ++i) {
        update_genre_classifier(0.6f, kLowMix, kHighMix, st);
        EXPECT_EQ(st.active_genre, 0);
        EXPECT_FLOAT_EQ(st.target_mix.load(), kLowMix);
    }
    // Desde la familia 1, la zona muerta tampoco la suelta.
    for (int i = 0; i < 8; ++i)
        update_genre_classifier(0.9f, kLowMix, kHighMix, st);
    EXPECT_EQ(st.active_genre, 1);
    for (int i = 0; i < 6; ++i) {
        update_genre_classifier(0.6f, kLowMix, kHighMix, st);
        EXPECT_EQ(st.active_genre, 1) << "la zona muerta soltó la familia 1";
        EXPECT_FLOAT_EQ(st.target_mix.load(), kHighMix);
    }
}

// Probabilidades no finitas / fuera de rango no rompen nada.
TEST(GenreSmoothState, NonFiniteProbabilityIsClamped) {
    GenreSmoothState st;
    EXPECT_FLOAT_EQ(update_genre_classifier(NAN, kLowMix, kHighMix, st), kLowMix);
    EXPECT_EQ(st.active_genre, 0);
    EXPECT_FLOAT_EQ(update_genre_classifier(5.0f, kLowMix, kHighMix, st), kLowMix);
}

// ── (e) El cerebro sigue clasificando tras las primeras ventanas ──────────
TEST(GenreSmoothBrain, StillClassifiesAfterFirstWindows) {
    AutonomousBrain brain; Synthesizer synth;
    EXPECT_STREQ(brain.getLastGenre(), kNone);
    std::vector<float> l, r;
    compressedLike(l, r, static_cast<int>(kSr) * 2, 0.6f);
    const std::string g = feedSeconds(brain, synth, l, r, 2);
    EXPECT_STRNE(g.c_str(), kNone) << "el gate bloqueó la primera clasificación";
    EXPECT_TRUE(isFamily1(g)) << "la señal comprimida dio familia 0: " << g;
}

// ── (f) El cerebro NO cambia de familia con CF alternando cada 1 s ───────
TEST(GenreSmoothBrain, HoldsFamilyUnderAlternatingCrestFactor) {
    // Control: las dos señales puras discriminan familias.
    {
        AutonomousBrain brain; Synthesizer synth;
        std::vector<float> l, r;
        compressedLike(l, r, static_cast<int>(kSr) * 12, 0.6f);
        EXPECT_TRUE(isFamily1(feedSeconds(brain, synth, l, r, 12)))
            << "la señal comprimida pura no dio familia 1";
    }
    {
        AutonomousBrain brain; Synthesizer synth;
        std::vector<float> l, r;
        dynamicLike(l, r, static_cast<int>(kSr) * 12, 0.9f);
        EXPECT_TRUE(isFamily0(feedSeconds(brain, synth, l, r, 12)))
            << "la señal dinámica pura no dio familia 0";
    }
    // Alternancia 1 s comprimido / 1 s dinámico durante 12 s: la etiqueta
    // publicada se queda en la familia 1 (la EMA ~2.8 s + dwell ~3 s se
    // tragan los brincos de 1 s). Sin el gate brincaría cada ventana.
    // Pre-roll de 2 s comprimido: establece la familia (la primera
    // inferencia del estabilizador — ~1 s — muestrea el segmento que esté
    // sonando; sin pre-roll muestrearía el dinámico y la siembra en frío
    // nacería en la familia 0, que es el transitorio de arranque, no el
    // comportamiento en régimen que este test mide).
    AutonomousBrain brain; Synthesizer synth;
    std::vector<float> lc, rc, ld, rd;
    compressedLike(lc, rc, static_cast<int>(kSr), 0.6f);
    dynamicLike(ld, rd, static_cast<int>(kSr), 0.9f);
    feedSeconds(brain, synth, lc, rc, 1);
    feedSeconds(brain, synth, lc, rc, 1);
    for (int sec = 0; sec < 12; ++sec) {
        const auto& l = (sec % 2 == 0) ? lc : ld;
        const auto& r = (sec % 2 == 0) ? rc : rd;
        for (int off = 0; off + 480 <= static_cast<int>(kSr); off += 480)
            npeAnalyzeStereo(brain, synth, l.data() + off, r.data() + off, 480, kSr);
        const std::string g = brain.getLastGenre();
        EXPECT_TRUE(isFamily1(g))
            << "la familia cambió en el segundo " << sec << ": " << g;
    }
}
