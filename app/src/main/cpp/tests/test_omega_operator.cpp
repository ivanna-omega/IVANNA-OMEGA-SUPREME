// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
//
// Tests host U1–U9 del operador universal Ω (v2.5.444).
// Formalización privada: ~/workspace/analysis-ivanna/OPERADOR_UNIVERSAL_
// Formalizacion.md §11.1. Matemática pura, sin audio real.
// Criterios (del documento):
//   U1 Identidad: Ω off ⇒ salida = HOLOS-Σ bit-exacto (ganancias 1.0).
//   U2 Firma: H_b sintética + headroom ⇒ C_b lleva la salida a S_b
//      dentro de ±0.5 dB por banda y E_sig ≈ 0.
//   U3 Firma recortada: sin headroom ⇒ C_b se recorta a C_max,b, el
//      residual ε_b es exactamente C_b^ideal − C_max,b, sin saturar (Ω0).
//   U4 Ancho de banda: banda fuera de [f_lo, f_hi] ⇒ C_b = 0.
//   U5 Techo de diafonía: κ=0.7 ⇒ ILD percibido ≤ D_max ≈ 3.1 dB aunque
//      el radiado tienda a ∞; κ=0 ⇒ percibido = diseño exacto.
//   U6 Pre-compensación: inversa de la Ec. 7 ⇒ D_percibido = D_obj.
//   U7 Reducción a Σ: E=PA, L alto, σ grande ⇒ Ω reproduce Σ (WFS,
//      γ → ½) dentro de tolerancia.
//   U8 No degradar: con Ω on, la corrección nunca supera el límite de
//      distorsión/headroom (proxy host de THD+N: el refuerzo está
//      acotado por C_max, así que no puede empujar al equipo a
//      distorsionar ni saturar).
//   U9 Jerarquía: en conflicto (firma que exige refuerzo que
//      distorsionaría), Ω recorta la firma y preserva Ω0/Ω1.
#include <cmath>

#include "gtest/gtest.h"
#include "spatial/OmegaOperator.hpp"

namespace ivanna::spatial {
namespace {

constexpr float kTolDb = 0.05f;  // tolerancia de redondeo en dB

// Perfil sintético: H_b conocida + C_max conocido (headroom/distorsión).
OmegaProfile syntheticProfile(OmegaEquipment eq, float hBase, float cMax,
                              bool hKnown = true, bool cMaxKnown = true) {
    OmegaProfile p;
    p.equipment = eq;
    p.hKnown = hKnown;
    p.cMaxKnown = cMaxKnown;
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        // H_b sintética con forma (no plana): verifica la corrección real.
        p.hDb[b] = hBase + 2.0f * std::cos(static_cast<float>(b) * 0.5f);
        p.cMaxDb[b] = cMax;
    }
    return p;
}

OmegaParams enabledParams(OmegaEquipment eq, float boostMaxDb = 12.0f) {
    OmegaParams p;
    p.enabled = true;
    p.equipment = eq;
    p.boostMaxDb = boostMaxDb;
    p.ildPrecomp = true;
    p.report = true;
    return p;
}

// U1 — Identidad: Ω off ⇒ ganancias 1.0, corrección 0, inactivo.
TEST(OmegaOperator, U1IdentityWhenOff) {
    OmegaOperator op;
    op.prepare(48000.0f);
    op.setProfile(syntheticProfile(OmegaEquipment::Headphones, 0.0f, 12.0f));
    op.setParams(OmegaParams{});  // enabled = false (default)
    EXPECT_FALSE(op.isActive());
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        EXPECT_FLOAT_EQ(op.bandGainLin(b), 1.0f);
        EXPECT_FLOAT_EQ(op.bandCorrectionDb(b), 0.0f);
    }
    // Ω on pero equipo Auto sin resolver (N0) ⇒ también identidad.
    OmegaParams p = enabledParams(OmegaEquipment::Auto);
    OmegaOperator op2;
    op2.prepare(48000.0f);
    op2.setParams(p);  // equipment Auto, perfil por defecto Auto → N0
    EXPECT_EQ(op2.knowledge(), OmegaKnowledge::N0);
    EXPECT_FALSE(op2.isActive());
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        EXPECT_FLOAT_EQ(op2.bandGainLin(b), 1.0f);
        EXPECT_FLOAT_EQ(op2.bandCorrectionDb(b), 0.0f);
    }
}

// U2 — Firma: con H_b sintética y headroom, C_b lleva la salida a S_b
// dentro de ±0.5 dB por banda y E_sig ≈ 0.
TEST(OmegaOperator, U2SignatureReachesTarget) {
    OmegaOperator op;
    op.prepare(48000.0f);
    // Audífonos (κ=0, f_lo=20 Hz): las 24 bandas están en rango.
    // cMax = 12 dB: headroom suficiente (el ideal máx. es ~11.6 dB).
    op.setProfile(syntheticProfile(OmegaEquipment::Headphones, 0.0f, 12.0f));
    op.setParams(enabledParams(OmegaEquipment::Headphones, 12.0f));
    EXPECT_TRUE(op.isActive());
    EXPECT_EQ(op.knowledge(), OmegaKnowledge::N2);
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        // La H_b sintética que el perfil guardó (reconstruirla aquí).
        const float h = 0.0f + 2.0f * std::cos(static_cast<float>(b) * 0.5f);
        const float out = h + op.bandCorrectionDb(b);
        EXPECT_NEAR(out, op.signatureDb(b), 0.5f)
            << "banda " << b << " (" << op.bandCenterHz(b) << " Hz)";
        EXPECT_NEAR(op.bandResidualDb(b), 0.0f, 0.5f);
    }
    EXPECT_LT(op.signatureErrorDb(), 0.5f);
}

// U3 — Firma recortada: sin headroom, C_b se recorta a C_max,b, el
// residual es exactamente C_b^ideal − C_max,b, y no satura (Ω0).
TEST(OmegaOperator, U3SignatureClippedByHeadroom) {
    OmegaOperator op;
    op.prepare(48000.0f);
    // cMax = 1 dB: headroom insuficiente (el ideal llega a ~+8.6 dB).
    op.setProfile(syntheticProfile(OmegaEquipment::Headphones, 0.0f, 1.0f));
    op.setParams(enabledParams(OmegaEquipment::Headphones, 12.0f));
    bool sawClip = false;
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        const float h = 0.0f + 2.0f * std::cos(static_cast<float>(b) * 0.5f);
        const float ideal = op.signatureDb(b) - h;
        const float c = op.bandCorrectionDb(b);
        // Nunca supera C_max = 1 dB (Ω0: no satura).
        EXPECT_LE(c, 1.0f + 1e-6f);
        // La ganancia lineal está acotada por el headroom.
        EXPECT_LE(op.bandGainLin(b), std::pow(10.0f, 1.0f / 20.0f) + 1e-6f);
        if (ideal > 1.0f) {
            sawClip = true;
            EXPECT_NEAR(c, 1.0f, 1e-5f);
            // Residual exactamente ideal − max.
            EXPECT_NEAR(op.bandResidualDb(b), ideal - 1.0f, 1e-4f);
        }
    }
    EXPECT_TRUE(sawClip);  // el test realmente recortó alguna banda
    EXPECT_GT(op.signatureErrorDb(), 0.0f);  // hay error residual honesto
}

// U4 — Ancho de banda: banda fuera de [f_lo, f_hi] ⇒ C_b = 0.
TEST(OmegaOperator, U4BandwidthRule) {
    OmegaOperator op;
    op.prepare(48000.0f);
    // Bocinas del teléfono (E2, f_lo ≈ 400 Hz): sin graves.
    op.setProfile(syntheticProfile(OmegaEquipment::PhoneSpeakers, 0.0f, 12.0f));
    op.setParams(enabledParams(OmegaEquipment::PhoneSpeakers, 12.0f));
    bool sawBelow = false, sawAbove = false;
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        const float f = op.bandCenterHz(b);
        if (f < 400.0f) {
            sawBelow = true;
            EXPECT_FLOAT_EQ(op.bandCorrectionDb(b), 0.0f)
                << "banda " << b << " a " << f << " Hz debe ser 0";
            EXPECT_FLOAT_EQ(op.bandGainLin(b), 1.0f);
        } else {
            sawAbove = true;
        }
    }
    EXPECT_TRUE(sawBelow);
    EXPECT_TRUE(sawAbove);
}

// U5 — Techo de diafonía: κ=0.7 ⇒ percibido ≤ ~3.1 dB aunque el radiado
// tienda a ∞; κ=0 ⇒ percibido = diseño exacto. Tabla §5.1.
TEST(OmegaOperator, U5CrosstalkCeiling) {
    // Tabla del documento (§5.1).
    EXPECT_NEAR(OmegaOperator::crosstalkCeilingDb(0.2f), 14.0f, 0.1f);
    EXPECT_NEAR(OmegaOperator::crosstalkCeilingDb(0.3f), 10.5f, 0.1f);
    EXPECT_NEAR(OmegaOperator::crosstalkCeilingDb(0.5f), 6.0f, 0.1f);
    EXPECT_NEAR(OmegaOperator::crosstalkCeilingDb(0.7f), 3.1f, 0.1f);
    EXPECT_FLOAT_EQ(OmegaOperator::crosstalkCeilingDb(1.0f), 0.0f);
    EXPECT_GT(OmegaOperator::crosstalkCeilingDb(0.0f), 1e5f);  // sin techo
    // Radiado → ∞ con κ=0.7: el percibido se queda en el techo.
    const float perc = OmegaOperator::perceivedIldDb(100.0f, 0.7f);
    EXPECT_NEAR(perc, 3.1f, 0.05f);
    EXPECT_LE(perc, OmegaOperator::crosstalkCeilingDb(0.7f) + 1e-4f);
    // κ=0: percibido = diseño exacto.
    EXPECT_FLOAT_EQ(OmegaOperator::perceivedIldDb(10.0f, 0.0f), 10.0f);
    EXPECT_FLOAT_EQ(OmegaOperator::perceivedIldDb(-7.5f, 0.0f), -7.5f);
    // κ=1 (mono): percibido siempre 0.
    EXPECT_FLOAT_EQ(OmegaOperator::perceivedIldDb(20.0f, 1.0f), 0.0f);
}

// U6 — Pre-compensación: la inversa de la Ec. 7 devuelve D_percibido =
// D_obj exacto (por debajo del techo).
TEST(OmegaOperator, U6CrosstalkPrecompensationRoundTrip) {
    const float kappas[] = {0.2f, 0.3f, 0.5f, 0.7f};
    const float dobjs[] = {1.0f, 3.0f, 5.0f, -4.0f};
    for (float k : kappas) {
        const float dmax = OmegaOperator::crosstalkCeilingDb(k);
        for (float dObj : dobjs) {
            if (std::fabs(dObj) >= dmax - 0.5f) continue;  // bajo el techo
            const float radiated = OmegaOperator::radiatedIldDb(dObj, k);
            EXPECT_GT(std::fabs(radiated), std::fabs(dObj))
                << "κ=" << k << " D_obj=" << dObj << ": debe expandir";
            const float perceived = OmegaOperator::perceivedIldDb(radiated, k);
            EXPECT_NEAR(perceived, dObj, 0.01f)
                << "κ=" << k << " D_obj=" << dObj;
        }
    }
    // Por encima del techo no hay inversa: la función lo marca.
    EXPECT_GT(OmegaOperator::radiatedIldDb(10.0f, 0.7f), 1e5f);
    // La versión recortada nunca supera el máximo radiable y el residual
    // queda como ε_ILD (no se finge).
    const float clipped = OmegaOperator::radiatedIldClippedDb(
        3.0f, 0.7f, OmegaOperator::kMaxRadiatedIldDb);
    EXPECT_LE(std::fabs(clipped), OmegaOperator::kMaxRadiatedIldDb + 1e-6f);
    EXPECT_GT(OmegaOperator::ildResidualDb(10.0f, 0.7f,
                                           OmegaOperator::kMaxRadiatedIldDb),
              0.0f);
}

// U7 — Reducción a Σ: E=PA, σ grande ⇒ WFS y γ → ½.
TEST(OmegaOperator, U7ReducesToSigma) {
    EXPECT_EQ(OmegaOperator::radiationFor(OmegaEquipment::Pa, 500.0f),
              OmegaRadiation::Wfs);
    EXPECT_EQ(OmegaOperator::radiationFor(OmegaEquipment::Stadium, 800.0f),
              OmegaRadiation::Wfs);
    EXPECT_EQ(OmegaOperator::radiationFor(OmegaEquipment::Headphones, 4.0f),
              OmegaRadiation::Binaural);
    EXPECT_EQ(OmegaOperator::radiationFor(OmegaEquipment::HomeStereo, 25.0f),
              OmegaRadiation::Dipolar);
    // γ → 1 a escala pequeña, γ → ½ a escala gigante.
    EXPECT_NEAR(OmegaOperator::gammaOfSigma(4.0f), 1.0f, 0.02f);
    EXPECT_NEAR(OmegaOperator::gammaOfSigma(500.0f), 0.5f, 0.05f);
    EXPECT_NEAR(OmegaOperator::gammaOfSigma(900.0f), 0.5f, 0.02f);
    // El operador lo reconoce como Σ.
    OmegaOperator op;
    op.prepare(48000.0f);
    OmegaParams p = enabledParams(OmegaEquipment::Pa, 3.0f);
    op.setParams(p);
    EXPECT_TRUE(op.reducesToSigma(500.0f));
    EXPECT_FALSE(op.reducesToSigma(4.0f));
}

// U8 — No degradar: con Ω on, la corrección nunca supera el límite de
// distorsión/headroom (C_max) ni satura; el pico de ganancia está acotado.
TEST(OmegaOperator, U8NoDegrade) {
    OmegaOperator op;
    op.prepare(48000.0f);
    // Límite de distorsión estrecho: C_max = 2 dB en todas las bandas.
    op.setProfile(syntheticProfile(OmegaEquipment::HomeStereo, 0.0f, 2.0f));
    op.setParams(enabledParams(OmegaEquipment::HomeStereo, 12.0f));
    float maxGain = 0.0f;
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        EXPECT_LE(op.bandCorrectionDb(b), 2.0f + 1e-6f)
            << "banda " << b << ": la corrección supera el límite";
        EXPECT_LE(op.bandGainLin(b),
                  std::pow(10.0f, 2.0f / 20.0f) + 1e-6f);
        maxGain = std::max(maxGain, op.bandGainLin(b));
    }
    // El pico de ganancia con Ω on no supera el límite (no satura).
    EXPECT_LE(maxGain, std::pow(10.0f, 2.0f / 20.0f) + 1e-6f);
    // Apagar Ω devuelve la identidad (el equipo sin Ω no empeora).
    OmegaOperator off;
    off.prepare(48000.0f);
    off.setProfile(syntheticProfile(OmegaEquipment::HomeStereo, 0.0f, 2.0f));
    off.setParams(OmegaParams{});
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        EXPECT_FLOAT_EQ(off.bandGainLin(b), 1.0f);
    }
}

// U9 — Jerarquía: en conflicto (la firma exige +10 dB pero distorsionaría
// a +1 dB), Ω recorta la firma y preserva Ω0/Ω1 (la firma cede).
TEST(OmegaOperator, U9HierarchySignatureYields) {
    OmegaOperator op;
    op.prepare(48000.0f);
    // H_b muy por debajo de la firma (ideal ~+10 dB) pero C_max = 1 dB.
    OmegaProfile prof;
    prof.equipment = OmegaEquipment::HomeStereo;
    prof.hKnown = true;
    prof.cMaxKnown = true;
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        prof.hDb[b] = -10.0f;    // equipo 10 dB por debajo de plano
        prof.cMaxDb[b] = 1.0f;   // distorsionaría por encima de +1 dB
    }
    op.setProfile(prof);
    op.setParams(enabledParams(OmegaEquipment::HomeStereo, 12.0f));
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        const float ideal = op.signatureDb(b) - (-10.0f);
        // La firma pedía `ideal` (hasta ~+18 dB); Ω aplica solo 1 dB.
        EXPECT_NEAR(op.bandCorrectionDb(b), 1.0f, 1e-5f)
            << "banda " << b << ": Ω0 debe recortar la firma a C_max";
        EXPECT_GT(ideal, 1.0f);
        // El residual es la firma que cede (no se esconde).
        EXPECT_NEAR(op.bandResidualDb(b), ideal - 1.0f, 1e-4f);
    }
    // Ω1 intacto: el techo de diafonía sigue valiendo con κ=0.3.
    EXPECT_NEAR(OmegaOperator::crosstalkCeilingDb(op.kappa()), 10.5f, 0.1f);
    EXPECT_GT(op.signatureErrorDb(), 0.0f);  // el error se reporta
}

// N1: clase conocida sin medición ⇒ refuerzo máx. +3 dB (aunque el usuario
// pida más) y κ de clase.
TEST(OmegaOperator, N1BoostCapWithoutMeasurement) {
    OmegaOperator op;
    op.prepare(48000.0f);
    OmegaProfile prof;
    prof.equipment = OmegaEquipment::PhoneSpeakers;  // clase, sin medir
    op.setProfile(prof);
    op.setParams(enabledParams(OmegaEquipment::PhoneSpeakers, 12.0f));
    EXPECT_EQ(op.knowledge(), OmegaKnowledge::N1);
    EXPECT_NEAR(op.kappa(), 0.7f, 1e-6f);
    for (int b = 0; b < OmegaOperator::kNumBands; ++b) {
        EXPECT_LE(op.bandCorrectionDb(b), 3.0f + 1e-6f)
            << "banda " << b << ": N1 nunca refuerza más de +3 dB";
    }
}

}  // namespace
}  // namespace ivanna::spatial
