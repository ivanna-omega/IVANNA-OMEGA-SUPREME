// test_no_effect_rearm.cpp — Re-arme del auto-enable tras DISABLE espurio (v2.5.470).
//
// EL DEFECTO (NO_EFFECT): en este HAL EFFECT_CMD_ENABLE jamás llega pero
// EFFECT_CMD_DISABLE sí (al cambiar de música). El DISABLE pone
// explicitlyDisabled=true y el auto-enable queda bloqueado para siempre
// (la instancia vive en audioserver). El fix re-arme tras ~1 s de audio
// sostenido (kNoEffectRearmFrames) porque AudioFlinger sigue llamando a
// process() con bloques reales cuando una sesión nueva suena por la misma
// instancia (ni SET_CONFIG ni el snapshot cambian en un cambio de pista).
//
// omega_effect.cpp está BLOQUEADO para host (REPORT 0021), así que este
// test fija la decisión PURA (include/omega_effect_no_effect.h), que es la
// que omega_process() llama. El cableado (una llamada) se verifica en el
// teléfono: DISABLE al cambiar de música -> el efecto vuelve solo en ~1 s.
// Compila en host sin NDK ni root.
#include "include/omega_effect_no_effect.h"

#include <cstdint>
#include <cstdio>

using namespace ivanna;

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { std::printf("FAIL: %s\n", msg); ++failures; } \
    else         { std::printf("ok:   %s\n", msg); } \
} while (0)

int main() {
    // El umbral es el contrato: ~1 s a 48 kHz.
    CHECK(kNoEffectRearmFrames == 48000u, "umbral kNoEffectRearmFrames == 48000 (~1 s a 48 kHz)");

    // 1. Nunca deshabilitado -> auto-enable normal, inmediato, contador a cero.
    {
        bool dis = false;
        uint64_t frames = 12345u; // basura previa: debe resetearse
        CHECK(noEffectShouldEnable(dis, frames, 480u), "nunca deshabilitado -> enable inmediato");
        CHECK(!dis && frames == 0u, "nunca deshabilitado -> flag false y contador reseteado");
    }

    // 2. Deshabilitado + poco audio -> NO re-arme, el flag sigue y el contador acumula.
    {
        bool dis = true;
        uint64_t frames = 0;
        for (int i = 0; i < 10; ++i) {
            CHECK(!noEffectShouldEnable(dis, frames, 480u), "deshabilitado + 10 bloques -> sigue sin re-armar");
        }
        CHECK(dis, "tras 10 bloques el flag explicitlyDisabled sigue true");
        CHECK(frames == 4800u, "tras 10 bloques x 480 el contador == 4800");
    }

    // 3. Deshabilitado + audio sostenido hasta el umbral -> re-arme, flag limpio, contador a cero.
    {
        bool dis = true;
        uint64_t frames = 0;
        bool enabled = false;
        for (int i = 0; i < 100; ++i) { // 100 bloques x 480 = 48000
            enabled = noEffectShouldEnable(dis, frames, 480u);
            if (i < 99) CHECK(!enabled, "antes del umbral no hay re-arme");
        }
        CHECK(enabled, "al llegar a 48000 frames -> re-arme");
        CHECK(!dis, "tras el re-arme explicitlyDisabled == false");
        CHECK(frames == 0u, "tras el re-arme el contador vuelve a cero");
        // Y el siguiente bloque ya es auto-enable normal.
        CHECK(noEffectShouldEnable(dis, frames, 480u), "tras el re-arme el siguiente bloque habilita normal");
    }

    // 4. Borde exacto: 47999 -> false; un frame más (48000) -> true.
    {
        bool dis = true;
        uint64_t frames = 0;
        CHECK(!noEffectShouldEnable(dis, frames, 47999u), "47999 frames -> sin re-arme");
        CHECK(dis && frames == 47999u, "a 47999 el flag sigue y el contador == 47999");
        CHECK(noEffectShouldEnable(dis, frames, 1u), "48000 frames exactos -> re-arme");
        CHECK(!dis && frames == 0u, "en el borde el flag se limpia y el contador a cero");
    }

    // 5. Bloques de cero frames no avanzan el contador.
    {
        bool dis = true;
        uint64_t frames = 0;
        CHECK(!noEffectShouldEnable(dis, frames, 0u), "bloque de 0 frames -> sin re-arme");
        CHECK(frames == 0u, "bloque de 0 frames no suma");
        CHECK(!noEffectShouldEnable(dis, frames, 0u), "otro bloque de 0 frames -> sigue sin re-arme");
    }

    // 6. Un solo bloque grande (>= umbral) re-arme de inmediato.
    {
        bool dis = true;
        uint64_t frames = 0;
        CHECK(noEffectShouldEnable(dis, frames, 96000u), "un bloque de 96000 frames -> re-arme inmediato");
        CHECK(!dis && frames == 0u, "bloque grande: flag limpio y contador a cero");
    }

    // 7. Dos ciclos DISABLE -> re-arme: el segundo ciclo exige el umbral completo otra vez.
    {
        bool dis = true;
        uint64_t frames = 0;
        for (int i = 0; i < 100; ++i) noEffectShouldEnable(dis, frames, 480u);
        CHECK(!dis, "primer ciclo: re-armado");
        // Segundo DISABLE (el handler resetea el contador a cero).
        dis = true;
        frames = 0;
        for (int i = 0; i < 99; ++i) {
            CHECK(!noEffectShouldEnable(dis, frames, 480u), "segundo ciclo: 99 bloques aun sin re-arme");
        }
        CHECK(noEffectShouldEnable(dis, frames, 480u), "segundo ciclo: bloque 100 -> re-arme");
        CHECK(!dis && frames == 0u, "segundo ciclo: flag limpio y contador a cero");
    }

    // 8. DISABLE parcial y luego ENABLE explícito: el contador se resetea (no se
    // hereda un re-arme a medias al siguiente DISABLE).
    {
        bool dis = true;
        uint64_t frames = 0;
        for (int i = 0; i < 50; ++i) noEffectShouldEnable(dis, frames, 480u); // 24000
        CHECK(frames == 24000u, "medio ciclo: contador == 24000");
        dis = false; // ENABLE explícito (el handler pone explicitlyDisabled=false)
        CHECK(noEffectShouldEnable(dis, frames, 480u), "tras ENABLE explícito -> enable normal");
        CHECK(frames == 0u, "tras ENABLE explícito el contador se resetea");
    }

    if (failures != 0) {
        std::printf("FAILURES: %d\n", failures);
        return 1;
    }
    std::printf("test_no_effect_rearm: ALL OK\n");
    return 0;
}
