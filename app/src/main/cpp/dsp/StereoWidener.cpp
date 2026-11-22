#include "../include/StereoWidener.h"

namespace ivanna {

// FIX (tuning magistral): antes gamma alimentaba EL MISMO parámetro que
// controla el timing del compresor (ver Compressor.cpp: atk/rel dependen
// de p.gamma). StereoWidener::setParams también leía p.gamma para el
// ancho estéreo — colisión real: mover el timing del compresor cambiaba
// el ancho estéreo en silencio, y viceversa. DSPState.stereoWidth (Kotlin)
// nunca llegaba al motor nativo (pushToNative() no lo incluía) — el
// control de ancho de la UI estaba completamente muerto.
// Ahora setParams() ya NO deriva el ancho de gamma; sólo setWidth() (vía
// nativeSetStereoWidth, wireado end-to-end) lo controla.
void StereoWidener::setParams(const DSPParams& p) {
    if (p.sampleRate != lastSampleRate_) {
        lastSampleRate_ = p.sampleRate;
        // Rampa de ancho ~15 ms a cualquier sample rate (anti-zipper al
        // arrastrar el slider). exp(-1/(sr*0.015)) ≈ 0.99956 @ 48k.
        widthSmooth_ = 1.0 - 1.0 / (static_cast<double>(p.sampleRate) * 0.015);
        if (widthSmooth_ < 0.99) widthSmooth_ = 0.99;
        // Corte a 150Hz, Q=0.707 (Butterworth) — protege el "punch" de bajo
        // (kick/bajo eléctrico) de cancelación de fase al sumar en mono.
        sideLpf_.setLowpass(150.0, 0.70710678, static_cast<double>(p.sampleRate));
        // DC blocker fc~5Hz
        double fc5 = 2.0 * 3.14159265 * 5.0 / static_cast<double>(p.sampleRate);
        dcCoef_ = 1.0 - fc5;
        if (dcCoef_ < 0.99) dcCoef_ = 0.99;
    }
}

void StereoWidener::setWidth(float w) {
    // w en rango [0,2]: 0=mono, 1=unity, 2=ancho máximo
    width_ = w < 0.0 ? 0.0 : (w > 2.0 ? 2.0 : static_cast<double>(w));
}

__attribute__((hot, flatten))
void StereoWidener::process(double* __restrict__ left, double* __restrict__ right, int frames) {
    if (frames <= 0) return;

    // FIX (zipper noise): width_ cambia de golpe al mover el slider y el
    // widener M/S es lineal en width -> cada salto discretizado por bloque
    // se oía como un click. Se suaviza widthNow_ hacia width_ muestra a
    // muestra (one-pole ~15 ms). Si ya está convergido, camino rápido sin
    // multiplicar por bloque.
    const double wTarget = width_;
    double wNow = widthNow_;
    const double ws = widthSmooth_;
    const double wsi = 1.0 - ws;
    // Si la diferencia es inaudible, snap directo (evita convergencia
    // asintótica eterna y el branch extra por muestra).
    const double d = wTarget - wNow;
    if (d > -1e-5 && d < 1e-5) wNow = wTarget;

    // FIX: unity debe ser transparente.
    // Con width=1 no existe transformación M/S real,
    // por lo tanto no debe pasar por filtros que cambien fase.
    if (wNow > 0.999999 && wNow < 1.000001 && wTarget == wNow) {

        // Mantener estados calientes para evitar clicks al reactivar.
        for (int i = 0; i < frames; ++i) {
            const double xL = left[i];
            const double xR = right[i];
            // FIX (thump al activar widener): Mantener lpf side caliente
            sideLpf_.process(0.5 * (xL - xR));

            dcyL_ = xL - dcxL_ + dcCoef_ * dcyL_;
            dcxL_ = xL;

            dcyR_ = xR - dcxR_ + dcCoef_ * dcyR_;
            dcxR_ = xR;
        }

        widthNow_ = wNow;
        return;
    }

    // FIX (tuning magistral): a w<=1 (unity/narrow) el comportamiento es
    // IDÉNTICO al widener naive anterior (bassFactor==w) — no cambia el
    // sonido por defecto. Sólo al ensanchar (w>1, el caso real de riesgo
    // de cancelación en mono) se limita el boost de graves, rampa lineal
    // de 1.0 en w=1 hasta 0.25 en w=2 (75% menos boost de side en graves
    // al ancho máximo, altas siguen recibiendo el ensanche completo).
    // NOTE: loop contains stateful sideLpf_.process() — cannot be auto-vectorized.
    // Pragma removed to suppress -Wpass-failed=transform-warning.
    for (int i = 0; i < frames; ++i) {
        // Suavizado por muestra del ancho (anti-zipper).
        wNow = ws * wNow + wsi * wTarget;
        const double w = wNow;
        const double bassFactor = (w <= 1.0) ? w : (1.0 + (0.25 - 1.0) * (w - 1.0));

        const double l = left[i];
        const double r = right[i];
        const double mid  = 0.5 * (l + r);
        const double side = 0.5 * (l - r);

        const double sideLow  = sideLpf_.process(side);
        const double sideHigh = side - sideLow;

        const double sideOut = sideHigh * w + sideLow * bassFactor;

        double outL = mid + sideOut;
        double outR = mid - sideOut;

        // DC blocking primer orden — elimina drift acumulado
        double newxL = outL;
        outL = outL - dcxL_ + dcCoef_ * dcyL_;
        dcxL_ = newxL; dcyL_ = outL;

        double newxR = outR;
        outR = outR - dcxR_ + dcCoef_ * dcyR_;
        dcxR_ = newxR; dcyR_ = outR;

        left[i]  = outL;
        right[i] = outR;
    }

    widthNow_ = wNow;
}

void StereoWidener::process(float* left, float* right, int frames) {
    if (frames <= 0) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(left[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(right[i]);
    }
    process(wrapL_.data(), wrapR_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        right[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}

void StereoWidener::reset() {
    sideLpf_.reset();
    dcxL_ = dcyL_ = dcxR_ = dcyR_ = 0.0;
    widthNow_ = width_;  // arrancar sin rampa tras reset
}

} // namespace ivanna
