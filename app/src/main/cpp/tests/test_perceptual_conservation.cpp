// test_perceptual_conservation.cpp — Capa de conservación perceptual (2026-11-21).
//
// Cubre el núcleo (solver), la capa standalone y la integración en FusedChain.
// Reglas que fijan estas pruebas:
//   · El solver NUNCA crea energía (ganancia por banda <= 1, siempre).
//   · El techo de energía se cumple (ninguna banda supera energyCap).
//   · En silencio la ganancia es 1 (no se amplifica el silencio).
//   · Con amount=0 la capa NO TOCA el buffer (bit-exacto).
//   · La ganancia aplicada se mueve suave (slew limit = coherencia de fase).
//   · La cadena en neutro sigue siendo bit-exacta (la capa no rompe el
//     contrato de FusedChain).
//   · Con la capa encendida y un techo bajo, la energía de salida baja.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
#include "../fused/perceptual_conservation.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {
constexpr double kFs = 48000.0;
constexpr int kFrames = 48000;  // 1 s

double rms(const double* x, std::size_t n) {
    double s = 0.0;
    for (std::size_t i = 0; i < n; ++i) s += x[i] * x[i];
    return std::sqrt(s / static_cast<double>(n));
}

bool allFinite(const double* x, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (!std::isfinite(x[i])) return false;
    }
    return true;
}

// Propuestas neutras (todos los operadores inactivos, confianza 0).
void neutralProposals(ConservationProposal* p, int n) {
    for (int i = 0; i < n; ++i) {
        p[i].confidence = 0.0f;
        p[i].priority = 1.0f;
        for (int b = 0; b < kConservationBands; ++b) {
            p[i].energyMul[static_cast<std::size_t>(b)] = 1.0f;
            p[i].pressureMul[static_cast<std::size_t>(b)] = 1.0f;
        }
    }
}
}  // namespace

// ── Config ──────────────────────────────────────────────────────────────────
TEST(PerceptualConservation, ConfigDefaultsOffAndClamps) {
    ConservationConfig c;
    EXPECT_FLOAT_EQ(c.amount, 0.0f);       // apagado por defecto (fail-closed)
    EXPECT_TRUE(c.isNeutral());
    EXPECT_FLOAT_EQ(c.energyCap, 1.0f);
    EXPECT_FLOAT_EQ(c.phaseCoherence, 0.5f);
    EXPECT_FLOAT_EQ(c.inertia, 0.82f);

    ConservationConfig bad;
    bad.amount = 2.0f;
    bad.energyCap = -1.0f;
    bad.phaseCoherence = 5.0f;
    bad.inertia = 2.0f;
    bad.clamp();
    EXPECT_FLOAT_EQ(bad.amount, 1.0f);
    EXPECT_FLOAT_EQ(bad.energyCap, 0.0f);
    EXPECT_FLOAT_EQ(bad.phaseCoherence, 1.0f);
    EXPECT_FLOAT_EQ(bad.inertia, 0.99f);
    EXPECT_FALSE(bad.isNeutral());
}

// ── Solver (puro, sin audio) ────────────────────────────────────────────────
TEST(PerceptualConservation, SolverNeverCreatesEnergy) {
    // Propuestas que QUIEREN duplicar la energía (mul 2.0): la ganancia
    // debe seguir <= 1 (nunca amplifica = nunca crea energía).
    ConservationProposal p[PerceptualConservation::kNumOperators];
    neutralProposals(p, PerceptualConservation::kNumOperators);
    for (int i = 0; i < PerceptualConservation::kNumOperators; ++i) {
        p[i].confidence = 1.0f;
        for (int b = 0; b < kConservationBands; ++b) {
            p[i].energyMul[static_cast<std::size_t>(b)] = 2.0f;
            p[i].pressureMul[static_cast<std::size_t>(b)] = 2.0f;
        }
    }
    float measured[kConservationBands];
    for (int b = 0; b < kConservationBands; ++b) measured[b] = 0.8f;
    ConservationConfig cfg;
    cfg.amount = 1.0f;
    cfg.energyCap = 1.0f;
    cfg.clamp();
    ConservationFieldState prev{};
    float gains[kConservationBands]{};
    ConservationFieldState out{};
    ConservationSolver::solve(prev, p, PerceptualConservation::kNumOperators,
                              measured, cfg, 0.0, 0.00267, gains, &out);
    for (int b = 0; b < kConservationBands; ++b) {
        EXPECT_GE(gains[b], 0.0f);
        EXPECT_LE(gains[b], 1.0f) << "banda " << b << ": ganancia > 1 (crea energía)";
        EXPECT_TRUE(std::isfinite(gains[b]));
    }
    // Con propuestas que quieren MITAD de energía, la ganancia baja.
    for (int i = 0; i < PerceptualConservation::kNumOperators; ++i) {
        for (int b = 0; b < kConservationBands; ++b) {
            p[i].energyMul[static_cast<std::size_t>(b)] = 0.5f;
        }
    }
    ConservationSolver::solve(prev, p, PerceptualConservation::kNumOperators,
                              measured, cfg, 0.0, 0.00267, gains, &out);
    for (int b = 0; b < kConservationBands; ++b) {
        EXPECT_NEAR(gains[b], std::sqrt(0.4f / 0.8f), 1e-5f) << "banda " << b;
    }
}

TEST(PerceptualConservation, SolverEnergyCapEnforced) {
    // Medida 0.9, techo 0.5, propuestas neutras -> ganancia sqrt(0.5/0.9).
    ConservationProposal p[PerceptualConservation::kNumOperators];
    neutralProposals(p, PerceptualConservation::kNumOperators);
    float measured[kConservationBands];
    for (int b = 0; b < kConservationBands; ++b) measured[b] = 0.9f;
    ConservationConfig cfg;
    cfg.amount = 1.0f;
    cfg.energyCap = 0.5f;
    cfg.clamp();
    ConservationFieldState prev{};
    float gains[kConservationBands]{};
    ConservationFieldState out{};
    ConservationSolver::solve(prev, p, PerceptualConservation::kNumOperators,
                              measured, cfg, 0.0, 0.00267, gains, &out);
    for (int b = 0; b < kConservationBands; ++b) {
        EXPECT_NEAR(gains[b], std::sqrt(0.5f / 0.9f), 1e-5f) << "banda " << b;
    }
    // El estado de campo no supera el techo.
    for (int b = 0; b < kConservationBands; ++b) {
        EXPECT_LE(out.band[static_cast<std::size_t>(b)].energy, 0.5f + 1e-6f);
    }
}

TEST(PerceptualConservation, SolverSilenceStaysSilent) {
    ConservationProposal p[PerceptualConservation::kNumOperators];
    neutralProposals(p, PerceptualConservation::kNumOperators);
    float measured[kConservationBands]{};
    ConservationConfig cfg;
    cfg.amount = 1.0f;
    cfg.energyCap = 0.3f;
    cfg.clamp();
    ConservationFieldState prev{};
    float gains[kConservationBands]{};
    ConservationFieldState out{};
    ConservationSolver::solve(prev, p, PerceptualConservation::kNumOperators,
                              measured, cfg, 0.0, 0.00267, gains, &out);
    for (int b = 0; b < kConservationBands; ++b) {
        EXPECT_FLOAT_EQ(gains[b], 1.0f) << "banda " << b << ": no se amplifica el silencio";
        EXPECT_FLOAT_EQ(out.band[static_cast<std::size_t>(b)].energy, 0.0f);
    }
}

// ── Capa standalone ─────────────────────────────────────────────────────────
TEST(PerceptualConservation, LayerOffIsBitExact) {
    PerceptualConservation layer;
    layer.prepare(kFs);
    // Default: amount 0 (apagada).
    EXPECT_FALSE(layer.isActive());
    std::vector<double> l(kFrames), r(kFrames);
    fillToneDouble(l.data(), kFrames, kFs, 1000.0, 0.9);
    fillToneDouble(r.data(), kFrames, kFs, 1000.0, 0.9);
    const std::vector<double> inL = l, inR = r;
    layer.process(l.data(), r.data(), kFrames, 0.0);
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_DOUBLE_EQ(l[static_cast<std::size_t>(i)], inL[static_cast<std::size_t>(i)]);
        EXPECT_DOUBLE_EQ(r[static_cast<std::size_t>(i)], inR[static_cast<std::size_t>(i)]);
    }
}

TEST(PerceptualConservation, LayerOnCapsHotSignal) {
    PerceptualConservation layer;
    layer.prepare(kFs);
    ConservationConfig cfg;
    cfg.amount = 1.0f;
    cfg.energyCap = 0.3f;
    cfg.phaseCoherence = 0.5f;
    cfg.inertia = 0.82f;
    layer.setConfig(cfg);
    EXPECT_TRUE(layer.isActive());
    std::vector<double> l(kFrames), r(kFrames);
    fillToneDouble(l.data(), kFrames, kFs, 1000.0, 1.0);  // seno a fondo de escala
    fillToneDouble(r.data(), kFrames, kFs, 1000.0, 1.0);
    const double inRms = rms(l.data(), kFrames);
    double t = 0.0;
    layer.process(l.data(), r.data(), kFrames, t);
    EXPECT_TRUE(allFinite(l.data(), kFrames));
    EXPECT_TRUE(allFinite(r.data(), kFrames));
    // La ganancia aplicada bajó (la capa está limitando la energía).
    EXPECT_LT(layer.appliedGain(), 0.9f) << "appliedGain=" << layer.appliedGain();
    // El último medio segundo ya está en régimen (ganancia ~sqrt(0.3/1)=0.55).
    const double outRmsTail = rms(l.data() + kFrames / 2, kFrames / 2);
    const double ratio = outRmsTail / inRms;
    std::printf("[Conservation] LayerOnCapsHotSignal: appliedGain=%.4f ratioCola=%.4f\n",
                layer.appliedGain(), ratio);
    EXPECT_GT(ratio, 0.35) << "la capa recorta demasiado";
    EXPECT_LT(ratio, 0.75) << "la capa no está limitando la energía";
    // Telemetría: el estado de campo ve la banda de 1 kHz (índice 4).
    const ConservationFieldState fs = layer.fieldState();
    EXPECT_GT(fs.band[4].energy, 0.1f) << "el campo no mide la banda de 1 kHz";
    EXPECT_LE(fs.band[4].energy, 0.3f + 1e-6f) << "el campo supera el techo";
    EXPECT_GT(fs.band[4].pressure, 0.0f);
    // Señal idéntica L/R: sin desbalance lateral.
    EXPECT_NEAR(fs.band[4].velocity.x, 0.0f, 0.05f);
    EXPECT_FLOAT_EQ(fs.band[4].velocity.z, 0.0f);  // z no se mide en estéreo
}

TEST(PerceptualConservation, LayerGainMovesSmoothly) {
    // La ganancia no salta: el slew limit (coherencia de fase) acota el
    // cambio por bloque. phaseCoherence 0.5 -> max 2.1/s -> ~0.0056/bloque.
    PerceptualConservation layer;
    layer.prepare(kFs);
    ConservationConfig cfg;
    cfg.amount = 1.0f;
    cfg.energyCap = 0.2f;
    cfg.phaseCoherence = 0.5f;
    layer.setConfig(cfg);
    const int kBlock = 128;
    std::vector<double> l(kBlock), r(kBlock);
    double t = 0.0;
    float prevGain = 1.0f;
    float maxStep = 0.0f;
    for (int b = 0; b < 100; ++b) {
        fillToneDouble(l.data(), kBlock, kFs, 1000.0, 1.0);
        fillToneDouble(r.data(), kBlock, kFs, 1000.0, 1.0);
        layer.process(l.data(), r.data(), kBlock, t);
        t += static_cast<double>(kBlock) / kFs;
        const float g = layer.appliedGain();
        maxStep = std::max(maxStep, std::abs(g - prevGain));
        prevGain = g;
    }
    std::printf("[Conservation] LayerGainMovesSmoothly: maxStep=%.6f finalGain=%.4f\n",
                maxStep, prevGain);
    EXPECT_LE(maxStep, 0.01f) << "la ganancia salta entre bloques (clic)";
    EXPECT_LT(prevGain, 0.9f) << "la ganancia no llegó a limitar";
}

// ── Integración en FusedChain ───────────────────────────────────────────────
TEST(PerceptualConservation, ChainNeutralStillBitExact) {
    // La capa no rompe el contrato bit-exacto de la cadena en neutro.
    FusedChain chain;
    chain.prepare(kFs);
    FusedControls c;
    c.setNeutral();  // conservación amount 0 (apagada)
    chain.setControls(c);
    EXPECT_TRUE(chain.isNeutral());
    std::vector<double> l(kFrames), r(kFrames);
    fillToneDouble(l.data(), kFrames, kFs, 1000.0, 0.7071);
    fillToneDouble(r.data(), kFrames, kFs, 1000.0, 0.7071);
    const std::vector<double> inL = l, inR = r;
    chain.process(l.data(), r.data(), kFrames);
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_DOUBLE_EQ(l[static_cast<std::size_t>(i)], inL[static_cast<std::size_t>(i)]);
        EXPECT_DOUBLE_EQ(r[static_cast<std::size_t>(i)], inR[static_cast<std::size_t>(i)]);
    }
}

TEST(PerceptualConservation, ChainConservationCapsEnergy) {
    // Cadena neutra + solo la conservación (amount 1, techo 0.3): la
    // energía de salida baja. APEX apagado para medir la capa sola.
    FusedChain chain;
    chain.prepare(kFs);
    chain.setApexEnabled(false);
    FusedControls c;
    c.setNeutral();
    c.conservation.amount = 1.0f;
    c.conservation.energyCap = 0.3f;
    c.clamp();
    chain.setControls(c);
    EXPECT_FALSE(chain.isNeutral());
    std::vector<double> l(kFrames), r(kFrames);
    fillToneDouble(l.data(), kFrames, kFs, 1000.0, 1.0);
    fillToneDouble(r.data(), kFrames, kFs, 1000.0, 1.0);
    const double inRms = rms(l.data(), kFrames);
    chain.process(l.data(), r.data(), kFrames);
    EXPECT_TRUE(allFinite(l.data(), kFrames));
    EXPECT_TRUE(allFinite(r.data(), kFrames));
    const double outRmsTail = rms(l.data() + kFrames / 2, kFrames / 2);
    const double ratio = outRmsTail / inRms;
    std::printf("[Conservation] ChainConservationCapsEnergy: ratioCola=%.4f appliedGain=%.4f\n",
                ratio, chain.conservation().appliedGain());
    EXPECT_GT(ratio, 0.35) << "la cadena recorta demasiado";
    EXPECT_LT(ratio, 0.75) << "la conservación no está limitando en la cadena";
    EXPECT_LT(chain.conservation().appliedGain(), 0.9f);
}

// ── Gemelo acústico → conservación (cierre 2026-11-21) ─────────────────────
TEST(PerceptualConservation, TwinScaleZeroDisablesIntervention) {
    // Escala del gemelo en 0 (confianza cero): aunque amount=1 y la señal
    // esté caliente, la conservación NO interviene (ganancia 1.0).
    // Fail-closed: la incertidumbre del trayecto apaga la intervención.
    PerceptualConservation layer;
    layer.prepare(kFs);
    ConservationConfig cfg;
    cfg.amount = 1.0f;
    cfg.energyCap = 0.3f;
    cfg.phaseCoherence = 0.5f;
    cfg.inertia = 0.82f;
    layer.setConfig(cfg);
    layer.setTwinInterventionScale(0.0);
    EXPECT_DOUBLE_EQ(layer.twinInterventionScale(), 0.0);
    std::vector<double> l(kFrames), r(kFrames);
    fillToneDouble(l.data(), kFrames, kFs, 1000.0, 1.0);
    fillToneDouble(r.data(), kFrames, kFs, 1000.0, 1.0);
    const std::vector<double> inL = l, inR = r;
    layer.process(l.data(), r.data(), kFrames, 0.0);
    EXPECT_FLOAT_EQ(layer.appliedGain(), 1.0f);
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_DOUBLE_EQ(l[static_cast<std::size_t>(i)], inL[static_cast<std::size_t>(i)]);
        EXPECT_DOUBLE_EQ(r[static_cast<std::size_t>(i)], inR[static_cast<std::size_t>(i)]);
    }
}

TEST(PerceptualConservation, TwinScaleHalfIntervenesLess) {
    // Escala 0.5: la conservación interviene menos que con escala 1.0
    // (la ganancia aplicada es mayor, más cerca de 1.0).
    auto runWithScale = [](double scale) {
        PerceptualConservation layer;
        layer.prepare(kFs);
        ConservationConfig cfg;
        cfg.amount = 1.0f;
        cfg.energyCap = 0.3f;
        cfg.phaseCoherence = 0.5f;
        cfg.inertia = 0.82f;
        layer.setConfig(cfg);
        layer.setTwinInterventionScale(scale);
        std::vector<double> l(kFrames), r(kFrames);
        fillToneDouble(l.data(), kFrames, kFs, 1000.0, 1.0);
        fillToneDouble(r.data(), kFrames, kFs, 1000.0, 1.0);
        double t = 0.0;
        for (int b = 0; b < 20; ++b) {
            fillToneDouble(l.data(), kFrames, kFs, 1000.0, 1.0);
            fillToneDouble(r.data(), kFrames, kFs, 1000.0, 1.0);
            layer.process(l.data(), r.data(), kFrames, t);
            t += static_cast<double>(kFrames) / kFs;
        }
        return layer.appliedGain();
    };
    const float gainFull = runWithScale(1.0);
    const float gainHalf = runWithScale(0.5);
    std::printf("[Conservation] TwinScale: gainFull=%.4f gainHalf=%.4f\n",
                gainFull, gainHalf);
    EXPECT_LT(gainFull, 0.9f) << "con escala 1.0 la capa debería limitar";
    EXPECT_GT(gainHalf, gainFull) << "con escala 0.5 debería intervenir menos";
    EXPECT_LE(gainHalf, 1.0f);
}

TEST(PerceptualConservation, TwinInvalidLeavesScaleAtOne) {
    // Gemelo inválido -> escala 1.0 (la conservación se comporta como
    // antes). Fail-closed: un gemelo roto no la apaga ni la desvía.
    using namespace ivanna::fused::acoustic_twin;
    PerceptualConservation layer;
    layer.prepare(kFs);
    layer.setTwinInterventionScale(0.3);  // valor previo cualquiera
    AcousticTwinEstimate twin;
    twin.valid = false;
    applyTwinToConservation(layer, twin);
    EXPECT_DOUBLE_EQ(layer.twinInterventionScale(), 1.0);
    // Y con el gemelo real de Esse (válido) la escala es su confianza.
    const AcousticTwinEstimate esse =
        AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
    ASSERT_TRUE(esse.valid);
    applyTwinToConservation(layer, esse);
    // La escala se guarda como float (atomic<float>): comparar con
    // precisión float, no double exacto.
    EXPECT_FLOAT_EQ(static_cast<float>(layer.twinInterventionScale()),
                    static_cast<float>(esse.overallInterventionScale));
    EXPECT_GT(layer.twinInterventionScale(), 0.0);
    EXPECT_LE(layer.twinInterventionScale(), 1.0);
}

TEST(PerceptualConservation, TwinScaleClampedAndAmountZeroStillBypass) {
    // La escala se clampa a [0,1] (NaN -> 1.0) y con amount=0 el bypass
    // bit-exacto se mantiene cualquiera que sea la escala del gemelo.
    PerceptualConservation layer;
    layer.prepare(kFs);
    layer.setTwinInterventionScale(5.0);
    EXPECT_DOUBLE_EQ(layer.twinInterventionScale(), 1.0);
    layer.setTwinInterventionScale(-1.0);
    EXPECT_DOUBLE_EQ(layer.twinInterventionScale(), 0.0);
    layer.setTwinInterventionScale(std::numeric_limits<double>::quiet_NaN());
    EXPECT_DOUBLE_EQ(layer.twinInterventionScale(), 1.0);
    // amount=0 + escala 0.5 -> bypass bit-exacto.
    ConservationConfig cfg;
    cfg.amount = 0.0f;
    layer.setConfig(cfg);
    layer.setTwinInterventionScale(0.5);
    std::vector<double> l(kFrames), r(kFrames);
    fillToneDouble(l.data(), kFrames, kFs, 1000.0, 0.9);
    fillToneDouble(r.data(), kFrames, kFs, 1000.0, 0.9);
    const std::vector<double> inL = l, inR = r;
    layer.process(l.data(), r.data(), kFrames, 0.0);
    for (int i = 0; i < kFrames; ++i) {
        EXPECT_DOUBLE_EQ(l[static_cast<std::size_t>(i)], inL[static_cast<std::size_t>(i)]);
        EXPECT_DOUBLE_EQ(r[static_cast<std::size_t>(i)], inR[static_cast<std::size_t>(i)]);
    }
}
