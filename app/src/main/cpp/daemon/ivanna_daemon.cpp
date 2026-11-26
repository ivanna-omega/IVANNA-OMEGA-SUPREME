// ivanna_daemon_v2.cpp — El CEREBRO de la Fusión Magistral (reescritura total).
//
// El daemon es UN proceso propio (root), NO vive dentro del HAL:
//   - Vive en /vendor/bin/ivanna_daemon_v2 (lo instala el módulo).
//   - Lo arranca service.sh en cada arranque (después de los servicios).
//   - Se fusiona con el ASRC (que vive DENTRO del HAL) SOLO a través del
//     estado compartido IvannaFusionState (SHM). Un solo estado, un solo bucle.
//   - NO puede vivir dentro del HAL: el HAL (usuario audioserver, contexto
//     hal_audio_default) NO puede correr dumpsys, NO puede leer la UI y NO
//     puede ser root. Y NO debe: si el cerebro muere, el HAL debe sobrevivir
//     (el músculo pasa a passthrough, el audio sigue).
//
// El cerebro hace (un solo bucle, ~20 Hz):
//   1. Leer las mediciones del músculo desde el estado compartido.
//   2. Correr el servo Kalman de 3 estados (deriva + tasa + aceleración)
//      y calcular el ratio (salida/entrada).
//   3. Certificar el reloj en vivo (mmap vs CLOCK_MONOTONIC) y decidir la
//      fuente de reloj.
//   4. Descubrir la sesión REAL de música (parser de 5 campos, cada 3 s).
//   5. Decidir el veredicto y el control de la cadena de súper efectos
//      (lee /data/adb/ivanna_omega/fusion_fx.conf; por defecto TODO apagado).
//   6. Escribir ratio + veredicto + efectos en el estado compartido.
//   7. Publicar el snapshot NEUTRO (para el efecto, si lo hay) + heartbeat.
//
// Fail-closed: si la SHM no está → no publica; si el músculo no mide →
// veredicto BYPASS; si descubrir la sesión falla (kError) → se conserva la
// sesión anterior; si no hay música (kNone) → sessionId 0. Kill switch:
// /data/adb/ivanna_omega/pal_asr.off (SIN la c, daemon_paths.h) → fusión
// en bypass (ratio 1.0, efectos off). La ruta vieja
// /data/vendor/ivanna_audio/pal_asrc.off es del write-stage v1 (HAL), NO
// del daemon v2: mirarla dejaba el cerebro vivo con pal_asr.off creado.
// Si existe /data/adb/ivanna_omega/ivanna_daemon.off, el daemon no
// arranca (service.sh lo respeta).
//
// Compilación: la CI lo compila para arm64-v8a. El módulo lo instala en
// system/bin/ivanna_daemon_v2.

#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "control/daemon_command_responder.h"
#include "control/effect_telemetry_mirror.h"  // P2 (2026-11-26): reflejar RMS/pico/frames medidos por el efecto en el snapshot de control
#include "daemon_paths.h"
#include "daemon/session_discovery.hpp"
#include "daemon/hardware_clock_from_muscle.hpp"  // EVOL-LIVEPATH: muestra de reloj HAL desde el músculo ASRC (hardware_rate_millihz)
#include "effect_local_files.h"
#include "include/audio_hardware_clock_bridge.hpp"  // EVOL-LIVEPATH: HardwareClockPublisher (único publicador del snapshot de reloj)
#include "include/omega_control_bus.h"
#include "include/omega_effect_beacon.h"
#include "include/omega_effect_evidence.h"
#include "include/psi_core.h"
#include "include/ivanna_fusion_state.h"

namespace {

std::atomic<bool> g_running{true};
void onSignal(int) noexcept { g_running.store(false); }

uint64_t monotonicMs() noexcept {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000u
         + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
}
int64_t monotonicNs() noexcept {
    struct timespec ts {};
    if (::clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

void logLine(const char* msg) noexcept {
    std::FILE* f = std::fopen("/data/adb/ivanna_omega/daemon_v2.log", "a");
    if (!f) return;
    std::fprintf(f, "[%llu] %s\n",
                 static_cast<unsigned long long>(monotonicMs()), msg);
    std::fclose(f);
}

// Heartbeat de la fusión (para que la APK lo LEA y se ACOPLE).
// Se escribe en DOS sitios:
//  - /data/adb/ivanna_omega/daemon_v2_heartbeat (diagnóstico, solo root).
//  - /data/local/tmp/ivanna_fusion_heartbeat (LA APK LO LEE: /data/local/tmp
//    sí lo puede leer una app normal; /data/adb NO). Sin este segundo
//    archivo la APK sería ciega a la fusión cuando la Ruta B no está activa
//    (el efecto solo publica al bus local cuando procesa).
// Formato texto clave=valor (como el beacon del efecto): la app lo lee por
// JNI (DSPBridge.nativeFusionState) cada 10 s en AudioBackendSelector.
// fusion_active=1 cuando el veredicto es LOADED (el HAL procesa TODO el
// audio); la app entonces detiene la Ruta A (no procesa dos veces).
void writeHeartbeat(int32_t sessionId, uint64_t generation,
                    float driftPpm, uint32_t verdict, float eta, float psiGamma,
                    float ratio, int32_t clockSource, uint32_t psiActive) noexcept {
    const uint32_t fusionActive = (verdict == IVANNA_VERDICT_LOADED) ? 1u : 0u;
    const char* paths[2] = {"/data/adb/ivanna_omega/daemon_v2_heartbeat",
                            "/data/local/tmp/ivanna_fusion_heartbeat"};
    for (int i = 0; i < 2; ++i) {
        std::FILE* f = std::fopen(paths[i], "w");
        if (!f) continue;
        std::fprintf(f, "alive=1\nts_ms=%llu\nsession_id=%d\ngeneration=%llu\ndrift_ppm=%.2f\nverdict=%u\neta=%.3f\npsi_gamma=%.3f\nfusion_active=%u\npsi_active=%u\nratio=%.6f\nclock=%d\n",
                     static_cast<unsigned long long>(monotonicMs()),
                     static_cast<int>(sessionId),
                     static_cast<unsigned long long>(generation),
                     static_cast<double>(driftPpm), verdict,
                     static_cast<double>(eta), static_cast<double>(psiGamma),
                     fusionActive, psiActive,
                     static_cast<double>(ratio), static_cast<int>(clockSource));
        std::fclose(f);
    }
}

bool fileExists(const char* path) noexcept {
    return ::access(path, F_OK) == 0;
}

// Socket abstracto @omega_daemon_socket (compatibilidad con el módulo y
// la app). service.sh, supervisor.sh y la app (MagiskBridge.kt) deciden
// "SOCKET=CONECTADO" grepeando /proc/net/unix por este nombre, y el
// supervisor relanza el daemon si el proceso corre SIN socket.
//
// v2.5.440 — EL SOCKET YA NO ES SOLO SEÑAL DE VIDA: sirve comandos
// (HELLO + GET_STATUS) en un hilo propio (ver control/
// daemon_command_responder.h). Antes (v2.5.401–439) el cerebro bindeaba
// el socket y el bucle principal drenaba las conexiones con accept()+
// close() inmediato, así que GET_STATUS jamás se respondía y la app no
// podía descubrir la sesión: el efecto omega nunca se acoplaba (Ruta B
// muerta). El hilo de comandos lee el estado vivo (sesión, generación,
// veredicto) de los atómicos que el bucle principal actualiza cada
// iteración; si el hilo muere, la fusión sigue (fail-closed).
int bindDaemonSocket() noexcept {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr {};
    addr.sun_family = AF_UNIX;
    // Abstracto: sun_path[0] = '\0' + nombre (sin archivo en el fs).
    const char* name = "omega_daemon_socket";
    const size_t nlen = std::strlen(name);
    addr.sun_path[0] = '\0';
    std::memcpy(addr.sun_path + 1, name, nlen);
    const socklen_t len = static_cast<socklen_t>(
        offsetof(struct sockaddr_un, sun_path) + 1 + nlen);
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

// ── Servo Kalman de 3 estados ─────────────────────────────────────────────
// Estado: x = [deriva_ppm, tasa_deriva_ppm/s, aceleracion_ppm/s^2].
// Medición: z = deriva instantánea (ppm) desde el reloj del músculo.
// El ratio = 1 + deriva/1e6 (HAL rápido → ratio > 1, ver auditoría riesgo D).
struct Kalman3 {
    double x[3] = {0.0, 0.0, 0.0};  // estado
    double P[3][3] = {{100.0, 0, 0}, {0, 10.0, 0}, {0, 0, 1.0}}; // covarianza
    // Ruido de proceso (Q) y de medición (R). R se adapta: si la medición
    // es ruidosa, el filtro confía más en el modelo (como en la v5).
    double q0 = 0.04, q1 = 0.004, q2 = 0.0004;
    double R = 25.0;
    bool initialized = false;
    double lastInnovation = 0.0;  // |y| de la última actualización (para la pérdida L de η)

    void predict(double dt) noexcept {
        if (dt <= 0.0 || dt > 1.0) dt = 0.05;
        // F = [[1, dt, 0.5*dt^2],[0,1,dt],[0,0,1]]
        double nx0 = x[0] + x[1] * dt + 0.5 * x[2] * dt * dt;
        double nx1 = x[1] + x[2] * dt;
        x[0] = nx0; x[1] = nx1;  // x[2] no cambia
        // P = F*P*F' + Q (Q diagonal). Se hace explícito para 3x3.
        double F[3][3] = {{1, dt, 0.5*dt*dt}, {0, 1, dt}, {0, 0, 1}};
        double FP[3][3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) FP[i][j] += F[i][k] * P[k][j];
        double FPF[3][3] = {};
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) FPF[i][j] += FP[i][k] * F[j][k];
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) P[i][j] = FPF[i][j];
        P[0][0] += q0; P[1][1] += q1; P[2][2] += q2;
    }

    void update(double z) noexcept {
        // H = [1,0,0]. S = P00 + R. K = P[:,0]/S.
        double S = P[0][0] + R;
        if (S <= 0.0) return;
        double K[3] = {P[0][0] / S, P[1][0] / S, P[2][0] / S};
        double y = z - x[0];  // innovación
        lastInnovation = std::fabs(y);
        // Puerta 3σ: si la innovación es absurda, no se traga la medición.
        double sigma = std::sqrt(S);
        if (std::fabs(y) > 3.0 * sigma + 500.0) return;  // outlier
        x[0] += K[0] * y; x[1] += K[1] * y; x[2] += K[2] * y;
        // P = (I - K*H)*P
        for (int j = 0; j < 3; ++j) {
            P[0][j] -= K[0] * P[0][j];
            P[1][j] -= K[1] * P[0][j];
            P[2][j] -= K[2] * P[0][j];
        }
        // R adaptativa: si la innovación es grande, la medición es ruidosa.
        double ay = std::fabs(y);
        R = (ay > 50.0) ? 400.0 : (ay > 10.0 ? 100.0 : 25.0);
        initialized = true;
    }

    double driftPpm() const noexcept { return x[0]; }
    double ratio() const noexcept { return 1.0 + x[0] / 1e6; }
};

// ── Control de efectos (lo lee el cerebro de un archivo de config) ────────
// Formato (una línea por efecto, "nombre=0.0..1.0"): orbe, holos, phi,
// volterra, supreme, npe, reality, eq, limiter (0/1). Por defecto TODO 0
// (neutro). La UI / el usuario escribe este archivo para encender efectos.
// El cerebro lo relee en cada ciclo (barato) para que el cambio sea en vivo.
struct FxControl {
    uint32_t flags = 0;
    float orbe = 0, holos = 0, phi = 0, volterra = 0, supreme = 0;
    float npe = 0, reality = 0, eq = 0;
    uint32_t limiter = 0;
};
FxControl readFxConfig() noexcept {
    FxControl fx;
    std::FILE* f = std::fopen("/data/adb/ivanna_omega/fusion_fx.conf", "r");
    if (!f) return fx;  // sin config = todo apagado (neutro, fail-closed)
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
        char name[32]; float v = 0;
        if (std::sscanf(line, "%31[^=]=%f", name, &v) != 2) continue;
        if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
        if (!std::strcmp(name, "orbe"))     { fx.orbe = v;     if (v > 0) fx.flags |= IVANNA_FX_ORBE; }
        else if (!std::strcmp(name, "holos"))    { fx.holos = v;    if (v > 0) fx.flags |= IVANNA_FX_HOLOS; }
        else if (!std::strcmp(name, "phi"))      { fx.phi = v;      if (v > 0) fx.flags |= IVANNA_FX_PHI; }
        else if (!std::strcmp(name, "volterra")) { fx.volterra = v; if (v > 0) fx.flags |= IVANNA_FX_VOLTERRA; }
        else if (!std::strcmp(name, "supreme"))  { fx.supreme = v;  if (v > 0) fx.flags |= IVANNA_FX_SUPREME; }
        else if (!std::strcmp(name, "npe"))      { fx.npe = v;      if (v > 0) fx.flags |= IVANNA_FX_NPE; }
        else if (!std::strcmp(name, "reality"))  { fx.reality = v;  if (v > 0) fx.flags |= IVANNA_FX_REALITY; }
        else if (!std::strcmp(name, "eq"))       { fx.eq = v;       if (v > 0) fx.flags |= IVANNA_FX_EQ; }
        else if (!std::strcmp(name, "limiter"))  { fx.limiter = (v > 0) ? 1u : 0u; if (v > 0) fx.flags |= IVANNA_FX_LIMITER; }
    }
    std::fclose(f);
    return fx;
}

// Snapshot neutro (defensa en profundidad: aunque makeDefault cambiara,
// esto seguiría siendo neutro).
// Aplica el estado de la fusión + sesión a un snapshot EXISTENTE (lo
// modifica in situ). Solo toca los campos del cerebro; CONSERVA los ajustes
// de efectos de la APK (ORBE, HOLOS, Φ, Volterra, 5 Ejes, NPE, Reality, EQ,
// limitador) que ya trae el snapshot. Así la Ruta B aplica TODOS los efectos
// de la APK en la intercepción, y el cerebro no los borra con un default
// neutro en cada publicación (20 Hz). El default neutro solo se usa en el
// primer publish (makeDefault, fail-closed) si aún no hay snapshot.
void applyFusionToSnapshot(ivanna::OmegaDspSnapshot& s, int32_t sessionId, uint64_t sessionGen,
                           uint64_t generation, uint32_t verdict, double driftPpm, double ratio,
                           int32_t clockSource, double eta, double psiGamma,
                           uint32_t psiActive, uint64_t fusionGen) noexcept {
    s.discovered_session_id = sessionId;
    s.discovered_session_gen = sessionGen;
    s.generation = generation;
    s.timestamp_ms = monotonicMs();
    // ── Fusión (ABI v12): la APK lo lee y se acopla. ─────────────────────
    s.fusion_active = (verdict == IVANNA_VERDICT_LOADED) ? 1u : 0u;
    s.fusion_eta = static_cast<float>(eta);
    s.fusion_psi_gamma = static_cast<float>(psiGamma);
    s.fusion_psi_active = psiActive;
    s.fusion_asrc_verdict = static_cast<int32_t>(verdict);
    s.fusion_asrc_ratio = static_cast<float>(ratio);
    s.fusion_asrc_drift_ppm = static_cast<float>(driftPpm);
    s.fusion_asrc_clock = clockSource;
    s.fusion_generation = fusionGen;
}

} // namespace

int main() {
    ::signal(SIGINT, onSignal);
    ::signal(SIGTERM, onSignal);
    ::signal(SIGPIPE, SIG_IGN);

    ::mkdir("/data/adb/ivanna_omega", 0771);
    ::mkdir("/data/vendor/ivanna_audio", 0771);

    // v2.5.443 — Pre-crear los archivos locales de Ruta B (beacon + bus de
    // control + evidencia). El efecto (audioserver, UID no-root) NO PUEDE
    // crearlos solo; sin ellos el beacon nunca existe, el detector de
    // huérfanos queda ciego y la app no ve si Ruta B procesa.
    // precreateLocalFile existe y está testeado (test_effect_local_files)
    // pero NADIE lo llamaba: el daemon es root y es quien debe hacerlo en
    // cada arranque. Solo existencia + 0666 + tamaño; el efecto inicializa
    // el magic al mapearlos. No es fatal si falla (se loguea y se sigue).
    // 0043: los tres viven en /data/adb/ivanna_omega/ (NO /data/local/tmp:
    // ahí el kernel los ve como `unlabeled` pese a chcon ivanna_data_file,
    // beacon 72 B en ceros, G85 v2.5.462). En el directorio nuevo heredan
    // ivanna_data_file del padre (lo etiqueta service.sh); el chcon de
    // precreateAndRelabelLocalFile queda como cinturón + tirantes.
    {
        // 0038: además del precreate, re-etiquetar a ivanna_data_file —
        // sepolicy.rule ya permite a audioserver escribir ese tipo.
        const auto beacon = ivanna::precreateAndRelabelLocalFile(
            ivanna::OMEGA_EFFECT_BEACON_PATH, ivanna::OMEGA_EFFECT_BEACON_SIZE,
            "u:object_r:ivanna_data_file:s0");
        logLine((std::string("local-files: beacon ") +
            (beacon.ok ? "OK (" + beacon.detail + ")" : "FALLO (" + beacon.detail + ")")).c_str());
        const auto ctrl = ivanna::precreateAndRelabelLocalFile(
            ivanna::OMEGA_EFFECT_LOCAL_BUS_PATH, ivanna::OmegaControlBus::REGION_SIZE,
            "u:object_r:ivanna_data_file:s0");
        logLine((std::string("local-files: ctrl ") +
            (ctrl.ok ? "OK (" + ctrl.detail + ")" : "FALLO (" + ctrl.detail + ")")).c_str());
        // 0043: la evidencia (HRTF/RIR/SAF medidos) tenía el mismo defecto
        // de /data/local/tmp y el efecto la crea con O_CREAT; pre-crearla
        // aquí la deja 0666 + ivanna_data_file antes de que el efecto abra.
        const auto evidence = ivanna::precreateAndRelabelLocalFile(
            ivanna::OMEGA_EFFECT_EVIDENCE_PATH, ivanna::OMEGA_EFFECT_EVIDENCE_SIZE,
            "u:object_r:ivanna_data_file:s0");
        logLine((std::string("local-files: evidence ") +
            (evidence.ok ? "OK (" + evidence.detail + ")" : "FALLO (" + evidence.detail + ")")).c_str());
    }

    // Kill switch del daemon: si existe, no arrancar.
    if (fileExists(ivanna::daemon::kDaemonOffPath)) return 0;

    // Abrir/crear el estado compartido de la fusión (SHM por archivo + mmap).
    int fd = ::open(IVANNA_FUSION_SHM_PATH, O_RDWR | O_CREAT, 0666);
    if (fd < 0) { logLine("FATAL: no se pudo abrir el estado de fusión"); return 1; }
    ::ftruncate(fd, sizeof(IvannaFusionState));
    auto* fusion = static_cast<IvannaFusionState*>(
        ::mmap(nullptr, sizeof(IvannaFusionState), PROT_READ | PROT_WRITE,
               MAP_SHARED, fd, 0));
    if (fusion == MAP_FAILED) { logLine("FATAL: mmap del estado de fusión falló"); return 1; }
    // Inicializar magic/version si es la primera vez (sequence par = estable).
    {
        IvannaFusionState cur{};
        if (!ivanna_fusion_read(fusion, &cur) || cur.magic != IVANNA_FUSION_MAGIC) {
            IvannaFusionState init{};
            init.magic = IVANNA_FUSION_MAGIC;
            init.version = IVANNA_FUSION_VERSION;
            init.verdict = IVANNA_VERDICT_BYPASS;
            init.ratio = 1.0f;
            ivanna_fusion_write(fusion, &init);
        }
    }

    ivanna::OmegaControlBus bus;
    bool shmOk = false;
    // Snapshot que publica el cerebro: se conserva entre iteraciones para
    // NO borrar los ajustes de efectos de la APK (ORBE, HOLOS, EQ, ...).
    // Cada vuelta se adopta el snapshot más reciente de la app (si publicó
    // algo nuevo) y solo se actualizan los campos del cerebro (fusión +
    // sesión). Así TODOS los efectos de la APK llegan a la Ruta B.
    // 0023 (bofo Ruta B): la base es makeNeutral(), no makeDefault().
    // makeDefault() es el default MAGISTRAL de Ruta A (HRTF on, widener
    // 2.14x, intensity 0.92): como primer publish antes de que la app
    // publique, coloreaba el audio aunque nadie pidiera nada.
    ivanna::OmegaDspSnapshot lastSnap = ivanna::OmegaDspSnapshot::makeNeutral();
    uint64_t snapGeneration = 0;
    // P2 (2026-11-26, RMS=0 en vivo): el snapshot de control mostraba
    // RMS/pico/frames en 0 con música sonando porque el daemon nunca leía
    // el bus local de telemetría del efecto. El espejo lee ese bus (solo
    // lectura) y refleja los niveles medidos en lastSnap antes de publicar;
    // el daemon sigue siendo el único escritor del bus de control. Si el
    // bus local no existe, el espejo se salta (fail-closed, ceros como hoy).
    ivanna::OmegaControlBus localTelemetryBus;
    ivanna::daemon::EffectTelemetryMirror telemetryMirror;
    bool localTelemetryOpen = false;
    uint64_t lastLocalOpenAttemptMs = 0;
    // EVOL-LIVEPATH (hardware_rate_millihz en 0): publicador del snapshot
    // de reloj HAL (/data/vendor/ivanna_audio/clock_snapshot.v2). El daemon
    // es el único publicador: el músculo mide el reloj, el daemon
    // re-publica la muestra para que omega_effect la lea. Apertura
    // perezosa con reintento espaciado 5 s; solo se publica cuando la
    // muestra AVANZA (con el músculo parado no se refresca nada).
    ivanna::audio::HardwareClockPublisher hwClockPublisher;
    bool hwClockPublisherOpen = false;
    uint64_t lastHwClockOpenAttemptMs = 0;
    uint64_t lastHwClockTimestampNs = 0;
    uint64_t lastHwClockFrames = 0;
    Kalman3 servo;
    // El núcleo Ψ vivo: el cerebro corre el campo (Ec. 4 η + Ec. 2 ILD +
    // Ec. 3 personalización) y publica sus ganancias por banda en el estado.
    ivanna::psi::PsiCore psi;
    uint64_t psiGeneration = 0;
    // El FIR del campo (lo diseña el cerebro, lo aplica el músculo).
    // Se recalcula solo cuando η o la fuente cambian de forma significativa.
    float psiFirL[ivanna::psi::PsiCore::kPsiFirTaps] = {};
    float psiFirR[ivanna::psi::PsiCore::kPsiFirTaps] = {};
    uint64_t psiFirGen = 0;
    double lastFirEta = -1.0;
    float lastFirR = -1.0f, lastFirTheta = -999.0f;
    bool firDesigned = false;
    int32_t sessionId = 0;
    uint64_t sessionGen = 0;
    // CORONA Frente A (defecto 4): puerta de cambio de sesión con dwell
    // (FIX 2, v2.5.465). Sin ella, cada poll ruidoso de descubrimiento
    // (~3 s) reseteaba el snapshot a neutro y subía sessionGen (re-acople
    // del efecto). Instancia persistente: el gate acumula polls; solo un
    // cambio confirmado (3 polls seguidos) publica sesión y resetea.
    ivanna::daemon::SessionChangeGate sessionGate;
    // EVOL-LIVEPATH (sesión rancia 4057): vigilante de viveza + sesiones
    // degradadas. El gate solo filtra el ruido de dumpsys; el tracker
    // desaloja la sesión confirmada cuando el beacon prueba que no suena
    // y hay otra ganadora, y la degradación impide que el ranking la
    // re-elija por el hint de música. Ver session_discovery.hpp.
    ivanna::daemon::SessionLivenessTracker sessionLiveness;
    ivanna::daemon::DemotedSessions demotedSessions;
    // FASE A: package de la sesión descubierta + app en foreground (del
    // mismo ciclo de descubrimiento; se publican en GET_STATUS vía cmdState).
    std::string sessionPackage;
    std::string foregroundPackage;
    uint64_t generation = 0;
    uint64_t servoUpdates = 0;
    int32_t lastPublishedSession = -1;
    // Estado previo del músculo (para calcular deriva instantánea).
    bool havePrev = false;
    int64_t prevMmapNs = 0; uint32_t prevMmapFrames = 0;
    int64_t prevWriteNs = 0; uint32_t prevFramesWritten = 0;
    uint64_t prevTimeMs = 0;
    // Certificación del reloj: diferencia mmap_ns - write_ns (debería ser
    // estable y pequeña si el timestamp mmap es CLOCK_MONOTONIC).
    double clockDiffMean = 0.0; double clockDiffM2 = 0.0; uint64_t clockDiffN = 0;
    bool clockCertified = false;
    uint32_t clockSource = IVANNA_CLOCK_NONE;
    uint32_t verdict = IVANNA_VERDICT_BYPASS;
    int sessionCountdown = 0;  // descubrir sesión cada ~3 s (60 ciclos de 50 ms)

    // Socket de comandos (abstracto @omega_daemon_socket) + hilo que lo
    // sirve (HELLO + GET_STATUS). v2.5.440: antes el socket se bindeaba y
    // nadie lo atendía (el bucle drenaba accept()+close()), así que la app
    // jamás recibía la sesión descubierta y Ruta B no se acoplaba. El hilo
    // lee el estado vivo de cmdState (el bucle lo actualiza cada
    // iteración); si el bind falla, el cerebro sigue sin comandos (la
    // fusión no depende del socket) y la app verá Ruta B sin acople.
    // 0044 (Fallo 5): reintentar el bind si el socket aún está en
    // TIME_WAIT / lo tiene un daemon viejo (EADDRINUSE): 10 intentos x
    // 200 ms. Cualquier otro errno es definitivo (no reintentar).
    int sockFd = -1;
    for (int attempt = 0; attempt < 10 && g_running.load(); ++attempt) {
        sockFd = bindDaemonSocket();
        if (sockFd >= 0) break;
        if (errno != EADDRINUSE) break;
        ::usleep(200000);
    }
    ivanna::daemon::CommandPlaneState cmdState;
    cmdState.startMs.store(monotonicMs(), std::memory_order_relaxed);
    std::thread cmdThread;
    if (sockFd >= 0) {
        logLine("socket @omega_daemon_socket bindeado");
        cmdThread = std::thread(ivanna::daemon::serveCommandSocket, sockFd,
                                std::ref(cmdState), std::ref(g_running), true);
        logLine("hilo de comandos @omega_daemon_socket iniciado (GET_STATUS/HELLO)");
    } else {
        logLine("WARN: no se pudo bindear @omega_daemon_socket (la fusión sigue; Ruta B sin acople)");
    }

    logLine("ivanna_daemon_v2 (CEREBRO) iniciado — fusión homogénea");

    // Throttle heartbeat (2.5.461): el bucle corre a ~20 Hz pero el heartbeat
    // es solo señal de vida (el lector tolera 5 s de antigüedad; la ruta de
    // control real es el bus SHM, que sigue publicando a 20 Hz). Escribir a
    // disco 20x/s (2 ficheros, fopen/fwrite/fclose por ciclo) era I/O
    // innecesaria: ahora <= 1x/s por RELOJ (no por conteo de ciclos: si el
    // bucle se ralentiza, el intervalo sigue siendo 1 s), con refresco
    // inmediato si cambia el estado discreto (sesión, veredicto, fuente de
    // reloj o Ψ activo). El primer ciclo escribe siempre (hbLastMs = 0 y
    // centinelas distintos de cualquier valor real).
    uint64_t hbLastMs = 0;
    int32_t hbLastSession = -1;
    uint32_t hbLastVerdict = 0xFFFFFFFFu;
    int32_t hbLastClock = -1;
    uint8_t hbLastPsiAct = 0xFFu;
    // 0044 (Fallo 4): último estado publicado al bus SHM. El daemon solo
    // publica si el snapshot cambió (dirty), cambió la meta (sesión,
    // veredicto, Ψ) o pasó 1 s (keepalive); no 20 Hz sin cambios.
    uint64_t lastBusPubMs = 0;
    int32_t lastBusSession = -1;
    uint64_t lastBusSessionGen = 0;
    uint32_t lastBusVerdict = 0xFFFFFFFFu;
    uint32_t lastBusPsiActive = 0xFFFFFFFFu;
    // 0044 (Fallo 6, reconciliado con 0043): último chequeo de beacon.
    uint64_t lastBeaconCheckMs = 0;

    while (g_running.load()) {
        const uint64_t nowMs = monotonicMs();
        const double dt = havePrev ? (nowMs - prevTimeMs) / 1000.0 : 0.05;
        const bool killSwitch = fileExists(ivanna::daemon::kDaemonKillSwitchPath);

        // 1. Leer las mediciones del músculo (seqlock; reintentar si cambia).
        IvannaFusionState st{};
        bool readOk = false;
        for (int i = 0; i < 5 && !readOk; ++i) readOk = ivanna_fusion_read(fusion, &st);
        const bool muscleAlive = readOk && st.magic == IVANNA_FUSION_MAGIC
            && st.version == IVANNA_FUSION_VERSION && st.muscle_health == 1;
        const bool muscleFresh = muscleAlive
            && (nowMs - st.muscle_timestamp_ms) < 500;  // el músculo escribe a menudo

        // 2. Deriva instantánea desde el reloj del músculo (si hay datos).
        bool haveMeasurement = false;
        double measPpm = 0.0;
        if (muscleFresh && havePrev && st.stream_rate_hz > 0) {
            const double rate = static_cast<double>(st.stream_rate_hz);
            // Reloj mmap (preferido si está certificado).
            double mmapPpm = 0.0; bool mmapOk = false;
            if (st.clock_mmap_frames != prevMmapFrames && st.clock_mmap_ns != prevMmapNs) {
                double dF = static_cast<double>(st.clock_mmap_frames - prevMmapFrames);
                double dT = static_cast<double>(st.clock_mmap_ns - prevMmapNs) / 1e9;
                if (dT > 0.0 && dF > 0.0) {
                    mmapPpm = (dF / dT / rate - 1.0) * 1e6;
                    mmapOk = std::fabs(mmapPpm) < 5000.0;  // puerta: outlier no
                }
            }
            // Reloj write-timing.
            double writePpm = 0.0; bool writeOk = false;
            if (st.frames_written != prevFramesWritten && st.clock_write_ns != prevWriteNs) {
                double dF = static_cast<double>(st.frames_written - prevFramesWritten);
                double dT = static_cast<double>(st.clock_write_ns - prevWriteNs) / 1e9;
                if (dT > 0.0 && dF > 0.0) {
                    writePpm = (dF / dT / rate - 1.0) * 1e6;
                    writeOk = std::fabs(writePpm) < 5000.0;
                }
            }
            // Certificación del reloj: mmap_ns vs write_ns (misma época).
            if (mmapOk && writeOk) {
                double diffMs = static_cast<double>(st.clock_mmap_ns - st.clock_write_ns) / 1e6;
                ++clockDiffN;
                double delta = diffMs - clockDiffMean;
                clockDiffMean += delta / static_cast<double>(clockDiffN);
                clockDiffM2 += delta * (diffMs - clockDiffMean);
                double var = (clockDiffN > 1) ? clockDiffM2 / (clockDiffN - 1) : 0.0;
                // Certificado si la diferencia es estable (<5 ms de media y poca varianza).
                clockCertified = (std::fabs(clockDiffMean) < 5.0 && var < 25.0 && clockDiffN >= 20);
            }
            // Fusión de sensores: si ambos, promedio; si uno, ese; si ninguno, nada.
            if (mmapOk && writeOk && clockCertified) {
                measPpm = 0.5 * (mmapPpm + writePpm);
                haveMeasurement = true;
                clockSource = IVANNA_CLOCK_MMAP;
            } else if (mmapOk && clockCertified) {
                measPpm = mmapPpm; haveMeasurement = true; clockSource = IVANNA_CLOCK_MMAP;
            } else if (writeOk) {
                measPpm = writePpm; haveMeasurement = true; clockSource = IVANNA_CLOCK_WRITE_TIMING;
            } else if (mmapOk) {
                measPpm = mmapPpm; haveMeasurement = true; clockSource = IVANNA_CLOCK_MMAP;
            } else {
                clockSource = IVANNA_CLOCK_NONE;
            }
        }
        // Guardar el estado previo para el próximo ciclo.
        if (muscleFresh) {
            prevMmapNs = st.clock_mmap_ns; prevMmapFrames = st.clock_mmap_frames;
            prevWriteNs = st.clock_write_ns; prevFramesWritten = st.frames_written;
            prevTimeMs = nowMs; havePrev = true;
        }

        // 3. Servo: predecir + actualizar con la medición (si la hay).
        servo.predict(dt);
        if (haveMeasurement) { servo.update(measPpm); ++servoUpdates; }
        double ratio = servo.ratio();
        double drift = servo.driftPpm();
        // Vigilante: si el ratio diverge demasiado, bypass.
        bool ratioSane = std::fabs(drift) < 1000.0 && ratio > 0.99 && ratio < 1.01;
        if (!ratioSane) { ratio = 1.0; }

        // 4. Descubrir la sesión real cada ~3 s.
        // 0038+0040 Parche B: si la sesión cambia o cae, el snapshot
        // coloreado NO debe sobrevivir en omega_shm: se resetea a neutro
        // antes de publicar (si no, el bofo persiste tras cerrar IVANNA
        // hasta matar la app de música).
        // CORONA Frente A (defecto 4): el cambio se publica SOLO a través
        // de SessionChangeGate (applyDiscoveryPoll): un kNone aislado o un
        // flap de 1-2 polls ya no resetea el snapshot ni sube sessionGen;
        // un cambio real se adopta en <= ~9 s. El reset-a-neutro en sí
        // (Parche B) NO se toca: sigue protegiendo contra bofo colgado
        // cuando la sesión cae de verdad (3 kNone seguidos).
        bool resetSnapForSessionChange = false;
        if (--sessionCountdown <= 0) {
            sessionCountdown = 60;
            const auto out = ivanna::daemon::discoverMusicSession("com.ivanna.omega",
                                                                  demotedSessions);
            // FASE A: el foreground se refresca en CADA ciclo (aunque la
            // sesión no cambie): la app expira la guarda de huérfano al
            // cambiar la app en foreground, y eso no espera a que cambie
            // la sesión. En kError se conservan los valores anteriores.
            if (out.status != ivanna::daemon::SessionDiscoveryStatus::kError) {
                foregroundPackage = out.foregroundPackage;
            }
            // EVOL-LIVEPATH (sesión rancia 4057): el beacon es el juez de
            // "esta sesión suena". Se lee en cada poll (~3 s); si la
            // confirmada lleva >= 5 polls muda y el poll trae otra
            // ganadora, se desaloja + degrada ANTES de alimentar el gate
            // (el gate confirmaría la viva desde cero en <= 9 s).
            // Sin beacon no se juzga; sin otra ganadora no se desaloja.
            {
                uint64_t beaconFrames = 0, beaconAppliedGen = 0;
                const auto beaconState = ivanna::readEffectBeacon(
                    2000ull, ivanna::OMEGA_EFFECT_BEACON_PATH,
                    &beaconFrames, &beaconAppliedGen);
                const bool beaconAvailable =
                    beaconState != ivanna::EffectBeaconState::Unavailable;
                const int32_t pollWinner =
                    (out.status == ivanna::daemon::SessionDiscoveryStatus::kFound)
                        ? out.sessionId : 0;
                if (sessionLiveness.notePoll(sessionId, pollWinner,
                                             beaconAvailable, beaconFrames)) {
                    const int32_t evicted = sessionId;
                    demotedSessions.add(evicted);
                    sessionGate.evict();
                    sessionId = 0;
                    ++sessionGen;
                    resetSnapForSessionChange = true;
                    char evictBuf[128];
                    std::snprintf(evictBuf, sizeof(evictBuf),
                                  "sesión desalojada por silencio del beacon: %d (ganadora %d)",
                                  static_cast<int>(evicted),
                                  static_cast<int>(pollWinner));
                    logLine(evictBuf);
                } else if (sessionLiveness.advancedOnLastPoll() && sessionId > 0) {
                    // La confirmada suena: redención (si estaba degradada
                    // por un silencio viejo, vuelve a competir normal).
                    demotedSessions.redeem(sessionId);
                }
            }
            resetSnapForSessionChange = ivanna::daemon::applyDiscoveryPoll(
                sessionGate, out, sessionId, sessionGen, sessionPackage) ||
                resetSnapForSessionChange;
            // kError: el gate congela todo (se conserva la sesión anterior).
        }

        // 0044 (Fallo 2): vigilar si la app IVANNA fue forzada a cerrar.
        // Si murió (ESRCH), limpiar desiredPerceptual + snapshot a neutro
        // para no dejar bofo colgado sobre Tidal/Qobuz. Si sigue viva y
        // solo cambió la app de música, desiredPerceptual se re-siembra
        // en el nuevo sessionId (paso 8).
        bool appDiedReset = false;
        const int32_t knownAppPid = cmdState.appPid.load(std::memory_order_relaxed);
        if (knownAppPid > 0 && ::kill(static_cast<pid_t>(knownAppPid), 0) != 0 && errno == ESRCH) {
            ivanna::daemon::clearOnAppExit(cmdState);
            lastSnap = ivanna::OmegaDspSnapshot::makeNeutral();
            appDiedReset = true;
        }

        // 5. Veredicto.
        if (killSwitch) verdict = IVANNA_VERDICT_OFF;
        else if (!muscleAlive || !muscleFresh) verdict = IVANNA_VERDICT_NO_CLOCK;
        else if (!servo.initialized || !haveMeasurement) verdict = IVANNA_VERDICT_BYPASS;
        else if (!ratioSane) verdict = IVANNA_VERDICT_BYPASS;
        else verdict = IVANNA_VERDICT_LOADED;
        if (verdict != IVANNA_VERDICT_LOADED) ratio = 1.0f;

        // 5b. El campo Ψ vivo (el corazón de la fusión).
        // El cerebro actualiza η con la coherencia que mide el músculo
        // (Ec. 4: dη/dt = (η*−η)/τ − λ·L; L = innovación del servo) y
        // calcula las ganancias por banda del campo (Ec. 2 + Ec. 3 + Γ(η)).
        // El músculo las aplica a TODO el audio. El campo solo actúa cuando
        // la fusión está enganchada (LOADED); si no, queda neutro.
        {
            const double coherence = muscleFresh && std::isfinite(st.coherence)
                ? std::clamp(static_cast<double>(st.coherence), 0.0, 1.0) : 0.5;
            const double loss = std::clamp(servo.lastInnovation / 1000.0, 0.0, 1.0);
            psi.update(dt, coherence, loss);
        }
        float psiGainL[ivanna::psi::kPsiBands];
        float psiGainR[ivanna::psi::kPsiBands];
        psi.bandGains(psiGainL, psiGainR);
        const uint32_t psiActive = (verdict == IVANNA_VERDICT_LOADED && !killSwitch) ? 1u : 0u;
        if (psiActive) ++psiGeneration;
        // Diseñar el FIR del campo solo cuando η o la fuente cambian
        // (el campo se mueve en segundos, no en cada muestra).
        if (psiActive) {
            const double dEta = (lastFirEta < 0.0) ? 1.0 : std::fabs(psi.eta - lastFirEta);
            const bool srcChanged = (std::fabs(psi.srcR - lastFirR) > 0.01f)
                || (std::fabs(psi.srcTheta - lastFirTheta) > 0.01f);
            if (!firDesigned || dEta > 0.01 || srcChanged) {
                const float sr = (muscleFresh && st.stream_rate_hz > 0)
                    ? static_cast<float>(st.stream_rate_hz) : 48000.0f;
                psi.designFirs(psiFirL, psiFirR, sr);
                lastFirEta = psi.eta; lastFirR = psi.srcR; lastFirTheta = psi.srcTheta;
                firDesigned = true;
                ++psiFirGen;
            }
        }

        // 6. Control de efectos (config; por defecto todo apagado).
        FxControl fx = readFxConfig();
        if (verdict != IVANNA_VERDICT_LOADED && verdict != IVANNA_VERDICT_BYPASS) {
            fx = FxControl{};  // sin cerebro sano, sin efectos
        }
        if (killSwitch) fx = FxControl{};

        // 7. Escribir las decisiones del cerebro en el estado compartido.
        {
            IvannaFusionState w = st;  // parte de lo último leído (conserva el músculo)
            w.magic = IVANNA_FUSION_MAGIC; w.version = IVANNA_FUSION_VERSION;
            w.generation = ++generation;
            w.session_id = sessionId; w.session_gen = sessionGen;
            w.ratio = static_cast<float>(ratio);
            w.drift_ppm = static_cast<float>(drift);
            w.clock_source = clockSource; w.verdict = verdict;
            w.brain_health = 1; w.ratio_timestamp_ms = nowMs;
            // v2 (Fase 2, ítem E): heartbeat del cerebro en ns monotónicos
            // (el músculo lo exige fresco <100 ms; al morir el daemon el
            // heartbeat envejece solo y el músculo cae a passthrough) y
            // ASRC en BYPASS 0033 explícito (protección de estabilidad: el
            // ASRC v6 NO se reactiva hasta la etapa 5A medida).
            w.daemon_heartbeat_ns = nowMs * 1000000ULL;
            w.asrc_bypass = 1;
            w.servo_updates = servoUpdates;
            w.fx_flags = fx.flags; w.fx_orbe_amount = fx.orbe;
            w.fx_holos_amount = fx.holos; w.fx_phi_amount = fx.phi;
            w.fx_volterra_amount = fx.volterra; w.fx_supreme_amount = fx.supreme;
            w.fx_npe_amount = fx.npe; w.fx_reality_amount = fx.reality;
            w.fx_eq_amount = fx.eq; w.fx_limiter_on = fx.limiter;
            ++w.fx_generation;
            // El campo Ψ vivo (lo calcula el cerebro, lo aplica el músculo).
            w.psi_eta = static_cast<float>(psi.eta);
            w.psi_gamma = static_cast<float>(psi.coupling());
            w.psi_src_r = psi.srcR; w.psi_src_theta = psi.srcTheta; w.psi_src_phi = psi.srcPhi;
            for (int i = 0; i < ivanna::psi::kPsiBands; ++i) {
                w.psi_gain_l[i] = psiGainL[i];
                w.psi_gain_r[i] = psiGainR[i];
            }
            w.psi_active = psiActive;
            w.psi_generation = psiGeneration;
            for (int i = 0; i < ivanna::psi::PsiCore::kPsiFirTaps; ++i) {
                w.psi_fir_l[i] = psiFirL[i];
                w.psi_fir_r[i] = psiFirR[i];
            }
            w.psi_fir_gen = psiFirGen;
            ivanna_fusion_write(fusion, &w);
        }

        // EVOL-LIVEPATH (hardware_rate_millihz en 0): re-publicar el reloj
        // real del músculo como snapshot HAL para omega_effect. Sin músculo
        // fresco no hay muestra (fail-closed: el beacon sigue en 0 y la
        // app muestra "—", nunca una tasa inventada). Sin sesión
        // confirmada todavía, la muestra lleva identidad 1/1 (ver el
        // helper). Solo publica si la muestra avanzó: un músculo parado
        // no refresca el snapshot y el tracker del efecto deja de
        // validar en vez de sostener una tasa rancia.
        if (muscleFresh) {
            ivanna::audio::HardwareClockSample hwSample{};
            if (ivanna::daemon::buildHardwareClockSample(st, sessionId, sessionGen,
                                                         hwSample)) {
                if (!hwClockPublisherOpen &&
                    (nowMs - lastHwClockOpenAttemptMs) >= 5000ULL) {
                    lastHwClockOpenAttemptMs = nowMs;
                    hwClockPublisherOpen = hwClockPublisher.open();
                    if (hwClockPublisherOpen) {
                        // 0666 como el beacon: el efecto (audioserver,
                        // otro uid) debe poder leerlo. asrc_link_fixperms
                        // lo deja en 0664 solo si su watcher sigue vivo
                        // tras el arranque; esto no depende de él.
                        ::chmod(ivanna::audio::kHardwareClockSnapshotPath, 0666);
                    }
                }
                if (hwClockPublisherOpen &&
                    (hwSample.hardwareTimestampNs != lastHwClockTimestampNs ||
                     hwSample.presentedFrames != lastHwClockFrames)) {
                    if (hwClockPublisher.publish(hwSample)) {
                        lastHwClockTimestampNs = hwSample.hardwareTimestampNs;
                        lastHwClockFrames = hwSample.presentedFrames;
                    }
                }
            }
        }

        // Estado vivo para el hilo de comandos (GET_STATUS): la app lee la
        // sesión descubierta de aquí, no del bucle. Stores atómicos baratos.
        cmdState.sessionId.store(sessionId, std::memory_order_relaxed);
        cmdState.sessionGen.store(sessionGen, std::memory_order_relaxed);
        // FASE A: packages de sesión/foreground (bajo lock; solo cambian en
        // el ciclo de descubrimiento, no en cada iteración del bucle).
        ivanna::daemon::setDiscoveredSessionPackages(cmdState, sessionPackage, foregroundPackage);
        cmdState.generation.store(generation, std::memory_order_relaxed);
        cmdState.verdict.store(verdict, std::memory_order_relaxed);

        // 8. Publicar el snapshot (SHM del efecto) + heartbeat.
        // READ-MODIFY-WRITE: el bucle adopta el snapshot más reciente del
        // bus y SOLO actualiza los campos del cerebro (fusión + sesión).
        // CORRECCIÓN (2026-11-14, causa raíz Ruta B neutra): el bus lo
        // escribe SOLO el daemon (la app nunca abre el SHM como escritora
        // y el servidor viejo de SET_* no se compila en v2), así que
        // readLatest() re-lee la publicación propia y lastSnap NACÍA de
        // makeDefault() (ORBE 0 / HOLOS 0) para siempre. El estado real de
        // la app llega por SET_PERCEPTUAL_STATE al socket (stash del hilo
        // de comandos) y se aplica AQUÍ, antes de publicar: sin ningún
        // SET_PERCEPTUAL_STATE, el snapshot publicado SIGUE siendo neutro
        // por diseño fail-closed (eso lo delata perceptualGen==0 en
        // GET_STATUS); con él, los ajustes de la app sobreviven.
        if (!shmOk) shmOk = bus.openWriter();
        if (shmOk) {
            // 0044 (Fallo 4): el escritor NO re-lee su propia publicación
            // (readLatest era del lector; re-absorbía el snapshot viejo al
            // reiniciar). lastSnap vive solo en este proceso.
            bool dirtySnap = appDiedReset || (snapGeneration == 0);
            if (resetSnapForSessionChange) {
                // 0038+0040 Parche B + 0044 (Fallo 2): sesión cambió o cayó
                // → neutro ANTES de aplicar el pending; si la app sigue
                // viva, se re-siembra desiredPerceptual (sus efectos
                // sobreviven al cambio de app de música).
                lastSnap = ivanna::OmegaDspSnapshot::makeNeutral();
                if (sessionId > 0) {
                    ivanna::daemon::PendingPerceptual desired;
                    if (ivanna::daemon::copyDesiredPerceptual(cmdState, desired)) {
                        ivanna::daemon::applyPendingPerceptual(lastSnap, desired);
                    }
                }
                dirtySnap = true;
            }
            {
                ivanna::daemon::PendingPerceptual pending;
                if (ivanna::daemon::takePendingPerceptual(cmdState, pending)) {
                    ivanna::daemon::applyPendingPerceptual(lastSnap, pending);
                    dirtySnap = true;
                }
            }
            const bool metaChanged = (sessionId != lastBusSession) ||
                (sessionGen != lastBusSessionGen) ||
                (verdict != lastBusVerdict) ||
                (psiActive != lastBusPsiActive);
            const bool keepaliveDue = (nowMs - lastBusPubMs) >= 1000ULL;
            if (dirtySnap || metaChanged || keepaliveDue) {
                applyFusionToSnapshot(lastSnap, sessionId, sessionGen, ++snapGeneration,
                                      verdict, drift, ratio,
                                      static_cast<int32_t>(st.clock_source),
                                      psi.eta, psi.coupling(), psiActive,
                                      static_cast<uint64_t>(st.sequence));
                // P2 (2026-11-26, RMS=0 en vivo): reflejar la telemetría
                // medida por el efecto (bus local) en el snapshot de
                // control antes de publicar. Apertura best-effort con
                // reintento espaciado 5 s; si el bus local no abre, el
                // snapshot publica ceros como hoy (fail-closed). El espejo
                // solo toca campos de telemetría; publish() estampa
                // generación/timestamp/CRC después.
                if (!localTelemetryOpen &&
                    (nowMs - lastLocalOpenAttemptMs) >= 5000ULL) {
                    lastLocalOpenAttemptMs = nowMs;
                    localTelemetryOpen = localTelemetryBus.openReader(
                        ivanna::OMEGA_EFFECT_LOCAL_BUS_PATH);
                }
                // EVOL-LIVEPATH (consumer_generation muerto): el beacon
                // lleva la generación que el efecto confirmó aplicar. Se
                // lee en cada publicación (~1/s) y se anota en el espejo
                // ANTES de applyTo, para que lastSnap.consumer_generation
                // avance con lo que el efecto aplica de verdad. Sin
                // beacon (efecto apagado) no se anota nada y el campo
                // conserva su último valor (0 si nunca aplicó nada).
                bool telFresh = false;
                if (localTelemetryOpen) {
                    telFresh = telemetryMirror.poll(localTelemetryBus, nowMs);
                }
                {
                    uint64_t beaconFrames = 0, beaconAppliedGen = 0;
                    const auto beaconState = ivanna::readEffectBeacon(
                        2000ull, ivanna::OMEGA_EFFECT_BEACON_PATH,
                        &beaconFrames, &beaconAppliedGen);
                    if (beaconState != ivanna::EffectBeaconState::Unavailable) {
                        telemetryMirror.noteBeaconAppliedGeneration(beaconAppliedGen);
                    }
                }
                telemetryMirror.applyTo(lastSnap, telFresh);
                if (!bus.publish(lastSnap)) {
                    shmOk = false;
                } else {
                    // 0044 (Fallo 3): GET_STATUS publica la generación del
                    // bus (la que el efecto aplica), no la de fusión.
                    cmdState.snapshotGeneration.store(
                        bus.lastPublishedGeneration(), std::memory_order_relaxed);
                    lastBusPubMs = nowMs;
                    lastBusSession = sessionId;
                    lastBusSessionGen = sessionGen;
                    lastBusVerdict = verdict;
                    lastBusPsiActive = psiActive;
                }
            }
            // Estado vivo para GET_STATUS: amounts del último snapshot
            // vigente (publicado o no en este ciclo).
            cmdState.snapshotOrbeAmount.store(lastSnap.orbe_amount, std::memory_order_relaxed);
            cmdState.snapshotHolosAmount.store(lastSnap.holos_amount, std::memory_order_relaxed);
            if (shmOk && sessionId != lastPublishedSession) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "sesión publicada: %d (gen %llu)",
                              static_cast<int>(sessionId),
                              static_cast<unsigned long long>(sessionGen));
                logLine(buf);
                lastPublishedSession = sessionId;
            }
        }
        // 0044 (Fallo 6, reconciliado con 0043): auto-reparación del
        // beacon/bus si quedó ciego tras el arranque (máx 1x/10 s). Usa las
        // constantes de ruta (0043 las apunta a /data/adb/ivanna_omega/).
        if ((nowMs - lastBeaconCheckMs) >= 10000ULL) {
            lastBeaconCheckMs = nowMs;
            if (ivanna::readEffectBeacon() == ivanna::EffectBeaconState::Unavailable) {
                (void)ivanna::precreateAndRelabelLocalFile(
                    ivanna::OMEGA_EFFECT_BEACON_PATH, ivanna::OMEGA_EFFECT_BEACON_SIZE,
                    "u:object_r:ivanna_data_file:s0");
                (void)ivanna::precreateAndRelabelLocalFile(
                    ivanna::OMEGA_EFFECT_LOCAL_BUS_PATH, ivanna::OmegaControlBus::REGION_SIZE,
                    "u:object_r:ivanna_data_file:s0");
            }
        }
        // Heartbeat con throttle (2.5.461): 1 Hz por reloj + cambio de estado.
        const bool hbStateChanged = (sessionId != hbLastSession) ||
            (verdict != hbLastVerdict) ||
            (static_cast<int32_t>(st.clock_source) != hbLastClock) ||
            (static_cast<uint8_t>(psiActive ? 1 : 0) != hbLastPsiAct);
        if (hbStateChanged || (nowMs - hbLastMs) >= 1000ULL) {
            writeHeartbeat(sessionId, generation, static_cast<float>(drift), verdict,
                           static_cast<float>(psi.eta), static_cast<float>(psi.coupling()),
                           static_cast<float>(ratio), static_cast<int32_t>(st.clock_source),
                           psiActive);
            hbLastMs = nowMs;
            hbLastSession = sessionId;
            hbLastVerdict = verdict;
            hbLastClock = static_cast<int32_t>(st.clock_source);
            hbLastPsiAct = static_cast<uint8_t>(psiActive ? 1 : 0);
        }

        // El socket de comandos lo atiende el hilo propio (serveCommandSocket,
        // v2.5.440). Antes este punto drenaba accept()+close() y GET_STATUS
        // jamás se respondía; ese drenaje se eliminó: robaría conexiones al
        // hilo y rompería el acople de Ruta B de forma intermitente.

        ::usleep(50000);  // ~20 Hz
    }

    // El hilo de comandos sale solo cuando g_running cae (select 1 s);
    // se espera antes de cerrar el socket que atiende.
    if (cmdThread.joinable()) cmdThread.join();

    // Al salir, dejar el veredicto en BYPASS y el campo Ψ neutro para que
    // el músculo pase limpio.
    {
        IvannaFusionState cur{};
        if (ivanna_fusion_read(fusion, &cur) && cur.magic == IVANNA_FUSION_MAGIC) {
            IvannaFusionState w = cur;
            w.verdict = IVANNA_VERDICT_BYPASS; w.ratio = 1.0f;
            w.brain_health = 0; w.fx_flags = 0;
            w.psi_active = 0;
            for (int i = 0; i < 32; ++i) { w.psi_gain_l[i] = 1.0f; w.psi_gain_r[i] = 1.0f; }
            ivanna_fusion_write(fusion, &w);
        }
    }
    if (sockFd >= 0) ::close(sockFd);
    logLine("ivanna_daemon_v2 (CEREBRO) detenido (señal)");
    return 0;
}
