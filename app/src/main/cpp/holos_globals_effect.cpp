// ─────────────────────────────────────────────────────────────────────────────
// holos_globals_effect.cpp — stub de los atomics g_holos_* para el proceso
// audioserver (libomega_effect.so). Mismo patrón que
// orbe_globals_effect.cpp: la definición real está en
// holos_controls_bridge.cpp (target ivanna_omega). NO añadir a ivanna_omega:
// ODR violation. Valores iniciales: amount=0 (valor inicial del atomic;
// la app lo pone a 1.0, harmonics 0.002, spread 1.5, proximityDb 6.0,
// diffuse 0.85 (ATREVIDA 502), al restaurar estado y la Ruta B lo
// recibe por snapshot ABI v10, campos holos_* — omega_apply_snapshot lo
// aplica vía setHolosParams()). Hasta que llega el primer snapshot, el
// proceso audioserver usa este 0 inicial (bypass bit-exacto). Corre EN
// SERIE tras ORBE (ORBE → HOLOS → IVANNA → Φ).
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>

std::atomic<float> g_holos_amount{0.0f};
std::atomic<float> g_holos_distance_m{0.35f};
std::atomic<float> g_holos_spread{1.5f};         // ATREVIDA 502 (directiva Esse 2026-11-24)
std::atomic<float> g_holos_proximity_db{6.0f};  // ATREVIDA 502
std::atomic<float> g_holos_diffuse{0.85f};      // ATREVIDA 502
std::atomic<float> g_holos_harmonics{0.002f};  // v2.5.411: inicial = perfil de arranque (ATREVIDA 502, harmonics 0.002); la unica fuente de verdad es ParameterStore.FactoryProfile; el 0.32 viejo no estaba validado (tarea 1: max 0.002)
std::atomic<float> g_holos_warmth{0.5f};
std::atomic<float> g_holos_dynamics{1.0f};
std::atomic<float> g_holos_width{1.0f};
std::atomic<float> g_holos_gamma{1.0f};
std::atomic<float> g_holos_room_rt60{0.34f};
std::atomic<bool>  g_holos_head_tracking{true};
std::atomic<int>   g_holos_band_count{24};
std::atomic<float> g_holos_eq_low{0.0f};
std::atomic<float> g_holos_eq_mid{0.0f};
std::atomic<float> g_holos_eq_high{0.0f};
std::atomic<float> g_holos_eq_presence{0.0f};
std::atomic<float> g_holos_eq_tilt_db{0.0f};
std::atomic<float> g_holos_eq_freq{1000.0f};
