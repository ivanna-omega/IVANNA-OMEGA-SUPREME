// MasterStateAbxSession.kt — FASE 5: ABX ciego BYPASS / LIMPIO_OFF / ACTIVO
// (el ABX que faltaba: el existente, AbxTestScreen, solo compara HRTF
// estático vs dinámico — protocolo ABX_Static_vs_DynamicHRTF).
//
// Protocolo: "ABX_MasterState_Bypass_vs_Limpio_vs_Activo" (v1). Tres pares
// por comparación (BYPASS vs LIMPIO_OFF, BYPASS vs ACTIVO, LIMPIO_OFF vs
// ACTIVO), cada uno un AbxTrialSession binario (A/B/X, X aleatorio, el
// oyente no sabe cuál es cuál). El orden de los pares se baraja para
// evitar sesgo de orden. La estadística es la misma que el ABX existente
// (AbxStats.binomialTest, Wilson 95%).
//
// MATCHED-LUFS (Regla de Oro, Fase 5): ningún par puede empezar sin que
// sus dos estados estén igualados en LUFS integrado a ±0.05 LU (el
// objetivo es que la única diferencia audible sea el PROCESADO, no el
// volumen). LufsMatch es matemática pura (trim = target - medido);
// la medición de LUFS integrado y la aplicación del trim las hace la
// pantalla (usa el medidor LUFS de la app) y las registra aquí con
// setLufsMatched(pair, ...). beginPair() es fail-closed: si el par no
// está igualado, no empieza.
//
// El cableado a MasterStateController (qué estado suena al pulsar A/B/X)
// lo hace AbxTestScreen: llama stateFor(sample) y pasa el MasterState a
// MasterStateController.submitState(state, "abx"). Esta clase NO toca el
// audio: solo lógica de pares + LUFS-gate + agregación de resultados.
// Los resultados se registran en AbxResultStore con protocolo
// "ABX_MasterState_Bypass_vs_Limpio_vs_Activo" (validTrials() del
// protocolo viejo NO los mezcla: filtra por protocolo).
//
// © 2026 Luis Uriel Pimentel Pérez
// ===========================================================================

package com.ivanna.omega.spatial

import com.ivanna.omega.audio.MasterState
import org.json.JSONObject

/**
 * Matemática pura del igualado LUFS (sin dependencias Android).
 * trimDb = cuánto subir/bajar el estado medido para igualarlo al target.
 * Después de aplicar el trim, residualDb verifica que quedó dentro de
 * la tolerancia (±0.05 LU, Regla de Oro de Niveles Matched-LUFS).
 */
object LufsMatch {
    /** Tolerancia de igualado: ±0.05 LU (Fase 5). */
    const val TOLERANCE_LUFS = 0.05

    /** Trim (dB) para llevar el estado medido al target. */
    fun trimDb(targetLufs: Double, measuredLufs: Double): Double =
        targetLufs - measuredLufs

    /** ¿El estado medido está dentro de la tolerancia del target? */
    fun isMatched(
        targetLufs: Double,
        measuredLufs: Double,
        tolerance: Double = TOLERANCE_LUFS,
    ): Boolean = kotlin.math.abs(targetLufs - measuredLufs) <= tolerance

    /** Error residual (LU) tras aplicar el trim: debe ser |·| <= 0.05. */
    fun residualDb(
        targetLufs: Double,
        measuredLufs: Double,
        appliedTrimDb: Double,
    ): Double = (measuredLufs + appliedTrimDb) - targetLufs
}

/**
 * Sesión ABX de los tres estados maestros. Un AbxTrialSession por par;
 * el orden de los pares se baraja al crear la sesión (seed opcional para
 * tests). La sesión NO toca el audio: la pantalla pregunta stateFor()
 * y conmuta el Maestro.
 */
class MasterStateAbxSession(
    seed: Long? = null,
) {
    companion object {
        const val PROTOCOL = "ABX_MasterState_Bypass_vs_Limpio_vs_Activo"
        const val PROTOCOL_VERSION = 1
    }

    /** Los tres pares por comparación (orden barajado en [pairOrder]). */
    enum class Pair(val a: MasterState, val b: MasterState) {
        BYPASS_VS_LIMPIO(MasterState.BYPASS, MasterState.LIMPIO_OFF),
        BYPASS_VS_ACTIVO(MasterState.BYPASS, MasterState.ACTIVO),
        LIMPIO_VS_ACTIVO(MasterState.LIMPIO_OFF, MasterState.ACTIVO),
    }

    /** Orden barajado de los pares (evita sesgo de orden). */
    val pairOrder: List<Pair> = run {
        val list = Pair.values().toMutableList()
        val rng = if (seed != null) java.util.Random(seed) else java.util.Random()
        list.shuffle(rng)
        list.toList()
    }

    private val trials: Map<Pair, AbxTrialSession> = Pair.values().associateWith {
        // drawDynamic inyectable: por defecto aleatorio (como AbxTrialSession).
        AbxTrialSession()
    }

    /** Pares cuyo igualado LUFS ya fue verificado (fail-closed). */
    private val lufsMatchedPairs = mutableSetOf<Pair>()

    /** Par actual (el primero no terminado del orden barajado). */
    val currentPair: Pair?
        get() = pairOrder.firstOrNull { !isPairDone(it) }

    /** ¿El par ya terminó (finish() devolvió un resultado)? */
    fun isPairDone(pair: Pair): Boolean = pairResults.containsKey(pair)

    /** Resultados por par: true = el oyente identificó X correctamente. */
    private val pairResults = mutableMapOf<Pair, Boolean>()
    val results: Map<Pair, Boolean> get() = pairResults.toMap()

    /** Registra que el par quedó igualado en LUFS (la pantalla lo verifica). */
    fun setLufsMatched(pair: Pair, matched: Boolean) {
        if (matched) lufsMatchedPairs.add(pair) else lufsMatchedPairs.remove(pair)
    }

    fun isLufsMatched(pair: Pair): Boolean = pair in lufsMatchedPairs

    /**
     * Empieza el par. Fail-closed: si el par no está igualado en LUFS,
     * no empieza (devuelve false) — el oyente no debe comparar dos
     * estados con distinto volumen.
     */
    fun beginPair(pair: Pair): Boolean {
        if (!isLufsMatched(pair)) return false
        trials.getValue(pair).begin()
        return true
    }

    /** El MasterState que suena para la muestra ("A"/"B"/"X") del par actual. */
    fun stateFor(pair: Pair, sample: String): MasterState {
        val scenario = trials.getValue(pair).scenario(sample)
        return if (scenario == 1) pair.a else pair.b
    }

    /** ¿La muestra fue escuchada (y aceptada) en el par? (delegado). */
    fun confirm(pair: Pair, sample: String, accepted: Boolean): Boolean {
        trials.getValue(pair).confirm(sample, accepted)
        return accepted
    }

    /** ¿El oyente puede enviar su respuesta para el par? (delegado). */
    fun canSubmit(pair: Pair, selection: String?): Boolean =
        trials.getValue(pair).canSubmit(selection)

    /**
     * Cierra el par con la respuesta del oyente. Devuelve true si X fue
     * identificado correctamente. Registra el resultado (en memoria; la
     * persistencia la hace la pantalla con AbxResultStore.record()).
     */
    fun finish(pair: Pair, selection: String): Boolean {
        val ok = trials.getValue(pair).finish(selection)
        pairResults[pair] = ok
        return ok
    }

    fun cancelPair(pair: Pair) = trials.getValue(pair).cancel()

    /** Aciertos totales (0..3) y total de pares terminados. */
    val totalHits: Int get() = pairResults.values.count { it }
    val totalPairs: Int get() = pairResults.size

    /** p-value binomial bilateral (H0: chance = 0.5) sobre los pares terminados. */
    fun pValue(): Double = AbxStats.binomialTest(totalHits, totalPairs)

    /** Trial JSON para AbxResultStore (un registro por par terminado). */
    fun trialJson(pair: Pair, timestampMs: Long): JSONObject {
        val correct = pairResults[pair] ?: false
        return JSONObject().apply {
            put("protocol", PROTOCOL)
            put("protocolVersion", PROTOCOL_VERSION)
            put("condition", "MasterState_${pair.name}")
            put("correct", correct)
            put("timestampMs", timestampMs)
            put("valid", true)
        }
    }
}
