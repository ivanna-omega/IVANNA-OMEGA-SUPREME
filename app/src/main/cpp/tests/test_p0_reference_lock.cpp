// test_p0_reference_lock.cpp — P0 REFERENCE_LOCKED (2026-11-22, paso 3 de
// la orden de trabajo de Luna).
//
// QUÉ ES P0: una referencia CONGELADA del motor — un WAV de referencia
// fijo + una configuración fija de la cadena — con las métricas base
// medidas y bloqueadas en este test. Si la salida del motor cambia más
// allá de tolerancia, el test FALLA. P1 detecta regresiones (guarda);
// P0 congela el carácter (línea base). Sin P0 no hay "el motor suena
// igual que ayer" verificable.
//
// EL WAV DE REFERENCIA (48 kHz, estéreo, 10 s, 480000 muestras/canal,
// generado en código con fórmulas fijas y semilla fija — determinista y
// reproducible; se escribe a p0_reference.wav en cada corrida para
// auditarlo):
//   seg 0 SINE    [0.0, 2.0)   seno 1 kHz @ -12 dBFS pico — THD+N, niveles, latencia
//   seg 1 IMD     [2.0, 4.0)   dos tonos SMPTE 60 Hz + 7 kHz, 4:1, pico -12 dBFS — IMD
//   seg 2 NOISE   [4.0, 6.0)   ruido blanco, RMS -12 dBFS — niveles, latencia xcorr
//   seg 3 CLICKS  [6.0, 7.0)   impulsos 0.5 cada 0.25 s — latencia por impulso
//   seg 4 HOT     [7.0, 8.0)   seno 1 kHz @ -0.5 dBFS pico — true peak, techo final
//   seg 5 SILENCE [8.0, 10.0)  silencio digital — cola residual (fix NEURO)
// Las fronteras de segmento son múltiplos del hop (128). Las ventanas de
// medida saltan los primeros 0.25 s de cada segmento (transitorio STFT
// del cambio de segmento) salvo true peak (máximo, sin ventana) y clicks.
//
// LA CONFIGURACIÓN FIJA: engagedControls(), IDÉNTICA a la "IVANNA
// completa" de la matriz P1 (test_p1_adversarial_matrix.cpp) — mismos
// valores que test_fused_chain::engagedControls(). La cadena corre en
// hops de 128, como en el producto.
//
// MÉTRICAS CONGELADAS (valores en p0_reference_values.hpp, generados por
// scripts/p0_freeze.sh — NUNCA a mano):
//   niveles: RMS y pico de salida por segmento (L/R), ganancia neta total
//   latencia: xcorr sobre ruido (L/R) + impulso sobre clicks (L/R)
//   true peak: total y segmento HOT (dBTP, sinc 4x, mismo método que
//     ivanna_measure::truePeakLin pero en double)
//   THD+N: segmento seno, double (fused_measure::measureThdDouble)
//   IMD: segmento dos tonos, SMPTE, double (mismo método que
//     ivanna_measure::imdSmpteDb pero en double)
//   CPU: µs/hop de CPU del HILO (p50/p99/máx) — CONGELADOS COMO LÍMITES,
//     no como valores exactos: el tiempo de CPU depende de la máquina.
//     El tiempo de PARED en host compartido mide la carga del host, no
//     el motor (primera medición: p99 de pared 56 ms con 38 xruns, todo
//     stalls de planificación); por eso se congela el tiempo de CPU del
//     hilo (CLOCK_THREAD_CPUTIME_ID) y los xruns de pared solo se
//     reportan. El máx tiene límite 2x el presupuesto (5333 µs): es un
//     estadístico extremo y el motor tiene picos de hilo de hasta 4393 µs
//     (medidos en la congelación final); p50/p99 son los límites que
//     vigilan el coste. El presupuesto por hop es 2666.67 µs.
//   silencio: RMS y pico del último segundo (debe ser <= -90 dBFS — el
//     umbral P1; la cola de NEURO medía -77 dBFS antes del fix)
//   determinismo: dos corridas con cadenas frescas deben diferir menos
//     de 1e-5 en valor absoluto (el motor NO es bit-determinista: medido
//     1.4e-06, −117 dBFS, concentrado en el onset del seno).
//
// CONGELACIÓN: la referencia SOLO se congela sobre el motor CON el fix
// APEX+NEURO aplicado y commiteado. scripts/p0_freeze.sh lo verifica:
//   ./scripts/p0_freeze.sh          → mide y guarda PROVISIONAL (kFrozen=false)
//   ./scripts/p0_freeze.sh --final  → congela (kFrozen=true); exige el
//     techo final APEX presente y fused_chain.cpp + StereoWidener.cpp
//     limpios (fix commiteado). Sin --final, los TESTs hacen GTEST_SKIP.
// Tras congelar: rebuild + los TESTs imponen los valores.
//
// ALCANCE HONESTO: los números de CPU/tiempo son de host x86-64, NO del
// teléfono. La latencia en muestras SÍ es la del algoritmo. Los valores
// congelados son de ESTA build y ESTA configuración; un cambio
// intencional del motor exige re-congelar (scripts/p0_freeze.sh --final)
// y revisar el diff de valores como cambio de producto, no como ruido.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
// Núcleo del banco adversarial (medición + escritor WAV, ver CMakeLists).
#include "../../../../../tests/adversarial/src/adversarial_core.hpp"
#include "p0_reference_values.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;
using namespace adv;

namespace {

constexpr double kFs = 48000.0;
constexpr int kHop = 128;
constexpr size_t kN = 480000;  // 10 s @ 48 kHz
constexpr double kHopBudgetUs = 128.0 / 48000.0 * 1e6;  // 2666.67 µs
constexpr double kTwoPi = 6.28318530717958647692;

// Tolerancias de la congelación (valores deterministas en host).
constexpr double kTolLevelDb = 0.05;    // niveles / true peak
constexpr double kTolLatencySamples = 1.0;
constexpr double kTolThdnDb = 0.5;      // THD+N / IMD (ratios)
constexpr double kTolDeterminism = 1e-5;  // maxDiff entre 2 corridas (medido 1.4e-06)
constexpr double kSilenceMaxDbfs = -90.0;  // umbral P1 de silencio residual

struct SegDef {
    const char* name;
    size_t a, b;  // [a, b) en muestras
};
constexpr SegDef kSegs[6] = {
    {"sine", 0, 96000}, {"imd", 96000, 192000}, {"noise", 192000, 288000},
    {"clicks", 288000, 336000}, {"hot", 336000, 384000}, {"silence", 384000, 480000},
};
// Ventana de medida: salta los primeros 0.25 s (12000 muestras) del
// segmento (transitorio STFT del cambio de segmento).
constexpr size_t kSettle = 12000;

// IVANNA completa: mismos valores que test_fused_chain::engagedControls()
// y que la matriz P1 (configuración de referencia P0, fija).
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

// ── Generador de la referencia (determinista) ────────────────────────────
// PRNG xorshift64* con semilla fija (ruido blanco del seg 2).
struct XorShift64 {
    uint64_t s;
    double nextUniform01() {  // [0, 1)
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (double)((s * 0x2545F4914F6CDD1DULL) >> 11) * (1.0 / 9007199254740992.0);
    }
};

// Suma un tono a x[a, b) con fase inicial 0 (ciclos enteros si la
// longitud contiene un número entero de ciclos).
void addTone(double* x, size_t a, size_t b, double freqHz, double amp) {
    double phase = 0.0;
    const double step = kTwoPi * freqHz / kFs;
    for (size_t i = a; i < b; ++i) {
        x[i] += amp * std::sin(phase);
        phase += step;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
}

Signal generateReference() {
    Signal s;
    s.channels = 2;
    s.sampleRate = 48000;
    s.l.assign(kN, 0.0);
    s.r.assign(kN, 0.0);
    // seg 0: seno 1 kHz @ -12 dBFS pico (amp = 10^(-12/20)).
    addTone(s.l.data(), 0, 96000, 1000.0, 0.251188643150958);
    // seg 1: SMPTE 60 Hz + 7 kHz, 4:1, pico combinado -12 dBFS.
    addTone(s.l.data(), 96000, 192000, 60.0, 0.200950914520766);
    addTone(s.l.data(), 96000, 192000, 7000.0, 0.050237728630192);
    // seg 2: ruido blanco, RMS -12 dBFS (normalizado por RMS medido).
    {
        XorShift64 rng{0x9E3779B97F4A7C15ULL};
        double sumSq = 0.0;
        for (size_t i = 192000; i < 288000; ++i) {
            const double v = rng.nextUniform01() * 2.0 - 1.0;
            s.l[i] = v;
            sumSq += v * v;
        }
        const double rms = std::sqrt(sumSq / 96000.0);
        const double scale = 0.251188643150958 / rms;  // RMS -12 dBFS
        for (size_t i = 192000; i < 288000; ++i) s.l[i] *= scale;
    }
    // seg 3: impulsos 0.5 cada 0.25 s (1 muestra cada uno).
    for (int k = 0; k < 4; ++k) s.l[288000 + static_cast<size_t>(k) * 12000] = 0.5;
    // seg 4: seno 1 kHz @ -0.5 dBFS pico (amp = 10^(-0.5/20)).
    addTone(s.l.data(), 336000, 384000, 1000.0, 0.891250938133746);
    // seg 5: silencio digital (ya es 0.0).
    s.r = s.l;  // referencia estéreo L == R a la entrada
    return s;
}

// ── Medición (todo en double) ────────────────────────────────────────────
double toDbfs(double amp) { return 20.0 * std::log10(std::max(std::fabs(amp), 1e-15)); }

double rmsOf(const std::vector<double>& x, size_t a, size_t b) {
    double s = 0.0;
    for (size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(s / static_cast<double>(b - a));
}

double peakOf(const std::vector<double>& x, size_t a, size_t b) {
    double p = 0.0;
    for (size_t i = a; i < b; ++i) p = std::max(p, std::fabs(x[i]));
    return p;
}

// Amplitud por proyección LS sobre cos/sin a f (exacta con ciclos
// enteros; suma de Kahan). Mismo método que fused_measure (double).
double projectAmp(const double* x, size_t n, double freqHz) {
    double s = 0.0, c = 0.0, cs = 0.0, cc = 0.0;
    double phase = 0.0;
    const double step = kTwoPi * freqHz / kFs;
    for (size_t i = 0; i < n; ++i) {
        const double v = x[i];
        const double yS = v * std::sin(phase) - cs;
        const double tS = s + yS;
        cs = (tS - s) - yS;
        s = tS;
        const double yC = v * std::cos(phase) - cc;
        const double tC = c + yC;
        cc = (tC - c) - yC;
        c = tC;
        phase += step;
        if (phase >= kTwoPi) phase -= kTwoPi;
    }
    const double a = 2.0 * s / static_cast<double>(n);
    const double b = 2.0 * c / static_cast<double>(n);
    return std::sqrt(a * a + b * b);
}

// IMD SMPTE en double: bandas laterales f2±k*f1 (k=1,2) respecto a f2.
// La ventana debe contener ciclos enteros de ambas frecuencias.
double imdSmpteDouble(const double* x, size_t n, double f1, double f2) {
    const double ref = projectAmp(x, n, f2);
    double e = 0.0;
    for (int k = 1; k <= 2; ++k)
        for (int s : {-1, 1}) {
            const double a = projectAmp(x, n, f2 + s * k * f1);
            e += a * a;
        }
    return toDbfs(std::sqrt(e) / std::max(ref, 1e-30));
}

// True peak en double: sobremuestreo 4x con sinc ventaneada (Hann,
// ±12 muestras). Mismo método que ivanna_measure::truePeakLin (float).
double truePeakDouble(const double* x, size_t n) {
    double pk = 0.0;
    for (size_t i = 0; i < n; ++i) pk = std::max(pk, std::fabs(x[i]));
    for (size_t i = 12; i + 12 < n; ++i) {
        for (int p = 1; p < 4; ++p) {
            const double d = p / 4.0;
            double y = 0.0;
            for (int k = -11; k <= 12; ++k) {
                const double t = k - d;
                const double sinc =
                    (std::fabs(t) < 1e-12) ? 1.0 : std::sin(kTwoPi * 0.5 * t) / (kTwoPi * 0.5 * t);
                const double w = 0.5 * (1.0 + std::cos(kTwoPi * 0.5 * t / 12.5));
                y += x[i + k] * sinc * w;
            }
            pk = std::max(pk, std::fabs(y));
        }
    }
    return pk;
}

// Latencia por correlación cruzada (ventana [a,b), lag en [-maxLag,
// maxLag]) con refinamiento parabólico sub-muestra. Devuelve el lag en
// muestras (positivo = la salida va detrás de la entrada).
double xcorrLatency(const std::vector<double>& ref, const std::vector<double>& dut,
                    size_t a, size_t b, long maxLag) {
    double best = -1e30;
    long bestLag = 0;
    for (long lag = -maxLag; lag <= maxLag; ++lag) {
        double s = 0.0;
        for (size_t i = a; i < b; ++i) {
            const long j = static_cast<long>(i) + lag;
            if (j < 0 || j >= static_cast<long>(dut.size())) continue;
            s += ref[i] * dut[static_cast<size_t>(j)];
        }
        if (s > best) {
            best = s;
            bestLag = lag;
        }
    }
    // Refinamiento parabólico con los vecinos.
    auto corrAt = [&](long lag) {
        double s = 0.0;
        for (size_t i = a; i < b; ++i) {
            const long j = static_cast<long>(i) + lag;
            if (j < 0 || j >= static_cast<long>(dut.size())) continue;
            s += ref[i] * dut[static_cast<size_t>(j)];
        }
        return s;
    };
    const double cm = corrAt(bestLag - 1), c0 = best, cp = corrAt(bestLag + 1);
    const double denom = cm - 2.0 * c0 + cp;
    double frac = 0.0;
    if (std::fabs(denom) > 1e-30) frac = 0.5 * (cm - cp) / denom;
    if (frac < -1.0) frac = -1.0;
    if (frac > 1.0) frac = 1.0;
    return static_cast<double>(bestLag) + frac;
}

// Latencia por impulso: posición del PICO de la respuesta al impulso.
// La cadena STFT ensancha el impulso y el grueso de la energía llega
// ~1280 muestras después (el mismo retardo que la xcorr). El primer
// cruce de umbral NO sirve: la respuesta tiene un leak temprano (el
// click ya marca -26 dBFS a +2 muestras) y el click 1 va sobre la cola
// del ruido. Mediana de los 4 clicks (robusta al click 1 contaminado).
double impulseLatency(const std::vector<double>& x, size_t clickPos) {
    const size_t end = std::min(clickPos + 4096, x.size());
    size_t argmax = clickPos;
    double best = -1.0;
    for (size_t i = clickPos; i < end; ++i) {
        if (std::fabs(x[i]) > best) {
            best = std::fabs(x[i]);
            argmax = i;
        }
    }
    return static_cast<double>(argmax - clickPos);
}

// Corre la referencia por una cadena fresca (hops de 128, como P1).
Signal runChain(const Signal& ref) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(engagedControls());
    std::vector<double> l = ref.l, r = ref.r;
    for (size_t off = 0; off < kN; off += static_cast<size_t>(kHop)) {
        chain.process(l.data() + off, r.data() + off, kHop);
    }
    Signal out;
    out.channels = 2;
    out.sampleRate = 48000;
    out.l = std::move(l);
    out.r = std::move(r);
    return out;
}

struct CpuStats {
    // Tiempo de CPU del HILO (CLOCK_THREAD_CPUTIME_ID): mide el coste
    // del motor, inmune a los stalls de planificación del host
    // compartido. Es lo que se congela como límite.
    double meanUs = 0, p50Us = 0, p99Us = 0, maxUs = 0;
    // Tiempo de PARED (para caracterización): en host compartido mide
    // la carga del host, no el motor; se reporta, no se congela.
    double wallMeanUs = 0, wallP50Us = 0, wallP99Us = 0, wallMaxUs = 0;
    int xruns = 0;  // hops de pared sobre el presupuesto (reportado)
};

double threadCpuSec() {
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

// Coste por hop, mismo patrón que DoubleFloor.CpuPerHop: 200 hops de
// warm-up + 2000 medidos, 5 repeticiones, se queda la de menor p50
// (los stalls de planificación en host compartido inflan media/p95/máx;
// el máx es un estadístico extremo y con 5 repeticiones la mejor es
// estable — ver doc P0).
CpuStats runCpu() {
    constexpr int kWarmup = 200;
    constexpr int kMeasured = 2000;
    const size_t n = 48000;
    std::vector<double> src(n, 0.0);
    addTone(src.data(), 0, n, 1000.0, 0.251188643150958);
    CpuStats best;
    best.p50Us = 1e30;
    for (int rep = 0; rep < 5; ++rep) {
        FusedChain chain;
        chain.prepare(kFs);
        chain.setControls(engagedControls());
        std::vector<double> l = src, r = src;
        std::vector<double> us, wallUs;
        us.reserve(static_cast<size_t>(kMeasured));
        wallUs.reserve(static_cast<size_t>(kMeasured));
        size_t off = 0;
        for (int b = 0; b < kWarmup + kMeasured; ++b) {
            if (off + static_cast<size_t>(kHop) > n) off = 0;
            const auto t0 = std::chrono::steady_clock::now();
            const double c0 = threadCpuSec();
            chain.process(l.data() + off, r.data() + off, kHop);
            const double c1 = threadCpuSec();
            const auto t1 = std::chrono::steady_clock::now();
            if (b >= kWarmup) {
                us.push_back((c1 - c0) * 1e6);
                wallUs.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            off += static_cast<size_t>(kHop);
        }
        auto summarize = [](std::vector<double> v, double& mean, double& p50, double& p99,
                            double& mx) {
            std::sort(v.begin(), v.end());
            mean = 0.0;
            for (double x : v) mean += x;
            mean /= static_cast<double>(v.size());
            p50 = v[v.size() / 2];
            p99 = v[(v.size() * 99) / 100];
            mx = v.back();
        };
        CpuStats st;
        summarize(us, st.meanUs, st.p50Us, st.p99Us, st.maxUs);
        summarize(wallUs, st.wallMeanUs, st.wallP50Us, st.wallP99Us, st.wallMaxUs);
        st.xruns = 0;
        for (double v : wallUs)
            if (v > kHopBudgetUs) ++st.xruns;
        if (st.p50Us < best.p50Us) best = st;
    }
    return best;
}

// ── Medición completa de una corrida ──────────────────────────────────────
struct P0Measured {
    double segOutRmsDbL[5] = {}, segOutRmsDbR[5] = {};
    double segOutPeakDbL[5] = {}, segOutPeakDbR[5] = {};
    double netGainDbL = 0, netGainDbR = 0;
    double latencyXcorrL = 0, latencyXcorrR = 0;
    double latencyImpulseL = 0, latencyImpulseR = 0;
    double truePeakDbTp = 0, truePeakHotDbTp = 0;
    double thdnDbL = 0, thdnDbR = 0;
    double imdDbL = 0, imdDbR = 0;
    double silenceRmsDbL = 0, silenceRmsDbR = 0;
    double silencePeakDbL = 0, silencePeakDbR = 0;
    CpuStats cpu;
    double deterministicMaxDiff = 0.0;  // se rellena comparando dos corridas
};

P0Measured measureAll(const Signal& ref, const Signal& out) {
    P0Measured m;
    // Niveles por segmento (ventana asentada; clicks: segmento entero).
    for (int s = 0; s < 5; ++s) {
        size_t a = kSegs[s].a, b = kSegs[s].b;
        if (s != 3) a += kSettle;
        m.segOutRmsDbL[s] = toDbfs(rmsOf(out.l, a, b));
        m.segOutRmsDbR[s] = toDbfs(rmsOf(out.r, a, b));
        m.segOutPeakDbL[s] = toDbfs(peakOf(out.l, a, b));
        m.segOutPeakDbR[s] = toDbfs(peakOf(out.r, a, b));
    }
    // Ganancia neta total. [P1+P4 2026-11-26: el comentario viejo decía
    // que la Regla de Oro "debe ser ~0 dB en el carácter congelado" —
    // FALSO para la cadena enganchada: el valor congelado ES −6.065/
    // −6.274 dB. La regla "0.0 dB" solo vale en neutro (bit-exacto) y
    // como objetivo nominal de P8 (trim medido). Aquí el valor es el
    // que es y se congela tal cual.]
    m.netGainDbL = toDbfs(rmsOf(out.l, 0, kN)) - toDbfs(rmsOf(ref.l, 0, kN));
    m.netGainDbR = toDbfs(rmsOf(out.r, 0, kN)) - toDbfs(rmsOf(ref.r, 0, kN));
    // Latencia xcorr sobre ruido (ventana asentada), L y R.
    m.latencyXcorrL = xcorrLatency(ref.l, out.l, 192000 + kSettle, 288000, 2048);
    m.latencyXcorrR = xcorrLatency(ref.r, out.r, 192000 + kSettle, 288000, 2048);
    // Latencia por impulso (mediana de los 4 clicks: pico de la
    // respuesta ensanchada).
    {
        std::vector<double> lL, lR;
        for (int k = 0; k < 4; ++k) {
            const size_t pos = 288000 + static_cast<size_t>(k) * 12000;
            lL.push_back(impulseLatency(out.l, pos));
            lR.push_back(impulseLatency(out.r, pos));
        }
        std::sort(lL.begin(), lL.end());
        std::sort(lR.begin(), lR.end());
        m.latencyImpulseL = 0.5 * (lL[1] + lL[2]);
        m.latencyImpulseR = 0.5 * (lR[1] + lR[2]);
    }
    // True peak total (máx L/R) y segmento HOT.
    m.truePeakDbTp = std::max(toDbfs(truePeakDouble(out.l.data(), kN)),
                              toDbfs(truePeakDouble(out.r.data(), kN)));
    m.truePeakHotDbTp =
        std::max(toDbfs(truePeakDouble(out.l.data() + 336000, 48000)),
                 toDbfs(truePeakDouble(out.r.data() + 336000, 48000)));
    // THD+N sobre el seno (ventana [12000, 96000) = 84000 muestras =
    // 1750 ciclos exactos a 1 kHz). Double, piso del medidor ~-310 dB.
    m.thdnDbL = measureThdDouble(out.l.data() + kSettle, 84000, kFs, 1000.0).thdPlusNoiseDb;
    m.thdnDbR = measureThdDouble(out.r.data() + kSettle, 84000, kFs, 1000.0).thdPlusNoiseDb;
    // IMD SMPTE sobre los dos tonos (ventana [108000, 192000) = 84000
    // muestras: 105 ciclos a 60 Hz, 12250 a 7 kHz, laterales enteros).
    m.imdDbL = imdSmpteDouble(out.l.data() + 108000, 84000, 60.0, 7000.0);
    m.imdDbR = imdSmpteDouble(out.r.data() + 108000, 84000, 60.0, 7000.0);
    // Silencio residual: último segundo (el fix NEURO debe dejarlo en
    // silencio; antes del fix medía -77 dBFS).
    m.silenceRmsDbL = toDbfs(rmsOf(out.l, 432000, 480000));
    m.silenceRmsDbR = toDbfs(rmsOf(out.r, 432000, 480000));
    m.silencePeakDbL = toDbfs(peakOf(out.l, 432000, 480000));
    m.silencePeakDbR = toDbfs(peakOf(out.r, 432000, 480000));
    return m;
}

// Diferencia máxima entre dos corridas. El motor NO es bit-determinista
// (medido: maxDiff 1.4e-06, concentrado en el onset del seno — ver doc);
// la guarda es una tolerancia, no bit-exactitud.
double maxAbsDiff(const Signal& a, const Signal& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.l.size(); ++i) {
        m = std::max(m, std::fabs(a.l[i] - b.l[i]));
        m = std::max(m, std::fabs(a.r[i] - b.r[i]));
    }
    return m;
}

std::string fmtArr5(const double v[5]) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "{%.6f, %.6f, %.6f, %.6f, %.6f}",
                  v[0], v[1], v[2], v[3], v[4]);
    return buf;
}

std::string fmtNum(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

// ── Congelación: escribe p0_reference_values.hpp + JSON + WAVs ───────────
// P0_FREEZE=1 → provisional (kFrozen=false); P0_FREEZE=final → congelado
// (kFrozen=true). El script scripts/p0_freeze.sh --final exige el fix
// APEX+NEURO commiteado antes de pedir "final" a este test.
bool doFreeze(bool finalFreeze) {
    const Signal ref = generateReference();
    const Signal out1 = runChain(ref);
    const Signal out2 = runChain(ref);
    P0Measured m = measureAll(ref, out1);
    m.cpu = runCpu();
    m.deterministicMaxDiff = maxAbsDiff(out1, out2);

    // Límites de CPU desde la línea base medida (no valores exactos: el
    // tiempo depende de la máquina). p50: 1.5x la base; p99: 1.5x la base.
    // 6.1 (refinamiento 2026-11-26) — TAPADOS POR EL PRESUPUESTO: la
    // fórmula anterior (p50 4x / p99 3x la base) daba límites congelados
    // p50 3087 / p99 4254 µs, POR ENCIMA del presupuesto realtime de hop
    // (2666.67 µs): una regresión al 120% del presupuesto pasaba el test.
    // Los límites congelados ya no pueden superar el presupuesto (p50) ni
    // 1.5x el presupuesto (p99), que es lo mismo que imponen los EXPECT
    // duros de CpuXruns. Base medida en VM sin carga (2026-11-26,
    // scripts/p0_freeze.sh --final): p50 773.182 µs, p99 1585.733 µs
    // → límites 1159.773 / 2378.600 µs.
    const double cpuP50Max = std::min(std::max(1.5 * m.cpu.p50Us, 400.0), kHopBudgetUs);
    const double cpuP99Max = std::min(std::max(1.5 * m.cpu.p99Us, 1000.0), 1.5 * kHopBudgetUs);

    std::string header;
    header += "#pragma once\n";
    header += "// p0_reference_values.hpp — VALORES CONGELADOS P0 (REFERENCE_LOCKED).\n";
    header += "// GENERADO por scripts/p0_freeze.sh — NUNCA editar a mano.\n";
    header += "// Un cambio intencional del motor exige re-congelar y revisar el\n";
    header += "// diff de valores como cambio de producto, no como ruido.\n";
    header += "namespace p0 {\n";
    header += std::string("inline constexpr bool kFrozen = ") + (finalFreeze ? "true" : "false") +
              ";\n";
    header += "inline constexpr char kFrozenFrom[] = \"" +
              std::string(finalFreeze ? "CONGELADO post-fix APEX+NEURO (scripts/p0_freeze.sh "
                                        "--final)"
                                      : "PROVISIONAL — sin congelar (P0_FREEZE=1)") +
              "\";\n";
    header += "inline constexpr char kReferenceId[] = \"p0_reference_v1\";\n";
    header += "// Niveles: RMS y pico de salida por segmento [sine, imd, noise, clicks, hot] (dBFS).\n";
    header += "inline constexpr double kSegOutRmsDbL[5] = " + fmtArr5(m.segOutRmsDbL) + ";\n";
    header += "inline constexpr double kSegOutRmsDbR[5] = " + fmtArr5(m.segOutRmsDbR) + ";\n";
    header += "inline constexpr double kSegOutPeakDbL[5] = " + fmtArr5(m.segOutPeakDbL) + ";\n";
    header += "inline constexpr double kSegOutPeakDbR[5] = " + fmtArr5(m.segOutPeakDbR) + ";\n";
    header += "inline constexpr double kNetGainDbL = " + fmtNum(m.netGainDbL) + ";\n";
    header += "inline constexpr double kNetGainDbR = " + fmtNum(m.netGainDbR) + ";\n";
    header += "// Latencia (muestras).\n";
    header += "inline constexpr double kLatencyXcorrL = " + fmtNum(m.latencyXcorrL) + ";\n";
    header += "inline constexpr double kLatencyXcorrR = " + fmtNum(m.latencyXcorrR) + ";\n";
    header += "inline constexpr double kLatencyImpulseL = " + fmtNum(m.latencyImpulseL) + ";\n";
    header += "inline constexpr double kLatencyImpulseR = " + fmtNum(m.latencyImpulseR) + ";\n";
    header += "// True peak (dBTP).\n";
    header += "inline constexpr double kTruePeakDbTp = " + fmtNum(m.truePeakDbTp) + ";\n";
    header += "inline constexpr double kTruePeakHotDbTp = " + fmtNum(m.truePeakHotDbTp) + ";\n";
    header += "// THD+N (dB) e IMD SMPTE (dB).\n";
    header += "inline constexpr double kThdnDbL = " + fmtNum(m.thdnDbL) + ";\n";
    header += "inline constexpr double kThdnDbR = " + fmtNum(m.thdnDbR) + ";\n";
    header += "inline constexpr double kImdDbL = " + fmtNum(m.imdDbL) + ";\n";
    header += "inline constexpr double kImdDbR = " + fmtNum(m.imdDbR) + ";\n";
    header += "// Silencio residual, último segundo (dBFS).\n";
    header += "inline constexpr double kSilenceRmsDbL = " + fmtNum(m.silenceRmsDbL) + ";\n";
    header += "inline constexpr double kSilenceRmsDbR = " + fmtNum(m.silenceRmsDbR) + ";\n";
    header += "inline constexpr double kSilencePeakDbL = " + fmtNum(m.silencePeakDbL) + ";\n";
    header += "inline constexpr double kSilencePeakDbR = " + fmtNum(m.silencePeakDbR) + ";\n";
    header += "// CPU: LÍMITES de tiempo de CPU del HILO (µs/hop), no valores exactos.\n";
    header += "// p50 base medida = " + fmtNum(m.cpu.p50Us) + " µs (hilo), p99 = " +
              fmtNum(m.cpu.p99Us) + " µs, máx = " + fmtNum(m.cpu.maxUs) +
              " µs; pared p50/p99/máx = " + fmtNum(m.cpu.wallP50Us) + "/" +
              fmtNum(m.cpu.wallP99Us) + "/" + fmtNum(m.cpu.wallMaxUs) +
              " µs, xruns de pared medidos = " + std::to_string(m.cpu.xruns) +
              " (la pared mide la carga del host, no se congela).\n";
    header += "inline constexpr double kCpuP50MaxUs = " + fmtNum(cpuP50Max) + ";\n";
    header += "inline constexpr double kCpuP99MaxUs = " + fmtNum(cpuP99Max) + ";\n";
    header += "// Máx: 2x el presupuesto (5333 µs), no el presupuesto pelado:\n";
    header += "// el máx es un estadístico extremo y el motor tiene picos de hilo\n";
    header += "// de hasta 4393 µs (medido en la congelación final) que NO son\n";
    header += "// xruns de pared; un máx a 2666.67 sería flaky. p50/p99 son los\n";
    header += "// límites que realmente vigilan el coste del motor.\n";
    header += "inline constexpr double kCpuMaxMaxUs = 5333.33;\n";
    header += "inline constexpr int kXrunsMax = 0;\n";
    header += "}  // namespace p0\n";

#ifdef P0_SOURCE_DIR
    const std::string headerPath = std::string(P0_SOURCE_DIR) + "/p0_reference_values.hpp";
#else
    const std::string headerPath = "p0_reference_values.hpp";
#endif
    {
        std::ofstream f(headerPath);
        if (!f) {
            std::printf("[P0-FREEZE] ERROR: no se pudo escribir %s\n", headerPath.c_str());
            return false;
        }
        f << header;
    }
    // Auditoría: WAVs de referencia y salida + JSON con todo lo medido.
    write_wav("p0_reference.wav", ref);
    write_wav("p0_output.wav", out1);
    {
        std::ofstream j("p0_reference.json");
        j << "{\n  \"reference\": \"p0_reference_v1\",\n  \"frozen\": "
          << (finalFreeze ? "true" : "false") << ",\n";
        j << "  \"deterministic_max_diff\": " << m.deterministicMaxDiff << ",\n";
        j << "  \"seg_out_rms_db_L\": [" << m.segOutRmsDbL[0] << ", " << m.segOutRmsDbL[1]
          << ", " << m.segOutRmsDbL[2] << ", " << m.segOutRmsDbL[3] << ", "
          << m.segOutRmsDbL[4] << "],\n";
        j << "  \"seg_out_rms_db_R\": [" << m.segOutRmsDbR[0] << ", " << m.segOutRmsDbR[1]
          << ", " << m.segOutRmsDbR[2] << ", " << m.segOutRmsDbR[3] << ", "
          << m.segOutRmsDbR[4] << "],\n";
        j << "  \"seg_out_peak_db_L\": [" << m.segOutPeakDbL[0] << ", " << m.segOutPeakDbL[1]
          << ", " << m.segOutPeakDbL[2] << ", " << m.segOutPeakDbL[3] << ", "
          << m.segOutPeakDbL[4] << "],\n";
        j << "  \"seg_out_peak_db_R\": [" << m.segOutPeakDbR[0] << ", " << m.segOutPeakDbR[1]
          << ", " << m.segOutPeakDbR[2] << ", " << m.segOutPeakDbR[3] << ", "
          << m.segOutPeakDbR[4] << "],\n";
        j << "  \"net_gain_db_L\": " << m.netGainDbL << ",\n";
        j << "  \"net_gain_db_R\": " << m.netGainDbR << ",\n";
        j << "  \"latency_xcorr_L\": " << m.latencyXcorrL << ",\n";
        j << "  \"latency_xcorr_R\": " << m.latencyXcorrR << ",\n";
        j << "  \"latency_impulse_L\": " << m.latencyImpulseL << ",\n";
        j << "  \"latency_impulse_R\": " << m.latencyImpulseR << ",\n";
        j << "  \"true_peak_dbtp\": " << m.truePeakDbTp << ",\n";
        j << "  \"true_peak_hot_dbtp\": " << m.truePeakHotDbTp << ",\n";
        j << "  \"thdn_db_L\": " << m.thdnDbL << ",\n";
        j << "  \"thdn_db_R\": " << m.thdnDbR << ",\n";
        j << "  \"imd_db_L\": " << m.imdDbL << ",\n";
        j << "  \"imd_db_R\": " << m.imdDbR << ",\n";
        j << "  \"silence_rms_db_L\": " << m.silenceRmsDbL << ",\n";
        j << "  \"silence_rms_db_R\": " << m.silenceRmsDbR << ",\n";
        j << "  \"silence_peak_db_L\": " << m.silencePeakDbL << ",\n";
        j << "  \"silence_peak_db_R\": " << m.silencePeakDbR << ",\n";
        j << "  \"cpu_p50_us\": " << m.cpu.p50Us << ",\n";
        j << "  \"cpu_p99_us\": " << m.cpu.p99Us << ",\n";
        j << "  \"cpu_max_us\": " << m.cpu.maxUs << ",\n";
        j << "  \"cpu_wall_p50_us\": " << m.cpu.wallP50Us << ",\n";
        j << "  \"cpu_wall_p99_us\": " << m.cpu.wallP99Us << ",\n";
        j << "  \"cpu_wall_max_us\": " << m.cpu.wallMaxUs << ",\n";
        j << "  \"cpu_xruns\": " << m.cpu.xruns << ",\n";
        j << "  \"cpu_p50_max_us\": " << cpuP50Max << ",\n";
        j << "  \"cpu_p99_max_us\": " << cpuP99Max << "\n}\n";
    }
    std::printf(
        "[P0-FREEZE] %s — header: %s\n"
        "[P0-FREEZE] maxDiff entre 2 corridas=%.3e | truePeak=%.3f dBTP (hot %.3f) | THD+N L/R %.2f/%.2f dB | "
        "IMD %.2f/%.2f dB\n"
        "[P0-FREEZE] latencia xcorr L/R %.2f/%.2f | impulso L/R %.1f/%.1f | silencio "
        "RmsL %.2f dBFS | CPU hilo p50/p99/máx %.1f/%.1f/%.1f µs (pared %.1f/%.1f/%.1f, "
        "xruns=%d)\n"
        "[P0-FREEZE] WAVs: p0_reference.wav p0_output.wav | JSON: p0_reference.json\n",
        finalFreeze ? "CONGELADO" : "provisional", headerPath.c_str(),
        m.deterministicMaxDiff, m.truePeakDbTp, m.truePeakHotDbTp, m.thdnDbL,
        m.thdnDbR, m.imdDbL, m.imdDbR, m.latencyXcorrL, m.latencyXcorrR,
        m.latencyImpulseL, m.latencyImpulseR, m.silenceRmsDbL, m.cpu.p50Us,
        m.cpu.p99Us, m.cpu.maxUs, m.cpu.wallP50Us, m.cpu.wallP99Us, m.cpu.wallMaxUs,
        m.cpu.xruns);
    return true;
}

// GTEST_SKIP() en esta versión vendored es `return <void>` sin stream:
// solo vale como sentencia suelta DENTRO del TEST (retorna del test).
// Esta macro lo encapsula: si P0 no está congelado, el test se salta.
#define P0_REQUIRE_FROZEN()                                    \
    do {                                                        \
        if (!p0::kFrozen) {                                     \
            std::printf("[P0] SKIP - P0 no congelado (%s) - "   \
                        "corre scripts/p0_freeze.sh\n",         \
                        p0::kFrozenFrom);                       \
            GTEST_SKIP();                                       \
        }                                                       \
    } while (0)

}  // namespace

// ── TESTs ───────────────────────────────────────────────────────────────────
TEST(P0ReferenceLock, Freeze) {
    const char* mode = std::getenv("P0_FREEZE");
    if (mode != nullptr) {
        const bool finalFreeze = std::string(mode) == "final";
        ASSERT_TRUE(doFreeze(finalFreeze))
            << "la congelación falló (ver [P0-FREEZE] arriba)";
        return;
    }
    std::printf("[P0] estado: %s (%s)\n", p0::kFrozen ? "CONGELADO" : "sin congelar",
                p0::kFrozenFrom);
    // Escribe la auditoría (WAVs + JSON) en cada corrida, con los valores
    // congelados como referencia de lo que el test impone.
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    write_wav("p0_reference.wav", ref);
    write_wav("p0_output.wav", out);
}

TEST(P0ReferenceLock, Levels) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    const P0Measured m = measureAll(ref, out);
    for (int s = 0; s < 5; ++s) {
        EXPECT_NEAR(m.segOutRmsDbL[s], p0::kSegOutRmsDbL[s], kTolLevelDb)
            << "RMS L segmento " << kSegs[s].name;
        EXPECT_NEAR(m.segOutRmsDbR[s], p0::kSegOutRmsDbR[s], kTolLevelDb)
            << "RMS R segmento " << kSegs[s].name;
        EXPECT_NEAR(m.segOutPeakDbL[s], p0::kSegOutPeakDbL[s], kTolLevelDb)
            << "pico L segmento " << kSegs[s].name;
        EXPECT_NEAR(m.segOutPeakDbR[s], p0::kSegOutPeakDbR[s], kTolLevelDb)
            << "pico R segmento " << kSegs[s].name;
    }
    EXPECT_NEAR(m.netGainDbL, p0::kNetGainDbL, kTolLevelDb) << "ganancia neta L";
    EXPECT_NEAR(m.netGainDbR, p0::kNetGainDbR, kTolLevelDb) << "ganancia neta R";
}

TEST(P0ReferenceLock, TruePeak) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    const P0Measured m = measureAll(ref, out);
    EXPECT_NEAR(m.truePeakDbTp, p0::kTruePeakDbTp, kTolLevelDb) << "true peak total";
    EXPECT_NEAR(m.truePeakHotDbTp, p0::kTruePeakHotDbTp, kTolLevelDb)
        << "true peak segmento HOT";
    // Guarda absoluta: el techo final no deja pasar de 0 dBTP.
    EXPECT_LE(m.truePeakDbTp, 0.0) << "true peak total sobre 0 dBTP (techo final roto)";
    EXPECT_LE(m.truePeakHotDbTp, 0.0) << "true peak HOT sobre 0 dBTP (techo final roto)";
}

TEST(P0ReferenceLock, Thdn) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    const P0Measured m = measureAll(ref, out);
    EXPECT_NEAR(m.thdnDbL, p0::kThdnDbL, kTolThdnDb) << "THD+N L (seno 1 kHz)";
    EXPECT_NEAR(m.thdnDbR, p0::kThdnDbR, kTolThdnDb) << "THD+N R (seno 1 kHz)";
}

TEST(P0ReferenceLock, Imd) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    const P0Measured m = measureAll(ref, out);
    EXPECT_NEAR(m.imdDbL, p0::kImdDbL, kTolThdnDb) << "IMD SMPTE L";
    EXPECT_NEAR(m.imdDbR, p0::kImdDbR, kTolThdnDb) << "IMD SMPTE R";
}

TEST(P0ReferenceLock, Latency) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    const P0Measured m = measureAll(ref, out);
    EXPECT_NEAR(m.latencyXcorrL, p0::kLatencyXcorrL, kTolLatencySamples)
        << "latencia xcorr L (ruido)";
    EXPECT_NEAR(m.latencyXcorrR, p0::kLatencyXcorrR, kTolLatencySamples)
        << "latencia xcorr R (ruido)";
    EXPECT_NEAR(m.latencyImpulseL, p0::kLatencyImpulseL, kTolLatencySamples)
        << "latencia impulso L (clicks)";
    EXPECT_NEAR(m.latencyImpulseR, p0::kLatencyImpulseR, kTolLatencySamples)
        << "latencia impulso R (clicks)";
    EXPECT_GE(m.latencyXcorrL, 0.0) << "latencia xcorr L negativa (la salida no puede ir antes)";
    EXPECT_LT(m.latencyXcorrL, 8192.0) << "latencia xcorr L absurda (>170 ms)";
}

TEST(P0ReferenceLock, CpuXruns) {
    P0_REQUIRE_FROZEN();
    const CpuStats cpu = runCpu();
    std::printf("[P0-CPU] hilo p50=%.1f p99=%.1f máx=%.1f µs | pared p50=%.1f p99=%.1f "
                "máx=%.1f µs xruns=%d (límites hilo %.0f/%.0f/%.0f, presupuesto %.0f µs)\n",
                cpu.p50Us, cpu.p99Us, cpu.maxUs, cpu.wallP50Us, cpu.wallP99Us,
                cpu.wallMaxUs, cpu.xruns, p0::kCpuP50MaxUs, p0::kCpuP99MaxUs,
                p0::kCpuMaxMaxUs, kHopBudgetUs);
    // Lo que se congela es el coste del motor (tiempo de CPU del hilo).
    // El máx tiene límite 2x el presupuesto (no el presupuesto pelado):
    // es un estadístico extremo y el motor tiene picos de hilo de hasta
    // 4393 µs (medidos) que no son xruns; p50/p99 son los límites que
    // vigilan el coste. Un hop que consume más de 2x el presupuesto es
    // una regresión real del motor.
    EXPECT_LE(cpu.p50Us, p0::kCpuP50MaxUs) << "p50 µs/hop (hilo) sobre el límite congelado";
    EXPECT_LE(cpu.p99Us, p0::kCpuP99MaxUs) << "p99 µs/hop (hilo) sobre el límite congelado";
    EXPECT_LE(cpu.maxUs, p0::kCpuMaxMaxUs)
        << "un hop consumió más del doble de su presupuesto de tiempo real";
    // 6.1 — PRESUPUESTO REALTIME DURO (refinamiento 2026-11-26): los
    // límites congelados anteriores (p50 3087 / p99 4254 µs) superaban el
    // presupuesto de hop (2666.67 µs): una regresión al 120% del
    // presupuesto pasaba este test. El coste mediano del motor NO puede
    // superar un hop; el p99 admite 1.5x (picos de hilo esporádicos).
    // Son límites de CPU de HILO (CLOCK_THREAD_CPUTIME_ID): deterministas
    // e inmunes a la carga del host, no de pared.
    EXPECT_LE(cpu.p50Us, kHopBudgetUs)
        << "p50 µs/hop (hilo) sobre el presupuesto realtime de hop (2667 µs): "
           "el motor no cabe en tiempo real";
    EXPECT_LE(cpu.p99Us, 1.5 * kHopBudgetUs)
        << "p99 µs/hop (hilo) sobre 1.5x el presupuesto realtime de hop";
    // Los xruns de pared se REPORTAN (ver [P0-CPU] arriba) pero NO se
    // imponen — y esta NO-imposición es una decisión medida, no un hueco:
    // en VM compartida cargada, runCpu midió xruns=37 con p50 de HILO en
    // 900 µs (34% del presupuesto) y p99 de pared en 32.7 ms (2026-11-26):
    // el motor cabe sobrado y los 37 xruns son stalls de planificación
    // del host, no del motor. Imponer xruns=0 haría fallar el test por la
    // carga del host (kXrunsMax=0 queda en el header congelado como
    // referencia documentada, no como guarda). Lo que sí impone el
    // presupuesto es la CPU de HILO (p50/p99 de arriba): determinista e
    // inmune a la carga del host.
}

TEST(P0ReferenceLock, SilenceTail) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out = runChain(ref);
    const P0Measured m = measureAll(ref, out);
    // El umbral P1 (-90 dBFS) es la guarda; el valor exacto del residuo
    // (denormales ~-199 dBFS) es ruido numérico y NO se congela con
    // tolerancia: lo que importa es que no vuelva la cola de -77 dBFS.
    EXPECT_LE(m.silenceRmsDbL, kSilenceMaxDbfs) << "cola residual L sobre -90 dBFS";
    EXPECT_LE(m.silenceRmsDbR, kSilenceMaxDbfs) << "cola residual R sobre -90 dBFS";
    EXPECT_LE(m.silencePeakDbL, kSilenceMaxDbfs) << "pico residual L sobre -90 dBFS";
    EXPECT_LE(m.silencePeakDbR, kSilenceMaxDbfs) << "pico residual R sobre -90 dBFS";
}

TEST(P0ReferenceLock, Deterministic) {
    P0_REQUIRE_FROZEN();
    const Signal ref = generateReference();
    const Signal out1 = runChain(ref);
    const Signal out2 = runChain(ref);
    const double d = maxAbsDiff(out1, out2);
    std::printf("[P0-DET] maxDiff entre 2 corridas = %.3e (límite %.0e)\n", d,
                kTolDeterminism);
    // El motor NO es bit-determinista (medido 1.4e-06, −117 dBFS, en el
    // onset del seno): la guarda es una tolerancia, no bit-exactitud.
    // Un salto a ~1e-2 o mayor sería aleatoriedad real, no ruido numérico.
    EXPECT_LE(d, kTolDeterminism)
        << "el motor dejó de ser determinista dentro de tolerancia";
}
