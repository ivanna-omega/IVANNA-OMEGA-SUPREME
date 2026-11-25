// living_room_test.cpp — Tests host de P9 (simulador en host).
//
// Standalone (propio main(), sin gtest), mismo patrón que P6/P7/P8.
// Cubre lo que exige el plan (PLAN_P7_P10.md §4.3):
//   1. Modelo de sala del gemelo: volumen, Schroeder ~139 Hz, distancia
//      crítica ~0.82 m, modos axiales por posición.
//   2. Gate: el simulador predice el cambio de Δsala al mover al oyente
//      1 m; los valores esperados se calculan A MANO en los comentarios
//      (no se copian de la salida del programa).
//   3. Los 7 modos de la tabla del plan existen como datos (nombre,
//      objetivo, intervención) y ninguno se aplica como efecto.
//   4. Fail-closed: distancias inválidas no producen predicción.
#include "living_room.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ivanna::fused::living_room;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::printf("  PASS %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", name);
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

}  // namespace

int main() {
    RoomGeometry room;  // sala de Esse: 7.0 x 4.0 x 2.5(nominal), RT60 0.34
    std::printf("P9 living_room: sala %.1f x %.1f m, RT60 %.2f s\n",
                room.lengthM, room.widthM, room.rt60Sec);

    // ── 1. Volumen y constantes derivadas ────────────────────────────────
    {
        // V = 7.0 * 4.0 * 2.5 = 70 m³ (altura nominal 2.5 m, no medida).
        check(near(roomVolumeM3(room), 70.0, 1e-9), "volumen = 70 m3");
        // Schroeder = 2000*sqrt(0.34/70) = 2000*sqrt(0.004857)
        //           = 2000*0.06969 = 139.39 Hz (~139 Hz del plan).
        check(near(schroederHz(room), 139.39, 0.5),
              "Schroeder ~139 Hz (plan)");
        // Dc = 0.057*sqrt(70/0.34) = 0.057*sqrt(205.88)
        //    = 0.057*14.348 = 0.818 m (~0.82 m del plan).
        check(near(criticalDistanceM(room), 0.818, 0.01),
              "distancia critica ~0.82 m (plan)");
        RoomGeometry bad;
        bad.rt60Sec = 0.0;
        check(schroederHz(bad) == 0.0 && criticalDistanceM(bad) == 0.0,
              "RT60 0: Schroeder y Dc en 0 (fail-closed)");
    }

    // ── 2. Modos axiales ─────────────────────────────────────────────────
    {
        // c/2 = 171.5. Eje largo 7.0 m: 171.5/7 = 24.5, 49.0, 73.5 Hz.
        check(near(axialModeHz(room, 0, 1), 24.5, 1e-9),
              "modo axial largo n=1: 24.5 Hz");
        check(near(axialModeHz(room, 0, 2), 49.0, 1e-9),
              "modo axial largo n=2: 49.0 Hz");
        check(near(axialModeHz(room, 0, 3), 73.5, 1e-9),
              "modo axial largo n=3: 73.5 Hz");
        // Eje ancho 4.0 m: 171.5/4 = 42.875, 85.75 Hz.
        check(near(axialModeHz(room, 1, 1), 42.875, 1e-9),
              "modo axial ancho n=1: 42.875 Hz");
        check(near(axialModeHz(room, 1, 2), 85.75, 1e-9),
              "modo axial ancho n=2: 85.75 Hz");
        // Eje alto 2.5 m (nominal): 171.5/2.5 = 68.6, 137.2 Hz.
        check(near(axialModeHz(room, 2, 1), 68.6, 1e-9),
              "modo axial alto n=1: 68.6 Hz (altura nominal)");
        check(axialModeHz(room, 0, 0) == 0.0, "modo n=0: 0 (invalido)");
        check(axialModeHz(room, 3, 1) == 0.0, "eje 3: 0 (invalido)");
    }

    // ── 3. Gate P9: Δsala al mover al oyente 1 m ────────────────────────
    // Δ(d) = 10*log10(1 + (d/Dc)²), Dc = 0.8179 m.
    //   d=3.0: (3/0.8179)² = 13.452 → 10*log10(14.452) = 11.599 dB.
    //   d=4.0: (4/0.8179)² = 23.914 → 10*log10(24.914) = 13.965 dB.
    //   d=2.0: (2/0.8179)² = 5.979  → 10*log10(6.979)  = 8.438 dB.
    //   d=1.5: (1.5/0.8179)² = 3.363 → 10*log10(4.363) = 6.398 dB.
    //   d=4.5: (4.5/0.8179)² = 30.268 → 10*log10(31.268) = 14.951 dB.
    {
        RoomDeltaResult d3 = roomDeltaDb(room, 3.0);
        check(d3.valid && near(d3.deltaDb, 11.599, 0.01),
              "delta sala a 3 m (referencia): 11.60 dB");
        check(roomDeltaDb(room, 4.0).valid &&
                  near(roomDeltaDb(room, 4.0).deltaDb, 13.965, 0.01),
              "delta sala a 4 m: 13.97 dB");
        // Gate: mover 1 m alejándose (3 → 4 m) sube Δsala ~+2.37 dB.
        DeltaChangeResult away = predictDeltaChange(room, 3.0, 4.0);
        check(away.valid && near(away.changeDb, 2.366, 0.01),
              "gate P9: 3m -> 4m predice +2.37 dB");
        // Mover 1 m acercándose (3 → 2 m) baja Δsala ~−3.16 dB.
        DeltaChangeResult near_move = predictDeltaChange(room, 3.0, 2.0);
        check(near_move.valid && near(near_move.changeDb, -3.161, 0.01),
              "gate P9: 3m -> 2m predice -3.16 dB");
        // Posiciones de prueba del plan: 1.5 m y 4.5 m.
        DeltaChangeResult toNear = predictDeltaChange(room, 3.0, 1.5);
        check(toNear.valid && near(toNear.changeDb, -5.201, 0.01),
              "3m -> 1.5m predice -5.20 dB");
        DeltaChangeResult toFar = predictDeltaChange(room, 3.0, 4.5);
        check(toFar.valid && near(toFar.changeDb, 3.352, 0.01),
              "3m -> 4.5m predice +3.35 dB");
        // Monotonía: Δsala crece con la distancia (más reverberante).
        check(roomDeltaDb(room, 1.5).deltaDb <
                      roomDeltaDb(room, 3.0).deltaDb &&
                  roomDeltaDb(room, 3.0).deltaDb <
                      roomDeltaDb(room, 4.5).deltaDb,
              "delta sala monotono creciente con la distancia");
        // La predicción es simétrica en magnitud solo si se invierte el
        // sentido: 4 → 3 m es exactamente lo contrario de 3 → 4 m.
        DeltaChangeResult back = predictDeltaChange(room, 4.0, 3.0);
        check(back.valid && near(back.changeDb, -away.changeDb, 1e-12),
              "cambio inverso: signo opuesto exacto");
    }

    // ── 4. Los 7 modos como datos, no como efectos ──────────────────────
    {
        check(kNumModes == 7, "7 modos en la tabla del plan");
        bool allOk = true;
        for (std::size_t m = 0; m < kNumModes; ++m) {
            ModeInfo info = modeInfo(static_cast<RoomMode>(m));
            if (info.name[0] == '\0' || info.objective[0] == '\0' ||
                info.intervention[0] == '\0' ||
                !info.testableWithCurrentHardware) {
                allOk = false;
            }
        }
        check(allOk, "7 modos con nombre, objetivo e intervencion");
        check(std::strcmp(modeInfo(RoomMode::kReference).name,
                          "Referencia") == 0,
              "modo Referencia presente");
        check(std::strcmp(modeInfo(RoomMode::kDirector).name, "Director") ==
                  0,
              "modo Director presente");
        check(std::strcmp(modeInfo(RoomMode::kBlind).name, "Ciego") == 0,
              "modo Ciego presente");
        // El modo Histórico se declara recreación, no fidelidad.
        check(std::strstr(modeInfo(RoomMode::kHistorical).intervention,
                          "recreacion") != nullptr,
              "modo Historico: declarado recreacion, no fidelidad");
        // El modo Referencia exige confianza >= 0.80 (umbral de Luna).
        check(std::strstr(modeInfo(RoomMode::kReference).intervention,
                          "0.80") != nullptr,
              "modo Referencia: exige confianza >= 0.80");
        check(kReferenceListenerM == 3.0 && kNearListenerM == 1.5 &&
                  kFarListenerM == 4.5,
              "posiciones de escucha: 3.0 / 1.5 / 4.5 m");
    }

    // ── 5. Fail-closed ───────────────────────────────────────────────────
    {
        check(!roomDeltaDb(room, 0.0).valid, "distancia 0: invalida");
        check(!roomDeltaDb(room, -1.0).valid, "distancia negativa: invalida");
        check(!roomDeltaDb(room, std::nan("")).valid, "distancia NaN: invalida");
        check(!predictDeltaChange(room, 3.0, 0.0).valid,
              "cambio a distancia 0: invalido");
        check(!predictDeltaChange(room, std::nan(""), 4.0).valid,
              "cambio desde NaN: invalido");
    }

    std::printf("P9: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
