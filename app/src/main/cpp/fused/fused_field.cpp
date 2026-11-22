// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
// fused_field.cpp — OPERADOR DE CAMPO UNIFICADO (#2 de la Fusión Magistral).
// Envuelve SIN TOCAR: HolosOperator (núcleo, Fase B −102.79 dB intacta) →
// OmniField → HyperStateOperator (Phi, XTC interno apagado).
// Ver fused_field.hpp para el mapeo completo de controles.
#include "fused/fused_field.hpp"

namespace ivanna::fused {

void FusedField::prepare(double sampleRate) noexcept {
    const double sr = (std::isfinite(sampleRate) && sampleRate >= 8000.0 &&
                       sampleRate <= 192000.0)
                          ? sampleRate
                          : 48000.0;
    sampleRate_ = sr;
    holos_.prepare(static_cast<float>(sr));
    omni_.prepare(static_cast<float>(sr));
    phi_.prepare(static_cast<float>(sr));
    prepared_ = true;
    pushParamsToEngines();
}

void FusedField::setControls(const FieldControls& c) noexcept {
    controls_ = c;
    controls_.clamp();
    pushParamsToEngines();
}

void FusedField::setOutputMode(OmegaOutputMode mode) noexcept {
    // PARCHE v2.6: escribe en controls_.mode (única fuente de verdad);
    // outputMode_ queda como espejo. UNKNOWN (0) fail-closed a HEADPHONES.
    const OmegaOutputMode m = (mode == OmegaOutputMode::SPEAKERS)
                      ? OmegaOutputMode::SPEAKERS
                      : OmegaOutputMode::HEADPHONES;
    controls_.mode = m;
    outputMode_ = m;
    pushParamsToEngines();
}

void FusedField::pushParamsToEngines() noexcept {
    // ── HOLOS (núcleo; se envuelve, no se toca) ──────────────────────────
    HolosParams hp;
    // Puerta Genezi (Fallo 3a): en SPEAKERS el GreenKernel binaural
    // (ITD/ILD + pinna, renderBins) y la cola difusa rir_ (Master Studio,
    // decorrelación tardía entre oídos) no tienen sentido: amount → 0
    // deja a HolosOperator en passthrough tras su fade de salida, y
    // diffuse → 0 corta la alimentación del término difuso. En
    // HEADPHONES, valores vivos de siempre (amount del usuario,
    // diffuse 0.5 por defecto de HolosParams).
    hp.amount = isSpeakers() ? 0.0f : controls_.amount;
    hp.diffuse = isSpeakers() ? 0.0f : 0.5f;
    hp.gamma = controls_.gamma;
    hp.roomRt60 = controls_.roomRt60;
    // Guarda validada (v2.5.411): el K nunca recibe más de 0.002.
    hp.harmonics = std::clamp(controls_.harmonics, 0.0f, kHolosHarmonicsMaxValidated);
    hp.warmth = controls_.warmth;
    // Bombeo CERO: pumping=0 → dynamics=1.0 (Γ dinámica de HOLOS como
    // hoy, sin compresión extra); pumping=1 → dynamics=0 (Γ dinámica off).
    hp.dynamics = 1.0f - controls_.pumping;
    // Defaults conservados: distanceM 0.35, spread 1.0, proximityDb 3.0,
    // diffuse 0.5, width 1.0, headTracking true, bandCount 24, EQ plano.
    holos_.setParams(hp);

    // ── OmniField (campo unificado IVANNA, después de HOLOS) ─────────────
    ivanna::omnifield::OmniParams op;
    op.amount = controls_.amount;  // mismo amount de campo que HOLOS
    op.headTracking = false;       // sin pose: la escena se auto-estima
    omni_.setParams(op);

    // ── Phi (después de HOLOS+Omni, como en la cadena viva) ──────────────
    phi_.setAmount(controls_.phiAmount);
    phi_.setRoutePreset(static_cast<ivanna::spatial::HyperRoutePreset>(controls_.phiPreset));
    // XTC interno APAGADO: la cancelación de diafonía vive en el
    // Operador Espacial (#1); aquí duplicarla pelearía con Γ.
    phi_.setXtcEnabled(false);
}

void FusedField::process(double* left, double* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0) return;
    if (isNeutral()) return;  // bit-transparente, ganancia exactamente 1.0
    if (!prepared_) return;   // fail-closed: prepare() primero

    // F2/486: los tres motores corren en double extremo a extremo; ya no
    // hay conversión double→float→double por bloque. Motores en serie, en
    // trozos de 128 frames como la cadena viva.
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kFusedMaxBlockFrames);
        for (int off = 0; off < chunk; off += kChunkFrames) {
            const int m = std::min(kChunkFrames, chunk - off);
            double* l = left + offset + off;
            double* r = right + offset + off;
            holos_.process(l, r, m);
            omni_.process(l, r, m);
            phi_.processBlock(l, r, m);
        }
        // Saneo fail-closed (no-finito → 0.0) sobre la salida double.
        for (int i = 0; i < chunk; ++i) {
            const double l = left[offset + i];
            const double r = right[offset + i];
            left[offset + i] = std::isfinite(l) ? l : 0.0;
            right[offset + i] = std::isfinite(r) ? r : 0.0;
        }
        offset += chunk;
        remaining -= chunk;
    }
}

void FusedField::reset() noexcept {
    holos_.reset();
    omni_.reset();
    phi_.reset();
}

}  // namespace ivanna::fused
