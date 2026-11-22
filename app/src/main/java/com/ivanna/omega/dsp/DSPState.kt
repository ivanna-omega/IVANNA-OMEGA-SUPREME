package com.ivanna.omega.dsp

import android.util.Log
import com.ivanna.omega.audio.MasterState
import com.ivanna.omega.audio.MasterStateController
import com.ivanna.omega.audio.ParameterValidator.safe
import com.ivanna.omega.core.IVANNAApplication
import kotlinx.coroutines.launch

/**
 * DSP State — inmutable data class para todos los parámetros DSP.
 * © 2026 Luis Uriel Pimentel Pérez - GORE TNS. All rights reserved.
 */
data class DSPState(
    // Core parameters — CALIBRACIÓN MAGISTRAL v4.0 (alineados con dsp_types.h + ParameterStore)
    val drive: Float     = 0.48f,  // Drive rico en armónicos pares sin aliasing
    val wet: Float       = 0.38f,  // Exciter wet 38% (presencia cristalina y cuerpo)
    // CALIBRACION DE ARRANQUE (gain staging): GainStage.inputGain_ =
    // dbToLin((mix - 0.5) * 12). Con mix=0.72 el audio recibia +2.64 dB ANTES
    // del EQ, y con master=+0.8 dB otros +0.8 dB a la salida: +3.4 dB estaticos
    // que empujan los picos de un master moderno (~-0.5..-1 dBFS) por encima de
    // 0 dBFS y obligan al SafetyLimiter (umbral -4 dBFS) a trabajar en cada
    // bloque fuerte -> bombeo/crujido al subir nivel. dsp_types.h ya documenta
    // este mismo defecto para DSPParams. 0.50 = 0 dB (neutro); el volumen lo
    // pone el sistema, no una ganancia oculta.
    val mix: Float       = 0.50f,
    val alpha: Float     = 0.35f,  // Comp threshold -15.6 dB (preserva micro-dinámica)
    val beta: Float      = 0.08f,  // Ratio ~2.5:1 (musical y cinematográfico)
    val gamma: Float     = 0.72f,  // Attack 31ms, release 176ms
    val freq: Float      = 1000f,
    val resonance: Float = 0.707f,

    // EQ gains (dB) — Curva Magistral (autoridad sub-grave, vocales claras, aire 3D)
    var low: Float      = 2.2f,
    var mid: Float      = 0.8f,
    var high: Float     = 1.8f,
    var presence: Float = 1.6f,
    var master: Float   = 0.0f,   // dB; 0 = sin ganancia oculta (ver comentario de mix)

    // Compressor
    val compThreshold: Float = -16.0f,
    val compRatio: Float     =   2.4f,

    // Exciter
    val exciterDrive: Float = 0.38f,

    // Stereo
    val stereoWidth: Float = 1.32f,
    val makeupGain: Float  = 0.0f,

    // Bypass
    val bypass: Boolean = false
) {
    /** EQ gains array para JNI */
    val eqGains: FloatArray
        get() = floatArrayOf(low, mid, high, presence)

    /**
     * Envía todos los parámetros al DSP nativo vía DSPBridge.
     *
     * FIX (tuning magistral): stereoWidth nunca se enviaba — nativeSetParams
     * no lo incluía en su firma. Ahora va por un canal JNI dedicado
     * (nativeSetStereoWidth), separado de gamma (que sólo controla timing
     * del compresor).
     *
     * FIX (auditoría del módulo Magisk — hallazgo crítico): IVANNAApplication.
     * omegaBridge (el cliente socket hacia el daemon system-wide) sólo se
     * usaba para connect()/disconnect() — ningún control de la UI le llegaba
     * nunca. El daemon procesaba TODAS las apps del sistema con los valores
     * por defecto de OmegaSharedState congelados desde el arranque, sordo a
     * cualquier ajuste. Ahora pushToNative() —el único punto de entrada real
     * de cambios de parámetros en toda la app— también espeja hacia el
     * daemon. setPFParams() de OmegaEngineBridge ya maneja reconexión/no-op
     * silencioso si el daemon no está disponible (modo no-root), así que
     * esta llamada es segura incondicionalmente.
     */
    fun pushToNative() {
        // FIX (ParameterValidator estaba huérfano — nadie lo llamaba en
        // todo el repo): estas 13 llamadas cruzan a C++ y luego a un socket
        // Unix system-wide (ver más abajo); un NaN/Infinity o un valor fuera
        // de rango aquí llega tal cual al DSP nativo y al daemon del
        // sistema, que procesa TODAS las apps del sistema. Se sanea con
        // .safe() (extension ya existente en ParameterValidator, solo que
        // nunca se invocaba) usando los rangos reales de cada slider de
        // este mismo archivo (sliderToDrive, sliderToDb, etc. arriba).
        val safeDrive     = drive.safe(0.45f, 0f, 4f)
        val safeWet       = wet.safe(0.32f, 0f, 1f)
        val safeMix       = mix.safe(0.70f, 0f, 1f)
        val safeAlpha     = alpha.safe(0.375f, 0f, 1f)
        val safeBeta      = beta.safe(0.105f, 0f, 1f)
        val safeGamma     = gamma.safe(0.72f, 0f, 1f)
        val safeFreq      = freq.safe(1000f, 20f, 20000f)
        val safeResonance = resonance.safe(0.707f, 0.1f, 10f)
        val safeLow       = low.safe(0f, -18f, 18f)
        val safeMid       = mid.safe(0f, -18f, 18f)
        val safeHigh      = high.safe(0f, -18f, 18f)
        val safePresence  = presence.safe(0f, -18f, 18f)
        val safeMaster    = master.safe(0f, -18f, 18f)
        val safeStereoWidth = stereoWidth.safe(1f, 0f, 2f)

        if (safeDrive != drive || safeWet != wet || safeMix != mix ||
            safeAlpha != alpha || safeBeta != beta || safeGamma != gamma ||
            safeFreq != freq || safeResonance != resonance ||
            safeLow != low || safeMid != mid || safeHigh != high ||
            safePresence != presence || safeMaster != master ||
            safeStereoWidth != stereoWidth) {
            Log.w("DSPState", "pushToNative: valor(es) fuera de rango o no finito(s) — clamped antes de enviar al DSP nativo")
        }

        // FASE 5 — Maestro: sincronizar el estado en cada push (pushToNative
        // corre en cada tick de slider y al aplicar perfiles: es el punto de
        // sincronización frecuente). Y cerrar el hueco 1: con LIMPIO_OFF
        // activo, NO re-emitir valores activos (un slider movido con todo
        // apagado re-coloreaba el DSP detrás de applyOff; el KDoc de
        // EffectsOffTransition lo documentaba como "acción del usuario
        // manda" — Fase 5 lo supersede: LIMPIO_OFF = todo apagado real;
        // el usuario vuelve a ACTIVO cambiando un amount/modo, que dispara
        // evaluate() y restaura la cadena viva).
        // La emisión neutra sigue EXACTAMENTE lo que applyOff neutraliza:
        // EQ a 0 y width a 1.0 (drive/wet/mix/alpha/beta/gamma/freq/
        // resonance NO los toca applyOff, así que tampoco se tocan aquí).
        val neutralOff = MasterStateController.isNeutralOffActive()
        val neutralEmission = bypass || neutralOff
        MasterStateController.submitState(
            when {
                bypass -> MasterState.BYPASS
                neutralOff -> MasterState.LIMPIO_OFF
                else -> MasterState.ACTIVO
            }, "pushToNative"
        )

        DSPBridge.setParams(
            safeDrive, safeWet, safeMix,
            safeAlpha, safeBeta, safeGamma,
            safeFreq, safeResonance,
            if (neutralEmission) 0f else safeLow,
            if (neutralEmission) 0f else safeMid,
            if (neutralEmission) 0f else safeHigh,
            if (neutralEmission) 0f else safePresence,
            if (neutralEmission) 0f else safeMaster
        )
        DSPBridge.setStereoWidth(if (neutralEmission) 1.0f else safeStereoWidth)

        // Bypass maestro (2.5.461): propagar DSPState.bypass al nativo.
        // El flag hace que nativeProcess retorne antes de tocar el buffer
        // (bit-exacto). SIN P2 aquí: el neutro-en-off de la cadena base
        // va en el Movimiento 2 (helper de transición), no en pushToNative.
        DSPBridge.setBypass(bypass)

        // FIX CRÍTICO (causa real de trabazón/crash al mover sliders):
        // omegaBridge.setPFParams() hace I/O de socket Unix síncrono (13
        // comandos de texto en serie, cada uno con su propio write()+flush(),
        // y potencialmente un connect() bloqueante si la conexión se cayó).
        // pushToNative() se llama en CADA tick de CUALQUIER slider del DSP
        // (onExciterChange/onEqChange/onWidthChange/onCompThresholdChange/
        // etc.), disparado desde Compose en el hilo principal — eso
        // significa que se estaba haciendo I/O de red síncrono en el hilo
        // de UI decenas de veces por segundo mientras se arrastraba un
        // slider. Combinado con el bug del lado del daemon (que cerraba la
        // conexión tras cada comando, forzando reconexión en cada uno de
        // los 13 — ver fix en omega_daemon.cpp/socketLoop), esto producía
        // ANR/trabazón real y, en carga sostenida, el sistema mataba el
        // proceso. Con el daemon ya arreglado esto es mucho más rápido,
        // pero I/O de red síncrono en el hilo principal sigue siendo
        // arquitectónicamente incorrecto — se despacha al appScope (IO)
        // igual que se hizo con LearningBias.captureCorrection().
        // Actualizar AudioEffect sessions (Spotify/YouTube/etc) con sliders actuales.
        // mid == EQ global dB (la UI setea low=mid=high=presence al mismo valor).
        // alpha/beta → compressor threshold/ratio con la misma formula que el C++.
        //
        // FIX (causa real confirmada del crash/trabazón al mover sliders o
        // aplicar un perfil): el comentario de arriba decía que esto ya iba
        // a appScope, pero la llamada seguía siendo síncrona acá mismo, en
        // el hilo que invoca pushToNative() — que es el hilo principal tanto
        // para los onValueChange de los sliders (MainActivity) como para
        // ProfileManagerBridge.applyProfile() (dispara desde el onClick de
        // seleccionar perfil). adjustLiveParams() itera TODAS las sesiones
        // de audio activas (Spotify/YouTube/etc.) y por cada una hace varias
        // llamadas AudioEffect.set*() — cada una es una llamada Binder al
        // audio HAL del sistema. Con una sesión externa activa y arrastrando
        // un slider a 60fps, o al aplicar un perfil, esto podía saturar el
        // hilo principal el tiempo suficiente para que el sistema matara el
        // proceso (ANR). Ahora se despacha a IVANNAApplication.appScope
        // (Dispatchers.IO), igual que ya hace LearningBias.captureCorrection()
        // para su propio I/O.
        val eqGainDbSnapshot = if (neutralEmission) 0f else safeMid
        val stereoWidthSnapshot = if (neutralEmission) 1.0f else safeStereoWidth
        val compThresholdDbSnapshot = if (neutralEmission) 0f else (-24f + safeAlpha * 24f)
        val compRatioSnapshot = if (neutralEmission) 1f else (1f + safeBeta * 19f)
        IVANNAApplication.appScope.launch {
            globalEffectManager?.adjustLiveParams(
                eqGainDb        = eqGainDbSnapshot,
                stereoWidth     = stereoWidthSnapshot,
                compThresholdDb = compThresholdDbSnapshot,
                compRatio       = compRatioSnapshot
            )
        }

        // FIX (crash): trySend es non-blocking y O(1).
        // Channel(CONFLATED) en IVANNAApplication descarta valores intermedios
        // — solo el ultimo llega al socket. Elimina la acumulacion de coroutines
        // bloqueadas (CONNECT_TIMEOUT_MS=2000ms x 60fps = OOM en minutos).
        // 0038+0040 Parche D: con bypass, el canal PF NO lleva los valores
        // activos (safeMid/safeMaster/etc. seguían coloreando aunque el
        // snapshot local ya fuera neutro): se envía el array neutro.
        // FASE 5: lo mismo en LIMPIO_OFF (neutralEmission).
        val pfParams = if (neutralEmission) {
            floatArrayOf(0f, 0f, 0.5f, 0f, 0f, safeGamma,
                         safeFreq, safeResonance, 0f, 0f, 0f, 0f, 0f)
        } else {
            floatArrayOf(safeDrive, safeWet, safeMix, safeAlpha, safeBeta, safeGamma,
                         safeFreq, safeResonance, safeLow, safeMid, safeHigh, safePresence, safeMaster)
        }
        IVANNAApplication.pfParamChannel.trySend(pfParams)
    }

    companion object {
        /**
         * Referencia a IvannaGlobalEffectManager seteada por IVANNAApplication.
         * Permite que pushToNative() actualice las sesiones de AudioEffect
         * (Spotify, YouTube, etc.) sin necesitar un Context.
         */
        @Volatile
        var globalEffectManager: com.ivanna.omega.audio.IvannaGlobalEffectManager? = null

        // ── Campos estáticos del PF Engine (escritos desde AudioEngine) ──────
        @JvmField var pfDrive:     Float = 0.65f
        @JvmField var pfWet:       Float = 0.50f
        @JvmField var pfAlpha:     Float = 0.50f
        @JvmField var pfBeta:      Float = 0.50f
        @JvmField var pfDelta:     Float = 0.50f
        @JvmField var pfSigma:     Float = 0.50f
        @JvmField var pfFreq:      Float = 1000f
        @JvmField var pfResonance: Float = 0.707f
        @JvmField var pfMix:       Float = 0.70f
        @JvmField var pfLowGain:   Float = 0.0f
        @JvmField var pfMidGain:   Float = 0.0f
        @JvmField var pfHighGain:  Float = 0.0f
        @JvmField var pfPresence:  Float = 0.0f
        @JvmField var pfAmpModel:  Int   = 0

        /**
         * Slider [0..1] → ganancia dB [-18..+18]
         * Rango ampliado de ±12 a ±18 dB para mayor dinámica.
         */
        fun sliderToDb(slider: Float): Float = slider * 36f - 18f

        /**
         * Ganancia dB [-18..+18] → slider [0..1]
         */
        fun dbToSlider(db: Float): Float = (db + 18f) / 36f

        /**
         * Slider [0..1] → drive con curva logarítmica suave.
         * Permite control fino en valores bajos y agresivo en altos.
         * Rango efectivo: 0.0 (limpio) → 4.0 (saturación máxima).
         */
        fun sliderToDrive(slider: Float): Float =
            (Math.pow(slider.toDouble(), 2.0) * 4.0).toFloat().coerceIn(0f, 4f)

        /**
         * Drive [0..4] → slider [0..1]
         */
        fun driveToSlider(drive: Float): Float =
            Math.sqrt((drive / 4.0).coerceIn(0.0, 1.0)).toFloat()

        /** Slider [0..1] → frecuencia Hz [20..20000] (escala logarítmica) */
        fun sliderToFreq(slider: Float): Float =
            (20.0 * Math.pow(1000.0, slider.toDouble())).toFloat()

        /** Slider [0..1] → factor Q [0.1..10] (escala logarítmica) */
        fun sliderToQ(slider: Float): Float =
            (0.1 * Math.pow(100.0, slider.toDouble())).toFloat()
    }
}
