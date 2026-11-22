#pragma once
#include "dsp_types.h"
#include <vector>

namespace ivanna {

// Compresor monobanda con detector de pico link estéreo, soft knee y
// makeup acoplado.
//
// F2/486 (2026-11-20) — MOTOR DOUBLE: estado y matemática en double 64-bit.
// process(double*, double*, int) es el núcleo; process(float*, float*, int)
// es un envoltorio de compat (convierte al núcleo y redondea la salida).
// Los setters siguen recibiendo float (valores de UI); el estado interno
// es double.
class Compressor {
public:
    Compressor();

    void setParams(const DSPParams& p);
    void setThreshold(float db);
    void setRatio(float ratio);
    void setAttack(float ms);
    void setRelease(float ms);
    void process(double* left, double* right, int frames);
    void process(float* left, float* right, int frames);
    void reset();

    /// Clears only the ADE runtime overlay; user threshold/ratio/attack/release are preserved.
    void resetRuntimeAmount() noexcept {
        runtimeAmount_ = 0.0;
        runtimeTarget_ = 0.0;
    }

    void setRuntimeAmount(float amount01) noexcept {
        // Solo fija el OBJETIVO — process() converge con one-pole (~20 ms).
        // El motor adaptativo llama esto por bloque y antes el valor entraba
        // directo a threshold (-12 dB) y ratio (+8): cada actualización era
        // un salto de ganancia audible en material sostenido (zipper adaptativo).
        runtimeTarget_ =
            amount01 < 0.f ? 0.0 :
            (amount01 > 1.f ? 1.0 : static_cast<double>(amount01));
    }

private:
    void recomputeMakeup() noexcept;

    double sr_ = 96000.0;
    // FIX (discontinuidad audible real, mismo patrón que el peak-guard ya
    // reparado en este frente): threshold_/ratio_ eran los valores que
    // process() usaba DIRECTAMENTE, y setThreshold()/setRatio() los
    // pisaban de golpe — disparado por cada actualización térmica
    // (potencialmente varias veces por minuto durante una sesión larga)
    // o cada cambio de preset de usuario por voz. runtimeAmount_ (el
    // ajuste fino del motor adaptativo) YA tenía protección anti-zipper
    // documentada explícitamente para este mismo problema; los valores
    // BASE no la tenían. threshold_/ratio_ ahora son el OBJETIVO fijado
    // por el llamador; *_smoothed_ son los que process() realmente usa,
    // convergiendo con el mismo one-pole de ~20ms ya validado abajo.
    double threshold_ = -12.0;
    double ratio_ = 4.0;
    double thresholdSmoothed_ = -12.0;
    double ratioSmoothed_ = 4.0;
    double attackCoef_ = 0.99;
    double releaseCoef_ = 0.999;

    double makeupGain_ = 1.0;   // lineal (solo informativo/compat)
    double makeupDb_ = 0.0;     // techo de compensacion en dB
    double env_ = 0.0;
    double envSmooth_ = 0.0;   // 2a etapa del detector: filtra el rizado a 2f de |x| (distorsion armonica)
    double runtimeAmount_ = 0.0;   // valor suavizado (uso real en process)
    double runtimeTarget_ = 0.0;   // objetivo fijado por setRuntimeAmount
    uint32_t lastSr_ = 0;             // sample rate con el que se armo el sidechain
    double runtimeCoef_ = 0.0;     // coef one-pole; 0 => recalcular en process

    Biquad scHpfL_;
    Biquad scHpfR_;

    // Scratch del envoltorio float (crece bajo demanda).
    std::vector<double> wrapL_, wrapR_;
    // FIX: inv_atk_, inv_rel_, slope_ eran calculados en setAttack/setRelease
    // y recomputeMakeup() pero NUNCA leídos en process() — el compresor real
    // usa attackCoef_ y releaseCoef_ (coeficientes de one-pole). Los tres
    // campos [[maybe_unused]] añadían 12 bytes de estado falso por instancia,
    // confundían el análisis de cobertura, y podían enmascarar bugs futuros
    // (un linter podría marcar un campo como "usado" solo porque existe).
    // Eliminados: el comportamiento del compresor no cambia en absoluto.
};

}
