// test_corona_control_route.cpp — Regresión HOLOS EN VIVO (evol-holos, 2026-11-26).
//
// CAUSA RAÍZ MEDIDA (antes del fix, repro /tmp/corona_before_repro.cpp,
// g++-14, exit 1): el preset CORONA atrevido (HOLOS amount 1.0, spread
// 1.2, proximidad 6.0 dB, diffuse 0.85, harmonics 0.002; ORBE spread 1.2)
// NO llegaba al engine:
//   - omega_effect.cpp clampaba holos_amount a [0, 0.5] → 1.0 se volvía 0.5
//   - holos_diffuse a [0, 0.5] → 0.85 se volvía 0.5
//   - orbe_spread a [0, 1.0] → 1.2 se volvía 1.0
//   - el Genezi codificaba como SPEAKERS (2) y la puerta speakersGate
//     ponía holos_amount = 0.00 en el engine.
// El daemon, el JNI y la app ya admitían los valores CORONA; solo el
// efecto los recortaba y el modo 2 los apagaba en Genezi.
//
// EL FIX (esta rama): clamps canónicos compartidos en omega_control_bus.h
// (única fuente de los topes: amount [0,1], diffuse [0,1], spread [0,1.5]),
// usados por el daemon (applyPendingPerceptual) y por el efecto
// (omega_apply_snapshot); tercer modo GENEZI = 3 en temporal_field bits
// 18..19; puerta binaural (amounts) solo-SPEAKERS, puerta general
// (upmix/EQ/sala) SPEAKERS||GENEZI.
//
// Este test simula la ruta completa (sin NDK, sin red):
//   1. JSON CORONA (outputMode 3) -> responder -> take -> apply sobre
//      makeDefault(): el snapshot publica 1.0 / 1.2 / 1.0 / 1.2 / 6.0 /
//      0.85 / 0.002 y el modo decodifica GENEZI.
//   2. Los helpers de clamp admiten los valores CORONA exactos y siguen
//      clampando lo fuera de rango (amount 5 -> 1.0, diffuse 2 -> 1.0,
//      spread 9 -> 1.5).
//   3. Las puertas: binaural NO gatea GENEZI ni HEADPHONES, SÍ gatea
//      SPEAKERS; speakers-like es GENEZI y SPEAKERS (XTC/Aux/twin).
//   4. Lo que recibe el engine (las mismas líneas de
//      omega_apply_snapshot, con los helpers): en GENEZI 1.0 / 0.85 /
//      1.2; en SPEAKERS 0.0 (puerta); en HEADPHONES 1.0 / 0.85 / 1.2.
//
// Umbrales fijados ANTES de medir: valores CORONA exactos del preset
// (1.0 / 1.2 / 6.0 / 0.85 / 0.002). El ANTES es el repro de /tmp (exit 1);
// este test es el DESPUÉS (exit 0).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../daemon/control/daemon_command_responder.h"
#include "../daemon/control/perceptual_state.h"
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

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

// Réplica literal de las líneas de omega_apply_snapshot (efecto) para
// los amounts, con los helpers canónicos: binauralGate ? 0 : clamp(v).
// El efecto compila estos mismos símbolos desde omega_control_bus.h.
float engineHolosAmount(ivanna::OmegaOutputMode mode, float amount) noexcept {
    const bool binauralGate = ivanna::omega_output_binaural_gated(mode);
    return binauralGate ? 0.f : ivanna::omega_clamp_holos_amount(amount);
}

float engineOrbeSpread(ivanna::OmegaOutputMode mode, float spread) noexcept {
    (void)mode;  // El spread no lo gatea el modo, solo se clampa.
    return ivanna::omega_clamp_orbe_spread(spread);
}

}  // namespace

int main() {
    using namespace ivanna;
    using namespace ivanna::daemon;

    // 1. Ruta completa: JSON CORONA (outputMode 3) -> snapshot.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"outputMode\":3.0,"
            "\"orbeAmount\":1.0,\"orbeSpread\":1.2,"
            "\"holosAmount\":1.0,\"holosSpread\":1.2,\"holosProximityDb\":6.0,"
            "\"holosDiffuse\":0.85,\"holosHarmonics\":0.002}",
            st);
        check(contains(r, "\"ok\":true"), "ruta: ok true");
        check(contains(r, "\"generation\":1"), "ruta: generation 1");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "ruta: take true");
        OmegaDspSnapshot snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(snap.orbe_amount == 1.0f, "ruta: orbe_amount 1.0");
        check(snap.orbe_spread == 1.2f, "ruta: orbe_spread 1.2");
        check(snap.holos_amount == 1.0f, "ruta: holos_amount 1.0");
        check(snap.holos_spread == 1.2f, "ruta: holos_spread 1.2");
        check(snap.holos_proximity_db == 6.0f, "ruta: holos_proximity_db 6.0");
        check(snap.holos_diffuse == 0.85f, "ruta: holos_diffuse 0.85");
        check(snap.holos_harmonics == 0.002f, "ruta: holos_harmonics 0.002");
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::GENEZI,
              "ruta: modo GENEZI en temporal_field");
    }

    // 2. Los helpers admiten CORONA y siguen clampando lo fuera de rango.
    check(omega_clamp_orbe_amount(1.0f) == 1.0f, "clamp: orbe_amount 1.0 pasa");
    check(omega_clamp_orbe_spread(1.2f) == 1.2f, "clamp: orbe_spread 1.2 pasa");
    check(omega_clamp_holos_amount(1.0f) == 1.0f, "clamp: holos_amount 1.0 pasa");
    check(omega_clamp_holos_diffuse(0.85f) == 0.85f, "clamp: holos_diffuse 0.85 pasa");
    check(omega_clamp_orbe_amount(5.0f) == 1.0f, "clamp: orbe_amount 5 -> 1.0");
    check(omega_clamp_holos_amount(-1.0f) == 0.0f, "clamp: holos_amount -1 -> 0");
    check(omega_clamp_holos_diffuse(2.0f) == 1.0f, "clamp: holos_diffuse 2 -> 1.0");
    check(omega_clamp_orbe_spread(9.0f) == 1.5f, "clamp: orbe_spread 9 -> 1.5");

    // 3. Puertas: binaural solo-SPEAKERS; speakers-like SPEAKERS||GENEZI.
    check(!omega_output_binaural_gated(OmegaOutputMode::GENEZI),
          "puerta: binaural NO gatea GENEZI");
    check(!omega_output_binaural_gated(OmegaOutputMode::HEADPHONES),
          "puerta: binaural NO gatea HEADPHONES");
    check(omega_output_binaural_gated(OmegaOutputMode::SPEAKERS),
          "puerta: binaural SI gatea SPEAKERS");
    check(omega_output_is_speakers_like(OmegaOutputMode::GENEZI),
          "puerta: GENEZI es speakers-like");
    check(omega_output_is_speakers_like(OmegaOutputMode::SPEAKERS),
          "puerta: SPEAKERS es speakers-like");
    check(!omega_output_is_speakers_like(OmegaOutputMode::HEADPHONES),
          "puerta: HEADPHONES NO es speakers-like");

    // 4. Lo que recibe el engine (efecto: gate + clamp).
    // ANTES: GENEZI 0.00 / 0.50 / 1.00 (puerta + clamps viejos).
    // DESPUÉS: GENEZI 1.00 / 0.85 / 1.20.
    check(engineHolosAmount(OmegaOutputMode::GENEZI, 1.0f) == 1.0f,
          "engine: GENEZI holos_amount 1.0");
    check(omega_clamp_holos_diffuse(0.85f) == 0.85f,
          "engine: GENEZI holos_diffuse 0.85");
    check(engineOrbeSpread(OmegaOutputMode::GENEZI, 1.2f) == 1.2f,
          "engine: GENEZI orbe_spread 1.2");
    check(engineHolosAmount(OmegaOutputMode::SPEAKERS, 1.0f) == 0.0f,
          "engine: SPEAKERS holos_amount 0.0 (puerta)");
    check(engineHolosAmount(OmegaOutputMode::HEADPHONES, 1.0f) == 1.0f,
          "engine: HEADPHONES holos_amount 1.0");

    std::printf("corona_control_route: %d PASS, %d FALLA\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
