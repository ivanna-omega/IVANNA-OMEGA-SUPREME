// daemon_command_responder.h — Servidor MÍNIMO de comandos del daemon v2.
//
// CAUSA RAÍZ (v2.5.440, verificado en vivo 2026-11-13): la app acopla el
// efecto omega con tryAttachDiscoveredSession → GET_STATUS por el socket
// ABSTRACTO @omega_daemon_socket. El daemon v2 (CEREBRO) bindeaba ese
// socket en bindDaemonSocket() pero NADIE aceptaba conexiones: main()
// guardaba el fd y el bucle principal lo drenaba con accept()+close()
// inmediato ("nadie debería conectar (no hay servidor de comandos)").
// El connect de la app entraba al backlog, GET_STATUS jamás se respondía,
// la app logueaba "GET_STATUS sin respuesta/JSON inválido" cada 3 s y el
// efecto nunca se creaba en audioserver (0 mapeos, asrc_frames.bin jamás
// existió, ivanna_asrcd en ratio 1.0). La reescritura a CEREBRO v2 (2.5.401)
// eliminó el CommandServer viejo y nunca se repuso.
//
// ESTE ARCHIVO lo repone en mínimo, fail-closed, SIN resucitar el
// CommandServer viejo (que arrastra shm_manager, snapshots, user flags y
// 800+ líneas de estado que el cerebro v2 no mantiene — dos escritores del
// mismo snapshot serían un riesgo mayor que el hueco que se cierra):
//   - Un hilo propio hace accept() sobre el socket abstracto YA bindeado
//     por el daemon; el bucle de fusión (~20 Hz) nunca se bloquea.
//   - Un comando por conexión, luego close (igual que el servidor viejo y
//     que el cliente de la app espera: exchange() abre conexión efímera
//     por comando).
//   - HELLO (handshake, proto >= 2) y GET_STATUS (sesión descubierta viva
//     + estado real del efecto por el beacon).
//   - SET_PERCEPTUAL_STATE (alias SEND_PERCEPTUAL_STATE): el canal
//     principal por el que la app envía sus ajustes (ORBE/HOLOS/upmixing/
//     EQ) al daemon. El hilo de comandos hace stash del comando; el bucle
//     principal lo aplica al snapshot antes de publicar.
//   - 0044: el protocolo COMPLETO del servidor viejo (Fallo 1): PING,
//     GET_CLOCK_TELEMETRY, GET_PROCESSOR_EVIDENCE, GET_TELEMETRY y los
//     SET_* (ROOM_RT60, AUX_FIELD, BYPASS, EQ_BANDS, WFS, WFS_LAYOUT, 4D,
//     PF_PARAMS, HEAD_POSE, SCENE_POSE, INTENSITY, HARMONIC_GAIN,
//     ADAPTIVE_STATE, SAF, AUDIO_FEATURES, PUNCH, SUPREME_AXES (parcial:
//     solo harmonicGain tiene slot en ABI v13), USER_FLAGS, NPE,
//     TEMPORAL_FIELD, XTC/CROSSTALK, ROOM_CORRECTION). Además los comandos
//     de TEXTO de MagiskBridge (SET_BYPASS:1, STATUS, GET_TELEMETRY,
//     SET_PF_<NOMBRE>:<v>, RELOAD_PARAMS). v2.5.486 (ABI v16): se portan
//     los 16 restantes del handler muerto (GET_AUDIO_FEATURES_STATUS,
//     GET_HEAD_POSE_STATUS, GET_HEALTH, GET_ROOM_STATUS, PUSH_YAMNET_SCORES,
//     SET_COCHLEAR_INTENSITY, SET_COCHLEAR_INVERSE_ENABLED,
//     SET_FOTOHOLO_STATE, SET_HIRES, SET_PINNA_METRICS, SET_PRESET,
//     SET_REALITY_RECONSTRUCTION, SET_ROUTE_PROFILE, SET_SPSC_RING_FACTOR,
//     SET_UPMIXING, SET_YAMNET_SCORES; SET_PRESET también por texto, con
//     llamador vivo MagiskBridge.setPreset). Sin soporte queda solo
//     SET_REVERB: (texto legacy sin llamador ni handler en el servidor
//     viejo); responde {"ok":false,"error":"unsupported"} para que la app
//     no confunda "sin respuesta" con "daemon muerto".
//   - Peer auth igual que el servidor viejo (peer_auth.hpp: root, shell
//     o el UID de la app). Sin ella, cualquier app podría preguntar.
//   - Conexión silenciosa (sin datos en 150 ms): se cierra sin responder.
//     La "Modo B" (fd por SCM_RIGHTS) NUNCA se implementó en el daemon
//     (ver shm_hyperplane.cpp: "nunca hubo sendmsg en el daemon") y ningún
//     cliente vivo la espera; el SHM viaja por backing file.
//   - Si el hilo muere por cualquier motivo, el proceso NO muere: la
//     fusión y el descubrimiento siguen (ratio 1.0, fail-closed).
//
// EL SOCKET ES EL ABSTRACTO (@omega_daemon_socket). No se crea ningún
// socket de archivo. /data/adb/ivanna_omega/omega_effect_ctrl_local es OTRO
// canal (bus local de telemetría del efecto, lo escribe el efecto en
// audioserver) y no se toca aquí.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include "include/asrc_state_bridge.hpp"
#include "include/ivanna_fusion_state.h"
#include "include/omega_effect_beacon.h"
#include "include/omega_effect_evidence.h"
#include "include/write_stage_asrc_telemetry.hpp"
#include "ivanna_hires_config.hpp"
#include "peer_auth.hpp"
#include "perceptual_state.h"

namespace ivanna::daemon {

// Versión del protocolo de comandos (HELLO). Igual que OMEGA_PROTO_VERSION
// del CommandServer viejo (command_server.h). La app exige proto >= 2
// (MIN_PROTO_VERSION en OmegaEngineBridge.kt).
inline constexpr uint32_t kDaemonProtoVersion = 3u;

// Estado vivo que el hilo de comandos lee. El bucle principal del daemon
// lo actualiza cada iteración con stores atómicos; el hilo solo lee.
// (No es copiable por los atómicos: se pasa siempre por referencia.)
struct CommandPlaneState {
    std::atomic<int32_t> sessionId{0};
    std::atomic<uint64_t> sessionGen{0};
    // FASE A (2026-11-18): package de la sesión descubierta + app en
    // foreground. Los escribe el bucle principal en el ciclo de
    // descubrimiento (~3 s) y los lee GET_STATUS; nunca se tocan en el hot
    // path de audio. Protegidos por sessionPackageMutex (no por atómicos:
    // son strings). La app los usa para expirar la guarda de huérfano al
    // cambiar de app y para no re-acoplar a una sesión rancia de fondo.
    // "" = desconocido (daemon viejo o /proc ilegible): la app conserva el
    // comportamiento anterior.
    mutable std::mutex sessionPackageMutex;
    mutable std::string sessionPackage;
    mutable std::string foregroundPackage;
    std::atomic<uint64_t> generation{0};
    std::atomic<uint32_t> verdict{IVANNA_VERDICT_BYPASS};
    std::atomic<uint64_t> startMs{0};
    std::atomic<uint64_t> clientsServed{0};
    // PID de la app IVANNA que abrió el socket (peer creds). El bucle
    // principal lo vigila: si muere (forzar cierre), limpia
    // desiredPerceptual y vuelve el snapshot a neutro (Fallo 2).
    std::atomic<int32_t> appPid{0};

    // SET_PERCEPTUAL_STATE pendiente (hilo de comandos -> bucle principal).
    // `mutable` porque handleCommand recibe const CommandPlaneState&: el
    // hilo solo hace stash (merge bajo mutex); el bucle principal hace
    // take (vacía) y aplica al snapshot antes de publicar. El mutex es el
    // único lock del plano de comandos y nunca se toma en el bucle de
    // fusión salvo para este drenaje (una copia por iteración como mucho).
    mutable std::mutex pendingMutex;
    mutable PendingPerceptual pendingPerceptual;
    // Estado DESEADO por la app (acumulado, nunca vaciado por el bucle):
    // sobrevive al cambio de app de música (Fallo 2). takePending lo
    // vacía; este no. Se limpia solo si la app muere (clearOnAppExit).
    mutable PendingPerceptual desiredPerceptual;
    // Nº de SET_PERCEPTUAL_STATE recibidos (0 = la app nunca envió su
    // estado: el snapshot publicado es makeDefault() neutro). Lo lee
    // GET_STATUS para que el doctor distinga "neutro por diseño" de
    // "neutro porque la app no publicó".
    mutable std::atomic<uint64_t> perceptualGen{0};

    // Último snapshot publicado al SHM (lo escribe el bucle principal).
    // orbe/holos == 0 con perceptualGen == 0 significa "la app nunca
    // publicó su estado"; con perceptualGen > 0 significa "la app lo
    // puso en 0" (decisión del usuario, no un fallo).
    std::atomic<uint64_t> snapshotGeneration{0};
    std::atomic<float> snapshotOrbeAmount{0.f};
    std::atomic<float> snapshotHolosAmount{0.f};
};

// Guarda un comando perceptual (merge: los campos presentes sobrescriben,
// los ausentes conservan el pendiente anterior). La generación se asigna
// aquí, bajo el mismo lock. No bloquea el bucle de fusión: el mutex solo
// se contiende con el drenaje del bucle (una vez por iteración).

// FASE A: publica los packages de la sesión descubierta / foreground
// (bucle principal -> hilo de comandos). Solo se escribe en el ciclo de
// descubrimiento (~3 s); GET_STATUS copia bajo el mismo lock.
inline void setDiscoveredSessionPackages(const CommandPlaneState& st,
                                         const std::string& pkg,
                                         const std::string& fg) noexcept {
    std::lock_guard<std::mutex> lk(st.sessionPackageMutex);
    st.sessionPackage = pkg;
    st.foregroundPackage = fg;
}

inline void copyDiscoveredSessionPackages(const CommandPlaneState& st,
                                          std::string& pkg, std::string& fg) noexcept {
    std::lock_guard<std::mutex> lk(st.sessionPackageMutex);
    pkg = st.sessionPackage;
    fg = st.foregroundPackage;
}
inline uint64_t stashPendingPerceptual(const CommandPlaneState& st,
                                       const PendingPerceptual& p) noexcept {
    std::lock_guard<std::mutex> lk(st.pendingMutex);
    st.pendingPerceptual.merge(p);
    st.desiredPerceptual.merge(p);
    const uint64_t g =
        st.perceptualGen.fetch_add(1, std::memory_order_relaxed) + 1;
    st.pendingPerceptual.gen = g;
    st.desiredPerceptual.gen = g;
    return g;
}

// Vacía el pendiente (el bucle principal lo aplica al snapshot). false =
// no había nada pendiente.
inline bool takePendingPerceptual(CommandPlaneState& st,
                                  PendingPerceptual& out) noexcept {
    std::lock_guard<std::mutex> lk(st.pendingMutex);
    if (!st.pendingPerceptual.any()) return false;
    out = st.pendingPerceptual;
    st.pendingPerceptual = PendingPerceptual{};
    return true;
}

// Copia el estado DESEADO acumulado (no lo vacía). El bucle principal lo
// usa para re-sembrar el snapshot neutro al cambiar de sesión (Fallo 2):
// los efectos del usuario sobreviven al cambio de app de música.
inline bool copyDesiredPerceptual(const CommandPlaneState& st,
                                  PendingPerceptual& out) noexcept {
    std::lock_guard<std::mutex> lk(st.pendingMutex);
    if (!st.desiredPerceptual.any()) return false;
    out = st.desiredPerceptual;
    return true;
}

// v2.5.472 (Fase 2, ítem E): escala de parallax vigente para la pose de
// Ruta B. La guarda el deseado acumulado (SET_PERCEPTUAL_STATE la trae
// desde SpatialControls.parallaxAmount); un comando de pose puede
// sobreescribirla en el mismo envío. Ausente en ambos = 1.0
// (comportamiento actual). El snapshot ABI v13 NO cambia: la escala se
// aplica al yaw/pitch al parsear, antes del stash.
inline float headPoseParallaxScale(const CommandPlaneState& st,
                                   float commandParallax) noexcept {
    if (!std::isnan(commandParallax))
        return std::clamp(commandParallax, 0.0f, 1.0f);
    PendingPerceptual desired;
    if (copyDesiredPerceptual(st, desired) && !std::isnan(desired.parallaxAmount))
        return std::clamp(desired.parallaxAmount, 0.0f, 1.0f);
    return 1.0f;
}

// La app murió (forzar cierre / crash): limpiar el estado deseado para
// no dejar bofo colgado sobre Tidal/Qobuz. El bucle principal vuelve el
// snapshot a neutro en la misma iteración (ivanna_daemon.cpp).
inline void clearOnAppExit(const CommandPlaneState& st) noexcept {
    std::lock_guard<std::mutex> lk(st.pendingMutex);
    st.desiredPerceptual = PendingPerceptual{};
}

// Texto de estado para GET_STATUS, derivado del veredicto del cerebro.
// (El beacon puede sobreescribirlo cuando el efecto procesa de verdad.)
inline const char* verdictStatus(uint32_t v) noexcept {
    switch (v) {
        case IVANNA_VERDICT_LOADED: return "loaded";
        case IVANNA_VERDICT_OFF: return "off";
        case IVANNA_VERDICT_NO_CLOCK: return "no_clock";
        case IVANNA_VERDICT_STALE: return "stale";
        case IVANNA_VERDICT_BYPASS: default: return "bypass";
    }
}

// Vista del estado del efecto, pura (testeable en host, sin I/O).
struct StatusView {
    int effectState;            // EffectBeaconState como int (-1..2)
    const char* status;         // texto para GET_STATUS
    const char* consumer;       // "omega_effect" o "null"
    uint64_t dspFrames;
    uint64_t dspAppliedGen;
};

// Mapeo beacon -> vista de estado. Regla del servidor viejo: el snapshot
// solo cuenta como aplicado cuando el efecto procesa Y su generación
// aplicada alcanza la generación publicada por el daemon.
inline StatusView computeStatusView(uint64_t generation, EffectBeaconState beacon,
                                    uint64_t dspFrames, uint64_t dspAppliedGen,
                                    uint32_t verdict) noexcept {
    StatusView v{};
    v.effectState = static_cast<int>(beacon);
    v.dspFrames = dspFrames;
    v.dspAppliedGen = dspAppliedGen;
    const bool processing = (beacon == EffectBeaconState::Processing);
    const bool idle = (beacon == EffectBeaconState::EnabledIdle);
    v.consumer = (processing || idle) ? "omega_effect" : "null";
    if (processing && generation > 0 && dspAppliedGen >= generation) {
        v.status = "dsp_block_applied";
    } else if (processing) {
        v.status = "awaiting_dsp_generation";
    } else if (idle) {
        v.status = "effect_enabled_idle";
    } else {
        v.status = verdictStatus(verdict);
    }
    return v;
}

// Extrae el valor de "action" de un JSON compacto (el que manda la app).
// No es un parser JSON general: el único cliente es OmegaEngineBridge,
// que manda {"action":"..."} compacto. Devuelve false si no hay acción.
inline bool extractAction(const std::string& req, std::string& action) noexcept {
    const std::size_t k = req.find("\"action\"");
    if (k == std::string::npos) return false;
    const std::size_t colon = req.find(':', k + 8);
    if (colon == std::string::npos) return false;
    const std::size_t q1 = req.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    const std::size_t q2 = req.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    action = req.substr(q1 + 1, q2 - q1 - 1);
    return !action.empty();
}

// Lee un número tras "key" en el JSON. Fallo 5: el viejo usaba strtod
// directo: "bypass":true daba 0.0 (igual que false) y "bypass":NaN daba
// NaN. Ahora: true/false -> 1/0, y lo no finito -> default.
inline float jsonNumber(const std::string& req, const char* key, float dflt) noexcept {
    const std::string pat = std::string("\"") + key + "\"";
    const std::size_t k = req.find(pat);
    if (k == std::string::npos) return dflt;
    const std::size_t colon = req.find(':', k + pat.size());
    if (colon == std::string::npos) return dflt;
    const char* p = req.c_str() + colon + 1;
    while (*p == ' ' || *p == '\t') ++p;
    if (std::strncmp(p, "true", 4) == 0) return 1.f;
    if (std::strncmp(p, "false", 5) == 0) return 0.f;
    const float v = static_cast<float>(std::strtod(p, nullptr));
    return std::isfinite(v) ? v : dflt;
}

// Lee un array de N floats tras "key" ([1.0, 2.0, ...]). exact=true exige
// los N valores; exact=false acepta al menos 1 (rellena el resto con 0).
// Devuelve false si la clave no existe o el array no parsea.
inline bool jsonFloatArray(const std::string& req, const char* key, float* out,
                           int count, bool exact) noexcept {
    const std::string pat = std::string("\"") + key + "\"";
    const std::size_t k = req.find(pat);
    if (k == std::string::npos) return false;
    const std::size_t colon = req.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    const std::size_t open = req.find('[', colon + 1);
    if (open == std::string::npos) return false;
    const char* p = req.c_str() + open + 1;
    int n = 0;
    while (n < count) {
        while (*p == ' ' || *p == '\t' || *p == ',' || *p == '\n' || *p == '\r') ++p;
        if (*p == ']' || *p == '\0') break;
        char* end = nullptr;
        const double v = std::strtod(p, &end);
        if (end == p) return false;
        if (!std::isfinite(v)) return false;
        out[n++] = static_cast<float>(v);
        p = end;
    }
    if (exact && n != count) return false;
    if (!exact && n == 0) return false;
    for (int i = n; i < count; ++i) out[i] = 0.f;
    return true;
}

// Respuesta HELLO. Misma forma que el servidor viejo (command, proto,
// compatible, status); sin shm_version/ctrl_version: el cerebro v2 no es
// dueño de esos layouts. La app solo exige proto >= 2.
inline std::string buildHelloReply(uint32_t clientProto) {
    const bool compatible = clientProto >= 1 && clientProto <= kDaemonProtoVersion;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"command\":\"HELLO\",\"proto\":%u,\"daemon\":\"ivanna_daemon_v2\","
        "\"compatible\":%s,\"status\":\"%s\"}",
        kDaemonProtoVersion,
        compatible ? "true" : "false",
        compatible ? "ready" : "proto_mismatch");
    return std::string(buf);
}

// Respuesta GET_STATUS. Campos que parseControlPlaneEvidence (Kotlin)
// exige: ok, generation, dsp_applied_generation, dsp_frames, effect_state,
// status, consumer, upmixing_enabled/immersivity, wfs_enabled/spread,
// discovered_session_id/gen. Los campos de efecto son REALES (beacon del
// efecto, no inventados): sin beacon, effect_state=-1 y status=veredicto.
// upmixing/wfs son 0.0 honestos: el cerebro v2 no los gestiona (los aplica
// el efecto desde el snapshot de la app, no desde el daemon).
// Campos NUEVOS (aditivos, 2026-11-14): snapshot_generation (gen del
// último snapshot publicado), perceptual_generation (nº de
// SET_PERCEPTUAL_STATE recibidos; 0 = la app nunca publicó su estado y el
// snapshot es makeDefault neutro), orbe_amount / holos_amount (amounts del
// snapshot publicado). El parse Kotlin de estos 4 campos está PENDIENTE
// (MagiskBridge.kt, fuera del dominio Ruta B C++); el doctor los leerá
// cuando el parse exista.
inline std::string buildStatusReply(const CommandPlaneState& st, uint64_t nowMs,
                                    const char* beaconPath = OMEGA_EFFECT_BEACON_PATH) {
    const int32_t sid = st.sessionId.load(std::memory_order_relaxed);
    const uint64_t sgen = st.sessionGen.load(std::memory_order_relaxed);
    // 0044 (Fallo 3): "generation" es la del BUS (la que el efecto aplica),
    // no la de fusión (20 Hz). Con la de fusión, GET_STATUS quedaba
    // atascado en "awaiting_dsp_generation" porque dspAppliedGen nunca la
    // alcanzaba. st.generation ya no se usa aquí (la mantiene el bucle
    // para telemetría interna, no para el protocolo).
    const uint32_t verdict = st.verdict.load(std::memory_order_relaxed);
    const uint64_t startMs = st.startMs.load(std::memory_order_relaxed);
    const uint64_t uptime = (nowMs >= startMs) ? (nowMs - startMs) : 0;
    const uint64_t clients = st.clientsServed.load(std::memory_order_relaxed);
    const uint64_t snapGen = st.snapshotGeneration.load(std::memory_order_relaxed);
    const uint64_t percGen = st.perceptualGen.load(std::memory_order_relaxed);
    const float orbeAmt = st.snapshotOrbeAmount.load(std::memory_order_relaxed);
    const float holosAmt = st.snapshotHolosAmount.load(std::memory_order_relaxed);
    uint64_t dspFrames = 0, dspAppliedGen = 0;
    const EffectBeaconState beacon =
        readEffectBeacon(2000ull, beaconPath, &dspFrames, &dspAppliedGen);
    const StatusView view = computeStatusView(snapGen, beacon, dspFrames, dspAppliedGen, verdict);
    // FASE A: discovered_session_package + foreground_package (saneados
    // [A-Za-z0-9._] en session_discovery.hpp: el %s no necesita escape).
    std::string sessionPkg, foregroundPkg;
    copyDiscoveredSessionPackages(st, sessionPkg, foregroundPkg);
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"command\":\"GET_STATUS\",\"status\":\"%s\",\"generation\":%llu,"
        "\"route\":\"SYSTEM_WIDE\",\"consumer\":%s,\"effect_state\":%d,"
        "\"dsp_applied_generation\":%llu,\"dsp_frames\":%llu,\"error\":null,"
        "\"upmixing_enabled\":0.0,\"upmixing_immersivity\":0.0,"
        "\"wfs_enabled\":0.0,\"wfs_spread\":0.0,"
        "\"discovered_session_id\":%d,\"discovered_session_gen\":%llu,"
        "\"discovered_session_package\":\"%s\",\"foreground_package\":\"%s\","
        "\"snapshot_generation\":%llu,\"perceptual_generation\":%llu,"
        "\"orbe_amount\":%.3f,\"holos_amount\":%.3f,"
        "\"uptime_ms\":%llu,\"clients_served\":%llu}",
        view.status,
        (unsigned long long)snapGen,
        (std::strcmp(view.consumer, "omega_effect") == 0) ? "\"omega_effect\"" : "null",
        view.effectState,
        (unsigned long long)view.dspAppliedGen,
        (unsigned long long)view.dspFrames,
        static_cast<int>(sid), (unsigned long long)sgen,
        sessionPkg.c_str(), foregroundPkg.c_str(),
        (unsigned long long)snapGen, (unsigned long long)percGen,
        static_cast<double>(orbeAmt), static_cast<double>(holosAmt),
        (unsigned long long)uptime, (unsigned long long)clients);
    return std::string(buf);
}

// GET_TELEMETRY (texto, panel Magisk + OmegaDaemon): sin fuente viva para
// temperatura/latencia/frames/SR en v2 — centinelas -1 (como el servidor
// viejo), nunca datos inventados. bypass sale de desiredPerceptual (lo
// último que la app pidió); generation es la del bus (Fallo 3).
// OmegaDaemon.parseField acepta JSON ("temp":-1.0) igual que texto.
inline std::string buildTelemetryReply(const CommandPlaneState& st) noexcept {
    const uint64_t snapGen = st.snapshotGeneration.load(std::memory_order_relaxed);
    const uint32_t verdict = st.verdict.load(std::memory_order_relaxed);
    const int32_t sid = st.sessionId.load(std::memory_order_relaxed);
    const uint64_t clients = st.clientsServed.load(std::memory_order_relaxed);
    PendingPerceptual desired;
    const bool haveDesired = copyDesiredPerceptual(st, desired);
    const int bypass = (haveDesired && !std::isnan(desired.masterBypass) &&
                        desired.masterBypass >= 0.5f) ? 1 : 0;
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"command\":\"GET_TELEMETRY\",\"temp\":-1.0,\"latency\":-1.0,"
        "\"frames\":-1,\"bypass\":%d,\"samplerate\":-1,\"version\":\"unknown\","
        "\"generation\":%llu,\"status\":\"%s\",\"discovered_session_id\":%d,"
        "\"clients_served\":%llu}",
        bypass, (unsigned long long)snapGen, verdictStatus(verdict),
        static_cast<int>(sid), (unsigned long long)clients);
    return std::string(buf);
}

// Nombres PF (texto SET_PF_<NOMBRE>:<v>) -> slot. Del servidor viejo
// (pfIndexForName, command_server.cpp:297): DRIVE=0, WET=1, MIX=2, ALPHA=3,
// BETA=4, GAMMA=5, FREQ=6, RESONANCE=7, LOW=8, MID=9, HIGH=10, PRESENCE=11,
// MASTER=12.
inline int pfIndexForName(const std::string& k) noexcept {
    static const char* names[13] = {"DRIVE", "WET", "MIX", "ALPHA", "BETA", "GAMMA",
                                    "FREQ", "RESONANCE", "LOW", "MID", "HIGH",
                                    "PRESENCE", "MASTER"};
    for (int i = 0; i < 13; ++i) if (k == names[i]) return i;
    return -1;
}

// Lee un string tras "key" en el JSON compacto ("preset":"Warm").
// Devuelve false si la clave no existe o el valor no es un string.
inline bool jsonString(const std::string& req, const char* key, std::string& out) noexcept {
    const std::string pat = std::string("\"") + key + "\"";
    const std::size_t k = req.find(pat);
    if (k == std::string::npos) return false;
    const std::size_t colon = req.find(':', k + pat.size());
    if (colon == std::string::npos) return false;
    const std::size_t q1 = req.find('"', colon + 1);
    if (q1 == std::string::npos) return false;
    const std::size_t q2 = req.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    out = req.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

// Presets del servidor viejo (applyPresetByName, command_server.cpp:262),
// portados a PendingPerceptual (el daemon v2 no tiene estado propio: el
// preset se stashea y el bucle lo aplica al snapshot). Los nombres son los
// de MagiskBridge.setPreset ("Spatial", "Warm") y los del servidor viejo
// ("ivanna omega", "spatial", "warm", "flat"/"neutral"); el match es
// insensible a mayúsculas y a espacios. Devuelve false si el nombre no
// existe (el llamador responde unknown_preset). "ivanna omega" NO toca
// dialogBoost/widener (el servidor viejo tampoco): se conservan.
inline bool applyPresetToPending(PendingPerceptual& p, const std::string& name) noexcept {
    std::string n;
    for (char c : name) {
        if (c == ' ' || c == '\t') continue;
        n.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    if (n == "ivannaomega") {
        p.spatialWidth = 1.55f; p.harmonicGain = 0.78f; p.compressor = -5.5f;
        p.highCutHz = 19500.f; p.antiDolby = 0.85f; p.loudnessTarget = -16.f;
        p.bassBoostDb = 0.f;
        return true;
    }
    if (n == "spatial") {
        p.spatialWidth = 2.2f; p.harmonicGain = 0.55f; p.compressor = -4.f;
        p.highCutHz = 19500.f; p.antiDolby = 0.85f; p.loudnessTarget = -16.f;
        p.bassBoostDb = 0.f; p.dialogBoostDb = 0.f; p.widenerMult = 1.4f;
        return true;
    }
    if (n == "warm") {
        p.spatialWidth = 1.1f; p.harmonicGain = 0.4f; p.compressor = -4.f;
        p.highCutHz = 19000.f; p.antiDolby = 0.85f; p.loudnessTarget = -16.f;
        p.bassBoostDb = 3.f; p.dialogBoostDb = 1.f; p.widenerMult = 1.1f;
        return true;
    }
    if (n == "flat" || n == "neutral") {
        p.spatialWidth = 1.f; p.harmonicGain = 0.f; p.compressor = -0.5f;
        p.highCutHz = 19500.f; p.antiDolby = 0.f; p.loudnessTarget = -16.f;
        p.bassBoostDb = 0.f; p.dialogBoostDb = 0.f; p.widenerMult = 1.f;
        return true;
    }
    return false;
}

// Comandos de TEXTO (MagiskBridge.sendCommand(String)): la app envía
// "SET_BYPASS:1", "STATUS", "GET_TELEMETRY", "SET_PF_DRIVE:0.5" y
// "RELOAD_PARAMS" sin JSON. El servidor viejo los atendía por prefijo; el
// respondedor v2 solo aceptaba JSON y los devolvía bad_request (el panel
// Magisk y OmegaDaemon los recibían como error). Se despachan aquí.
// Devuelve "" si el texto no es un comando conocido (sin respuesta,
// fail-closed; el test "petición no-JSON -> sin respuesta" lo exige).
inline std::string handleTextCommand(const std::string& req, const CommandPlaneState& st) {
    std::string t = req;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r' || t.back() == '\n')) t.pop_back();
    std::string upper = t;
    for (char& c : upper) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    if (t.rfind("SET_BYPASS:", 0) == 0) {
        const float v = static_cast<float>(std::strtod(t.c_str() + 11, nullptr));
        const bool on = std::isfinite(v) && v > 0.5f;
        PendingPerceptual p;
        p.masterBypass = on ? 1.f : 0.f;
        const uint64_t g = stashPendingPerceptual(st, p);
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"SET_BYPASS\",\"bypass\":%s,\"generation\":%llu}",
            on ? "true" : "false", (unsigned long long)g);
        return std::string(buf);
    }
    if (upper == "STATUS" || upper == "GET_STATUS") {
        return buildStatusReply(st, effectBeaconNowMs());
    }
    if (upper == "GET_TELEMETRY" || upper == "TELEMETRY") {
        return buildTelemetryReply(st);
    }
    if (t.rfind("SET_PF_", 0) == 0) {
        const std::size_t colon = t.find(':');
        if (colon == std::string::npos) return std::string();
        const std::string key = t.substr(7, colon - 7);
        const int idx = pfIndexForName(key);
        char* end = nullptr;
        const float v = static_cast<float>(std::strtod(t.c_str() + colon + 1, &end));
        if (idx < 0 || end == t.c_str() + colon + 1 || !std::isfinite(v)) return std::string();
        PendingPerceptual p;
        p.pfSingleIdx = idx;
        p.pfSingleValue = v;
        const uint64_t g = stashPendingPerceptual(st, p);
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"pf_%s\":%.3f,\"generation\":%llu}",
            key.c_str(), static_cast<double>(v), (unsigned long long)g);
        return std::string(buf);
    }
    if (upper == "RELOAD_PARAMS" || upper == "RELOAD") {
        // v2 re-lee fusion_fx.conf en cada ciclo del bucle principal: no
        // hay nada que recargar a mano; se confirma ok (el panel lo espera).
        char buf[192];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"RELOAD_PARAMS\",\"reloaded\":true,\"generation\":%llu}",
            (unsigned long long)st.snapshotGeneration.load(std::memory_order_relaxed));
        return std::string(buf);
    }
    if (upper.rfind("SET_PRESET:", 0) == 0) {
        // v2.5.486 (ABI v16): SET_PRESET:<nombre> SÍ tiene llamador vivo
        // (MagiskBridge.setPreset: "Spatial", "Warm"). La respuesta copia
        // la forma del servidor viejo (command_server.cpp:1349):
        // {"ok":true,"preset":"<nombre>","gen":<gen>} o
        // {"ok":false,"error":"preset desconocido"}.
        const std::string name = t.substr(11);
        PendingPerceptual p;
        if (!applyPresetToPending(p, name)) {
            return std::string("{\"ok\":false,\"error\":\"preset desconocido\"}");
        }
        const uint64_t g = stashPendingPerceptual(st, p);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"preset\":\"%s\",\"gen\":%llu}",
            name.c_str(), (unsigned long long)g);
        return std::string(buf);
    }
    // SET_REVERB: no tiene llamador vivo en la app ni handler en el
    // servidor viejo: sigue sin soporte, documentado en REPORT.md.
    return std::string();
}

// Despacho de un comando (texto JSON ya recibido). Devuelve la respuesta,
// o cadena vacía si no hay nada que responder.
// Despacho de un comando JSON (texto ya recibido). Devuelve la respuesta,
// o cadena vacía si no hay nada que responder.
// 0044 (Fallo 1): el respondedor v2 solo aceptaba HELLO/GET_STATUS/
// SET_PERCEPTUAL_STATE y devolvía "unsupported" para ~20 comandos reales
// de la app (SET_ROOM_RT60, SET_BYPASS, GET_CLOCK_TELEMETRY, ...). Ahora
// se despacha el protocolo completo del servidor viejo. Las claves JSON son
// las del servidor viejo (command_server.cpp); donde Gemini propuso otra
// clave se aceptan ambas (vieja primero).
inline std::string handleCommand(const std::string& req, const CommandPlaneState& st) {
    std::string action;
    if (!extractAction(req, action)) return std::string("{\"ok\":false,\"error\":\"bad_request\"}");
    if (action == "HELLO") {
        const float proto = jsonNumber(req, "proto", 0.0f);
        const uint32_t p = (proto < 0.0f) ? 0u : static_cast<uint32_t>(proto);
        return buildHelloReply(p);
    }
    if (action == "PING") {
        return std::string("{\"ok\":true,\"command\":\"PING\",\"daemon\":\"ivanna_daemon_v2\"}");
    }
    if (action == "GET_STATUS" || action == "STATUS") {
        return buildStatusReply(st, effectBeaconNowMs());
    }
    if (action == "GET_TELEMETRY" || action == "TELEMETRY") {
        return buildTelemetryReply(st);
    }
    if (action == "GET_CLOCK_TELEMETRY") {
        // 0044 (Fallo 1): relevo del daemon para la APK (0043: la app no
        // puede leer /data/adb/ivanna_omega/ directamente). Campos reales
        // de cada struct; sin productor, *_valid=false y valores 0.
        ivanna::EffectCallbackClockStatus cb{};
        ivanna::EffectHardwareClockStatus hw{};
        const bool cbOk = ivanna::readEffectCallbackClockStatus(cb);
        const bool hwOk = ivanna::readEffectHardwareClockStatus(hw);
        ivanna::audio::AsrcStateSample asrc{};
        bool asrcOk = false;
        {
            ivanna::audio::AsrcStateReader r;
            if (r.open()) asrcOk = r.read(asrc);
        }
        ivanna::audio::WriteStageAsrcSample ws{};
        const bool wsOk = ivanna::audio::readWriteStageAsrc(ws);
        char buf[1024];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_CLOCK_TELEMETRY\","
            "\"callback_rate_hz\":%.3f,\"callback_drift_ppm\":%d,\"callback_valid\":%s,"
            "\"hardware_rate_hz\":%.3f,\"hardware_drift_ppm\":%d,\"hardware_valid\":%s,"
            "\"asrc_state\":%d,\"asrc_drift_ppm\":%.3f,\"asrc_target_ppm\":%.3f,\"asrc_ratio\":%.6f,\"asrc_valid\":%s,"
            "\"write_stage_valid\":%s,\"write_stage_enabled\":%s,\"write_stage_ratio\":%.6f,"
            "\"write_stage_drift_ppm\":%.3f,\"write_stage_kalman_ppm\":%.3f,"
            "\"write_stage_clock\":\"%s\",\"write_stage_opens\":%llu,\"write_stage_ok\":%llu,"
            "\"write_stage_sat\":%d,\"write_stage_fir_taps\":%d,\"write_stage_neon\":%d,"
            "\"write_stage_age_ms\":%llu}",
            cbOk ? cb.observedSampleRateHz : 0.0, cbOk ? cb.driftPpm : 0, cbOk ? "true" : "false",
            hwOk ? hw.measuredSampleRateHz : 0.0, hwOk ? hw.driftPpm : 0, hwOk ? "true" : "false",
            asrcOk ? static_cast<int>(asrc.state) : -1,
            asrcOk ? asrc.driftPpm : 0.0, asrcOk ? asrc.targetPpm : 0.0,
            asrcOk ? asrc.appliedRatio : 0.0, asrcOk ? "true" : "false",
            wsOk ? "true" : "false", (wsOk && ws.enabled) ? "true" : "false",
            wsOk ? ws.ratio : 0.0, wsOk ? ws.driftPpm : 0.0, wsOk ? ws.kalmanDriftPpm : 0.0,
            wsOk ? ws.clock : "unknown",
            wsOk ? static_cast<unsigned long long>(ws.opens) : 0ULL,
            wsOk ? static_cast<unsigned long long>(ws.ok) : 0ULL,
            wsOk ? ws.sat : 0, wsOk ? ws.firTaps : 0, wsOk ? ws.neon : 0,
            wsOk ? static_cast<unsigned long long>(ws.ageMs) : 0ULL);
        return std::string(buf);
    }
    if (action == "GET_PROCESSOR_EVIDENCE") {
        ivanna::EffectEvidence ev{};
        if (!ivanna::readEffectEvidence(ev)) {
            return std::string("{\"ok\":false,\"error\":\"no_evidence\"}");
        }
        char buf[1280];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_PROCESSOR_EVIDENCE\",\"version\":%u,"
            "\"hrtf_enabled\":%u,\"hrtf_cpu_blocks\":%llu,\"hrtf_dsp_blocks\":%llu,"
            "\"hrtf_pose_applies\":%llu,\"hrtf_in_energy\":%.6f,\"hrtf_out_energy\":%.6f,"
            "\"rir_loads\":%llu,\"rir_relinks\":%llu,\"rir_active_blocks\":%llu,"
            "\"rir_in_energy\":%.6f,\"rir_out_energy\":%.6f,\"rir_wet\":%.3f,"
            "\"sample_rate\":%u,\"hrtf_dataset_loaded\":%u,\"control_generation\":%llu,"
            "\"adaptive_enabled\":%u,\"adaptive_apply_blocks\":%llu,"
            "\"omni_process_calls\":%llu,\"omni_blocks_rendered\":%llu,\"omni_frames_rendered\":%llu,"
            "\"omni_applied_amount\":%.3f,\"omni_convergence_r\":%.3f,"
            "\"omni_scene_azimuth_deg\":%.3f,\"omni_diffuseness\":%.3f,\"omni_fault\":%u}",
            ev.version,
            ev.hrtfEnabled, static_cast<unsigned long long>(ev.hrtfCpuBlocks),
            static_cast<unsigned long long>(ev.hrtfDspBlocks),
            static_cast<unsigned long long>(ev.hrtfPoseApplies),
            ev.hrtfInEnergy, ev.hrtfOutEnergy,
            static_cast<unsigned long long>(ev.rirLoads),
            static_cast<unsigned long long>(ev.rirRelinks),
            static_cast<unsigned long long>(ev.rirActiveBlocks),
            ev.rirInEnergy, ev.rirOutEnergy, static_cast<double>(ev.rirWet),
            ev.sampleRate, ev.hrtfDatasetLoaded,
            static_cast<unsigned long long>(ev.controlGeneration),
            ev.adaptiveEnabled, static_cast<unsigned long long>(ev.adaptiveApplyBlocks),
            static_cast<unsigned long long>(ev.omniProcessCalls),
            static_cast<unsigned long long>(ev.omniBlocksRendered),
            static_cast<unsigned long long>(ev.omniFramesRendered),
            static_cast<double>(ev.omniAppliedAmount), static_cast<double>(ev.omniConvergenceR),
            static_cast<double>(ev.omniSceneAzimuthDeg), static_cast<double>(ev.omniDiffuseness),
            ev.omniFault);
        return std::string(buf);
    }
    if (action == "GET_SUPREME_AXES") {
        // v2.5.481 (Esse, daemon NUEVO): el daemon SÍ guarda los ejes
        // supremos (desiredPerceptual acumulado). Ausente = nunca se ha
        // puesto por socket: se devuelve el default del snapshot
        // (omega_control_bus.h: saturadores en 0 desde la 481).
        PendingPerceptual d;
        copyDesiredPerceptual(st, d);
        const auto val = [](float v, float def) {
            return std::isnan(v) ? def : v;
        };
        char buf[768];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_SUPREME_AXES\",\"generation\":%llu,"
            "\"warpedLatticeBlDrive\":%.4f,\"warpedLatticeLambda\":%.4f,"
            "\"transharmonicHarmonicGain\":%.4f,\"transharmonicImdCancel\":%.4f,"
            "\"transharmonicAnalogTapeDrive\":%.4f,\"snnHoaImmersivity\":%.4f,"
            "\"snnSpikeThreshold\":%.4f,\"pinnaManifoldWetMix\":%.4f,"
            "\"pinnaConchaDepth\":%.4f,\"pinnaHelixCurl\":%.4f,"
            "\"pinnaHeadWidth\":%.4f,\"msoItdNanoseconds\":%.1f}",
            (unsigned long long)st.perceptualGen.load(std::memory_order_relaxed),
            (double)val(d.supremeLatticeBlDrive, 0.f),
            (double)val(d.supremeLatticeLambda, 0.f),
            (double)val(d.supremeCvnnHarmonicGain, 0.f),
            (double)val(d.supremeCvnnImdCancel, 0.85f),
            (double)val(d.supremeCvnnTapeDrive, 0.f),
            (double)val(d.supremeSnnImmersivity, 0.65f),
            (double)val(d.supremeSnnSpikeThreshold, 0.45f),
            (double)val(d.supremePinnaWetMix, 0.62f),
            (double)val(d.supremePinnaConchaDepth, 0.163986f),
            (double)val(d.supremePinnaHelixCurl, 0.119832f),
            (double)val(d.supremePinnaHeadWidth, 0.138745f),
            (double)val(d.supremeMsoItdNs, 0.f));
        return std::string(buf);
    }
    if (action == "GET_HEALTH") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:1136).
        // generation = último snapshot publicado por el daemon (bus);
        // consumer = "omega_effect" solo si el efecto procesa o está
        // habilitado e inactivo (beacon); bus_open = el escritor del bus
        // está abierto. "healthy" exige: generación > 0, consumidor
        // enganchado y bus abierto — lo demás es parcial.
        const uint64_t gen = ivanna::controlBus().lastPublishedGeneration();
        uint64_t dspFrames = 0, dspAppliedGen = 0;
        const EffectBeaconState beacon =
            readEffectBeacon(2000ull, OMEGA_EFFECT_BEACON_PATH, &dspFrames, &dspAppliedGen);
        const bool hasConsumer = (beacon == EffectBeaconState::Processing) ||
                                 (beacon == EffectBeaconState::EnabledIdle);
        const bool busOpen = ivanna::controlBus().isWriterOpen();
        const bool healthy = (gen > 0) && hasConsumer && busOpen;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_HEALTH\",\"generation\":%llu,"
            "\"consumer\":%s,\"bus_open\":%s,\"healthy\":%s}",
            (unsigned long long)gen,
            hasConsumer ? "\"omega_effect\"" : "null",
            busOpen ? "true" : "false",
            healthy ? "true" : "false");
        return std::string(buf);
    }
    if (action == "GET_ROOM_STATUS") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:996).
        // rt60/idx/wet salen del estado DESEADO acumulado (lo último que la
        // app pidió); ausente = default del snapshot (0.34 s / 51 / 0.22).
        PendingPerceptual d;
        copyDesiredPerceptual(st, d);
        const float rt60 = std::isnan(d.roomRt60S) ? 0.34f : d.roomRt60S;
        const int idx = std::isnan(d.roomIdx) ? 51 : static_cast<int>(d.roomIdx);
        const float wet = std::isnan(d.roomWet) ? 0.22f : d.roomWet;
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_ROOM_STATUS\",\"rt60\":%.3f,"
            "\"room_idx\":%d,\"wet\":%.3f,\"generation\":%llu}",
            (double)rt60, idx, (double)wet,
            (unsigned long long)st.perceptualGen.load(std::memory_order_relaxed));
        return std::string(buf);
    }
    if (action == "GET_HEAD_POSE_STATUS") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:480).
        // El muerto comparaba el ack contra la última pose publicada (gen
        // propia); aquí se compara contra la pose DESEADA acumulada: si el
        // efecto confirma yaw/pitch (±0.001 rad), la pose está aplicada.
        // Sin pose pedida nunca = applied:false, requested:false.
        PendingPerceptual d;
        copyDesiredPerceptual(st, d);
        const bool requested = !std::isnan(d.headYawRad) || !std::isnan(d.headPitchRad);
        const float reqYaw = std::isnan(d.headYawRad) ? 0.f : d.headYawRad;
        const float reqPitch = std::isnan(d.headPitchRad) ? 0.f : d.headPitchRad;
        ivanna::EffectHeadPoseAck ack{};
        const bool ackOk = ivanna::readEffectHeadPoseAck(ack);
        const bool applied = ackOk && ack.valid &&
            std::fabs(ack.yaw_rad - reqYaw) <= 0.001f &&
            std::fabs(ack.pitch_rad - reqPitch) <= 0.001f;
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_HEAD_POSE_STATUS\",\"applied\":%s,"
            "\"requested\":%s,\"yaw_rad\":%.3f,\"pitch_rad\":%.3f,"
            "\"generation\":%llu,\"consumer_generation\":%llu}",
            applied ? "true" : "false",
            requested ? "true" : "false",
            (double)reqYaw, (double)reqPitch,
            (unsigned long long)st.perceptualGen.load(std::memory_order_relaxed),
            (unsigned long long)(ackOk ? ack.generation : 0ULL));
        return std::string(buf);
    }
    if (action == "GET_AUDIO_FEATURES_STATUS") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:891).
        // El muerto guardaba la gen del SET y el seq de evidencia de ese
        // momento; aquí se comprueba contra el estado VIVO: aplicado =
        // la evidencia existe, el efecto aplicó una generación >= la del
        // último snapshot publicado (que ya lleva los toggles) y los
        // toggles coinciden con lo deseado. Sin toggles pedidos nunca =
        // "no_request" (como el muerto).
        PendingPerceptual d;
        copyDesiredPerceptual(st, d);
        const bool wantHrtf = !std::isnan(d.hrtfEnabled);
        const bool wantAdaptive = !std::isnan(d.adaptiveEnabled);
        if (!wantHrtf && !wantAdaptive) {
            return std::string("{\"ok\":true,\"command\":\"GET_AUDIO_FEATURES_STATUS\","
                "\"status\":\"no_request\",\"applied\":false}");
        }
        ivanna::EffectEvidence ev{};
        const bool evOk = ivanna::readEffectEvidence(ev);
        const uint64_t snapGen = st.snapshotGeneration.load(std::memory_order_relaxed);
        const bool applied = evOk && snapGen > 0 &&
            ev.controlGeneration >= snapGen &&
            (!wantHrtf || (ev.hrtfEnabled != 0) == (d.hrtfEnabled >= 0.5f)) &&
            (!wantAdaptive || (ev.adaptiveEnabled != 0) == (d.adaptiveEnabled >= 0.5f));
        char buf[512];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"GET_AUDIO_FEATURES_STATUS\","
            "\"status\":\"%s\",\"applied\":%s,\"generation\":%llu,"
            "\"evidence_generation\":%llu}",
            applied ? "applied" : "pending",
            applied ? "true" : "false",
            (unsigned long long)st.perceptualGen.load(std::memory_order_relaxed),
            (unsigned long long)(evOk ? ev.controlGeneration : 0ULL));
        return std::string(buf);
    }
    if (action == "SET_HIRES") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:915).
        // HIRES no necesita campo en el snapshot: escribe
        // /data/adb/ivanna_omega/hires.conf (contrato ivanna_hires_config.hpp);
        // el cambio exige reinicio del daemon/efecto (saved_restart_required).
        // La forma de la respuesta copia la del servidor viejo (applied:false
        // siempre: este comando no cambia nada en caliente).
        const float rateF = jsonNumber(req, "rate", 0.0f);
        const float depthF = jsonNumber(req, "depth", 0.0f);
        const int rate = static_cast<int>(rateF);
        const int depth = static_cast<int>(depthF);
        if (!ivanna::hires::isValidRate(rate) || !ivanna::hires::isValidDepth(depth)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "{\"ok\":false,\"command\":\"SET_HIRES\",\"applied\":false,"
                "\"status\":\"invalid_value\",\"error\":\"rate=%d depth=%d fuera de contrato\"}",
                rate, depth);
            return std::string(buf);
        }
        if (!ivanna::hires::writeConf("/data/adb/ivanna_omega/hires.conf", rate, depth)) {
            return std::string("{\"ok\":false,\"command\":\"SET_HIRES\",\"applied\":false,"
                "\"status\":\"write_failed\",\"error\":\"no se pudo escribir hires.conf\"}");
        }
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"SET_HIRES\",\"applied\":false,"
            "\"status\":\"saved_restart_required\",\"rate\":%d,\"depth\":%d}",
            rate, depth);
        return std::string(buf);
    }
    if (action == "SET_PRESET") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:1055).
        // El preset se stashea como PendingPerceptual (applyPresetToPending)
        // y el bucle lo aplica al snapshot. La forma de texto
        // "SET_PRESET:<nombre>" vive en handleTextCommand.
        std::string name;
        if (!jsonString(req, "preset", name)) {
            return std::string("{\"ok\":false,\"command\":\"SET_PRESET\",\"error\":\"bad_request\"}");
        }
        PendingPerceptual p;
        if (!applyPresetToPending(p, name)) {
            return std::string("{\"ok\":false,\"command\":\"SET_PRESET\",\"error\":\"unknown_preset\"}");
        }
        const uint64_t g = stashPendingPerceptual(st, p);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"SET_PRESET\",\"status\":\"accepted_pending_consumer\","
            "\"generation\":%llu}",
            (unsigned long long)g);
        return std::string(buf);
    }
    if (action == "SET_PINNA_METRICS") {
        // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:1029).
        // Claves reales: "concha"/"helix"/"fosa" (default 0) y "width"/
        // "height"/"depth" (default 9999 = ausente; si están, SOBRESCRIBEN
        // a concha/helix/fosa). Normalización idéntica: |v| > 1.5 -> mm
        // (concha (v-18)/10, helix (v-60)/25, fosa (v-15)/10); si no, v
        // tal cual, todo clamp [-1,1]. La fosa va al slot head_width (el
        // snapshot no tiene slot de fosa; el muerto también la mapeaba
        // ahí). El flag SUPREME_PINNA_ON no se toca: en el modelo vivo la
        // pinna se gobierna por el wet mix (default 0.62). La respuesta
        // copia la del servidor viejo (valores CRUDOS + generación).
        const float absent = std::numeric_limits<float>::quiet_NaN();
        auto num = [&req, absent](const char* key) { return jsonNumber(req, key, absent); };
        float concha = num("concha");
        if (std::isnan(concha)) concha = 0.f;
        float helix = num("helix");
        if (std::isnan(helix)) helix = 0.f;
        float fosa = num("fosa");
        if (std::isnan(fosa)) fosa = 0.f;
        const float w = num("width");
        const float h = num("height");
        const float d = num("depth");
        if (!std::isnan(w)) concha = w;
        if (!std::isnan(h)) helix = h;
        if (!std::isnan(d)) fosa = d;
        const auto norm = [](float v, float center, float scale) {
            return (v > 1.5f || v < -1.5f)
                ? std::clamp((v - center) / scale, -1.f, 1.f)
                : std::clamp(v, -1.f, 1.f);
        };
        PendingPerceptual p;
        p.supremePinnaConchaDepth = norm(concha, 18.f, 10.f);
        p.supremePinnaHelixCurl = norm(helix, 60.f, 25.f);
        p.supremePinnaHeadWidth = norm(fosa, 15.f, 10.f);
        const uint64_t g = stashPendingPerceptual(st, p);
        char buf[384];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"action\":\"SET_PINNA_METRICS\",\"generation\":%llu,"
            "\"concha\":%.3f,\"helix\":%.3f,\"fosa\":%.3f}",
            (unsigned long long)g,
            (double)concha, (double)helix, (double)fosa);
        return std::string(buf);
    }
    if (action == "SET_PERCEPTUAL_STATE" || action == "SEND_PERCEPTUAL_STATE") {
        // Único canal app -> daemon para los ajustes (ORBE/HOLOS/upmixing/
        // EQ). Cada comando trae solo un subconjunto: ausente = NaN = no
        // tocar. El clamp se hace al aplicar (perceptual_state.h), no aquí.
        // No se publica directamente: el stash lo aplica el bucle principal
        // al snapshot antes de publicar (un solo escritor del SHM).
        const float absent = std::numeric_limits<float>::quiet_NaN();
        auto num = [&req, absent](const char* key) {
            return jsonNumber(req, key, absent);
        };
        PendingPerceptual p;
        p.compressor = num("compressor");
        // exciterRed acepta los dos alias del servidor viejo.
        p.exciterRed = num("exciterRed");
        if (std::isnan(p.exciterRed)) p.exciterRed = num("exciterReduction");
        p.highCutHz = num("highCutHz");
        if (std::isnan(p.highCutHz)) p.highCutHz = num("highCut");
        p.spatialWidth = num("spatialWidth");
        p.loudnessTarget = num("loudnessTarget");
        if (std::isnan(p.loudnessTarget)) p.loudnessTarget = num("loudnessTargetLuFS");
        p.harmonicGain = num("harmonicGain");
        p.antiDolby = num("antiDolby");
        if (std::isnan(p.antiDolby)) p.antiDolby = num("antiDolbyIntensity");
        p.upmixingEnabled = num("upmixingEnabled");
        p.upmixingImmersivity = num("upmixingImmersivity");
        p.orbeAmount = num("orbeAmount");
        p.orbeDistanceM = num("orbeDistanceM");
        p.orbeSpread = num("orbeSpread");
        p.orbeProximityDb = num("orbeProximityDb");
        p.orbeDiffuse = num("orbeDiffuse");
        p.orbeHeadTracking = num("orbeHeadTracking");
        p.holosAmount = num("holosAmount");
        p.holosDistanceM = num("holosDistanceM");
        p.holosSpread = num("holosSpread");
        p.holosProximityDb = num("holosProximityDb");
        p.holosDiffuse = num("holosDiffuse");
        p.holosHarmonics = num("holosHarmonics");
        p.holosWarmth = num("holosWarmth");
        p.holosDynamics = num("holosDynamics");
        p.holosWidth = num("holosWidth");
        p.holosGamma = num("holosGamma");
        p.holosRoomRt60 = num("holosRoomRt60");
        p.holosHeadTracking = num("holosHeadTracking");
        p.holosBandCount = num("holosBandCount");
        p.holosEqLow = num("holosEqLow");
        p.holosEqMid = num("holosEqMid");
        p.holosEqHigh = num("holosEqHigh");
        p.holosEqPresence = num("holosEqPresence");
        p.holosEqTiltDb = num("holosEqTiltDb");
        p.holosEqFreq = num("holosEqFreq");
        // Φ + Volterra (ABI v13, 0038+0040): paridad Ruta B.
        p.hyperAmount = num("hyperAmount");
        p.hyperPreset = num("hyperPreset");
        p.hyperXtcEnabled = num("hyperXtcEnabled");
        p.volterraEnabled = num("volterraEnabled");
        // Física Nueva (ABI v14, Fase 2, v2.5.472): los 17 controles
        // NewPhysicsControls. Claves camelCase, mismo patrón que
        // orbeAmount/holosAmount/hyperAmount (las pone la app en
        // NewPhysicsStore.buildNewPhysicsPerceptualFields, NewPhysicsBridge.kt). El clamp
        // se hace al aplicar (perceptual_state.h), no aquí; ausente =
        // NaN = se conserva el vigente.
        p.npAmount = num("npAmount");
        p.npLgnMix = num("npLgnMix");
        p.npBsDamp = num("npBsDamp");
        p.npTorsoMix = num("npTorsoMix");
        p.npHeadRadiusM = num("npHeadRadiusM");
        p.npAzDeg = num("npAzDeg");
        p.npElDeg = num("npElDeg");
        p.npEqClass = num("npEqClass");
        p.npOmegaSigMix = num("npOmegaSigMix");
        p.npBoostCapDb = num("npBoostCapDb");
        p.npXtalkMix = num("npXtalkMix");
        p.npSigmaDistM = num("npSigmaDistM");
        p.npSplDb = num("npSplDb");
        p.npAirMix = num("npAirMix");
        p.npVolterraMix = num("npVolterraMix");
        p.npFreeEnergyMix = num("npFreeEnergyMix");
        p.npAbxMode = num("npAbxMode");
        // Conservación perceptual (ABI v17, 2026-11-21): el árbitro de
        // campo. Claves camelCase, mismo patrón que orbeAmount/holosAmount
        // (las pone la app en buildFusedPerceptualFields, FusedControls.kt).
        // El clamp se hace al aplicar (perceptual_state.h), no aquí;
        // ausente = NaN = se conserva el vigente.
        p.consvAmount = num("consvAmount");
        p.consvEnergyCap = num("consvEnergyCap");
        p.consvPhaseCoherence = num("consvPhaseCoherence");
        p.consvInertia = num("consvInertia");
        // FIX 1 (bofo Genezi, v2.5.465): modo de salida (0 = desconocido,
        // 1 = audífonos, 2 = bocinas/Genezi). Lo pone la app
        // (PersistedStateRestorer, RouteBDeviceGate + usbDacTarget);
        // ausente = se conserva el vigente (merge en perceptual_state.h).
        p.outputMode = num("outputMode");
        // v2.5.472: escala del parallax (SpatialControls.parallaxAmount,
        // la trae buildFusedPerceptualFields). Se guarda como deseada y la
        // aplica el próximo SET_HEAD_POSE (ver headPoseParallaxScale).
        p.parallaxAmount = num("parallaxAmount");
        if (!p.any()) {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"command\":\"%s\",\"status\":\"no_fields\",\"generation\":%llu}",
                action.c_str(),
                (unsigned long long)st.perceptualGen.load(std::memory_order_relaxed));
            return std::string(buf);
        }
        stashPendingPerceptual(st, p);
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "{\"ok\":true,\"command\":\"%s\",\"status\":\"accepted_pending_consumer\",\"generation\":%llu}",
            action.c_str(),
            (unsigned long long)st.perceptualGen.load(std::memory_order_relaxed));
        return std::string(buf);
    }
    // ── Fallo 1: resto del protocolo real de la app ─────────────────────
    // (antes: {"ok":false,"error":"unsupported"} para todos). Claves = las
    // del servidor viejo; num2 acepta también la clave propuesta por Gemini.
    {
        PendingPerceptual p;
        bool handled = false;
        const float absent = std::numeric_limits<float>::quiet_NaN();
        auto num = [&req, absent](const char* key) { return jsonNumber(req, key, absent); };
        auto num2 = [&req, absent](const char* k1, const char* k2) {
            const float v = jsonNumber(req, k1, absent);
            return std::isnan(v) ? jsonNumber(req, k2, absent) : v;
        };
        if (action == "SET_ROOM_RT60") {
            p.roomRt60S = num("rt60");
            p.roomWet = num("wet");
            p.roomIdx = num("idx");
            handled = true;
        } else if (action == "SET_AUX_FIELD") {
            p.auxEnabled = num2("auxEnabled", "enabled");
            p.auxXoverHz = num2("auxXoverHz", "xoverHz");
            p.auxCoherence = num2("auxCoherence", "coherence");
            handled = true;
        } else if (action == "SET_BYPASS") {
            p.masterBypass = num2("bypass", "enabled");
            handled = true;
        } else if (action == "SET_EQ_BANDS") {
            if (jsonFloatArray(req, "gains", p.eqGains, 10, true)) p.hasEqBands = true;
            p.listenPhon = num("listenPhon");
            p.refPhon = num("refPhon");
            handled = true;
        } else if (action == "SET_WFS") {
            p.wfsEnabled = num2("wfsEnabled", "enabled");
            p.wfsSpread = num2("wfsSpread", "spread");
            handled = true;
        } else if (action == "SET_WFS_LAYOUT") {
            if (jsonFloatArray(req, "wfsSpeakerX", p.wfsSpeakerX, 7, true) &&
                jsonFloatArray(req, "wfsSpeakerY", p.wfsSpeakerY, 7, true) &&
                jsonFloatArray(req, "wfsSpeakerZ", p.wfsSpeakerZ, 7, true)) {
                p.hasWfsLayout = true;
            } else if (jsonFloatArray(req, "x", p.wfsSpeakerX, 7, true) &&
                       jsonFloatArray(req, "y", p.wfsSpeakerY, 7, true) &&
                       jsonFloatArray(req, "z", p.wfsSpeakerZ, 7, true)) {
                p.hasWfsLayout = true;
            }
            handled = true;
        } else if (action == "SET_4D") {
            p.fourdOn = num2("fourdOn", "enabled");
            p.fourdArc = num2("arcOn", "arc");
            p.fourdXtc = num2("xtcOn", "xtc");
            p.fourdArcDepth = num("arcDepth");
            p.fourdDepth = num("depth");
            p.fourdLeadMs = num("leadMs");
            handled = true;
        } else if (action == "SET_PF_PARAMS") {
            if (jsonFloatArray(req, "params", p.pfParams, 13, true)) p.hasPfParams = true;
            handled = true;
        } else if (action == "SET_HEAD_POSE") {
            p.headYawRad = num2("yawRad", "yaw");
            p.headPitchRad = num2("pitchRad", "pitch");
            p.parallaxAmount = num("parallaxAmount");
            // v2.5.472: el yaw/pitch se escalan por el parallax vigente
            // (0 = la pose no mueve el sonido en Ruta B; 1 = actual).
            const float par = headPoseParallaxScale(st, p.parallaxAmount);
            if (!std::isnan(p.headYawRad)) p.headYawRad *= par;
            if (!std::isnan(p.headPitchRad)) p.headPitchRad *= par;
            handled = true;
        } else if (action == "SET_SCENE_POSE") {
            const float clearV = num("clear");
            if (!std::isnan(clearV) && clearV >= 0.5f) {
                p.headYawRad = 0.f; p.headPitchRad = 0.f; p.sceneDistance = 1.f;
            } else {
                p.headYawRad = num2("azimuthRad", "yaw");
                p.headPitchRad = num2("elevationRad", "pitch");
                p.sceneDistance = num2("distance", "sceneDistance");
                // v2.5.472: misma escala de parallax que SET_HEAD_POSE.
                p.parallaxAmount = num("parallaxAmount");
                const float par = headPoseParallaxScale(st, p.parallaxAmount);
                if (!std::isnan(p.headYawRad)) p.headYawRad *= par;
                if (!std::isnan(p.headPitchRad)) p.headPitchRad *= par;
            }
            handled = true;
        } else if (action == "SET_INTENSITY") {
            p.intensity = num("intensity");
            handled = true;
        } else if (action == "SET_HARMONIC_GAIN") {
            p.harmonicGain = num("harmonicGain");
            handled = true;
        } else if (action == "SET_ADAPTIVE_STATE") {
            // Servidor viejo: targetGain/compAmount/excRed (NO
            // adaptiveEnabled/hrtfEnabled: esos son de SET_AUDIO_FEATURES).
            p.targetGain = num("targetGain");
            p.compAmount = num("compAmount");
            p.excRed = num2("excRed", "exciterRed");
            handled = true;
        } else if (action == "PUSH_SAF_STATE" || action == "SET_SAF_STATE") {
            p.safDeltaEnergy = num("deltaEnergy");
            p.safMetricNorm = num("metricNorm");
            p.safMemory = num("memory");
            p.safGain = num("safGain");
            if (jsonFloatArray(req, "q", p.safQ, 7, true)) p.hasSafQ = true;
            handled = true;
        } else if (action == "SET_AUDIO_FEATURES") {
            // Servidor viejo: los dos actuadores (hrtf/adaptive). 9999 =
            // ausente en el servidor viejo; aquí ausente = NaN.
            p.hrtfEnabled = num("hrtfEnabled");
            p.adaptiveEnabled = num("adaptiveEnabled");
            handled = true;
        } else if (action == "SET_PUNCH") {
            p.punchAmount = num("punchAmount");
            handled = true;
        } else if (action == "SET_SUPREME_AXES") {
            // v2.5.481 (Esse, daemon NUEVO garantizado): handler COMPLETO.
            // Antes solo harmonicGain tenía slot ("ABI v13 sin campos")
            // y el resto se ignoraba — por eso el tronidito en burbujita
            // (tape/harm/lattice) no se podía apagar por socket. Ahora
            // los 12 escalares supremos tienen campo en PendingPerceptual
            // y se publican al snapshot (applyPendingPerceptual). Las
            // claves JSON son las mismas que el servidor viejo
            // (command_server.cpp) para que la app no cambie nada.
            p.harmonicGain = num("harmonicGain");
            p.supremeLatticeBlDrive = num("warpedLatticeBlDrive");
            p.supremeLatticeLambda = num("warpedLatticeLambda");
            p.supremeCvnnHarmonicGain = num("transharmonicHarmonicGain");
            p.supremeCvnnImdCancel = num("transharmonicImdCancel");
            p.supremeCvnnTapeDrive = num("transharmonicAnalogTapeDrive");
            p.supremeSnnImmersivity = num("snnHoaImmersivity");
            p.supremeSnnSpikeThreshold = num("snnSpikeThreshold");
            p.supremePinnaWetMix = num("pinnaManifoldWetMix");
            p.supremePinnaConchaDepth = num("pinnaConchaDepth");
            p.supremePinnaHelixCurl = num("pinnaHelixCurl");
            p.supremePinnaHeadWidth = num("pinnaHeadWidth");
            p.supremeMsoItdNs = num("msoItdNanoseconds");
            handled = true;
        } else if (action == "SET_USER_FLAGS") {
            for (const auto& e : ivanna::kOmegaUserFlagTable) {
                const float v = num(e.key);
                if (!std::isnan(v) && v != ivanna::OMEGA_USER_FLAG_UNSET) {
                    p.userFlagsMask |= e.bit;
                    if (v >= 0.5f) p.userFlagsValue |= e.bit;
                    else p.userFlagsValue &= ~e.bit;
                }
            }
            const float style = num(ivanna::kOmegaUserFlagStyleKey);
            if (!std::isnan(style) && style != ivanna::OMEGA_USER_FLAG_UNSET) {
                const uint32_t enc = ivanna::omega_atlas_style_encode(static_cast<int>(style));
                if (enc != 0u) {
                    p.userFlagsMask |= ivanna::OMEGA_FLAG_ATLAS_STYLE_MASK;
                    p.userFlagsValue = (p.userFlagsValue & ~ivanna::OMEGA_FLAG_ATLAS_STYLE_MASK) | enc;
                }
            }
            handled = true;
        } else if (action == "SET_NPE") {
            p.npeEnabled = num("enabled");
            p.npeLateralInhib = num("lateralInhib");
            p.npeOhcCompression = num2("ohc", "ohcCompression");
            p.npeMasterDb = num("masterDb");
            handled = true;
        } else if (action == "SET_TEMPORAL_FIELD") {
            p.temporalEnabled = num("temporalEnabled");
            p.temporalAmount = num("temporalAmount");
            p.temporalAuto = num("temporalAuto");
            handled = true;
        } else if (action == "SET_XTC" || action == "SET_CROSSTALK") {
            p.xtcEnabled = num2("enabled", "xtcEnabled");
            handled = true;
        } else if (action == "SET_ROOM_CORRECTION") {
            if (jsonFloatArray(req, "roomCorrDb", p.roomCorrDb, 10, true)) p.hasRoomCorr = true;
            else if (jsonFloatArray(req, "db", p.roomCorrDb, 10, true)) p.hasRoomCorr = true;
            handled = true;
        } else if (action == "SET_FOTOHOLO_STATE") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:360).
            p.fotoholoEnabled = num2("enabled", "fotoholoEnabled");
            p.fotoholoIntensity = num2("intensity", "fotoholoIntensity");
            handled = true;
        } else if (action == "SET_UPMIXING") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:418).
            p.upmixingEnabled = num("upmixingEnabled");
            p.upmixingImmersivity = num("upmixingImmersivity");
            handled = true;
        } else if (action == "SET_COCHLEAR_INTENSITY" || action == "SET_COCHLEAR") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:724).
            p.cochlearIntensity = num2("intensity", "cochlearIntensity");
            handled = true;
        } else if (action == "SET_COCHLEAR_INVERSE_ENABLED" || action == "SET_COCHLEAR_ENABLED") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:743).
            p.cochlearEnabled = num("enabled");
            handled = true;
        } else if (action == "SET_SPSC_RING_FACTOR" || action == "SET_SPSC") {
            // v2.5.486 (ABI v16): por fin con slot en el snapshot (ABI v16).
            // El llamador Kotlin también lo manda por JNI a la Ruta A; por
            // socket llega aquí y el bucle lo aplica al snapshot.
            p.spscRingFactor = num2("factor", "spscRingFactor");
            handled = true;
        } else if (action == "SET_REALITY_RECONSTRUCTION" || action == "SET_REALITY") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:826).
            p.realityReconEnabled = num("enabled");
            p.intensity = num("intensity");
            handled = true;
        } else if (action == "SET_ROUTE_PROFILE") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:971).
            // Claves con nombre y alias a/b/c (el muerto aceptaba a/b/c
            // como alias; si están presentes, tienen prioridad).
            p.bassBoostDb = num("bassBoostDb");
            if (std::isnan(p.bassBoostDb)) p.bassBoostDb = num("a");
            p.dialogBoostDb = num("dialogBoostDb");
            if (std::isnan(p.dialogBoostDb)) p.dialogBoostDb = num("b");
            p.widenerMult = num("widenerMult");
            if (std::isnan(p.widenerMult)) p.widenerMult = num("c");
            handled = true;
        } else if (action == "SET_YAMNET_SCORES" || action == "PUSH_YAMNET_SCORES") {
            // v2.5.486 (ABI v16): relevo del servidor viejo (command_server.cpp:939).
            // Validación idéntica: speech/music/conf finitos en [0,1] y
            // classId entero en [0,520]. Payload inválido = error (no se
            // stashea nada). Los scores van al snapshot (transporte + futuro
            // consumo por el efecto); el daemon v2 no los usa aún.
            const float speech = num("speech");
            const float music = num("music");
            const float conf = num("confidence");
            const float cid = num("classId");
            const bool valid = !std::isnan(speech) && speech >= 0.f && speech <= 1.f &&
                               !std::isnan(music) && music >= 0.f && music <= 1.f &&
                               !std::isnan(conf) && conf >= 0.f && conf <= 1.f &&
                               !std::isnan(cid) && cid >= 0.f && cid <= 520.f &&
                               std::floor(cid) == cid;
            if (!valid) {
                return std::string("{\"ok\":false,\"command\":\"SET_YAMNET_SCORES\","
                    "\"error\":\"invalid_classifier_payload\"}");
            }
            p.yamnetSpeech = speech;
            p.yamnetMusic = music;
            p.yamnetClassId = cid;
            p.yamnetConfidence = conf;
            handled = true;
        }
        if (handled) {
            const uint64_t g = p.any() ? stashPendingPerceptual(st, p)
                                       : st.perceptualGen.load(std::memory_order_relaxed);
            char buf[384];
            std::snprintf(buf, sizeof(buf),
                "{\"ok\":true,\"command\":\"%s\",\"status\":\"%s\",\"generation\":%llu}",
                action.c_str(),
                p.any() ? "accepted_pending_consumer" : "no_fields",
                (unsigned long long)g);
            return std::string(buf);
        }
    }
    // v2.5.486 (ABI v16): los 16 comandos del handler muerto ya tienen
    // soporte (ver arriba). Lo que sigue sin soporte es solo SET_REVERB:
    // (texto legacy sin llamador ni handler en el servidor viejo). Se
    // responde un error bien formado para que la app no confunda "sin
    // respuesta" con "daemon muerto".
    return std::string("{\"ok\":false,\"error\":\"unsupported\"}");
}

// Una conexión: auth, recv con acumulador (JSON con balanceo de llaves o
// comando de texto de una línea), una respuesta, close. Sin datos en
// 150 ms se cierra sin responder (conexión silenciosa; ver nota Modo B
// arriba). Fallo 5: el viejo hacía un solo recv() de 4096 B — un JSON
// fragmentado se perdía. El acumulador lee hasta 16 KiB con balanceo de
// llaves (consciente de strings/escapes); un JSON incompleto al agotar el
// timeout se descarta sin responder (nunca aplicar un comando a medias).
// Los comandos de texto se despachan por línea (o al timeout, sin '\n').
inline void handleCommandConnection(int cfd, CommandPlaneState& st,
                                    bool enforcePeerAuth) noexcept {
    if (enforcePeerAuth && !ivanna::security::isAuthorizedControlPeer(cfd)) {
        ::close(cfd);
        return;
    }
    // Fallo 2: registrar el PID de la app que conecta (peer creds). El
    // bucle principal vigila si muere y limpia desiredPerceptual.
    // Solo se registra si el peer es la app IVANNA (UID en
    // cachedIvannaAppUids); root/shell (ivanna_client efímero) NO: cuando
    // el cliente sale, el daemon no debe creer que "la app murió".
    // (peer_auth.hpp no expone controlPeerPid/kIvannaAppUid: se lee
    // SO_PEERCRED directo, igual que isAuthorizedControlPeer.)
    {
        struct PeerCreds { pid_t pid; uid_t uid; gid_t gid; } peer{};
        socklen_t peerLen = sizeof(peer);
        if (::getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &peer, &peerLen) == 0 &&
            peerLen == sizeof(peer) && peer.pid > 0) {
            const auto appUids = ivanna::security::cachedIvannaAppUids();
            if (std::find(appUids.begin(), appUids.end(), peer.uid) != appUids.end()) {
                st.appPid.store(static_cast<int32_t>(peer.pid), std::memory_order_relaxed);
            }
        }
    }
    struct timeval rcvTv{0, 150000};
    ::setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &rcvTv, sizeof(rcvTv));
    std::string req;
    req.reserve(1024);
    char chunk[2048];
    int depth = 0;
    bool started = false, inStr = false, esc = false, complete = false;
    bool isJson = false, decided = false;
    while (req.size() < 16384 && !complete) {
        const ssize_t n = ::recv(cfd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            const char c = chunk[i];
            if (!decided) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
                decided = true;
                isJson = (c == '{');
                if (!isJson) {
                    req.push_back(c);
                    started = true;
                    continue;
                }
            }
            if (!isJson) {
                if (c == '\n' || c == '\r') { complete = true; break; }
                req.push_back(c);
                if (req.size() >= 1024) { complete = true; break; }
                continue;
            }
            req.push_back(c);
            if (inStr) {
                if (esc) esc = false;
                else if (c == '\\') esc = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') inStr = true;
            else if (c == '{') { ++depth; started = true; }
            else if (c == '}' && started && --depth == 0) { complete = true; break; }
        }
    }
    (void)started;
    if (!req.empty()) {
        std::string reply;
        if (isJson) {
            if (complete) reply = handleCommand(req, st);
        } else {
            reply = handleTextCommand(req, st);
        }
        if (!reply.empty()) {
            (void)::send(cfd, reply.data(), reply.size(), MSG_NOSIGNAL);
        }
    }
    ::close(cfd);
}

// Bucle accept del hilo de comandos. Patrón del CommandServer viejo
// (select 1 s, un comando por conexión). Nunca bloquea el bucle de fusión:
// corre en su propio hilo. Si algo inesperado ocurre, el hilo muere y el
// proceso sigue (la fusión y el descubrimiento no dependen de él).
inline void serveCommandSocket(int listenFd, CommandPlaneState& st,
                               const std::atomic<bool>& running,
                               bool enforcePeerAuth) noexcept {
    try {
        while (running.load(std::memory_order_relaxed)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(listenFd, &rfds);
            struct timeval tv{1, 0};
            const int ret = ::select(listenFd + 1, &rfds, nullptr, nullptr, &tv);
            if (ret <= 0) continue;
            if (!FD_ISSET(listenFd, &rfds)) continue;
            const int cfd = ::accept(listenFd, nullptr, nullptr);
            if (cfd < 0) continue;
            st.clientsServed.fetch_add(1, std::memory_order_relaxed);
            handleCommandConnection(cfd, st, enforcePeerAuth);
        }
    } catch (...) {
        // El hilo de comandos nunca tumba el proceso.
    }
}

} // namespace ivanna::daemon
