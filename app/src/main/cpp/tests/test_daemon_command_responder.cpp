// test_daemon_command_responder.cpp — Regresión v2.5.440 (Ruta B muerta).
//
// CAUSA RAÍZ: el daemon v2 bindeaba el socket abstracto @omega_daemon_socket
// pero nadie aceptaba conexiones (el bucle drenaba accept()+close()): la
// app preguntaba GET_STATUS y jamás recibía respuesta, así que el efecto
// omega nunca se acoplaba a la sesión. Este test fija el contrato que
// repone control/daemon_command_responder.h:
//   1. HELLO con proto 2 -> proto 3, compatible true.
//   2. HELLO con proto 99 -> compatible false.
//   3. GET_STATUS con sesión viva 329/gen 8 -> JSON con ok:true y los
//      campos que parseControlPlaneEvidence (Kotlin) exige, incluida la
//      sesión descubierta.
//   4. Acción desconocida -> {"ok":false,"error":"unsupported"}.
//   5. Petición no-JSON -> sin respuesta (conexión cerrada, fail-closed).
//   6. Conexión silenciosa (sin datos en 150 ms) -> cerrada sin respuesta.
//   7. Mapeo beacon -> vista de estado (Processing/EnabledIdle/NoEffect/
//      Unavailable) puro, sin I/O.
//   8. Round-trip real por socket abstracto: el hilo servidor responde a
//      un cliente que escribe el JSON y lee hasta '}' balanceado (igual que
//      OmegaEngineBridge.exchange()).
//
// Sin NDK, sin red: sockets UNIX abstractos de Linux + std::thread.
// El socket de prueba usa nombre único por PID para no chocar bajo ctest -j.
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "../daemon/control/daemon_command_responder.h"
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

// Cliente mínimo que replica OmegaEngineBridge.exchange(): conecta, escribe
// el JSON, lee hasta '}' balanceado o EOF/timeout. Devuelve la respuesta
// (vacía si el servidor cerró sin responder).
std::string exchangeOnce(const std::string& sockName, const std::string& req,
                         int timeoutMs = 3000) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::string();
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const socklen_t len = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + 1 + sockName.size());
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, sockName.data(), sockName.size());
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), len) != 0) {
        ::close(fd);
        return std::string();
    }
    if (!req.empty()) {
        (void)::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    }
    std::string out;
    char buf[1024];
    int depth = 0;
    bool started = false;
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            out.push_back(c);
            if (c == '{') { ++depth; started = true; }
            if (c == '}' && started && --depth == 0) {
                ::close(fd);
                return out;
            }
        }
        if (out.size() > 65536) break;
    }
    ::close(fd);
    return out;
}

// Envía el JSON en 2 fragmentos (50 ms entre ellos) y lee la respuesta.
// Replica un cliente con el JSON fragmentado (Fallo 5: el viejo hacía un
// solo recv() de 4096 B y perdía el resto).
std::string exchangeFragmented(const std::string& sockName, const std::string& part1,
                               const std::string& part2, int timeoutMs = 3000) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return std::string();
    struct timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const socklen_t len = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + 1 + sockName.size());
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, sockName.data(), sockName.size());
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), len) != 0) {
        ::close(fd);
        return std::string();
    }
    (void)::send(fd, part1.data(), part1.size(), MSG_NOSIGNAL);
    ::usleep(50000);
    (void)::send(fd, part2.data(), part2.size(), MSG_NOSIGNAL);
    std::string out;
    char buf[1024];
    int depth = 0;
    bool started = false;
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        for (ssize_t i = 0; i < n; ++i) {
            const char c = buf[i];
            out.push_back(c);
            if (c == '{') { ++depth; started = true; }
            if (c == '}' && started && --depth == 0) {
                ::close(fd);
                return out;
            }
        }
        if (out.size() > 65536) break;
    }
    ::close(fd);
    return out;
}

int makeAbstractListener(const std::string& sockName) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const socklen_t len = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + 1 + sockName.size());
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, sockName.data(), sockName.size());
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), len) != 0) {
        ::close(fd);
        return -1;
    }
    if (::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

int main() {
    using namespace ivanna::daemon;

    std::printf("test_daemon_command_responder (v2.5.440)\n");

    // ── 1-4. Despacho puro (sin sockets) ──────────────────────────────────
    {
        CommandPlaneState st;
        st.sessionId.store(329, std::memory_order_relaxed);
        st.sessionGen.store(8, std::memory_order_relaxed);
        // 0044 (Fallo 3): "generation" es la del BUS (snapshotGeneration),
        // no la de fusión. Se ponen valores DISTINTOS para que el test
        // delate si se usa la equivocada: fusión 99999, bus 48440.
        st.generation.store(99999, std::memory_order_relaxed);
        st.snapshotGeneration.store(48440, std::memory_order_relaxed);
        st.verdict.store(IVANNA_VERDICT_BYPASS, std::memory_order_relaxed);
        st.startMs.store(1000, std::memory_order_relaxed);

        const std::string hello = handleCommand("{\"action\":\"HELLO\",\"proto\":2}", st);
        check(contains(hello, "\"ok\":true"), "HELLO proto 2 -> ok:true");
        check(contains(hello, "\"proto\":3"), "HELLO proto 2 -> proto 3");
        check(contains(hello, "\"compatible\":true"), "HELLO proto 2 -> compatible true");

        const std::string helloNew = handleCommand("{\"action\":\"HELLO\",\"proto\":99}", st);
        check(contains(helloNew, "\"compatible\":false"), "HELLO proto 99 -> compatible false");

        const std::string status = buildStatusReply(st, 5000);
        check(contains(status, "\"ok\":true"), "GET_STATUS -> ok:true");
        check(contains(status, "\"discovered_session_id\":329"), "GET_STATUS -> session 329");
        check(contains(status, "\"discovered_session_gen\":8"), "GET_STATUS -> gen 8");
        // FASE A: sin packages publicados, los campos van vacíos (daemon
        // viejo / aún sin descubrir): JSON válido, sin inventar packages.
        check(contains(status, "\"discovered_session_package\":\"\""), "GET_STATUS -> discovered_session_package vacío por defecto");
        check(contains(status, "\"foreground_package\":\"\""), "GET_STATUS -> foreground_package vacío por defecto");
        // FASE A: con packages, GET_STATUS los publica (la app expira la
        // guarda de huérfano al cambiar de app y no re-acopla sesiones rancias).
        setDiscoveredSessionPackages(st, "com.aspiro.tidal", "com.aspiro.tidal");
        const std::string statusPkg = buildStatusReply(st, 6000);
        check(contains(statusPkg, "\"discovered_session_package\":\"com.aspiro.tidal\""), "GET_STATUS -> discovered_session_package tidal");
        check(contains(statusPkg, "\"foreground_package\":\"com.aspiro.tidal\""), "GET_STATUS -> foreground_package tidal");
        check(contains(status, "\"generation\":48440"), "GET_STATUS -> generation es la del BUS (48440), no la de fusión (99999)");
        check(contains(status, "\"effect_state\":-1"), "GET_STATUS sin beacon -> effect_state -1");
        check(contains(status, "\"status\":\"bypass\""), "GET_STATUS sin beacon -> status bypass");
        check(contains(status, "\"upmixing_enabled\":0.0"), "GET_STATUS -> upmixing 0.0");
        check(contains(status, "\"wfs_enabled\":0.0"), "GET_STATUS -> wfs 0.0");
        check(contains(status, "\"uptime_ms\":4000"), "GET_STATUS -> uptime 4000 ms");
        check(contains(status, "\"consumer\":null"), "GET_STATUS sin efecto -> consumer null");

        // 0044 (Fallo 1): SET_INTENSITY ya NO es "unsupported" (es
        // comando real). La acción desconocida pasa a ser SET_NO_EXISTE.
        const std::string intensity = handleCommand("{\"action\":\"SET_INTENSITY\",\"intensity\":0.5}", st);
        check(contains(intensity, "\"ok\":true") &&
              contains(intensity, "accepted_pending_consumer"),
              "0044: SET_INTENSITY -> ok:true accepted_pending_consumer");
        const std::string unknown = handleCommand("{\"action\":\"SET_NO_EXISTE\",\"x\":1}", st);
        check(contains(unknown, "\"ok\":false") && contains(unknown, "unsupported"),
              "acción desconocida -> ok:false unsupported");

        const std::string bad = handleCommand("esto no es json", st);
        check(contains(bad, "\"ok\":false") && contains(bad, "bad_request"),
              "petición sin action -> ok:false bad_request");
    }

    // ── 0044 T1. Booleanos JSON + PING (Fallo 5) ──────────────────────────
    {
        CommandPlaneState st;
        // "bypass":true debe leerse como 1.0 (antes strtod daba 0.0, igual
        // que false) y quedar stasheado como masterBypass.
        const std::string r1 = handleCommand("{\"action\":\"SET_BYPASS\",\"bypass\":true}", st);
        check(contains(r1, "\"ok\":true"), "0044 T1: SET_BYPASS bypass:true -> ok:true");
        PendingPerceptual taken;
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.masterBypass) &&
              taken.masterBypass >= 0.5f,
              "0044 T1: bypass:true -> masterBypass=1.0 stasheado");
        const std::string r2 = handleCommand("{\"action\":\"SET_BYPASS\",\"bypass\":false}", st);
        check(contains(r2, "\"ok\":true"), "0044 T1: SET_BYPASS bypass:false -> ok:true");
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.masterBypass) &&
              taken.masterBypass < 0.5f,
              "0044 T1: bypass:false -> masterBypass=0.0 stasheado");
        const std::string ping = handleCommand("{\"action\":\"PING\"}", st);
        check(contains(ping, "\"ok\":true") && contains(ping, "\"command\":\"PING\""),
              "0044 T1: PING -> ok:true");
    }

    // ── 0044 T2. desired sobrevive al cambio de sesión (Fallo 2) ───────────
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":0.5,\"holosAmount\":0.25}", st);
        check(contains(r, "\"ok\":true"), "0044 T2: SET_PERCEPTUAL_STATE -> ok:true");
        PendingPerceptual taken;
        check(takePendingPerceptual(st, taken), "0044 T2: takePending vacía el pendiente");
        check(!std::isnan(taken.orbeAmount) && taken.orbeAmount == 0.5f,
              "0044 T2: el pendiente traía orbeAmount 0.5");
        // El pendiente ya está vacío, pero desiredPerceptual conserva los
        // efectos (el bucle lo re-siembra al cambiar de sesión).
        PendingPerceptual desired;
        check(copyDesiredPerceptual(st, desired) && !std::isnan(desired.orbeAmount) &&
              desired.orbeAmount == 0.5f && !std::isnan(desired.holosAmount) &&
              desired.holosAmount == 0.25f,
              "0044 T2: desired sobrevive a takePending (orbe 0.5, holos 0.25)");
        check(!takePendingPerceptual(st, taken),
              "0044 T2: segundo takePending -> nada pendiente");
        clearOnAppExit(st);
        check(!copyDesiredPerceptual(st, desired),
              "0044 T2: clearOnAppExit (app muerta) -> desired vacío");
    }

    // ── 0044 T3. generation del BUS en buildStatusReply (Fallo 3) ──────────
    {
        CommandPlaneState st;
        st.sessionId.store(7, std::memory_order_relaxed);
        st.generation.store(99999, std::memory_order_relaxed);  // fusión: debe ignorarse
        st.snapshotGeneration.store(777, std::memory_order_relaxed);  // bus: es la que cuenta
        st.verdict.store(IVANNA_VERDICT_BYPASS, std::memory_order_relaxed);
        st.startMs.store(0, std::memory_order_relaxed);
        const std::string status = buildStatusReply(st, 1000);
        check(contains(status, "\"generation\":777"),
              "0044 T3: buildStatusReply -> generation 777 (bus), no 99999 (fusión)");
        check(!contains(status, "\"generation\":99999"),
              "0044 T3: buildStatusReply -> no contiene la generación de fusión");
    }

    // ── 7. Mapeo beacon -> vista de estado (puro) ──────────────────────────
    {
        StatusView v = computeStatusView(100, ivanna::EffectBeaconState::Processing,
                                         12345, 100, IVANNA_VERDICT_BYPASS);
        check(v.effectState == 2 && std::string(v.status) == "dsp_block_applied" &&
              std::string(v.consumer) == "omega_effect" && v.dspFrames == 12345,
              "beacon Processing + gen aplicada -> dsp_block_applied");
        v = computeStatusView(100, ivanna::EffectBeaconState::Processing,
                              12345, 90, IVANNA_VERDICT_BYPASS);
        check(std::string(v.status) == "awaiting_dsp_generation",
              "beacon Processing + gen vieja -> awaiting_dsp_generation");
        v = computeStatusView(100, ivanna::EffectBeaconState::EnabledIdle,
                              0, 0, IVANNA_VERDICT_BYPASS);
        check(v.effectState == 1 && std::string(v.status) == "effect_enabled_idle",
              "beacon EnabledIdle -> effect_enabled_idle");
        v = computeStatusView(100, ivanna::EffectBeaconState::NoEffect,
                              0, 0, IVANNA_VERDICT_LOADED);
        check(v.effectState == 0 && std::string(v.status) == "loaded",
              "beacon NoEffect -> status del veredicto (loaded)");
        v = computeStatusView(100, ivanna::EffectBeaconState::Unavailable,
                              0, 0, IVANNA_VERDICT_BYPASS);
        check(v.effectState == -1 && std::string(v.consumer) == "null",
              "beacon Unavailable -> effect_state -1, consumer null");
    }

    // ── 5, 6, 8. Round-trip real por socket abstracto ──────────────────────
    {
        const std::string sockName =
            "ivanna_test_cmd_" + std::to_string(static_cast<long long>(::getpid()));
        const int listenFd = makeAbstractListener(sockName);
        check(listenFd >= 0, "socket abstracto de prueba bindeado");
        if (listenFd >= 0) {
            CommandPlaneState st;
            st.sessionId.store(329, std::memory_order_relaxed);
            st.sessionGen.store(8, std::memory_order_relaxed);
            st.generation.store(48440, std::memory_order_relaxed);
            st.verdict.store(IVANNA_VERDICT_LOADED, std::memory_order_relaxed);
            st.startMs.store(1000, std::memory_order_relaxed);
            std::atomic<bool> running{true};
            // enforcePeerAuth=false: en host no existe /data/data/com.ivanna.omega
            // (el UID del test no estaría autorizado); en el daemon es true.
            std::thread server(serveCommandSocket, listenFd, std::ref(st),
                               std::cref(running), false);

            const std::string r1 = exchangeOnce(sockName, "{\"action\":\"GET_STATUS\"}");
            check(contains(r1, "\"ok\":true") &&
                  contains(r1, "\"discovered_session_id\":329") &&
                  contains(r1, "\"discovered_session_gen\":8") &&
                  contains(r1, "\"status\":\"loaded\""),
                  "socket: GET_STATUS responde sesión 329/gen 8 (veredicto loaded)");

            const std::string r2 = exchangeOnce(sockName, "{\"action\":\"HELLO\",\"proto\":2}");
            check(contains(r2, "\"proto\":3") && contains(r2, "\"compatible\":true"),
                  "socket: HELLO responde proto 3");

            const std::string r3 = exchangeOnce(sockName, "basura sin llaves");
            check(r3.empty(), "socket: petición no-JSON -> sin respuesta");

            // Conexión silenciosa: sin escribir nada, el servidor cierra
            // a los ~150 ms sin responder (recv devuelve 0/EOF).
            const std::string r4 = exchangeOnce(sockName, "", 3000);
            check(r4.empty(), "socket: conexión silenciosa -> cerrada sin respuesta");

            // Una segunda consulta tras las anteriores: el servidor sigue
            // vivo (un comando por conexión, sin estado stale).
            const std::string r5 = exchangeOnce(sockName, "{\"action\":\"GET_STATUS\"}");
            check(contains(r5, "\"discovered_session_id\":329"),
                  "socket: segunda GET_STATUS tras varios comandos -> sigue vivo");
            check(st.clientsServed.load() >= 5, "socket: clients_served cuenta las conexiones");

            running.store(false);
            server.join();
            ::close(listenFd);
        }
    }

    // ── 0044 T4. JSON fragmentado se reensambla (Fallo 5) ─────────────────
    {
        const std::string sockName =
            "ivanna_test_frag_" + std::to_string(static_cast<long long>(::getpid()));
        const int listenFd = makeAbstractListener(sockName);
        check(listenFd >= 0, "0044 T4: socket abstracto de prueba bindeado");
        if (listenFd >= 0) {
            CommandPlaneState st;
            st.sessionId.store(329, std::memory_order_relaxed);
            st.sessionGen.store(8, std::memory_order_relaxed);
            st.verdict.store(IVANNA_VERDICT_LOADED, std::memory_order_relaxed);
            st.startMs.store(1000, std::memory_order_relaxed);
            std::atomic<bool> running{true};
            std::thread server(serveCommandSocket, listenFd, std::ref(st),
                               std::cref(running), false);
            // El JSON llega en 2 fragmentos (50 ms entre ellos, < 150 ms
            // de RCVTIMEO): el acumulador debe reensamblarlo y responder.
            const std::string r = exchangeFragmented(sockName, "{\"action\":\"GET_STATUS\"",
                                                     "}", 3000);
            check(contains(r, "\"ok\":true") &&
                  contains(r, "\"discovered_session_id\":329"),
                  "0044 T4: JSON en 2 fragmentos -> GET_STATUS responde sesión 329");
            // Comando de texto por socket (MagiskBridge.sendCommand(String)).
            const std::string rb = exchangeOnce(sockName, "SET_BYPASS:1");
            check(contains(rb, "\"ok\":true") && contains(rb, "\"bypass\":true"),
                  "0044 T4: texto SET_BYPASS:1 -> ok:true bypass:true");
            const std::string rs = exchangeOnce(sockName, "STATUS");
            check(contains(rs, "\"ok\":true") && contains(rs, "\"command\":\"GET_STATUS\""),
                  "0044 T4: texto STATUS -> GET_STATUS");
            running.store(false);
            server.join();
            ::close(listenFd);
        }
    }

    // ── v2.5.472 (Fase 2, ítem E). parallaxAmount escala la pose de Ruta B ──
    // Sin parallax guardada la pose pasa íntegra (default 1.0). Guardada
    // por SET_PERCEPTUAL_STATE (la trae buildFusedPerceptualFields desde
    // SpatialControls), la próxima SET_HEAD_POSE se escala. Un comando de
    // pose puede traer su propio parallaxAmount y sobreescribirla. El
    // snapshot ABI v13 NO cambia: la escala se aplica al parsear.
    {
        CommandPlaneState st;
        PendingPerceptual taken;
        const std::string r1 = handleCommand(
            "{\"action\":\"SET_HEAD_POSE\",\"yawRad\":0.5,\"pitchRad\":0.2}", st);
        check(contains(r1, "\"ok\":true"), "parallax: SET_HEAD_POSE sin parallax -> ok:true");
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.headYawRad) &&
              std::fabs(taken.headYawRad - 0.5f) < 1e-6f &&
              std::fabs(taken.headPitchRad - 0.2f) < 1e-6f,
              "parallax: sin guardar, yaw 0.5/pitch 0.2 pasan íntegros (default 1.0)");
        const std::string r2 = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"parallaxAmount\":0}", st);
        check(contains(r2, "\"ok\":true"), "parallax: SET_PERCEPTUAL_STATE parallaxAmount 0 -> ok:true");
        PendingPerceptual desired;
        check(copyDesiredPerceptual(st, desired) && !std::isnan(desired.parallaxAmount) &&
              desired.parallaxAmount == 0.f,
              "parallax: parallaxAmount 0 queda guardado como deseado");
        const std::string r3 = handleCommand(
            "{\"action\":\"SET_HEAD_POSE\",\"yawRad\":0.5,\"pitchRad\":0.2}", st);
        check(contains(r3, "\"ok\":true"), "parallax: SET_HEAD_POSE con parallax 0 -> ok:true");
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.headYawRad) &&
              taken.headYawRad == 0.f && taken.headPitchRad == 0.f,
              "parallax: con parallax 0 guardado, yaw/pitch se anulan (pose no mueve el sonido)");
        const std::string r4 = handleCommand(
            "{\"action\":\"SET_HEAD_POSE\",\"yawRad\":0.5,\"pitchRad\":0.2,\"parallaxAmount\":0.5}", st);
        check(contains(r4, "\"ok\":true"), "parallax: SET_HEAD_POSE con parallax 0.5 -> ok:true");
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.headYawRad) &&
              std::fabs(taken.headYawRad - 0.25f) < 1e-6f &&
              std::fabs(taken.headPitchRad - 0.1f) < 1e-6f,
              "parallax: parallax 0.5 en el comando -> yaw 0.25/pitch 0.1 (mitad)");
        // SET_SCENE_POSE escala igual que SET_HEAD_POSE.
        const std::string r5 = handleCommand(
            "{\"action\":\"SET_SCENE_POSE\",\"azimuthRad\":0.6,\"elevationRad\":0.3,\"parallaxAmount\":0.5}", st);
        check(contains(r5, "\"ok\":true"), "parallax: SET_SCENE_POSE con parallax 0.5 -> ok:true");
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.headYawRad) &&
              std::fabs(taken.headYawRad - 0.3f) < 1e-6f &&
              std::fabs(taken.headPitchRad - 0.15f) < 1e-6f,
              "parallax: SET_SCENE_POSE parallax 0.5 -> yaw 0.3/pitch 0.15 (mitad)");
    }

    // ── 0044 T5. SHM nueva nace neutra (Fallo 4) ───────────────────────────
    {
        // openWriter sobre un archivo NUEVO debe inicializarlo con
        // makeNeutral() (orbe/holos 0, intensity 1.0), no makeDefault()
        // (coloreado: HRTF on, widener, intensity 0.92).
        const std::string path =
            "/tmp/ivanna_test_bus_" + std::to_string(static_cast<long long>(::getpid()));
        ::unlink(path.c_str());
        ivanna::OmegaControlBus bus;
        check(bus.openWriter(path.c_str()), "0044 T5: openWriter sobre archivo nuevo");
        ivanna::OmegaDspSnapshot snap{};
        // readLatest solo devuelve generaciones NUEVAS (gen == lastSeen ->
        // "nada nuevo", false). El snapshot inicial tiene gen 0, así que
        // lastSeen debe ser != 0 para que lo devuelva (aquí ~0ULL).
        uint64_t lastSeen = ~0ULL;
        check(bus.readLatest(snap, lastSeen), "0044 T5: readLatest lee el snapshot inicial");
        check(snap.orbe_amount == 0.f && snap.holos_amount == 0.f,
              "0044 T5: snapshot inicial neutro (orbe 0, holos 0)");
        check(snap.intensity == 1.0f && snap.spatial_width == 1.0f,
              "0044 T5: snapshot inicial neutro (intensity 1.0, spatial_width 1.0)");
        ::unlink(path.c_str());
    }

    // ── v2.5.486 (ABI v16). Los 16 comandos portados del handler muerto ──
    // Cada SET se despacha, se verifica la respuesta, se toma el pendiente
    // (takePendingPerceptual) y se aplica al snapshot (applyPendingPerceptual)
    // para verificar que el campo llega al sitio correcto. Los GET_* se
    // verifican contra el estado deseado acumulado.
    {
        using namespace ivanna;
        CommandPlaneState st;
        PendingPerceptual taken;
        OmegaDspSnapshot snap;

        // SET_FOTOHOLO_STATE: enabled + intensidad -> bits 6..15 de
        // temporal_field (encode 9 bits: 0.5 -> 256/511 ≈ 0.501).
        const std::string fh = handleCommand(
            "{\"action\":\"SET_FOTOHOLO_STATE\",\"enabled\":1,\"intensity\":0.5}", st);
        check(contains(fh, "\"ok\":true") && contains(fh, "accepted_pending_consumer"),
              "v16: SET_FOTOHOLO_STATE -> ok:true accepted_pending_consumer");
        check(takePendingPerceptual(st, taken) && !std::isnan(taken.fotoholoEnabled) &&
              taken.fotoholoEnabled == 1.f && !std::isnan(taken.fotoholoIntensity) &&
              taken.fotoholoIntensity == 0.5f,
              "v16: SET_FOTOHOLO_STATE stashea enabled 1 / intensity 0.5");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        {
            bool on = false; float inten = 0.f;
            omega_fotoholo_decode(snap.temporal_field, on, inten);
            check(on && std::fabs(inten - 0.5f) < 0.01f,
                  "v16: SET_FOTOHOLO_STATE aplica bits 6..15 (on, intensidad ~0.5)");
        }

        // SET_UPMIXING: enabled + immersivity -> campos propios.
        const std::string up = handleCommand(
            "{\"action\":\"SET_UPMIXING\",\"upmixingEnabled\":1,\"upmixingImmersivity\":0.7}", st);
        check(contains(up, "\"ok\":true"), "v16: SET_UPMIXING -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.upmixingEnabled == 1.f &&
              taken.upmixingImmersivity == 0.7f,
              "v16: SET_UPMIXING stashea enabled 1 / immersivity 0.7");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check(snap.upmixing_enabled == 1u && std::fabs(snap.upmixing_immersivity - 0.7f) < 1e-6f,
              "v16: SET_UPMIXING aplica upmixing_enabled 1 / immersivity 0.7");

        // SET_COCHLEAR_INTENSITY + SET_COCHLEAR_INVERSE_ENABLED (bool true).
        const std::string ci = handleCommand(
            "{\"action\":\"SET_COCHLEAR_INTENSITY\",\"intensity\":0.6}", st);
        check(contains(ci, "\"ok\":true"), "v16: SET_COCHLEAR_INTENSITY -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.cochlearIntensity == 0.6f,
              "v16: SET_COCHLEAR_INTENSITY stashea intensity 0.6");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check(std::fabs(snap.cochlear_intensity - 0.6f) < 1e-6f,
              "v16: SET_COCHLEAR_INTENSITY aplica cochlear_intensity 0.6");
        const std::string ce = handleCommand(
            "{\"action\":\"SET_COCHLEAR_INVERSE_ENABLED\",\"enabled\":true}", st);
        check(contains(ce, "\"ok\":true"), "v16: SET_COCHLEAR_INVERSE_ENABLED true -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.cochlearEnabled == 1.f,
              "v16: SET_COCHLEAR_INVERSE_ENABLED true -> cochlearEnabled 1 (bool leido)");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check((snap.flags & OMEGA_FLAG_COCHLEAR_ON) != 0u,
              "v16: SET_COCHLEAR_INVERSE_ENABLED aplica flag COCHLEAR_ON");

        // SET_SPSC_RING_FACTOR: por fin con slot (ABI v16).
        const std::string sp = handleCommand(
            "{\"action\":\"SET_SPSC_RING_FACTOR\",\"factor\":1.35}", st);
        check(contains(sp, "\"ok\":true"), "v16: SET_SPSC_RING_FACTOR -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.spscRingFactor == 1.35f,
              "v16: SET_SPSC_RING_FACTOR stashea factor 1.35");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check(std::fabs(snap.spsc_ring_factor - 1.35f) < 1e-6f,
              "v16: SET_SPSC_RING_FACTOR aplica spsc_ring_factor 1.35");

        // SET_PERCEPTUAL_STATE con conservación (ABI v17): los 4 campos
        // consv_* viajan por el canal fusionado, se stashean y se aplican
        // al snapshot con clamp. Un valor fuera de rango se clampa
        // (energyCap 2.0 -> 1.0; inertia 1.5 -> 0.99).
        const std::string cv = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"consvAmount\":0.6,"
            "\"consvEnergyCap\":2.0,\"consvPhaseCoherence\":0.75,\"consvInertia\":1.5}", st);
        check(contains(cv, "\"ok\":true"), "v17: SET_PERCEPTUAL_STATE consv_* -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.consvAmount == 0.6f &&
              taken.consvEnergyCap == 2.0f && taken.consvPhaseCoherence == 0.75f &&
              taken.consvInertia == 1.5f,
              "v17: SET_PERCEPTUAL_STATE stashea consv 0.6/2.0/0.75/1.5");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check(std::fabs(snap.consv_amount - 0.6f) < 1e-6f &&
              std::fabs(snap.consv_energy_cap - 1.0f) < 1e-6f &&
              std::fabs(snap.consv_phase_coherence - 0.75f) < 1e-6f &&
              std::fabs(snap.consv_inertia - 0.99f) < 1e-6f,
              "v17: SET_PERCEPTUAL_STATE aplica consv_* con clamp (0.6/1.0/0.75/0.99)");
        // Merge parcial: un SET_PERCEPTUAL_STATE sin consv_* conserva los
        // valores vigentes (no los resetea a NaN/default).
        const std::string cv2 = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":0.5}", st);
        check(contains(cv2, "\"ok\":true"), "v17: SET_PERCEPTUAL_STATE parcial -> ok:true");
        check(takePendingPerceptual(st, taken) && std::isnan(taken.consvAmount) &&
              taken.orbeAmount == 0.5f,
              "v17: SET_PERCEPTUAL_STATE parcial no trae consv_* (NaN) y sí orbeAmount");

        // SET_REALITY_RECONSTRUCTION: flag + intensidad (slot p.intensity).
        const std::string rr = handleCommand(
            "{\"action\":\"SET_REALITY_RECONSTRUCTION\",\"enabled\":1,\"intensity\":0.8}", st);
        check(contains(rr, "\"ok\":true"), "v16: SET_REALITY_RECONSTRUCTION -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.realityReconEnabled == 1.f &&
              taken.intensity == 0.8f,
              "v16: SET_REALITY_RECONSTRUCTION stashea enabled 1 / intensity 0.8");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check((snap.flags & OMEGA_FLAG_REALITY_RECON_ON) != 0u &&
              std::fabs(snap.intensity - 0.8f) < 1e-6f,
              "v16: SET_REALITY_RECONSTRUCTION aplica flag REALITY_RECON_ON + intensity 0.8");

        // SET_ROUTE_PROFILE: claves con nombre y alias a/b/c.
        const std::string rp = handleCommand(
            "{\"action\":\"SET_ROUTE_PROFILE\",\"bassBoostDb\":2.5,\"dialogBoostDb\":1.0,\"widenerMult\":1.2}", st);
        check(contains(rp, "\"ok\":true"), "v16: SET_ROUTE_PROFILE -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.bassBoostDb == 2.5f &&
              taken.dialogBoostDb == 1.0f && taken.widenerMult == 1.2f,
              "v16: SET_ROUTE_PROFILE stashea bass 2.5 / dialog 1.0 / widener 1.2");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check(std::fabs(snap.bass_boost_db - 2.5f) < 1e-6f &&
              std::fabs(snap.dialog_boost_db - 1.0f) < 1e-6f &&
              std::fabs(snap.widener_mult - 1.2f) < 1e-6f,
              "v16: SET_ROUTE_PROFILE aplica bass/dialog/widener");
        const std::string rp2 = handleCommand(
            "{\"action\":\"SET_ROUTE_PROFILE\",\"a\":3.0,\"b\":0.5,\"c\":1.5}", st);
        check(contains(rp2, "\"ok\":true"), "v16: SET_ROUTE_PROFILE a/b/c -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.bassBoostDb == 3.0f &&
              taken.dialogBoostDb == 0.5f && taken.widenerMult == 1.5f,
              "v16: SET_ROUTE_PROFILE alias a/b/c tienen prioridad (3.0/0.5/1.5)");

        // SET_YAMNET_SCORES: válido -> ok + slots; inválido -> error.
        const std::string ym = handleCommand(
            "{\"action\":\"SET_YAMNET_SCORES\",\"speech\":0.8,\"music\":0.15,\"classId\":137,\"confidence\":0.9}", st);
        check(contains(ym, "\"ok\":true"), "v16: SET_YAMNET_SCORES válido -> ok:true");
        check(takePendingPerceptual(st, taken) && taken.yamnetSpeech == 0.8f &&
              taken.yamnetMusic == 0.15f && taken.yamnetClassId == 137.f &&
              taken.yamnetConfidence == 0.9f,
              "v16: SET_YAMNET_SCORES stashea speech/music/class/conf");
        snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, taken);
        check(std::fabs(snap.yamnet_speech - 0.8f) < 1e-6f &&
              std::fabs(snap.yamnet_music - 0.15f) < 1e-6f &&
              snap.yamnet_class_id == 137 &&
              std::fabs(snap.yamnet_confidence - 0.9f) < 1e-6f,
              "v16: SET_YAMNET_SCORES aplica los 4 campos al snapshot");
        const std::string ymBad = handleCommand(
            "{\"action\":\"SET_YAMNET_SCORES\",\"speech\":1.5,\"music\":0.15,\"classId\":137,\"confidence\":0.9}", st);
        check(contains(ymBad, "\"ok\":false") && contains(ymBad, "invalid_classifier_payload"),
              "v16: SET_YAMNET_SCORES speech 1.5 -> ok:false invalid_classifier_payload");
        const std::string ymBad2 = handleCommand(
            "{\"action\":\"PUSH_YAMNET_SCORES\",\"speech\":0.8,\"music\":0.15,\"classId\":521,\"confidence\":0.9}", st);
        check(contains(ymBad2, "\"ok\":false") && contains(ymBad2, "invalid_classifier_payload"),
              "v16: PUSH_YAMNET_SCORES classId 521 -> ok:false invalid_classifier_payload");
        const std::string ymBad3 = handleCommand(
            "{\"action\":\"SET_YAMNET_SCORES\",\"speech\":0.8,\"music\":0.15,\"classId\":1.5,\"confidence\":0.9}", st);
        check(contains(ymBad3, "\"ok\":false") && contains(ymBad3, "invalid_classifier_payload"),
              "v16: SET_YAMNET_SCORES classId 1.5 (no entero) -> ok:false");
        check(!takePendingPerceptual(st, taken),
              "v16: SET_YAMNET_SCORES inválido no stashea nada");

        // SET_PINNA_METRICS: normalización idéntica al servidor viejo
        // (concha (20-18)/10=0.2, helix (70-60)/25=0.4, fosa (18-15)/10=0.3
        // -> head_width) y respuesta con los valores crudos.
        const std::string pm = handleCommand(
            "{\"action\":\"SET_PINNA_METRICS\",\"concha\":20.0,\"helix\":70.0,\"fosa\":18.0}", st);
        check(contains(pm, "\"ok\":true") && contains(pm, "\"concha\":20.000") &&
              contains(pm, "\"helix\":70.000") && contains(pm, "\"fosa\":18.000"),
              "v16: SET_PINNA_METRICS -> ok:true + valores crudos en la respuesta");
        check(takePendingPerceptual(st, taken) &&
              std::fabs(taken.supremePinnaConchaDepth - 0.2f) < 1e-6f &&
              std::fabs(taken.supremePinnaHelixCurl - 0.4f) < 1e-6f &&
              std::fabs(taken.supremePinnaHeadWidth - 0.3f) < 1e-6f,
              "v16: SET_PINNA_METRICS normaliza concha 0.2 / helix 0.4 / fosa->width 0.3");
        const std::string pm2 = handleCommand(
            "{\"action\":\"SET_PINNA_METRICS\",\"width\":25.0,\"height\":80.0,\"depth\":20.0}", st);
        check(contains(pm2, "\"ok\":true"), "v16: SET_PINNA_METRICS width/height/depth -> ok:true");
        check(takePendingPerceptual(st, taken) &&
              std::fabs(taken.supremePinnaConchaDepth - 0.7f) < 1e-6f &&
              std::fabs(taken.supremePinnaHelixCurl - 0.8f) < 1e-6f &&
              std::fabs(taken.supremePinnaHeadWidth - 0.5f) < 1e-6f,
              "v16: SET_PINNA_METRICS width/height/depth sobreescriben (0.7/0.8/0.5)");

        // SET_PRESET (JSON): Warm -> los 9 campos del preset; desconocido -> error.
        const std::string pr = handleCommand(
            "{\"action\":\"SET_PRESET\",\"preset\":\"Warm\"}", st);
        check(contains(pr, "\"ok\":true") && contains(pr, "accepted_pending_consumer"),
              "v16: SET_PRESET Warm -> ok:true accepted_pending_consumer");
        check(takePendingPerceptual(st, taken) &&
              taken.spatialWidth == 1.1f && taken.harmonicGain == 0.4f &&
              taken.bassBoostDb == 3.f && taken.dialogBoostDb == 1.f &&
              taken.widenerMult == 1.1f && taken.antiDolby == 0.85f,
              "v16: SET_PRESET Warm stashea spatial 1.1 / harmonic 0.4 / bass 3 / dialog 1 / widener 1.1");
        const std::string prBad = handleCommand(
            "{\"action\":\"SET_PRESET\",\"preset\":\"NoExiste\"}", st);
        check(contains(prBad, "\"ok\":false") && contains(prBad, "unknown_preset"),
              "v16: SET_PRESET NoExiste -> ok:false unknown_preset");
        const std::string prNoKey = handleCommand("{\"action\":\"SET_PRESET\"}", st);
        check(contains(prNoKey, "\"ok\":false") && contains(prNoKey, "bad_request"),
              "v16: SET_PRESET sin clave preset -> ok:false bad_request");

        // SET_HIRES: validación del contrato (en host /data/adb no existe:
        // un rate/depth válido llega a write_failed; un rate fuera de
        // contrato se rechaza antes de tocar el disco).
        const std::string hrBad = handleCommand(
            "{\"action\":\"SET_HIRES\",\"rate\":12345,\"depth\":24}", st);
        check(contains(hrBad, "\"ok\":false") && contains(hrBad, "invalid_value"),
              "v16: SET_HIRES rate 12345 -> ok:false invalid_value");
        const std::string hrOk = handleCommand(
            "{\"action\":\"SET_HIRES\",\"rate\":96000,\"depth\":24}", st);
        check(contains(hrOk, "\"ok\":false") && contains(hrOk, "write_failed"),
              "v16: SET_HIRES rate 96000 válido en host -> write_failed (sin /data/adb)");

        // GET_ROOM_STATUS: defaults sin SET previo; valores tras SET_ROOM_RT60.
        {
            CommandPlaneState st2;
            const std::string room0 = handleCommand("{\"action\":\"GET_ROOM_STATUS\"}", st2);
            check(contains(room0, "\"ok\":true") && contains(room0, "\"rt60\":0.340") &&
                  contains(room0, "\"room_idx\":51") && contains(room0, "\"wet\":0.220"),
                  "v16: GET_ROOM_STATUS sin SET -> defaults 0.34 / 51 / 0.22");
            const std::string rr1 = handleCommand(
                "{\"action\":\"SET_ROOM_RT60\",\"rt60\":1.2,\"wet\":0.5,\"idx\":3}", st2);
            check(contains(rr1, "\"ok\":true"), "v16: SET_ROOM_RT60 previo a GET_ROOM_STATUS -> ok:true");
            const std::string room1 = handleCommand("{\"action\":\"GET_ROOM_STATUS\"}", st2);
            check(contains(room1, "\"rt60\":1.200") && contains(room1, "\"room_idx\":3") &&
                  contains(room1, "\"wet\":0.500"),
                  "v16: GET_ROOM_STATUS tras SET_ROOM_RT60 -> 1.2 / 3 / 0.5");
        }

        // GET_HEAD_POSE_STATUS: sin pose pedida -> requested:false,
        // applied:false (sin beacon en host).
        {
            CommandPlaneState st3;
            const std::string hp0 = handleCommand("{\"action\":\"GET_HEAD_POSE_STATUS\"}", st3);
            check(contains(hp0, "\"ok\":true") && contains(hp0, "\"applied\":false") &&
                  contains(hp0, "\"requested\":false"),
                  "v16: GET_HEAD_POSE_STATUS sin pose -> applied:false requested:false");
            const std::string hp1 = handleCommand(
                "{\"action\":\"SET_HEAD_POSE\",\"yawRad\":0.5,\"pitchRad\":0.2}", st3);
            check(contains(hp1, "\"ok\":true"), "v16: SET_HEAD_POSE previo -> ok:true");
            const std::string hp2 = handleCommand("{\"action\":\"GET_HEAD_POSE_STATUS\"}", st3);
            check(contains(hp2, "\"requested\":true") && contains(hp2, "\"applied\":false") &&
                  contains(hp2, "\"yaw_rad\":0.500") && contains(hp2, "\"pitch_rad\":0.200"),
                  "v16: GET_HEAD_POSE_STATUS tras SET_HEAD_POSE -> requested:true, pose 0.5/0.2, applied:false (sin beacon)");
        }

        // GET_AUDIO_FEATURES_STATUS: sin toggles -> no_request; con toggle
        // y sin evidencia en host -> pending, applied:false.
        {
            CommandPlaneState st4;
            const std::string af0 = handleCommand("{\"action\":\"GET_AUDIO_FEATURES_STATUS\"}", st4);
            check(contains(af0, "\"status\":\"no_request\"") && contains(af0, "\"applied\":false"),
                  "v16: GET_AUDIO_FEATURES_STATUS sin SET -> no_request");
            const std::string af1 = handleCommand(
                "{\"action\":\"SET_AUDIO_FEATURES\",\"hrtfEnabled\":1}", st4);
            check(contains(af1, "\"ok\":true"), "v16: SET_AUDIO_FEATURES previo -> ok:true");
            const std::string af2 = handleCommand("{\"action\":\"GET_AUDIO_FEATURES_STATUS\"}", st4);
            check(contains(af2, "\"status\":\"pending\"") && contains(af2, "\"applied\":false"),
                  "v16: GET_AUDIO_FEATURES_STATUS con toggle y sin evidencia -> pending");
        }

        // GET_HEALTH: en host no hay bus escritor ni beacon -> healthy:false,
        // generation 0, consumer null, bus_open false (sin inventar salud).
        {
            CommandPlaneState st5;
            const std::string h = handleCommand("{\"action\":\"GET_HEALTH\"}", st5);
            check(contains(h, "\"ok\":true") && contains(h, "\"generation\":0") &&
                  contains(h, "\"consumer\":null") && contains(h, "\"bus_open\":false") &&
                  contains(h, "\"healthy\":false"),
                  "v16: GET_HEALTH en host -> gen 0, consumer null, bus_open false, healthy false");
        }

        // Texto SET_PRESET:<nombre> (MagiskBridge.setPreset): forma del
        // servidor viejo ({"ok":true,"preset":"...","gen":...}).
        {
            CommandPlaneState st6;
            const std::string t1 = handleTextCommand("SET_PRESET:Spatial", st6);
            check(contains(t1, "\"ok\":true") && contains(t1, "\"preset\":\"Spatial\"") &&
                  contains(t1, "\"gen\":"),
                  "v16: texto SET_PRESET:Spatial -> ok:true + preset + gen");
            PendingPerceptual taken2;
            check(takePendingPerceptual(st6, taken2) && taken2.spatialWidth == 2.2f &&
                  taken2.widenerMult == 1.4f,
                  "v16: texto SET_PRESET:Spatial stashea spatial 2.2 / widener 1.4");
            const std::string t2 = handleTextCommand("SET_PRESET:NoExiste", st6);
            check(contains(t2, "\"ok\":false") && contains(t2, "preset desconocido"),
                  "v16: texto SET_PRESET:NoExiste -> ok:false preset desconocido");
        }
    }

    std::printf("RESULTADO: %d pasan, %d fallan\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
