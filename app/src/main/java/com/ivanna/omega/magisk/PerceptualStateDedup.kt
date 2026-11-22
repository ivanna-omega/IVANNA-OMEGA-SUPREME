// PerceptualStateDedup.kt — Deduplicación del empuje SET/SEND_PERCEPTUAL_STATE.
//
// El defecto (micro-cortes en TIDAL, medido en vivo 2026-11-21): la app
// empuja estado perceptual ~2/s aunque nada cambie — PerceptualCortex
// cierra el loop por bloque PCM → PerceptualDecisionEngine.dispatchDecision
// → OmegaEngineBridge.sendPerceptualState (SEND_PERCEPTUAL_STATE), y
// FusedControls.pushFusedToDaemon en cada apply (SET_PERCEPTUAL_STATE).
// El daemon republica el snapshot en cada generación nueva y el efecto
// re-aplica (omega_apply_snapshot); ese re-trabajo constante en el camino
// de audio produce underruns (~5.6/s, cada uno un micro-corte).
//
// El fix: solo se envía si algo cambió de verdad. Esta clase es el
// comparador + la máquina de estados. La clase es PURA (sin Android);
// la conversión JSON→mapa es la función perceptualPayloadToMap de este
// mismo archivo, que sí usa org.json y es solo para runtime (en tests
// unitarios de JVM se testea con Robolectric).
//
// Diseño:
//   - Comparación campo por campo con epsilon RELATIVO (0.1%) y piso
//     absoluto (1e-4): traga el jitter numérico (orbeAmount 0.386 → 0.3861,
//     highCut 16000 → 16001 Hz, compressor −12.0 → −12.005 dB) y deja pasar
//     los cambios reales (orbeAmount +0.01, highCut +100 Hz, outputMode 1→2).
//   - El caché es POR ACCIÓN (SEND y SET tienen juegos de campos distintos;
//     el daemon los trata como alias, pero sus payloads no se comparan entre sí).
//   - Fail-closed: si la comparación falla, se ENVÍA (nunca se deja de
//     enviar por un error de comparación). Si el envío falla (daemon caído),
//     el llamador debe invalidar el caché con reset() — el daemon pudo
//     perder su estado y el próximo empuje, aunque sea idéntico, debe
//     re-enviarse.
//   - La comparación se hace sobre el payload POST-PUERTA (lo que el daemon
//     realmente recibe): en bocinas la puerta neutraliza campos, así que dos
//     payloads pre-puerta distintos pueden ser el mismo post-puerta, y un
//     cambio de modo (bocinas→audífonos) con los mismos sliders SÍ cambia el
//     post-puerta y debe enviarse. Por eso el dedup vive en sendCommand,
//     después de la puerta, y no en los call sites.
package com.ivanna.omega.magisk

import kotlin.math.abs
import org.json.JSONArray
import org.json.JSONObject

/**
 * Deduplicador del empuje de estado perceptual.
 *
 * Una instancia guarda el último payload aceptado por acción. [shouldSend]
 * devuelve true si el payload debe enviarse (primera vez, cambió, o la
 * comparación falló → fail-closed) y false solo si es idéntico (dentro del
 * epsilon) al último aceptado para esa acción. Cuando devuelve true,
 * actualiza el caché con el nuevo payload.
 *
 * Sincronizado: sendCommand se llama desde varios hilos (hilo principal,
 * loop perceptual, UI).
 */
class PerceptualStateDedup {

    private val lastByAction = mutableMapOf<String, Map<String, Any?>>()

    /**
     * Decide si el payload debe enviarse. Fail-closed: ante cualquier fallo
     * de comparación, devuelve true (enviar). No actualiza el caché en ese
     * caso (un fallo persistente de comparación → se envía siempre, que es
     * el comportamiento de antes del fix).
     */
    @Synchronized
    fun shouldSend(action: String, fields: Map<String, Any?>): Boolean {
        return try {
            val previous = lastByAction[action]
            if (previous == null) {
                lastByAction[action] = fields.toMap()
                true
            } else if (mapsEqual(previous, fields)) {
                false
            } else {
                lastByAction[action] = fields.toMap()
                true
            }
        } catch (_: Throwable) {
            true
        }
    }

    /**
     * Invalida el caché. Llamar cuando el daemon pudo perder su estado
     * (envío fallido, reconexión): el próximo empuje se re-envía aunque
     * sea idéntico al último aceptado.
     */
    @Synchronized
    fun reset() {
        lastByAction.clear()
    }

    companion object {
        /** Epsilon relativo: 0.1% del mayor de los dos valores. */
        const val REL_EPS = 1e-3

        /** Piso absoluto: para valores cercanos a cero manda este umbral. */
        const val ABS_EPS = 1e-4

        /**
         * Igualdad numérica con epsilon. No-finito (NaN/Inf) nunca es igual:
         * fail-closed, se trata como cambio (aunque sendCommand ya rechaza
         * los no-finitos antes de llegar aquí).
         */
        fun valuesEqual(a: Double, b: Double): Boolean {
            if (!a.isFinite() || !b.isFinite()) return false
            if (a == b) return true
            val diff = abs(a - b)
            val scale = maxOf(abs(a), abs(b))
            return diff <= maxOf(ABS_EPS, REL_EPS * scale)
        }

        /**
         * Igualdad de dos mapas de campos. Los juegos de claves deben
         * coincidir exactamente (una clave añadida o quitada es un cambio).
         * Los números se comparan con [valuesEqual]; los anidados
         * (Map/List) se comparan recursivamente; el resto por igualdad.
         */
        fun mapsEqual(previous: Map<String, Any?>, current: Map<String, Any?>): Boolean {
            if (previous.size != current.size) return false
            for ((key, prevValue) in previous) {
                if (!current.containsKey(key)) return false
                if (!valuesDeepEqual(prevValue, current[key])) return false
            }
            return true
        }

        private fun valuesDeepEqual(prev: Any?, cur: Any?): Boolean {
            return when {
                prev is Number && cur is Number ->
                    valuesEqual(prev.toDouble(), cur.toDouble())
                prev is Map<*, *> && cur is Map<*, *> -> {
                    if (prev.size != cur.size) return false
                    prev.entries.all { (k, v) ->
                        cur.containsKey(k) && valuesDeepEqual(v, cur[k])
                    }
                }
                prev is List<*> && cur is List<*> -> {
                    if (prev.size != cur.size) return false
                    prev.indices.all { i -> valuesDeepEqual(prev[i], cur[i]) }
                }
                else -> prev == cur
            }
        }
    }
}

/**
 * Convierte un payload JSON perceptual a un mapa comparable por
 * [PerceptualStateDedup]. Los números pasan a Double; los objetos y arrays
 * se convierten recursivamente; JSON null pasa a null. Solo para runtime
 * (org.json es clase del framework Android; en tests unitarios de JVM
 * usar Robolectric — ver PerceptualStateDedupJsonTest).
 */
fun perceptualPayloadToMap(json: JSONObject): Map<String, Any?> {
    val map = LinkedHashMap<String, Any?>()
    val keys = json.keys()
    while (keys.hasNext()) {
        val key = keys.next()
        map[key] = jsonValueToComparable(json.opt(key))
    }
    return map
}

private fun jsonValueToComparable(value: Any?): Any? {
    return when (value) {
        null, JSONObject.NULL -> null
        is Number -> value.toDouble()
        is JSONObject -> perceptualPayloadToMap(value)
        is JSONArray -> {
            val list = ArrayList<Any?>(value.length())
            for (i in 0 until value.length()) {
                list.add(jsonValueToComparable(value.opt(i)))
            }
            list
        }
        else -> value
    }
}
