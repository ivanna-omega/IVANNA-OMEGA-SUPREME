// omega_effect_no_effect.h — Re-arme del auto-enable tras un DISABLE espurio (v2.5.470).
//
// EL DEFECTO (NO_EFFECT): en este HAL (Qualcomm) EFFECT_CMD_ENABLE jamás llega
// al efecto nativo aunque la app haga setEnabled(true), pero EFFECT_CMD_DISABLE
// SÍ llega (al cambiar de música / al reconfigurar la cadena). El DISABLE pone
// explicitlyDisabled=true y el auto-enable de omega_process() (que solo dispara
// si !explicitlyDisabled) queda bloqueado para siempre: la instancia vive en
// audioserver, reiniciar la app no la resetea, y el efecto queda en passthrough
// (la música suena sin IVANNA) hasta que una sesión NUEVA crea otra instancia.
//
// POR QUÉ EL RE-ARME ES SEGURO:
//  - La app NUNCA envía DISABLE: ningún setEnabled(false) en todo el Kotlin
//    (el "apagado" del usuario es DSP neutro — EffectsOffTransition /
//    setNeutral —, no DISABLE). Todo DISABLE que llega es de AudioFlinger
//    al reconfigurar, nunca intención del usuario.
//  - Re-habilitar una cadena neutra es sonoramente transparente (neutro =
//    bit-exacto, −303.89 dB); re-habilitar una cadena activa es lo que el
//    usuario quiere (el DSP no neutro es su estado elegido).
//  - El re-arme exige audio SOSTENIDO (~1 s de bloques reales tras el
//    DISABLE): sin audio no hay re-arme. Un DISABLE seguido de silencio
//    (sesión realmente terminada) no re-arme nada.
//  - CAVEAT: si la app ganara algún día una vía de DISABLE real (usuario
//    apagando el efecto), este re-arme la revertiría a ~1 s y habría que
//    distinguirlo con un flag propio (userDisabled). Hoy esa vía no existe.
// LA SEÑAL: tras un DISABLE, AudioFlinger sigue llamando a omega_process()
// con bloques reales cuando una sesión nueva suena por la misma instancia
// (mismo session ID, cambio de pista: ni SET_CONFIG ni el snapshot cambian,
// solo process() sigue). Acumular los frames procesados tras el DISABLE y
// re-armar al llegar a kNoEffectRearmFrames es la única señal fiable.
//
// PURA Y TESTEABLE EN HOST (sin dependencias): omega_effect.cpp está
// BLOQUEADO para host (IvannaFusionCore no enlaza + drain no compila con
// g++-14, REPORT 0021), así que la decisión vive aquí y el test
// test_no_effect_rearm.cpp la fija. La integración en omega_process() es
// una sola llamada (el cableado no se puede probar en host; se verifica en
// el teléfono: DISABLE al cambiar de música -> el efecto vuelve solo en ~1 s).
// El re-arme reutiliza el evento kAutoEnable del rtlog (el resultado es el
// mismo: auto-enable); no se añade evento nuevo para no tocar el drenador.
#pragma once

#include <cstdint>

namespace ivanna {

// Frames de audio sostenido tras un DISABLE antes de re-armar el
// auto-enable. 48000 = ~1 s a 48 kHz (~0.5 s a 96 kHz, ~0.25 s a 192 kHz:
// el re-arme es igual o más rápido a mayor sample rate, nunca más lento
// que ~1 s). Suficiente para que la rampa de salida (18 ms) complete y
// para no reaccionar a un DISABLE transitorio; corto para que el usuario
// no note el hueco al cambiar de música.
inline constexpr uint64_t kNoEffectRearmFrames = 48000u;

// Decide si este bloque debe (re-)habilitar el efecto. Llamar solo cuando
// !enabled (el llamador ya lo garantiza).
//  - Nunca deshabilitado explícitamente -> true (auto-enable normal) y
//    resetea el contador por higiene.
//  - Deshabilitado explícitamente -> acumula framesInBlock; cuando el audio
//    sostenido alcanza kNoEffectRearmFrames, limpia explicitlyDisabled,
//    resetea el contador y devuelve true (re-arme). Antes, false.
// RT-safe: solo aritmética entera, sin alocación, sin locks, sin I/O.
// Hilo: el contador lo escribe omega_process (audio) y lo resetea
// omega_command en DISABLE/ENABLE (control); mismo patrón benigno que
// enabled/explicitlyDisabled (eventualmente consistente; el peor caso es
// un re-arme un bloque tarde).
inline bool noEffectShouldEnable(bool& explicitlyDisabled,
                                 uint64_t& disabledFrames,
                                 uint32_t framesInBlock) noexcept {
    if (!explicitlyDisabled) {
        disabledFrames = 0;
        return true;
    }
    disabledFrames += framesInBlock;
    if (disabledFrames >= kNoEffectRearmFrames) {
        disabledFrames = 0;
        explicitlyDisabled = false;
        return true;
    }
    return false;
}

} // namespace ivanna
