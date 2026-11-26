// Calibración P8 medida (P3, 3.4) — REFINAMIENTO TOTAL MAGISTRAL.
// ================================================================
// VEREDICTO MEDIDO (2026-11-26, g++-14, cadena real, 48 kHz):
//
// La brecha nominal-real ~18-20 dB NO es un error de coeficientes
// que una calibración estática pueda cerrar. Es ESTRUCTURAL y
// dependiente de señal, por tres mecanismos medidos:
//
//   (1) LOS OPERADORES SOLOS SON CASI NEUTROS. Standalone (trim 0,
//       tono 1 kHz -3 dBFS): ORBE 0.000, HOLOS -0.001, SALA -0.13/+0.04,
//       VOLTERRA 0.000, ARMONIA 0.000, NEURO 0.000 dB. Solo PULSO
//       cuesta algo (-2.59 dB). Suma standalone ~-2.7 dB. Pero la
//       cadena ALL7 sin trim pierde -19.56 dB. Los ~-17 dB restantes
//       son INTERACCION (perdida del enganche de cadena APEX/T-Omega
//       + absorcion del trim por PULSO), no ganancia de operador.
//       El modelo lineal de P8 (suma de ganancias por operador) es
//       estructuralmente incorrecto; los coeficientes kNominalGainDb
//       NO se pueden "corregir" operador por operador sin romper el
//       punto de calibracion del tono.
//
//   (2) EL COEFICIENTE NEURO ES OBSOLETO. kNominalGainDb[NEURO] =
//       -15.570 dB, pero NEURO standalone mide 0.000 dB en todos los
//       estimulos y niveles, y su marginal LOO es solo -0.9 dB.
//       Sin embargo es LOAD-BEARING: la suma de coeficientes debe
//       seguir siendo -19.482 dB (el trim = +19.482 dB) o el tono
//       1 kHz se rompe (-14.7 dB). La descomposicion es arbitraria
//       mientras la suma sea correcta PARA EL TONO; cambiar NEURO
//       sin re-derivar toda la descomposicion rompe D10. Por eso
//       kNominalGainDb NO se toca en 3.4.
//
//   (3) LA GANANCIA POR BANDA ES DEPENDIENTE DE SENAL. La misma
//       banda (1 kHz) necesita +0.08 dB para un tono coherente y
//       +6.58 dB para ruido incorrelado. Ningun trim estatico
//       (escalar ni por banda) puede satisfacer ambos. La
//       calibracion por banda estatica esta REFUTADA por medicion.
//
// TABLA MEDIDA (ALL7, trim fijo +19.482 dB, 2o segundo):
//   tono 1 kHz -3 dBFS:  L -0.077 R +0.076 dB   (punto D10, PASA)
//   tono 100 Hz -3 dBFS: L +2.562 R +2.648 dB   (residual +2.6 dB)
//   ruido -3 dBFS:       L -2.549 R -2.524 dB   (residual -2.5 dB)
//   tono 1 kHz -20 dBFS: L +13.865 R +14.020 dB (oscilacion de nivel!)
//   ruido, perfil por banda L: +2.09 +1.18 -1.33 -3.85 -6.94 -8.85
//     -8.36 -7.23 -6.58 -6.26 -3.16 +0.72 +2.12 -0.03 +2.17 +4.22
//     +7.30 +4.55 -1.71 -13.93 -11.09 -10.32 -10.59 -9.54 dB
//     (dispersion ~21 dB; peor banda b19 -13.93 dB)
// LOO (tono 1 kHz -3 dBFS, trim fijo; ARMONIA incluye el trim porque
// la etapa tonal se bypassa entera): ORBE -1.096/+1.539, HOLOS
// -0.031/-0.023, SALA -0.109/+0.045, VOLTERRA 0, PULSO -2.801/-2.787,
// NEURO -0.976/-0.867 dB. SOLO PULSO: -2.587 dB a -3 dBFS, -0.008 dB
// a -20 dBFS (absorcion dependiente de nivel: el trim empuja la
// senal al compresor de PULSO, que lo absorbe segun el nivel).
//
// QUE FALTA (fuera de 3.4): cerrar la brecha exige (a) encontrar y
// reparar la perdida de ~-17 dB del enganche de cadena (APEX/T-Omega),
// (b) compensacion DINAMICA (la correccion de energia D10, hoy
// deshabilitada por romper P0/pisos), o (c) mover el trim despues
// de la dinamica (rompe el techo: el trim +19.5 dB tras el limiter
// clippearia). Nada de eso es calibracion estatica.
// ================================================================

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include "fused/experience_compiler/experience_compiler.hpp"
#include "fused/fused_chain.hpp"
#include "fused/fused_controls.hpp"

using namespace ivanna::fused;

namespace {

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;
constexpr int kDur = 96000;  // 2 s
constexpr int kSec = 48000;  // medir el 2o segundo
constexpr int kBands = 24;

// El trim (+19.482 dB = -suma de kNominalGainDb) lo pone el compilador
// al aplicar (applyCompiledExperience lo recomputa sobre los 7 aplicados).

double db10(double x) { return 10.0 * std::log10(std::max(x, 1e-300)); }

FusedControls all7Controls() {
    // Via del compilador, IDENTICA a la sonda probe_p8_cal y al test D10:
    // objetivo 1.0 en las 7 componentes + applyCompiledExperience.
    // NO construir a mano con setNeutral(): este pone hrtfWet=0.0
    // (default fresco 1.0) y brain.mode=1 MANUAL (clampa adaptation a 0,
    // NEURO apagado) — los controles compilados difieren ~+1.9 dB en
    // tono y ~+4.5 dB en ruido de los manuales.
    FusedChain tmp;
    tmp.prepare(kFs);
    tmp.setExperienceCompilerEnabled(true);
    experience_compiler::ExperienceVector target;
    for (std::size_t i = 0; i < experience_compiler::kNumComponents; ++i)
        target.v[i] = 1.0;
    const auto compiled = tmp.compileExperience(
        target, experience_compiler::ReachableRegion{});
    tmp.applyCompiledExperience(compiled);
    return tmp.controls();
}

void runChain(const FusedControls& c, const std::vector<double>& inL,
              const std::vector<double>& inR, std::vector<double>& outL,
              std::vector<double>& outR) {
    // Orden identico a la sonda probe_p8_cal (y al test D10):
    // prepare -> setControls -> setVolumetricEnabled. Invertirlo
    // cambia la ganancia medida (~+4.6 dB en ALL7): el enganche
    // volumetrico re-aplica los controles al activarse.
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(c);
    chain.setVolumetricEnabled(true);
    outL = inL;
    outR = inR;
    chain.process(outL.data(), outR.data(), (int)inL.size());
}

double rmsDb(const std::vector<double>& v, int from, int to) {
    double s = 0.0;
    for (int i = from; i < to; ++i) s += v[i] * v[i];
    return db10(s / (to - from));
}

double goertzelDb(const std::vector<double>& v, int from, int to, double f0) {
    const int n = to - from;
    const double w = 2.0 * kPi * f0 / kFs;
    const double cw = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < to; ++i) {
        const double s0 = v[i] + cw * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * std::cos(w);
    const double im = s2 * std::sin(w);
    const double p = (re * re + im * im) / ((double)n * (double)n / 4.0);
    return db10(p);
}

std::vector<double> makeTone(double freq, double amp) {
    std::vector<double> v(kDur);
    for (int i = 0; i < kDur; ++i)
        v[i] = amp * std::sin(2.0 * kPi * freq * i / kFs);
    return v;
}

std::vector<double> makeNoise(double rms) {
    // Como la sonda probe_p8_cal: uniforme [-0.5, 0.5], semilla 61,
    // dual-mono, escalado a RMS 0.5 (-6.02 dBFS).
    std::vector<double> v(kDur);
    std::mt19937 rng(61);
    std::uniform_real_distribution<double> d(-0.5, 0.5);
    for (int i = 0; i < kDur; ++i) v[i] = d(rng);
    double p = 0.0;
    for (int i = 0; i < kDur; ++i) p += v[i] * v[i];
    const double sc = rms / std::sqrt(p / kDur);
    for (int i = 0; i < kDur; ++i) v[i] *= sc;
    return v;
}

// Potencia por banda ERB (Welch, FFT 16384 Hann, 4 ventanas) en el 2o s.
void bandPower(const std::vector<double>& v, double* out) {
    constexpr int N = 16384;
    constexpr int kWin = 4;
    double acc[kBands];
    std::memset(acc, 0, sizeof(acc));
    std::vector<double> hann(N);
    double hannSum = 0.0;
    for (int n = 0; n < N; ++n) {
        hann[n] = 0.5 * (1.0 - std::cos(2.0 * kPi * n / (N - 1)));
        hannSum += hann[n] * hann[n];
    }
    auto erbRate = [](double f) { return 21.4 * std::log10(1.0 + 0.00437 * f); };
    const double e0 = erbRate(60.0), e1 = erbRate(16000.0);
    // FIX-INTEGRACIÓN (2026-11-26): el hop original (kSec/kWin = 12000)
    // hacía que la última ventana (off 84000 + N 16384 = 100384) leyera
    // fuera del vector (kDur 96000) — segfault al correr el test solo
    // (en la suite completa el heap tenía holgura y no se veía). El hop
    // se calcula para que las 4 ventanas quepan en el 2º segundo.
    const int hop = (kDur - kSec - N) / (kWin - 1);  // 10538
    for (int w = 0; w < kWin; ++w) {
        const int off = kSec + w * hop;
        std::vector<double> re(N), im(N);
        for (int n = 0; n < N; ++n) {
            re[n] = v[off + n] * hann[n];
            im[n] = 0.0;
        }
        for (int i = 1, j = 0; i < N; ++i) {
            int bit = N >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(re[i], re[j]), std::swap(im[i], im[j]);
        }
        for (int len = 2; len <= N; len <<= 1) {
            const double ang = -2.0 * kPi / len;
            const double wr = std::cos(ang), wi = std::sin(ang);
            for (int i = 0; i < N; i += len) {
                double cwr = 1.0, cwi = 0.0;
                for (int j2 = 0; j2 < len / 2; ++j2) {
                    const double ur = re[i + j2], ui = im[i + j2];
                    const double vr = re[i + j2 + len / 2] * cwr -
                                      im[i + j2 + len / 2] * cwi;
                    const double vi = re[i + j2 + len / 2] * cwi +
                                      im[i + j2 + len / 2] * cwr;
                    re[i + j2] = ur + vr;
                    im[i + j2] = ui + vi;
                    re[i + j2 + len / 2] = ur - vr;
                    im[i + j2 + len / 2] = ui - vi;
                    const double nwr = cwr * wr - cwi * wi;
                    cwi = cwr * wi + cwi * wr;
                    cwr = nwr;
                }
            }
        }
        const double scale = 2.0 / (hannSum * (double)kWin);
        for (int k = 0; k <= N / 2; ++k) {
            const double f = k * kFs / N;
            if (f < 60.0 || f > 16000.0) continue;
            const int b = std::min(kBands - 1, (int)((erbRate(f) - e0) /
                                                    (e1 - e0) * kBands));
            acc[b] += (re[k] * re[k] + im[k] * im[k]) * scale;
        }
    }
    for (int b = 0; b < kBands; ++b) out[b] = acc[b];
}

}  // namespace

TEST(P8Calibration, Tone1kNetIsZero) {
    // Punto de calibracion D10: debe pasar (cota 0.10 dB, como D10).
    const auto in = makeTone(1000.0, 0.7071);
    std::vector<double> outL, outR;
    runChain(all7Controls(), in, in, outL, outR);
    const double inDb = rmsDb(in, kSec, kDur);
    const double nL = rmsDb(outL, kSec, kDur) - inDb;
    const double nR = rmsDb(outR, kSec, kDur) - inDb;
    const double gL = goertzelDb(outL, kSec, kDur, 1000.0) -
                      goertzelDb(in, kSec, kDur, 1000.0);
    const double gR = goertzelDb(outR, kSec, kDur, 1000.0) -
                      goertzelDb(in, kSec, kDur, 1000.0);
    EXPECT_LE(std::fabs(nL), 0.10);
    EXPECT_LE(std::fabs(nR), 0.10);
    EXPECT_LE(std::fabs(gL), 0.10);
    EXPECT_LE(std::fabs(gR), 0.10);
}

TEST(P8Calibration, Tone100HzResidual) {
    // Residual medido +2.56/+2.65 dB (2026-11-26). Cota = lo medido + margen,
    // NO un objetivo: documenta el hueco de baja frecuencia del trim escalar.
    const auto in = makeTone(100.0, 0.7071);
    std::vector<double> outL, outR;
    runChain(all7Controls(), in, in, outL, outR);
    const double inDb = rmsDb(in, kSec, kDur);
    const double nL = rmsDb(outL, kSec, kDur) - inDb;
    const double nR = rmsDb(outR, kSec, kDur) - inDb;
    EXPECT_GE(nL, 0.0);
    EXPECT_LE(nL, 3.0);
    EXPECT_GE(nR, 0.0);
    EXPECT_LE(nR, 3.0);
}

TEST(P8Calibration, NoiseResidualAndBandProfile) {
    // Ruido -3 dBFS: neto medido -2.55/-2.52 dB; perfil por banda L
    // medido de +7.30 a -13.93 dB (dispersion ~21 dB). Las cotas fijan
    // lo medido: si el perfil mejora, la cota se estrecha; si empeora,
    // el test lo caza. El trim escalar no puede cerrar esto (ver cabecera).
    const auto in = makeNoise(0.5);
    std::vector<double> outL, outR;
    runChain(all7Controls(), in, in, outL, outR);
    const double inDb = rmsDb(in, kSec, kDur);
    const double nL = rmsDb(outL, kSec, kDur) - inDb;
    const double nR = rmsDb(outR, kSec, kDur) - inDb;
    EXPECT_GE(nL, -3.0);
    EXPECT_LE(nL, 0.0);
    EXPECT_GE(nR, -3.0);
    EXPECT_LE(nR, 0.0);
    double pAll[kBands], pIn[kBands];
    bandPower(outL, pAll);
    bandPower(in, pIn);
    double worstLo = 0.0, worstHi = 0.0;
    for (int b = 0; b < kBands; ++b) {
        const double d = db10(pAll[b] / pIn[b]);
        worstLo = std::min(worstLo, d);
        worstHi = std::max(worstHi, d);
    }
    EXPECT_GE(worstLo, -14.5);  // medido -13.93 (b19)
    EXPECT_LE(worstHi, 8.0);    // medido +7.30 (b16)
}

TEST(P8Calibration, LevelDependence) {
    // Oscilacion de nivel medida: tono 1 kHz -20 dBFS da +13.87/+14.02 dB
    // (vs -0.08 dB a -3 dBFS). Causa: el trim +19.482 dB empuja la senal
    // al compresor de PULSO, que lo absorbe segun el nivel. La cota
    // documenta el defecto; cerrarlo exige compensacion dinamica o
    // reparar la perdida de enganche (ver cabecera), no un trim nuevo.
    const auto in = makeTone(1000.0, 0.1414);  // -20 dBFS RMS
    std::vector<double> outL, outR;
    runChain(all7Controls(), in, in, outL, outR);
    const double inDb = rmsDb(in, kSec, kDur);
    const double nL = rmsDb(outL, kSec, kDur) - inDb;
    const double nR = rmsDb(outR, kSec, kDur) - inDb;
    EXPECT_GE(nL, 13.0);
    EXPECT_LE(nL, 15.0);
    EXPECT_GE(nR, 13.0);
    EXPECT_LE(nR, 15.0);
}

TEST(P8Calibration, NeuroStandaloneIsZero) {
    // kNominalGainDb[NEURO] = -15.570 dB, pero NEURO solo (trim 0) mide
    // 0.000 dB en todo estimulo y nivel. El coeficiente es obsoleto
    // (de una configuracion anterior del cerebro); se deja intacto
    // porque la SUMA es load-bearing para el tono (ver cabecera).
    FusedControls c;
    c.setNeutral();
    c.tonal.trimDb = 0.0f;
    c.brain.adaptation = 1.0f;
    const auto in = makeTone(1000.0, 0.7071);
    std::vector<double> outL, outR;
    runChain(c, in, in, outL, outR);
    const double inDb = rmsDb(in, kSec, kDur);
    EXPECT_LE(std::fabs(rmsDb(outL, kSec, kDur) - inDb), 0.05);
    EXPECT_LE(std::fabs(rmsDb(outR, kSec, kDur) - inDb), 0.05);
}

TEST(P8Calibration, PulsoAbsorptionIsLevelDependent) {
    // SOLO PULSO (trim 0), tono 1 kHz: -2.587 dB a -3 dBFS, -0.008 dB
    // a -20 dBFS. Es el mecanismo de la oscilacion de nivel de 3.4:
    // el compresor absorbe segun el nivel que ve.
    const auto inHot = makeTone(1000.0, 0.7071);
    const auto inQuiet = makeTone(1000.0, 0.1414);
    FusedControls c;
    c.setNeutral();
    c.tonal.trimDb = 0.0f;
    c.dynamics.punch = 1.0f;
    std::vector<double> outL, outR;
    runChain(c, inHot, inHot, outL, outR);
    const double nHot = rmsDb(outL, kSec, kDur) - rmsDb(inHot, kSec, kDur);
    runChain(c, inQuiet, inQuiet, outL, outR);
    const double nQuiet =
        rmsDb(outL, kSec, kDur) - rmsDb(inQuiet, kSec, kDur);
    EXPECT_LE(nHot, -2.0);
    EXPECT_GE(nHot, -3.0);
    EXPECT_LE(std::fabs(nQuiet), 0.05);
}

TEST(P8Calibration, OtherOperatorsStandaloneAreNeutral) {
    // SOLO (trim 0), tono 1 kHz -3 dBFS: ORBE, HOLOS, VOLTERRA y
    // ARMONIA son neutros en ganancia (0.000 dB medido). La perdida
    // de la cadena NO vive en los operadores.
    const auto in = makeTone(1000.0, 0.7071);
    const double inDb = rmsDb(in, kSec, kDur);
    std::vector<double> outL, outR;
    FusedControls c;
    c.setNeutral();
    c.tonal.trimDb = 0.0f;
    c.spatial.amount = 1.0f;
    runChain(c, in, in, outL, outR);
    EXPECT_LE(std::fabs(rmsDb(outL, kSec, kDur) - inDb), 0.05);
    EXPECT_LE(std::fabs(rmsDb(outR, kSec, kDur) - inDb), 0.05);
    c.setNeutral();
    c.tonal.trimDb = 0.0f;
    c.field.amount = 1.0f;
    runChain(c, in, in, outL, outR);
    EXPECT_LE(std::fabs(rmsDb(outL, kSec, kDur) - inDb), 0.05);
    EXPECT_LE(std::fabs(rmsDb(outR, kSec, kDur) - inDb), 0.05);
    c.setNeutral();
    c.tonal.trimDb = 0.0f;
    c.transducer.correction = 1.0f;
    runChain(c, in, in, outL, outR);
    EXPECT_LE(std::fabs(rmsDb(outL, kSec, kDur) - inDb), 0.05);
    EXPECT_LE(std::fabs(rmsDb(outR, kSec, kDur) - inDb), 0.05);
    c.setNeutral();
    c.tonal.trimDb = 0.0f;
    c.tonal.amount = 1.0f;
    runChain(c, in, in, outL, outR);
    EXPECT_LE(std::fabs(rmsDb(outL, kSec, kDur) - inDb), 0.05);
    EXPECT_LE(std::fabs(rmsDb(outR, kSec, kDur) - inDb), 0.05);
}
