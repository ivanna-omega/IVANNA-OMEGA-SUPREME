// ─────────────────────────────────────────────────────────────────────────────
// new_physics_controls.hpp — Controles unificados de FÍSICA NUEVA (Fase 2,
// v2.5.472). Paquete FISICA_NUEVA_CONSOLIDADA (Esse, 2026-11-18): LGN 3D +
// Ω·Σ + Volterra Laguerre + lazo F heurístico [P1+P4 2026-11-26: antes
// "lazo variacional F" — no hay funcional F ni ∂F/∂Γ]. Header-only, RT-safe.
// Estado por defecto OFF (amount = 0 → bypass bit-exacto, fail-closed).
// Adaptación de integración: ninguna de nombres — el paquete ya empataba con
// el repo (namespace ivanna::newphys propio, sin colisiones).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ivanna::newphys {

enum class EquipmentClass : int32_t {
    HEADPHONES_E1     = 0, // κ = 0.00, f_lo = 20 Hz
    PHONE_SPEAKER_E2  = 1, // κ = 0.70, f_lo = 350 Hz
    BT_SPEAKER_E3     = 2, // κ = 0.45, f_lo = 90 Hz
    HOME_STEREO_E4    = 3, // κ = 0.30, f_lo = 50 Hz (Sony Genezi MHC-PZ1D)
    CAR_E6            = 4, // κ = 0.35, f_lo = 50 Hz
    PA_CLUB_E7        = 5, // WFS, f_lo = 35 Hz
    STADIUM_E8        = 6  // WFS + Aire, f_lo = 28 Hz
};

struct NewPhysicsControls {
    // Master fail-closed (0.0f = bypass bit-exacto)
    float amount = 0.0f;                 // [0..1]

    // Pilar 1: LGN 3D + Esfera + Torso + Antropometría (foto+tarjeta)
    float lgnPinnaMix      = 1.0f;       // [0..1]
    float brightSpotDamp   = 0.65f;      // [0..1]
    float torsoEchoMix     = 0.18f;      // [0..0.5]
    float headRadiusM      = 0.0875f;    // [0.065..0.115] m (KEMAR=0.0875)
    float azimuthDeg       = 0.0f;       // [-180..180] (o auto desde estimador)
    float elevationDeg     = 0.0f;       // [-40..70]

    // Pilar 2: Ω (Equipo + Firma Harman2018 + Diafonía) + Σ (Escala + Alto SPL)
    EquipmentClass eqClass = EquipmentClass::HEADPHONES_E1;
    float omegaSignatureMix = 0.0f;      // [0..1] corrección hacia Harman2018+Δ_b
    float boostCapDb        = 3.0f;      // [0..9] dB (3 dB conservador en N1)
    float crosstalkCompMix  = 0.0f;      // [0..1] pre-compensación Ec.7 acotada
    float sigmaDistanceM    = 0.35f;     // [0.15..80.0] m -> σ = D / a
    float splEstimateDb     = 85.0f;     // [65..115] dB SPL (para α(L) e ISO226)
    float airCompMix        = 0.0f;      // [0..1] ISO 9613-1

    // Pilar 3: Volterra Laguerre H2 (Sony Genezi 68.5 Hz / 60-450 Hz)
    float volterraMix       = 0.0f;      // [0..1] 0 = identidad exacta
    bool  hasMeasuredKernel = false;     // fail-closed si no se ha calibrado

    // Pilar 4: Lazo F heurístico (MOC + Anti-Adaptación Sináptica)
    // [P1+P4 2026-11-26: antes "Lazo Variacional F"]
    float freeEnergyMix     = 0.0f;      // [0..1]

    // Modo de prueba ciega ABX integrado (0=Control normal, 1=Forzar A, 2=Forzar B)
    int32_t abxOverrideMode = 0;

    void clamp() noexcept {
        auto cl = [](float v, float lo, float hi, float def) noexcept {
            return std::isfinite(v) ? std::clamp(v, lo, hi) : def;
        };
        amount            = cl(amount,            0.0f,   1.5f,   0.0f);
        lgnPinnaMix       = cl(lgnPinnaMix,       0.0f,   1.0f,   1.0f);
        brightSpotDamp    = cl(brightSpotDamp,    0.0f,   1.0f,   0.65f);
        torsoEchoMix      = cl(torsoEchoMix,      0.0f,   0.5f,   0.18f);
        headRadiusM       = cl(headRadiusM,       0.065f, 0.115f, 0.0875f);
        azimuthDeg        = cl(azimuthDeg,       -180.0f, 180.0f, 0.0f);
        elevationDeg      = cl(elevationDeg,     -40.0f,  70.0f,  0.0f);
        omegaSignatureMix = cl(omegaSignatureMix, 0.0f,   1.0f,   0.0f);
        boostCapDb        = cl(boostCapDb,        0.0f,   9.0f,   3.0f);
        crosstalkCompMix  = cl(crosstalkCompMix,  0.0f,   1.5f,   0.0f);
        sigmaDistanceM    = cl(sigmaDistanceM,    0.15f,  80.0f,  0.35f);
        splEstimateDb     = cl(splEstimateDb,     65.0f,  115.0f, 85.0f);
        airCompMix        = cl(airCompMix,        0.0f,   1.0f,   0.0f);
        volterraMix       = cl(volterraMix,       0.0f,   1.0f,   0.0f);
        freeEnergyMix     = cl(freeEnergyMix,     0.0f,   1.0f,   0.0f);
        int eq = static_cast<int>(eqClass);
        if (eq < 0 || eq > 6) eqClass = EquipmentClass::HEADPHONES_E1;
        if (abxOverrideMode < 0 || abxOverrideMode > 2) abxOverrideMode = 0;
    }

    bool isBypass() const noexcept {
        return amount <= 1e-6f;
    }
};

struct NewPhysicsTelemetry {
    float signatureErrorRmsDb = 0.0f; // E_sig residual tras clamp C_max,b
    float crosstalkCeilingDb  = 99.0f;// D_max(κ) = -20*log10(κ)
    float scaleGamma          = 1.0f; // γ(σ) ∈ [0.5, 1.0]
    float ildAlphaCompression = 1.0f; // α(L) coclear
    float mocFatigueIntegral  = 0.0f; // ∫ e_b dt (predicción de fatiga)
    float volterraPeakCorr    = 0.0f; // magnitud de corrección H2 activa
    float cpuUsPerHop         = 0.0f; // costo medido en tiempo real
    int32_t kernelLoaded      = 0;    // 1 si GENEZY01 / calibrado activo
};

} // namespace ivanna::newphys
