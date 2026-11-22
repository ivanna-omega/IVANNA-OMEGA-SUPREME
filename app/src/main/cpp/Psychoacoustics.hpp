#pragma once

#include "IvannaFusionCore.hpp"
#include <array>

namespace Ivanna {

constexpr size_t LSTM_UNITS = 8; // Ultra-fast int8 matrix execution tuned for L1 cache fit

// F2/486 (2026-11-20) — MOTOR DOUBLE: el LSTM es int8 cuantizado (plano
// de control, NO se convierte: sus pesos son enteros) y las envolventes /
// ganancias / estado IIR son double 64-bit. Las versiones planares
// double son el núcleo; las versiones AudioBuffer* (float) son
// envoltorios de compat que convierten al núcleo y redondean la salida.
class Psychoacoustics {
public:
    Psychoacoustics();
    void applyMaskingCompensation(Ivanna::AudioBuffer* buffer);
    void predictAndMitigateFatigue(Ivanna::AudioBuffer* buffer);
    void applyMaskingCompensation(double* left, double* right, int n);
    void predictAndMitigateFatigue(double* left, double* right, int n);

    [[nodiscard]] float fatigueIndex() const noexcept { return static_cast<float>(m_fatigueIndex); }
    [[nodiscard]] float maskingGainL() const noexcept { return static_cast<float>(m_gainSmL); }
    [[nodiscard]] float maskingGainR() const noexcept { return static_cast<float>(m_gainSmR); }

private:
    // Quantized int8_t LSTM Weights
    ALIGN_NEON int8_t m_Wf[LSTM_UNITS][LSTM_UNITS];
    ALIGN_NEON int8_t m_Wi[LSTM_UNITS][LSTM_UNITS];
    ALIGN_NEON int8_t m_Wc[LSTM_UNITS][LSTM_UNITS];
    ALIGN_NEON int8_t m_Wo[LSTM_UNITS][LSTM_UNITS];

    ALIGN_NEON int16_t m_h_state[LSTM_UNITS];
    ALIGN_NEON int16_t m_c_state[LSTM_UNITS];

    double m_fatigueIndex = 0.0;
    double m_envLeft  = 0.0;
    double m_envRight = 0.0;

    // Gain smoothing para applyMaskingCompensation (evita clicks de ganancia)
    double m_gainSmL = 1.0;
    double m_gainSmR = 1.0;

    // Estado IIR persistente entre bloques (evita tronidos/pops periódicos)
    double m_iirStateL = 0.0;
    double m_iirStateR = 0.0;

    // Scratch de los envoltorios AudioBuffer* (128 fijos, sin malloc).
    std::array<double, BLOCK_SIZE> wrapL_{};
    std::array<double, BLOCK_SIZE> wrapR_{};
};

} // namespace Ivanna
