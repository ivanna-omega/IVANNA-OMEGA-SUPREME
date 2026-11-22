package com.ivanna.omega.core

import com.ivanna.omega.audio.AudioRouteManager
import com.ivanna.omega.audio.OutputRoute

/**
 * Ω — Operador Universal (v2.5.444): clases de equipo E1–E8.
 *
 * Los valores coinciden con el enum `OmegaEquipment` de
 * `app/src/main/cpp/spatial/OmegaOperator.hpp` (0 Auto, 1 E1 … 8 E8).
 * Si se añade una clase, cambiar AMBOS lados en el mismo commit.
 *
 * Resolución de Auto: por la ruta de salida real de Android
 * (AudioRouteManager, que ya existe). Si no se puede resolver, se
 * devuelve AUTO (N0 → identidad, fail-closed): Ω nunca corrige a ciegas.
 * La medición del equipo (barrido §2.3 del documento privado) llenará
 * H_b/cMaxDb en una fase posterior (N2); en v1 el perfil es de clase (N1).
 */
object OmegaEquipment {
    const val AUTO = 0
    const val HEADPHONES = 1      // E1 audífonos / earbuds (κ=0)
    const val PHONE_SPEAKERS = 2  // E2 bocinas del teléfono (κ≈0.7)
    const val BLUETOOTH = 3       // E3 bocina Bluetooth (κ≈0.4)
    const val HOME_STEREO = 4     // E4 estéreo de casa (κ≈0.3)
    const val CINEMA = 5          // E5 multicanal / cine (κ≈0.2)
    const val CAR = 6             // E6 auto (κ≈0.35)
    const val PA = 7              // E7 PA / club (WFS, Σ)
    const val STADIUM = 8         // E8 estadio / mainstage (WFS, Σ)

    val LABELS = mapOf(
        AUTO to "Automático (por salida)",
        HEADPHONES to "Audífonos",
        PHONE_SPEAKERS to "Bocinas del teléfono",
        BLUETOOTH to "Bocina Bluetooth",
        HOME_STEREO to "Estéreo de casa",
        CINEMA to "Cine / multicanal",
        CAR to "Auto",
        PA to "PA / club",
        STADIUM to "Estadio / mainstage"
    )

    fun label(equipment: Int): String = LABELS[equipment.coerceIn(0, 8)] ?: "Automático (por salida)"

    /** Resuelve Auto por la ruta de salida real. Sin ruta, AUTO (N0). */
    fun resolve(selected: Int): Int {
        val sel = selected.coerceIn(0, 8)
        if (sel != AUTO) return sel
        return when (runCatching { AudioRouteManager.detectOutputRoute() }.getOrNull()) {
            OutputRoute.BLUETOOTH -> BLUETOOTH
            OutputRoute.WIRED_AUX -> HEADPHONES
            OutputRoute.USB -> HEADPHONES // DAC USB-C: binaural, κ=0
            OutputRoute.SPEAKER -> PHONE_SPEAKERS
            else -> AUTO // UNKNOWN o error: sin info → N0 (identidad)
        }
    }

    /** Aplica los parámetros Ω guardados al nativo (Ruta A). Llamarlo al
     *  arrancar (PersistedStateRestorer) y al cambiar cualquier ajuste Ω. */
    fun applyToNative(paramStore: ParameterStore) {
        if (!IvannaNativeLib.isLoaded) return
        runCatching {
            IvannaNativeLib.nativeSetOmegaParams(
                paramStore.isOmegaEnabled(),
                resolve(paramStore.getOmegaEquipment()),
                paramStore.getOmegaBoostMaxDb(),
                paramStore.isOmegaIldPrecomp()
            )
        }
    }
}
