// ─────────────────────────────────────────────────────────────────────────────
// new_physics_controls_bridge.cpp — atomics de FÍSICA NUEVA (Fase 2, v2.5.472)
// para el proceso app (libivanna_omega.so). Mismo patrón que
// orbe_controls_bridge.cpp: el estado escrito desde la UI/JNI vive aquí y
// el motor Ruta A (g_newPhysicsRouteA en ivanna_omega_jni.cpp) lo lee cada
// bloque. El lado audioserver (libomega_effect.so) tiene su propio stub en
// new_physics_globals_effect.cpp; NO duplicar en un mismo target (ODR).
// Apagado solo como valor inicial del atomic (amount=0 = bypass
// bit-exacto): la app lo pone a 1.0 al encender la tarjeta CONTROL.
// Desde ABI v14 (v2.5.472) la Ruta B SÍ recibe estos controles: la app los
// empuja por SET_PERCEPTUAL_STATE, el daemon los publica en el snapshot y
// el efecto los aplica al motor. El efecto usa estos mismos defaults
// (bypass) en su propio proceso hasta que llega el primer snapshot v14.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>

std::atomic<float>   g_np_amount{0.0f};
std::atomic<float>   g_np_lgn_mix{1.0f};
std::atomic<float>   g_np_bs_damp{0.65f};
std::atomic<float>   g_np_torso_mix{0.18f};
std::atomic<float>   g_np_head_radius_m{0.0875f};
std::atomic<float>   g_np_az_deg{0.0f};
std::atomic<float>   g_np_el_deg{0.0f};
std::atomic<int32_t> g_np_eq_class{0};
std::atomic<float>   g_np_omega_sig_mix{0.0f};
std::atomic<float>   g_np_boost_cap_db{3.0f};
std::atomic<float>   g_np_xtalk_mix{0.0f};
std::atomic<float>   g_np_sigma_dist_m{0.35f};
std::atomic<float>   g_np_spl_db{85.0f};
std::atomic<float>   g_np_air_mix{0.0f};
std::atomic<float>   g_np_volterra_mix{0.0f};
std::atomic<float>   g_np_free_energy_mix{0.0f};
std::atomic<int32_t> g_np_abx_mode{0};
