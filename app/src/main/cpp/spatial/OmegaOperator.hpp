// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
//
// Ω — Operador Universal: magistral en cualquier equipo, hasta la firma élite.
//
// Formalización privada: `~/workspace/analysis-ivanna/OPERADOR_UNIVERSAL_
// Formalizacion.md` (873 líneas, 15 secciones; NO va al repo público).
// Ω generaliza a Σ (Σ = Ω en un PA a volumen máximo) con dos dimensiones
// nuevas: el equipo E y la firma S (Harman 2018 + Δ insignia).
//
//   Y_b = G_b^Ω(s_b^Ω) ⊛ [ K_b^Ω(s_b^Ω) · X_b ] · Γ_b^Ω(s_b^Ω)
//
// Ω es un PRE-PROCESADOR DE PARÁMETROS de HOLOS/Σ/NEU·Ω: no tiene su
// propia STFT. Calcula, una vez por cambio de equipo o de nivel (no por
// muestra), la corrección de firma C_b por banda y la pre-compensación
// de diafonía; sus correcciones LINEALES se pliegan en Γ (HolosOperator)
// y en la matriz 2×2 por banda de NEU·Ω, sin coste extra en vivo.
// K^Ω (kernel del equipo, no lineal) queda fuera de la matriz, como el
// Volterra hoy.
//
//   S_b = Harman2018_b + Δ_b,   Δ_b = 0 (hueco #1: Δ insignia POR
//   DETERMINAR a partir de las firmas medidas; sin ella, S_b = Harman).
//   C_b = clamp(S_b − H_b, −∞, C_max,b);  C_b = 0 si f_b ∉ [f_lo, f_hi].
//   D_max(κ) = −20·log10(κ);  D_radiado = inversa de Ψ Ec. 7.
//   E_sig = RMS ponderado de ε_b;  ε_b = S_b − (H_b + C_b).
//
// Jerarquía estricta: Ω0 no degradar > Ω1 holofonía > Ω2 firma > Ω3
// nivel. Regla de oro: Ω nunca sube el nivel global.
// Fail-closed (escalera N0/N1/N2): N0 (sin info) → identidad; N1 (clase
// conocida) → defaults de clase, refuerzo máx. +3 dB; N2 (medido) →
// corrección completa hasta el headroom medido.
// omega_enabled = off ⇒ HOLOS-Σ bit-exacto (U1).
//
// Alcance v1 (444) — honesto:
//   - Núcleo completo (taxonomía E1–E8, firma, corrección, diafonía,
//     radiación/γ, jerarquía, telemetría E_sig/ε_ILD) + tests U1–U9.
//   - Γ (firma C_b) plegada en HolosOperator (cubre camino serial y NEU).
//   - G (diafonía) aplicada en la etapa ORBE (render binaural primario,
//     serial + matriz NEU). La pre-compensación en la Green de HOLOS
//     queda para una fase posterior (requiere hilar κ por el kernel
//     Green; ORBE es el render binaural dominante).
//   - H_b de clase: sin medición, se asume plana (0 dB) en N1 (hueco
//     #2: las H_b típicas por clase están POR MEDIR; el barrido §2.3
//     las dará). El tope de +3 dB de N1 limita el riesgo.
//   - Φ también persigue Harman 2018 en bucle cerrado; con Ω on y Φ
//     activo ambas correcciones componen (el tope N1 limita el
//     sobre-refuerzo). La arbitración (Ω dueño de la firma cuando está
//     on) queda para una fase posterior.
//   - K^Ω = identidad sin kernel medido (hueco #4).
//   - w_b del error de firma = 1 uniforme (hipótesis, hueco #8).
//   - σ0 = 35, p = 2 para w(σ) (hipótesis, hueco #7: rango 20–50).
//   - D_radiado máx. = 12 dB (conservador; radiar 36 dB para percibir
//     3 dB no es práctico — el teléfono, §5.2).
//
// Coste en vivo: O(1) por banda en compute() (hilo de control); en el
// hilo de audio solo lecturas de ganancias precalculadas y, si κ>0, la
// inversa de la Ec. 7 por bin en ORBE. Cero malloc, cero I/O en audio.
#pragma once

#include <algorithm>
#include <cmath>

#include "EliteTargetCurves.hpp"

namespace ivanna::spatial {

// ---------------------------------------------------------------------------
// Equipo: taxonomía E1–E8 (§2.1) y radiación (§5.4)
// ---------------------------------------------------------------------------
enum class OmegaEquipment : int {
    Auto = 0,          // por ruta Android; sin resolver → N0 (identidad)
    Headphones = 1,    // E1 audífonos / earbuds
    PhoneSpeakers = 2, // E2 bocinas del teléfono
    Bluetooth = 3,     // E3 bocina Bluetooth
    HomeStereo = 4,    // E4 estéreo de casa
    Cinema = 5,        // E5 multicanal / cine (5.1/7.1)
    Car = 6,           // E6 auto
    Pa = 7,            // E7 PA / club
    Stadium = 8,       // E8 estadio / mainstage
};

enum class OmegaRadiation : int {
    Binaural = 0,  // ORBE/HRTF (dos oídos, κ=0). Domina en E1.
    Dipolar = 1,   // reparto dipolar Ψ Ec. 8, con diafonía κ. E2–E6.
    Wfs = 2,       // ley de excitación WFS (frente de onda). E7–E8 (Σ).
};

enum class OmegaKnowledge : int {
    N0 = 0,  // Sin info: ni siquiera la clase → identidad.
    N1 = 1,  // Clase conocida: defaults de clase, refuerzo máx. +3 dB.
    N2 = 2,  // Medido: H_b, L_max, T_60 medidos → Ω completo.
};

// Defaults de clase (§2.1). κ de primeros principios (Ψ Ec. 7);
// f_lo órdenes de magnitud típicos (POR DETERMINAR por equipo concreto).
struct OmegaClassDefaults {
    float kappa;      // diafonía (fracción de mezcla del canal opuesto)
    float sigma;      // escala típica σ = D/a
    float fLoHz;      // ancho de banda útil inferior
    float fHiHz;      // ancho de banda útil superior
    OmegaRadiation radiation;
};

inline OmegaClassDefaults omegaClassDefaults(OmegaEquipment eq) noexcept {
    switch (eq) {
        case OmegaEquipment::Headphones:
            return {0.0f, 4.0f, 20.0f, 20000.0f, OmegaRadiation::Binaural};
        case OmegaEquipment::PhoneSpeakers:
            return {0.7f, 4.0f, 400.0f, 16000.0f, OmegaRadiation::Dipolar};
        case OmegaEquipment::Bluetooth:
            // 0.3–0.5 estéreo (1.0 mono). Default estéreo 0.4; mono se
            // pasa como override de κ = 1.0 en el perfil.
            return {0.4f, 7.0f, 100.0f, 18000.0f, OmegaRadiation::Dipolar};
        case OmegaEquipment::HomeStereo:
            return {0.3f, 25.0f, 50.0f, 20000.0f, OmegaRadiation::Dipolar};
        case OmegaEquipment::Cinema:
            return {0.2f, 25.0f, 40.0f, 20000.0f, OmegaRadiation::Dipolar};
        case OmegaEquipment::Car:
            return {0.35f, 7.0f, 50.0f, 18000.0f, OmegaRadiation::Dipolar};
        case OmegaEquipment::Pa:
            return {0.0f, 225.0f, 35.0f, 18000.0f, OmegaRadiation::Wfs};
        case OmegaEquipment::Stadium:
            return {0.0f, 625.0f, 30.0f, 18000.0f, OmegaRadiation::Wfs};
        case OmegaEquipment::Auto:
        default:
            // Sin clase: N0. Los valores no se usan (identidad).
            return {0.0f, 4.0f, 20.0f, 20000.0f, OmegaRadiation::Binaural};
    }
}

// ---------------------------------------------------------------------------
// Parámetros (todos fail-closed, todo off por defecto — §10.2)
// ---------------------------------------------------------------------------
struct OmegaParams {
    bool enabled = false;  // off ⇒ HOLOS-Σ bit-exacto (U1)
    OmegaEquipment equipment = OmegaEquipment::Auto;  // auto = por ruta
    EliteTargetKind signature = EliteTargetKind::Harman2018;  // S_b
    float boostMaxDb = 3.0f;   // 0..12 dB; N1 → 3 dB; N2 → headroom
    bool ildPrecomp = true;    // pre-compensación de diafonía (hasta D_max)
    bool report = true;        // expone E_sig y ε_ILD en telemetría
};

// Perfil del equipo E (§2.2). Lo que se sabe; lo que no, no se usa.
// En v1 el perfil viene de la clase (N1) o de una medición (N2, barrido
// §2.3). Sin medición, H_b se asume plana y cMaxDb no se provee.
struct OmegaProfile {
    OmegaEquipment equipment = OmegaEquipment::Auto;
    // Overrides opcionales (si no se fijan, valen los defaults de clase).
    float kappa = -1.0f;    // <0 → default de clase
    float fLoHz = -1.0f;    // <0 → default de clase
    float fHiHz = -1.0f;    // <0 → default de clase
    // H_b: respuesta del equipo por banda ERB (dB). Si hKnown=false se
    // asume plana (0 dB) — N1 conservador (hueco #2).
    float hDb[24] = {};
    bool hKnown = false;
    // C_max,b medido por banda (headroom + distorsión, §4.1). Si
    // cMaxKnown=false, el refuerzo se limita a min(boostMax, 3 dB) (N1).
    float cMaxDb[24] = {};
    bool cMaxKnown = false;
};

// ---------------------------------------------------------------------------
// Operador Ω (header-only, matemática pura, sin estado global)
// ---------------------------------------------------------------------------
class OmegaOperator {
public:
    static constexpr int kNumBands = 24;
    // Tope N1 (§8.2): sin L_max conocido, nunca se refuerza más de +3 dB.
    static constexpr float kBoostMaxN1Db = 3.0f;
    // ILD máximo radiable limpio (§5.2): radiar 36 dB para percibir 3 dB
    // (teléfono) no es práctico. Conservador; por medir por equipo.
    static constexpr float kMaxRadiatedIldDb = 12.0f;
    // Ley de escala (§7/§9): w(σ) = 1/(1+(σ0/σ)^p). Hipótesis (hueco #7:
    // σ0 20–50 por comparar binaural vs WFS a 2–4 m). Punto medio 35.
    static constexpr float kSigma0 = 35.0f;
    static constexpr float kSigmaP = 2.0f;
    // ILD de diseño de referencia para reportar ε_ILD (un ILD lateral
    // fuerte típico). El residual depende de D_obj; se reporta a 10 dB.
    static constexpr float kRefDesignIldDb = 10.0f;

    // --- Funciones estáticas puras (§5, §7) --------------------------------
    // Teorema del techo de diafonía: D_max(κ) = −20·log10(κ).
    // κ=0 → +∞ (sin techo); κ=1 → 0 dB (sin ILD posible, mono).
    static float crosstalkCeilingDb(float kappa) noexcept {
        if (!(kappa > 0.0f)) return 1e6f;  // κ=0 (o inválido): sin techo
        if (kappa >= 1.0f) return 0.0f;    // κ=1 (mono): techo 0 dB
        return -20.0f * std::log10(kappa);
    }

    // Diafonía percibida (Ψ Ec. 7, la ley que Ω invierte):
    // D'(κ) = 20·log10((ρ+κ)/(1+κ·ρ)), ρ = 10^(D/20). D con signo.
    static float perceivedIldDb(float dRadiatedDb, float kappa) noexcept {
        if (!(kappa > 0.0f) || kappa >= 1.0f) {
            // κ=0: percibido = radiado. κ=1 (mono): percibido = 0.
            return (kappa >= 1.0f) ? 0.0f : dRadiatedDb;
        }
        if (!std::isfinite(dRadiatedDb)) return 0.0f;
        const float sign = (dRadiatedDb < 0.0f) ? -1.0f : 1.0f;
        const float rho = std::pow(10.0f, std::fabs(dRadiatedDb) / 20.0f);
        const float rhoP = (rho + kappa) / (1.0f + kappa * rho);
        return sign * 20.0f * std::log10(std::max(rhoP, 1e-6f));
    }

    // Pre-compensación de diafonía (inversa de la Ec. 7, §5.2):
    // D_radiado = 20·log10((ρ_obj−κ)/(1−κ·ρ_obj)).
    // Solo existe si ρ_obj < 1/κ (D_obj < D_max). Si D_obj ≥ D_max, no hay
    // ILD radiado que lo produzca: se devuelve +∞ (el llamador lo recorta
    // al techo radiable y el residual queda en ε_ILD).
    static float radiatedIldDb(float dObjDb, float kappa) noexcept {
        if (!(kappa > 0.0f)) return dObjDb;  // κ=0: radiar = objetivo
        if (kappa >= 1.0f) return 1e6f;      // κ=1: imposible
        if (!std::isfinite(dObjDb)) return 0.0f;
        const float sign = (dObjDb < 0.0f) ? -1.0f : 1.0f;
        const float rhoObj = std::pow(10.0f, std::fabs(dObjDb) / 20.0f);
        if (!(rhoObj < 1.0f / kappa)) return 1e6f;  // ≥ techo: imposible
        const float rhoRad = (rhoObj - kappa) / (1.0f - kappa * rhoObj);
        if (!(rhoRad > 0.0f)) return 1e6f;
        return sign * 20.0f * std::log10(rhoRad);
    }

    // ILD radiado EFECTIVO: la inversa recortada al máximo radiable limpio
    // (§5.2: min(D_radiado^ideal, D_radiado^max,equipo)). Con signo.
    static float radiatedIldClippedDb(float dObjDb, float kappa,
                                      float maxRadiatedDb) noexcept {
        const float ideal = radiatedIldDb(dObjDb, kappa);
        if (!std::isfinite(ideal)) return 0.0f;
        const float cap = std::isfinite(maxRadiatedDb) ? std::fabs(maxRadiatedDb)
                                                       : kMaxRadiatedIldDb;
        const float mag = std::min(std::fabs(ideal), cap);
        return (ideal < 0.0f) ? -mag : mag;
    }

    // Residual de ILD: ε_ILD = D_obj − D_percibido ≥ 0 (lo que falta).
    static float ildResidualDb(float dObjDb, float kappa,
                               float maxRadiatedDb) noexcept {
        if (!(kappa > 0.0f)) return 0.0f;
        const float radiated = radiatedIldClippedDb(dObjDb, kappa, maxRadiatedDb);
        const float perceived = perceivedIldDb(radiated, kappa);
        return std::max(0.0f, std::fabs(dObjDb) - std::fabs(perceived));
    }

    // Ley de escala (§7/§9): γ(σ) = 1 − ½·w(σ), w(σ) = 1/(1+(σ0/σ)^p).
    // γ → 1 a escala pequeña (binaural), γ → ½ a escala gigante (WFS).
    static float scaleWeight(float sigma) noexcept {
        if (!(sigma > 0.0f) || !std::isfinite(sigma)) return 0.0f;
        const float r = kSigma0 / sigma;
        return 1.0f / (1.0f + std::pow(r, kSigmaP));
    }
    static float gammaOfSigma(float sigma) noexcept {
        return 1.0f - 0.5f * scaleWeight(sigma);
    }

    // Radiación dominante por equipo y escala (§5.4). La clase fija el
    // mecanismo; σ interpola dentro de él (Σ: γ(σ) sigue valiendo).
    static OmegaRadiation radiationFor(OmegaEquipment eq, float /*sigma*/) noexcept {
        return omegaClassDefaults(eq).radiation;
    }

    // --- Ciclo de vida ------------------------------------------------------
    void prepare(float sampleRate) noexcept {
        const float sr = (sampleRate > 0.0f) ? sampleRate : 48000.0f;
        eliteErbCenters24(sr, centersHz_);
        buildEliteTarget24(params_.signature, centersHz_, signatureDb_);
        prepared_ = true;
        compute();
    }

    void setParams(const OmegaParams& p) noexcept {
        params_ = p;
        params_.boostMaxDb = std::isfinite(p.boostMaxDb)
                                 ? std::clamp(p.boostMaxDb, 0.0f, 12.0f)
                                 : 3.0f;
        if (prepared_) {
            // La firma depende del kind; reconstruirla si cambió.
            buildEliteTarget24(params_.signature, centersHz_, signatureDb_);
        }
        compute();
    }

    void setProfile(const OmegaProfile& p) noexcept {
        profile_ = p;
        compute();
    }

    // Recalcula C_b, ganancias, E_sig y ε_ILD. Hilo de CONTROL (no audio):
    // se llama al cambiar equipo/perfil/parámetros, no por muestra.
    void compute() noexcept {
        if (!prepared_) {
            for (int b = 0; b < kNumBands; ++b) {
                corrDb_[b] = 0.0f;
                gainLin_[b] = 1.0f;
                residDb_[b] = 0.0f;
            }
            eSigDb_ = 0.0f;
            eIldDb_ = 0.0f;
            return;
        }
        const OmegaEquipment eq = resolvedEquipment();
        const OmegaClassDefaults def = omegaClassDefaults(eq);
        const float kappa = (profile_.kappa >= 0.0f) ? profile_.kappa : def.kappa;
        const float fLo = (profile_.fLoHz > 0.0f) ? profile_.fLoHz : def.fLoHz;
        const float fHi = (profile_.fHiHz > 0.0f) ? profile_.fHiHz : def.fHiHz;
        kappa_ = kappa;
        knowledge_ = knowledgeLevel();
        const bool active = params_.enabled && knowledge_ != OmegaKnowledge::N0;

        double err2 = 0.0;  // Σ w_b·ε_b², w_b = 1 uniforme (hueco #8)
        for (int b = 0; b < kNumBands; ++b) {
            const float f = centersHz_[b];
            const float h = profile_.hKnown ? profile_.hDb[b] : 0.0f;  // N1: plana
            const float s = signatureDb_[b];
            float c = 0.0f;
            if (active) {
                if (f >= fLo && f <= fHi) {
                    const float ideal = s - h;  // C_b^ideal
                    // C_max,b = min(boostMax, cMax medido, 3 dB si no hay
                    // medición). Los cortes son libres (C_min = −∞).
                    float cmax = std::min(params_.boostMaxDb, kBoostMaxN1Db);
                    if (profile_.cMaxKnown) {
                        cmax = std::min(params_.boostMaxDb, profile_.cMaxDb[b]);
                    }
                    c = std::min(ideal, cmax);
                } else {
                    c = 0.0f;  // §4.2: no se refuerza lo que no existe
                }
            }
            corrDb_[b] = c;
            gainLin_[b] = std::pow(10.0f, c / 20.0f);
            const float eps = s - (h + c);  // ε_b
            residDb_[b] = eps;
            err2 += static_cast<double>(eps) * static_cast<double>(eps);
        }
        // E_sig en dB RMS ponderado (w_b = 1): sqrt(Σ ε_b² / 24).
        eSigDb_ = static_cast<float>(std::sqrt(err2 / kNumBands));
        // ε_ILD de referencia (D_obj = 10 dB, §5.2). 0 si no hay pre-comp.
        eIldDb_ = (!active || !params_.ildPrecomp)
                      ? 0.0f
                      : ildResidualDb(kRefDesignIldDb, kappa_, kMaxRadiatedIldDb);
    }

    // --- Salidas ------------------------------------------------------------
    // Ganancia lineal por banda (10^(C_b/20)) para plegar en Γ / matriz.
    // 1.0 si Ω off o N0 (identidad, U1).
    float bandGainLin(int b) const noexcept {
        if (b < 0 || b >= kNumBands) return 1.0f;
        return gainLin_[b];
    }
    float bandCorrectionDb(int b) const noexcept {
        if (b < 0 || b >= kNumBands) return 0.0f;
        return corrDb_[b];
    }
    float bandResidualDb(int b) const noexcept {
        if (b < 0 || b >= kNumBands) return 0.0f;
        return residDb_[b];
    }
    float bandCenterHz(int b) const noexcept {
        if (b < 0 || b >= kNumBands) return 0.0f;
        return centersHz_[b];
    }
    float signatureDb(int b) const noexcept {
        if (b < 0 || b >= kNumBands) return 0.0f;
        return signatureDb_[b];
    }
    // E_sig: error total de firma en dB RMS (0 = el equipo llega a S_b).
    float signatureErrorDb() const noexcept { return eSigDb_; }
    // ε_ILD de referencia (D_obj = 10 dB): cuánto ILD le falta al equipo.
    float ildResidualDb() const noexcept { return eIldDb_; }
    float kappa() const noexcept { return kappa_; }
    OmegaKnowledge knowledge() const noexcept { return knowledge_; }
    OmegaEquipment equipment() const noexcept { return resolvedEquipment(); }
    bool isActive() const noexcept {
        return params_.enabled && knowledge_ != OmegaKnowledge::N0;
    }
    bool isEnabled() const noexcept { return params_.enabled; }
    const OmegaParams& params() const noexcept { return params_; }

    // Reducción a Σ (§10.4 / U7): Ω(E=PA, L alto, σ grande) = Σ.
    // En v1 se verifica por sus leyes: radiación WFS y γ → ½.
    bool reducesToSigma(float sigma) const noexcept {
        return resolvedEquipment() == OmegaEquipment::Pa &&
               radiationFor(resolvedEquipment(), sigma) == OmegaRadiation::Wfs &&
               std::fabs(gammaOfSigma(sigma) - 0.5f) < 0.05f;
    }

private:
    OmegaEquipment resolvedEquipment() const noexcept {
        // La selección del usuario (params) manda; Auto se resuelve por el
        // perfil, que el lado JNI fija a la clase concreta según la ruta
        // de salida de Android (o Auto si no se pudo resolver → N0).
        if (params_.equipment != OmegaEquipment::Auto) return params_.equipment;
        return profile_.equipment;
    }

    OmegaKnowledge knowledgeLevel() const noexcept {
        if (resolvedEquipment() == OmegaEquipment::Auto) return OmegaKnowledge::N0;
        if (profile_.hKnown && profile_.cMaxKnown) return OmegaKnowledge::N2;
        return OmegaKnowledge::N1;
    }

    OmegaParams params_{};
    OmegaProfile profile_{};
    bool prepared_ = false;
    float centersHz_[kNumBands] = {};
    float signatureDb_[kNumBands] = {};  // S_b
    float corrDb_[kNumBands] = {};       // C_b
    float gainLin_[kNumBands] = {};      // 10^(C_b/20)
    float residDb_[kNumBands] = {};      // ε_b
    float eSigDb_ = 0.0f;
    float eIldDb_ = 0.0f;
    float kappa_ = 0.0f;
    OmegaKnowledge knowledge_ = OmegaKnowledge::N0;
};

}  // namespace ivanna::spatial
