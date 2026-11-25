// session_discovery.hpp — Descubrimiento del sessionId REAL de música por el daemon root.
// © 2026 Luis Uriel Pimentel Pérez — IVANNA OMEGA SUPREME
//
// PROBLEMA (medido 2026-11-09, v2.5.378):
//   - El fix del -38 funcionó: el efecto omega se crea (1 Client, sin -38).
//   - PERO procesa 0 frames: quedó en sesión 0 (global), que en este Moto G85
//     falla en silencio (el efecto se crea pero nunca recibe frames). El propio
//     IvannaGlobalEffectManager.kt advierte que los efectos insert en sesión 0
//     son "deprecated/poco fiable: en algunos dispositivos falla en silencio".
//   - El sondeo por reflexión de la app NO descubre sessionIds reales:
//     getActivePlaybackConfigurations() devuelve copias ANONIMIZADAS (-1/0) sin
//     MODIFY_AUDIO_ROUTING (signature|privileged). Por eso la app no puede
//     acoplarse a la sesión real de YouTube Music (la 297).
// SOLUCIÓN: el daemon corre como root, así que ejecuta `dumpsys
// media.audio_flinger` y parsea la sección "Global session refs:" (formato
// AOSP verificado en fuente, frameworks/av AudioFlinger.cpp):
//     Global session refs:
//      session cnt pid uid name
//          297    1   12345 10123 com.google.android.apps.youtube.music
//   y publica el sessionId de música en el snapshot (ABI v11) + GET_STATUS.
//   La app acopla el efecto omega a ESA sesión en lugar de a la sesión 0.
//
// FORMATO (AOSP, android-latest-release):
//   Moderno (Android 14/15): líneas " %7d %4d %7d %6u %s" (session cnt pid
//   uid package) bajo el encabezado " session cnt pid uid name".
//   Antiguo (algunos ROMs): triples "session pid count" separados por comas
//   en una sola línea ("Global session refs: session pid count"), sin package.
//   El parser acepta ambos; el formato antiguo no trae package, así que esas
//   refs solo sirven como fallback sin atribución (package vacío).
//
// GARANTÍAS:
//   - parseAudioFlingerSessionRefs / selectMusicSessionId son PURAS (sin I/O,
//     sin estado global): unit-testeables en host con salidas reales de dumpsys.
//   - Fail-closed: si dumpsys falla, si no hay sección, o si no hay sesión de
//     música, el resultado es kNone/kError (sessionId 0) y la app sigue con su
//     comportamiento actual (sesión 0 como fallback). Nunca se inventa un ID.
//   - Sin malloc en el hot path de audio: el descubrimiento corre en un hilo
//     propio del daemon cada ~3 s (nunca en el callback de audio).
//   - No depende del package exacto de YouTube Music: cualquier sesión que no
//     sea de la propia app ni del sistema y que huela a música gana; si no hay
//     ninguna de música, gana la sesión no excluida más reciente (mayor ID).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <dirent.h>

namespace ivanna::daemon {

// ── Referencia de sesión de audio (una línea de "Global session refs") ──────
struct AudioSessionRef {
    int32_t sessionId = 0;   // > 0 si es real; 0/-1 = inválido
    int32_t cnt = 0;         // referencias (cuántos clientes la usan)
    int32_t pid = 0;
    int32_t uid = 0;
    std::string packageName; // vacío en el formato antiguo (sin package)
};

// ── Parseo puro de `dumpsys media.audio_flinger` ────────────────────────────
// Extrae las refs de la sección "Global session refs:". Devuelve vacío si no
// hay sección, si está vacía o si ninguna línea parsea. Nunca lanza.
inline std::vector<AudioSessionRef> parseAudioFlingerSessionRefs(const std::string& dumpsysText) {
    std::vector<AudioSessionRef> refs;
    const std::string kMarker = "Global session refs:";
    const size_t markerPos = dumpsysText.find(kMarker);
    if (markerPos == std::string::npos) return refs;

    // Texto desde el marcador. El resto de la línea del marcador puede traer
    // el encabezado del formato antiguo ("session pid count").
    std::string rest = dumpsysText.substr(markerPos + kMarker.size());
    // Recorre línea por línea.
    size_t pos = 0;
    bool sawAnyRef = false;
    while (pos <= rest.size()) {
        const size_t nl = rest.find('\n', pos);
        const std::string line = (nl == std::string::npos)
            ? rest.substr(pos) : rest.substr(pos, nl - pos);
        pos = (nl == std::string::npos) ? rest.size() + 1 : nl + 1;

        // Línea en blanco: saltar.
        bool blank = true;
        for (char c : line) if (c != ' ' && c != '\t' && c != '\r') { blank = false; break; }
        if (blank) continue;

        // Encabezado (" session cnt pid uid name" / " session pid count"):
        // contiene "session" y no empieza por dígito. Saltar.
        size_t firstNonSpace = 0;
        while (firstNonSpace < line.size() && (line[firstNonSpace] == ' ' || line[firstNonSpace] == '\t')) ++firstNonSpace;
        if (firstNonSpace < line.size() && (line[firstNonSpace] < '0' || line[firstNonSpace] > '9')) {
            // No es una ref (encabezado u otra sección). Si ya vimos refs, la
            // sección terminó; si no, seguir buscando (por si hay basura antes).
            if (sawAnyRef) break;
            continue;
        }

        // Formato moderno: " session cnt pid uid package" (5 campos).
        {
            int session = 0, cnt = 0, pid = 0;
            unsigned uid = 0;
            char pkg[128] = {};
            const int n = std::sscanf(line.c_str(), "%d %d %d %u %127s",
                                      &session, &cnt, &pid, &uid, pkg);
            if (n == 5 && session >= 0) {  // sesión 0 (systemui) sí se registra; el selector la descarta
                refs.push_back(AudioSessionRef{session, cnt, pid, static_cast<int32_t>(uid), pkg});
                sawAnyRef = true;
                continue;
            }
        }
        // Formato antiguo: triples "session pid count" separados por comas.
        {
            bool anyTriple = false;
            size_t tpos = 0;
            while (tpos < line.size()) {
                const size_t comma = line.find(',', tpos);
                const std::string tok = (comma == std::string::npos)
                    ? line.substr(tpos) : line.substr(tpos, comma - tpos);
                tpos = (comma == std::string::npos) ? line.size() + 1 : comma + 1;
                int session = 0, pid = 0, cnt = 0;
                if (std::sscanf(tok.c_str(), "%d %d %d", &session, &pid, &cnt) == 3 && session >= 0) {
                    refs.push_back(AudioSessionRef{session, cnt, pid, 0, ""});
                    anyTriple = true;
                }
            }
            if (anyTriple) { sawAnyRef = true; continue; }
        }
        // Línea que no es ref ni encabezado ni blanco: fin de la sección.
        if (sawAnyRef) break;
    }
    return refs;
}

// ── ¿Huele a música? (substrings en minúsculas sobre el package) ────────────
inline bool packageLooksLikeMusic(const std::string& pkg) noexcept {
    static const char* kMusicHints[] = {
        "music", "spotify", "tidal", "deezer", "soundcloud", "pandora",
        "applemusic", "amazon.music", "youtube", "sound", "audio", "radio",
        "tunein", "iheart", "plex", "vlc", "audible", "podcast", "player",
        "jiosaavn", "gaana", "wynk", "hungama", "napster", "qobuz",
    };
    std::string lower;
    lower.reserve(pkg.size());
    for (char c : pkg) lower.push_back((c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c);
    for (const char* hint : kMusicHints) {
        if (lower.find(hint) != std::string::npos) return true;
    }
    return false;
}

// Paquetes del sistema que nunca son la app de música (ni la propia IVANNA).
inline bool packageIsExcluded(const std::string& pkg, const std::string& ownPackage) noexcept {
    if (pkg.empty()) return true;  // formato antiguo sin package: no atribuible
    if (pkg == ownPackage) return true;
    if (pkg == "com.android.systemui" || pkg == "android" || pkg == "system" ||
        pkg == "com.android.phone" || pkg == "com.android.shell") return true;
    return false;
}

// ── Saneado de package (para publicar en JSON sin escapar) ─────────────────
// Solo [A-Za-z0-9._], máx. 128 chars. Un package de dumpsys/cmdline con
// caracteres raros queda recortado, nunca roto: GET_STATUS sigue siendo JSON
// válido aunque el nombre venga sucio.
inline std::string sanitizePackageName(const std::string& pkg) noexcept {
    std::string out;
    out.reserve(pkg.size() < 128 ? pkg.size() : 128);
    for (char c : pkg) {
        if (out.size() >= 128) break;
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_';
        if (ok) out.push_back(c);
    }
    return out;
}

// ¿Parece un package de app Android (com.ejemplo.app)? Los cmdline de
// daemons del sistema ("/system/bin/...", "zygote64", "logd") no lo parecen:
// sin este filtro, cualquier daemon con oom_score_adj 0 haría ambigua la
// detección de foreground (ver selectForegroundPackage).
inline bool looksLikePackageName(const std::string& cmd) noexcept {
    if (cmd.empty() || cmd.size() > 128) return false;
    if (cmd[0] < 'a' || cmd[0] > 'z') return false;
    bool hasDot = false;
    for (size_t i = 0; i < cmd.size(); ++i) {
        const char c = cmd[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '.' || c == '_';
        if (!ok) return false;
        if (c == '.') {
            if (i + 1 >= cmd.size() || cmd[i + 1] == '.') return false;
            hasDot = true;
        }
    }
    return hasDot;
}

// ── Selección pura: elige el sessionId de música ────────────────────────────
// Agrupa refs por sesión. Descarta sesiones cuyos refs son TODOS de paquetes
// excluidos (propia app / sistema). Entre las restantes prefiere una sesión
// de música; a igualdad, mayor cnt total; luego mayor sessionId (más reciente).
// Devuelve 0 si no hay ninguna sesión elegible (fail-closed).
// FASE A (2026-11-18): la app en foreground desempataba por cnt y la sesión
// VIEJA en pausa ganaba siempre (Qobuz 585 con cnt>=2 vencía a la sesión
// nueva de Tidal con cnt 1: el daemon seguía publicando 585 y el cambio de
// app cortaba el audio). La clave de orden ahora es (música, foreground,
// cnt, sessionId): el foreground solo reordena ENTRE sesiones de música —
// una app en foreground que NO es música (p. ej. un juego) NO le roba la
// sesión a la música que suena en segundo plano. Sin foreground (""), el
// ranking es exactamente el de antes.
struct MusicSessionChoice {
    int32_t sessionId = 0;      // 0 = ninguna elegible (fail-closed)
    std::string packageName;    // package de la sesión elegida ("" si ninguna)
};

inline MusicSessionChoice selectMusicSession(const std::vector<AudioSessionRef>& refs,
                                             const std::string& ownPackage = "com.ivanna.omega",
                                             const std::string& foregroundPackage = "") noexcept {
    struct Agg { int32_t sessionId = 0; int32_t cnt = 0; bool hasMusic = false; bool hasEligible = false; bool hasForeground = false; std::string packageName; };
    std::vector<Agg> aggs;
    for (const auto& r : refs) {
        if (r.sessionId <= 0) continue;
        auto it = std::find_if(aggs.begin(), aggs.end(),
                               [&](const Agg& a) { return a.sessionId == r.sessionId; });
        if (it == aggs.end()) { aggs.push_back(Agg{r.sessionId, 0, false, false, false, ""}); it = aggs.end() - 1; }
        it->cnt += r.cnt;
        if (!packageIsExcluded(r.packageName, ownPackage)) {
            it->hasEligible = true;
            if (it->packageName.empty()) it->packageName = r.packageName;
            if (packageLooksLikeMusic(r.packageName)) it->hasMusic = true;
            if (!foregroundPackage.empty() && r.packageName == foregroundPackage) it->hasForeground = true;
        }
    }
    const Agg* best = nullptr;
    for (const auto& a : aggs) {
        if (!a.hasEligible) continue;
        if (best == nullptr) { best = &a; continue; }
        if (a.hasMusic != best->hasMusic) { if (a.hasMusic) best = &a; continue; }
        if (a.hasForeground != best->hasForeground) { if (a.hasForeground) best = &a; continue; }
        if (a.cnt != best->cnt) { if (a.cnt > best->cnt) best = &a; continue; }
        if (a.sessionId > best->sessionId) best = &a;
    }
    if (best == nullptr) return MusicSessionChoice{};
    return MusicSessionChoice{best->sessionId, best->packageName};
}

inline int32_t selectMusicSessionId(const std::vector<AudioSessionRef>& refs,
                                    const std::string& ownPackage = "com.ivanna.omega",
                                    const std::string& foregroundPackage = "") noexcept {
    return selectMusicSession(refs, ownPackage, foregroundPackage).sessionId;
}

// ── App en foreground por /proc (daemon root) ───────────────────────────────
// En Android, la app en foreground corre con oom_score_adj == 0
// (FOREGROUND_APP_ADJ); las visibles van a 100 y el sistema a valores
// negativos. El daemon (root) puede leer /proc/<pid>/oom_score_adj +
// /proc/<pid>/cmdline sin permisos especiales. El cmdline de una app es su
// package ("com.aspiro.tidal"); los procesos remotos ("pkg:proc") se
// recortan al package. Puro sobre entradas ya leídas: unit-testeable en
// host con un /proc falso.
struct ForegroundProcEntry {
    int32_t pid = 0;
    int32_t oomScoreAdj = 0;
    std::string cmdline;  // primer token de /proc/<pid>/cmdline, ya saneado
};

// Devuelve el package de la app en foreground, o "" si no hay ninguna
// (ningún proceso de app en adj 0) o si hay AMBIGÜEDAD (dos packages
// distintos en adj 0): fail-closed, el llamador usa el ranking viejo.
inline std::string selectForegroundPackage(const std::vector<ForegroundProcEntry>& entries) noexcept {
    std::string found;
    for (const auto& e : entries) {
        if (e.oomScoreAdj != 0) continue;
        if (!looksLikePackageName(e.cmdline)) continue;
        if (found.empty()) { found = e.cmdline; continue; }
        if (found != e.cmdline) return {};  // ambiguo: no se inventa un foreground
    }
    return found;
}

// Lee /proc (o un árbol falso en host) y devuelve el package en foreground.
// Nunca lanza; "" si /proc no se puede leer o no hay foreground claro.
inline std::string readForegroundPackage(const std::string& procRoot = "/proc") noexcept {
    DIR* dir = ::opendir(procRoot.c_str());
    if (dir == nullptr) return {};
    std::vector<ForegroundProcEntry> entries;
    while (dirent* ent = ::readdir(dir)) {
        const char* name = ent->d_name;
        bool numeric = name[0] != '\0';
        for (const char* p = name; *p != '\0'; ++p) {
            if (*p < '0' || *p > '9') { numeric = false; break; }
        }
        if (!numeric) continue;
        const std::string adjPath = procRoot + "/" + name + "/oom_score_adj";
        FILE* f = std::fopen(adjPath.c_str(), "r");
        if (f == nullptr) continue;
        int adj = 0;
        const bool okAdj = std::fscanf(f, "%d", &adj) == 1;
        std::fclose(f);
        if (!okAdj) continue;
        const std::string cmdPath = procRoot + "/" + name + "/cmdline";
        FILE* cf = std::fopen(cmdPath.c_str(), "r");
        if (cf == nullptr) continue;
        char buf[256] = {};
        const size_t n = std::fread(buf, 1, sizeof(buf) - 1, cf);
        std::fclose(cf);
        if (n == 0) continue;
        std::string cmd(buf, ::strnlen(buf, sizeof(buf)));  // hasta el primer NUL
        const size_t colon = cmd.find(':');
        if (colon != std::string::npos) cmd = cmd.substr(0, colon);
        entries.push_back(ForegroundProcEntry{std::atoi(name), adj, sanitizePackageName(cmd)});
    }
    ::closedir(dir);
    return selectForegroundPackage(entries);
}

// ── Descubrimiento impuro (popen a dumpsys) ──────────────────────────────────
enum class SessionDiscoveryStatus { kFound, kNone, kError };
struct SessionDiscoveryOutcome {
    SessionDiscoveryStatus status = SessionDiscoveryStatus::kError;
    int32_t sessionId = 0;  // > 0 solo si status == kFound
    // FASE A: package de la sesión elegida + app en foreground (ambos
    // saneados [A-Za-z0-9._], "" si se desconocen). La app los usa para
    // expirar la guarda de huérfano al cambiar de app y para no re-acoplar
    // a una sesión rancia de fondo. Un daemon viejo no los publica y la
    // app los lee como "" (comportamiento anterior).
    std::string packageName;
    std::string foregroundPackage;
};

// ── ¿Resetea el snapshot a neutro al cambiar/caer la sesión? (0038+0040) ───
// El daemon publica el snapshot coloreado para la sesión descubierta. Si la
// sesión cambia (kFound con ID distinto) o cae (kNone con sesión vigente),
// el estado coloreado de la sesión vieja NO debe sobrevivir en omega_shm:
// el llamador resetea lastSnap a makeNeutral() antes de publicar (si no, el
// bofo persiste tras cerrar IVANNA hasta matar la app de música).
// Pura (sin I/O): unit-testeable en host.
// LEGACY (CORONA Frente A, defecto 4): el bucle del daemon ya NO usa este
// helper directo — usa applyDiscoveryPoll() con SessionChangeGate (el helper
// decide por poll, sin dwell, y cada poll ruidoso reseteaba + subía gen).
// Se conserva para sus tests y como decisión por poll sin histéresis.
inline bool shouldResetSnapshotForSessionChange(int32_t currentSessionId,
                                                const SessionDiscoveryOutcome& out) noexcept {
    if (out.status == SessionDiscoveryStatus::kFound) {
        return out.sessionId != currentSessionId;
    }
    if (out.status == SessionDiscoveryStatus::kNone) {
        return currentSessionId != 0;
    }
    return false;  // kError: se conserva la sesión anterior (y su snapshot)
}

// ── Puerta de cambio de sesión con dwell / histéresis (FIX 2, v2.5.465) ────
// CAUSA RAÍZ MEDIDA (vivo 2026-11-18, v2.5.464): el daemon publicaba sesión
// nueva en cada poll ruidoso — 13 generaciones en ~2 min (sesiones
// 0/97/169/185/193/2217/2321/2329/2305/2337, media ~9.2 s por generación y
// varias a 1 poll de distancia). Cada cambio de sessionId soltaba y
// re-acoplaba el efecto (sessionGen++ → re-attach en la app / efecto) =
// salto + underruns=7 en AudioFlinger. El descubrimiento en sí NO es la
// fuente del ruido que hay que arreglar: dumpsys es un snapshot instantáneo
// de refs que aparecen/desaparecen (carreras de AudioFlinger, cnt empatado,
// foreground ambiguo) y selectMusicSession() reordena al ganador en cada
// poll; el ruido es inherente al muestreo, así que se filtra AQUÍ, en el
// cambio publicado, no en el parser ni en el selector (que se conservan).
//
// REGLA: una sesión nueva solo se PUBLICA (confirmedSessionId + ++gen) tras
// kConfirmPolls polls consecutivos reportándola; la caída a 0 exige
// kConfirmPolls kNone consecutivos; un kNone aislado entre sesiones válidas
// se ignora (no resetea el candidato pendiente ni la sesión confirmada);
// kError congela todo (ni confirma ni resetea: fail-closed, como antes).
// sessionGen solo se incrementa en un cambio REAL confirmado: el efecto y
// la app solo re-acoplan entonces. Pura (sin I/O): unit-testeable en host.
//
// PARÁMETRO — kConfirmPolls = 3 (poll cada ~3 s: 60 ciclos x 50 ms):
//   - Ventana de observación ~9 s (1ª vista t=0, confirmación en el 3er
//     poll, t≈6 s; hasta ~9 s con la fase del poll). Ninguna sesión del
//     flap medido vivió 3 polls seguidos (≤1–2 polls), así que NINGUNA
//     habría confirmado jamás: 13 generaciones → 0.
//   - Un cambio REAL (el usuario cambia de app de música) persiste minutos
//     → se adopta en ≤9 s; mientras tanto el efecto sigue acoplado a la
//     sesión confirmada anterior (el audio no se corta, la app nueva suena
//     sin procesar como mucho 9 s y se acopla UNA vez, no a cada poll).
//   - Por qué no N=2: un transitorio puede vivir 2 polls seguidos (sesión
//     corta de una notificación, flip de foreground al cambiar de app) y
//     N=2 lo publicaría. Por qué no N=4: 9–12 s de latencia de adopción sin
//     evidencia de transitorios de 3 polls; el coste supera el beneficio.
//     N=3 es la rodilla: la ventana (~9 s) ≈ el periodo medio del flap
//     medido (~9.2 s), así que un ciclo de flap no completa una ventana.
struct SessionChangeGate {
    static constexpr int kConfirmPolls = 3;

    // Alimenta un resultado de descubrimiento (un poll, ~3 s). Devuelve
    // true solo si hubo un cambio REAL confirmado (nueva sesión o caída
    // a 0): el llamador publica sessionId()/sessionGen() entonces (o los
    // re-lee en cada poll: solo cambian en un commit).
    bool feed(const SessionDiscoveryOutcome& out) noexcept {
        switch (out.status) {
            case SessionDiscoveryStatus::kError:
                return false;  // dumpsys falló: todo congelado, se conserva
            case SessionDiscoveryStatus::kFound: {
                if (out.sessionId <= 0) return false;  // kFound sin ID: ruido
                noneCount_ = 0;  // hay música: se rompe la racha de kNone
                if (out.sessionId == confirmedSessionId_) {
                    // La sesión confirmada sigue viva: el candidato muere.
                    pendingSessionId_ = 0; pendingCount_ = 0;
                    return false;
                }
                if (out.sessionId == pendingSessionId_) ++pendingCount_;
                else { pendingSessionId_ = out.sessionId; pendingCount_ = 1; }
                if (pendingCount_ >= kConfirmPolls) {
                    confirmedSessionId_ = pendingSessionId_;
                    pendingSessionId_ = 0; pendingCount_ = 0;
                    ++confirmedGen_;
                    return true;
                }
                return false;
            }
            case SessionDiscoveryStatus::kNone: {
                // 0 intermitente: NO toca el candidato pendiente (congelado,
                // no reseteado) y solo cuenta hacia la caída a 0. Un solo
                // kNone entre sesiones válidas no suelta nada.
                if (confirmedSessionId_ == 0) { noneCount_ = 0; return false; }
                if (++noneCount_ >= kConfirmPolls) {
                    confirmedSessionId_ = 0;
                    pendingSessionId_ = 0; pendingCount_ = 0; noneCount_ = 0;
                    ++confirmedGen_;
                    return true;
                }
                return false;
            }
        }
        return false;
    }

    int32_t sessionId() const noexcept { return confirmedSessionId_; }
    uint64_t sessionGen() const noexcept { return confirmedGen_; }
    // Solo para tests/diagnóstico: candidato pendiente y contadores.
    int32_t pendingSessionId() const noexcept { return pendingSessionId_; }
    int pendingCount() const noexcept { return pendingCount_; }
    int noneCount() const noexcept { return noneCount_; }

 private:
    int32_t confirmedSessionId_ = 0;  // sesión publicada (0 = ninguna)
    uint64_t confirmedGen_ = 0;       // solo ++ en un cambio REAL confirmado
    int32_t pendingSessionId_ = 0;    // candidato aún no confirmado (0 = ninguno)
    int pendingCount_ = 0;            // polls seguidos reportando al candidato
    int noneCount_ = 0;               // kNone seguidos con sesión confirmada
};

// ── Cableado del gate en el bucle del daemon (CORONA Frente A, defecto 4) ──
// El gate (FIX 2, v2.5.465) existía y estaba unit-testeado, pero el bucle
// de ivanna_daemon.cpp NO lo usaba: aplicaba shouldResetSnapshotForSession-
// Change + actualización inmediata de sessionId/sessionGen en cada poll, así
// que cada poll ruidoso reseteaba el snapshot a neutro y subía sessionGen
// (1.380 "sesión publicada" en daemon_v2.log, sesiones 1105/1297/1313
// alternando). ESTA función es la lógica cableada del paso 4 del daemon,
// pura (sin I/O) para poder testearla en host:
//   - alimenta el poll al gate; SOLO cuando el gate confirma un cambio
//     real se publican sessionId/sessionGen nuevos (gen++ una vez) y se
//     devuelve true → el llamador resetea el snapshot a neutro (Parche B);
//   - `package` sigue a la sesión CONFIRMADA, no al poll crudo (el package
//     del poll solo vale si es el de la sesión publicada; al caer a 0 se
//     limpia); en polls sin commit, si el poll reporta la sesión
//     confirmada, su package se refresca (mismo valor en la práctica).
// El foreground NO se toca aquí: el daemon lo refresca en cada poll no
// error, fuera del gate (FASE A). Sin noexcept: la asignación de std::string
// puede lanzar (igual que en el código anterior del daemon).
inline bool applyDiscoveryPoll(SessionChangeGate& gate,
                               const SessionDiscoveryOutcome& out,
                               int32_t& sessionId, uint64_t& sessionGen,
                               std::string& package) {
    const bool committed = gate.feed(out);
    if (committed) {
        sessionId = gate.sessionId();
        ++sessionGen;
    }
    if (gate.sessionId() != 0 && out.status == SessionDiscoveryStatus::kFound &&
        out.sessionId == gate.sessionId()) {
        package = out.packageName;
    } else if (gate.sessionId() == 0) {
        package.clear();
    }
    return committed;
}

// Ejecuta `dumpsys media.audio_flinger` (root) y devuelve el sessionId de
// música. kError si dumpsys falla (popen/read): el llamador NO debe borrar
// el valor anterior (un fallo transitorio no es "sin música"). kNone si
// dumpsys funciona pero no hay sesión de música: el llamador SÍ debe
// publicar 0 (la música paró). Lectura acotada (256 KB): la sección de
// sesiones está cerca del inicio del dump.
inline SessionDiscoveryOutcome discoverMusicSession(
        const std::string& ownPackage = "com.ivanna.omega") noexcept {
    SessionDiscoveryOutcome out;
    FILE* pipe = ::popen("/system/bin/dumpsys media.audio_flinger 2>/dev/null", "r");
    if (pipe == nullptr) return out;  // kError
    std::string text;
    text.reserve(65536);
    char buf[8192];
    size_t total = 0;
    constexpr size_t kMaxBytes = 256u * 1024u;
    while (total < kMaxBytes) {
        const size_t want = sizeof(buf) < (kMaxBytes - total) ? sizeof(buf) : (kMaxBytes - total);
        const size_t n = std::fread(buf, 1, want, pipe);
        if (n == 0) break;
        text.append(buf, n);
        total += n;
        if (n < want) break;
    }
    const int rc = ::pclose(pipe);
    if (rc != 0 && text.empty()) return out;  // kError (dumpsys falló sin salida)
    const auto refs = parseAudioFlingerSessionRefs(text);
    // FASE A: el foreground se lee en el mismo ciclo (~3 s) para que el
    // selector prefiera la sesión de la app que el usuario tiene delante.
    // Si /proc no se puede leer, foreground queda "" y el ranking es el
    // viejo (cnt, sessionId): fail-closed, nunca se inventa un foreground.
    out.foregroundPackage = readForegroundPackage();
    const auto choice = selectMusicSession(refs, ownPackage, out.foregroundPackage);
    const int32_t sid = choice.sessionId;
    out.status = (sid > 0) ? SessionDiscoveryStatus::kFound : SessionDiscoveryStatus::kNone;
    out.sessionId = sid;
    out.packageName = sanitizePackageName(choice.packageName);
    return out;
}

} // namespace ivanna::daemon
