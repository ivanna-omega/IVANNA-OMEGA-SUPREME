#pragma once
// ivanna_hal_stage.hpp — MÚSCULO F5 (Bloque 4, Fase 2 v2.5.472): etapa HAL
// del contrato SHM IvannaFusionState (consolidación integral §3.4).
//
// QUÉ ES: el lado MÚSCULO del contrato. Vive en la .so del HAL (o en el
// proceso que la cargue) y hace TRES cosas, nada más:
//   1. Lee las decisiones del cerebro (session_id, ratio, verdict,
//      heartbeat, control compacto) del SHM con seqlock.
//   2. Publica las mediciones del músculo (frames_written, sample_rate,
//      dsp_us_avg, xrun_count, muscle_timestamp_ms).
//   3. Decide passthrough vs engage con fail-closed estricto.
//
// QUÉ NO ES (en esta etapa): NO resamplea (ASRC en BYPASS 0033 —
// protección de estabilidad, ver F5_HAL_PROTOCOL.md), NO corre ORBE/HOLOS/
// Volterra (5B/5C), NO toca el snapshot ABI v13 del daemon (ese es otro
// contrato: omega_control_bus.h). El motor DSP es un slot lazy para 5A.
//
// REGLAS ANTI-BOOTLOOP (al pie de la letra, lección del 4º brick):
//   - Constructor/open(): SOLO open+mmap del SHM (sin O_CREAT: el músculo
//     NUNCA crea el SHM; si el cerebro no lo creó, passthrough). Cero new,
//     cero lectura de archivos de datos (SOFA/IVK1/config), cero hilos.
//     El kill switch NO se lee en open(): se consulta con throttle de 1 s
//     en processBlock().
//   - Init lazy de UN SOLO intento con new (std::nothrow) en el primer
//     processBlock() (equivalente al primer pal_stream_write). Si falla,
//     passthrough para siempre (no se reintenta: reintentar en el hilo de
//     audio es como se tumbó audioserver).
//   - Fail-closed a passthrough + ASRC off si: SHM ausente/inválido,
//     heartbeat del cerebro expirado (>100 ms), veredicto != LOADED,
//     kill switch presente, o safe-mode.
//   - Safe-mode tras 3 fallos consecutivos de lectura del SHM: passthrough
//     latched hasta reiniciar el proceso (no hay recuperación en caliente
//     dentro del HAL).
//   - fstat ≥ sizeof(IvannaFusionState) antes de mmap (FIX C-2: sin
//     SIGBUS si el archivo está truncado).
//   - Seqlock con sequence (ivanna_fusion_read_seq, FIX H-1): el lector
//     reintenta si el escritor estaba a mitad; el read-modify-write del
//     músculo conserva los campos del cerebro que leyó.
//
// KILL SWITCHES (no confundir, ver daemon_paths.h):
//   - Músculo (este archivo): /data/adb/ivanna_omega/pal_asrc.off (CON c).
//   - Daemon v2 (cerebro):   /data/adb/ivanna_omega/pal_asr.off  (SIN c).
//
// CÓDIGO SOLAMENTE: nada de esto se instala ni se reinicia sin la luz
// verde explícita de Esse. La .so del HAL no existe aún; este header es
// el contrato + la etapa, lista para enlazarse en 5A.

#include "include/ivanna_fusion_state.h"

#include <cstdint>
#include <cstring>
#include <new>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace ivanna::hal {

// Kill switch del músculo (CON la c, kDefaultKillSwitch en ivanna_asrc_ipc.h).
inline constexpr const char* kHalKillSwitchPath =
    "/data/adb/ivanna_omega/pal_asrc.off";
// El heartbeat del cerebro expira a los 100 ms (IVANNA_RATIO_MAX_AGE_MS).
inline constexpr uint64_t kHalHeartbeatMaxAgeNs = 100ULL * 1000000ULL;
// Safe-mode tras 3 fallos consecutivos (lectura SHM o init del motor).
inline constexpr int kHalMaxConsecutiveFailures = 3;

namespace detail {

// Slot del motor DSP. En esta etapa (5A pendiente) es passthrough puro:
// existe para que el init lazy y el gate engage/passthrough sean reales
// y testeables, y para que 5A sustituya esta clase por el ASRC v6 sin
// tocar la etapa. Scratch pre-reservado: cero allocs en el camino caliente.
class HalEngine {
public:
    HalEngine() = default;
    // Passthrough bit-exacto (ASRC BYPASS 0033): no transforma muestras.
    void process(float* /*left*/, float* /*right*/, int /*frames*/) noexcept {}
private:
    float scratch_[256]{};  // 128 frames estéreo, para 5A/5B/5C
};

inline uint64_t monotonicNs() noexcept {
    struct timespec ts;
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace detail

class IvannaHalStage {
public:
    IvannaHalStage() = default;
    IvannaHalStage(const IvannaHalStage&) = delete;
    IvannaHalStage& operator=(const IvannaHalStage&) = delete;
    ~IvannaHalStage() { close(); }

    // Solo open+mmap del SHM. Sin O_CREAT, sin new, sin hilos, sin leer
    // ningún archivo de datos. false = passthrough (el llamador sigue
    // sonando limpio; no es un error fatal).
    bool open(const char* shmPath = IVANNA_FUSION_SHM_PATH,
              const char* killSwitchPath = kHalKillSwitchPath) noexcept {
        if (shm_) return true;
        killSwitchPath_ = killSwitchPath ? killSwitchPath : kHalKillSwitchPath;
        if (!shmPath) return false;
        const int fd = ::open(shmPath, O_RDWR | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st {};
        if (::fstat(fd, &st) != 0 ||
            st.st_size < static_cast<off_t>(sizeof(IvannaFusionState))) {
            ::close(fd);
            return false;  // FIX C-2: sin mmap de un archivo truncado
        }
        void* p = ::mmap(nullptr, sizeof(IvannaFusionState),
                         PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            ::close(fd);
            return false;
        }
        shmFd_ = fd;
        shm_ = static_cast<IvannaFusionState*>(p);
        return true;
    }

    void close() noexcept {
        if (shm_) {
            ::munmap(shm_, sizeof(IvannaFusionState));
            shm_ = nullptr;
        }
        if (shmFd_ >= 0) {
            ::close(shmFd_);
            shmFd_ = -1;
        }
        delete engine_;
        engine_ = nullptr;
    }

    bool isShmOpen() const noexcept { return shm_ != nullptr; }
    bool isSafeMode() const noexcept { return safeMode_; }
    bool isPassthrough() const noexcept { return !engaged_; }
    int failureCount() const noexcept { return failures_; }
    float dspUsAvg() const noexcept { return dspUsAvg_; }
    uint32_t xrunCount() const noexcept { return xrunCount_; }
    uint64_t framesWritten() const noexcept { return framesWritten_; }

    // El envoltorio HAL llama a esto cuando detecta un xrun (underrun del
    // buffer). La etapa no puede detectarlo sola: no tiene reloj de
    // presentación propio. Contador acumulado, publicado en el SHM.
    void noteXrun() noexcept {
        if (xrunCount_ < 0xFFFFFFFFu) ++xrunCount_;
    }

    // Un bloque de audio (hilo RT del HAL; equivalente al primer
    // pal_stream_write para el init lazy). En esta etapa SIEMPRE pasa
    // limpio (ASRC BYPASS 0033) y solo mide + publica. `frames` es el
    // número de frames estéreo del bloque.
    void processBlock(float* left, float* right, int frames,
                      uint32_t sampleRateHz) noexcept {
        if (!left || !right || frames <= 0) return;
        const uint64_t t0 = detail::monotonicNs();

        // 1. Init lazy de UN SOLO intento (new (std::nothrow)).
        if (!initAttempted_) {
            initAttempted_ = true;
            engine_ = new (std::nothrow) detail::HalEngine();
            if (!engine_) {
                if (++failures_ >= kHalMaxConsecutiveFailures) safeMode_ = true;
            }
        }

        // 2. Kill switch (throttle 1 s: un access() por bloque es un
        // syscall por bloque en el hilo RT; una vez por segundo basta).
        const uint64_t nowMs = t0 / 1000000ULL;
        if (nowMs - lastKillCheckMs_ > 1000ULL) {
            lastKillCheckMs_ = nowMs;
            killSwitch_ = (::access(killSwitchPath_, F_OK) == 0);
        }

        // 3. Decisión engage/passthrough (fail-closed).
        engaged_ = false;
        if (shm_ && engine_ && !safeMode_ && !killSwitch_) {
            IvannaFusionState st{};
            bool ok = false;
            for (int i = 0; i < 3 && !ok; ++i)
                ok = ivanna_fusion_read(shm_, &st) != 0;
            if (ok && st.magic == IVANNA_FUSION_MAGIC &&
                st.version >= IVANNA_FUSION_VERSION) {
                failures_ = 0;
                // 5A: engage solo con cerebro vivo + heartbeat fresco +
                // veredicto LOADED + ASRC habilitado (asrc_bypass == 0).
                // Hoy el daemon publica asrc_bypass = 1 (BYPASS 0033), así
                // que engaged_ queda en false y todo pasa limpio.
                engaged_ = brainFresh(st, t0) &&
                           st.verdict == IVANNA_VERDICT_LOADED &&
                           st.asrc_bypass == 0u;
            } else {
                if (++failures_ >= kHalMaxConsecutiveFailures) safeMode_ = true;
            }
        }

        // 4. Proceso: en esta etapa el motor es passthrough aunque
        // engaged_ sea true (el ASRC v6 llega en 5A). El gate existe para
        // que el cambio de 5A sea un cambio de motor, no de etapa.
        if (engaged_ && engine_) engine_->process(left, right, frames);

        // 5. Medición + publicación del bloque músculo (best-effort: si
        // el SHM no se puede leer, el audio ya pasó limpio; solo se
        // pierde telemetría, nunca sonido).
        const uint64_t t1 = detail::monotonicNs();
        const float blockUs = static_cast<float>(t1 - t0) / 1000.0f;
        dspUsAvg_ = (dspUsAvg_ <= 0.0f) ? blockUs
                                        : dspUsAvg_ * 0.9f + blockUs * 0.1f;
        framesWritten_ += static_cast<uint64_t>(frames);
        publishMuscle(frames, sampleRateHz, nowMs);
    }

private:
    bool brainFresh(const IvannaFusionState& st, uint64_t nowNs) const noexcept {
        if (st.brain_health != 1u) return false;
        if (st.daemon_heartbeat_ns == 0u) return false;
        if (nowNs < st.daemon_heartbeat_ns) return false;  // reloj hacia atrás
        return (nowNs - st.daemon_heartbeat_ns) <= kHalHeartbeatMaxAgeNs;
    }

    void publishMuscle(int frames, uint32_t sampleRateHz, uint64_t nowMs) noexcept {
        if (!shm_) return;
        IvannaFusionState st{};
        bool ok = false;
        uint32_t seq = 0;
        for (int i = 0; i < 3 && !ok; ++i)
            ok = ivanna_fusion_read_seq(shm_, &st, &seq) != 0;
        if (!ok) {
            if (++failures_ >= kHalMaxConsecutiveFailures) safeMode_ = true;
            return;
        }
        // Read-modify-write: se conservan los campos del cerebro que se
        // acaban de leer (mismo patrón que el daemon; el seqlock evita
        // lecturas rotas, no escrituras perdidas entre los dos escritores
        // — limitación conocida del contrato, ver F5_HAL_PROTOCOL.md).
        IvannaFusionState w = st;
        w.frames_written = static_cast<uint32_t>(framesWritten_ & 0xFFFFFFFFu);
        w.stream_rate_hz = sampleRateHz;
        w.stream_channels = 2;
        w.muscle_health = 1;
        w.muscle_timestamp_ms = nowMs;
        w.dsp_us_avg = dspUsAvg_;
        w.xrun_count = xrunCount_;
        w.frames_passthrough += static_cast<uint64_t>(frames);
        ivanna_fusion_write(shm_, &w);
    }

    IvannaFusionState* shm_ = nullptr;
    int shmFd_ = -1;
    detail::HalEngine* engine_ = nullptr;
    bool initAttempted_ = false;
    bool safeMode_ = false;
    bool engaged_ = false;
    bool killSwitch_ = false;
    int failures_ = 0;
    uint64_t lastKillCheckMs_ = 0;
    float dspUsAvg_ = 0.0f;
    uint32_t xrunCount_ = 0;
    uint64_t framesWritten_ = 0;
    const char* killSwitchPath_ = kHalKillSwitchPath;
};

}  // namespace ivanna::hal
