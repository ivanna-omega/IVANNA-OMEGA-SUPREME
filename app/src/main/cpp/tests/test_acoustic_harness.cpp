// test_acoustic_harness.cpp — HARNESS ACÚSTICO OFFLINE (2026-11-27, plan ajustado).
//
// Laboratorio offline: reproduce CON NÚMEROS la degradación al subir HOLOS +
// hiperoperador (Φ). No opina: instancia la FusedChain REAL (mismo núcleo
// que Ruta A y Ruta B), inyecta 5 estímulos deterministas y mide 6 métricas
// por estímulo. Los umbrales están escritos abajo con su justificación en
// una línea (medido del proyecto o estándar; nada inventado).
//
// CONFIGURACIONES (FusedControls; modo HEADPHONES en todas: TΩ/D10 son
// no-op en HEADPHONES, así que el harness mide la cadena serial viva):
//   N  neutro (control: debe ser bit-exacto).
//   F0 campo solo, HOLOS 1.0 + Φ 0.0 (HOLOS+Omni sin Phi).
//   F  campo solo, HOLOS 1.0 + Φ 1.0 (MÁXIMO).
//   L  cadena completa al máximo (ORBE 1.0 + HOLOS 1.0 + Φ 1.0 + resto a
//      valores vivos: sala wet 0.22, Volterra 1.0, Harman, safety 1.0,
//      cerebro AUTO). Es "como lo oye Esse" con el campo al máximo.
// Los valores atrevidos llegan solos: FusedField::pushParamsToEngines pone
// spread 1.2 / proximity 6.0 dB / diffuse 0.85 por default (fused_field.cpp)
// y FieldControls::clamp limita harmonics a 0.002 (la guarda validada).
//
// ESTÍMULOS (48 kHz, doble, dual-mono L=R; 2.5 s: 0.5 s de pre-roll para
// asentar rampas/STFT + 2.0 s de ventana de medida):
//   tono 1 kHz −3 dBFS · barrido log 20 Hz–20 kHz −3 dBFS · ruido rosa
//   RMS −12 dBFS (filtro Kellet, semilla fija) · impulso 1.0 tras 0.5 s
//   de silencio · música sintética (5 senos + trémolo 0.5 Hz, pico −6 dBFS).
// La música sintética NO es una grabación: es un compuesto determinista;
// el caso con audio real queda para el Golden-Master.
//
// MÉTRICAS (por estímulo y configuración; L/R donde aplica):
//   clipping (muestras > 0 dBFS) · True Peak BS.1770 (IvannaLab, 4x) ·
//   THD+N (fused_measure, mínimos cuadrados, piso ~−310 dB) · tono
//   emergente (barrido FFT 65536 Hann: pico espurio más fuerte vs
//   fundamental) · crest factor · balance L/R · fase intercanal
//   (correlación lag 0 + mejor lag ±64 muestras) · ringing del impulso ·
//   CPU por hop (128 muestras).
//
// UMBRALES (cada uno con su justificación; las puertas duras son las
// constantes k*Max*, la caracterización va en expectCommon):
//   clipping: 0 muestras > 0 dBFS (límite duro digital, por definición).
//   True Peak: ≤ 0.0 dBTP (BS.1770: sobre 0 dBTP hay clipping inter-muestra).
//   THD+N: ≤ −200 dB SOLO en neutro (meta del proyecto, test_double_floor).
//     En configuraciones enganchadas el THD+N lo dominaba el CARÁCTER
//     intencional (tono emergente OmniField −17.32 dB a 0.5/0.5 y THD+N
//     activo −5.49/−6.48 dB, fused_chain.hpp) — PERO ambas líneas base
//     son PRE-2.5.488 (el tono murió en 2.5.488): ahí se REPORTA y el
//     paso/fallo lo decide el tono emergente. MEDIDO en g++-14
//     (2026-11-27, 20 corridas): THD+N a 1 kHz F0 −49.00 dB, F −48.79 dB,
//     L −26.36 dB — el criterio ≤ −70 dB de HOLOS NO se puede promover a
//     nivel cadena: el carácter del campo completo lo supera por diseño.
//     El criterio THD+N ≤ −70 dB de HOLOS sigue vigente a nivel operador
//     (test_holos_retune_sweep, ya verde en CI); duplicarlo aquí con el
//     campo completo sería medir el carácter, no la distorsión.
//   Tono emergente: ≤ −10 dB relativo a la fundamental. CORRECCIÓN
//     (verificado 2026-11-27): el tono a 884 Hz ("piiiii") está MUERTO
//     desde v2.5.488 (OmniField.hpp:71, kEmergentToneEnabled=false; el
//     bloque se conserva pero no se ejecuta; el campo Kuramoto sigue
//     corriendo). La línea base −17.32 dB a 0.5/0.5 (fused_chain.hpp) es
//     HISTÓRICA (medida 2026-11-20, antes del kill): el barrido del
//     harness es ahora una GUARDA DE REGRESIÓN (verifica que el tono
//     sigue muerto) y la primera corrida re-mide el THD+N real del campo
//     al máximo. El umbral −10 dB sigue valiendo: si el tono volviera
//     por encima de −10 dB competiría con la fundamental.
//   Crest / balance / fase: SOLO CARACTERIZACIÓN en F0/F/L (se imprimen,
//     no fallan). CORRECCIÓN 2026-11-27: el campo espacial descorrelaciona
//     POR DISEÑO — ORBE/HOLOS/Φ crean diferencias L/R intencionales
//     (HRTF, campo, fase), así que aplicarles los umbrales de ruta neutra
//     (azimut 0, dual-mono idéntico) mediría el efecto, no el defecto.
//     Los valores neutros (Δcrest ≤ 6 dB, balance ≤ 0.5 dB, |lag| ≤ 1,
//     corr@0 ≥ 0.99) siguen siendo la referencia de la ruta neutra, no
//     del campo. Medido en g++-14 (20 corridas): balance hasta −5.87 dB
//     (impulso), corr@0 0.40–0.99, lag hasta −12, Δcrest hasta −8.8 dB.
//     El Golden-Master congelará las líneas base del campo por banda.
//   CPU: media y p95 ≤ 2667 µs/hop (presupuesto hop 128 @48 kHz).
//   Techo APEX 0.9885531f (−0.1 dBFS, fused_dynamics.hpp:96): 0 muestras
//     por encima en TODAS las configuraciones de campo (F0/F/L) — puerta
//     dura en expectCommon. En F/F0 (sin limitador) el campo llega al
//     techo pero no lo cruza (barrido −0.10 dBFS en las 20 corridas).
//   Ringing del impulso: SOLO caracterización (sin línea base medida aún;
//     la primera medida ES la base del Golden-Master).
//
// LÍNEAS BASE MEDIDAS (g++-14, 2026-11-27, 20 corridas = 4 configs ×
// 5 estímulos; tabla completa en artex-pivot/frentes/harness-acustico.md):
//   neutro: THD+N −303.85 dB a 1 kHz, bit-exacto.
//   F0 tono 1 kHz: THD+N −49.00/−49.58 dB, emergente −50.33 dB.
//   F tono 1 kHz: THD+N −48.79/−49.32 dB, emergente −50.32 dB.
//   L tono 1 kHz: THD+N −26.36/−26.49 dB, emergente −45.96 dB @ 3000 Hz.
//   clip 0 y overCeil 0 en las 20 corridas; True Peak ≤ −0.10 dBTP.
//   CPU música: F media 621 µs / p95 508 µs; L media 1129 µs / p95 1170 µs.
//   Ringing del impulso: F0 3.4 ms, F 3.5 ms, L 63.1 ms.
//
// LO QUE ESTE HARNESS NO CUBRE (ver frentes/harness-acustico.md):
//   ruta SPEAKERS/TΩ · firmas espectrales por banda (Golden-Master) ·
//   audio real grabado · telemetría del teléfono (SnapshotV18 JSON).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
#include "ivannalab/ivannalab.h"
#include "spatial/fft_radix2.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {
constexpr double kFs = 48000.0;
constexpr int kTotalSamples = 120000;    // 2.5 s
constexpr int kPreRollSamples = 24000;    // 0.5 s de asentamiento
constexpr int kWindowSamples = 96000;     // 2.0 s de medida
constexpr int kChunkFrames = 128;         // hop vivo
constexpr double kTwoPi = 6.28318530717958647692;
constexpr float kApexCeiling = 0.9885531f;  // fused_dynamics.hpp:96

// ── Umbrales (justificación en el encabezado) ─────────────────────────────
// Puertas duras en TODAS las configuraciones de campo (F0/F/L). Los valores
// de caracterización de ruta neutra (Δcrest 6 dB, balance 0.5 dB, lag 1,
// corr 0.99) viven solo en el encabezado: el campo los supera por diseño.
constexpr double kTruePeakMaxDbTP = 0.0;    // BS.1770
constexpr double kThdMaxDbNeutral = -200.0; // meta del proyecto
constexpr double kEmergentToneMaxDb = -10.0;  // 7 dB sobre base −17.32 dB @0.5/0.5
constexpr double kCpuMeanMaxUs = 2667.0;    // hop 128 @48 kHz
constexpr double kCpuP95MaxUs = 2667.0;

enum class Stimulus { kTone, kSweep, kPink, kImpulse, kMusic };

const char* stimulusName(Stimulus s) noexcept {
    switch (s) {
        case Stimulus::kTone: return "tono1k";
        case Stimulus::kSweep: return "barrido";
        case Stimulus::kPink: return "rosa";
        case Stimulus::kImpulse: return "impulso";
        case Stimulus::kMusic: return "musica";
    }
    return "?";
}

// ── Generadores deterministas (mono; la entrada es dual-mono L=R) ─────────
std::vector<double> makeTone() {
    std::vector<double> v(static_cast<std::size_t>(kTotalSamples));
    fillToneDouble(v.data(), v.size(), kFs, 1000.0, 0.7071);
    return v;
}

std::vector<double> makeSweep() {
    std::vector<double> v(static_cast<std::size_t>(kTotalSamples));
    const double f0 = 20.0, f1 = 20000.0, tTotal = kTotalSamples / kFs;
    const double logRatio = std::log(f1 / f0);
    for (int i = 0; i < kTotalSamples; ++i) {
        const double t = i / kFs;
        const double phase = kTwoPi * f0 * tTotal * (std::pow(f1 / f0, t / tTotal) - 1.0) / logRatio;
        v[static_cast<std::size_t>(i)] = 0.7071 * std::sin(phase);
    }
    return v;
}

std::uint64_t xorshift64star(std::uint64_t& s) noexcept {
    s ^= s >> 12; s ^= s << 25; s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
}

std::vector<double> makePink() {
    // Ruido rosa, filtro de Paul Kellet (b0..b6), semilla fija.
    std::vector<double> v(static_cast<std::size_t>(kTotalSamples));
    std::uint64_t seed = 0x123456789ABCDEFULL;
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    double power = 0.0;
    for (int i = 0; i < kTotalSamples; ++i) {
        const double white = (static_cast<double>(xorshift64star(seed) >> 11) /
                              static_cast<double>(1ULL << 53)) * 2.0 - 1.0;
        b0 = 0.99886 * b0 + white * 0.0555179;
        b1 = 0.99332 * b1 + white * 0.0750759;
        b2 = 0.96900 * b2 + white * 0.1538520;
        b3 = 0.86650 * b3 + white * 0.3104856;
        b4 = 0.55000 * b4 + white * 0.5329522;
        b5 = -0.7616 * b5 - white * 0.0168980;
        const double pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362;
        b6 = white * 0.115926;
        v[static_cast<std::size_t>(i)] = pink;
        power += pink * pink;
    }
    const double rms = std::sqrt(power / kTotalSamples);
    const double target = std::pow(10.0, -12.0 / 20.0);  // RMS −12 dBFS
    for (double& x : v) x *= target / rms;
    return v;
}

std::vector<double> makeImpulse() {
    std::vector<double> v(static_cast<std::size_t>(kTotalSamples), 0.0);
    v[static_cast<std::size_t>(kPreRollSamples)] = 1.0;
    return v;
}

std::vector<double> makeMusic() {
    std::vector<double> v(static_cast<std::size_t>(kTotalSamples));
    const double freqs[5] = {110.0, 220.0, 440.0, 880.0, 1760.0};
    const double amps[5] = {0.30, 0.22, 0.16, 0.10, 0.06};
    double peak = 0.0;
    for (int i = 0; i < kTotalSamples; ++i) {
        const double t = i / kFs;
        const double tremolo = 0.75 + 0.25 * std::sin(kTwoPi * 0.5 * t);
        double s = 0.0;
        for (int k = 0; k < 5; ++k)
            s += amps[k] * std::sin(kTwoPi * freqs[k] * t + 0.7 * k);
        v[static_cast<std::size_t>(i)] = s * tremolo;
        const double a = v[static_cast<std::size_t>(i)] < 0.0
                             ? -v[static_cast<std::size_t>(i)] : v[static_cast<std::size_t>(i)];
        if (a > peak) peak = a;
    }
    const double target = std::pow(10.0, -6.0 / 20.0);  // pico −6 dBFS
    for (double& x : v) x *= target / peak;
    return v;
}

std::vector<double> makeStimulus(Stimulus s) {
    switch (s) {
        case Stimulus::kTone: return makeTone();
        case Stimulus::kSweep: return makeSweep();
        case Stimulus::kPink: return makePink();
        case Stimulus::kImpulse: return makeImpulse();
        case Stimulus::kMusic: return makeMusic();
    }
    return {};
}

// ── Configuraciones ────────────────────────────────────────────────────────
FusedControls neutralControls() {
    FusedControls c;
    c.setNeutral();
    return c;
}

FusedControls fieldOnlyControls(float phiAmount) {
    FusedControls c;
    c.setNeutral();
    c.field.amount = 1.0f;      // HOLOS al máximo
    c.field.phiAmount = phiAmount;
    c.field.harmonics = 0.002f; // guarda validada
    c.clamp();
    return c;
}

FusedControls liveMaxControls() {
    FusedControls c;  // defaults = valores vivos
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 0.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 1.0f;
    c.field.amount = 1.0f;      // HOLOS al máximo
    c.field.phiAmount = 1.0f;   // Φ al máximo
    c.field.harmonics = 0.002f; // guarda validada
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 1.0f;
    c.tonal.curve = 1;          // Harman
    c.tonal.trimDb = 0.0f;      // sin compensar: se mide la pérdida real
    c.dynamics.safety = 1.0f;   // limitador con techo APEX
    c.brain.mode = 0;           // AUTO
    c.brain.adaptation = 1.0f;
    c.conservation.amount = 0.0f;
    c.clamp();
    return c;
}

// ── Ejecución: cadena fresca por corrida, en chunks de 128 como en vivo ───
struct RunResult {
    std::vector<double> outL, outR;
    std::vector<double> cpuUs;  // µs por chunk de 128 (sin el último parcial)
};

RunResult runChain(const FusedControls& c, const std::vector<double>& in) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(c);
    RunResult r;
    r.outL = in;
    r.outR = in;
    int off = 0;
    while (off < kTotalSamples) {
        const int chunk = std::min(kChunkFrames, kTotalSamples - off);
        const auto t0 = std::chrono::steady_clock::now();
        chain.process(r.outL.data() + off, r.outR.data() + off, chunk);
        const auto t1 = std::chrono::steady_clock::now();
        if (chunk == kChunkFrames)
            r.cpuUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        off += chunk;
    }
    return r;
}

// ── Métricas ───────────────────────────────────────────────────────────────
struct Metrics {
    double peakLDb = -320.0, peakRDb = -320.0;
    std::uint32_t clipped = 0;      // |x| > 1.0 (0 dBFS)
    std::uint32_t overCeiling = 0;  // |x| > 0.9885531
    double rmsLDb = -320.0, rmsRDb = -320.0;
    double crestLDb = 0.0, crestRDb = 0.0;
    double balanceDb = 0.0;         // rmsL − rmsR (dB)
    double corr0 = 0.0;
    int bestLag = 0;
    double truePeakDbTP = -320.0;
    double thdLDb = -320.0, thdRDb = -320.0;  // solo tono
    double emergentDb = -320.0, emergentHz = 0.0;  // solo tono
    double ringingMs = 0.0;         // solo impulso
    double peakDelaySamples = 0.0;  // solo impulso
    double cpuMeanUs = 0.0, cpuP95Us = 0.0, cpuMaxUs = 0.0;
    bool finite = false;
};

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
    return v[idx];
}

Metrics computeMetrics(const std::vector<double>& in, const RunResult& run, Stimulus s) {
    Metrics m;
    const double* wL = run.outL.data() + kPreRollSamples;
    const double* wR = run.outR.data() + kPreRollSamples;
    const std::size_t n = static_cast<std::size_t>(kWindowSamples);
    m.peakLDb = 20.0 * std::log10(peakAbs(wL, n) + 1e-32);
    m.peakRDb = 20.0 * std::log10(peakAbs(wR, n) + 1e-32);
    m.rmsLDb = rmsDb(wL, n);
    m.rmsRDb = rmsDb(wR, n);
    m.crestLDb = m.peakLDb - m.rmsLDb;
    m.crestRDb = m.peakRDb - m.rmsRDb;
    m.balanceDb = m.rmsLDb - m.rmsRDb;
    for (std::size_t i = 0; i < n; ++i) {
        const double aL = wL[i] < 0.0 ? -wL[i] : wL[i];
        const double aR = wR[i] < 0.0 ? -wR[i] : wR[i];
        if (aL > 1.0 || aR > 1.0) ++m.clipped;
        if (aL > kApexCeiling || aR > kApexCeiling) ++m.overCeiling;
    }
    // Fase intercanal: corr@0 + mejor lag en ±64 muestras.
    double sumLL = 0.0, sumRR = 0.0, sumLR0 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        sumLL += wL[i] * wL[i];
        sumRR += wR[i] * wR[i];
        sumLR0 += wL[i] * wR[i];
    }
    const double denom = std::sqrt(sumLL * sumRR) + 1e-32;
    m.corr0 = sumLR0 / denom;
    double bestCorr = -2.0;
    for (int lag = -64; lag <= 64; ++lag) {
        double acc = 0.0;
        const int lo = lag < 0 ? -lag : 0;
        const int hi = static_cast<int>(n) - (lag > 0 ? lag : 0);
        for (int i = lo; i < hi; ++i)
            acc += wL[static_cast<std::size_t>(i)] * wR[static_cast<std::size_t>(i + lag)];
        const double c = acc / denom;
        if (c > bestCorr) { bestCorr = c; m.bestLag = lag; }
    }
    // True Peak BS.1770 vía IvannaLab (4x, verificado en test_ivannalab).
    {
        std::vector<float> inter(static_cast<std::size_t>(kWindowSamples) * 2);
        for (int i = 0; i < kWindowSamples; ++i) {
            inter[static_cast<std::size_t>(2 * i)] = static_cast<float>(wL[i]);
            inter[static_cast<std::size_t>(2 * i + 1)] = static_cast<float>(wR[i]);
        }
        ivanna::IvannaLab lab(static_cast<std::uint32_t>(kFs), 4096);
        lab.feed(inter.data(), kWindowSamples);
        const ivanna::LabResult lr = lab.measure();
        m.truePeakDbTP = static_cast<double>(lr.truepeakDBTP);
    }
    // CPU por hop.
    if (!run.cpuUs.empty()) {
        double sum = 0.0;
        m.cpuMaxUs = 0.0;
        for (double u : run.cpuUs) {
            sum += u;
            if (u > m.cpuMaxUs) m.cpuMaxUs = u;
        }
        m.cpuMeanUs = sum / static_cast<double>(run.cpuUs.size());
        m.cpuP95Us = percentile(run.cpuUs, 0.95);
    }
    m.finite = isFiniteBuffer(run.outL.data(), run.outL.size()) &&
               isFiniteBuffer(run.outR.data(), run.outR.size());
    // Solo tono: THD+N + barrido del tono emergente.
    if (s == Stimulus::kTone) {
        const ThdDoubleResult tL = measureThdDouble(wL, n, kFs, 1000.0);
        const ThdDoubleResult tR = measureThdDouble(wR, n, kFs, 1000.0);
        m.thdLDb = tL.thdPlusNoiseDb;
        m.thdRDb = tR.thdPlusNoiseDb;
        // Barrido FFT 65536 Hann sobre L: pico espurio más fuerte fuera
        // de la fundamental ±4 bins y DC.
        constexpr int kNfft = 65536;
        ivanna::FFTRadix2d fft(kNfft);
        std::vector<double> re(static_cast<std::size_t>(kNfft)), im(static_cast<std::size_t>(kNfft), 0.0);
        for (int i = 0; i < kNfft; ++i) {
            const double w = 0.5 * (1.0 - std::cos(kTwoPi * i / (kNfft - 1)));
            re[static_cast<std::size_t>(i)] = wL[i] * w;
        }
        fft.forward(re.data(), im.data());
        const int fundBin = static_cast<int>(std::lround(1000.0 / (kFs / kNfft)));
        const double fundMag = std::hypot(re[static_cast<std::size_t>(fundBin)],
                                          im[static_cast<std::size_t>(fundBin)]);
        double bestMag = 0.0;
        int bestBin = -1;
        for (int b = 3; b < kNfft / 2; ++b) {
            if (b >= fundBin - 4 && b <= fundBin + 4) continue;
            const double mag = std::hypot(re[static_cast<std::size_t>(b)],
                                          im[static_cast<std::size_t>(b)]);
            if (mag > bestMag) { bestMag = mag; bestBin = b; }
        }
        if (bestBin > 0 && fundMag > 0.0) {
            m.emergentDb = 20.0 * std::log10(bestMag / fundMag);
            m.emergentHz = bestBin * (kFs / kNfft);
        }
    }
    // Solo impulso: ringing desde el pico de salida hasta caer bajo −60 dBFS.
    if (s == Stimulus::kImpulse) {
        double peak = 0.0;
        std::size_t peakIdx = 0;
        for (std::size_t i = 0; i < n; ++i) {
            const double aL = wL[i] < 0.0 ? -wL[i] : wL[i];
            const double aR = wR[i] < 0.0 ? -wR[i] : wR[i];
            const double a = aL > aR ? aL : aR;
            if (a > peak) { peak = a; peakIdx = i; }
        }
        std::size_t lastIdx = peakIdx;
        for (std::size_t i = peakIdx; i < n; ++i) {
            const double aL = wL[i] < 0.0 ? -wL[i] : wL[i];
            const double aR = wR[i] < 0.0 ? -wR[i] : wR[i];
            if (aL > 0.001 || aR > 0.001) lastIdx = i;
        }
        m.peakDelaySamples = static_cast<double>(peakIdx);
        m.ringingMs = static_cast<double>(lastIdx - peakIdx) / 48.0;
        (void)peak;
    }
    (void)in;
    return m;
}

double inputCrestDb(const std::vector<double>& in) {
    const double* w = in.data() + kPreRollSamples;
    const std::size_t n = static_cast<std::size_t>(kWindowSamples);
    return 20.0 * std::log10(peakAbs(w, n) + 1e-32) - rmsDb(w, n);
}

void printRow(const char* cfg, Stimulus s, const Metrics& m, double inCrestDb) {
    std::printf(
        "HARNESS %-4s %-8s peak %+.2f/%+.2f dBFS clip %u overCeil %u "
        "tp %+.2f dBTP thdn %+.2f/%+.2f dB emerg %+.2f dB @ %.0f Hz "
        "crest %+.2f/%+.2f dB (in %+.2f) bal %+.2f dB lag %+d corr %.4f "
        "ring %.1f ms cpu %.0f/%.0f/%.0f us\n",
        cfg, stimulusName(s), m.peakLDb, m.peakRDb, m.clipped, m.overCeiling,
        m.truePeakDbTP, m.thdLDb, m.thdRDb, m.emergentDb, m.emergentHz,
        m.crestLDb, m.crestRDb, inCrestDb, m.balanceDb, m.bestLag, m.corr0,
        m.ringingMs, m.cpuMeanUs, m.cpuP95Us, m.cpuMaxUs);
    std::fflush(stdout);
}

// Puertas duras comunes a toda corrida enganchada (no neutro). Crest,
// balance, lag y corr@0 son CARACTERIZACIÓN en F0/F/L (se imprimen, no
// fallan): el campo espacial descorrelaciona por diseño y sus umbrales
// de ruta neutra medirían el efecto, no el defecto (ver encabezado).
void expectCommon(const char* cfg, Stimulus s, const Metrics& m, double inCrestDb) {
    EXPECT_TRUE(m.finite) << cfg << " " << stimulusName(s) << ": salida no finita (NaN/Inf)";
    EXPECT_EQ(m.clipped, 0u) << cfg << " " << stimulusName(s)
                             << ": clipping digital (" << m.clipped << " muestras > 0 dBFS)";
    EXPECT_LE(m.truePeakDbTP, kTruePeakMaxDbTP)
        << cfg << " " << stimulusName(s) << ": True Peak sobre 0 dBTP";
    EXPECT_EQ(m.overCeiling, 0u) << cfg << " " << stimulusName(s)
                                 << ": " << m.overCeiling
                                 << " muestras sobre el techo APEX 0.9885531";
    const double dCrestL = m.crestLDb - inCrestDb;
    const double dCrestR = m.crestRDb - inCrestDb;
    std::printf("HARNESS %-4s %-8s CARACT dCrest %+.2f/%+.2f dB bal %+.2f dB lag %+d corr %.4f\n",
                cfg, stimulusName(s), dCrestL, dCrestR, m.balanceDb, m.bestLag, m.corr0);
    std::fflush(stdout);
}

const Stimulus kAllSignals[5] = {Stimulus::kTone, Stimulus::kSweep, Stimulus::kPink,
                                 Stimulus::kImpulse, Stimulus::kMusic};
}  // namespace

TEST(AcousticHarness, NeutralIsBitExact) {
    for (Stimulus s : kAllSignals) {
        const std::vector<double> in = makeStimulus(s);
        const RunResult run = runChain(neutralControls(), in);
        const NullResult nl = nullTest(in.data(), run.outL.data(), in.size());
        const NullResult nr = nullTest(in.data(), run.outR.data(), in.size());
        EXPECT_TRUE(nl.bitExact) << stimulusName(s) << " L residual " << nl.residualDb << " dB";
        EXPECT_TRUE(nr.bitExact) << stimulusName(s) << " R residual " << nr.residualDb << " dB";
        if (s == Stimulus::kTone) {
            const Metrics m = computeMetrics(in, run, s);
            EXPECT_LE(m.thdLDb, kThdMaxDbNeutral) << "neutro THD+N L " << m.thdLDb << " dB";
            EXPECT_LE(m.thdRDb, kThdMaxDbNeutral) << "neutro THD+N R " << m.thdRDb << " dB";
            printRow("N", s, m, inputCrestDb(in));
        }
    }
}

TEST(AcousticHarness, FieldOnlyMax) {
    // F0 = HOLOS+Omni sin Phi; F = HOLOS+Omni+Phi al máximo.
    const std::pair<const char*, FusedControls> cfgs[2] = {
        {"F0", fieldOnlyControls(0.0f)},
        {"F", fieldOnlyControls(1.0f)},
    };
    for (const auto& [name, c] : cfgs) {
        for (Stimulus s : kAllSignals) {
            const std::vector<double> in = makeStimulus(s);
            const RunResult run = runChain(c, in);
            const Metrics m = computeMetrics(in, run, s);
            printRow(name, s, m, inputCrestDb(in));
            expectCommon(name, s, m, inputCrestDb(in));
            if (s == Stimulus::kTone) {
                // THD+N se reporta (carácter intencional); el paso/fallo
                // de pureza tonal lo decide el tono emergente.
                EXPECT_LE(m.emergentDb, kEmergentToneMaxDb)
                    << name << " tono emergente " << m.emergentDb << " dB @ "
                    << m.emergentHz << " Hz (tono muerto desde 2.5.488; "
                    << "el umbral guarda la regresión)";
            }
        }
    }
}

TEST(AcousticHarness, LiveMaxFullChain) {
    const FusedControls c = liveMaxControls();
    for (Stimulus s : kAllSignals) {
        const std::vector<double> in = makeStimulus(s);
        const RunResult run = runChain(c, in);
        const Metrics m = computeMetrics(in, run, s);
        printRow("L", s, m, inputCrestDb(in));
        // expectCommon ya exige overCeiling 0 (techo APEX) en todas las
        // configuraciones de campo; con safety=1.0 el limitador lo respeta.
        expectCommon("L", s, m, inputCrestDb(in));
        if (s == Stimulus::kTone) {
            EXPECT_LE(m.emergentDb, kEmergentToneMaxDb)
                << "L tono emergente " << m.emergentDb << " dB @ " << m.emergentHz
                << " Hz (tono muerto desde 2.5.488; el umbral guarda la regresión)";
        }
    }
}

TEST(AcousticHarness, CpuPerHopWithinBudget) {
    const std::vector<double> in = makeMusic();
    const std::pair<const char*, FusedControls> cfgs[2] = {
        {"F", fieldOnlyControls(1.0f)},
        {"L", liveMaxControls()},
    };
    for (const auto& [name, c] : cfgs) {
        const RunResult run = runChain(c, in);
        const Metrics m = computeMetrics(in, run, Stimulus::kMusic);
        std::printf("HARNESS %-4s cpu musica media %.0f us p95 %.0f us max %.0f us (%zu chunks)\n",
                    name, m.cpuMeanUs, m.cpuP95Us, m.cpuMaxUs, run.cpuUs.size());
        std::fflush(stdout);
        EXPECT_LE(m.cpuMeanUs, kCpuMeanMaxUs)
            << name << " CPU media por hop " << m.cpuMeanUs << " us (presupuesto 2667 us)";
        EXPECT_LE(m.cpuP95Us, kCpuP95MaxUs)
            << name << " CPU p95 por hop " << m.cpuP95Us << " us (presupuesto 2667 us)";
    }
}
