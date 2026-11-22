// ─────────────────────────────────────────────────────────────────────────────
// omega_sigma_operator.hpp — Física 2: Ω (Firma/Diafonía) + Σ (Escala/Alto SPL).
// Calcula por banda ERB (en Reloj 2, double 64-bit) la curva restringida Ω
// (Harman 2018 − H_b con water-filling), la pre-compensación de diafonía
// acotada a D_max(κ), la ley de escala Σ (σ = D/a, γ(σ), aire ISO 9613-1,
// compresión coclear α(L)), y la reconstrucción de fase mínima cepstral para
// cero pre-ringing. Paquete FISICA_NUEVA_CONSOLIDADA (Esse, 2026-11-18).
// NOTA RT: buildMinimumPhaseSpectrum usa std::vector — se ejecuta SOLO al
// cambiar controles (compileDirty_), nunca por hop; el camino por hop es
// cero-alloc. La primera compilación tras un cambio de controles cuesta
// ~O(N²) con N=512 (medir; ver informe de integración v2.5.472).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "new_physics_controls.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <vector>

namespace ivanna::newphys {

struct Band24Profile {
    static constexpr int kBands = 24;
    // Centros ERB estándar de ORBE/HOLOS (60 Hz .. 16 kHz)
    static constexpr std::array<double, kBands> kCentersHz{{
        72.0, 102.0, 136.0, 174.0, 217.0, 266.0, 321.0, 384.0,
        455.0, 535.0, 626.0, 729.0, 845.0, 977.0, 1126.0, 1295.0,
        1486.0, 1703.0, 1948.0, 2226.0, 2540.0, 2896.0, 3300.0, 11500.0
    }};
    // Curva objetivo Harman 2018 (dB relativos por banda ERB, estándar Olive et al.)
    static constexpr std::array<double, kBands> kHarman2018Db{{
        +5.2, +4.8, +4.0, +2.8, +1.5, +0.4,  0.0,  0.0,
         0.0,  0.0,  0.0,  0.0,  0.0, +0.3, +0.8, +1.5,
        +2.3, +3.0, +3.4, +3.6, +3.2, +2.4, +1.2, -1.5
    }};
};

class OmegaSigmaOperator {
public:
    static constexpr int kBands = Band24Profile::kBands;

    struct BandResult {
        double gainDb = 0.0;       // Corrección C_b + Aire + ISO226
        double ildScale = 1.0;     // Pre-expansión ILD (coclear 1/α(L) + diafonía)
        double residualErrDb = 0.0;// ε_b reportado (no se oculta)
    };

    struct FrameOutputs {
        std::array<BandResult, kBands> bands{};
        double sigErrorRmsDb = 0.0;
        double dMaxDb = 99.0;
        double gammaScale = 1.0;
        double alphaCochlear = 1.0;
    };

    OmegaSigmaOperator() {
        measuredEqDb_.fill(0.0);
        headroomDb_.fill(6.0);
    }

    // Carga la respuesta medida del equipo H_b (desde genezi_calibrate.py o sensor acústico)
    void setMeasuredEquipmentResponse(const double* hbDb, const double* headroomDb,
                                      double fLoHz, double fHiHz) noexcept {
        if (hbDb) {
            for (int b = 0; b < kBands; ++b) {
                measuredEqDb_[b] = std::isfinite(hbDb[b]) ? std::clamp(hbDb[b], -24.0, 24.0) : 0.0;
            }
            hasMeasuredHb_ = true;
        }
        if (headroomDb) {
            for (int b = 0; b < kBands; ++b) {
                headroomDb_[b] = std::isfinite(headroomDb[b]) ? std::clamp(headroomDb[b], 0.0, 18.0) : 3.0;
            }
        }
        measuredFLo_ = std::clamp(fLoHz, 15.0, 2000.0);
        measuredFHi_ = std::clamp(fHiHz, 2000.0, 22000.0);
    }

    // Defaults físicos por clase de equipo (cuando aún no se midió H_b)
    static void classDefaults(EquipmentClass eq, double& kappa, double& fLo, double& fHi) noexcept {
        switch (eq) {
            case EquipmentClass::HEADPHONES_E1:    kappa = 0.00; fLo = 20.0;  fHi = 20000.0; break;
            case EquipmentClass::PHONE_SPEAKER_E2: kappa = 0.70; fLo = 350.0; fHi = 14000.0; break;
            case EquipmentClass::BT_SPEAKER_E3:    kappa = 0.45; fLo = 90.0;  fHi = 16000.0; break;
            case EquipmentClass::HOME_STEREO_E4:   kappa = 0.30; fLo = 50.0;  fHi = 18500.0; break;
            case EquipmentClass::CAR_E6:           kappa = 0.35; fLo = 50.0;  fHi = 16000.0; break;
            case EquipmentClass::PA_CLUB_E7:       kappa = 0.20; fLo = 35.0;  fHi = 17500.0; break;
            case EquipmentClass::STADIUM_E8:       kappa = 0.20; fLo = 28.0;  fHi = 17000.0; break;
        }
    }

    FrameOutputs compute(const NewPhysicsControls& c) const noexcept {
        FrameOutputs out{};

        double kappa = 0.0, fLo = 20.0, fHi = 20000.0;
        classDefaults(c.eqClass, kappa, fLo, fHi);
        if (hasMeasuredHb_) {
            fLo = measuredFLo_;
            fHi = measuredFHi_;
        }

        // Techo de diafonía: D_max(κ) = -20*log10(κ)
        out.dMaxDb = (kappa > 1e-4) ? (-20.0 * std::log10(kappa)) : 99.0;

        // Ley de escala Σ: σ = D / a, w(σ) = 1/(1+(σ0/σ)^p), γ(σ) = 1 - 0.5*w(σ)
        const double sigma = static_cast<double>(c.sigmaDistanceM) / static_cast<double>(c.headRadiusM);
        constexpr double sigma0 = 30.0; // ~2.6 m transición binaural -> arreglo
        const double wSigma = 1.0 / (1.0 + std::pow(sigma0 / std::max(sigma, 0.5), 2.0));
        out.gammaScale = 1.0 - 0.5 * wSigma;

        // Compresión coclear del ILD dependiente de nivel α(L): forma en U suave (mínimo ~100 dB)
        const double spl = static_cast<double>(c.splEstimateDb);
        const double dSpl = (spl - 100.0) / 18.0;
        out.alphaCochlear = std::clamp(0.55 + 0.45 * (dSpl * dSpl) / (1.0 + dSpl * dSpl), 0.50, 1.0);

        double errSqSum = 0.0;
        for (int b = 0; b < kBands; ++b) {
            const double fc = Band24Profile::kCentersHz[b];
            const double targetDb = Band24Profile::kHarman2018Db[b];
            const double hb = hasMeasuredHb_ ? measuredEqDb_[b] : 0.0;

            // 1) Corrección de firma Ω restringida: nunca reforzar fuera de [f_lo, f_hi]
            double cIdeal = (targetDb - hb) * static_cast<double>(c.omegaSignatureMix);
            double cMax = hasMeasuredHb_ ? headroomDb_[b] : static_cast<double>(c.boostCapDb);
            cMax = std::min(cMax, static_cast<double>(c.boostCapDb));

            double cApplied = 0.0;
            if (fc >= fLo && fc <= fHi) {
                cApplied = std::clamp(cIdeal, -18.0, cMax);
            } else {
                cApplied = 0.0; // Ω regla §4.2: no inventar graves/agudos fuera de banda útil
            }

            // 2) Compensación de absorción del aire ISO 9613-1 aproximada (20°C, 50% HR)
            double airDb = 0.0;
            if (c.airCompMix > 1e-5f && c.sigmaDistanceM > 2.0f) {
                const double alphaAirDbPerM = 0.004 * std::pow(fc / 1000.0, 1.45);
                airDb = std::min(6.0, alphaAirDbPerM * c.sigmaDistanceM) * c.airCompMix;
            }

            const double err = targetDb - (hb + cApplied);
            errSqSum += err * err;

            // 3) Pre-expansión ILD acotada (Σ coclear + Ω diafonía)
            double ildExp = 1.0 / out.alphaCochlear;
            if (kappa > 1e-4 && c.crosstalkCompMix > 1e-5f) {
                const double maxBoostFactor = std::min(2.2, 1.0 / std::max(0.15, 1.0 - kappa));
                ildExp *= (1.0 + c.crosstalkCompMix * (maxBoostFactor - 1.0));
            }
            // OVERDRIVE v2.5.480 (Esse): el techo 2.5 se escala con el
            // xtalk cuando pasa de 1.0 (2.5 × xtalk), para que el 1.5
            // tenga recorrido real. Con xtalk ≤ 1.0 el techo sigue en
            // 2.5 exacto — el sonido magistral a 1.0 no cambia.
            const double ildCeil = (c.crosstalkCompMix > 1.0f)
                ? 2.5 * static_cast<double>(c.crosstalkCompMix) : 2.5;
            ildExp = std::clamp(ildExp, 1.0, ildCeil);

            out.bands[b].gainDb = cApplied + airDb;
            out.bands[b].ildScale = ildExp;
            out.bands[b].residualErrDb = err;
        }

        out.sigErrorRmsDb = std::sqrt(errSqSum / static_cast<double>(kBands));
        return out;
    }

    // Convierte un perfil de magnitud |H[k]| (N/2+1 bins) en respuesta compleja de
    // FASE MÍNIMA EXACTA mediante Cepstrum real en 64-bit double (cero pre-ringing).
    static void buildMinimumPhaseSpectrum(const double* magLinearHalf, int fftSize,
                                          std::complex<double>* outHalf) noexcept {
        const int halfBins = fftSize / 2 + 1;
        // 1) Log-magnitud simétrica (en N puntos)
        std::vector<double> logMag(fftSize, 0.0);
        for (int k = 0; k < halfBins; ++k) {
            const double m = std::max(magLinearHalf[k], 1e-9);
            logMag[k] = std::log(m);
            if (k > 0 && k < fftSize / 2) logMag[fftSize - k] = logMag[k];
        }
        // 2) IDFT real -> Cepstrum c[n] (solo corre al cambiar parámetros, Reloj 2)
        std::vector<double> cep(fftSize, 0.0);
        const double invN = 1.0 / fftSize;
        for (int n = 0; n < fftSize; ++n) {
            double s = 0.0;
            for (int k = 0; k < fftSize; ++k) {
                s += logMag[k] * std::cos(2.0 * M_PI * k * n * invN);
            }
            cep[n] = s * invN;
        }
        // 3) Ventana causal de fase mínima: c[0], 2*c[1..N/2-1], c[N/2], 0 para n>N/2
        for (int n = 1; n < fftSize / 2; ++n) cep[n] *= 2.0;
        for (int n = fftSize / 2 + 1; n < fftSize; ++n) cep[n] = 0.0;

        // 4) DFT de cepstrum causal -> exp(Re + j*Im) con simetría hermítica exacta en 0 y N/2
        for (int k = 0; k < halfBins; ++k) {
            double re = 0.0, im = 0.0;
            for (int n = 0; n < fftSize / 2 + 1; ++n) {
                const double ang = -2.0 * M_PI * k * n * invN;
                re += cep[n] * std::cos(ang);
                im += cep[n] * std::sin(ang);
            }
            const double mag = std::exp(re);
            const double phase = (k == 0 || k == fftSize / 2) ? 0.0 : im;
            outHalf[k] = std::polar(mag, phase);
        }
    }

private:
    std::array<double, kBands> measuredEqDb_{};
    std::array<double, kBands> headroomDb_{};
    double measuredFLo_ = 20.0;
    double measuredFHi_ = 20000.0;
    bool hasMeasuredHb_ = false;
};

} // namespace ivanna::newphys
