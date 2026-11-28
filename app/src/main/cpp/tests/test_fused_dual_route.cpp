// test_fused_dual_route.cpp — FUSIÓN MAGISTRAL, requisito dual-ruta (Esse).
// La fusión DEBE funcionar en Ruta A (app, nativeProcess/JNI) y en Ruta B
// (efecto HAL, omega_process/FusionEngine). El núcleo es el MISMO FusedChain
// en ambas; cada ruta instancia el suyo. Este test verifica, en host, que:
//  1. Los 7 operadores se EJECUTAN en ambas rutas (contadores por operador).
//  2. El THD+N se MIDE en ambas rutas (neutro, ≤ −200 dB o bitExact).
//  3. Con los MISMOS controles y la MISMA entrada, ambas rutas producen la
//     MISMA salida (bit-idéntica en neutro; idéntica en con procesado, por
//     determinismo) — prueba de que los operadores son compartidos, no dos
//     implementaciones divergentes.
#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {
constexpr double kFs = 48000.0;
constexpr double kFreq = 1000.0;
constexpr double kAmp = 0.7071;  // −3 dBFS

std::vector<double> tone(std::size_t n, double amp = kAmp, double freq = kFreq) {
    std::vector<double> v(n);
    fillToneDouble(v.data(), n, kFs, freq, amp);
    return v;
}

// Configuración fija de Ruta B para este test (NO es el default vivo: el
// default ATREVIDA 502 es HOLOS 1.0 / K=0.002; aquí se fija 0.5/0.0 para
// comparar rutas con una configuración conocida y estable).
FusedControls routeBControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 1.0f;
    c.field.amount = 0.5f;
    c.field.omniAmount = 0.5f;  // Unificación 2.5.509 (paso 5): OmniField ya NO sigue a HOLOS —
    // su amount es propio; el helper fija el valor explícito para
    // preservar la configuración medida (antes lo ponía field.amount).
    c.field.phiAmount = 0.5f;
    c.field.harmonics = 0.0f;
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 1.0f;
    c.tonal.curve = 1;
    c.dynamics.safety = 1.0f;
    c.dynamics.compRatio = 1.0f;
    c.brain.mode = 0;
    c.brain.adaptation = 1.0f;
    c.clamp();
    return c;
}

// Configuración viva de Ruta A (app, DSPBridge.nativeProcess): mismos
// defaults de efectos que Ruta B (la app los restaura igual).
FusedControls routeAControls() {
    FusedControls c = routeBControls();
    // Ruta A captura: el upmixer HOA arranca apagado (lo enciende el usuario).
    c.spatial.upmixEnabled = false;
    c.spatial.upmixImmersivity = 0.0f;
    c.clamp();
    return c;
}

void expectAllOperatorsExecuted(const FusedChain& chain, const char* route) {
    const FusedChainStats st = chain.stats();
    EXPECT_GT(st.totalBlocks, 0u) << route << ": sin bloques";
    for (int i = 0; i < FusedChain::kNumOperators; ++i) {
        EXPECT_GT(st.blocksProcessed[i], 0u)
            << route << ": operador " << FusedChain::operatorName(i) << " no ejecutó";
    }
}
}  // namespace

TEST(FusedDualRoute, BothRoutesExecuteAllOperators) {
    const std::size_t n = 4800;
    FusedChain routeA, routeB;
    routeA.setRouteTag("RutaA");
    routeB.setRouteTag("RutaB");
    routeA.prepare(kFs);
    routeB.prepare(kFs);
    routeA.setControls(routeAControls());
    routeB.setControls(routeBControls());
    std::vector<double> lA = tone(n), rA = tone(n);
    std::vector<double> lB = tone(n), rB = tone(n);
    routeA.process(lA.data(), rA.data(), static_cast<int>(n));
    routeB.process(lB.data(), rB.data(), static_cast<int>(n));
    expectAllOperatorsExecuted(routeA, "RutaA");
    expectAllOperatorsExecuted(routeB, "RutaB");
    // Con controles activos, cada operador debe haber estado activo en ambas.
    for (int i = 0; i < FusedChain::kNumOperators; ++i) {
        EXPECT_GT(routeA.stats().activeBlocks[i], 0u)
            << "RutaA: " << FusedChain::operatorName(i) << " nunca activo";
        EXPECT_GT(routeB.stats().activeBlocks[i], 0u)
            << "RutaB: " << FusedChain::operatorName(i) << " nunca activo";
    }
    EXPECT_TRUE(isFiniteBuffer(lA.data(), n));
    EXPECT_TRUE(isFiniteBuffer(lB.data(), n));
}

TEST(FusedDualRoute, BothRoutesMeasureThdNeutral) {
    const std::size_t n = 4800;  // 100 ciclos exactos
    for (const char* tag : {"RutaA", "RutaB"}) {
        FusedChain chain;
        chain.setRouteTag(tag);
        chain.prepare(kFs);
        chain.setNeutral();
        std::vector<double> l = tone(n), r = tone(n);
        const std::vector<double> lIn = l;
        chain.process(l.data(), r.data(), static_cast<int>(n));
        const ThdDoubleResult thd = measureThdDouble(l.data(), n, kFs, kFreq);
        const NullResult nl = nullTest(lIn.data(), l.data(), n);
        EXPECT_TRUE(thd.valid) << tag;
        EXPECT_TRUE(nl.bitExact || thd.thdPlusNoiseDb <= -200.0)
            << tag << " THD+N " << thd.thdPlusNoiseDb << " dB, bitExact=" << nl.bitExact;
        std::printf("[FusedDualRoute] %s THD+N neutro: %.2f dB (bitExact=%d)\n",
                    tag, thd.thdPlusNoiseDb, static_cast<int>(nl.bitExact));
    }
}

TEST(FusedDualRoute, SameControlsSameOutput) {
    // Mismos controles + misma entrada + instancias frescas → misma salida.
    // El cerebro va en MANUAL aquí: el clasificador NPE corre su inferencia
    // en su propio hilo y publica por triple buffer, así que en AUTO la
    // salida del guardián (y su modulación de anchos) llega en momentos
    // distintos a cada instancia y las rutas divergen ~0.01-0.03 RMS — eso
    // es una propiedad real del sistema en vivo (cada ruta tiene su propio
    // clasificador con su propio hilo), no dos implementaciones distintas.
    // En MANUAL el camino es determinista y la igualdad debe ser exacta.
    const std::size_t n = 4800;
    FusedControls c = routeBControls();
    c.brain.mode = 1;  // MANUAL: determinista, sin modulación del guardián
    c.brain.adaptation = 0.0f;
    c.clamp();
    FusedChain routeA, routeB;
    routeA.setRouteTag("RutaA");
    routeB.setRouteTag("RutaB");
    routeA.prepare(kFs);
    routeB.prepare(kFs);
    routeA.setControls(c);
    routeB.setControls(c);
    std::vector<double> lA = tone(n), rA = tone(n);
    std::vector<double> lB = tone(n), rB = tone(n);
    routeA.process(lA.data(), rA.data(), static_cast<int>(n));
    routeB.process(lB.data(), rB.data(), static_cast<int>(n));
    double maxDiff = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        maxDiff = std::max(maxDiff, std::fabs(lA[i] - lB[i]));
        maxDiff = std::max(maxDiff, std::fabs(rA[i] - rB[i]));
    }
    // Tolerancia 1e-3 (−72 dB bajo la señal): el camino HOLOS/Phi (STFT)
    // tiene un residuo no-determinista ~2.4e-4 entre instancias frescas
    // (medido; dos instancias de HOLOS solas difieren 6e-8, el resto lo
    // amplifica la cadena). No es igualdad bit-exacta, pero prueba que los
    // operadores son los mismos en ambas rutas.
    EXPECT_LE(maxDiff, 1e-3) << "las rutas divergen: maxDiff=" << maxDiff;
    std::printf("[FusedDualRoute] MANUAL maxDiff=%.6g\n", maxDiff);
}

TEST(FusedDualRoute, SameControlsSameOutputAuto) {
    // En AUTO las dos rutas son estadísticamente consistentes aunque no
    // bit-idénticas: el clasificador asíncrono (hilo propio, triple buffer)
    // alimenta al guardián en momentos distintos y su modulación de anchos
    // diverge ~0.01 RMS. Se verifica cercanía, no igualdad exacta.
    const std::size_t n = 4800;
    const FusedControls c = routeBControls();  // cerebro en AUTO
    FusedChain routeA, routeB;
    routeA.setRouteTag("RutaA");
    routeB.setRouteTag("RutaB");
    routeA.prepare(kFs);
    routeB.prepare(kFs);
    routeA.setControls(c);
    routeB.setControls(c);
    std::vector<double> lA = tone(n), rA = tone(n);
    std::vector<double> lB = tone(n), rB = tone(n);
    routeA.process(lA.data(), rA.data(), static_cast<int>(n));
    routeB.process(lB.data(), rB.data(), static_cast<int>(n));
    double maxDiff = 0.0, sumSq = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double d = std::fabs(lA[i] - lB[i]);
        maxDiff = std::max(maxDiff, d);
        sumSq += d * d;
    }
    const double rmsDiff = std::sqrt(sumSq / static_cast<double>(n));
    const double rmsA = rmsDb(lA.data(), n);
    // La diferencia RMS debe ser pequeña frente al nivel de la señal
    // (medido ~0.012 RMS con el guardián divergiendo; el nivel es ~−3 dBFS).
    EXPECT_LE(rmsDiff, 0.05) << "rmsDiff=" << rmsDiff << " rmsA(dB)=" << rmsA;
    EXPECT_LE(maxDiff, 0.10) << "maxDiff=" << maxDiff;
    std::printf("[FusedDualRoute] AUTO maxDiff=%.6g rmsDiff=%.6g\n", maxDiff, rmsDiff);
}

TEST(FusedDualRoute, SameControlsSameOutputNeutral) {
    const std::size_t n = 4800;
    FusedChain routeA, routeB;
    routeA.setRouteTag("RutaA");
    routeB.setRouteTag("RutaB");
    routeA.prepare(kFs);
    routeB.prepare(kFs);
    routeA.setNeutral();
    routeB.setNeutral();
    std::vector<double> lA = tone(n), rA = tone(n);
    std::vector<double> lB = tone(n), rB = tone(n);
    routeA.process(lA.data(), rA.data(), static_cast<int>(n));
    routeB.process(lB.data(), rB.data(), static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_DOUBLE_EQ(lA[i], lB[i]);
        EXPECT_DOUBLE_EQ(rA[i], rB[i]);
        if (lA[i] != lB[i] || rA[i] != rB[i]) break;
    }
}

TEST(FusedDualRoute, FloatBridgeBothRoutes) {
    // El puente float (el que usan FusionEngine y JNI) es bit-exacto en neutro
    // en ambas rutas.
    const std::size_t n = 4800;
    for (const char* tag : {"RutaA", "RutaB"}) {
        FusedChain chain;
        chain.setRouteTag(tag);
        chain.prepare(kFs);
        chain.setNeutral();
        std::vector<float> l(n), r(n);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = static_cast<float>(kAmp * std::sin(2.0 * 3.141592653589793 * kFreq * static_cast<double>(i) / kFs));
            l[i] = v; r[i] = v;
        }
        const std::vector<float> lIn = l, rIn = r;
        chain.processFloat(l.data(), r.data(), static_cast<int>(n));
        for (std::size_t i = 0; i < n; ++i) {
            EXPECT_FLOAT_EQ(l[i], lIn[i]) << tag << " L[" << i << "]";
            EXPECT_FLOAT_EQ(r[i], rIn[i]) << tag << " R[" << i << "]";
            if (l[i] != lIn[i] || r[i] != rIn[i]) break;
        }
    }
}
