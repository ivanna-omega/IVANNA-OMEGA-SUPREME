// test_omega_daemon_bridge.cpp — Tests de host del puente SHM al daemon
// (propuesta 0018, coordinador del Plan Omnipotencia).
//
// omega_daemon_bridge.cpp (156 líneas) estaba entre los 19 módulos sin
// tests: la app recibe el fd del SHM del daemon por SCM_RIGHTS en el
// socket abstracto @omega_daemon_socket, valida el layout y lo mapea.
// El test levanta un mock daemon (hilo) que escucha en ese socket y
// envía 1 byte + el fd de un SHM válido (creado con OmegaShmManager).
//
// Lo que se verifica (comportamiento, no implementación):
// - Sin daemon escuchando, omega_daemon_get_shared_state() devuelve
//   nullptr (sin colgarse: el connect falla de inmediato).
// - Con el mock daemon, devuelve un OmegaSharedState* no nulo cuyo
//   header (base = state - SHM_STATE_OFFSET) pasa validateShmHeader.
// - La segunda llamada devuelve EL MISMO puntero (caché: no se repite
//   el handshake). El test de caché es autocontenido (su propio mock
//   daemon): CTest ejecuta cada TEST en un proceso aparte, así que no
//   puede depender del mapeo del test anterior.
//
// Nota de orden: el test NoDaemon VA PRIMERO (antes de cualquier mapeo
// exitoso en el proceso); gtest corre en orden de definición. El mock
// daemon tiene timeout en accept (2 s): si el bridge ya tiene el estado
// cacheado (corrida directa de los 3 tests en un proceso), el mock no
// recibe conexión y sale solo en vez de colgar el join.
#include <atomic>
#include <chrono>
#include <cstring>
#include <gtest/gtest.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

#include "daemon/core/shm_manager.h"
#include "include/omega_shared.h"

extern "C" OmegaSharedState* omega_daemon_get_shared_state();

namespace {

constexpr char kSocketName[] = "omega_daemon_socket";

// Envía 1 byte + un fd por SCM_RIGHTS (el protocolo que espera
// ivanna::ipc::receiveExactlyOneFd).
bool SendOneFd(int sock, int fdToSend) {
    char payload = 'B';
    iovec io{};
    io.iov_base = &payload;
    io.iov_len = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))]{};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);
    cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fdToSend, sizeof(int));
    return ::sendmsg(sock, &msg, 0) == 1;
}

// Mock daemon: escucha en el socket abstracto, acepta una conexión y
// envía el fd del SHM. Señala listening=true cuando está listo. Si
// nadie conecta en 2 s, sale solo (el bridge pudo usar el caché).
void MockDaemon(int shmFd, std::atomic<bool>* listening) {
    const int srv = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (srv < 0) return;
    timeval timeout{};
    timeout.tv_sec = 2;
    timeout.tv_usec = 0;
    (void)::setsockopt(srv, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, kSocketName, sizeof(kSocketName) - 1);
    const socklen_t len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + 1 + sizeof(kSocketName) - 1);
    if (::bind(srv, reinterpret_cast<const sockaddr*>(&addr), len) != 0) {
        ::close(srv);
        return;
    }
    if (::listen(srv, 1) != 0) {
        ::close(srv);
        return;
    }
    listening->store(true, std::memory_order_release);
    const int conn = ::accept(srv, nullptr, nullptr);
    if (conn >= 0) {
        (void)SendOneFd(conn, shmFd);
        ::close(conn);
    }
    ::close(srv);
}

void WaitListening(const std::atomic<bool>* listening) {
    // 5 s de margen: bajo ctest -j4 en runners de 2 núcleos el hilo del
    // mock daemon puede tardar en ser planificado; con 1 s el arranque
    // fallaba de forma intermitente y el ASSERT posterior abortaba el
    // binario (ver destructor de MockDaemonRun).
    for (int i = 0; i < 1000 && !listening->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Crea un SHM válido y levanta el mock daemon; devuelve el manager
// (debe vivir mientras el bridge use el mmap) y el hilo del daemon.
struct MockDaemonRun {
    ivanna::OmegaShmManager mgr;
    std::atomic<bool> listening{false};
    std::thread daemon;
    // FIX (flaky "Subprocess aborted" bajo ctest -j4, v2.5.457): si un
    // ASSERT falla antes del join() explícito del test, el destructor de
    // std::thread con el hilo aún joinable llama a std::terminate() y
    // CTest reporta el binario entero como abortado. El join aquí
    // garantiza que el hilo nunca quede joinable al destruirse el run,
    // falle lo que falle. El mock daemon sale solo tras 2 s sin conexión,
    // así que este join no puede colgar el test.
    ~MockDaemonRun() {
        if (daemon.joinable()) daemon.join();
    }
};

bool StartMockDaemon(MockDaemonRun* run) {
    // SHM único por proceso: un archivo de una corrida anterior (o de
    // otro binario en paralelo) no debe confundir a este test.
    const std::string path =
        "/tmp/omega_daemon_bridge_test_shm_" + std::to_string(::getpid());
    if (!run->mgr.init(path)) return false;
    if (!run->mgr.isReady() || run->mgr.fd() < 0) return false;
    run->daemon = std::thread(MockDaemon, run->mgr.fd(), &run->listening);
    WaitListening(&run->listening);
    return run->listening.load();
}

}  // namespace

TEST(OmegaDaemonBridgeTest, NoDaemonReturnsNull) {
    // Debe correr ANTES de cualquier mapeo exitoso (orden de definición).
    EXPECT_EQ(omega_daemon_get_shared_state(), nullptr);
}

TEST(OmegaDaemonBridgeTest, MockDaemonHandshakeMapsValidShm) {
    MockDaemonRun run;
    ASSERT_TRUE(StartMockDaemon(&run));
    OmegaSharedState* state = omega_daemon_get_shared_state();
    run.daemon.join();
    ASSERT_NE(state, nullptr);

    // El header en la base del mmap pasa la validación canónica.
    const void* base = static_cast<const uint8_t*>(
        static_cast<const void*>(state)) - ivanna::SHM_STATE_OFFSET;
    EXPECT_TRUE(ivanna::validateShmHeader(base, ivanna::SHM_SIZE));
}

TEST(OmegaDaemonBridgeTest, SecondCallReturnsCachedPointer) {
    // Autocontenido: su propio mock daemon (CTest ejecuta cada TEST en
    // un proceso aparte). Si el bridge ya mapeó en este proceso, el
    // mock no recibe conexión y sale por timeout.
    MockDaemonRun run;
    ASSERT_TRUE(StartMockDaemon(&run));
    OmegaSharedState* first = omega_daemon_get_shared_state();
    OmegaSharedState* second = omega_daemon_get_shared_state();
    run.daemon.join();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(second, first);
}
