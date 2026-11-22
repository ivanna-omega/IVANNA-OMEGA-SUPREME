// test_effect_beacon.cpp -- barrera de regresion de la senal "omega_effect procesa".
//
// Cierra el hallazgo de AGENT_CLAIMS.md (doble procesamiento Ruta A+B): isDaemonRunning()
// no prueba que el efecto este insertado. Este test fija el contrato del beacon:
//   1. Clasificacion pura: NoEffect / EnabledIdle / Processing, ventana idleMs exacta y
//      datos de un arranque anterior (stamp en el futuro) => NoEffect, nunca Processing.
//      FIX 4 (v2.5.465): bloque reciente SIN enable => Processing (HAL Qualcomm con
//      auto-enable: enabled_count queda en 0 aunque el efecto procese; la fuente de
//      verdad son los frames avanzando, no enabled_count/consumer_gen).
//   2. Round-trip real escritor->lector sobre un backing file (mmap compartido):
//      sin archivo => Unavailable; open+enable => EnabledIdle; onBlock => Processing con
//      frames acumulados; disable => NoEffect; disable extra no hace underflow.
//   3. Instancias multiples: dos enable requieren dos disable.
//   4. Archivo con magic invalido => Unavailable (el llamador conserva su comportamiento).
// Compila en host sin NDK ni root.
#include "include/omega_effect_beacon.h"
#include "include/omega_control_bus.h"
#include "include/omega_effect_evidence.h"

#include <cstddef>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

using namespace ivanna;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } \
    else         { std::printf("ok:   %s\n", msg); } \
} while (0)

int main() {
    // 1. Clasificacion pura
    CHECK(classifyEffectBeacon(0, 0, 0, 10000, 2000) == EffectBeaconState::NoEffect, "enabled=0 -> NoEffect");
    CHECK(classifyEffectBeacon(1, 0, 9000, 10000, 2000) == EffectBeaconState::EnabledIdle, "enabled sin bloques -> EnabledIdle");
    CHECK(classifyEffectBeacon(1, 9000, 9000, 10000, 2000) == EffectBeaconState::Processing, "bloque reciente -> Processing");
    CHECK(classifyEffectBeacon(1, 8000, 8000, 10000, 2000) == EffectBeaconState::Processing, "borde exacto idleMs -> Processing");
    CHECK(classifyEffectBeacon(1, 7999, 7999, 10000, 2000) == EffectBeaconState::EnabledIdle, "pasado idleMs -> EnabledIdle");
    CHECK(classifyEffectBeacon(3, 90000, 90000, 5000, 2000) == EffectBeaconState::NoEffect, "stamp futuro (otro arranque) -> NoEffect");
    CHECK(classifyEffectBeacon(1, 90000, 100, 5000, 2000) == EffectBeaconState::NoEffect, "last_block futuro -> NoEffect");
    // FIX 4 (v2.5.465, vivo 2026-11-18): bloque reciente SIN enable = procesando.
    // En este HAL EFFECT_CMD_ENABLE jamás llega (auto-enable en omega_process) y
    // enabled_count queda en 0 con frames avanzando (0 -> 5.046.272). La telemetría
    // de Ruta B NO puede exigir enabled_count: el bloque fresco manda.
    CHECK(classifyEffectBeacon(0, 9000, 9000, 10000, 2000) == EffectBeaconState::Processing, "FIX4: bloque reciente sin enable (HAL auto-enable) -> Processing");
    CHECK(classifyEffectBeacon(0, 8000, 8000, 10000, 2000) == EffectBeaconState::Processing, "FIX4: borde exacto idleMs sin enable -> Processing");
    CHECK(classifyEffectBeacon(0, 7999, 7999, 10000, 2000) == EffectBeaconState::NoEffect, "FIX4: sin enable y bloque viejo -> NoEffect");
    CHECK(classifyEffectBeacon(0, 90000, 100, 5000, 2000) == EffectBeaconState::NoEffect, "FIX4: sin enable y last_block futuro -> NoEffect");
    CHECK(classifyEffectBeacon(0, 90000, 90000, 5000, 2000) == EffectBeaconState::NoEffect, "FIX4: sin enable y stamp futuro -> NoEffect");

    // 2. Round-trip real
    const char* tmp = std::getenv("TMPDIR");
    const std::string path = std::string(tmp && *tmp ? tmp : "/tmp") + "/omega_effect_beacon_test_" + std::to_string(::getpid());
    ::unlink(path.c_str());
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::Unavailable, "sin archivo -> Unavailable");

    EffectBeaconWriter w;
    CHECK(w.open(path.c_str()), "writer.open crea y mapea el archivo");
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::NoEffect, "abierto sin enable -> NoEffect");
    w.onEnable();
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::EnabledIdle, "enable sin audio -> EnabledIdle");
    w.onBlock(480, 17u);
    w.onBlock(480, 19u);
    uint64_t frames = 0;
    uint64_t appliedGeneration = 0;
    CHECK(readEffectBeacon(2000, path.c_str(), &frames, &appliedGeneration) == EffectBeaconState::Processing, "onBlock -> Processing");
    CHECK(frames == 960, "frames acumulados == 960");
    CHECK(appliedGeneration == 19u, "read-back refleja la última generación aplicada tras bloque DSP completo");
    w.onBlock(480, 20u);
    CHECK(readEffectBeacon(2000, path.c_str(), nullptr, &appliedGeneration) == EffectBeaconState::Processing && appliedGeneration == 20u,
          "generación aplicada avanza solo con un bloque DSP completado");

    // 3. Instancias multiples y sin underflow
    w.onEnable();
    w.onDisable();
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::Processing, "1 de 2 instancias deshabilitada -> sigue Processing");
    w.onDisable();
    // FIX 4: tras deshabilitar, el último bloque sigue fresco (ventana idleMs): el estado
    // es Processing TRANSITORIO (<= 2 s) y solo después cae a NoEffect. No es un fallo:
    // es la misma regla que hace visible el procesado del HAL sin enable (bloque fresco
    // manda sobre enabled_count). La telemetría muestra "procesando" un ciclo más, sin daño.
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::Processing, "FIX4: todas deshabilitadas con bloque fresco -> Processing transitorio (<=idleMs)");
    CHECK(classifyEffectBeacon(0, 9000, 9000, 12000, 2000) == EffectBeaconState::NoEffect, "FIX4: sin enable y bloque ya viejo -> NoEffect");
    w.onDisable();
    w.onDisable();
    w.onEnable();
    CHECK(readEffectBeacon(2000, path.c_str()) != EffectBeaconState::NoEffect, "enable tras disables extra -> habilitado");
    w.onDisable();
    // FIX 4: el último disable deja el contador en 0, pero el bloque sigue fresco, así
    // que el estado es Processing transitorio. El no-underflow se prueba esperando a que
    // expire la ventana idleMs: con contador en 0 el estado cae a NoEffect; si el
    // disable hubiera hecho underflow (contador 2^32-1) quedaría en EnabledIdle.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::NoEffect, "FIX4: tras expirar idleMs, contador == 0 -> NoEffect (disable extra no hizo underflow)");

    // 4. Un segundo proceso/escritor que reabre descarta contadores previos (primer open del proceso)
    EffectBeaconWriter w2;
    CHECK(w2.open(path.c_str()), "segundo writer abre el mismo archivo");
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::NoEffect, "reabrir reinicia enabled (audioserver reiniciado)");

    // 4b. Carrera de open(): N hilos hacen onEnable() a la vez sobre un writer nuevo; el contador final
    //     debe ser exactamente N (sin reinicios por doble mapeo).
    {
        const std::string rp = path + "_race";
        ::unlink(rp.c_str());
        EffectBeaconWriter wr;
        constexpr int kThreads = 8;
        std::vector<std::thread> th;
        for (int i = 0; i < kThreads; ++i) th.emplace_back([&] { if (wr.open(rp.c_str())) wr.onEnable(); });
        for (auto& t : th) t.join();
        for (int i = 0; i < kThreads - 1; ++i) wr.onDisable();
        CHECK(readEffectBeacon(2000, rp.c_str()) == EffectBeaconState::EnabledIdle, "carrera de open(): N enables concurrentes -> quedan N-(N-1)=1 habilitado");
        wr.onDisable();
        CHECK(readEffectBeacon(2000, rp.c_str()) == EffectBeaconState::NoEffect, "carrera de open(): contador exacto (0 tras N disables)");
        ::unlink(rp.c_str());
    }

    // 5. Magic invalido => Unavailable
    {
        FILE* f = std::fopen(path.c_str(), "r+b");
        if (f) { const uint32_t bad = 0xDEADBEEFu; std::fwrite(&bad, sizeof(bad), 1, f); std::fclose(f); }
    }
    CHECK(readEffectBeacon(2000, path.c_str()) == EffectBeaconState::Unavailable, "magic invalido -> Unavailable");

    // 6. Evidencia IVANNA (v2.5.380): campos omni_* añadidos AL FINAL, sin
    // mover offsets existentes y sin cambiar la versión (sigue 3): un lector
    // v3 (daemon, script viejo) los ignora. Barrera de regresión del
    // contrato append-only + round-trip publish/read con campos omni.
    static_assert(offsetof(EffectEvidence, magic) == 0, "ABI evidencia: magic se movio");
    static_assert(offsetof(EffectEvidence, version) == 4, "ABI evidencia: version se movio");
    static_assert(offsetof(EffectEvidence, seq) == 8, "ABI evidencia: seq se movio");
    static_assert(offsetof(EffectEvidence, hrtfCpuBlocks) == 16, "ABI evidencia: hrtfCpuBlocks se movio");
    static_assert(offsetof(EffectEvidence, rirLoads) == 48, "ABI evidencia: rirLoads se movio");
    static_assert(offsetof(EffectEvidence, controlGeneration) == 144, "ABI evidencia: controlGeneration se movio");
    static_assert(offsetof(EffectEvidence, adaptiveApplyBlocks) == 160, "ABI evidencia: adaptiveApplyBlocks se movio");
    static_assert(offsetof(EffectEvidence, omniProcessCalls) == 168, "ABI evidencia: omniProcessCalls no esta al final esperado");
    static_assert(offsetof(EffectEvidence, omniBlocksRendered) == 176, "ABI evidencia: omniBlocksRendered no esta al final esperado");
    static_assert(offsetof(EffectEvidence, omniFramesRendered) == 184, "ABI evidencia: omniFramesRendered no esta al final esperado");
    static_assert(offsetof(EffectEvidence, omniAppliedAmount) == 192, "ABI evidencia: omniAppliedAmount no esta al final esperado");
    static_assert(offsetof(EffectEvidence, omniFault) == 208, "ABI evidencia: omniFault no esta al final esperado");
    static_assert(sizeof(EffectEvidence) <= 256, "EffectEvidence excede 256 B");
    static_assert(OMEGA_EFFECT_EVIDENCE_VERSION == 3u, "la extension omni_* no debe subir la version (append-only)");
    {
        const std::string epath = path + ".evidence";
        ::unlink(epath.c_str());
        EvidencePublisher pub;
        CHECK(pub.open(epath.c_str()), "evidence: publisher.open crea y mapea el archivo");
        EffectEvidence ev{};
        ev.controlGeneration = 42u;
        ev.omniProcessCalls = 375u;
        ev.omniBlocksRendered = 375u;
        ev.omniFramesRendered = 48000u;
        ev.omniAppliedAmount = 1.0f;
        ev.omniConvergenceR = 0.964f;
        ev.omniSceneAzimuthDeg = 0.0f;
        ev.omniDiffuseness = 0.0f;
        ev.omniFault = 0u;
        pub.publish(ev);
        EffectEvidence back{};
        CHECK(readEffectEvidence(back, epath.c_str()), "evidence: round-trip publish/read con seqlock estable");
        CHECK(back.version == 3u, "evidence: version sigue 3 (lectores v3 no se rompen)");
        CHECK(back.controlGeneration == 42u, "evidence: campo viejo intacto tras la extension");
        CHECK(back.omniProcessCalls == 375u && back.omniBlocksRendered == 375u && back.omniFramesRendered == 48000u,
              "evidence: contadores omni sobreviven al round-trip");
        CHECK(back.omniAppliedAmount == 1.0f && back.omniConvergenceR == 0.964f && back.omniFault == 0u,
              "evidence: wet/r/fault omni sobreviven al round-trip");
        ::unlink(epath.c_str());
    }

    // 7. Parche A (0038+0040): open() sin O_CREAT sobre archivo pre-creado.
    // El daemon (root) pre-crea el beacon 0666; el efecto (audioserver, UID
    // no-root) debe abrirlo SIN O_CREAT (con O_CREAT, fs.protected_regular
    // da EACCES sobre un archivo ajeno en directorio grabable). El open no
    // debe fallar ni corromper el archivo: tras onEnable+onBlock el beacon
    // es Processing con frames y applied_control_generation creciendo.
    {
        const std::string pp = path + "_precreated";
        ::unlink(pp.c_str());
        // Simular el precreate del daemon: archivo 0666, tamaño exacto, ceros.
        {
            FILE* f = std::fopen(pp.c_str(), "wb");
            CHECK(f != nullptr, "parche A: precreate del daemon (fopen)");
            if (f) {
                std::vector<char> zeros(OMEGA_EFFECT_BEACON_SIZE, 0);
                CHECK(std::fwrite(zeros.data(), 1, zeros.size(), f) == zeros.size(),
                      "parche A: precreate deja tamaño exacto");
                std::fclose(f);
                ::chmod(pp.c_str(), 0666);
            }
        }
        struct stat st {};
        CHECK(::stat(pp.c_str(), &st) == 0 && st.st_size == (off_t)OMEGA_EFFECT_BEACON_SIZE,
              "parche A: archivo pre-creado con tamaño 72");
        EffectBeaconWriter wp;
        CHECK(wp.open(pp.c_str()), "parche A: open() sin O_CREAT abre el archivo pre-creado");
        CHECK(readEffectBeacon(2000, pp.c_str()) == EffectBeaconState::NoEffect,
              "parche A: abierto sin enable -> NoEffect");
        wp.onEnable();
        wp.onBlock(480, 17u);
        wp.onBlock(480, 19u);
        uint64_t framesA = 0, genA = 0;
        CHECK(readEffectBeacon(2000, pp.c_str(), &framesA, &genA) == EffectBeaconState::Processing,
              "parche A: onBlock sobre archivo pre-creado -> Processing");
        CHECK(framesA == 960, "parche A: frames acumulados == 960");
        CHECK(genA == 19u, "parche A: applied_control_generation == 19");
        wp.onBlock(480, 20u);
        CHECK(readEffectBeacon(2000, pp.c_str(), nullptr, &genA) == EffectBeaconState::Processing && genA == 20u,
              "parche A: generación aplicada avanza solo con bloque DSP completado");
        // Reabrir (segundo writer) sobre el mismo archivo pre-creado también funciona.
        EffectBeaconWriter wp2;
        CHECK(wp2.open(pp.c_str()), "parche A: segundo writer reabre el archivo pre-creado");
        ::unlink(pp.c_str());
    }

    // 8. 0043: las rutas de Ruta B viven en /data/adb/ivanna_omega/ (NO en
    // /data/local/tmp, donde el kernel las ve como `unlabeled` pese a chcon
    // ivanna_data_file — beacon 72 B en ceros, G85 v2.5.462). Las tres
    // comparten el mismo directorio padre etiquetado por service.sh; si
    // alguna vuelve a /data/local/tmp, este test lo caza antes que el teléfono.
    {
        const std::string dir = "/data/adb/ivanna_omega/";
        const std::string beaconPath(OMEGA_EFFECT_BEACON_PATH);
        const std::string busPath(OMEGA_EFFECT_LOCAL_BUS_PATH);
        const std::string evidencePath(OMEGA_EFFECT_EVIDENCE_PATH);
        CHECK(beaconPath == dir + "omega_effect_beacon", "0043: beacon bajo /data/adb/ivanna_omega/");
        CHECK(busPath == dir + "omega_effect_ctrl_local", "0043: bus local bajo /data/adb/ivanna_omega/");
        CHECK(evidencePath == dir + "omega_effect_evidence", "0043: evidencia bajo /data/adb/ivanna_omega/");
        CHECK(beaconPath.find("/data/local/tmp") == std::string::npos, "0043: beacon fuera de /data/local/tmp");
        CHECK(busPath.find("/data/local/tmp") == std::string::npos, "0043: bus fuera de /data/local/tmp");
        CHECK(evidencePath.find("/data/local/tmp") == std::string::npos, "0043: evidencia fuera de /data/local/tmp");
        CHECK(OMEGA_EFFECT_EVIDENCE_SIZE == static_cast<std::size_t>(256), "0043: evidencia 256 B (misma que precreate del daemon)");
    }

    ::unlink(path.c_str());
    std::printf(failures ? "RESULT: %d FAILED\n" : "RESULT: all passed\n", failures);
    return failures ? 1 : 0;
}
