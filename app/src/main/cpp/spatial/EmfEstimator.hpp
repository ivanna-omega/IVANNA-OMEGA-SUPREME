// =====================================================================
//  IVANNA — Estimador de estado del cono por V/I (Thiele-Small) v2.5.368
//  EmfEstimator.hpp — header-only, C++20/26, cero malloc en audio.
//
//  © 2026 Luis Uriel Pimentel Pérez · GORE TNS. All rights reserved.
//
//  El lado SOFTWARE del pilar 2 (docs/AUX_GENEZI_ROUTE.md §2): la
//  física de la bobina SÍ se puede estimar a partir de tensión y
//  corriente medidas en bornes del altavoz — pero la MEDIDA no puede
//  venir del cable Aux 3.5 mm (salida analógica de solo salida, sin
//  camino de retorno). Este módulo es el estimador, listo para cuando
//  exista una fuente real de V/I; no finge medir nada.
//
//  Modelo electromecánico (Thiele-Small, bobina móvil):
//
//      V(t) = Re·I(t) + Le·dI/dt + Bl·v(t)
//
//  donde Bl·v(t) es la fuerza contra-electromotriz (Back-EMF). De ahí:
//
//      v(t) = (V − Re·I − Le·dI/dt) / Bl        (velocidad del cono)
//      x(t) = ∫ v dt                            (desplazamiento)
//      F(t) = Bl·I(t)                           (fuerza sobre el cono)
//
//  dI/dt se estima por diferencia primera; el desplazamiento usa un
//  integrador con fuga (leak) para que el ruido DC no lo haga derivar
//  sin límite.
//
//  Qué fuente de V/I es real (hardware, fuera de este repo):
//    * Un amplificador con sensado de corriente (p. ej., Class-D con
//      diagnóstico de carga / shunt + ADC) que reporte V e I.
//    * Una interfaz USB de audio con entrada de línea conectada a
//      través de un divisor + shunt de corriente en bornes.
//    * NUNCA el DAC USB-C por el Aux: no hay ADC en ese camino.
//
//  API: prepare(fs, params) valida los parámetros Thiele-Small
//  (Re>0, Bl>0, Le≥0, leak en [0,1)); process(V, I, n, vOut, xOut)
//  estima velocidad y desplazamiento por muestra. Sin fuente, las
//  entradas son ceros y la salida es cero: identidad, no invención.
// =====================================================================
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace ivanna::spatial {

// Parámetros Thiele-Small del transductor (valores por defecto:
// woofer pequeño tipo Genezi; medir el real con el protocolo de
// caracterización antes de usarlo en producción).
struct TsParams {
    float Re = 6.0f;    // resistencia DC de la bobina [ohm]
    float Le = 0.0005f; // inductancia de la bobina [H]
    float Bl = 5.0f;    // factor de fuerza [T·m]
    float leak = 0.999f;  // fuga del integrador de desplazamiento [0,1)
};

class EmfEstimator {
public:
    void prepare(float sampleRate, const TsParams& p) noexcept {
        sampleRate_ = (sampleRate > 8000.0f) ? static_cast<double>(sampleRate) : 48000.0;
        params_.Re = (std::isfinite(p.Re) && p.Re > 0.0f) ? p.Re : 6.0f;
        params_.Le = (std::isfinite(p.Le) && p.Le >= 0.0f) ? p.Le : 0.0005f;
        params_.Bl = (std::isfinite(p.Bl) && p.Bl > 0.0f) ? p.Bl : 5.0f;
        params_.leak =
            (std::isfinite(p.leak) && p.leak >= 0.0f && p.leak < 1.0f)
                ? p.leak
                : 0.999f;
        prevI_ = 0.0;
        displacement_ = 0.0;
        prepared_ = true;
    }

    [[nodiscard]] TsParams params() const noexcept { return params_; }

    void reset() noexcept {
        prevI_ = 0.0;
        displacement_ = 0.0;
    }

    // Hilo de audio (o de medición). vOut/xOut pueden ser nullptr si
    // solo interesa una de las dos. Entradas no finitas -> 0.
    // Núcleo double (F2/486): la estimación V/I es plano de señal.
    void process(const double* voltage, const double* current, int n,
                 double* velocityOut, double* displacementOut) noexcept {
        if (!prepared_ || !voltage || !current || n <= 0) return;
        const double fs = sampleRate_;
        const double re = static_cast<double>(params_.Re);
        const double le = static_cast<double>(params_.Le);
        const double bl = static_cast<double>(params_.Bl);
        const double leak = static_cast<double>(params_.leak);
        for (int i = 0; i < n; ++i) {
            const double v = std::isfinite(voltage[i]) ? voltage[i] : 0.0;
            const double cur = std::isfinite(current[i]) ? current[i] : 0.0;
            const double didt = (cur - prevI_) * fs;
            prevI_ = cur;
            // Back-EMF = V − Re·I − Le·dI/dt ; v = Back-EMF / Bl.
            double vel = (v - re * cur - le * didt) / bl;
            if (!std::isfinite(vel)) vel = 0.0;
            displacement_ = leak * displacement_ + vel / fs;
            if (!std::isfinite(displacement_)) displacement_ = 0.0;
            if (velocityOut) velocityOut[i] = vel;
            if (displacementOut) displacementOut[i] = displacement_;
        }
    }

    // Envoltorio float (compat legacy): convierte por scratch y delega
    // en el núcleo double; el estado es compartido (double).
    void process(const float* voltage, const float* current, int n,
                 float* velocityOut, float* displacementOut) noexcept {
        if (!prepared_ || !voltage || !current || n <= 0) return;
        const std::size_t un = static_cast<std::size_t>(n);
        if (wrapV_.size() < un) {
            wrapV_.resize(un); wrapI_.resize(un);
            wrapVel_.resize(un); wrapDisp_.resize(un);
        }
        for (int i = 0; i < n; ++i) {
            wrapV_[i] = std::isfinite(voltage[i]) ? static_cast<double>(voltage[i]) : 0.0;
            wrapI_[i] = std::isfinite(current[i]) ? static_cast<double>(current[i]) : 0.0;
        }
        process(wrapV_.data(), wrapI_.data(), n, wrapVel_.data(), wrapDisp_.data());
        if (velocityOut) {
            for (int i = 0; i < n; ++i)
                velocityOut[i] = std::isfinite(wrapVel_[i]) ? static_cast<float>(wrapVel_[i]) : 0.0f;
        }
        if (displacementOut) {
            for (int i = 0; i < n; ++i)
                displacementOut[i] = std::isfinite(wrapDisp_[i]) ? static_cast<float>(wrapDisp_[i]) : 0.0f;
        }
    }

private:
    double sampleRate_ = 48000.0;
    TsParams params_{};
    bool prepared_ = false;
    double prevI_ = 0.0;
    double displacement_ = 0.0;
    // Scratch del envoltorio float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapV_{};
    std::vector<double> wrapI_{};
    std::vector<double> wrapVel_{};
    std::vector<double> wrapDisp_{};
};

}  // namespace ivanna::spatial
