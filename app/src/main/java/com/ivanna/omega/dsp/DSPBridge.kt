package com.ivanna.omega.dsp

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.NativeLibraryLoader
import com.ivanna.omega.core.OEMTelemetry

/**
 * IVANNA-OMEGA-SUPREME — DSP Bridge
 * Wraps libivanna_omega.so, providing the full DSP chain:
 *   GainStage → HarmonicExciter → Compressor → ParametricEQ → StereoWidener → GainStage(out)
 *
 * Source lineage: IVANNA-FUSION-PRO (all FIX patches applied)
 */
object DSPBridge {

    private const val TAG = "IVANNA_OMEGA_DSP"
    private val loaded = NativeLibraryLoader.ensureLoaded()

    val isLoaded: Boolean get() = loaded

    // FIX (persistencia 2026-09-01): los parámetros DSP se pierden al matar la app.
    // Se agrega SharedPreferences para guardar/restaurar estado entre sesiones.
    private const val PREFS_NAME = "ivanna_dsp_state_v1"
    private var prefs: SharedPreferences? = null

    fun initPreferences(context: Context) {
        prefs = context.applicationContext.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    }

    fun saveState(drive: Float, wet: Float, mix: Float, master: Float, stereoWidth: Float) {
        prefs?.edit()?.apply {
            putFloat("drive", drive)
            putFloat("wet", wet)
            putFloat("mix", mix)
            putFloat("master", master)
            putFloat("stereoWidth", stereoWidth)
            apply()
        }
    }

    fun loadState(): Map<String, Float> {
        return mapOf(
            "drive" to (prefs?.getFloat("drive", 0.5f) ?: 0.5f),
            "wet" to (prefs?.getFloat("wet", 0.5f) ?: 0.5f),
            "mix" to (prefs?.getFloat("mix", 0.5f) ?: 0.5f),
            "master" to (prefs?.getFloat("master", 1.0f) ?: 1.0f),
            "stereoWidth" to (prefs?.getFloat("stereoWidth", 0.5f) ?: 0.5f)
        )
    }

    // FIX (auditoría 2026-08-24): el default 96000 era una trampa latente —
    // un call-site futuro que lo invocara sin argumento reintroduciría el bug
    // de SR fijo (filtros desafinados) ya reparado en IVANNAApplication y
    // BootRestoreReceiver. Sin default: el compilador obliga a pasar SR real.
    fun init(sampleRate: Int) {
        // Seguridad anti-piratería (v2.5.369): si la integridad falló
        // (IntegrityGate.isLocked), no inicializar el DSP. Fail-closed:
        // el audio pasa en limpio y el teléfono sigue funcionando.
        if (com.ivanna.omega.security.IntegrityGate.isLocked) return
        if (loaded) {
            nativeInit(sampleRate)
            // Bypass maestro (2.5.461): reenviar el último valor pedido si
            // setBypass() se llamó antes de init() (el flag nativo es
            // process-global y el store es idempotente: reenviar el mismo
            // valor no cambia nada, pero cierra la ventana de orden).
            pendingBypass?.let { nativeSetBypass(it) }
        }
    }

    fun setParams(
        drive: Float, wet: Float, mix: Float,
        alpha: Float, beta: Float, gamma: Float,
        freq: Float, resonance: Float,
        low: Float, mid: Float, high: Float,
        presence: Float, master: Float
    ) {
        if (!loaded) return
        nativeSetParams(drive, wet, mix, alpha, beta, gamma, freq, resonance, low, mid, high, presence, master)
        if (IvannaNativeLib.isLoaded) {
            runCatching {
                IvannaNativeLib.nativeSetParams(
                    floatArrayOf(drive, wet, mix, alpha, beta, gamma, freq, resonance, low, mid, high, presence, master)
                )
            }
        }
    }

    // FIX (tuning magistral): antes el ancho estéreo (DSPState.stereoWidth)
    // nunca llegaba al motor nativo — StereoWidener derivaba el ancho de
    // "gamma", que también controla el timing del compresor (colisión de
    // parámetros). Canal dedicado, sin relación con setParams()/gamma.
    fun setStereoWidth(width: Float) {
        if (!loaded) return
        nativeSetStereoWidth(width)
    }

    // Bypass maestro (2.5.461): propaga DSPState.bypass al nativo. Mismo
    // patrón que setStereoWidth (guarda `loaded`, JNI dedicado), con una
    // diferencia: el valor pedido se guarda SIEMPRE en pendingBypass
    // (@Volatile) antes de decidir si llamar al JNI, para que nunca se
    // pierda en silencio si la lib aún no cargó — init() lo reenvía.
    // `loaded` es un val (sin carrera: NativeLibraryLoader.ensureLoaded()
    // es one-shot con AtomicBoolean + @Volatile); si el load falló, la
    // lib no existe en este proceso y ningún setter funcionaría.
    @Volatile
    private var pendingBypass: Boolean? = null

    fun setBypass(bypass: Boolean) {
        pendingBypass = bypass
        if (!loaded) return
        nativeSetBypass(bypass)
    }

    // FEATURE (Voice Protection): score 0..1 de voz detectada
    // (VoiceProtectionController, YamnetClassifier real). Canal dedicado,
    // mismo patrón que setStereoWidth.
    fun setVoiceProtectScore(score: Float) {
        if (!loaded) return
        nativeSetVoiceProtectScore(score)
    }

    /** Clear a non-inference score and mark voice_score_valid=false natively. */
    fun clearVoiceProtectScore() {
        if (!loaded) return
        nativeClearVoiceProtectScore()
    }

    fun setAdaptiveParams(params: FloatArray) {
        if (!loaded) return

        fun at(index: Int, default: Float): Float =
            if (index < params.size) params[index] else default

        setParams(
            drive = at(0, 0f),
            wet = at(1, 0f),
            mix = at(2, 0f),
            alpha = at(3, 0f),
            beta = at(4, 0f),
            gamma = at(5, 0f),
            freq = at(6, 1000f),
            resonance = at(7, 0.7f),
            low = at(8, 0f),
            mid = at(9, 0f),
            high = at(10, 0f),
            presence = at(11, 0f),
            master = at(12, 0f)
        )
    }

    fun process(buffer: FloatArray, numFrames: Int) {
        if (!loaded) return
        // Telemetría real del DSP (F2 / cortes BridgePlayer, 2026-11-14):
        // OEMTelemetry publicaba dspProcessingUs = 0 SIEMPRE porque
        // reportFrameProcessed() no tenía NINGÚN llamador en el repo
        // (verificado por grep): el snapshot del puente no podía decir si
        // el DSP corría ni cuánto tardaba por bloque. Dos nanoTime por
        // bloque (2048 frames) no mueven el presupuesto RT. Sin native
        // cargado no se reporta nada: 0 = sin DSP, no "DSP instantáneo".
        val t0 = System.nanoTime()
        nativeProcess(buffer, numFrames)
        OEMTelemetry.reportFrameProcessed((System.nanoTime() - t0) / 1_000L)
    }

    fun reset() { if (loaded) nativeReset() }

    // ── AUDIT FIX PR 4: Métodos para que PerceptualCortex envíe parámetros DSP ──────
    /**
     * Aplicar ganancia calculada por PerceptualCortex.
     * Rango: 0.0..2.0 (0 = mute, 1 = unity, 2 = 6dB boost)
     *
     * @param gain ganancia perceptual calculada
     */
    fun applyPerceptualGain(gain: Float) {
        // FIX (desconexión): `loaded` refleja NativeLibraryLoader, pero estos
        // métodos llaman a IvannaNativeLib — otra librería con su propio
        // estado de carga. Si ésta falló, UnsatisfiedLinkError en caliente.
        if (!loaded || !IvannaNativeLib.isLoaded) return
        Log.d(TAG, "applyPerceptualGain: $gain")
        runCatching { IvannaNativeLib.nativeSetPerceptualGain(gain.coerceIn(0f, 2f)) }
    }

    /**
     * Aplicar compresión calculada por PerceptualCortex.
     * Rango: 0.0..1.0 (0 = no compression, 1 = máxima compresión)
     *
     * @param amount compresión calculada
     */
    fun applyCompressorAmount(amount: Float) {
        if (!loaded || !IvannaNativeLib.isLoaded) return
        Log.d(TAG, "applyCompressorAmount: $amount")
        runCatching { IvannaNativeLib.nativeSetCompressorAmount(amount.coerceIn(0f, 1f)) }
    }

    /**
     * Aplicar reducción de exciter calculada por PerceptualCortex.
     * Rango: 0.0..1.0 (0 = máximo exciter, 1 = sin exciter)
     *
     * @param amount reducción de exciter
     */
    fun applyExciterReduction(amount: Float) {
        if (!loaded || !IvannaNativeLib.isLoaded) return
        Log.d(TAG, "applyExciterReduction: $amount")
        runCatching { IvannaNativeLib.nativeSetExciterReduction(amount.coerceIn(0f, 1f)) }
    }

    /**
     * Aplicar ancho espacial calculado por PerceptualCortex.
     * Rango: 0.0..2.0 (0.5 = mono, 1.0 = stereo normal, 2.0 = extra wide)
     *
     * @param width ancho espacial calculado
     */
    fun applySpatialWidth(width: Float) {
        if (!loaded || !IvannaNativeLib.isLoaded) return
        Log.d(TAG, "applySpatialWidth: $width")
        runCatching { IvannaNativeLib.nativeSetSpatialWidth(width.coerceIn(0.5f, 2f)) }
    }

    /**
     * Aplicar EQ calculado por PerceptualCortex.
     * Rango: ±12 dB en 3 bandas (low, mid, high)
     *
     * @param lowDb ganancia en bajos
     * @param midDb ganancia en medios
     * @param highDb ganancia en altos
     */
    fun applyPerceptualEQ(lowDb: Float, midDb: Float, highDb: Float) {
        if (!loaded || !IvannaNativeLib.isLoaded) return
        Log.d(TAG, "applyPerceptualEQ: low=$lowDb, mid=$midDb, high=$highDb")
        runCatching {
            IvannaNativeLib.nativeSetPerceptualEQ(
                lowDb.coerceIn(-12f, 12f),
                midDb.coerceIn(-12f, 12f),
                highDb.coerceIn(-12f, 12f)
            )
        }
    }


    // ── Adaptive DSP State bridge ───────────────────────────────────────
    // Ruta:
    // Perceptual AI → AdaptiveDSPState → DSPBridge → JNI → DSP

    fun applyAdaptiveState(state: AdaptiveDSPState) {
        if (!loaded || !IvannaNativeLib.isLoaded) return

        applyPerceptualGain(state.gain)

        applyCompressorAmount(
            state.compressor
        )

        applyExciterReduction(
            state.exciter
        )

        applySpatialWidth(
            state.spatial
        )

        applyPerceptualEQ(
            state.lowEqDb,
            state.midEqDb,
            state.highEqDb
        )

        applyFatigueProtection(
            state.iso226Compensation,
            state.fatigueProtection
        )
    }

    private fun applyFatigueProtection(
        iso226: Float,
        fatigue: Float
    ) {
        if (!loaded || !IvannaNativeLib.isLoaded) return

        runCatching {
            IvannaNativeLib.nativeSetFatigueProtection(
                iso226.coerceIn(-12f, 12f),
                fatigue.coerceIn(0f, 1f)
            )
        }
    }

    fun version(): String = if (loaded) nativeVersion() else "native unavailable"

    /**
     * Estado REAL del efecto omega_effect.so en audioserver (Ruta B), leido del beacon que el
     * propio efecto mantiene. A diferencia de MagiskBridge.isDaemonRunning (solo prueba que el
     * proceso daemon vive), esto prueba que el efecto esta insertado/procesando.
     */
    enum class EffectState { UNAVAILABLE, NO_EFFECT, ENABLED_IDLE, PROCESSING }

    /** UNAVAILABLE => el beacon no se puede leer (DAC/SELinux o libreria no cargada): el llamador debe conservar su comportamiento previo. */
    fun effectState(idleMs: Int = 2000): EffectState {
        if (!loaded) return EffectState.UNAVAILABLE
        return when (runCatching { nativeEffectBeaconState(idleMs) }.getOrDefault(-1)) {
            0 -> EffectState.NO_EFFECT
            1 -> EffectState.ENABLED_IDLE
            2 -> EffectState.PROCESSING
            else -> EffectState.UNAVAILABLE
        }
    }

    /**
     * v2.5.386 — Frames acumulados procesados por el efecto (beacon).
     * -1 = beacon no disponible: el llamador debe conservar su comportamiento
     * previo. El watchdog de Ruta B lo usa para detectar el efecto huérfano
     * (frames congelados mientras la sesión descubierta sigue sonando).
     */
    fun effectBeaconFrames(): Long {
        if (!loaded) return -1L
        return runCatching { nativeEffectBeaconFrames() }.getOrDefault(-1L)
    }

    /**
     * FUSIÓN MAGISTRAL (v2.5.401, ABI v12) — Estado de la fusión para la APK.
     * Lo escribe el cerebro (daemon v2) en /data/local/tmp/ivanna_fusion_heartbeat
     * (~1 Hz con throttle 2.5.461, antes ~20 Hz) y lo lee el JNI. null = sin fusión / heartbeat obsoleto (el llamador
     * conserva su comportamiento previo, fail-closed). fusionActive=true significa
     * que el HAL ya está procesando TODO el audio (ASRC v6 + campo Ψ): la Ruta A
     * debe ceder para no procesar dos veces.
     */
    data class FusionState(
        val active: Boolean,
        val eta: Float,
        val psiGamma: Float,
        val psiActive: Boolean,
        val verdict: Int,
        val ratio: Float,
        val driftPpm: Float,
        val clock: Int,
    )

    fun fusionState(): FusionState? {
        if (!loaded) return null
        val out = FloatArray(9)
        val ok = runCatching { nativeFusionState(out) }.getOrDefault(0)
        if (ok != 1) return null
        return FusionState(
            active = out[0] != 0f,
            eta = out[1],
            psiGamma = out[2],
            psiActive = out[3] != 0f,
            verdict = out[4].toInt(),
            ratio = out[5],
            driftPpm = out[6],
            clock = out[7].toInt(),
        )
    }

    // ── Reinyeccion diferencial de Ruta A (v2.5.0) ──────────────────────────
    // Output = Dry + g_banda * Delta; aqui solo se calcula el termino Delta*g (el Dry ya suena por la via original).
    fun reinjectPrepare(sampleRate: Int, maxFrames: Int) { if (loaded) nativeReinjectPrepare(sampleRate, maxFrames) }
    fun reinjectReset() { if (loaded) nativeReinjectReset() }
    /** Intensidad perceptual 0..1 (slider / rampa de arranque). */
    fun reinjectSetIntensity(v: Float) { if (loaded) nativeReinjectSetIntensity(v) }
    /** Latencia real que ve el oyente entre el original y lo reinyectado (ms). */
    fun reinjectSetLatencyMs(ms: Float) { if (loaded) nativeReinjectSetLatencyMs(ms) }
    /** Techos por banda (graves/medios/agudos), 0..1. */
    fun reinjectSetBandCaps(low: Float, mid: Float, high: Float) { if (loaded) nativeReinjectSetBandCaps(low, mid, high) }
    /** false => el motor nativo no esta listo; el llamador usa el delta simple. */
    fun reinjectProcess(dry: FloatArray, wet: FloatArray, out: FloatArray, numFrames: Int): Boolean =
        loaded && nativeReinjectProcess(dry, wet, out, numFrames)
    /** Telemetria real: [latMs, dspLag, lagConf, coh x3, delta/seco x3, ganancia x3, riesgoPeine, inmersion, techo, 0]. */
    fun reinjectTelemetry(out: FloatArray): Boolean = loaded && out.size >= 16 && nativeReinjectTelemetry(out) > 0

    private external fun nativeReinjectPrepare(sampleRate: Int, maxFrames: Int)
    private external fun nativeReinjectReset()
    private external fun nativeReinjectSetIntensity(v: Float)
    private external fun nativeReinjectSetLatencyMs(ms: Float)
    private external fun nativeReinjectSetBandCaps(lo: Float, mid: Float, hi: Float)
    private external fun nativeReinjectProcess(dry: FloatArray, wet: FloatArray, out: FloatArray, nFrames: Int): Boolean
    private external fun nativeReinjectTelemetry(out: FloatArray): Int

    private external fun nativeInit(sampleRate: Int)
    private external fun nativeSetParams(
        drive: Float, wet: Float, mix: Float,
        alpha: Float, beta: Float, gamma: Float,
        freq: Float, resonance: Float,
        low: Float, mid: Float, high: Float,
        presence: Float, master: Float
    )
    private external fun nativeSetStereoWidth(width: Float)
    private external fun nativeSetVoiceProtectScore(score: Float)
    private external fun nativeClearVoiceProtectScore()
    

    
    private external fun nativeProcess(buf: FloatArray, numFrames: Int)
    private external fun nativeSetBypass(bypass: Boolean)
    private external fun nativeReset()
    private external fun nativeVersion(): String
    private external fun nativeEffectBeaconState(idleMs: Int): Int
    private external fun nativeEffectBeaconFrames(): Long
    private external fun nativeFusionState(out: FloatArray): Int

    // ── FÍSICA NUEVA (Fase 2, v2.5.472) ──────────────────────────────────────
    /** Empuja los 18 parámetros de física nueva a los atomics g_np_* (Ruta A). */
    fun setNewPhysicsParams(
        amount: Float, lgnMix: Float, bsDamp: Float, torsoMix: Float, headRadiusM: Float,
        azDeg: Float, elDeg: Float, eqClass: Int, omegaMix: Float, boostCapDb: Float,
        xtalkMix: Float, sigmaDistM: Float, splDb: Float, airMix: Float,
        volterraMix: Float, freeEnergyMix: Float, abxMode: Int
    ) {
        if (loaded) nativeSetNewPhysicsParams(
            amount, lgnMix, bsDamp, torsoMix, headRadiusM,
            azDeg, elDeg, eqClass, omegaMix, boostCapDb,
            xtalkMix, sigmaDistM, splDb, airMix,
            volterraMix, freeEnergyMix, abxMode
        )
    }
    /** Carga la calibración medida del Genezi (tri21 L/R + H_b 24 + headroom 24). */
    fun loadNewPhysicsCalibration(
        tri21L: FloatArray, tri21R: FloatArray,
        hb24: DoubleArray, headroom24: DoubleArray, fLo: Double, fHi: Double
    ): Boolean = loaded && nativeLoadNewPhysicsCalibration(tri21L, tri21R, hb24, headroom24, fLo, fHi)
    /** Telemetría en vivo [E_sig, D_max, γ(σ), α(L), fatiga MOC, pico Volterra, cpuUs/hop, kernel]. */
    fun getNewPhysicsTelemetry(): FloatArray =
        if (loaded) runCatching { nativeGetNewPhysicsTelemetry() ?: FloatArray(8) }.getOrDefault(FloatArray(8))
        else FloatArray(8)

    // ── FÍSICA NUEVA v3 (v2.5.483): órganos despiertos ───────────────────────
    /** Despierta los órganos v3 (sub-graves, Klippel H2+H3, cola rir_0101). */
    fun wakeNewPhysicsV3Organs(): Boolean =
        loaded && runCatching { nativeWakeNewPhysicsV3Organs() }.getOrDefault(false)

    /** Una etapa modal sub-graves (idx 1..5; la 0 es el diferencial Ω automático). */
    fun setNewPhysicsSubBassStage(
        idx: Int, freqHz: Double, q: Double, gainDb: Double, lowShelf: Boolean, active: Boolean
    ) {
        if (loaded) runCatching {
            nativeSetNewPhysicsSubBassStage(idx, freqHz, q, gainDb, lowShelf, active)
        }
    }

    /** Mezcla de cola de sala 0..1. */
    fun setNewPhysicsRoomTailMix(mix: Float) {
        if (loaded) runCatching { nativeSetNewPhysicsRoomTailMix(mix) }
    }

    /** Mezcla H3 0..1. */
    fun setNewPhysicsKlippelH3Mix(mix: Float) {
        if (loaded) runCatching { nativeSetNewPhysicsKlippelH3Mix(mix) }
    }

    /**
     * Carga una RIR estéreo como cola de sala (PCM flotante, sr 8k..96k).
     * El JNI recorta (onset+25 ms) y remuestrea a la sr del motor.
     */
    fun loadNewPhysicsRoomTail(
        irL: FloatArray, irR: FloatArray, rirRate: Int, tailMix: Float
    ): Boolean = loaded && runCatching {
        nativeLoadNewPhysicsRoomTail(irL, irR, rirRate, tailMix)
    }.getOrDefault(false)

    /** Telemetría v3 [tempL °C, tempR °C, cola 0/1, colaMix, h3Mix, máscara sub-graves, kernel, picoVolterra, cpuUs/hop, kernelGeneziMedido 0/1]. */
    fun getNewPhysicsV3Telemetry(): FloatArray =
        if (loaded) runCatching { nativeGetNewPhysicsV3Telemetry() ?: FloatArray(10) }.getOrDefault(FloatArray(10))
        else FloatArray(10)

    private external fun nativeSetNewPhysicsParams(
        amount: Float, lgnMix: Float, bsDamp: Float, torsoMix: Float, headRadiusM: Float,
        azDeg: Float, elDeg: Float, eqClass: Int, omegaMix: Float, boostCapDb: Float,
        xtalkMix: Float, sigmaDistM: Float, splDb: Float, airMix: Float,
        volterraMix: Float, freeEnergyMix: Float, abxMode: Int
    )
    private external fun nativeLoadNewPhysicsCalibration(
        tri21L: FloatArray, tri21R: FloatArray,
        hb24: DoubleArray, headroom24: DoubleArray, fLo: Double, fHi: Double
    ): Boolean
    private external fun nativeGetNewPhysicsTelemetry(): FloatArray
    // ── FÍSICA NUEVA v3 (v2.5.483) ───────────────────────────────────────────
    private external fun nativeWakeNewPhysicsV3Organs(): Boolean
    private external fun nativeSetNewPhysicsSubBassStage(
        idx: Int, freqHz: Double, q: Double, gainDb: Double, lowShelf: Boolean, active: Boolean
    )
    private external fun nativeSetNewPhysicsRoomTailMix(mix: Float)
    private external fun nativeSetNewPhysicsKlippelH3Mix(mix: Float)
    private external fun nativeLoadNewPhysicsRoomTail(
        irL: FloatArray, irR: FloatArray, rirRate: Int, tailMix: Float
    ): Boolean
    private external fun nativeGetNewPhysicsV3Telemetry(): FloatArray
}
