// test_abi_v16_spsc_yamnet_snapshot.cpp — ABI v16: spsc_ring_factor + yamnet_* en el snapshot.
//
// ABI v16 añade al FINAL del snapshot (antes de crc32) 6 campos
// (spsc_ring_factor, yamnet_speech, yamnet_music, yamnet_class_id,
// yamnet_confidence, reserved_v16) — 24 bytes (6×4): sizeof 768 -> 792,
// crc32 764 -> 788, OMEGA_CTRL_VERSION 15 -> 16. Ningún campo v15 ni
// anterior se mueve: la compatibilidad es de prefijo (un lector v15 puede
// leer los primeros 768 bytes de un snapshot v16 y seguir viendo sus campos
// en su sitio; solo ignora la cola spsc/yamnet y rechaza la versión).
//
// Este test fija el contrato completo de ABI v16 (hereda todo el contrato
// v15 de los np_* y las bandas, y añade los campos SPSC/YAMNet):
//   1. Versión 16 y tamaño exacto 792.
//   2. Offset exacto de cada uno de los 17 campos np_*, de los 4 raw_band_*,
//      de los 6 campos v16 y de crc32 (788).
//   3. makeDefault(): los 17 campos con sus defaults (iguales que los
//      atomics C++); snapshot válido (magic+versión+CRC).
//   4. makeNeutral(): np_amount == 0.0f (bypass) y el resto a sus defaults;
//      snapshot válido.
//   5. Round-trip con CRC: valores no-default en los 17 campos sobreviven
//      a una copia byte a byte (memcpy) y el CRC sigue válido; corromper
//      un byte de un campo np_* invalida el CRC (los np_* están dentro del
//      área cubierta por el CRC, [magic .. crc32)).
//   6. Compatibilidad de prefijo: los offsets de los campos v13 y
//      anteriores NO se movieron.
//   7. Defaults v16: spsc_ring_factor 1.0 (neutro), yamnet_class_id -1
//      (desconocida), yamnet_speech/music/confidence 0.0, reserved_v16 0.
//   8. Round-trip v16: valores no-default en los 5 campos sobreviven a
//      memcpy; corromper un byte de spsc_ring_factor invalida el CRC.
//
// Sin NDK, sin red: solo el header. Escrito contra ABI v16 (OMEGA_CTRL_VERSION
// 16, sizeof 792); NO compila contra el header v15 (los campos spsc/yamnet
// no existen ahí) — eso es esperado hasta integrar el header ABI v16.
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

    // 1. Versión y tamaño exactos de ABI v14.
    check(OMEGA_CTRL_VERSION == 16u, "v16: OMEGA_CTRL_VERSION == 16");
    check(sizeof(OmegaDspSnapshot) == 792u, "v16: sizeof(OmegaDspSnapshot) == 792");

    // 2. Offsets exactos de los 17 campos np_* (en el orden del struct) y crc32.
    check(offsetof(OmegaDspSnapshot, np_amount) == 680u, "v14: np_amount @680");
    check(offsetof(OmegaDspSnapshot, np_lgn_mix) == 684u, "v14: np_lgn_mix @684");
    check(offsetof(OmegaDspSnapshot, np_bs_damp) == 688u, "v14: np_bs_damp @688");
    check(offsetof(OmegaDspSnapshot, np_torso_mix) == 692u, "v14: np_torso_mix @692");
    check(offsetof(OmegaDspSnapshot, np_head_radius_m) == 696u, "v14: np_head_radius_m @696");
    check(offsetof(OmegaDspSnapshot, np_az_deg) == 700u, "v14: np_az_deg @700");
    check(offsetof(OmegaDspSnapshot, np_el_deg) == 704u, "v14: np_el_deg @704");
    check(offsetof(OmegaDspSnapshot, np_eq_class) == 708u, "v14: np_eq_class @708");
    check(offsetof(OmegaDspSnapshot, np_omega_sig_mix) == 712u, "v14: np_omega_sig_mix @712");
    check(offsetof(OmegaDspSnapshot, np_boost_cap_db) == 716u, "v14: np_boost_cap_db @716");
    check(offsetof(OmegaDspSnapshot, np_xtalk_mix) == 720u, "v14: np_xtalk_mix @720");
    check(offsetof(OmegaDspSnapshot, np_sigma_dist_m) == 724u, "v14: np_sigma_dist_m @724");
    check(offsetof(OmegaDspSnapshot, np_spl_db) == 728u, "v14: np_spl_db @728");
    check(offsetof(OmegaDspSnapshot, np_air_mix) == 732u, "v14: np_air_mix @732");
    check(offsetof(OmegaDspSnapshot, np_volterra_mix) == 736u, "v14: np_volterra_mix @736");
    check(offsetof(OmegaDspSnapshot, np_free_energy_mix) == 740u, "v14: np_free_energy_mix @740");
    check(offsetof(OmegaDspSnapshot, np_abx_mode) == 744u, "v14: np_abx_mode @744");
    check(offsetof(OmegaDspSnapshot, crc32) == 788u, "v16: crc32 @788");
    // 2b. Offsets exactos de los 3 campos raw_band_* (tras np_abx_mode @744).
    check(offsetof(OmegaDspSnapshot, raw_band_low) == 748u, "v15: raw_band_low @748");
    check(offsetof(OmegaDspSnapshot, raw_band_mid) == 752u, "v15: raw_band_mid @752");
    check(offsetof(OmegaDspSnapshot, raw_band_high) == 756u, "v15: raw_band_high @756");
    check(offsetof(OmegaDspSnapshot, raw_band_spread) == 760u, "v15: raw_band_spread @760");
    // 2c. Offsets exactos de los 6 campos v16 (tras raw_band_spread @760).
    check(offsetof(OmegaDspSnapshot, spsc_ring_factor) == 764u, "v16: spsc_ring_factor @764");
    check(offsetof(OmegaDspSnapshot, yamnet_speech) == 768u, "v16: yamnet_speech @768");
    check(offsetof(OmegaDspSnapshot, yamnet_music) == 772u, "v16: yamnet_music @772");
    check(offsetof(OmegaDspSnapshot, yamnet_class_id) == 776u, "v16: yamnet_class_id @776");
    check(offsetof(OmegaDspSnapshot, yamnet_confidence) == 780u, "v16: yamnet_confidence @780");
    check(offsetof(OmegaDspSnapshot, reserved_v16) == 784u, "v16: reserved_v16 @784");
    // Los 17 campos np_* son contiguos y ocupan exactamente [680, 748);
    // tras ellos van los 4 campos raw_band_* [748, 764), los 6 campos v16
    // [764, 788) y luego crc32.
    check(offsetof(OmegaDspSnapshot, np_abx_mode) + sizeof(int32_t) ==
              offsetof(OmegaDspSnapshot, raw_band_low),
          "v15: bloque np_* contiguo, termina justo en raw_band_low");
    check(offsetof(OmegaDspSnapshot, raw_band_spread) + sizeof(float) ==
              offsetof(OmegaDspSnapshot, spsc_ring_factor),
          "v16: bloque raw_band_* contiguo, termina justo en spsc_ring_factor");
    check(offsetof(OmegaDspSnapshot, reserved_v16) + sizeof(uint32_t) ==
              offsetof(OmegaDspSnapshot, crc32),
          "v16: bloque v16 contiguo, termina justo en crc32");

    // 3. makeDefault(): los 17 campos con sus defaults; snapshot válido.
    const OmegaDspSnapshot d = OmegaDspSnapshot::makeDefault();
    check(d.isValid(), "default: snapshot valido (magic/version/CRC)");
    check(near(d.np_amount, 0.0f), "default: np_amount 0.0");
    check(near(d.np_lgn_mix, 1.0f), "default: np_lgn_mix 1.0");
    check(near(d.np_bs_damp, 0.65f), "default: np_bs_damp 0.65");
    check(near(d.np_torso_mix, 0.18f), "default: np_torso_mix 0.18");
    check(near(d.np_head_radius_m, 0.0875f), "default: np_head_radius_m 0.0875");
    check(near(d.np_az_deg, 0.0f), "default: np_az_deg 0.0");
    check(near(d.np_el_deg, 0.0f), "default: np_el_deg 0.0");
    check(d.np_eq_class == 0, "default: np_eq_class 0");
    check(near(d.np_omega_sig_mix, 0.0f), "default: np_omega_sig_mix 0.0");
    check(near(d.np_boost_cap_db, 3.0f), "default: np_boost_cap_db 3.0");
    check(near(d.np_xtalk_mix, 0.0f), "default: np_xtalk_mix 0.0");
    check(near(d.np_sigma_dist_m, 0.35f), "default: np_sigma_dist_m 0.35");
    check(near(d.np_spl_db, 85.0f), "default: np_spl_db 85.0");
    check(near(d.np_air_mix, 0.0f), "default: np_air_mix 0.0");
    check(near(d.np_volterra_mix, 0.0f), "default: np_volterra_mix 0.0");
    check(near(d.np_free_energy_mix, 0.0f), "default: np_free_energy_mix 0.0");
    check(d.np_abx_mode == 0, "default: np_abx_mode 0");
    check(near(d.raw_band_low, 0.0f), "default: raw_band_low 0.0");
    check(near(d.raw_band_mid, 0.0f), "default: raw_band_mid 0.0");
    check(near(d.raw_band_high, 0.0f), "default: raw_band_high 0.0");
    check(near(d.raw_band_spread, 0.0f), "default: raw_band_spread 0.0");
    // 3b. Defaults v16 (ABI v16): SPSC neutro, YAMNet sin clasificar.
    check(near(d.spsc_ring_factor, 1.0f), "default: spsc_ring_factor 1.0");
    check(near(d.yamnet_speech, 0.0f), "default: yamnet_speech 0.0");
    check(near(d.yamnet_music, 0.0f), "default: yamnet_music 0.0");
    check(d.yamnet_class_id == -1, "default: yamnet_class_id -1");
    check(near(d.yamnet_confidence, 0.0f), "default: yamnet_confidence 0.0");
    check(d.reserved_v16 == 0u, "default: reserved_v16 0");

    // 4. makeNeutral(): np_amount == 0.0f (bypass) y el resto a sus defaults.
    const OmegaDspSnapshot n = OmegaDspSnapshot::makeNeutral();
    check(n.isValid(), "neutral: snapshot valido (magic/version/CRC)");
    check(near(n.np_amount, 0.0f), "neutral: np_amount 0.0 (bypass)");
    check(near(n.np_lgn_mix, 1.0f), "neutral: np_lgn_mix 1.0");
    check(near(n.np_bs_damp, 0.65f), "neutral: np_bs_damp 0.65");
    check(near(n.np_torso_mix, 0.18f), "neutral: np_torso_mix 0.18");
    check(near(n.np_head_radius_m, 0.0875f), "neutral: np_head_radius_m 0.0875");
    check(near(n.np_az_deg, 0.0f), "neutral: np_az_deg 0.0");
    check(near(n.np_el_deg, 0.0f), "neutral: np_el_deg 0.0");
    check(n.np_eq_class == 0, "neutral: np_eq_class 0");
    check(near(n.np_omega_sig_mix, 0.0f), "neutral: np_omega_sig_mix 0.0");
    check(near(n.np_boost_cap_db, 3.0f), "neutral: np_boost_cap_db 3.0");
    check(near(n.np_xtalk_mix, 0.0f), "neutral: np_xtalk_mix 0.0");
    check(near(n.np_sigma_dist_m, 0.35f), "neutral: np_sigma_dist_m 0.35");
    check(near(n.np_spl_db, 85.0f), "neutral: np_spl_db 85.0");
    check(near(n.np_air_mix, 0.0f), "neutral: np_air_mix 0.0");
    check(near(n.np_volterra_mix, 0.0f), "neutral: np_volterra_mix 0.0");
    check(near(n.np_free_energy_mix, 0.0f), "neutral: np_free_energy_mix 0.0");
    check(n.np_abx_mode == 0, "neutral: np_abx_mode 0");
    check(near(n.raw_band_low, 0.0f), "neutral: raw_band_low 0.0");
    check(near(n.raw_band_mid, 0.0f), "neutral: raw_band_mid 0.0");
    check(near(n.raw_band_high, 0.0f), "neutral: raw_band_high 0.0");
    check(near(n.raw_band_spread, 0.0f), "neutral: raw_band_spread 0.0");
    check(near(n.spsc_ring_factor, 1.0f), "neutral: spsc_ring_factor 1.0");
    check(near(n.yamnet_speech, 0.0f), "neutral: yamnet_speech 0.0");
    check(near(n.yamnet_music, 0.0f), "neutral: yamnet_music 0.0");
    check(n.yamnet_class_id == -1, "neutral: yamnet_class_id -1");
    check(near(n.yamnet_confidence, 0.0f), "neutral: yamnet_confidence 0.0");
    check(n.reserved_v16 == 0u, "neutral: reserved_v16 0");

    // 5. Round-trip con CRC: valores no-default en los 17 campos sobreviven
    // a una copia byte a byte y el CRC sigue válido; corromper un byte de un
    // campo np_* invalida el CRC (los np_* están dentro del área CRC).
    {
        OmegaDspSnapshot s = OmegaDspSnapshot::makeDefault();
        s.np_amount = 0.7f;
        s.np_lgn_mix = 0.5f;
        s.np_bs_damp = 0.45f;
        s.np_torso_mix = 0.30f;
        s.np_head_radius_m = 0.09f;
        s.np_az_deg = 30.0f;
        s.np_el_deg = 15.0f;
        s.np_eq_class = 2;
        s.np_omega_sig_mix = 0.8f;
        s.np_boost_cap_db = 6.0f;
        s.np_xtalk_mix = 0.25f;
        s.np_sigma_dist_m = 0.50f;
        s.np_spl_db = 90.0f;
        s.np_air_mix = 0.4f;
        s.np_volterra_mix = 0.6f;
        s.np_free_energy_mix = 0.35f;
        s.np_abx_mode = 1;
        s.raw_band_low = 0.42f;
        s.raw_band_mid = 0.31f;
        s.raw_band_high = 0.18f;
        s.raw_band_spread = 1200.0f;
        // 5b. Valores no-default v16 (SPSC + YAMNet).
        s.spsc_ring_factor = 1.35f;
        s.yamnet_speech = 0.82f;
        s.yamnet_music = 0.17f;
        s.yamnet_class_id = 137;
        s.yamnet_confidence = 0.91f;
        s.stampCrc();
        check(s.isValid(), "round-trip: snapshot con np_* no-default valido tras stampCrc()");

        // Copia byte a byte (como la hace el seqlock / mmap cross-process).
        OmegaDspSnapshot copy{};
        std::memcpy(&copy, &s, sizeof(OmegaDspSnapshot));
        check(copy.isValid(), "round-trip: copia memcpy sigue valida (CRC)");
        check(near(copy.np_amount, 0.7f), "round-trip: np_amount 0.7 sobrevive");
        check(near(copy.np_lgn_mix, 0.5f), "round-trip: np_lgn_mix 0.5 sobrevive");
        check(near(copy.np_bs_damp, 0.45f), "round-trip: np_bs_damp 0.45 sobrevive");
        check(near(copy.np_torso_mix, 0.30f), "round-trip: np_torso_mix 0.30 sobrevive");
        check(near(copy.np_head_radius_m, 0.09f), "round-trip: np_head_radius_m 0.09 sobrevive");
        check(near(copy.np_az_deg, 30.0f), "round-trip: np_az_deg 30.0 sobrevive");
        check(near(copy.np_el_deg, 15.0f), "round-trip: np_el_deg 15.0 sobrevive");
        check(copy.np_eq_class == 2, "round-trip: np_eq_class 2 sobrevive");
        check(near(copy.np_omega_sig_mix, 0.8f), "round-trip: np_omega_sig_mix 0.8 sobrevive");
        check(near(copy.np_boost_cap_db, 6.0f), "round-trip: np_boost_cap_db 6.0 sobrevive");
        check(near(copy.np_xtalk_mix, 0.25f), "round-trip: np_xtalk_mix 0.25 sobrevive");
        check(near(copy.np_sigma_dist_m, 0.50f), "round-trip: np_sigma_dist_m 0.50 sobrevive");
        check(near(copy.np_spl_db, 90.0f), "round-trip: np_spl_db 90.0 sobrevive");
        check(near(copy.np_air_mix, 0.4f), "round-trip: np_air_mix 0.4 sobrevive");
        check(near(copy.np_volterra_mix, 0.6f), "round-trip: np_volterra_mix 0.6 sobrevive");
        check(near(copy.np_free_energy_mix, 0.35f), "round-trip: np_free_energy_mix 0.35 sobrevive");
        check(copy.np_abx_mode == 1, "round-trip: np_abx_mode 1 sobrevive");
        check(near(copy.raw_band_low, 0.42f), "round-trip: raw_band_low 0.42 sobrevive");
        check(near(copy.raw_band_mid, 0.31f), "round-trip: raw_band_mid 0.31 sobrevive");
        check(near(copy.raw_band_high, 0.18f), "round-trip: raw_band_high 0.18 sobrevive");
        check(near(copy.raw_band_spread, 1200.0f), "round-trip: raw_band_spread 1200.0 sobrevive");
        check(near(copy.spsc_ring_factor, 1.35f), "round-trip: spsc_ring_factor 1.35 sobrevive");
        check(near(copy.yamnet_speech, 0.82f), "round-trip: yamnet_speech 0.82 sobrevive");
        check(near(copy.yamnet_music, 0.17f), "round-trip: yamnet_music 0.17 sobrevive");
        check(copy.yamnet_class_id == 137, "round-trip: yamnet_class_id 137 sobrevive");
        check(near(copy.yamnet_confidence, 0.91f), "round-trip: yamnet_confidence 0.91 sobrevive");

        // Corromper UN byte dentro de un campo np_* (sin re-estampar) debe
        // invalidar el CRC: prueba que los np_* están dentro del área CRC.
        OmegaDspSnapshot corrupted{};
        std::memcpy(&corrupted, &s, sizeof(OmegaDspSnapshot));
        reinterpret_cast<char*>(&corrupted)[offsetof(OmegaDspSnapshot, np_az_deg)] ^= 0xFF;
        check(!corrupted.isValid(), "crc: corromper 1 byte de np_az_deg invalida el CRC");
        std::memcpy(&corrupted, &s, sizeof(OmegaDspSnapshot));
        reinterpret_cast<char*>(&corrupted)[offsetof(OmegaDspSnapshot, np_abx_mode)] ^= 0x01;
        check(!corrupted.isValid(), "crc: corromper 1 byte de np_abx_mode invalida el CRC");
        std::memcpy(&corrupted, &s, sizeof(OmegaDspSnapshot));
        reinterpret_cast<char*>(&corrupted)[offsetof(OmegaDspSnapshot, raw_band_mid)] ^= 0xFF;
        check(!corrupted.isValid(), "crc: corromper 1 byte de raw_band_mid invalida el CRC");
        std::memcpy(&corrupted, &s, sizeof(OmegaDspSnapshot));
        reinterpret_cast<char*>(&corrupted)[offsetof(OmegaDspSnapshot, np_spl_db) + 3] ^= 0x80;
        check(!corrupted.isValid(), "crc: corromper 1 byte de np_spl_db invalida el CRC");
        // Los campos v16 también están dentro del área CRC.
        std::memcpy(&corrupted, &s, sizeof(OmegaDspSnapshot));
        reinterpret_cast<char*>(&corrupted)[offsetof(OmegaDspSnapshot, spsc_ring_factor)] ^= 0xFF;
        check(!corrupted.isValid(), "crc: corromper 1 byte de spsc_ring_factor invalida el CRC");
        std::memcpy(&corrupted, &s, sizeof(OmegaDspSnapshot));
        reinterpret_cast<char*>(&corrupted)[offsetof(OmegaDspSnapshot, yamnet_class_id)] ^= 0x01;
        check(!corrupted.isValid(), "crc: corromper 1 byte de yamnet_class_id invalida el CRC");
        // Re-estampar tras la corrupción vuelve a validar (el CRC se
        // recomputa sobre el contenido actual, no es un sello fijo).
        corrupted.stampCrc();
        check(corrupted.isValid(), "crc: re-estampar tras corrupcion vuelve a validar");
    }

    // 6. Compatibilidad de prefijo: los offsets v15 y anteriores NO se
    // movieron — ABI v16 solo añadió al final (antes de crc32).
    check(offsetof(OmegaDspSnapshot, consumer_generation) == 32u,
          "prefijo: consumer_generation @32 (sin mover)");
    check(offsetof(OmegaDspSnapshot, raw_rms) == 412u,
          "prefijo: raw_rms @412 (sin mover)");
    check(offsetof(OmegaDspSnapshot, raw_peak) == 416u,
          "prefijo: raw_peak @416 (sin mover)");
    check(offsetof(OmegaDspSnapshot, effect_frames) == 440u,
          "prefijo: effect_frames @440 (sin mover)");
    check(offsetof(OmegaDspSnapshot, discovered_session_id) == 608u,
          "prefijo: discovered_session_id @608 (sin mover)");
    check(offsetof(OmegaDspSnapshot, fusion_active) == 624u,
          "prefijo: fusion_active @624 (sin mover)");
    check(offsetof(OmegaDspSnapshot, hyper_amount) == 664u,
          "prefijo: hyper_amount @664 (sin mover)");
    check(offsetof(OmegaDspSnapshot, volterra_enabled) == 676u,
          "prefijo: volterra_enabled @676 (sin mover)");
    check(offsetof(OmegaDspSnapshot, raw_band_low) == 748u,
          "prefijo: raw_band_low @748 (sin mover)");

    std::printf("test_abi_v16_spsc_yamnet_snapshot: %d PASS, %d FALLA\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
