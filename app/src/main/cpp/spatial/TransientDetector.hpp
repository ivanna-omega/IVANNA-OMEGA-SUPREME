/*
 * ============================================================================
 * IVANNA-OMEGA-SUPREME — Motor de Audio Holográfico de Bajo Nivel
 * ============================================================================
 * Autoría Exclusiva y Propiedad Absoluta:
 * Luis Uriel Pimentel Pérez (alias Gore TNS)
 *
 * Todos los modelos matemáticos, arquitecturas de sistema e implementaciones
 * de código contenidos en este archivo son propiedad intelectual exclusiva
 * del autor citado. Queda estrictamente prohibida la reproducción, distribución,
 * modificación o uso comercial no autorizado.
 *
 * Este software NO se distribuye bajo licencia CC0 ni dominio público.
 * Todos los derechos reservados. © 2026 Luis Uriel Pimentel Pérez.
 * ============================================================================
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ivanna {

// ============================================================================
// TransientDetector — detector de transientes REAL por comparación de dos
// seguidores de envolvente (rápido vs lento), el método clásico y verificable.
//
// POR QUÉ ESTE MÓDULO EXISTE Y POR QUÉ ESTÁ SEPARADO
// --------------------------------------------------
// El encargo "HOA + Binaural + Upmixing" pedía reusar «el clasificador CRNN
// que ya distingue centro/lados/bajos/transientes cada 50 ms». Ese CRNN NO
// existe en el repositorio (verificado; ver AGENT_CLAIMS.md). El clasificador
// real del proyecto (IvannaAudioClassifier) devuelve ESCENA completa
// (AudioContextClass + confianza + energía), no separación de fuentes y no
// marca transientes por bloque. Así que la detección de transientes se
// construye aquí desde cero, con procesamiento de señal clásico, y vive en su
// propio módulo con sus propios tests — NO escondida dentro del upmixer.
//
// ESTO NO ES IA. Es una comparación de envolventes. Documentado así a
// propósito para no maquillar un detector determinista como "inteligencia".
//
// MÉTODO
// ------
//   env_fast[n] = max(|x[n]|, env_fast[n-1] * a_fast)   (ataque instantáneo)
//   env_slow[n] = env_slow[n-1] + (|x[n]| - env_slow[n-1]) * a_slow
//   ratio       = env_fast / max(env_slow, piso)
// Un transiente es un salto de energía muy por encima de la media reciente:
// ratio > threshold. El piso evita divisiones por ~0 en silencio (donde
// cualquier ruido de bit bajo daría un ratio infinito y un falso positivo).
//
// Sin asignación de memoria, sin bloqueo, sin estado compartido: apto para el
// hilo de audio.
// ============================================================================

// F2/486 (labor grande, double interno): las envolventes y los
// coeficientes viven en double; processBlock(double) es el núcleo y
// processBlock(float) delega en él. La salida es un booleano (plano
// de CONTROL: hit sí/no), pero la decisión se toma con la precisión
// del plano de señal que la alimenta.
class TransientDetector {
public:
    /**
     * @param sampleRate  Hz. Valores no finitos o <= 0 caen al default 48 kHz
     *                    en vez de producir coeficientes NaN que envenenarían
     *                    la envolvente para siempre.
     */
    void prepare(float sampleRate) noexcept {
        const double sr = (std::isfinite(sampleRate) && sampleRate > 0.0f) ? static_cast<double>(sampleRate) : 48000.0;
        sampleRate_ = sr;
        // Release del seguidor rápido ~5 ms, constante del lento ~150 ms:
        // suficiente separación para que un golpe percusivo dispare sin que
        // una nota sostenida (que sube en ambos) lo haga.
        releaseFast_ = std::exp(-1.0 / (0.005 * sr));
        alphaSlow_   = 1.0 - std::exp(-1.0 / (0.150 * sr));
        reset();
    }

    void reset() noexcept {
        envFast_ = 0.0;
        envSlow_ = 0.0;
        lastRatio_ = 0.0;
    }

    /** Umbral de ratio rápido/lento a partir del cual se declara transiente. */
    void setThreshold(float ratio) noexcept {
        if (std::isfinite(ratio) && ratio > 1.0f) threshold_ = static_cast<double>(ratio);
    }

    float getThreshold() const noexcept { return static_cast<float>(threshold_); }

    /**
     * Procesa un bloque y devuelve true si contiene al menos un transiente.
     * El estado de las envolventes persiste entre bloques (un golpe a caballo
     * entre dos bloques se detecta igual).
     */
    bool processBlock(const double* x, std::size_t n) noexcept {
        if (x == nullptr || n == 0) return false;
        bool hit = false;
        double maxRatio = 0.0;
        for (std::size_t i = 0; i < n; ++i) {
            const double s = x[i];
            // Una muestra NaN/Inf de una etapa anterior contaminaría ambas
            // envolventes de forma permanente: se ignora, no se propaga.
            if (!std::isfinite(s)) continue;
            const double a = std::fabs(s);

            envFast_ = (a > envFast_) ? a : envFast_ * releaseFast_;
            envSlow_ += (a - envSlow_) * alphaSlow_;

            const double denom = std::max(envSlow_, kFloor);
            const double ratio = envFast_ / denom;
            if (ratio > maxRatio) maxRatio = ratio;
            // Sólo cuenta como transiente si además hay señal audible: en
            // silencio digital el ratio contra el piso no significa nada.
            if (ratio > threshold_ && envFast_ > kAudibleFloor) hit = true;
        }
        lastRatio_ = maxRatio;
        return hit;
    }

    // Envoltorio float (compat legacy): convierte la entrada y delega
    // en el núcleo double; el estado es compartido (double).
    bool processBlock(const float* x, std::size_t n) noexcept {
        if (x == nullptr || n == 0) return false;
        if (wrap_.size() < n) wrap_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
            wrap_[i] = std::isfinite(x[i]) ? static_cast<double>(x[i]) : 0.0;
        return processBlock(wrap_.data(), n);
    }

    /** Último ratio máximo observado — para telemetría/tests, no para decidir. */
    float lastRatio() const noexcept { return static_cast<float>(lastRatio_); }

    float envelopeFast() const noexcept { return static_cast<float>(envFast_); }
    float envelopeSlow() const noexcept { return static_cast<float>(envSlow_); }

private:
    // Piso del denominador: -120 dBFS. Debajo de eso la "media reciente" no
    // es información, es ruido de cuantización.
    static constexpr double kFloor        = 1e-6;
    // Nivel mínimo del pico para considerarlo audible: -80 dBFS.
    static constexpr double kAudibleFloor = 1e-4;

    double sampleRate_  = 48000.0;
    double releaseFast_ = 0.0;
    double alphaSlow_   = 0.0;
    double envFast_     = 0.0;
    double envSlow_     = 0.0;
    double lastRatio_   = 0.0;
    double threshold_   = 3.0;
    // Scratch del envoltorio float (crece bajo demanda, se reutiliza).
    std::vector<double> wrap_{};
};

}  // namespace ivanna
