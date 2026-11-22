// np483_defaults.hpp — VALORES POR DEFECTO DE LOS ÓRGANOS v3 (v2.5.483).
// Despierta los órganos dormidos del motor v3 con valores aproximados
// DOCUMENTADOS (no medidos): cada número tiene su fuente en
// ~/workspace/np483/APPROXIMATIONS.md y en los comentarios de abajo.
// NADA de lo de aquí toca las 6 correcciones de la 482 (tope de polo
// 0.9995, polo Genezi 0.9983, κ 0.20, H2 ×2 en cruces, LPF 1500 Hz):
// el kernel aproximado usa el mismo polo 0.9983 que el ajuste medido.
//
// Uso: wakeV3Organs(eng) tras prepare(). Idempotente y fail-closed:
// cada carga valida sus entradas (puertas del motor) y un órgano que
// falla queda en OFF sin afectar a los demás ni al audio.
#pragma once

#include "fused/new_physics_engine.hpp"
#include "fused/room_ir_default.hpp"

namespace ivanna::newphys {

// ── Klippel H2 aproximado (pre-distorsión, signo NEGATIVO) ───────────────
// Calibrado con ~/workspace/np483/calibrate_klippel_approx.py (modelo
// Python verificado contra el C++ en la 482): H2 = 0.50% (-46 dB) @85 dB
// SPL, seno 0.5 @68.5 Hz. THD típico ~0.5% a 85 dB en bocinas pequeñas,
// casi todo H2 (High Fidelity / Diversified Science Labs). Σ|k| = 0.0031
// (puerta del motor ≤ 0.35). Forma diagonal-dominante decreciente + cruces
// a 1/2 de la media geométrica.
inline constexpr float kNp483Tri21Approx[NewPhysicsEngine::kLagTri21] = {
    -0.000837f, -0.000296f, -0.000209f, -0.000148f, -0.000105f, -0.000074f,
    -0.000419f, -0.000148f, -0.000105f, -0.000074f, -0.000052f, -0.000209f,
    -0.000074f, -0.000052f, -0.000037f, -0.000105f, -0.000037f, -0.000026f,
    -0.000052f, -0.000019f, -0.000026f
};

// ── Klippel H3 aproximado (pre-distorsión, signo NEGATIVO) ───────────────
// Mismo modelo: H3 = 0.20% (-54 dB) @85 dB SPL (H3 típico ~1/3 del H2 en
// un driver razonablemente simétrico). alpha3 = término cúbico sin memoria;
// c3[m] = diagonal Laguerre decreciente. Dentro de los clamps del motor
// (±0.15 c3, ±0.25 alpha3).
inline constexpr float kNp483C3Approx[NewPhysicsEngine::kLagOrder] = {
    -0.000167f, -0.000083f, -0.000042f, -0.000021f, -0.000010f, -0.000005f
};
inline constexpr float kNp483Alpha3Approx = -0.000167f;

// Polo Laguerre del kernel aproximado: el mismo del ajuste medido
// (exp(-1/(0.0125·fs)) ≈ 0.9983 @48 kHz, admitido por el tope 0.9995).
inline constexpr float kNp483LagPoleApprox = 0.9983f;

// ── Sub-graves modal (etapas 1..5; la 0 es el diferencial Ω automático) ──
// Cut-only (regla durable Fase 4: la corrección de sala solo corta, nunca
// refuerza). Sala = la de rir_0101.wav (3.5 x 2.9 x 4.6 m), la misma de la
// cola de sala: los modos axiales f = c/2L (c = 343 m/s) y la cola describen
// LA MISMA sala. Picos modales típicos en sala pequeña sin tratar: +10 a
// +20 dB (axial, el más fuerte); la corrección es conservadora (-3/-4 dB,
// Q 4) para no vaciar el grave.
// Etapa 1: polo 68.5 Hz del Genezi (banco modal del motor v3; el puerto
// bass-reflex típico abulta +3..+6 dB en su sintonía: corte -3 dB, Q 3).
struct Np483SubStage {
    int idx;
    double freqHz, q, gainDb;
    bool lowShelf, active;
};
inline constexpr Np483SubStage kNp483SubStages[5] = {
    {1, 68.5, 3.0, -3.0, false, true},  // polo Genezi 68.5 Hz
    {2, 37.3, 4.0, -4.0, false, true},  // axial 4.6 m (largo)
    {3, 49.0, 4.0, -4.0, false, true},  // axial 3.5 m (ancho)
    {4, 59.1, 4.0, -4.0, false, true},  // axial 2.9 m (alto)
    {5, 74.6, 4.0, -3.0, false, true},  // 2º armónico del largo (4.6 m)
};

// Mezcla de cola de sala por defecto (0..1): 0.5 = cola real audible sin
// ahogar el directo (la cola medida tiene RMS -45 dB re directo).
inline constexpr float kNp483RoomTailMixDefault = 0.5f;

// Despierta los 3 órganos en un motor ya preparado (prepare() antes).
// Devuelve true si el kernel Klippel y la cola quedaron armados.
// Seguro llamarlo varias veces (recarga los mismos valores).
inline bool wakeV3Organs(NewPhysicsEngine& eng) noexcept {
    for (const auto& s : kNp483SubStages) {
        NewPhysicsEngine::SubBassStageParams p;
        p.freqHz = s.freqHz;
        p.q = s.q;
        p.gainDb = s.gainDb;
        p.lowShelf = s.lowShelf;
        p.active = s.active;
        eng.setSubBassModalStage(s.idx, p);
    }
    eng.setKlippelH3(kNp483C3Approx, kNp483C3Approx,
                     kNp483Alpha3Approx, kNp483Alpha3Approx);
    eng.setKlippelH3Mix(1.0f);
    const bool klippel = eng.loadKlippelH2H3(
        kNp483Tri21Approx, kNp483Tri21Approx,
        kNp483C3Approx, kNp483C3Approx,
        kNp483Alpha3Approx, kNp483Alpha3Approx, kNp483LagPoleApprox);
    const bool tail = eng.loadMeasuredRoomMimoTail(
        kRoomIrDefaultLL, kRoomIrDefaultLR, kRoomIrDefaultRL, kRoomIrDefaultRR,
        kRoomIrDefaultLen, kNp483RoomTailMixDefault);
    return klippel && tail;
}

}  // namespace ivanna::newphys
