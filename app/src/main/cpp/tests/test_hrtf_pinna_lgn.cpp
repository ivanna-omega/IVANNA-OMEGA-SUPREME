// test_hrtf_pinna_lgn.cpp — LGN cableada en HrtfManager::synthesizeHrtf.
// Verifica que el pabellón (Ley Geodésica del Notch, física nueva 2026-11-18)
// realmente se aplica al HRIR sintetizado, que el notch medido coincide con
// la predicción de la LGN, y que setPinnaEnabled(false) deja la esfera sola
// (fallback). Método: respuesta al impulso del HRTF (canal L) y cociente
// espectral ON/OFF, que aísla el factor de pabellón F_pinna (la base, el
// ILD, el ITD y la normalización son idénticos en ambos casos salvo una
// constante de escala, que se cancela con la referencia de 500 Hz, donde
// |F_pinna| ≈ 0.992 ≈ 1).
#include <gtest/gtest.h>
#include "HrtfManager.hpp"
#include "fused/geodesic_pinna_model.hpp"
#include <cmath>
#include <complex>
#include <cstring>
#include <memory>
#include <vector>

using namespace Ivanna;

namespace {
constexpr float kSr = 48000.0f;
constexpr int kSettleBlocks = 140;  // > XFADE_FRAMES (128): el banco nuevo manda
constexpr int kCaptureBlocks = 4;   // 512 muestras: cabe el HRIR (128 + pinna)

// Respuesta al impulso del HRTF (canal L) tras asentar el crossfade.
// Entrada: impulso 1.0 en la muestra 0 del canal L, resto 0. Con wet=1 la
// salida es la convolución pura (sin seco): outL[i] = hrir[i].
std::vector<float> impulseResponseL(HrtfManager& h) {
    AudioBuffer silence{};
    std::memset(&silence, 0, sizeof(silence));
    for (int b = 0; b < kSettleBlocks; ++b) h.processBinauralScene(&silence);
    std::vector<float> out;
    for (int b = 0; b < kCaptureBlocks; ++b) {
        AudioBuffer buf{};
        std::memset(&buf, 0, sizeof(buf));
        if (b == 0) buf.left[0] = 1.0f;
        h.processBinauralScene(&buf);
        out.insert(out.end(), buf.left, buf.left + BLOCK_SIZE);
    }
    return out;
}

// |DTFT| de la respuesta a la frecuencia f (Hz), SR 48 kHz.
double dtftMag(const std::vector<float>& x, double freqHz) {
    std::complex<double> s(0.0, 0.0);
    const double w = 2.0 * 3.14159265358979323846 * freqHz / static_cast<double>(kSr);
    for (size_t n = 0; n < x.size(); ++n)
        s += static_cast<double>(x[n]) *
             std::exp(std::complex<double>(0.0, -w * static_cast<double>(n)));
    return std::abs(s);
}

double rmsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    double s = 0.0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
        s += d * d;
    }
    return std::sqrt(s / std::max<size_t>(1, n));
}

std::unique_ptr<HrtfManager> mkHrtf(bool pinnaEnabled) {
    auto h = std::make_unique<HrtfManager>();
    h->prepare(kSr);
    h->setPinnaEnabled(pinnaEnabled);
    h->setHeadPose(0.f, 0.f, 0.f);  // frontal, elevación 0
    h->setWetDry(1.f);
    return h;
}

// Respuesta al impulso en AMBOS canales (L y R) tras asentar el crossfade.
// Se alimenta un impulso por el canal indicado (0 = L, 1 = R) y se devuelve
// la salida de ese mismo canal (el HRIR directo LL o RR). Sirve para medir
// el ITD entre oídos por xcorr.
std::vector<float> impulseResponseDirect(HrtfManager& h, int channel) {
    AudioBuffer silence{};
    std::memset(&silence, 0, sizeof(silence));
    for (int b = 0; b < kSettleBlocks; ++b) h.processBinauralScene(&silence);
    std::vector<float> out;
    for (int b = 0; b < kCaptureBlocks; ++b) {
        AudioBuffer buf{};
        std::memset(&buf, 0, sizeof(buf));
        if (b == 0) {
            if (channel == 0) buf.left[0] = 1.0f;
            else buf.right[0] = 1.0f;
        }
        h.processBinauralScene(&buf);
        const float* ch = (channel == 0) ? buf.left : buf.right;
        out.insert(out.end(), ch, ch + BLOCK_SIZE);
    }
    return out;
}

// Centroide de energía (muestras) de una respuesta al impulso. El ITD
// entre oídos es la diferencia de centroides: es robusto para HRIR difusos,
// donde la xcorr es inestable (los picos de L y R no coinciden).
double energyCentroid(const std::vector<float>& x) {
    double e = 0.0, s = 0.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double v = static_cast<double>(x[i]);
        e += v * v;
        s += static_cast<double>(i) * v * v;
    }
    return e > 0.0 ? s / e : 0.0;
}
}  // namespace

TEST(HrtfPinnaLgn, OnChangesOutputVsOff) {
    auto on = mkHrtf(true);
    auto off = mkHrtf(false);
    const auto hOn = impulseResponseL(*on);
    const auto hOff = impulseResponseL(*off);
    // El pabellón cambia el HRIR: la salida con LGN difiere de la esfera sola.
    EXPECT_GT(rmsDiff(hOn, hOff), 0.01)
        << "LGN ON y OFF dan la misma salida: el pabellón no está cableado";
    // Ambas son filtros no triviales (no silencio, no identidad).
    double eOn = 0, eOff = 0;
    for (float v : hOn) eOn += static_cast<double>(v) * v;
    for (float v : hOff) eOff += static_cast<double>(v) * v;
    EXPECT_GT(eOn, 1e-6);
    EXPECT_GT(eOff, 1e-6);
}

TEST(HrtfPinnaLgn, NotchMatchesPrediction) {
    auto on = mkHrtf(true);
    auto off = mkHrtf(false);
    const auto hOn = impulseResponseL(*on);
    const auto hOff = impulseResponseL(*off);
    // Notches predichos por la LGN a elev 0, az 0 (KEMAR).
    const double fLow = ivanna::physical::pinnaNotchFrequencyHz(
        ivanna::physical::kPinnaLowFamily, 0.0);
    const double fHigh = ivanna::physical::pinnaNotchFrequencyHz(
        ivanna::physical::kPinnaHighFamily, 0.0);
    ASSERT_GT(fLow, 0.0);
    ASSERT_GT(fHigh, 0.0);
    // R(f) = |H_ON|/|H_OFF| = C * |F_pinna(f)|; C se cancela con la
    // referencia de 500 Hz, donde |F_pinna| ≈ 0.992 ≈ 1.
    const double rRef = dtftMag(hOn, 500.0) / dtftMag(hOff, 500.0);
    ASSERT_GT(rRef, 0.0);
    const double freqs[] = {500.0, 5000.0, fLow, fHigh, 14000.0};
    for (double f : freqs) {
        const double r = dtftMag(hOn, f) / dtftMag(hOff, f) / rRef;
        const double expected =
            std::abs(ivanna::physical::pinnaFactor(f, 0.0, 0.0));
        EXPECT_NEAR(r, expected, 0.15)
            << "f=" << f << " Hz: |H_ON|/|H_OFF| normalizado no coincide con |F_pinna|";
    }
    // El notch bajo es un dip claro frente a la referencia de 5 kHz.
    const double rLow = dtftMag(hOn, fLow) / dtftMag(hOff, fLow) / rRef;
    const double r5k = dtftMag(hOn, 5000.0) / dtftMag(hOff, 5000.0) / rRef;
    EXPECT_LT(rLow, r5k * 0.7) << "el notch bajo no es un dip frente a 5 kHz";
}

TEST(HrtfPinnaLgn, FlagTogglesSynthesis) {
    auto h = mkHrtf(false);
    const auto offOut = impulseResponseL(*h);
    // Reactivar el pabellón y re-sintetizar (setHeadPose) restaura la LGN.
    h->setPinnaEnabled(true);
    h->setHeadPose(0.f, 0.f, 0.f);
    const auto onOut = impulseResponseL(*h);
    EXPECT_GT(rmsDiff(offOut, onOut), 0.01)
        << "setPinnaEnabled(true) no restaura el pabellón";
    // Y coincide con un manager nuevo con LGN activa (determinista).
    auto fresh = mkHrtf(true);
    const auto freshOut = impulseResponseL(*fresh);
    EXPECT_LT(rmsDiff(onOut, freshOut), 1e-6)
        << "la re-síntesis con LGN no es determinista";
}

// ── CURVA ITD (Ley de Curvatura, 2026-11-18) ─────────────────────────────────
// El ITD de azimut es la Ley de Curvatura en forma cerrada (recupera
// Woodworth exacto en campo lejano). Se mide por DIFERENCIA de centroides
// entre 90° y frontal. ANTES (2026-11-18) se usaba la diferencia (no el
// absoluto) porque el par base PCA tenía un offset constante (pedestal)
// que era un problema de DATOS (falta un HRIR frontal real), no de la
// fórmula de ITD. REPARADO evol-hrtf 2026-11-26: la base PCA es
// monoaural (E_R≈0) y el oído derecho usa la reconstrucción válida (L),
// así que el frontal es simétrico (itdFront = 0) y la variación ES el
// ITD absoluto. El valor esperado NO es 31–32 muestras (Woodworth 656 µs
// @1 m): la Ley de Curvatura se multiplica por la escala Riemanniana
// (1 + 0.15·sin(distancia geodésica) = 1.15 a 90°) y lleva el exceso de
// campo cercano: 656 µs × 1.15 + exceso ≈ 770 µs ≈ 37 muestras @48 kHz.
// La cota vieja [28, 36] solo pasaba porque el pedestal frontal (−16.1
// muestras de centroide) cancelaba parte del valor a 90° (−50.8);
// con el pedestal reparado la variación real es 36.86 muestras.
// El ITD se mide por diferencia de centroides de energía entre los HRIR
// LL y RR (robusto para HRIR difusos, donde la xcorr es inestable).
TEST(HrtfPinnaLgn, ItdAt90DegMatchesWoodworth) {
    // ITD frontal (referencia, incluye el offset constante del par base).
    auto hF = mkHrtf(true);
    const auto hFL = impulseResponseDirect(*hF, 0);
    auto hF2 = mkHrtf(true);
    const auto hFR = impulseResponseDirect(*hF2, 1);
    const double itdFront = energyCentroid(hFR) - energyCentroid(hFL);
    // ITD a 90° a la derecha: yaw = -90° (eff_azimuth = -yaw = +90°).
    auto h = std::make_unique<HrtfManager>();
    h->prepare(kSr);
    h->setPinnaEnabled(true);
    h->setHeadPose(-1.5707963f, 0.f, 0.f);
    h->setWetDry(1.f);
    const auto hL = impulseResponseDirect(*h, 0);
    auto h2 = std::make_unique<HrtfManager>();
    h2->prepare(kSr);
    h2->setPinnaEnabled(true);
    h2->setHeadPose(-1.5707963f, 0.f, 0.f);
    h2->setWetDry(1.f);
    const auto hR = impulseResponseDirect(*h2, 1);
    const double itd90 = energyCentroid(hR) - energyCentroid(hL);
    // Frontal reparado (evol-hrtf): el par PCA es simétrico, el ITD
    // frontal es 0 (antes: −16.1 muestras de pedestal).
    EXPECT_LE(std::fabs(itdFront), 2.0)
        << "ITD frontal distinto de 0: el pedestal del par PCA volvió (frontal "
        << itdFront << " muestras)";
    // Variación azimutal: Ley de Curvatura × escala Riemanniana @1 m ≈
    // 770 µs = 37 muestras. La aproximación vieja daba 18 muestras.
    const double swing = std::fabs(itd90 - itdFront);
    EXPECT_GE(swing, 32.0)
        << "variación de ITD a 90° demasiado corta (aproximación vieja): "
        << swing << " muestras (frontal " << itdFront << ", 90° " << itd90 << ")";
    EXPECT_LE(swing, 42.0)
        << "variación de ITD a 90° demasiado larga: " << swing << " muestras";
}
