// test_route_b_kill_switches.cpp — Kill switches de Ruta B (2026-11-14).
//
// Los kill switches tienen DOS nombres parecidos y el daemon v2 miraba la
// ruta equivocada: ivanna_daemon.cpp comprobaba
// /data/vendor/ivanna_audio/pal_asrc.off (write-stage v1, HAL) mientras
// service.sh/supervisor/asrcd usan /data/adb/ivanna_omega/pal_asr.off
// (SIN la c). Crear pal_asr.off paraba el asrcd pero el cerebro seguía
// publicando heartbeat/veredicto/snapshot. Y el default de asrcd era
// pal_asrc.off: sin --kill, pal_asr.off no lo paraba. Este test fija el
// contrato (daemon/daemon_paths.h + engine/asrc/ivanna_asrc_ipc.h):
//   1. Kill switch del daemon v2 == /data/adb/ivanna_omega/pal_asr.off.
//   2. Kill switch del efecto (puente) == /data/adb/ivanna_omega/pal_asrc.off.
//   3. Son DISTINTOS por diseño (daemon sin c, efecto con c).
//   4. Default de asrcd == pal_asr.off (el del daemon).
//   5. killSwitchPresent(): archivo presente -> true, ausente -> false.
//   6. El off de arranque del daemon es ivanna_daemon.off en el mismo $STATE.
//
// Umbrales fijados ANTES de medir: literales de ruta exactos. Sin NDK.
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>

#include "../daemon/daemon_paths.h"
#include "asrc/ivanna_asrc_ipc.h"

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

}  // namespace

int main() {
    // 1-2. Rutas exactas.
    check(std::strcmp(ivanna::daemon::kDaemonKillSwitchPath,
                      "/data/adb/ivanna_omega/pal_asr.off") == 0,
          "daemon: kill switch es pal_asr.off (sin c)");
    check(std::strcmp(ivanna::asrc::kDefaultKillSwitch,
                      "/data/adb/ivanna_omega/pal_asrc.off") == 0,
          "efecto: kill switch es pal_asrc.off (con c)");
    check(std::strcmp(ivanna::daemon::kDaemonStateDir,
                      "/data/adb/ivanna_omega") == 0,
          "daemon: state dir es /data/adb/ivanna_omega");
    check(std::strcmp(ivanna::daemon::kDaemonOffPath,
                      "/data/adb/ivanna_omega/ivanna_daemon.off") == 0,
          "daemon: off es ivanna_daemon.off en $STATE");

    // 3. Distintos por diseño.
    check(std::strcmp(ivanna::daemon::kDaemonKillSwitchPath,
                      ivanna::asrc::kDefaultKillSwitch) != 0,
          "daemon y efecto usan archivos DISTINTOS (sin c vs con c)");

    // 4. Default de asrcd == el del daemon (no el del efecto).
    check(std::strcmp(ivanna::asrc::kDefaultDaemonKillSwitch,
                      ivanna::daemon::kDaemonKillSwitchPath) == 0,
          "asrcd: default es pal_asr.off (igual que el daemon)");
    check(std::strcmp(ivanna::asrc::kDefaultDaemonKillSwitch,
                      ivanna::asrc::kDefaultKillSwitch) != 0,
          "asrcd: default NO es pal_asrc.off");

    // 5. killSwitchPresent: presencia real en disco.
    const std::string tmp = "/tmp/rb_kill_switch_test_pal_asr.off";
    ::unlink(tmp.c_str());
    check(!ivanna::asrc::killSwitchPresent(tmp),
          "killSwitchPresent: ausente -> false");
    check(!ivanna::asrc::killSwitchPresent("/data/adb/ivanna_omega/pal_asr.off"),
          "killSwitchPresent: ruta del telefono ausente en host -> false");
    {
        FILE* f = std::fopen(tmp.c_str(), "w");
        check(f != nullptr, "killSwitchPresent: se crea el archivo de prueba");
        if (f) std::fclose(f);
    }
    check(ivanna::asrc::killSwitchPresent(tmp),
          "killSwitchPresent: presente -> true");
    ::unlink(tmp.c_str());
    check(!ivanna::asrc::killSwitchPresent(tmp),
          "killSwitchPresent: tras borrar -> false");

    std::printf("test_route_b_kill_switches: %d pasan, %d fallan\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
