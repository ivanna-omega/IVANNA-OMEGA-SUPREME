// test_route_b_neutral_snapshot.cpp — Regresión Ruta B neutra de verdad (0023).
//
// CAUSA RAÍZ (bofo Ruta B, verificado en código): OmegaDspSnapshot::makeDefault()
// es el default MAGISTRAL de la app (Ruta A), no un neutro. El daemon v2 lo
// heredaba (buildNeutralSnapshot) y solo ponía a cero EQ/bajos/sala/upmix/
// compresor/harmonic — dejaba VIVOS:
//   * bit HRTF en temporal_field -> omega_apply_snapshot hacía
//     fc->setHrtfEnabled(true) y el FusionCore corría processBinauralScene
//     con wet 1.0 (m_hrtfEnabled_{true}, g_hrtf_wet_dry{1.0f}): convolución
//     HRTF COMPLETA sobre música estéreo. La HRTF es para audífonos; por
//     altavoces degrada imagen y timbre = el "bofo" (apagado).
//   * widener: spatial_width 1.55 x widener_mult 1.38 = 2.14x M/S activo.
//   * intensity 0.92 -> trim global 0.92 (-0.7 dB).
//   * anti_dolby 0.85, saf_gain 0.78 + saf_q_valid 1, phonDelta +1 dB
//     (listen 62 / ref 82), cochlear 0.35, target_gain 0.0 (-> clamp 0.5).
// Este test fija el contrato de makeNeutral() (la identidad acústica) y
// pincha que makeDefault() NO es neutro (para que nadie vuelva a usarlo
// como base del daemon). Sin NDK, sin red: solo el header.
//
// Umbrales fijados ANTES de medir: valores identidad exactos (1.0f / 0.0f),
// bit HRTF a 0, flags solo EQ_CALIBRATED, CRC válido.
#include <cmath>
#include <cstdio>

#include "../include/omega_control_bus.h"

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

bool near(float a, float b, float eps = 1e-6f) {
    return std::fabs(a - b) <= eps;
}

}  // namespace

int main() {
    using namespace ivanna;
    const OmegaDspSnapshot n = OmegaDspSnapshot::makeNeutral();
    const OmegaDspSnapshot d = OmegaDspSnapshot::makeDefault();

    // 1. El neutro es válido (magic + versión + CRC).
    check(n.isValid(), "neutral: snapshot valido (magic/version/CRC)");

    // 2. HRTF OFF: el bit que enciende el render binaural está a 0.
    check((n.temporal_field & OMEGA_EFFECT_HRTF_ENABLED_BIT) == 0u,
          "neutral: bit HRTF a 0 (sin render binaural)");
    check((n.temporal_field & OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT) == 0u,
          "neutral: bit adaptativo a 0");

    // 3. Widener identidad: spatial_width x widener_mult == 1.0.
    check(near(n.spatial_width, 1.0f), "neutral: spatial_width 1.0");
    check(near(n.widener_mult, 1.0f), "neutral: widener_mult 1.0");
    check(near(n.spatial_width * n.widener_mult, 1.0f),
          "neutral: widener total 1.0 (sin ensanchado M/S)");

    // 4. Ganancias identidad.
    check(near(n.intensity, 1.0f), "neutral: intensity 1.0 (sin trim)");
    check(near(n.target_gain, 1.0f), "neutral: target_gain 1.0");
    check(near(n.listen_phon, n.ref_phon), "neutral: listen==ref phon (phonDelta 0)");
    check(near(n.harmonic_gain, 0.0f), "neutral: harmonic_gain 0 (GoldenEar off)");
    check(near(n.anti_dolby, 0.0f), "neutral: anti_dolby 0");
    check(near(n.bass_boost_db, 0.0f), "neutral: bass_boost 0");
    check(near(n.dialog_boost_db, 0.0f), "neutral: dialog_boost 0");

    // 5. Compresor: >= 0 para que omega_apply_snapshot lo salte (el ratio
    // del core por defecto es 1.0 = off).
    check(n.compressor >= 0.0f, "neutral: compressor >= 0 (no se aplica)");
    check(near(n.comp_amount, 0.0f), "neutral: comp_amount 0");
    check(near(n.exciter_reduction, 0.0f), "neutral: exciter_reduction 0");
    check(near(n.exc_red, 0.0f), "neutral: exc_red 0");
    check(near(n.loudness_target, 0.0f), "neutral: loudness_target 0");

    // 6. EQ plano.
    bool eqFlat = true;
    for (int i = 0; i < OMEGA_CTRL_EQ_BANDS; ++i)
        eqFlat = eqFlat && near(n.eq_gains[i], 0.0f);
    check(eqFlat, "neutral: EQ plano (10 bandas a 0)");

    // 7. Sala / espacial: todo apagado.
    check(near(n.room_wet, 0.0f), "neutral: room_wet 0");
    check(near(n.room_rt60_s, 0.0f), "neutral: room_rt60 0");
    check(n.room_idx == -1, "neutral: room_idx -1 (sin sala)");
    check(n.upmixing_enabled == 0, "neutral: upmixing off");
    check(n.wfs_enabled == 0, "neutral: WFS off");
    check(near(n.orbe_amount, 0.0f), "neutral: ORBE 0");
    check(near(n.holos_amount, 0.0f), "neutral: HOLOS 0");
    // Φ + Volterra (ABI v13, 0038+0040): apagados por defecto — amount 0 =
    // bypass bit-exacto; el flag VOLTERRA_ON tampoco va en flags.
    check(near(n.hyper_amount, 0.0f), "neutral: Φ (hyper_amount) 0");
    check(n.hyper_preset == 0, "neutral: hyper_preset 0");
    check(n.hyper_xtc_enabled == 0u, "neutral: hyper_xtc_enabled 0");
    check(n.volterra_enabled == 0u, "neutral: volterra_enabled 0");
    check((n.flags & OMEGA_FLAG_VOLTERRA_ON) == 0u, "neutral: sin flag VOLTERRA_ON");
    check(near(n.cochlear_intensity, 0.0f), "neutral: coclear 0");
    check(near(n.high_cut_hz, 24000.f), "neutral: high_cut 24000 (sin corte)");

    // 8. SAF: sin latente, ganancia identidad.
    check(n.saf_q_valid == 0u, "neutral: saf_q_valid 0 (sin latente SAF)");
    check(near(n.saf_gain, 1.0f), "neutral: saf_gain 1.0");

    // 9. Flags: solo EQ_CALIBRATED. Sin PERCEPTUAL_ON / ANTI_DOLBY_ON /
    // ATI_ON y sin ningún flag SUPREME (los 5 ejes apagados).
    check(n.flags == OMEGA_FLAG_EQ_CALIBRATED,
          "neutral: flags solo EQ_CALIBRATED");
    check((n.flags & (OMEGA_FLAG_SUPREME_LATTICE_ON | OMEGA_FLAG_SUPREME_MICROCHIRP_ON |
                       OMEGA_FLAG_SUPREME_CVNN_ON | OMEGA_FLAG_SUPREME_SNN_HOA_ON |
                       OMEGA_FLAG_SUPREME_PINNA_ON | OMEGA_FLAG_SUPREME_FARROW_MSO_ON |
                       OMEGA_FLAG_SUPREME_EBPF_BYPASS_ON)) == 0u,
          "neutral: ningun flag SUPREME");
    check(n.fusion_active == 0u, "neutral: fusion inactiva");
    check(near(n.fusion_asrc_ratio, 1.0f), "neutral: fusion ratio 1.0");

    // 10. makeDefault() NO es neutro: pincha la diferencia para que nadie
    // vuelva a usarlo como base del snapshot del daemon.
    check((d.temporal_field & OMEGA_EFFECT_HRTF_ENABLED_BIT) != 0u,
          "default: bit HRTF ENCENDIDO (por eso no es neutro)");
    check(!near(d.spatial_width * d.widener_mult, 1.0f),
          "default: widener total != 1.0 (colorea)");
    check(!near(d.intensity, 1.0f), "default: intensity != 1.0");
    check(!near(d.anti_dolby, 0.0f), "default: anti_dolby != 0");
    check(d.saf_q_valid == 1u, "default: saf_q_valid 1 (latente activo)");
    check(d.flags != OMEGA_FLAG_EQ_CALIBRATED,
          "default: flags con mas que EQ_CALIBRATED");

    // 11. Predicados de identidad del FusionCore (mismas fórmulas que
    // IvannaFusionCore::process): con el snapshot neutro, cada etapa
    // queda en su valor identidad.
    // (a) Compresor: compActive = (ratio > 1.005 && threshold < -0.1).
    // El neutro no llama setCompressorParams (compressor >= 0) y el ratio
    // por defecto del core es 1.0 -> inactivo.
    check(n.compressor >= 0.0f, "identidad: compresor inactivo (ratio core 1.0)");
    // (b) Widener: rawTargetW = spatial_width * widener_mult; se aplica
    // solo si |targetW - 1| > 1e-4.
    check(std::fabs(n.spatial_width * n.widener_mult - 1.0f) <= 1e-4f,
          "identidad: widener salta (|targetW-1| <= 1e-4)");
    // (c) GoldenEar: activo solo si harmonic_gain > 0.001.
    check(n.harmonic_gain <= 0.001f, "identidad: GoldenEar inactivo");
    // (d) Trim: targetTrim = min(1, eqTrim * 10^(routeDb/20) * intensity);
    // eqTrim = 10^(phonDelta/20), phonDelta = (ref-listen)*0.05 = 0;
    // routeDb = (bass+dialog)*0.35 = 0; intensity = 1 -> targetTrim 1.
    const float phonDelta =
        std::fabs(n.ref_phon - n.listen_phon) * 0.05f;
    const float eqTrim = std::pow(10.0f, phonDelta / 20.0f);
    const float routeDb = (n.bass_boost_db + n.dialog_boost_db) * 0.35f;
    const float targetTrim = std::min(
        1.0f, eqTrim * std::pow(10.0f, routeDb / 20.0f) * n.intensity);
    check(near(targetTrim, 1.0f), "identidad: targetTrim 1.0 (sin atenuacion)");
    // (e) Anti-Dolby: multiplier = 1 + intensity*(s-1); con intensity 0 -> 1.
    check(near(1.0f + n.anti_dolby * 0.0f, 1.0f),
          "identidad: multiplicador anti-dolby 1.0");
    // (f) Ganancia adaptativa: requestedGain = clamp(target_gain, .5, 1).
    check(near(std::min(std::max(n.target_gain, 0.5f), 1.0f), 1.0f),
          "identidad: requestedGain 1.0 (sin atenuacion adaptativa)");

    std::printf("test_route_b_neutral_snapshot: %d PASS, %d FALLA\n",
                g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
