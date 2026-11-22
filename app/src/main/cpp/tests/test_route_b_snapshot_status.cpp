// test_route_b_snapshot_status.cpp — GET_STATUS expone el snapshot (2026-11-14).
//
// Para que la app/doctor distingan "Ruta B neutra porque la app nunca
// publicó su estado" (snapshot makeDefault, ORBE 0 / HOLOS 0) de "neutra
// porque el usuario lo puso en 0", GET_STATUS publica 4 campos aditivos:
// snapshot_generation, perceptual_generation (nº de SET_PERCEPTUAL_STATE
// recibidos; 0 = la app nunca envió su estado), orbe_amount y
// holos_amount (amounts del snapshot publicado). Este test fija:
//   1. Estado fresco -> snapshot_generation 0, perceptual_generation 0,
//      orbe_amount 0.000, holos_amount 0.000 (neutro por ausencia).
//   2. Tras SET_PERCEPTUAL_STATE (ORBE 1.0) + stores del bucle ->
//      perceptual_generation 1, snapshot_generation 8, orbe 1.000,
//      holos 0.500 (valores del snapshot publicado, no del comando).
//   3. Los campos viejos que parseControlPlaneEvidence exige siguen
//      presentes (ok, generation, discovered_session_id).
//
// Umbrales fijados ANTES de medir: literales JSON exactos. Sin NDK.
#include <cstdio>
#include <string>

#include "../daemon/control/daemon_command_responder.h"

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

    // 1. Estado fresco: neutro por ausencia de la app.
    // 0044 (Fallo 3): "generation" es la del BUS (snapshotGeneration),
    // no la de fusión. En fresco nada se ha publicado: bus 0, aunque la
    // fusión vaya por 999.
    {
        CommandPlaneState st;
        st.sessionId.store(329);
        st.sessionGen.store(7);
        st.generation.store(999);
        const std::string r = buildStatusReply(st, 5000);
        check(contains(r, "\"ok\":true"), "fresco: ok true");
        check(contains(r, "\"generation\":0"), "fresco: generation 0 (bus, nada publicado)");
        check(contains(r, "\"discovered_session_id\":329"), "fresco: sesion 329");
        check(contains(r, "\"snapshot_generation\":0"), "fresco: snapshot_generation 0");
        check(contains(r, "\"perceptual_generation\":0"), "fresco: perceptual_generation 0");
        check(contains(r, "\"orbe_amount\":0.000"), "fresco: orbe 0.000");
        check(contains(r, "\"holos_amount\":0.000"), "fresco: holos 0.000");
    }

    // 2. Tras SET_PERCEPTUAL_STATE + publicación del bucle.
    {
        CommandPlaneState st;
        st.sessionId.store(329);
        st.sessionGen.store(7);
        st.generation.store(8);
        const std::string cmd = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1.0}", st);
        check(contains(cmd, "\"ok\":true"), "cmd: ok true");
        check(st.perceptualGen.load() == 1, "cmd: perceptualGen 1");
        // Lo que haría el bucle principal tras publicar el snapshot.
        st.snapshotGeneration.store(8);
        st.snapshotOrbeAmount.store(1.0f);
        st.snapshotHolosAmount.store(0.5f);
        const std::string r = buildStatusReply(st, 5000);
        check(contains(r, "\"snapshot_generation\":8"), "publicado: snapshot_generation 8");
        check(contains(r, "\"perceptual_generation\":1"), "publicado: perceptual_generation 1");
        check(contains(r, "\"orbe_amount\":1.000"), "publicado: orbe 1.000");
        check(contains(r, "\"holos_amount\":0.500"), "publicado: holos 0.500");
        // Los campos viejos siguen intactos.
        check(contains(r, "\"discovered_session_id\":329"), "publicado: sesion 329 sigue");
        check(contains(r, "\"effect_state\":"), "publicado: effect_state presente");
    }

    std::printf("test_route_b_snapshot_status: %d pasan, %d fallan\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
