package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import kotlinx.serialization.*
import kotlinx.serialization.json.Json
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.dsp.DSPState
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.neuromorphic.PiLstmBridge
import java.io.InputStreamReader

/**
 * ProfileManager — Carga y aplica presets de audio desde audio_profiles.json
 * Gestiona perfiles para bandas específicas (Steve Miller, RUSH, Budgie, Grand Funk)
 */

@Serializable
data class AudioEngineParams(
    val gain: Float,
    val exciterAmount: Float,
    val eqGain: Float,
    val widthAmount: Float,
    val bypass: Boolean = false,
    // v2.5.473 (§3.2/§3.6): EQ por banda. null = ausente en el JSON viejo →
    // se usa eqGain (fallback de compatibilidad, defecto §5B #1). eqGain
    // queda como fallback con valor 0.0 en el JSON nuevo; el código nuevo lo
    // ignora cuando existen las 4 bandas.
    val eqLowDb: Float? = null,
    val eqMidDb: Float? = null,
    val eqHighDb: Float? = null,
    val eqPresenceDb: Float? = null
)

@Serializable
data class AntiDolbyParams(
    val speechThreshold: Float,
    val bassThreshold: Float,
    val eqBoost2k4k: Float,
    val exciterLowOnly: Boolean = false,
    val widenerMultiplier: Float
)

@Serializable
data class NeuromorphicParams(
    val harmonicGain: Float,
    val lateralInhibition: Float,
    val ohcCompression: Float,
    val masterGainDb: Float,
    val cochlearBandwidth: String = "adaptive"
)

@Serializable
data class RouteParams(
    val bassBoostDb: Float,
    val dialogBoostDb: Float,
    val widenerMult: Float
)


// ── v2.5.473 (§3.6, schema v1.1.0) ──────────────────────────────────────────
// Bloques nuevos del perfil: ORBE/HOLOS/Φ/Volterra + EQ por banda + salida.
// Todos con default: un JSON v1.0.0 viejo sigue parseando (ignoreUnknownKeys
// + coerceInputValues). Los campos nullables de tonal/dynamics significan
// "ausente": se conserva el valor vigente en vez de inventar uno (así los
// otros 11 perfiles no cambian de sonido al aplicarlos).
@Serializable
data class SpatialProfileParams(
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
data class FieldProfileParams(
    // Unificación 2.5.509 (paso 1): defaults ATREVIDA (1.0 / 0.002), igual
    // que IvannaFieldProfileParams (ProfilesLoader). Antes 0.5 / 0.0:
    // dos dataclasses de perfil con distinto default de holos, y los
    // perfiles integrados que no especifican holosAmount explícito
    // tomaban 0.5 aquí y 1.0 allá. CONSECUENCIA: los perfiles integrados
    // sin holosAmount explícito pasan de HOLOS 0.5 a 1.0.
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
data class VolterraProfileParams(
    val correction: Float = 1.0f,
    val temporalAmount: Float = 0.0f,
    val emfEnabled: Boolean = true
)

@Serializable
data class SalaProfileParams(
    val rt60Sec: Float = 0.34f,
    val roomSizeM: Float = 5.0f,
    val roomWet: Float = 0.22f,
    val hearingProfile: Int = 0,
    val roomCorrDb: List<Float> = List(10) { 0.0f }
)

@Serializable
data class TonalProfileParams(
    val curve: Int? = null,
    val trimDb: Float? = null,
    val lowDb: Float? = null,
    val midDb: Float? = null,
    val highDb: Float? = null,
    val presenceDb: Float? = null,
    val loudnessComp: Boolean? = null
)

@Serializable
data class DynamicsProfileParams(
    val punch: Float? = null,
    val width: Float? = null,
    val safety: Float? = null,
    val compThresholdDb: Float? = null,
    val compRatio: Float? = null,
    val exciter: Float? = null,
    val voice: Float? = null
)

@Serializable
data class BrainProfileParams(
    val mode: Int = 0,
    val adaptation: Float = 1.0f
)

// Override por salida (§3.3): SOLO estos 3 campos difieren audífonos/Genezi.
// null = sin override → se usa el valor del bloque (headphones) o del perfil.
@Serializable
data class OutputOverrideParams(
    val masterGainDb: Float? = null,
    val tonalTrimDb: Float? = null,
    val phiPreset: Int? = null
)

@Serializable
data class OutputOverrides(
    val headphones: OutputOverrideParams = OutputOverrideParams(),
    val genezi: OutputOverrideParams = OutputOverrideParams()
)

@Serializable
data class AudioProfile(
    val id: String,
    val name: String,
    val description: String,
    val category: String,
    val priority: Int,
    val audioEngine: AudioEngineParams,
    val antiDolby: AntiDolbyParams,
    val neuromorphic: NeuromorphicParams,
    val route: RouteParams,
    val tags: List<String>,
    val recommendedFor: String,
    // v2.5.473 (§3.6): bloques fusionados + salida + referencia medible.
    val spatial: SpatialProfileParams = SpatialProfileParams(),
    val field: FieldProfileParams = FieldProfileParams(),
    val volterra: VolterraProfileParams = VolterraProfileParams(),
    val sala: SalaProfileParams = SalaProfileParams(),
    val tonal: TonalProfileParams = TonalProfileParams(),
    val dynamics: DynamicsProfileParams = DynamicsProfileParams(),
    val brain: BrainProfileParams = BrainProfileParams(),
    val output: OutputOverrides = OutputOverrides(),
    val referenceTrack: String = "",
    val targetLufsDeltaDb: Float = 0.0f,
    val lufsToleranceDb: Float = 0.5f
)

@Serializable
data class AudioProfilesContainer(
    val audioProfiles: List<AudioProfile>,
    val metadata: ProfileMetadata
)

@Serializable
data class ProfileMetadata(
    val version: String,
    val createdDate: String,
    val lastModified: String,
    val totalProfiles: Int,
    val trainingSequence: List<String>,
    val trainingNotes: String
)

class ProfileManager(private val context: Context, private val audioEngine: AudioEngine) {
    private val TAG = "ProfileManager"
    private var profiles: Map<String, AudioProfile> = emptyMap()
    private var currentProfileId: String? = null
    private var metadata: ProfileMetadata? = null

    init {
        loadProfiles()
    }

    /**
     * Carga los presets de audio desde audio_profiles.json (raw resources)
     */
    fun loadProfiles(): Boolean {
        return try {
            val resId = context.resources.getIdentifier(
                "audio_profiles", "raw", context.packageName
            )
            if (resId == 0) {
                Log.e(TAG, "Recurso raw/audio_profiles no encontrado")
                profiles = emptyMap()
                metadata = null
                return false
            }
            val inputStream = context.resources.openRawResource(resId)
            val reader = InputStreamReader(inputStream)
            val json = Json {
                ignoreUnknownKeys = true
                coerceInputValues = true
            }
            val container = json.decodeFromString<AudioProfilesContainer>(reader.readText())
            
            profiles = container.audioProfiles.associateBy { it.id }
            metadata = container.metadata
            
            Log.i(TAG, "✓ Cargados ${profiles.size} perfiles de audio")
            profiles.forEach { (id, profile) ->
                Log.d(TAG, "  • $id: ${profile.name}")
            }
            true
        } catch (e: Exception) {
            Log.e(TAG, "✗ Error cargando perfiles: ${e.message}", e)
            false
        }
    }

    /**
     * Aplica un perfil al AudioEngine (por ID)
     */
    fun applyProfile(profileId: String): Boolean {
        val profile = profiles[profileId] ?: run {
            Log.e(TAG, "Perfil no encontrado: $profileId")
            return false
        }

        return try {
            // Validar floats del JSON antes de JNI (valores corruptos crashean)
            val gain    = profile.audioEngine.gain.takeIf { it.isFinite() } ?: 0f
            val exciter = profile.audioEngine.exciterAmount.takeIf { it.isFinite() } ?: 0.3f
            val eqGain  = profile.audioEngine.eqGain.takeIf { it.isFinite() } ?: 0f
            val width   = profile.audioEngine.widthAmount.takeIf { it.isFinite() } ?: 0.5f
            // Aplicar parámetros de AudioEngine
            audioEngine.setGain(gain)
            audioEngine.setExciter(exciter)
            audioEngine.setEqGain(eqGain)
            audioEngine.setWidth(width)
            audioEngine.setBypass(profile.audioEngine.bypass)

            // Aplicar ruta de audio (vía instancia única de AudioEngine, sin duplicar chamada estática)
            audioEngine.setRouteProfile(
                profile.route.bassBoostDb,
                profile.route.dialogBoostDb,
                profile.route.widenerMult
            )
            val musicScore = (1f - profile.antiDolby.speechThreshold - profile.antiDolby.bassThreshold).coerceIn(0f, 1f)
            audioEngine.setAntiDolbyScores(
                profile.antiDolby.speechThreshold,
                musicScore,
                profile.antiDolby.bassThreshold
            )
            audioEngine.setManifoldEnabled(!profile.audioEngine.bypass)
            audioEngine.logCurrentBenchmark(
                speech = profile.antiDolby.speechThreshold,
                music = musicScore,
                bass = profile.antiDolby.bassThreshold,
                dolbyState = 0
            )

            currentProfileId = profileId
            Log.i(TAG, "✓ Aplicado perfil: ${profile.name}")
            true
        } catch (e: Exception) {
            Log.e(TAG, "✗ Error aplicando perfil $profileId: ${e.message}", e)
            false
        }
    }

    /**
     * Obtiene información detallada de un perfil
     */
    fun getProfile(profileId: String): AudioProfile? = profiles[profileId]

    /**
     * Lista todos los perfiles disponibles
     */
    fun getAllProfiles(): List<AudioProfile> = profiles.values.sortedBy { it.priority }

    /**
     * Obtiene la secuencia recomendada de entrenamiento
     */
    fun getTrainingSequence(): List<AudioProfile> {
        return metadata?.trainingSequence?.mapNotNull { profiles[it] } ?: emptyList()
    }

    /**
     * Obtiene el ID del perfil actual
     */
    fun getCurrentProfileId(): String? = currentProfileId

    /**
     * Obtiene el perfil actual
     */
    fun getCurrentProfile(): AudioProfile? = currentProfileId?.let { profiles[it] }

    /**
     * Busca perfiles por etiquetas
     */
    fun findProfilesByTag(tag: String): List<AudioProfile> {
        return profiles.values.filter { it.tags.contains(tag) }
    }

    /**
     * Busca perfiles por categoría
     */
    fun findProfilesByCategory(category: String): List<AudioProfile> {
        return profiles.values.filter { it.category == category }
    }

    /**
     * Obtiene las notas de entrenamiento
     */
    fun getTrainingNotes(): String? = metadata?.trainingNotes

    /**
     * Aplica un perfil por nombre (fuzzy search)
     */
    fun applyProfileByName(name: String): Boolean {
        val profile = profiles.values.find { 
            it.name.equals(name, ignoreCase = true) 
        }
        return if (profile != null) {
            applyProfile(profile.id)
        } else {
            Log.w(TAG, "Perfil no encontrado por nombre: $name")
            false
        }
    }

    /**
     * Obtiene estadísticas de los perfiles
     */
    fun getProfileStats(): String {
        val allProfiles = getAllProfiles()
        val avgGain = allProfiles.map { it.audioEngine.gain }.average()
        val avgExciter = allProfiles.map { it.audioEngine.exciterAmount }.average()
        val avgEq = allProfiles.map { it.audioEngine.eqGain }.average()
        val avgWidth = allProfiles.map { it.audioEngine.widthAmount }.average()

        return """
            ╔════════════════════════════════════════╗
            ║     ESTADÍSTICAS DE PERFILES IVANNA    ║
            ╚════════════════════════════════════════╝
            
            Total de perfiles: ${profiles.size}
            Versión: ${metadata?.version}
            Última actualización: ${metadata?.lastModified}
            
            PROMEDIOS:
            ├─ Gain:          ${"%.2f".format(avgGain)}
            ├─ Exciter:       ${"%.2f".format(avgExciter)}
            ├─ EQ (dB):       ${"%.2f".format(avgEq)}
            └─ Width:         ${"%.2f".format(avgWidth)}
            
            PERFILES DISPONIBLES:
            ${allProfiles.joinToString("\n") { 
                "├─ [${it.id}] ${it.name}\n" +
                "│  Tags: ${it.tags.joinToString(", ")}\n" +
                "│  Para: ${it.recommendedFor}"
            }}
        """.trimIndent()
    }


    /**
     * applyToDsp — ruta REAL magistral.
     * Aplica el perfil completo a la cadena DSP activa:
     *   DSPState → pushToNative (gain/exciter/eq/width/stereo)
     *   PiLstmBridge (harmonic/lateralInhib/ohc/masterGain)
     *   nativeSetRouteProfileStatic (bass/dialog/widener)
     *   nativeSetAntiDolbyScoresStatic (preset scores)
     *
     * @param currentDsp estado actual (se usa como base para copy)
     * @param onDspUpdated callback con el nuevo DSPState listo para
     *        asignar a dsp.value en el Composable caller.
     */
    fun applyToDsp(
        currentDsp: DSPState,
        profileId: String,
        onDspUpdated: (DSPState) -> Unit
    ): Boolean {
        val profile = profiles[profileId] ?: run {
            Log.e(TAG, "applyToDsp: perfil no encontrado: $profileId")
            return false
        }
        fun Float.safe(default: Float, min: Float, max: Float): Float =
            if (isFinite()) coerceIn(min, max) else default

        return try {
            val gainSafe    = profile.audioEngine.gain.safe(0f,  -24f, 24f)
            val exciterSafe = profile.audioEngine.exciterAmount.safe(0.3f, 0f, 1f)
            val eqSafe      = profile.audioEngine.eqGain.safe(0f,  -18f, 18f)
            val widthSafe   = profile.audioEngine.widthAmount.safe(1f, 0f, 2f)
            // v2.5.473 (§3.2): EQ POR BANDA — el defecto §5B #1 era aplicar el
            // mismo eqGain a las 4 bandas. Con las 4 bandas en el JSON, cada
            // una manda; sin ellas (JSON v1.0.0) se usa eqGain como antes.
            val lowSafe  = (profile.audioEngine.eqLowDb ?: profile.tonal.lowDb ?: eqSafe).safe(0f, -18f, 18f)
            val midSafe  = (profile.audioEngine.eqMidDb ?: profile.tonal.midDb ?: eqSafe).safe(0f, -18f, 18f)
            val highSafe = (profile.audioEngine.eqHighDb ?: profile.tonal.highDb ?: eqSafe).safe(0f, -18f, 18f)
            val presSafe = (profile.audioEngine.eqPresenceDb ?: profile.tonal.presenceDb ?: eqSafe).safe(0f, -18f, 18f)

            // v2.5.473 (§3.3/§3.6): override por salida. La puerta de bocinas
            // la decide el modo fusionado vigente (SPEAKERS = Genezi por DAC
            // USB); el JSON es uno solo, solo 3 campos cambian por salida.
            val fusedStore = ParameterStore(context)
            val currentFused = runCatching { fusedStore.loadFusedControls() }.getOrNull()
            val isGenezi = currentFused?.spatial?.mode == FusedSpatialMode.SPEAKERS
            val ov = if (isGenezi) profile.output.genezi else profile.output.headphones
            val masterGainSafe = (ov.masterGainDb ?: profile.neuromorphic.masterGainDb).safe(0f, -18f, 18f)
            val trimSafe = (ov.tonalTrimDb ?: profile.tonal.trimDb ?: 0f).safe(0f, -6f, 6f)
            val phiPresetSafe = (ov.phiPreset ?: profile.field.phiPreset).coerceIn(0, 2)

            val newDsp = currentDsp.copy(
                master       = gainSafe,
                wet          = exciterSafe,
                low          = lowSafe,
                mid          = midSafe,
                high         = highSafe,
                presence     = presSafe,
                stereoWidth  = widthSafe,
                compThreshold = profile.dynamics.compThresholdDb?.safe(-16f, -60f, 0f) ?: currentDsp.compThreshold,
                compRatio     = profile.dynamics.compRatio?.safe(2f, 1f, 20f) ?: currentDsp.compRatio
            )
            onDspUpdated(newDsp)
            runCatching { newDsp.pushToNative() }
                .onFailure { Log.e(TAG, "pushToNative: $it") }

            // v2.5.473 (§3.6.2b): el perfil también escribe FusedControls
            // completo (spatial/field/volterra/sala/tonal/dynamics/brain) vía
            // el puente único → save + push al daemon. Lo que el perfil NO
            // define se conserva del estado vigente (modo de salida, WFS,
            // upmix, XTC, parallax, sala firma y corrección MEDIDA de sala:
            // una corrección medida nunca la borra un perfil).
            runCatching {
                val cur = currentFused ?: FusedControls()
                val fused = FusedControls(
                    spatial = SpatialControls(
                        amount = profile.spatial.orbeAmount,
                        azimuthDeg = profile.spatial.orbeAzimuthDeg,
                        distanceM = profile.spatial.orbeDistanceM,
                        width = profile.spatial.orbeWidth,
                        mode = cur.spatial.mode,
                        wfsSpread = cur.spatial.wfsSpread,
                        hrtfWet = cur.spatial.hrtfWet,
                        headTracking = profile.spatial.orbeHeadTracking,
                        xtcEnabled = cur.spatial.xtcEnabled,
                        upmixEnabled = cur.spatial.upmixEnabled,
                        upmixImmersivity = cur.spatial.upmixImmersivity,
                        parallaxAmount = cur.spatial.parallaxAmount,
                        headRadiusM = profile.spatial.orbeHeadRadiusM
                    ),
                    field = FieldControls(
                        amount = profile.field.holosAmount,
                        phiAmount = profile.field.phiAmount,
                        phiPreset = FusedPhiPreset.fromJni(phiPresetSafe),
                        pumping = 0.0f,
                        coherence = profile.field.holosCoherence,
                        harmonics = profile.field.holosHarmonics,
                        gamma = profile.field.holosGamma,
                        warmth = profile.field.holosWarmth,
                        dynamics = profile.field.holosDynamics,
                        roomRt60 = profile.field.holosRoomRt60
                    ),
                    listenerRoom = ListenerRoomControls(
                        rt60Sec = profile.sala.rt60Sec,
                        roomSizeM = profile.sala.roomSizeM,
                        roomWet = profile.sala.roomWet,
                        hearingProfile = FusedHearingProfile.fromJni(profile.sala.hearingProfile),
                        signatureRoom = cur.listenerRoom.signatureRoom,
                        roomCorrDb = cur.listenerRoom.roomCorrDb
                    ),
                    transducer = TransducerControls(
                        correction = profile.volterra.correction,
                        temporalAmount = profile.volterra.temporalAmount,
                        emfEnabled = profile.volterra.emfEnabled
                    ),
                    tonal = TonalControls(
                        curve = profile.tonal.curve?.let { FusedTonalCurve.fromJni(it) } ?: cur.tonal.curve,
                        trimDb = trimSafe,
                        lowDb = lowSafe,
                        midDb = midSafe,
                        highDb = highSafe,
                        presenceDb = presSafe,
                        loudnessComp = profile.tonal.loudnessComp ?: cur.tonal.loudnessComp
                    ),
                    dynamics = DynamicsControls(
                        punch = profile.dynamics.punch ?: cur.dynamics.punch,
                        width = profile.dynamics.width ?: widthSafe,
                        safety = profile.dynamics.safety ?: cur.dynamics.safety,
                        compThresholdDb = profile.dynamics.compThresholdDb ?: cur.dynamics.compThresholdDb,
                        compRatio = profile.dynamics.compRatio ?: cur.dynamics.compRatio,
                        exciter = profile.dynamics.exciter ?: exciterSafe,
                        voice = profile.dynamics.voice ?: cur.dynamics.voice,
                        upmixImmersivity = cur.dynamics.upmixImmersivity
                    ),
                    brain = BrainControls(
                        mode = if (profile.brain.mode == 1) FusedBrainMode.MANUAL else FusedBrainMode.AUTO,
                        adaptation = profile.brain.adaptation
                    )
                ).clamp()
                FusedControlsBridge.safeSetFusedControls(fused)
                fusedStore.saveFusedControls(fused)
            }.onFailure { Log.e(TAG, "applyToDsp fused: $it") }

            if (PiLstmBridge.isReady && IvannaNativeLib.isLoaded) {
                runCatching {
                    PiLstmBridge.setHarmonicGain(profile.neuromorphic.harmonicGain.safe(0.3f, 0f, 1f))
                    PiLstmBridge.setBeta(profile.neuromorphic.lateralInhibition.safe(0.5f, 0f, 1f))
                    PiLstmBridge.setAlpha(profile.neuromorphic.ohcCompression.safe(0.4f, 0f, 1f))
                    PiLstmBridge.setMasterGain(masterGainSafe)
                }.onFailure { Log.e(TAG, "PiLstm neuro: $it") }
            }

            if (IvannaNativeLib.isLoaded) {
                runCatching {
                    audioEngine.setRouteProfile(
                        profile.route.bassBoostDb.safe(0f, -18f, 18f),
                        profile.route.dialogBoostDb.safe(0f, -18f, 18f),
                        profile.route.widenerMult.safe(1f, 0f, 3f)
                    )
                }.onFailure { Log.e(TAG, "route: $it") }

                val sp = profile.antiDolby.speechThreshold.safe(0.33f, 0f, 1f)
                val ba = profile.antiDolby.bassThreshold.safe(0.33f, 0f, 1f)
                val mu = (1f - sp - ba).coerceIn(0f, 1f)
                val total = sp + ba + mu
                val nSp = if (total > 0f) sp / total else 0.34f
                val nBa = if (total > 0f) ba / total else 0.33f
                val nMu = if (total > 0f) mu / total else 0.33f
                runCatching {
                    audioEngine.setAntiDolbyScores(nSp, nMu, nBa)
                    audioEngine.setManifoldEnabled(!profile.audioEngine.bypass)
                    audioEngine.logCurrentBenchmark(nSp, nMu, nBa, 0)
                }.onFailure { Log.e(TAG, "antiDolby scores: $it") }
            }

            currentProfileId = profileId
            runCatching { ParameterStore(context).setCurrentAudioProfileId(profileId) }
            Log.i(TAG, "applyToDsp OK: ${profile.name}")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "applyToDsp fatal: ${t.message}", t)
            false
        }
    }
}