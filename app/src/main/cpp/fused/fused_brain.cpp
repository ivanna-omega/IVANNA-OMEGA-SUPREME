// fused_brain.cpp — OPERADOR #7: CEREBRO NEURAL (Fusión Magistral).
// Ver fused_brain.hpp para el mapa análisis vs actuación y los caveats
// de host (bucle cerrado sin sensor, TinyML stub en host, memoria del
// agente fail-closed).
#include "fused/fused_brain.hpp"

#include <algorithm>

namespace ivanna::fused {

void FusedBrain::prepare(double sampleRate) noexcept {
    const double sr = (std::isfinite(sampleRate) && sampleRate >= 8000.0 &&
                       sampleRate <= 192000.0)
                          ? sampleRate
                          : 48000.0;
    sampleRate_ = sr;
    const float srf = static_cast<float>(sr);

    // Análisis: el clasificador, la prosodia, TinyML y el kernel no
    // tienen prepare() (se inicializan en su constructor).
    predictive_.prepare(srf);
    oracle_.init(srf);

    // Actuación.
    loop_.prepare(srf, kBrainChunkFrames);
    bandEq_.prepare(srf);
    npe_.prepare(srf);
    npe_.setParams(false, kBrainNpeHarmonic, kBrainNpeLateral, kBrainNpeOhc,
                   kBrainNpeMasterDb);

    // Memoria del super-agente, misma ruta que el FusionEngine. En host
    // /data no existe → open() falla → false, sin crear archivos y sin
    // crashear; updateContext() sigue alimentando el estado local.
    agentMemoryReady_ = agentMemory_.initialize(kBrainAgentMemoryPath);

    reset();
    applyGuardianIntensity();
    prepared_ = true;
}

void FusedBrain::setControls(const BrainControls& c) noexcept {
    BrainControls clamped = c;
    clamped.clamp();
    const bool wasNeutral = controls_.isNeutral();
    controls_ = clamped;
    // FALLO 4 (2026-11-18): al entrar en neutro el bypass YA NO es un
    // reset() duro inmediato — eso ponía el fade del NPE a 0 de golpe y
    // vaciaba la NeuroBandEq, cortando la cola y produciendo un clic.
    // En su lugar se arma el fundido suave de salida (20 ms, el propio
    // del NPE): setParams(false, ...) deja que fade_ baje a 0 en
    // process() y la NeuroBandEq se desliza a 0 dB con ese mismo fade
    // (ver runActuationChunk); el reset() llega diferido cuando el
    // fade termina (ver process()), ya inaudible.
    if (!wasNeutral && controls_.isNeutral()) {
        npe_.setParams(false, kBrainNpeHarmonic, kBrainNpeLateral,
                       kBrainNpeOhc, kBrainNpeMasterDb);
    }
    // El guardián sigue observando en MANUAL, pero con intensidad 0
    // (no modula a los otros operadores); en AUTO, con adaptation.
    applyGuardianIntensity();
}

void FusedBrain::reset() noexcept {
    predictive_.reset();
    oracle_.reset();
    loop_.reset();
    bandEq_.reset();
    npe_.reset();
    antiDolby_.reset();  // no toca la intensidad (la gobierna setControls)
    dominantGenre_ = 0;
    confidence_ = 0.0f;
    predictiveConfidence_ = 0.0f;
    // Sin reset(): IvannaAudioClassifier, IvannaVoiceProsodyEngine,
    // TinyMLAudioEngine, IvannaTinyMLAudioKernel, IvannaSelfHealingEngine
    // e IvannaSuperAgentMemory no exponen API de reset; su estado es
    // solo-observación y no afecta a la señal.
}

void FusedBrain::runAnalysisChunk(const float* l, const float* r, int n) noexcept {
    if (l == nullptr || r == nullptr || n <= 0) return;

    // Clasificador de género (NPE): ingesta wait-free; la inferencia
    // corre en su propio hilo y publica por triple buffer.
    classifier_.ingestAudioFrame(l, r, static_cast<std::size_t>(n));

    // Prosodia (F0 / VAD / tilt).
    prosody_.analyzeAudio(l, r, static_cast<std::size_t>(n));

    // TinyML: IngestAudio espera buffer entrelazado (frames × canales).
    for (int i = 0; i < n; ++i) {
        scratchInterleaved_[2 * i] = l[i];
        scratchInterleaved_[2 * i + 1] = r[i];
    }
    tinyMl_.IngestAudio(scratchInterleaved_, n, 2);

    // TinyMLKernel: inferencia INT8 real por trozo (pesos cero en host).
    tinyMlKernel_.ingestPcmBlock(l, r, static_cast<std::size_t>(n));

    // Predictivo LPC (solo análisis: no modifica el audio).
    predictive_.processStereo(l, r, n);

    // Phase Oracle (Kalman): seguimiento de fase sobre el mono.
    for (int i = 0; i < n; ++i) {
        oracle_.tick(0.5f * (l[i] + r[i]));
    }

    // Self-Healing: solo monitor — heartbeat por trozo. El hilo de
    // monitorización NO se arranca desde el camino de audio.
    selfHealing_.pingAudioEngine();

    // Telemetría del clasificador (0 si aún no hay salida válida).
    Ivanna::AIModelOutput modelOut{};
    if (classifier_.getModelOutput(modelOut) && modelOut.is_valid) {
        dominantGenre_ = static_cast<int>(modelOut.dominant_class);
        confidence_ = modelOut.confidence;
        // Guardián AntiDolby: se alimenta de la clasificación cada vez
        // que hay una (en MANUAL también: observa, no modula).
        antiDolby_.updateFromNeuralContext(
            static_cast<std::uint8_t>(dominantGenre_), confidence_,
            modelOut.scene_energy);
    }
    predictiveConfidence_ = predictive_.frame().confidence;

    // Suavizado attack/release del guardián, por trozo
    // (dt = duración del trozo en segundos).
    antiDolby_.tick(static_cast<float>(n) / static_cast<float>(sampleRate_));

    // Memoria del super-agente: contexto de largo plazo (clase, pitch,
    // loudness del trozo). No toca disco aquí (commitToDisk no se llama
    // en el camino caliente); sin mmap funciona solo en local.
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double m = 0.5 * (static_cast<double>(l[i]) + static_cast<double>(r[i]));
        sumSq += m * m;
    }
    const double rms = std::sqrt(sumSq / static_cast<double>(n));
    const float loudnessDb =
        static_cast<float>(20.0 * std::log10(rms + 1e-9));
    agentMemory_.updateContext(static_cast<std::uint8_t>(dominantGenre_),
                               prosody_.getMetrics().pitchFreq, loudnessDb);
}

void FusedBrain::runActuationChunk(double* l, double* r, int n) noexcept {
    if (l == nullptr || r == nullptr || n <= 0) return;

    // Bucle cerrado: compute(0) usa el contador interno de bloques
    // (reloj monotónico propio, sin depender del reloj del sistema).
    // Sin frames de sensor frescos devuelve identidad (fail-closed) y
    // la NeuroBandEq queda en bypass bit-exacto.
    const ivanna::neuromorphic::NeuroAdjustments adj = loop_.compute(0);
    std::array<float, ivanna::neuromorphic::kNeuroBands> gains = adj.gainDb;
    // FALLO 4 (2026-11-18): en neutro (cola de fundido a neutro) la
    // ganancia de actuación se escala por el fade vigente del NPE
    // (1→0 en 20 ms) en vez de caer a 0 de golpe — adaptation ya es 0
    // en neutro — así la NeuroBandEq se desliza a 0 dB y sus estados
    // decaen en vez de cortarse. En AUTO se usa adaptation, como antes.
    // F2/486: el fade del NPE ya es double; adaptation (float, control)
    // se promueve a double y las ganancias se re-escalan en float.
    const double actGain = controls_.isNeutral()
                               ? npe_.fade()
                               : static_cast<double>(controls_.adaptation);
    for (float& g : gains) g *= static_cast<float>(actGain);
    bandEq_.setGains(gains, static_cast<float>(sampleRate_));
    bandEq_.process(l, r, n);

    // NPE: parámetros del FusionEngine. En neutro se mantiene
    // deshabilitado para que su fade interno siga bajando a 0 (si se
    // pusiera true aquí, el fundido de salida no terminaría nunca);
    // en AUTO, habilitado como antes.
    npe_.setParams(!controls_.isNeutral(), kBrainNpeHarmonic,
                   kBrainNpeLateral, kBrainNpeOhc, kBrainNpeMasterDb);
    npe_.process(l, r, n);
}

void FusedBrain::process(double* left, double* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0) return;
    if (!prepared_) prepare(48000.0);

    const bool neutral = isNeutral();
    int done = 0;
    while (done < frames) {
        // Rebanada externa: como mucho kFusedMaxBlockFrames en scratch.
        const int slice = std::min(kFusedMaxBlockFrames, frames - done);

        // double -> double con saneo (la entrada no finita llega a los
        // motores como 0.0; el buffer original NO se toca en neutro).
        for (int i = 0; i < slice; ++i) {
            const double xl = left[done + i];
            const double xr = right[done + i];
            scratchL_[i] = std::isfinite(xl) ? xl : 0.0;
            scratchR_[i] = std::isfinite(xr) ? xr : 0.0;
        }

        // Trozos internos de kBrainChunkFrames (128 == Ivanna::BLOCK_SIZE).
        for (int off = 0; off < slice; off += kBrainChunkFrames) {
            const int n = std::min(kBrainChunkFrames, slice - off);
            double* l = scratchL_ + off;
            double* r = scratchR_ + off;

            // Análisis en float (clasificadores/TinyML son control):
            // copia del trozo; la señal double no se toca.
            for (int i = 0; i < n; ++i) {
                anaL_[i] = static_cast<float>(l[i]);
                anaR_[i] = static_cast<float>(r[i]);
            }
            runAnalysisChunk(anaL_, anaR_, n);

            // FALLO 4 (2026-11-18): en neutro se sigue observando, pero
            // la actuación NO se salta mientras quede cola de fundido
            // (npe_.isActive(): fade_ > 0): esos trozos corren la
            // actuación en fundido para que el NPE complete sus 20 ms.
            // Solo cuando el fade llega a 0 el reset diferido deja NPE
            // + NeuroBandEq limpios para la próxima activación — ya
            // inaudible, porque la ruta es bit-exacta en ese punto.
            if (neutral) {
                if (!npe_.isActive()) {
                    npe_.reset();
                    bandEq_.reset();
                    continue;  // solo observa: sin tocar el buffer
                }
            }

            runActuationChunk(l, r, n);

            // double con saneo fail-closed (no finito -> 0.0).
            for (int i = 0; i < n; ++i) {
                const double yl = l[i];
                const double yr = r[i];
                left[done + off + i] = std::isfinite(yl) ? yl : 0.0;
                right[done + off + i] = std::isfinite(yr) ? yr : 0.0;
            }
        }
        done += slice;
    }
}

}  // namespace ivanna::fused
