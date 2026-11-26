// test_tronidos_layer_step.cpp — Caza de los tronidos de la 493 (2026-11-22).
//
// Qué hace: corre la FusedChain REAL con señal musical estacionaria (suma de
// senos, pico ~-12 dBFS) y aplica UN cambio de parámetros en mitad del stream
// (t = 2.0 s, en frontera de hop de 128), por capa. Mide por capa:
//   - step_dbfs: máximo salto muestra a muestra en el bloque del cambio
//     (+ el bloque siguiente, por si el transitorio rebota ahí).
//   - baseline_dbfs: el mismo salto en un bloque en régimen (t = 1.0 s),
//     con la misma cadena y la misma señal: es el slew natural de la señal.
//   - excess_db = step - baseline: cuánto salto AÑADE el cambio de
//     parámetros por encima del slew natural. Un escalón sin crossfade
//     (la hipótesis del tronido) aparece como excess >> 0 dB.
//   - true peak de toda la salida, y duración del callback por bloque
//     (media/p99/máx en µs; la del bloque del cambio, aparte).
//
// Casos:
//   neutral_neutral — control: neutro → neutro. Debe ser bit-exacto
//     (la cadena en neutro es transparente) y excess ≈ 0.
//   repush_identical — IVANNA → la MISMA IVANNA: el evento real del
//     tronido (re-empuje del mismo estado por cambio de ventana). Si
//     setControls re-aplica estado y eso mete un escalón, aquí sale.
//   ivanna_on — neutro → IVANNA completa (todos los operadores a la vez).
//   layer_on_<op> — neutro → solo ese operador (0 NEURO..6 PULSO).
//   *_step — IVANNA → IVANNA con UN parámetro cambiado (orbe 1→0, holos
//     0.5→0, phi 0.5→0, roomWet 0.22→0, volterra 1→0, EQ low +2→-2,
//     compThreshold -12→-24, exciter 0→0.55, brain adaptation 1→0,
//     conservación 0→0.5).
//   mode_speakers — IVANNA HEADPHONES → IVANNA SPEAKERS (cambio de ruta;
//     en SPEAKERS se engancha la etapa TΩ).
//
// Filosofía P1: se REPORTA, no se afirma carácter. Solo FALLA si el control
// neutro no es bit-exacto o si alguna salida no es finita. El excess por
// capa dice qué capa mete el transitorio; la caza en el teléfono (§5.3
// del informe) es la que lo decide en el dispositivo real.
//
// Compilación: ivanna_add_test(test_tronidos_layer_step) + ${FUSED_CHAIN_SOURCES}
// (mismo patrón que test_p1_adversarial_matrix).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"

using namespace ivanna::fused;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kHop = 128;             // hop del motor (muestras)
constexpr double kDurS = 4.0;         // duración del stream
constexpr size_t kN = static_cast<size_t>(kFs * kDurS);
constexpr size_t kChangeAt = static_cast<size_t>(2.0 * kFs);   // cambio a 2.0 s
constexpr size_t kBaselineAt = static_cast<size_t>(1.0 * kFs); // bloque en régimen

double dbfs(double x) { return 20.0 * std::log10(std::max(std::fabs(x), 1e-15)); }

std::string fmt(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

// IVANNA completa: mismos valores que test_p1_adversarial_matrix::engagedControls().
FusedControls engagedControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.field.amount = 0.5f;
    c.field.phiAmount = 0.5f;
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 1.0f;
    c.tonal.curve = 1;
    c.tonal.lowDb = 2.0f;
    c.dynamics.compRatio = 2.0f;
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.safety = 1.0f;
    c.brain.mode = 0;  // AUTO
    c.brain.adaptation = 1.0f;
    c.clamp();
    return c;
}

// Solo el operador `op` activo (0 NEURO .. 6 PULSO); el resto neutro.
// Safety limiter siempre activo, como en la matriz P1.
FusedControls operatorOnlyControls(int op) {
    FusedControls c;
    c.setNeutral();
    switch (op) {
        case 0: c.brain.mode = 0; c.brain.adaptation = 1.0f; break;
        case 1:
            c.spatial.amount = 1.0f; c.spatial.azimuthDeg = 30.0f;
            c.spatial.distanceM = 1.0f; c.spatial.hrtfWet = 0.5f; break;
        case 2: c.field.amount = 0.5f; c.field.phiAmount = 0.5f; break;
        case 3: c.listenerRoom.roomWet = 0.22f; break;
        case 4: c.transducer.correction = 1.0f; break;
        case 5: c.tonal.curve = 1; c.tonal.lowDb = 2.0f; break;
        case 6:
            c.dynamics.compRatio = 2.0f; c.dynamics.compThresholdDb = -12.0f; break;
        default: break;
    }
    c.dynamics.safety = 1.0f;
    c.clamp();
    return c;
}

const char* kOpName[7] = {"neuro", "orbe", "holos", "sala_viva",
                          "volterra", "armonia", "pulso"};

// Señal musical estacionaria: senos 110/440/1000/3000 Hz, pico ~-12 dBFS.
// L y R con fase/ganancia distintas (estéreo real, sin transitorios de
// entrada: cualquier salto en la salida es del cambio de parámetros).
void makeSignal(std::vector<double>& l, std::vector<double>& r) {
    l.assign(kN, 0.0);
    r.assign(kN, 0.0);
    const double f[4] = {110.0, 440.0, 1000.0, 3000.0};
    const double a[4] = {0.10, 0.07, 0.05, 0.03};
    for (size_t n = 0; n < kN; ++n) {
        const double t = static_cast<double>(n) / kFs;
        double sl = 0.0, sr = 0.0;
        for (int k = 0; k < 4; ++k) {
            sl += a[k] * std::sin(2.0 * kPi * f[k] * t + 0.3 * k);
            sr += a[k] * 0.9 * std::sin(2.0 * kPi * f[k] * t + 0.3 * k + 0.7);
        }
        l[n] = sl;
        r[n] = sr;
    }
}

// Máximo |x[n]-x[n-1]| en [a, b) de ambos canales, en dBFS.
double maxStepDb(const std::vector<double>& l, const std::vector<double>& r,
                 size_t a, size_t b) {
    double m = 0.0;
    for (size_t n = a + 1; n < b && n < l.size(); ++n) {
        m = std::max(m, std::fabs(l[n] - l[n - 1]));
        m = std::max(m, std::fabs(r[n] - r[n - 1]));
    }
    return dbfs(m);
}

struct LayerResult {
    std::string name;
    double stepDb = 0.0;       // salto en bloque del cambio + siguiente
    double baselineDb = 0.0;   // salto en bloque en régimen (t = 1 s)
    double excessDb = 0.0;     // step - baseline
    double inStepDb = 0.0;     // salto de la ENTRADA en la misma ventana
    double excessVsInputDb = 0.0;  // step - inStep (mismo instante)
    double peakDb = 0.0;       // true peak de la salida
    double cbMeanUs = 0.0, cbP99Us = 0.0, cbMaxUs = 0.0;
    double cbChangeUs = 0.0;   // duración del callback del bloque del cambio
    // Ventana del cambio: estadística de la salida (distingue silencio
    // de congelado: RMS 0 = silencio; RMS > 0 con step 0 = congelado).
    double winMin = 0.0, winMax = 0.0, winMean = 0.0, winRms = 0.0;
    double baseWinRms = 0.0;  // 6.4: RMS en la ventana de régimen (t = 1 s)
    size_t winZeros = 0;       // muestras exactamente 0 en la ventana
    bool bitExact = false;     // solo para el control neutro
    bool finite = true;
    bool kernelEngaged = false;  // 6.4: kernel LTI D10 enganchado al final
    bool morphComplete = false;  // 6.4: morph hActive->hTarget terminado
};

// Corre un caso: controles A hasta el cambio, controles B desde el cambio.
// apexEnabled=false aísla la contribución del splitter APEX (que también
// engancha al pasar de neutro a activo y no lo cubre el wet por operador).
// beforeHook corre tras prepare() (p. ej. apagar el D10 antes del stream);
// changeHook corre en el punto de cambio, ANTES de setControls(after)
// (p. ej. encender/apagar el D10 en caliente). Ambos pueden ser nulos.
LayerResult runCaseHook(const std::string& name, const FusedControls& before,
                        const FusedControls& after, bool apexEnabled,
                        const std::function<void(FusedChain&)>& beforeHook,
                        const std::function<void(FusedChain&)>& changeHook) {
    std::vector<double> inL, inR;
    makeSignal(inL, inR);
    std::vector<double> l = inL, r = inR;

    FusedChain chain;
    chain.prepare(kFs);
    chain.setApexEnabled(apexEnabled);
    if (beforeHook) beforeHook(chain);
    chain.setControls(before);

    std::vector<double> cbUs;
    cbUs.reserve(kN / kHop + 1);
    double cbChangeUs = 0.0;
    bool changed = false;
    for (size_t off = 0; off < kN; off += static_cast<size_t>(kHop)) {
        const int chunk = static_cast<int>(std::min<size_t>(kHop, kN - off));
        if (!changed && off >= kChangeAt) {
            if (changeHook) changeHook(chain);
            chain.setControls(after);
            changed = true;
        }
        const auto t0 = std::chrono::steady_clock::now();
        chain.process(l.data() + off, r.data() + off, chunk);
        const auto t1 = std::chrono::steady_clock::now();
        const double us =
            std::chrono::duration<double, std::micro>(t1 - t0).count();
        cbUs.push_back(us);
        if (off == kChangeAt) cbChangeUs = us;
    }

    LayerResult res;
    res.name = name;
    res.stepDb = maxStepDb(l, r, kChangeAt, kChangeAt + 2 * kHop);
    res.baselineDb = maxStepDb(l, r, kBaselineAt, kBaselineAt + kHop);
    res.excessDb = res.stepDb - res.baselineDb;
    res.inStepDb = maxStepDb(inL, inR, kChangeAt, kChangeAt + 2 * kHop);
    res.excessVsInputDb = res.stepDb - res.inStepDb;
    // Estadística de la salida en la ventana del cambio.
    {
        double mn = 1e30, mx = -1e30, mean = 0.0, sq = 0.0;
        size_t zeros = 0, cnt = 0;
        for (size_t n = kChangeAt; n < kChangeAt + 2 * kHop && n < kN; ++n) {
            for (int ch = 0; ch < 2; ++ch) {
                const double v = ch == 0 ? l[n] : r[n];
                mn = std::min(mn, v); mx = std::max(mx, v);
                mean += v; sq += v * v;
                if (v == 0.0) ++zeros;
                ++cnt;
            }
        }
        res.winMin = mn; res.winMax = mx;
        res.winMean = cnt ? mean / cnt : 0.0;
        res.winRms = cnt ? std::sqrt(sq / cnt) : 0.0;
        res.winZeros = zeros;
    }
    double peak = 0.0;
    res.finite = true;
    for (size_t n = 0; n < kN; ++n) {
        if (!std::isfinite(l[n]) || !std::isfinite(r[n])) res.finite = false;
        peak = std::max(peak, std::fabs(l[n]));
        peak = std::max(peak, std::fabs(r[n]));
    }
    // 6.4: RMS en la ventana de régimen (misma longitud que la del cambio):
    // el guarda de hueco compara winRms contra esta referencia.
    {
        double sq = 0.0;
        size_t cnt = 0;
        for (size_t n = kBaselineAt; n < kBaselineAt + kHop && n < kN; ++n) {
            sq += l[n] * l[n] + r[n] * r[n];
            cnt += 2;
        }
        res.baseWinRms = cnt ? std::sqrt(sq / cnt) : 0.0;
    }
    res.peakDb = dbfs(peak);
    std::sort(cbUs.begin(), cbUs.end());
    double sum = 0.0;
    for (double v : cbUs) sum += v;
    res.cbMeanUs = cbUs.empty() ? 0.0 : sum / cbUs.size();
    res.cbP99Us = cbUs.empty() ? 0.0 : cbUs[static_cast<size_t>(0.99 * (cbUs.size() - 1))];
    res.cbMaxUs = cbUs.empty() ? 0.0 : cbUs.back();
    res.cbChangeUs = cbChangeUs;
    res.bitExact = (l == inL && r == inR);
    res.kernelEngaged = chain.d10Engine().ltiKernel().engaged();
    res.morphComplete = chain.d10Engine().ltiKernel().morphComplete();
    return res;
}

// Corre un caso: controles A hasta el cambio, controles B desde el cambio.
// apexEnabled=false aísla la contribución del splitter APEX (que también
// engancha al pasar de neutro a activo y no lo cubre el wet por operador).
LayerResult runCase(const std::string& name, const FusedControls& before,
                    const FusedControls& after, bool apexEnabled = true) {
    return runCaseHook(name, before, after, apexEnabled, nullptr, nullptr);
}

void printRow(const LayerResult& r) {
    std::printf("%-18s step %8s dBFS  base %8s  excess %7s dB  vsIn %7s dB  peak %8s  "
                "cb mean %7.1f p99 %7.1f max %7.1f chg %7.1f us  win[%s..%s] rms %s baseRms %s zeros %zu kern %d morph %d%s\n",
                r.name.c_str(), fmt(r.stepDb).c_str(), fmt(r.baselineDb).c_str(),
                fmt(r.excessDb).c_str(), fmt(r.excessVsInputDb).c_str(),
                fmt(r.peakDb).c_str(), r.cbMeanUs,
                r.cbP99Us, r.cbMaxUs, r.cbChangeUs,
                fmt(r.winMin).c_str(), fmt(r.winMax).c_str(), fmt(r.winRms).c_str(),
                fmt(r.baseWinRms).c_str(),
                r.winZeros, static_cast<int>(r.kernelEngaged),
                static_cast<int>(r.morphComplete), r.finite ? "" : "  [NO FINITO]");
}

}  // namespace

TEST(TronidosLayerStep, AllLayers) {
    std::vector<LayerResult> results;
    const FusedControls neutral = [] {
        FusedControls c;
        c.setNeutral();
        return c;
    }();
    const FusedControls engaged = engagedControls();

    // 1. Control: neutro → neutro (debe ser bit-exacto).
    results.push_back(runCase("neutral_neutral", neutral, neutral));
    // 2. Re-empuje idéntico: el evento real del tronido.
    results.push_back(runCase("repush_identical", engaged, engaged));
    // 3. Encendido completo.
    results.push_back(runCase("ivanna_on", neutral, engaged));
    // 4-10. Una capa a la vez desde neutro.
    for (int op = 0; op < 7; ++op) {
        results.push_back(runCase(std::string("layer_on_") + kOpName[op],
                                  neutral, operatorOnlyControls(op)));
    }
    // 11-20. Un parámetro cambiado dentro de IVANNA.
    {
        FusedControls c = engaged; c.spatial.amount = 0.0f; c.clamp();
        results.push_back(runCase("step_orbe_off", engaged, c));
    }
    {
        FusedControls c = engaged; c.field.amount = 0.0f; c.clamp();
        results.push_back(runCase("step_holos_off", engaged, c));
    }
    {
        FusedControls c = engaged; c.field.phiAmount = 0.0f; c.clamp();
        results.push_back(runCase("step_phi_off", engaged, c));
    }
    {
        FusedControls c = engaged; c.listenerRoom.roomWet = 0.0f; c.clamp();
        results.push_back(runCase("step_room_off", engaged, c));
    }
    {
        FusedControls c = engaged; c.transducer.correction = 0.0f; c.clamp();
        results.push_back(runCase("step_volterra_off", engaged, c));
    }
    {
        FusedControls c = engaged; c.tonal.lowDb = -2.0f; c.clamp();
        results.push_back(runCase("step_eq_low", engaged, c));
    }
    {
        FusedControls c = engaged; c.dynamics.compThresholdDb = -24.0f; c.clamp();
        results.push_back(runCase("step_comp_thresh", engaged, c));
    }
    {
        FusedControls c = engaged; c.dynamics.exciter = 0.55f; c.clamp();
        results.push_back(runCase("step_exciter_on", engaged, c));
    }
    {
        FusedControls c = engaged; c.brain.adaptation = 0.0f; c.clamp();
        results.push_back(runCase("step_brain_off", engaged, c));
    }
    {
        FusedControls c = engaged;
        c.conservation.amount = 0.5f; c.conservation.energyCap = 0.8f; c.clamp();
        results.push_back(runCase("step_consv_on", engaged, c));
    }
    // 21. Cambio de ruta HEADPHONES → SPEAKERS (se engancha TΩ).
    {
        FusedControls c = engaged;
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; c.clamp();
        results.push_back(runCase("mode_speakers", engaged, c));
    }

    std::printf("\n=== TRONIDOS — salto por capa (cambio a t=2.0 s, hop 128) ===\n");
    std::printf("Señal: senos 110/440/1000/3000 Hz, pico ~-12 dBFS, 4 s @ 48 kHz.\n");
    std::printf("excess = salto en el bloque del cambio menos slew natural en régimen.\n");
    for (const auto& r : results) printRow(r);
    std::printf("=== fin ===\n");

    std::ofstream jf("test_tronidos_layer_step.json");
    jf << "{\n  \"cases\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        jf << "    {\"name\": \"" << r.name << "\", \"step_dbfs\": " << fmt(r.stepDb)
           << ", \"baseline_dbfs\": " << fmt(r.baselineDb)
           << ", \"excess_db\": " << fmt(r.excessDb)
           << ", \"in_step_dbfs\": " << fmt(r.inStepDb)
           << ", \"excess_vs_input_db\": " << fmt(r.excessVsInputDb)
           << ", \"peak_dbfs\": " << fmt(r.peakDb)
           << ", \"cb_mean_us\": " << fmt(r.cbMeanUs)
           << ", \"cb_p99_us\": " << fmt(r.cbP99Us)
           << ", \"cb_max_us\": " << fmt(r.cbMaxUs)
           << ", \"cb_change_us\": " << fmt(r.cbChangeUs)
           << ", \"win_min\": " << fmt(r.winMin)
           << ", \"win_max\": " << fmt(r.winMax)
           << ", \"win_rms\": " << fmt(r.winRms)
           << ", \"win_zeros\": " << r.winZeros
           << ", \"finite\": " << (r.finite ? "true" : "false") << "}"
           << (i + 1 < results.size() ? "," : "") << "\n";
    }
    jf << "  ]\n}\n";
    jf.close();

    // Guardas: el control neutro es bit-exacto y todo es finito.
    // (El excess del neutro no es 0 exacto: el slew propio de la entrada
    // varía ±0.5 dB entre bloques; la prueba es la bit-exactitud, no el excess.)
    EXPECT_TRUE(results[0].bitExact) << "neutral_neutral no es bit-exacto";
    EXPECT_NEAR(results[0].excessVsInputDb, 0.0, 1e-9)
        << "neutral_neutral: el salto de salida difiere del de entrada";
    for (const auto& r : results) {
        EXPECT_TRUE(r.finite) << r.name << ": salida no finita";
    }
}

// ── Fix 2026-11-22 (tronidos 493) ──────────────────────────────────────────
// Tras el fix (wet por operador de 15 ms + fundido de modo con historia
// caliente de TΩ), el enganche de capas debe quedar en ~0 dB de excess
// (el control neutro mide +0.43 dB de variación propia de la entrada) y
// el cambio de modo no debe dejar bloques de 0 exacto (antes: 512/512
// muestras en 0, 5.33 ms de silencio).
TEST(TronidosLayerStep, FixEngageNoStepAndModeNoSilence) {
    const FusedControls neutral = [] {
        FusedControls c;
        c.setNeutral();
        return c;
    }();
    const FusedControls engaged = engagedControls();

    std::vector<LayerResult> r;
    // Enganche completo y por capa desde neutro.
    r.push_back(runCase("fix_ivanna_on", neutral, engaged));
    for (int op = 0; op < 7; ++op) {
        r.push_back(runCase(std::string("fix_layer_on_") + kOpName[op],
                            neutral, operatorOnlyControls(op)));
    }
    // Enganche de una capa DENTRO de la cadena enganchada (el caso real
    // del tronido: una capa se enciende mientras las demás suenan).
    {
        FusedControls before = engaged; before.spatial.amount = 0.0f; before.clamp();
        r.push_back(runCase("fix_orbe_on_engaged", before, engaged));
    }
    {
        FusedControls before = engaged; before.field.amount = 0.0f; before.clamp();
        r.push_back(runCase("fix_holos_on_engaged", before, engaged));
    }
    {
        FusedControls before = engaged; before.field.phiAmount = 0.0f; before.clamp();
        r.push_back(runCase("fix_phi_on_engaged", before, engaged));
    }
    // Cambio de modo HEADPHONES→SPEAKERS y vuelta, y encendido en SPEAKERS.
    {
        FusedControls c = engaged;
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; c.clamp();
        r.push_back(runCase("fix_mode_speakers", engaged, c));
    }
    {
        FusedControls spk = engaged;
        spk.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; spk.clamp();
        r.push_back(runCase("fix_mode_headphones", spk, engaged));
    }
    {
        FusedControls spk = engaged;
        spk.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; spk.clamp();
        r.push_back(runCase("fix_ivanna_on_speakers", neutral, spk));
    }

    std::printf("\n=== TRONIDOS FIX — enganche sin escalón / modo sin silencio ===\n");
    for (const auto& x : r) printRow(x);
    std::printf("=== fin ===\n");

    for (const auto& x : r) {
        EXPECT_TRUE(x.finite) << x.name << ": salida no finita";
    }
    // Enganches (índices 0..10): excess ~0 dB (control neutro: +0.43 dB).
    for (size_t i = 0; i <= 10; ++i) {
        EXPECT_LE(r[i].excessDb, 1.0) << r[i].name << ": escalón de enganche";
    }
    // Modos (índices 11..13): sin bloques de 0 exacto y sin escalón.
    for (size_t i = 11; i < r.size(); ++i) {
        EXPECT_LE(r[i].winZeros, 8u) << r[i].name << ": bloques de silencio exacto";
        EXPECT_LE(r[i].excessDb, 2.0) << r[i].name << ": escalón de cambio de modo";
    }
}

// ── Diagnóstico: cuánto del escalón de enganche es el APEX ────────────────
// El splitter APEX también engancha al pasar de neutro a activo (y no lo
// cubre el wet por operador: el transitorio se suma después de los
// operadores). Si con APEX apagado el excess cae a ~0, el residuo es el
// APEX y el fix es un wet a nivel de cadena; si ARMONÍA sigue alta, el
// residuo es propio del operador tonal (EvoEQ).
TEST(TronidosLayerStep, ApexOffProbe) {
    const FusedControls neutral = [] {
        FusedControls c;
        c.setNeutral();
        return c;
    }();
    const FusedControls engaged = engagedControls();

    std::vector<LayerResult> r;
    r.push_back(runCase("apexoff_ivanna_on", neutral, engaged, false));
    r.push_back(runCase("apexoff_layer_on_neuro", neutral, operatorOnlyControls(0), false));
    r.push_back(runCase("apexoff_layer_on_orbe", neutral, operatorOnlyControls(1), false));
    r.push_back(runCase("apexoff_layer_on_holos", neutral, operatorOnlyControls(2), false));
    r.push_back(runCase("apexoff_layer_on_sala", neutral, operatorOnlyControls(3), false));
    r.push_back(runCase("apexoff_layer_on_volterra", neutral, operatorOnlyControls(4), false));
    r.push_back(runCase("apexoff_layer_on_armonia", neutral, operatorOnlyControls(5), false));
    r.push_back(runCase("apexoff_layer_on_pulso", neutral, operatorOnlyControls(6), false));

    std::printf("\n=== TRONIDOS FIX — sonda APEX apagado (enganche desde neutro) ===\n");
    for (const auto& x : r) printRow(x);
    std::printf("=== fin ===\n");
    for (const auto& x : r) EXPECT_TRUE(x.finite) << x.name << ": salida no finita";
}

// ── 6.4 (refinamiento 2026-11-26) — cambio de coeficientes / D10 en caliente ─
// Auditoría previa (código, no suposición):
//   ARMONÍA: RoomCorrectionEq::setTarget() solo guarda objetivos; process()
//     acerca cur_ a tgt_ a 0.25 dB POR BLOQUE (kSlewDbPerBlock) y re-calcula
//     los biquads RBJ una vez por bloque con la ganancia ya limitada. NO hay
//     rampa por muestra de coeficientes, pero el escalón por bloque es de
//     0.25 dB como máximo: este test mide si ese escalón es un clic.
//   D10: el kernel LTI tiene morph por muestra (512 muestras, morphAlpha_)
//     pero la conmutación espectral→LTI es una rama dura por chunk
//     (fused_chain.cpp, TΩ/LTI) y el anillo del kernel NACE FRÍO (la historia
//     espectral no se alimenta mientras el LTI corre: warmTOmegaHistory solo
//     corre en la rama else). El cambio de modo HEADPHONES→SPEAKERS sí está
//     cubierto por el fundido de modo (256 muestras fuera / 512 dentro).
//     El toggle de D10 solo (setConfig en caliente) NO tiene llamador en
//     producción (solo FusedChain::prepare lo fija): se mide igual para
//     dejar el comportamiento documentado.
// Casos (todos con música estacionaria, cambio a t = 2.0 s):
//   hot_eq_max_swing — low +12 → -12 dB (24 dB: el mayor salto posible;
//     96 bloques de slew). Si el escalón de 0.25 dB/bloque es clic, aquí sale.
//   hot_eq_curve_flip — curve 1 (Harman) → 2 (élite-lean): cambian los 10
//     objetivos a la vez por el delta élite−Harman.
//   hot_d10_disable — SPEAKERS + D10 ON → D10 OFF en caliente: la rama cae
//     a applyTOmega con la historia espectral FRÍA (no se alimentó mientras
//     el LTI corría). Mide el hueco/clic de esa vuelta.
//   hot_d10_enable_via_mode — HEADPHONES + D10 OFF → SPEAKERS + D10 ON: el
//     kernel se engancha desde anillo frío DURANTE el fundido de modo.
//   hot_d10_enable_alone — SPEAKERS + D10 OFF → D10 ON sin cambio de modo:
//     sin recompilación de TΩ el kernel NO se engancha (el toggle solo es
//     no-op audible); documenta que encender el D10 en caliente no suena
//     hasta el próximo cambio de modo/reset.
// Umbral: excess en [-6.0, +2.0] dB (el piso caza huecos: un excess de
// -22.8 dB es silencio, no "ausencia de clic"), winRms ≥ 50% del RMS en
// régimen (−6 dB) y ≤ 8 muestras en 0 exacto en la ventana del cambio.
// Fix 6.4 aplicado en fused_chain.cpp: warmTOmegaHistory ANTES del kernel
// LTI (historia espectral caliente mientras el LTI corre).
TEST(TronidosLayerStep, HotCoefficientSwitchNoClick) {
    const FusedControls neutral = [] {
        FusedControls c;
        c.setNeutral();
        return c;
    }();
    const FusedControls engaged = engagedControls();
    (void)neutral;

    const auto d10Off = [](FusedChain& chain) {
        d10::D10Config cfg;
        cfg.enabled = false;
        chain.d10Engine().setConfig(cfg);
    };
    const auto d10On = [](FusedChain& chain) {
        d10::D10Config cfg;
        cfg.enabled = true;
        chain.d10Engine().setConfig(cfg);
    };

    std::vector<LayerResult> r;
    // 1. ARMONÍA: mayor salto de EQ posible en caliente.
    {
        FusedControls before = engaged; before.tonal.lowDb = 12.0f; before.clamp();
        FusedControls after = engaged; after.tonal.lowDb = -12.0f; after.clamp();
        r.push_back(runCase("hot_eq_max_swing", before, after));
    }
    // 2. ARMONÍA: cambio de curva (los 10 objetivos a la vez).
    {
        FusedControls before = engaged; before.tonal.curve = 1; before.clamp();
        FusedControls after = engaged; after.tonal.curve = 2; after.clamp();
        r.push_back(runCase("hot_eq_curve_flip", before, after));
    }
    // 3. D10: apagar en caliente en SPEAKERS (vuelta a la ruta espectral fría).
    {
        FusedControls spk = engaged;
        spk.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; spk.clamp();
        r.push_back(runCaseHook("hot_d10_disable", spk, spk, true, nullptr, d10Off));
    }
    // 4. D10: encender vía cambio de modo (enganche con fundido de modo).
    {
        FusedControls spk = engaged;
        spk.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; spk.clamp();
        r.push_back(runCaseHook("hot_d10_enable_via_mode", engaged, spk, true,
                                d10Off, d10On));
    }
    // 5. D10: encender solo, sin cambio de modo (debe ser no-op audible).
    {
        FusedControls spk = engaged;
        spk.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS; spk.clamp();
        r.push_back(runCaseHook("hot_d10_enable_alone", spk, spk, true,
                                d10Off, d10On));
    }

    std::printf("\n=== TRONIDOS 6.4 — coeficientes / D10 en caliente ===\n");
    for (const auto& x : r) printRow(x);
    std::printf("=== fin ===\n");

    for (const auto& x : r) {
        EXPECT_TRUE(x.finite) << x.name << ": salida no finita";
        EXPECT_LE(x.excessDb, 2.0) << x.name << ": escalón en caliente (clic)";
        // 6.4: el excess TAMBIÉN tiene piso — un desplome del salto máximo
        // es un hueco de silencio, no "ausencia de clic": hot_d10_disable
        // medía excess -22.8 dB (salida ~-25 dB RMS 512 muestras) y el
        // guarda superior lo dejaba pasar. El guarda de RMS (winRms ≥ 50%
        // del RMS en régimen, −6 dB) caza el hueco directamente.
        EXPECT_GE(x.excessDb, -6.0) << x.name << ": hueco en el cambio (el salto máximo se desploma)";
        EXPECT_GE(x.winRms, 0.5 * x.baseWinRms)
            << x.name << ": hueco de silencio en el cambio (RMS cae > 6 dB vs régimen)";
        EXPECT_LE(x.winZeros, 8u) << x.name << ": hueco de silencio en el cambio";
    }
    // 4: el kernel debe quedar enganchado tras el cambio de modo con D10 ON.
    EXPECT_TRUE(r[3].kernelEngaged) << "hot_d10_enable_via_mode: el kernel LTI no se enganchó";
    EXPECT_TRUE(r[3].morphComplete) << "hot_d10_enable_via_mode: el morph no completó en 2 s";
    // 5: sin recompilación de TΩ el toggle solo NO engancha el kernel.
    EXPECT_FALSE(r[4].kernelEngaged) << "hot_d10_enable_alone: el kernel se enganchó sin cambio de modo";
}
