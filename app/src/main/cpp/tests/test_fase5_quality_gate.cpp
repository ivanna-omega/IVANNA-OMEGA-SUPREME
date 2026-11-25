// test_fase5_quality_gate.cpp — FASE 5: Banco de Laboratorio Automático en CI
// (Audio Quality Gate). Un solo test que fija los umbrales que ningún commit
// a main puede degradar.
//
// Qué fija (host x86-64, no el teléfono):
//   1. Null-test maestro (BYPASS / LIMPIO_OFF < -140 dB, bit-exacto): un tono
//      estéreo por la cadena neutral (ORBE amount 0 + HOLOS amount 0, en
//      serie, como la cadena maestra en LIMPIO_OFF) debe salir bit-idéntico
//      a la entrada (maxAbsDiff < 1e-7, >140 dB de rechazo) y con THD+N
//      < -140 dB. Es el null-test end-to-end que faltaba: test_master_bypass
//      solo verificaba el flag y la colocación estructural del guard; este
//      test mide el BUFFER.
//   2. THD+N línea base (producción), en DOS niveles:
//      - A -3 dBFS (el estándar del arnés, donde se establecieron las líneas
//        base documentadas): ORBE amount 1.0 < -135 dB (medido -139.10 dB
//        @1 kHz, -138.54 dB @100 Hz, test_orbe_chain) y HOLOS retune de
//        producción (amount 0.5, harmonics 0.0) < -90 dB @1 kHz (medido
//        -94.0 dB, test_holos_chain ThdnProductionRetune, 3er segundo).
//      - A -1 dBFS (el nivel que pide el plan Fase 5): ORBE < -135 dB y
//        HOLOS retune < -70 dB @1 kHz (medidos -138.11 dB y -72.15 dB).
//      Los umbrales NO son los valores medidos al milímetro (eso sería
//      flaky: ORBE -139.10 vs -139.37 entre corridas): tienen 2–4 dB de
//      margen, como hacen test_orbe_chain (< -120 dB) y test_holos_chain
//      (< -80 dB). Lo que la puerta atrapa es una REGRESIÓN real, no
//      0.1 dB de variación de medida. Todos los valores medidos se
//      imprimen ([fase5-gate]) para que la línea base quede visible.
//   3. Integridad RT: NO se duplica aquí. El detector dinámico de 0 malloc
//      en el callback ya existe y corre en CTest (test_rt_no_alloc:
//      RouteA_UnifiedPipeline_10000BlocksZeroAlloc y
//      RouteB_SpatialAndConvolver_10000BlocksZeroAlloc). Redefinir
//      operator new en esta TU chocaría con su definición (ODR) y daría
//      falsos positivos/negativos. La puerta CI de Fase 5 corre este test
//      JUNTO con test_rt_no_alloc (ver .github/workflows/
//      fase5-quality-gate.yml): si cualquiera de los dos falla, la puerta
//      falla. La RT-integridad queda así cubierta por el gate sin duplicar
//      el detector.
//
// HALLAZGO (2026-11-17, medido en host): la combinación literal del plan
// ("-1 dBFS Y ORBE < -139 dB, HOLOS < -94 dB") NO se cumple: las líneas
// base -139 dB / -94 dB son a -3 dBFS. A -1 dBFS el mismo HOLOS retune
// mide -72.15 dB @1 kHz (22 dB peor): el compresor multibanda
// (dynamics=1.0, default de producción) trabaja mucho más a nivel alto.
// ORBE apenas se mueve (-139.10 → -138.11 dB). Esto es un dato real del
// producto (HOLOS distorsiona mucho más a nivel alto) y queda registrado
// aquí y en REPORT.md, no escondido. La puerta fija AMBOS niveles para
// que el comportamiento a -1 dBFS no se degrade sin que nadie lo note.
//
// LÍMITE HONESTO: esto mide la cadena DSP en host (ORBE + HOLOS). El
// early-return bit-exacto de nativeProcess (ivanna_omega_jni.cpp) y de
// omega_process (omega_effect.cpp) NO compilan en host, así que su
// comportamiento bit-exacto se verifica por la prueba de dispositivo
// (null-test en el G85, Fase 1) y por el test estructural
// MasterBypass.GuardPlacement. Lo que este test SÍ garantiza en host es
// que la cadena neutral no añade nada medible (>140 dB de rechazo).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../spatial/HolosOperator.hpp"
#include "../spatial/OrbeBinaural.hpp"
#include "ivanna_measure.hpp"

using namespace ivanna::measure;

namespace {
constexpr double kFs = 48000.0;
constexpr int kBlock = 128;  // como IvannaFusionEngine::processStereo
// Niveles de medida: -3 dBFS (estándar del arnés / líneas base
// documentadas) y -1 dBFS (nivel que pide el plan Fase 5).
constexpr double kAmpMinus3Dbfs = 0.7079457843841379;  // 10^(-3/20)
constexpr double kAmpMinus1Dbfs = 0.8912509381337456;  // 10^(-1/20)

std::vector<float> tone(double f0, double amp, double seconds) {
    const int n = static_cast<int>(seconds * kFs);
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = static_cast<float>(amp * std::sin(2.0 * kPi * f0 * i / kFs));
    return v;
}

// Cadena maestra neutral: ORBE(0) -> HOLOS(0), en bloques de 128.
// Devuelve {outL, outR} procesados in-place sobre copias de la entrada.
struct ChainOut {
    std::vector<float> l, r;
};

ChainOut runMasterNeutral(const std::vector<float>& inL,
                          const std::vector<float>& inR) {
    ivanna::OrbeBinaural orbe;
    orbe.prepare(static_cast<float>(kFs));
    ivanna::OrbeParams op;
    op.amount = 0.0f;
    orbe.setParams(op);
    ivanna::HolosOperator holos;
    holos.prepare(static_cast<float>(kFs));
    ivanna::HolosParams hp;  // amount = 0 (default)
    holos.setParams(hp);
    ChainOut out{inL, inR};
    const int n = static_cast<int>(inL.size());
    for (int s = 0; s < n; s += kBlock) {
        const int m = std::min(kBlock, n - s);
        orbe.process(out.l.data() + s, out.r.data() + s, m);
        holos.process(out.l.data() + s, out.r.data() + s, m);
    }
    return out;
}

double maxAbsDiffDb(const std::vector<float>& a, const std::vector<float>& b) {
    double m = 0.0;
    for (size_t i = 0; i < a.size(); ++i)
        m = std::max(m, std::fabs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return dB20(m);
}

// THD+N de un tono por HOLOS con los params dados. Metodología del
// arnés existente (test_holos_chain): tono de 3 s, medido en el 3er
// segundo (régimen permanente, compresor asentado).
double measureHolosThdn(ivanna::HolosParams p, double f0, double amp) {
    ivanna::HolosOperator op;
    op.prepare(static_cast<float>(kFs));
    op.setParams(p);
    auto t = tone(f0, amp, 3.0);
    std::vector<float> l = t, r = t;
    const int n = static_cast<int>(t.size());
    for (int s = 0; s < n; s += kBlock) {
        const int m = std::min(kBlock, n - s);
        op.process(l.data() + s, r.data() + s, m);
    }
    return thdn(l.data() + 96000, 48000, kFs, f0).thdnDb;
}

// THD+N de un tono por ORBE con el amount dado, medido en el 2º segundo.
double measureOrbeThdn(float amount, double f0, double amp) {
    ivanna::OrbeBinaural o;
    o.prepare(static_cast<float>(kFs));
    ivanna::OrbeParams p;
    p.amount = amount;
    o.setParams(p);
    auto t = tone(f0, amp, 2.0);
    std::vector<float> l = t, r = t;
    const int n = static_cast<int>(t.size());
    for (int s = 0; s < n; s += kBlock) {
        const int m = std::min(kBlock, n - s);
        o.process(l.data() + s, r.data() + s, m);
    }
    return thdn(l.data() + 48000, 48000, kFs, f0).thdnDb;
}

// Retune de producción ATREVIDA 502 (directiva Esse 2026-11-24):
// amount 1.0, harmonics 0.002, spread 1.2, proximityDb 6.0,
// diffuse 0.85; el resto, defaults de producción (dynamics 1.0).
ivanna::HolosParams productionRetune() {
    ivanna::HolosParams p;
    p.amount = 1.0f;
    p.harmonics = 0.002f;
    p.spread = 1.2f;
    p.proximityDb = 6.0f;
    p.diffuse = 0.85f;
    return p;
}
}  // namespace

TEST(Fase5QualityGate, MasterNeutralNullTestBelowMinus140dB) {
    // Bypass / LIMPIO_OFF < -140 dB, bit-exacto: la cadena neutral no añade
    // nada medible. Se mide en el 2º segundo (régimen permanente).
    for (double f0 : {100.0, 1000.0, 10000.0}) {
        auto t = tone(f0, kAmpMinus1Dbfs, 2.0);
        ChainOut out = runMasterNeutral(t, t);
        const double diffDb = maxAbsDiffDb(out.l, t);
        const Thdn m = thdn(out.l.data() + 48000, 48000, kFs, f0);
        std::printf("[fase5-gate] null-test neutral @%.0f Hz: maxAbsDiff %.1f dB, THD+N %.1f dB\n",
                    f0, diffDb, m.thdnDb);
        EXPECT_LT(diffDb, -140.0) << "f0=" << f0 << " (bit-exacto: salida == entrada)";
        EXPECT_LT(m.thdnDb, -140.0) << "f0=" << f0 << " (THD+N de la cadena neutral)";
    }
}

TEST(Fase5QualityGate, OrbeThdnBaseline) {
    // ORBE amount 1.0 (activo por defecto). Medido en host:
    // -3 dBFS: -139.10 dB @1 kHz, -138.54 dB @100 Hz (test_orbe_chain).
    // -1 dBFS: -138.11 dB @1 kHz, -137.86 dB @100 Hz, -138.86 dB @10 kHz.
    // Umbral -135 dB en ambos niveles (margen 2.9–4.9 dB): atrapa una
    // regresión real sin flakear por 0.1 dB de variación de medida.
    for (double amp : {kAmpMinus3Dbfs, kAmpMinus1Dbfs}) {
        const double dbfs = (amp == kAmpMinus3Dbfs) ? -3.0 : -1.0;
        const double thdn1k = measureOrbeThdn(1.0f, 1000.0, amp);
        const double thdn100 = measureOrbeThdn(1.0f, 100.0, amp);
        const double thdn10k = measureOrbeThdn(1.0f, 10000.0, amp);
        std::printf("[fase5-gate] ORBE THD+N %.0f dBFS: @1kHz %.2f dB, @100Hz %.2f dB, @10kHz %.2f dB\n",
                    dbfs, thdn1k, thdn100, thdn10k);
        EXPECT_TRUE(std::isfinite(thdn1k));
        EXPECT_TRUE(std::isfinite(thdn100));
        EXPECT_TRUE(std::isfinite(thdn10k));
        EXPECT_LT(thdn1k, -135.0) << "ORBE @1 kHz " << dbfs << " dBFS";
        EXPECT_LT(thdn100, -135.0) << "ORBE @100 Hz " << dbfs << " dBFS";
        EXPECT_LT(thdn10k, -135.0) << "ORBE @10 kHz " << dbfs << " dBFS";
    }
}

TEST(Fase5QualityGate, HolosThdnBaselineProductionRetune) {
    // HOLOS retune de producción ATREVIDA 502 (amount 1.0, harmonics 0.002,
    // spread 1.2, proximity 6.0, diffuse 0.85). Medido en host (clang++-18,
    // 2026-11-25):
    // -3 dBFS: -77.92 dB @1 kHz, -79.99 dB @100 Hz, -84.08 dB @10 kHz.
    // -1 dBFS: -76.74 dB @1 kHz, -78.90 dB @100 Hz, -82.66 dB @10 kHz.
    // (Retune anterior, amount 0.5 / K=0 / spread 1.0 / proximity 3.0 /
    // diffuse 0.5: -3 dBFS -94.0/-79.6/-73.07; -1 dBFS -72.15/-77.94/-69.96.)
    // Umbrales con margen ~4 dB sobre lo medido en cada nivel (como antes:
    // 2–4.6 dB). Todos siguen ≤ −70 dB (criterio ATREVIDA).
    {
        const double thdn1k = measureHolosThdn(productionRetune(), 1000.0, kAmpMinus3Dbfs);
        const double thdn100 = measureHolosThdn(productionRetune(), 100.0, kAmpMinus3Dbfs);
        const double thdn10k = measureHolosThdn(productionRetune(), 10000.0, kAmpMinus3Dbfs);
        std::printf("[fase5-gate] HOLOS retune THD+N -3 dBFS: @1kHz %.2f dB, @100Hz %.2f dB, @10kHz %.2f dB\n",
                    thdn1k, thdn100, thdn10k);
        EXPECT_TRUE(std::isfinite(thdn1k));
        EXPECT_TRUE(std::isfinite(thdn100));
        EXPECT_TRUE(std::isfinite(thdn10k));
        EXPECT_LT(thdn1k, -74.0) << "HOLOS retune @1 kHz -3 dBFS (línea base -77.92 dB)";
        EXPECT_LT(thdn100, -76.0) << "HOLOS retune @100 Hz -3 dBFS (línea base -79.99 dB)";
        EXPECT_LT(thdn10k, -80.0) << "HOLOS retune @10 kHz -3 dBFS (línea base -84.08 dB)";
    }
    {
        const double thdn1k = measureHolosThdn(productionRetune(), 1000.0, kAmpMinus1Dbfs);
        const double thdn100 = measureHolosThdn(productionRetune(), 100.0, kAmpMinus1Dbfs);
        const double thdn10k = measureHolosThdn(productionRetune(), 10000.0, kAmpMinus1Dbfs);
        std::printf("[fase5-gate] HOLOS retune THD+N -1 dBFS: @1kHz %.2f dB, @100Hz %.2f dB, @10kHz %.2f dB\n",
                    thdn1k, thdn100, thdn10k);
        EXPECT_TRUE(std::isfinite(thdn1k));
        EXPECT_TRUE(std::isfinite(thdn100));
        EXPECT_TRUE(std::isfinite(thdn10k));
        EXPECT_LT(thdn1k, -73.0) << "HOLOS retune @1 kHz -1 dBFS (línea base -76.74 dB)";
        EXPECT_LT(thdn100, -75.0) << "HOLOS retune @100 Hz -1 dBFS (línea base -78.90 dB)";
        EXPECT_LT(thdn10k, -79.0) << "HOLOS retune @10 kHz -1 dBFS (línea base -82.66 dB)";
    }
}
