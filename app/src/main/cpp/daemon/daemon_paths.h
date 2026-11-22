// daemon_paths.h — Rutas de estado del daemon v2 (una sola fuente).
//
// Los kill switches de Ruta B tienen DOS nombres parecidos y TRES rutas
// históricas; confundirlos deja un componente vivo cuando Esse cree haberlo
// apagado. Contrato vigente (2026-11-14):
//
//   Daemon v2 (cerebro) + ivanna_asrcd (servo):
//       /data/adb/ivanna_omega/pal_asr.off      (SIN la c)
//     - service.sh arranca asrcd con --kill $STATE/pal_asr.off.
//     - supervisor.sh vigila $STATE/pal_asr.off.
//     - ESTE daemon (antes) miraba /data/vendor/ivanna_audio/pal_asrc.off
//       (ruta del write-stage v1): crear pal_asr.off paraba el asrcd pero
//       el cerebro seguía publicando heartbeat/veredicto/snapshot.
//     - ivanna_daemon.off (mismo $STATE) impide arrancar el daemon.
//
//   Efecto (puente ASRC, binario en audioserver):
//       /data/adb/ivanna_omega/pal_asrc.off     (CON la c)
//     - kDefaultKillSwitch (ivanna_asrc_ipc.h): el puente lo consulta en
//       cada bloque; presente -> passthrough bit-exacto.
//
//   Write-stage v5 (HAL, .so inerte sin .rc):
//       /data/vendor/ivanna_audio/pal_asrc.off  (CON la c, otro directorio)
//     - Solo lo mira la .so del write-stage; el daemon v2 NO debe mirarlo.
//
// Estas constantes son la única fuente para el daemon v2; el test
// test_route_b_kill_switches fija que no se vuelvan a cruzar.
#pragma once

namespace ivanna::daemon {

// Directorio de estado compartido (Magisk, root).
inline constexpr const char* kDaemonStateDir = "/data/adb/ivanna_omega";

// Kill switch del daemon v2 (SIN la c): mismo archivo que asrcd y
// supervisor. Presente -> fusión en bypass (ratio 1.0, efectos off).
inline constexpr const char* kDaemonKillSwitchPath =
    "/data/adb/ivanna_omega/pal_asr.off";

// Kill switch de arranque: presente -> el daemon no arranca.
inline constexpr const char* kDaemonOffPath =
    "/data/adb/ivanna_omega/ivanna_daemon.off";

}  // namespace ivanna::daemon
