// test_route_b_asrc_bridge.cpp — Puente ASRC del efecto (2026-11-14).
//
// La suite del motor (test_ivanna_asrc.cpp, 25/25) cubre Core + IPC +
// servo, pero NUNCA llama a ivanna_asrc_bridge::process(): la función
// exacta que omega_process() ejecuta en cada bloque. Este test cierra ese
// hueco en el punto de integración real (engine/effect/ivanna_asrc_bridge.cpp):
//   1. Kill switch presente -> passthrough BIT-EXACTO (memcmp 0) y
//      retorno == frames. Es el bypass de emergencia sin reiniciar.
//   2. Ratio fresco 1.0001 (+100 ppm, deriva de bocina) -> la salida
//      DIFIERE de la entrada (maxDiff > 0), applied_ratio == 1.0001
//      exacto y el retorno consume la deriva (frames-64..frames).
//   3. Ratio rancio (>200 ms, timeout del Reader) -> fallback: applied
//      1.0 y fallback_events >= 1, sin crash y retorno == frames.
//   4. Sin archivo de ratio (sin daemon) -> fallback 1.0, sin crash.
//   5. Ratio inválido 2.0 (fuera del riel) -> applied 1.0, retorno frames.
//
// Umbrales fijados ANTES de medir: bit-exacto en kill switch,
// applied_ratio exacto, maxDiff > 0 con ratio 1.0001, fallback_events >= 1.
// Sin NDK, sin root: SHM en /tmp. El puente se reconfigura entre casos
// (configure() + shutdown()) porque tryInitOnce es de un solo intento.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>

#include "../asrc/ivanna_asrc_core.h"
#include "../asrc/ivanna_asrc_ipc.h"
#include "../effect/ivanna_asrc_bridge.h"

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

const char* kRatioPath = "/tmp/rb_bridge_ratio.shm";
const char* kFramesPath = "/tmp/rb_bridge_frames.shm";
const char* kKillPath = "/tmp/rb_bridge_kill.off";

constexpr size_t kFrames = 4800;  // 100 ms @ 48 kHz, estéreo
constexpr int kCh = 2;

void makeSine(int16_t* buf, size_t frames) {
    for (size_t n = 0; n < frames; ++n) {
        const double ph = 2.0 * 3.141592653589793 * 1000.0 *
                          static_cast<double>(n) / 48000.0;
        const int16_t v = static_cast<int16_t>(12000.0 * std::sin(ph));
        buf[2 * n] = v;
        buf[2 * n + 1] = v;
    }
}

int maxAbsDiff(const int16_t* a, const int16_t* b, size_t samples) {
    int m = 0;
    for (size_t i = 0; i < samples; ++i) {
        const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
        if (d > m) m = d;
    }
    return m;
}

void cleanupFiles() {
    ::unlink(kRatioPath);
    ::unlink(kFramesPath);
    ::unlink(kKillPath);
}

}  // namespace

int main() {
    using namespace ivanna::asrc;
    cleanupFiles();

    // 1. Kill switch -> bit-exacto.
    {
        ivanna_asrc_bridge::configure(kRatioPath, kFramesPath, kKillPath);
        FILE* f = std::fopen(kKillPath, "w");
        check(f != nullptr, "kill: se crea el archivo kill switch");
        if (f) std::fclose(f);
        static int16_t buf[kFrames * 2];
        static int16_t orig[kFrames * 2];
        makeSine(buf, kFrames);
        std::memcpy(orig, buf, sizeof(buf));
        const size_t ret = ivanna_asrc_bridge::process(buf, kFrames, kCh, 48000);
        check(ret == kFrames, "kill: retorno == frames");
        check(std::memcmp(buf, orig, sizeof(buf)) == 0,
              "kill: salida BIT-EXACTA (memcmp 0)");
        check(std::strcmp(ivanna_asrc_bridge::lastReason(), "kill_switch") == 0,
              "kill: reason kill_switch");
        ivanna_asrc_bridge::shutdown();
        ::unlink(kKillPath);
    }

    // 2. Ratio fresco +100 ppm -> sigue la deriva, salida difiere.
    {
        ivanna_asrc_bridge::configure(kRatioPath, kFramesPath, kKillPath);
        Writer w;
        check(w.open(kRatioPath), "ratio: Writer abre el SHM");
        static int16_t buf[kFrames * 2];
        static int16_t orig[kFrames * 2];
        // Calentamiento (priming del FIR): 3 bloques con ratio fresco.
        for (int i = 0; i < 3; ++i) {
            w.write(1.0001, 100.0, kFlagDaemonAlive, 0, monotonicNs());
            makeSine(buf, kFrames);
            ivanna_asrc_bridge::process(buf, kFrames, kCh, 48000);
        }
        w.write(1.0001, 100.0, kFlagDaemonAlive, 0, monotonicNs());
        makeSine(buf, kFrames);
        std::memcpy(orig, buf, sizeof(buf));
        const size_t ret = ivanna_asrc_bridge::process(buf, kFrames, kCh, 48000);
        const int diff = maxAbsDiff(buf, orig, kFrames * 2);
        const auto tel = ivanna_asrc_bridge::telemetry();
        check(ret >= kFrames - 64 && ret <= kFrames,
              "ratio: retorno consume la deriva (frames-64..frames)");
        check(diff > 0, "ratio: la salida DIFIERE de la entrada (maxDiff > 0)");
        check(std::fabs(tel.applied_ratio - 1.0001) < 1e-9,
              "ratio: applied_ratio == 1.0001 exacto");
        check(!tel.passthrough, "ratio: passthrough == false con ratio != 1");
        std::printf("      (ret=%zu maxDiff=%d applied=%.6f)\n", ret, diff,
                    tel.applied_ratio);
        ivanna_asrc_bridge::shutdown();
    }

    // 3. Ratio rancio (>200 ms) -> fallback 1.0.
    {
        ivanna_asrc_bridge::configure(kRatioPath, kFramesPath, kKillPath);
        Writer w;
        check(w.open(kRatioPath), "rancio: Writer abre el SHM");
        w.write(1.0001, 100.0, kFlagDaemonAlive, 0, monotonicNs());
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        static int16_t buf[kFrames * 2];
        makeSine(buf, kFrames);
        const size_t ret = ivanna_asrc_bridge::process(buf, kFrames, kCh, 48000);
        const auto tel = ivanna_asrc_bridge::telemetry();
        check(ret == kFrames, "rancio: retorno == frames");
        check(tel.applied_ratio == 1.0, "rancio: applied_ratio == 1.0");
        check(tel.fallback_events >= 1, "rancio: fallback_events >= 1");
        check(tel.passthrough, "rancio: passthrough == true en fallback");
        ivanna_asrc_bridge::shutdown();
    }

    // 4. Sin daemon (sin archivo de ratio) -> fallback 1.0, sin crash.
    {
        ::unlink(kRatioPath);
        ivanna_asrc_bridge::configure(kRatioPath, kFramesPath, kKillPath);
        static int16_t buf[kFrames * 2];
        makeSine(buf, kFrames);
        const size_t ret = ivanna_asrc_bridge::process(buf, kFrames, kCh, 48000);
        const auto tel = ivanna_asrc_bridge::telemetry();
        check(ret == kFrames, "sin-daemon: retorno == frames");
        check(tel.applied_ratio == 1.0, "sin-daemon: applied_ratio == 1.0");
        check(tel.fallback_events >= 1, "sin-daemon: fallback_events >= 1");
        ivanna_asrc_bridge::shutdown();
    }

    // 5. Ratio inválido 2.0 -> 1.0 (fuera del riel).
    {
        ivanna_asrc_bridge::configure(kRatioPath, kFramesPath, kKillPath);
        Writer w;
        check(w.open(kRatioPath), "invalido: Writer abre el SHM");
        w.write(2.0, 1000000.0, kFlagDaemonAlive, 0, monotonicNs());
        static int16_t buf[kFrames * 2];
        makeSine(buf, kFrames);
        const size_t ret = ivanna_asrc_bridge::process(buf, kFrames, kCh, 48000);
        const auto tel = ivanna_asrc_bridge::telemetry();
        check(ret == kFrames, "invalido: retorno == frames");
        check(tel.applied_ratio == 1.0, "invalido: applied_ratio == 1.0");
        ivanna_asrc_bridge::shutdown();
    }

    cleanupFiles();
    std::printf("test_route_b_asrc_bridge: %d pasan, %d fallan\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
