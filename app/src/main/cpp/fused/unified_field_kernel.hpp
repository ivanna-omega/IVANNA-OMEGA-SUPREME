#pragma once
// unified_field_kernel.hpp — IVANNA-APEX: Operador Matricial Único 2x2 en 64-bit.
// Unifica en un solo dominio espectral (doble precisión, cero cascadas IIR/STFT):
//   1) Reconstrucción de fase mínima exacta por Hilbert-Cepstrum (Sala + Tonal + Φ + ISO-226).
//   2) Inversión acústica 2x2 Tikhonov-Kirkeby con difracción por esfera rígida (Altavoces Genezi)
//      + transición coherente de fase en sub-graves (< xoverHz, AuxField).
//   3) Render espacial continuo por curvatura κ=1/r + dispersión exacta por esfera rígida (Audífonos).
//   4) Estimación de distancia fusionada (exceso ITD en graves + ILD en medios/agudos).
//
// RT-safe: cero heap allocations usando UnifiedFieldWorkspace pre-reservado.
//
// Fuente: propuesta IVANNA-APEX (revisión externa 2026-11-18). Verificar con
// CTest antes de conectar a FusedChain. FIX: el default de estimateDistanceFusedM
// usaba ivanna::physical::kHeadRadiusKemarM, pero ese símbolo vive en
// ivanna::fused::propagation; se usa ivanna::physical::kHeadRadiusM (mismo valor 0.0875).

#include "fused/physical_head_model.hpp"
#include "fused/physical_propagation.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace ivanna::fused::apex {

inline constexpr int kMaxUnifiedFft = 512;
inline constexpr int kMaxUnifiedBins = kMaxUnifiedFft / 2 + 1;
inline constexpr double kPiUnified = 3.14159265358979323846;

// Matriz compleja 2x2 por bin para salida estéreo unificada:
//   [YL; YR] = [m00 m01; m10 m11] * [XL; XR]
struct ComplexMatrix2x2 {
    std::complex<double> m00{1.0, 0.0};
    std::complex<double> m01{0.0, 0.0};
    std::complex<double> m10{0.0, 0.0};
    std::complex<double> m11{1.0, 0.0};
};

struct SpeakerPairPhysics {
    double spanM = 0.90;            // separación entre altavoces (m)
    double distanceM = 0.85;        // distancia perpendicular al plano del par (m)
    double listenerOffsetM = 0.0;   // corrimiento lateral del oyente (m, + derecha)
    double headRadiusM = ivanna::physical::kHeadRadiusM;
    double speedOfSoundMps = ivanna::physical::kSpeedOfSoundMps;
    double bassXoverHz = 120.0;     // frecuencia de coherencia de graves (AuxField)
    double bassCoherence = 1.0;     // 0..1 coherencia L/R bajo bassXoverHz
    double maxBoostDb = 3.0;        // cota dura de ganancia singular en XTC (adiós pico +4/+6 dB)
    double regMid = 0.06;           // regularización Kirkeby en 300..5000 Hz
    double regHigh = 0.35;          // regularización Kirkeby > 6 kHz (protección de sweet spot)
};

struct UnifiedFieldWorkspace {
    ivanna::physical::RigidSphereWorkspace sphereWs;
    std::array<std::complex<double>, kMaxUnifiedFft> fftBuf{};
    std::array<double, kMaxUnifiedBins> logMagBuf{};
    std::array<std::complex<double>, kMaxUnifiedBins> minPhaseBuf{};
    std::array<ComplexMatrix2x2, kMaxUnifiedBins> outMatrix{};
};

// ── 1) FFT Radix-2 iterativa en 64-bit double (cero malloc, N potencia de 2 <= kMaxUnifiedFft) ──
inline void fftRadix2Double(std::complex<double>* x, int n, bool inverse) noexcept {
    if (!x || n <= 1 || (n & (n - 1)) != 0 || n > kMaxUnifiedFft) return;
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = (inverse ? 2.0 : -2.0) * kPiUnified / static_cast<double>(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            const int half = len >> 1;
            for (int k = 0; k < half; ++k) {
                const std::complex<double> u = x[i + k];
                const std::complex<double> v = x[i + k + half] * w;
                x[i + k] = u + v;
                x[i + k + half] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse) {
        const double inv = 1.0 / static_cast<double>(n);
        for (int i = 0; i < n; ++i) x[i] *= inv;
    }
}

// ── 2) Transformada Discreta de Hilbert por Cepstrum Real (Fase Mínima Exacta en 64-bit) ──
// Entrada: magDbHalf[0..nFft/2] en dB (cualquier suma de Sala cut-only + Tonal + Φ + ISO-226).
// Salida:  outMinPhase[0..nFft/2] complejo causal de fase mínima con |H_min[k]| == 10^(dB/20).
// Cero pre-ringing antes del impulso. Error de magnitud < 1e-12 dB.
inline bool buildMinimumPhaseSpectrum64(const double* magDbHalf, int nFft,
                                        std::complex<double>* outMinPhase,
                                        UnifiedFieldWorkspace& ws) noexcept {
    if (!magDbHalf || !outMinPhase || nFft < 16 || nFft > kMaxUnifiedFft ||
        (nFft & (nFft - 1)) != 0) {
        return false;
    }
    const int half = nFft >> 1;
    constexpr double kDbToLn = 0.11512925464970228420; // ln(10) / 20

    // Espectro real par de log-magnitud.
    for (int k = 0; k <= half; ++k) {
        const double db = std::isfinite(magDbHalf[k]) ? std::clamp(magDbHalf[k], -60.0, 24.0) : 0.0;
        const double lnMag = db * kDbToLn;
        ws.fftBuf[static_cast<std::size_t>(k)] = std::complex<double>(lnMag, 0.0);
        if (k > 0 && k < half) {
            ws.fftBuf[static_cast<std::size_t>(nFft - k)] = std::complex<double>(lnMag, 0.0);
        }
    }

    // 1. Cepstrum real = IFFT(log|H|).
    fftRadix2Double(ws.fftBuf.data(), nFft, /*inverse=*/true);

    // 2. Ventana causal de fase mínima en el dominio cepstral.
    ws.fftBuf[0] = std::complex<double>(ws.fftBuf[0].real(), 0.0);
    for (int n = 1; n < half; ++n) {
        ws.fftBuf[static_cast<std::size_t>(n)] =
            std::complex<double>(2.0 * ws.fftBuf[static_cast<std::size_t>(n)].real(), 0.0);
    }
    ws.fftBuf[static_cast<std::size_t>(half)] =
        std::complex<double>(ws.fftBuf[static_cast<std::size_t>(half)].real(), 0.0);
    for (int n = half + 1; n < nFft; ++n) {
        ws.fftBuf[static_cast<std::size_t>(n)] = std::complex<double>(0.0, 0.0);
    }

    // 3. Regresar a frecuencia: Re = log|H|, Im = fase mínima exacta (convención DFT e^{-iωn}).
    fftRadix2Double(ws.fftBuf.data(), nFft, /*inverse=*/false);

    for (int k = 0; k <= half; ++k) {
        const std::complex<double> z = ws.fftBuf[static_cast<std::size_t>(k)];
        const double re = z.real();
        // DC (0) y Nyquist (half) deben quedar estrictamente reales para preservar simetría hermitiana.
        const double im = (k == 0 || k == half) ? 0.0 : z.imag();
        outMinPhase[k] = std::exp(std::complex<double>(re, im));
    }
    return true;
}

// ── 3) Inversor Acústico 2x2 Tikhonov-Kirkeby para Altavoces + Esfera Rígida + AuxField ──
// Modela la propagación física real altavoz→oídos A(ω) usando:
//   - Retardos y divergencia 1/d de los 4 caminos (dLL, dLR, dRL, dRR)
//   - Difracción exacta de esfera rígida (rigidSphereTransfer, conjugada a convención DFT e^{-iωt})
//   - Regularización β(ω) dependiente de frecuencia + límite de valor singular (maxBoostDb)
//   - Transición suave hacia campo coherente de presión en subgraves (< bassXoverHz).
inline ComplexMatrix2x2 computeSpeakersMatrixBin(double freqHz,
                                                 const SpeakerPairPhysics& sp,
                                                 ivanna::physical::RigidSphereWorkspace& sphereWs) noexcept {
    ComplexMatrix2x2 W{};
    if (!(freqHz > 0.0) || !std::isfinite(freqHz)) {
        // DC: sin XTC; aplicar coherencia de graves pura si está activa.
        const double k = std::clamp(sp.bassCoherence, 0.0, 1.0);
        const double d = 1.0 - 0.5 * k;
        const double c = 0.5 * k;
        W.m00 = {d, 0.0}; W.m01 = {c, 0.0};
        W.m10 = {c, 0.0}; W.m11 = {d, 0.0};
        return W;
    }

    const double a = std::clamp(sp.headRadiusM, 0.05, 0.15);
    const double cSound = std::clamp(sp.speedOfSoundMps, 300.0, 380.0);
    const double halfSpan = std::clamp(sp.spanM, 0.15, 5.0) * 0.5;
    const double zDist = std::clamp(sp.distanceM, 0.25, 6.0);
    const double offset = std::clamp(sp.listenerOffsetM, -1.0, 1.0);

    const double dxL = -halfSpan - offset;
    const double dxR = +halfSpan - offset;
    const double dLL = std::hypot(dxL + a, zDist);
    const double dLR = std::hypot(dxL - a, zDist);
    const double dRL = std::hypot(dxR + a, zDist);
    const double dRR = std::hypot(dxR - a, zDist);

    const double rCenterL = std::hypot(dxL, zDist);
    const double rCenterR = std::hypot(dxR, zDist);
    // cosGamma según convención de physical_head_model.hpp: dot(-s, ear) = -(dx/r)*ear_x.
    const double cgLL = std::clamp(dxL / rCenterL, -1.0, 1.0);
    const double cgLR = std::clamp(-dxL / rCenterL, -1.0, 1.0);
    const double cgRL = std::clamp(dxR / rCenterR, -1.0, 1.0);
    const double cgRR = std::clamp(-dxR / rCenterR, -1.0, 1.0);

    // Difracción exacta de esfera rígida (conjugada de e^{-iωt} a DFT e^{-iωn}).
    const std::complex<double> sLL = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgLL, a, cSound, sphereWs));
    const std::complex<double> sLR = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgLR, a, cSound, sphereWs));
    const std::complex<double> sRL = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgRL, a, cSound, sphereWs));
    const std::complex<double> sRR = std::conj(
        ivanna::physical::rigidSphereTransfer(freqHz, cgRR, a, cSound, sphereWs));

    // Normalizar caminos respecto a la llegada ipsilateral media (preserva 0 dB neto y quita
    // el retardo común de vuelo del cuarto para operar en fase causal mínima).
    const double dRef = 0.5 * (dLL + dRR);
    const double omega = 2.0 * kPiUnified * freqHz;
    auto pathTransfer = [&](double d, const std::complex<double>& diff) {
        const double mag = dRef / std::max(d, 0.1);
        const double ph = -omega * (d - dRef) / cSound;
        return mag * std::exp(std::complex<double>(0.0, ph)) * diff;
    };

    // Matriz de planta A(ω): [oído L; oído R] = A * [bafle L; bafle R].
    const std::complex<double> A00 = pathTransfer(dLL, sLL);
    const std::complex<double> A01 = pathTransfer(dRL, sRL);
    const std::complex<double> A10 = pathTransfer(dLR, sLR);
    const std::complex<double> A11 = pathTransfer(dRR, sRR);

    // Regularización moldeada por frecuencia:
    //  - < bassXoverHz: β grande (sin XTC en graves) + mezcla hacia coherencia de graves.
    //  - 300..5000 Hz: regMid (máxima cancelación limpia).
    //  - > 5000 Hz: sube suavemente hacia regHigh (sweet spot amplio).
    const double fCutLow = std::clamp(sp.bassXoverHz, 40.0, 250.0);
    const double lowRoll = 1.0 / (1.0 + std::pow(freqHz / fCutLow, 4.0));   // LR4 low-weight
    const double hiWeight = std::clamp((freqHz - 5000.0) / 5000.0, 0.0, 1.0);
    const double beta = sp.regMid + hiWeight * (sp.regHigh - sp.regMid) + lowRoll * 4.0;

    // R = A^H * A + beta * I (Hermítica 2x2 definida positiva).
    const double r00 = std::norm(A00) + std::norm(A10) + beta;
    const double r11 = std::norm(A01) + std::norm(A11) + beta;
    const std::complex<double> r01 = std::conj(A00) * A01 + std::conj(A10) * A11;
    const double det = r00 * r11 - std::norm(r01);
    if (!(det > 1e-18) || !std::isfinite(det)) return W;

    const double invDet = 1.0 / det;
    const std::complex<double> inv00(r11 * invDet, 0.0);
    const std::complex<double> inv01 = -r01 * invDet;
    const std::complex<double> inv10 = -std::conj(r01) * invDet;
    const std::complex<double> inv11(r00 * invDet, 0.0);

    // W_xtc = R^{-1} * A^H (compensa además retardo de planta manteniendo unidad en suma frontal).
    std::complex<double> w00 = inv00 * std::conj(A00) + inv01 * std::conj(A01);
    std::complex<double> w01 = inv00 * std::conj(A10) + inv01 * std::conj(A11);
    std::complex<double> w10 = inv10 * std::conj(A00) + inv11 * std::conj(A01);
    std::complex<double> w11 = inv10 * std::conj(A10) + inv11 * std::conj(A11);

    // Cota exacta de ganancia peor caso (mono + estéreo) <= maxBoostDb (p.ej. +3.0 dB).
    const double maxLin = std::pow(10.0, std::clamp(sp.maxBoostDb, 0.5, 9.0) / 20.0);
    const double rowNormL = std::abs(w00) + std::abs(w01);
    const double rowNormR = std::abs(w10) + std::abs(w11);
    const double worstGain = std::max(rowNormL, rowNormR);
    if (worstGain > maxLin && worstGain > 1e-12) {
        const double sc = maxLin / worstGain;
        w00 *= sc; w01 *= sc; w10 *= sc; w11 *= sc;
    }

    // Mezcla continua LR4 hacia AuxFieldCoherence en subgraves (< bassXoverHz).
    const double k = std::clamp(sp.bassCoherence, 0.0, 1.0);
    const double highWeight = 1.0 - lowRoll;
    const double auxDiag = 1.0 - 0.5 * k;
    const double auxCross = 0.5 * k;

    W.m00 = highWeight * w00 + lowRoll * std::complex<double>(auxDiag, 0.0);
    W.m01 = highWeight * w01 + lowRoll * std::complex<double>(auxCross, 0.0);
    W.m10 = highWeight * w10 + lowRoll * std::complex<double>(auxCross, 0.0);
    W.m11 = highWeight * w11 + lowRoll * std::complex<double>(auxDiag, 0.0);

    // Renormalización del mono <80 Hz (F1/472): la mezcla LR4 hacia
    // AuxField deja el mono en −1.1 dB a 80 Hz (−0.39 dB a 60 Hz). Un
    // escalar por bin, IGUAL en ambas filas, devuelve el mono a 0.00 dB
    // para f ≤ 80 Hz con taper suave 80→120 Hz hasta 1.0 (sin
    // discontinuidad). Al ser un escalar común no toca la separación
    // A·W (cociente) ni la simetría L/R, y la ganancia de fila queda
    // ≤ ~+1.1 dB, muy por debajo de la cota maxBoostDb (+3 dB). A DC el
    // mono ya es exactamente 1.0 (rama temprana), así que no se toca.
    if (freqHz > 0.0 && freqHz < 120.0) {
        const double monoL = std::abs(W.m00 + W.m01);
        const double monoR = std::abs(W.m10 + W.m11);
        const double meanMono = 0.5 * (monoL + monoR);
        if (meanMono > 1e-12 && std::isfinite(meanMono)) {
            const double t = (freqHz <= 80.0) ? 1.0 : (120.0 - freqHz) / 40.0;
            const double s = 1.0 + (1.0 / meanMono - 1.0) * t;
            W.m00 *= s; W.m01 *= s; W.m10 *= s; W.m11 *= s;
        }
    }
    return W;
}

// ── 4) Estimador Fusionado de Distancia (Curvatura ITD en graves + ILD en agudos) ──
// Resuelve el punto ciego de distancia en graves donde el ILD cae a ~0.
// Devuelve distancia estimada en [0.15, 2.0] m.
inline double estimateDistanceFusedM(double freqHz, double azimuthRad,
                                     double measuredItdSec, double fallbackIldDistM,
                                     double headRadiusM = ivanna::physical::kHeadRadiusM) noexcept {
    const double safeFallback = std::isfinite(fallbackIldDistM)
        ? std::clamp(fallbackIldDistM, 0.15, 2.0) : 0.35;
    if (!std::isfinite(freqHz) || !std::isfinite(azimuthRad) || !std::isfinite(measuredItdSec))
        return safeFallback;

    // En graves (< 750 Hz) y fuera del eje central (|sin θ| > 0.707), la inversión por
    // exceso de ITD sobre Woodworth es independiente del nivel.
    const double absSin = std::fabs(std::sin(azimuthRad));
    if (freqHz < 750.0 && absSin > 0.707) {
        const double woodworth = ivanna::fused::propagation::curvatureLawItdSeconds(
            azimuthRad, 100.0, headRadiusM);
        const double excess = std::fabs(measuredItdSec) - std::fabs(woodworth);
        const double rItd = ivanna::fused::propagation::distanceFromItdExcess(excess, headRadiusM);
        if (rItd > 0.0) {
            // En graves el ILD es ~0, así que el fallback ILD no aporta
            // información: a |sin θ| ≥ 0.907 la inversión ITD→distancia se
            // confía al 100% (w=1, recuperación exacta, error ~0); por
            // debajo, rampa 0.707→0.907. Mezclar hacia un fallback erróneo
            // solo sumaba error (antes: 0.0113 m a 85° con fallback 1.0 m;
            // ahora: <0.01 m, criterio F1).
            const double w = std::clamp((absSin - 0.707) / 0.20, 0.0, 1.0);
            const double rClamped = std::clamp(rItd, 0.15, 2.0);
            return safeFallback + w * (rClamped - safeFallback);
        }
    }
    return safeFallback;
}

// ── 5) Composición Matricial Única por Bin: T(ω) = W_out(ω) * H_min(ω) ──
// Combina la matriz espacial/XTC 2x2 y la respuesta tonal+sala de fase mínima en una sola
// aplicación en 64-bit double: T_out * [XL; XR].
inline void applyUnifiedMatrix2x2(std::complex<double>* specL,
                                  std::complex<double>* specR,
                                  int nFft,
                                  const ComplexMatrix2x2* matricesHalf,
                                  const std::complex<double>* minPhaseHalf) noexcept {
    if (!specL || !specR || !matricesHalf || !minPhaseHalf || nFft < 4) return;
    const int half = nFft >> 1;
    for (int k = 0; k <= half; ++k) {
        const std::complex<double> h = minPhaseHalf[k];
        const ComplexMatrix2x2& M = matricesHalf[k];
        const std::complex<double> xL = specL[k] * h;
        const std::complex<double> xR = specR[k] * h;
        const std::complex<double> yL = M.m00 * xL + M.m01 * xR;
        const std::complex<double> yR = M.m10 * xL + M.m11 * xR;
        // DC (k=0) y Nyquist (k=half) de una señal real DEBEN ser reales:
        // la matriz XTC a 24 kHz es compleja y sin este forzado el IFFT
        // deja un residuo imaginario (~2.5e-05). Se conserva la parte
        // real (la imaginaria es espuria para una señal real).
        if (k == 0 || k == half) {
            specL[k] = std::complex<double>(yL.real(), 0.0);
            specR[k] = std::complex<double>(yR.real(), 0.0);
        } else {
            specL[k] = yL;
            specR[k] = yR;
        }
        if (k > 0 && k < half) {
            specL[nFft - k] = std::conj(yL);
            specR[nFft - k] = std::conj(yR);
        }
    }
}

} // namespace ivanna::fused::apex
