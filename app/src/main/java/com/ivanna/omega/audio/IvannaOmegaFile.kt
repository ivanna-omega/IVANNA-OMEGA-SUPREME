package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.dsp.DSPState
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject

/**
 * IvannaOmegaFile.kt — Cargador del formato IVANNA OMEGA (v2.5.473, feature).
 *
 * Un archivo IVANNA OMEGA es un JSON por canción: la rola se descarga, se
 * desmenuza (espectro, transitorios/tonales, sonoridad) y se convierte en
 * un archivo con su perfil; el motor lo lee y aplica el procesamiento de
 * esa rola. La rola queda etiquetada como "IVANNA OMEGA" igual que una
 * rola dice "Dolby Atmos" — formato propio, archivo propio, motor propio.
 *
 * Ejemplo real: `voodoo_child.ivanna.json` (Jimi Hendrix, Voodoo Child).
 *
 * Estructura:
 *   format: "IVANNA OMEGA" (exacto), format_version: "1.0"
 *   track, artist, source, duration_s
 *   fingerprint: huella medida (loudness/spectrum/transient_tonal/stereo)
 *     — informativa, no se aplica al DSP.
 *   engine: lo que sí se aplica al motor (orbe/holos/phi/volterra, EQ por
 *     banda, compresor, exciter, trim, structural_split).
 *   derivation: por qué cada ajuste (informativa).
 *
 * Reglas duras:
 *  - Fail-closed: si format != "IVANNA OMEGA" o format_version != "1.0",
 *    el parseo FALLA y no se aplica nada parcial. Lo mismo si el JSON
 *    está roto o si net_gain_db viola la Regla de Oro Matched-LUFS
 *    (|net| > 0.5 dB).
 *  - El parser y el mapeo son PUROS (sin Android): se unit-testean en JVM
 *    sin Robolectric. Solo [IvannaOmegaApplier] toca Context/DSP.
 *  - El mapeo conserva el estado vigente en todo lo que el archivo NO
 *    define (modo de salida, WFS, upmix, XTC, parallax, sala, cerebro…),
 *    igual que [ProfileManager.applyToDsp].
 *  - Sin uso, el cargador no cambia nada: no hay comportamiento nuevo en
 *    arranque ni en el perfil de arranque (BOOT_PROFILE sigue COMPLETO).
 *
 * Límites honestos (campos del archivo SIN slot en FusedControls/DSPState
 * hoy, se parsean y se guardan pero NO se aplican):
 *  - structural_split.transient_amount/tonal_amount: el operador Split
 *    estructural aún no existe en el DSP (diseño en
 *    analysis-ivanna/STRUCTURAL_SPLIT_DISENO_2026-11-19.md). Cuando se
 *    construya, leerá estos valores del archivo.
 *  - compressor.attack_ms/release_ms: DynamicsControls no tiene slot.
 *  - eq_bands_db: solo viajan las 4 bandas con slot (low/mid/high/
 *    presence); las otras 6 (sub, lowmid, highmid, air, ultra) quedan en
 *    la huella para uso futuro.
 */

// ── Esquema del archivo ─────────────────────────────────────────────────────

@Serializable
data class IvannaOmegaCompressor(
    val threshold_db: Float = -12.0f,
    val ratio: Float = 2.0f,
    val attack_ms: Float = 10.0f,
    val release_ms: Float = 120.0f,
)

@Serializable
data class IvannaOmegaStructuralSplit(
    val transient_amount: Float = 0.55f,
    val tonal_amount: Float = 1.0f,
    val transient_energy_ratio: Float = 0.0f,
    val tonal_energy_ratio: Float = 1.0f,
)

@Serializable
data class IvannaOmegaEngine(
    val orbe_amount: Float = 1.0f,
    val orbe_distance_m: Float = 0.35f,
    val holos_amount: Float = 0.5f,
    val phi_amount: Float = 0.5f,
    val volterra_enabled: Boolean = true,
    val volterra_mix: Float = 0.55f,
    val eq_bands_db: Map<String, Float> = emptyMap(),
    val compressor: IvannaOmegaCompressor = IvannaOmegaCompressor(),
    val exciter_amount: Float = 0.3f,
    val trim_db: Float = 0.0f,
    val net_gain_db: Float = 0.0f,
    val structural_split: IvannaOmegaStructuralSplit = IvannaOmegaStructuralSplit(),
)

@Serializable
data class IvannaOmegaFile(
    val format: String,
    val format_version: String,
    val track: String = "",
    val artist: String = "",
    val source: String = "",
    val duration_s: Double = 0.0,
    val fingerprint: JsonObject? = null,
    val engine: IvannaOmegaEngine = IvannaOmegaEngine(),
    val derivation: JsonObject? = null,
) {
    /** Etiqueta para la UI: "Voodoo Child (Slight Return) — Jimi Hendrix". */
    val displayLabel: String
        get() = if (artist.isNotBlank()) "$track — $artist" else track
}

// ── Parser (puro, fail-closed) ──────────────────────────────────────────────

object IvannaOmegaFileParser {
    private const val TAG = "IvannaOmegaFile"
    const val EXPECTED_FORMAT = "IVANNA OMEGA"
    const val EXPECTED_VERSION = "1.0"

    /** Regla de Oro Matched-LUFS: ningún procesador entra con ganancia neta ≠ 0.0 dB. */
    const val MAX_NET_GAIN_DB = 0.5f

    private val json = Json {
        ignoreUnknownKeys = true
        coerceInputValues = true
        isLenient = false
    }

    /**
     * Parsea el texto de un .ivanna.json. Devuelve failure (sin aplicar
     * nada) si el JSON está roto, si no es formato IVANNA OMEGA, si la
     * versión no es la soportada, o si la ganancia neta viola Matched-LUFS.
     */
    fun parse(text: String): Result<IvannaOmegaFile> = runCatching {
        val file = json.decodeFromString<IvannaOmegaFile>(text)
        require(file.format == EXPECTED_FORMAT) {
            "Formato no es IVANNA OMEGA (viene '${file.format}')"
        }
        require(file.format_version == EXPECTED_VERSION) {
            "Versión no soportada: '${file.format_version}' (se esperaba '$EXPECTED_VERSION')"
        }
        require(file.track.isNotBlank()) { "El archivo no trae rola (track vacío)" }
        val net = file.engine.net_gain_db
        require(net.isFinite() && kotlin.math.abs(net) <= MAX_NET_GAIN_DB) {
            "Ganancia neta $net dB viola Matched-LUFS (debe ser 0.0 dB)"
        }
        // Todos los números del engine deben ser finitos: un NaN/Infinity
        // en el JSON llegaría tal cual al DSP nativo y al daemon.
        val e = file.engine
        val nums = listOf(
            e.orbe_amount, e.orbe_distance_m, e.holos_amount, e.phi_amount,
            e.volterra_mix, e.exciter_amount, e.trim_db,
            e.compressor.threshold_db, e.compressor.ratio,
            e.compressor.attack_ms, e.compressor.release_ms,
            e.structural_split.transient_amount, e.structural_split.tonal_amount,
        ) + e.eq_bands_db.values
        require(nums.all { it.isFinite() }) { "El engine trae valores no finitos (NaN/Infinity)" }
        Log.i(TAG, "Parse OK: ${file.displayLabel}")
        file
    }.onFailure { Log.e(TAG, "Parse fallido: ${it.message}") }
}

// ── Mapeo engine → FusedControls / DSPState (puro) ──────────────────────────

/** Banda EQ del archivo → slot tonal con el que viaja (las otras 6 quedan en la huella). */
private fun IvannaOmegaEngine.eqBand(name: String): Float? =
    eq_bands_db[name]?.takeIf { it.isFinite() }

/**
 * Aplica el engine del archivo sobre el [FusedControls] vigente.
 * Lo que el archivo NO define se conserva del estado vigente (mismo
 * criterio que ProfileManager.applyToDsp). El resultado pasa por .clamp().
 */
fun IvannaOmegaFile.toFusedControls(current: FusedControls): FusedControls {
    val e = engine
    return FusedControls(
        spatial = current.spatial.copy(
            amount = e.orbe_amount,
            distanceM = e.orbe_distance_m,
        ),
        field = current.field.copy(
            amount = e.holos_amount,
            phiAmount = e.phi_amount,
        ),
        listenerRoom = current.listenerRoom,
        transducer = current.transducer.copy(
            correction = if (e.volterra_enabled) e.volterra_mix else 0.0f,
        ),
        tonal = current.tonal.copy(
            trimDb = e.trim_db,
            lowDb = e.eqBand("low_60_120") ?: current.tonal.lowDb,
            midDb = e.eqBand("mid_500_1k") ?: current.tonal.midDb,
            highDb = e.eqBand("high_2k_4k") ?: current.tonal.highDb,
            presenceDb = e.eqBand("presence_4k_8k") ?: current.tonal.presenceDb,
        ),
        dynamics = current.dynamics.copy(
            compThresholdDb = e.compressor.threshold_db,
            compRatio = e.compressor.ratio,
            exciter = e.exciter_amount,
        ),
        brain = current.brain,
    ).clamp()
}

/**
 * Aplica el engine del archivo sobre el [DSPState] vigente (misma vía que
 * applyToDsp: el DSPState se actualiza y se empuja con pushToNative).
 * master se conserva: el trim del archivo va al tonal fusionado, no a una
 * ganancia oculta en DSPState.
 */
fun IvannaOmegaFile.toDspState(current: DSPState): DSPState {
    val e = engine
    return current.copy(
        wet = e.exciter_amount,
        low = e.eqBand("low_60_120") ?: current.low,
        mid = e.eqBand("mid_500_1k") ?: current.mid,
        high = e.eqBand("high_2k_4k") ?: current.high,
        presence = e.eqBand("presence_4k_8k") ?: current.presence,
        compThreshold = e.compressor.threshold_db,
        compRatio = e.compressor.ratio,
    )
}

// ── Aplicador (Android: Context + DSP, misma vía que applyToDsp) ────────────

/**
 * Aplica un archivo IVANNA OMEGA ya parseado al motor:
 *   FusedControls → safeSetFusedControls (Ruta A + daemon Ruta B) + save
 *   DSPState → onDspUpdated + pushToNative
 *   Etiqueta del archivo → ParameterStore (la UI la muestra en CONTROL).
 *
 * Fail-closed: si algo falla, devuelve false y loguea; no se deja un
 * estado a medias sin avisar (el fused y el DSPState se construyen
 * primero, puros, y solo después se aplican).
 */
object IvannaOmegaApplier {
    private const val TAG = "IvannaOmegaApplier"

    fun apply(
        context: Context,
        file: IvannaOmegaFile,
        currentDsp: DSPState,
        onDspUpdated: (DSPState) -> Unit,
    ): Boolean {
        return try {
            val store = ParameterStore(context)
            val currentFused = runCatching { store.loadFusedControls() }.getOrNull()
                ?: FusedControls()
            // Construcción pura primero (sin efectos secundarios).
            val newFused = file.toFusedControls(currentFused)
            val newDsp = file.toDspState(currentDsp)
            // Aplicación: misma vía que ProfileManager.applyToDsp.
            onDspUpdated(newDsp)
            runCatching { newDsp.pushToNative() }
                .onFailure { Log.e(TAG, "pushToNative: $it") }
            FusedControlsBridge.safeSetFusedControls(newFused)
            store.saveFusedControls(newFused)
            store.setOmegaFileLabel(file.displayLabel)
            Log.i(TAG, "Aplicado archivo IVANNA OMEGA: ${file.displayLabel}")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "No se pudo aplicar el archivo: ${t.message}", t)
            false
        }
    }
}
