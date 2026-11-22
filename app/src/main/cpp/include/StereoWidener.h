#pragma once
#include "dsp_types.h"
#include <vector>

namespace ivanna {
class StereoWidener {
public:
    void setParams(const DSPParams& p);
    void setWidth(float w);
    // Núcleo double 64-bit (F2/486): camino de señal de la FusedChain.
    void process(double* l, double* r, int frames);
    // Envoltorio float (compat): convierte al núcleo double y redondea la
    // salida a float. Los llamadores históricos no cambian de API.
    void process(float* l, float* r, int frames);
    void reset();
private:
    double width_ = 1.0;        // objetivo (slider)
    double widthNow_ = 1.0;     // valor suavizado aplicado por muestra
    double widthSmooth_ = 0.9995;  // ~15ms @ 96kHz, recalculado en setParams()
    // FIX: halfWidth_ era [[maybe_unused]] — calculado como 0.5*width_ en
    // setParams() pero nunca leído en process(). Eliminado: sin cambio de
    // comportamiento. El proceso M/S usa widthNow_ directamente.
    // FIX (tuning magistral): crossover mono-safe de graves — sin esto, un
    // widener M/S puro cancela fase en mono por debajo de ~150Hz (bug real
    // de todo widener naive). El "side" se separa en low/high; el ensanche
    // sólo se aplica por encima del corte, y por debajo se limita el boost
    // de forma proporcional a cuánto se está ensanchando (ver .cpp).
    Biquad sideLpf_;
    uint32_t lastSampleRate_ = 96000;

    // DC blockers en canales L y R — HPF primer orden ~5Hz
    double dcxL_ = 0.0, dcyL_ = 0.0;
    double dcxR_ = 0.0, dcyR_ = 0.0;
    double dcCoef_ = 0.99985; // fc≈5Hz @ 96kHz

    // Scratch del envoltorio float (crece bajo demanda, nunca en RT tras
    // el primer uso): el núcleo double es el único que toca el estado.
    std::vector<double> wrapL_, wrapR_;
};
}
