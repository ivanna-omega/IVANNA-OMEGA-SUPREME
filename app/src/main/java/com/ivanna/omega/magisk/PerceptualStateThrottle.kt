// PerceptualStateThrottle.kt — Throttle del empuje SET/SEND_PERCEPTUAL_STATE.
//
// El defecto (crujido + ahogo al abrir la app y al cambiar de ventana,
// medido en vivo 2026-11-21): la app empuja estado perceptual en ráfagas
// durante las transiciones de UI (recomposición, restorer) y el daemon
// reconfigura el efecto ~20/s (generación subiendo ~20/s, RouteBDeviceGate
// evaluado 165 veces por transición). El dedup (PerceptualStateDedup) corta
// los duplicados exactos, pero los valores jitterean apenas por encima de
// su epsilon (0.1% / 1e-4) y cada jitter pasa como "cambio" y se envía.
// Ese re-trabajo constante en el camino de audio es el crujido; el ahogo
// es el sonido cortándose a cada reconfiguración.
//
// El fix (segundo filtro, después del dedup, en OmegaEngineBridge.sendCommand):
// no enviar la misma acción más de una vez por 100 ms, salvo cambio GRANDE.
// Un cambio es grande si:
//   - cambia el modo/salida (outputMode u otra clave de modo), o
//   - algún campo numérico cambia más del 5% relativo, o
//   - algún campo hace flip 0 ↔ no-cero, o
//   - cambia el juego de claves, o un campo no-numérico cambia.
// El valor final nunca se pierde: si un empuje se suprime por throttle, el
// llamador programa un envío diferido del último payload al cierre de la
// ventana (≤100 ms de retraso para un cambio de slider real; un cambio
// grande o de modo pasa inmediato, sin esperar).
//
// Diseño (misma filosofía que PerceptualStateDedup):
//   - Clase PURA (sin Android salvo el reloj por defecto): el reloj es
//     inyectable para tests deterministas (fake clock en JVM).
//   - Comparación sobre el payload POST-PUERTA (lo que el daemon realmente
//     recibe), igual que el dedup: el throttle vive en sendCommand después
//     de la puerta y del dedup.
//   - El caché es POR ACCIÓN (SEND y SET tienen juegos de campos distintos).
//   - Fail-closed: ante cualquier fallo de comparación o de reloj, se ENVÍA
//     (nunca se deja de enviar por un error). Un fallo persistente → se
//     envía siempre, que es el comportamiento de antes del fix.
//   - Sincronizado: sendCommand se llama desde varios hilos (hilo principal,
//     loop perceptual, UI).
package com.ivanna.omega.magisk

import kotlin.math.abs

/**
 * Throttle del empuje de estado perceptual.
 *
 * Una instancia guarda, por acción, el último payload ENVIADO y su instante,
 * más el último payload SUPRIMIDO (trailing) para el envío diferido.
 * [shouldSend] devuelve true si el payload debe enviarse ahora (primera vez,
 * cambio grande, o ventana vencida) y false si debe suprimirse por throttle
 * (cambio pequeño dentro de la ventana). Cuando devuelve false, el payload
 * queda guardado como trailing; el llamador debe programar su envío con
 * [delayUntilFlushMs] y ejecutarlo con [noteFlushed].
 *
 * El reloj es inyectable: en producción es SystemClock.uptimeMillis
 * (monotónico, inmune a cambios de hora); en tests es un fake clock.
 */
class PerceptualStateThrottle(
    private val windowMs: Long = WINDOW_MS,
    private val clockMs: () -> Long = { android.os.SystemClock.uptimeMillis() },
) {

    private data class ActionState(
        var lastSentAtMs: Long = Long.MIN_VALUE,
        var lastSent: Map<String, Any?>? = null,
        var trailing: Map<String, Any?>? = null,
    )

    private val stateByAction = mutableMapOf<String, ActionState>()

    /**
     * Decide si el payload debe enviarse ahora. Fail-closed: ante cualquier
     * fallo devuelve true (enviar). Si devuelve true, actualiza el último
     * enviado; si devuelve false, guarda el payload como trailing.
     */
    @Synchronized
    fun shouldSend(action: String, fields: Map<String, Any?>): Boolean {
        return try {
            val now = clockMs()
            val st = stateByAction.getOrPut(action) { ActionState() }
            val last = st.lastSent
            if (last == null) {
                st.lastSent = fields.toMap()
                st.lastSentAtMs = now
                st.trailing = null
                true
            } else if (isLargeChange(last, fields)) {
                st.lastSent = fields.toMap()
                st.lastSentAtMs = now
                st.trailing = null
                true
            } else if (now - st.lastSentAtMs >= windowMs) {
                st.lastSent = fields.toMap()
                st.lastSentAtMs = now
                st.trailing = null
                true
            } else {
                st.trailing = fields.toMap()
                false
            }
        } catch (_: Throwable) {
            true
        }
    }

    /**
     * Milisegundos hasta que el trailing pendiente de la acción deba
     * enviarse (0 si no hay trailing o la ventana ya venció). Para que el
     * llamador programe el envío diferido. Fail-closed: ante fallo, 0.
     */
    @Synchronized
    fun delayUntilFlushMs(action: String): Long {
        return try {
            val st = stateByAction[action] ?: return 0L
            if (st.trailing == null) return 0L
            val elapsed = clockMs() - st.lastSentAtMs
            (windowMs - elapsed).coerceAtLeast(0L)
        } catch (_: Throwable) {
            0L
        }
    }

    /**
     * Registra que el trailing de la acción se envió por el envío diferido.
     * Devuelve false si el payload ya no es el trailing vigente (un envío
     * posterior lo dejó obsoleto) o si ya era el último enviado; true si
     * procede el envío y actualiza el último enviado (la ventana reinicia
     * desde ahora). Fail-closed: ante fallo devuelve true (enviar).
     */
    @Synchronized
    fun noteFlushed(action: String, fields: Map<String, Any?>): Boolean {
        return try {
            val st = stateByAction[action] ?: return true
            val trailing = st.trailing ?: return false
            // El diferido debe llevar el trailing VIGENTE; si los campos no
            // coinciden con él, un valor más nuevo ya lo dejó obsoleto.
            if (!PerceptualStateDedup.mapsEqual(trailing, fields)) return false
            val last = st.lastSent
            if (last != null && PerceptualStateDedup.mapsEqual(last, fields)) {
                st.trailing = null
                return false
            }
            st.lastSent = fields.toMap()
            st.lastSentAtMs = clockMs()
            st.trailing = null
            true
        } catch (_: Throwable) {
            true
        }
    }

    /** Invalida todo el estado (daemon caído/reinicio: re-enviar todo). */
    @Synchronized
    fun reset() {
        stateByAction.clear()
    }

    companion object {
        /** Ventana mínima entre dos envíos de la misma acción. */
        const val WINDOW_MS: Long = 100L

        /** Umbral de cambio grande: 5% relativo. */
        const val LARGE_REL = 0.05

        /**
         * Claves de modo/salida: cualquier cambio en una de ellas es un
         * cambio grande (pasa inmediato, sin esperar a la ventana). El
         * outputMode es el modo de salida de SET_PERCEPTUAL_STATE; el resto
         * son claves de modo que pueden aparecer en payloads perceptuales.
         */
        private val MODE_KEYS = setOf(
            "outputMode", "mode", "output", "route", "spatialMode",
            "usbDacTarget", "routeProfile",
        )

        /**
         * ¿Es el cambio entre dos payloads un cambio GRANDE? Grande si el
         * juego de claves difiere, si alguna clave de modo cambió, si algún
         * campo numérico cambió más del 5% relativo o hizo flip 0 ↔ no-cero,
         * o si algún campo no-numérico cambió. Fail-closed: ante cualquier
         * fallo o valor no-finito, es grande (se envía).
         */
        fun isLargeChange(previous: Map<String, Any?>, current: Map<String, Any?>): Boolean {
            return try {
                if (previous.keys != current.keys) return true
                for ((key, prevValue) in previous) {
                    if (isLargeValueChange(key, prevValue, current[key])) return true
                }
                false
            } catch (_: Throwable) {
                true
            }
        }

        private fun isLargeValueChange(key: String, prev: Any?, cur: Any?): Boolean {
            // Clave de modo: cualquier cambio es grande.
            if (key in MODE_KEYS) return prev != cur
            if (prev is Number && cur is Number) {
                val a = prev.toDouble()
                val b = cur.toDouble()
                if (!a.isFinite() || !b.isFinite()) return true
                if (a == b) return false
                // Flip 0 ↔ no-cero: es un encendido/apagado, siempre grande.
                if ((a == 0.0) != (b == 0.0)) return true
                val diff = abs(a - b)
                val scale = maxOf(abs(a), abs(b))
                return diff > LARGE_REL * scale
            }
            if (prev is Map<*, *> && cur is Map<*, *>) {
                @Suppress("UNCHECKED_CAST")
                return isLargeChange(prev as Map<String, Any?>, cur as Map<String, Any?>)
            }
            if (prev is List<*> && cur is List<*>) {
                if (prev.size != cur.size) return true
                return prev.indices.any { i -> isLargeValueChange(key, prev[i], cur[i]) }
            }
            // Cambio no-numérico (string/bool/enum): es un cambio de modo.
            return prev != cur
        }
    }
}
