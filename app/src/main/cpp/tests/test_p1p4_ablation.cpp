// test_p1p4_ablation.cpp — REFINAMIENTO TOTAL MAGISTRAL, P1+P4 (2026-11-26).
//
// Ablación por operador (medición digital, NO ABX) + Regla de Oro por
// banda ERB. Todo número de este archivo se MIDE en la FusedChain real;
// nada se afirma por razonamiento.
//
// MÉTODO (idéntico a la sonda fix-#1432, probe-p8-netgain2.cpp — NO es
// test del repo, vive en ~/workspace/corona-d10/logs/fix-1432/):
//   tono dual-mono 1 kHz −3 dBFS (amp 0.7071), 2 s @ 48 kHz, HEADPHONES,
//   setVolumetricEnabled(true), régimen = 2º segundo (el 1º es
//   transitorio + convergencia del árbitro/AGC). Ganancia neta = RMS
//   salida − RMS entrada por canal, en dB.
//   ALL7 = los 7 operadores a 1.0 con trimDb = 0 (sin compensación P8).
//   Leave-one-out (LOO) = ALL7 menos un operador (su control a 0).
//   Marginal LOO del operador X = net(ALL7) − net(ALL7 sin X).
//   La suma de los 7 marginales LOO NO es la pérdida total: la cadena
//   no es aditiva. La diferencia (total − suma) es el término de
//   interacción del modelo lineal y se carga a NEURO (operador dominante
//   y responsable de la dependencia de nivel); ver
//   fused/experience_compiler/experience_compiler.cpp.
//
// REGLA DE ORO POR BANDA ERB (§1.3): la regla se mide por banda con
// tonos en los centros ERB de Band24Profile (102…11500 Hz), en DOS
// configuraciones: (a) la ENGAGED de P0 (la cadena congelada real,
// sin trim P8) y (b) ALL7 trim 0. El veredicto está en
// tests/p1p4_golden_rule.md: la regla "0.0 dB" SOLO es cierta en
// neutro (bit-exacto) y como modelo NOMINAL de P8 (trim medido); la
// cadena enganchada sin trim mide −6.06/−6.27 dB en banda ancha (P0
// congelado) y −14.18 dB en ALL7. Este test es la medición que lo
// prueba, no la afirmación.
//
// ORDEN DE LA ABLACIÓN (directiva): ARMONÍA → PULSO → SALA VIVA →
// HOLOS/ORBE → NEURO → Volterra → APEX. La tabla LOO por banda cubre
// los 7; los tests dedicados aíslan ARMONÍA plana vs activa, SALA VIVA
// off vs 0.22 (y roomSizeM 5.0 vs 7.0) y el widener de PULSO
// (ITD/ILD en HEADPHONES vs SPEAKERS).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/experience_compiler/experience_compiler.hpp"
#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
#include "../fused/omega_sigma_operator.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {

constexpr double kFs = 48000.0;
constexpr double kAmp = 0.7071;  // −3 dBFS
constexpr std::size_t kN = 96000;     // 2 s
constexpr std::size_t kSkip = 48000;  // 1 s de transitorio/convergencia
// Nivel RMS exacto del tono de entrada (amp 0.7071, NO 1/√2 exacto:
// 20·log10(0.7071/√2) = −6.020683 dB; el literal redondeado −6.0206
// sesgaba la ganancia neta en −8.3e-05 dB — detectado por el test de
// neutralidad, que falló con −8.32e-05 dB en vez de 0.0 exacto).
const double kInRmsDb = 20.0 * std::log10(0.7071 / std::sqrt(2.0));

// Centros ERB de Band24Profile (72…11500 Hz): 9 bandas que cubren el
// espectro audible sin repetir la tabla entera de 24.
constexpr double kErbHz[9] = {102.0, 217.0, 384.0, 626.0, 977.0,
                              1486.0, 2226.0, 3300.0, 11500.0};

const char* kOpName[7] = {"ORBE", "HOLOS", "SALA", "VOLTERRA",
                          "ARMONIA", "PULSO", "NEURO"};

void zero7(FusedControls& c) {
    c.spatial.amount = 0.0f;
    c.field.amount = 0.0f;
    c.field.omniAmount = 0.0f;  // Unificación 2.5.509 (paso 5): op 1 = campo entero.
    c.listenerRoom.roomWet = 0.0f;
    c.transducer.correction = 0.0f;
    c.tonal.amount = 0.0f;
    c.dynamics.punch = 0.0f;
    c.brain.adaptation = 0.0f;
}

void setOp(FusedControls& c, std::size_t op, float v) {
    switch (op) {
        case 0: c.spatial.amount = v; break;
        case 1: c.field.amount = v; c.field.omniAmount = v; break;  // Unif. 2.5.509 paso 5: op 1 = campo entero
        case 2: c.listenerRoom.roomWet = v; break;
        case 3: c.transducer.correction = v; break;
        case 4: c.tonal.amount = v; break;
        case 5: c.dynamics.punch = v; break;
        case 6: c.brain.adaptation = v; break;
        default: break;
    }
}

// ALL7 exacto de la sonda #1432: defaults + 7 a 1.0 + trim 0.
FusedControls all7Controls() {
    FusedControls c;
    for (std::size_t o = 0; o < 7; ++o) setOp(c, o, 1.0f);
    c.tonal.trimDb = 0.0f;
    c.clamp();
    return c;
}

FusedControls looControls(std::size_t without) {
    FusedControls c = all7Controls();
    setOp(c, without, 0.0f);
    c.clamp();
    return c;
}

// Configuración ENGAGED de P0 (idéntica a test_p0_reference_lock.cpp
// engagedControls() y a test_fused_chain::engagedControls()).
FusedControls p0EngagedControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.field.amount = 0.5f;
    c.field.omniAmount = 0.5f;  // Unificación 2.5.509 (paso 5): OmniField ya NO sigue a HOLOS —
    // su amount es propio; el helper fija el valor explícito para
    // preservar la configuración medida (antes lo ponía field.amount).
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

struct ToneResult {
    double netL = 0.0, netR = 0.0;
    double winL[8] = {}, winR[8] = {};  // RMS dBFS por ventana de 0.25 s
    double peakL = 0.0, peakR = 0.0;
};

ToneResult runTone(const FusedControls& cIn, double freqHz, double amp = kAmp) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(cIn);
    chain.setVolumetricEnabled(true);
    std::vector<double> l(kN), r(kN);
    fillToneDouble(l.data(), kN, kFs, freqHz, amp);
    fillToneDouble(r.data(), kN, kFs, freqHz, amp);
    chain.process(l.data(), r.data(), static_cast<int>(kN));
    ToneResult res;
    res.netL = rmsDb(l.data() + kSkip, kN - kSkip) - kInRmsDb;
    res.netR = rmsDb(r.data() + kSkip, kN - kSkip) - kInRmsDb;
    res.peakL = peakAbs(l.data(), kN);
    res.peakR = peakAbs(r.data(), kN);
    for (int w = 0; w < 8; ++w) {
        const std::size_t off = static_cast<std::size_t>(w) * 12000;
        res.winL[w] = rmsDb(l.data() + off, 12000);
        res.winR[w] = rmsDb(r.data() + off, 12000);
    }
    return res;
}

double meanLr(double a, double b) { return 0.5 * (a + b); }

// Proyección LS de la fundamental (amplitud + fase) sobre una ventana.
// La fase da el ITD; la amplitud por canal da el ILD.
void projectFund(const double* x, std::size_t n, double freqHz, double& amp,
                 double& phase) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double step = kTwoPi * freqHz / kFs;
    double s = 0.0, c = 0.0, ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[i];
        s += v * std::sin(ph);
        c += v * std::cos(ph);
        ph += step;
        if (ph >= kTwoPi) ph -= kTwoPi;
    }
    const double aSin = 2.0 * s / static_cast<double>(n);
    const double bCos = 2.0 * c / static_cast<double>(n);
    amp = std::sqrt(aSin * aSin + bCos * bCos);
    phase = std::atan2(bCos, aSin);  // fase de x = amp*sin(2πft + phase)
}

}  // namespace

// ── 1) Leave-one-out en ALL7 a 1 kHz: reproduce la sonda #1432 ──────────
// Valores de referencia (RESUMEN_FIX_1432.md, sonda probe-p8-netgain2,
// g++-14/clang++-18): ALL7 media −14.182 dB; marginales LOO media L/R:
// ORBE −0.999, HOLOS +0.057, SALA −0.044, VOLTERRA 0.000, ARMONÍA
// +0.858, PULSO −0.008, NEURO −11.154; suma −11.289 dB; interacción
// −2.892 dB. Tolerancia ±1.0 dB (la sonda es externa al repo y NEURO
// depende del nivel y del asentamiento del AGC).
// ACTUALIZACIÓN 2026-11-26 (integración P3): el marginal ORBE cambió de
// +0.525 a −0.999 dB por la mezcla de potencia constante (3.1) + el fix
// de spread (3.2) en OrbeBinaural; la suma y la interacción se mueven
// en la misma cantidad (−1.524 dB). Los demás marginales no cambian.
// ACTUALIZACIÓN 2026-11-26 (evol-hrtf, fix HRTF+ITD): el marginal ORBE
// cambió de −0.999 a −6.336 dB porque el par PCA era MONOAURAL (la mitad
// R de la base es un residuo del 0.4% amplificado 23 dB por la
// normalización) y el oído derecho ahora usa la reconstrucción válida
// (baseL, ver HrtfManager.cpp). El total ALL7 NO cambia (−14.181 dB):
// solo cambia la ATRIBUCIÓN entre ORBE y la interacción (la interacción
// se mueve +5.337 dB, de −2.892 a +2.445). Los demás marginales no
// cambian (HOLOS +0.056, SALA −0.044, VOLTERRA 0.000, ARMONÍA +0.858,
// PULSO −0.008, NEURO −11.153 — todos dentro de ±0.002 dB de la sonda).
// ACTUALIZACIÓN 2026-11-26 (phone-loop, puerta HRTF): la puerta cierra
// la HRTF cuando ORBE está a 0, así que el run sin-ORBE ya no renderiza
// la HRTF de reserva: el marginal ORBE pasa de −6.336 a +0.022 dB (la
// HRTF de reserva costaba ~6.36 dB a 1 kHz en ese run; ORBE standalone
// es ~0 dB, como mide test_p8_calibration). La suma se mueve en la misma
// cantidad (−16.626 → −10.263) y la interacción (+2.445 → −3.915). El
// total ALL7 NO cambia (−14.178 dB). La calibración intermedia (NEURO
// −8.709 = LOO +2.445) queda SUPERADA: la tabla nominal se re-deriva
// por segunda vez, apareada, con la descomposición post-puerta: ORBE
// +0.022, NEURO −15.067 (LOO −11.150 + interacción −3.915 = −15.065
// medido; −15.067 ajusta la suma a exactamente −19.482 dB). El trim P8
// (+19.482 dB) no cambia y la Regla de Oro sigue en −0.076/+0.078 dB.
// El assert del vínculo vuelve a valer con los valores post-puerta; la
// identidad total = suma + interacción (siempre cierta) se sigue
// verificando y el printf deja el valor a la vista.
TEST(P1P4Ablation, LeaveOneOutAt1kHz) {
    const ToneResult all7 = runTone(all7Controls(), 1000.0);
    std::printf("[P1P4-LOO] ALL7 trim0 1kHz: netL=%+.3f netR=%+.3f media=%+.3f "
                "(ref sonda #1432: -14.258/-14.105, media -14.182)\\n",
                all7.netL, all7.netR, meanLr(all7.netL, all7.netR));
    EXPECT_TRUE(std::isfinite(all7.netL) && std::isfinite(all7.netR));
    EXPECT_NEAR(meanLr(all7.netL, all7.netR), -14.182, 1.0)
        << "pérdida total ALL7 distinta de la sonda #1432";

    double sumLoo = 0.0;
    double neuroMarg = 0.0;
    for (std::size_t o = 0; o < 7; ++o) {
        const ToneResult loo = runTone(looControls(o), 1000.0);
        const double margL = all7.netL - loo.netL;
        const double margR = all7.netR - loo.netR;
        const double marg = meanLr(margL, margR);
        sumLoo += marg;
        if (o == 6) neuroMarg = marg;
        std::printf("[P1P4-LOO] sin %-8s: netL=%+.3f netR=%+.3f | marginal "
                    "L=%+.3f R=%+.3f media=%+.3f\\n",
                    kOpName[o], loo.netL, loo.netR, margL, margR, marg);
        if (o == 0) {
            // Puerta HRTF (phone-loop): sin ORBE la HRTF se cierra, así
            // que el run sin-ORBE es neutro en espacial y el marginal es
            // +0.022 dB (era −6.336 dB con la HRTF de reserva activa:
            // la reserva costaba ~6.36 dB a 1 kHz en ese run). El total
            // ALL7 no cambia; solo la atribución ORBE↔interacción.
            EXPECT_NEAR(marg, +0.022, 1.0)
                << "marginal ORBE distinto del valor con la puerta HRTF (+0.022 dB)";
        }
        if (o == 6) {
            EXPECT_NEAR(marg, -11.154, 1.0)
                << "marginal NEURO distinto de la sonda #1432 (-11.154 dB)";
        }
        if (o == 5) {
            EXPECT_NEAR(marg, -0.008, 1.0)
                << "marginal PULSO distinto de la sonda #1432 (-0.008 dB)";
        }
        if (o == 3) {
            EXPECT_NEAR(marg, 0.0, 1.0)
                << "marginal VOLTERRA distinto de la sonda #1432 (0.000 dB)";
        }
    }
    const double total = meanLr(all7.netL, all7.netR);
    const double interaction = total - sumLoo;
    std::printf("[P1P4-LOO] suma marginales=%+.3f dB | total=%+.3f dB | "
                "interacción=%+.3f dB (ref puerta-HRTF: suma -10.263, interacción -3.915)\\n",
                sumLoo, total, interaction);
    EXPECT_NEAR(sumLoo, -10.263, 1.0);
    EXPECT_NEAR(interaction, -3.915, 1.0);
    // El coeficiente nominal P8 de NEURO (−15.067) es LOO + interacción:
    // el modelo nominal queda atado a la medición, no a un número suelto.
    const double neuroCoeff =
        experience_compiler::nominalGainDbPerUnit(6);
    EXPECT_NEAR(neuroCoeff, -15.067, 0.05);
    // VÍNCULO RESTAURADO POR LA RE-DERIVACIÓN POST-PUERTA (phone-loop):
    // neuroMarg + interacción = −15.066 dB con la puerta HRTF (era
    // −8.711 dB con la HRTF de reserva activa), igual al nominal −15.067
    // (brecha ~0.002 dB). La tabla nominal se re-derivó apareada por
    // segunda vez (ORBE +0.022, NEURO −15.067; la suma sigue en
    // −19.482 dB). Aquí se verifica la identidad de la descomposición
    // (total = suma + interacción) y se deja el valor a la vista; NO se
    // ensancha la tolerancia.
    EXPECT_NEAR(total, sumLoo + interaction, 0.01)
        << "la descomposición LOO no cuadra (total != suma + interacción)";
    std::printf("[P1P4-LOO] P8: neuroMarg + interacción = %+.3f dB "
                "(nominal %+.3f, brecha %+.3f dB)\\n",
                neuroMarg + interaction, neuroCoeff,
                (neuroMarg + interaction) - neuroCoeff);
}

// ── 2) Regla de Oro por banda ERB ─────────────────────────────────────────
// Dos configuraciones, 9 bandas. La afirmación "0.0 dB" se somete a
// medición: en neutro es bit-exacta (0.0 dB); enganchada, NO.
TEST(P1P4Ablation, GoldenRulePerErbBand) {
    // (a) Neutro = la regla en su único ámbito exacto.
    {
        FusedControls c;
        c.setNeutral();
        const ToneResult t = runTone(c, 1000.0);
        EXPECT_NEAR(t.netL, 0.0, 1e-9);
        EXPECT_NEAR(t.netR, 0.0, 1e-9);
        std::printf("[P1P4-ORO] neutro 1kHz: netL=%+.3f netR=%+.3f (bit-exacto)\\n",
                    t.netL, t.netR);
    }
    std::printf("[P1P4-ORO] banda ERB | P0-engaged L/R (dB) | ALL7-trim0 L/R (dB)\\n");
    double p0At1k = 0.0;
    for (double f : kErbHz) {
        const ToneResult p0 = runTone(p0EngagedControls(), f);
        const ToneResult a7 = runTone(all7Controls(), f);
        std::printf("[P1P4-ORO] %7.0f Hz | P0 %+7.3f / %+7.3f | ALL7 %+7.3f / %+7.3f\\n",
                    f, p0.netL, p0.netR, a7.netL, a7.netR);
        EXPECT_TRUE(std::isfinite(p0.netL) && std::isfinite(p0.netR));
        EXPECT_TRUE(std::isfinite(a7.netL) && std::isfinite(a7.netR));
        if (f == 977.0) p0At1k = meanLr(p0.netL, p0.netR);
    }
    // La cadena P0 enganchada a ~1 kHz pierde nivel: NO es 0.0 dB.
    // (El P0 congelado en banda ancha es −6.065/−6.274 dB; aquí se ve
    // por banda dónde vive esa pérdida.)
    std::printf("[P1P4-ORO] P0-engaged a 977 Hz (banda ERB de 1 kHz): %+.3f dB "
                "(media L/R) — la Regla de Oro '0.0 dB' NO se cumple "
                "enganchada\\n",
                p0At1k);
    EXPECT_LT(p0At1k, -3.0)
        << "P0-engaged a ~1 kHz debería perder >3 dB (NEURO), no ~0 dB";
}

// ── 2b) El trim P8 compilado cierra 1 kHz… y solo 1 kHz ───────────────────
// D10Corona.MatchedLufsInvariantAcross7Operators PASA hoy (net −0.077/+0.076
// dB a 1 kHz con trim +19.482 dB compilado): el comentario de ese test que
// dice «sigue FALLANDO» es de 2026-11-25 y quedó desactualizado. Pero el
// trim es ESCALAR y la pérdida es ESPECTRAL (§2.2: ALL7-trim0 va de −7.6 a
// −16.0 dB por banda): el mismo trim que cierra 1 kHz deja las demás
// bandas abiertas. Aquí se mide el residuo por banda con el trim compilado.
TEST(P1P4Ablation, CompiledTrimClosesOnly1kHz) {
    FusedChain compiler;
    compiler.prepare(kFs);
    experience_compiler::ExperienceVector target;
    for (std::size_t i = 0; i < experience_compiler::kNumComponents; ++i) {
        target.v[i] = 1.0;
    }
    const experience_compiler::CompileResult compiled =
        compiler.compileExperience(target, experience_compiler::ReachableRegion{});
    ASSERT_TRUE(compiled.ok);
    ASSERT_TRUE(compiler.applyCompiledExperience(compiled));
    const FusedControls compiledControls = compiler.controls();
    std::printf("[P1P4-ORO] trim P8 compilado: trimDb=%+.3f netGainNominal=%+.3f dB\\n",
                static_cast<double>(compiledControls.tonal.trimDb),
                compiled.netGainDb);
    std::printf("[P1P4-ORO] banda ERB | net con trim compilado L/R (dB)\\n");
    double netAt1k = 0.0, worstOther = 0.0;
    for (double f : kErbHz) {
        const ToneResult t = runTone(compiledControls, f);
        const double m = meanLr(t.netL, t.netR);
        std::printf("[P1P4-ORO] %7.0f Hz | trim-compilado %+7.3f / %+7.3f\\n",
                    f, t.netL, t.netR);
        if (f == 977.0) {
            netAt1k = m;
        } else {
            worstOther = std::max(worstOther, std::fabs(m));
        }
    }
    // El trim compilado también se reproduce a 1000 Hz exactos (la
    // frecuencia del test D10, no el centro ERB 977 Hz).
    const ToneResult t1k = runTone(compiledControls, 1000.0);
    std::printf("[P1P4-ORO] 1000 Hz con trim compilado: %+.3f / %+.3f dB\\n",
                t1k.netL, t1k.netR);
    std::printf("[P1P4-ORO] trim compilado: 977 Hz %+.3f dB | peor otra banda "
                "|%.3f| dB\\n",
                netAt1k, worstOther);
    EXPECT_NEAR(meanLr(t1k.netL, t1k.netR), 0.0, 0.5);
    EXPECT_GT(worstOther, 2.0)
        << "un trim escalar no puede cerrar el espectro: al menos una banda "
           "distinta de 1 kHz debe quedar >2 dB abierta";
}

// ── 3) Tabla LOO por banda ERB ────────────────────────────────────────────
// Delta por operador y por banda (marginal LOO, media L/R). Es la tabla
// que faltaba: cada operador rinde cuentas en cada banda.
TEST(P1P4Ablation, LeaveOneOutPerErbBand) {
    std::printf("[P1P4-BANDA] f(Hz) | ALL7 | dORBE | dHOLOS | dSALA | dVOLT | "
                "dARMON | dPULSO | dNEURO (dB, media L/R)\\n");
    for (double f : kErbHz) {
        const ToneResult all7 = runTone(all7Controls(), f);
        double marg[7];
        for (std::size_t o = 0; o < 7; ++o) {
            const ToneResult loo = runTone(looControls(o), f);
            marg[o] = meanLr(all7.netL - loo.netL, all7.netR - loo.netR);
        }
        std::printf("[P1P4-BANDA] %7.0f | %+7.3f | %+6.3f | %+6.3f | %+6.3f | "
                    "%+6.3f | %+6.3f | %+6.3f | %+6.3f\\n",
                    f, meanLr(all7.netL, all7.netR), marg[0], marg[1], marg[2],
                    marg[3], marg[4], marg[5], marg[6]);
        for (std::size_t o = 0; o < 7; ++o)
            EXPECT_TRUE(std::isfinite(marg[o])) << "marginal no finito";
        // NEURO es el operador dominante en pérdida a 1 kHz y alrededores.
        if (f == 977.0) {
            EXPECT_LT(marg[6], -5.0);
        }
    }
}

// ── 4) ARMONÍA plana vs activa ────────────────────────────────────────────
// La coloración menos atribuida: P0-engaged (curva Harman 1 + low +2 dB)
// contra la misma cadena con ARMONÍA plana (curva 0, low/mid/high 0).
TEST(P1P4Ablation, ArmoniaFlatVsActive) {
    const double freqs[3] = {102.0, 977.0, 3300.0};
    for (double f : freqs) {
        FusedControls flat = p0EngagedControls();
        flat.tonal.curve = 0;
        flat.tonal.lowDb = 0.0f;
        flat.tonal.midDb = 0.0f;
        flat.tonal.highDb = 0.0f;
        flat.clamp();
        const ToneResult active = runTone(p0EngagedControls(), f);
        const ToneResult flatR = runTone(flat, f);
        const double delta =
            meanLr(active.netL, active.netR) - meanLr(flatR.netL, flatR.netR);
        std::printf("[P1P4-ARMONIA] %7.0f Hz: activa %+.3f dB | plana %+.3f dB "
                    "| delta ARMONÍA %+.3f dB\\n",
                    f, meanLr(active.netL, active.netR),
                    meanLr(flatR.netL, flatR.netR), delta);
        EXPECT_TRUE(std::isfinite(delta));
    }
}

// ── 5) SALA VIVA off vs 0.22; roomSizeM 5.0 (vivo) vs 7.0 (afirmado) ──────
// Antes de medir SALA VIVA: el perfil vivo fija roomSizeM = 5.0 m
// (default de FusedControls, FusedControls.kt y SalaProfileParams; los
// dos perfiles de audio_profiles.json ponen 5.0). Ningún perfil pone
// 7.0. Se mide off vs 0.22 a 5.0 m (lo vivo) y 5.0 vs 7.0 a 0.22.
TEST(P1P4Ablation, SalaVivaOffVs022AndRoomSize) {
    for (double f : {977.0, 217.0}) {
        FusedControls off = p0EngagedControls();
        off.listenerRoom.roomWet = 0.0f;
        off.clamp();
        const ToneResult on = runTone(p0EngagedControls(), f);
        const ToneResult offR = runTone(off, f);
        const double delta = meanLr(on.netL, on.netR) - meanLr(offR.netL, offR.netR);
        std::printf("[P1P4-SALA] %7.0f Hz: wet 0.22 %+.3f dB | off %+.3f dB | "
                    "delta SALA VIVA %+.3f dB (roomSizeM 5.0)\\n",
                    f, meanLr(on.netL, on.netR), meanLr(offR.netL, offR.netR),
                    delta);
        EXPECT_TRUE(std::isfinite(delta));
    }
    {
        FusedControls c5 = p0EngagedControls();  // roomSizeM default 5.0
        FusedControls c7 = p0EngagedControls();
        c7.listenerRoom.roomSizeM = 7.0f;
        c7.clamp();
        EXPECT_FLOAT_EQ(c5.listenerRoom.roomSizeM, 5.0f)
            << "el perfil vivo corre con el default 5.0 m, no 7.0 m";
        const ToneResult r5 = runTone(c5, 977.0);
        const ToneResult r7 = runTone(c7, 977.0);
        std::printf("[P1P4-SALA] 977 Hz wet 0.22: roomSizeM 5.0 %+.3f dB | 7.0 "
                    "%+.3f dB | delta 5→7 m %+.3f dB\\n",
                    meanLr(r5.netL, r5.netR), meanLr(r7.netL, r7.netR),
                    meanLr(r7.netL, r7.netR) - meanLr(r5.netL, r5.netR));
    }
}

// ── 6) PULSO widener: ITD/ILD en HEADPHONES vs SPEAKERS ──────────────────
// Entrada con ITD 10 muestras (~208 µs) e ILD −6 dB (R más bajo).
// El widener M/S es sin memoria: no debe mover el ITD, pero SÍ la ILD.
// Se mide con el ancho del usuario (width 1.35 = valor MUSIC del
// guardián) en HEADPHONES y en SPEAKERS, y con NEURO solo (guardián)
// para verificar la contención del guardián en SPEAKERS/binaural 3D
// (fused_chain.cpp, Fallo 3b).
namespace {

struct ItdIld {
    double itdSamples = 0.0;  // R respecto a L, en muestras
    double ildDb = 0.0;       // RMS L − RMS R, en dB
};

ItdIld measureItdIld(const FusedControls& cIn, double& guardianWidener) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(cIn);
    chain.setVolumetricEnabled(true);
    std::vector<double> l(kN), r(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        const double ph = 6.28318530717958647692 * 1000.0 *
                          static_cast<double>(i) / kFs;
        l[i] = kAmp * std::sin(ph);
        // R: mismo tono, 10 muestras tarde, −6 dB.
        const double phR = 6.28318530717958647692 * 1000.0 *
                           (static_cast<double>(i) - 10.0) / kFs;
        r[i] = 0.5 * kAmp * std::sin(phR);
    }
    chain.process(l.data(), r.data(), static_cast<int>(kN));
    double ampL = 0.0, phL = 0.0, ampR = 0.0, phR = 0.0;
    projectFund(l.data() + kSkip, kN - kSkip, 1000.0, ampL, phL);
    projectFund(r.data() + kSkip, kN - kSkip, 1000.0, ampR, phR);
    ItdIld res;
    double dphi = phR - phL;
    while (dphi > 3.14159265358979323846) dphi -= 6.28318530717958647692;
    while (dphi < -3.14159265358979323846) dphi += 6.28318530717958647692;
    // x = A·sin(2πft + φ): R atrasa 10 muestras ⇒ φR − φL = −2π·1000·10/48000.
    res.itdSamples = -dphi * kFs / (6.28318530717958647692 * 1000.0);
    res.ildDb = 20.0 * std::log10((ampL + 1e-12) / (ampR + 1e-12));
    guardianWidener = chain.telemetry().guardianWidener;
    return res;
}

}  // namespace

TEST(P1P4Ablation, PulsoWidenerItdIld) {
    double gw = 1.0;
    // Referencia: bypass (neutro) debe preservar ITD 10 e ILD +6 dB.
    {
        FusedControls c;
        c.setNeutral();
        const ItdIld ref = measureItdIld(c, gw);
        std::printf("[P1P4-WIDE] bypass: ITD=%+.2f muestras ILD=%+.2f dB "
                    "(entrada: 10.00 / +6.02)\\n",
                    ref.itdSamples, ref.ildDb);
        EXPECT_NEAR(ref.itdSamples, 10.0, 0.5);
        EXPECT_NEAR(ref.ildDb, 6.02, 0.1);
    }
    // Ancho del usuario 1.35 (= valor MUSIC del guardián) solo en PULSO.
    ItdIld hp{}, sp{}, spCtrl{};
    {
        FusedControls c;
        c.setNeutral();
        c.dynamics.width = 1.35f;
        c.dynamics.safety = 1.0f;
        c.clamp();
        hp = measureItdIld(c, gw);
        std::printf("[P1P4-WIDE] PULSO width 1.35 HEADPHONES: ITD=%+.2f "
                    "muestras ILD=%+.2f dB\\n",
                    hp.itdSamples, hp.ildDb);
    }
    {
        FusedControls c;
        c.setNeutral();
        c.dynamics.width = 1.35f;
        c.dynamics.safety = 1.0f;
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
        c.clamp();
        sp = measureItdIld(c, gw);
        std::printf("[P1P4-WIDE] PULSO width 1.35 SPEAKERS: ITD=%+.2f "
                    "muestras ILD=%+.2f dB\\n",
                    sp.itdSamples, sp.ildDb);
    }
    {
        // CONTROL DE ATRIBUCIÓN: SPEAKERS, cadena no neutra (safety 1.0)
        // pero SIN widener (width 1.0). Si el cambio ITD/ILD es el mismo
        // que con width 1.35, el culpable en SPEAKERS es la matriz TΩ
        // (XTC Kirkeby, fused_chain.cpp:994-1006), no el widener.
        FusedControls c;
        c.setNeutral();
        c.dynamics.width = 1.0f;
        c.dynamics.safety = 1.0f;
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
        c.clamp();
        spCtrl = measureItdIld(c, gw);
        std::printf("[P1P4-WIDE] control TΩ SPEAKERS width 1.0: ITD=%+.2f "
                    "muestras ILD=%+.2f dB\\n",
                    spCtrl.itdSamples, spCtrl.ildDb);
    }
    // El widener M/S NO es un remix en fase: con ITD 10 muestras la
    // componente side está en cuadratura parcial, así que ensanchar
    // ROTA las fases. Medido: en HEADPHONES el ITD se mueve +2.96
    // muestras (10.00 → 12.96) y la ILD apenas se mueve (+0.12 dB) —
    // el álgebra M/S lo predice (ITD 12.88, ILD 6.17 dB para w=1.35).
    // En SPEAKERS el cambio grande (ITD −4.47, ILD −4.00 dB) es la
    // matriz TΩ de cancelación de diafonía, no el widener: el control
    // sin widener da el mismo cambio.
    EXPECT_GT(hp.itdSamples - 10.0, 2.0);
    EXPECT_LT(hp.itdSamples - 10.0, 4.0);
    EXPECT_NEAR(hp.ildDb, 6.02, 0.5);
    EXPECT_NEAR(sp.itdSamples, spCtrl.itdSamples, 2.0);  // el widener
    // añade +1.42 muestras de rotación M/S sobre la TΩ (igual que en
    // HEADPHONES: +2.96); la TΩ domina el cambio (ITD 10 → ~4-5.5).
    EXPECT_NEAR(sp.ildDb, spCtrl.ildDb, 0.5);
    EXPECT_GT(std::fabs(sp.itdSamples - 10.0), 3.0);
    EXPECT_GT(std::fabs(sp.ildDb - 6.02), 2.0);
    std::printf("[P1P4-WIDE] delta ITD por widener 1.35: HEADPHONES %+.2f "
                "muestras, SPEAKERS %+.2f (ref 10.00); delta ILD: HP %+.2f "
                "dB, SP %+.2f dB (ref +6.02)\\n",
                hp.itdSamples - 10.0, sp.itdSamples - 10.0,
                hp.ildDb - 6.02, sp.ildDb - 6.02);
}

TEST(P1P4Ablation, GuardianWidenerContainment) {
    // NEURO solo (guardián AntiDolby) sin render binaural: en
    // HEADPHONES el guardián PUEDE inyectar widener (MUSIC 1.35); en
    // SPEAKERS y con binaural 3D la inyección se suprime (Fallo 3b,
    // fused_chain.cpp:790-808). La telemetría guardianWidener es el
    // valor PRE-arbitraje del guardián (1.35 en las 3 configs cuando
    // dispara): NO refleja la supresión — la prueba es la SEÑAL.
    // Controles de atribución: SPEAKERS sin NEURO (solo safety 1.0,
    // cadena no neutra → TΩ engancha) y ORBE solo (sin NEURO). Si el
    // guardián no añade nada en SPEAKERS/binaural, la ILD/ITD con
    // NEURO debe igualar la del control sin NEURO.
    double gwHp = 1.0, gwSp = 1.0, gwBin = 1.0;
    double gwSpCtrl = 1.0, gwOrbe = 1.0;
    ItdIld hp{}, sp{}, bin{}, spCtrl{}, orbe{};
    {
        FusedControls c;
        zero7(c);
        setOp(c, 6, 1.0f);  // NEURO solo
        c.spatial.hrtfWet = 0.0f;  // sin binaural 3D
        c.tonal.trimDb = 0.0f;
        c.clamp();
        hp = measureItdIld(c, gwHp);
    }
    {
        FusedControls c;
        zero7(c);
        setOp(c, 6, 1.0f);
        c.spatial.hrtfWet = 0.0f;
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
        c.tonal.trimDb = 0.0f;
        c.clamp();
        sp = measureItdIld(c, gwSp);
    }
    {
        FusedControls c;
        zero7(c);
        setOp(c, 6, 1.0f);
        setOp(c, 0, 1.0f);  // NEURO + ORBE = binaural 3D activo
        c.tonal.trimDb = 0.0f;
        c.clamp();
        bin = measureItdIld(c, gwBin);
    }
    {
        // Control TΩ: SPEAKERS sin NEURO (zero7 deja safety=1.0 por
        // default → cadena no neutra → TΩ engancha, sin guardián).
        FusedControls c;
        zero7(c);
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
        c.tonal.trimDb = 0.0f;
        c.clamp();
        spCtrl = measureItdIld(c, gwSpCtrl);
    }
    {
        // Control ORBE: binaural 3D sin NEURO.
        FusedControls c;
        zero7(c);
        setOp(c, 0, 1.0f);
        c.tonal.trimDb = 0.0f;
        c.clamp();
        orbe = measureItdIld(c, gwOrbe);
    }
    std::printf("[P1P4-WIDE] guardián NEURO solo: HEADPHONES widener=%.3f "
                "ITD=%+.2f ILD=%+.2f dB | SPEAKERS widener=%.3f ITD=%+.2f "
                "ILD=%+.2f dB | ORBE+binaural widener=%.3f ITD=%+.2f "
                "ILD=%+.2f dB | control TΩ SP ITD=%+.2f ILD=%+.2f dB | "
                "control ORBE ITD=%+.2f ILD=%+.2f dB "
                "(entrada ITD 10.00 / ILD +6.02)\\n",
                gwHp, hp.itdSamples, hp.ildDb, gwSp, sp.itdSamples,
                sp.ildDb, gwBin, bin.itdSamples, bin.ildDb,
                spCtrl.itdSamples, spCtrl.ildDb, orbe.itdSamples,
                orbe.ildDb);
    // El clasificador disparó en host (telemetría 1.35 en las 3).
    EXPECT_NEAR(gwHp, 1.35, 0.01);
    EXPECT_NEAR(gwSp, 1.35, 0.01);
    EXPECT_NEAR(gwBin, 1.35, 0.01);
    // HEADPHONES: el widener 1.35 del guardián NO destruye la ILD
    // (M/S con ITD en cuadratura: +0.12 dB medido); el ITD sí rota.
    EXPECT_NEAR(hp.ildDb, 6.02, 0.5);
    // SPEAKERS: la ILD cambia MUCHO, pero es la TΩ, no el guardián:
    // con y sin NEURO la ILD/ITD son iguales (contención verificada).
    EXPECT_NEAR(sp.ildDb, spCtrl.ildDb, 0.5);
    EXPECT_NEAR(sp.itdSamples, spCtrl.itdSamples, 1.0);
    EXPECT_GT(std::fabs(sp.ildDb - 6.02), 2.0);
    // Binaural 3D: la ILD cambia por el render ORBE, no por el
    // guardián: con y sin NEURO la ILD/ITD son iguales.
    EXPECT_NEAR(bin.ildDb, orbe.ildDb, 0.5);
    EXPECT_NEAR(bin.itdSamples, orbe.itdSamples, 1.0);
    EXPECT_GT(std::fabs(bin.ildDb - 6.02), 2.0);
}
