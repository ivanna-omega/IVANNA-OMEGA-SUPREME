// ─────────────────────────────────────────────────────────────────────────────
// new_physics_engine.hpp — MOTOR UNIFICADO OMNIPOTENTE DE FÍSICA NUEVA
// Fusión total RT-safe (NewPhysicsEngine v3 — Fusión Total):
//   1) WOLA 512 empaquetada (L+jR) alineada al sample exacto (512 = dryDelay)
//   2) Fase mínima Kramers-Kronig C0 (interpolación log-frecuencia sin Gibbs)
//   3) LGN 3D + Bright-Spot + ILD con conservación exacta de energía
//   4) Operador S 2x2 de Diafonía en base de Paridad (±) con cota Tikhonov
//   5) Cola MIMO 2x2 de Sala UPOLA (24 particiones = 64 ms, 0 latencia extra)
//   6) Solver integrado Kirkeby-Nelson 2x2 (para IR medidas por Farina ESS)
//   7) Banco Modal Sub-100 Hz IIR en 64-bit double (resuelve 68.5 Hz, 0 lat.)
//   8) Klippel Volterra H2 + H3 O(M) Laguerre + Red Térmica Dual-RC (Tv, Tm)
//   9) Lazo variacional F (MOC + Onsets) independiente de sampleRate
//  10) Crossfade matricial anti-zipper + rodilla suave anti-clip en Overdrive
// Bypass bit-exacto fail-closed intacto. Cero malloc en process().
//
// CORRECCIONES DE VERIFICACIÓN CONTRA EL CÓDIGO VIVO (2026-11-20, v2.5.482):
//  [V1] Tabla κ E7/E8 = 0.20/0.20 (no 0.15/0.10): es lo que dice
//       OmegaSigmaOperator::classDefaults (omega_sigma_operator.hpp:88-89).
//  [V2] H2: los términos cruzados (a<b) llevan factor ×2, igual que
//       LaguerreVolterraH2::processBlock y que el fit de
//       tools/genezi_calibrate.py. Sin él, el kernel medido corrige la mitad.
//  [V3] Polo Laguerre Genezi = exp(-1/(0.0125·fs)) ≈ 0.9983 @48k (no 0.72):
//       es el polo con el que genezi_calibrate.py ajusta el kernel K2.
//       loadGeneziVolterra() lo deriva de sampleRate_; prepare() lo
//       re-deriva si hay kernel Genezi cargado (geneziKernel_).
//  [V4] LPF del excitador a 1500 Hz (no 450 Hz): es el one_pole_lowpass_1500
//       de genezi_calibrate.py, el mismo que veía el kernel al ajustarse.
//  [V5] kSpanDegTable: ángulos SUPUESTOS (no medidos); solo fijan tauXtalk.
//       Si se miden los spans reales, se actualiza la tabla.
// NOTA: la ganancia térmica (thermalGain, hasta +1.4 dB en caliente) y la
// rodilla suave (>0.98) solo actúan con kernel cargado / sobre 0.98; hoy el
// teléfono NO tiene kernel Genezi cargado, así que ambas están dormidas.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "free_energy_loop.hpp"
#include "geodesic_pinna_3d.hpp"
#include "new_physics_controls.hpp"
#include "omega_sigma_operator.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace ivanna::newphys {

class NewPhysicsEngine {
public:
    static constexpr int kHopSize   = 128;
    static constexpr int kFftSize   = 512;
    static constexpr int kHalfBins  = kFftSize / 2 + 1; // 257
    static constexpr int kOlsFft    = 256;
    static constexpr int kOlsBins   = kOlsFft / 2 + 1;  // 129
    static constexpr int kRoomParts = 24;               // 24 * 128 = 3072 taps (64 ms @ 48k)
    static constexpr int kLagOrder  = 6;
    static constexpr int kLagTri21  = (kLagOrder * (kLagOrder + 1)) / 2; // 21
    static constexpr int kSubStages = 6;
    static constexpr double kPi     = 3.14159265358979323846;

    struct SubBassStageParams {
        double freqHz   = 68.5;
        double q        = 3.0;
        double gainDb   = 0.0;
        bool   lowShelf = false;
        bool   active   = false;
    };

    void prepare(float sampleRate) noexcept {
        sampleRate_ = (sampleRate >= 8000.0f && sampleRate <= 384000.0f) ? sampleRate : 48000.0f;

        freeEnergy_.prepare(sampleRate_, kHopSize);
        prepareSubBassIir();
        prepareKlippelVolterra();

        // Ventana Hann periódica + normalización COLA exacta: 512/128 * 0.375 = 1.5
        for (int i = 0; i < kFftSize; ++i) {
            win_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(kPi) * i / kFftSize));
        }
        colaInv_ = 1.0f / ((static_cast<float>(kFftSize) / kHopSize) * 0.375f);

        // Pertenencia bin -> banda ERB [0..23]
        for (int k = 0; k < kHalfBins; ++k) {
            const double f = static_cast<double>(k) * sampleRate_ / kFftSize;
            int bestB = 0;
            double bestD = 1e12;
            for (int b = 0; b < 24; ++b) {
                const double d = std::abs(f - Band24Profile::kCentersHz[b]);
                if (d < bestD) { bestD = d; bestB = b; }
            }
            binToBand_[k] = bestB;
        }

        // Twiddles 512 y 256 (cero alloc en RT)
        for (int k = 0; k < kFftSize / 2; ++k) {
            const double ang = -2.0 * kPi * k / kFftSize;
            twiddle512_[k] = std::complex<float>(std::cos(ang), std::sin(ang));
        }
        for (int k = 0; k < kOlsFft / 2; ++k) {
            const double ang = -2.0 * kPi * k / kOlsFft;
            twiddle256_[k] = std::complex<float>(std::cos(ang), std::sin(ang));
        }

        // Pre-reservar memoria FDL de sala UPOLA fuera del hilo RT
        const size_t totalFdl = static_cast<size_t>(kRoomParts) * kOlsBins;
        fdlL_.assign(totalFdl, {0.0f, 0.0f});
        fdlR_.assign(totalFdl, {0.0f, 0.0f});
        hRoomLL_.assign(totalFdl, {0.0f, 0.0f});
        hRoomLR_.assign(totalFdl, {0.0f, 0.0f});
        hRoomRL_.assign(totalFdl, {0.0f, 0.0f});
        hRoomRR_.assign(totalFdl, {0.0f, 0.0f});

        // Constante de tiempo envolvente independiente de fs (~16.4 ms)
        envAlpha_ = std::exp(-static_cast<float>(kHopSize) / (0.0164f * sampleRate_));

        reset();
        prepared_ = true;
    }

    void reset() noexcept {
        inRingL_.fill(0.0f);
        inRingR_.fill(0.0f);
        olaL_.fill(0.0f);
        olaR_.fill(0.0f);
        dryDelayL_.fill(0.0f);
        dryDelayR_.fill(0.0f);
        wetHopL_.fill(0.0f);
        wetHopR_.fill(0.0f);
        bandEnv_.fill(0.0f);
        bandTrans_.fill(0.0f);
        feGain_.fill(1.0f);

        std::fill(fdlL_.begin(), fdlL_.end(), std::complex<float>{0.0f, 0.0f});
        std::fill(fdlR_.begin(), fdlR_.end(), std::complex<float>{0.0f, 0.0f});
        roomCarryL_.fill(0.0f);
        roomCarryR_.fill(0.0f);
        fdlHead_ = 0;

        ringPos_ = 0;
        olaPos_  = 0;
        dryPos_  = 0;
        hopFill_ = 0;
        frameIdx_ = 0;
        smoothWet_ = 0.0f;
        matMorph_ = 1.0f;

        resetSubBassIir();
        resetKlippelVolterra();
        freeEnergy_.reset();
        compileDirty_ = true;
    }

    void setControls(const NewPhysicsControls& raw) noexcept {
        NewPhysicsControls c = raw;
        c.clamp();
        if (!sameMatrixControls(c, controls_)) {
            compileDirty_ = true;
        }
        controls_ = c;
    }

    // Compatibilidad exacta con ABI existente (carga H2 Genezi).
    // [V3] El kernel Genezi se ajustó con polo exp(-1/(0.0125·fs)) ≈ 0.9983
    // @48k (tools/genezi_calibrate.py); usar otro polo lo invalidaría.
    bool loadGeneziVolterra(const float* tri21L, const float* tri21R) noexcept {
        const float pole = std::exp(-1.0f / (0.0125f * sampleRate_));
        const bool ok = loadKlippelH2H3(tri21L, tri21R, nullptr, nullptr, 0.0f, 0.0f, pole);
        geneziKernel_ = ok;
        telemetry_.kernelLoaded = ok ? 1 : 0;
        return ok;
    }

    // Extensión Klippel completa: H2 (21 términos) + H3 simétrico O(M).
    // El polo por defecto (0.72) es para kernels Klippel genéricos, NO para
    // el Genezi (que pasa por loadGeneziVolterra con su polo calibrado).
    //
    // v2.5.483 — setKlippelH3: carga SOLO los coeficientes H3 (diagonal
    // Laguerre c3[6] + término cúbico sin memoria alpha3) sin tocar H2, polo
    // ni geneziKernel_. Fail-closed: punteros nulos o no finitos -> H3 a 0
    // (órgano apagado, nunca crash). Los valores por defecto de la 483 son
    // los APROXIMADOS de np483_defaults.hpp (H3 ≈ 0.20% @85 dB SPL, típico
    // de bocina pequeña; ver ~/workspace/np483/APPROXIMATIONS.md).
    bool setKlippelH3(const float* h3DiagL, const float* h3DiagR,
                      float alpha3L, float alpha3R) noexcept {
        for (int m = 0; m < kLagOrder; ++m) {
            c3L_[m] = (h3DiagL && std::isfinite(h3DiagL[m]))
                          ? std::clamp(h3DiagL[m], -0.15f, 0.15f) : 0.0f;
            c3R_[m] = (h3DiagR && std::isfinite(h3DiagR[m]))
                          ? std::clamp(h3DiagR[m], -0.15f, 0.15f) : 0.0f;
        }
        alpha3L_ = std::isfinite(alpha3L) ? std::clamp(alpha3L, -0.25f, 0.25f) : 0.0f;
        alpha3R_ = std::isfinite(alpha3R) ? std::clamp(alpha3R, -0.25f, 0.25f) : 0.0f;
        return true;
    }

    // v2.5.483 — mezcla H3 (0..1, default 1.0 = H3 completo). 0 = H3 off
    // sin tocar H2. NaN -> 1.0 (default, fail-closed).
    void setKlippelH3Mix(float mix) noexcept {
        h3Mix_ = std::isfinite(mix) ? std::clamp(mix, 0.0f, 1.0f) : 1.0f;
    }

    // v2.5.483 — mezcla de cola de sala (0..1, default 1.0 en carga).
    // NaN -> 0.0 (cola off, fail-closed).
    void setRoomTailMix(float mix) noexcept {
        roomTailMix_ = std::isfinite(mix) ? std::clamp(mix, 0.0f, 1.0f) : 0.0f;
    }

    // v2.5.483 — telemetría de órganos para la UI (lecturas puras).
    bool roomTailActive() const noexcept { return roomTailActive_; }
    float roomTailMix() const noexcept { return roomTailMix_; }
    float klippelH3Mix() const noexcept { return h3Mix_; }
    bool geneziKernel() const noexcept { return geneziKernel_; }
    uint32_t subBassActiveMask() const noexcept {
        uint32_t mask = 0;
        for (int i = 0; i < kSubStages; ++i)
            if (subStages_[i].enabled) mask |= (1u << i);
        return mask;
    }
    bool loadKlippelH2H3(const float* tri21L, const float* tri21R,
                         const float* h3DiagL, const float* h3DiagR,
                         float alpha3L, float alpha3R, float pole = 0.72f) noexcept {
        if (!tri21L || !tri21R) return false;
        double sumL = 0.0, sumR = 0.0;
        for (int i = 0; i < kLagTri21; ++i) {
            if (!std::isfinite(tri21L[i]) || !std::isfinite(tri21R[i])) return false;
            sumL += std::abs(tri21L[i]);
            sumR += std::abs(tri21R[i]);
        }
        if (sumL > 0.35 || sumR > 0.35) return false;

        for (int i = 0; i < kLagTri21; ++i) {
            c2L_[i] = tri21L[i];
            c2R_[i] = tri21R[i];
        }
        for (int m = 0; m < kLagOrder; ++m) {
            c3L_[m] = (h3DiagL && std::isfinite(h3DiagL[m])) ? std::clamp(h3DiagL[m], -0.15f, 0.15f) : 0.0f;
            c3R_[m] = (h3DiagR && std::isfinite(h3DiagR[m])) ? std::clamp(h3DiagR[m], -0.15f, 0.15f) : 0.0f;
        }
        alpha3L_ = std::isfinite(alpha3L) ? std::clamp(alpha3L, -0.25f, 0.25f) : 0.0f;
        alpha3R_ = std::isfinite(alpha3R) ? std::clamp(alpha3R, -0.25f, 0.25f) : 0.0f;
        // [V6] El tope 0.92 CAPABA el polo Genezi calibrado (0.9983 @48k):
        // loadGeneziVolterra pasaba 0.9983 y el clamp lo dejaba en 0.92,
        // con lo que el kernel medido corría con otro banco (pico 0.059 en
        // vez de 0.099, verificado contra modelo Python). 0.9995 lo admite
        // y sigue siendo estable (< 1.0).
        lagPole_ = std::clamp(pole, 0.2f, 0.9995f);
        lagSqrt1mp2_ = std::sqrt(1.0f - lagPole_ * lagPole_);
        klippelLoaded_ = true;
        geneziKernel_ = false; // carga genérica: el polo Genezi no aplica
        telemetry_.kernelLoaded = 1;
        return true;
    }

    void setMeasuredEquipment(const double* hbDb, const double* headroomDb,
                              double fLoHz, double fHiHz) noexcept {
        omegaSigma_.setMeasuredEquipmentResponse(hbDb, headroomDb, fLoHz, fHiHz);
        compileDirty_ = true;
    }

    // Configura modos de sala/transductor sub-120 Hz manuales (etapas 2..5; 0-1 reservadas a Omega diferencial)
    void setSubBassModalStage(int idx, const SubBassStageParams& p) noexcept {
        if (idx < 0 || idx >= kSubStages) return;
        subParams_[idx] = p;
        compileSubBassStage(idx);
    }

    // Carga cola de respuesta MIMO 2x2 de sala medida (t >= 128 muestras -> UPOLA sin latencia extra)
    bool loadMeasuredRoomMimoTail(const float* irLL, const float* irLR,
                                  const float* irRL, const float* irRR,
                                  int totalLen, float tailMix = 1.0f) noexcept {
        roomTailActive_ = false;
        roomTailMix_ = std::clamp(tailMix, 0.0f, 1.0f);
        if (!prepared_ || !irLL || !irRR || totalLen <= kHopSize) return false;

        std::fill(hRoomLL_.begin(), hRoomLL_.end(), std::complex<float>{0.0f, 0.0f});
        std::fill(hRoomLR_.begin(), hRoomLR_.end(), std::complex<float>{0.0f, 0.0f});
        std::fill(hRoomRL_.begin(), hRoomRL_.end(), std::complex<float>{0.0f, 0.0f});
        std::fill(hRoomRR_.begin(), hRoomRR_.end(), std::complex<float>{0.0f, 0.0f});

        const int avail = std::min(totalLen - kHopSize, kRoomParts * kHopSize);
        const int usedParts = (avail + kHopSize - 1) / kHopSize;
        std::array<std::complex<float>, kOlsFft> buf{};

        auto encode = [&](const float* src, std::vector<std::complex<float>>& dst) {
            if (!src) return;
            for (int p = 0; p < usedParts; ++p) {
                buf.fill({0.0f, 0.0f});
                const int off = kHopSize + p * kHopSize;
                const int cnt = std::min(kHopSize, totalLen - off);
                for (int i = 0; i < cnt; ++i) buf[i] = {src[off + i], 0.0f};
                fft256(buf.data(), false);
                const size_t base = static_cast<size_t>(p) * kOlsBins;
                for (int k = 0; k < kOlsBins; ++k) dst[base + k] = buf[k];
            }
        };

        encode(irLL, hRoomLL_);
        encode(irLR, hRoomLR_);
        encode(irRL, hRoomRL_);
        encode(irRR, hRoomRR_);
        roomTailActive_ = true;
        return true;
    }

    // Solver MIMO 2x2 Kirkeby-Nelson con perfil BACCH en frecuencia para calibración in-situ
    // Entrada: planta medida C(k) de tamaño N/2+1. Salida: filtro inverso W(k) acotado en norma.
    static void solveKirkebyMimoBin(const std::complex<double>& cLL, const std::complex<double>& cLR,
                                    const std::complex<double>& cRL, const std::complex<double>& cRR,
                                    double betaReg, const std::complex<double>& targetDelayPhase,
                                    std::complex<double>& wLL, std::complex<double>& wLR,
                                    std::complex<double>& wRL, std::complex<double>& wRR) noexcept {
        const double reg = std::max(betaReg, 1e-9);
        // A = C^H * C + beta * I (Hermítica definida positiva)
        const double a00 = std::norm(cLL) + std::norm(cRL) + reg;
        const double a11 = std::norm(cLR) + std::norm(cRR) + reg;
        const std::complex<double> a01 = std::conj(cLL) * cLR + std::conj(cRL) * cRR;
        const double det = std::max(a00 * a11 - std::norm(a01), reg * reg);
        const double invDet = 1.0 / det;

        // A^-1 = invDet * [[a11, -a01], [-conj(a01), a00]]
        const std::complex<double> i00{a11 * invDet, 0.0};
        const std::complex<double> i01 = -a01 * invDet;
        const std::complex<double> i10 = -std::conj(a01) * invDet;
        const std::complex<double> i11{a00 * invDet, 0.0};

        // W = A^-1 * C^H * targetDelayPhase
        const std::complex<double> ch00 = std::conj(cLL) * targetDelayPhase;
        const std::complex<double> ch01 = std::conj(cRL) * targetDelayPhase;
        const std::complex<double> ch10 = std::conj(cLR) * targetDelayPhase;
        const std::complex<double> ch11 = std::conj(cRR) * targetDelayPhase;

        wLL = i00 * ch00 + i01 * ch10;
        wLR = i00 * ch01 + i01 * ch11;
        wRL = i10 * ch00 + i11 * ch10;
        wRR = i10 * ch01 + i11 * ch11;
    }

    const NewPhysicsTelemetry& telemetry() const noexcept { return telemetry_; }
    float coilTempCL() const noexcept { return static_cast<float>(25.0 + dTvL_); }
    float coilTempCR() const noexcept { return static_cast<float>(25.0 + dTvR_); }

    void process(float* L, float* R, int n) noexcept {
        if (!prepared_ || !L || !R || n <= 0) return;

        // Bypass bit-exacto fail-closed
        if (controls_.abxOverrideMode == 1 ||
            (controls_.abxOverrideMode == 0 && controls_.isBypass() && smoothWet_ <= 1e-6f)) {
            telemetry_.cpuUsPerHop = 0.0f;
            return;
        }

        const auto t0 = std::chrono::steady_clock::now();

        if (compileDirty_) {
            prevM_ = compiledM_;
            compileStaticMatrices();
            matMorph_ = 0.0f; // crossfade suave en 4 hops
            compileDirty_ = false;
        }

        const float targetWet = (controls_.abxOverrideMode == 2) ? 1.0f : controls_.amount;
        const float slewStep  = 1.0f / (0.015f * sampleRate_);

        int processed = 0;
        while (processed < n) {
            const int take = std::min(kHopSize - hopFill_, n - processed);
            for (int i = 0; i < take; ++i) {
                float inL = L[processed + i];
                float inR = R[processed + i];
                inL = std::isfinite(inL) ? std::clamp(inL, -4.0f, 4.0f) : 0.0f;
                inR = std::isfinite(inR) ? std::clamp(inR, -4.0f, 4.0f) : 0.0f;

                const int wrIdx = (ringPos_ + kFftSize - kHopSize + hopFill_ + i) & (kFftSize - 1);
                inRingL_[wrIdx] = inL;
                inRingR_[wrIdx] = inR;

                const int dryRd = dryPos_;
                const float dL = dryDelayL_[dryRd];
                const float dR = dryDelayR_[dryRd];
                dryDelayL_[dryRd] = inL;
                dryDelayR_[dryRd] = inR;
                dryPos_ = (dryPos_ + 1) & (kFftSize - 1);

                // Leer exactamente el bloque OLA completado hace 1 hop (latencia = 512 = dryDelay)
                const int olaRd = (olaPos_ + kFftSize - kHopSize + hopFill_ + i) & (kFftSize - 1);
                const float wL = olaL_[olaRd];
                const float wR = olaR_[olaRd];
                olaL_[olaRd] = 0.0f;
                olaR_[olaRd] = 0.0f;

                if (smoothWet_ < targetWet)      smoothWet_ = std::min(targetWet, smoothWet_ + slewStep);
                else if (smoothWet_ > targetWet) smoothWet_ = std::max(targetWet, smoothWet_ - slewStep);

                L[processed + i] = dL + smoothWet_ * (wL - dL);
                R[processed + i] = dR + smoothWet_ * (wR - dR);
            }

            hopFill_ += take;
            processed += take;

            if (hopFill_ == kHopSize) {
                runStftHop();
                hopFill_ = 0;
                ringPos_ = (ringPos_ + kHopSize) & (kFftSize - 1);
                olaPos_  = (olaPos_  + kHopSize) & (kFftSize - 1);
                ++frameIdx_;
            }
        }

        // Etapa Klippel H2 + H3 + Vigilancia Térmica Dual-RC (solo si aplica o hay kernel)
        const bool speakerMode = (controls_.eqClass != EquipmentClass::HEADPHONES_E1);
        const float vMix = (klippelLoaded_ && controls_.volterraMix > 1e-6f)
                           ? std::clamp(controls_.volterraMix * smoothWet_, 0.0f, 1.0f)
                           : 0.0f;
        if (vMix > 1e-6f || (klippelLoaded_ && speakerMode)) {
            telemetry_.volterraPeakCorr =
                processKlippelBlock(L, R, n, vMix, controls_.splEstimateDb, speakerMode);
        } else {
            telemetry_.volterraPeakCorr = 0.0f;
        }

        // Rodilla suave transparente solo en extremo (> 0.98) para proteger Overdrive 1.5
        for (int i = 0; i < n; ++i) {
            L[i] = softKneeLimit(L[i]);
            R[i] = softKneeLimit(R[i]);
        }

        const auto t1 = std::chrono::steady_clock::now();
        const float us = std::chrono::duration<float, std::micro>(t1 - t0).count();
        telemetry_.cpuUsPerHop = 0.9f * telemetry_.cpuUsPerHop + 0.1f * (us * kHopSize / std::max(n, 1));
    }

private:
    struct Mat2x2 {
        std::complex<float> m00{1.0f, 0.0f}, m01{0.0f, 0.0f};
        std::complex<float> m10{0.0f, 0.0f}, m11{1.0f, 0.0f};
    };

    struct SubBassBiquad {
        bool enabled = false;
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1L = 0.0, z2L = 0.0, z1R = 0.0, z2R = 0.0;
    };

    static bool sameMatrixControls(const NewPhysicsControls& a, const NewPhysicsControls& b) noexcept {
        return a.lgnPinnaMix == b.lgnPinnaMix &&
               a.brightSpotDamp == b.brightSpotDamp &&
               a.torsoEchoMix == b.torsoEchoMix &&
               a.headRadiusM == b.headRadiusM &&
               a.azimuthDeg == b.azimuthDeg &&
               a.elevationDeg == b.elevationDeg &&
               a.eqClass == b.eqClass &&
               a.omegaSignatureMix == b.omegaSignatureMix &&
               a.boostCapDb == b.boostCapDb &&
               a.crosstalkCompMix == b.crosstalkCompMix &&
               a.sigmaDistanceM == b.sigmaDistanceM &&
               a.splEstimateDb == b.splEstimateDb &&
               a.airCompMix == b.airCompMix;
    }

    static inline float softKneeLimit(float x) noexcept {
        const float ax = std::abs(x);
        if (ax <= 0.98f) return x;
        const float sign = (x >= 0.0f) ? 1.0f : -1.0f;
        return sign * (0.98f + 0.02f * std::tanh((ax - 0.98f) / 0.02f));
    }

    void compileStaticMatrices() noexcept {
        const auto osOut = omegaSigma_.compute(controls_);
        telemetry_.signatureErrorRmsDb = static_cast<float>(osOut.sigErrorRmsDb);
        telemetry_.crosstalkCeilingDb  = static_cast<float>(osOut.dMaxDb);
        telemetry_.scaleGamma          = static_cast<float>(osOut.gammaScale);
        telemetry_.ildAlphaCompression = static_cast<float>(osOut.alphaCochlear);

        // 1) Diferencial sub-grave < 95 Hz al banco modal IIR 64-bit (etapa 0)
        //    para resolver el rango 20-94 Hz donde la bin 1 de WOLA no llega, sin doble boost.
        const double diffSubDb = std::clamp(osOut.bands[0].gainDb - osOut.bands[1].gainDb, -12.0, 9.0);
        subParams_[0] = {65.0, 0.707, diffSubDb, true, std::abs(diffSubDb) > 0.05};
        compileSubBassStage(0);

        // 2) Perfil de magnitud continuo C0 (interpolación log-frecuencia) -> Fase Mínima Kramers-Kronig
        std::array<double, kHalfBins> eqMagHalf{};
        eqMagHalf[0] = std::pow(10.0, osOut.bands[1].gainDb / 20.0);
        for (int k = 1; k < kHalfBins; ++k) {
            const double f = static_cast<double>(k) * sampleRate_ / kFftSize;
            double gDb = osOut.bands[23].gainDb;
            if (f <= Band24Profile::kCentersHz[0]) {
                gDb = osOut.bands[0].gainDb;
            } else if (f < Band24Profile::kCentersHz[23]) {
                for (int b = 0; b < 23; ++b) {
                    const double f0 = Band24Profile::kCentersHz[b];
                    const double f1 = Band24Profile::kCentersHz[b + 1];
                    if (f >= f0 && f <= f1) {
                        const double u = std::log(f / f0) / std::log(f1 / f0);
                        gDb = osOut.bands[b].gainDb + u * (osOut.bands[b + 1].gainDb - osOut.bands[b].gainDb);
                        break;
                    }
                }
            }
            eqMagHalf[k] = std::pow(10.0, gDb / 20.0);
        }

        std::array<std::complex<double>, kHalfBins> eqMinPhase{};
        OmegaSigmaOperator::buildMinimumPhaseSpectrum(eqMagHalf.data(), kFftSize, eqMinPhase.data());

        // 3) Planta de diafonía acústica C(ω) por clase de equipo + cota espectral Tikhonov
        // [V1] κ E7/E8 = 0.20/0.20: es lo que dice classDefaults
        // (omega_sigma_operator.hpp:88-89), no 0.15/0.10.
        // [V5] kSpanDegTable: ángulos SUPUESTOS (no medidos); solo fijan
        // tauXtalk. Si se miden los spans reales, se actualiza la tabla.
        static constexpr double kKappaTable[7]   = {0.00, 0.70, 0.45, 0.30, 0.35, 0.20, 0.20};
        static constexpr double kSpanDegTable[7] = {0.0,  6.0,  14.0, 28.0, 22.0, 35.0, 40.0};
        const int eqIdx = std::clamp(static_cast<int>(controls_.eqClass), 0, 6);
        const double kappa    = kKappaTable[eqIdx];
        const double xtalkMix = static_cast<double>(controls_.crosstalkCompMix);
        const double thetaSpk = kSpanDegTable[eqIdx] * kPi / 180.0;
        const double tauXtalk = (static_cast<double>(controls_.headRadiusM) / 343.0)
                                * (thetaSpk + std::sin(thetaSpk));

        const double maxBoostDb = std::min(static_cast<double>(controls_.boostCapDb),
                                           std::max(0.0, osOut.dMaxDb));
        const double gMax = std::pow(10.0, maxBoostDb / 20.0);
        const double muTikhonov = 1.0 / (4.0 * gMax * gMax + 1e-12);

        const double azRad = static_cast<double>(controls_.azimuthDeg) * kPi / 180.0;
        const double elRad = static_cast<double>(controls_.elevationDeg) * kPi / 180.0;

        for (int k = 0; k < kHalfBins; ++k) {
            const double f = static_cast<double>(k) * sampleRate_ / kFftSize;
            const double w = 2.0 * kPi * f;
            const int b = binToBand_[k];

            std::complex<double> gL = eqMinPhase[k];
            std::complex<double> gR = eqMinPhase[k];

            if (k > 0 && k < kFftSize / 2) {
                const auto hL = pinna3d_.evalTransfer(
                    f, azRad, elRad, false, controls_.headRadiusM,
                    controls_.lgnPinnaMix, controls_.brightSpotDamp, controls_.torsoEchoMix);
                const auto hR = pinna3d_.evalTransfer(
                    f, azRad, elRad, true, controls_.headRadiusM,
                    controls_.lgnPinnaMix, controls_.brightSpotDamp, controls_.torsoEchoMix);

                // Conservación EXACTA de energía media ILD + guarda ante muescas profundas
                const double ildExp   = osOut.bands[b].ildScale;
                const double magL2    = std::norm(hL);
                const double magR2    = std::norm(hR);
                const double ratio    = std::sqrt((magL2 + 1e-12) / (magR2 + 1e-12));
                const double expRatio = std::clamp(std::pow(ratio, ildExp - 1.0), 0.25, 4.0);
                const double norm     = std::sqrt((magL2 + magR2 + 1e-18) /
                                                  (magL2 * expRatio + magR2 / expRatio + 1e-18));

                gL *= hL * std::sqrt(expRatio) * norm;
                gR *= hR / std::sqrt(expRatio) * norm;
            } else {
                gL = {gL.real(), 0.0};
                gR = {gR.real(), 0.0};
            }

            // Operador S de cancelación de diafonía 2x2 en base de Paridad (±)
            std::complex<double> wDiag{1.0, 0.0};
            std::complex<double> wCross{0.0, 0.0};
            if (kappa > 1e-4 && xtalkMix > 1e-5) {
                const double fCut = 1800.0;
                const std::complex<double> hShadow =
                    std::complex<double>(1.0, 0.35 * f / fCut) /
                    std::complex<double>(1.0, f / fCut);
                const double ang = -w * tauXtalk;
                const std::complex<double> gPlant =
                    kappa * hShadow * std::complex<double>(std::cos(ang), std::sin(ang));

                const std::complex<double> lamPlus  = 1.0 + gPlant;
                const std::complex<double> lamMinus = 1.0 - gPlant;

                const std::complex<double> wPlus  = std::conj(lamPlus)  / (std::norm(lamPlus)  + muTikhonov);
                const std::complex<double> wMinus = std::conj(lamMinus) / (std::norm(lamMinus) + muTikhonov);

                const std::complex<double> w00 = 0.5 * (wPlus + wMinus);
                const std::complex<double> w01 = 0.5 * (wPlus - wMinus);

                wDiag  = 1.0 + xtalkMix * (w00 - 1.0);
                wCross = xtalkMix * w01;

                if (k == 0 || k == kFftSize / 2) {
                    wDiag  = {wDiag.real(), 0.0};
                    wCross = {wCross.real(), 0.0};
                }
            }

            compiledM_[k].m00 = static_cast<std::complex<float>>(wDiag  * gL);
            compiledM_[k].m01 = static_cast<std::complex<float>>(wCross * gR);
            compiledM_[k].m10 = static_cast<std::complex<float>>(wCross * gL);
            compiledM_[k].m11 = static_cast<std::complex<float>>(wDiag  * gR);
        }
    }

    void runStftHop() noexcept {
        // 1) Empaquetar z[n] = w[n]*(L[n] + j*R[n]) -> UNA sola FFT de 512
        for (int i = 0; i < kFftSize; ++i) {
            const int idx = (ringPos_ + i) & (kFftSize - 1);
            fftBuf_[i] = {win_[i] * inRingL_[idx], win_[i] * inRingR_[idx]};
        }
        fft512(fftBuf_.data(), false);

        // 2) Crossfade matricial en 4 hops (cero zipper noise)
        const float morph = matMorph_;
        const bool morphing = (morph < 0.999f);
        if (morphing) matMorph_ = std::min(1.0f, morph + 0.25f);

        std::array<float, 24> instEnergy{};
        for (int k = 0; k < kHalfBins; ++k) {
            const auto Zk  = fftBuf_[k];
            const auto Znk = (k == 0) ? std::conj(fftBuf_[0]) : std::conj(fftBuf_[kFftSize - k]);
            const std::complex<float> XL = 0.5f * (Zk + Znk);
            const std::complex<float> XR = std::complex<float>(0.0f, -0.5f) * (Zk - Znk);

            const int b = binToBand_[k];
            instEnergy[b] += std::norm(XL) + std::norm(XR);

            const float feMul = feGain_[b];
            const auto& C = compiledM_[k];
            std::complex<float> YL, YR;
            if (!morphing) {
                YL = (C.m00 * XL + C.m01 * XR) * feMul;
                YR = (C.m10 * XL + C.m11 * XR) * feMul;
            } else {
                const auto& P = prevM_[k];
                const float a = morph;
                const float b0 = 1.0f - a;
                YL = ((b0 * P.m00 + a * C.m00) * XL + (b0 * P.m01 + a * C.m01) * XR) * feMul;
                YR = ((b0 * P.m10 + a * C.m10) * XL + (b0 * P.m11 + a * C.m11) * XR) * feMul;
            }

            if (k == 0 || k == kFftSize / 2) {
                fftBuf_[k] = {YL.real(), YR.real()};
            } else {
                fftBuf_[k] = YL + std::complex<float>(0.0f, 1.0f) * YR;
                fftBuf_[kFftSize - k] = std::conj(YL) + std::complex<float>(0.0f, 1.0f) * std::conj(YR);
            }
        }

        // Lazo variacional F (MOC + Onsets) con tasa independiente de sampleRate_
        const float oneMinusA = 1.0f - envAlpha_;
        std::array<float, 24> transRatio{};
        for (int b = 0; b < 24; ++b) {
            const float mag  = std::sqrt(instEnergy[b] / kFftSize);
            const float prev = bandEnv_[b];
            const float next = envAlpha_ * prev + oneMinusA * mag;
            bandEnv_[b]   = (next < 1e-20f) ? 0.0f : next;
            transRatio[b] = mag / (prev + 1e-5f);
            bandTrans_[b] = transRatio[b];
        }
        freeEnergy_.step(bandEnv_.data(), bandTrans_.data(), controls_.freeEnergyMix, feGain_);
        telemetry_.mocFatigueIntegral = freeEnergy_.fatigueIntegral();

        // 3) UN solo IFFT 512 + OLA Hann
        fft512(fftBuf_.data(), true);
        for (int i = 0; i < kFftSize; ++i) {
            const int dst = (olaPos_ + i) & (kFftSize - 1);
            const float w = win_[i] * colaInv_;
            olaL_[dst] += fftBuf_[i].real() * w;
            olaR_[dst] += fftBuf_[i].imag() * w;
        }

        // 4) Post-procesado sin latencia extra sobre los 128 samples recién completados:
        //    cola MIMO de sala UPOLA (p >= 1) + Banco Modal Sub-100 Hz en double 64-bit
        for (int i = 0; i < kHopSize; ++i) {
            const int src = (olaPos_ + i) & (kFftSize - 1);
            wetHopL_[i] = olaL_[src];
            wetHopR_[i] = olaR_[src];
        }
        if (roomTailActive_ && roomTailMix_ > 1e-6f) {
            processRoomTailHop(wetHopL_.data(), wetHopR_.data(), roomTailMix_);
        }
        processSubBassIir(wetHopL_.data(), wetHopR_.data(), kHopSize);
        for (int i = 0; i < kHopSize; ++i) {
            const int dst = (olaPos_ + i) & (kFftSize - 1);
            olaL_[dst] = wetHopL_[i];
            olaR_[dst] = wetHopR_[i];
        }
    }

    // ── UPOLA Causal MIMO 2x2 (N=256, hop=128, P=24 -> 64 ms, 0 latencia extra) ─
    void processRoomTailHop(float* hopL, float* hopR, float wetMix) noexcept {
        for (int i = 0; i < kHopSize; ++i) olsBuf_[i] = {hopL[i], hopR[i]};
        for (int i = kHopSize; i < kOlsFft; ++i) olsBuf_[i] = {0.0f, 0.0f};
        fft256(olsBuf_.data(), false);

        const size_t headBase = static_cast<size_t>(fdlHead_) * kOlsBins;
        for (int k = 0; k < kOlsBins; ++k) {
            const auto Zk  = olsBuf_[k];
            const auto Znk = (k == 0) ? std::conj(olsBuf_[0]) : std::conj(olsBuf_[kOlsFft - k]);
            fdlL_[headBase + k] = 0.5f * (Zk + Znk);
            fdlR_[headBase + k] = std::complex<float>(0.0f, -0.5f) * (Zk - Znk);
        }

        std::array<std::complex<float>, kOlsBins> accL{}, accR{};
        for (int p = 0; p < kRoomParts; ++p) {
            const int pastSlot = (fdlHead_ - p - 1 + kRoomParts) % kRoomParts;
            const size_t xBase = static_cast<size_t>(pastSlot) * kOlsBins;
            const size_t hBase = static_cast<size_t>(p) * kOlsBins;
            for (int k = 0; k < kOlsBins; ++k) {
                const auto xL = fdlL_[xBase + k];
                const auto xR = fdlR_[xBase + k];
                accL[k] += hRoomLL_[hBase + k] * xL + hRoomLR_[hBase + k] * xR;
                accR[k] += hRoomRL_[hBase + k] * xL + hRoomRR_[hBase + k] * xR;
            }
        }

        for (int k = 0; k < kOlsBins; ++k) {
            if (k == 0 || k == kOlsFft / 2) {
                olsBuf_[k] = {accL[k].real(), accR[k].real()};
            } else {
                olsBuf_[k] = accL[k] + std::complex<float>(0.0f, 1.0f) * accR[k];
                olsBuf_[kOlsFft - k] = std::conj(accL[k]) + std::complex<float>(0.0f, 1.0f) * std::conj(accR[k]);
            }
        }
        fft256(olsBuf_.data(), true);

        for (int i = 0; i < kHopSize; ++i) {
            hopL[i] += wetMix * (olsBuf_[i].real() + roomCarryL_[i]);
            hopR[i] += wetMix * (olsBuf_[i].imag() + roomCarryR_[i]);
            roomCarryL_[i] = olsBuf_[kHopSize + i].real();
            roomCarryR_[i] = olsBuf_[kHopSize + i].imag();
        }
        fdlHead_ = (fdlHead_ + 1) % kRoomParts;
    }

    // ── Banco Modal Sub-100 Hz en 64-bit double ────────────────────────────────
    void prepareSubBassIir() noexcept {
        resetSubBassIir();
        for (int i = 0; i < kSubStages; ++i) compileSubBassStage(i);
    }

    void resetSubBassIir() noexcept {
        for (auto& s : subStages_) {
            s.z1L = s.z2L = s.z1R = s.z2R = 0.0;
        }
    }

    void compileSubBassStage(int idx) noexcept {
        const auto& p = subParams_[idx];
        auto& st = subStages_[idx];
        // v2.5.483 fail-closed: parámetro no finito (NaN/Inf) -> etapa OFF,
        // nunca coeficientes NaN en el hilo RT (un NaN en b0/a1 contaminaría
        // toda la salida). Cubre setSubBassModalStage y la etapa 0 auto.
        if (!p.active || std::abs(p.gainDb) <= 1e-4 ||
            !std::isfinite(p.freqHz) || !std::isfinite(p.q) || !std::isfinite(p.gainDb)) {
            st.enabled = false;
            st.b0 = 1.0; st.b1 = st.b2 = st.a1 = st.a2 = 0.0;
            return;
        }
        const double fs = static_cast<double>(sampleRate_);
        const double f0 = std::clamp(p.freqHz, 15.0, fs * 0.45);
        const double q  = std::clamp(p.q, 0.3, 18.0);
        const double A  = std::pow(10.0, p.gainDb / 40.0);
        const double w0 = 2.0 * kPi * f0 / fs;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);

        double b0, b1, b2, a0, a1, a2;
        if (p.lowShelf) {
            const double S = std::clamp(q, 0.3, 1.5);
            const double alpha = 0.5 * sw * std::sqrt((A + 1.0 / A) * (1.0 / S - 1.0) + 2.0);
            const double beta  = 2.0 * std::sqrt(A) * alpha;
            b0 =      A * ((A + 1.0) - (A - 1.0) * cw + beta);
            b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cw);
            b2 =      A * ((A + 1.0) - (A - 1.0) * cw - beta);
            a0 =          ((A + 1.0) + (A - 1.0) * cw + beta);
            a1 = -2.0 *   ((A - 1.0) + (A + 1.0) * cw);
            a2 =          ((A + 1.0) + (A - 1.0) * cw - beta);
        } else {
            const double alpha = sw / (2.0 * q);
            b0 =  1.0 + alpha * A;
            b1 = -2.0 * cw;
            b2 =  1.0 - alpha * A;
            a0 =  1.0 + alpha / A;
            a1 = -2.0 * cw;
            a2 =  1.0 - alpha / A;
        }
        const double invA0 = 1.0 / a0;
        st.b0 = b0 * invA0; st.b1 = b1 * invA0; st.b2 = b2 * invA0;
        st.a1 = a1 * invA0; st.a2 = a2 * invA0;
        st.enabled = true;
    }

    void processSubBassIir(float* L, float* R, int n) noexcept {
        for (int s = 0; s < kSubStages; ++s) {
            auto& st = subStages_[s];
            if (!st.enabled) continue;
            const double b0 = st.b0, b1 = st.b1, b2 = st.b2, a1 = st.a1, a2 = st.a2;
            double z1L = st.z1L, z2L = st.z2L, z1R = st.z1R, z2R = st.z2R;
            for (int i = 0; i < n; ++i) {
                const double xL = static_cast<double>(L[i]);
                const double yL = b0 * xL + z1L;
                z1L = b1 * xL - a1 * yL + z2L;
                z2L = b2 * xL - a2 * yL;
                L[i] = static_cast<float>(yL);

                const double xR = static_cast<double>(R[i]);
                const double yR = b0 * xR + z1R;
                z1R = b1 * xR - a1 * yR + z2R;
                z2R = b2 * xR - a2 * yR;
                R[i] = static_cast<float>(yR);
            }
            st.z1L = (std::abs(z1L) < 1e-24) ? 0.0 : z1L;
            st.z2L = (std::abs(z2L) < 1e-24) ? 0.0 : z2L;
            st.z1R = (std::abs(z1R) < 1e-24) ? 0.0 : z1R;
            st.z2R = (std::abs(z2R) < 1e-24) ? 0.0 : z2R;
        }
    }

    // ── Klippel Volterra H2 + H3 O(M) Laguerre + Red Térmica Dual-RC ───────────
    void prepareKlippelVolterra() noexcept {
        const float wc = 2.0f * static_cast<float>(kPi) * 15.0f / sampleRate_;
        dcPole_ = std::exp(-wc);
        // [V4] LPF del excitador a 1500 Hz: es el one_pole_lowpass_1500 de
        // genezi_calibrate.py — el mismo que veía el kernel al ajustarse.
        const float wx = 2.0f * static_cast<float>(kPi) * 1500.0f / sampleRate_;
        excAlpha_ = 1.0f - std::exp(-wx);
        // [V3] Si hay kernel Genezi cargado, el polo Laguerre se re-deriva
        // de sampleRate_ (exp(-1/(0.0125·fs)) ≈ 0.9983 @48k).
        if (geneziKernel_) {
            lagPole_ = std::exp(-1.0f / (0.0125f * sampleRate_));
            lagSqrt1mp2_ = std::sqrt(1.0f - lagPole_ * lagPole_);
        }
        resetKlippelVolterra();
    }

    void resetKlippelVolterra() noexcept {
        uL_.fill(0.0f); uR_.fill(0.0f);
        uPrevL_.fill(0.0f); uPrevR_.fill(0.0f);
        excL_ = excR_ = 0.0f;
        dcInL_ = dcOutL_ = dcInR_ = dcOutR_ = 0.0f;
        dTvL_ = dTmL_ = dTvR_ = dTmR_ = 0.0;
    }

    float processKlippelBlock(float* L, float* R, int n, float mix, float splDb, bool thermalActive) noexcept {
        const float splScale = std::pow(10.0f, (std::clamp(splDb, 65.0f, 115.0f) - 85.0f) / 20.0f);
        const float gThL = thermalActive ? thermalGain(dTvL_) : 1.0f;
        const float gThR = thermalActive ? thermalGain(dTvR_) : 1.0f;

        double sqSumL = 0.0, sqSumR = 0.0;
        float peakCorr = 0.0f;

        for (int i = 0; i < n; ++i) {
            const float xL = L[i] * gThL;
            const float xR = R[i] * gThR;
            sqSumL += static_cast<double>(xL) * xL;
            sqSumR += static_cast<double>(xR) * xR;

            if (mix > 1e-6f) {
                const float cL = evalKlippelChannel(xL, uL_, uPrevL_, c2L_, c3L_, alpha3L_, excL_, dcInL_, dcOutL_, splScale);
                const float cR = evalKlippelChannel(xR, uR_, uPrevR_, c2R_, c3R_, alpha3R_, excR_, dcInR_, dcOutR_, splScale);
                peakCorr = std::max(peakCorr, std::max(std::abs(cL), std::abs(cR)));
                L[i] = xL + mix * cL;
                R[i] = xR + mix * cR;
            } else {
                L[i] = xL;
                R[i] = xR;
            }
        }

        if (thermalActive) {
            const double dt = static_cast<double>(n) / sampleRate_;
            stepThermal(sqSumL / n, dTvL_, dTmL_, dt);
            stepThermal(sqSumR / n, dTvR_, dTmR_, dt);
        }
        return peakCorr;
    }

    float evalKlippelChannel(float x,
                             std::array<float, kLagOrder>& u,
                             std::array<float, kLagOrder>& uPrev,
                             const std::array<float, kLagTri21>& c2,
                             const std::array<float, kLagOrder>& c3,
                             float alpha3,
                             float& excState,
                             float& dcIn, float& dcOut,
                             float splScale) noexcept {
        excState += excAlpha_ * (x - excState);
        if (std::abs(excState) < 1e-20f) excState = 0.0f;

        u[0] = lagPole_ * uPrev[0] + lagSqrt1mp2_ * excState;
        for (int m = 1; m < kLagOrder; ++m) {
            u[m] = lagPole_ * uPrev[m] + uPrev[m - 1] - lagPole_ * u[m - 1];
        }
        for (int m = 0; m < kLagOrder; ++m) {
            uPrev[m] = (std::abs(u[m]) < 1e-20f) ? 0.0f : u[m];
        }

        // [V2] Forma cuadrática simétrica: los términos cruzados (a<b)
        // aparecen DOS veces (K[a][b] = K[b][a]); sin el ×2 el kernel
        // medido corrige la mitad de lo que debe en los cruces.
        float h2 = 0.0f;
        int idx = 0;
        for (int a = 0; a < kLagOrder; ++a) {
            for (int b = a; b < kLagOrder; ++b) {
                h2 += ((a == b) ? c2[idx] : 2.0f * c2[idx]) * u[a] * u[b];
                ++idx;
            }
        }

        float h3 = alpha3 * excState * excState * excState;
        for (int m = 0; m < kLagOrder; ++m) {
            const float um = u[m];
            h3 += c3[m] * um * um * um;
        }
        h3 *= h3Mix_;  // v2.5.483: mezcla H3 (1.0 = completo, 0 = H3 off)

        const float rawNl = (h2 + h3 * splScale) * splScale;
        const float yDc = rawNl - dcIn + dcPole_ * dcOut;
        dcIn = rawNl;
        dcOut = (std::abs(yDc) < 1e-20f) ? 0.0f : yDc;
        return std::clamp(dcOut, -0.35f, 0.35f);
    }

    void stepThermal(double meanSq, double& dTv, double& dTm, double dt) const noexcept {
        constexpr double re0 = 6.0, rTv = 1.8, cTv = 3.5, rTm = 1.2, cTm = 220.0, vPeak = 22.0;
        const double reT = re0 * (1.0 + 0.00393 * dTv);
        const double pElec = (meanSq * vPeak * vPeak * 0.5) / reT;
        const double fluxVm = (dTv - dTm) / rTv;
        dTv = std::max(0.0, dTv + (pElec - fluxVm) * (dt / cTv));
        dTm = std::max(0.0, dTm + (fluxVm - dTm / rTm) * (dt / cTm));
    }

    float thermalGain(double dTv) const noexcept {
        constexpr double ambC = 25.0, maxC = 135.0;
        const double tempC = ambC + dTv;
        if (tempC < maxC - 20.0) {
            return static_cast<float>(std::clamp(1.0 + 0.00393 * dTv * 0.5, 1.0, 1.18));
        }
        const double over = (tempC - (maxC - 20.0)) / 20.0;
        return static_cast<float>(std::clamp(1.0 - 0.5 * over, 0.35, 1.0));
    }

    // ── FFTs iterativas cero-malloc (512 y 256) ────────────────────────────────
    void fft512(std::complex<float>* x, bool inverse) const noexcept {
        fftRadix2(x, kFftSize, twiddle512_.data(), inverse);
    }
    void fft256(std::complex<float>* x, bool inverse) const noexcept {
        fftRadix2(x, kOlsFft, twiddle256_.data(), inverse);
    }

    static void fftRadix2(std::complex<float>* x, int N,
                          const std::complex<float>* tw, bool inverse) noexcept {
        for (int i = 1, j = 0; i < N; ++i) {
            int bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(x[i], x[j]);
        }
        for (int len = 2; len <= N; len <<= 1) {
            const int half = len >> 1;
            const int step = N / len;
            for (int i = 0; i < N; i += len) {
                for (int k = 0; k < half; ++k) {
                    auto w = tw[k * step];
                    if (inverse) w = std::conj(w);
                    const auto u = x[i + k];
                    const auto t = w * x[i + k + half];
                    x[i + k]        = u + t;
                    x[i + k + half] = u - t;
                }
            }
        }
        if (inverse) {
            const float invN = 1.0f / static_cast<float>(N);
            for (int i = 0; i < N; ++i) x[i] *= invN;
        }
    }

    float sampleRate_ = 48000.0f;
    bool prepared_ = false;
    bool compileDirty_ = true;
    NewPhysicsControls controls_{};
    NewPhysicsTelemetry telemetry_{};

    GeodesicPinna3D pinna3d_{};
    OmegaSigmaOperator omegaSigma_{};
    FreeEnergyAdaptationLoop freeEnergy_{};

    // WOLA 512
    std::array<float, kFftSize> win_{};
    float colaInv_ = 1.0f / 1.5f;
    std::array<int, kHalfBins> binToBand_{};
    std::array<std::complex<float>, kFftSize / 2> twiddle512_{};
    std::array<Mat2x2, kHalfBins> compiledM_{}, prevM_{};
    float matMorph_ = 1.0f;

    std::array<float, kFftSize> inRingL_{}, inRingR_{};
    std::array<float, kFftSize> olaL_{}, olaR_{};
    std::array<float, kFftSize> dryDelayL_{}, dryDelayR_{};
    std::array<float, kHopSize> wetHopL_{}, wetHopR_{};
    std::array<std::complex<float>, kFftSize> fftBuf_{};
    std::array<float, 24> bandEnv_{}, bandTrans_{};
    std::array<float, 24> feGain_{
        1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1
    };
    float envAlpha_ = 0.85f;
    int ringPos_ = 0, olaPos_ = 0, dryPos_ = 0, hopFill_ = 0;
    uint64_t frameIdx_ = 0;
    float smoothWet_ = 0.0f;

    // UPOLA MIMO sala (pre-alocado en prepare)
    std::array<std::complex<float>, kOlsFft / 2> twiddle256_{};
    std::array<std::complex<float>, kOlsFft> olsBuf_{};
    std::array<float, kHopSize> roomCarryL_{}, roomCarryR_{};
    std::vector<std::complex<float>> fdlL_, fdlR_;
    std::vector<std::complex<float>> hRoomLL_, hRoomLR_, hRoomRL_, hRoomRR_;
    int fdlHead_ = 0;
    bool roomTailActive_ = false;
    float roomTailMix_ = 1.0f;

    // Sub-Bass Modal IIR (64-bit double)
    std::array<SubBassStageParams, kSubStages> subParams_{};
    std::array<SubBassBiquad, kSubStages> subStages_{};

    // Klippel H2 + H3 + Térmico
    bool klippelLoaded_ = false;
    bool geneziKernel_ = false; // [V3] kernel Genezi: polo calibrado 0.9983
    float h3Mix_ = 1.0f;        // v2.5.483: mezcla H3 (1.0 = completo)
    float lagPole_ = 0.72f, lagSqrt1mp2_ = 0.6939f;
    float dcPole_ = 0.998f, excAlpha_ = 0.057f;
    std::array<float, kLagOrder> uL_{}, uR_{}, uPrevL_{}, uPrevR_{};
    std::array<float, kLagTri21> c2L_{}, c2R_{};
    std::array<float, kLagOrder> c3L_{}, c3R_{};
    float alpha3L_ = 0.0f, alpha3R_ = 0.0f;
    float excL_ = 0.0f, excR_ = 0.0f;
    float dcInL_ = 0.0f, dcOutL_ = 0.0f, dcInR_ = 0.0f, dcOutR_ = 0.0f;
    double dTvL_ = 0.0, dTmL_ = 0.0, dTvR_ = 0.0, dTmR_ = 0.0;
};

} // namespace ivanna::newphys
