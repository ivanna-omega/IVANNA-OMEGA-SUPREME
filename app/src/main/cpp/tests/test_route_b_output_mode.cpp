// test_route_b_output_mode.cpp — Regresión FIX 1 (bofo Genezi, v2.5.465).
//
// CAUSA RAÍZ MEDIDA (vivo 2026-11-18): el snapshot trae ORBE=1.0,
// HOLOS=0.5 y el efecto en el HAL los aplica al Genezi aunque la puerta
// de la app (ff6f706d, RouteBDeviceGate.kt) diga SPEAKERS. El efecto
// aplicaba el snapshot directo, sin chequeo de dispositivo/modo.
// El fix: el modo de salida viaja EN el snapshot (temporal_field bits
// 18..19, OmegaOutputMode: 0 = desconocido, 1 = audífonos, 2 = bocina
// plana, 3 = Genezi — tercer modo, evol-holos 2026-11-26), puesto por
// la app vía SET_PERCEPTUAL_STATE ("outputMode"), y el efecto lo hace
// cumplir en omega_apply_snapshot (omega_effect.cpp).
// Este test fija el contrato del lado control plane (sin NDK, sin red):
//   1. encode/decode round-trip de los 4 modos (0 UNKNOWN, 1 audífonos,
//      2 bocina plana, 3 GENEZI — tercer modo, evol-holos 2026-11-26:
//      el Genezi ya no codifica como bocina plana).
//   2. Sin solape de máscaras con features HRTF/adaptive (16..17),
//      FotoHolo (6..15), XTC (5) ni temporal (0..4).
//   3. ABI v17 (conservación): sizeof 808, VERSION 17, crc32 en 804
//      (4 campos nuevos al final: consv_amount/energy_cap/
//      phase_coherence/inertia; offsets viejos intactos).
//      makeDefault()/makeNeutral() decodifican UNKNOWN.
//   4. SET_PERCEPTUAL_STATE con orbeAmount 1.0 + outputMode 2 -> ok,
//      generation 1; al aplicar a makeDefault(), orbe_amount == 1.0
//      (el snapshot CONSERVA el monto del usuario: neutraliza el efecto,
//      no el daemon) y el modo decodifica SPEAKERS.
//   5. Un segundo SET sin outputMode (solo holosAmount) CONSERVA el modo
//      SPEAKERS (merge por NaN, como el resto de campos).
//   6. Un SET con solo outputMode cuenta como campo (any() true, la
//      generación avanza): el modo puede publicarse solo.
//   7. outputMode fuera de rango (9) -> UNKNOWN (fail-closed).
//   9. outputMode 3 (GENEZI) vía responder: decodifica GENEZI y el
//      merge lo conserva (el sello de modo del gate lo re-empuja en
//      cada comando perceptual).
//   8. Una actualización parcial del campo temporal (temporalEnabled)
//      conserva los bits del modo (máscara ~0x1F, no rebuild).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "../daemon/control/daemon_command_responder.h"
#include "../daemon/control/perceptual_state.h"
#include "../include/omega_control_bus.h"

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, const char* name) {
    if (ok) {
        ++g_pass;
        std::printf("  PASS: %s\n", name);
    } else {
        ++g_fail;
        std::printf("  FALLA: %s\n", name);
    }
}

bool contains(const std::string& s, const char* needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    using namespace ivanna;
    using namespace ivanna::daemon;

    // 1. encode/decode round-trip; tercer modo (3) = GENEZI (evol-holos).
    check(omega_output_mode_decode(omega_output_mode_encode(OmegaOutputMode::UNKNOWN)) ==
              OmegaOutputMode::UNKNOWN,
          "codec: UNKNOWN round-trip");
    check(omega_output_mode_decode(omega_output_mode_encode(OmegaOutputMode::HEADPHONES)) ==
              OmegaOutputMode::HEADPHONES,
          "codec: HEADPHONES round-trip");
    check(omega_output_mode_decode(omega_output_mode_encode(OmegaOutputMode::SPEAKERS)) ==
              OmegaOutputMode::SPEAKERS,
          "codec: SPEAKERS round-trip");
    check(omega_output_mode_decode(omega_output_mode_encode(OmegaOutputMode::GENEZI)) ==
              OmegaOutputMode::GENEZI,
          "codec: GENEZI round-trip");
    check(omega_output_mode_decode(0x3u << OMEGA_OUTPUT_MODE_SHIFT) == OmegaOutputMode::GENEZI,
          "codec: valor 3 = GENEZI");

    // 2. Sin solape de máscaras.
    check((OMEGA_OUTPUT_MODE_MASK & OMEGA_EFFECT_FEATURES_MASK) == 0u,
          "mascaras: modo no solapa features HRTF/adaptive");
    check((OMEGA_OUTPUT_MODE_MASK & OMEGA_FOTOHOLO_INTENSITY_MASK) == 0u &&
              (OMEGA_OUTPUT_MODE_MASK & OMEGA_FOTOHOLO_ON_BIT) == 0u,
          "mascaras: modo no solapa FotoHolo");
    check((OMEGA_OUTPUT_MODE_MASK & 0x1Fu) == 0u &&
              (OMEGA_OUTPUT_MODE_MASK & OMEGA_TEMPORAL_FIELD_XTC_BIT) == 0u,
          "mascaras: modo no solapa temporal/XTC");

    // 3. Cero cambio de ABI salvo el bump intencionado (pin v17, conservación:
    //    4 campos nuevos al final; offsets viejos intactos).
    check(sizeof(OmegaDspSnapshot) == 808, "abi: sizeof sigue 808 (v17)");
    check(OMEGA_CTRL_VERSION == 17u, "abi: VERSION sigue 17 (v17)");
    check(offsetof(OmegaDspSnapshot, crc32) == 804, "abi: crc32 sigue en 804 (v17)");
    check(omega_output_mode_decode(OmegaDspSnapshot::makeDefault().temporal_field) ==
              OmegaOutputMode::UNKNOWN,
          "abi: makeDefault decodifica UNKNOWN");
    check(omega_output_mode_decode(OmegaDspSnapshot::makeNeutral().temporal_field) ==
              OmegaOutputMode::UNKNOWN,
          "abi: makeNeutral decodifica UNKNOWN");

    // 4. SET_PERCEPTUAL_STATE con orbeAmount 1.0 + outputMode 2.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1.0,"
            "\"outputMode\":2.0}",
            st);
        check(contains(r, "\"ok\":true"), "responder: ok true");
        check(contains(r, "accepted_pending_consumer"), "responder: accepted_pending_consumer");
        check(contains(r, "\"generation\":1"), "responder: generation 1");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "responder: take true");
        check(!std::isnan(p.outputMode) && p.outputMode == 2.0f, "responder: outputMode 2 parseado");
        OmegaDspSnapshot snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(snap.orbe_amount == 1.0f, "apply: orbe_amount 1.0 conservado (no lo toca el daemon)");
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::SPEAKERS,
              "apply: modo SPEAKERS en temporal_field");
        // El bit HRTF de makeDefault (features bit 16) sobrevive al apply.
        check((snap.temporal_field & OMEGA_EFFECT_HRTF_ENABLED_BIT) != 0u,
              "apply: bit HRTF preservado al escribir el modo");

        // 5. Segundo SET sin outputMode: el modo se conserva (merge).
        const std::string r2 = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":0.5}",
            st);
        check(contains(r2, "\"generation\":2"), "merge: generation 2");
        PendingPerceptual p2;
        check(takePendingPerceptual(st, p2), "merge: take true");
        check(std::isnan(p2.outputMode), "merge: outputMode ausente = NaN");
        applyPendingPerceptual(snap, p2);
        check(snap.holos_amount == 0.5f, "merge: holos_amount 0.5");
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::SPEAKERS,
              "merge: modo SPEAKERS conservado sin reenviarlo");

        // 8. Actualización parcial del campo temporal: conserva el modo.
        PendingPerceptual p3;
        p3.temporalEnabled = 1.0f;
        p3.temporalAmount = 0.6f;
        applyPendingPerceptual(snap, p3);
        check((snap.temporal_field & 1u) != 0u, "temporal: bit on puesto");
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::SPEAKERS,
              "temporal: modo SPEAKERS conservado tras update parcial");
    }

    // 6. SET con solo outputMode: cuenta como campo, avanza la generación.
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"outputMode\":1.0}",
            st);
        check(contains(r, "\"ok\":true"), "solo-modo: ok true");
        check(!contains(r, "no_fields"), "solo-modo: no es no_fields");
        check(contains(r, "\"generation\":1"), "solo-modo: generation 1");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "solo-modo: take true");
        OmegaDspSnapshot snap = OmegaDspSnapshot::makeNeutral();
        applyPendingPerceptual(snap, p);
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::HEADPHONES,
              "solo-modo: modo HEADPHONES aplicado");
    }

    // 7. outputMode fuera de rango -> UNKNOWN (fail-closed).
    {
        CommandPlaneState st;
        (void)handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"outputMode\":9.0}",
            st);
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "rango: take true");
        OmegaDspSnapshot snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::UNKNOWN,
              "rango: outputMode 9 = UNKNOWN");
    }

    // 9. outputMode 3 (GENEZI) vía responder: decodifica GENEZI; un
    // segundo SET sin outputMode conserva GENEZI (merge por NaN).
    {
        CommandPlaneState st;
        const std::string r = handleCommand(
            "{\"action\":\"SET_PERCEPTUAL_STATE\",\"holosAmount\":1.0,"
            "\"outputMode\":3.0}",
            st);
        check(contains(r, "\"ok\":true"), "genezi: ok true");
        PendingPerceptual p;
        check(takePendingPerceptual(st, p), "genezi: take true");
        OmegaDspSnapshot snap = OmegaDspSnapshot::makeDefault();
        applyPendingPerceptual(snap, p);
        check(snap.holos_amount == 1.0f, "genezi: holos_amount 1.0");
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::GENEZI,
              "genezi: modo GENEZI aplicado");
        check(omega_output_binaural_gated(OmegaOutputMode::GENEZI) == false,
              "genezi: binaural NO gateado");
        check(omega_output_binaural_gated(OmegaOutputMode::SPEAKERS) == true,
              "genezi: binaural SI gateado en SPEAKERS");
        check(omega_output_is_speakers_like(OmegaOutputMode::GENEZI) == true,
              "genezi: speakers-like (XTC/Aux/twin)");
        (void)handleCommand("{\"action\":\"SET_PERCEPTUAL_STATE\",\"orbeAmount\":1.0}", st);
        PendingPerceptual p2;
        check(takePendingPerceptual(st, p2), "genezi-merge: take true");
        applyPendingPerceptual(snap, p2);
        check(omega_output_mode_decode(snap.temporal_field) == OmegaOutputMode::GENEZI,
              "genezi-merge: modo GENEZI conservado sin reenviarlo");
    }

    std::printf("test_route_b_output_mode: %d pass, %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
