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
//     El Golden-Master (S7/510, tests/golden/acoustic_harness_baseline.json)
//     congela las líneas base del campo por banda.
//   CPU: media y p95 ≤ 2667 µs/hop (presupuesto hop 128 @48 kHz) — C11/510:
//     MÉTRICA REGISTRADA en hosted (se publica, no falla) y PUERTA DURA solo
//     en self-hosted (IVANNA_CPU_GATE_HARD=1). El p95 de pared mide la
//     planificación del host, no el motor: L p95 2678.53 µs vs 2667 en VM
//     compartida, re-run inmediato 1284 µs.
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
//   firmas espectrales por banda — RESUELTO en S7/510 (abajo) ·
//   audio real grabado · telemetría del teléfono (SnapshotV18 JSON).
//
// S7/510 — MODO «FIRMA ERB» + GOLDEN-MASTER (§S7 de
// evolucion-magistral-autonomia-completa.md):
//   (a) Predictor lineal: la respuesta en magnitud del EQ candidato (10
//       biquads RBJ de RoomCorrectionEq + TonalControls) se evalúa sobre
//       los 24 centros ERB con LA MISMA matemática que el motor
//       (ivanna::room::cascadeMagDb — los coeficientes RBJ de
//       AutoEqFilter::setBand) y el mapeo EXACTO de FusedTonal::
//       applyControls (bandas 0-2 lowDb, 3-6 midDb, 7-9 highDb,
//       presenceDb aditivo en 7-8, delta élite×0.5 si curve==2, todo
//       × amount; trimDb como término plano). El RMS predicho post-EQ es
//       RMS(errorMeanDb + respuesta) con los clamps del motor aplicados
//       (roomCorrDb cut-only [−12,0], slots tonales ±12; un candidato en
//       formato .ivanna.json v1.0 se pre-clampea a ±2 dB por el llamador).
//       Puerta: el RMS predicho debe bajar ≥20% respecto a la línea base
//       (10.46 dB → objetivo ≤ 8.37 dB); si no, la candidata NO se emite.
//       Es PRE-PANTALLA, no sustituto del A/B: el predictor es lineal y
//       la cadena real no lo es.
//   (b) Firma espectral por banda (Golden-Master, pendiente 2 del doc del
//       harness): en el barrido se mide por cada una de las 24 bandas ERB
//       el nivel L/R y la correlación intercanal (líneas HARNESS ERB) y se
//       congela en tests/golden/acoustic_harness_baseline.json; el script
//       scripts/check_acoustic_golden.py compara y falla si F0/F/L se
//       desvían más que la tolerancia.
//   Centros ERB: RÉPLICA EXACTA de SessionAggregator.erbCenterHz(48000)
//       (Kotlin, misma escala que RealAcousticSensor, 60 Hz–16 kHz,
//       Glasberg & Moore): 24 bandas, media geométrica de los bordes
//       equiespaciados en tasa ERB. Verificación: la banda 3 da 209 Hz,
//       el único centro medido/documentado (ERB-3 209 Hz, perfil v1.1).
//   Pendientes (NO inventados aquí): ruta SPEAKERS/TΩ (TΩ/D10 son no-op
//       en HEADPHONES por puerta; medirlos exige una configuración
//       SPEAKERS que hoy no existe en el harness) · audio real grabado
//       (la «música» sigue siendo sintética) · la tabla ERB completa de
//       24 bandas del servidor: solo ERB-3 está documentada (+9.86 dB @
//       209 Hz); sin esa exportación la puerta del §S7 para la candidata
//       v1.1 es INDECIDIBLE (ver ErbSignaturePredictsRms) — no se
//       inventa el vector.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
#include "ivannalab/ivannalab.h"
#include "spatial/RoomProbe.hpp"  // S7/510: cascadeMagDb (misma RBJ que AutoEqFilter)
#include "spatial/EliteTargetCurves.hpp"  // S7/510: delta élite (curve==2)
#include "spatial/fft_radix2.hpp"

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

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

// C11/510 — cuarentena flaky (wall-clock): la CPU por hop es métrica
// registrada en hosted y puerta dura SOLO en self-hosted, donde la carga es
// conocida. Opt-in explícito por variable de entorno (default: métrica, no
// falla). La variable la ponen los workflows que corren en self-hosted
// (tests-quarantined-weekly.yml, job self-hosted; en tests-selfhosted.yml
// es línea pendiente — ver reporte C11). Bajo sanitizador nunca es puerta
// dura (el sanitizador infla el CPU 2-3x por diseño; ya estaba así).
bool CpuGateHard() {
    const char* v = std::getenv("IVANNA_CPU_GATE_HARD");
    return v != nullptr && std::strcmp(v, "1") == 0;
}

// ── Centros ERB (S7/510; usados por Metrics y por el predictor) ───────────
// RÉPLICA EXACTA de SessionAggregator.erbCenterHz(48000) (Kotlin; misma
// escala que RealAcousticSensor, 60 Hz–16 kHz; Glasberg & Moore): bordes
// equiespaciados en tasa ERB, centro = media geométrica. Verificación:
// erbCenterHz(2) = 209 Hz (ERB-3, único centro medido/documentado).
constexpr int kErbBands = 24;
constexpr int kErbRegions = 6;
double erbRate(double f) noexcept { return 21.4 * std::log10(1.0 + 0.00437 * f); }
double erbInv(double r) noexcept { return (std::pow(10.0, r / 21.4) - 1.0) / 0.00437; }
double erbEdgeHz(int e) noexcept {  // e = 0..24 (borde inferior de la banda e)
    const double rLo = erbRate(60.0);
    const double rHi = erbRate(16000.0);
    return erbInv(rLo + (rHi - rLo) * static_cast<double>(e) / kErbBands);
}
double erbCenterHz(int b) noexcept {  // b = 0..23
    return std::sqrt(erbEdgeHz(b) * erbEdgeHz(b + 1));
}

// Regiones espectrales (por frecuencia central; la v1.1 nombra subgraves,
// graves, medios-graves, medios, presencia y brillo; S8 agrupa
// graves/medios-graves — aquí se separan subgraves <100 Hz por claridad).
//   0 subgraves <100 Hz · 1 graves 100–250 Hz · 2 medios 250–2000 Hz ·
//   3 brillo 2000–4000 Hz · 4 presencia 4000–8000 Hz · 5 aire >8000 Hz.
int erbRegion(int b) noexcept {
    const double f = erbCenterHz(b);
    if (f < 100.0) return 0;
    if (f < 250.0) return 1;
    if (f < 2000.0) return 2;
    if (f < 4000.0) return 3;
    if (f < 8000.0) return 4;
    return 5;
}
const char* erbRegionName(int r) noexcept {
    switch (r) {
        case 0: return "subgraves";
        case 1: return "graves";
        case 2: return "medios";
        case 3: return "brillo";
        case 4: return "presencia";
        default: return "aire";
    }
}

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
    std::uint64_t seed = ivanna::test::IvannaTestSeedFor(1);
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
    // Unificación 2.5.509 (paso 5): OmniField ya NO sigue a HOLOS —
    // tiene su propio amount y setNeutral() lo deja en 0. La config
    // F0/F es "HOLOS+Omni al máximo", así que se re-enciende explícito
    // (antes lo re-encendía implícitamente field.amount=1.0).
    c.field.omniAmount = 1.0f;  // OmniField al máximo
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
    // S7/510 — firma espectral por banda (solo barrido): nivel L/R (dB) y
    // correlación intercanal por cada una de las 24 bandas ERB. Es la
    // línea base del Golden-Master (pendiente 2 del doc del harness).
    bool hasErb = false;
    double erbLvlLDb[kErbBands] = {};
    double erbLvlRDb[kErbBands] = {};
    double erbCorr[kErbBands] = {};
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
    // Solo barrido (S7/510): firma espectral por banda ERB — nivel L/R y
    // correlación intercanal por banda. FFT 131072 Hann con zero-padding
    // sobre la ventana completa (2.0 s); los bins se agrupan por banda
    // ERB (bordes erbEdgeHz). Nivel de banda:
    // 20*log10(2*sqrt(P)/Nfft) con P = suma |X|² de la banda — para un tono
    // puro recupera su amplitud (la ventana Hann tiene ganancia coherente
    // 0.5); para banda ancha es un nivel equivalente comparable entre
    // corridas. La regresión solo exige determinismo, no calibración
    // absoluta: el barrido no es plano por banda. corr por banda =
    // Re(Σ XL·conj(XR)) / sqrt(PL·PR).
    if (s == Stimulus::kSweep) {
        // FFT de 131072 con zero-padding de la ventana completa (96000
        // muestras, 2.0 s): el barrido log recorre 20 Hz–20 kHz en los
        // 2.5 s del estímulo, así que la ventana de medida (0.5–2.5 s)
        // cubre todas las bandas ERB. Con 65536 puntos solo se verían
        // los primeros 1.37 s (~hasta 3.5 kHz) y las bandas altas
        // quedarían en el piso numérico.
        constexpr int kNfft = 131072;
        ivanna::FFTRadix2d fft(kNfft);
        std::vector<double> reL(static_cast<std::size_t>(kNfft), 0.0),
            imL(static_cast<std::size_t>(kNfft), 0.0),
            reR(static_cast<std::size_t>(kNfft), 0.0),
            imR(static_cast<std::size_t>(kNfft), 0.0);
        for (int i = 0; i < kWindowSamples; ++i) {
            const double w = 0.5 * (1.0 - std::cos(kTwoPi * i / (kWindowSamples - 1)));
            reL[static_cast<std::size_t>(i)] = wL[i] * w;
            reR[static_cast<std::size_t>(i)] = wR[i] * w;
        }
        fft.forward(reL.data(), imL.data());
        fft.forward(reR.data(), imR.data());
        const double df = kFs / kNfft;
        for (int b = 0; b < kErbBands; ++b) {
            const double lo = erbEdgeHz(b), hi = erbEdgeHz(b + 1);
            double pL = 0.0, pR = 0.0, cross = 0.0;
            for (int k = 1; k < kNfft / 2; ++k) {
                const double f = k * df;
                if (f < lo || f >= hi) continue;
                const double xl = reL[static_cast<std::size_t>(k)];
                const double yl = imL[static_cast<std::size_t>(k)];
                const double xr = reR[static_cast<std::size_t>(k)];
                const double yr = imR[static_cast<std::size_t>(k)];
                pL += xl * xl + yl * yl;
                pR += xr * xr + yr * yr;
                cross += xl * xr + yl * yr;  // Re(XL · conj(XR))
            }
            m.erbLvlLDb[b] = 20.0 * std::log10(2.0 * std::sqrt(pL) / kNfft + 1e-32);
            m.erbLvlRDb[b] = 20.0 * std::log10(2.0 * std::sqrt(pR) / kNfft + 1e-32);
            m.erbCorr[b] = std::clamp(cross / (std::sqrt(pL * pR) + 1e-32), -1.0, 1.0);
        }
        m.hasErb = true;
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

void printErbSignature(const char* cfg, const Metrics& m) {
    if (!m.hasErb) return;
    for (int b = 0; b < kErbBands; ++b) {
        std::printf("HARNESS ERB %-4s banda %2d %7.1f Hz lvl %+7.2f/%+7.2f dB corr %+0.4f\n",
                    cfg, b + 1, erbCenterHz(b), m.erbLvlLDb[b], m.erbLvlRDb[b], m.erbCorr[b]);
    }
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

// ── S7/510 — MODO «FIRMA ERB»: PREDICTOR LINEAL + PUERTA ───────────────────
// Línea base RMS ERB medida (perfil v1.1, fingerprint ERB, 177 sesiones en
// vivo, 2026-11-28): spectralErrorRmsDb = 10.46 dB. La puerta exige una
// mejora ≥20% → objetivo ≤ 8.37 dB (0.8 × 10.46 = 8.368).
constexpr double kErbBaselineRmsDb = 10.46;
constexpr double kErbGateMinImprovement = 0.20;  // fracción
constexpr double kErbTargetRmsDb = 8.368;        // 0.8 × 10.46

// Mapeo EXACTO de FusedTonal::applyControls (fused_tonal.cpp:56-94) a las
// 10 ganancias de banda: 0-2 lowDb, 3-6 midDb, 7-9 highDb; presenceDb
// aditivo en 7-8; curve==2 suma (élite − Harman) × 0.5 (kCurveTargetScale);
// todo × amount. trimDb NO entra aquí: es ganancia plana y se suma aparte
// en candidateEqResponseDb. Los clamps del motor se aplican sobre copias.
void tonalBandGains(const TonalControls& cIn, float gains[10]) noexcept {
    TonalControls c = cIn;
    c.clamp();
    for (int b = 0; b < 10; ++b) {
        float g = (b <= 2) ? c.lowDb : (b <= 6) ? c.midDb : c.highDb;
        if (c.presenceDb != 0.0f && (b == 7 || b == 8)) g += c.presenceDb;
        if (c.curve == 2) {
            const float f = ivanna::room::kBandHz[b];
            g += (ivanna::spatial::eliteTargetDbAt(
                      ivanna::spatial::EliteTargetKind::HarmanEliteLean, f) -
                  ivanna::spatial::eliteTargetDbAt(
                      ivanna::spatial::EliteTargetKind::Harman2018, f)) *
                 0.5f;  // FusedTonal::kCurveTargetScale
        }
        gains[b] = g * c.amount;
    }
}

// Respuesta en magnitud (dB) del EQ candidato a la frecuencia f: cascada
// RoomCorrectionEq (roomCorrDb, cut-only [−12,0] por
// ListenerRoomControls::clamp) + cascada de las 10 ganancias tonales +
// trimDb plano. Usa ivanna::room::cascadeMagDb — la misma matemática RBJ
// que AutoEqFilter::setBand — no una reimplementación paralela.
// NO incluye EvoEQ ni la curva Harman/élite como tal (curve 1/0 no añade
// nada al EQ de usuario; curve 2 añade solo su delta, ver arriba): el
// predictor cubre el EQ candidato, no toda la cadena tonal.
double candidateEqResponseDb(const ListenerRoomControls& roomIn,
                             const TonalControls& tonalIn, double f) noexcept {
    ListenerRoomControls room = roomIn;
    room.clamp();  // cut-only [−12,0] por banda
    TonalControls tonal = tonalIn;
    tonal.clamp();  // slots ±12 dB, presence ±12 dB, amount 0..1
    float tg[10];
    tonalBandGains(tonal, tg);
    return static_cast<double>(ivanna::room::cascadeMagDb(room.roomCorrDb, static_cast<float>(f),
                                                          static_cast<float>(kFs))) +
           static_cast<double>(ivanna::room::cascadeMagDb(tg, static_cast<float>(f),
                                                          static_cast<float>(kFs))) +
           static_cast<double>(tonal.trimDb);
}

struct ErbPrediction {
    double rmsDb = 0.0;               // RMS post-EQ predicho (24 bandas)
    double baselineRmsDb = 0.0;       // RMS pre-EQ del vector de entrada
    double residualDb[kErbBands] = {};
    double eqResponseDb[kErbBands] = {};
    double regionRmsDb[kErbRegions] = {};
    double regionBaselineRmsDb[kErbRegions] = {};
};

// RMS(errorMeanDb + respuesta EQ) por banda ERB, con el clamp aplicado.
// El vector errorMeanDb[24] es el error MEDIDO (SessionChunk.errorMeanDb):
// el llamador lo trae del servidor/perfil; aquí NUNCA se inventa.
ErbPrediction predictErbRms(const double errorMeanDb[kErbBands],
                            const ListenerRoomControls& room,
                            const TonalControls& tonal) noexcept {
    ErbPrediction p;
    double sumBase = 0.0, sumRes = 0.0;
    double regBase[kErbRegions] = {}, regRes[kErbRegions] = {};
    int regN[kErbRegions] = {};
    for (int b = 0; b < kErbBands; ++b) {
        const double resp = candidateEqResponseDb(room, tonal, erbCenterHz(b));
        p.eqResponseDb[b] = resp;
        p.residualDb[b] = errorMeanDb[b] + resp;
        sumBase += errorMeanDb[b] * errorMeanDb[b];
        sumRes += p.residualDb[b] * p.residualDb[b];
        const int r = erbRegion(b);
        regBase[r] += errorMeanDb[b] * errorMeanDb[b];
        regRes[r] += p.residualDb[b] * p.residualDb[b];
        ++regN[r];
    }
    p.baselineRmsDb = std::sqrt(sumBase / kErbBands);
    p.rmsDb = std::sqrt(sumRes / kErbBands);
    for (int r = 0; r < kErbRegions; ++r) {
        if (regN[r] == 0) continue;
        p.regionBaselineRmsDb[r] = std::sqrt(regBase[r] / regN[r]);
        p.regionRmsDb[r] = std::sqrt(regRes[r] / regN[r]);
    }
    return p;
}

// Puerta S7: la candidata solo se emite si el RMS predicho baja ≥20%
// respecto a la línea base (10.46 dB → ≤ 8.37 dB).
bool passesErbGate(double predictedRmsDb, double baselineRmsDb) noexcept {
    return predictedRmsDb <= (1.0 - kErbGateMinImprovement) * baselineRmsDb;
}

void printPredict(const char* candidate, const ErbPrediction& p) {
    std::printf("HARNESS PREDICT %-8s rms predicho %+.2f dB (base %+.2f dB, "
                "objetivo <= %.2f dB) puerta %s\n",
                candidate, p.rmsDb, p.baselineRmsDb, kErbTargetRmsDb,
                passesErbGate(p.rmsDb, p.baselineRmsDb) ? "PASA" : "RECHAZA");
    for (int r = 0; r < kErbRegions; ++r) {
        std::printf("HARNESS PREDICT %-8s region %-10s rms %+.2f dB (base %+.2f dB)\n",
                    candidate, erbRegionName(r), p.regionRmsDb[r], p.regionBaselineRmsDb[r]);
    }
    std::fflush(stdout);
}
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
        if (s == Stimulus::kSweep) {
            // S7/510: la firma ERB del neutro es la referencia (bit-exacto:
            // es el espectro del propio barrido de entrada).
            const Metrics m = computeMetrics(in, run, s);
            printErbSignature("N", m);
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
            if (s == Stimulus::kSweep) printErbSignature(name, m);  // S7/510
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
        if (s == Stimulus::kSweep) printErbSignature("L", m);  // S7/510
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
    // C11/510 — la métrica se PUBLICA siempre (línea [METRIC] estable para
    // el log JUnit de C5); la puerta solo muerde con IVANNA_CPU_GATE_HARD=1
    // (self-hosted) y nunca bajo sanitizador.
#if defined(__SANITIZE_ADDRESS__) || defined(IVANNA_SANITIZER_ACTIVE)
    const bool hardGate = false;  // sanitizador: el CPU se infla 2-3x por diseño
#else
    const bool hardGate = CpuGateHard();
#endif
    for (const auto& [name, c] : cfgs) {
        const RunResult run = runChain(c, in);
        const Metrics m = computeMetrics(in, run, Stimulus::kMusic);
        std::printf("HARNESS %-4s cpu musica media %.0f us p95 %.0f us max %.0f us (%zu chunks)\n",
                    name, m.cpuMeanUs, m.cpuP95Us, m.cpuMaxUs, run.cpuUs.size());
        std::printf("[METRIC] cpu_per_hop %s mean_us=%.1f p95_us=%.1f max_us=%.1f budget_us=2667 gate=%s\n",
                    name, m.cpuMeanUs, m.cpuP95Us, m.cpuMaxUs,
                    hardGate ? "hard" : "metric-only");
        std::fflush(stdout);
        // Bajo ASan/TSan el CPU se infla 2-3x por diseño: el presupuesto
        // por hop NO se impone (el sanitizador mide seguridad de memoria,
        // no rendimiento). La medición sigue (se reporta arriba) y el
        // presupuesto sigue vigente en builds sin sanitizador.
#if !defined(__SANITIZE_ADDRESS__) && !defined(IVANNA_SANITIZER_ACTIVE)
        if (hardGate) {
            EXPECT_LE(m.cpuMeanUs, kCpuMeanMaxUs)
                << name << " CPU media por hop " << m.cpuMeanUs << " us (presupuesto 2667 us)";
            EXPECT_LE(m.cpuP95Us, kCpuP95MaxUs)
                << name << " CPU p95 por hop " << m.cpuP95Us << " us (presupuesto 2667 us)";
        }
#endif
    }
}

TEST(AcousticHarness, ErbSignaturePredictsRms) {
    // S7/510 — el predictor es la pre-pantalla del laboratorio: una
    // candidata que ni en teoría puede mejorar el RMS ERB un 20% no se
    // envía al teléfono. El predictor es LINEAL (la cadena real no lo
    // es): pre-pantalla, no sustituto del A/B.
    ListenerRoomControls roomNeutral;
    TonalControls tonalNeutral;  // curve 1 (Harman), todo 0, trim 0

    // (1) Centros ERB: la banda 3 (índice 2) es 209 Hz — el único centro
    // medido/documentado (ERB-3 209 Hz, perfil v1.1). Si esto falla, la
    // réplica de SessionAggregator.erbCenterHz está rota.
    EXPECT_NEAR(erbCenterHz(2), 209.0, 1.0) << "ERB-3 debe ser 209 Hz";
    EXPECT_NEAR(erbCenterHz(0), 82.0, 5.0) << "ERB-1 ~82 Hz (60 Hz–16 kHz)";
    EXPECT_NEAR(erbCenterHz(23), 14700.0, 1500.0) << "ERB-24 ~14.7 kHz";

    // (2) Identidad: candidato neutro no mueve el RMS (respuesta 0 en
    // las 24 bandas) para cualquier vector de error.
    {
        double err[kErbBands];
        for (int b = 0; b < kErbBands; ++b) err[b] = 5.0 + 0.5 * b;
        const ErbPrediction p = predictErbRms(err, roomNeutral, tonalNeutral);
        for (int b = 0; b < kErbBands; ++b)
            EXPECT_NEAR(p.eqResponseDb[b], 0.0, 1e-9) << "banda " << b + 1;
        EXPECT_NEAR(p.rmsDb, p.baselineRmsDb, 1e-9) << "neutro: RMS intacto";
        EXPECT_FALSE(passesErbGate(p.rmsDb, p.baselineRmsDb))
            << "neutro no pasa la puerta del 20%";
    }

    // (3) SINTÉTICO (etiquetado: NO es dato medido): error plano +10 dB
    // en las 24 bandas + candidato plano −10 dB (slots tonales −10, trim
    // 0) → el RMS predicho debe colapsar de 10 dB a ~3 dB. NO a 0: la
    // cascada RBJ de 10 peakings a una octava (Q 1.41) no suma plana —
    // la respuesta real oscila entre −6.5 y −15.1 dB según el centro ERB
    // (rizado medido; el mismo motivo por el que RoomProbe::
//       designCorrection itera con relajación). El valor 3.15 dB se
    //       verificó con una implementación independiente en Python de la
    //       misma fórmula RBJ; la tolerancia ±0.5 dB es el cruce.
    {
        double err[kErbBands];
        for (int b = 0; b < kErbBands; ++b) err[b] = 10.0;
        TonalControls flat;
        flat.lowDb = -10.0f;
        flat.midDb = -10.0f;
        flat.highDb = -10.0f;
        const ErbPrediction p = predictErbRms(err, roomNeutral, flat);
        EXPECT_NEAR(p.baselineRmsDb, 10.0, 1e-9);
        EXPECT_NEAR(p.rmsDb, 3.15, 0.5) << "RMS predicho del fixture plano";
        EXPECT_LE(p.rmsDb, 5.0) << "EQ plano −10 dB debe recortar el error plano +10 dB";
        EXPECT_TRUE(passesErbGate(p.rmsDb, p.baselineRmsDb))
            << "cancelación parcial pasa la puerta";
        printPredict("SINT-PLANO", p);
    }

    // (4) Aritmética de la puerta sobre la línea base MEDIDA (10.46 dB):
    // objetivo ≤ 8.37 dB; 9.0 dB RECHAZA, 8.0 dB PASA, 10.46 dB RECHAZA.
    EXPECT_FALSE(passesErbGate(10.46, kErbBaselineRmsDb));
    EXPECT_FALSE(passesErbGate(9.0, kErbBaselineRmsDb));
    EXPECT_TRUE(passesErbGate(8.0, kErbBaselineRmsDb));
    EXPECT_TRUE(passesErbGate(kErbTargetRmsDb, kErbBaselineRmsDb));
    EXPECT_NEAR(kErbTargetRmsDb, 0.8 * kErbBaselineRmsDb, 1e-9);

    // (5) Candidata v1.1 REAL con el único dato MEDIDO disponible (ERB-3
    // +9.86 dB @ 209 Hz): slots formato v1.0 (clamp ±2 dB) low −2, mid +2,
    // high +2, presence +2, trim −4.0. Se evalúa SOLO esa banda: la
    // respuesta del candidato a 209 Hz y su residual. La puerta COMPLETA
    // es INDECIDIBLE: las otras 23 bandas del error viven en el servidor
    // sin exportar (S7 «dato que falta») — el RMS 10.46 dB es el escalar
    // medido, no el vector. NO se reconstruye el vector ni se emite
    // veredicto de puerta para v1.1.
    {
        TonalControls v11;
        v11.lowDb = -2.0f;       // low_60_120 (propuesto −3, residual −1.0)
        v11.midDb = 2.0f;        // mid_500_1k (propuesto +4, residual +2.0)
        v11.highDb = 2.0f;       // high_2k_4k / brillo (propuesto +9, residual +7.0)
        v11.presenceDb = 2.0f;   // presence_4k_8k (propuesto +9, residual +7.0)
        v11.trimDb = -4.0f;      // compensa el neto +4.0 dB de los slots
        const double resp209 = candidateEqResponseDb(roomNeutral, v11, 209.0);
        const double resid209 = 9.86 + resp209;
        std::printf("HARNESS PREDICT v1.1     ERB-3 209 Hz err +9.86 dB eq %+.2f dB resid %+.2f dB\n",
                    resp209, resid209);
        std::printf("HARNESS PREDICT v1.1     GATE UNDECIDIBLE (23/24 bandas ERB sin exportar del servidor; RMS base medido 10.46 dB, objetivo <= 8.37 dB)\n");
        std::fflush(stdout);
        EXPECT_TRUE(std::isfinite(resp209)) << "respuesta v1.1 a 209 Hz finita";
        EXPECT_TRUE(std::isfinite(resid209)) << "residual v1.1 a 209 Hz finito";
        EXPECT_NEAR(resid209, 9.86 + resp209, 1e-9) << "residual = error + respuesta";
        // Cruce independiente (Python, misma fórmula RBJ): la respuesta
        // del candidato a 209 Hz es −2.86 dB (el −2 dB de graves tira
        // hacia abajo, los +2 dB de medios tiran hacia arriba, el trim
        // −4.0 dB es plano) y el residual queda en +7.00 dB.
        EXPECT_NEAR(resp209, -2.86, 0.25) << "respuesta v1.1 a 209 Hz";
        EXPECT_NEAR(resid209, 7.00, 0.25) << "residual v1.1 a 209 Hz";
        // El clamp del formato v1.0 (±2 dB) ya está aplicado en los slots
        // de arriba; el clamp del motor (±12 dB) lo aplica el predictor.
        // El residual a 209 Hz sigue siendo positivo: el −2 dB de graves
        // no cancela el +9.86 dB de ERB-3 (el resto del trabajo lo hace
        // roomCorrDb cut-only tras S3, o el A/B).
        EXPECT_GT(resid209, 0.0) << "ERB-3 sigue en exceso tras v1.1 (esperado: +9.86 con solo −2 dB cerca)";
    }

    // (6) Cut-only: roomCorrDb por debajo de −12 dB se clampea a −12 y por
    // encima de 0 dB a 0 (ListenerRoomControls::clamp, vía el predictor).
    {
        ListenerRoomControls room;
        room.roomCorrDb[5] = -30.0f;  // 1 kHz: fuera de rango por abajo
        room.roomCorrDb[6] = 5.0f;    // 2 kHz: realce prohibido en cut-only
        const double r1k = candidateEqResponseDb(room, tonalNeutral, 1000.0);
        const double r2k = candidateEqResponseDb(room, tonalNeutral, 2000.0);
        // A 1 kHz la respuesta es exactamente −12 dB (el propio centro de
        // la banda clampea a −12). A 2 kHz no es 0: la banda de 1 kHz
        // derrama −2.50 dB a una octava de distancia (peaking RBJ Q 1.41);
        // lo que se verifica es que NO hay realce (+5 clampea a 0) y que
        // el derrame es el de un −12 dB, no el de un −30 dB.
        EXPECT_NEAR(r1k, -12.0, 0.1) << "cut-only clampea −30 a −12 dB";
        EXPECT_NEAR(r2k, -2.50, 0.5) << "cut-only: +5 a 0 dB, derrame de −12 dB a 1 octava";
    }
}
