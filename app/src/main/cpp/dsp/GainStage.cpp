#include "../include/GainStage.h"
#include "../include/dsp_types.h"
#include <cmath>

namespace ivanna {

static inline double dbToLin(double db) {
    return std::exp2(db * 0.1660964);
}

void GainStage::setParams(const DSPParams& p) {
    sr_ = static_cast<double>(p.sampleRate);
    smoothCoeff_ = std::exp(-1.0 / (sr_ * 0.015));
    oneMinusSmooth_ = 1.0 - smoothCoeff_;
    inputGain_ = dbToLin((static_cast<double>(p.mix) - 0.5) * 12.0);
    outputGain_ = dbToLin(static_cast<double>(p.master));
    // FIX (clicks al mover sliders): setParams() saltaba currentIn_/currentOut_
    // directamente al nuevo objetivo, anulando la rampa anti-zipper de 15 ms
    // del propio process(). Cada actualizacion de mix/master desde la UI (o del
    // motor adaptativo) era un escalon de ganancia de bloque -> chasquido.
    // Ahora solo se inicializa en el primer setParams o si cambio el sample
    // rate (donde no hay continuidad de senal que preservar).
    if (!initialized_ || sr_ != lastSr_) {
        initialized_ = true;
        lastSr_ = sr_;
        currentIn_ = inputGain_;
        currentOut_ = outputGain_;
    }
}

void GainStage::processInput(double* __restrict__ left, double* __restrict__ right, int frames) {
    if (frames <= 0) return;
    const double s = smoothCoeff_, o = oneMinusSmooth_, t = inputGain_;
    double c = currentIn_;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpass-failed"
    for (int i = 0; i < frames; ++i) {
        c = s * c + o * t;
        left[i] *= c;
        right[i] *= c;
    }
#pragma clang diagnostic pop
    currentIn_ = c;
}

void GainStage::processOutput(double* __restrict__ left, double* __restrict__ right, int frames) {
    if (frames <= 0) return;
    const double s = smoothCoeff_, o = oneMinusSmooth_, t = outputGain_ * runtimeMul_;
    double c = currentOut_;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wpass-failed"
    // Ganancia de salida suavizada, sin limitacion: el SafetyLimiter que corre
    // inmediatamente despues aplica el techo de -0.1 dBFS por bloque. Limitar
    // aqui tambien (ataque de 1 muestra) creaba una segunda modulacion de
    // ganancia en cascada -> intermodulacion de banda ancha audible.
    for (int i = 0; i < frames; ++i) {
        c = s * c + o * t;
        left[i]  *= c;
        right[i] *= c;
    }
#pragma clang diagnostic pop
    currentOut_ = c;
}

void GainStage::processInput(float* left, float* right, int frames) {
    if (frames <= 0) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(left[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(right[i]);
    }
    processInput(wrapL_.data(), wrapR_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        right[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}

void GainStage::processOutput(float* left, float* right, int frames) {
    if (frames <= 0) return;
    if (static_cast<int>(wrapL_.size()) < frames) {
        wrapL_.resize(static_cast<size_t>(frames));
        wrapR_.resize(static_cast<size_t>(frames));
    }
    for (int i = 0; i < frames; ++i) {
        wrapL_[static_cast<size_t>(i)] = static_cast<double>(left[i]);
        wrapR_[static_cast<size_t>(i)] = static_cast<double>(right[i]);
    }
    processOutput(wrapL_.data(), wrapR_.data(), frames);
    for (int i = 0; i < frames; ++i) {
        left[i] = static_cast<float>(wrapL_[static_cast<size_t>(i)]);
        right[i] = static_cast<float>(wrapR_[static_cast<size_t>(i)]);
    }
}

void GainStage::reset() {
    // FIX (pop al resetear con audio activo): reset() saltaba currentIn_/
    // currentOut_ al objetivo de golpe -> escalon de ganancia audible si se
    // invoca en cambio de ventana/sesion/motor. Una vez inicializado se
    // conserva la ganancia actual: processInput/Output la llevan al objetivo
    // con la rampa de 15 ms. Antes del primer setParams no hay senal previa,
    // asi que ahi si se fija directamente.
    if (!initialized_) {
        currentIn_ = inputGain_;
        currentOut_ = outputGain_;
    }
}

} // namespace ivanna
