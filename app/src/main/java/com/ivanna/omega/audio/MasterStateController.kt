// MasterStateController.kt
// ============================================================================
// FASE 5 — UN SOLO CONTROLADOR MAESTRO DE ESTADO (Single Source of Truth)
// © 2026 Luis Uriel Pimentel Pérez
// ============================================================================
//
// Fase 1 (parches A-E, 2.5.461) cortó los escritores duplicados con guards
// (EffectsOffTransition.isNeutralOffActive() + gate holosEq_*), pero NO había
// un dueño del estado: tres árbitros independientes (RouteArbiter por beacon,
// AudioBackendSelector cada 10 s, RouteBDeviceGate por tipos AudioDeviceInfo)
// y seis emisores escribían al daemon/efecto por vías distintas, con 5 huecos
// medidos (DSPState.pushToNative sin guard, DspStateUpdater parcial,
// PerceptualViewModel.setAggressiveness sin guard, Iso226 capas 1-2 sin
// guard, PersistedStateRestorer sin sendHyperVolterraToDaemon al arrancar).
//
// Este objeto es LA ÚNICA FUENTE DE LA VERDAD del estado maestro:
//
//   BYPASS     = bypass del usuario (bit-exacto, nativeProcess retorna antes
//                  de tocar el buffer; g_masterBypass). Gana sobre LIMPIO_OFF.
//   LIMPIO_OFF = todo apagado real (fórmula effectsOff: orbe/holos/omni/hyper
//                  en 0 + Volterra OFF + adaptiveMode OFF). Cadena base
//                  transparente + snapshot neutro al daemon.
//   ACTIVO     = resto (cadena viva, una sola ruta activa).
//
// Reglas:
// - La DECISIÓN es central (solo aquí se publica MasterState); los
//   DISPARADORES son distribuidos pero todos desembocan en submitState /
//   noteNeutralOff (idempotentes): (a) EffectsOffTransition.evaluate()
//   avisa vía noteNeutralOff() tras cada transición neutra; (b)
//   DSPState.pushToNative() sincroniza en cada push (bypass + neutral-off);
//   (c) evaluate(context) es la entrada canónica explícita.
// - El fan-out es ordenado y solo en cambio real: DSPBridge.setBypass +
//   AudioEngine.setBypass (mismo flag g_masterBypass); al volver a ACTIVO,
//   re-publicar Ruta B con el gate vigente. Sin cambio: no JNI, no socket.
// - La transición neutra se DELEGA a EffectsOffTransition.evaluate()
//   (reutiliza su fórmula, applyOff/applyOn y su lock privado; NO se
//   duplican). El Maestro solo publica el estado.
// - RouteArbiter y AudioBackendSelector siguen como SENSORES de evidencia
//   (no se reemplazan en Fase 5); el Maestro registra si Ruta B procesa
//   (routeBProcessing, telemetría) pero la evidencia de beacon NO cambia
//   el estado maestro: el beacon selecciona la RUTA y el gate, no el
//   estado que el usuario pidió. Conflarlos haría que un beacon muerto
//   volteara el estado del usuario.
// - Nunca locks en el hilo de audio: el Maestro solo vive en hilo de
//   control/UI. Fail-closed: nunca lanza; ante fallo, no se publica nada.
// - BYPASS (bit-exacto, sin fundido) ≠ LIMPIO_OFF (cadena transparente +
//   snapshot neutro). No se cambia la semántica de ninguno.

package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import com.ivanna.omega.core.PersistedStateRestorer
import com.ivanna.omega.dsp.DSPBridge
import com.ivanna.omega.dsp.DSPStatePrefs
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow

enum class MasterState { BYPASS, LIMPIO_OFF, ACTIVO }

object MasterStateController {
    private const val TAG = "MasterStateController"

    private val _currentState = MutableStateFlow(MasterState.ACTIVO)
    val currentState: StateFlow<MasterState> = _currentState.asStateFlow()

    // Evidencia de ruta (telemetría/UI): true si el beacon dice que Ruta B
    // procesa. NO cambia el estado maestro (ver KDoc).
    private val _routeBProcessing = MutableStateFlow(false)
    val routeBProcessing: StateFlow<Boolean> = _routeBProcessing.asStateFlow()

    private val lock = Any()

    // true tras el primer evaluate(): antes de eso, isNeutralOffActive()
    // consulta la fuente viva (EffectsOffTransition) para no mentir en el
    // arranque (restore() aún no corrió).
    @Volatile
    private var evaluated = false

    @Volatile
    private var bypassActive = false

    @Volatile
    private var appContext: Context? = null

    /** Lectura canónica del neutro (compat con los guards Fase 1). */
    fun isNeutralOffActive(): Boolean =
        _currentState.value == MasterState.LIMPIO_OFF ||
            (!evaluated && EffectsOffTransition.isNeutralOffActive())

    fun isBypassActive(): Boolean = _currentState.value == MasterState.BYPASS

    /**
     * Entrada canónica explícita. Delega la transición neutra, calcula el
     * estado desde las fuentes canónicas y lo publica. Idempotente.
     */
    fun evaluate(context: Context) {
        val ctx = context.applicationContext
        appContext = ctx
        // 1. Transición neutra (reutiliza fórmula + applyOff/applyOn + lock;
        //    al tener éxito avisa vía noteNeutralOff y sincroniza el Maestro).
        EffectsOffTransition.evaluate(ctx)
        // 2. BYPASS gana sobre LIMPIO_OFF (bit-exacto del usuario).
        val bypass = runCatching { DSPStatePrefs.load(ctx).bypass }.getOrDefault(false)
        bypassActive = bypass
        evaluated = true
        if (bypass) {
            submitState(MasterState.BYPASS, "evaluate")
        } else {
            val desired =
                if (EffectsOffTransition.isNeutralOffActive()) MasterState.LIMPIO_OFF
                else MasterState.ACTIVO
            submitState(desired, "evaluate")
        }
        // 3. Evidencia de ruta (telemetría; no cambia el estado).
        runCatching { _routeBProcessing.value = RouteArbiter.systemWideEffectProcessing() }
            .onFailure { Log.w(TAG, "evaluate: beacon no legible: ${it.message}") }
    }

    /**
     * Lo llama EffectsOffTransition.evaluate() tras una transición neutra
     * exitosa. Sincroniza LIMPIO_OFF/ACTIVO sin tocar BYPASS (el bypass lo
     * posee evaluate()/submitState; si bypass está activo, el estado se
     * queda en BYPASS aunque la fórmula neutra cambie).
     */
    fun noteNeutralOff(effectsOff: Boolean) {
        synchronized(lock) {
            if (bypassActive) return
            val desired = if (effectsOff) MasterState.LIMPIO_OFF else MasterState.ACTIVO
            if (_currentState.value == desired) return
            _currentState.value = desired
            // El fan-out de la transición neutra YA lo hizo
            // EffectsOffTransition.applyOff/applyOn (JNI + SET_PERCEPTUAL_STATE
            // neutro + pushRouteBState al volver a ON). El Maestro solo publica.
            Log.d(TAG, "noteNeutralOff: -> $desired")
        }
    }

    /**
     * Única publicación del estado. Idempotente: sin cambio real no hay
     * JNI ni socket. Fan-out ordenado en cambio real.
     */
    fun submitState(state: MasterState, source: String) {
        synchronized(lock) {
            if (_currentState.value == state) return
            val prev = _currentState.value
            _currentState.value = state
            bypassActive = (state == MasterState.BYPASS)
            // 1. Flag de bypass (Ruta A bit-exacta). DSPBridge.setBypass
            // propaga al mismo flag nativo g_masterBypass que
            // AudioEngine.setBypass (método de instancia de la clase
            // AudioEngine, sin acceso desde aquí); un solo canal, sin
            // duplicar. try/catch explícito: la cadena runCatching no
            // infiere el tipo en CI (mismo patrón que 0037/0040).
            try {
                DSPBridge.setBypass(state == MasterState.BYPASS)
            } catch (e: Exception) {
                Log.w(TAG, "submitState($source): setBypass falló: ${e.message}")
            }
            // 2. Al volver a ACTIVO, re-publicar Ruta B con el gate vigente
            //    (restaura la cadena viva; idempotente).
            if (state == MasterState.ACTIVO) {
                appContext?.let { ctx ->
                    runCatching { PersistedStateRestorer.pushRouteBState(ctx) }
                        .onFailure { Log.w(TAG, "submitState($source): pushRouteBState falló: ${it.message}") }
                }
            }
            Log.d(TAG, "submitState($source): $prev -> $state")
        }
    }
}
