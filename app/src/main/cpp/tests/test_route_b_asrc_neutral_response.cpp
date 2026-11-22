// test_route_b_asrc_neutral_response.cpp — Exoneración del ASRC (0023).
//
// PREGUNTA: ¿el ASRC del módulo (el resampler que corre en omega_process
// ANTES de toda la cadena, incluso en passthrough) es el bofo de Ruta B?
// RESPUESTA (medida en host, no afirmada): NO.
//   * A ratio 1.0 el Core hace memcpy bit-exacto (processS16, camino
//     rápido): ni un LSB de diferencia con lo que entrega AudioFlinger.
//   * A ratio != 1.0 (la deriva real medida es +8.5 ppm) el FIR polifásico
//     (319 taps, corte 0.45 -> 21.6 kHz a 48 kHz) es plano hasta 20 kHz:
//     este test mide la respuesta en magnitud a ratio 1.0000085 con senos
//     puros y exige +-0.5 dB hasta 20 kHz, y atenuación real por encima
//     del corte (sanidad: el FIR sí es un paso-bajo, no un test degenerado).
// Si este test pasa, el ASRC queda descartado como causa del bofo y la
// causa queda en el snapshot neutro no-neutro (HRTF wet 1.0 + widener
// 2.14x), que es lo que arregla 0023.
//
// Método: seno puro de amplitud 0.5 a la frecuencia de prueba, 1 s de
// calentamiento (el FIR tiene 319 taps + priming de 1024 frames) + 1 s de
// medida; RMS de salida / RMS de entrada = respuesta en magnitud. Se usa
// processF32 (la misma matemática que processS16, sin cuantización de
// 16 bits en la medida). El desplazamiento de frecuencia por el ratio
// (8.5 ppm) es despreciable para el RMS.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../../../../../magisk_module/engine/asrc/ivanna_asrc_core.h"

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
constexpr double kDriftRatio = 1.0000085;  // +8.5 ppm, deriva medida en vivo

// Respuesta en magnitud del Core a la frecuencia f (Hz) con el ratio dado:
// RMS(salida)/RMS(entrada) en régimen permanente.
double magnitudeAt(double freqHz, double ratio) {
    ivanna::asrc::Core core;
    if (!core.init()) return -1.0;
    core.setRatio(ratio);
    constexpr int kBlock = 512;
    constexpr int kWarm = kRate;      // 1 s de calentamiento
    constexpr int kMeas = kRate;      // 1 s de medida
    std::vector<float> in(kBlock * 2), out(kBlock * 2);
    double phase = 0.0;
    const double step = 2.0 * kPi * freqHz / kRate;
    double sumIn = 0.0, sumOut = 0.0;
    long nMeas = 0;
    for (int b = 0; b < (kWarm + kMeas) / kBlock; ++b) {
        for (int i = 0; i < kBlock; ++i) {
            const float s = static_cast<float>(0.5 * std::sin(phase));
            phase += step;
            in[2 * i] = s;
            in[2 * i + 1] = s;
        }
        const auto st = core.processF32(in.data(), kBlock, out.data(), kBlock, 2);
        if (b * kBlock >= kWarm) {
            for (size_t i = 0; i < st.out_frames * 2; ++i) {
                sumIn += static_cast<double>(in[i]) * in[i];
                sumOut += static_cast<double>(out[i]) * out[i];
            }
            nMeas += static_cast<long>(st.out_frames * 2);
        }
    }
    if (nMeas == 0 || sumIn <= 0.0) return -1.0;
    return std::sqrt(sumOut / sumIn);
}

double db(double mag) { return 20.0 * std::log10(mag); }

}  // namespace

int main() {
    // 1. Ratio 1.0: bit-exacto (memcpy). Ni un LSB de diferencia.
    {
        ivanna::asrc::Core core;
        check(core.init(), "asrc: init OK");
        core.setRatio(1.0);
        constexpr int kN = 4096;
        std::vector<int16_t> in(kN * 2), out(kN * 2, 0);
        for (int i = 0; i < kN * 2; ++i)
            in[i] = static_cast<int16_t>((i * 7919) % 32768 - 16384);
        const auto st = core.processS16(in.data(), kN, out.data(), kN, 2);
        bool exact = st.out_frames == static_cast<size_t>(kN) && st.passthrough;
        for (int i = 0; exact && i < kN * 2; ++i) exact = (out[i] == in[i]);
        check(exact, "asrc: ratio 1.0 bit-exacto (memcpy, 0 LSB)");
    }

    // 2. Ratio de deriva real (+8.5 ppm): plano hasta 20 kHz.
    const double freqs[] = {100.0, 1000.0, 6000.0, 12000.0, 16000.0, 19000.0, 20000.0};
    for (double f : freqs) {
        const double m = magnitudeAt(f, kDriftRatio);
        std::printf("  asrc @ %.0f Hz, ratio 1.0000085: %.4f (%+.2f dB)\n",
                    f, m, db(m));
        char name[96];
        std::snprintf(name, sizeof(name),
                      "asrc: plano +-0.5 dB a %.0f Hz (deriva +8.5 ppm)", f);
        check(m > 0.0 && std::fabs(db(m)) <= 0.5, name);
    }

    // 3. Sanidad: por encima del corte (23 kHz) el FIR sí atenúa.
    // Si no atenuara, el test de planitud sería degenerado.
    {
        const double m = magnitudeAt(23000.0, kDriftRatio);
        std::printf("  asrc @ 23000 Hz, ratio 1.0000085: %.4f (%+.2f dB)\n",
                    m, db(m));
        check(m > 0.0 && m < 0.9, "asrc: atenua por encima del corte (23 kHz < 0.9)");
    }

    std::printf("test_route_b_asrc_neutral_response: %d PASS, %d FALLA\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
