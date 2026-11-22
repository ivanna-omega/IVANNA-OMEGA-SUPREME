// OrbeHolosAbxSession.kt — Repair 6 / F1 (v2.5.471): ABX ORBE·Ω+HOLOS·Ω ON vs OFF.
//
// Protocolo "ABX_OrbeHolos_On_vs_Off" (v1). 16 ensayos; en cada ensayo
// A = OFF y B = ON (o al revés: lo decide el azar por ensayo, no por
// sesión), X es uno de los dos al azar y el oyente responde "X es igual
// a A o a B" sin saber cuál es cuál. Un AbxTrialSession por ensayo
// (misma máquina de estados ciega que el ABX HRTF y el ABX de estado
// maestro).
//
// CONDICIÓN (qué suena): ON = los controles FUSIÓN guardados del usuario
// (orbeAmount / holosAmount con sus valores); OFF = EXACTAMENTE los
// mismos controles con orbeAmount = 0 y holosAmount = 0. Todo lo demás
// (Φ, Volterra, EQ, sala, dinámica, cerebro) queda idéntico, así que la
// única diferencia audible entre A y B es ORBE·Ω+HOLOS·Ω. El cambio de
// condición lo hace la pantalla con FusedControlsBridge.safeSetFusedControls
// (JNI Ruta A + SET_PERCEPTUAL_STATE al daemon, Ruta B); esta clase NO
// toca el audio: solo lógica de ensayos + gate LUFS + agregación.
//
// LEVEL-MATCH (tolerancia ±0.2 LU, Repair 6 / F1): ningún ensayo puede
// empezar sin que ON y OFF estén igualados en LUFS a ±0.2 LU. La
// pantalla mide el LUFS de cada condición con el medidor LUFS vivo de
// la app (AudioEngine.getLufs(), BS.1770-4, EMA ~400 ms) sobre el mismo
// pasaje de referencia, calcula el trim con LufsMatch y lo aplica con
// el master gain global (AudioEngine.setMasterGain — un escalar a la
// salida, no cambia el procesado, solo el nivel). beginTrial() es
// fail-closed: sin setLufsMatched(true) no arranca.
//
// NOTA DE MEDICIÓN (verificada en audio_orchestrator.cpp): el medidor
// LUFS se alimenta ANTES del master gain, así que medir con el trim
// aplicado lee el nivel crudo y LufsMatch.residualDb(medido + trim) es
// el residual efectivo correcto. El medidor es momentáneo (EMA ~400 ms),
// no un integrado acumulado: las ventanas de 3 s por condición son
// válidas.
//
// Los resultados se registran en AbxResultStore con protocolo
// "ABX_OrbeHolos_On_vs_Off" (validTrials() del protocolo HRTF viejo NO
// los mezcla: filtra por protocolo). La estadística es la misma que el
// ABX existente (AbxStats.binomialTest, Wilson 95%). Esta clase NO
// declara ningún veredicto: es una herramienta lista para que el dueño
// corra las 2 sesiones; el resultado lo pone él.
//
// © 2026 Luis Uriel Pimentel Pérez
// ===========================================================================

package com.ivanna.omega.spatial

import org.json.JSONObject

/** Constantes del protocolo ABX ORBE·Ω+HOLOS·Ω ON vs OFF. */
object OrbeHolosAbx {
    const val PROTOCOL = "ABX_OrbeHolos_On_vs_Off"
    const val PROTOCOL_VERSION = 1

    /** Ensayos por sesión (Repair 6 / F1). */
    const val TRIAL_COUNT = 16

    /** Tolerancia de igualado de nivel para este estudio: ±0.2 LU. */
    const val TOLERANCE_LU = 0.2
}

/** Las dos condiciones del estudio. OFF = orbeAmount 0 + holosAmount 0. */
enum class OrbeHolosCondition { OFF, ON }

/**
 * Sesión ABX de 16 ensayos ORBE·Ω+HOLOS·Ω ON vs OFF.
 * Un AbxTrialSession por ensayo; el orden A/B se sortea por ensayo
 * (drawDynamic por ensayo, seed opcional para reproducibilidad en tests).
 * La sesión NO toca el audio: la pantalla pregunta conditionFor() y
 * conmuta los controles FUSIÓN.
 */
class OrbeHolosAbxSession(
    seed: Long? = null,
) {
    private val rng = if (seed != null) java.util.Random(seed) else java.util.Random()
    private val trials: List<AbxTrialSession> =
        List(OrbeHolosAbx.TRIAL_COUNT) { AbxTrialSession(drawDynamic = { rng.nextBoolean() }) }

    /** Aciertos por ensayo terminado, en orden (true = X identificado). */
    private val results = mutableListOf<Boolean>()

    /** Gate LUFS: solo true tras setLufsMatched(true) (fail-closed). */
    private var lufsMatched = false
    private var lufsTrimDbOn = 0.0
    private var lufsResidualLu = Double.NaN

    /** Ensayos terminados (0..16). */
    val finishedCount: Int get() = results.size

    /** Número del ensayo en curso (1..16) para la UI. */
    val currentTrialNumber: Int get() = finishedCount + 1

    val isDone: Boolean get() = finishedCount >= OrbeHolosAbx.TRIAL_COUNT
    val totalHits: Int get() = results.count { it }
    val totalTrials: Int get() = results.size

    /** Trim (dB) aplicado a ON para igualarlo a OFF; residual (LU) medido. */
    val trimDbOn: Double get() = lufsTrimDbOn
    val residualLu: Double get() = lufsResidualLu

    /**
     * Registra que ON y OFF quedaron igualados en LUFS (la pantalla lo
     * verifica y mide el trim/residual). Sin esto, beginTrial() no arranca.
     */
    fun setLufsMatched(matched: Boolean, trimDbOn: Double, residualLu: Double) {
        lufsMatched = matched
        lufsTrimDbOn = trimDbOn
        lufsResidualLu = residualLu
    }

    fun isLufsMatched(): Boolean = lufsMatched

    /**
     * Empieza el siguiente ensayo. Fail-closed: si ON/OFF no están
     * igualados en LUFS, o la sesión ya terminó, no empieza (false).
     * Idempotente si el ensayo en curso ya empezó.
     */
    fun beginTrial(): Boolean {
        if (!lufsMatched || isDone) return false
        val trial = trials[finishedCount]
        if (!trial.active) trial.begin()
        return true
    }

    /** La condición que suena para la muestra ("A"/"B"/"X") del ensayo en curso. */
    fun conditionFor(sample: String): OrbeHolosCondition {
        check(!isDone) { "Sesión terminada" }
        val scenario = trials[finishedCount].scenario(sample)
        return if (scenario == 1) OrbeHolosCondition.OFF else OrbeHolosCondition.ON
    }

    /** ¿La muestra fue escuchada (y aceptada) en el ensayo en curso? (delegado). */
    fun confirm(sample: String, accepted: Boolean) {
        check(!isDone) { "Sesión terminada" }
        trials[finishedCount].confirm(sample, accepted)
    }

    /** ¿El oyente puede enviar su respuesta para el ensayo en curso? (delegado). */
    fun canSubmit(selection: String?): Boolean {
        check(!isDone) { "Sesión terminada" }
        return trials[finishedCount].canSubmit(selection)
    }

    /**
     * Cierra el ensayo en curso con la respuesta del oyente. Devuelve true
     * si X fue identificado correctamente. Registra el resultado (en
     * memoria; la persistencia la hace la pantalla con
     * AbxResultStore.record()).
     */
    fun finish(selection: String): Boolean {
        check(!isDone) { "Sesión terminada" }
        val ok = trials[finishedCount].finish(selection)
        results.add(ok)
        return ok
    }

    fun cancelTrial() {
        if (!isDone) trials[finishedCount].cancel()
    }

    /** p-value binomial bilateral (H0: chance = 0.5) sobre los ensayos terminados. */
    fun pValue(): Double = AbxStats.binomialTest(totalHits, totalTrials)

    /** Trial JSON para AbxResultStore (un registro por ensayo terminado). */
    fun trialJson(trialNumber: Int, selection: String, timestampMs: Long): JSONObject {
        val idx = (trialNumber - 1).coerceIn(0, OrbeHolosAbx.TRIAL_COUNT - 1)
        val trial = trials[idx]
        val correct = results.getOrNull(idx) ?: false
        return JSONObject().apply {
            put("protocol", OrbeHolosAbx.PROTOCOL)
            put("protocolVersion", OrbeHolosAbx.PROTOCOL_VERSION)
            put("condition", "OrbeHolos_ON_vs_OFF")
            put("trial", trialNumber)
            put("trialOf", OrbeHolosAbx.TRIAL_COUNT)
            put("scenarioX", trial.scenarioX)
            put("userSelection", selection)
            put("correct", correct)
            put("lufsTrimDbOn", lufsTrimDbOn)
            put("lufsResidualLu", lufsResidualLu)
            put("lufsToleranceLu", OrbeHolosAbx.TOLERANCE_LU)
            put("timestampMs", timestampMs)
            put("valid", true)
        }
    }
}
