// tenth_dimension_action.hpp — CORONA D10: acción de décima dimensión.
//
// Tres piezas, todo en C++20, plano double, `noexcept`, cero malloc/new/
// locks/syscalls/I-O tras prepare():
//   1) ThermalMechanicalObserver64 — observador térmico-mecánico del
//      transductor (bobina + imán + excursión por biquad resonante).
//   2) TimeInvariantMatrixKernel2x2 — kernel matricial 2x2 LTI (FIR 512
//      taps por rama, la IFFT completa con desplazamiento circular de
//      256) compilado desde el espectro TΩ unificado
//      (apex::UnifiedFieldWorkspace::outMatrix), con morph suave.
//   3) TenthDimensionActionEngine — árbitro D10: observación térmica y
//      telemetría de acción hamiltoniana. La CORRECCIÓN DE GANANCIA por
//      muestra con slew (18 ms) es LEGACY DESACTIVADA por defecto
//      (fix-arbiter 2026-11-25, ver D10Config::energyCorrectionEnabled):
//      medida en vivo, esa ganancia MODULABA el tono (THD+N), degradaba
//      los pisos 10-101 dB, sumaba +2.34 dB y rompía P0ReferenceLock, y
//      su autoridad (±3 dB) no podía cerrar la brecha LUFS medida de
//      ~18 dB entre seco y cadena con 7 operadores a 1.0. El motor ya no
//      toca el buffer: observa y reporta; el kernel LTI (2) y el
//      observador térmico (1) siguen activos.
//
// Origen: documento D10 de Gemini, §3, RECONSTRUIDO y CORREGIDO contra el
// código vivo (el código del documento estaba parcialmente corrupto y su
// fórmula ITD era inventada — aquí el ITD acoplado usa la ley de
// curvatura verificada propagation::curvatureLawItdSeconds).
//
// En neutro el motor NO toca el buffer: arbitrateAction() es no-op si
// !enabled, y el kernel es no-op si !engaged_. El llamador además solo
// lo invoca cuando !neutral.
//
// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once

#include "fused/physical_propagation.hpp"
#include "fused/unified_field_kernel.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>

namespace ivanna::fused::d10 {

// ═══════════════════════════════════════════════════════════════════════════
// 1) Observador térmico-mecánico del transductor
// ═══════════════════════════════════════════════════════════════════════════

// SUPOSICIONES NO MEDIDAS para el Sony Genezi MHC-PZ1D: no existen
// parámetros térmicos medidos de este transductor (tau térmica de bobina
// e imán, temperatura máxima segura, Qts, fs, potencia nominal). Los
// valores por defecto son estimaciones de ingeniería razonables para un
// altavoz de estantería / minicadena de ~120 W. El observador ESTIMA el
// estado térmico a partir de la señal, NO MIDE temperatura real: no hay
// sensor térmico en el transductor. Si algún día se miden los parámetros
// reales, se sustituyen aquí y solo aquí.
struct TransducerPhysicalSpec {
    double nominalImpedanceOhm = 6.0;    // impedancia nominal (ohm)
    double ratedPowerWatts = 120.0;      // potencia nominal (W)
    double coilThermalTauSec = 8.5;      // tau térmica de la bobina (s)
    double magnetThermalTauSec = 180.0;  // tau térmica del imán (s)
    double ambientTempC = 25.0;          // temperatura ambiente (°C)
    double maxSafeTempC = 145.0;         // temperatura máxima segura (°C)
    double resonantFreqHz = 52.0;        // fs del transductor (Hz)
    double qts = 0.68;                   // Q total
    double xMaxNormalized = 1.0;         // excursión máxima normalizada
};

struct TransducerThermalState {
    double coilTempC = 25.0;              // temperatura estimada bobina (°C)
    double magnetTempC = 25.0;            // temperatura estimada imán (°C)
    double thermalCompressionRatio = 1.0; // 1/rFactor, clamp [0.5, 1]
    double normalizedExcursionPeak = 0.0; // pico de excursión / xMax
    double powerCompressionDb = 0.0;      // -20*log10(rFactor) (<= 0 dB)
};

class ThermalMechanicalObserver64 {
public:
    void prepare(double sampleRate, const TransducerPhysicalSpec& spec = TransducerPhysicalSpec{}) noexcept {
        sampleRate_ = (std::isfinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;
        spec_ = spec;
        // Biquad resonante pasa-bajos de 2º orden para estimar excursión
        // (desplazamiento ~ integral doble de la aceleración; el resonador
        // en fs aproxima la respuesta mecánica del transductor).
        const double fs = std::clamp(spec_.resonantFreqHz, 20.0, 200.0);
        const double w0 = 2.0 * kPiD10 * fs / sampleRate_;
        const double cosw0 = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * std::max(0.2, spec_.qts));
        const double denom = 1.0 + alpha;
        b0_ = ((1.0 - cosw0) * 0.5) / denom;
        b1_ = (1.0 - cosw0) / denom;
        b2_ = b0_;
        a1_ = (-2.0 * cosw0) / denom;
        a2_ = (1.0 - alpha) / denom;
        // Coeficientes térmicos por muestra (integración exacta 1er orden).
        alphaCoil_ = 1.0 - std::exp(-1.0 / (std::max(0.1, spec_.coilThermalTauSec) * sampleRate_));
        alphaMagnet_ = 1.0 - std::exp(-1.0 / (std::max(1.0, spec_.magnetThermalTauSec) * sampleRate_));
        reset();
    }

    void reset() noexcept {
        x1L_ = x2L_ = y1L_ = y2L_ = 0.0;
        x1R_ = x2R_ = y1R_ = y2R_ = 0.0;
        coilRise_ = 0.0;
        magnetRise_ = 0.0;
        maxExcursion_ = 0.0;
        state_ = TransducerThermalState{};
        state_.coilTempC = spec_.ambientTempC;
        state_.magnetTempC = spec_.ambientTempC;
    }

    // Observa un bloque y devuelve el estado térmico estimado.
    // meanSq es la potencia media cuadrática (media de l^2 y r^2).
    TransducerThermalState observeBlock(const double* l, const double* r, int frames) noexcept {
        if (!l || !r || frames <= 0) return state_;
        double sumSq = 0.0;
        for (int i = 0; i < frames; ++i) {
            const double xl = std::isfinite(l[i]) ? l[i] : 0.0;
            const double xr = std::isfinite(r[i]) ? r[i] : 0.0;
            // Biquad resonante por canal (desplazamiento estimado).
            const double dL = b0_ * xl + b1_ * x1L_ + b2_ * x2L_ - a1_ * y1L_ - a2_ * y2L_;
            x2L_ = x1L_; x1L_ = xl; y2L_ = y1L_; y1L_ = dL;
            const double dR = b0_ * xr + b1_ * x1R_ + b2_ * x2R_ - a1_ * y1R_ - a2_ * y2R_;
            x2R_ = x1R_; x1R_ = xr; y2R_ = y1R_; y1R_ = dR;
            // Envolvente de excursión: máximo absoluto con decaimiento.
            const double absD = std::max(std::fabs(dL), std::fabs(dR));
            if (absD > maxExcursion_) {
                maxExcursion_ = absD;
            } else {
                maxExcursion_ *= 0.9992;
            }
            sumSq += xl * xl + xr * xr;
        }
        const double meanSq = sumSq / (2.0 * static_cast<double>(frames));
        const double estWatts = meanSq * spec_.ratedPowerWatts;
        // Resistencia de la bobina de cobre: +0.393 %/°C.
        const double rFactor = 1.0 + 0.00393 * (coilRise_ + magnetRise_);
        const double effWatts = estWatts * rFactor;
        const double targetRise = (effWatts / std::max(1.0, spec_.ratedPowerWatts)) *
                                  (spec_.maxSafeTempC - spec_.ambientTempC);
        // Paso térmico exacto del bloque (1er orden, `frames` muestras).
        const double stepCoil = 1.0 - std::pow(1.0 - alphaCoil_, static_cast<double>(frames));
        const double stepMagnet = 1.0 - std::pow(1.0 - alphaMagnet_, static_cast<double>(frames));
        coilRise_ += stepCoil * (targetRise * 0.72 - coilRise_);
        magnetRise_ += stepMagnet * (targetRise * 0.28 - magnetRise_);

        state_.coilTempC = spec_.ambientTempC + coilRise_;
        state_.magnetTempC = spec_.ambientTempC + magnetRise_;
        state_.normalizedExcursionPeak = maxExcursion_ / std::max(1e-6, spec_.xMaxNormalized);
        state_.powerCompressionDb = -20.0 * std::log10(std::max(1.0, rFactor));
        state_.thermalCompressionRatio = std::clamp(1.0 / rFactor, 0.5, 1.0);
        return state_;
    }

    const TransducerThermalState& state() const noexcept { return state_; }
    double sampleRate() const noexcept { return sampleRate_; }

private:
    static inline constexpr double kPiD10 = 3.14159265358979323846;

    double sampleRate_ = 48000.0;
    TransducerPhysicalSpec spec_{};
    // Coeficientes del biquad resonante.
    double b0_ = 0.0, b1_ = 0.0, b2_ = 0.0, a1_ = 0.0, a2_ = 0.0;
    // Estados del biquad por canal.
    double x1L_ = 0.0, x2L_ = 0.0, y1L_ = 0.0, y2L_ = 0.0;
    double x1R_ = 0.0, x2R_ = 0.0, y1R_ = 0.0, y2R_ = 0.0;
    double alphaCoil_ = 0.0;
    double alphaMagnet_ = 0.0;
    double coilRise_ = 0.0;    // subida térmica bobina sobre ambiente (°C)
    double magnetRise_ = 0.0;  // subida térmica imán sobre ambiente (°C)
    double maxExcursion_ = 0.0;
    TransducerThermalState state_{};
};

// ═══════════════════════════════════════════════════════════════════════════
// 2) Kernel matricial 2x2 LTI (FIR por rama, compilado desde el espectro TΩ)
// ═══════════════════════════════════════════════════════════════════════════

class TimeInvariantMatrixKernel2x2 {
public:
    // FIR 512 taps con DESPLAZAMIENTO CIRCULAR de 256 (fix-2, 2026-11-25):
    // la FIR de 128 taps NO reproducía W(1000 Hz) — fuera de rejilla
    // (1000/93.75 = 10.667 bins), daba xt 0.128 vs pred 0.058 y nivel
    // −1.07 dB (test FusedChain.TOmegaSpeakersCrosstalk #1372); a 3000 Hz
    // (bin exacto, 32) sí pasaba. La causa NO era el truncado: alargar a
    // 256 midió xt 0.133 / −1.03 dB y la IFFT completa a 512 midió PEOR
    // en xt (0.276 / −0.58 dB, aunque 3000 Hz quedó exacto: 0.036/0.00).
    // La causa real (sonda probe-fir-interp.cpp, logs/fix-2): el IR del
    // inverso Kirkeby es BILATERAL — 93.3% de la energía en n[0,128)
    // pero 6.5% en tiempo NEGATIVO h[-128..-1] (la cancelación de
    // crosstalk exige adelanto en la rama cruzada). Sin desplazar, esos
    // taps quedan envueltos en [384,512) con fase e^{-jw·512} errónea
    // fuera de rejilla (a 1000 Hz, 240° de error sobre el 6.5% de la
    // energía → error relativo 34% en las ramas cruzadas). Afinar la
    // rejilla NO lo arregla (N=1024/2048 miden lo mismo: el error depende
    // solo de la posición fraccionaria del bin). El desplazamiento
    // circular de 256 centra el IR ([-256,+256)) y devuelve cada tap a
    // su retardo correcto, salvo una fase lineal COMÚN e^{-jw·256} (5.33
    // ms @ 48 kHz, retardo puro en serie: no toca magnitudes). Medido
    // con la sonda: 512 taps + shift 256 da xt 0.059 vs pred 0.058 y
    // nivel +0.03 dB a 1000 Hz, y 0.036/0.00 dB a 3000 Hz (exacto);
    // barrido 381..8000 Hz esencialmente exacto (|dxt|<=0.01). La
    // variante 256 taps + shift 128 también pasaba (xt 0.050, −0.06 dB)
    // pero con menos margen y peor banda media; se queda 512.
    // Coste: 2048 MAC/muestra (4 ramas × 512); el test
    // FusedChain.TOmegaDspUs lo vigila (p50 < 6666 µs/bloque de 320).
    static inline constexpr int kMatrixFirTaps = 512;
    // Desplazamiento circular del IR (muestras): centra la ventana en
    // [-kFirCenterShift, +kFirCenterShift). Debe ser < kMatrixFirTaps.
    static inline constexpr int kFirCenterShift = 256;
    // LEGACY: longitud anterior (128 taps, ventana en 96..127). Medido
    // insuficiente fuera de rejilla (#1372). La FIR de 256 taps sin
    // desplazar también se midió insuficiente (xt 0.133, −1.03 dB) y no
    // se conserva: el valor activo es la IFFT completa de 512 con
    // desplazamiento circular.
    static inline constexpr int kMatrixFirTapsLegacy128 = 128;
    // Ventana de coseno elevado en los últimos taps (suaviza el truncado).
    static inline constexpr int kFirWindowTaps = 32;
    static inline constexpr int kFftSizeD10 = 512;
    static inline constexpr int kFftBinsD10 = 257;

    void prepare() noexcept {
        for (auto& h : hActive_) h.fill(0.0);
        for (auto& h : hTarget_) h.fill(0.0);
        // Identidad: L->L y R->R con un solo tap en 0.
        hActive_[0][0] = 1.0;
        hActive_[3][0] = 1.0;
        hTarget_[0][0] = 1.0;
        hTarget_[3][0] = 1.0;
        ringL_.fill(0.0);
        ringR_.fill(0.0);
        wp_ = 0;
        morphAlpha_ = 1.0;
        engaged_ = false;
    }

    void reset() noexcept {
        ringL_.fill(0.0);
        ringR_.fill(0.0);
        wp_ = 0;
        morphAlpha_ = 1.0;
    }

    bool engaged() const noexcept { return engaged_; }
    double morphAlpha() const noexcept { return morphAlpha_; }
    // Morph terminado: coeficientes activos == objetivo.
    bool morphComplete() const noexcept { return engaged_ && morphAlpha_ >= 1.0; }

    // Compila las FIR desde el espectro TΩ (matriz 2x2 por bin, 257 bins).
    // Ramas: [0]=m00 (L->L), [1]=m01 (R->L), [2]=m10 (L->R), [3]=m11 (R->R).
    void updateFromCompiledSpectrum(
        const std::array<apex::ComplexMatrix2x2, apex::kMaxUnifiedBins>& m) noexcept {
        for (int branch = 0; branch < 4; ++branch) {
            // Buffer hermítico de 512 puntos: DC y Nyquist forzados reales,
            // espejo conjugado para k = 1..255.
            scratch_.fill(std::complex<double>(0.0, 0.0));
            for (int k = 0; k < kFftBinsD10; ++k) {
                const std::complex<double> v = branchValue(m[static_cast<std::size_t>(k)], branch);
                if (k == 0 || k == kFftBinsD10 - 1) {
                    scratch_[static_cast<std::size_t>(k)] =
                        std::complex<double>(std::isfinite(v.real()) ? v.real() : 0.0, 0.0);
                } else {
                    scratch_[static_cast<std::size_t>(k)] =
                        std::complex<double>(std::isfinite(v.real()) ? v.real() : 0.0,
                                             std::isfinite(v.imag()) ? v.imag() : 0.0);
                    scratch_[static_cast<std::size_t>(kFftSizeD10 - k)] = std::conj(scratch_[static_cast<std::size_t>(k)]);
                }
            }
            apex::fftRadix2Double(scratch_.data(), kFftSizeD10, /*inverse=*/true);
            // Taps 0..kMatrixFirTaps-1 con DESPLAZAMIENTO CIRCULAR de
            // kFirCenterShift: hTarget[n] = IFFT[(n - shift) mod 512].
            // Sin desplazar, los taps anti-causales h[-256..-1] quedan
            // envueltos al final del buffer con fase errónea fuera de
            // rejilla (ver comentario de kMatrixFirTaps). Con el
            // desplazamiento, la FIR es W(w) · e^{-jw·shift}: retardo
            // puro en serie, magnitudes exactas. Sin ventana: la IFFT
            // es completa (kMatrixFirTaps == kFftSizeD10); una ventana
            // rompería la exactitud en los bins. Si algún día se vuelve
            // a una FIR más corta que la FFT, la ventana de coseno
            // elevado en los últimos kFirWindowTaps taps suaviza el
            // truncado (código preservado abajo, inactivo con 512 taps).
            for (int n = 0; n < kMatrixFirTaps; ++n) {
                const int src = (n - kFirCenterShift + kFftSizeD10) % kFftSizeD10;
                double tap = scratch_[static_cast<std::size_t>(src)].real();
                if (!std::isfinite(tap)) tap = 0.0;
                if (kMatrixFirTaps < kFftSizeD10) {
                    const int winStart = kMatrixFirTaps - kFirWindowTaps;
                    if (n >= winStart) {
                        const double win = 0.5 * (1.0 + std::cos(kPiD10 * static_cast<double>(n - winStart) / static_cast<double>(kFirWindowTaps)));
                        tap *= win;
                    }
                }
                hTarget_[static_cast<std::size_t>(branch)][static_cast<std::size_t>(n)] = tap;
            }
        }
        morphAlpha_ = 0.0;
        engaged_ = true;
    }

    // Convolución matricial in-place. No-op si !engaged_.
    // morphSamples: muestras para el morph hActive_ -> hTarget_.
    void process(double* l, double* r, int frames, double morphSamples = 512.0) noexcept {
        if (!l || !r || frames <= 0 || !engaged_) return;
        const double morphStep = 1.0 / std::max(1.0, morphSamples);
        for (int i = 0; i < frames; ++i) {
            const double inL = std::isfinite(l[i]) ? l[i] : 0.0;
            const double inR = std::isfinite(r[i]) ? r[i] : 0.0;
            ringL_[static_cast<std::size_t>(wp_)] = inL;
            ringR_[static_cast<std::size_t>(wp_)] = inR;
            // Morph: interpola hActive_ hacia hTarget_ por tap; al llegar
            // a 1.0 copia hTarget_ a hActive_ (exacto). En régimen
            // (morphAlpha_ == 1.0) este bloque se salta entero.
            if (morphAlpha_ < 1.0) {
                morphAlpha_ = std::min(1.0, morphAlpha_ + morphStep);
                for (int b = 0; b < 4; ++b) {
                    for (int t = 0; t < kMatrixFirTaps; ++t) {
                        const std::size_t bi = static_cast<std::size_t>(b);
                        const std::size_t ti = static_cast<std::size_t>(t);
                        hActive_[bi][ti] += morphStep * (hTarget_[bi][ti] - hActive_[bi][ti]);
                    }
                }
                if (morphAlpha_ >= 1.0) hActive_ = hTarget_;
            }
            double outL = 0.0;
            double outR = 0.0;
            for (int t = 0; t < kMatrixFirTaps; ++t) {
                const int idx = (wp_ - t + kMatrixFirTaps) & (kMatrixFirTaps - 1);
                const std::size_t ri = static_cast<std::size_t>(idx);
                const std::size_t ti = static_cast<std::size_t>(t);
                outL += hActive_[0][ti] * ringL_[ri] + hActive_[1][ti] * ringR_[ri];
                outR += hActive_[2][ti] * ringL_[ri] + hActive_[3][ti] * ringR_[ri];
            }
            l[i] = outL;
            r[i] = outR;
            wp_ = (wp_ + 1) & (kMatrixFirTaps - 1);
        }
    }

private:
    static inline constexpr double kPiD10 = 3.14159265358979323846;

    static std::complex<double> branchValue(const apex::ComplexMatrix2x2& m, int branch) noexcept {
        switch (branch) {
            case 0: return m.m00;
            case 1: return m.m01;
            case 2: return m.m10;
            default: return m.m11;
        }
    }

    // [rama][tap]: 0=m00, 1=m01, 2=m10, 3=m11.
    std::array<std::array<double, kMatrixFirTaps>, 4> hActive_{};
    std::array<std::array<double, kMatrixFirTaps>, 4> hTarget_{};
    std::array<double, kMatrixFirTaps> ringL_{};
    std::array<double, kMatrixFirTaps> ringR_{};
    // Scratch MIEMBRO para la IFFT (no un array grande en pila).
    std::array<std::complex<double>, kFftSizeD10> scratch_{};
    int wp_ = 0;
    double morphAlpha_ = 1.0;
    bool engaged_ = false;
};

// ═══════════════════════════════════════════════════════════════════════════
// 3) Motor árbitro D10
// ═══════════════════════════════════════════════════════════════════════════

struct D10Config {
    bool enabled = true;               // DEFAULT TRUE: orden "todo prendido"
    // LEGACY DESACTIVADO (fix-arbiter 2026-11-25): la corrección de
    // ganancia de energía por muestra con slew de 18 ms. Medido con ella
    // ON: +2.34 dB que rompía P0ReferenceLock.Levels/TruePeak, pisos
    // degradados 10-101 dB (audífonos -91.34 -> -81.06 dB, ORBE+ARMONÍA
    // -180.80 -> -79.31 dB) y Matched-LUFS en -17.86 dB pese a corregir
    // (su tope ±3 dB no cierra la brecha real de ~18 dB). Con ella OFF
    // (default) arbitrateAction SOLO observa (térmico) y calcula
    // telemetría: NO toca el buffer. Reactivarla exige medir de nuevo
    // pisos + P0 + bandas; no es un flag para encender en caliente.
    bool energyCorrectionEnabled = false;
    double actionWeight = 0.75;        // peso de la acción hamiltoniana [0, 1]
    double listenerDistanceM = 3.0;    // posición de escucha de la sala (m)
    double phaseCoherenceTarget = 0.85; // coherencia L/R objetivo [0, 1]
    double thermalProtectionFloor = 0.15; // piso de protección térmica [0, 0.9]
};

struct D10Telemetry {
    double hamiltonianAction = 0.0;      // acción D10 del bloque
    double phaseCoherenceIndex = 0.0;    // |coherencia L/R| del bloque
    double lufsDeltaDb = 0.0;            // 10*log10(eOut/eDry)
    double thermalHeadroomScale = 1.0;   // escala térmica aplicada (<= 1)
    double continuousTonalAmount = 0.0;  // amount tonal continuo (eco)
    bool symplecticLocked = false;       // kernel LTI enganchado, morph completo
};

class TenthDimensionActionEngine {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate_ = (std::isfinite(sampleRate) && sampleRate > 0.0) ? sampleRate : 48000.0;
        thermalObserver_.prepare(sampleRate_, TransducerPhysicalSpec{});
        ltiKernel_.prepare();
        reset();
    }

    void reset() noexcept {
        thermalObserver_.reset();
        ltiKernel_.reset();
        smoothEnergyGain_ = 1.0;
        smoothPhaseBlend_ = 0.0;
        telemetry_ = D10Telemetry{};
    }

    void setConfig(const D10Config& cfg) noexcept {
        config_.enabled = cfg.enabled;
        config_.energyCorrectionEnabled = cfg.energyCorrectionEnabled;
        config_.actionWeight = std::clamp(cfg.actionWeight, 0.0, 1.0);
        config_.listenerDistanceM = std::clamp(cfg.listenerDistanceM, 0.3, 10.0);
        config_.phaseCoherenceTarget = std::clamp(cfg.phaseCoherenceTarget, 0.0, 1.0);
        config_.thermalProtectionFloor = std::clamp(cfg.thermalProtectionFloor, 0.0, 0.9);
    }

    const D10Config& config() const noexcept { return config_; }
    bool isEnabled() const noexcept { return config_.enabled; }
    TimeInvariantMatrixKernel2x2& ltiKernel() noexcept { return ltiKernel_; }
    const TimeInvariantMatrixKernel2x2& ltiKernel() const noexcept { return ltiKernel_; }
    ThermalMechanicalObserver64& thermalObserver() noexcept { return thermalObserver_; }
    const ThermalMechanicalObserver64& thermalObserver() const noexcept { return thermalObserver_; }
    const D10Telemetry& telemetry() const noexcept { return telemetry_; }

    // ITD acoplado por la LEY DE CURVATURA verificada (ORBE). NO usa la
    // fórmula inventada del documento de Gemini.
    double coupledApexItdSeconds(double thetaRad) const noexcept {
        return propagation::curvatureLawItdSeconds(
            thetaRad, std::max(0.3, config_.listenerDistanceM));
    }

    // Árbitro D10 in-place sobre outL/outR. refDryL/refDryR son opcionales
    // (pueden ser nulos: sin referencia seca no hay medida LUFS, solo
    // protección térmica y telemetría). No-op si !enabled.
    //
    // FIX-ARBITER (2026-11-25): con energyCorrectionEnabled=false
    // (default) esta función NO TOCA EL BUFFER: solo observa el estado
    // térmico y calcula la telemetría (acción, coherencia, lufsDelta).
    // La aplicación de ganancia por muestra con slew queda como código
    // LEGACY detrás del flag (ver paso 5): medir pisos + P0 + bandas
    // antes de reactivarla. La telemetría NO depende de la ganancia:
    // lufsDelta/acción se miden pre-ganancia, así que con el flag OFF
    // los tests de observación (térmico, convergencia de acción, P7
    // Ξ_10) miden lo mismo que con el flag ON.
    void arbitrateAction(double* outL, double* outR,
                         const double* refDryL, const double* refDryR,
                         int frames, double tonalNormalizedAmount) noexcept {
        if (!config_.enabled || !outL || !outR || frames <= 0) return;
        // (1) Estado térmico del bloque (observa la salida real).
        const TransducerThermalState th = thermalObserver_.observeBlock(outL, outR, frames);

        // (2) Energías y coherencia L/R (saneando no-finito a 0).
        double eDry = 0.0, eOut = 0.0, crossLR = 0.0, normL = 0.0, normR = 0.0;
        const bool haveDry = (refDryL != nullptr) && (refDryR != nullptr);
        for (int i = 0; i < frames; ++i) {
            const double oL = std::isfinite(outL[i]) ? outL[i] : 0.0;
            const double oR = std::isfinite(outR[i]) ? outR[i] : 0.0;
            eOut += oL * oL + oR * oR;
            crossLR += oL * oR;
            normL += oL * oL;
            normR += oR * oR;
            if (haveDry) {
                const double dL = std::isfinite(refDryL[i]) ? refDryL[i] : 0.0;
                const double dR = std::isfinite(refDryR[i]) ? refDryR[i] : 0.0;
                eDry += dL * dL + dR * dR;
            }
        }
        const double invFrames = 1.0 / static_cast<double>(frames);
        eDry *= invFrames;
        eOut *= invFrames;
        crossLR *= invFrames;
        normL *= invFrames;
        normR *= invFrames;
        const double coh = std::clamp(
            crossLR / std::sqrt(std::max(1e-24, normL * normR)), -1.0, 1.0);
        const double absCoh = std::fabs(coh);

        // (3) Ganancia objetivo: corrección LUFS acotada a ±3 dB, con
        // protección térmica multiplicativa.
        double targetGain = 1.0;
        double lufsDeltaDb = 0.0;
        double thermalScale = 1.0;
        if (haveDry && eDry > 1e-18 && eOut > 1e-18) {
            const double ratio = std::sqrt(eDry / eOut);
            lufsDeltaDb = 10.0 * std::log10(eOut / eDry);
            const double boundedRatio = std::clamp(ratio, 0.7079, 1.4125); // ±3 dB
            targetGain = 1.0 + config_.actionWeight * (boundedRatio - 1.0);
        }
        if (th.normalizedExcursionPeak > 0.92) {
            thermalScale *= std::clamp(0.92 / th.normalizedExcursionPeak,
                                       1.0 - config_.thermalProtectionFloor, 1.0);
        }
        if (th.coilTempC > 125.0) {
            thermalScale *= std::clamp(1.0 - 0.12 * ((th.coilTempC - 125.0) / 20.0),
                                       0.85, 1.0);
        }
        targetGain *= thermalScale;

        // (4) Acción hamiltoniana del bloque.
        const double action =
            std::pow(std::fabs(lufsDeltaDb) / 6.0, 2.0) +
            0.5 * std::pow(std::max(0.0, config_.phaseCoherenceTarget - absCoh), 2.0) +
            std::pow(std::max(0.0, th.normalizedExcursionPeak - 0.85), 2.0);

        // (5) LEGACY — aplicación por muestra con slew (tau 18 ms).
        // DESACTIVADA por defecto (energyCorrectionEnabled=false):
        // la ganancia por muestra modulaba el tono y degradaba los pisos
        // (fix-arbiter 2026-11-25). Solo corre si el flag legacy se
        // enciende explícitamente; smoothEnergyGain_ se queda en 1.0 si
        // no. El targetGain y thermalScale de arriba se siguen
        // calculando para telemetría aunque la ganancia no se aplique.
        if (config_.energyCorrectionEnabled) {
            const double slewCoeff = 1.0 - std::exp(-1.0 / (0.018 * sampleRate_));
            for (int i = 0; i < frames; ++i) {
                smoothEnergyGain_ += slewCoeff * (targetGain - smoothEnergyGain_);
                outL[i] *= smoothEnergyGain_;
                outR[i] *= smoothEnergyGain_;
            }
        }

        // (6) Telemetría.
        telemetry_.hamiltonianAction = action;
        telemetry_.phaseCoherenceIndex = absCoh;
        telemetry_.lufsDeltaDb = lufsDeltaDb;
        telemetry_.thermalHeadroomScale = thermalScale;
        telemetry_.continuousTonalAmount =
            std::clamp(tonalNormalizedAmount, 0.0, 1.0);
        // Simpléctico bloqueado = kernel LTI enganchado con morph completo.
        telemetry_.symplecticLocked = ltiKernel_.morphComplete();
    }

private:
    D10Config config_{};
    double sampleRate_ = 48000.0;
    ThermalMechanicalObserver64 thermalObserver_{};
    TimeInvariantMatrixKernel2x2 ltiKernel_{};
    double smoothEnergyGain_ = 1.0;
    double smoothPhaseBlend_ = 0.0; // reservado: mezcla de fase (uso futuro)
    D10Telemetry telemetry_{};
};

}  // namespace ivanna::fused::d10
