// test_bofo_isolation_matrix.cpp — Matriz de aislamiento bofo/tronidos
// (Ruta A) + presupuesto de tiempo real, medidos en host.
//
// CONTEXTO: el fix ISO 226 de la v2.5.441 NO resolvió el bofo; es problema
// de Ruta A. La atribución de los tronidos en Tidal sigue abierta. Este
// archivo NO declara el bofo resuelto: fija en tests lo que SÍ se puede
// medir en host y deja el protocolo de aislamiento en dispositivo como
// matriz testeable (cada fila: condición → observable → veredicto).
//
// Lo que se mide aquí, con números:
//   1. Presupuesto RT real: periodo = frames / sampleRate (NO 6.67 ms
//      universal). Tabla para los tamaños de bloque reales de la cadena.
//   2. Peine original + copia reinyectada: rizado pico a pico
//      20*log10((1+x)/(1-x)), x = ganancia coherente g*|H|. Con el techo
//      de diseño del DifferentialReinjector (x <= 0.0965) el rizado pico a
//      pico es 1.68 dB (0.8 dB por lado: ver hallazgo de definición en el
//      test CombRippleDepthIsSetByCoherentGainNotDelay); con delta coherente
//      a ganancia 0.60 (la copia procesada que suena junto al original) es
//      12.0 dB: coloración grave ("bofo") por peine, no por EQ. El retardo L solo mueve los
//      dientes (1/L Hz); la profundidad la fija la ganancia coherente.
//   3. DifferentialReinjector en host: wet == dry -> delta ~ 0 (la
//      reinyección no inventa señal); wet atenuado coherente -> la
//      salida no amplifica y el riesgo de peine queda acotado por el
//      guardián. Perfil p50/p95/máx del process() en host.
//   4. Crossfade Volterra: el release es 18 ms EN TIEMPO (Supreme
//      TransitionEnvelope::release_ms), así que en muestras es
//      0.018 * sampleRate: 794 @44.1k, 864 @48k, 1728 @96k, 3456 @192k.
//      Un fundido "de 18 ms" en muestras fijas sería incorrecto a otra
//      tasa. Además: equal-power NO mantiene energía constante para
//      señales correlacionadas (la suma coherente suma en amplitud,
//      no en potencia): el test lo demuestra con dos senos en fase.
//
// PENDIENTE (necesita dispositivo + Esse): la matriz completa de
// aislamiento en el teléfono (fuente sin IVANNA, BridgePlayer bypass vs
// procesado, captura, módulos individuales, archivo local vs streaming,
// salidas). Ver docs en el comentario final del archivo.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

#include "../include/differential_reinjector.hpp"

namespace {

// Presupuesto RT: periodo de un bloque, en ms. frames/sampleRate, con la
// arquitectura REAL de buffers: el periodo depende del tamaño de bloque
// que entrega el callback, no hay un 6.67 ms universal.
double rtBudgetMs(int frames, double sampleRate) {
    return 1000.0 * static_cast<double>(frames) / sampleRate;
}

// Rizado de peine pico a pico (dB) para ganancia coherente x = g*|H|:
// y(t) = x(t) + g*delta(t-L), delta coherente con x -> |Y/X| oscila
// entre (1+x) y (1-x). Fórmula del propio differential_reinjector.hpp.
double combRippleDb(double x) {
    return 20.0 * std::log10((1.0 + x) / (1.0 - x));
}

double rms(const std::vector<float>& v) {
    double s = 0.0;
    for (float f : v) s += static_cast<double>(f) * f;
    return std::sqrt(s / static_cast<double>(v.size()));
}

}  // namespace

TEST(BofoMatrix, RtBudgetIsFramesOverSampleRate) {
    // 320 frames @48 kHz = 6.67 ms es UN caso, no el presupuesto.
    EXPECT_NEAR(rtBudgetMs(128, 48000), 2.6667, 1e-3);
    EXPECT_NEAR(rtBudgetMs(256, 48000), 5.3333, 1e-3);
    EXPECT_NEAR(rtBudgetMs(320, 48000), 6.6667, 1e-3);
    EXPECT_NEAR(rtBudgetMs(512, 48000), 10.6667, 1e-3);
    EXPECT_NEAR(rtBudgetMs(1024, 48000), 21.3333, 1e-3);
    EXPECT_NEAR(rtBudgetMs(256, 44100), 5.80499, 1e-3);
    EXPECT_NEAR(rtBudgetMs(128, 96000), 1.3333, 1e-3);
    EXPECT_NEAR(rtBudgetMs(256, 192000), 1.3333, 1e-3);
    std::printf("[bofo] periodo: 128f@48k=%.3f ms, 256f@48k=%.3f ms, "
                "320f@48k=%.3f ms, 256f@44.1k=%.3f ms, 128f@96k=%.3f ms\n",
                rtBudgetMs(128, 48000), rtBudgetMs(256, 48000),
                rtBudgetMs(320, 48000), rtBudgetMs(256, 44100),
                rtBudgetMs(128, 96000));
}

TEST(BofoMatrix, CombRippleDepthIsSetByCoherentGainNotDelay) {
    // Techo de diseño del reinjector: x <= 10^(0.8/20)-1 = 0.0965.
    const double riskMax = std::pow(10.0, 0.8 / 20.0) - 1.0;
    EXPECT_NEAR(riskMax, ivanna::DifferentialReinjector::kRiskMax, 1e-4);
    // HALLAZGO DE DEFINICIÓN (no se toca la constante): kRiskMax se define
    // como 10^(R/20)-1 con R = 0.8 dB, lo que acota la excursión de UN SOLO
    // lado a 0.8 dB (20*log10(1+x)). El rizado PICO A PICO con ese x es
    // 20*log10((1+x)/(1-x)) = 1.68 dB, no 0.8 dB: el comentario del header
    // ("el rizado de peine pico a pico es 20*log10((1+g|H|)/(1-g|H|))" con
    // "R = kRippleDb") mezcla ambas definiciones. El guardián aplica la
    // constante, así que el comportamiento real es el de 1.68 dB p-p; si
    // el requisito es 0.8 dB PICO A PICO, kRiskMax debería ser 0.0470
    // ((10^(0.8/20)-1)/(10^(0.8/20)+1)). Se deja documentado, no cambiado:
    // cambiar el techo cambia la inmersión permitida y es decisión de
    // producto con escucha en dispositivo.
    EXPECT_NEAR(combRippleDb(riskMax), 1.68, 0.02);
    EXPECT_NEAR(20.0 * std::log10(1.0 + riskMax), 0.8, 0.02);
    // Copia procesada coherente a ganancia 0.60 junto al original:
    // 12.0 dB pico a pico de peine -> coloración grave audible ("bofo").
    EXPECT_NEAR(combRippleDb(0.60), 12.04, 0.05);
    EXPECT_NEAR(combRippleDb(0.90), 25.58, 0.05);
    // El retardo L solo fija la separación de dientes (1/L Hz):
    // a L=130 ms los dientes están cada 7.7 Hz (peine denso en graves);
    // a L=30 ms, cada 33.3 Hz. La PROFUNDIDAD no depende de L.
    EXPECT_NEAR(1.0 / 0.130, 7.69, 0.05);
    EXPECT_NEAR(1.0 / 0.030, 33.33, 0.05);
    std::printf("[bofo] rizado peine: x=0.0965 -> %.2f dB; x=0.60 -> %.2f dB; "
                "x=0.90 -> %.2f dB; dientes: 130ms -> %.1f Hz, 30ms -> %.1f Hz\n",
                combRippleDb(riskMax), combRippleDb(0.60), combRippleDb(0.90),
                1.0 / 0.130, 1.0 / 0.030);
}

TEST(BofoMatrix, VolterraCrossfadeSamplesScaleWithSampleRate) {
    // Release 18 ms en tiempo -> muestras = 0.018 * sr (redondeo).
    const double kReleaseMs = 18.0;
    auto samples = [&](double sr) {
        return static_cast<int>(std::lround(kReleaseMs * sr / 1000.0));
    };
    EXPECT_EQ(samples(44100), 794);
    EXPECT_EQ(samples(48000), 864);
    EXPECT_EQ(samples(96000), 1728);
    EXPECT_EQ(samples(192000), 3456);
    // Un fundido de 864 muestras fijas a 44.1 kHz duraría 19.6 ms, no 18:
    // por eso el envelope es temporal, no en muestras fijas.
    EXPECT_NEAR(864.0 / 44100.0 * 1000.0, 19.59, 0.01);
}

TEST(BofoMatrix, EqualPowerDoesNotHoldForCorrelatedSignals) {
    // Equal-power (g1^2+g2^2=1) conserva energía solo si las señales son
    // INCORRELADAS. Dos senos en fase a igual amplitud suman en amplitud:
    // con g1=g2=1/sqrt(2), la suma es sqrt(2)*A, potencia 2*A^2, no A^2.
    const double a = 0.5;
    const double g = 1.0 / std::sqrt(2.0);
    const double sumAmp = g * a + g * a;  // en fase
    EXPECT_NEAR(sumAmp, std::sqrt(2.0) * a, 1e-9);
    EXPECT_NEAR(sumAmp * sumAmp, 2.0 * a * a, 1e-9);  // el doble, no igual
    // En contrafase se cancelan: 0. El crossfade equal-power entre dos
    // versiones correlacionadas del mismo audio NO es neutro en energía.
    EXPECT_NEAR(g * a - g * a, 0.0, 1e-9);
}

TEST(BofoMatrix, ReinjectorWetEqualsDryProducesNoSignal) {
    ivanna::DifferentialReinjector rj;
    rj.prepare(48000.f, 256);
    rj.setIntensity(1.0f);
    rj.setListenerLatencyMs(30.f);
    constexpr int kFrames = 256;
    std::vector<float> dry(2 * kFrames), wet(2 * kFrames), out(2 * kFrames);
    double dryRms = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        const float s = 0.5f * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        dry[2 * i] = s;
        dry[2 * i + 1] = s;
        wet[2 * i] = s;
        wet[2 * i + 1] = s;
    }
    dryRms = rms(dry);
    // Calentamiento: 40 bloques (el estimador de lag/FFT necesita hops).
    for (int b = 0; b < 40; ++b) rj.process(dry.data(), wet.data(), out.data(), kFrames);
    const double outRms = rms(out);
    std::printf("[bofo] reinjector wet==dry: dry RMS=%.4f out RMS=%.6f\n", dryRms, outRms);
    EXPECT_LT(outRms, 0.01 * dryRms + 1e-4);
}

TEST(BofoMatrix, ReinjectorCoherentAttenuationIsBoundedByGuardian) {
    // wet = 0.9 * dry: como si un compresor atenuara 10% de forma
    // coherente. Delta = -0.1*dry: el guardián debe acotar la ganancia
    // de reinyección para que el riesgo de peine no supere el techo.
    ivanna::DifferentialReinjector rj;
    rj.prepare(48000.f, 256);
    rj.setIntensity(1.0f);
    rj.setListenerLatencyMs(30.f);
    constexpr int kFrames = 256;
    std::vector<float> dry(2 * kFrames), wet(2 * kFrames), out(2 * kFrames);
    for (int i = 0; i < kFrames; ++i) {
        const float s = 0.5f * std::sin(2.0 * M_PI * 440.0 * i / 48000.0);
        dry[2 * i] = s;
        dry[2 * i + 1] = s;
        wet[2 * i] = 0.9f * s;
        wet[2 * i + 1] = 0.9f * s;
    }
    const double dryRms = rms(dry);
    for (int b = 0; b < 60; ++b) rj.process(dry.data(), wet.data(), out.data(), kFrames);
    const double outRms = rms(out);
    const auto tel = rj.telemetry();
    std::printf("[bofo] reinjector wet=0.9*dry: dry RMS=%.4f out RMS=%.4f "
                "combRisk=%.4f (techo %.4f) immersion=%.3f\n",
                dryRms, outRms, tel.combRisk,
                ivanna::DifferentialReinjector::kRiskMax, tel.immersion);
    // La reinyección no amplifica por encima de la propia delta (0.1*dry):
    EXPECT_LE(outRms, 0.11 * dryRms + 1e-3);
    // Y el riesgo de peine declarado no supera el techo de diseño + margen
    // de estimación (el guardián actúa por banda; la telemetría es el máx).
    EXPECT_LE(tel.combRisk, ivanna::DifferentialReinjector::kRiskMax + 0.05);
}

TEST(BofoMatrix, ReinjectorHostProfile) {
    // Perfil en host (NO es el teléfono: CPU distinta, sin throttling).
    // Solo fija que el coste está órdenes de magnitud por debajo del
    // periodo de 256 frames @48 kHz (5.33 ms) en esta máquina.
    ivanna::DifferentialReinjector rj;
    rj.prepare(48000.f, 256);
    rj.setIntensity(1.0f);
    rj.setListenerLatencyMs(30.f);
    constexpr int kFrames = 256;
    std::vector<float> dry(2 * kFrames), wet(2 * kFrames), out(2 * kFrames);
    for (int i = 0; i < kFrames; ++i) {
        const float s = 0.5f * std::sin(2.0 * M_PI * 1000.0 * i / 48000.0);
        dry[2 * i] = s;
        dry[2 * i + 1] = 0.9f * s;
        wet[2 * i] = s;
        wet[2 * i + 1] = s;
    }
    constexpr int kBlocks = 500;
    std::vector<double> us(kBlocks);
    for (int b = 0; b < kBlocks; ++b) {
        const auto t0 = std::chrono::steady_clock::now();
        rj.process(dry.data(), wet.data(), out.data(), kFrames);
        us[b] = static_cast<double>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - t0)
                .count()) /
                1000.0;
    }
    std::sort(us.begin(), us.end());
    const double p50 = us[kBlocks / 2];
    const double p95 = us[static_cast<size_t>(kBlocks * 0.95)];
    const double mx = us.back();
    std::printf("[bofo] reinjector host 256f@48k: p50=%.1f us p95=%.1f us "
                "max=%.1f us (periodo 5333 us)\n",
                p50, p95, mx);
    EXPECT_LT(p95, rtBudgetMs(256, 48000) * 1000.0);
}

// ── CELDA BRIDGEPLAYER (F2, reporte Esse 2026-11-14 20:11 CST) ─────────────
// El BridgePlayer es celda PROPIA, no un caso de la captura: Tidal dentro
// de IVANNA TAMBIÉN corta, así que los cortes no son solo de la vía de
// captura. Hechos verificados en código (no en dispositivo):
//   - La red queda EXCLUIDA como causa DURANTE la reproducción: el
//     resolver (v2.5.418, TidalStreamResolver.kt) descarga el archivo
//     COMPLETO (BTS y DASH) antes de sonar; un corte a mitad de tema no
//     es red. El prefetch del siguiente tema corre en Dispatchers.IO
//     mientras suena el actual (contención CPU/disco: sospechoso
//     secundario, no causa en el hilo de audio).
//   - El hilo de audio YA es URGENT_AUDIO y el servicio en primer plano
//     mediaPlayback existe desde la 438: si corta EN primer plano (app
//     visible), el throttling por segundo plano queda excluido y es
//     pipeline/DSP/dispositivo, no el servicio.
//   - El snapshot secuencia 394 NO prueba "DSP parado" (ver fila H).
//   - La telemetría que lo habría probado no existía: dspProcessingUs era
//     0 siempre porque OEMTelemetry::reportFrameProcessed() no tenía
//     ningún llamador, y los underruns de OEMTelemetry tampoco se
//     alimentaban. Desde esta versión DSPBridge.process() reporta el
//     tiempo real por bloque y el BridgePlayer publica el delta de
//     AudioTrack.underrunCount: el próximo snapshot SÍ puede decidir.

// Tiempo de audio que cubre un búfer de `bytes`, estéreo float
// (2 canales x 4 B): bytes / (rate * 8) segundos.
double bufferTimeMs(long bytes, double sampleRate) {
    return 1000.0 * static_cast<double>(bytes) / (sampleRate * 2.0 * 4.0);
}

// Periodo de un bloque en microsegundos: el presupuesto RT del DSP por
// bloque en el BridgePlayer (MAX_CHUNK_FRAMES = 2048).
double chunkPeriodUs(int frames, double sampleRate) {
    return 1e6 * static_cast<double>(frames) / sampleRate;
}

// Regla de veredicto de underruns (umbral fijado ANTES de medir en
// dispositivo; la aplica el experimento G, no este test):
//   0 = sin underruns (sin veredicto), 1 = cuello DSP,
//   2 = inanición no-DSP (decoder / planificación / prefetch / térmica).
int classifyUnderrun(long underrunDelta, double dspP95Us, double periodUs) {
    if (underrunDelta <= 0) return 0;
    if (dspP95Us >= periodUs) return 1;
    return 2;
}

TEST(BofoMatrix, BridgePlayerBufferTimeHalvesAt96k) {
    // El mismo búfer en bytes cubre LA MITAD de milisegundos a 96 kHz que
    // a 48 kHz (el BridgePlayer fija TARGET_SAMPLE_RATE = 96 000 y pide
    // minBuf * 6 bytes). 96 000 B = 125 ms @96k, 250 ms @48k.
    EXPECT_NEAR(bufferTimeMs(96000, 96000), 125.0, 1e-9);
    EXPECT_NEAR(bufferTimeMs(96000, 48000), 250.0, 1e-9);
    // Simbólico con minBuf arbitrario B: minBuf*6 a 96k cubre la mitad
    // que a 48k, sea cual sea B (el factor 2 no depende del dispositivo).
    const long kMinBuf = 16384;
    EXPECT_NEAR(bufferTimeMs(6 * kMinBuf, 96000), 128.0, 1e-9);
    EXPECT_NEAR(bufferTimeMs(6 * kMinBuf, 48000), 256.0, 1e-9);
    // Periodo del bloque real del BridgePlayer (2048 frames): 21.333 ms
    // @96k frente a 42.667 ms @48k. A 96k el DSP tiene la mitad de tiempo
    // por bloque Y el búfer cubre la mitad de ms: el doble de exigencia
    // por los dos lados a la vez.
    EXPECT_NEAR(chunkPeriodUs(2048, 96000) / 1000.0, 21.3333, 1e-3);
    EXPECT_NEAR(chunkPeriodUs(2048, 48000) / 1000.0, 42.6667, 1e-3);
    std::printf("[bofo] BridgePlayer: 96kB -> %.0f ms @96k, %.0f ms @48k; "
                "minBuf*6 (B=16384) -> %.0f ms @96k, %.0f ms @48k; "
                "periodo 2048f -> %.3f ms @96k, %.3f ms @48k\n",
                bufferTimeMs(96000, 96000), bufferTimeMs(96000, 48000),
                bufferTimeMs(6 * kMinBuf, 96000),
                bufferTimeMs(6 * kMinBuf, 48000),
                chunkPeriodUs(2048, 96000) / 1000.0,
                chunkPeriodUs(2048, 48000) / 1000.0);
}

TEST(BofoMatrix, BridgePlayerChunkPeriodAtRealRates) {
    // Periodos del bloque de 2048 frames a las tasas reales de la cadena.
    EXPECT_NEAR(chunkPeriodUs(2048, 44100), 46439.91, 0.01);
    EXPECT_NEAR(chunkPeriodUs(2048, 48000), 42666.67, 0.01);
    EXPECT_NEAR(chunkPeriodUs(2048, 96000), 21333.33, 0.01);
    EXPECT_NEAR(chunkPeriodUs(2048, 192000), 10666.67, 0.01);
    // El presupuesto de 6.67 ms citado en la 423 es el de la CAPTURA
    // (320f@48k), NO el del BridgePlayer: mezclarlos subestima el margen
    // del BridgePlayer por 3.2x (21.33 / 6.67).
    EXPECT_NEAR(chunkPeriodUs(2048, 96000) / chunkPeriodUs(320, 48000), 3.2, 0.01);
}

TEST(BofoMatrix, BridgePlayerUnderrunDecisionRule) {
    const double kPeriodUs = chunkPeriodUs(2048, 96000);  // 21 333 us
    // Sin underruns no hay veredicto, aunque el DSP vaya justo.
    EXPECT_EQ(classifyUnderrun(0, 30000.0, kPeriodUs), 0);
    // Underruns + DSP p95 por encima del periodo -> cuello DSP.
    EXPECT_EQ(classifyUnderrun(5, 25000.0, kPeriodUs), 1);
    // En el borde (p95 == periodo) también es cuello: el periodo es el
    // tiempo DISPONIBLE, no un objetivo con margen.
    EXPECT_EQ(classifyUnderrun(1, kPeriodUs, kPeriodUs), 1);
    // Underruns + DSP holgado (8 ms << 21.33 ms) -> inanición NO-DSP:
    // decoder/MediaCodec, planificación del hilo, prefetch, térmica.
    // Bajar el DSP no arreglaría este caso; hay que buscar fuera de él.
    EXPECT_EQ(classifyUnderrun(5, 8000.0, kPeriodUs), 2);
    EXPECT_EQ(classifyUnderrun(12, 1000.0, kPeriodUs), 2);
    std::printf("[bofo] regla underrun: periodo=%.0f us; "
                "delta=0->0, delta>0+p95>=periodo->1 (DSP), "
                "delta>0+p95<periodo->2 (no-DSP)\n",
                kPeriodUs);
}

// ── MATRIZ DE AISLAMIENTO (dispositivo; PENDIENTE, necesita a Esse) ────────
// Cada fila es una condición con observable y veredicto. Ninguna fila se
// declara aquí: el teléfono está limpio tras el 7º reset y no se instala
// nada sin luz verde de Esse.
//   A. Fuente sin IVANNA: Tidal solo, WiFi vs datos, tema descargado.
//      Si el tronido está -> no es IVANNA (atribución a Tidal/red).
//   B. BridgePlayer bypass (amounts 0) vs procesado, mismo archivo local.
//      Si bypass suena limpio y procesado bofo -> la cadena DSP, no la fuente.
//   C. Módulos individuales: solo ORBE / solo HOLOS / solo Φ / solo
//      Volterra / solo ISO 226 / solo OmniField. El que reproduzca el bofo
//      es el responsable; si ninguno lo reproduce solo, es interacción.
//   D. Captura vs BridgePlayer: captura (original + copia ~130 ms tarde,
//      peine 12 dB a ganancia coherente 0.6) vs BridgePlayer (100% cadena).
//      Si el bofo solo está en captura -> peine, no EQ.
//   E. Archivo local vs streaming: mismo tema en local y en Tidal.
//   F. Salidas: altavoz del teléfono vs USB (DAC Genezi) vs Bluetooth.
//      Si solo una salida -> transductor/ruta, no DSP.
//   G. BridgePlayer como celda propia (F2, 2026-11-14): mismo tema,
//      mismo teléfono, en Tidal dentro de IVANNA (BridgePlayer) vs
//      archivo local en BridgePlayer vs Tidal solo (fila A). La red queda
//      excluida durante la reproducción (descarga completa previa,
//      v2.5.418). En primer plano, el FGS/throttling queda excluido.
//      Experimento exacto: 1 tema FLAC conocido (el mismo archivo local
//      y en Tidal), app en primer plano, WiFi, 60 s de escucha; anotar
//      cada corte (minuto:segundo) y leer al final: underrunCount delta
//      (OmegaMetrics / logcat IVANNA.Telemetry 1 Hz), dspProcessingUs
//      (ahora real), clipCount DELTA por tema. Veredicto por la regla
//      fijada arriba: periodo 2048f@96k = 21 333 µs; underruns>0 +
//      dsp p95 >= 21 333 µs -> cuello DSP (H1: probar salida a 48 kHz,
//      que duplica periodo y cobertura del búfer); underruns>0 + dsp
//      p95 < 21 333 µs -> inanición no-DSP (sospechosos: prefetch del
//      siguiente tema en IO, MediaCodec, térmica); underruns==0 y se
//      oye el corte -> no es underrun (clip / peine / ASRC / salida).
//      Repetir con el prefetch desactivado si el caso no-DSP sale:
//      si los cortes desaparecen, es contención del prefetch.
//   H. Relectura del snapshot secuencia 394 (NO es "DSP parado"):
//      dspProcessingUs=0 era un hueco de telemetría (reportFrameProcessed
//      sin llamadores, ya cableado); adaptiveEngineRunning=false es el
//      flag [7] adaptiveActive del pipeline unificado (ADE arrancado Y
//      applied>0), NO la cadena DSP del BridgePlayer (DSPBridge.process
//      + NPE + Spatial), que puede correr con adaptiveActive=0;
//      daemonConnected=false es Ruta B (OmegaEngineBridge), irrelevante
//      para Ruta A y esperado en el teléfono limpio sin módulo;
//      clipCount=17 es acumulado sin delta ni marca temporal: hubo clips
//      alguna vez, no prueba dónde. Ninguno de los cuatro campos
//      atribuye el corte al DSP parado.
// Observable en cada fila: grabación + telemetría (nativeGetDspShed,
// nativeGetSpatialGateState, nativeGetEffectAmounts, reinjector combRisk).
// Criterio de cierre: el bofo se declara resuelto solo cuando Esse lo
// escucha resuelto en el teléfono con la fila B en procesado; nunca por
// un test host.
