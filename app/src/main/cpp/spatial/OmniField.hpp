// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// OmniField.hpp — Operador Unificado de Campo Espacial (OMNI-FIELD), v1.
//
// Fusión matemática de 4 fuentes en UN SOLO operador (ver docs/OMNI_FIELD.md,
// ~/workspace/analysis-ivanna/OMNI_FIELD.md):
//   (a) Separación por fuentes estilo OmniSpatial (4 stems por crossover +
//       transitorios; el residual garantiza que la suma reproduce el mid).
//   (b) Personalización HRTF con gradiente escalado por MAA (30–52% menos
//       cómputo: diezmado por sector + umbral de rediseño = MAA del sector).
//   (c) Escena espacial auto-supervisada estilo AudioSphere (azimut dominante
//       + difusividad estimados de la propia señal, sin etiquetas).
//   (d) Campo neural INAF como renderer continuo por muestra (SIREN N1+N3,
//       FIR de fase mínima + retardo fraccional, sin STFT).
//
//   y[n] = (1−a)·x[n] + a · Σ_k R_{p'(θ_k, φ_k, r_k)} { s_k[n] }
//
// Cadena Ruta A: ORBE → HOLOS → OMNI-FIELD → Φ. ORBE y HOLOS SIGUEN ACTIVOS
// (directiva 2026-11-08); OMNI re-espacializa el campo ya mejorado por ellos.
// amount=0 ⇒ bypass bit-exacto. Fail-closed absoluto: entrada o parámetros no
// finitos ⇒ bypass; fallo de diseño FIR ⇒ se conserva el FIR anterior (o un
// FIR plano si no hay ninguno); renderer mudo con entrada presente ⇒ bypass.
// Cero allocations en process() (todo prealocado en prepare()).
// Núcleo double (F2/486): el plano de señal corre en 64-bit; los
// envoltorios float de process()/separate()/etc. son compat legacy
// (JNI Ruta A y tests) y delegan en el núcleo double vía scratch.
//
// Contrato: header-only, C++17 portable, sin excepciones. prepare() DEBE
// llamarse antes de process() (lo hace nativeInit/nativeInitDSP); sin prepare,
// process() es bypass silencioso (fail-closed).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>

#include "OmniFieldConvergence.hpp"
#include "OmniFieldRender.hpp"
#include "OmniFieldScene.hpp"
#include "OmniFieldSiren.hpp"

namespace ivanna {
namespace omnifield {

// Parámetros del operador (los lee el JNI cada bloque desde los atomics).
struct OmniParams {
    float amount = 1.0f;        // 0 = bypass bit-exacto, 1 = render completo
    float scenePull = 0.3f;     // atracción de stems hacia el centroide (0..1)
    float distanceM = 1.2f;     // distancia de la voz (m); escala r de todos
    bool headTracking = true;   // si false, el yaw se ignora
    bool convergence = true;    // convergencia Omega–SAFMUS (Kuramoto +
                                // fusión por r + tono emergente); false =
                                // comportamiento previo (K=0, sin tono
                                // emergente, pull_eff = pull)
};

class OmniField {
public:
    static constexpr int kNumStems = StemSeparator::kNumStems;  // 4
    static constexpr int kChunk = 512;  // frames por sub-bloque interno

    // 5º stem (tono emergente) MUERTO — v2.5.488 (V1 en vivo 2026-11-21):
    // el tono a 884 Hz (2º armónico de 442 Hz, r·0.10·energyGate_) es el
    // "piiiii" que se escucha en audífonos; al pausar la música muere al
    // instante (Kuramoto, no auto-oscilación — V1 CONFIRMADA) y regresa
    // al cambiar la salida a audífonos (en bocinas la puerta Genezi lo
    // anula todo). Con false el paso 7.5 no renderiza el tono: el campo
    // de Kuramoto SIGUE corriendo (stepBlock, rBlock_/pullEff, fusión de
    // los 4 stems) y solo desaparece la línea de 884 Hz. El código del
    // 5º stem se conserva como LEGACY (no se borra) por si se re-evalúa.
    static constexpr bool kEmergentToneEnabled = false;

    // Posiciones canónicas por stem: (azimut°, elevación°, distancia m).
    static constexpr float kStemAzDeg[kNumStems] = {0.0f, -30.0f, 0.0f, 60.0f};
    static constexpr float kStemElDeg[kNumStems] = {0.0f, 0.0f, -10.0f, 15.0f};
    static constexpr float kStemDistM[kNumStems] = {1.2f, 1.6f, 1.0f, 2.0f};

    OmniField() = default;
    OmniField(const OmniField&) = delete;
    OmniField& operator=(const OmniField&) = delete;

    // Prepara todo (puede allocar: FFT del diseñador). Idempotente.
    void prepare(float sampleRate) {
        sampleRate_ = (std::isfinite(sampleRate) && sampleRate >= 8000.0f &&
                       sampleRate <= 192000.0f)
                          ? static_cast<double>(sampleRate)
                          : 48000.0;
        separator_.prepare(sampleRate_);
        scene_.prepare(sampleRate_);
        personalizer_.prepare(sampleRate_);
        designer_.prepare(sampleRate_);
        kuramoto_.prepare(sampleRate_);
        for (int k = 0; k < kNumStems; ++k) renderers_[k].prepare(sampleRate_);
        emergentRenderer_.prepare(sampleRate_);
        prepared_ = true;
        reset();
    }

    void reset() noexcept {
        processCalls_.store(0, std::memory_order_relaxed);
        blocksRendered_.store(0, std::memory_order_relaxed);
        framesRendered_.store(0, std::memory_order_relaxed);
        separator_.reset();
        scene_.reset();
        personalizer_.reset();
        kuramoto_.reset();
        emergentRenderer_.reset();
        emergentFirValid_ = false;
        emergentLastTheta_ = 0.0;
        emergentLastR_ = 0.0;
        rBlock_ = 0.0;
        energyGate_ = 0.0;
        for (int k = 0; k < kNumStems; ++k) {
            renderers_[k].reset();
            firValid_[k] = false;
            lastTheta_[k] = 0.0;
            lastR_[k] = 0.0;
        }
        appliedAmount_ = std::isfinite(params_.amount)
                             ? static_cast<double>(std::clamp(params_.amount, 0.0f, 1.0f))
                             : 0.0;
        yawRad_ = 0.0;
        fault_ = false;
        designCount_ = 0;
        blockIndex_ = 0;
        levelGain_ = 1.0f;
        hasProcessed_ = false;
    }

    void setParams(const OmniParams& p) noexcept {
        params_.amount = std::isfinite(p.amount) ? std::clamp(p.amount, 0.0f, 1.0f) : 0.0f;
        params_.scenePull =
            std::isfinite(p.scenePull) ? std::clamp(p.scenePull, 0.0f, 1.0f) : 0.3f;
        params_.distanceM =
            std::isfinite(p.distanceM) ? std::clamp(p.distanceM, 0.5f, 2.0f) : 1.2f;
        params_.headTracking = p.headTracking;
        params_.convergence = p.convergence;
        // Antes del primer process() no ha fluido audio: el amount aplicado
        // sigue al objetivo sin rampa (así amount=0 es bypass bit-exacto
        // desde el primer bloque). Después, los cambios de amount usan la
        // rampa suave de processChunk para no hacer clic.
        if (!hasProcessed_) appliedAmount_ = params_.amount;
    }

    void setAmount(float a) noexcept {
        params_.amount = std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 0.0f;
        if (!hasProcessed_) appliedAmount_ = params_.amount;
        if (params_.amount == 0.0f) fault_ = false;  // re-arme (como INAF)
    }

    void setHeadPose(float yawRad) noexcept {
        yawRad_ = std::isfinite(yawRad)
                      ? static_cast<double>(std::clamp(yawRad, -3.14159265f, 3.14159265f))
                      : 0.0;
    }

    void setAudiogram(const float lossL[6], const float lossR[6]) noexcept {
        personalizer_.setAudiogram(lossL, lossR);
    }

    // In-place, cualquier n>0. amount=0 ⇒ no toca los buffers.
    // Núcleo double (F2/486); el overload float es compat legacy (JNI
    // Ruta A) y delega en él vía scratch miembro.
    void process(double* l, double* r, int n) noexcept {
        if (!prepared_ || l == nullptr || r == nullptr || n <= 0) return;
        if (fault_) return;  // bypass hasta reset() o setAmount(0)
        hasProcessed_ = true;
        processCalls_.fetch_add(1, std::memory_order_relaxed);
        for (int off = 0; off < n; off += kChunk) {
            const int m = std::min(kChunk, n - off);
            processChunk(l + off, r + off, m);
            if (fault_) return;  // el chunk marcó fault: resto en bypass
        }
    }

    // Envoltorio float (compat legacy): convierte por scratch y delega
    // en el núcleo double. El NaN/Inf se PRESERVA en la conversión (sin
    // sanear): el núcleo marca fault en el paso 1 y el chunk queda en
    // bypass, como en el float histórico. La devolución usa el delta de
    // blocksRendered_: solo los chunks renderizados se escriben; el chunk
    // que marcó fault se restaura al original y el resto queda intacto.
    // Una sola llamada al núcleo: processCalls() cuenta 1, como antes.
    void process(float* l, float* r, int n) noexcept {
        if (!prepared_ || l == nullptr || r == nullptr || n <= 0) return;
        if (fault_) return;
        const std::size_t un = static_cast<std::size_t>(n);
        if (wrapL_.size() < un) {
            wrapL_.resize(un); wrapR_.resize(un);
            origL_.resize(un); origR_.resize(un);
        }
        for (int i = 0; i < n; ++i) {
            origL_[i] = l[i];
            origR_[i] = r[i];
            wrapL_[i] = static_cast<double>(l[i]);
            wrapR_[i] = static_cast<double>(r[i]);
        }
        const std::uint64_t renderedBefore = blocksRendered_.load(std::memory_order_relaxed);
        process(wrapL_.data(), wrapR_.data(), n);
        const std::uint64_t rendered =
            blocksRendered_.load(std::memory_order_relaxed) - renderedBefore;
        const int doneChunks = static_cast<int>(std::min<std::uint64_t>(rendered, 0x7fffffffu));
        int off = 0;
        for (int c = 0; c < doneChunks && off < n; ++c, off += kChunk) {
            const int m = std::min(kChunk, n - off);
            for (int i = 0; i < m; ++i) {
                l[off + i] = std::isfinite(wrapL_[off + i]) ? static_cast<float>(wrapL_[off + i]) : 0.0f;
                r[off + i] = std::isfinite(wrapR_[off + i]) ? static_cast<float>(wrapR_[off + i]) : 0.0f;
            }
        }
        if (fault_) {
            // El chunk que marcó fault (off) se restaura al original; los
            // chunks posteriores nunca se tocaron. Sin fault, off == n y
            // no hay nada que restaurar (el bucle de abajo no corre si
            // off >= n... salvo amount≈0: entonces ningún chunk renderizó
            // y el núcleo no tocó wrap: devolver el original es identidad).
            if (off < n) {
                const int m = std::min(kChunk, n - off);
                for (int i = 0; i < m; ++i) {
                    l[off + i] = origL_[off + i];
                    r[off + i] = origR_[off + i];
                }
            }
            return;
        }
        // Sin fault: los chunks no renderizados (amount≈0, bypass) se
        // devuelven tal cual (float→double→float es identidad exacta).
        for (; off < n; off += kChunk) {
            const int m = std::min(kChunk, n - off);
            for (int i = 0; i < m; ++i) {
                l[off + i] = std::isfinite(wrapL_[off + i]) ? static_cast<float>(wrapL_[off + i]) : 0.0f;
                r[off + i] = std::isfinite(wrapR_[off + i]) ? static_cast<float>(wrapR_[off + i]) : 0.0f;
            }
        }
    }

    // ── Telemetría Ruta A (v2.5.380): evidencia de que el operador procesa
    // de verdad en el teléfono. Contadores atómicos lock-free (relaxed):
    // el hilo de audio solo hace fetch_add (sin locks, sin log, sin alloc)
    // y el hilo de control los lee para el log con límite de frecuencia
    // (ivanna_omega_jni.cpp: logOmniTelemetryThrottled, max 1 línea / 5 s).
    //   processCalls_   = llamadas a process() que entraron al operador
    //                     (preparado, punteros válidos, n>0, sin fault
    //                     latcheado). Incluye llamadas con amount=0.
    //   blocksRendered_ = chunks completados con mezcla wet/dry (paso 9);
    //                     con amount≈0 o fault el chunk retorna antes y NO
    //                     cuenta. Es el contador que demuestra procesado real.
    //   framesRendered_ = muestras de esos chunks (m por chunk).
    // Se ponen a cero en reset() (prepare() lo llama). El static_assert
    // guarda el invariante "sin locks en el hilo de audio" en ARM64/x86_64.
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "OmniField: se necesitan atomicos de 64 bits lock-free");
    std::atomic<std::uint64_t> processCalls_{0};
    std::atomic<std::uint64_t> blocksRendered_{0};
    std::atomic<std::uint64_t> framesRendered_{0};
    std::uint64_t processCalls() const noexcept {
        return processCalls_.load(std::memory_order_relaxed);
    }
    std::uint64_t blocksRendered() const noexcept {
        return blocksRendered_.load(std::memory_order_relaxed);
    }
    std::uint64_t framesRendered() const noexcept {
        return framesRendered_.load(std::memory_order_relaxed);
    }
    bool isActive() const noexcept { return appliedAmount_ > 0.001f && !fault_; }
    bool faulted() const noexcept { return fault_; }
    float appliedAmount() const noexcept { return static_cast<float>(appliedAmount_); }
    uint32_t designCount() const noexcept { return designCount_; }
    uint32_t totalMaaUpdates() const noexcept {
        return static_cast<uint32_t>(personalizer_.totalUpdates());
    }
    float sceneAzimuthDeg() const noexcept { return scene_.azimuthDeg(); }
    float sceneDiffuseness() const noexcept { return scene_.diffuseness(); }
    // Telemetría de convergencia: parámetro de orden r y fase media ψ del
    // campo de Kuramoto (ver OmniFieldConvergence.hpp).
    float convergenceR() const noexcept { return kuramoto_.orderR(); }
    float convergencePhase() const noexcept { return kuramoto_.meanPhase(); }
    // Bloqueado si r ≥ 0.90 y las diferencias de fase son estables
    // (ver KuramotoField::isLocked; solo r no basta).
    bool convergenceLocked() const noexcept { return kuramoto_.isLocked(); }
    // Latencia conservadora: base del renderer (15) + FIR (≈16). ORBE ya
    // aporta 512 muestras; la de OMNI es despreciable en la cadena.
    int latencySamples() const noexcept { return 31; }

private:
    static constexpr float kDeg2Rad = 0.017453292519943295f;
    static constexpr float kRad2Deg = 57.29577951308232f;

    static float wrap180(float deg) noexcept {
        float x = std::fmod(deg + 180.0f, 360.0f);
        if (x < 0.0f) x += 360.0f;
        return x - 180.0f;
    }

    static double wrap180(double deg) noexcept {
        double x = std::fmod(deg + 180.0, 360.0);
        if (x < 0.0) x += 360.0;
        return x - 180.0;
    }

    // Sector MAA (0..7) para un azimut en grados: centros en 0,45,...,315.
    static int sectorOf(float azDeg) noexcept {
        const float w = wrap180(azDeg);
        int s = static_cast<int>(std::floor((w + 22.5f) / 45.0f));
        s %= 8;
        if (s < 0) s += 8;
        return s;
    }

    static float dbToLin(float db) noexcept {
        const float c = std::isfinite(db) ? std::clamp(db, -24.0f, 24.0f) : 0.0f;
        return std::pow(10.0f, c / 20.0f);
    }

    static double dbToLin(double db) noexcept {
        const double c = std::isfinite(db) ? std::clamp(db, -24.0, 24.0) : 0.0;
        return std::pow(10.0, c / 20.0);
    }

    void processChunk(double* l, double* r, int m) noexcept {
        // 1. Entrada finita o fault (passthrough: no se tocan los buffers).
        for (int i = 0; i < m; ++i) {
            if (!std::isfinite(l[i]) || !std::isfinite(r[i])) {
                fault_ = true;
                return;
            }
        }
        // 2. Parámetros saneados.
        const double target =
            std::isfinite(params_.amount) ? static_cast<double>(std::clamp(params_.amount, 0.0f, 1.0f)) : 0.0;
        const double pull = std::isfinite(params_.scenePull)
                               ? static_cast<double>(std::clamp(params_.scenePull, 0.0f, 1.0f))
                               : 0.3;
        const double distScale = std::isfinite(params_.distanceM)
                                    ? static_cast<double>(std::clamp(params_.distanceM, 0.5f, 2.0f)) / 1.2
                                    : 1.0;
        // 3. Rampa de enganche del amount (constante de tiempo 15 ms).
        const double alpha =
            1.0 - std::exp(-static_cast<double>(m) / (sampleRate_ * 0.015));
        appliedAmount_ += (target - appliedAmount_) * alpha;
        if (appliedAmount_ <= 0.0001f) return;  // bypass bit-exacto
        // 4. Escena + refresco MAA (diezmado por sector dentro de update).
        scene_.update(l, r, m);
        personalizer_.update(blockIndex_++);
        const double thetaScene = scene_.azimuthDeg();
        const double psi = scene_.diffuseness();
        // 4.5 CONVERGENCIA Omega–SAFMUS (un solo sistema dinámico:
        // escena → K → Kuramoto → r/ψ → fusión + tono emergente).
        // INTEGRATION POINT (refinamiento Omega–SAFMUS): la ley de
        // acoplamiento refinada se conecta AQUÍ, sustituyendo la línea
        // K = K0·(1−Ψ) (ver OmniFieldConvergence.hpp, § PUNTO DE
        // INTEGRACIÓN). El campo KuramotoField no cambia.
        double pullEff = pull;
        if (params_.convergence) {
            const float k = KuramotoField::kCoupling0 * static_cast<float>(1.0 - psi);
            kuramoto_.setCoupling(k);
            // Puerta de energía (fail-closed): RMS de entrada del bloque;
            // objetivo = rms²/(rms²+thr²), thr = 1e-3 (−60 dBFS). Con
            // entrada silenciosa la puerta es 0 y el tono emergente es
            // silencio: el operador jamás auto-oscila.
            double eIn = 0.0;
            for (int i = 0; i < m; ++i)
                eIn += l[i] * l[i] + r[i] * r[i];
            const double inRms = std::sqrt(eIn / static_cast<double>(2 * m));
            const double gateTgt =
                (inRms * inRms) / (inRms * inRms + 1e-6);
            energyGate_ += 0.25 * (gateTgt - energyGate_);
            // Avance por muestra + tono emergente e[n] = A4·r·sin(2ψ)·puerta
            // (2º armónico, 884 Hz con bloqueo; ver OmniFieldConvergence.hpp).
            const double rMean = kuramoto_.stepBlock(
                emergentMono_, m,
                static_cast<double>(KuramotoField::kEmergentGain) * energyGate_);
            // r suavizado POR BLOQUE (no por muestra, sin jitter).
            rBlock_ += 0.3 * (rMean - rBlock_);
            // Fusión: con r→1 los stems se atraen al centroide (campo
            // coherente); con r→0 se quedan en sus posiciones canónicas.
            pullEff = pull * (0.5 + 0.5 * rBlock_);
        }
        // 5. Separación en 4 stems (suma = mid por construcción).
        separator_.separate(l, r, m, stems_[0], stems_[1], stems_[2], stems_[3]);
        // 6. Cuota de energía por stem (para la atracción hacia la escena).
        double w[kNumStems];
        double wSum = 0.0;
        for (int k = 0; k < kNumStems; ++k) {
            double e = 0.0;
            for (int i = 0; i < m; ++i) e += stems_[k][i] * stems_[k][i];
            w[k] = std::sqrt(e / static_cast<double>(m));
            wSum += w[k];
        }
        const double wInv = (wSum > 1e-12) ? 1.0 / wSum : 0.0;
        // 7. Por stem: posición → personalización → (re)diseño FIR → render.
        for (int i = 0; i < m; ++i) {
            outL_[i] = 0.0;
            outR_[i] = 0.0;
        }
        const double yawDeg = params_.headTracking ? yawRad_ * static_cast<double>(kRad2Deg) : 0.0;
        for (int k = 0; k < kNumStems; ++k) {
            const double wk = w[k] * wInv;
            double theta = static_cast<double>(kStemAzDeg[k]) +
                          pullEff * (thetaScene - static_cast<double>(kStemAzDeg[k])) * wk * (1.0 - psi);
            const int sector = sectorOf(static_cast<float>(theta));
            const double warp = personalizer_.warpDeg(sector);
            const double gain = dbToLin(static_cast<double>(personalizer_.gainDb(sector)));
            const double thetaQ = wrap180(theta + warp - yawDeg);
            const double rk = static_cast<double>(kStemDistM[k]) * distScale;
            // Rediseño solo si la posición se movió más que el MAA del sector.
            const double thr = personalizer_.redesignThresholdDeg(sector);
            if (!firValid_[k] || std::fabs(thetaQ - lastTheta_[k]) > thr ||
                std::fabs(rk - lastR_[k]) > 0.05) {
                const float azR = static_cast<float>(thetaQ) * kDeg2Rad;
                const float elR = kStemElDeg[k] * kDeg2Rad;
                const float dir[3] = {std::sin(azR) * std::cos(elR),
                                      std::cos(azR) * std::cos(elR), std::sin(elR)};
                BinauralFir fir;
                if (designer_.design(dir, rk, fir)) {
                    firs_[k] = fir;
                    renderers_[k].setFir(fir);
                    firValid_[k] = true;
                    lastTheta_[k] = thetaQ;
                    lastR_[k] = rk;
                    ++designCount_;
                } else if (!firValid_[k]) {
                    // Sin FIR válido y el diseño falló: FIR plano (el stem
                    // suena centrado) en vez de silenciarse. Fail-closed.
                    BinauralFir flat;
                    for (int t = 0; t < BinauralFir::kTaps; ++t) {
                        flat.l[t] = 0.0;
                        flat.r[t] = 0.0;
                    }
                    flat.l[0] = 1.0;
                    flat.r[0] = 1.0;
                    flat.delayL = 0.0;
                    flat.delayR = 0.0;
                    firs_[k] = flat;
                    renderers_[k].setFir(flat);
                    firValid_[k] = true;
                    lastTheta_[k] = thetaQ;
                    lastR_[k] = rk;
                }
                // Diseño fallido con FIR previo válido: se conserva el anterior.
            }
            renderers_[k].processMono(stems_[k], tmpL_, tmpR_, m);
            for (int i = 0; i < m; ++i) {
                outL_[i] += tmpL_[i] * gain;
                outR_[i] += tmpR_[i] * gain;
            }
        }
        // 7.5 Tono emergente como 5º stem por el MISMO camino — LEGACY,
        // MUERTO en v2.5.488 (kEmergentToneEnabled=false): el tono a 884 Hz
        // es el "piiiii" (V1 2026-11-21). El bloque se conserva intacto
        // (no se borra) pero ya no se ejecuta; el campo de Kuramoto sigue
        // corriendo en el paso 4.5 (rBlock_/pullEff) y los 4 stems no se
        // tocan. Si se re-evalúa, basta poner kEmergentToneEnabled=true.
        // StemRenderer/FIR que los demás (miembro separado, no se extienden
        // los arrays de 4 stems). FIR en el azimut de escena (elevación 0,
        // distancia = distanceM), rediseñado cuando Θ se mueve más que el
        // umbral MAA del sector. Pasa por el mismo level-match y los mismos
        // checks de fault del paso 8 (se suma a outL/outR antes de ellos).
        if (kEmergentToneEnabled && params_.convergence && energyGate_ > 1e-4) {
            const double thetaE = thetaScene;
            const int sectorE = sectorOf(static_cast<float>(thetaE));
            const double warpE = personalizer_.warpDeg(sectorE);
            const double gainE = dbToLin(static_cast<double>(personalizer_.gainDb(sectorE)));
            const double thetaQE = wrap180(thetaE + warpE - yawDeg);
            const double rE = std::isfinite(params_.distanceM)
                                 ? static_cast<double>(std::clamp(params_.distanceM, 0.5f, 2.0f))
                                 : 1.2;
            const double thrE = personalizer_.redesignThresholdDeg(sectorE);
            if (!emergentFirValid_ || std::fabs(thetaQE - emergentLastTheta_) > thrE ||
                std::fabs(rE - emergentLastR_) > 0.05) {
                const float azR = static_cast<float>(thetaQE) * kDeg2Rad;
                const float dir[3] = {std::sin(azR), std::cos(azR), 0.0f};
                BinauralFir fir;
                if (designer_.design(dir, rE, fir)) {
                    emergentFir_ = fir;
                    emergentRenderer_.setFir(fir);
                    emergentFirValid_ = true;
                    emergentLastTheta_ = thetaQE;
                    emergentLastR_ = rE;
                    ++designCount_;
                } else if (!emergentFirValid_) {
                    BinauralFir flat;
                    for (int t = 0; t < BinauralFir::kTaps; ++t) {
                        flat.l[t] = 0.0;
                        flat.r[t] = 0.0;
                    }
                    flat.l[0] = 1.0;
                    flat.r[0] = 1.0;
                    flat.delayL = 0.0;
                    flat.delayR = 0.0;
                    emergentFir_ = flat;
                    emergentRenderer_.setFir(flat);
                    emergentFirValid_ = true;
                    emergentLastTheta_ = thetaQE;
                    emergentLastR_ = rE;
                }
            }
            emergentRenderer_.processMono(emergentMono_, tmpL_, tmpR_, m);
            for (int i = 0; i < m; ++i) {
                outL_[i] += tmpL_[i] * gainE;
                outR_[i] += tmpR_[i] * gainE;
            }
        }
        // 8. Nivel: match suave y acotado hacia la energía de entrada, y
        // fault si el renderer produce (casi) silencio con entrada presente.
        double inE = 0.0, renE = 0.0;
        for (int i = 0; i < m; ++i) {
            inE += l[i] * l[i] + r[i] * r[i];
            renE += outL_[i] * outL_[i] + outR_[i] * outR_[i];
        }
        inE /= static_cast<double>(2 * m);
        renE /= static_cast<double>(2 * m);
        if (inE > 1e-8 && renE < inE * 1e-6) {
            fault_ = true;
            return;  // no se tocan l/r: bypass
        }
        if (inE > 1e-12 && renE > 1e-12) {
            double tgt = std::sqrt(inE / renE);
            tgt = std::clamp(tgt, 0.5, 2.0);
            levelGain_ += (tgt - levelGain_) * 0.05;
        }
        // 9. Mezcla wet/dry (el dry es la entrada original, aún intacta).
        const double a = appliedAmount_;
        const double g = levelGain_;
        for (int i = 0; i < m; ++i) {
            l[i] = (1.0 - a) * l[i] + a * outL_[i] * g;
            r[i] = (1.0 - a) * r[i] + a * outR_[i] * g;
        }
        // Chunk renderizado de verdad: cuenta para la telemetría Ruta A.
        blocksRendered_.fetch_add(1, std::memory_order_relaxed);
        framesRendered_.fetch_add(static_cast<std::uint64_t>(m),
                                 std::memory_order_relaxed);
    }

    double sampleRate_ = 48000.0;
    bool prepared_ = false;
    bool hasProcessed_ = false;
    OmniParams params_;
    double appliedAmount_ = 0.0;
    double yawRad_ = 0.0;
    bool fault_ = false;
    uint32_t designCount_ = 0;
    int blockIndex_ = 0;
    double levelGain_ = 1.0;

    StemSeparator separator_;
    SceneEstimator scene_;
    MaaPersonalizer personalizer_;
    FirDesigner designer_;
    KuramotoField kuramoto_;
    StemRenderer renderers_[kNumStems];
    StemRenderer emergentRenderer_;  // 5º stem: tono emergente (r·sin ψ)
    BinauralFir emergentFir_;
    bool emergentFirValid_ = false;
    double emergentLastTheta_ = 0.0;
    double emergentLastR_ = 0.0;
    double rBlock_ = 0.0;    // r suavizado por bloque (fusión)
    double energyGate_ = 0.0;  // puerta suave de energía de entrada
    BinauralFir firs_[kNumStems];
    bool firValid_[kNumStems] = {};
    double lastTheta_[kNumStems] = {};
    double lastR_[kNumStems] = {};

    double stems_[kNumStems][kChunk];
    double emergentMono_[kChunk];  // tono emergente por muestra (5º stem)
    double outL_[kChunk];
    double outR_[kChunk];
    double tmpL_[kChunk];
    double tmpR_[kChunk];
    // Scratch del envoltorio float de process() (crece bajo demanda).
    std::vector<double> wrapL_{}, wrapR_{};
    // Copia del original para restaurar el chunk que marque fault.
    std::vector<float> origL_{}, origR_{};
};

}  // namespace omnifield
}  // namespace ivanna
