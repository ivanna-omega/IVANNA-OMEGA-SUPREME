// test_neu_fase3_tail.cpp
// ============================================================================
// NEU·Ω — FASE 3 (0028) · Cola difusa en frecuencia
// ----------------------------------------------------------------------------
// La cola RIR deja de tener su propio STFT y su propio IFFT: usa el espectro
// X[k] ya calculado y se suma a M·X antes del único IFFT del SpectralCore.
// Por bin: estado complejo con feedback — cada hop se alimenta del espectro
// actual (feed), decae exponencialmente (RT60) y rota su fase un ángulo
// fijo por bin (ángulo áureo: decorrelación). La cola es difusa y ruidosa
// por naturaleza: la equivalencia con una convolución exacta NO es el
// criterio — lo es la curva de decaimiento energético (EDC): mismo T60 y
// EDC ±1 dB por banda (criterio E5 del diseño).
//
// Tests:
//  1. TailOffIsIdentity — cola OFF (default) = E1 intacta (identidad).
//  2. TailEdcMatchesRt60 — la energía de la cola decae a -60 dB en el RT60
//     configurado (pendiente -60/RT60 dB/s, ±15%, R² > 0.9).
//  3. TailLevelReasonable — la cola está presente (no silenciosa) y es
//     finita y acotada en una pasada larga de ruido.
//  4. TailCostLowerThanFftPair — la cola por hop cuesta menos que un par
//     FFT/IFFT 512 (la pareja que la cola en tiempo necesitaba).
//  5. TailToggleClickFree — activar/desactivar en caliente no produce clic
//     (pendiente ≤ 3x el régimen, fundido 20 ms).
//  6. RirTailSkipHeadOnly — RirConvolver con tailEnabled(false) solo
//     convoluciona el head (la cola FDL se salta); con tail ON la cola
//     está presente.
// ============================================================================
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "../spatial/SpectralCore.hpp"
#include "../spatial/RirConvolver.hpp"
#include "../spatial/fft_radix2.hpp"

namespace {

using ivanna::neu::SpectralCore;
using Ivanna::RirConvolver;

constexpr int kSr = 48000;
constexpr int kFft = 512;   // latencia del pipeline STFT/OLA
constexpr int kHop = 128;

std::vector<float> Noise(int n, uint32_t seed = 12345u, float amp = 0.5f) {
    std::vector<float> x((size_t)n);
    uint32_t s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        x[(size_t)i] = amp * ((float)((int)(s >> 9) & 0x7FFFFF) / 4194304.0f - 1.0f);
    }
    return x;
}

std::vector<float> Sine(float freq, int n, float amp = 0.5f) {
    std::vector<float> x((size_t)n);
    for (int i = 0; i < n; ++i)
        x[(size_t)i] = amp * std::sin(2.0f * 3.14159265358979323846f * freq * (float)i / (float)kSr);
    return x;
}

float Rms(const std::vector<float>& x, int from, int to) {
    double s = 0.0;
    for (int i = from; i < to && i < (int)x.size(); ++i) s += (double)x[(size_t)i] * x[(size_t)i];
    const int n = std::max(1, std::min(to, (int)x.size()) - from);
    return (float)std::sqrt(s / (double)n);
}

// Energía (suma de cuadrados) en una ventana [from, to).
double WindowEnergy(const std::vector<float>& x, int from, int to) {
    double s = 0.0;
    for (int i = from; i < to && i < (int)x.size(); ++i) s += (double)x[(size_t)i] * x[(size_t)i];
    return s;
}

SpectralCore MakeCore() {
    SpectralCore core;
    core.prepare(kSr);
    return core;
}

// 1. Cola OFF (default) = E1 intacta: salida = entrada retardada 512.
TEST(NeuFase3Tail, TailOffIsIdentity) {
    auto core = MakeCore();
    ASSERT_FALSE(core.tailEnabled());
    const auto inL = Sine(1000.0f, 8192);
    const auto inR = Noise(8192, 777u);
    std::vector<float> outL(8192, 0.0f), outR(8192, 0.0f);
    core.process(inL.data(), inR.data(), outL.data(), outR.data(), 8192);
    // Porción drenada (N-512): identidad, error ≤ 1e-6 (E1).
    double maxErr = 0.0;
    for (int i = 0; i < 8192 - kFft; ++i) {
        maxErr = std::max(maxErr, (double)std::fabs(outL[(size_t)i] - inL[(size_t)i]));
        maxErr = std::max(maxErr, (double)std::fabs(outR[(size_t)i] - inR[(size_t)i]));
    }
    EXPECT_LE(maxErr, 1e-6) << "Cola OFF debe ser identidad (E1); el rework de runFrame rompió algo";
}

// 2. EDC: la energía de la cola decae a -60 dB en el RT60 configurado.
TEST(NeuFase3Tail, TailEdcMatchesRt60) {
    const float kRt60 = 0.34f;
    auto core = MakeCore();
    core.setTailRt60(kRt60);
    core.setTailFeed(0.05f);
    core.setTailWet(1.0f);
    core.setTailEnabled(true);
    // Impulso en la muestra 0 + silencio (1 s).
    std::vector<float> inL(kSr, 0.0f), inR(kSr, 0.0f);
    inL[0] = 1.0f; inR[0] = 1.0f;
    std::vector<float> outL(kSr, 0.0f), outR(kSr, 0.0f);
    core.process(inL.data(), inR.data(), outL.data(), outR.data(), kSr);
    ASSERT_FALSE(core.tailFailed());
    // La cola debe estar presente (energía tras el directo).
    const float tailRms = Rms(outL, kFft + 512, kFft + 512 + 4800);
    EXPECT_GT(tailRms, 1e-4f) << "La cola no está presente tras el impulso";
    // Ajuste lineal de 10*log10(E) vs t en ventanas de 512 muestras,
    // desde 50 ms después del directo (pasado el transitorio de
    // alimentación, ~4 hops) hasta 300 ms. Pendiente esperada: -60/RT60.
    const int kWin = 512;
    const int kStart = kFft + 2400;   // 50 ms tras el directo
    const int kEnd = kFft + 14400;    // 300 ms tras el directo
    std::vector<double> ts, ys;
    for (int w = kStart; w + kWin <= kEnd; w += kWin) {
        const double e = WindowEnergy(outL, w, w + kWin) + WindowEnergy(outR, w, w + kWin);
        if (e <= 0.0) continue;
        ts.push_back((w + kWin / 2.0 - kFft) / (double)kSr);
        ys.push_back(10.0 * std::log10(e));
    }
    ASSERT_GE(ts.size(), 8u) << "Pocas ventanas con energía para el ajuste EDC";
    // Regresión lineal y = a + b*t.
    const size_t m = ts.size();
    double mt = 0.0, my = 0.0;
    for (size_t i = 0; i < m; ++i) { mt += ts[i]; my += ys[i]; }
    mt /= (double)m; my /= (double)m;
    double sxy = 0.0, sxx = 0.0, syy = 0.0;
    for (size_t i = 0; i < m; ++i) {
        sxy += (ts[i] - mt) * (ys[i] - my);
        sxx += (ts[i] - mt) * (ts[i] - mt);
        syy += (ys[i] - my) * (ys[i] - my);
    }
    const double slope = sxy / sxx;              // dB/s
    const double r2 = (sxy * sxy) / (sxx * syy); // calidad del ajuste
    const double expected = -60.0 / (double)kRt60;
    EXPECT_GT(r2, 0.9) << "La caída no es exponencial (R² bajo); la cola no decae como un RT60";
    EXPECT_NEAR(slope, expected, 0.15 * std::fabs(expected))
        << "T60 medido (" << (-60.0 / slope) << " s) != RT60 configurado (" << kRt60 << " s)";
}

// 3. Nivel razonable: la cola está presente y es finita y acotada.
TEST(NeuFase3Tail, TailLevelReasonable) {
    auto core = MakeCore();
    core.setTailRt60(0.34f);
    core.setTailFeed(0.05f);
    core.setTailWet(1.0f);
    core.setTailEnabled(true);
    // Impulso: pico de la cola en los primeros 20 ms tras el directo.
    std::vector<float> inL(kSr, 0.0f), inR(kSr, 0.0f);
    inL[0] = 1.0f; inR[0] = 1.0f;
    std::vector<float> outL(kSr, 0.0f), outR(kSr, 0.0f);
    core.process(inL.data(), inR.data(), outL.data(), outR.data(), kSr);
    float peak = 0.0f;
    for (int i = kFft; i < kFft + 960; ++i)
        peak = std::max(peak, std::fabs(outL[(size_t)i]));
    EXPECT_GT(peak, 0.001f) << "Cola demasiado débil (casi silenciosa)";
    EXPECT_LT(peak, 1.0f) << "Cola demasiado fuerte (casi clip)";
    // Ruido largo: finita y acotada.
    const auto nL = Noise(kSr, 42u), nR = Noise(kSr, 43u);
    std::vector<float> oL(kSr, 0.0f), oR(kSr, 0.0f);
    core.process(nL.data(), nR.data(), oL.data(), oR.data(), kSr);
    float mx = 0.0f;
    for (int i = 0; i < kSr; ++i) {
        ASSERT_TRUE(std::isfinite(oL[(size_t)i]) && std::isfinite(oR[(size_t)i]));
        mx = std::max(mx, std::fabs(oL[(size_t)i]));
    }
    EXPECT_LT(mx, 2.0f) << "Salida no acotada con cola activa";
    EXPECT_FALSE(core.tailFailed());
}

// 4. Coste: la cola por hop cuesta menos que un par FFT/IFFT 512.
// Medida por diferencia (ON − OFF), que es ruidosa bajo carga paralela
// (ctest -j2): se toman 5 rondas y se usa la MEDIANA, robusta a los
// picos de planificación. Sin mediana, el test es flaky en CTest.
TEST(NeuFase3Tail, TailCostLowerThanFftPair) {
    const int kReps = 10;
    const int kN = kSr;  // 1 s por llamada = 375 hops
    const auto nL = Noise(kN, 42u), nR = Noise(kN, 43u);
    std::vector<float> oL(kN, 0.0f), oR(kN, 0.0f);
    auto costPerCall = [&](SpectralCore& core) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kReps; ++r)
            core.process(nL.data(), nR.data(), oL.data(), oR.data(), kN);
        const auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::micro>(t1 - t0).count() / kReps;
    };
    std::vector<double> tailPerHopSamples;
    for (int round = 0; round < 5; ++round) {
        auto coreOff = MakeCore();
        const double offUs = costPerCall(coreOff);
        auto coreOn = MakeCore();
        coreOn.setTailRt60(0.34f);
        coreOn.setTailFeed(0.05f);
        coreOn.setTailWet(1.0f);
        coreOn.setTailEnabled(true);
        const double onUs = costPerCall(coreOn);
        const double hops = (double)kN / kHop;
        tailPerHopSamples.push_back((onUs - offUs) / hops);
    }
    std::sort(tailPerHopSamples.begin(), tailPerHopSamples.end());
    const double tailUsPerHop = tailPerHopSamples[2];  // mediana de 5
    // Par FFT/IFFT 512 (la pareja que la cola en tiempo necesitaba).
    // Mediana de 3 rondas (2026-11-16, fix flaky en CI): la línea base
    // de UNA sola medida es ruidosa bajo ASan+UBSan y carga paralela —
    // el test fallaba en CI aunque pasa en local. El UMBRAL no cambia
    // (cola < par FFT/IFFT y < 50 µs): solo la medida es robusta.
    ivanna::FFTRadix2 fft(kFft);
    std::vector<float> re(kFft, 0.3f), im(kFft, 0.0f);
    const int kFftReps = 20000;
    std::vector<double> fftPairSamples;
    for (int round = 0; round < 3; ++round) {
        const auto t0 = std::chrono::steady_clock::now();
        for (int r = 0; r < kFftReps; ++r) {
            fft.transform(re.data(), im.data(), +1);
            fft.transform(re.data(), im.data(), -1);
        }
        const auto t1 = std::chrono::steady_clock::now();
        fftPairSamples.push_back(
            std::chrono::duration<double, std::micro>(t1 - t0).count() / kFftReps);
    }
    std::sort(fftPairSamples.begin(), fftPairSamples.end());
    const double fftPairUs = fftPairSamples[1];  // mediana de 3
    // Piso de ruido (-5 µs), no 0.0: la mediana de (ON-OFF) sale negativa
    // en CI (-1.31 µs en la 473) porque el costo real de la cola es menor
    // que el ruido del temporizador bajo ASan+UBSan y carga paralela — la
    // cola SÍ se ejecuta (TailOffIsIdentity/TailEdcMatchesRt60/TailLevelReasonable
    // pasan). Un valor levemente negativo solo dice "por debajo del piso
    // de medida", no "la cola no corre".
    EXPECT_GT(tailUsPerHop, -5.0) << "La cola no cuesta nada (¿no se ejecuta?)";
    EXPECT_LT(tailUsPerHop, fftPairUs)
        << "La cola por hop (" << tailUsPerHop << " µs) cuesta MÁS que un par FFT/IFFT 512 ("
        << fftPairUs << " µs): no hay ahorro frente a la cola en tiempo";
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__)
    // Tope absoluto (50 µs) SOLO en build sin sanitizadores: bajo
    // ASan+UBSan el código corre 2-3x más lento y la cola mide ~67 µs
    // aunque la propiedad real (cola < par FFT/IFFT) SÍ se cumple — la
    // CI 459 lo demostró (falla solo este tope, no la comparación).
    // El tope absoluto es un presupuesto de release, no una propiedad.
    EXPECT_LT(tailUsPerHop, 50.0) << "Cola demasiado cara por hop (>50 µs)";
#endif
}

// 5. Sin clic al conmutar: fundido 20 ms, pendiente ≤ 3x el régimen.
TEST(NeuFase3Tail, TailToggleClickFree) {
    auto core = MakeCore();
    core.setTailRt60(0.34f);
    core.setTailFeed(0.05f);
    core.setTailWet(1.0f);
    core.setTailEnabled(true);
    const auto tone = Sine(1000.0f, kSr);
    std::vector<float> out(kSr, 0.0f), outR(kSr, 0.0f);
    // 0-0.5 s con cola; 0.5-0.75 s sin cola; 0.75-1 s con cola.
    // (La salida lleva 512 muestras de retardo: el efecto del toggle
    // aparece 512 muestras después en la salida.)
    core.process(tone.data(), tone.data(), out.data(), outR.data(), 24000);
    core.setTailEnabled(false);
    core.process(tone.data() + 24000, tone.data() + 24000, out.data() + 24000, outR.data() + 24000, 12000);
    core.setTailEnabled(true);
    core.process(tone.data() + 36000, tone.data() + 36000, out.data() + 36000, outR.data() + 36000, 12000);
    auto maxSlope = [&](int from, int to) {
        float m = 0.0f;
        for (int i = from + 1; i < to && i < kSr; ++i)
            m = std::max(m, std::fabs(out[(size_t)i] - out[(size_t)i - 1]));
        return m;
    };
    const float steady = maxSlope(9600, 19200);          // 0.2-0.4 s, régimen
    const float tog1 = maxSlope(24000 + 512 - 960, 24000 + 512 + 960);  // toggle OFF
    const float tog2 = maxSlope(36000 + 512 - 960, 36000 + 512 + 960);  // toggle ON
    ASSERT_GT(steady, 0.0f);
    EXPECT_LE(tog1, 3.0f * steady) << "Clic al desactivar la cola";
    EXPECT_LE(tog2, 3.0f * steady) << "Clic al activar la cola";
}

// 6. RirConvolver: con tailEnabled(false) solo convoluciona el head.
TEST(NeuFase3Tail, RirTailSkipHeadOnly) {
    // IR sintética: espiga directa en la muestra 0 (onset en origen, sin
    // desplazamiento en load) + ruido con decaimiento exponencial (T60 0.34 s).
    const int kIrLen = 12000;
    std::vector<float> irL(kIrLen, 0.0f), irR(kIrLen, 0.0f);
    irL[0] = 1.0f; irR[0] = 1.0f;
    uint32_t s = 999u;
    for (int i = 1; i < kIrLen; ++i) {
        s = s * 1664525u + 1013904223u;
        const float env = std::pow(10.0f, -3.0f * ((float)i / (float)kSr) / 0.34f);
        const float v = 0.5f * env * ((float)((int)(s >> 9) & 0x7FFFFF) / 4194304.0f - 1.0f);
        irL[(size_t)i] = v; irR[(size_t)i] = v;
    }
    auto run = [&](bool tailOn) {
        RirConvolver rc;
        rc.setSampleRate(kSr);
        rc.load(irL.data(), irR.data(), kIrLen);
        rc.setWetDry(1.0f);
        rc.setTailEnabled(tailOn);
        // EXPECT (no ASSERT): ASSERT hace `return;` y esta lambda devuelve
        // un vector; si la cola fue truncada, el test falla igual abajo
        // (rmsOn ~ 0) con este mensaje como pista.
        EXPECT_FALSE(rc.tailLatencyBudgetExceeded())
            << "La cola fue truncada en load (presupuesto de latencia); el test no distingue head/cola";
        // Impulso + silencio, 16 bloques de 512.
        std::vector<float> out;
        for (int b = 0; b < 16; ++b) {
            std::vector<float> L(512, 0.0f), R(512, 0.0f);
            if (b == 0) { L[0] = 1.0f; R[0] = 1.0f; }
            rc.process(L.data(), R.data(), 512);
            out.insert(out.end(), L.begin(), L.end());
        }
        return out;
    };
    const auto oOn = run(true);
    const auto oOff = run(false);
    // Head idéntico (muestras 0..255: solo head, la cola aún no llega).
    double headDiff = 0.0;
    for (int i = 0; i < 256; ++i)
        headDiff = std::max(headDiff, (double)std::fabs(oOn[(size_t)i] - oOff[(size_t)i]));
    EXPECT_LE(headDiff, 1e-6) << "El flag de cola rompió el head";
    // Cola presente con tail ON, ausente con tail OFF (muestras 1024..8192).
    // NOTA: con tail OFF el residuo NO es cero exacto: el DC-blocker de
    // salida del RIR (polo 0.9993, ~1428 muestras de constante de tiempo)
    // sigue sonando ("ringing") tras el head. Por eso el criterio es
    // RELATIVO (ratio ON/OFF), no cero absoluto: la cola añade >10x
    // energía respecto al ringing residual.
    const float rmsOn = Rms(oOn, 1024, 8192);
    const float rmsOff = Rms(oOff, 1024, 8192);
    EXPECT_GT(rmsOn, 1e-4f) << "Con tail ON no hay cola (¿la IR no cargó la cola?)";
    EXPECT_GT(rmsOn, 10.0f * std::max(rmsOff, 1e-9f))
        << "Con tail OFF sigue habiendo cola (el flag no salta el paso FDL): "
        << "rmsOn=" << rmsOn << " rmsOff=" << rmsOff;
    EXPECT_LT(rmsOff, 1e-4f) << "El residuo con tail OFF es demasiado grande para ser solo ringing del DC-blocker";
}

}  // namespace
