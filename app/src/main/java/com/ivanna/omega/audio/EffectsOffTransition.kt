// EffectsOffTransition.kt
// ============================================================================
// P2 REAL — "TODO APAGADO" CON BYPASS OFF (2.5.461, Movimiento 2)
// Con bypass == false, cuando todo está apagado (ORBE/HOLOS/OMNI/HYPER en 0,
// Volterra OFF y AdaptiveMode OFF), la cadena base quedaba encendida
// (multibanda, EvoEQ Harman, loudness trim, WFS, RIR, PDEngine modo 1):
// ese es el trío sin interruptor que produce el bofo/hueco (0035).
// Este helper es la ÚNICA transición: evalúa la fórmula y, SOLO al cambiar
// de estado, apaga la cadena base (OFF) o la restaura (ON).
// NO vive en DspStateUpdater (orden de Esse): es un objeto aparte que los
// sitios de escritura invocan tras persistir.
// © 2026 Luis Uriel Pimentel Pérez
// ============================================================================

package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.OmegaEngine
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.core.PersistedStateRestorer
import com.ivanna.omega.magisk.OmegaEngineBridge
import com.ivanna.omega.ui.SpatialAudioPrefs

/**
 * Transición única "todo apagado" (Movimiento 2, 2.5.461).
 *
 * effectsOff = orbe<=0 && holos<=0 && omni<=0 && hyper<=0
 *   && !volterraEnabled && adaptiveMode==OFF.
 *
 * - Solo actúa en la TRANSICIÓN (lastEffectsOff); repetir la evaluación
 *   sin cambio no ejecuta JNI ni altera estado (idempotente).
 * - Fail-closed: sin librería nativa, sin datos o con cualquier fallo,
 *   no se toca nada; la transición NO se marca, así que el próximo
 *   evaluate() la reintenta.
 * - Hilos: los sitios de invocación son callbacks UI (hilo main) salvo
 *   PersistedStateRestorer.restore() (suspend, hilo no verificado). La
 *   transición COMPLETA (lectura + decisión + JNI) va bajo UN solo lock:
 *   sin él, dos transiciones opuestas podían ejecutarse a la vez e
 *   interleavar los setters nativos (centinela, ronda 2). El lock es
 *   PRIVADO de este helper: el hilo de audio jamás lo toma y los JNI
 *   de dentro son escrituras de control no bloqueantes (atómicos /
 *   mutex breve nativo, sin espera al hilo de audio), así que no hay
 *   inversión de prioridad ni deadlock con el audio — a diferencia del
 *   lock de DSPState del Movimiento 1, que vivía en la ruta de
 *   actualización de estado. Tampoco hay reentrancia: ningún setter
 *   vuelve a llamar a evaluate().
 * - La marca (lastEffectsOff) y el reseteo tras fallo ocurren en la
 *   MISMA sección crítica: ningún hilo ve jamás una transición
 *   marcada pero no aplicada. El modo guardado solo se limpia tras
 *   un ON exitoso, dentro del lock; un reintento tras fallo parcial
 *   reusa el modo guardado, nunca lo mezcla con el vivo.
 * - Caso borde documentado (SUPERSEDED en Fase 5): antes, si el usuario
 *   movía sliders EQ/width con effectsOff==true, DspStateUpdater/DSPState
 *   re-empujaban sus valores ("acción explícita del usuario manda"). Fase 5
 *   lo cierra: el Maestro (MasterStateController) bloquea la re-emisión
 *   activa en LIMPIO_OFF (DSPState.pushToNative emite neutro y
 *   DspStateUpdater no escribe); el usuario vuelve a ACTIVO cambiando un
 *   amount/modo, que dispara evaluate() y restaura la cadena viva.
 */
object EffectsOffTransition {
    private const val TAG = "EffectsOffTransition"
    private const val HIGH_CUT_OFF_HZ = 20000f
    private const val HIGH_CUT_ON_HZ = 19500f

    @Volatile
    private var lastEffectsOff: Boolean? = null

    /**
     * Lectura rápida del estado neutro/apagado (0038+0040): true si la última
     * evaluación dejó todo apagado. La usan los emisores periódicos (poll de
     * 500 ms, republishOmegaEq, Iso226, DspStateUpdater) para no re-encender
     * ni re-colorear el DSP 500 ms después de haberlo apagado.
     */
    fun isNeutralOffActive(): Boolean = lastEffectsOff == true

    // Modo NHO previo a entrar en OFF (null = nunca se entró en OFF: al
    // volver a ON se conserva el modo vigente, nunca se fuerza 0).
    @Volatile
    private var savedModeBeforeOff: Int? = null

    private val lock = Any()

    /**
     * Evalúa la fórmula con el estado persistido ACTUAL y aplica la
     * transición solo si cambió. Nunca lanza: cualquier fallo se loguea
     * y se sale sin tocar nada. Toda la transición va bajo UN solo
     * lock (ver KDoc): la marca solo se escribe si la aplicación
     * tuvo éxito; si falla, no se marca nada y el próximo evaluate()
     * la reintenta con el mismo modo guardado.
     */
    fun evaluate(context: Context) {
        if (!IvannaNativeLib.isLoaded) return
        val ctx = context.applicationContext
        synchronized(lock) {
            val effectsOff = runCatching {
                // Unificación 2.5.509 (paso 1): la fórmula lee LO FUSIONADO
                // directamente (una sola carga), no los getters legacy.
                val fused = ParameterStore(ctx).loadFusedControls()
                fused.spatial.amount <= 0f &&
                    fused.field.amount <= 0f &&
                    fused.field.omniAmount <= 0f &&
                    fused.field.phiAmount <= 0f &&
                    fused.transducer.correction <= 0f &&
                    AudioStateManager.getCurrentState().adaptiveMode == AdaptiveMode.OFF
            }.getOrNull() ?: return
            if (lastEffectsOff == effectsOff) return
            val ok: Boolean
            if (effectsOff) {
                // Solo se guarda si no hay uno guardado: un reintento
                // tras fallo parcial no debe pisarlo con el modo ya
                // neutralizado (0).
                if (savedModeBeforeOff == null) {
                    savedModeBeforeOff = OmegaEngine.getMode()
                }
                ok = applyOff()
            } else {
                ok = applyOn(ctx)
            }
            if (ok) {
                lastEffectsOff = effectsOff
                if (!effectsOff) savedModeBeforeOff = null
                // FASE 5: avisar al Maestro (única fuente de la verdad del
                // estado). Sincroniza LIMPIO_OFF/ACTIVO sin tocar BYPASS.
                MasterStateController.noteNeutralOff(effectsOff)
            }
            // Si falló: lastEffectsOff queda como estaba y el modo
            // guardado se conserva; el próximo evaluate() reintenta.
        }
    }

    /** Cadena base a transparente: modo DSP puro + 4 toggles OFF + sin sala. */
    private fun applyOff(): Boolean = runCatching {
        OmegaEngine.setMode(0)
        IvannaNativeLib.nativeSetMultibandEnabled(false)
        IvannaNativeLib.nativeSetEvolutionaryEqEnabled(false)
        IvannaNativeLib.nativeSetLoudnessTrimEnabled(false)
        IvannaNativeLib.nativeSetWfsEnabled(false)
        OmegaEngineBridge.disableRoom()
        OmegaEngineBridge.setFatigueHighCutLocal(HIGH_CUT_OFF_HZ)
        IvannaNativeLib.nativeSetEQParams(0f, 0f, 0f, 0f)
        IvannaNativeLib.nativeSetSpatialWidthDirect(1.0f)
        IvannaNativeLib.nativeSetHarmonicGain(0f)
        // 0038+0040 Parche D: limpiar omega_shm con un SET_PERCEPTUAL_STATE
        // neutro explícito, para que el snapshot coloreado no sobreviva en
        // el daemon aunque luego se cierre IVANNA. No marca fallo si el
        // daemon no está (los toggles locales ya quedaron en neutro).
        // Unificación 2.5.509 (paso 3): el neutro lo arma el constructor
        // único sobre el mapa neutro fusionado + los 6 campos generales
        // (sin slot fused: compressor/loudnessTarget/exciterRed/
        // harmonicGain/antiDolby/holosEqTiltDb). Mismos valores que el
        // JSON a mano que había antes (compressor 0 = bypass, etc.).
        runCatching {
            val fields = FusedControlsBridge.buildFusedPerceptualFields(FusedControls().setNeutral())
                .toMutableMap().apply {
                    put("compressor", 0f)
                    put("loudnessTarget", 0f)
                    put("exciterRed", 0f)
                    put("harmonicGain", 0f)
                    put("antiDolby", 0f)
                    put("holosEqTiltDb", 0f)
                }
            val ok = OmegaEngineBridge.sendCommand(
                com.ivanna.omega.magisk.PerceptualStatePayload.buildSet(
                    fields, AudioRouteManager.routeBStateFlow.value.output
                )
            )
            if (!ok) Log.w(TAG, "applyOff: SET_PERCEPTUAL_STATE neutro no llegó al daemon")
        }.onFailure { Log.w(TAG, "applyOff: SET_PERCEPTUAL_STATE neutro falló: ${it.message}") }
    }.onFailure { Log.w(TAG, "applyOff falló: ${it.message}") }.isSuccess

    /** Restaura el estado activo: modo guardado + 4 toggles ON + sala + EQ/width. */
    private fun applyOn(context: Context): Boolean = runCatching {
        val mode = savedModeBeforeOff ?: OmegaEngine.getMode()
        val state = AudioStateManager.getCurrentState()
        OmegaEngine.setMode(mode)
        IvannaNativeLib.nativeSetMultibandEnabled(true)
        IvannaNativeLib.nativeSetEvolutionaryEqEnabled(true)
        IvannaNativeLib.nativeSetLoudnessTrimEnabled(true)
        IvannaNativeLib.nativeSetWfsEnabled(true)
        val prefs = SpatialAudioPrefs.load(context.applicationContext)
        if (prefs.rirEnabled) {
            OmegaEngineBridge.setRoom(prefs.rirRt60, prefs.rirWet)
        }
        OmegaEngineBridge.setFatigueHighCutLocal(HIGH_CUT_ON_HZ)
        IvannaNativeLib.nativeSetEQParams(
            state.eqBass.coerceIn(-18f, 18f),
            state.eqMid.coerceIn(-18f, 18f),
            state.eqTreble.coerceIn(-18f, 18f),
            state.masterGain.coerceIn(0.1f, 2f)
        )
        IvannaNativeLib.nativeSetSpatialWidthDirect(
            state.spatialWidth.coerceIn(0f, 2f))
        // La ganancia armónica viva es exciterAmount (DspStateUpdater
        // :110 y AudioStateManager :207 la empujan así); el 0f de OFF se
        // restaura aquí, no se deja en 0.
        IvannaNativeLib.nativeSetHarmonicGain(
            state.exciterAmount.coerceIn(0f, 1f))
        // 0038+0040 Parche D: al volver a ON, re-enviar el estado de Ruta B
        // al daemon (con el gate por dispositivo vigente) para restaurar la
        // cadena completa. No marca fallo si no llega.
        runCatching { PersistedStateRestorer.pushRouteBState(context) }
            .onFailure { Log.w(TAG, "applyOn: pushRouteBState falló: ${it.message}") }
    }.onFailure { Log.w(TAG, "applyOn falló: ${it.message}") }.isSuccess
}
