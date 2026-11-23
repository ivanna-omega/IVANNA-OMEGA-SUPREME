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
 * STUB (2026-11-23) — este clasificador NO clasifica contenido real hoy:
 *   1. El banco mel (m_melFilterbank) se pone a cero en
 *      initFilterbankAndWindow() y nada lo llena nunca → extractLogMelFilterbank()
 *      devuelve log(1e-10) en las 64 bandas, siempre.
 *   2. computeSTFT() NO calcula FFT: el espectro es |muestra| x 0.5.
 *   3. Los pesos INT8 son placeholder sintético salvo que alguien llame
 *      loadWeights(); en producción nadie lo llama (solo un test) →
 *      la salida es constante (siempre MUSIC en la práctica).
 * Consecuencia: el guardián AntiDolby corre sobre un objetivo FIJO, no
 * adaptativo al contenido, en host Y en el teléfono (mismo código).
 * Arreglar el frontend cambiaría el sonido: queda fuera de este commit por
 * decisión explícita; el cableado real (CRNN TFLite + frontend idéntico al
 * entrenamiento) es trabajo de laboratorio detrás de flag + ABX.
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
    // STUB: siempre a cero (ver initFilterbankAndWindow) — nunca se llena
    // con filtros triangulares reales.
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

    void initFilterbankAndWindow() noexcept;
    void inferenceLoop() noexcept;
    void computeSTFT(const float* frame) noexcept;
    void extractLogMelFilterbank() noexcept;
    void runInt8Inference() noexcept;
};

} // namespace Ivanna
