// test_bridgeplayer_level_control.cpp
//
// Dinámica del bucle de escritura del BridgePlayer (Ruta A) — v2.5.457.
//
// REPRODUCE el fallo medido en vivo en la 456 (2026-11-16 04:41 CST) y
// VALIDA el fix raíz (control de nivel por ritmo de escritura).
//
// Fallo (snap 455): pll_occ clavado ~98k (cap 92_256, objetivo 46_128),
// pll_snap=2048 disparando SIEMPRE pero el buffer NO baja (97_568 →
// 97_967 → 98_429). Síntoma de Esse: MÚSICA = "metralleta" (micro-cortes).
// Y el disparador del tronido es el CAMBIO DE PISTA (2026-11-16 05:16 CST):
// estable durante una canción, pero al brincar de canción regresan los
// tronidos y la distorsión — cada pista nueva re-prima el buffer a lleno
// (el track/PLL/ASRC son frescos por pista) y el snap vuelve a soltar
// contenido al arrancar.
//
// Mecanismo (verificado en código, no una hipótesis suelta):
//  - El productor (decodificador) no tiene límite de tasa: decodifica por
//    delante y `track.write(WRITE_BLOCKING)` es el único freno. Su
//    equilibrio mantiene el buffer LLENO (~98k), no en el objetivo.
//  - El snap soltaba 2048 frames de CONTENIDO por sondeo, pero el productor
//    rellenaba el hueco con el siguiente bloque: el nivel no bajaba, solo
//    se perdía audio (17% del contenido saltado = metralleta).
//  - El ppm solo drena 24 frames/s (±500 ppm): no puede con 52k de exceso.
// Fix (457): el nivel lo controla el RITMO DE ESCRITURA — el reproductor
// pausa el productor cuando la ocupación estimada supera el objetivo
// (backpressure natural del codec, sin perder audio) y el snap queda
// legacy (siempre 0). El ppm queda solo para la deriva real (+8.5 ppm).
// El pacing frena el priming DESDE EL PRIMER BLOQUE de cada pista
// (estimateOccupancy sin ancla = lo enviado), así que el cambio de pista
// tampoco sobre-llena: cada pista arranca al objetivo, no a lleno.
//
// Esta simulación usa los números REALES (capacidad, objetivo, bloque,
// snap, deriva) y modela: productor con backlog infinito (decodifica por
// delante), track con capacidad y drenado a 48 kHz, write-blocking,
// sondeo PLL cada 250 ms (P = -800 ppm por buffer lleno + deriva) y snap
// o pacing según el escenario. Es un modelo de tasas (el ASRC se modela
// por su ratio; su comportamiento exacto ya lo cubre test_route_a_asrc).
// No depende de Android: corre en host (CTest).

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kSampleRateHz = 48000.0;
constexpr long kCapacityFrames = 92256;   // minBuf x12 (medido en vivo)
constexpr long kTargetFrames = 46128;     // capacidad / 2
constexpr long kBlockFrames = 1984;       // chunkFrames 44.1k -> 48k
constexpr double kDriftPpm = 8.5;         // deriva real medida en vivo
constexpr long kSnapMaxFrames = 2048;
constexpr double kSnapThresholdPhase = 0.25;
constexpr double kPllKpPpmPerFullBuffer = 800.0;
constexpr double kPllMaxPpm = 500.0;
constexpr double kPollIntervalSec = 0.25;  // sondeo PLL cada 250 ms
constexpr double kBlockProcessSec = 0.005; // decodificar+DSP+ASRC un bloque
constexpr double kSimSeconds = 10.0;       // ventana simulada

struct SimResult {
    long finalLevelFrames = 0;
    long maxLevelFrames = 0;
    long droppedFrames = 0;      // contenido perdido (snap)
    long producedFrames = 0;     // línea de tiempo consumida del decodificador
    long writtenFrames = 0;      // frames que llegaron al track
    double finalPpm = 0.0;
    double simSeconds = 0.0;
};

// Estado de UNA pista (track/PLL/ASRC frescos por pista, como en el
// reproductor real: play() -> stop() -> nuevo runDecodeLoop).
struct TrackState {
    double timeSec = 0.0;
    long submitted = 0;   // frames escritos al track
    long consumed = 0;    // frames drenados por el consumidor
    double ppm = 0.0;         // demanda actual del PLL
    long pendingSnap = 0;     // snap pendiente (frames a soltar)
    double lastPollSec = 0.0;
    long maxLevel = 0;
    long level() const { return submitted - consumed; }
    // Avanza el reloj virtual dt segundos: el consumidor drena a 48 kHz.
    void advanceTime(double dt) {
        if (dt <= 0.0) return;
        timeSec += dt;
        consumed += static_cast<long>(std::llround(dt * kSampleRateHz));
        if (consumed > submitted) consumed = submitted;  // no drenar de más
    }
};

// Corre el bucle de escritura durante `seconds` sobre el estado dado.
// Acumula dropped/produced/written en los contadores por referencia.
void RunLoop(TrackState& s, double seconds, bool useSnap, bool usePacing,
             long& dropped, long& produced, long& written) {
    const double endSec = s.timeSec + seconds;
    while (s.timeSec < endSec) {
        // ── Pacing (457): si el nivel supera el objetivo, el productor se
        // pausa y deja que el track drene. Sin producir ni soltar nada.
        if (usePacing) {
            long occ = s.level();
            if (occ > kTargetFrames) {
                double waitSec =
                    static_cast<double>(occ - kTargetFrames) / kSampleRateHz;
                s.advanceTime(waitSec);
                continue;
            }
        }
        // ── Sondeo PLL cada 250 ms: P por error de nivel + deriva.
        if (s.timeSec - s.lastPollSec >= kPollIntervalSec) {
            s.lastPollSec = s.timeSec;
            double phase = static_cast<double>(s.level() - kTargetFrames) /
                           static_cast<double>(kCapacityFrames);
            double pTerm = -kPllKpPpmPerFullBuffer * phase;
            s.ppm = std::clamp(kDriftPpm + pTerm, -kPllMaxPpm, kPllMaxPpm);
            if (useSnap && std::abs(phase) > kSnapThresholdPhase) {
                s.pendingSnap = std::clamp(s.level() - kTargetFrames,
                                           -kSnapMaxFrames, kSnapMaxFrames);
            } else {
                s.pendingSnap = 0;
            }
        }
        // ── Producir un bloque (ASRC por ratio) ──
        double ratio = 1.0 + s.ppm * 1.0e-6;
        long producedBlock =
            static_cast<long>(std::llround(kBlockFrames * ratio));
        produced += kBlockFrames;  // la línea de tiempo avanza por la entrada
        // ── Snap (455): soltar contenido del bloque ──
        long writtenBlock = producedBlock;
        if (useSnap && s.pendingSnap > 0) {
            long drop = std::min(s.pendingSnap, writtenBlock - 1);
            if (drop > 0) {
                writtenBlock -= drop;
                dropped += drop;
                s.pendingSnap -= drop;
            }
        }
        // ── Escribir (blocking): esperar a que haya sitio ──
        long room = kCapacityFrames - s.level();
        if (writtenBlock > room) {
            double waitSec = static_cast<double>(writtenBlock - room) /
                             kSampleRateHz;
            s.advanceTime(waitSec);
        }
        s.submitted += writtenBlock;
        written += writtenBlock;
        s.maxLevel = std::max(s.maxLevel, s.level());
        // Coste de producir el bloque (decodificar + DSP + ASRC).
        s.advanceTime(kBlockProcessSec);
    }
}

// Simula el bucle de escritura. useSnap = snap 455; usePacing = pacing 457.
SimResult Simulate(bool useSnap, bool usePacing) {
    TrackState s;
    long dropped = 0, produced = 0, written = 0;
    RunLoop(s, kSimSeconds, useSnap, usePacing, dropped, produced, written);
    SimResult r;
    r.finalLevelFrames = s.level();
    r.maxLevelFrames = s.maxLevel;
    r.droppedFrames = dropped;
    r.producedFrames = produced;
    r.writtenFrames = written;
    r.finalPpm = s.ppm;
    r.simSeconds = s.timeSec;
    return r;
}

// Simula DOS pistas seguidas (5 s cada una): al cambiar de pista el estado
// se resetea (track/PLL/ASRC frescos, como play() -> stop() -> nuevo
// runDecodeLoop) y se devuelven las métricas de la SEGUNDA pista — ahí es
// donde Esse oye el tronido (2026-11-16 05:16 CST).
SimResult SimulateTwoTracks(bool useSnap, bool usePacing) {
    TrackState s;
    long dropped = 0, produced = 0, written = 0;
    RunLoop(s, kSimSeconds / 2.0, useSnap, usePacing, dropped, produced,
            written);
    // ── Cambio de pista: estado fresco (el track viejo se hace
    // pause+flush+stop+release; el nuevo arranca vacío) ──
    TrackState s2;
    long dropped2 = 0, produced2 = 0, written2 = 0;
    RunLoop(s2, kSimSeconds / 2.0, useSnap, usePacing, dropped2, produced2,
            written2);
    SimResult r;
    r.finalLevelFrames = s2.level();
    r.maxLevelFrames = s2.maxLevel;
    r.droppedFrames = dropped2;
    r.producedFrames = produced2;
    r.writtenFrames = written2;
    r.finalPpm = s2.ppm;
    r.simSeconds = s2.timeSec;
    return r;
}

}  // namespace

// ── 455 (snap): REPRODUCE el fallo ──────────────────────────────────────────
// El buffer se queda clavado cerca de lleno y el snap pierde contenido sin
// bajar el nivel. Si este test dejara de fallar con el snap, el modelo no
// estaría reproduciendo el fallo real (o el fallo habría desaparecido).
TEST(BridgePlayerLevelControl, Snap455KeepsBufferFullAndDropsContent) {
    SimResult r = Simulate(/*useSnap=*/true, /*usePacing=*/false);
    // El nivel se queda cerca de lleno (medido en vivo: ~98k de 92_256).
    EXPECT_GT(r.finalLevelFrames, static_cast<long>(0.90 * kCapacityFrames))
        << "el snap debería dejar el buffer clavado cerca de lleno";
    // El nivel NO converge al objetivo (sigue >25% de capacidad por encima).
    EXPECT_GT(r.finalLevelFrames - kTargetFrames,
              static_cast<long>(0.25 * kCapacityFrames))
        << "el snap no drena el nivel hacia el objetivo";
    // Contenido perdido: el snap suelta 2048 frames por sondeo (teórico
    // 8192/s = 17% del audio; el modelo mide ~11% por el acople
    // bloque/sondeo — mismo orden, mismo mecanismo: metralleta).
    double droppedFrac = r.producedFrames > 0
        ? static_cast<double>(r.droppedFrames) / r.producedFrames : 0.0;
    EXPECT_GT(droppedFrac, 0.10)
        << "el snap debería perder >10% del contenido (metralleta)";
    // El ppm se queda en el riel negativo (medido en vivo: ~-450).
    EXPECT_LT(r.finalPpm, -300.0)
        << "con el nivel clavado, el PI debería estar en el riel negativo";
}

// ── 457 (pacing): VALIDA el fix ─────────────────────────────────────────────
// El nivel converge al objetivo sin perder contenido y el ppm se queda en
// la deriva real (+8.5 ppm), no en el riel.
TEST(BridgePlayerLevelControl, Pacing457ConvergesToTargetWithoutDropping) {
    SimResult r = Simulate(/*useSnap=*/false, /*usePacing=*/true);
    // El nivel converge al objetivo (±5% de capacidad).
    EXPECT_NEAR(static_cast<double>(r.finalLevelFrames),
                static_cast<double>(kTargetFrames),
                0.05 * kCapacityFrames)
        << "el pacing debería llevar el nivel al objetivo";
    // Cero contenido perdido: el pacing pausa el productor, no suelta frames.
    EXPECT_EQ(r.droppedFrames, 0L) << "el pacing no debe perder contenido";
    // Todo lo producido llega al track (sin saltos en la línea de tiempo).
    EXPECT_EQ(r.writtenFrames, r.producedFrames)
        << "sin snap, lo producido debe llegar íntegro al track";
    // El ppm se queda en la deriva real (~+8.5), no en el riel -450.
    EXPECT_NEAR(r.finalPpm, kDriftPpm, 50.0)
        << "con el nivel en el objetivo, el ppm debe seguir la deriva real";
}

// ── 457: el priming no llena el buffer ──────────────────────────────────────
// Con pacing, el nivel nunca supera el objetivo más un bloque (el priming
// se frena en el objetivo, no llena el buffer hasta arriba).
TEST(BridgePlayerLevelControl, Pacing457CapsPrimingAtTarget) {
    SimResult r = Simulate(/*useSnap=*/false, /*usePacing=*/true);
    EXPECT_LE(r.maxLevelFrames, kTargetFrames + 2 * kBlockFrames)
        << "el priming no debe llenar el buffer: el pacing lo frena en el objetivo";
}

// ── CAMBIO DE PISTA (2026-11-16 05:16 CST) ──────────────────────────────────
// Esse: estable durante una canción, pero al BRINCAR de canción regresan los
// tronidos y la distorsión. Cada pista nueva re-prima el buffer a lleno y el
// snap vuelve a soltar contenido al arrancar. El pacing frena el priming
// desde el primer bloque de CADA pista, así que la segunda pista tampoco se
// sobre-llena ni pierde contenido.
TEST(BridgePlayerLevelControl, Snap455OverfillsSecondTrackToo) {
    SimResult r = SimulateTwoTracks(/*useSnap=*/true, /*usePacing=*/false);
    // La segunda pista también se sobre-llena (re-priming a lleno).
    EXPECT_GT(r.maxLevelFrames, static_cast<long>(0.90 * kCapacityFrames))
        << "con snap, la segunda pista también debería sobre-llenarse al arrancar";
    EXPECT_GT(r.droppedFrames, 0L)
        << "con snap, la segunda pista también debería perder contenido (tronido al cambiar)";
}

TEST(BridgePlayerLevelControl, Pacing457SecondTrackStartsAtTarget) {
    SimResult r = SimulateTwoTracks(/*useSnap=*/false, /*usePacing=*/true);
    // La segunda pista arranca al objetivo (nunca se sobre-llena).
    EXPECT_LE(r.maxLevelFrames, kTargetFrames + 2 * kBlockFrames)
        << "con pacing, la segunda pista no debe sobre-llenarse al arrancar";
    EXPECT_NEAR(static_cast<double>(r.finalLevelFrames),
                static_cast<double>(kTargetFrames),
                0.05 * kCapacityFrames)
        << "con pacing, la segunda pista debe converger al objetivo";
    // Sin contenido perdido en el cambio de pista (sin tronido).
    EXPECT_EQ(r.droppedFrames, 0L)
        << "con pacing, el cambio de pista no debe perder contenido";
    EXPECT_EQ(r.writtenFrames, r.producedFrames)
        << "la segunda pista debe llegar íntegra al track";
}
