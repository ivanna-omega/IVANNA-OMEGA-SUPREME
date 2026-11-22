// new_physics_globals_effect.cpp — stub de los símbolos de FÍSICA NUEVA
// (g_np_*) para libomega_effect.so.
//
// POR QUE EXISTE: omega_effect.cpp referencia g_np_* con `extern`. La
// definición real vive en new_physics_controls_bridge.cpp, que entra SOLO al
// target ivanna_omega (proceso app). libomega_effect.so corre en el proceso
// audioserver — address-space separado — así que el linker con --no-undefined
// no puede resolverlos sin este stub.
//
// Mismo patrón ya usado para g_orbe_* en orbe_globals_effect.cpp:
// stub independiente con los mismos valores iniciales, definido solo para
// ESTE target. NO añadir a ivanna_omega: generaría ODR violation con
// new_physics_controls_bridge.cpp.
//
// Valores iniciales idénticos a new_physics_controls_bridge.cpp: amount=0
// (bypass bit-exacto). Desde ABI v14 (v2.5.472) los controles SÍ llegan a
// Ruta B: el snapshot lleva los 17 campos np_* y omega_apply_snapshot los
// reenvía a estos atomics (mismo patrón que orbe/holos); omega_process los
// sincroniza al motor vía omega_apply_new_physics → setControls.
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
