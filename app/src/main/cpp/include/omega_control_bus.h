#pragma once
/*
 * ============================================================
 * IVANNA OMEGA SUPREME — OmegaControlBus
 *
 * Control Plane cross-process para audio system-wide.
 *
 * ARQUITECTURA:
 *   Writer (daemon process):
 *     - OmegaControlBus::openWriter(path) → mmap con MAP_SHARED+PROT_WRITE
 *     - OmegaControlBus::publish(snapshot) → seqlock atómico en SHM
 *     - command_server.cpp es el único publicador autorizado
 *
 *   Reader (audioserver / omega_effect.so):
 *     - OmegaControlBus::openReader(path) → mmap con MAP_SHARED+PROT_READ
 *     - OmegaControlBus::readLatest(out)  → seqlock read, no bloquea
 *     - omega_effect.cpp consume y aplica si route == SYSTEM_WIDE
 *
 *   Route Arbiter:
 *     - RouteMode::SYSTEM_WIDE  → omega_effect aplica, nativeProcess pasa
 *     - RouteMode::IN_PROCESS   → nativeProcess aplica, omega_effect pasa
 *     - RouteMode::OFF          → nadie aplica DSP
 *
 * GARANTÍAS:
 *   - OmegaDspSnapshot es trivialmente copiable (POD puro, sin punteros)
 *   - Seqlock embebido: guard es odd durante escritura, even en reposo
 *   - Validable por MAGIC + VERSION + CRC32
 *   - Tamaño <= 640 bytes (verificado con static_assert; v9 sube de 512 a
 *     536 por los campos ORBE, v10 a 616 por los campos HOLOS — el
 *     presupuesto de 512 B quedó agotado en v9)
 *   - Sin malloc, sin std::vector, sin std::string, sin heap
 *   - Seguro para placement-new sobre región mmap
 *   - Sin locks del SO en el hot path de lectura
 *
 * PROHIBIDO en omega_process() / audio callback:
 *   - malloc / new
 *   - locks pesados
 *   - parsing JSON
 *   - I/O bloqueante
 *   - logging excesivo
 *
 * SHM PATH: /data/adb/ivanna_omega/omega_control_snapshot
 * ============================================================
 */

#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

namespace ivanna::ipc {
enum class ReceiveFdResult { kOk, kRecvError, kBadPayloadLength, kTruncated, kMissingRights, kMalformedControl, kWrongDescriptorCount };

// Receive the protocol's one-byte payload and exactly one SCM_RIGHTS fd.
// Any descriptor received on a rejected message is closed before returning.
inline ReceiveFdResult receiveExactlyOneFd(int socket_fd, int* out_fd, int* out_errno = nullptr) noexcept {
    if (out_fd) *out_fd = -1;
    if (out_errno) *out_errno = 0;
    if (!out_fd) return ReceiveFdResult::kMalformedControl;
    char payload = 0;
    iovec io{}; io.iov_base = &payload; io.iov_len = sizeof(payload);
    constexpr std::size_t kMaxRights = 16;
    alignas(cmsghdr) unsigned char control[CMSG_SPACE(sizeof(int) * kMaxRights)]{};
    msghdr msg{}; msg.msg_iov = &io; msg.msg_iovlen = 1;
    msg.msg_control = control; msg.msg_controllen = sizeof(control);
    // recvmsg() puede fallar con EINTR si una senal llega mientras se espera el
    // descriptor (Android entrega SIGCHLD/SIGTERM a audioserver con frecuencia).
    // Sin reintento, el cliente interpretaba esa carrera como "el daemon no
    // entrega el fd" y caia al modo por ruta, perdiendo el handshake por SCM_RIGHTS.
    ssize_t received;
    do {
        received = ::recvmsg(socket_fd, &msg, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);
    const int receive_error = received < 0 ? errno : 0;
    if (out_errno) *out_errno = receive_error;

    int descriptors[kMaxRights]; std::size_t descriptor_count = 0;
    bool saw_rights = false, malformed = false;
    for (cmsghdr* cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS) continue;
        saw_rights = true;
        if (cmsg->cmsg_len < CMSG_LEN(0)) { malformed = true; continue; }
        const std::size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (bytes % sizeof(int)) malformed = true;
        const auto* data = reinterpret_cast<const unsigned char*>(CMSG_DATA(cmsg));
        for (std::size_t i = 0; i < bytes / sizeof(int); ++i) {
            int fd = -1; std::memcpy(&fd, data + i * sizeof(int), sizeof(fd));
            if (fd < 0) { malformed = true; continue; }
            if (descriptor_count < kMaxRights) descriptors[descriptor_count++] = fd;
            else { ::close(fd); malformed = true; }
        }
    }
    ReceiveFdResult result = ReceiveFdResult::kOk;
    if (received < 0) result = ReceiveFdResult::kRecvError;
    else if (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) result = ReceiveFdResult::kTruncated;
    else if (received != 1) result = ReceiveFdResult::kBadPayloadLength;
    else if (malformed) result = ReceiveFdResult::kMalformedControl;
    else if (!saw_rights) result = ReceiveFdResult::kMissingRights;
    else if (descriptor_count != 1) result = ReceiveFdResult::kWrongDescriptorCount;
    if (result != ReceiveFdResult::kOk) {
        for (std::size_t i = 0; i < descriptor_count; ++i) ::close(descriptors[i]);
        return result;
    }
    *out_fd = descriptors[0];
    return ReceiveFdResult::kOk;
}
} // namespace ivanna::ipc


// Evitar conflictos con android/log.h en omega_effect.cpp
// v2.5.440: LOGW tiene guarda propia (antes solo LOGD la cubría): el
// responder de comandos incluye omega_effect_beacon.h, que define LOGW
// bajo #ifndef OMEGA_CTRL_LOGW; sin esta guarda, incluir el beacon antes
// que este header redefinía LOGW (warning en el build del daemon).
#ifndef OMEGA_CTRL_LOGD
#  if defined(__ANDROID__)
#    include <android/log.h>
#    define OMEGA_CTRL_LOGD(fmt,...) __android_log_print(ANDROID_LOG_DEBUG,"OmegaCtrlBus",fmt,##__VA_ARGS__)
#  else
#    include <cstdio>
#    define OMEGA_CTRL_LOGD(fmt,...) std::fprintf(stderr,"[OCB DBG] " fmt "\n",##__VA_ARGS__)
#  endif
#endif
#ifndef OMEGA_CTRL_LOGW
#  if defined(__ANDROID__)
#    include <android/log.h>
#    define OMEGA_CTRL_LOGW(fmt,...) __android_log_print(ANDROID_LOG_WARN, "OmegaCtrlBus",fmt,##__VA_ARGS__)
#  else
#    include <cstdio>
#    define OMEGA_CTRL_LOGW(fmt,...) std::fprintf(stderr,"[OCB WRN] " fmt "\n",##__VA_ARGS__)
#  endif
#endif

namespace ivanna {

// ══════════════════════════════════════════════════════════════════════════════
// RouteMode — Route Arbiter explícito
// ══════════════════════════════════════════════════════════════════════════════

enum class RouteMode : int32_t {
    OFF          = 0,  // Nadie aplica DSP
    IN_PROCESS   = 1,  // nativeProcess / Ruta A (en proceso de la app)
    SYSTEM_WIDE  = 2,  // omega_effect.so / Ruta B (audioserver, cross-process)
};

inline const char* routeModeStr(RouteMode r) noexcept {
    switch (r) {
        case RouteMode::OFF:         return "OFF";
        case RouteMode::IN_PROCESS:  return "IN_PROCESS";
        case RouteMode::SYSTEM_WIDE: return "SYSTEM_WIDE";
        default:                      return "UNKNOWN";
    }
}

// ══════════════════════════════════════════════════════════════════════════════
// OmegaDspSnapshot — Snapshot de control (ABI fijo, versionado, cross-process)
//
// REGLAS ABI:
//   1. Solo campos trivialmente copiables (floats, ints, arrays fijos)
//   2. Sin punteros, sin VTables, sin herencia
//   3. MAGIC y VERSION deben coincidir para aceptar un snapshot
//   4. CRC32 cubre todos los campos desde generation hasta crc32 (exclusive)
//   5. Incrementar VERSION si cambia el layout
// ══════════════════════════════════════════════════════════════════════════════

constexpr uint32_t OMEGA_CTRL_MAGIC   = 0x4F4D4543u; // "OMEC"
// ABI v2 (2026-08-19): añadido saf_q[7] al snapshot para cerrar la cadena
// Φ_SAF → q[7] → OmegaControlBus → DSP-like adaptive rendering en Ruta B.
// Antes solo viajaban escalares (saf_gain, saf_delta_energy) y el morph
// vector completo se quedaba en Ruta A. Ahora omega_effect recibe el q[7]
// crudo y lo entrega directo al ObjectRenderer/HRTFConvolver via
// setLatentParams(q), habilitando morph HRTF cross-process.
// El bump de VERSION invalida snapshots viejos por diseño (mismatch
// detectado en isVersionValid()); el reader cae a defaults hasta que
// el daemon publique con la ABI nueva — sin ruido audible.
// ABI v4 (2026-09-19): añadido wfs_enabled/wfs_spread/wfs_speaker_{x,y,z}[7]
// al snapshot — cierra la cadena UI->JNI->daemon->snapshot->omega_effect
// ->WfsRenderer con geometría 3D real de sala (antes: atomics aislados por
// proceso, jamás publicados por el daemon).
// ABI v5 (2026-09-26): añadidos los 5 Ejes de Supremacía Cuántico-Neuromórfica
// (8 flags OMEGA_FLAG_SUPREME_* + 11 escalares supreme_*) manteniendo
// sizeof(OmegaDspSnapshot) <= 512 bytes y copia trivial cross-process.
// ABI v9 (2026-11-07): añadidos los campos ORBE (Binaural Esférico de Campo
// Cercano) al snapshot — amount/distancia/spread/proximidad/difuso/head-tracking.
// El snapshot v8 estaba exactamente en 512 B (sin campo nuevo posible): v9 sube
// a 536 B y el static_assert pasa a <= 544 B. El bump invalida snapshots viejos
// por diseño (isVersionValid()); el reader cae a defaults (ORBE off) hasta que
// el daemon publique con la ABI nueva — sin ruido audible. App y daemon se
// distribuyen juntos en el mismo release (APK + módulo Magisk v2.5.363).
// ABI v10 (2026-11-07): añadidos los campos HOLOS (Operador Único de Campo
// Acústico, FUSION.md) — los 13 parámetros de §6 + el EQ estático de Γ (6
// campos, plano por defecto). El snapshot v9 estaba en 536 B: v10 sube a
// 616 B y el static_assert pasa a <= 640 B. Mismo contrato que v9: el bump
// invalida snapshots viejos; el reader cae a defaults (HOLOS off) hasta que
// el daemon publique con la ABI nueva. App y daemon se distribuyen juntos
// en el mismo release (APK + módulo Magisk v2.5.364).
// ABI v11 (2026-11-09): añadidos discovered_session_id (int32) +
// discovered_session_gen (uint64) — el daemon root publica aquí el sessionId
// REAL de la app que reproduce música (descubierto por dumpsys
// media.audio_flinger). La sesión 0 es un callejón sin salida en este G85
// (el efecto se crea pero nunca recibe frames) y el sondeo por reflexión de
// la app no ve sessionIds reales (anonimizados sin MODIFY_AUDIO_ROUTING).
// Los campos van AL FINAL (antes de crc32) para no desplazar ningún offset
// existente (effect_frames sigue en 440, raw_rms en 412): sizeof 616 -> 632,
// dentro del static_assert <= 640. 0 = sin sesión descubierta; gen = contador
// que incrementa solo cuando la sesión descubierta CAMBIA (0->N, N->M, N->0).
// ABI v12 (2026-11-11, fusión magistral): añadido el estado de la FUSIÓN
// (cerebro + músculo + campo Ψ) al snapshot para que la APK lo VEA y se
// ACOPLE — el acoplamiento con el APK que ya está hecho. 9 campos al FINAL
// (antes de crc32): fusion_active, fusion_eta, fusion_psi_gamma,
// fusion_psi_active, fusion_asrc_verdict, fusion_asrc_ratio,
// fusion_asrc_drift_ppm, fusion_asrc_clock, fusion_generation. El cerebro
// (daemon v2) los publica en cada snapshot; la app los lee por el canal que
// ya usa y sabe si el HAL está procesando TODO el audio (fusion_active=1),
// así la Ruta A cede y no procesa dos veces. sizeof 632 -> 672, crc32 en
// 664, static_assert pasa a <= 704. Mismo contrato que v9/v10/v11: el bump
// invalida snapshots viejos; el reader cae a defaults (fusión off) hasta que
// el daemon publique con la ABI nueva. App y daemon se distribuyen juntos
// en el mismo release (APK + módulo Magisk v2.5.401).
// ABI v13 (2026-11-17, 0038+0040): añadidos Φ (HyperState) + Volterra al
// snapshot — hyper_amount/hyper_preset/hyper_xtc_enabled/volterra_enabled
// al FINAL (antes de crc32): 16 bytes. sizeof 672 -> 688, crc32 en 680.
// ABI v14 (2026-11-19, Física Nueva Fase 2, v2.5.472): añadidos los 17
// campos np_* (NewPhysicsControls: LGN 3D + Ω·Σ + Volterra Laguerre +
// lazo variacional F + ABX) al FINAL (antes de crc32): 68 bytes (17×4;
// sizeof 688→752, +64 netos porque v13 tenía 4 B de padding final). Los
// atomics g_np_* ya existían en app y efecto con estos mismos defaults;
// el snapshot v13 no tenía slot para ellos y la Ruta B corría Física
// Nueva siempre en bypass. sizeof 688 -> 752, crc32 en 748,
// static_assert pasa a <= 768. Mismo contrato que v9..v13: el bump
// invalida snapshots viejos; el reader cae a defaults (np_amount 0 =
// bypass bit-exacto) hasta que el daemon publique con la ABI nueva.
// ABI v17 (2026-11-21, capa de conservación perceptual): añadidos los 4
// campos consv_* (árbitro de campo: amount/energyCap/phaseCoherence/
// inertia) al FINAL (antes de crc32): 16 bytes (4×4). sizeof 792 -> 808,
// crc32 en 804, static_assert pasa a <= 808. Mismo contrato que v9..v16:
// el bump invalida snapshots viejos; el reader cae a defaults (consv_amount
// 0 = bypass bit-exacto) hasta que el daemon publique con la ABI nueva.
constexpr uint16_t OMEGA_CTRL_VERSION = 17u;  // v17: consv_* (SET_PERCEPTUAL_STATE, conservación perceptual) en el snapshot.
constexpr int      OMEGA_CTRL_EQ_BANDS = 10;
constexpr int      OMEGA_CTRL_SAF_Q    = 7;   // Dim del morph vector Φ_SAF
constexpr int      OMEGA_CTRL_WFS_SPEAKERS = 7; // FL,FR,SL,SR,TL,TR,SW

// Flags de procesamiento acústico avanzado (campo flags)
constexpr uint32_t OMEGA_FLAG_MASTER_BYPASS = (1u << 0);
constexpr uint32_t OMEGA_FLAG_EQ_CALIBRATED = (1u << 1);
constexpr uint32_t OMEGA_FLAG_PERCEPTUAL_ON = (1u << 2);
constexpr uint32_t OMEGA_FLAG_ANTI_DOLBY_ON = (1u << 3);
constexpr uint32_t OMEGA_FLAG_VOLTERRA_ON   = (1u << 4); // Anti-Lossy Volterra H2 Reconstruction
constexpr uint32_t OMEGA_FLAG_FASTRPC_ON    = (1u << 5); // Qualcomm Hexagon cDSP FastRPC Offload
constexpr uint32_t OMEGA_FLAG_ATI_ON        = (1u << 6); // Active Transducer Inversion (Phase/Group Delay)
constexpr uint32_t OMEGA_FLAG_COCHLEAR_ON   = (1u << 7); // Eje Supremo: Inversión Biomecánica Coclear Activa (Cochlear-PINN)
constexpr uint32_t OMEGA_FLAG_SUPREME_LATTICE_ON     = (1u << 8);  // Eje 1: Warped Lattice Z(ω) + Bl(x)
constexpr uint32_t OMEGA_FLAG_SUPREME_MICROCHIRP_ON  = (1u << 9);  // Eje 1: Micro-Chirp 17.5-19 kHz
constexpr uint32_t OMEGA_FLAG_SUPREME_CVNN_ON        = (1u << 10); // Eje 2: Transharmonic CVNN + DDSP
constexpr uint32_t OMEGA_FLAG_SUPREME_SNN_HOA_ON     = (1u << 11); // Eje 3: SNN INT8 + NMF -> HOA 4th Order
constexpr uint32_t OMEGA_FLAG_SUPREME_PINNA_ON       = (1u << 12); // Eje 4: Pinna Manifold INR-SDF 32-tap FIR
constexpr uint32_t OMEGA_FLAG_SUPREME_FARROW_MSO_ON  = (1u << 13); // Eje 5: Farrow Order-5 MSO Phase Alignment
constexpr uint32_t OMEGA_FLAG_SUPREME_EBPF_BYPASS_ON = (1u << 14); // Eje 5: eBPF/XDP SHM Direct-to-HAL Bypass
constexpr uint32_t OMEGA_FLAG_REALITY_RECON_ON       = (1u << 15); // Fases 1-8: Acoustic Reality Reconstruction Hyperengine
// Flags de Singularidad Acústica Atlas-Escena (bits 16..23; 0 = defaults ON activos en arranque)
constexpr uint32_t OMEGA_FLAG_ATLAS_SCENE_OFF        = (1u << 16); // 1 = desactivar SceneReconstruction
constexpr uint32_t OMEGA_FLAG_ATLAS_LEGACY_DEREVERB  = (1u << 17); // 1 = usar Dereverb legacy en vez de Lebart/Habets
constexpr uint32_t OMEGA_FLAG_ATLAS_LEGACY_ER        = (1u << 18); // 1 = usar ER legacy en vez de PhysicalEarlyReflections
constexpr uint32_t OMEGA_FLAG_ATLAS_LEGACY_SHAPER    = (1u << 19); // 1 = usar softClip legacy en vez de Chebyshev T2+T3
constexpr uint32_t OMEGA_FLAG_ATLAS_STYLE_SHIFT      = 20u;        // bits 20..23: (manualStyle + 1) & 0xF (0 = Auto)
constexpr uint32_t OMEGA_FLAG_ATLAS_STYLE_MASK       = (0xFu << 20u);
// v2.5.15: 1 = interruptor del usuario APAGA la coordinación del Acoustic Unity Engine (contexto
// neutro). Semántica "_OFF": un snapshot por defecto (bit en 0) conserva el comportamiento previo.
constexpr uint32_t OMEGA_FLAG_UNITY_OFF              = (1u << 24);

// v2.5.49 — palabra de flags EXTENDIDA de 16 bits (snapshot.ext_flags16, antes _pad0). `flags` está agotado
// (bits 0..31 asignados) y _pad1 ya es temporal_field, así que los controles nuevos viajan aquí. Un snapshot de
// una versión anterior trae 0 = todo apagado/por defecto.
//   bits 0..3 = PUNCH (impacto predictivo, PunchEngine): nivel 0..15 -> cantidad 0..1; 0 = apagado (bypass exacto)
constexpr uint32_t OMEGA_EXT_PUNCH_SHIFT = 0u;
constexpr uint32_t OMEGA_EXT_PUNCH_MASK  = (0xFu << 0u);
inline uint32_t omega_punch_encode(float amount) noexcept {
    const float a = (amount == amount) ? amount : 0.f;
    int lv = static_cast<int>(a * 15.f + 0.5f); lv = lv < 0 ? 0 : (lv > 15 ? 15 : lv);
    return static_cast<uint32_t>(lv) << OMEGA_EXT_PUNCH_SHIFT;
}
inline float omega_punch_decode(uint32_t ext) noexcept {
    return static_cast<float>((ext & OMEGA_EXT_PUNCH_MASK) >> OMEGA_EXT_PUNCH_SHIFT) / 15.f;
}
// Cadena 4D en Ruta B (bits 4..15 de la misma palabra; 0 = todo apagado = identidad exacta):
//   bit 4 = 4D activo · bit 5 = arco narrativo · bits 6..8 = profundidad del arco (nivel/7)
//   bits 9..11 = profundidad de modulación (nivel/7) · bit 12 = diafonía (solo altavoces)
//   bits 13..15 = antelación, nivel 0..7 -> 5 + nivel*5/7 ms (5..10)
// La háptica y el dosímetro viven en la app (el demonio no tiene motor de vibración ni UI).
constexpr uint32_t OMEGA_EXT_4D_ON       = (1u << 4u);
constexpr uint32_t OMEGA_EXT_4D_ARC_ON   = (1u << 5u);
constexpr uint32_t OMEGA_EXT_4D_XTC_ON   = (1u << 12u);
constexpr uint32_t OMEGA_EXT_4D_MASK     = (0xFFFu << 4u);
struct Omega4DExt { bool on=false, arc=false, xtc=false; float arcDepth=0.f, depth=0.f, leadMs=7.5f; };
inline uint32_t omega_lv7_encode(float v) noexcept {
    const float a = (v == v) ? v : 0.f; int lv = static_cast<int>(a * 7.f + 0.5f);
    return static_cast<uint32_t>(lv < 0 ? 0 : (lv > 7 ? 7 : lv));
}
inline uint32_t omega_4d_encode(const Omega4DExt& e) noexcept {
    uint32_t w = 0u;
    if (e.on)  w |= OMEGA_EXT_4D_ON;
    if (e.arc) w |= OMEGA_EXT_4D_ARC_ON;
    if (e.xtc) w |= OMEGA_EXT_4D_XTC_ON;
    w |= omega_lv7_encode(e.arcDepth) << 6u;
    w |= omega_lv7_encode(e.depth) << 9u;
    const float l = (e.leadMs == e.leadMs) ? e.leadMs : 7.5f;
    int ll = static_cast<int>((l - 5.f) * (7.f / 5.f) + 0.5f); ll = ll < 0 ? 0 : (ll > 7 ? 7 : ll);
    w |= static_cast<uint32_t>(ll) << 13u;
    return w;
}
inline Omega4DExt omega_4d_decode(uint32_t ext) noexcept {
    Omega4DExt e;
    e.on = (ext & OMEGA_EXT_4D_ON) != 0u; e.arc = (ext & OMEGA_EXT_4D_ARC_ON) != 0u; e.xtc = (ext & OMEGA_EXT_4D_XTC_ON) != 0u;
    e.arcDepth = static_cast<float>((ext >> 6u) & 7u) / 7.f;
    e.depth    = static_cast<float>((ext >> 9u) & 7u) / 7.f;
    e.leadMs   = 5.f + static_cast<float>((ext >> 13u) & 7u) * (5.f / 7.f);
    return e;
}
// v8 — Distancia de la escena para la Ruta B (azimut/elevación viajan en head_yaw_rad/head_pitch_rad).
// 16 bits en npe_ctl bits 16..31 (el snapshot v8 estaba al límite de 512 B: sin campo nuevo;
// la v9 lo rompió para ORBE, 536 B). 0 = sin distancia publicada
// (el efecto no toca el nivel); 1..65535 mapean linealmente 0.5..2.0 (escala relativa al campo lejano de referencia).
// omega_npe_ctl_decode() solo mira los bits 0 y 8..15: compatible con lectores antiguos. No finito / fuera de rango -> 0.
constexpr uint32_t OMEGA_SCENE_DIST_SHIFT = 16u;
inline uint32_t omega_scene_distance_encode(float distance) noexcept {
    if (!(distance == distance) || distance < 0.4999f || distance > 2.0001f) return 0u;
    int w = static_cast<int>((distance - 0.5f) / 1.5f * 65534.f + 0.5f) + 1;
    return static_cast<uint32_t>(w < 1 ? 1 : (w > 65535 ? 65535 : w)) << OMEGA_SCENE_DIST_SHIFT;
}
// Devuelve 0 si no hay distancia publicada; si no, la distancia en 0.5..2.0.
inline float omega_scene_distance_decode(uint32_t npeCtl) noexcept {
    const uint32_t w = npeCtl >> OMEGA_SCENE_DIST_SHIFT;
    return w == 0u ? 0.f : 0.5f + static_cast<float>(w - 1u) * (1.5f / 65534.f);
}
// v2.5.36 — Modo AUX/altavoces (campo de presion coherente, AuxFieldCoherence.hpp). Cero cambio de ABI:
//   bit 25      = modo activo
//   bits 26..28 = crossover, paso 0..7  -> 60 + 20*paso Hz (60..200)
//   bits 29..31 = coherencia de graves, nivel 0..7 -> nivel/7 (0..1)
constexpr uint32_t OMEGA_FLAG_AUX_FIELD_ON           = (1u << 25);
constexpr uint32_t OMEGA_FLAG_AUX_XOVER_SHIFT        = 26u;
constexpr uint32_t OMEGA_FLAG_AUX_XOVER_MASK         = (0x7u << 26u);
constexpr uint32_t OMEGA_FLAG_AUX_COH_SHIFT          = 29u;
constexpr uint32_t OMEGA_FLAG_AUX_COH_MASK           = (0x7u << 29u);
// v2.5.44 — Dinámica espacial temporal ("4D", TemporalSpatialDynamics.hpp). Campo propio (no cabe en flags):
//   bit 0 = activo ; bits 1..3 = profundidad, nivel 0..7 -> nivel/7 (0..1)
//   bit 4 = modo adaptativo (profundidad según densidad de golpes; 0 en escritores antiguos = fija)
// bit 5 de la MISMA palabra = cancelación de diafonía para altavoces (CrosstalkCanceller en omega_effect). El snapshot está
// al tope de tamaño; los bits 5.. de temporal_field estaban libres y los lectores antiguos los ignoran (máscaras 1/0xE/16).
constexpr uint32_t OMEGA_TEMPORAL_FIELD_XTC_BIT = 32u;
// FotoHolo Route B occupies previously unused temporal_field bits 6..15.
// bit 6 = observer enabled; bits 7..15 = intensity quantized to 9 bits.
// Existing 4D/XTC readers mask only bits 0..5, so ABI v8 remains unchanged.
constexpr uint32_t OMEGA_FOTOHOLO_ON_BIT = (1u << 6u);
constexpr uint32_t OMEGA_FOTOHOLO_INTENSITY_SHIFT = 7u;
constexpr uint32_t OMEGA_FOTOHOLO_INTENSITY_MASK = (0x1FFu << OMEGA_FOTOHOLO_INTENSITY_SHIFT);
inline uint32_t omega_fotoholo_encode(bool enabled, float intensity) noexcept {
    const float v = std::isfinite(intensity) ? std::clamp(intensity, 0.0f, 1.0f) : 0.0f;
    const uint32_t q = static_cast<uint32_t>(v * 511.0f + 0.5f);
    return (enabled ? OMEGA_FOTOHOLO_ON_BIT : 0u) | (q << OMEGA_FOTOHOLO_INTENSITY_SHIFT);
}
inline void omega_fotoholo_decode(uint32_t field, bool& enabled, float& intensity) noexcept {
    enabled = (field & OMEGA_FOTOHOLO_ON_BIT) != 0u;
    intensity = static_cast<float>((field & OMEGA_FOTOHOLO_INTENSITY_MASK) >> OMEGA_FOTOHOLO_INTENSITY_SHIFT) / 511.0f;
}
// Route-B runtime feature controls use the still-reserved high half of temporal_field.
// Bits 0..15 remain owned by temporal dynamics, XTC and FotoHolo. Bits 18..19 are the
// output mode (FIX 1, bofo Genezi v2.5.465); bits 20..31 stay free. This is ABI-neutral:
// temporal_field is already a 32-bit field and all existing decoders mask only their own bits.
constexpr uint32_t OMEGA_EFFECT_HRTF_ENABLED_BIT = (1u << 16u);
constexpr uint32_t OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT = (1u << 17u);
constexpr uint32_t OMEGA_EFFECT_FEATURES_MASK =
    OMEGA_EFFECT_HRTF_ENABLED_BIT | OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT;
inline uint32_t omega_effect_features_encode(bool hrtfEnabled, bool adaptiveEnabled) noexcept {
    return (hrtfEnabled ? OMEGA_EFFECT_HRTF_ENABLED_BIT : 0u) |
           (adaptiveEnabled ? OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT : 0u);
}
inline void omega_effect_features_decode(uint32_t field, bool& hrtfEnabled, bool& adaptiveEnabled) noexcept {
    hrtfEnabled = (field & OMEGA_EFFECT_HRTF_ENABLED_BIT) != 0u;
    adaptiveEnabled = (field & OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT) != 0u;
}
static_assert((OMEGA_EFFECT_FEATURES_MASK & 0x0000FFFFu) == 0u,
              "Route-B feature flags must not overlap temporal/XTC/FotoHolo controls");
// FIX 1 (bofo Genezi, v2.5.465): modo de salida / transductor en Ruta B.
// CAUSA RAÍZ MEDIDA (vivo 2026-11-18): el snapshot trae ORBE=1.0, HOLOS=0.5
// y el efecto en el HAL los aplica al Genezi aunque la puerta de la app
// (ff6f706d, RouteBDeviceGate.kt) diga SPEAKERS: el efecto aplicaba el
// snapshot directo, sin ningún chequeo de dispositivo/modo. La app es la
// única que conoce el dispositivo (AudioManager + selector usbDacTarget
// persistido); el daemon root no tiene AudioManager y el efecto en el HAL
// tampoco puede preguntarlo. Por eso el modo viaja EN el snapshot, puesto
// por la app vía SET_PERCEPTUAL_STATE ("outputMode"), y el EFECTO lo hace
// cumplir en omega_apply_snapshot (omega_effect.cpp): con SPEAKERS la
// cadena de audífonos (ORBE/HOLOS/upmix/RIR/EQ) se neutraliza a 0 en el
// efecto, SIN reescribir el snapshot y SIN apagar ORBE/HOLOS globalmente
// (siguen activos para audífonos).
// Codificación: bits 18..19 de temporal_field (libres: temporal 0..4, XTC
// bit 5, FotoHolo 6..15, features HRTF/adaptive 16..17, resto libre).
// Sin cambio de ABI por este campo: reusa bits libres de temporal_field
// (no añade campos al struct); los lectores antiguos enmascaran solo sus bits y los
// escritores antiguos dejan 0 = UNKNOWN. UNKNOWN = fail-closed: el efecto
// conserva el comportamiento actual (aplicar el snapshot tal cual) y no
// deja el audio en silencio. SPEAKERS cubre Genezi (DAC USB JieLi,
// TYPE_USB_HEADSET con usbDacTarget=SPEAKERS) y bocina genérica: ambas
// reciben la cadena de audífonos neutralizada; lo que las distingue
// (kernel Volterra Genezi, XTC, Aux Field) sigue viajando por sus propios
// campos y NO lo toca esta puerta.
enum class OmegaOutputMode : uint32_t {
    UNKNOWN    = 0u,  // sin publicar: fail-closed, aplicar snapshot tal cual
    HEADPHONES = 1u,  // audífonos: cadena binaural del usuario, sin cambios
    SPEAKERS   = 2u,  // Genezi / bocinas: cadena de audífonos neutralizada
};
constexpr uint32_t OMEGA_OUTPUT_MODE_SHIFT = 18u;
constexpr uint32_t OMEGA_OUTPUT_MODE_MASK  = (0x3u << OMEGA_OUTPUT_MODE_SHIFT);
inline uint32_t omega_output_mode_encode(OmegaOutputMode mode) noexcept {
    const uint32_t v = (mode == OmegaOutputMode::HEADPHONES) ? 1u
                     : (mode == OmegaOutputMode::SPEAKERS)   ? 2u : 0u;
    return v << OMEGA_OUTPUT_MODE_SHIFT;
}
inline OmegaOutputMode omega_output_mode_decode(uint32_t field) noexcept {
    const uint32_t v = (field & OMEGA_OUTPUT_MODE_MASK) >> OMEGA_OUTPUT_MODE_SHIFT;
    if (v == 1u) return OmegaOutputMode::HEADPHONES;
    if (v == 2u) return OmegaOutputMode::SPEAKERS;
    return OmegaOutputMode::UNKNOWN;  // 0 y 3 (reservado) = desconocido
}
static_assert((OMEGA_OUTPUT_MODE_MASK & OMEGA_EFFECT_FEATURES_MASK) == 0u,
              "Output-mode bits must not overlap Route-B feature flags");
static_assert((OMEGA_OUTPUT_MODE_MASK & OMEGA_FOTOHOLO_INTENSITY_MASK) == 0u &&
              (OMEGA_OUTPUT_MODE_MASK & OMEGA_FOTOHOLO_ON_BIT) == 0u,
              "Output-mode bits must not overlap FotoHolo controls");
inline uint32_t omega_temporal_field_encode(bool on, float amount, bool autoMode) noexcept {
    const float a = (amount == amount) ? amount : 0.6f;
    int lv = static_cast<int>(a * 7.f + 0.5f); lv = lv < 0 ? 0 : (lv > 7 ? 7 : lv);
    return (on ? 1u : 0u) | (static_cast<uint32_t>(lv) << 1u) | (autoMode ? 16u : 0u);
}
inline uint32_t omega_temporal_field_encode(bool on, float amount) noexcept { return omega_temporal_field_encode(on, amount, false); }
inline void omega_temporal_field_decode(uint32_t field, bool& on, float& amount, bool& autoMode) noexcept {
    on = (field & 1u) != 0u;
    amount = static_cast<float>((field >> 1u) & 0x7u) / 7.f;
    autoMode = (field & 16u) != 0u;
}
inline void omega_temporal_field_decode(uint32_t field, bool& on, float& amount) noexcept {
    bool a; omega_temporal_field_decode(field, on, amount, a);
}
// NPE (Ruta B): palabra de control = bit 0 encendido ; bits 8..15 = master gain en dB enteros con signo (int8, -24..+12).
inline uint32_t omega_npe_ctl_encode(bool on, float masterDb) noexcept {
    const float m = (masterDb == masterDb) ? masterDb : 0.f;
    int d = static_cast<int>(m < 0.f ? m - 0.5f : m + 0.5f); d = d < -24 ? -24 : (d > 12 ? 12 : d);
    return (on ? 1u : 0u) | (static_cast<uint32_t>(static_cast<uint8_t>(static_cast<int8_t>(d))) << 8u);
}
inline void omega_npe_ctl_decode(uint32_t ctl, bool& on, float& masterDb) noexcept {
    on = (ctl & 1u) != 0u;
    masterDb = static_cast<float>(static_cast<int8_t>((ctl >> 8u) & 0xFFu));
}
inline uint32_t omega_aux_field_encode(bool on, float xoverHz, float coherence) noexcept {
    const float xo = (xoverHz == xoverHz) ? xoverHz : 120.f;
    const float co = (coherence == coherence) ? coherence : 1.f;
    int xs = static_cast<int>((xo - 60.f) / 20.f + 0.5f); xs = xs < 0 ? 0 : (xs > 7 ? 7 : xs);
    int cs = static_cast<int>(co * 7.f + 0.5f);           cs = cs < 0 ? 0 : (cs > 7 ? 7 : cs);
    return (on ? OMEGA_FLAG_AUX_FIELD_ON : 0u)
         | (static_cast<uint32_t>(xs) << OMEGA_FLAG_AUX_XOVER_SHIFT)
         | (static_cast<uint32_t>(cs) << OMEGA_FLAG_AUX_COH_SHIFT);
}
inline void omega_aux_field_decode(uint32_t flags, bool& on, float& xoverHz, float& coherence) noexcept {
    on = (flags & OMEGA_FLAG_AUX_FIELD_ON) != 0u;
    xoverHz   = 60.f + 20.f * static_cast<float>((flags & OMEGA_FLAG_AUX_XOVER_MASK) >> OMEGA_FLAG_AUX_XOVER_SHIFT);
    coherence = static_cast<float>((flags & OMEGA_FLAG_AUX_COH_MASK) >> OMEGA_FLAG_AUX_COH_SHIFT) / 7.f;
}

// CRC32 simple (tabla inline, no requiere zlib)
inline uint32_t omega_crc32(const void* data, size_t len) noexcept {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

struct OmegaDspSnapshot {
    // ── Identidad ────────────────────────────────────────────────────────────
    uint32_t magic;            // OMEGA_CTRL_MAGIC — valida que es nuestro struct
    uint16_t version;          // OMEGA_CTRL_VERSION — valida compatibilidad ABI
    uint16_t ext_flags16;      // v2.5.49: palabra extendida de 16 bits (antes _pad0: mismo offset/tamaño, sin cambio de ABI; 0 = por defecto)

    // ── Control de generación ─────────────────────────────────────────────────
    uint64_t generation;       // Monotónicamente creciente; 0 = nunca publicado
    uint64_t timestamp_ms;     // CLOCK_MONOTONIC en ms al momento de publicar

    // ── Route Arbiter ─────────────────────────────────────────────────────────
    int32_t  active_route;     // RouteMode cast a int32_t
    uint32_t temporal_field;   // v2.5.44: Dinámica espacial temporal (antes _pad1; 0 = apagado, compatible con escritores antiguos)
    uint64_t consumer_generation; // Última generation que omega_effect confirmó aplicar

    // ── Intensidad global ─────────────────────────────────────────────────────
    float    intensity;        // [0, 1]

    // ── EQ ISO 226 ────────────────────────────────────────────────────────────
    float    eq_gains[OMEGA_CTRL_EQ_BANDS]; // dB por banda
    float    listen_phon;
    float    ref_phon;

    // ── Estado perceptual ─────────────────────────────────────────────────────
    float    compressor;
    float    exciter_reduction;
    float    high_cut_hz;
    float    spatial_width;
    float    loudness_target;
    float    harmonic_gain;
    float    anti_dolby;

    // ── Estado adaptativo ─────────────────────────────────────────────────────
    float    target_gain;
    float    comp_amount;
    float    exc_red;

    // ── PF Engine (13 parámetros) ─────────────────────────────────────────────
    float    pf_params[13];

    // ── Route profile ─────────────────────────────────────────────────────────
    float    bass_boost_db;
    float    dialog_boost_db;
    float    widener_mult;

    // ── SAF ───────────────────────────────────────────────────────────────────
    float    saf_delta_energy;
    float    saf_metric_norm;
    float    saf_memory;
    float    saf_gain;
    // ABI v2: morph vector Φ_SAF^∞ para HRTF cross-process.
    // El optimizador escribe q[0..6] tras cada iteración; omega_effect
    // lo entrega a ObjectRenderer::setLatentParams(q) por bloque. Rango
    // típico [-1, 1] (normalizado por el PCA decoder); consumidores
    // deben clampear defensivamente.
    float    saf_q[OMEGA_CTRL_SAF_Q];
    uint32_t saf_q_valid;   // 1 = q[7] recén publicado; 0 = usar HRTF neutro

    // ── Sala RIR ──────────────────────────────────────────────────────────────
    // Selección y mezcla de la respuesta al impulso de sala (RirDataset).
    // room_rt60_s: RT60 objetivo en segundos [0.1, 6.0] — selector de sala.
    //   0.0 = sala desactivada (bypass del convolver RIR).
    // room_idx: índice de la sala seleccionada en el dataset (0..199).
    //   Calculado por RirDataset::findByRt60(room_rt60_s).
    // room_wet: mezcla wet/dry [0.0, 1.0].
    //   0.0 = solo señal seca, 1.0 = solo señal con reverberación.
    float    room_rt60_s;   // RT60 objetivo [0.1,6.0], 0 = bypass
    int32_t  room_idx;      // índice sala [-1 = ninguna, 0..199]
    float    room_wet;      // wet/dry [0,1]

    // ── Intelligent Upmixing (HOA) ────────────────────────────────────────────
    uint32_t upmixing_enabled; // 1 = enabled, 0 = disabled
    float    upmixing_immersivity;

    // ── Wave Field Synthesis (WFS) ─────────────────────────────────────────────
    // wfs_enabled: 1 = activo (crossfade smoothstep ~15-20ms hacia/desde el
    //   bypass, gestionado en WfsRenderer/IvannaFusionCore — nunca switch duro).
    // wfs_spread: escala de apertura de las fuentes primarias L/R [0, 2].
    // wfs_speaker_{x,y,z}: geometría real de 7 altavoces (orden fijo: FL, FR,
    //   SL, SR, TL, TR, SW), coordenadas de SALA en metros (x=ancho, y=altura,
    //   z=profundidad) — ver RoomGeometryConfig::defaultLayout() para los
    //   valores de referencia. omega_effect convierte a coordenadas relativas
    //   al oyente antes de pasarlas a WfsRenderer::setSpeakerLayout3D().
    uint32_t wfs_enabled;
    float    wfs_spread;
    float    wfs_speaker_x[OMEGA_CTRL_WFS_SPEAKERS];
    float    wfs_speaker_y[OMEGA_CTRL_WFS_SPEAKERS];
    float    wfs_speaker_z[OMEGA_CTRL_WFS_SPEAKERS];


    // ── Cochlear Active Inverse & Flags ─────────────────────────────────────
    float    cochlear_intensity; // Intensidad [0.0, 1.0]

    // ── 5 Ejes de Supremacía Cuántico-Neuromórfica (Prompt Maestro 2026) ────
    float    supreme_lattice_bl_drive;      // Eje 1: [0.0, 2.0]
    float    supreme_lattice_lambda;        // Eje 1: [-0.85, 0.85]
    float    supreme_cvnn_harmonic_gain;    // Eje 2: [0.0, 1.0]
    float    supreme_cvnn_imd_cancel;       // Eje 2: [0.0, 1.0]
    float    supreme_cvnn_tape_drive;       // Eje 2: [0.0, 1.0] Histéresis Cinta 2" Jiles-Atherton
    float    supreme_snn_immersivity;       // Eje 3: [0.0, 1.0]
    float    supreme_snn_spike_threshold;   // Eje 3: [0.15, 1.50]
    float    supreme_pinna_wet_mix;         // Eje 4: [0.0, 1.0]
    float    supreme_pinna_concha_depth;    // Eje 4: [-1.0, 1.0]
    float    supreme_pinna_helix_curl;      // Eje 4: [-1.0, 1.0]
    float    supreme_pinna_head_width;      // Eje 4: [-1.0, 1.0]
    float    supreme_mso_itd_ns;            // Eje 5: [-50000.0, 50000.0]

    // ── Flags ────────────────────────────────────────────────────────────────
    // bit 0: bypass global
    // bit 1: eq_calibrated
    // bit 2: perceptual_on
    // bit 3: anti_dolby_on
    uint32_t flags;

    // ── Telemetría de audio real (Ruta B) ────────────────────────────────────
    // Escritos por omega_effect.cpp en el proceso audioserver (cada bloque DSP).
    // Leídos por audioRouteBridgeLoop() en el proceso de la app vía OmegaControlBus.
    // Sin estos campos, audioRouteBridgeLoop() nunca detecta Ruta B activa aunque
    // sí esté procesando — por eso la UI marcaba "sin audio" con Ruta B viva.
    float    raw_rms;          // RMS del bloque actual [0, 1] — 0 = silencio
    float    raw_peak;         // Peak del bloque actual [0, 1]
    uint32_t supreme_declipped_peaks; // Picos reconstruidos por Master De-Clipper en Ruta B
    float    supreme_tape_mag;        // Magnetización Jiles-Atherton en Ruta B
    uint32_t supreme_snn_spikes;      // Espigas activas SNN INT8 en Ruta B
    float    supreme_subsample_delay; // Retardo inverso sub-muestra en Ruta B
    uint64_t effect_frames;    // Total de frames procesados por omega_effect (monotónico)

    // ── Corrección acústica de sala (v6) ─────────────────────────────────────
    // Ganancias (dB) por banda de octava (RoomProbe::kBandHz) medidas con el micrófono del teléfono. Todas en 0 =
    // apagado (bit-exacto). La aplica RoomCorrectionEq en omega_effect.
    float    room_corr_db[OMEGA_CTRL_EQ_BANDS];

    // ── Motor NPE neuromórfico en Ruta B (v7) ────────────────────────────────
    // Antes el NPE (NHO + LIF + banco coclear + inhibición lateral + OHC + AGC) solo existía en la app (Ruta A): con el
    // módulo instalado sus controles no tenían efecto. NpeStereoCore lo aplica en omega_effect. La ganancia armónica
    // reutiliza `harmonic_gain`. Apagado = bit-exacto. (Master gain dentro de la palabra para dejar margen < 512 B.)
    uint32_t npe_ctl;               // omega_npe_ctl_encode(): bit0 encendido, bits 8..15 master dB (int8)
    float    npe_lateral_inhib;     // [0,1]
    float    npe_ohc_compression;   // [0,1]

    // ABI v8: pose espacial publicada por el daemon; ángulos en radianes.
    float    head_yaw_rad;
    float    head_pitch_rad;

    // ── ORBE — Binaural Esférico de Campo Cercano (ABI v9) ───────────────────
    // amount 0 = apagado (bypass bit-exacto, default); distancia 0.15..2.0 m;
    // spread 0..1.5; proximidad 0..6 dB; difuso 0..1; head_tracking 1 = parallax
    // por yaw. Los lee omega_apply_snapshot (Ruta B) vía fc->setOrbeParams().
    float    orbe_amount;
    float    orbe_distance_m;
    float    orbe_spread;
    float    orbe_proximity_db;
    float    orbe_diffuse;
    uint32_t orbe_head_tracking;

    // ── HOLOS — Operador Único de Campo Acústico (ABI v10) ───────────────────
    // Los 13 parámetros de FUSION.md §6 + EQ estático de Γ (plano = no-op).
    // amount 0 = apagado (bypass bit-exacto, default); band_count 12 o 24
    // (24 = default; el tier térmico puede forzar 12). Los lee
    // omega_apply_snapshot (Ruta B) vía fc->setHolosParams()/setHolosEq().
    float    holos_amount;
    float    holos_distance_m;
    float    holos_spread;
    float    holos_proximity_db;
    float    holos_diffuse;
    float    holos_harmonics;
    float    holos_warmth;
    float    holos_dynamics;
    float    holos_width;
    float    holos_gamma;
    float    holos_room_rt60;
    uint32_t holos_head_tracking;
    uint32_t holos_band_count;
    float    holos_eq_low;
    float    holos_eq_mid;
    float    holos_eq_high;
    float    holos_eq_presence;
    float    holos_eq_tilt_db;
    float    holos_eq_freq;

    // ── Sesión real descubierta por el daemon root (ABI v11) ────────────────
    // El daemon (root) ejecuta `dumpsys media.audio_flinger` cada ~3 s y
    // publica aquí el sessionId REAL de la app que reproduce música. La app
    // (IvannaGlobalEffectManager) acopla el efecto omega a ESTA sesión en
    // lugar de a la sesión 0 (global), que en este Moto G85 falla en silencio
    // (el efecto se crea pero nunca recibe frames). 0 = sin sesión descubierta
    // (sin música o dumpsys fallido: fail-closed, la app sigue con su
    // comportamiento actual). gen = contador que incrementa SOLO cuando la
    // sesión descubierta cambia (0->N, N->M, N->0); la app lo usa para
    // detectar el cambio de sesión y re-acoplar. Al FINAL (antes de crc32)
    // para no desplazar ningún offset existente (effect_frames sigue en 440).
    int32_t  discovered_session_id;
    uint64_t discovered_session_gen;

    // ── FUSIÓN MAGISTRAL (ABI v12, 2026-11-11) ─────────────────────────────
    // El cerebro (daemon v2) publica aquí el estado de la fusión para que
    // la APK lo VEA y se ACOPLE: si la fusión está viva y procesando en el
    // HAL, la app lo sabe por este snapshot (el canal que ya lee) y no
    // procesa dos veces (Ruta A cede ante la fusión). Al FINAL (antes de
    // crc32) para no desplazar ningún offset existente.
    // fusion_active: 1 = cerebro + músculo vivos, veredicto LOADED (el HAL
    //   está procesando TODO el audio); 0 = fusión inactiva / bypass.
    // fusion_eta: η del campo Ψ [0,1] — se adapta a tu cerebro (Ec. 4).
    // fusion_psi_gamma: Γ(η) = 1 + γ·(η−η̄) — ganancia del campo.
    // fusion_psi_active: 1 = el campo Ψ está aplicado (FIR 64 taps).
    // fusion_asrc_verdict: 0=LOADED, 1=BYPASS, 2=STALE, 3=NO_CLOCK, 4=OFF.
    // fusion_asrc_ratio: ratio que aplica el músculo (1.0 = sin corrección).
    // fusion_asrc_drift_ppm: deriva medida del reloj en ppm.
    // fusion_asrc_clock: 0=MMAP, 1=WRITE_TIMING, 2=NONE.
    // fusion_generation: generación del estado de fusión (cambia al actualizar).
    uint32_t fusion_active;
    float    fusion_eta;
    float    fusion_psi_gamma;
    uint32_t fusion_psi_active;
    int32_t  fusion_asrc_verdict;
    float    fusion_asrc_ratio;
    float    fusion_asrc_drift_ppm;
    int32_t  fusion_asrc_clock;
    uint64_t fusion_generation;

    // ── Φ (HyperState) + Volterra — Paridad Ruta B (ABI v13, 0038+0040) ───
    // Φ (g_hyper_amount) y Volterra no viajaban en SET_PERCEPTUAL_STATE:
    // Ruta B no podía inyectar la cadena completa (ORBE+HOLOS+Φ+Volterra).
    // Al FINAL (antes de crc32) para no desplazar ningún offset existente.
    // hyper_amount: [0,1] (0 = apagado, bypass bit-exacto).
    // hyper_preset: 0..2 (preset del operador Φ).
    // hyper_xtc_enabled: 1 = cancelador de crosstalk activo.
    // volterra_enabled: 1 = Volterra H2 activo (el daemon sincroniza el
    //   bit OMEGA_FLAG_VOLTERRA_ON en flags; el efecto lo lee en
    //   omega_process para gobernar ctx->volterraEngine).
    float    hyper_amount;
    int32_t  hyper_preset;
    uint32_t hyper_xtc_enabled;
    uint32_t volterra_enabled;

    // ── FÍSICA NUEVA — NewPhysicsControls (ABI v14, Fase 2, v2.5.472) ────
    // Los 17 controles de new_physics_controls.hpp (LGN 3D + Ω·Σ +
    // Volterra Laguerre + lazo variacional F + ABX), en el MISMO ORDEN que
    // los atomics g_np_* (new_physics_controls_bridge.cpp y
    // new_physics_globals_effect.cpp). Al FINAL (antes de crc32) para no
    // desplazar ningún offset existente. np_amount 0 = apagado (bypass
    // bit-exacto, default); el resto lleva los defaults de los atomics
    // (ver makeDefault). Los lee omega_apply_snapshot (Ruta B) vía
    // omega_forward_snapshot_new_physics → atomics g_np_* →
    // omega_apply_new_physics → NewPhysicsEngine::setControls.
    // np_eq_class: EquipmentClass 0..6 (0 audífonos, 3 Genezi PZ1D).
    // np_abx_mode: 0 normal, 1 forzar A (bypass), 2 forzar B (Física Nueva).
    float    np_amount;
    float    np_lgn_mix;
    float    np_bs_damp;
    float    np_torso_mix;
    float    np_head_radius_m;
    float    np_az_deg;
    float    np_el_deg;
    int32_t  np_eq_class;
    float    np_omega_sig_mix;
    float    np_boost_cap_db;
    float    np_xtalk_mix;
    float    np_sigma_dist_m;
    float    np_spl_db;
    float    np_air_mix;
    float    np_volterra_mix;
    float    np_free_energy_mix;
    int32_t  np_abx_mode;

    // ── BANDAS ESPECTRALES Ruta B (ABI v15, v2.5.476) ─────────────────────
    // Energías RMS por banda Gammatone (low ~120 Hz, mid ~1500 Hz, high
    // ~8000 Hz) medidas por RtBandMeter en omega_process. Las publica el
    // efecto en cada bloque; las lee audioRouteBridgeLoop() para alimentar
    // nativeGetAudioCharacteristics() (centroide/tonalidad/DYN de la SAF).
    // Sin estos campos el puente ponía band_energy_valid=false y las bandas
    // quedaban en 0 → centroide 0 Hz con música sonando por Ruta B.
    // Al FINAL (antes de crc32) para no desplazar ningún offset existente.
    float    raw_band_low;      // Energía RMS banda baja [0, 1]
    float    raw_band_mid;      // Energía RMS banda media [0, 1]
    float    raw_band_high;     // Energía RMS banda alta [0, 1]
    float    raw_band_spread;   // Dispersión espectral (Hz) — 4º float para
                                // alinear sizeof a múltiplo de 8 (768 B)

    // ── SPSC + YAMNET (ABI v16, v2.5.486) ─────────────────────────────────
    // SET_SPSC_RING_FACTOR y SET_YAMNET_SCORES no tenían slot en el
    // snapshot (el servidor viejo los guardaba en su estado pero nunca los
    // publicaba: el factor SPSC solo vivía en la Ruta A por JNI y los
    // scores YAMNet solo en el SHM OmegaSharedState que el daemon v2 no
    // mapea). Al FINAL (antes de crc32) para no desplazar ningún offset
    // existente. reserved_v16 completa el múltiplo de 8 (sizeof 792).
    // spsc_ring_factor: [0.5, 2.0], 1.0 = neutro (default JNI 1.0; la app
    //   persiste 0.95 y lo re-envía al restaurar).
    // yamnet_speech / yamnet_music: probabilidad [0, 1] de voz / música.
    // yamnet_class_id: clase YAMNet ganadora 0..520, -1 = desconocida.
    // yamnet_confidence: probabilidad [0, 1] de la clase ganadora.
    float    spsc_ring_factor;
    float    yamnet_speech;
    float    yamnet_music;
    int32_t  yamnet_class_id;
    float    yamnet_confidence;
    uint32_t reserved_v16;     // reservado (0); mantiene sizeof múltiplo de 8

    // ── CONSERVACIÓN PERCEPTUAL (ABI v17, 2026-11-21) ─────────────────────
    // El árbitro de campo (fused/perceptual_conservation.hpp) en Ruta B:
    // los 4 controles de ConservationControls. Al FINAL (antes de crc32)
    // para no desplazar ningún offset existente. sizeof 792 -> 808.
    // consv_amount: [0,1] (0 = apagado, bypass bit-exacto, default).
    // consv_energy_cap: [0,1] (techo de energía por banda, 1 = sin techo).
    // consv_phase_coherence: [0,1] (suavidad del cambio de ganancia).
    // consv_inertia: [0,0.99] (continuidad temporal del estado de campo).
    float    consv_amount;
    float    consv_energy_cap;
    float    consv_phase_coherence;
    float    consv_inertia;

    // ── Integridad ────────────────────────────────────────────────────────────
    uint32_t crc32;            // CRC32 de bytes [magic .. crc32) (todo el struct salvo crc32)

    // ── Helpers ───────────────────────────────────────────────────────────────

    static OmegaDspSnapshot makeDefault() noexcept {
        OmegaDspSnapshot s{};
        s.magic      = OMEGA_CTRL_MAGIC;
        s.version    = OMEGA_CTRL_VERSION;
        s.generation = 0;
        s.active_route = static_cast<int32_t>(RouteMode::SYSTEM_WIDE);
        s.intensity  = 0.92f;
        s.listen_phon = 62.f;
        s.ref_phon    = 82.f;
        {
            // FIX (v2.5.400, retumbo 2026-11-11): el default del daemon
            // DEBE ser neutro. La curva "Magistral" subía +2.4/+2.0/+1.4 dB
            // en 31.5/63/125 Hz y el módulo sonaba inundado de bajos en
            // cada arranque, sin abrir la app y sin que ningún slider lo
            // pudiera quitar (ORBE/HOLOS ya estaban en 0). EQ plano por
            // defecto; la curva Magistral es preset opt-in en la UI.
            constexpr float kMagistralEq[OMEGA_CTRL_EQ_BANDS] = {
                0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f
            };
            for (int i = 0; i < OMEGA_CTRL_EQ_BANDS; ++i) s.eq_gains[i] = kMagistralEq[i];
        }
        {
            constexpr float kMagistralPf[13] = {
                0.48f, 0.38f, 0.72f, 0.35f, 0.08f, 0.72f, 1000.f, 0.707f,
                2.2f, 0.8f, 1.8f, 1.6f, 0.8f
            };
            for (int i = 0; i < 13; ++i) s.pf_params[i] = kMagistralPf[i];
        }
        s.compressor        = -5.5f;
        s.exciter_reduction = 0.15f;
        s.high_cut_hz       = 19500.f;
        s.spatial_width     = 1.55f;
        s.loudness_target   = -16.f;
        s.harmonic_gain     = 0.78f;
        s.anti_dolby        = 0.85f;
        s.target_gain       = 1.0f;
        s.comp_amount       = 0.22f;
        s.exc_red           = 0.15f;
        // FIX (v2.5.400, retumbo): sin realce de graves por defecto.
        // bass 2.5 + dialog 1.5 daban trim global +1.4 dB y, sumado al EQ
        // Magistral, inundaban de bajos cada arranque. 0 por defecto;
        // el perfil de ruta lo sube solo si la ruta lo pide (AUX/BT).
        s.bass_boost_db     = 0.0f;
        s.dialog_boost_db   = 0.0f;
        s.widener_mult      = 1.38f;
        // FIX (v2.5.400, retumbo): NPE apagado por defecto. Es un motor
        // pesado y su coloración sumaba al retumbo del arranque. Opt-in
        // desde la UI; el default debe ser neutro.
        s.npe_ctl = omega_npe_ctl_encode(false, 0.0f); s.npe_lateral_inhib = 0.2f; s.npe_ohc_compression = 0.3f;
        s.head_yaw_rad = 0.f; s.head_pitch_rad = 0.f;
        // ORBE (ABI v9): apagado por defecto — amount 0 = bypass bit-exacto.
        s.orbe_amount = 0.f; s.orbe_distance_m = 0.35f; s.orbe_spread = 1.0f;
        s.orbe_proximity_db = 3.0f; s.orbe_diffuse = 0.5f; s.orbe_head_tracking = 1u;
        // HOLOS (ABI v10): apagado por defecto — amount 0 = bypass bit-exacto.
        // El resto son los defaults de FUSION.md §6; EQ estático plano (no-op).
        s.holos_amount = 0.f; s.holos_distance_m = 0.35f; s.holos_spread = 1.0f;
        s.holos_proximity_db = 3.0f; s.holos_diffuse = 0.5f;
        s.holos_harmonics = 0.32f; s.holos_warmth = 0.5f; s.holos_dynamics = 1.0f;
        s.holos_width = 1.0f; s.holos_gamma = 1.0f; s.holos_room_rt60 = 0.34f;
        s.holos_head_tracking = 1u; s.holos_band_count = 24u;
        s.holos_eq_low = 0.f; s.holos_eq_mid = 0.f; s.holos_eq_high = 0.f;
        s.holos_eq_presence = 0.f; s.holos_eq_tilt_db = 0.f; s.holos_eq_freq = 1000.f;
        s.room_rt60_s = 0.34f;  // Golden Master Studio Control Room (rir_0051.wav, RT60=0.340s, DRR=10.31dB)
        s.room_idx    = 51;
        s.room_wet    = 0.22f;
        s.upmixing_enabled = 0;
        s.upmixing_immersivity = 1.0f;
        s.wfs_enabled = 0;      // opt-in, coherente con ParameterStore default
        s.wfs_spread  = 1.0f;
        s.cochlear_intensity = 0.35f;  // FIX artefactos: era 1.0f. Cochlear arranca OFF en el bus;
                                       // el usuario lo activa desde el panel (SpatialAudioPrefs).
                                       // Cuando se activa, 0.35 es perceptible sin coloring agresivo.
        // v2.5.481 (Esse, tronidito en burbujita en cola de tambor):
        // los saturadores supremos arrancan en 0. El tape 0.25, el harm
        // 0.28 y el lattice 1.0/0.72 metían un crackle suave en la cola
        // de cada golpe de tambor (aislamiento completo: NP/ORBE/HOLOS/
        // comp/AntiDolby en 0 y el tronidito seguía). Con los drives en
        // 0 los ejes quedan inertes por defecto; se encienden por
        // SET_SUPREME_AXES (handler completo en el daemon, v2.5.481).
        s.supreme_lattice_bl_drive    = 0.0f;
        s.supreme_lattice_lambda      = 0.0f;
        s.supreme_cvnn_harmonic_gain  = 0.0f;
        s.supreme_cvnn_imd_cancel     = 0.85f;
        s.supreme_cvnn_tape_drive     = 0.0f;
        s.supreme_snn_immersivity     = 0.65f;
        s.supreme_snn_spike_threshold = 0.45f;
        s.supreme_pinna_wet_mix       = 0.62f;
        s.supreme_pinna_concha_depth  = 0.163986f;
        s.supreme_pinna_helix_curl    = 0.119832f;
        s.supreme_pinna_head_width    = 0.138745f;
        s.supreme_mso_itd_ns          = 0.0f;
        {
            // RoomGeometryConfig::defaultLayout() (orden FL,FR,SL,SR,TL,TR,SW).
            // Duplicado aquí como literales (no como dependencia de
            // WfsRenderer/spatial) para mantener este header autocontenido.
            constexpr float kX[OMEGA_CTRL_WFS_SPEAKERS] = {0.565f, 3.265f, 0.00f, 3.83f, 0.25f, 3.58f, 3.00f};
            constexpr float kY[OMEGA_CTRL_WFS_SPEAKERS] = {1.50f, 1.50f, 1.80f, 1.80f, 2.83f, 2.83f, 0.00f};
            constexpr float kZ[OMEGA_CTRL_WFS_SPEAKERS] = {0.00f, 0.00f, 3.50f, 3.50f, 3.50f, 3.50f, 3.00f};
            for (int i = 0; i < OMEGA_CTRL_WFS_SPEAKERS; ++i) {
                s.wfs_speaker_x[i] = kX[i]; s.wfs_speaker_y[i] = kY[i]; s.wfs_speaker_z[i] = kZ[i];
            }
        }
        s.saf_gain        = 0.78f;
        // Golden Master SOFA-SAF 7-D Latent Vector (214-subject manifold + 12 IHR1 anchors)
        constexpr float kGoldenSafQ[OMEGA_CTRL_SAF_Q] = {
            4.00711025e-03f, 0.00000000e+00f, 5.49971378e-03f, 4.70094676e-03f,
            -3.88360593e-03f, 4.82163913e-03f, 9.68858446e-03f
        };
        for (int i = 0; i < OMEGA_CTRL_SAF_Q; ++i) s.saf_q[i] = kGoldenSafQ[i];
        s.saf_q_valid     = 1u;
        s.consumer_generation = 0;
        // ABI v11: sin sesión descubierta por defecto (fail-closed).
        s.discovered_session_id = 0;
        s.discovered_session_gen = 0;
        // ABI v12: fusión inactiva por defecto (fail-closed). El cerebro
        // (daemon v2) la enciende al publicar el snapshot con el estado real.
        s.fusion_active = 0u;
        s.fusion_eta = 0.0f;
        s.fusion_psi_gamma = 1.0f;
        s.fusion_psi_active = 0u;
        s.fusion_asrc_verdict = 1;  // BYPASS
        s.fusion_asrc_ratio = 1.0f;
        s.fusion_asrc_drift_ppm = 0.0f;
        s.fusion_asrc_clock = 2;    // NONE
        s.fusion_generation = 0ULL;
        // ABI v14: Física Nueva apagada por defecto (np_amount 0 = bypass
        // bit-exacto, fail-closed). El resto son los defaults de los
        // atomics g_np_* (new_physics_controls_bridge.cpp) y de
        // NewPhysicsControls::clamp() (fused/new_physics_controls.hpp):
        // la app los enciende con SET_PERCEPTUAL_STATE cuando el usuario
        // activa la tarjeta CONTROL.
        s.np_amount = 0.0f;
        s.np_lgn_mix = 1.0f;
        s.np_bs_damp = 0.65f;
        s.np_torso_mix = 0.18f;
        s.np_head_radius_m = 0.0875f;
        s.np_az_deg = 0.0f;
        s.np_el_deg = 0.0f;
        s.np_eq_class = 0;
        s.np_omega_sig_mix = 0.0f;
        s.np_boost_cap_db = 3.0f;
        s.np_xtalk_mix = 0.0f;
        s.np_sigma_dist_m = 0.35f;
        s.np_spl_db = 85.0f;
        s.np_air_mix = 0.0f;
        s.np_volterra_mix = 0.0f;
        s.np_free_energy_mix = 0.0f;
        s.np_abx_mode = 0;
        // ABI v16: SPSC neutro (1.0); YAMNet sin clasificar (speech/music/
        // confidence 0 por value-init; class_id -1 = desconocida, no 0).
        s.spsc_ring_factor = 1.0f;
        s.yamnet_class_id = -1;
        s.reserved_v16 = 0u;
        // ABI v17: conservación perceptual APAGADA por defecto (amount 0 =
        // bypass bit-exacto); el resto lleva los defaults de ConservationControls.
        s.consv_amount = 0.0f;
        s.consv_energy_cap = 1.0f;
        s.consv_phase_coherence = 0.5f;
        s.consv_inertia = 0.82f;
        // FIX (v2.5.400, retumbo 2026-11-11): default neutro y fail-closed.
        // Antes el daemon publicaba en cada arranque Volterra + los 5 Ejes
        // Supremos + Reality Recon, ~10 motores encendidos que coloreaban
        // el audio y sumaban al retumbo, sin que ningún slider de la UI
        // los apagara (ORBE/HOLOS ya estaban en 0). Ahora son opt-in:
        // la app los enciende con SET cuando el usuario los activa.
        // Solo quedan los flags seguros (EQ calibrado, perceptual,
        // anti-Dolby, ATI). OMEGA_FLAG_COCHLEAR_ON ya era opt-in.
        s.flags = OMEGA_FLAG_EQ_CALIBRATED
                | OMEGA_FLAG_PERCEPTUAL_ON
                | OMEGA_FLAG_ANTI_DOLBY_ON
                | OMEGA_FLAG_ATI_ON;
        s.raw_rms      = 0.0f;
        s.raw_peak     = 0.0f;
        s.supreme_declipped_peaks = 0u;
        s.supreme_tape_mag        = 0.0f;
        s.supreme_snn_spikes      = 2u;
        s.supreme_subsample_delay = 0.24f;
        s.effect_frames = 0ULL;
        // Fail closed for adaptive actuation until the app restores the user's
        // persisted preference with SET_AUDIO_FEATURES. HRTF retains its
        // historical default, while an absent/unavailable control plane must
        // never silently re-enable a user-disabled adaptive engine.
        s.temporal_field |= omega_effect_features_encode(true, false);
        s.stampCrc();
        return s;
    }

    // ── Snapshot TRULAMENTE neutro (0023, bofo Ruta B) ─────────────────────
    // CAUSA RAÍZ: makeDefault() es el default MAGISTRAL de la app (Ruta A),
    // no un neutro. El daemon v2 lo heredaba (buildNeutralSnapshot) y solo
    // ponía a cero EQ/bajos/sala/upmix/compresor/harmonic — dejaba VIVOS:
    //   * bit HRTF en temporal_field -> omega_apply_snapshot hacía
    //     fc->setHrtfEnabled(true) y el FusionCore corría processBinauralScene
    //     con wet 1.0 (m_hrtfEnabled_{true}, g_hrtf_wet_dry{1.0f}):
    //     convolución HRTF COMPLETA sobre música estéreo. La HRTF es para
    //     audífonos; por altavoces degrada imagen y timbre = el "bofo".
    //   * widener: spatial_width 1.55 x widener_mult 1.38 = 2.14x M/S activo.
    //   * intensity 0.92 -> trim global 0.92 (-0.7 dB).
    //   * anti_dolby 0.85, saf_gain 0.78 + saf_q_valid 1, phonDelta +1 dB
    //     (listen 62 / ref 82), cochlear 0.35.
    // makeNeutral() es la identidad acústica: todo lo que colorea queda en
    // su valor identidad (verificado por test_route_b_neutral_snapshot).
    // Los daemons (v2 fusión + app) publican ESTE snapshot como base; los
    // efectos del usuario llegan ENCIMA por SET_PERCEPTUAL_STATE / snapshot
    // de la app. makeDefault() NO cambia: sigue siendo el default de Ruta A.
    static OmegaDspSnapshot makeNeutral() noexcept {
        OmegaDspSnapshot s = makeDefault();
        s.intensity = 1.0f;
        s.listen_phon = 82.f; s.ref_phon = 82.f;  // phonDelta 0 (sin trim)
        s.spatial_width = 1.0f;
        s.widener_mult = 1.0f;
        s.harmonic_gain = 0.0f;
        s.anti_dolby = 0.0f;
        s.compressor = 0.0f; s.comp_amount = 0.0f;  // >= 0: apply_snapshot lo salta; ratio del core 1.0 = off
        s.exciter_reduction = 0.0f; s.exc_red = 0.0f;
        s.target_gain = 1.0f; s.loudness_target = 0.0f;
        s.bass_boost_db = 0.0f; s.dialog_boost_db = 0.0f;
        s.room_rt60_s = 0.0f; s.room_wet = 0.0f; s.room_idx = -1;
        s.upmixing_enabled = 0; s.upmixing_immersivity = 0.0f;
        s.wfs_enabled = 0;
        s.orbe_amount = 0.f; s.holos_amount = 0.f;
        // Φ + Volterra (ABI v13): apagados por defecto — amount 0 = bypass
        // bit-exacto; el flag VOLTERRA_ON tampoco va en flags (ver abajo).
        s.hyper_amount = 0.f; s.hyper_preset = 0; s.hyper_xtc_enabled = 0u;
        s.volterra_enabled = 0u;
        // Física Nueva (ABI v14): apagada por defecto — np_amount 0 =
        // bypass bit-exacto. El resto lleva los mismos defaults que
        // makeDefault (explícito, como Φ/Volterra arriba): un neutro no
        // hereda valores de una tarjeta que el usuario no encendió.
        s.np_amount = 0.0f;
        s.np_lgn_mix = 1.0f;
        s.np_bs_damp = 0.65f;
        s.np_torso_mix = 0.18f;
        s.np_head_radius_m = 0.0875f;
        s.np_az_deg = 0.0f;
        s.np_el_deg = 0.0f;
        s.np_eq_class = 0;
        s.np_omega_sig_mix = 0.0f;
        s.np_boost_cap_db = 3.0f;
        s.np_xtalk_mix = 0.0f;
        s.np_sigma_dist_m = 0.35f;
        s.np_spl_db = 85.0f;
        s.np_air_mix = 0.0f;
        s.np_volterra_mix = 0.0f;
        s.np_free_energy_mix = 0.0f;
        s.np_abx_mode = 0;
        // ABI v16: neutro = SPSC 1.0 (sin anillo) y YAMNet sin clasificar.
        s.spsc_ring_factor = 1.0f;
        s.yamnet_class_id = -1;
        s.reserved_v16 = 0u;
        // ABI v17: neutro = conservación apagada (amount 0 = bypass).
        s.consv_amount = 0.0f;
        s.consv_energy_cap = 1.0f;
        s.consv_phase_coherence = 0.5f;
        s.consv_inertia = 0.82f;
        s.cochlear_intensity = 0.0f;
        s.high_cut_hz = 24000.f;
        s.saf_gain = 1.0f; s.saf_q_valid = 0u;
        // Solo el flag seguro: sin PERCEPTUAL_ON / ANTI_DOLBY_ON / ATI_ON
        // (no-ops en el core, pero un neutro no los anuncia) y sin ningún
        // flag SUPREME (los 5 ejes quedan apagados).
        s.flags = OMEGA_FLAG_EQ_CALIBRATED;
        // HRTF OFF y adaptativo OFF: el efecto no enciende el render binaural.
        s.temporal_field = omega_effect_features_encode(false, false);
        s.stampCrc();
        return s;
    }

    // Calcula y escribe el CRC sobre todos los campos excepto crc32 mismo
    void stampCrc() noexcept {
        crc32 = omega_crc32(this, offsetof(OmegaDspSnapshot, crc32));
    }

    bool isMagicValid()   const noexcept { return magic == OMEGA_CTRL_MAGIC; }
    bool isVersionValid() const noexcept { return version == OMEGA_CTRL_VERSION; }
    bool isCrcValid()     const noexcept {
        return crc32 == omega_crc32(this, offsetof(OmegaDspSnapshot, crc32));
    }
    bool isValid() const noexcept {
        return isMagicValid() && isVersionValid() && isCrcValid();
    }

    RouteMode route() const noexcept {
        return static_cast<RouteMode>(active_route);
    }
};

// Validar tamaño en tiempo de compilación (host + ARM64)
static_assert(sizeof(OmegaDspSnapshot) <= 808,
    "OmegaDspSnapshot supera 808 bytes — revisar campos o padding");
static_assert(std::is_trivially_copyable<OmegaDspSnapshot>::value,
    "OmegaDspSnapshot debe ser trivialmente copiable (POD) para seqlock + mmap");

// ABI v13 (Φ + Volterra 0038+0040, 2026-11-17): fijar el tamaño exacto y los
// offsets que el teléfono lee con `od` (effect_frames byte 448, raw_rms/raw_peak byte 420).
// Si alguien mueve un campo, esto rompe en compilación en vez de en silencio.
// v13 añade Φ (HyperState) + Volterra al FINAL (antes de crc32): 16 bytes.
// v12 era 672 (crc32 en 664); v13 es 688 (crc32 en 680).
// ABI v15 (bandas SAF, 2026-11-19): añade los 4 campos raw_band_* al FINAL
// (antes de crc32): 16 bytes (4×4). v14 era 752 (crc32 en 748); v15 es 768
// (crc32 en 764). raw_band_low en 748, raw_band_mid en 752,
// raw_band_high en 756, raw_band_spread en 760.
// ABI v16 (SPSC + YAMNet, 2026-11-20): añade 6 campos al FINAL (antes de
// crc32): 24 bytes (6×4). v15 era 768 (crc32 en 764); v16 es 792
// (crc32 en 788). spsc_ring_factor en 764, yamnet_speech en 768,
// yamnet_music en 772, yamnet_class_id en 776, yamnet_confidence en 780,
// reserved_v16 en 784 (completa el múltiplo de 8).
// ABI v17 (conservación perceptual, 2026-11-21): añade 4 campos al FINAL
// (antes de crc32): 16 bytes (4×4). v16 era 792 (crc32 en 788); v17 es 808
// (crc32 en 804). consv_amount en 788, consv_energy_cap en 792,
// consv_phase_coherence en 796, consv_inertia en 800.
static_assert(sizeof(OmegaDspSnapshot) == 808, "ABI v17: sizeof debe ser 808");
static_assert(offsetof(OmegaDspSnapshot, effect_frames) == 440, "ABI: effect_frames se movió");
static_assert(offsetof(OmegaDspSnapshot, raw_rms) == 412, "ABI: raw_rms se movió");
static_assert(offsetof(OmegaDspSnapshot, raw_peak) == 416, "ABI: raw_peak se movió");
static_assert(offsetof(OmegaDspSnapshot, consumer_generation) == 32, "ABI: consumer_generation se movió");
static_assert(offsetof(OmegaDspSnapshot, discovered_session_id) == 608, "ABI v11: discovered_session_id");
static_assert(offsetof(OmegaDspSnapshot, discovered_session_gen) == 616, "ABI v11: discovered_session_gen");
static_assert(offsetof(OmegaDspSnapshot, fusion_active) == 624, "ABI v12: fusion_active");
static_assert(offsetof(OmegaDspSnapshot, fusion_generation) == 656, "ABI v12: fusion_generation");
static_assert(offsetof(OmegaDspSnapshot, hyper_amount) == 664, "ABI v13: hyper_amount");
static_assert(offsetof(OmegaDspSnapshot, hyper_preset) == 668, "ABI v13: hyper_preset");
static_assert(offsetof(OmegaDspSnapshot, hyper_xtc_enabled) == 672, "ABI v13: hyper_xtc_enabled");
static_assert(offsetof(OmegaDspSnapshot, volterra_enabled) == 676, "ABI v13: volterra_enabled");
static_assert(offsetof(OmegaDspSnapshot, np_amount) == 680, "ABI v14: np_amount");
static_assert(offsetof(OmegaDspSnapshot, np_lgn_mix) == 684, "ABI v14: np_lgn_mix");
static_assert(offsetof(OmegaDspSnapshot, np_bs_damp) == 688, "ABI v14: np_bs_damp");
static_assert(offsetof(OmegaDspSnapshot, np_torso_mix) == 692, "ABI v14: np_torso_mix");
static_assert(offsetof(OmegaDspSnapshot, np_head_radius_m) == 696, "ABI v14: np_head_radius_m");
static_assert(offsetof(OmegaDspSnapshot, np_az_deg) == 700, "ABI v14: np_az_deg");
static_assert(offsetof(OmegaDspSnapshot, np_el_deg) == 704, "ABI v14: np_el_deg");
static_assert(offsetof(OmegaDspSnapshot, np_eq_class) == 708, "ABI v14: np_eq_class");
static_assert(offsetof(OmegaDspSnapshot, np_omega_sig_mix) == 712, "ABI v14: np_omega_sig_mix");
static_assert(offsetof(OmegaDspSnapshot, np_boost_cap_db) == 716, "ABI v14: np_boost_cap_db");
static_assert(offsetof(OmegaDspSnapshot, np_xtalk_mix) == 720, "ABI v14: np_xtalk_mix");
static_assert(offsetof(OmegaDspSnapshot, np_sigma_dist_m) == 724, "ABI v14: np_sigma_dist_m");
static_assert(offsetof(OmegaDspSnapshot, np_spl_db) == 728, "ABI v14: np_spl_db");
static_assert(offsetof(OmegaDspSnapshot, np_air_mix) == 732, "ABI v14: np_air_mix");
static_assert(offsetof(OmegaDspSnapshot, np_volterra_mix) == 736, "ABI v14: np_volterra_mix");
static_assert(offsetof(OmegaDspSnapshot, np_free_energy_mix) == 740, "ABI v14: np_free_energy_mix");
static_assert(offsetof(OmegaDspSnapshot, np_abx_mode) == 744, "ABI v14: np_abx_mode");
static_assert(offsetof(OmegaDspSnapshot, raw_band_low) == 748, "ABI v15: raw_band_low");
static_assert(offsetof(OmegaDspSnapshot, raw_band_mid) == 752, "ABI v15: raw_band_mid");
static_assert(offsetof(OmegaDspSnapshot, raw_band_high) == 756, "ABI v15: raw_band_high");
static_assert(offsetof(OmegaDspSnapshot, raw_band_spread) == 760, "ABI v15: raw_band_spread");
static_assert(offsetof(OmegaDspSnapshot, spsc_ring_factor) == 764, "ABI v16: spsc_ring_factor");
static_assert(offsetof(OmegaDspSnapshot, yamnet_speech) == 768, "ABI v16: yamnet_speech");
static_assert(offsetof(OmegaDspSnapshot, yamnet_music) == 772, "ABI v16: yamnet_music");
static_assert(offsetof(OmegaDspSnapshot, yamnet_class_id) == 776, "ABI v16: yamnet_class_id");
static_assert(offsetof(OmegaDspSnapshot, yamnet_confidence) == 780, "ABI v16: yamnet_confidence");
static_assert(offsetof(OmegaDspSnapshot, reserved_v16) == 784, "ABI v16: reserved_v16");
static_assert(offsetof(OmegaDspSnapshot, consv_amount) == 788, "ABI v17: consv_amount");
static_assert(offsetof(OmegaDspSnapshot, consv_energy_cap) == 792, "ABI v17: consv_energy_cap");
static_assert(offsetof(OmegaDspSnapshot, consv_phase_coherence) == 796, "ABI v17: consv_phase_coherence");
static_assert(offsetof(OmegaDspSnapshot, consv_inertia) == 800, "ABI v17: consv_inertia");
static_assert(offsetof(OmegaDspSnapshot, crc32) == 804, "ABI v17: crc32");


// ══════════════════════════════════════════════════════════════════════════════
// SharedControlRegion — layout real de la región mmap
//
// El seqlock embebe guard ANTES del snapshot para que un lector que solo
// mapea la primera página pueda validar la consistencia sin leer el resto.
// guard es odd durante escritura, even en reposo.
// ══════════════════════════════════════════════════════════════════════════════

struct alignas(64) SharedControlRegion {
    std::atomic<uint32_t> guard;   // seqlock (odd = escritura en curso)
    uint32_t              _pad;    // alineación 8 bytes para snapshot
    OmegaDspSnapshot      snapshot;
};

static_assert(sizeof(SharedControlRegion) <= 4096,
    "SharedControlRegion no cabe en una página (4096 bytes)");


// ══════════════════════════════════════════════════════════════════════════════
// OmegaControlBus — Bus de control cross-process
//
// Writer (daemon):  openWriter() → publish()
// Reader (effect):  openReader() → readLatest()
//
// readLatest() es lock-free, no bloquea, seguro en audio callback.
// publish()    es wait-free (seqlock sin contención en writer único).
// ══════════════════════════════════════════════════════════════════════════════

class OmegaControlBus {
public:
    static constexpr const char* DEFAULT_PATH =
        "/data/adb/ivanna_omega/omega_control_snapshot";
    static constexpr size_t REGION_SIZE = 4096; // una página

    OmegaControlBus() = default;
    ~OmegaControlBus() { close(); }

    // No copiable (gestiona un fd + puntero mmap)
    OmegaControlBus(const OmegaControlBus&) = delete;
    OmegaControlBus& operator=(const OmegaControlBus&) = delete;

    // ── Writer side (daemon) ──────────────────────────────────────────────────

    /** Crea/abre el archivo SHM y lo mapea con permisos de escritura.
     *  Inicializa con un snapshot default si el archivo es nuevo.
     *  @return true si listo para publish(). */
    bool openWriter(const char* path = DEFAULT_PATH) noexcept;

    /** Publica un snapshot de forma atómica (seqlock).
     *  Incrementa generation, calcula CRC, escribe bajo seqlock.
     *  Seguro llamar desde cualquier hilo serializado (command_server usa mutex). */
    bool publish(const OmegaDspSnapshot& snap) noexcept;

    /** Generación más reciente publicada (para reportar en respuestas JSON). */
    uint64_t lastPublishedGeneration() const noexcept { return m_lastGeneration; }

    // ── Reader side (omega_effect / audioserver) ──────────────────────────────

    /** Abre el archivo SHM y lo mapea en solo-lectura.
     *  Safe to call from constructor de omega_effect; nunca bloquea el audio callback.
     *  @return true si listo para readLatest(). */
    bool openReader(const char* path = DEFAULT_PATH) noexcept;

    /** Lee el snapshot más reciente de forma lock-free.
     *  Solo actualiza 'out' si hay un nuevo snapshot (generation > lastSeenGen).
     *  Si el snapshot está corrupto, deja 'out' sin cambios y retorna false.
     *  @param out          Destino del snapshot leído.
     *  @param lastSeenGen  IN/OUT: última generation vista por el lector.
     *  @return true si 'out' fue actualizado con un snapshot válido y nuevo. */
    bool readLatest(OmegaDspSnapshot& out, uint64_t& lastSeenGen) const noexcept;

    // ── Estado general ────────────────────────────────────────────────────────

    bool isWriterOpen() const noexcept { return m_region && m_isWriter; }
    bool isReaderOpen() const noexcept { return m_region && !m_isWriter; }
    bool isOpen()       const noexcept { return m_region != nullptr; }

    void close() noexcept;

private:
    SharedControlRegion* m_region     = nullptr;
    int                  m_fd         = -1;
    bool                 m_isWriter   = false;
    uint64_t             m_lastGeneration = 0;
};

// ── Singleton del daemon (writer) ──────────────────────────────────────────

inline OmegaControlBus& controlBus() noexcept {
    static OmegaControlBus instance;
    return instance;
}

// ── Bus LOCAL de telemetría del efecto (Ruta B, daemon presente o ausente) ──
// Writer único: instancia dedicada de OmegaControlBus en omega_effect.cpp
// (proceso audioserver). Reader: audioRouteBridgeLoop() en el proceso app.
// Es un SHM distinto del controlBus() root-only del daemon; nunca se usa como
// segundo writer del snapshot autoritativo. La app solo consume métricas PCM
// y generación aplicada. openWriter fija permisos 0644; SELinux aún puede
// denegar el acceso y en ese caso la telemetría queda explícitamente offline.
// 0043 (2026-11-17): el bus local vive en /data/adb/ivanna_omega/, NO
// en /data/local/tmp (mismo defecto que el beacon: el kernel ve el
// archivo como `unlabeled` pese a chcon ivanna_data_file; la app lo ve
// como "Permission denied" cada ~1 s). Hereda ivanna_data_file del
// directorio padre, etiquetado por service.sh; ver OMEGA_EFFECT_BEACON_PATH.
inline constexpr const char* OMEGA_EFFECT_LOCAL_BUS_PATH =
    "/data/adb/ivanna_omega/omega_effect_ctrl_local";

// ── Singleton del effect (reader) — proceso audioserver ────────────────────
// Separado del writer para evitar confusiones de proceso.
// En la práctica, el reader vive en omega_effect.cpp (proceso audioserver)
// y el writer en el daemon. Ambos llaman a openReader/openWriter respectivamente.
// El singleton es SOLO para el proceso actual.
inline OmegaControlBus& effectControlBus() noexcept {
    static OmegaControlBus instance;
    return instance;
}

} // namespace ivanna
