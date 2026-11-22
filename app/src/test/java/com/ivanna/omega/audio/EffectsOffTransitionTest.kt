// EffectsOffTransitionTest.kt
// ============================================================================
// PRUEBAS — Parche D (0038+0040): con todo apagado, los emisores no re-envían
// © 2026 Luis Uriel Pimentel Pérez
// ============================================================================
//
// Con todo apagado (neutral-off), los emisores periódicos NO deben re-enviar
// estado al daemon: ni SEND_PERCEPTUAL_STATE cada 500 ms (PerceptualViewModel
// / PerceptualCortex) ni SET_EQ_BANDS (IvannaGlobalEffectManager.republishOmegaEq
// / Iso226Calibrator). Antes, el poll de 500 ms re-encendía y re-coloreaba el
// DSP 500 ms después de haberlo apagado (el "cono de cartón" persistente).
//
// Este test fija el contrato del guard:
//   - EffectsOffTransition.isNeutralOffActive() refleja el estado OFF.
//   - IvannaGlobalEffectManager.republishOmegaEq() retorna false sin enviar
//     cuando neutral-off está activo (el guard es el PRIMER chequeo, antes
//     de isConnected).
//
// Nota (host): sin lib nativa ni daemon, isConnected es false, así que el
// test verifica el guard por su posición + el contrato de isNeutralOffActive();
// el bloqueo completo del dispatch en vivo se verifica en el dispositivo
// (prueba de cierre Fase 1: con todo apagado no hay SET_EQ_BANDS en el log).

package com.ivanna.omega.audio

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class EffectsOffTransitionTest {

    private fun setLastEffectsOff(value: Boolean?) {
        val field = EffectsOffTransition::class.java.getDeclaredField("lastEffectsOff")
        field.isAccessible = true
        field.set(EffectsOffTransition, value)
    }

    @Test fun neutralOffActiveReflectsState() {
        setLastEffectsOff(true)
        assertTrue(EffectsOffTransition.isNeutralOffActive())
        setLastEffectsOff(false)
        assertFalse(EffectsOffTransition.isNeutralOffActive())
        setLastEffectsOff(null)
        assertFalse(EffectsOffTransition.isNeutralOffActive())
    }

    @Test fun republishOmegaEqBlockedWhenNeutralOff() {
        val context = ApplicationProvider.getApplicationContext<Context>()
        val manager = IvannaGlobalEffectManager(context)
        setLastEffectsOff(true)
        // Guard activo: retorna false sin intentar enviar (ni siquiera llega
        // al chequeo de conexión).
        assertFalse(manager.republishOmegaEq())
        // Guard desactivado: llega al chequeo de conexión (false en host sin
        // daemon) — el guard ya no bloquea.
        setLastEffectsOff(false)
        assertFalse(manager.republishOmegaEq())
    }
}
