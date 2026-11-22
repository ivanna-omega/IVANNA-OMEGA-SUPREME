// test_fused_chain.cpp — FUSIÓN MAGISTRAL: la cadena única de los 7 operadores.
// Verifica que el FusedChain (núcleo compartido) procesa en el orden canónico,
// es bit-transparente en neutro (double y float), no produce NaN/Inf, la
// modulación del guardián AntiDolby llega a los operadores, el enrutado del
// upmix funciona, y el THD+N en neutro cumple ≤ −200 dB (o bitExact).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
#include "../fused/unified_field_kernel.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {
constexpr double kFs = 48000.0;
constexpr double kFreq = 1000.0;
constexpr double kAmp = 0.7071;  // −3 dBFS

std::vector<double> tone(std::size_t n, double amp = kAmp, double freq = kFreq) {
    std::vector<double> v(n);
    fillToneDouble(v.data(), n, kFs, freq, amp);
    return v;
}

FusedControls engagedControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.field.amount = 0.5f;
    c.field.phiAmount = 0.5f;
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 1.0f;
    c.tonal.curve = 1;
    c.tonal.lowDb = 2.0f;
    c.dynamics.compRatio = 2.0f;
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.safety = 1.0f;
    c.brain.mode = 0;  // AUTO
    c.brain.adaptation = 1.0f;
    c.clamp();
    return c;
}
}  // namespace

TEST(FusedChain, NeutralIsBitExactDouble) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setNeutral();
    const std::size_t n = 48000;
    std::vector<double> l = tone(n), r = tone(n);
    const std::vector<double> lIn = l, rIn = r;
    chain.process(l.data(), r.data(), static_cast<int>(n));
    const NullResult nl = nullTest(lIn.data(), l.data(), n);
    const NullResult nr = nullTest(rIn.data(), r.data(), n);
    EXPECT_TRUE(nl.bitExact) << "residual L " << nl.residualDb << " dB";
    EXPECT_TRUE(nr.bitExact) << "residual R " << nr.residualDb << " dB";
}

TEST(FusedChain, NeutralIsBitExactFloat) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setNeutral();
    const std::size_t n = 4800;
    std::vector<float> l(n), r(n);
    for (std::size_t i = 0; i < n; ++i) {
        const float v = static_cast<float>(kAmp * std::sin(2.0 * 3.141592653589793 * kFreq * static_cast<double>(i) / kFs));
        l[i] = v; r[i] = v;
    }
    const std::vector<float> lIn = l, rIn = r;
    chain.processFloat(l.data(), r.data(), static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i) {
        EXPECT_FLOAT_EQ(l[i], lIn[i]);
        EXPECT_FLOAT_EQ(r[i], rIn[i]);
        if (l[i] != lIn[i] || r[i] != rIn[i]) break;
    }
}

TEST(FusedChain, NeutralGainUnity) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setNeutral();
    const std::size_t n = 4800;
    std::vector<double> l = tone(n), r = tone(n);
    const double inDb = rmsDb(l.data(), n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    const double outDb = rmsDb(l.data(), n);
    EXPECT_NEAR(outDb, inDb, 1e-9) << "in " << inDb << " out " << outDb;
}

TEST(FusedChain, AllOperatorsExecute) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(engagedControls());
    const std::size_t n = 4800;
    std::vector<double> l = tone(n), r = tone(n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    const FusedChainStats st = chain.stats();
    EXPECT_GT(st.totalBlocks, 0u);
    for (int i = 0; i < FusedChain::kNumOperators; ++i) {
        EXPECT_GT(st.blocksProcessed[i], 0u) << "operador " << FusedChain::operatorName(i) << " no ejecutó";
    }
    // Con controles activos, cada operador debe haber estado activo al menos una vez.
    for (int i = 0; i < FusedChain::kNumOperators; ++i) {
        EXPECT_GT(st.activeBlocks[i], 0u) << "operador " << FusedChain::operatorName(i) << " nunca activo";
    }
}

TEST(FusedChain, NoNaNOrInf) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(engagedControls());
    const std::size_t n = 4800;
    // Silencio, DC, seno fuerte y ruido determinista.
    std::vector<double> silence(n, 0.0), dc(n, 0.5), loud = tone(n, 0.99), noise(n);
    std::uint64_t seed = 0x12345678u;
    for (std::size_t i = 0; i < n; ++i) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        noise[i] = static_cast<double>((seed >> 11) & 0xFFFFF) / 524288.0 - 1.0;
    }
    for (auto* v : {&silence, &dc, &loud, &noise}) {
        std::vector<double> l = *v, r = *v;
        chain.process(l.data(), r.data(), static_cast<int>(n));
        EXPECT_TRUE(isFiniteBuffer(l.data(), n));
        EXPECT_TRUE(isFiniteBuffer(r.data(), n));
        EXPECT_LE(peakAbs(l.data(), n), 4.0);
        EXPECT_LE(peakAbs(r.data(), n), 4.0);
    }
}

TEST(FusedChain, GuardianModulatesOperators) {
    // En AUTO con música, el guardián AntiDolby converge a widener ~1.35 y
    // spread ~1.25; la cadena debe aplicar esa modulación al ancho de PULSO
    // y de ORBE·Ω. Verificamos que la telemetría del guardián se mueve y
    // que el ancho efectivo difiere del ancho del usuario.
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c;
    c.setNeutral();
    c.brain.mode = 0;  // AUTO
    c.brain.adaptation = 1.0f;
    c.dynamics.width = 1.0f;
    c.spatial.width = 1.0f;
    // Activar un operador para que el audio fluya (el guardián se alimenta
    // del análisis del cerebro, que corre siempre).
    c.field.amount = 0.5f;
    c.clamp();
    chain.setControls(c);
    const std::size_t n = 48000;
    std::vector<double> l = tone(n), r = tone(n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    const FusedTelemetry tel = chain.telemetry();
    // El guardián debe haber convergido (intensidad > 0 en AUTO con música).
    EXPECT_GT(tel.guardianIntensity, 0.0f) << "guardián no convergió en AUTO";
    EXPECT_GT(tel.guardianWidener, 1.0f) << "widener del guardián no subió con música";
    // El ancho efectivo de ORBE·Ω (telemetría) debe reflejar el spread.
    EXPECT_GT(tel.spatialWidth, 1.0f) << "el spread del guardián no llegó a ORBE·Ω";
}

TEST(FusedChain, UpmixRoutesToSpatial) {
    // PULSO.upmixImmersivity debe encender el upmixer de ORBE·Ω.
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c;
    c.setNeutral();
    c.dynamics.upmixImmersivity = 1.0f;
    c.clamp();
    chain.setControls(c);
    const std::size_t n = 4800;
    std::vector<double> l = tone(n), r = tone(n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    EXPECT_TRUE(chain.spatial().controls().upmixEnabled)
        << "el upmix de PULSO no llegó a ORBE·Ω";
    EXPECT_GT(chain.spatial().controls().upmixImmersivity, 0.0f);
}

TEST(FusedChain, ThdAtNeutral) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setNeutral();
    const std::size_t n = 4800;  // 100 ciclos exactos de 1 kHz
    std::vector<double> l = tone(n), r = tone(n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    const ThdDoubleResult thd = measureThdDouble(l.data(), n, kFs, kFreq);
    const NullResult nl = nullTest(tone(n).data(), l.data(), n);
    EXPECT_TRUE(thd.valid);
    // Meta −200 dB: se cumple por bit-exactitud (residuo 0) o por el medidor.
    EXPECT_TRUE(nl.bitExact || thd.thdPlusNoiseDb <= -200.0)
        << "THD+N " << thd.thdPlusNoiseDb << " dB, bitExact=" << nl.bitExact;
    if (thd.valid) {
        // Reportar el número real en el log del test.
        std::printf("[FusedChain] THD+N neutro: %.2f dB (valid=%d, bitExact=%d)\n",
                    thd.thdPlusNoiseDb, static_cast<int>(thd.valid),
                    static_cast<int>(nl.bitExact));
    }
}

TEST(FusedChain, EngagedSmoke) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(engagedControls());
    const std::size_t n = 4800;
    std::vector<double> l = tone(n), r = tone(n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    EXPECT_TRUE(isFiniteBuffer(l.data(), n));
    EXPECT_TRUE(isFiniteBuffer(r.data(), n));
    EXPECT_LE(peakAbs(l.data(), n), 4.0);
    EXPECT_LE(peakAbs(r.data(), n), 4.0);
}

TEST(FusedChain, ThdAtEngaged) {
    // 470 (crítica externa): el −303.89 dB es el NEUTRO (cadena
    // transparente, sin matemática); este test mide el THD+N con la cadena
    // ENGAGED (operadores activos, engagedControls()). El número incluye el
    // carácter intencional (EQ, espacial, Phi, compresor), no solo ruido
    // numérico: es la desviación real de un tono puro y se reporta tal
    // cual, sin venderlo como precisión numérica. Sirve como ancla de
    // regresión: si el procesado activo se degrada a ruido dominante, el
    // test lo caza. Se descartan las primeras 9600 muestras (transitorio
    // de arranque de los operadores) y se mide en régimen permanente.
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(engagedControls());
    const std::size_t n = 48000;
    const std::size_t skip = 9600;
    std::vector<double> l = tone(n), r = tone(n);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    EXPECT_TRUE(isFiniteBuffer(l.data(), n));
    EXPECT_TRUE(isFiniteBuffer(r.data(), n));
    const ThdDoubleResult thdL = measureThdDouble(l.data() + skip, n - skip, kFs, kFreq);
    const ThdDoubleResult thdR = measureThdDouble(r.data() + skip, n - skip, kFs, kFreq);
    EXPECT_TRUE(thdL.valid);
    EXPECT_TRUE(thdR.valid);
    std::printf("[FusedChain] THD+N ENGAGED (1 kHz, %zu muestras, skip %zu): L=%.2f dB R=%.2f dB\n",
                n - skip, skip, thdL.thdPlusNoiseDb, thdR.thdPlusNoiseDb);
    // Ancla de regresión (umbral con margen sobre el valor medido): la
    // cadena activa no debe degradarse a ruido dominante.
    EXPECT_LT(thdL.thdPlusNoiseDb, -3.0) << "THD+N activo L=" << thdL.thdPlusNoiseDb;
    EXPECT_LT(thdR.thdPlusNoiseDb, -3.0) << "THD+N activo R=" << thdR.thdPlusNoiseDb;
}


// ── Matriz volumétrica cableada (XTC Genezi) ──────────────────────────────
// Helpers para medir el crosstalk de punta a punta (cadena + planta acústica).
namespace {
// Amplitud compleja de un tono a `freq` en [start, start+n) (Goertzel).
// La escala es común a todos los canales, así que se cancela en el cociente.
std::complex<double> toneBinGoertzel(const double* x, std::size_t start, std::size_t n,
                                     double freq, double fs) {
    const double w = 2.0 * 3.14159265358979323846 * freq / fs;
    const double coeff = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double s0 = x[start + i] + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - std::cos(w) * s2;
    const double im = std::sin(w) * s2;
    return {re, im};
}

// Planta acústica A (2x2) a un offset dado (mismo cálculo que
// test_ivanna_apex_kernel.cpp: retardo + 1/d + esfera rígida).
void buildPlantVol(double freqHz, double offsetM,
                   const apex::VolumetricSpeakerSpec& base,
                   ivanna::physical::RigidSphereWorkspace& ws,
                   apex::Complex2x2& A) {
    const double a = base.headRadiusM;
    const double cSound = base.speedOfSoundMps;
    const double halfSpan = base.spanM * 0.5;
    const double zDist = base.distanceM;
    const double omega = 2.0 * apex::kPi * freqHz;
    const double dxL = -halfSpan - offsetM;
    const double dxR = +halfSpan - offsetM;
    const double dLL = std::hypot(dxL + a, zDist);
    const double dLR = std::hypot(dxL - a, zDist);
    const double dRL = std::hypot(dxR + a, zDist);
    const double dRR = std::hypot(dxR - a, zDist);
    const double rCL = std::hypot(dxL, zDist);
    const double rCR = std::hypot(dxR, zDist);
    const std::complex<double> sLL = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(dxL / rCL, -1.0, 1.0), a, cSound, ws));
    const std::complex<double> sLR = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(-dxL / rCL, -1.0, 1.0), a, cSound, ws));
    const std::complex<double> sRL = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(dxR / rCR, -1.0, 1.0), a, cSound, ws));
    const std::complex<double> sRR = std::conj(ivanna::physical::rigidSphereTransfer(
        freqHz, std::clamp(-dxR / rCR, -1.0, 1.0), a, cSound, ws));
    const double dRef = 0.5 * (dLL + dRR);
    auto path = [&](double d, const std::complex<double>& diff) {
        return (dRef / std::max(d, 0.1)) *
               std::exp(std::complex<double>(0.0, -omega * (d - dRef) / cSound)) * diff;
    };
    A.m00 = path(dLL, sLL);
    A.m01 = path(dRL, sRL);
    A.m10 = path(dLR, sLR);
    A.m11 = path(dRR, sRR);
}
}  // namespace

TEST(FusedChain, VolumetricLowersCrosstalk) {
    // La matriz volumétrica cableada (XTC Genezi, hw 0.02) baja el crosstalk
    // a < 0.3 (−10 dB) a 1 kHz y 3 kHz, medido de punta a punta (cadena +
    // planta acústica en offset 0). Control mínimo no-neutro (tonal +0.1 dB,
    // mono) solo para abrir la puerta isNeutral; a 1/3 kHz el shelf de
    // graves es ~0 dB, así que los operadores son transparentes y la medida
    // es la del volumétrico. Se descarta el transitorio de arranque del
    // WOLA (primeras 4096 muestras) y se mide en régimen permanente. El
    // retardo del WOLA (~512 muestras) solo cambia la fase, no la magnitud,
    // así que no afecta al cociente de crosstalk.
    const double freqs[2] = {1000.0, 3000.0};
    const std::size_t n = 9600;
    const std::size_t skip = 4096;
    const std::size_t meas = n - skip;
    for (double freq : freqs) {
        double xtOn = 0.0, xtOff = 0.0;
        for (bool volOn : {true, false}) {
            double meanXt = 0.0;
            apex::ApexWorkspace ws;
            apex::VolumetricSpeakerSpec sp;
            for (int side = 0; side < 2; ++side) {
                FusedChain chain;
                chain.prepare(kFs);
                chain.setNeutral();
                FusedControls c;
                c.setNeutral();
                c.tonal.lowDb = 0.1f;  // abre la puerta isNeutral (mono)
                // 470: la matriz es SOLO SPEAKERS; sin este modo la puerta
                // la dejaría apagada y el crosstalk no bajaría.
                c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
                c.clamp();
                chain.setControls(c);
                chain.setVolumetricEnabled(volOn);
                std::vector<double> l(n, 0.0), r(n, 0.0);
                std::vector<double>& active = (side == 0) ? l : r;
                fillToneDouble(active.data(), n, kFs, freq, kAmp);
                chain.process(l.data(), r.data(), static_cast<int>(n));
                const std::complex<double> oL = toneBinGoertzel(l.data(), skip, meas, freq, kFs);
                const std::complex<double> oR = toneBinGoertzel(r.data(), skip, meas, freq, kFs);
                apex::Complex2x2 A;
                buildPlantVol(freq, 0.0, sp, ws.sphereWs, A);
                const std::complex<double> earL = A.m00 * oL + A.m01 * oR;
                const std::complex<double> earR = A.m10 * oL + A.m11 * oR;
                double xt = 0.0;
                if (side == 0) {
                    xt = std::abs(earL) > 1e-12 ? std::abs(earR) / std::abs(earL) : 1e9;
                } else {
                    xt = std::abs(earR) > 1e-12 ? std::abs(earL) / std::abs(earR) : 1e9;
                }
                EXPECT_TRUE(std::isfinite(xt)) << "crosstalk no finito";
                meanXt += xt;
            }
            meanXt /= 2.0;
            if (volOn) xtOn = meanXt; else xtOff = meanXt;
            std::printf("[FusedChain] Volumetric crosstalk %.0f Hz vol=%d: %.3f\n",
                        freq, volOn ? 1 : 0, meanXt);
        }
        EXPECT_LT(xtOn, 0.30) << "crosstalk cableado a " << freq << " Hz = " << xtOn;
        EXPECT_LT(xtOn, xtOff) << "el volumétrico no bajó el crosstalk a " << freq
                               << " Hz (on=" << xtOn << " off=" << xtOff << ")";
    }
}

TEST(FusedChain, VolumetricOffInHeadphones) {
    // 470 (crítica externa verificada): la matriz volumétrica es XTC para
    // altavoces Genezi; en HEADPHONES no hay diafonía acústica que cancelar,
    // así que la señal NO debe pasar por la matriz aunque volumetricEnabled
    // esté en true. Se comprueba que en HEADPHONES la salida con vol=on es
    // bit-idéntica a vol=off (la matriz no se aplica), y que en SPEAKERS
    // vol=on SÍ cambia la salida respecto a vol=off (la matriz aplica y la
    // puerta no quedó siempre apagada). Control mínimo no-neutro
    // (tonal +0.1 dB, mono) para abrir isNeutral; a 1/3 kHz el shelf es
    // ~0 dB, así que los operadores son transparentes y la única diferencia
    // posible entre vol=on y vol=off es la matriz.
    const double freqs[2] = {1000.0, 3000.0};
    const std::size_t n = 9600;
    for (double freq : freqs) {
        for (auto mode : {ivanna::OmegaOutputMode::HEADPHONES,
                          ivanna::OmegaOutputMode::SPEAKERS}) {
            std::vector<double> lOn = tone(n, kAmp, freq), rOn = tone(n, kAmp, freq);
            std::vector<double> lOff = lOn, rOff = rOn;
            for (bool volOn : {true, false}) {
                std::vector<double>& l = volOn ? lOn : lOff;
                std::vector<double>& r = volOn ? rOn : rOff;
                FusedChain chain;
                chain.prepare(kFs);
                chain.setNeutral();
                FusedControls c;
                c.setNeutral();
                c.tonal.lowDb = 0.1f;
                c.spatial.mode = mode;
                c.clamp();
                chain.setControls(c);
                chain.setVolumetricEnabled(volOn);
                chain.process(l.data(), r.data(), static_cast<int>(n));
            }
            double maxDiff = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                maxDiff = std::max(maxDiff, std::abs(lOn[i] - lOff[i]));
                maxDiff = std::max(maxDiff, std::abs(rOn[i] - rOff[i]));
            }
            std::printf("[FusedChain] Volumetric mode=%d %.0f Hz maxDiff(volOn-volOff)=%.3g\n",
                        static_cast<int>(mode), freq, maxDiff);
            if (mode == ivanna::OmegaOutputMode::HEADPHONES) {
                EXPECT_EQ(maxDiff, 0.0)
                    << "en HEADPHONES la matriz no debe aplicarse ("
                    << freq << " Hz, maxDiff=" << maxDiff << ")";
            } else {
                EXPECT_GT(maxDiff, 1e-6)
                    << "en SPEAKERS la matriz sí debe aplicarse ("
                    << freq << " Hz, maxDiff=" << maxDiff << ")";
            }
        }
    }
}

// ── TΩ conectado a FusedChain (F1/472) ────────────────────────────────────
// La etapa espectral TΩ (Reloj 2 compile en double + Reloj 1 apply hop 128
// empaquetado L+jR) sustituye a la matriz volumétrica WOLA en SPEAKERS.
// Estos tests verifican la conexión de punta a punta: crosstalk real a
// través de la planta acústica, nivel absoluto contra la predicción del
// kernel (valida la ganancia COLA 0.5 y el desempaquetado), compilación
// Reloj 2 solo al cambiar controles, neutro bit-exacto en SPEAKERS, y el
// coste dsp_us en host por bloque de Ruta A (320 muestras).
TEST(FusedChain, TOmegaSpeakersCrosstalk) {
    // Misma metodología que VolumetricLowersCrosstalk (tono en un solo
    // canal, planta acústica, Goertzel en régimen permanente), pero la
    // matriz que actúa es la TΩ (volOn=true → TΩ; volOn=false → nada).
    // Además se compara contra la predicción DIRECTA del kernel
    // (planta A · W_TΩ): el crosstalk medido debe coincidir con lo
    // predicho, y el nivel en el oído deseado debe coincidir con
    // |C| · |tono| · g_serie, donde g_serie es la ganancia de la cadena
    // serie medida en la pasada sin matriz (misma cadena, mismos
    // controles, misma entrada). Eso valida el camino completo de la TΩ
    // (pack/unpack, M[k]·X[k], OLA hop 128, COLA 0.5) sin que la ganancia
    // propia de la cadena serie contamine la medida.
    const double freqs[2] = {1000.0, 3000.0};
    const std::size_t n = 9600;
    const std::size_t skip = 4096;
    const std::size_t meas = n - skip;
    for (double freq : freqs) {
        double xtOn = 0.0, xtOff = 0.0, xtPredMean = 0.0;
        double measEarOn[2] = {0.0, 0.0};
        double inToneMag[2] = {0.0, 0.0};
        double desiredC[2] = {0.0, 0.0};
        double serialGain[2] = {0.0, 0.0};
        for (bool volOn : {true, false}) {
            double meanXt = 0.0;
            apex::ApexWorkspace ws;
            apex::VolumetricSpeakerSpec sp;
            ivanna::physical::RigidSphereWorkspace sphereWsT;
            for (int side = 0; side < 2; ++side) {
                FusedChain chain;
                chain.prepare(kFs);
                chain.setNeutral();
                FusedControls c;
                c.setNeutral();
                c.tonal.lowDb = 0.1f;  // abre la puerta isNeutral (mono)
                // loudnessComp=false para aislar la TΩ (el psico es
                // ≈ identidad sobre tonos, pero no se mide aquí).
                c.tonal.loudnessComp = false;
                c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
                c.clamp();
                chain.setControls(c);
                chain.setVolumetricEnabled(volOn);
                // APEX apagado para aislar la TΩ; los ratios con APEX
                // encendido los cubre VolumetricLowersCrosstalk.
                chain.setApexEnabled(false);
                std::vector<double> l(n, 0.0), r(n, 0.0);
                std::vector<double>& active = (side == 0) ? l : r;
                fillToneDouble(active.data(), n, kFs, freq, kAmp);
                const std::vector<double> inL = l, inR = r;
                // Bloques de 320 muestras (BLOCK_FRAMES de Ruta A): con
                // una sola llamada de 9600, la FIFO WOLA deja 384 ceros
                // al final del primer chunk interno de 8192 (déficit de
                // priming: 61 frames × 128 = 7808 emitidos vs 8192
                // drenados) DENTRO de la ventana de medida — −0.63 dB
                // ficticios (384/5504 = 0.930). En streaming por bloques
                // no hay ceros tras el priming (la alternancia 2/3 frames
                // por bloque equilibra la FIFO) y la ventana [4096,9600)
                // es señal plena. Mismo artefacto en el WOLA volumétrico
                // (sus tests solo miden ratios, inmunes a él).
                for (std::size_t off = 0; off < n; off += 320) {
                    chain.process(l.data() + off, r.data() + off, 320);
                }
                // La etapa TΩ debe haber compilado (Reloj 2) al enganchar.
                if (volOn) EXPECT_GE(chain.tOmegaCompiles(), 1);
                const std::complex<double> oL = toneBinGoertzel(l.data(), skip, meas, freq, kFs);
                const std::complex<double> oR = toneBinGoertzel(r.data(), skip, meas, freq, kFs);
                const std::complex<double> inTone = toneBinGoertzel(
                    (side == 0 ? inL.data() : inR.data()), skip, meas, freq, kFs);
                apex::Complex2x2 A;
                buildPlantVol(freq, 0.0, sp, ws.sphereWs, A);
                const std::complex<double> earL = A.m00 * oL + A.m01 * oR;
                const std::complex<double> earR = A.m10 * oL + A.m11 * oR;
                double xt = 0.0;
                if (side == 0) {
                    xt = std::abs(earL) > 1e-12 ? std::abs(earR) / std::abs(earL) : 1e9;
                } else {
                    xt = std::abs(earR) > 1e-12 ? std::abs(earL) / std::abs(earR) : 1e9;
                }
                EXPECT_TRUE(std::isfinite(xt)) << "crosstalk no finito";
                meanXt += xt;
                if (volOn) {
                    // Predicción directa del kernel: W_TΩ (misma geometría
                    // Genezi que compila la cadena) y composite C = A·W.
                    const apex::SpeakerPairPhysics spT;
                    const apex::ComplexMatrix2x2 W =
                        apex::computeSpeakersMatrixBin(freq, spT, sphereWsT);
                    const std::complex<double> c00 = A.m00 * W.m00 + A.m01 * W.m10;
                    const std::complex<double> c01 = A.m00 * W.m01 + A.m01 * W.m11;
                    const std::complex<double> c10 = A.m10 * W.m00 + A.m11 * W.m10;
                    const std::complex<double> c11 = A.m10 * W.m01 + A.m11 * W.m11;
                    const double predXt = std::abs((side == 0) ? c10 / c00 : c01 / c11);
                    EXPECT_NEAR(xt, predXt, 0.02)
                        << "crosstalk medido ≠ predicción del kernel TΩ";
                    xtPredMean += predXt;
                    measEarOn[side] = std::abs((side == 0) ? earL : earR);
                    inToneMag[side] = std::abs(inTone);
                    desiredC[side] = std::abs((side == 0) ? c00 : c11);
                } else {
                    // Ganancia serie (sin matriz): salida/entrada en el
                    // canal del tono. Misma cadena, mismos controles,
                    // misma entrada — solo falta la matriz.
                    const double outMag = std::abs((side == 0) ? oL : oR);
                    serialGain[side] = outMag / std::max(std::abs(inTone), 1e-300);
                }
            }
            meanXt /= 2.0;
            if (volOn) {
                xtOn = meanXt;
                xtPredMean /= 2.0;
            } else {
                xtOff = meanXt;
            }
            std::printf("[FusedChain] TOmega crosstalk %.0f Hz vol=%d: %.3f (pred %.3f)\n",
                        freq, volOn ? 1 : 0, meanXt, volOn ? xtPredMean : 0.0);
        }
        // Nivel absoluto auto-calibrado: predEar = |C| · |tono| · g_serie.
        // (La cadena serie pierde ~0.63 dB en régimen permanente sobre
        // tonos puros — medido, igual a 1 y 3 kHz, con APEX y psico fuera;
        // fuente exacta sin identificar dentro de la cadena serie. NO es
        // la TΩ: a 3 kHz el tono cae exacto en un bin y la TΩ es exacta
        // por cancelación de las bandas laterales en el OLA. Queda
        // documentado aquí; investigarlo es otro ítem, no este.)
        for (int side = 0; side < 2; ++side) {
            const double predEar =
                desiredC[side] * inToneMag[side] * serialGain[side];
            const double errDb = 20.0 * std::log10(
                measEarOn[side] / std::max(predEar, 1e-300));
            const double gDb = 20.0 * std::log10(std::max(serialGain[side], 1e-300));
            std::printf("[FusedChain] TOmega %.0f Hz lado=%d: xt pred ok, "
                        "nivelErr=%+.2f dB (g_serie=%+.2f dB)\n",
                        freq, side, errDb, gDb);
            EXPECT_NEAR(errDb, 0.0, 0.5)
                << "nivel en oído deseado ≠ predicción TΩ (COLA/pack?)";
        }
        EXPECT_LT(xtOn, 0.30) << "crosstalk TΩ a " << freq << " Hz = " << xtOn;
        EXPECT_LT(xtOn, xtOff) << "la TΩ no bajó el crosstalk a " << freq
                               << " Hz (on=" << xtOn << " off=" << xtOff << ")";
    }
}

TEST(FusedChain, TOmegaNeutralBitExactSpeakers) {
    // La puerta TΩ exige !neutral: en SPEAKERS-neutro la etapa no se
    // engancha y process() sigue siendo bit-exacto (ni STFT ni matriz).
    FusedChain chain;
    chain.prepare(kFs);
    chain.setNeutral();
    FusedControls c;
    c.setNeutral();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    chain.setControls(c);
    EXPECT_FALSE(chain.tOmegaActive());
    const std::size_t n = 4800;
    std::vector<double> l = tone(n), r = tone(n);
    const std::vector<double> lIn = l, rIn = r;
    chain.process(l.data(), r.data(), static_cast<int>(n));
    const NullResult nl = nullTest(lIn.data(), l.data(), n);
    const NullResult nr = nullTest(rIn.data(), r.data(), n);
    EXPECT_TRUE(nl.bitExact) << "residual L " << nl.residualDb << " dB";
    EXPECT_TRUE(nr.bitExact) << "residual R " << nr.residualDb << " dB";
    EXPECT_EQ(chain.tOmegaCompiles(), 0);
}

TEST(FusedChain, TOmegaCompileOnlyOnControlChange) {
    // Reloj 2: la matriz se compila UNA vez al enganchar y NO se recompila
    // en los bloques siguientes si los controles no cambian; al cambiar el
    // modo (o cualquier campo del snapshot) recompila exactamente una vez.
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c = engagedControls();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    chain.setControls(c);
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_EQ(chain.tOmegaCompiles(), 1);
    chain.process(l.data(), r.data(), 4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_EQ(chain.tOmegaCompiles(), 1) << "Reloj 2 recompiló sin cambio";
    c.spatial.mode = ivanna::OmegaOutputMode::HEADPHONES;
    c.clamp();
    chain.setControls(c);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_EQ(chain.tOmegaCompiles(), 1)
        << "en HEADPHONES la etapa no engancha (M=I, sin compilar)";
    // Volver a SPEAKERS con el MISMO snapshot (modo/fs/geometría) NO
    // recompila: la matriz compilada sigue siendo válida — no recompilar
    // es lo correcto, no un fallo del Reloj 2.
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    chain.setControls(c);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_EQ(chain.tOmegaCompiles(), 1)
        << "mismo snapshot: la matriz sigue válida, sin recompilar";
    // reset() fuerza recompilar en el próximo process().
    chain.reset();
    chain.process(l.data(), r.data(), 4800);
    EXPECT_EQ(chain.tOmegaCompiles(), 2) << "reset() fuerza recompilar";
    // Cambio real del snapshot (fs 48k → 44.1k) → recompila.
    chain.prepare(44100.0);
    chain.setControls(c);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_EQ(chain.tOmegaCompiles(), 3) << "cambio de fs recompila";
}

TEST(FusedChain, TOmegaDspUs) {
    // Benchmark en host: coste de FusedChain::process con controles
    // engaged, por bloque de Ruta A (320 muestras @ 48 kHz, periodo
    // 6666.67 us). Se reporta media/p50/p95/max por variante. El assert
    // va sobre la MEDIANA (p50): en esta VM compartida los stalls de
    // planificación inflan media/p95/max (decenas de ms) y no son DSP;
    // la p50 es el coste real por bloque. La medición en el G85 la hace
    // el dueño.
    constexpr int kBlock = 320;
    constexpr int kWarmup = 200;
    constexpr int kMeasured = 2000;
    const std::size_t n = 48000;
    const std::vector<double> srcL = tone(n), srcR = tone(n);
    auto bench = [&](const char* name, const FusedControls& c, bool volOn) {
        FusedChain chain;
        chain.prepare(kFs);
        chain.setControls(c);
        chain.setVolumetricEnabled(volOn);
        std::vector<double> l = srcL, r = srcR;
        std::vector<double> us;
        us.reserve(static_cast<std::size_t>(kMeasured));
        std::size_t off = 0;
        for (int b = 0; b < kWarmup + kMeasured; ++b) {
            if (off + static_cast<std::size_t>(kBlock) > n) off = 0;
            const auto t0 = std::chrono::steady_clock::now();
            chain.process(l.data() + off, r.data() + off, kBlock);
            const auto t1 = std::chrono::steady_clock::now();
            if (b >= kWarmup) {
                us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            }
            off += static_cast<std::size_t>(kBlock);
        }
        std::sort(us.begin(), us.end());
        const double mean = std::accumulate(us.begin(), us.end(), 0.0) /
                            static_cast<double>(us.size());
        const double p50 = us[us.size() / 2];
        const double p95 = us[(us.size() * 95) / 100];
        const double mx = us.back();
        std::printf("[FusedChain] dsp_us %s: mean=%.1f p50=%.1f p95=%.1f "
                    "max=%.1f us/bloque(%d) compiles=%d\n",
                    name, mean, p50, p95, mx, kBlock, chain.tOmegaCompiles());
        EXPECT_LT(p50, 6666.67) << "mediana dsp_us " << name << " supera 1 periodo";
        return mean;
    };
    const FusedControls hp = engagedControls();
    FusedControls sp = engagedControls();
    sp.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    sp.clamp();
    bench("HEADPHONES engaged (TΩ off, M=I)", hp, true);
    bench("SPEAKERS engaged (TΩ XTC activa)", sp, true);
    bench("SPEAKERS engaged (etapa off)", sp, false);
}

// ── GEMELO ACÚSTICO + CALIBRACIÓN — CALL SITE (2026-11-21/22) ──────────────
// El call site vive en FusedChain::pushEffectiveControls ->
// refreshTwinCalibration: en SPEAKERS aplica la estimación calibrada del
// gemelo (escala 0.75, confianza honesta, nota de voz) a la conservación,
// que es el camino vivo que ya consume esa escala en el hilo de audio; en
// HEADPHONES deja la escala en 1.0 (sin cambio). La conservación está
// encendida (amount 1.0) en estos tests para que la escala sea
// observable; con amount 0 la escala no toca el sonido (0 x escala = 0).
namespace {
FusedControls speakersControls() {
    FusedControls c = engagedControls();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.conservation.amount = 1.0f;
    c.clamp();
    return c;
}

FusedControls headphonesControls() {
    FusedControls c = engagedControls();
    c.spatial.mode = ivanna::OmegaOutputMode::HEADPHONES;
    c.conservation.amount = 1.0f;
    c.clamp();
    return c;
}
}  // namespace

TEST(FusedChainTwinCallSite, SpeakersAppliesCalibratedTwin) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(speakersControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // La escala calibrada (0.75, nota de voz) llega a la conservación en vivo.
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
    EXPECT_NEAR(chain.telemetry().twinCalibrationConfidence, 0.75f, 1e-6f);
    const auto& est = chain.twinEstimate();
    EXPECT_TRUE(est.valid);
    EXPECT_TRUE(est.calibrationApplied);
    EXPECT_NEAR(est.calibrationConfidence, 0.75, 1e-9);
    EXPECT_NEAR(est.overallConfidence, 0.75, 1e-9);
    EXPECT_NEAR(est.overallInterventionScale, 0.75, 1e-9);
    // La respuesta de calibración llega: 63 Hz -6.22 dB, 125 Hz +1.17 dB
    // (medidos desde la nota de voz, no el estimado -3.24/+1.97).
    EXPECT_NEAR(est.calibratedResponseDb[0], -6.22, 0.01);
    EXPECT_NEAR(est.calibratedResponseDb[1], 1.17, 0.01);
    EXPECT_FALSE(est.satellitesActive);  // surround OFF: satélite inactivo
    // La salida sigue siendo finita (la calibración no rompe el audio).
    for (std::size_t i = 0; i < l.size(); ++i) {
        ASSERT_TRUE(std::isfinite(l[i]) && std::isfinite(r[i]));
    }
}

TEST(FusedChainTwinCallSite, HeadphonesLeavesScaleAtOne) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(headphonesControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // Sin calibración (audífonos): escala 1.0 = comportamiento actual,
    // la conservación se comporta exactamente como antes del call site.
    EXPECT_DOUBLE_EQ(chain.conservation().twinInterventionScale(), 1.0);
    EXPECT_FALSE(chain.telemetry().twinCalibrationApplied);
    EXPECT_FLOAT_EQ(chain.telemetry().twinCalibrationConfidence, 0.0f);
    EXPECT_FALSE(chain.twinEstimate().valid);
}

TEST(FusedChainTwinCallSite, InvalidCalibrationFallsBackToUncalibrated) {
    using namespace ivanna::fused::acoustic_twin;
    FusedChain chain;
    chain.prepare(kFs);
    // Calibración inválida (confianza honesta fuera de [0,1]).
    TwinCalibration bad;
    bad.honestConfidence = 2.0;
    ASSERT_FALSE(bad.isValid());
    chain.setTwinCalibration(bad);
    chain.setControls(speakersControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // Fallback al estimado SIN calibrar: la confianza es la del gemelo
    // sin valores derivados (~0.46), NUNCA el 0.926 inflado ni el 0.75.
    const double expected =
        AcousticTwin::estimate(AcousticTwinParams::esseGenezi())
            .overallInterventionScale;
    EXPECT_GT(expected, 0.3);
    EXPECT_LT(expected, 0.7);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), expected, 1e-6);
    EXPECT_FALSE(chain.telemetry().twinCalibrationApplied);
    EXPECT_FLOAT_EQ(chain.telemetry().twinCalibrationConfidence, 0.0f);
    EXPECT_FALSE(chain.twinEstimate().calibrationApplied);
}

TEST(FusedChainTwinCallSite, ModeSwitchBackToHeadphonesResetsScale) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(speakersControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    // Cambio a audífonos: la escala vuelve a 1.0 (la calibración del
    // Genezi no debe quedarse escalando la conservación en audífonos).
    chain.setControls(headphonesControls());
    chain.process(l.data(), r.data(), 4800);
    EXPECT_DOUBLE_EQ(chain.conservation().twinInterventionScale(), 1.0);
    EXPECT_FALSE(chain.telemetry().twinCalibrationApplied);
}

TEST(FusedChainTwinCallSite, MeasuredCalibrationOverridesPreset) {
    using namespace ivanna::fused::acoustic_twin;
    FusedChain chain;
    chain.prepare(kFs);
    // Calibración MEDIDA futura (isEstimated=false, confianza honesta
    // 0.60, sin respuesta por banda): manda sobre la estimada del Genezi.
    TwinCalibration measured;
    measured.honestConfidence = 0.60;
    measured.directPathConfidence = 0.60;
    measured.bassConfidence = 0.60;
    measured.midConfidence = 0.60;
    measured.trebleConfidence = 0.60;
    measured.roomConfidence = 0.60;
    measured.isEstimated = false;
    measured.responseKnown = false;
    measured.valid = measured.isValid();
    ASSERT_TRUE(measured.isValid());
    chain.setTwinCalibration(measured);
    chain.setControls(speakersControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.60, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
    EXPECT_NEAR(chain.telemetry().twinCalibrationConfidence, 0.60f, 1e-6f);
}

TEST(FusedChainTwinCallSite, ResetForcesRefresh) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(speakersControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    chain.reset();
    chain.process(l.data(), r.data(), 4800);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
}

// ── GEMELO ACÚSTICO — CONTROL EN VIVO (orden de Esse, 2026-11-21) ─────────
// Con la calibración aplicada (SPEAKERS, válida), el gemelo CONTROLA
// la conservación: si el usuario no fijó amount ni techo, se usan los
// defaults del gemelo (amount 0.5, techo 0.8). Sin el amount la escala
// 0.75 no haría nada (0 × 0.75 = 0); sin el techo el solucionador nunca
// constriñe (ganancia siempre 1.0) y el control sería un placebo.
// Efectivo: 0.5 × 0.75 = 0.375. El amount del usuario (> 0) y un techo
// fijado por el usuario (< 1.0) siempre mandan. En audífonos y con
// calibración inválida no hay control (fail-closed).
namespace {
FusedControls speakersTwinDefaultControls() {
    FusedControls c = engagedControls();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.conservation.amount = 0.0f;     // default: sin fijar (manda el gemelo)
    c.conservation.energyCap = 1.0f;  // default: sin techo (manda el gemelo)
    c.clamp();
    return c;
}

FusedControls headphonesTwinDefaultControls() {
    FusedControls c = engagedControls();
    c.spatial.mode = ivanna::OmegaOutputMode::HEADPHONES;
    c.conservation.amount = 0.0f;
    c.conservation.energyCap = 1.0f;
    c.clamp();
    return c;
}

// Solo un toque de EQ grave en bocinas: la cadena NO es neutra (los
// defaults del gemelo aplican) pero el tono de 1 kHz no lo toca el EQ
// de graves (banda 60–120 Hz) ni ningún otro operador, así que llega a
// la conservación a fondo de escala. (Con los 7 encendidos, ORBE/HOLOS
// lo bajan ~17 dB y ninguna banda supera el techo 0.8; con solo el
// cerebro, brain.isNeutral() es true cuando mode==1.) APEX se apaga en
// el test para medir la conservación sola, como ChainConservationCapsEnergy.
FusedControls speakersTonalTickleControls() {
    FusedControls c;
    c.setNeutral();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.tonal.lowDb = 1.0f;  // solo un toque de graves: la cadena no es neutra
    c.conservation.amount = 0.0f;     // default: sin fijar (manda el gemelo)
    c.conservation.energyCap = 1.0f;  // default: sin techo (manda el gemelo)
    c.clamp();
    return c;
}
}  // namespace

TEST(FusedChainTwinControl, SpeakersAppliesTwinDefaults) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(speakersTwinDefaultControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // El gemelo pone sus defaults: amount 0.5 y techo 0.8 (el usuario
    // no fijó ninguno). La escala es la confianza honesta 0.75, así que
    // el amount efectivo es 0.5 × 0.75 = 0.375 > 0: la conservación está
    // ACTIVA y el gemelo la modula.
    EXPECT_FLOAT_EQ(chain.conservation().config().amount,
                    FusedChain::kTwinCalibratedDefaultConservationAmount);
    EXPECT_FLOAT_EQ(chain.conservation().config().energyCap,
                    FusedChain::kTwinCalibratedDefaultConservationEnergyCap);
    EXPECT_TRUE(chain.conservation().isActive());
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
    EXPECT_NEAR(chain.telemetry().twinCalibrationConfidence, 0.75f, 1e-6f);
    for (std::size_t i = 0; i < l.size(); ++i) {
        ASSERT_TRUE(std::isfinite(l[i]) && std::isfinite(r[i]));
    }
}

TEST(FusedChainTwinControl, SpeakersActsOnHotSignal) {
    using namespace ivanna::fused::acoustic_twin;
    // Tono caliente a 1 kHz, solo un toque de EQ grave en bocinas: la
    // banda de 1 kHz supera el techo 0.8 y la conservación (efectivo
    // 0.375) la recorta. Se compara contra una cadena idéntica con
    // calibración inválida (sin control del gemelo: conservación en
    // bypass), para aislar el efecto del gemelo. APEX apagado para medir
    // la conservación sola.
    FusedChain twin;
    twin.prepare(kFs);
    twin.setApexEnabled(false);
    twin.setControls(speakersTonalTickleControls());
    std::vector<double> lTwin = tone(48000, 1.0), rTwin = tone(48000, 1.0);
    twin.process(lTwin.data(), rTwin.data(), 48000);
    FusedChain plain;
    plain.prepare(kFs);
    plain.setApexEnabled(false);
    TwinCalibration bad;
    bad.honestConfidence = 2.0;
    ASSERT_FALSE(bad.isValid());
    plain.setTwinCalibration(bad);
    plain.setControls(speakersTonalTickleControls());
    std::vector<double> lPlain = tone(48000, 1.0), rPlain = tone(48000, 1.0);
    plain.process(lPlain.data(), rPlain.data(), 48000);
    // La del gemelo recorta (ganancia < 1) y la de bypass no (ganancia 1).
    EXPECT_LT(twin.conservation().appliedGain(), 1.0f)
        << "el gemelo no está recortando (appliedGain=1.0)";
    EXPECT_FLOAT_EQ(plain.conservation().appliedGain(), 1.0f);
    // El recorte es suave: la cola de la del gemelo no baja más de 1 dB
    // respecto a la de bypass (no recorta de más).
    const double dbTwin = rmsDb(lTwin.data() + 24000, 24000);
    const double dbPlain = rmsDb(lPlain.data() + 24000, 24000);
    EXPECT_LT(dbTwin, dbPlain) << "el gemelo no está recortando el audio";
    EXPECT_GT(dbTwin, dbPlain - 1.0) << "el gemelo recorta demasiado";
    for (std::size_t i = 0; i < lTwin.size(); ++i) {
        ASSERT_TRUE(std::isfinite(lTwin[i]) && std::isfinite(rTwin[i]));
    }
}

TEST(FusedChainTwinControl, UserAmountWins) {
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c = speakersTwinDefaultControls();
    c.conservation.amount = 1.0f;  // el usuario fija su amount: manda él
    c.clamp();
    chain.setControls(c);
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_FLOAT_EQ(chain.conservation().config().amount, 1.0f);
    // El techo sí lo pone el gemelo (el usuario lo dejó en 1.0).
    EXPECT_FLOAT_EQ(chain.conservation().config().energyCap,
                    FusedChain::kTwinCalibratedDefaultConservationEnergyCap);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
}

TEST(FusedChainTwinControl, UserCapWins) {
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c = speakersTwinDefaultControls();
    c.conservation.energyCap = 0.5f;  // el usuario fija su techo: manda él
    c.clamp();
    chain.setControls(c);
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // El amount lo pone el gemelo (el usuario lo dejó en 0).
    EXPECT_FLOAT_EQ(chain.conservation().config().amount,
                    FusedChain::kTwinCalibratedDefaultConservationAmount);
    EXPECT_FLOAT_EQ(chain.conservation().config().energyCap, 0.5f);
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
}

TEST(FusedChainTwinControl, InvalidCalibrationNoControl) {
    using namespace ivanna::fused::acoustic_twin;
    FusedChain chain;
    chain.prepare(kFs);
    TwinCalibration bad;
    bad.honestConfidence = 2.0;
    ASSERT_FALSE(bad.isValid());
    chain.setTwinCalibration(bad);
    chain.setControls(speakersTwinDefaultControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // Calibración inválida: no se aplican los defaults del gemelo y la
    // conservación queda apagada (fail-closed). La escala es la del
    // estimado sin calibrar (~0.46), no el 0.75.
    EXPECT_FLOAT_EQ(chain.conservation().config().amount, 0.0f);
    EXPECT_FLOAT_EQ(chain.conservation().config().energyCap, 1.0f);
    EXPECT_FALSE(chain.conservation().isActive());
    EXPECT_FALSE(chain.telemetry().twinCalibrationApplied);
    EXPECT_FLOAT_EQ(chain.telemetry().twinCalibrationConfidence, 0.0f);
    const double expected =
        AcousticTwin::estimate(AcousticTwinParams::esseGenezi())
            .overallInterventionScale;
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), expected, 1e-6);
    for (std::size_t i = 0; i < l.size(); ++i) {
        ASSERT_TRUE(std::isfinite(l[i]) && std::isfinite(r[i]));
    }
}

TEST(FusedChainTwinControl, NeutralSpeakersNoControl) {
    // En neutro el gemelo NO controla: el usuario pide bypass bit-exacto
    // y los defaults del gemelo no deben activarlo (TOmegaNeutralBitExact
    // y ChainNeutralStillBitExact lo exigen).
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c;
    c.setNeutral();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    chain.setControls(c);
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    EXPECT_FLOAT_EQ(chain.conservation().config().amount, 0.0f);
    EXPECT_FLOAT_EQ(chain.conservation().config().energyCap, 1.0f);
    EXPECT_FALSE(chain.conservation().isActive());
    // La escala sí se alimenta (0.75), pero sin amount no actúa.
    EXPECT_NEAR(chain.conservation().twinInterventionScale(), 0.75, 1e-6);
    EXPECT_TRUE(chain.telemetry().twinCalibrationApplied);
}

TEST(FusedChainTwinControl, HeadphonesNoChange) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(headphonesTwinDefaultControls());
    std::vector<double> l = tone(4800), r = tone(4800);
    chain.process(l.data(), r.data(), 4800);
    // Audífonos: la calibración del Genezi no aplica y no hay control
    // del gemelo. La conservación queda como la dejó el usuario
    // (apagada) y la escala en 1.0 (comportamiento actual intacto).
    EXPECT_FLOAT_EQ(chain.conservation().config().amount, 0.0f);
    EXPECT_FLOAT_EQ(chain.conservation().config().energyCap, 1.0f);
    EXPECT_FALSE(chain.conservation().isActive());
    EXPECT_DOUBLE_EQ(chain.conservation().twinInterventionScale(), 1.0);
    EXPECT_FALSE(chain.telemetry().twinCalibrationApplied);
    EXPECT_FLOAT_EQ(chain.telemetry().twinCalibrationConfidence, 0.0f);
}
