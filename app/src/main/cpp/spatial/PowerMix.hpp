// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// PowerMix — mezcla seco/wet de potencia constante con tratamiento
// explícito de correlación seco/wet (P3/3.1, 2026-11-26).
//
// La mezcla lineal (1−w)·dry + w·wet es FALSA como "potencia constante":
// con seco/wet incorrelados de igual potencia pierde −3 dB en w = 0.5
// (medido: HOLOS −2.5…−4.1 dB en contenido coherente, ORBE −2.05 dB
// defecto M1 en el render — aquí se trata la MEZCLA, no el render).
// La mezcla de SALA VIVA ya era de potencia constante
// (fused_listener_room.cpp: gDry² + gWet² = 1); esta es su equivalente
// con correlación explícita para los fundidos de ORBE/HOLOS.
//
// Para w ∈ [0,1], θ = w·π/2, c = cos θ, s = sin θ:
//   out = scale · (c·dry + s·wet)
//   scale² = P_ref / (P_ref + 2·c·s·C),  P_ref = c²·Pd + s²·Pw
// donde Pd, Pw son las potencias (L+R) de seco/wet y C = E[dry·wet]
// (L+R), estimadas con EMA τ ≈ 5 ms. La referencia P_ref INTERPOLA
// de Pd (w = 0) a Pw (w = 1): la escala solo corrige el término de
// correlación, nunca la diferencia de potencia seco/wet. Así:
//   - ρ = 0 (incorrelado): scale = 1, potencia constante clásica
//     (la lineal perdía −3 dB en w = 0.5).
//   - ρ = +1 (coherente en fase): scale = 1/√(1+sin 2θ); en w = 0.5 la
//     mezcla es (dry+wet)/2 como la lineal (0 dB si wet = dry).
//   - ρ = −1 (antifase): D → 0; scale se acota a 2 (sin blow-up) y la
//     salida sigue siendo ≈0 (cancelación física: ninguna ganancia
//     finita la recupera).
//   - Un lado en silencio (transitorio, priming): scale = 1, fundido
//     puro — el mixer NO refuerza el lado que suena (la referencia
//     anterior, Pd fija, daba scale = 1/c hasta 2.0 y derrotaba el
//     fundido: impulso vivo 0.5 → 0.466, centroide de latencia 274).
//   - w = 0: scale = 1 exacto (passthrough bit-exacto); w = 1: scale = 1
//     exacto (el wet sale con SU potencia: el mixer no compensa la
//     ganancia del render — limiter, conserver — ni re-escala el wet
//     contra el seco de hace 10.7 ms en el blend).
// La normalización es CONJUNTA (L+R): un solo escalar para ambos canales,
// así que el balance L/R (ILD) del wet se preserva exactamente muestra a
// muestra. El estado (EMAs) vive por instancia de mezcla: una por fundido
// (blend live/seco-retardado y wet seco/wet), nunca compartido entre
// operadores. Sin alloc, sin locks; la EMA se actualiza por muestra
// drenada (el fundido de 15 ms converge en ~5 ms).
//
// REGLA DEL BLEND (alineación de retardo): el fundido blend mezcla el
// vivo con el seco RETARDADO 512 muestras (dryQ). En régimen (w = 1) la
// salida debe ser dryQ EXACTO, sin reescalar: el mixer compararía la
// potencia actual con la de hace 10.7 ms y bombearía en transitorios
// (pre-eco de hasta +6 dB antes de cada ataque). Por eso el llamador usa
// extremos exactos en el blend (w = 0 → vivo, w = 1 → dryQ) y el mixer
// solo en 0 < w < 1. En el fundido WET no hay problema: seco y wet llevan
// el MISMO retardo (512), sus potencias caminan juntas, así que el mixer
// corre en todo w ∈ [0,1] y su escala solo corrige la correlación.
//
// CAMBIO DE SONORIDAD (P0): el nivel del wet en régimen permanente cambia
// respecto a la 504 cuando amount ∈ (0,1) y seco/wet no son coherentes
// (p. ej. HOLOS amount 0.5 con ruido incorrelado: −3 dB lineal → 0 dB
// ahora). El re-freeze de P0 exige luz verde de Esse (NO hecho aquí).
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <algorithm>
#include <cmath>

namespace ivanna {
namespace spatial {

// Mezcla seco/wet de potencia constante con correlación explícita.
class PowerMixer {
public:
    // τ de la EMA de potencia (5 ms: converge dentro del fundido de 15 ms
    // y no modula el audio — una potencia instantánea por muestra daría AM).
    static constexpr double kTauSeconds = 0.005;
    // τ del decaimiento del cap de pico (50 ms): el cap es un max-hold y
    // con τ = 5 ms caía un 18 % entre picos de un tono de 1 kHz (48
    // muestras), así que el recorte de escala mordía de más y costaba
    // ~0.05 dB en el fundido. Con 50 ms la caída entre picos es ~2 % y
    // el recorte solo actúa donde la mezcla de verdad superaría el pico
    // de entrada; el cap sigue cayendo en ~50 ms si la entrada se hace
    // más baja (la guarda sigue siendo significativa ante cambios de
    // nivel).
    static constexpr double kCapTauSeconds = 0.05;
    // τ del decaimiento del max-hold de la mezcla sin escalar (1 ms):
    // sigue la envolvente ACTUAL de la mezcla mientras w barre el
    // fundido (15 ms). Con τ = 5 ms recordaba el pico de w anteriores
    // (al inicio del fundido la mezcla sin escalar es ~el seco, pico A)
    // y capaba la escala de correlación (1.44 en mid-fade) durante todo
    // el fundido — costaba 0.12–0.17 dB. Con 1 ms el max-hold olvida en
    // ~1 ms y el cap de escala es el del instante actual: en el fundido
    // estable no muerde (la guarda cuesta ~0.001 dB) y en el fundido IN
    // (EMAs frías por el flush + wet primando) sí capa la escala cuando
    // la correlación estimada aún no es la real, como la guarda 3.1.
    static constexpr double kMixTauSeconds = 0.001;
    // Autoridad de la normalización: ±6 dB (nunca más, ni en antifase).
    static constexpr double kScaleMin = 0.5;
    static constexpr double kScaleMax = 2.0;

    void prepare(double sampleRate) noexcept {
        const double sr = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 48000.0;
        alpha_ = 1.0 - std::exp(-1.0 / (kTauSeconds * sr));
        capDecay_ = 1.0 - std::exp(-1.0 / (kCapTauSeconds * sr));
        mixDecay_ = 1.0 - std::exp(-1.0 / (kMixTauSeconds * sr));
        reset();
    }

    void reset() noexcept {
        pDry_ = 0.0;
        pWet_ = 0.0;
        cross_ = 0.0;
        peakMix_ = 0.0;
        peakCap_ = 0.0;
    }

    // Observa una muestra estéreo (actualiza las EMAs). Se llama por
    // muestra drenada, INCLUSO cuando la mezcla está en un extremo exacto
    // (w = 0 o 1), para que las EMAs estén calientes cuando empiece un
    // fundido. Sin alloc.
    template <typename Scalar>
    void observe(Scalar dryL, Scalar dryR, Scalar wetL, Scalar wetR) noexcept {
        const double dL = static_cast<double>(dryL);
        const double dR = static_cast<double>(dryR);
        const double wL = static_cast<double>(wetL);
        const double wR = static_cast<double>(wetR);
        pDry_ += alpha_ * ((dL * dL + dR * dR) - pDry_);
        pWet_ += alpha_ * ((wL * wL + wR * wR) - pWet_);
        cross_ += alpha_ * ((dL * wL + dR * wR) - cross_);
        if (!std::isfinite(pDry_) || !std::isfinite(pWet_) || !std::isfinite(cross_))
            reset();
    }

    // Mezcla una muestra estéreo con las EMAs actuales (NO las actualiza:
    // llamar a observe() antes, una vez por muestra). w ∈ [0,1].
    template <typename Scalar>
    void mix(Scalar& outL, Scalar& outR, Scalar dryL, Scalar dryR,
             Scalar wetL, Scalar wetR, double w) noexcept {
        const double dL = static_cast<double>(dryL);
        const double dR = static_cast<double>(dryR);
        const double wL = static_cast<double>(wetL);
        const double wR = static_cast<double>(wetR);
        const double ww = std::clamp(w, 0.0, 1.0);
        const double th = ww * 1.57079632679489661923;  // w·π/2
        const double c = std::cos(th), s = std::sin(th);
        // Guarda de pico (3.2): cap = max-hold del mayor pico de entrada,
        // actualizado en CADA muestra (también con el seco en silencio,
        // para que el recorte de escala tenga cap válido desde la primera
        // muestra). Ver el bloque de escala para el mecanismo completo.
        {
            const double cPeak = std::max(
                std::max(std::fabs(dL), std::fabs(wL)),
                std::max(std::fabs(dR), std::fabs(wR)));
            peakCap_ = std::max(cPeak, peakCap_ * (1.0 - capDecay_));
        }
        double scale = 1.0;
        if (std::isfinite(pDry_) && std::isfinite(pWet_) && std::isfinite(cross_) &&
            pDry_ > 1e-12) {
            // Referencia de potencia (3.1): la potencia objetivo
            // INTERPOLA de Pd (w = 0) a Pw (w = 1):
            //   P_ref = c²·Pd + s²·Pw
            // y la escala solo corrige el término de correlación:
            //   scale² = P_ref / (P_ref + 2·c·s·C).
            // La referencia anterior (Pd fija) era un defecto: cuando
            // un lado está en silencio (transitorio, priming: el wet
            // aún no llegó y Pw ≈ 0, C ≈ 0) daba scale = 1/c, hasta
            // el tope 2.0, y reforzaba el seco hasta casi amplitud
            // completa — el fundido quedaba derrotado (impulso vivo
            // de 0.5 salía a 0.466 en vez de 0.052 lineal; centroide
            // de latencia 274 en vez de 512, test LatencyIsFftSize).
            // Con la interpolación, un lado silencioso da scale = 1
            // (fundido de potencia constante puro) y en w = 1 scale = 1
            // por construcción: el mixer NO compensa la ganancia del
            // render (limiter, conserver) ni re-escala el wet contra
            // el seco — un tono hot limitado a 0.98855 ya no se
            // re-escala a 1.269. La pérdida de potencia del render
            // (p. ej. HOLOS −2.5 dB en tono coherente) es del render,
            // no de la mezcla, y se mide en el test 3.3.
            const double refPow = c * c * pDry_ + s * s * pWet_;
            const double denom = refPow + 2.0 * c * s * cross_;
            if (denom > 1e-12 && refPow > 1e-12) {
                scale = std::sqrt(refPow / denom);
                scale = std::clamp(scale, kScaleMin, kScaleMax);
            }
            // En w = 0 la escala es 1.0 exacta (denom = refPow = Pd):
            // el passthrough no se toca (bit-exacto). En w = 1 también
            // es 1.0 (denom = refPow = Pw): el wet sale exacto.
            // Guarda de pico en ventana (3.2): el mixer nunca crea un
            // pico por encima del mayor pico de sus entradas (contrato
            // de la mezcla lineal). Es un RECORTE DE ESCALA con max-hold
            // RÁPIDO (τ = 1 ms) de la mezcla sin escalar: capa la escala
            // a cap/pico de la mezcla del instante actual. La guarda 3.1
            // usaba τ = 5 ms y recordaba el pico de w anteriores (al
            // inicio del fundido la mezcla sin escalar es ~el seco,
            // pico A) y capaba la escala de correlación (1.44 en
            // mid-fade) durante todo el fundido — costaba 0.12–0.17 dB
            // de potencia en el fundido de tonos (medido con la sonda,
            // delays 128–1024; ver test_power_mix.cpp, nota del suelo
            // de medición). Con τ = 1 ms el max-hold sigue la envolvente
            // actual y en el fundido estable no muerde (la guarda cuesta
            // ~0.001 dB frente a no tener guarda); en el fundido IN
            // (EMAs frías por el flush + wet primando: la correlación
            // estimada aún no es la real y la escala sale demasiado
            // grande) sí capa la escala y la potencia del fundido queda
            // controlada, como con la 3.1. El ataque es instantáneo
            // (max) y la garantía es por muestra: peakMix_ ≥ |mezcla|
            // actual implica |salida| ≤ peakCap_.
            // NO hay clamp de salida (se probó y se quitó): clavar la
            // salida a ±peakCap_ aplana los picos y la onda se vuelve
            // cuadrada — mismo pico, MÁS potencia (fundido IN del
            // operador: +0.87 dB con pico 0.514, y ORBE +0.73 dB con
            // pico exactamente 0.5000). El recorte de escala preserva
            // la forma de onda (ganancia conjunta L+R por muestra) y
            // controla pico Y potencia a la vez; el clamp solo controla
            // el pico y rompe la potencia. La red de seguridad del
            // transitorio frío es el propio recorte: pDry_ supera 1e-12
            // en la primera muestra con señal y w arranca en ~0 (el
            // fundido acaba de empezar), así que la mezcla sin escalar
            // no puede superar el cap más de ~0.14 % antes de que el
            // recorte enganche. Una sola escala conjunta L+R: el ILD se
            // conserva (el cap es el mayor pico de AMBOS canales).
            {
                const double mPeak = std::max(std::fabs(c * dL + s * wL),
                                              std::fabs(c * dR + s * wR));
                peakMix_ = std::max(mPeak, peakMix_ * (1.0 - mixDecay_));
                if (peakMix_ > 1e-12 && peakCap_ > 1e-12) {
                    const double peakScale = peakCap_ / peakMix_;
                    if (peakScale < scale) scale = peakScale;
                }
            }
        }
        // Seco en silencio o EMAs no finitas: mezcla de potencia constante
        // clásica (scale = 1), sin blow-up.
        outL = static_cast<Scalar>(scale * (c * dL + s * wL));
        outR = static_cast<Scalar>(scale * (c * dR + s * wR));
    }

    double dryPower() const noexcept { return pDry_; }
    double wetPower() const noexcept { return pWet_; }
    double crossPower() const noexcept { return cross_; }

private:
    double alpha_ = 1.0 - std::exp(-1.0 / (kTauSeconds * 48000.0));
    double capDecay_ = 1.0 - std::exp(-1.0 / (kCapTauSeconds * 48000.0));
    double mixDecay_ = 1.0 - std::exp(-1.0 / (kMixTauSeconds * 48000.0));
    double pDry_ = 0.0;
    double pWet_ = 0.0;
    double cross_ = 0.0;
    double peakMix_ = 0.0;  // max-hold |mezcla sin escalar|, τ = 1 ms (guarda 3.2a)
    double peakCap_ = 0.0;  // max-hold max(|dry|, |wet|), τ = 50 ms (guarda 3.2b)
};

}  // namespace spatial
}  // namespace ivanna
