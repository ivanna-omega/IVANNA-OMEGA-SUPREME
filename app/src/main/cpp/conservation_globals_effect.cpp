// conservation_globals_effect.cpp — stub de los símbolos de CONSERVACIÓN
// PERCEPTUAL (árbitro de campo, 2026-11-21) para libomega_effect.so
//
// POR QUE EXISTE: IvannaFusionCore.cpp (incluido por unity-build en
// omega_effect.cpp) referencia g_consv_* con `extern`. La definición real
// vive en conservation_controls_bridge.cpp, que entra SOLO al target
// ivanna_omega (proceso app). libomega_effect.so corre en el proceso
// audioserver — address-space separado — así que el linker con
// --no-undefined no puede resolverlos sin este stub.
//
// Mismo patrón ya usado para g_orbe_* en orbe_globals_effect.cpp:
// stub independiente con los mismos valores iniciales, definido solo
// para ESTE target. NO añadir a ivanna_omega: generaría ODR violation
// con conservation_controls_bridge.cpp.
//
// Valores iniciales idénticos a conservation_controls_bridge.cpp:
// amount=0 (bypass bit-exacto; la app lo pone al restaurar estado y la
// Ruta B lo recibe por snapshot ABI v17, campos consv_* —
// omega_apply_snapshot lo aplica vía setConservationParams()). Hasta que
// llega el primer snapshot, el proceso audioserver usa este 0 inicial
// (bypass bit-exacto).
#include <atomic>

std::atomic<float> g_consv_amount{0.0f};
std::atomic<float> g_consv_energy_cap{1.0f};
std::atomic<float> g_consv_phase_coherence{0.5f};
std::atomic<float> g_consv_inertia{0.82f};
