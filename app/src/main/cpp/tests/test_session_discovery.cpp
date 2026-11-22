// test_session_discovery.cpp — Descubrimiento de sessionId REAL por el daemon root (Ruta B, v2.5.379).
//
// PROBLEMA: la sesión 0 es un callejón sin salida en este Moto G85 (el efecto
// se crea pero nunca recibe frames) y el sondeo por reflexión de la app no ve
// sessionIds reales (anonimizados -1/0 sin MODIFY_AUDIO_ROUTING). El daemon
// (root) ejecuta `dumpsys media.audio_flinger` y parsea "Global session refs".
//
// Cubre:
//   - parseo del formato moderno de dumpsys (Android 14/15: "session cnt pid
//     uid name" + líneas "%7d %4d %7d %6u %s") con salida realista.
//   - parseo del formato antiguo (triples "session pid count" por comas).
//   - edge cases: sin sección, sección vacía, basura (fail-closed: vacío).
//   - selección: prefiere música, excluye la propia app y systemui, desempata
//     por cnt y luego por sessionId mayor (más reciente), 0 si nada elegible.
//   - ABI v11 del snapshot: discovered_session_id/gen al final, offsets viejos
//     sin cambio (effect_frames=440, raw_rms=412, consumer_generation=32).
//
// NOTA: este test es header-only (session_discovery.hpp no tiene .cpp).
// Para registrarlo en la suite: ivanna_add_test(test_session_discovery) en
// tests/CMakeLists.txt (el parent lo añade al revisar/commitear).
#include <gtest/gtest.h>
#include "../daemon/session_discovery.hpp"
#include "../include/omega_control_bus.h"
#include <cstddef>  // offsetof
#include <cstring>  // strlen (árbol /proc falso)
#include <filesystem>
#include <fstream>

using ivanna::daemon::AudioSessionRef;
using ivanna::daemon::parseAudioFlingerSessionRefs;
using ivanna::daemon::selectMusicSessionId;
using ivanna::OmegaDspSnapshot;

// Salida realista de `dumpsys media.audio_flinger` (formato moderno AOSP).
// NOTA (2026-11-09 22:46 CST): el 14177 de este fixture es un número de
// sesión SINTÉTICO. En el volcado real del G85, 14177 era el PID de YTM y la
// sesión real era 249 (ver kDumpsysRealG85PidVsSession). El parser solo lee
// el primer campo como sessionId, así que el fixture sigue siendo válido
// como prueba de parseo; no confundir con el caso real.
static const char* kDumpsysModerno =
    "AudioFlinger Dump\n"
    "Clients:\n"
    " pid: 22156\n"
    "  pid 22156, uid 10123\n"
    "Global session refs:\n"
    " session cnt pid uid name\n"
    "   14177    2  22156 10123 com.google.android.apps.youtube.music\n"
    "       0    1  12345  1000 com.android.systemui\n"
    "   11198    1  11198 10332 com.ivanna.omega\n"
    "Hardware status: 0\n"
    "Global effect: 0\n";

TEST(SessionDiscovery, ParseFormatoModerno) {
    const auto refs = parseAudioFlingerSessionRefs(kDumpsysModerno);
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_EQ(refs[0].sessionId, 14177);
    EXPECT_EQ(refs[0].cnt, 2);
    EXPECT_EQ(refs[0].pid, 22156);
    EXPECT_EQ(refs[0].uid, 10123);
    EXPECT_EQ(refs[0].packageName, "com.google.android.apps.youtube.music");
    EXPECT_EQ(refs[1].sessionId, 0);  // systemui en sesión 0: se parsea pero no es elegible
    EXPECT_EQ(refs[1].packageName, "com.android.systemui");
    EXPECT_EQ(refs[2].sessionId, 11198);
    EXPECT_EQ(refs[2].packageName, "com.ivanna.omega");
}

TEST(SessionDiscovery, ParseFormatoAntiguoComas) {
    // Formato antiguo (algunos ROMs): triples "session pid count" por comas, sin package.
    const std::string text =
        "Global session refs: session pid count\n"
        " 25 5297 1, 33 5297 1, 14177 22156 2,\n"
        "Hardware status: 0\n";
    const auto refs = parseAudioFlingerSessionRefs(text);
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_EQ(refs[0].sessionId, 25);
    EXPECT_EQ(refs[0].pid, 5297);
    EXPECT_EQ(refs[0].cnt, 1);
    EXPECT_TRUE(refs[0].packageName.empty());  // sin package en el formato antiguo
    EXPECT_EQ(refs[2].sessionId, 14177);
    EXPECT_EQ(refs[2].cnt, 2);
}

TEST(SessionDiscovery, ParseSinSeccionDevuelveVacio) {
    EXPECT_TRUE(parseAudioFlingerSessionRefs("AudioFlinger Dump\nClients:\n pid: 1\n").empty());
    EXPECT_TRUE(parseAudioFlingerSessionRefs("").empty());
}

TEST(SessionDiscovery, ParseSeccionVaciaDevuelveVacio) {
    const std::string text =
        "Global session refs:\n"
        " session cnt pid uid name\n"
        "Hardware status: 0\n";
    EXPECT_TRUE(parseAudioFlingerSessionRefs(text).empty());
}

TEST(SessionDiscovery, ParseNoTragaOtrasSecciones) {
    // Las líneas de "Clients:" tienen menos campos y NO deben parsearse como refs.
    const std::string text =
        "Clients:\n"
        "  22156 10123 com.google.android.apps.youtube.music\n"
        "Global session refs:\n"
        " session cnt pid uid name\n"
        "   14177    2  22156 10123 com.google.android.apps.youtube.music\n"
        "Hardware status: 0\n";
    const auto refs = parseAudioFlingerSessionRefs(text);
    ASSERT_EQ(refs.size(), 1u);
    EXPECT_EQ(refs[0].sessionId, 14177);
}

// Volcado REAL del Moto G85 (Android 15, 2026-11-09 22:46 CST) — la confusión
// PID vs sessionId que dejó huérfano el efecto. El track de YTM está en
// Session 249, Client (PID) 14177. La línea "Global session refs" lo confirma:
//   249 1 14177 10258 com.google.android.apps.youtube.music
// (session=249, count=1, pid=14177, uid=10258). El bloque Termux original
// (`grep YTM | awk '{print $1}'`) tomaba la primera línea coincidente del
// dump completo, que es una lista donde el primer campo es el PID (14177),
// no la de "Global session refs". Todos los hot fixes inyectaron el PID.
// El parser SOLO lee dentro de "Global session refs", así que debe devolver
// 249, no 14177. La ref 14177 existe (IVANNA la creó al hacer el efecto para
// el PID) pero su package es com.ivanna.omega (propia app) → excluida.
static const char* kDumpsysRealG85PidVsSession =
    "AudioFlinger Dump\n"
    "Clients:\n"
    " pid: 14177\n"
    "  14177 10258 com.google.android.apps.youtube.music\n"
    "Playback threads:\n"
    "  Thread 0x7... type MIXER\n"
    "Global session refs:\n"
    " session cnt pid uid name\n"
    "     249    1   14177 10258 com.google.android.apps.youtube.music\n"
    "     673    2   17055 10332 com.ivanna.omega\n"
    "   14177    1   17055 10332 com.ivanna.omega\n"
    "Hardware status: 0\n"
    "Global effect: 0\n";

TEST(SessionDiscovery, ParseVolcadoRealG85PidVsSession) {
    const auto refs = parseAudioFlingerSessionRefs(kDumpsysRealG85PidVsSession);
    // Solo las 3 refs de la sección; la línea señuelo (PID primero) de
    // "Clients:" se ignora (está antes del marcador).
    ASSERT_EQ(refs.size(), 3u);
    EXPECT_EQ(refs[0].sessionId, 249);
    EXPECT_EQ(refs[0].cnt, 1);
    EXPECT_EQ(refs[0].pid, 14177);
    EXPECT_EQ(refs[0].uid, 10258);
    EXPECT_EQ(refs[0].packageName, "com.google.android.apps.youtube.music");
    EXPECT_EQ(refs[1].sessionId, 673);
    EXPECT_EQ(refs[2].sessionId, 14177);  // la sesión bogus que creó IVANNA
    EXPECT_EQ(refs[2].packageName, "com.ivanna.omega");
    // El selector debe devolver la sesión REAL de YTM (249), NO el PID (14177).
    // La 14177 se excluye por ser de la propia app (com.ivanna.omega).
    EXPECT_EQ(selectMusicSessionId(refs), 249);
}

TEST(SessionDiscovery, SelectPrefiereMusicaYExcluyePropiaYSistema) {
    const auto refs = parseAudioFlingerSessionRefs(kDumpsysModerno);
    // YTM (14177) gana; systemui (sesión 0) y la propia app (11198) se excluyen.
    EXPECT_EQ(selectMusicSessionId(refs), 14177);
}

TEST(SessionDiscovery, SelectSoloPropiaYSistemaDevuelveCero) {
    const std::vector<AudioSessionRef> refs = {
        {11198, 1, 11198, 10332, "com.ivanna.omega"},
        {0, 1, 12345, 1000, "com.android.systemui"},
    };
    EXPECT_EQ(selectMusicSessionId(refs), 0);  // fail-closed: sin sesión elegible
}

TEST(SessionDiscovery, SelectPrefiereMayorCnt) {
    const std::vector<AudioSessionRef> refs = {
        {100, 1, 1000, 1000, "com.spotify.music"},
        {200, 5, 2000, 1000, "com.google.android.apps.youtube.music"},
    };
    EXPECT_EQ(selectMusicSessionId(refs), 200);  // más referencias = más activa
}

TEST(SessionDiscovery, SelectDesempataPorSessionIdMayor) {
    const std::vector<AudioSessionRef> refs = {
        {297, 1, 1000, 1000, "com.google.android.apps.youtube.music"},
        {14177, 1, 2000, 1000, "com.google.android.apps.youtube.music"},
    };
    EXPECT_EQ(selectMusicSessionId(refs), 14177);  // mismo cnt: la más reciente
}

TEST(SessionDiscovery, SelectSinMusicaCaeEnCualquierSesionElegible) {
    // Sin app de música, cualquier sesión elegible vale más que la sesión 0
    // (que no procesa): el efecto debe ir a la sesión que suena, sea lo que sea.
    const std::vector<AudioSessionRef> refs = {
        {500, 1, 5000, 1000, "com.example.game"},
    };
    EXPECT_EQ(selectMusicSessionId(refs), 500);
}

TEST(SessionDiscovery, SelectIgnoraSessionIdsInvalidos) {
    const std::vector<AudioSessionRef> refs = {
        {0, 1, 1000, 1000, "com.spotify.music"},
        {-1, 1, 1000, 1000, "com.spotify.music"},
    };
    EXPECT_EQ(selectMusicSessionId(refs), 0);
}

TEST(SessionDiscovery, SelectAgrupaRefsDeLaMismaSesion) {
    // Dos refs de la misma sesión suman cnt; la sesión con más cnt total gana.
    const std::vector<AudioSessionRef> refs = {
        {14177, 2, 22156, 10123, "com.google.android.apps.youtube.music"},
        {14177, 3, 22157, 10123, "com.google.android.apps.youtube.music"},
        {100, 4, 1000, 1000, "com.spotify.music"},
    };
    EXPECT_EQ(selectMusicSessionId(refs), 14177);  // cnt total 5 > 4
}

TEST(SessionDiscovery, SnapshotAbiV17ConservationAlFinalYOffsetsViejosIntactos) {
    EXPECT_EQ(ivanna::OMEGA_CTRL_VERSION, 17u);
    EXPECT_EQ(sizeof(OmegaDspSnapshot), 808u);
    // Campos nuevos AL FINAL, antes de crc32 (los offsets viejos NO cambian:
    // el od del teléfono en byte 448/420/40 sigue siendo válido).
    EXPECT_EQ(offsetof(OmegaDspSnapshot, discovered_session_id), 608u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, discovered_session_gen), 616u);
    // v12: los campos fusion_* van DESPUÉS de discovered_session_gen (624..664);
    // v13: los campos hyper_*/volterra_enabled van DESPUÉS de fusion_generation
    // (664..680); v14: los 17 campos np_* van DESPUÉS de volterra_enabled
    // (680..748); v15: los 4 floats raw_band_* van DESPUÉS de np_abx_mode
    // (748..764); crc32 se mueve a 764. v16 (486): los 5 campos
    // spsc/yamnet van DESPUÉS de raw_band_spread (764..784) + reserved_v16
    // @784; crc32 se mueve a 788. v17: los 4 campos consv_* van DESPUÉS
    // de reserved_v16 (788..804); crc32 se mueve a 804. Los offsets
    // viejos (440/412/416/32) NO cambian.
    EXPECT_EQ(offsetof(OmegaDspSnapshot, fusion_active), 624u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, fusion_generation), 656u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, hyper_amount), 664u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, hyper_preset), 668u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, hyper_xtc_enabled), 672u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, volterra_enabled), 676u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, np_amount), 680u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, np_abx_mode), 744u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, raw_band_low), 748u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, raw_band_mid), 752u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, raw_band_high), 756u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, raw_band_spread), 760u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, crc32), 804u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, effect_frames), 440u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, raw_rms), 412u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, raw_peak), 416u);
    EXPECT_EQ(offsetof(OmegaDspSnapshot, consumer_generation), 32u);
    // Default: sin sesión descubierta (fail-closed) y CRC válido.
    OmegaDspSnapshot d = OmegaDspSnapshot::makeDefault();
    EXPECT_EQ(d.discovered_session_id, 0);
    EXPECT_EQ(d.discovered_session_gen, 0u);
    EXPECT_TRUE(d.isMagicValid());
    EXPECT_TRUE(d.isVersionValid());
    EXPECT_TRUE(d.isCrcValid());
    // Con sesión descubierta, el CRC se recalcula y sigue siendo válido.
    d.discovered_session_id = 14177;
    d.discovered_session_gen = 1;
    d.stampCrc();
    EXPECT_TRUE(d.isCrcValid());
    EXPECT_EQ(d.discovered_session_id, 14177);
    EXPECT_EQ(d.discovered_session_gen, 1u);
}

// ── FASE A (2026-11-18): la sesión de la app en foreground gana ──────────
// Caso medido: Qobuz 585 en pausa (cnt 5, refs viejas en dumpsys) vencía a
// la sesión nueva de Tidal (cnt 1) y el daemon seguía publicando 585; el
// cambio de app cortaba el audio y la guarda de huérfano bloqueaba el
// re-acople. Con foreground=Tidal, la sesión de Tidal gana aunque tenga
// menos cnt. Sin foreground, el ranking es el viejo (cnt, sessionId).
TEST(SessionDiscovery, SelectForegroundMusicBeatsHigherCntStaleSession) {
    using ivanna::daemon::selectMusicSession;
    const std::vector<AudioSessionRef> refs = {
        {585, 5, 1000, 1000, "com.qobuz.music"},   // vieja, en pausa, cnt alto
        {2417, 1, 2000, 1000, "com.aspiro.tidal"}, // nueva, en foreground
    };
    // Sin foreground: gana la vieja por cnt (el bug medido).
    EXPECT_EQ(selectMusicSession(refs).sessionId, 585);
    // Con foreground=Tidal: gana la nueva aunque tenga cnt 1.
    const auto choice = selectMusicSession(refs, "com.ivanna.omega", "com.aspiro.tidal");
    EXPECT_EQ(choice.sessionId, 2417);
    EXPECT_EQ(choice.packageName, "com.aspiro.tidal");
}

TEST(SessionDiscovery, SelectForegroundNonMusicDoesNotStealBackgroundMusic) {
    // El usuario juega en foreground mientras Tidal suena en segundo plano:
    // el juego NO debe robarle la sesión a la música.
    using ivanna::daemon::selectMusicSession;
    const std::vector<AudioSessionRef> refs = {
        {2417, 1, 2000, 1000, "com.aspiro.tidal"},
        {9001, 1, 3000, 1000, "com.example.game"},
    };
    const auto choice = selectMusicSession(refs, "com.ivanna.omega", "com.example.game");
    EXPECT_EQ(choice.sessionId, 2417);
    EXPECT_EQ(choice.packageName, "com.aspiro.tidal");
}

TEST(SessionDiscovery, SelectForegroundOnlyReranksAmongMusicSessions) {
    // Dos apps de música: el foreground desempataba por cnt; el background
    // con más cnt ya no gana si el foreground es música.
    using ivanna::daemon::selectMusicSession;
    const std::vector<AudioSessionRef> refs = {
        {100, 5, 1000, 1000, "com.spotify.music"},
        {200, 1, 2000, 1000, "com.google.android.apps.youtube.music"},
    };
    EXPECT_EQ(selectMusicSession(refs, "com.ivanna.omega", "com.google.android.apps.youtube.music").sessionId, 200);
    EXPECT_EQ(selectMusicSession(refs, "com.ivanna.omega", "com.spotify.music").sessionId, 100);
}

// ── FASE A: detección de foreground por /proc (función pura) ──────────────
TEST(SessionDiscovery, ForegroundPackageSingleAdjZeroWins) {
    using ivanna::daemon::ForegroundProcEntry;
    using ivanna::daemon::selectForegroundPackage;
    const std::vector<ForegroundProcEntry> entries = {
        {100, 100, "com.example.launcher"},   // visible, no foreground
        {200, 0, "com.aspiro.tidal"},         // foreground
        {300, -800, "system"},                // sistema, no es app
    };
    EXPECT_EQ(selectForegroundPackage(entries), "com.aspiro.tidal");
}

TEST(SessionDiscovery, ForegroundPackageAmbiguousReturnsEmpty) {
    // Dos packages distintos en adj 0: no se inventa un foreground.
    using ivanna::daemon::ForegroundProcEntry;
    using ivanna::daemon::selectForegroundPackage;
    const std::vector<ForegroundProcEntry> entries = {
        {200, 0, "com.aspiro.tidal"},
        {201, 0, "com.qobuz.music"},
    };
    EXPECT_TRUE(selectForegroundPackage(entries).empty());
}

TEST(SessionDiscovery, ForegroundPackageIgnoresDaemonsAndNoApp) {
    // Daemons del sistema (cmdline con '/' o sin punto) no cuentan como
    // foreground aunque tengan adj 0; sin app en adj 0, "" (fail-closed).
    using ivanna::daemon::ForegroundProcEntry;
    using ivanna::daemon::selectForegroundPackage;
    EXPECT_TRUE(selectForegroundPackage({
        {1, 0, "/system/bin/audioserver"},
        {2, 0, "zygote64"},
        {3, 100, "com.example.launcher"},
    }).empty());
    // Varios procesos de LA MISMA app en adj 0 (principal + remoto, ya
    // recortado al package por readForegroundPackage) no son ambigüedad.
    EXPECT_EQ(selectForegroundPackage({
        {200, 0, "com.aspiro.tidal"},
        {201, 0, "com.aspiro.tidal"},
    }), "com.aspiro.tidal");
}

TEST(SessionDiscovery, ForegroundPackageReadsFakeProcTree) {
    // /proc falso en disco: readForegroundPackage(procRoot) debe devolver
    // la app en adj 0 y recortar el proceso remoto ("pkg:remote").
    using ivanna::daemon::readForegroundPackage;
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "ivanna_fake_proc_fg";
    fs::remove_all(root);
    const auto mkProc = [&](const char* pid, int adj, const char* cmd) {
        const fs::path d = root / pid;
        fs::create_directories(d);
        { std::ofstream f(d / "oom_score_adj"); f << adj << "\n"; }
        { std::ofstream f(d / "cmdline", std::ios::binary); f.write(cmd, std::strlen(cmd) + 1); }
    };
    mkProc("100", 100, "com.example.launcher");
    mkProc("200", 0, "com.aspiro.tidal:remote");
    mkProc("300", -800, "system");
    EXPECT_EQ(readForegroundPackage(root.string()), "com.aspiro.tidal");
    fs::remove_all(root);
    // Raíz inexistente: "" (fail-closed, sin crash).
    EXPECT_TRUE(readForegroundPackage("/proc/no-existe-ivanna").empty());
}

TEST(SessionDiscovery, SanitizePackageNameKeepsJsonSafe) {
    using ivanna::daemon::sanitizePackageName;
    EXPECT_EQ(sanitizePackageName("com.aspiro.tidal"), "com.aspiro.tidal");
    EXPECT_EQ(sanitizePackageName("com.evil\";rm"), "com.evilrm");
    EXPECT_EQ(sanitizePackageName("/system/bin/x"), "systembinx");
    EXPECT_TRUE(sanitizePackageName("").empty());
}
// de sesión. Con lastSnap coloreado en sessionId=1697, al pasar a 0 (kNone,
// la música paró) o a 2329 (kFound, otra app), el daemon resetea lastSnap a
// makeNeutral() antes de publicar. Si no, el bofo persiste tras cerrar
// IVANNA hasta matar la app de música (experimento 16:40 CST).
TEST(SessionDiscovery, ParcheBSessionChangeNeutralizesSnapshot) {
    using ivanna::daemon::SessionDiscoveryOutcome;
    using ivanna::daemon::SessionDiscoveryStatus;
    using ivanna::daemon::shouldResetSnapshotForSessionChange;

    // Sesión coloreada 1697 -> cae a 0 (kNone): reset.
    {
        SessionDiscoveryOutcome out;
        out.status = SessionDiscoveryStatus::kNone;
        out.sessionId = 0;
        EXPECT_TRUE(shouldResetSnapshotForSessionChange(1697, out));
    }
    // Sesión coloreada 1697 -> cambia a 2329 (kFound): reset.
    {
        SessionDiscoveryOutcome out;
        out.status = SessionDiscoveryStatus::kFound;
        out.sessionId = 2329;
        EXPECT_TRUE(shouldResetSnapshotForSessionChange(1697, out));
    }
    // Misma sesión 1697 -> 1697 (kFound): NO reset (sigue sonando la misma app).
    {
        SessionDiscoveryOutcome out;
        out.status = SessionDiscoveryStatus::kFound;
        out.sessionId = 1697;
        EXPECT_FALSE(shouldResetSnapshotForSessionChange(1697, out));
    }
    // Sin sesión (0) y kNone: NO reset (ya está en neutro).
    {
        SessionDiscoveryOutcome out;
        out.status = SessionDiscoveryStatus::kNone;
        out.sessionId = 0;
        EXPECT_FALSE(shouldResetSnapshotForSessionChange(0, out));
    }
    // kError (dumpsys falló): NO reset (se conserva la sesión anterior).
    {
        SessionDiscoveryOutcome out;
        out.status = SessionDiscoveryStatus::kError;
        out.sessionId = 0;
        EXPECT_FALSE(shouldResetSnapshotForSessionChange(1697, out));
    }
    // El neutro que se publica tras el reset es la identidad acústica:
    // ORBE/HOLOS/Φ/Volterra en 0 y sin flag VOLTERRA_ON.
    {
        OmegaDspSnapshot n = OmegaDspSnapshot::makeNeutral();
        EXPECT_FLOAT_EQ(n.orbe_amount, 0.0f);
        EXPECT_FLOAT_EQ(n.holos_amount, 0.0f);
        EXPECT_FLOAT_EQ(n.hyper_amount, 0.0f);
        EXPECT_EQ(n.volterra_enabled, 0u);
        EXPECT_EQ(n.flags & ivanna::OMEGA_FLAG_VOLTERRA_ON, 0u);
        EXPECT_TRUE(n.isCrcValid());
    }
}
// ── FIX 2 (v2.5.465): puerta de dwell del cambio de sesión ────────────────
// Vivo 2026-11-18 (v2.5.464): 13 generaciones en ~2 min (0/97/169/185/193/
// 2217/2321/2329/2305/2337) porque cada poll ruidoso publicaba sesión nueva
// y el efecto se soltaba y re-acoplaba (saltos + underruns). La puerta exige
// 3 polls consistentes (~9 s) para publicar; un kNone aislado se ignora;
// kError congela todo. Estos tests replican la secuencia medida.
namespace {
using ivanna::daemon::SessionChangeGate;
using ivanna::daemon::SessionDiscoveryOutcome;
using ivanna::daemon::SessionDiscoveryStatus;

SessionDiscoveryOutcome mkFound(int32_t sid) {
    SessionDiscoveryOutcome o;
    o.status = SessionDiscoveryStatus::kFound;
    o.sessionId = sid;
    return o;
}
SessionDiscoveryOutcome mkNone() {
    SessionDiscoveryOutcome o;
    o.status = SessionDiscoveryStatus::kNone;
    o.sessionId = 0;
    return o;
}
SessionDiscoveryOutcome mkError() {
    SessionDiscoveryOutcome o;
    o.status = SessionDiscoveryStatus::kError;
    o.sessionId = 0;
    return o;
}
}  // namespace

TEST(SessionChangeGate, FlapMedidoVivoNoPublicaNada) {
    // Replay de la secuencia viva 2026-11-18: sesiones que viven 1 poll y
    // un 0 intermitente. Antes del fix: 13 generaciones. Con dwell: 0.
    SessionChangeGate g;
    EXPECT_FALSE(g.feed(mkFound(2217)));
    EXPECT_FALSE(g.feed(mkFound(2321)));
    EXPECT_FALSE(g.feed(mkFound(2329)));
    EXPECT_FALSE(g.feed(mkFound(2305)));
    EXPECT_FALSE(g.feed(mkFound(2337)));
    EXPECT_FALSE(g.feed(mkNone()));
    EXPECT_FALSE(g.feed(mkFound(97)));
    EXPECT_FALSE(g.feed(mkFound(169)));
    EXPECT_FALSE(g.feed(mkFound(185)));
    EXPECT_FALSE(g.feed(mkFound(193)));
    EXPECT_FALSE(g.feed(mkFound(2217)));
    EXPECT_FALSE(g.feed(mkFound(2321)));
    EXPECT_FALSE(g.feed(mkFound(2329)));
    EXPECT_EQ(g.sessionId(), 0);
    EXPECT_EQ(g.sessionGen(), 0u);
}

TEST(SessionChangeGate, SesionRealConfirmaAlTercerPoll) {
    SessionChangeGate g;
    EXPECT_FALSE(g.feed(mkFound(2305)));
    EXPECT_EQ(g.pendingSessionId(), 2305);
    EXPECT_EQ(g.pendingCount(), 1);
    EXPECT_FALSE(g.feed(mkFound(2305)));
    EXPECT_EQ(g.pendingCount(), 2);
    EXPECT_TRUE(g.feed(mkFound(2305)));  // 3er poll: commit
    EXPECT_EQ(g.sessionId(), 2305);
    EXPECT_EQ(g.sessionGen(), 1u);
    EXPECT_EQ(g.pendingSessionId(), 0);
    // La misma sesión en los polls siguientes no genera más generaciones.
    EXPECT_FALSE(g.feed(mkFound(2305)));
    EXPECT_FALSE(g.feed(mkFound(2305)));
    EXPECT_EQ(g.sessionGen(), 1u);
}

TEST(SessionChangeGate, CambioRealDeAppConfirmaUnaSolaGeneracion) {
    SessionChangeGate g;
    g.feed(mkFound(2305)); g.feed(mkFound(2305)); g.feed(mkFound(2305));
    ASSERT_EQ(g.sessionId(), 2305);
    // El usuario cambia a otra app: 2 polls no bastan, el 3º confirma.
    EXPECT_FALSE(g.feed(mkFound(2337)));
    EXPECT_FALSE(g.feed(mkFound(2337)));
    EXPECT_EQ(g.sessionId(), 2305);  // sigue la vieja mientras no confirma
    EXPECT_TRUE(g.feed(mkFound(2337)));
    EXPECT_EQ(g.sessionId(), 2337);
    EXPECT_EQ(g.sessionGen(), 2u);
}

TEST(SessionChangeGate, AlternanciaABNuncaConfirma) {
    // Dos sesiones vivas que se alternan poll a poll (típico del flap):
    // ninguna llega a 3 seguidos y la confirmada no se mueve.
    SessionChangeGate g;
    g.feed(mkFound(2305)); g.feed(mkFound(2305)); g.feed(mkFound(2305));
    ASSERT_EQ(g.sessionGen(), 1u);
    for (int i = 0; i < 3; ++i) {
        EXPECT_FALSE(g.feed(mkFound(2337)));
        EXPECT_FALSE(g.feed(mkFound(2305)));
    }
    EXPECT_EQ(g.sessionId(), 2305);
    EXPECT_EQ(g.sessionGen(), 1u);
}

TEST(SessionChangeGate, UnSoloNoneIntermitenteNoSueltaNiResetea) {
    SessionChangeGate g;
    g.feed(mkFound(2305)); g.feed(mkFound(2305)); g.feed(mkFound(2305));
    ASSERT_EQ(g.sessionId(), 2305);
    // kNone aislado entre sesiones válidas: no cae a 0...
    EXPECT_FALSE(g.feed(mkNone()));
    EXPECT_EQ(g.sessionId(), 2305);
    EXPECT_FALSE(g.feed(mkFound(2305)));
    EXPECT_FALSE(g.feed(mkNone()));
    EXPECT_EQ(g.sessionId(), 2305);
    EXPECT_EQ(g.sessionGen(), 1u);
    // ...y tampoco resetea el candidato pendiente (queda congelado).
    EXPECT_FALSE(g.feed(mkFound(2337)));  // pendiente 2337, cuenta 1
    EXPECT_FALSE(g.feed(mkNone()));       // 0 intermitente: no resetea
    EXPECT_EQ(g.pendingSessionId(), 2337);
    EXPECT_EQ(g.pendingCount(), 1);
    EXPECT_FALSE(g.feed(mkFound(2337)));  // cuenta 2 (no vuelve a 1)
    EXPECT_TRUE(g.feed(mkFound(2337)));   // cuenta 3: confirma
    EXPECT_EQ(g.sessionId(), 2337);
}

TEST(SessionChangeGate, CaidaACeroExigeTresNoneSeguidos) {
    SessionChangeGate g;
    g.feed(mkFound(2305)); g.feed(mkFound(2305)); g.feed(mkFound(2305));
    ASSERT_EQ(g.sessionId(), 2305);
    EXPECT_FALSE(g.feed(mkNone()));
    EXPECT_FALSE(g.feed(mkNone()));
    EXPECT_EQ(g.sessionId(), 2305);  // 2 kNone no bastan
    EXPECT_TRUE(g.feed(mkNone()));   // 3er kNone: cae a 0
    EXPECT_EQ(g.sessionId(), 0);
    EXPECT_EQ(g.sessionGen(), 2u);
    // Ya en 0, más kNone no generan generaciones.
    EXPECT_FALSE(g.feed(mkNone()));
    EXPECT_EQ(g.sessionGen(), 2u);
}

TEST(SessionChangeGate, ErrorCongelaTodoFailClosed) {
    SessionChangeGate g;
    g.feed(mkFound(2305)); g.feed(mkFound(2305));  // pendiente, cuenta 2
    EXPECT_FALSE(g.feed(mkError()));               // ni confirma ni resetea
    EXPECT_EQ(g.pendingSessionId(), 2305);
    EXPECT_EQ(g.pendingCount(), 2);
    EXPECT_TRUE(g.feed(mkFound(2305)));            // 3er poll real: confirma
    EXPECT_EQ(g.sessionGen(), 1u);
    // kError con sesión confirmada tampoco la suelta ni cuenta como kNone.
    EXPECT_FALSE(g.feed(mkError()));
    EXPECT_FALSE(g.feed(mkError()));
    EXPECT_EQ(g.sessionId(), 2305);
    EXPECT_EQ(g.noneCount(), 0);
    // Un kFound de la misma sesión borra el candidato pendiente.
    SessionChangeGate h;
    h.feed(mkFound(2305)); h.feed(mkFound(2305)); h.feed(mkFound(2305));
    h.feed(mkFound(2337)); h.feed(mkFound(2337));  // pendiente 2337, cuenta 2
    EXPECT_FALSE(h.feed(mkFound(2305)));            // la vieja sigue viva
    EXPECT_EQ(h.pendingSessionId(), 0);
    EXPECT_EQ(h.pendingCount(), 0);
    EXPECT_EQ(h.sessionId(), 2305);
}
