package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import kotlinx.serialization.*
import kotlinx.serialization.json.Json
import java.io.InputStreamReader

/**
 * ProfilesLoader — coagulo único para cargar los presets de audio desde
 * app/src/main/res/raw/audio_profiles.json (12 perfiles, schema v1.1.0 en la
 * 2.5.473; el comentario viejo decía 4 — desactualizado desde que el JSON
 * creció a 12).
 *
 * Reutilizado por ProfileSelector.kt (UI) y por MainActivity cuando un
 * preset se aplica (MainActivity.applyAudioProfile(...) toma el AudioProfile
 * y lo replica a dspState / DSPBridge / globalEffectManager).
 *
 * Data class AudioProfile / AudioEngineParams / AntiDolbyParams /
 * NeuromorphicParams / RouteParams viven en este mismo archivo audio/
 * (definidos aquí también para no acoplarse a ProfileManager, que depende
 * de la clase AudioEngine y rota en una rama legacy — regla de oro: no
 * borramos, mantenemos ProfileManager intacto y agregamos una vía limpia).
 */
@Serializable
data class IvannaAudioProfile(
    val id: String,
    val name: String,
    val description: String,
    val category: String,
    val priority: Int,
    val audioEngine: IvannaAudioEngineParams,
    val antiDolby: IvannaAntiDolbyParams,
    val neuromorphic: IvannaNeuromorphicParams,
    val route: IvannaRouteParams,
    val tags: List<String>,
    val recommendedFor: String,
    val spatial: IvannaSpatialProfileParams = IvannaSpatialProfileParams(),
    val field: IvannaFieldProfileParams = IvannaFieldProfileParams(),
    val volterra: IvannaVolterraProfileParams = IvannaVolterraProfileParams(),
    val sala: IvannaSalaProfileParams = IvannaSalaProfileParams(),
    val tonal: IvannaTonalProfileParams = IvannaTonalProfileParams(),
    val dynamics: IvannaDynamicsProfileParams = IvannaDynamicsProfileParams(),
    val brain: IvannaBrainProfileParams = IvannaBrainProfileParams(),
    val output: IvannaOutputOverrides = IvannaOutputOverrides(),
    val referenceTrack: String = "",
    val targetLufsDeltaDb: Float = 0.0f,
    val lufsToleranceDb: Float = 0.5f
)

@Serializable
data class IvannaAudioEngineParams(
    val gain: Float,
    val exciterAmount: Float,
    val eqGain: Float,
    val widthAmount: Float,
    val bypass: Boolean = false,
    // v2.5.473 (§3.6): EQ por banda, espejo de AudioEngineParams.
    val eqLowDb: Float? = null,
    val eqMidDb: Float? = null,
    val eqHighDb: Float? = null,
    val eqPresenceDb: Float? = null
)

// v2.5.473 (§3.6, M9): espejo Ivanna* de los bloques nuevos del schema
// v1.1.0. Mismos defaults que en ProfileManager; un JSON v1.0.0 viejo sigue
// parseando. Cualquier campo nuevo va en AMBOS lados o un lado lo pierde.
@Serializable
data class IvannaSpatialProfileParams(
    val orbeAmount: Float = 1.0f,
    val orbeDistanceM: Float = 0.35f,
    val orbeSpread: Float = 1.0f,
    val orbeProximityDb: Float = 3.0f,
    val orbeDiffuse: Float = 0.5f,
    val orbeHeadTracking: Boolean = false,
    val orbeAzimuthDeg: Float = 0.0f,
    val orbeWidth: Float = 1.0f,
    val orbeHeadRadiusM: Float = 0.0875f
)

@Serializable
data class IvannaFieldProfileParams(
    // Default = preset ATREVIDA 502 (directiva Esse 2026-11-24): los
    // perfiles que no especifican HOLOS heredan el default de fábrica.
    val holosAmount: Float = 1.0f,
    val holosHarmonics: Float = 0.002f,
    val holosWarmth: Float = 0.0f,
    val holosDynamics: Float = 1.0f,
    val holosGamma: Float = 1.0f,
    val holosCoherence: Float = 0.8f,
    val holosRoomRt60: Float = 0.34f,
    val phiAmount: Float = 0.5f,
    val phiPreset: Int = 0
)

@Serializable
data class IvannaVolterraProfileParams(
    val correction: Float = 1.0f,
    val temporalAmount: Float = 0.0f,
    val emfEnabled: Boolean = true
)

@Serializable
data class IvannaSalaProfileParams(
    val rt60Sec: Float = 0.34f,
    val roomSizeM: Float = 5.0f,
    val roomWet: Float = 0.22f,
    val hearingProfile: Int = 0,
    val roomCorrDb: List<Float> = List(10) { 0.0f }
)

@Serializable
data class IvannaTonalProfileParams(
    val curve: Int? = null,
    val trimDb: Float? = null,
    val lowDb: Float? = null,
    val midDb: Float? = null,
    val highDb: Float? = null,
    val presenceDb: Float? = null,
    val loudnessComp: Boolean? = null
)

@Serializable
data class IvannaDynamicsProfileParams(
    val punch: Float? = null,
    val width: Float? = null,
    val safety: Float? = null,
    val compThresholdDb: Float? = null,
    val compRatio: Float? = null,
    val exciter: Float? = null,
    val voice: Float? = null
)

@Serializable
data class IvannaBrainProfileParams(
    val mode: Int = 0,
    val adaptation: Float = 1.0f
)

@Serializable
data class IvannaOutputOverrideParams(
    val masterGainDb: Float? = null,
    val tonalTrimDb: Float? = null,
    val phiPreset: Int? = null
)

@Serializable
data class IvannaOutputOverrides(
    val headphones: IvannaOutputOverrideParams = IvannaOutputOverrideParams(),
    val genezi: IvannaOutputOverrideParams = IvannaOutputOverrideParams()
)

@Serializable
data class IvannaAntiDolbyParams(
    val speechThreshold: Float,
    val bassThreshold: Float,
    val eqBoost2k4k: Float,
    val exciterLowOnly: Boolean = false,
    val widenerMultiplier: Float
)

@Serializable
data class IvannaNeuromorphicParams(
    val harmonicGain: Float,
    val lateralInhibition: Float,
    val ohcCompression: Float,
    val masterGainDb: Float,
    val cochlearBandwidth: String = "adaptive"
)

@Serializable
data class IvannaRouteParams(
    val bassBoostDb: Float,
    val dialogBoostDb: Float,
    val widenerMult: Float
)

@Serializable
data class IvannaProfilesContainer(
    val audioProfiles: List<IvannaAudioProfile>,
    val metadata: IvannaProfileMetadata
)

@Serializable
data class IvannaProfileMetadata(
    val version: String,
    val createdDate: String,
    val lastModified: String,
    val totalProfiles: Int,
    val trainingSequence: List<String>,
    val trainingNotes: String
)

object ProfilesLoader {
    private const val TAG = "ProfilesLoader"
    private const val RES_NAME = "audio_profiles"
    private const val FILE_NAME = "audio_profiles.json"
    private const val BACKUP_FILE_NAME = "audio_profiles.json.bak"
    private const val TMP_FILE_NAME = "audio_profiles.json.tmp"
    private const val ASSET_NAME = "audio_profiles.json"

    private val json = Json {
        ignoreUnknownKeys = true
        coerceInputValues = true
    }

    private fun parse(text: String): List<IvannaAudioProfile> {
        val container = json.decodeFromString(IvannaProfilesContainer.serializer(), text)
        return container.audioProfiles.sortedBy { it.priority }
    }

    /**
     * Carga perfiles. Orden de resolución (primer origen que existe gana,
     * manteniendo compatibilidad con perfiles ya guardados):
     *   1) filesDir/audio_profiles.json     — override runtime editable por el usuario
     *   2) assets/audio_profiles.json       — versionado con el APK
     *   3) res/raw/audio_profiles.json      — fallback histórico (compat original)
     */
    fun load(context: Context): List<IvannaAudioProfile> {
        // 1) filesDir override. C8: si el principal no parsea (corte a
        // mitad de escritura en una versión vieja sin tmp+rename, o disco
        // corrupto), se restaura el .bak y se registra; si el .bak tampoco
        // parsea, se sigue al fallback de assets/res como antes.
        val override = java.io.File(context.filesDir, FILE_NAME)
        if (override.isFile && override.length() > 0) {
            runCatching { parse(override.readText(Charsets.UTF_8)) }
                .onSuccess { list ->
                    Log.i(TAG, "✓ Cargados ${list.size} perfiles (filesDir)")
                    return list
                }
                .onFailure { Log.w(TAG, "override corrupto (${it.message}); intentando .bak") }
            if (restoreBackup(context.filesDir)) {
                runCatching { parse(override.readText(Charsets.UTF_8)) }
                    .onSuccess { list ->
                        Log.i(TAG, "✓ Cargados ${list.size} perfiles (filesDir, restaurado desde .bak)")
                        return list
                    }
                    .onFailure { Log.w(TAG, ".bak restaurado pero tampoco parsea; se sigue al fallback") }
            }
        }
        // 2) assets
        try {
            context.assets.open(ASSET_NAME).use { stream ->
                val list = parse(InputStreamReader(stream).readText())
                Log.i(TAG, "✓ Cargados ${list.size} perfiles (assets)")
                return list
            }
        } catch (_: Exception) { /* asset opcional */ }
        // 3) res/raw (compat)
        return try {
            val resId = context.resources.getIdentifier(RES_NAME, "raw", context.packageName)
            if (resId == 0) {
                Log.w(TAG, "Recurso raw/$RES_NAME no encontrado")
                return emptyList<IvannaAudioProfile>()
            }
            context.resources.openRawResource(resId).use { stream ->
                val list = parse(InputStreamReader(stream).readText())
                Log.i(TAG, "✓ Cargados ${list.size} perfiles (res/raw)")
                list
            }
        } catch (e: Exception) {
            Log.e(TAG, "Error cargando profiles: ${e.message}", e)
            emptyList<IvannaAudioProfile>()
        }
    }

    /** Guarda una lista de perfiles como override en filesDir (para editor UI). */
    fun saveOverride(context: Context, profiles: List<IvannaAudioProfile>, meta: IvannaProfileMetadata) {
        try {
            val container = IvannaProfilesContainer(profiles, meta)
            val text = json.encodeToString(IvannaProfilesContainer.serializer(), container)
            writeOverrideAtomic(context.filesDir, text)
        } catch (e: Exception) {
            Log.e(TAG, "saveOverride falló: ${e.message}", e)
        }
    }

    /**
     * C8 (a): escritura atómica del override, mismo patrón que
     * `OrganismQueue.writeItem()`: antes de reemplazar, el archivo vigente
     * se copia a `audio_profiles.json.bak`; el contenido nuevo se escribe
     * a `audio_profiles.json.tmp` y solo entonces se renombra al principal.
     * Un corte de proceso deja el principal intacto (el .tmp huérfano se
     * ignora y se sobrescribe en la próxima escritura). Puro java.io,
     * testeable en JVM sobre un directorio cualquiera.
     */
    internal fun writeOverrideAtomic(dir: java.io.File, text: String) {
        dir.mkdirs()
        val dest = java.io.File(dir, FILE_NAME)
        val backup = java.io.File(dir, BACKUP_FILE_NAME)
        val tmp = java.io.File(dir, TMP_FILE_NAME)
        if (dest.isFile) dest.copyTo(backup, overwrite = true)
        tmp.writeText(text, Charsets.UTF_8)
        if (!tmp.renameTo(dest)) {
            tmp.copyTo(dest, overwrite = true)
            tmp.delete()
        }
    }

    /**
     * C8 (a): restaura el `.bak` sobre el principal (vía tmp+rename) solo
     * si el `.bak` parsea como contenedor válido. Devuelve true si el
     * principal quedó restaurado. Un `.bak` corrupto o ausente no toca
     * nada: el llamador sigue al fallback de assets/res.
     */
    internal fun restoreBackup(dir: java.io.File): Boolean {
        val backup = java.io.File(dir, BACKUP_FILE_NAME)
        if (!backup.isFile || backup.length() == 0L) return false
        return runCatching {
            val text = backup.readText(Charsets.UTF_8)
            parse(text) // solo se restaura contenido que parsea
            val dest = java.io.File(dir, FILE_NAME)
            val tmp = java.io.File(dir, TMP_FILE_NAME)
            tmp.writeText(text, Charsets.UTF_8)
            if (!tmp.renameTo(dest)) {
                tmp.copyTo(dest, overwrite = true)
                tmp.delete()
            }
            Log.i(TAG, "override restaurado desde .bak")
            true
        }.onFailure {
            Log.w(TAG, "restoreBackup falló (.bak no parsea o no se pudo escribir): ${it.message}")
            false
        }.getOrDefault(false)
    }

    fun loadMetadata(context: Context): IvannaProfileMetadata? = try {
        val resId = context.resources.getIdentifier(RES_NAME, "raw", context.packageName)
        context.resources.openRawResource(resId).use { stream ->
            json.decodeFromString(
                IvannaProfilesContainer.serializer(),
                InputStreamReader(stream).readText()
            ).metadata
        }
    } catch (_: Exception) { null }
}
