#pragma once
/**
 * IvannaAudioClassifier.hpp — IA Audio Intelligence Engine OEM++
 * ============================================================================
 * Reemplaza el modelo YAMNet obsoleto con una arquitectura TinyML nativa
 * cuantizada en INT8 (Fast-CRNN / MobileNetV3-Tiny-Audio).
 * 
 * Arquitectura:
 *   - Lock-free, Wait-free SPSC Ring Buffer para ingesta desde el Audio Callback.
 *   - Hilo de inferencia asíncrono con prioridad SCHED_FIFO (low-latency).
 *   - Extracción de Mel-Spectrogram optimizada con ARM NEON Intrinsics.
 *   - Clasificación en 6 categorías (Music, Movie, Game, Voice, Ambient, Unknown).
 *   - Uso de punteros atómicos para evitar Malloc/Free en tiempo de ejecución (Zero-Copy).
 *
 * FRONTEND REAL DETRÁS DE FLAG (2026-11-24, fix CORONA Frente A):
 *   La causa raíz del STUB era doble: (1) el banco mel se ponía a cero y
 *   nada lo llenaba nunca; (2) computeSTFT() NO calculaba FFT (el
 *   "espectro" era |muestra ventaneada| x 0.5). Ambas están corregidas:
 *   initFilterbankAndWindow() construye el banco triangular real (64
 *   bandas, 0-8000 Hz, misma construcción que IvannaTinyMLKernel) y
 *   computeSTFT() calcula la FFT radix-2 de 512 con las tablas
 *   precalculadas (bit-reversal + twiddles). PERO el uso del frontend
 *   real está detrás de setRealFrontendEnabled(), con default OFF
 *   (fail-closed): con el flag OFF, computeSTFT() y
 *   extractLogMelFilterbank() reproducen EXACTAMENTE el comportamiento
 *   legacy (espectro falso + log(1e-10) constante → siempre MUSIC), así
 *   que el sonido por defecto NO cambia y P0 sigue en verde. Encender el
 *   flag vuelve adaptativo al guardián AntiDolby (cambia el sonido): solo
 *   con decisión de Esse + ABX. Los pesos INT8 siguen siendo placeholder
 *   sintético salvo loadWeights(); el cableado real (CRNN TFLite +
 *   frontend idéntico al entrenamiento) sigue siendo trabajo de
 *   laboratorio.
 */

#include "IvannaFusionCore.hpp"
#include <cmath>
#include <cstdint>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>
#include <array>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define ALIGN_NEON alignas(16)
#else
#define ALIGN_NEON alignas(16)
#endif

namespace Ivanna {

// Audio context classes for Omega Supreme Engine
enum class AudioContextClass : uint8_t {
    UNKNOWN = 0,
    MUSIC = 1,
    MOVIE = 2,
    GAME = 3,
    VOICE = 4,
    AMBIENT = 5
};

// Lock-free context data struct
struct AIModelOutput {
    std::array<float, 6> probabilities{};
    AudioContextClass dominant_class = AudioContextClass::UNKNOWN;
    float confidence = 0.0f;
    float scene_energy = 0.0f; // Dynamic harmonic excitation target
    bool is_valid = false;
};

// Hiperparámetros TinyML
constexpr size_t MEL_BANDS           = 64;
constexpr size_t CLASSIFIER_FRAME_SIZE = 512;
constexpr size_t FFT_SPECTRUM_SIZE   = CLASSIFIER_FRAME_SIZE / 2 + 1;
constexpr size_t NUM_CLASSES         = 6;
constexpr size_t RING_BUFFER_CAPACITY = 32768; // Potencia de 2 para bitwise masking

// SPSC Ring Buffer lock-free
template <typename T, size_t Capacity>
class alignas(64) LockFreeAudioRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power-of-two");
public:
    LockFreeAudioRingBuffer() : m_head(0), m_tail(0) {}

    inline bool push(const T* src, size_t count) noexcept {
        const size_t h = m_head.load(std::memory_order_relaxed);
        const size_t t = m_tail.load(std::memory_order_acquire);
        if (Capacity - (h - t) < count) return false;
        
        for (size_t i = 0; i < count; ++i)
            m_buffer[(h + i) & (Capacity - 1)] = src[i];
            
        m_head.store(h + count, std::memory_order_release);
        return true;
    }

    inline bool pop(T* dst, size_t count) noexcept {
        const size_t t = m_tail.load(std::memory_order_relaxed);
        const size_t h = m_head.load(std::memory_order_acquire);
        if (h - t < count) return false;
        
        for (size_t i = 0; i < count; ++i)
            dst[i] = m_buffer[(t + i) & (Capacity - 1)];
            
        m_tail.store(t + count, std::memory_order_release);
        return true;
    }

    inline size_t available() const noexcept {
        return m_head.load(std::memory_order_acquire) 
             - m_tail.load(std::memory_order_relaxed);
    }
private:
    alignas(64) T m_buffer[Capacity];
    alignas(64) std::atomic<size_t> m_head;
    alignas(64) std::atomic<size_t> m_tail;
};

class alignas(64) IvannaAudioClassifier {
public:
    IvannaAudioClassifier();
    ~IvannaAudioClassifier();

    // Hot-path injection from audio callback. Wait-free.
    void ingestAudioFrame(const float* left, const float* right, size_t n) noexcept;

    // Output reading. Thread-safe, wait-free.
    void getClassProbabilities(float* outProbs) const noexcept;
    uint8_t getDominantClass() const noexcept;
    float getVoiceConfidence() const noexcept;
    bool getModelOutput(AIModelOutput& out) const noexcept;

    bool loadWeights(const void* data, size_t bytes) noexcept;
    bool loadWeights(const char* path) noexcept;
    constexpr size_t expectedWeightCount() const noexcept { return 8774u; }

    // ── Modo síncrono (determinista, solo host) ──────────────────────────
    // En host, el hilo de inferencia hace que el momento en que una
    // clasificación se hace visible dependa de la planificación del SO:
    // el guardián AntiDolby (que se alimenta de getModelOutput) sigue
    // entonces trayectorias distintas en cada corrida y NEURO·Ω no es
    // determinista entre corridas (hallazgo P0 2026-11-22: op_neuro
    // maxDiff 0.33-0.42; el primer chunk válido llegó en el chunk 126 en
    // una corrida y en el 1458 en otra). En modo síncrono NO se usa el
    // hilo: el llamador (hilo de audio) drena los frames completos con
    // processPendingFrames() antes de leer, así la secuencia de
    // clasificaciones es función pura del audio. La inferencia por frame
    // es la misma (mismo código, mismos frames, mismo orden): la salida
    // por frame es idéntica; solo su momento de entrega pasa a ser
    // función de la posición de audio. En Android el modo asíncrono no
    // cambia (tiempo real). One-way: no hay vuelta a asíncrono.
    void setSynchronous(bool on) noexcept;
    void processPendingFrames() noexcept;
    bool isSynchronous() const noexcept {
        return m_synchronous.load(std::memory_order_relaxed);
    }

    // ── Frontend mel real (fix CORONA Frente A, 2026-11-24) ─────────────
    // Default OFF (fail-closed): con el flag OFF el clasificador se
    // comporta EXACTAMENTE como antes del fix (siempre MUSIC) y el sonido
    // no cambia. Con el flag ON usa la FFT real + el banco mel triangular
    // y el guardián AntiDolby se vuelve adaptativo al contenido (cambia
    // el sonido: solo con decisión de Esse + ABX). Se puede volver a OFF
    // en cualquier momento: el comportamiento legacy se restaura exacto.
    void setRealFrontendEnabled(bool on) noexcept {
        m_realFrontendEnabled.store(on, std::memory_order_relaxed);
    }
    bool isRealFrontendEnabled() const noexcept {
        return m_realFrontendEnabled.load(std::memory_order_relaxed);
    }

    // ── Telemetría / tests del frontend (sin efecto en el audio) ───────
    // Suma de todos los pesos del banco mel triangular (0 si el banco
    // estuviera en cero, como antes del fix). Sirve para afirmar en tests
    // que el banco ya no es cero sin exponer el array interno.
    float melFilterbankWeightSum() const noexcept;
    // Copia de las 64 energías log-mel del último frame inferido (0 si
    // aún no hay inferencia). Permite afirmar en tests que el frontend
    // es selectivo en frecuencia (un tono grave concentra la energía en
    // bandas bajas y uno agudo en bandas altas).
    std::array<float, MEL_BANDS> logMelEnergies() const noexcept;

private:
    LockFreeAudioRingBuffer<float, RING_BUFFER_CAPACITY> m_audioRingBuffer;
    
    // Feature extraction buffers
    ALIGN_NEON float m_frameBuffer[CLASSIFIER_FRAME_SIZE];
    ALIGN_NEON float m_windowedFrame[CLASSIFIER_FRAME_SIZE];
    ALIGN_NEON float m_powerSpectrum[FFT_SPECTRUM_SIZE];
    ALIGN_NEON float m_melLogEnergies[MEL_BANDS];
    
    // NEON specific precomputed data
    ALIGN_NEON float m_hanningWindow[CLASSIFIER_FRAME_SIZE];
    ALIGN_NEON float m_fftTwiddleReal[CLASSIFIER_FRAME_SIZE / 2];
    ALIGN_NEON float m_fftTwiddleImag[CLASSIFIER_FRAME_SIZE / 2];
    uint16_t m_bitRevTable[CLASSIFIER_FRAME_SIZE];
    // Banco mel triangular real (64 bandas, 0-8000 Hz): lo construye
    // initFilterbankAndWindow(). Su USO está detrás del flag
    // m_realFrontendEnabled (default OFF): con el flag OFF,
    // extractLogMelFilterbank() devuelve el legacy log(1e-10) constante
    // y el banco no se consulta, así que el sonido no cambia.
    ALIGN_NEON float m_melFilterbank[MEL_BANDS][FFT_SPECTRUM_SIZE];

    // Decimador FIR state
    float  m_decimFirBuf[8];
    size_t m_decimFirPos;
    size_t m_decimBufPos;

    // TinyML Quantized INT8 Weights Arrays
    // STUB: sin loadWeights() estos pesos son placeholder sintético
    // (ver runInt8Inference); en producción loadWeights() no se llama.
    ALIGN_NEON int8_t m_qWeights[8192];
    bool m_weightsLoaded;

    // True wait-free, zero-allocation Triple Buffering for lock-free audio thread read
    ALIGN_NEON mutable AIModelOutput m_outputPool[3];
    alignas(64) mutable std::atomic<AIModelOutput*> m_cleanOutput;
    alignas(64) mutable AIModelOutput* m_readingOutput;
    alignas(64) AIModelOutput* m_writingOutput;
    
    std::atomic<bool> m_running{false};
    std::thread m_inferenceThread;
    // Modo síncrono (ver setSynchronous): true = hilo parado, el
    // llamador drena con processPendingFrames(). Solo host.
    std::atomic<bool> m_synchronous{false};
    // Frontend mel real (ver setRealFrontendEnabled): false = legacy
    // exacto (siempre MUSIC); true = FFT + banco mel reales.
    std::atomic<bool> m_realFrontendEnabled{false};

    void initFilterbankAndWindow() noexcept;
    void inferenceLoop() noexcept;
    void computeSTFT(const float* frame) noexcept;
    void extractLogMelFilterbank() noexcept;
    void runInt8Inference() noexcept;
};

} // namespace Ivanna
