#pragma once
//
// DspLoadShedGovernor — Gobernador de shed de carga del DSP (Ruta A).
//
// Problema medido en vivo (Moto G85, 2026-11-09, logcat CaptureEngine):
//   dsp_bridge = 3.5–8.9 ms por bloque de 320 frames @48 kHz, presupuesto
//   6.67 ms. La cadena nativa completa (EQ/Comp/Exciter/Widener, EvoEq,
//   ORBE + HOLOS — cada uno STFT 512/hop 128, ~10–14 FFT por bloque —,
//   Hiper-Operador Phi 24 bandas, PDEngine, WFS, RIR por convolucion
//   particionada, Volterra, coclear, pipeline unificado, 5 ejes supremos,
//   NPE, limiter) corre en UN SOLO hilo — el mismo que lee de AudioRecord.
//   En un Snapdragon 6s Gen 3 (2x A78 + 6x A55) el mismo codigo tarda
//   ~2.5x mas en un core pequeno que en uno grande, y el planificador EAS
//   migra el hilo entre ambos; a eso se suman etapas cuyo coste depende
//   del contenido (particiones activas de la cola RIR, rutas HRTF/upmix)
//   y la preemption por otros hilos. Resultado: carga sostenida 100–105%,
//   el anillo de AudioRecord (8 bloques = 53 ms) se drena en ~1 s y el
//   RecordThread se desborda -> dropouts. Subir el buffer no lo arregla
//   (una sobrecarga SOSTENIDA solo retrasa el desborde); el aviso de la
//   374 y el decimado del NPE solo absorben transitorios.
//
// Solucion: shed por DEUDA. El DSP dispone de un presupuesto por bloque
// (= 85% del periodo, v2.5.410; antes 70%). Cada bloque procesado suma a
// la deuda su sobrecoste (coste - presupuesto); cada bloque en shed la
// reduce en un presupuesto entero. Mientras haya deuda, el bloque se pasa
// en seco (passthrough). En regimen, el ciclo de trabajo converge
// exactamente a la fraccion sostenible presupuesto/coste (p. ej. coste
// 8.9 ms -> ~64% de bloques procesados, ~36% en passthrough), asi que la
// carga media del DSP nunca supera el presupuesto y la lectura nunca se
// retrasa mas de lo que el anillo absorbe. La deuda esta acotada a
// [-presupuesto, +presupuesto]: un pico aislado (GC, 20 ms) cuesta como
// mucho 1 bloque en shed, y tras una racha buena solo se toleran
// ~1 presupuesto de rafaga lenta antes de volver a soltar.
//
// Por que 85% (v2.5.410, medido en el Moto G85, captura 2026-11-11 19:09):
// el coste medio real de la cadena es ~4.90 ms/bloque (EMA muestreada
// 5.46 ms, sesgada al alza) y el 70% (4.67 ms) quedaba POR DEBAJO de la
// media: el gobernador soltaba 1 bloque de cada 21 (4.7% en la ventana,
// 3.4% acumulado) de forma cronica, ~7 veces/s — la imagen espacial
// respiraba. Con 85% (5.67 ms) la media cabe con 0.77 ms de margen y el
// shed cronico desaparece (solo quedan los sheds por picos aislados,
// ~1 bloque por pico). El 15% restante (1.0 ms) cubre el resto de la
// vuelta (reinjector ~0.12 ms medido en host, etapa de salida,
// masterTiming, NPE decimado: ~0.3-0.5 ms en total): 5.67 + 0.5 < 6.67 ms
// de periodo. El gobernador de vuelta completa (LoopLoadGovernor, Kotlin)
// sigue como red de seguridad si la vuelta entera supera el 100%.
// NOTA: AudioThreadBudgetGuard.SAFETY_MARGIN_RATIO (Kotlin) se queda en
// 70%: es solo referencia de medicion (su canExecute no se llama en la
// ruta en vivo) y no decide ningun shed; no confundir con este presupuesto.
//
// Fail-closed, por diseno:
//   - Shed = NO TOCAR el buffer: la senal seca sigue sonando por su via
//     y la reinyeccion diferencial aporta ~0 (delta = procesado - seco).
//     Jamas silencia ni pierde muestras; solo degrada el procesado.
//   - Se salta la llamada COMPLETA (igual que el early-return que ya
//     existe cuando otro hilo tiene g_dspProcessMutex): ningun estado
//     interno (IIR, STFT, convolver) queda avanzado a medias.
//   - Sin malloc, sin mutex, sin I/O, sin reloj de pared en el hot path
//     salvo dos lecturas de steady_clock por bloque en el llamador.
//   - La EMA solo mide la DEMANDA (bloques procesados): los bloques en
//     shed no la contaminan, asi que la telemetria sigue diciendo cuanto
//     cuesta el DSP de verdad.
//
// Hilo: una sola instancia por ruta, tocada solo bajo g_dspProcessMutex
// por el hilo de audio de Ruta A (igual que el resto del estado de
// nativeProcess). No es thread-safe por si misma a proposito: cero
// overhead de sincronizacion en el hot path.
//
// Tests host: dsp_core_stability.cpp (DspLoadShedGovernor.*) — reproduce
// el rango medido 3.5–8.9 ms y verifica que el shed acota la carga media
// al presupuesto sin silenciar nunca.
//

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace ivanna::dsp {

class DspLoadShedGovernor {
public:
    // Presupuesto del DSP por bloque, como fraccion del periodo:
    // 85% (v2.5.410; antes 70%). Ver el comentario de diseno arriba:
    // el coste medio medido en el G85 (~4.90 ms) superaba el 70%
    // (4.67 ms) y el shed era cronico (4.7% en la ventana 2026-11-11).
    // El 15% restante (1.0 ms) cubre el resto de la vuelta (~0.4 ms).
    static constexpr int64_t kBudgetNumerator = 17;
    static constexpr int64_t kBudgetDenominator = 20;

    static int64_t dspBudgetUs(int64_t periodUs) noexcept {
        if (periodUs <= 0) return 0;
        return periodUs * kBudgetNumerator / kBudgetDenominator;
    }

    // true mientras haya deuda pendiente: este bloque debe ir en seco.
    bool shouldShed(int64_t periodUs) const noexcept {
        if (periodUs <= 0) return false;
        return debtUs_ > 0;
    }

    // Bloque procesado: la deuda crece con el sobrecoste sobre el
    // presupuesto (negativa si el bloque fue mas rapido que el presupuesto).
    void observeRun(int64_t elapsedUs, int64_t periodUs) noexcept {
        if (periodUs <= 0 || elapsedUs < 0) return;
        const int64_t budgetUs = dspBudgetUs(periodUs);
        ++totalBlocks_;
        emaUs_ = (emaUs_ == 0) ? elapsedUs : emaUs_ + (elapsedUs - emaUs_) / 8;
        if (elapsedUs > peakUs_) peakUs_ = elapsedUs;
        // Pico de VENTANA (v2.5.415, bandera 2 de la 414): el pico de sesión
        // nunca se reseteaba, así que un spike aislado (arranque/GC, 117 ms
        // en la 414) persistía para siempre en dsp_pk y parecía una
        // regresión. El pico de ventana se resetea en cada lectura de
        // telemetría (takePeakWindowUs, 1 Hz, como los stage stats); el
        // pico de sesión se conserva aparte (peakUs) para el log.
        int64_t w = peakWindowUs_.load(std::memory_order_relaxed);
        while (elapsedUs > w &&
               !peakWindowUs_.compare_exchange_weak(w, elapsedUs,
                                                   std::memory_order_relaxed)) {
        }
        debtUs_ += elapsedUs - budgetUs;
        clampDebt(budgetUs);
    }

    // Bloque en shed (passthrough, coste ~0): devuelve un presupuesto
    // entero de deuda. La EMA no se toca: sigue midiendo la demanda real.
    void observeShed(int64_t periodUs) noexcept {
        if (periodUs <= 0) return;
        const int64_t budgetUs = dspBudgetUs(periodUs);
        ++totalBlocks_;
        ++shedBlocks_;
        debtUs_ -= budgetUs;
        clampDebt(budgetUs);
    }

    void reset() noexcept {
        debtUs_ = 0;
        emaUs_ = 0;
        peakUs_ = 0;
        peakWindowUs_.store(0, std::memory_order_relaxed);
        totalBlocks_ = 0;
        shedBlocks_ = 0;
    }

    int64_t debtUs() const noexcept { return debtUs_; }
    int64_t emaUs() const noexcept { return emaUs_; }
    // Pico de sesión (nunca se resetea salvo reset()): para el log.
    int64_t peakUs() const noexcept { return peakUs_; }
    // Pico de la ventana actual (1 s) + reset: lo llama el getter de
    // telemetría (nativeGetDspShed, 1 Hz). El valor devuelto es el peor
    // bloque de la ventana que termina; tras la lectura la ventana nueva
    // empieza en 0. Atómico relaxed: el hilo de audio escribe, el poller
    // lee/resetea; un pico perdido en la carrera es inocuo (telemetría).
    int64_t takePeakWindowUs() noexcept {
        return peakWindowUs_.exchange(0, std::memory_order_relaxed);
    }
    uint64_t totalBlocks() const noexcept { return totalBlocks_; }
    uint64_t shedBlocks() const noexcept { return shedBlocks_; }
    double shedFraction() const noexcept {
        return totalBlocks_ == 0 ? 0.0
                                 : static_cast<double>(shedBlocks_) /
                                       static_cast<double>(totalBlocks_);
    }

private:
    void clampDebt(int64_t budgetUs) noexcept {
        debtUs_ = std::clamp(debtUs_, -budgetUs, budgetUs);
    }

    int64_t debtUs_ = 0;       // >0: hay que soltar bloques. Acotada a ±presupuesto.
    int64_t emaUs_ = 0;        // EMA (1/8) del coste de los bloques PROCESADOS.
    int64_t peakUs_ = 0;       // Peor bloque procesado visto EN LA SESIÓN (log).
    // Peor bloque de la ventana de telemetría actual (~1 s). Atómico: lo
    // escribe el hilo de audio (observeRun) y lo lee+resetea el poller
    // (takePeakWindowUs). v2.5.415: antes dsp_pk era el pico de sesión y
    // un spike aislado (117 ms en la 414) persistía para siempre.
    std::atomic<int64_t> peakWindowUs_{0};
    uint64_t totalBlocks_ = 0;
    uint64_t shedBlocks_ = 0;
};

// ── RouteShedGovernors — UN gobernador por RUTA (0031, v2.5.459) ──────────
//
// Bug de la 458 (tronidos continuos "de inicio a fin", 2026-11-16): UN SOLO
// DspLoadShedGovernor global servía a los DOS llamadores de nativeProcess,
// que tienen periodos opuestos:
//   - BridgePlayer (MÚSICA): ~1984 frames @48 kHz = 41.3 ms de periodo,
//     presupuesto 85% = 35.1 ms, coste DSP ~6 ms (MUY por debajo).
//   - Captura (PlaybackCaptureService): BLOCK_FRAMES=320 @48 kHz = 6.67 ms,
//     presupuesto 5.67 ms, coste DSP ~8 ms (POR ENCIMA).
// La captura generaba deuda crónica (+2.3 ms/bloque) y esa deuda hacía que
// el BridgePlayer soltara ~20% de sus bloques aunque su propio coste estaba
// muy por debajo de su presupuesto (simulación del gobernador compartido,
// 10 s, costes medidos: BridgePlayer shed 20.2% sin estar sobrecargado;
// en vivo shed=28%). Cada cambio procesado->seco es un tronido
// (DspLoadTierPolicy.kt, medido 2026-11-12: "puro tronido, casi no hay
// melodía"): al reanudar, el DSP procesa con estado atrasado (STFT
// overlap-add, IIR, compresor) y el transitorio se oye como tronido.
// A ~5 transitorios/s el tronido es continuo, "de inicio a fin".
// La 0025 (priming + pacing de captura) no tocó el gobernador: por eso
// persistió en la 458 aunque la captura estuviera sana en telemetría.
//
// Fix: un gobernador por ruta, enrutado por el PERIODO del bloque (el
// presupuesto es 85% del periodo, así que dos periodos distintos no
// pueden compartir deuda sin contaminarse). El umbral 20 ms cae entre
// 6.67 ms (captura) y 41.3 ms (BridgePlayer) con margen 3x por cada lado.
// Cada ruta suelta solo por SU propia deuda: el BridgePlayer (~6 ms vs
// 35.1 ms) no suelta nunca en régimen; la captura sigue soltando su
// ~29% natural (su protección diseñada contra el desborde del
// RecordThread). El shed de una ruta ya no degrada la otra, y el tier
// de HOLOS (DspLoadTierPolicy, emaUs/budgetNs) vuelve a medir la carga
// de la ruta que se escucha (BridgePlayer) en vez de una mezcla.
//
// Sin malloc, sin mutex, sin I/O; dos instancias por valor, cero overhead
// en el hot path salvo una comparación de enteros por bloque.
// Tests host: test_shed_governor_split.cpp (reproduce el bug con un
// gobernador compartido y verifica el aislamiento con dos gobernadores).
//
// Umbral que separa las dos rutas de nativeProcess por su periodo de bloque.
static constexpr int64_t kShedLargeBlockPeriodUs = 20000;

class RouteShedGovernors {
public:
    // Gobernador de la ruta a la que pertenece este bloque, por su periodo.
    DspLoadShedGovernor& forPeriod(int64_t periodUs) noexcept {
        return periodUs > kShedLargeBlockPeriodUs ? large_ : small_;
    }
    const DspLoadShedGovernor& forPeriod(int64_t periodUs) const noexcept {
        return periodUs > kShedLargeBlockPeriodUs ? large_ : small_;
    }
    // BridgePlayer (MÚSICA): periodo ~41 ms. Es la ruta que se escucha.
    DspLoadShedGovernor& large() noexcept { return large_; }
    const DspLoadShedGovernor& large() const noexcept { return large_; }
    // Captura (PlaybackCaptureService): periodo 6.67 ms.
    DspLoadShedGovernor& small() noexcept { return small_; }
    const DspLoadShedGovernor& small() const noexcept { return small_; }
    void reset() noexcept {
        large_.reset();
        small_.reset();
    }

private:
    DspLoadShedGovernor large_;  // BridgePlayer: presupuesto ~35.1 ms.
    DspLoadShedGovernor small_;  // Captura: presupuesto 5.67 ms.
};

}  // namespace ivanna::dsp
