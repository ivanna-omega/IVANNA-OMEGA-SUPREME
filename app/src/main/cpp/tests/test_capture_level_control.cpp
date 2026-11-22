// test_capture_level_control.cpp
//
// Dinámica del bucle de escritura de la CAPTURA (PlaybackCaptureService,
// Ruta A por MediaProjection) — 0025.
//
// REPRODUCE el fallo medido en vivo en la 457 (2026-11-16 11:18 CST) y
// VALIDA el fix (pacing de nivel por descarte, espejo de 0020).
//
// Fallo: PLL de captura SATURADO EN EL RIEL (-500 ppm, demanda -557.3,
// pll_rail=1) con el buffer al 99% LLENO (pll_occ=7619/7696, objetivo
// 3848). Es el MISMO sobre-lleno que 0020 arregló en BridgePlayer, pero
// 0020 no tocó la captura.
//
// Mecanismo (verificado en código, no una hipótesis suelta):
//  - La captura es push (tiempo real): AudioRecord entrega 320 frames por
//    bloque (6.67 ms). Tras cualquier stall (GC, planificador), el anillo
//    del AudioRecord acumula audio y el bucle lo lee en RÁFAGA (varios
//    bloques seguidos sin esperar), escribiéndolos todos al track.
//  - El ppm es un actuador de TASA (±500 ppm = 24 frames/s de drenado):
//    no puede corregir un error de NIVEL acumulado. El exceso en vivo
//    (7619-3848 = 3771 frames) tardaría 3771/24 = 157 s en drenar por ppm.
//    El servo se satura en el riel y el buffer se queda al 99%.
//  - El ASRC aplica el ppm con el signo CORRECTO (ratio = 1+ppm*1e-6;
//    ppm negativo = menos frames de salida, verificado en
//    ivanna_route_a_asrc.cpp:124-134 y :466-468): no es un bug de signo.
//    La contabilidad de submitted (captureSubmittedFrames48k) también es
//    correcta (suma asrcFrames, la salida real del ASRC). El sobre-lleno
//    es real, no un artefacto de medida.
// Fix (0025): control de NIVEL por ritmo de escritura, espejo de 0020,
// adaptado a push: cuando la ocupación estimada supera el objetivo, se
// DESCARTA el bloque procesado (no se escribe ni se cuenta como
// submitted) y la pista drena. La fuente no puede rellenar el hueco más
// rápido que tiempo real, así que el descarte SÍ baja el nivel de forma
// duradera (a diferencia del snap en BridgePlayer, donde el decoder
// pull-based rellenaba el hueco al instante). El ppm queda solo para la
// deriva real. El descarte es de 1 bloque (320 frames, 6.67 ms) y solo
// ocurre sobre el objetivo.
//
// Esta simulación usa los números REALES (capacidad 7696, objetivo 3848,
// bloque 320, deriva +8.5 ppm) y modela: productor push con ráfagas de
// catch-up tras stalls, track con capacidad y drenado a 48 kHz,
// write-blocking, sondeo PLL cada 250 ms (P = -800 ppm por buffer lleno +
// deriva) y descarte por pacing según el escenario. Es un modelo de tasas
// (el ASRC se modela por su ratio; su comportamiento exacto ya lo cubre
// test_route_a_asrc). No depende de Android: corre en host (CTest).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kSampleRateHz = 48000.0;
constexpr long kCapacityFrames = 7696;    // buffer de captura (medido en vivo)
constexpr long kTargetFrames = 3848;      // capacidad / 2
constexpr long kBlockFrames = 320;        // BLOCK_FRAMES (6.67 ms)
constexpr double kDriftPpm = 8.5;         // deriva real (misma que BridgePlayer)
constexpr double kPllKpPpmPerFullBuffer = 800.0;   // KP real del PLL
constexpr double kPllKiPpmPerFullBufferSec = 120.0; // KI real del PLL
constexpr double kPllMaxIntegralSec = 4.0;          // tope del integrador
constexpr double kPllDeadbandFraction = 0.02;       // banda muerta real
constexpr double kPllMaxPpm = 500.0;
constexpr double kPollIntervalSec = 0.25;  // sondeo PLL cada 250 ms
constexpr double kSimSeconds = 10.0;       // ventana simulada
constexpr double kBlockPeriodSec = kBlockFrames / kSampleRateHz;  // 6.67 ms

struct SimResult {
    long finalLevelFrames = 0;
    long maxLevelFrames = 0;
    long droppedFrames = 0;      // bloques descartados por pacing (frames)
    long writtenFrames = 0;      // frames que llegaron al track
    double finalPpm = 0.0;
    double finalDemandPpm = 0.0;
    bool railed = false;         // el servo llegó al riel en algún momento
    double simSeconds = 0.0;
};

// Simula el bucle de captura. El productor es push: entrega 1 bloque por
// periodo (6.67 ms) en tiempo real, pero tras un stall entrega una RÁFAGA
// (varios bloques seguidos). `burstEverySec` = cada cuántos segundos hay
// un stall que acumula `burstBlocks` bloques en el anillo del AudioRecord.
// `usePacing` = descarte por pacing cuando occ > objetivo (fix 0025).
SimResult simulate(bool usePacing, double burstEverySec, int burstBlocks) {
    SimResult r;
    double timeSec = 0.0;
    long submitted = 0;   // frames escritos al track (dominio 48 kHz)
    long consumed = 0;    // frames drenados por el consumidor
    double ppm = 0.0;     // corrección actual del PLL (va al ASRC)
    double integralErrorSec = 0.0;  // integrador PI (segundos de error)
    double lastPollSec = 0.0;
    double nextBurstSec = burstEverySec;
    // El productor push tiene su propio backlog (anillo AudioRecord): los
    // bloques que llegan mientras el bucle está ocupado se acumulan y se
    // leen en ráfaga. Aquí lo modelamos como: en cada periodo el productor
    // genera 1 bloque; si hay ráfaga pendiente, el bucle procesa bloques
    // extra en el mismo periodo (sin esperar al siguiente).
    long producerBacklog = 0;
    long blocksProcessed = 0;

    const long totalPeriods = static_cast<long>(kSimSeconds / kBlockPeriodSec);
    for (long period = 0; period < totalPeriods; ++period) {
        timeSec = period * kBlockPeriodSec;
        // El productor genera 1 bloque por periodo (tiempo real).
        producerBacklog += 1;
        // Stall periódico: acumula burstBlocks bloques extra en el anillo.
        if (burstEverySec > 0.0 && timeSec >= nextBurstSec) {
            producerBacklog += burstBlocks;
            nextBurstSec += burstEverySec;
        }
        // El consumidor drena 1 bloque por periodo (48 kHz nominal).
        const long levelBefore = submitted - consumed;
        const long drain = std::min<long>(kBlockFrames, levelBefore);
        consumed += drain;
        // El bucle procesa TODO el backlog en este periodo (lectura en
        // ráfaga: AudioRecord.read() no bloquea si hay audio acumulado).
        while (producerBacklog > 0) {
            producerBacklog -= 1;
            ++blocksProcessed;
            // Sondeo PLL cada 250 ms: PI por error de nivel + deriva
            // (fiel al PLL real: KP=800, KI=120, integrador cap 4 s,
            // banda muerta 2%, anti-windup condicional, riel ±500).
            if (timeSec - lastPollSec >= kPollIntervalSec) {
                const double dt = timeSec - lastPollSec;
                lastPollSec = timeSec;
                const long level = submitted - consumed;
                const double phaseErr =
                    (static_cast<double>(level) - kTargetFrames) / kCapacityFrames;
                double occErr = 0.0;
                if (std::fabs(phaseErr) > kPllDeadbandFraction) {
                    occErr = phaseErr > 0.0 ? phaseErr - kPllDeadbandFraction
                                            : phaseErr + kPllDeadbandFraction;
                }
                const double candidateIntegral = std::clamp(
                    integralErrorSec + occErr * dt, -kPllMaxIntegralSec,
                    kPllMaxIntegralSec);
                // Demanda = deriva - (KP*error + KI*integral).
                double demand = kDriftPpm -
                    (kPllKpPpmPerFullBuffer * occErr +
                     kPllKiPpmPerFullBufferSec * integralErrorSec);
                // Anti-windup condicional: si la demanda ya está saturada
                // y el error empuja más hacia la saturación, no se integra.
                const bool pushesFurther =
                    (demand >= kPllMaxPpm && occErr < 0.0) ||
                    (demand <= -kPllMaxPpm && occErr > 0.0);
                if (!pushesFurther) integralErrorSec = candidateIntegral;
                demand = kDriftPpm -
                    (kPllKpPpmPerFullBuffer * occErr +
                     kPllKiPpmPerFullBufferSec * integralErrorSec);
                ppm = std::clamp(demand, -kPllMaxPpm, kPllMaxPpm);
                if (std::fabs(demand) >= kPllMaxPpm) r.railed = true;
                r.finalDemandPpm = demand;
            }
            // Pacing por descarte (0025): si occ > objetivo, el bloque se
            // descarta (no se escribe ni se cuenta). Si no, el ASRC aplica
            // el ppm y el bloque se escribe.
            const long level = submitted - consumed;
            if (usePacing && level > kTargetFrames) {
                r.droppedFrames += kBlockFrames;
                continue;
            }
            const double ratio = 1.0 + ppm * 1.0e-6;
            const long outFrames =
                static_cast<long>(std::llround(kBlockFrames * ratio));
            // El track tiene capacidad: WRITE_BLOCKING frena si está lleno.
            const long room = kCapacityFrames - (submitted - consumed);
            const long accepted = std::clamp(outFrames, 0L, std::max(0L, room));
            submitted += accepted;
            r.writtenFrames += accepted;
            r.maxLevelFrames =
                std::max(r.maxLevelFrames, submitted - consumed);
        }
    }
    r.finalLevelFrames = submitted - consumed;
    r.finalPpm = ppm;
    r.simSeconds = kSimSeconds;
    return r;
}

}  // namespace

// SIN pacing: el sobre-lleno de la ráfaga se queda (el ppm no lo drena) y
// el servo se satura en el riel, como en vivo (occ 7619/7696, ppm -500,
// demanda -557.3).
TEST(CaptureLevelControl, WithoutPacingStaysOverFullAndRails) {
    // Ráfaga de 12 bloques (80 ms de audio acumulado) cada 2 s: simula un
    // stall periódico del hilo (GC/planificador) con catch-up en ráfaga.
    const SimResult r = simulate(/*usePacing=*/false, /*burstEverySec=*/2.0,
                                 /*burstBlocks=*/12);
    // El nivel final queda muy por encima del objetivo (el ppm no drena).
    EXPECT_GT(r.finalLevelFrames, kTargetFrames + 1000)
        << "sin pacing el exceso debería persistir";
    // El servo llegó al riel (-500 ppm) intentando drenar por tasa.
    EXPECT_TRUE(r.railed) << "el servo debería saturarse en el riel";
    EXPECT_LT(r.finalPpm, -400.0) << "ppm final cerca del riel negativo";
    EXPECT_EQ(r.droppedFrames, 0) << "sin pacing no hay descartes";
    std::printf("[CAPTURE-LEVEL] sin pacing: nivel final=%ld (objetivo %ld) "
                "max=%ld ppm=%.1f demanda=%.1f railed=%d\n",
                r.finalLevelFrames, kTargetFrames, r.maxLevelFrames,
                r.finalPpm, r.finalDemandPpm, static_cast<int>(r.railed));
}

// CON pacing por descarte: el nivel converge al objetivo y el servo sale
// del riel (el ppm queda solo para la deriva real).
TEST(CaptureLevelControl, WithPacingConvergesToTargetAndUnrails) {
    const SimResult r = simulate(/*usePacing=*/true, /*burstEverySec=*/2.0,
                                 /*burstBlocks=*/12);
    // El nivel final está cerca del objetivo (dentro de 1 bloque).
    EXPECT_LE(r.finalLevelFrames, kTargetFrames + kBlockFrames)
        << "con pacing el nivel debe converger al objetivo";
    EXPECT_GE(r.finalLevelFrames, 0L) << "el nivel no puede ser negativo";
    // El servo NO se queda en el riel: la demanda final es pequeña
    // (solo deriva + error residual de nivel).
    EXPECT_GT(r.finalDemandPpm, -200.0)
        << "con pacing la demanda debe salir del riel";
    EXPECT_LT(r.finalDemandPpm, 200.0)
        << "con pacing la demanda debe ser pequeña";
    // Hubo descartes (el mecanismo actuó), pero pocos: solo el exceso.
    EXPECT_GT(r.droppedFrames, 0) << "el pacing debe haber descartado algo";
    EXPECT_LT(r.droppedFrames, r.writtenFrames / 4)
        << "los descartes deben ser una fracción pequeña del audio";
    std::printf("[CAPTURE-LEVEL] con pacing: nivel final=%ld (objetivo %ld) "
                "max=%ld ppm=%.1f demanda=%.1f descartados=%ld escritos=%ld\n",
                r.finalLevelFrames, kTargetFrames, r.maxLevelFrames,
                r.finalPpm, r.finalDemandPpm, r.droppedFrames,
                r.writtenFrames);
}

// El ppm solo (sin pacing) no puede drenar un exceso de nivel en tiempo
// razonable: 3771 frames de exceso a 24 frames/s = 157 s. Este test
// cuantifica el drenado por ppm para documentar por qué el riel no basta.
TEST(CaptureLevelControl, PpmAloneCannotDrainLevelError) {
    // Exceso en vivo: 7619 - 3848 = 3771 frames.
    const long excessFrames = 7619 - 3848;
    // Drenado máximo por ppm: 500 ppm de 48000 Hz = 24 frames/s.
    const double drainFps = kPllMaxPpm * 1.0e-6 * kSampleRateHz;
    const double drainSeconds = excessFrames / drainFps;
    EXPECT_GT(drainSeconds, 100.0)
        << "el ppm tarda más de 100 s en drenar el exceso en vivo";
    std::printf("[CAPTURE-LEVEL] exceso=%ld frames, drenado por ppm=%.1f "
                "frames/s -> %.1f s (el riel no basta)\n",
                excessFrames, drainFps, drainSeconds);
    // Con pacing, el mismo exceso se drena en ~exceso/bloque periodos
    // (cada bloque descartado quita 320 frames): orden de 12 bloques.
    const double pacingDrainBlocks =
        static_cast<double>(excessFrames) / kBlockFrames;
    EXPECT_LT(pacingDrainBlocks, 20.0)
        << "el pacing drena el exceso en menos de 20 bloques";
    std::printf("[CAPTURE-LEVEL] mismo exceso por pacing: %.1f bloques "
                "(%.1f ms)\n",
                pacingDrainBlocks, pacingDrainBlocks * kBlockPeriodSec * 1000.0);
}

// Sin ráfagas (fuente perfectamente en tiempo real), el pacing no descarta
// nada: no degrada el caso sano.
TEST(CaptureLevelControl, NoBurstNoDrops) {
    const SimResult r = simulate(/*usePacing=*/true, /*burstEverySec=*/0.0,
                                 /*burstBlocks=*/0);
    EXPECT_EQ(r.droppedFrames, 0)
        << "sin ráfagas el pacing no debe descartar nada";
    EXPECT_LE(r.finalLevelFrames, kTargetFrames + kBlockFrames);
    EXPECT_GE(r.finalLevelFrames, 0L);
    std::printf("[CAPTURE-LEVEL] sin ráfagas: nivel final=%ld descartados=%ld\n",
                r.finalLevelFrames, r.droppedFrames);
}
