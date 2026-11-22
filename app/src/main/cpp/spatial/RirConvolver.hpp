#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// spatial/RirConvolver.hpp — Convolucionador RIR overlap-save, real-time safe
//
// Implementa convolución linear eficiente con la respuesta al impulso de sala
// del RirDataset usando el algoritmo overlap-save en el dominio de la frecuencia.
//
// Propiedades de tiempo real:
//   - Sin malloc() en process() — toda la memoria se reserva en load()
//   - Sin locks — diseñado para el hilo de audio (actualización atómica de IR)
//   - Sin librerías externas — FFT Radix-2 de fft_radix2.hpp
//   - Wet/dry configurable en tiempo real (setWetDry sin re-convolución)
//
// Limitaciones documentadas:
//   - IR máximo: MAX_IR_LEN muestras (512 @ 16kHz, interpoladas a 48kHz)
//   - Bloque: BLOCK_SIZE (512 frames)
//   - Sólo estéreo (L + R independientes — misma IR para ambos canales)
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>
#include <atomic>
#include <cstring>
#include <algorithm>  // std::clamp en setLateDecorrelation()
#include <cmath>

// F2/486 (labor grande, double interno): el NÚCLEO de señal de este
// convolver es double 64-bit de extremo a extremo — espectros IR,
// overlap-save, FDL de cola, buffers de trabajo y salida. La FFT
// propia (fftReal + twiddles float) se reemplaza por la keystone
// templada FFTRadix2d (misma convención: forward exp(-2πi), inversa
// conjugada con escala 1/N). La API float se conserva: load() sigue
// recibiendo la IR en float (dataset) con cast exacto, y
// process(float) delega por scratch en el núcleo double (lo usan JNI
// Ruta A y los tests). Los atómicos de control (wetDry, decorrel,
// earlyBoost, wetScale, rt60Delta, trueStereoCross, xtcStrength)
// siguen en float: son sliders, no señal. setDoubleAccum() queda
// como compat sin efecto: la acumulación ya es double siempre (la
// 0029 la tenía como default; el fallback float32 se elimina).
#include "fft_radix2.hpp"

namespace Ivanna {

class RirConvolver {
public:
    static constexpr int BLOCK    = 512;
    static constexpr int MAX_IR   = 512;   // particion 0 (head, latencia cero)
    static constexpr int FFT_SIZE = 1024;  // BLOCK + MAX_IR - 1, redondeado a pot2

    // ── Convolucion particionada no uniforme (estado del arte) ─────────────
    // Un RIR de sala real dura 0.3-2 s (>= 96k muestras @48k). Con un solo
    // segmento la latencia seria inaceptable. Solucion de referencia
    // (Gardner / Wefers): particionar el IR en head corto (latencia 0,
    // procesado en el bloque actual) + cola larga (overlap-save por bloques,
    // latencia de 1 bloque, inaudible en el rango de reverb).
    static constexpr int MAX_IR_TAIL  = 16384;            // ~341 ms @48k de cola
    static constexpr int TAIL_PARTS   = MAX_IR_TAIL / BLOCK; // 32 particiones de cola
    static constexpr int MAX_IR_TOTAL = MAX_IR + MAX_IR_TAIL;
    static constexpr float TAIL_LATENCY_BUDGET_MS = 20.0f;

    static bool tailLatencyWithinBudgetFor(int sampleRate, float budgetMs = TAIL_LATENCY_BUDGET_MS) noexcept {
        return sampleRate >= 8000 && sampleRate <= 384000 &&
               std::isfinite(budgetMs) && budgetMs > 0.0f &&
               (static_cast<float>(BLOCK) * 1000.0f / static_cast<float>(sampleRate)) <= budgetMs;
    }

    void setSampleRate(int sampleRate) noexcept {
        if (sampleRate >= 8000 && sampleRate <= 384000)
            sampleRate_.store(sampleRate, std::memory_order_relaxed);
    }
    int sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    bool tailLatencyWithinBudget() const noexcept {
        return tailLatencyWithinBudgetFor(sampleRate(), TAIL_LATENCY_BUDGET_MS);
    }
    bool tailLatencyBudgetExceeded() const noexcept {
        return tailLatencyBudgetExceeded_.load(std::memory_order_acquire);
    }

    RirConvolver();

    // Carga una IR estéreo desde vectores ya resampleados a la SR del engine.
    // Se puede llamar desde el hilo de control (no durante process()).
    // Thread-safe con process() vía flag atómico pending_.
    void load(const float* irL, const float* irR, int irLen) noexcept;
    // Carga con IR larga: hasta MAX_IR_TOTAL muestras. Las primeras MAX_IR van
    // al camino head (latencia cero); el resto a las particiones de cola.

    // Descarga la IR — vuelve a bypass puro.
    void unload() noexcept;

    // Wet/dry [0,1]. 0=bypass, 1=sólo sala. Actualización atómica en tiempo real.

    // ── BRIR: decorrelación de la cola tardía (estado del arte) ────────────
    // Una reverb estéreo creíble necesita que las colas L y R NO sean copias
    // correladas (eso suena a "reverb mono dentro de la cabeza"). Se aplica una
    // red allpass decorreladora en frecuencia a la partición de cola de R:
    // rota la fase por bin (allpass), preservando magnitud — mismo RT60, pero
    // difusa y envolvente. 0.0 = correlada (legado), 1.0 = máxima difusión.
    void setLateDecorrelation(float amount) noexcept { decorrel_.store(std::clamp(amount, 0.f, 1.f), std::memory_order_relaxed); }
    float lateDecorrelation() const noexcept { return decorrel_.load(std::memory_order_relaxed); }
    void setEarlyClarityBoost(float boost) noexcept { earlyBoost_.store(std::clamp(boost, 0.5f, 1.5f), std::memory_order_relaxed); }
    float earlyClarityBoost() const noexcept { return earlyBoost_.load(std::memory_order_relaxed); }
    void setTrueStereoCrossGain(float gain) noexcept { trueStereoCross_.store(std::clamp(gain, 0.0f, 0.45f), std::memory_order_relaxed); }
    float trueStereoCrossGain() const noexcept { return trueStereoCross_.load(std::memory_order_relaxed); }
    // Experimental wet-tail crossmix only; not a measured speaker-to-ear inverse.
    // Production runtime paths intentionally keep this at zero; use CrosstalkCanceller for route-gated XTC.
    void setTransauralXtcStrength(float strength) noexcept { xtcStrength_.store(std::clamp(strength, 0.0f, 0.85f), std::memory_order_relaxed); }
    float transauralXtcStrength() const noexcept { return xtcStrength_.load(std::memory_order_relaxed); }
    void applySofaCoupling(const float q[7]) noexcept;
    float wetScale() const noexcept  { return wetScale_.load(std::memory_order_relaxed); }
    float rt60Delta() const noexcept { return rt60Delta_.load(std::memory_order_relaxed); }
    // true si el acoplamiento SAF vigente difiere del horneado en la IR cargada.
    bool couplingStale() const noexcept;
    // Hilo de control/worker (NO audio): re-hornea la IR cruda con el acoplamiento actual.
    bool relinkFromRaw() noexcept;
    // Evidencia medida en el camino de audio (no indicadores): energia entrada/salida
    // de los bloques realmente convolucionados, cargas y re-acoplamientos efectivos.
    struct Evidence {
        uint64_t loads = 0, relinks = 0, activeBlocks = 0;
        double inEnergy = 0.0, outEnergy = 0.0;
        float bakedEarly = 0.f, bakedDecorrel = 0.f, bakedRt60Delta = 0.f, wetScale = 1.f;
    };
    Evidence evidence() const noexcept;
    void synthesizeMasterStudioBrir(float rt60S = 0.34f, int sampleRate = 48000) noexcept;
    bool needsMasterStudioBrirSynthesis(float rt60S, int sampleRate) const noexcept {
        const int sr = (sampleRate > 8000) ? sampleRate : 48000;
        const float rt60 = std::clamp(rt60S, 0.18f, 1.80f);
        return !(loaded_.load(std::memory_order_acquire) &&
                 lastSynthSr_ == sr &&
                 (rt60 - lastSynthRt60_ < 0.005f && lastSynthRt60_ - rt60 < 0.005f));
    }
    void setWetDry(float wet) noexcept { wetDry_.store(wet, std::memory_order_relaxed); }
    float wetDry() const noexcept      { return wetDry_.load(std::memory_order_relaxed); }

    // ── Cola en frecuencia (NEU·Ω Fase 3, 0028) ─────────────────────────
    // Cuando NEU·Ω está activo, la cola difusa vive EN LA MATRIZ
    // espectral (SpectralCore::applyTail) y este convolver NO debe
    // procesar su cola particionada (sería doble cola). Con
    // setTailEnabled(false), process() solo convoluciona el HEAD
    // (primeros 512 muestras, latencia cero, reflexiones tempranas) y
    // salta el paso FDL de cola. Con true (default), el comportamiento
    // es el de siempre (head + cola). Atómico en tiempo real.
    void setTailEnabled(bool on) noexcept { tailEnabled_.store(on, std::memory_order_relaxed); }
    bool tailEnabled() const noexcept { return tailEnabled_.load(std::memory_order_relaxed); }

    // ── Acumulación double en la FDL (0029, doble precisión) ─────────────
    // F2/486: el núcleo entero es double, así que la acumulación de las
    // 32 particiones de cola es double SIEMPRE. setDoubleAccum() queda
    // como compat de API sin efecto (antes: true = acumular en double,
    // false = fallback float32; ese fallback se elimina).
    void setDoubleAccum(bool on) noexcept { doubleAccum_.store(on, std::memory_order_relaxed); }
    bool doubleAccum() const noexcept { return doubleAccum_.load(std::memory_order_relaxed); }

    bool isLoaded() const noexcept { return loaded_.load(std::memory_order_acquire); }

    // Procesa un bloque de BLOCK frames estéreo.
    // L y R son buffers in-place — se modifican con la señal convolucionada.
    // Si !isLoaded() o wetDry()==0, es no-op (bypass perfecto).
    // Núcleo double (F2/486).
    void process(double* L, double* R, int frames) noexcept;
    // Envoltorio float (compat: JNI Ruta A, tests): convierte por
    // scratch y delega en el núcleo double.
    void process(float* L, float* R, int frames) noexcept;

private:
    // FFT keystone en double (F2/486): reemplaza fftReal() + twiddles
    // float propios. N fijo = FFT_SIZE (1024) en todas las llamadas.
    ivanna::FFTRadix2d fft1024_{FFT_SIZE};

    // Buffers de convolución overlap-save (double, no complex separado)
    double irReL_[FFT_SIZE] = {};  // Re(FFT(irL)) de longitud FFT_SIZE/2+1
    double irImL_[FFT_SIZE] = {};  // Im(FFT(irL))
    double irReR_[FFT_SIZE] = {};
    double irImR_[FFT_SIZE] = {};

    double overlapL_[MAX_IR] = {};  // Cola overlap-save canal L
    double overlapR_[MAX_IR] = {};
    int   overlapLen_ = 0;

    // Cola particionada: espectros de cada particion + FDL (frequency delay line)
    std::vector<double> tailIrReL_, tailIrImL_, tailIrReR_, tailIrImR_; // [TAIL_PARTS][FFT_SIZE]
    std::vector<double> pendTailIrReL_, pendTailIrImL_, pendTailIrReR_, pendTailIrImR_;
    std::vector<double> fdlReL_, fdlImL_, fdlReR_, fdlImR_;             // espectros de entrada
    int tailPartsActive_ = 0;   // cuantas particiones de cola tienen energia
    int pendTailPartsActive_ = 0;
    int fdlIndex_ = 0;          // puntero circular del FDL
    double tailInL_[BLOCK]   = {}; // acumulador de entrada de exactamente BLOCK muestras para FDL
    double tailInR_[BLOCK]   = {};
    double tailHistL_[BLOCK] = {}; // mitad anterior (512 muestras) para ventana FFT_SIZE=1024 del FDL
    double tailHistR_[BLOCK] = {};
    double tailOutL_[BLOCK]  = {}; // bloque de cola convolucionado listo para drenar
    double tailOutR_[BLOCK]  = {};
    int   tailPos_          = 0;  // posicion [0..BLOCK-1] dentro del acumulador/salida de cola

    double workRe_[FFT_SIZE] = {};  // Buffers de trabajo — sin malloc en process()
    double workIm_[FFT_SIZE] = {};
    // (F2/486: los acumuladores fdlAcc* de la 0029 ya no existen como
    // miembros: la suma de particiones se acumula directamente en
    // workRe_/workIm_ double.)
    // Scratch del envoltorio float -> double. FIX RT 487: antes eran
    // std::vector que crecían con resize() DENTRO de process() — una
    // violación RT-Safety que la Puerta cazó en la 486 (RirConvolver.cpp:398).
    // Ahora son arrays FIJOS (mismo patrón que workRe_/workIm_): cero heap
    // en el hilo de audio. Capacidad 8192 = kFusedMaxBlockFrames de la
    // cadena; bloques mayores se procesan por trozos en process().
    static constexpr int kWrapCapacity = 8192;
    std::array<double, kWrapCapacity> wrapL_{};
    std::array<double, kWrapCapacity> wrapR_{};

    std::atomic<int> sampleRate_{48000};
    std::atomic<bool> tailLatencyBudgetExceeded_{false};
    std::atomic<float> wetDry_{0.f};
    std::atomic<bool> tailEnabled_{true};   // Fase 3: false = solo head (NEU lleva la cola)
    std::atomic<bool> doubleAccum_{true};   // 0029: true = FDL acumula en double
    std::atomic<float> decorrel_{0.62f};     // Acoplado a coherencia interaural SOFA por defecto
    std::atomic<float> earlyBoost_{1.08f};   // Pre-énfasis de claridad temprana acoplado a muesca de pinna SOFA
    std::atomic<float> wetScale_{1.0f};      // SAF: escala del wet (aplicada en process)
    std::atomic<float> rt60Delta_{0.0f};     // SAF: delta relativo de RT60 (horneado en load)
    std::atomic<float> bakedEarly_{-1.f}, bakedDecorrel_{-1.f}, bakedRt60Delta_{0.f};
    std::atomic<uint64_t> loads_{0}, relinks_{0}, activeBlocks_{0};
    std::atomic<double> inEnergy_{0.0}, outEnergy_{0.0};
    std::vector<float> rawL_, rawR_;         // IR cruda para re-acoplar en vivo
    std::atomic<float> trueStereoCross_{0.18f}; // Matriz True-Stereo 4-caminos (LL, LR, RL, RR)
    std::atomic<float> xtcStrength_{0.0f};      // Crossmix experimental de cola RIR; no es XTC físico calibrado
    alignas(64) double crossHistL_[32] = {};
    alignas(64) double crossHistR_[32] = {};
    int crossWriteIdx_ = 0;
    double xtcLpL_ = 0.0, xtcLpR_ = 0.0;
    double xtcBassL_ = 0.0, xtcBassR_ = 0.0;
    double dcX1L_ = 0.0, dcY1L_ = 0.0;
    double dcX1R_ = 0.0, dcY1R_ = 0.0;
    // Anti-zipper del wet/dry: el slider se aplica por BLOQUE (escalón duro
    // de ganancia = tronido). Se suaviza por muestra con un one-pole.
    double wetNow_    = 0.0;   // wet efectivo suavizado (muestra a muestra)
    double wetSmooth_ = 0.0;   // coef. one-pole; 0 = se deriva de sampleRate
    float lastSynthRt60_ = -1.0f;
    int   lastSynthSr_   = 0;
    std::atomic<bool>  loaded_{false};
    std::atomic<bool>  pending_{false};
    std::atomic<bool>  clearHistoryPending_{false};

    // IR pendiente de aplicar (escrita por load(), leída por process())
    double pendIrReL_[FFT_SIZE] = {};
    double pendIrImL_[FFT_SIZE] = {};
    double pendIrReR_[FFT_SIZE] = {};
    double pendIrImR_[FFT_SIZE] = {};
    int   pendOverlapLen_ = 0;

    // FIX (tronidos, 2026-08-27): crossfade del IR al cambiar de sala.
    // Antes el swap borraba el overlap y cambiaba el IR en seco — la cola
    // de reverb se cortaba abruptamente = tronido audible. Ahora la IR
    // anterior se conserva y se funde con la nueva durante XFADE_BLOCKS
    // bloques (~43 ms @ 512/48k) en el dominio de la frecuencia; la cola
    // vieja (overlap) sigue sirviéndose sin cortes durante el fade.
    static constexpr int XFADE_BLOCKS = 4;  // 4 × 512 muestras ≈ 43 ms @48k
    double oldIrReL_[FFT_SIZE] = {};
    double oldIrImL_[FFT_SIZE] = {};
    double oldIrReR_[FFT_SIZE] = {};
    double oldIrImR_[FFT_SIZE] = {};
    int   xfadeBlocks_ = 0;  // >0 mientras dura el crossfade
};

} // namespace Ivanna
