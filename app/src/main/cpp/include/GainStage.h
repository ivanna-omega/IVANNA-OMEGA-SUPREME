#pragma once
#include "dsp_types.h"
#include <cmath>
#include <vector>
namespace ivanna {
class GainStage {
public:
    void setParams(const DSPParams& p);
    // Núcleo double 64-bit (F2/486): camino de señal de la FusedChain.
    void processInput(double* left, double* right, int frames);
    void processOutput(double* left, double* right, int frames);
    // Envoltorios float (compat): mismo núcleo double, salida redondeada.
    void processInput(float* left, float* right, int frames);
    void processOutput(float* left, float* right, int frames);
    void reset();

    void setRuntimeGain(float mul) noexcept {
        // FIX: cap anterior era 1.5× = +3.5 dB. El motor adaptativo
        // (AdaptiveDecisionEngine) llama esto cada bloque para ajustar el
        // nivel de salida. Con cap en 1.5, podía aplicar +3.5 dB sobre una
        // señal ya cercana al techo → SafetyLimiter activado → compresión
        // de ganancia perceptible como pumping.
        // Semántica correcta del runtime gain: solo puede ATENUAR (proteger),
        // nunca amplificar. La ganancia positiva la gestiona el GainStage
        // normal vía setParams()/outputGain_. Cap máximo = 1.0 (unity).
        runtimeMul_ = mul < 0.1f ? 0.1 : (mul > 1.0f ? 1.0 : static_cast<double>(mul));
    }

private:
    double sr_ = 96000.0;
    double smoothCoeff_ = 0.99;
    double oneMinusSmooth_ = 0.01;
    double inputGain_ = 1.0;
    double outputGain_ = 1.0;
    double currentIn_ = 1.0;
    double currentOut_ = 1.0;
    double runtimeMul_ = 1.0;
    double lastSr_ = 0.0;
    bool  initialized_ = false;

    // Scratch de los envoltorios float (crece bajo demanda).
    std::vector<double> wrapL_, wrapR_;

    // FIX (ruido digital): aqui vivia un limitador brick-wall de 1 muestra de
    // ataque, en cascada delante del SafetyLimiter. Dos limiters en serie con
    // ataque instantaneo modulan la senal muestra a muestra y generan
    // intermodulacion de banda ancha. El SafetyLimiter final ya garantiza el
    // techo de -0.1 dBFS con escalado por bloque (sin AM), asi que esta etapa
    // ahora es solo ganancia suavizada, lineal y transparente.
};
}
