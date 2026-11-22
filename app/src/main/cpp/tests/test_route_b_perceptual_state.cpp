// test_route_b_perceptual_state.cpp — Regresión Ruta B neutra (2026-11-14).
//
// CAUSA RAÍZ: la app envía ORBE/HOLOS SOLO por SET_PERCEPTUAL_STATE al
// socket @omega_daemon_socket (PersistedStateRestorer.kt), pero el daemon
// v2 respondía {"ok":false,"error":"unsupported"} a todo SET_* (la 440
// repuso solo HELLO/GET_STATUS y el CommandServer viejo no se compila en
// v2). El daemon republicaba makeDefault() (ORBE 0 / HOLOS 0) 20 veces por
// segundo: el efecto se acoplaba y el beacon crecía, pero la salida era
// ≈ la entrada. Este test fija el contrato que lo repone
// (control/perceptual_state.h + control/daemon_command_responder.h):
//   1. SET_PERCEPTUAL_STATE (ORBE, valores de PersistedStateRestorer) ->
//      ok:true, accepted_pending_consumer, generation 1; al aplicar a un
//      makeDefault(), orbe_amount == 1.0 exacto y holos_amount sigue 0.
//   2. SET_PERCEPTUAL_STATE (HOLOS) -> generation 2; al aplicar, el merge
//      conserva ORBE 1.0 y pone holos_amount == 0.5.
//   3. Alias SEND_PERCEPTUAL_STATE funciona igual.
//   4. Valores fuera de rango se clampan (orbeAmount 5 -> 1.0,
//      orbeDistanceM 99 -> 2.0, holosGamma 0.1 -> 0.5, bandCount 12/24).
//   5. Alias de claves (exciterReduction, highCut, loudnessTargetLuFS,
//      antiDolbyIntensity) mapean al mismo campo.
//   6. Comando sin campos -> ok:true, no_fields, sin incrementar gen.
//   7. 0044: SET_INTENSITY ya es comando real (ok:true); la acción
//      desconocida (SET_NO_EXISTE) sigue -> unsupported.
//   8. takePendingPerceptual vacía: el segundo take es false.
//
// Umbrales fijados ANTES de medir: amounts exactos (1.0f / 0.5f), clamps
// exactos, generaciones 1 y 2. Sin NDK, sin red: solo el header.
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

}  // namespace

int main() {
    using namespace ivanna::daemon;

    // 1. ORBE (valores reales que envía PersistedStateRestorer).
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1.0,"
            "\"orbeDistanceM\":0.35,\"orbeSpread\":1.0,\"orbeProximityDb\":3.0,"
            "\"orbeDiffuse\":0.5,\"orbeHeadTracking\":1.0}",
            st);
        check(contains(r, "\"ok\":true"), "orbe: ok true");
        check(contains(r, "accepted_pending_consumer"), "orbe: accepted_pending_consumer");
        check(contains(r, "\"generation\":1"), "orbe: generation 1");
        check(st.perceptualGen.load() == 1, "orbe: perceptualGen 1");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "orbe: take true");
        check(!takePendingPerceptual(st, p), "orbe: segundo take false (vacío)");
        ivanna::OmegaDspSnapshot snap = ivanna::OmegaDspSnapshot::makeDefault();
        check(snap.orbe_amount == 0.f, "orbe: makeDefault neutro (0.0)");
        applyPendingPerceptual(snap, p);
        check(snap.orbe_amount == 1.0f, "orbe: amount 1.0 exacto tras apply");
        check(snap.orbe_distance_m == 0.35f, "orbe: distance 0.35");
        check(snap.orbe_spread == 1.0f, "orbe: spread 1.0");
        check(snap.orbe_proximity_db == 3.0f, "orbe: proximity 3.0");
        check(snap.orbe_diffuse == 0.5f, "orbe: diffuse 0.5");
        check(snap.orbe_head_tracking == 1u, "orbe: headTracking 1");
        check(snap.holos_amount == 0.f, "orbe: holos sigue 0 (merge parcial)");

        // 2. HOLOS: el merge conserva ORBE 1.0.
        const std::string r2 = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":0.5,"
            "\"holosDistanceM\":0.35,\"holosSpread\":1.0,\"holosProximityDb\":3.0,"
            "\"holosDiffuse\":0.5,\"holosHarmonics\":0.0,\"holosWarmth\":0.5,"
            "\"holosDynamics\":1.0,\"holosWidth\":1.0,\"holosGamma\":1.0,"
            "\"holosRoomRt60\":0.34,\"holosHeadTracking\":1.0,\"holosBandCount\":24.0,"
            "\"holosEqLow\":0.0,\"holosEqMid\":0.0,\"holosEqHigh\":0.0,"
            "\"holosEqPresence\":0.0,\"holosEqTiltDb\":0.0,\"holosEqFreq\":1000.0}",
            st);
        check(contains(r2, "\"generation\":2"), "holos: generation 2");
        PendingPerceptual p2;
        check(takePendingPerceptual(st, p2), "holos: take true");
        applyPendingPerceptual(snap, p2);
        check(snap.orbe_amount == 1.0f, "holos: merge conserva orbe 1.0");
        check(snap.holos_amount == 0.5f, "holos: amount 0.5 exacto tras apply");
        check(snap.holos_harmonics == 0.0f, "holos: harmonics 0.0");
        check(snap.holos_band_count == 24u, "holos: bandCount 24");
        check(snap.holos_eq_freq == 1000.f, "holos: eqFreq 1000");
    }

    // 3. Alias SEND_PERCEPTUAL_STATE.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SEND_PERCEPTUAL_STATE\",\"orbeAmount\":0.75}", st);
        check(contains(r, "\"ok\":true"), "alias SEND: ok true");
        check(contains(r, "\"generation\":1"), "alias SEND: generation 1");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "alias SEND: take true");
        ivanna::OmegaDspSnapshot snap = ivanna::OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(snap.orbe_amount == 0.75f, "alias SEND: orbe 0.75 aplicado");
    }

    // 4. Clamps (mismos que el servidor viejo).
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":5.0,"
            "\"orbeDistanceM\":99.0,\"holosGamma\":0.1,\"holosBandCount\":12.0,"
            "\"holosEqLow\":-99.0,\"holosEqFreq\":1.0}",
            st);
        check(contains(r, "\"ok\":true"), "clamp: ok true (no se rechaza)");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "clamp: take true");
        ivanna::OmegaDspSnapshot snap = ivanna::OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(snap.orbe_amount == 1.0f, "clamp: orbeAmount 5 -> 1.0");
        check(snap.orbe_distance_m == 2.0f, "clamp: orbeDistance 99 -> 2.0");
        check(snap.holos_gamma == 0.5f, "clamp: holosGamma 0.1 -> 0.5");
        check(snap.holos_band_count == 12u, "clamp: bandCount 12 -> 12");
        check(snap.holos_eq_low == -12.f, "clamp: eqLow -99 -> -12");
        check(snap.holos_eq_freq == 20.f, "clamp: eqFreq 1 -> 20");
    }

    // 5. Alias de claves del servidor viejo.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"exciterReduction\":0.3,"
            "\"highCut\":18000.0,\"loudnessTargetLuFS\":-14.0,"
            "\"antiDolbyIntensity\":0.9,\"upmixingEnabled\":1.0,"
            "\"upmixingImmersivity\":0.8}",
            st);
        check(contains(r, "\"ok\":true"), "alias claves: ok true");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "alias claves: take true");
        ivanna::OmegaDspSnapshot snap = ivanna::OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(snap.exciter_reduction == 0.3f, "alias: exciterReduction -> exciter_reduction");
        check(snap.high_cut_hz == 18000.f, "alias: highCut -> high_cut_hz");
        check(snap.loudness_target == -14.f, "alias: loudnessTargetLuFS -> loudness_target");
        check(snap.anti_dolby == 0.9f, "alias: antiDolbyIntensity -> anti_dolby");
        check(snap.upmixing_enabled == 1u, "alias: upmixingEnabled 1");
        check(snap.upmixing_immersivity == 0.8f, "alias: upmixingImmersivity 0.8");
    }

    // 6. Comando sin campos: no_fields, sin incrementar generación.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\"}", st);
        check(contains(r, "\"ok\":true"), "vacio: ok true");
        check(contains(r, "no_fields"), "vacio: no_fields");
        check(contains(r, "\"generation\":0"), "vacio: generation sigue 0");
        check(st.perceptualGen.load() == 0, "vacio: perceptualGen 0");
        PendingPerceptual p;
        check(!takePendingPerceptual(st, p), "vacio: take false");
    }

    // 7. 0044 (Fallo 1): SET_INTENSITY YA es comando real (antes
    // unsupported por el contrato de la 440). Ahora se acepta y toca
    // perceptualGen; la acción desconocida pasa a ser SET_NO_EXISTE.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_INTENSITY\",\"intensity\":0.5}", st);
        check(contains(r, "\"ok\":true"), "SET_INTENSITY: ok true (0044: comando real)");
        check(contains(r, "accepted_pending_consumer"), "SET_INTENSITY: accepted_pending_consumer");
        check(st.perceptualGen.load() == 1, "SET_INTENSITY: toca perceptualGen (1)");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p) && !std::isnan(p.intensity) && p.intensity == 0.5f,
              "SET_INTENSITY: intensity 0.5 stasheado");
        const std::string u = handleCommand("{\"action\":\"SET_NO_EXISTE\",\"x\":1}", st);
        check(contains(u, "\"ok\":false") && contains(u, "unsupported"),
              "SET_NO_EXISTE: ok false unsupported");
        check(st.perceptualGen.load() == 1, "SET_NO_EXISTE: no toca perceptualGen");
    }

    // 9. Φ + Volterra (ABI v13, 0038+0040): paridad Ruta B. El snapshot
    // transporta hyper_amount/preset/xtc y volterra_enabled; el daemon
    // sincroniza el bit OMEGA_FLAG_VOLTERRA_ON en flags (el efecto lo lee
    // en omega_process para gobernar ctx->volterraEngine).
    {
        CommandPlaneState st;
        ivanna::OmegaDspSnapshot snap = ivanna::OmegaDspSnapshot::makeNeutral();
        check(snap.hyper_amount == 0.f, "v13: neutro hyper_amount 0");
        check(snap.volterra_enabled == 0u, "v13: neutro volterra_enabled 0");
        check((snap.flags & ivanna::OMEGA_FLAG_VOLTERRA_ON) == 0u, "v13: neutro sin flag VOLTERRA_ON");
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"hyperAmount\":0.5,\"hyperPreset\":1,\"hyperXtcEnabled\":1,\"volterraEnabled\":1}", st);
        check(contains(r, "\"ok\":true"), "v13: SET_PERCEPTUAL_STATE hyper/volterra ok");
        ivanna::daemon::PendingPerceptual p;
        check(takePendingPerceptual(st, p), "v13: take true");
        applyPendingPerceptual(snap, p);
        check(snap.hyper_amount == 0.5f, "v13: hyper_amount == 0.5");
        check(snap.hyper_preset == 1, "v13: hyper_preset == 1");
        check(snap.hyper_xtc_enabled == 1u, "v13: hyper_xtc_enabled == 1");
        check(snap.volterra_enabled == 1u, "v13: volterra_enabled == 1");
        check((snap.flags & ivanna::OMEGA_FLAG_VOLTERRA_ON) != 0u, "v13: flag VOLTERRA_ON puesto");
        // Apagar Volterra limpia el bit (y el campo).
        const std::string r2 = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"volterraEnabled\":0}", st);
        check(contains(r2, "\"ok\":true"), "v13: volterraEnabled 0 ok");
        ivanna::daemon::PendingPerceptual p2;
        check(takePendingPerceptual(st, p2), "v13: segundo take true");
        applyPendingPerceptual(snap, p2);
        check(snap.volterra_enabled == 0u, "v13: volterra_enabled == 0 tras apagar");
        check((snap.flags & ivanna::OMEGA_FLAG_VOLTERRA_ON) == 0u, "v13: flag VOLTERRA_ON limpio tras apagar");
        // Clamps: hyperAmount 5 -> 1.0, hyperPreset 9 -> 2.
        const std::string r3 = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"hyperAmount\":5,\"hyperPreset\":9}", st);
        check(contains(r3, "\"ok\":true"), "v13: clamp ok");
        ivanna::daemon::PendingPerceptual p3;
        check(takePendingPerceptual(st, p3), "v13: tercer take true");
        applyPendingPerceptual(snap, p3);
        check(snap.hyper_amount == 1.0f, "v13: hyperAmount 5 clamp a 1.0");
        check(snap.hyper_preset == 2, "v13: hyperPreset 9 clamp a 2");
        check(snap.volterra_enabled == 0u, "v13: volterra_enabled intacto (ausente no se toca)");
    }

    // 10. FUSIÓN v2.5.469: los sliders de FUSIÓN llegan a Ruta B.
    // CAUSA RAÍZ: FusedControlsBridge.safeSetFusedControls solo escribía
    // g_fusedChainRouteA (Ruta A, app); nunca mandaba SET_PERCEPTUAL_STATE
    // al daemon, así que la Ruta B no se enteraba y mover sliders no
    // cambiaba el sonido. El fix (Kotlin, buildFusedPerceptualFields)
    // empuja los valores fusionados por el mismo canal legacy. Este test
    // fija el contrato del lado daemon: el JSON exacto que el puente
    // Kotlin construye (un FusedControls con valores distintivos) se
    // parsea, hace merge y deja los campos en el snapshot. Si el Kotlin
    // cambia un nombre de clave, este test falla (el campo queda en
    // default) — el contrato Kotlin ↔ daemon queda fijado aquí.
    // Además: los campos legacy que la pantalla fusionada NO controla
    // (orbeProximityDb, holosDistanceM) se conservan (merge parcial).
    {
        CommandPlaneState st;
        ivanna::OmegaDspSnapshot snap = ivanna::OmegaDspSnapshot::makeNeutral();
        // Siembra valores legacy que la pantalla fusionada no controla.
        {
            const std::string rl = handleCommand(
                "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeProximityDb\":3.0,"
                "\"holosDistanceM\":0.35}",
                st);
            check(contains(rl, "\"ok\":true"), "fusion: siembra legacy ok");
            PendingPerceptual pl;
            check(takePendingPerceptual(st, pl), "fusion: siembra legacy take");
            applyPendingPerceptual(snap, pl);
            check(snap.orbe_proximity_db == 3.0f, "fusion: siembra orbeProximity 3.0");
            check(snap.holos_distance_m == 0.35f, "fusion: siembra holosDistance 0.35");
        }
        // JSON exacto de buildFusedPerceptualFields (FusedControls con
        // valores distintivos; modo SPEAKERS = outputMode 2).
        // Nota: holosGamma va a 0.75 (el daemon lo clampa a [0.5, 1.0] en
        // applyPendingPerceptual; el slider fusionado es 0..2, así que en
        // Ruta B gamma solo es efectivo en [0.5, 1.0] — límite del canal
        // legacy, no del fix).
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\","
            "\"orbeAmount\":1.0,\"orbeDistanceM\":1.0,\"orbeSpread\":1.5,"
            "\"orbeHeadTracking\":0.0,\"outputMode\":2.0,"
            "\"upmixingEnabled\":0.0,\"upmixingImmersivity\":1.25,"
            "\"holosAmount\":0.5,\"holosHarmonics\":0.001,\"holosWarmth\":0.3,"
            "\"holosDynamics\":0.8,\"holosGamma\":0.75,\"holosRoomRt60\":0.5,"
            "\"hyperAmount\":0.5,\"hyperPreset\":1.0,\"hyperXtcEnabled\":1.0,"
            "\"volterraEnabled\":1.0,"
            "\"holosEqLow\":2.0,\"holosEqMid\":-1.0,\"holosEqHigh\":1.0,"
            "\"spatialWidth\":1.2,\"compressor\":-12.0,\"harmonicGain\":0.4}",
            st);
        check(contains(r, "\"ok\":true"), "fusion: SET_PERCEPTUAL_STATE ok");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "fusion: take true");
        applyPendingPerceptual(snap, p);
        check(snap.orbe_amount == 1.0f, "fusion: orbe_amount 1.0");
        check(snap.orbe_distance_m == 1.0f, "fusion: orbe_distance 1.0");
        check(snap.orbe_spread == 1.5f, "fusion: orbe_spread 1.5");
        check(snap.orbe_head_tracking == 0u, "fusion: orbeHeadTracking 0");
        check(ivanna::omega_output_mode_decode(snap.temporal_field) ==
                  ivanna::OmegaOutputMode::SPEAKERS,
              "fusion: outputMode SPEAKERS (bits 18..19)");
        check(snap.upmixing_enabled == 0u, "fusion: upmixingEnabled 0");
        check(snap.upmixing_immersivity == 1.25f, "fusion: upmixingImmersivity 1.25");
        check(snap.holos_amount == 0.5f, "fusion: holos_amount 0.5");
        check(snap.holos_harmonics == 0.001f, "fusion: holos_harmonics 0.001");
        check(snap.holos_warmth == 0.3f, "fusion: holos_warmth 0.3");
        check(snap.holos_dynamics == 0.8f, "fusion: holos_dynamics 0.8");
        check(snap.holos_gamma == 0.75f, "fusion: holos_gamma 0.75");
        check(snap.holos_room_rt60 == 0.5f, "fusion: holos_room_rt60 0.5");
        check(snap.hyper_amount == 0.5f, "fusion: hyper_amount 0.5");
        check(snap.hyper_preset == 1, "fusion: hyper_preset 1");
        check(snap.hyper_xtc_enabled == 1u, "fusion: hyper_xtc_enabled 1");
        check(snap.volterra_enabled == 1u, "fusion: volterra_enabled 1");
        check((snap.flags & ivanna::OMEGA_FLAG_VOLTERRA_ON) != 0u,
              "fusion: flag VOLTERRA_ON puesto");
        check(snap.holos_eq_low == 2.0f, "fusion: holos_eq_low 2.0");
        check(snap.holos_eq_mid == -1.0f, "fusion: holos_eq_mid -1.0");
        check(snap.holos_eq_high == 1.0f, "fusion: holos_eq_high 1.0");
        check(snap.spatial_width == 1.2f, "fusion: spatial_width 1.2");
        check(snap.compressor == -12.0f, "fusion: compressor -12.0");
        check(snap.harmonic_gain == 0.4f, "fusion: harmonic_gain 0.4");
        // Merge parcial: los legacy no controlados por la pantalla
        // fusionada se conservan.
        check(snap.orbe_proximity_db == 3.0f,
              "fusion: merge conserva orbeProximity 3.0");
        check(snap.holos_distance_m == 0.35f,
              "fusion: merge conserva holosDistance 0.35");
        // Clamp documentado: el daemon limita holos_gamma a [0.5, 1.0]
        // (applyPendingPerceptual); el slider fusionado (0..2) solo es
        // efectivo en ese rango en Ruta B.
        {
            const std::string rc = handleCommand(
                "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosGamma\":2.0}",
                st);
            check(contains(rc, "\"ok\":true"), "fusion: gamma clamp ok");
            PendingPerceptual pc;
            check(takePendingPerceptual(st, pc), "fusion: gamma clamp take");
            applyPendingPerceptual(snap, pc);
            check(snap.holos_gamma == 1.0f,
                  "fusion: holosGamma 2.0 clampa a 1.0");
        }
    }

    std::printf("test_route_b_perceptual_state: %d pasan, %d fallan\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
