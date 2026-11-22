// ─────────────────────────────────────────────────────────────────────────────
// conservation_controls_bridge.cpp — atomics de CONSERVACIÓN PERCEPTUAL
// (árbitro de campo, 2026-11-21) para el proceso app (libivanna_omega.so).
// Mismo patrón que orbe_controls_bridge.cpp: el estado escrito desde la
// UI/JNI vive aquí y IvannaFusionCore::process() lo lee cada bloque (vía
// syncFusedControlsFromAtomics). El lado audioserver (libomega_effect.so)
// tiene su propio stub en conservation_globals_effect.cpp; NO duplicar en
// un mismo target (ODR).
// Apagado como valor inicial del atomic (amount=0 = bypass bit-exacto):
// la app lo pone al restaurar estado (ParameterStore) y la Ruta B lo
// recibe por snapshot (OmegaDspSnapshot ABI v17, campos consv_*;
// omega_apply_snapshot lo reenvía vía setConservationParams()). El stub
// del audioserver arranca en el mismo 0 inicial hasta que llega el primer
// snapshot.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>

std::atomic<float> g_consv_amount{0.0f};
std::atomic<float> g_consv_energy_cap{1.0f};
std::atomic<float> g_consv_phase_coherence{0.5f};
std::atomic<float> g_consv_inertia{0.82f};
