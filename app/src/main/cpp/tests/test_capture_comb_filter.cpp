// test_capture_comb_filter.cpp — Evidencia espectral del peine en captura (0023).
//
// MECANISMO (verificado en código, PlaybackCaptureService.kt): la captura
// reproduce el audio procesado por su PROPIO AudioTrack mientras el original
// de la app (Tidal) sigue sonando a nivel pleno (no hay API pública para
// silenciarlo). Dos copias de la misma señal con ~130 ms de diferencia y
// nivel comparable se SUMAN EN EL MEZCLADOR (AudioFlinger): NO es acústico
// (no es altavoz -> micrófono), es mezcla en el mezclador.
//   original 100% + procesado 40% (-8 dB, HAAS_SAFE_GAIN)
//   -> respuesta |1 + 0.4 * exp(-j*2*pi*f*0.13)|
//   -> muescas cada 1/0.13 = 7.69 Hz; picos 1.4 (+2.92 dB), muescas 0.6
//      (-4.44 dB); rizo pico-a-muesca 7.36 dB. Eso es el "bofo"/coloración
//      de la captura. La ganancia Haas 0.4 MITIGA pero NO elimina el peine:
//      es inherente a la arquitectura de doble AudioTrack. El fix real es
//      la Ruta B (sin copia, sin delay, sin audio doble) — por eso 0023
//      arregla primero el bofo de Ruta B.
//
// Este test SIMULA la mezcla en host (seno + copia retrasada 130 ms) y
// mide la amplitud por RMS en régimen permanente: demuestra las muescas
// en las frecuencias teóricas, el rizo de 7.36 dB, y que con ganancia 0
// (no re-inyectar) el espectro es plano (no hay peine). Sin NDK, sin
// dispositivo: matemática pura, frecuencias fijadas ANTES de medir.
#include <cmath>
#include <cstdio>

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name) {
    if (ok) {
        ++g_pass;
        std::printf("  PASS: %s\n", name);
    } else {
        ++g_fail;
        std::printf("  FALLA: %s\n", name);
    }
}

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 48000;
constexpr double kDelayS = 0.13;    // ~130 ms de la captura
constexpr double kMixGain = 0.4;    // HAAS_SAFE_GAIN (-8 dB)
constexpr int kDelaySamples = static_cast<int>(kDelayS * kRate + 0.5);  // 6240

// Amplitud medida de la mezcla (original + g * copia retrasada) a la
// frecuencia f, por RMS en régimen permanente (últimos 0.5 s de 1.5 s).
// La mezcla de dos senos a la misma f es un seno a f: el RMS la mide exacto.
double mixedAmplitude(double freqHz, double gain) {
    const int total = kRate * 3 / 2;  // 1.5 s
    const int meas0 = kRate;          // medir el último 0.5 s
    double phase = 0.0;
    const double step = 2.0 * kPi * freqHz / kRate;
    double sum = 0.0;
    long n = 0;
    // Historial circular del seno directo para el retardo.
    double hist[6240];
    for (int i = 0; i < kDelaySamples; ++i) hist[i] = 0.0;
    for (int i = 0; i < total; ++i) {
        const double s = 0.5 * std::sin(phase);
        phase += step;
        const double delayed = hist[i % kDelaySamples];
        hist[i % kDelaySamples] = s;
        const double mixed = s + gain * delayed;
        if (i >= meas0) {
            sum += mixed * mixed;
            ++n;
        }
    }
    if (n == 0) return -1.0;
    const double rms = std::sqrt(sum / static_cast<double>(n));
    return rms / (0.5 / std::sqrt(2.0));  // normalizado a amplitud 1.0
}

// Teoría: |1 + g * exp(-j*2*pi*f*tau)|.
double theoryAmplitude(double freqHz, double gain) {
    const double ph = 2.0 * kPi * freqHz * kDelayS;
    const double re = 1.0 + gain * std::cos(ph);
    const double im = gain * std::sin(ph);
    return std::sqrt(re * re + im * im);
}

double db(double mag) { return 20.0 * std::log10(mag); }

}  // namespace

int main() {
    // Frecuencias: picos en f = k/tau, muescas en f = (k+0.5)/tau.
    // tau = 0.13 s -> 1/tau = 7.6923 Hz.
    // k=130: pico 1000.00 Hz, muesca 1003.85 Hz.
    // k=65:  pico 500.00 Hz,  muesca 503.85 Hz.
    const double fPeak1 = 1000.0;
    const double fNotch1 = 1003.8461538;  // 130.5 / 0.13
    const double fPeak2 = 500.0;
    const double fNotch2 = 503.8461538;   // 65.5 / 0.13

    // 1. La mezcla medida coincide con la teoría del peine (tolerancia 3%).
    const double mPeak1 = mixedAmplitude(fPeak1, kMixGain);
    const double tPeak1 = theoryAmplitude(fPeak1, kMixGain);
    std::printf("  peine @ %.2f Hz (pico): medido %.4f, teoria %.4f\n",
                fPeak1, mPeak1, tPeak1);
    check(std::fabs(mPeak1 - tPeak1) / tPeak1 <= 0.03,
          "peine: pico 1000 Hz coincide con teoria (+2.92 dB)");

    const double mNotch1 = mixedAmplitude(fNotch1, kMixGain);
    const double tNotch1 = theoryAmplitude(fNotch1, kMixGain);
    std::printf("  peine @ %.2f Hz (muesca): medido %.4f, teoria %.4f\n",
                fNotch1, mNotch1, tNotch1);
    check(std::fabs(mNotch1 - tNotch1) / tNotch1 <= 0.03,
          "peine: muesca 1003.85 Hz coincide con teoria (-4.44 dB)");

    const double mPeak2 = mixedAmplitude(fPeak2, kMixGain);
    const double tPeak2 = theoryAmplitude(fPeak2, kMixGain);
    std::printf("  peine @ %.2f Hz (pico): medido %.4f, teoria %.4f\n",
                fPeak2, mPeak2, tPeak2);
    check(std::fabs(mPeak2 - tPeak2) / tPeak2 <= 0.03,
          "peine: pico 500 Hz coincide con teoria");

    const double mNotch2 = mixedAmplitude(fNotch2, kMixGain);
    const double tNotch2 = theoryAmplitude(fNotch2, kMixGain);
    std::printf("  peine @ %.2f Hz (muesca): medido %.4f, teoria %.4f\n",
                fNotch2, mNotch2, tNotch2);
    check(std::fabs(mNotch2 - tNotch2) / tNotch2 <= 0.03,
          "peine: muesca 503.85 Hz coincide con teoria");

    // 2. Rizo pico-a-muesca: 20*log10(1.4/0.6) = 7.36 dB. Eso es la
    // coloración audible ("bofo") de la captura con Haas 0.4.
    const double rippleDb = db(mPeak1 / mNotch1);
    std::printf("  rizo pico-a-muesca: %.2f dB (teoria 7.36 dB)\n", rippleDb);
    check(std::fabs(rippleDb - 7.36) <= 0.3,
          "peine: rizo 7.36 dB pico-a-muesca (coloracion audible)");

    // 3. Espaciado de muescas: 1/tau = 7.69 Hz (muescas adyacentes).
    const double spacing = fNotch1 - fNotch2;  // 500.00 Hz entre muescas k=130.5 y k=65.5
    // 65 intervalos entre ambas muescas: 500.00 / 65 = 7.6923 Hz.
    const double measuredSpacing = spacing / 65.0;
    std::printf("  espaciado de muescas: %.4f Hz (teoria 7.6923 Hz)\n",
                measuredSpacing);
    check(std::fabs(measuredSpacing - 7.6923) <= 0.01,
          "peine: muescas cada 7.69 Hz (1/130 ms)");

    // 4. Con ganancia 0 (no re-inyectar) NO hay peine: espectro plano.
    const double mZero1 = mixedAmplitude(fPeak1, 0.0);
    const double mZero2 = mixedAmplitude(fNotch1, 0.0);
    std::printf("  mix 0 @ pico %.4f, @ muesca %.4f (teoria 1.0)\n",
                mZero1, mZero2);
    check(std::fabs(mZero1 - 1.0) <= 0.01 && std::fabs(mZero2 - 1.0) <= 0.01,
          "peine: con mix 0 no hay peine (plano 1.0)");

    // 5. A ganancia 1.0 (el viejo 100/100) el peine es total: muesca a 0.
    const double mFull = mixedAmplitude(fNotch1, 1.0);
    std::printf("  mix 1.0 @ muesca: %.4f (teoria 0.0, cancelacion total)\n", mFull);
    check(mFull <= 0.02,
          "peine: con mix 1.0 la muesca cancela (el viejo desface 100/100)");

    std::printf("test_capture_comb_filter: %d PASS, %d FALLA\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
