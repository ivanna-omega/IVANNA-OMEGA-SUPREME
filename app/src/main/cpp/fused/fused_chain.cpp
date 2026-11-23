// fused_chain.cpp — FUSIÓN MAGISTRAL: la cadena única de los 7 operadores.
// Núcleo C++ compartido (Ruta A y Ruta B). Ver fused_chain.hpp.
#include "fused/fused_chain.hpp"
#include "fused/ivanna_apex_kernel.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ivanna::fused {

void FusedChain::prepare(double sampleRate) noexcept {
    sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
    brain_.prepare(sampleRate_);
    spatial_.prepare(sampleRate_);
    field_.prepare(sampleRate_);
    listenerRoom_.prepare(sampleRate_);
    transducer_.prepare(sampleRate_);
    tonal_.prepare(sampleRate_);
    dynamics_.prepare(sampleRate_);
    // 8. Conservación perceptual: banco de medición por banda.
    conservation_.prepare(sampleRate_);
    scratchL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    scratchR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    // IVANNA-APEX: splitter + líneas de retardo + scratch de trans/sust.
    apexSplitter_.prepare(sampleRate_);
    apexDelayL_.reset();
    apexDelayR_.reset();
    apexTransL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    apexTransR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    apexSustL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    apexSustR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    // IVANNA-APEX: matriz volumétrica (W por bin + WOLA).
    prepareVolumetric();
    // TΩ (F1/472): kernel 2×2 64-bit (compilador + pipeline hop 128).
    prepareTOmega();
}

void FusedChain::setControls(const FusedControls& c) noexcept {
    userControls_ = c;
    userControls_.clamp();
    brain_.setControls(userControls_.brain);
    spatial_.setControls(userControls_.spatial);
    // Puerta Genezi (Fallo 3a, PARCHE v2.6): el modo de HOLOS·Ω vive en
    // FieldControls.mode y sigue a spatial.mode. Se fija aquí antes de
    // repartir, así field_.setControls() ya lleva el modo correcto.
    userControls_.field.mode = userControls_.spatial.mode;
    field_.setControls(userControls_.field);
    listenerRoom_.setControls(userControls_.listenerRoom);
    transducer_.setControls(userControls_.transducer);
    tonal_.setControls(userControls_.tonal);
    dynamics_.setControls(userControls_.dynamics);
}

void FusedChain::setNeutral() noexcept {
    userControls_.setNeutral();
    setControls(userControls_);
}

void FusedChain::reset() noexcept {
    brain_.reset();
    spatial_.reset();
    field_.reset();
    listenerRoom_.reset();
    transducer_.reset();
    tonal_.reset();
    dynamics_.reset();
    conservation_.reset();  // 8. Conservación: banco + estado de campo a cero.
    // Gemelo + calibración P3: forzar el refresh en el próximo bloque
    // (la escala vuelve a calcularse desde el modo vivo).
    twinCalDirty_.store(true, std::memory_order_release);
    twinCalReady_ = false;
    twinEstimate_ = acoustic_twin::AcousticTwinEstimate{};
    chainTime_ = 0.0;       // el tiempo perceptual vuelve a cero con el reset.
    stats_ = FusedChainStats{};
    telemetry_ = FusedTelemetry{};
    // IVANNA-APEX: el splitter y las líneas vuelven a cero (sin allocs).
    apexSplitter_.reset();
    apexDelayL_.reset();
    apexDelayR_.reset();
    // IVANNA-APEX: la matriz volumétrica vacía su pipeline (sin allocs).
    resetVolumetric();
    // TΩ (F1/472): la etapa espectral vacía su pipeline y fuerza
    // recompilar con el snapshot vivo en el próximo process().
    resetTOmega();
}

void FusedChain::resetStats() noexcept { stats_ = FusedChainStats{}; }

// ── IVANNA-APEX: matriz volumétrica (XTC Genezi) ─────────────────────────
// STFT WOLA (N=512, hop=256, Hann solo en análisis). W precalculada por
// bin (spec Genezi). Latencia ~512 muestras solo cuando está activa.
void FusedChain::prepareVolumetric() noexcept {
    volSpec_ = apex::VolumetricSpeakerSpec{};
    for (int n = 0; n < kVolFft; ++n) {
        const double ph = 2.0 * apex::kPi * static_cast<double>(n) / static_cast<double>(kVolFft);
        volWin_[static_cast<std::size_t>(n)] = 0.5 - 0.5 * std::cos(ph);
    }
    for (int k = 0; k < kVolBins; ++k) {
        const double freq = static_cast<double>(k) * sampleRate_ / static_cast<double>(kVolFft);
        volW_[static_cast<std::size_t>(k)] =
            apex::computeVolumetricSpeakersMatrixBin(freq, volSpec_, apexWs_.sphereWs);
    }
    volHistL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    volHistR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    volOlaL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    volOlaR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    const std::size_t fifoCap = static_cast<std::size_t>(kVolFft + kFusedMaxBlockFrames);
    volOutL_.assign(fifoCap, 0.0);
    volOutR_.assign(fifoCap, 0.0);
    resetVolumetric();
}

void FusedChain::resetVolumetric() noexcept {
    std::fill(volHistL_.begin(), volHistL_.end(), 0.0);
    std::fill(volHistR_.begin(), volHistR_.end(), 0.0);
    std::fill(volOlaL_.begin(), volOlaL_.end(), 0.0);
    std::fill(volOlaR_.begin(), volOlaR_.end(), 0.0);
    std::fill(volOutL_.begin(), volOutL_.end(), 0.0);
    std::fill(volOutR_.begin(), volOutR_.end(), 0.0);
    volHistWp_ = 0;
    volInTotal_ = 0;
    volNextFrameAt_ = kVolFft;
    volOutWp_ = 0;
    volOutRp_ = 0;
    volOutLen_ = 0;
}

// LEGACY (F1/472): la matriz volumétrica WOLA ya no se llama — la etapa
// la implementa TΩ (compileTOmega + applyTOmega, mismo rol XTC, un solo
// kernel 2×2 64-bit). Se conserva íntegra (nunca se borra código).
void FusedChain::applyVolumetric(double* l, double* r, int frames) noexcept {
    if (!l || !r || frames <= 0) return;
    // Fail-closed: sin buffers (prepare no corrido), passthrough.
    if (volHistL_.size() < static_cast<std::size_t>(kVolFft) || volOutL_.empty()) return;
    const std::size_t fifoCap = volOutL_.size();
    const int fifoCapI = static_cast<int>(fifoCap);
    for (int n = 0; n < frames; ++n) {
        volHistL_[static_cast<std::size_t>(volHistWp_)] = std::isfinite(l[n]) ? l[n] : 0.0;
        volHistR_[static_cast<std::size_t>(volHistWp_)] = std::isfinite(r[n]) ? r[n] : 0.0;
        volHistWp_ = (volHistWp_ + 1) % kVolFft;
        ++volInTotal_;
        if (volInTotal_ != volNextFrameAt_) continue;
        volNextFrameAt_ += kVolHop;
        // Frame: últimos N muestras (en orden), ventana Hann.
        for (int i = 0; i < kVolFft; ++i) {
            const int idx = (volHistWp_ + i) % kVolFft;
            const double w = volWin_[static_cast<std::size_t>(i)];
            volFftL_[static_cast<std::size_t>(i)] = {volHistL_[static_cast<std::size_t>(idx)] * w, 0.0};
            volFftR_[static_cast<std::size_t>(i)] = {volHistR_[static_cast<std::size_t>(idx)] * w, 0.0};
        }
        apex::fftDouble(volFftL_.data(), kVolFft, false);
        apex::fftDouble(volFftR_.data(), kVolFft, false);
        // W por bin + espejo hermítico (señal real).
        for (int k = 0; k < kVolBins; ++k) {
            const apex::Complex2x2& W = volW_[static_cast<std::size_t>(k)];
            const std::complex<double> xL = volFftL_[static_cast<std::size_t>(k)];
            const std::complex<double> xR = volFftR_[static_cast<std::size_t>(k)];
            const std::complex<double> yL = W.m00 * xL + W.m01 * xR;
            const std::complex<double> yR = W.m10 * xL + W.m11 * xR;
            volFftL_[static_cast<std::size_t>(k)] = yL;
            volFftR_[static_cast<std::size_t>(k)] = yR;
            if (k > 0 && k < kVolFft / 2) {
                volFftL_[static_cast<std::size_t>(kVolFft - k)] = std::conj(yL);
                volFftR_[static_cast<std::size_t>(kVolFft - k)] = std::conj(yR);
            }
        }
        apex::fftDouble(volFftL_.data(), kVolFft, true);
        apex::fftDouble(volFftR_.data(), kVolFft, true);
        // Overlap-add al acumulador (sin ventana de síntesis; COLA Hann).
        for (int i = 0; i < kVolFft; ++i) {
            volOlaL_[static_cast<std::size_t>(i)] += volFftL_[static_cast<std::size_t>(i)].real();
            volOlaR_[static_cast<std::size_t>(i)] += volFftR_[static_cast<std::size_t>(i)].real();
        }
        // Emitir los primeros H del acumulador a la FIFO de salida.
        for (int i = 0; i < kVolHop; ++i) {
            if (volOutLen_ < fifoCapI) {
                volOutL_[static_cast<std::size_t>(volOutWp_)] = volOlaL_[static_cast<std::size_t>(i)];
                volOutR_[static_cast<std::size_t>(volOutWp_)] = volOlaR_[static_cast<std::size_t>(i)];
                volOutWp_ = (volOutWp_ + 1) % fifoCapI;
                ++volOutLen_;
            }
        }
        // Desplazar el acumulador H a la izquierda (cola a cero).
        for (int i = 0; i < kVolFft - kVolHop; ++i) {
            volOlaL_[static_cast<std::size_t>(i)] = volOlaL_[static_cast<std::size_t>(i + kVolHop)];
            volOlaR_[static_cast<std::size_t>(i)] = volOlaR_[static_cast<std::size_t>(i + kVolHop)];
        }
        for (int i = kVolFft - kVolHop; i < kVolFft; ++i) {
            volOlaL_[static_cast<std::size_t>(i)] = 0.0;
            volOlaR_[static_cast<std::size_t>(i)] = 0.0;
        }
    }
    // Sacar `frames` muestras de la FIFO (retrasadas ~N). Si la FIFO no
    // tiene suficientes (arranque), rellenar con ceros (transitorio ≤10.7 ms).
    for (int n = 0; n < frames; ++n) {
        if (volOutLen_ > 0) {
            l[n] = volOutL_[static_cast<std::size_t>(volOutRp_)];
            r[n] = volOutR_[static_cast<std::size_t>(volOutRp_)];
            volOutRp_ = (volOutRp_ + 1) % fifoCapI;
            --volOutLen_;
        } else {
            l[n] = 0.0;
            r[n] = 0.0;
        }
    }
}

// ── TΩ (F1/472): kernel 2×2 64-bit conectado a FusedChain ───────────────
// Reloj 2 (compile, double) + Reloj 1 (apply, hop 128 empaquetado L+jR).
// Ver fused_chain.hpp para el contrato completo (orden M[k], puertas,
// COLA, hermítica). La ventana Hann es la misma que la volumétrica
// (volWin_, N=512 periódica).
void FusedChain::prepareTOmega() noexcept {
    tOmegaHistL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    tOmegaHistR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    tOmegaOlaL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    tOmegaOlaR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    const std::size_t fifoCap = static_cast<std::size_t>(kVolFft + kFusedMaxBlockFrames);
    tOmegaOutL_.assign(fifoCap, 0.0);
    tOmegaOutR_.assign(fifoCap, 0.0);
    tOmegaFdlL_.fill(std::complex<double>(0.0, 0.0));
    tOmegaFdlR_.fill(std::complex<double>(0.0, 0.0));
    tOmegaWs_.outMatrix.fill(apex::ComplexMatrix2x2{});
    tOmegaWs_.minPhaseBuf.fill(std::complex<double>(1.0, 0.0));
    resetTOmega();
}

void FusedChain::resetTOmega() noexcept {
    std::fill(tOmegaHistL_.begin(), tOmegaHistL_.end(), 0.0);
    std::fill(tOmegaHistR_.begin(), tOmegaHistR_.end(), 0.0);
    std::fill(tOmegaOlaL_.begin(), tOmegaOlaL_.end(), 0.0);
    std::fill(tOmegaOlaR_.begin(), tOmegaOlaR_.end(), 0.0);
    std::fill(tOmegaOutL_.begin(), tOmegaOutL_.end(), 0.0);
    std::fill(tOmegaOutR_.begin(), tOmegaOutR_.end(), 0.0);
    tOmegaHistWp_ = 0;
    tOmegaInTotal_ = 0;
    tOmegaNextFrameAt_ = kVolFft;
    tOmegaOutWp_ = 0;
    tOmegaOutRp_ = 0;
    tOmegaOutLen_ = 0;
    tOmegaCompiled_ = false;  // fuerza recompilar con el snapshot vivo
    // tOmegaCompiles_ NO se resetea: contador acumulado (telemetría).
}

void FusedChain::compileTOmega() noexcept {
    // Geometría Genezi (la misma spec que usaba la matriz volumétrica).
    apex::SpeakerPairPhysics sp;
    sp.spanM = volSpec_.spanM;
    sp.distanceM = volSpec_.distanceM;
    sp.headRadiusM = volSpec_.headRadiusM;
    sp.speedOfSoundMps = volSpec_.speedOfSoundMps;

    // T_Eq: magnitud neta PROPIA de TΩ, fase mínima cepstral en double.
    // En F1 es plana (0 dB): el tonal lo aplica ARMONÍA serial y
    // duplicarlo colorearía dos veces. El cepstrum corre igual (g_eq = 1
    // exacto) para que la estructura M = T_XTC · (g_eq·I) quede cableada.
    std::array<double, kVolBins> magDb{};
    magDb.fill(0.0);
    apex::buildMinimumPhaseSpectrum64(magDb.data(), kVolFft,
                                      tOmegaWs_.minPhaseBuf.data(), tOmegaWs_);

    const bool speakers = tOmegaSnapshot_.mode == OmegaOutputMode::SPEAKERS;
    for (int k = 0; k < kVolBins; ++k) {
        const double freq = static_cast<double>(k) * sampleRate_ / static_cast<double>(kVolFft);
        const std::complex<double> gEq =
            tOmegaWs_.minPhaseBuf[static_cast<std::size_t>(k)];
        // T_XTC: Kirkeby mono-renormalizado en SPEAKERS; identidad en
        // HEADPHONES. T_Transd = T_HOLOS/LGN = T_ORBE = I en F1 (ver hpp).
        const apex::ComplexMatrix2x2 xtc =
            speakers ? apex::computeSpeakersMatrixBin(freq, sp, tOmegaWs_.sphereWs)
                     : apex::ComplexMatrix2x2{};
        apex::ComplexMatrix2x2& M = tOmegaWs_.outMatrix[static_cast<std::size_t>(k)];
        M.m00 = xtc.m00 * gEq;
        M.m01 = xtc.m01 * gEq;
        M.m10 = xtc.m10 * gEq;
        M.m11 = xtc.m11 * gEq;
    }
    tOmegaCompiled_ = true;
    ++tOmegaCompiles_;
}

void FusedChain::applyTOmega(double* l, double* r, int frames) noexcept {
    if (!l || !r || frames <= 0) return;
    // Fail-closed: sin buffers (prepare no corrido) o sin matriz
    // compilada, passthrough (no se aplica nada a ciegas).
    if (tOmegaHistL_.size() < static_cast<std::size_t>(kVolFft) || tOmegaOutL_.empty()) return;
    if (!tOmegaCompiled_) return;
    const std::size_t fifoCap = tOmegaOutL_.size();
    const int fifoCapI = static_cast<int>(fifoCap);
    for (int n = 0; n < frames; ++n) {
        tOmegaHistL_[static_cast<std::size_t>(tOmegaHistWp_)] =
            std::isfinite(l[n]) ? l[n] : 0.0;
        tOmegaHistR_[static_cast<std::size_t>(tOmegaHistWp_)] =
            std::isfinite(r[n]) ? r[n] : 0.0;
        tOmegaHistWp_ = (tOmegaHistWp_ + 1) % kVolFft;
        ++tOmegaInTotal_;
        if (tOmegaInTotal_ != tOmegaNextFrameAt_) continue;
        tOmegaNextFrameAt_ += kTOmegaHop;
        // Frame: últimos N muestras (en orden), ventana Hann, empaquetado
        // z = L + jR en UN solo buffer (1 FFT en vez de 2).
        for (int i = 0; i < kVolFft; ++i) {
            const int idx = (tOmegaHistWp_ + i) % kVolFft;
            const double w = volWin_[static_cast<std::size_t>(i)];
            tOmegaPack_[static_cast<std::size_t>(i)] = {
                tOmegaHistL_[static_cast<std::size_t>(idx)] * w,
                tOmegaHistR_[static_cast<std::size_t>(idx)] * w};
        }
        apex::fftRadix2Double(tOmegaPack_.data(), kVolFft, false);
        // Desempaquetar X_L/X_R (Z[k] = XL[k] + j·XR[k]):
        //   XL = (Z[k] + conj(Z[N-k])) / 2
        //   XR = (Z[k] - conj(Z[N-k])) / (2j) = -0.5j · (Z[k] - conj(Z[N-k]))
        // Aplicar Y[k] = M[k]·X[k] + T_FDL[k]; DC y Nyquist reales.
        for (int k = 0; k < kVolBins; ++k) {
            const std::complex<double> zk = tOmegaPack_[static_cast<std::size_t>(k)];
            const int nk = (k == 0) ? 0 : (kVolFft - k);
            const std::complex<double> znk = tOmegaPack_[static_cast<std::size_t>(nk)];
            const std::complex<double> diff = zk - std::conj(znk);
            const std::complex<double> xL = 0.5 * (zk + std::conj(znk));
            const std::complex<double> xR =
                std::complex<double>(0.0, -0.5) * diff;
            const apex::ComplexMatrix2x2& M =
                tOmegaWs_.outMatrix[static_cast<std::size_t>(k)];
            std::complex<double> yL = M.m00 * xL + M.m01 * xR +
                                      tOmegaFdlL_[static_cast<std::size_t>(k)];
            std::complex<double> yR = M.m10 * xL + M.m11 * xR +
                                      tOmegaFdlR_[static_cast<std::size_t>(k)];
            // DC (k=0) y Nyquist (k=256) de una señal real DEBEN ser
            // reales: se conserva la parte real (la imaginaria es espuria
            // para una señal real; mismo forzado que
            // applyUnifiedMatrix2x2, residual <1e-14 tras IFFT).
            if (k == 0 || k == kVolFft / 2) {
                yL = std::complex<double>(yL.real(), 0.0);
                yR = std::complex<double>(yR.real(), 0.0);
            }
            tOmegaYl_[static_cast<std::size_t>(k)] = yL;
            tOmegaYr_[static_cast<std::size_t>(k)] = yR;
        }
        // Reempaquetar W[k] = YL[k] + j·YR[k] (hermítico:
        // W[N-k] = conj(YL[k] - j·YR[k])) y UN solo IFFT.
        for (int k = 0; k < kVolBins; ++k) {
            const std::complex<double> yL = tOmegaYl_[static_cast<std::size_t>(k)];
            const std::complex<double> yR = tOmegaYr_[static_cast<std::size_t>(k)];
            tOmegaPack_[static_cast<std::size_t>(k)] = std::complex<double>(
                yL.real() - yR.imag(), yL.imag() + yR.real());
            if (k > 0 && k < kVolFft / 2) {
                const std::complex<double> d =
                    yL - std::complex<double>(0.0, 1.0) * yR;
                tOmegaPack_[static_cast<std::size_t>(kVolFft - k)] = std::conj(d);
            }
        }
        apex::fftRadix2Double(tOmegaPack_.data(), kVolFft, true);
        // Overlap-add al acumulador (Re = L, Im = R del IFFT empaquetado).
        for (int i = 0; i < kVolFft; ++i) {
            tOmegaOlaL_[static_cast<std::size_t>(i)] += tOmegaPack_[static_cast<std::size_t>(i)].real();
            tOmegaOlaR_[static_cast<std::size_t>(i)] += tOmegaPack_[static_cast<std::size_t>(i)].imag();
        }
        // Emitir los primeros H del acumulador a la FIFO de salida, con
        // ganancia COLA 0.5 (Hann análisis, hop 128, N=512: Σw = 2.0).
        for (int i = 0; i < kTOmegaHop; ++i) {
            if (tOmegaOutLen_ < fifoCapI) {
                tOmegaOutL_[static_cast<std::size_t>(tOmegaOutWp_)] =
                    0.5 * tOmegaOlaL_[static_cast<std::size_t>(i)];
                tOmegaOutR_[static_cast<std::size_t>(tOmegaOutWp_)] =
                    0.5 * tOmegaOlaR_[static_cast<std::size_t>(i)];
                tOmegaOutWp_ = (tOmegaOutWp_ + 1) % fifoCapI;
                ++tOmegaOutLen_;
            }
        }
        // Desplazar el acumulador H a la izquierda (cola a cero).
        for (int i = 0; i < kVolFft - kTOmegaHop; ++i) {
            tOmegaOlaL_[static_cast<std::size_t>(i)] =
                tOmegaOlaL_[static_cast<std::size_t>(i + kTOmegaHop)];
            tOmegaOlaR_[static_cast<std::size_t>(i)] =
                tOmegaOlaR_[static_cast<std::size_t>(i + kTOmegaHop)];
        }
        for (int i = kVolFft - kTOmegaHop; i < kVolFft; ++i) {
            tOmegaOlaL_[static_cast<std::size_t>(i)] = 0.0;
            tOmegaOlaR_[static_cast<std::size_t>(i)] = 0.0;
        }
    }
    // Sacar `frames` muestras de la FIFO (retrasadas ~N). Si la FIFO no
    // tiene suficientes (arranque), rellenar con ceros (transitorio ≤10.7 ms).
    for (int n = 0; n < frames; ++n) {
        if (tOmegaOutLen_ > 0) {
            l[n] = tOmegaOutL_[static_cast<std::size_t>(tOmegaOutRp_)];
            r[n] = tOmegaOutR_[static_cast<std::size_t>(tOmegaOutRp_)];
            tOmegaOutRp_ = (tOmegaOutRp_ + 1) % fifoCapI;
            --tOmegaOutLen_;
        } else {
            l[n] = 0.0;
            r[n] = 0.0;
        }
    }
}

void FusedChain::setRouteTag(const char* tag) noexcept {
    if (!tag) {
        routeTag_[0] = '\0';
        return;
    }
    std::size_t i = 0;
    for (; i + 1 < sizeof(routeTag_) && tag[i] != '\0'; ++i) routeTag_[i] = tag[i];
    routeTag_[i] = '\0';
}

void FusedChain::setTwinCalibration(
    const acoustic_twin::TwinCalibration& cal) noexcept {
    twinCalibrationOverride_ = cal;
    twinCalibrationHasOverride_.store(true, std::memory_order_release);
    twinCalDirty_.store(true, std::memory_order_release);
}

void FusedChain::refreshTwinCalibration(OmegaOutputMode mode) noexcept {
    // Solo recomputar cuando cambia el modo o la calibración (o la
    // primera vez): la estimación es función pura de parámetros
    // estáticos, por bloque sería trabajo inútil.
    const bool dirty =
        twinCalDirty_.exchange(false, std::memory_order_acq_rel);
    if (twinCalReady_ && !dirty && mode == twinCalMode_) return;
    twinCalMode_ = mode;
    twinCalReady_ = true;

    if (mode == OmegaOutputMode::SPEAKERS) {
        // Trayecto Genezi: calibración con la respuesta MEDIDA desde el
        // barrido Farina de la app (2026-11-22 05:54, TwinCalibration::
        // geneziPz1dMeasured(), confianza honesta 0.80), o la
        // alimentada por la app. Calibración inválida -> estimado SIN
        // calibrar (~0.46) con el preset sin valores derivados, para
        // que la fórmula no infle la confianza a 0.926.
        // La P3 estimada (geneziPz1dEstimated(), 0.70) y la nota de voz
        // (geneziPz1dVoiceNote(), 0.75) quedan como respaldo
        // documentado; la medida en WAV limpio manda porque su
        // respuesta 63 Hz-8 kHz es MEDIDA en limpio, no AAC.
        const acoustic_twin::TwinCalibration cal =
            twinCalibrationHasOverride_.load(std::memory_order_acquire)
                ? twinCalibrationOverride_
                : acoustic_twin::TwinCalibration::geneziPz1dMeasured();
        acoustic_twin::AcousticTwinEstimate est;
        if (cal.isValid()) {
            est = acoustic_twin::AcousticTwin::estimateCalibrated(
                acoustic_twin::AcousticTwinParams::esseGeneziCalibrated(), cal);
        } else {
            est = acoustic_twin::AcousticTwin::estimate(
                acoustic_twin::AcousticTwinParams::esseGenezi());
        }
        twinEstimate_ = est;
        applyTwinToConservation(conservation_, est);
        telemetry_.twinCalibrationApplied = est.calibrationApplied;
        telemetry_.twinCalibrationConfidence =
            static_cast<float>(est.calibrationConfidence);
    } else {
        // Audífonos: la calibración del Genezi no aplica. Escala 1.0
        // (sin cambio) y telemetría en 0. Si se venía de SPEAKERS, el
        // reset de la escala es obligatorio: si no, la conservación en
        // audífonos quedaría escalada por la confianza del Genezi.
        twinEstimate_ = acoustic_twin::AcousticTwinEstimate{};
        conservation_.setTwinInterventionScale(1.0);
        telemetry_.twinCalibrationApplied = false;
        telemetry_.twinCalibrationConfidence = 0.0f;
    }
}

void FusedChain::pushEffectiveControls() noexcept {
    // El guardián AntiDolby (NEURO·Ω) ya se actualizó con el bloque actual
    // (el cerebro corre primero). Su modulación se aplica en este bloque:
    // widener → ancho de PULSO, EQ 2-4 kHz → agudos de ARMONÍA,
    // spread → ancho de ORBE·Ω. Intensidad 0 = sin modulación.
    const float gW = brain_.antiDolbyWidener();
    const float gE = brain_.antiDolbyEqBoostDb();
    const float gS = brain_.antiDolbySpreadMul();
    const float gI = brain_.antiDolbyIntensity();

    // Widener/spread se interpolan por intensidad: con intensidad 0
    // (MANUAL) no hay modulación (factor 1.0, el ancho queda en 1.0 y el
    // operador sigue neutro); con intensidad 1, modulación completa.
    // (Sin esta ponderación, el guardián sacaba al operador del neutro
    // aunque el usuario no pidiera nada: width != 1.0f rompe isNeutral().)
    const float spreadFactor = 1.0f + (gS - 1.0f) * gI;
    const float widenFactor = 1.0f + (gW - 1.0f) * gI;
    FusedControls eff = userControls_;
    // ORBE·Ω: ancho efectivo = ancho del usuario × spread del guardián.
    eff.spatial.width = clampf(eff.spatial.width * spreadFactor, 0.0f, 2.0f);
    // ARMONÍA: agudos efectivos = agudos del usuario + boost del guardián.
    eff.tonal.highDb = clampf(eff.tonal.highDb + gE * gI, -12.0f, 12.0f);
    // Enrutado del upmix: la inmersión la gobierna PULSO pero el upmixer
    // HOA vive en ORBE·Ω. Efectivo = el mayor de los dos paneles. El
    // upmixer se enciende si el usuario lo activó en el panel espacial O si
    // la inmersión de PULSO es > 0 (enrutado). NO se deriva de la
    // inmersividad espacial cruda: su default es 1.25 y setNeutral() la
    // pone a 0, pero un valor residual no debe encender el upmixer solo.
    eff.spatial.upmixImmersivity =
        std::max(eff.spatial.upmixImmersivity, eff.dynamics.upmixImmersivity);
    eff.spatial.upmixEnabled =
        eff.spatial.upmixEnabled || eff.dynamics.upmixImmersivity > 0.0f;

    // ── Fallo 3b: el guardián NO inyecta StereoWidener (M/S) sobre un
    // render binaural 3D ni en SPEAKERS ──────────────────────────────────
    // El widener M/S remezcla mid/side y destruye el ITD/ILD que ORBE·Ω,
    // HOLOS·Ω (GreenKernel), el HRTF o el upmix HOA acaban de codificar;
    // en SPEAKERS tampoco hay escena binaural que ensanchar (la ruta de
    // altavoz es XTC + AuxField). Solo se suprime el FACTOR del guardián:
    // el ancho que el usuario puso a mano en PULSO se respeta (elección
    // explícita, no inyección automática). Sin render binaural activo y
    // en audífonos, el guardián modula como siempre (MUSIC widener 1.35).
    const bool speakers =
        eff.spatial.mode == OmegaOutputMode::SPEAKERS;
    const bool binaural3d =
        !speakers &&
        (eff.spatial.amount > 0.0f || eff.spatial.hrtfWet > 0.0f ||
         eff.spatial.upmixEnabled || eff.spatial.wfsSpread > 0.0f ||
         eff.field.amount > 0.0f);
    const float widenFactorEff =
        (speakers || binaural3d) ? 1.0f : widenFactor;
    // PULSO: ancho efectivo = ancho del usuario × widener del guardián
    // (×1.0 cuando el arbitraje lo suprime: el ancho queda en el del
    // usuario y PULSO sigue neutro si era 1.0).
    eff.dynamics.width = clampf(eff.dynamics.width * widenFactorEff, 0.0f, 2.0f);

    // ── Fallo 3c: presupuesto tonal único ────────────────────────────────
    // Tres fuentes empujaban hacia Harman a la vez: ARMONÍA (EvoEQ Harman
    // 2018 ×0.5, o élite-lean), Φ dentro de HOLOS·Ω (target Harman AE/OE
    // 2018 por defecto en HyperStateOperator, corrección cut-only hacia
    // esa curva) y SALA VIVA (hearingProfile==1, aproximación Harman:
    // +2.9 dB graves / +1.0 dB agudos). Dueño único por prioridad:
    //   1. ARMONÍA (curve != 0) posee la curva: SALA VIVA pierde su
    //      Harman (solo su aproximación; su RIR y su corrección de sala
    //      cut-only no se tocan) y Φ —cuya corrección Harman NO se puede
    //      separar de su campo por controles— se atenúa a la mitad de su
    //      wet (sigue vivo: Volterra, EMF, Γ; solo tira la mitad hacia
    //      la curva).
    //   2. Sin ARMONÍA, Φ posee la curva: SALA VIVA pierde su Harman.
    //   3. Sin ARMONÍA ni Φ, SALA VIVA puede ser la única fuente Harman.
    // hearingProfile==2 (CUSTOM, audiograma personal) NO es Harman y no
    // se suprime nunca. Solo se tocan los controles EFECTIVOS; los del
    // usuario quedan intactos en userControls_.
    float phiScale = 1.0f;
    bool hearingSuppressed = false;
    if (eff.tonal.curve != 0) {
        if (eff.listenerRoom.hearingProfile == 1) {
            eff.listenerRoom.hearingProfile = 0;
            hearingSuppressed = true;
        }
        phiScale = 0.5f;
    } else if (eff.field.phiAmount > 0.0f) {
        if (eff.listenerRoom.hearingProfile == 1) {
            eff.listenerRoom.hearingProfile = 0;
            hearingSuppressed = true;
        }
    }
    eff.field.phiAmount = clampf(eff.field.phiAmount * phiScale, 0.0f, 1.0f);
    eff.clamp();

    // Puerta Genezi (Fallo 3a, PARCHE v2.6): el modo efectivo de HOLOS·Ω
    // sigue a spatial.mode en cada bloque (por si cambió sin setControls).
    eff.field.mode = eff.spatial.mode;
    brain_.setControls(eff.brain);
    spatial_.setControls(eff.spatial);
    field_.setControls(eff.field);
    listenerRoom_.setControls(eff.listenerRoom);
    transducer_.setControls(eff.transducer);
    tonal_.setControls(eff.tonal);
    dynamics_.setControls(eff.dynamics);

    // ── GEMELO ACÚSTICO + CALIBRACIÓN P3 — CALL SITE (2026-11-21) ────────
    // En runtime, la cadena consulta la estimación calibrada del gemelo
    // y alimenta su interventionScale a la conservación (el camino vivo
    // que ya la consume: process() la multiplica por el amount). Solo se
    // recomputa al cambiar el modo o la calibración; el primer bloque
    // tras prepare() ya la aplica. En SPEAKERS la escala es 0.70
    // (confianza honesta de la P3 estimada); en HEADPHONES es 1.0.
    // Corre ANTES de la sección 8: los defaults del gemelo para la
    // conservación (amount y techo) dependen de si la calibración se
    // aplicó (telemetry_.twinCalibrationApplied).
    refreshTwinCalibration(eff.spatial.mode);

    // ── 8. CONSERVACIÓN PERCEPTUAL (árbitro de campo, 2026-11-21) ────────
    // La capa recibe la config del usuario y los AMOUNTS EFECTIVOS de los
    // 7 operadores (de `eff`, no de userControls_: el guardián y el
    // arbitraje ya los modularon). El amount es la confianza de la
    // propuesta de cada operador: 0 = inactivo (no propone nada).
    // Proxies (documentados, no medidos): ARMONÍA no tiene un amount único
    // — su actividad es el mayor |dB| de su EQ (o 1.0 si lleva curva); PULSO
    // usa el mayor de sus amounts (punch/exciter/voice/safety).
    {
        ConservationConfig cc;
        // ── Control del gemelo (orden de Esse, 2026-11-21) ──
        // Con la calibración P3 aplicada (SPEAKERS, válida), el gemelo
        // CONTROLA la conservación: si el usuario no fijó amount ni
        // techo, se usan los defaults del gemelo
        // (kTwinCalibratedDefaultConservation*). Sin el amount, la escala
        // 0.70 no haría nada (0 × 0.70 = 0); sin el techo, el
        // solucionador nunca constriñe (ganancia siempre 1.0: solo baja
        // si el techo < energía medida) y el control sería un placebo.
        // El amount del usuario (> 0) y un techo fijado por el usuario
        // (< 1.0) siempre mandan. En audífonos no se aplica nada (la
        // calibración no está aplicada).
        float consAmount = eff.conservation.amount;
        float consEnergyCap = eff.conservation.energyCap;
        // Solo cuando la cadena está ACTIVA (no neutra): en neutro el
        // usuario pide bypass bit-exacto y el gemelo no debe intervenir
        // (TOmegaNeutralBitExactSpeakers y ChainNeutralStillBitExact lo
        // exigen). En escucha real (operadores encendidos) el gemelo sí
        // controla.
        if (telemetry_.twinCalibrationApplied && !isNeutral()) {
            if (consAmount <= 0.0f) {
                consAmount = kTwinCalibratedDefaultConservationAmount;
            }
            if (consEnergyCap >= 1.0f) {
                consEnergyCap = kTwinCalibratedDefaultConservationEnergyCap;
            }
        }
        cc.amount = consAmount;
        cc.energyCap = consEnergyCap;
        cc.phaseCoherence = eff.conservation.phaseCoherence;
        cc.inertia = eff.conservation.inertia;
        conservation_.setConfig(cc);
        const float tonalDb = std::max(
            std::max(std::abs(eff.tonal.trimDb), std::abs(eff.tonal.lowDb)),
            std::max(std::abs(eff.tonal.midDb), std::abs(eff.tonal.highDb)));
        const float tonalAmt = (eff.tonal.curve != 0)
            ? 1.0f
            : clampf(tonalDb / 12.0f, 0.0f, 1.0f);
        const float pulseAmt = clampf(
            std::max(std::max(eff.dynamics.punch, eff.dynamics.exciter),
                     std::max(eff.dynamics.voice, eff.dynamics.safety)),
            0.0f, 1.0f);
        const float amounts[PerceptualConservation::kNumOperators] = {
            clampf(eff.brain.adaptation, 0.0f, 1.0f),           // 0 NEURO
            clampf(eff.spatial.amount, 0.0f, 1.0f),            // 1 ORBE
            clampf(eff.field.amount, 0.0f, 1.0f),              // 2 HOLOS
            clampf(eff.listenerRoom.roomWet, 0.0f, 1.0f),      // 3 SALA VIVA
            clampf(eff.transducer.correction, 0.0f, 1.0f),     // 4 VOLTERRA
            tonalAmt,                                          // 5 ARMONÍA
            pulseAmt,                                          // 6 PULSO
        };
        conservation_.setOperatorAmounts(amounts, PerceptualConservation::kNumOperators);
    }

    // (El call site del gemelo corre ANTES de la sección 8: los defaults
    // del gemelo para la conservación dependen de si la calibración se
    // aplicó.)

    // Telemetría para FotoHolo (ancho espacial + guardián + género).
    telemetry_.spatialWidth = eff.spatial.width;
    telemetry_.guardianWidener = gW;
    telemetry_.guardianEqBoostDb = gE * gI;
    telemetry_.guardianSpread = gS;
    telemetry_.guardianIntensity = gI;
    telemetry_.tonalBudgetPhiScale = phiScale;
    telemetry_.hearingHarmanSuppressed = hearingSuppressed;
    telemetry_.dominantGenre = brain_.dominantGenre();
    telemetry_.genreConfidence = brain_.confidence();
    telemetry_.brainAuto = (userControls_.brain.mode == 0);
}

void FusedChain::process(double* left, double* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    // ── IVANNA-APEX: el camino de transitorios solo engancha cuando hay ───
    // procesamiento que proteger. Si la cadena es NEUTRA (los 7 operadores
    // en bypass), el APEX se salta por completo: process() es bit-idéntico
    // al anterior (sin splitter ni suma), preservando la garantía de
    // neutralidad bit-exacta del chain (NeutralIsBitExactFloat). El APEX
    // protege los ataques DE los operadores; sin operadores activos no hay
    // nada que proteger, y el retardo de 2.0 muestras rompería la identidad.
    const bool apex = apexEnabled_.load(std::memory_order_relaxed) && !isNeutral();
    // ── TΩ (F1/472): matriz 2×2 64-bit (Kirkeby Genezi) ──────────────────
    // SOLO en SPEAKERS (470, crítica externa verificada): la matriz es
    // cancelación de diafonía para altavoces Genezi; en audífonos no hay
    // diafonía acústica que cancelar y aplicarla colorearía la señal.
    // La puerta exige: flag on + no neutro + modo SPEAKERS. El modo llega
    // a userControls_.spatial.mode por setControls (Ruta A vía JNI, Ruta B
    // vía setFusedOutputMode; el sync por atomics no lo toca, así que
    // persiste). Si nunca se puso, el default HEADPHONES deja la matriz
    // apagada (fail-closed). En neutro es passthrough bit-exacto.
    // La implementación es la TΩ (compileTOmega + applyTOmega, hop 128
    // empaquetado, double); el WOLA volumétrico queda LEGACY. Sin toggle
    // propio de TΩ: el interruptor de la etapa es setVolumetricEnabled().
    const bool tOmega = volumetricEnabled_.load(std::memory_order_relaxed) && !isNeutral() &&
                        userControls_.spatial.mode == OmegaOutputMode::SPEAKERS;
    if (tOmega) {
        // Reloj 2: recompilar solo si cambió lo que la matriz refleja.
        TOmegaSnapshot snap;
        snap.mode = userControls_.spatial.mode;
        snap.sampleRate = sampleRate_;
        snap.spanM = volSpec_.spanM;
        snap.distanceM = volSpec_.distanceM;
        if (!tOmegaCompiled_ || snap != tOmegaSnapshot_) {
            tOmegaSnapshot_ = snap;
            compileTOmega();
        }
    }
    // ── IVANNA-APEX: ITD fraccionario del azimut de ORBE ──────────────────
    // Se calcula una vez por llamada (el azimut no cambia dentro de un
    // bloque). theta: mismo convenio que HrtfManager (azimut efectivo
    // negado). Distancia 1.0 m (default de escena; el ITD azimutal fino
    // con distancia viva llega en la siguiente fase). Base causal 2.0
    // muestras (la línea de Lagrange la exige >= 1.0, óptimo >= 2.0).
    double apexDelayL = 2.0, apexDelayR = 2.0;
    if (apex) {
        const SpatialControls sc = spatial_.controls();
        float azDeg = sc.azimuthDeg;
        if (sc.headTracking) azDeg += spatial_.trackerYawDeg();
        const double theta = -static_cast<double>(azDeg) *
                             (3.14159265358979323846 / 180.0);
        const double itdSec =
            ivanna::fused::propagation::curvatureLawItdSeconds(theta, 1.0);
        const double itdSamples = itdSec * sampleRate_;
        apexDelayL = 2.0 + std::max(0.0, itdSamples);
        apexDelayR = 2.0 + std::max(0.0, -itdSamples);
    }
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kFusedMaxBlockFrames);
        double* l = left + offset;
        double* r = right + offset;

        if (apex) {
            // ── APEX 1: separar transitorio/sostenido (muestra a muestra) ──
            // El SOSTENIDO es lo que procesan los 7 operadores; el
            // TRANSITORIO se guarda para sumarlo al final sin procesar.
            for (int i = 0; i < chunk; ++i) {
                double tL, tR, sL, sR;
                apexSplitter_.splitSample(l[i], r[i], tL, tR, sL, sR);
                apexTransL_[static_cast<std::size_t>(i)] = tL;
                apexTransR_[static_cast<std::size_t>(i)] = tR;
                apexSustL_[static_cast<std::size_t>(i)] = sL;
                apexSustR_[static_cast<std::size_t>(i)] = sR;
            }
            for (int i = 0; i < chunk; ++i) {
                l[i] = apexSustL_[static_cast<std::size_t>(i)];
                r[i] = apexSustR_[static_cast<std::size_t>(i)];
            }
        }

        // 0. NEURO·Ω primero: observa y actúa; actualiza el guardián.
        const bool brainActive = !brain_.isNeutral();
        brain_.process(l, r, chunk);
        ++stats_.blocksProcessed[0];
        if (brainActive) ++stats_.activeBlocks[0];

        // Modulación del guardián + enrutado del upmix (con el cerebro ya actualizado).
        pushEffectiveControls();

        // 1..6 en orden canónico. Cada operador es bit-transparente en neutro.
        const bool active[7] = {
            brainActive,
            !spatial_.isNeutral(),
            !field_.isNeutral(),
            !listenerRoom_.isNeutral(),
            !transducer_.isNeutral(),
            !tonal_.isNeutral(),
            !dynamics_.isNeutral(),
        };
        spatial_.process(l, r, chunk);
        ++stats_.blocksProcessed[1];
        if (active[1]) ++stats_.activeBlocks[1];
        field_.process(l, r, chunk);
        ++stats_.blocksProcessed[2];
        if (active[2]) ++stats_.activeBlocks[2];
        listenerRoom_.process(l, r, chunk);
        ++stats_.blocksProcessed[3];
        if (active[3]) ++stats_.activeBlocks[3];
        transducer_.process(l, r, chunk);
        ++stats_.blocksProcessed[4];
        if (active[4]) ++stats_.activeBlocks[4];
        tonal_.process(l, r, chunk);
        ++stats_.blocksProcessed[5];
        if (active[5]) ++stats_.activeBlocks[5];
        dynamics_.process(l, r, chunk);
        ++stats_.blocksProcessed[6];
        if (active[6]) ++stats_.activeBlocks[6];

        if (apex) {
            // ── APEX 2: el TRANSITORIO (sin operadores) se retrasa ────────
            // por la línea de Lagrange (ITD fraccionario) y se SUMA a la
            // salida de los operadores. Los ataques no pasan por STFT ni
            // por compresor: no se emborronan ni se comprimen.
            for (int i = 0; i < chunk; ++i) {
                l[i] += apexDelayL_.process(apexTransL_[static_cast<std::size_t>(i)], apexDelayL);
                r[i] += apexDelayR_.process(apexTransR_[static_cast<std::size_t>(i)], apexDelayR);
            }
        }

        // ── 8. CONSERVACIÓN PERCEPTUAL (árbitro de campo, 2026-11-21) ────
        // Tras los 7 operadores + transitorios APEX, antes de TΩ. Mide el
        // estado de campo por banda, arbitra las propuestas de los 7
        // operadores y aplica la ganancia de conservación (<= 1, con rampa
        // por muestra). Con amount=0 no toca el buffer (bit-exacto) y el
        // coste es un atomic + una rama. El tiempo perceptual es el tiempo
        // monotónico de la cadena.
        conservation_.process(l, r, chunk, chainTime_);
        chainTime_ += static_cast<double>(chunk) / sampleRate_;

        // ── TΩ (F1/472): matriz 2×2 64-bit ───────────────────────────────
        // Corrección de altavoz al final (tras operadores + transitorios),
        // en el lugar que ocupaba la matriz volumétrica WOLA (LEGACY).
        if (tOmega) applyTOmega(l, r, chunk);

        // ── TECHO FINAL (P1 2026-11-22): el safety limiter de PULSO ya no
        // es la última barrera — APEX 2 suma el transitorio DESPUÉS de él
        // (hotmix_01: APEX ON +0.81 dBFS, APEX OFF −0.14 dBFS) y TΩ también
        // puede subir el pico. Este techo es la última barrera absoluta:
        // clava la salida a ±0.98855 (−0.1 dBFS, el MISMO techo del safety
        // limiter, para que ambos no diverjan). Transparente bajo el techo
        // (bit-idéntico: solo actúa cuando |x| > techo) y fail-closed.
        // Solo cuando la cadena NO es neutra: en bypass no corre y se
        // preserva la bit-exactitud. No cambia la ganancia neta (Regla de
        // Oro Matched-LUFS: 0.0 dB).
        if (!isNeutral()) {
            const double ceil = static_cast<double>(FusedDynamics::kLimiterCeiling);
            for (int i = 0; i < chunk; ++i) {
                l[i] = std::clamp(l[i], -ceil, ceil);
                r[i] = std::clamp(r[i], -ceil, ceil);
            }
        }

        ++stats_.totalBlocks;
        for (int i = 0; i < kNumOperators; ++i) telemetry_.active[i] = active[i];

        offset += chunk;
        remaining -= chunk;
    }
}

void FusedChain::processFloat(float* left, float* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    if (scratchL_.size() < static_cast<std::size_t>(kFusedMaxBlockFrames) ||
        scratchR_.size() < static_cast<std::size_t>(kFusedMaxBlockFrames)) {
        prepare(sampleRate_);
    }
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kFusedMaxBlockFrames);
        for (int i = 0; i < chunk; ++i) {
            scratchL_[static_cast<std::size_t>(i)] = static_cast<double>(left[offset + i]);
            scratchR_[static_cast<std::size_t>(i)] = static_cast<double>(right[offset + i]);
        }
        process(scratchL_.data(), scratchR_.data(), chunk);
        for (int i = 0; i < chunk; ++i) {
            const double vl = scratchL_[static_cast<std::size_t>(i)];
            const double vr = scratchR_[static_cast<std::size_t>(i)];
            left[offset + i] = std::isfinite(vl) ? static_cast<float>(vl) : 0.0f;
            right[offset + i] = std::isfinite(vr) ? static_cast<float>(vr) : 0.0f;
        }
        offset += chunk;
        remaining -= chunk;
    }
}

}  // namespace ivanna::fused
