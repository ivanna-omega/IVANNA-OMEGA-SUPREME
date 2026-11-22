package com.ivanna.omega

import androidx.compose.foundation.Image
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.layout.ContentScale
import com.ivanna.omega.spatial.IvannaSpatialEngine
import android.media.AudioManager
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.launch
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.verticalScroll
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.livedata.observeAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.draw.clip
import androidx.compose.ui.draw.rotate
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.lifecycle.viewmodel.compose.viewModel
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import com.ivanna.omega.audio.AdaptiveBackend
import com.ivanna.omega.audio.AudioState
import com.ivanna.omega.audio.AudioStateManager
import com.ivanna.omega.audio.EffectsOffTransition
import com.ivanna.omega.audio.AdaptiveMode
import com.ivanna.omega.neuromorphic.IvannaDspManager
import com.ivanna.omega.ui.AdaptiveTelemetrySnapshot
import com.ivanna.omega.audio.toSnapshot
import com.ivanna.omega.audio.ProfilesLoader
import com.ivanna.omega.neuromorphic.PiLstmBridge
import com.ivanna.omega.magisk.OmegaEngineBridge
import com.ivanna.omega.ui.MagiskStatusPanel
import com.ivanna.omega.ui.NetworkStatusPanel
import com.ivanna.omega.ui.ProfileSelectorScreen
import com.ivanna.omega.audio.VoiceProtectionManager
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.ui.AdaptiveEngineScreen
import com.ivanna.omega.ui.IvannaControlPanel
import com.ivanna.omega.audio.AntiDolbyController
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.OmegaEngine
import com.ivanna.omega.dsp.DSPBridge
import com.ivanna.omega.dsp.DSPState
import com.ivanna.omega.dsp.DSPStatePrefs
import android.app.Activity
import android.content.Intent
import android.media.projection.MediaProjectionManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import com.ivanna.omega.audio.PlaybackCaptureService
import kotlin.math.PI
import android.net.Uri
import com.ivanna.omega.audio.IvannaBridgePlayer
import com.ivanna.omega.audio.VolterraSwitch
import com.ivanna.omega.ui.IvannaRoute
import com.ivanna.omega.ui.BridgePlayerCard
import kotlin.math.log10
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.withContext
import com.ivanna.omega.magisk.ShmManager
import com.ivanna.omega.core.PresetManager
import com.ivanna.omega.audio.AudioRoutingManager
import com.ivanna.omega.audio.ParameterValidator
import com.ivanna.omega.ai.RealtimeLearningController
import com.ivanna.omega.audio.AudioPipeline
import com.ivanna.omega.ai.PerceptualBrainEngine
import com.ivanna.omega.ai.PerceptualSnapshot
import com.ivanna.omega.ui.SpatialControlPanel
import com.ivanna.omega.ui.MainScaffold
import com.ivanna.omega.ui.Bark64VisualizerPanel
import com.ivanna.omega.audio.IvannaLabMonitor
import com.ivanna.omega.ui.ScreenSpecCard
import com.ivanna.omega.ui.CollapsibleScreenSpecCard
import com.ivanna.omega.ui.SpecInfo
import com.ivanna.omega.ui.SpecInfoButton
import com.ivanna.omega.ui.theme.IvannaTheme
import com.ivanna.omega.ui.theme.MagistralGold
import com.ivanna.omega.ui.theme.MagistralGoldHi
import com.ivanna.omega.ui.theme.MagistralLine
import com.ivanna.omega.ui.theme.MagistralLineStrong
import com.ivanna.omega.ui.theme.MagistralMuted
import com.ivanna.omega.ui.theme.MagistralPanel
import com.ivanna.omega.ui.theme.MagistralPanel2
import com.ivanna.omega.ui.theme.MagistralSerif
import com.ivanna.omega.ui.theme.MagistralText
import com.ivanna.omega.ui.theme.MagistralVoid
import com.ivanna.omega.ui.theme.magistralGoldBrush

// ── Palette (Aurora Obsidiana v3.0 — alineada con ui/theme/IvannaTheme.kt) ──
// Mismos nombres de variable (drop-in, sin tocar los ~50 usos internos) pero
// con los valores del tema central: profundidad azul-obsidiana en vez de
// gris plano, cian aurora suavizado (el 00F5FF puro fatigaba la vista),
// magenta reducido de puro saturado al tono del tema, texto con jerarquía.
private val Carbon = Color(0xFF010204)      // ObsidianVoid
private val Surface1 = Color(0xFF0A101C)    // ObsidianSoft
private val Surface2 = Color(0xFF0D1524)    // ObsidianGlass
private val Border1 = Color(0xFF223050)     // ObsidianEdge
private val CyanGlow = Color(0xFF6FF3FF)    // AuroraCyan
private val CyanDim = Color(0x556FF3FF)     // AuroraCyanGlow
private val GoldGlow = Color(0xFFFFD976)    // OmniGoldCore
private val MagentaGlow = Color(0xFFFF3E86) // NeonMagenta
private val NeonMagenta  = Color(0xFFFF3E86)
private val MagentaDim = Color(0x55FF3E86)  // NeonMagentaGlow
private val TextPri = Color(0xFFF1F8FF)     // TextPrimary
private val TextSec = Color(0xFF93A8C6)     // TextSecondary
private val TextMid = Color(0xFF57708F)     // TextMuted

/**
 * PerceptualDspRecommendations - Computed DSP output parameters from Perceptual Brain evaluation.
 */
data class PerceptualDspRecommendations(
    val compressorAmount: Float = 0.35f,
    val exciterReduction: Float = 0.15f,
    val eqHighCut: Float = 18000.0f,
    val spatialWidth: Float = 1.20f,
    val loudnessTarget: Float = -14.0f
)

/**
 * PerceptualDecisionEngine - Real-time Psychoacoustic & TinyML decision controller.
 * Evaluates fatigue, immersion, ISO 226 loudness, masking efficiency, dynamic range,
 * and ConvNeXt INT8 confidence to produce real-time native DSP adjustments.
 */
class PerceptualDecisionEngine {
    fun evaluate(snapshot: PerceptualSnapshot): PerceptualDspRecommendations {
        val fatigueFactor = snapshot.fatigue.coerceIn(0f, 1f)
        val immersionFactor = snapshot.immersion.coerceIn(0f, 1f)
        val loudnessIso226 = snapshot.iso226LoudnessDb
        val maskingEff = snapshot.maskingEfficiency
        val dynRange = snapshot.dynamicRangeDb
        val confidence = snapshot.convNextConfidence

        // Psychoacoustic rules calculation
        val exciterRed = (fatigueFactor * 0.5f + (1.0f - confidence) * 0.2f).coerceIn(0f, 0.8f)
        val highCut = if (fatigueFactor > 0.5f) (18000f - fatigueFactor * 4000f) else 20000f
        val compAmount = (0.2f + (1.0f - (dynRange / 24f).coerceIn(0f, 1f)) * 0.5f + fatigueFactor * 0.2f).coerceIn(0.1f, 0.9f)
        val width = (immersionFactor * 1.4f * (1.0f - fatigueFactor * 0.3f)).coerceIn(0.5f, 1.8f)
        val targetLoudness = (-14.0f + (loudnessIso226 - 84f) * 0.1f + maskingEff * 2.0f).coerceIn(-24.0f, -8.0f)

        return PerceptualDspRecommendations(
            compressorAmount = compAmount,
            exciterReduction = exciterRed,
            eqHighCut = highCut,
            spatialWidth = width,
            loudnessTarget = targetLoudness
        )
    }

    fun dispatchRecommendations(recommendations: PerceptualDspRecommendations) {
        if (IvannaNativeLib.isLoaded) {
            runCatching {
                IvannaNativeLib.nativeSetSpatialWidthDirect(recommendations.spatialWidth)
                IvannaNativeLib.nativeSetHarmonicGain(1.0f - recommendations.exciterReduction)
                IvannaNativeLib.nativeSetAntiDolbyIntensity(recommendations.compressorAmount)
            }
        }
    }
}

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        val am = getSystemService(AUDIO_SERVICE) as AudioManager
        val sr = am.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE)?.toIntOrNull() ?: 48000
        // La inicialización JNI/DSP puede tardar al cargar el motor o despertar
        // hilos nativos. No bloquear el hilo de UI: hacerlo en IO para evitar
        // jank/ANR en el primer frame. El sample rate sigue siendo el del HAL.
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching { DSPBridge.init(sr) }
            // Seguridad anti-piratería (v2.5.369): si la integridad falló
            // (IntegrityGate.isLocked), no inicializar el DSP ni crear el
            // motor adaptativo. Fail-closed: audio en passthrough.
            if (!com.ivanna.omega.security.IntegrityGate.isLocked && IvannaNativeLib.isLoaded) {
                val dspReady = runCatching {
                    IvannaNativeLib.nativeInitDSP(sr)
                }.getOrDefault(false)

                // AudioStateManager is restored by attachPersistence() before
                // this bootstrap. Read manual mode and perform the native
                // toggle/create in one Main.immediate section so a UI toggle
                // cannot slip between the check and native engine creation.
                val (adaptiveRequested, adaptiveHandle, adaptiveRunning) =
                    kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Main.immediate) {
                        val preferred =
                            com.ivanna.omega.ui.AdaptiveControlsPrefs.load(applicationContext).binauralAdaptEnabled &&
                            !com.ivanna.omega.audio.AudioStateManager.audioState.value.manualModeEnabled
                        val requested = preferred && dspReady
                        // Conservar la preferencia aunque DSP falle: el watchdog
                        // podrá recuperar el motor cuando la ruta vuelva a estar lista.
                        runCatching {
                            IvannaNativeLib.nativeSetAdaptiveEngineEnabled(preferred)
                        }.onFailure {
                            android.util.Log.e("IVANNA.Boot", "No se pudo aplicar el modo adaptativo solicitado", it)
                        }
                        val handle = if (requested) {
                            runCatching { IvannaNativeLib.nativeCreateAdaptiveEngine() }.getOrDefault(0L)
                        } else {
                            0L
                        }
                        val running = requested && handle != 0L &&
                            runCatching { IvannaNativeLib.nativeIsAdaptiveEngineRunning() }.getOrDefault(false)
                        Triple(requested, handle, running)
                    }

                if (adaptiveRequested && adaptiveRunning) {
                    android.util.Log.i("IVANNA.Boot", "AdaptiveDecisionEngine iniciado y vivo")
                } else if (dspReady && !adaptiveRequested) {
                    android.util.Log.i("IVANNA.Boot", "Modo manual/adaptativo apagado por preferencia; DSP preservado")
                } else if (dspReady) {
                    android.util.Log.e("IVANNA.Boot", "AdaptiveDecisionEngine NOT_READY (handle=$adaptiveHandle); DSP preservado")
                } else {
                    android.util.Log.e("IVANNA.Boot", "DSP nativo no inicializado; ruta NOT_READY")
                }
                // El control loop es de proceso, no de pantalla: debe seguir
                // publicando ControlFrames aunque el usuario salga del Dashboard.
                runCatching { IvannaNativeLib.nativeStartEvoThread() }
                com.ivanna.omega.audio.IvannaControlLoop.start()
            }
        }
        // FIX (root / sin root): elige el backend real de procesado. En
        // dispositivos sin root arranca el camino AudioEffect +
        // DynamicsProcessing en vez de escribir a un daemon inexistente.
        com.ivanna.omega.audio.AudioBackendSelector.start(applicationContext)
        // FIX (socket DESCONECTADO tras boot): startKeepalive() existía pero
        // NADIE lo invocaba — el hilo de probe cada 5 s nunca arrancaba y el
        // panel Magisk quedaba en DESCONECTADO hasta pulsar RECONECTAR a mano.
        // Se arranca aquí: el bridge se auto-reconecta en cuanto el daemon
        // bindea, sin intervención del usuario.
        OmegaEngineBridge.startKeepalive()
        // Idempotente: si el renderer HRTF se liberó (cierre real de la Activity con el proceso vivo) se
        // vuelve a crear aquí; si ya está listo o en curso no hace nada.
        com.ivanna.omega.spatial.IvannaSpatialManager.init(applicationContext)
        // LICENCIAS (v2.5.446): validación al arrancar, en IO, sin bloquear
        // la UI ni el audio. Fail-open con caché: si no hay red o el
        // servidor no responde, la app sigue funcionando como hoy.
        // El estado queda en LicenseManager para la UI y la telemetría.
        lifecycleScope.launch(Dispatchers.IO) {
            runCatching { com.ivanna.omega.licensing.LicenseManager(applicationContext).refresh() }
                .onFailure { android.util.Log.w("IVANNA.License", "refresh de licencia falló", it) }
        }
        setContent { com.ivanna.omega.ui.AcousticConsentGate { OmegaApp() } }
    }

    override fun onDestroy() {
        super.onDestroy()
        // FIX (HRTF «Renderer no inicializado» tras girar la pantalla): onDestroy también corre en
        // cada cambio de configuración (rotación), pero el proceso y la Application siguen vivos y
        // IvannaSpatialManager.init() solo se llamaba UNA vez en Application.onCreate → tras el primer
        // giro el renderer HRTF, el motor adaptativo y la SHM se destruían y nunca volvían.
        if (isChangingConfigurations) return
        runCatching { com.ivanna.omega.audio.IvannaControlLoop.stop() }
        runCatching { com.ivanna.omega.spatial.IvannaSpatialManager.release() }
        runCatching { ShmManager.release() }
        if (IvannaNativeLib.isLoaded) {
            runCatching { IvannaNativeLib.nativeSaveEvoState() }
            runCatching { IvannaNativeLib.nativeDestroyAdaptiveEngine() }
        }
    }
}

/**
 * DevGate — v2.5.473 (§1.2 fase 3): las pantallas internas / de diagnóstico
 * solo existen detrás del Modo desarrollador (SISTEMA → 7 toques en la
 * versión). Sin él, la ruta muestra el aviso y vuelve atrás. Ni la ruta ni
 * el archivo se borran: un desarrollador la re-activa sin recompilar.
 */
@Composable
private fun DevGate(nav: androidx.navigation.NavHostController, content: @Composable () -> Unit) {
    val context = LocalContext.current
    if (com.ivanna.omega.ui.DevMode.isEnabled(context)) {
        content()
    } else {
        androidx.compose.foundation.layout.Column(
            modifier = Modifier.fillMaxSize()
                .background(com.ivanna.omega.ui.theme.ObsidianVoid)
                .padding(24.dp),
            verticalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(12.dp)
        ) {
            androidx.compose.material3.Text(
                "PANTALLA INTERNA",
                color = com.ivanna.omega.ui.theme.TextSecondary,
                fontSize = 12.sp, fontWeight = androidx.compose.ui.text.font.FontWeight.Bold
            )
            androidx.compose.material3.Text(
                "Esta pantalla es diagnóstico interno y está detrás del Modo desarrollador (SISTEMA → toca 7 veces la versión).",
                color = com.ivanna.omega.ui.theme.TextPrimary, fontSize = 14.sp
            )
            androidx.compose.material3.Button(onClick = { nav.popBackStack() }) {
                androidx.compose.material3.Text("VOLVER")
            }
        }
    }
}

@Composable
fun OmegaApp() {
    val nav = rememberNavController()
    val appContext = LocalContext.current.applicationContext
    // FIX (persistencia): DSPStatePrefs.load/save existian pero NADIE los
    // llamaba en todo el repo (grep DSPStatePrefs = 0 hits fuera de su
    // archivo). El estado arrancaba SIEMPRE en DSPState() por defecto y todo
    // lo que el usuario ajustaba se perdia al cerrar la app. Ahora se carga
    // al entrar y se guarda con debounce en cada cambio.
    val dsp = remember { mutableStateOf(DSPStatePrefs.load(appContext)) }
    LaunchedEffect(Unit) {
        // Re-aplicar al DSP nativo/daemon lo que se acaba de restaurar.
        runCatching { dsp.value.pushToNative() }
        runCatching {
            com.ivanna.omega.ui.SupremeAxesPrefs.applyToNative(
                com.ivanna.omega.ui.SupremeAxesPrefs.load(appContext)
            )
        }
        snapshotFlow { dsp.value }.collectLatest { state ->
            delay(400)  // debounce: los sliders emiten decenas de valores/seg
            runCatching { DSPStatePrefs.save(appContext, state) }
        }
    }
    // v2.5.448: IvannaTheme (Aurora Obsidiana) envuelve TODA la app. Antes
    // un MaterialTheme genérico con darkColorScheme(background=Carbon,
    // surface=Surface1) pisaba el tema central: su `primary` era el morado
    // por defecto de Material, así que TODOS los Switch/Slider/Button/
    // Checkbox sin color explícito salían morados en las 27+ pantallas.
    // IvannaTheme usa primary=AuroraCyan, background=ObsidianVoid (=Carbon)
    // y surface=ObsidianSoft (=Surface1): mismo fondo, primario magistral.
    IvannaTheme {
        val context = LocalContext.current
        // Revisor de actualizaciones (v2.5.444): al abrir la app revisa el
        // latest release de GitHub (como mucho una vez al día) y, si hay
        // versión nueva, muestra el diálogo "Nueva versión disponible"
        // (Instalar / Más tarde). Sin red o sin versión nueva: silencioso.
        com.ivanna.omega.ui.AppUpdateHost()
        var captureRequested by remember { mutableStateOf(false) }
        val captureActive by PlaybackCaptureService.isCapturing.collectAsState()
        val projectionManager = context.getSystemService(MediaProjectionManager::class.java)
        val projectionLauncher = rememberLauncherForActivityResult(
            ActivityResultContracts.StartActivityForResult()
        ) { result ->
            if (result.resultCode == Activity.RESULT_OK && result.data != null) {
                val intent = Intent(context, PlaybackCaptureService::class.java).apply {
                    putExtra("resultCode", result.resultCode)
                    putExtra("data", result.data)
                }
                context.startForegroundService(intent)
                captureRequested = true
            }
        }

        // La captura de audio del sistema (AudioPlaybackCapture) exige RECORD_AUDIO además del
        // consentimiento de MediaProjection. Sin él el AudioRecord queda sin inicializar y la captura
        // falla en silencio. El micrófono se pide aquí, solo cuando el usuario activa la captura.
        val micForCaptureLauncher = rememberLauncherForActivityResult(
            ActivityResultContracts.RequestPermission()
        ) { granted ->
            if (granted) {
                projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
            } else {
                val st = com.ivanna.omega.hardware.PermissionGate.stateFor(context, android.Manifest.permission.RECORD_AUDIO)
                android.widget.Toast.makeText(
                    context, com.ivanna.omega.hardware.PermissionPolicy.micDeniedMessage(st), android.widget.Toast.LENGTH_LONG
                ).show()
            }
        }
        val startCapture: () -> Unit = {
            val micState = com.ivanna.omega.hardware.PermissionGate.stateFor(context, android.Manifest.permission.RECORD_AUDIO)
            when (micState) {
                com.ivanna.omega.hardware.PermissionState.GRANTED ->
                    projectionLauncher.launch(projectionManager.createScreenCaptureIntent())
                com.ivanna.omega.hardware.PermissionState.OPEN_SETTINGS -> {
                    android.widget.Toast.makeText(
                        context, com.ivanna.omega.hardware.PermissionPolicy.micDeniedMessage(micState), android.widget.Toast.LENGTH_LONG
                    ).show()
                    com.ivanna.omega.hardware.PermissionGate.openAppSettings(context)
                }
                else -> {
                    com.ivanna.omega.hardware.PermissionGate.markRequested(context, android.Manifest.permission.RECORD_AUDIO)
                    micForCaptureLauncher.launch(android.Manifest.permission.RECORD_AUDIO)
                }
            }
        }

        val adaptiveBackend = remember { AdaptiveBackend(context) }
        val voiceProtectionManager = remember {
            com.ivanna.omega.audio.VoiceProtectionManager(
                com.ivanna.omega.audio.ParameterStore(context)
            )
        }
        // MÚSICA LOCAL (v2.5.447): el reproductor vive en el ámbito de la
        // actividad, NO de la pestaña: la música sigue sonando al cambiar
        // de pestaña (CONTROL ↔ MÚSICA) o al ir a otras pantallas.
        // TIDAL (v2.5.416–v2.5.446) quedó LEGACY (2026-11-15, directiva de
        // Esse): el programa oficial no incluye reproducción completa y la
        // vía no oficial es fuera de términos y revocable. Su cliente ya no
        // se crea aquí; el código se conserva marcado @Deprecated.
        // El DashboardScreen legacy creaba su propio player y lo liberaba al
        // salir (~líneas 1187-1188); ese código está muerto (nadie llama a
        // DashboardScreen) y no se toca.
        val bridgePlayer = remember { IvannaBridgePlayer(context) }
        DisposableEffect(bridgePlayer) {
            com.ivanna.omega.audio.MediaSessionManager.init(context, bridgePlayer)
            onDispose {
                com.ivanna.omega.audio.MediaSessionManager.release()
                com.ivanna.omega.audio.BridgePlaybackService.stop(context)
                bridgePlayer.release()
            }
        }
        // Sondeo del estado del reproductor (v2.5.447: ya sin Tidal — el
        // avance de cola vive en LocalMusicScreen vía onQueueAdvance).
        // Vive aquí, no en la pantalla, para que el servicio en primer
        // plano se mantenga aunque se esté en otra pestaña.
        LaunchedEffect(bridgePlayer) {
            while (true) {
                // v2.5.438: servicio en primer plano mientras suena el
                // BridgePlayer. Sin él, al cambiar a otra app Android
                // estrangula el proceso y el hilo de audio no alcanza a
                // llenar el buffer (metralleta). Solo corre en PLAYING;
                // en pausa/parado se detiene (no hay audio que proteger).
                // Idempotente (el servicio lleva su propio flag isRunning).
                if (bridgePlayer.state == IvannaBridgePlayer.State.PLAYING) {
                    com.ivanna.omega.audio.BridgePlaybackService.start(context)
                } else {
                    com.ivanna.omega.audio.BridgePlaybackService.stop(context)
                }
                kotlinx.coroutines.delay(100L)
            }
        }
        NavHost(nav, startDestination = "dashboard") {
            // MusicIntelligencePanel estaba huérfano (0 referencias): sin ruta ni acceso desde la UI.
            composable("music_intelligence") {
                DevGate(nav) {
                                androidx.compose.foundation.layout.Column(
                                    modifier = androidx.compose.ui.Modifier
                                        .fillMaxSize()
                                        .verticalScroll(androidx.compose.foundation.rememberScrollState())
                                ) { com.ivanna.omega.ui.MusicIntelligencePanel(); com.ivanna.omega.ui.FourDPredictionPanel(); com.ivanna.omega.ui.ConservationPanel() }
                }
            }
            composable("hires") { com.ivanna.omega.ui.HiResAudioScreen(onBack = { nav.popBackStack() }) }
            // Ψ (v2.5.389): medir la cabeza por foto + tarjeta (HeadMeasureScreen).
            composable(IvannaRoute.HEAD_MEASURE) { com.ivanna.omega.ui.HeadMeasureScreen(onBack = { nav.popBackStack() }) }
            // Colección de los 30 mejores conciertos de rock (v2.5.372).
            composable(IvannaRoute.ROCK_CONCERTS) {
                DevGate(nav) {
                com.ivanna.omega.ui.RockConcertsScreen(onBack = { nav.popBackStack() })
                }
            }
            composable("dashboard") {
                LaunchedEffect(Unit) {
                    com.ivanna.omega.assistant.IvannaLaunchGreeting.greetOnce(context)
                }
                // v2.5.473 (§4.1 paso 3, M2): aplicar el perfil de arranque COMPLETO.
                // El perfil guardado, o steve_miller en fresh install. Vía única:
                // ProfileManagerBridge.applyProfile → applyToDsp (DSP + fused +
                // neuro + ruta + antiDolby). Sin esto el arranque volvía al perfil
                // de fábrica con Φ en 0 y Volterra OFF y la elección del Intro
                // (5 campos, sin persistir) era la única que sonaba.
                LaunchedEffect(Unit) {
                    val store = com.ivanna.omega.core.ParameterStore(context)
                    // MIGRACIÓN v2.5.485 (una sola vez, marcada por
                    // rock70sDefault485): Rock 70s al abrir. Si el usuario
                    // nunca eligió perfil (null) o trae el default de fresh
                    // install (steve_miller), el arranque pasa a rock_70s.
                    // Cualquier otra banda elegida explícitamente se respeta
                    // (perfil y escena Atlas quedan como estaban).
                    if (!store.hasSeenRock70sDefault485()) {
                        store.markRock70sDefault485()
                        val savedId = store.getCurrentAudioProfileId()
                        if (savedId == null || savedId == "steve_miller") {
                            store.setCurrentAudioProfileId("rock_70s")
                            // Pin Atlas: progressive_rock_70s (índice 0),
                            // fijado. El worker ya arrancó en
                            // IVANNAApplication.onCreate, así que el pin se
                            // persiste (manual_style) y se aplica en nativo;
                            // un toque en el panel vuelve a AUTO (-1).
                            runCatching {
                                com.ivanna.omega.audio.MusicIntelligenceWorker
                                    .setManualStyleOverride(0)
                            }
                        }
                    }
                    val id = store.getCurrentAudioProfileId() ?: "steve_miller"
                    runCatching {
                        com.ivanna.omega.ProfileManagerBridge(context)
                            .applyProfile(id, dsp.value) { dsp.value = it }
                    }
                    // FIX 2.5.475 — Restaurar la física nueva al arrancar:
                    // antes solo se restauraba al entrar a la pantalla de NP,
                    // así que tras reiniciar la app el daemon quedaba con
                    // defaults y Ruta B procesaba con generación 0. Se empuja
                    // el estado guardado a Ruta A (nativo) y Ruta B (daemon).
                    runCatching {
                        val np = com.ivanna.omega.dsp.NewPhysicsStore.load(context)
                        com.ivanna.omega.dsp.NewPhysicsStore.pushToNative(np)
                        com.ivanna.omega.dsp.NewPhysicsStore.pushToDaemon(np)
                    }
                }
                val audioStateMs by com.ivanna.omega.audio.AudioStateManager.audioState.collectAsState()
                MainScaffold(
                    outerNav     = nav,
                    dsp          = dsp,
                    adaptiveBack = adaptiveBackend,
                    voiceMgr     = voiceProtectionManager,
                    player       = bridgePlayer,
                    captureActive = captureActive,
                    onToggleCapture = {
                        if (captureActive) {
                            context.stopService(android.content.Intent(context, com.ivanna.omega.audio.PlaybackCaptureService::class.java))
                        } else startCapture()
                    },
                    onPausePlayback = { bridgePlayer.pause() },
                    metrics      = com.ivanna.omega.audio.OmegaMetrics.shared.collectAsState().value,
                    adaptiveMode = com.ivanna.omega.audio.AdaptiveMode.valueOf(audioStateMs.adaptiveMode.name),
                    onAdaptiveModeChange = { uiMode ->
                        val bm = com.ivanna.omega.audio.AdaptiveMode.valueOf(uiMode.name)
                        com.ivanna.omega.audio.AudioStateManager.updateState { it.copy(adaptiveMode = bm) }
                        // P2 (2.5.461): re-evaluar "todo apagado" tras cambiar el modo.
                        EffectsOffTransition.evaluate(context)
                    },
                    adaptiveIntensity = audioStateMs.adaptiveIntensity * 100f,
                    onAdaptiveIntensityChange = { percent ->
                        val v = percent / 100f
                        com.ivanna.omega.audio.AudioStateManager.updateState { it.copy(adaptiveIntensity = v) }
                    }
                )
            }
            composable("magisk") {
                MagiskStatusPanel(
                    omegaBridge = OmegaEngineBridge,
                    onBack = { nav.popBackStack() }
                )
            }
            composable("profiles") {
                val context = LocalContext.current
                val profiles = remember { ProfilesLoader.load(context) }
                val metadata = remember { ProfilesLoader.loadMetadata(context) }
                val profileBridge = remember { ProfileManagerBridge(context) }
                val profileStore = remember { com.ivanna.omega.core.ParameterStore(context) }
                var activeProfileId by remember {
                    mutableStateOf(profileStore.getCurrentAudioProfileId())
                }
                LaunchedEffect(Unit) {
                    val saved = profileStore.getCurrentAudioProfileId() ?: return@LaunchedEffect
                    val profile = profiles.find { it.id == saved } ?: return@LaunchedEffect
                    profileBridge.applyProfile(profile.id, dsp.value) { updatedDsp ->
                        dsp.value = updatedDsp
                    }
                }
                ProfileSelectorScreen(
                    profiles = profiles,
                    metadata = metadata,
                    currentId = activeProfileId,
                    onApply = { profile ->
                        profileBridge.applyProfile(profile.id, dsp.value) { updatedDsp ->
                            dsp.value = updatedDsp
                        }
                        activeProfileId = profile.id
                        profileStore.setCurrentAudioProfileId(profile.id)
                    },
                    onClose = { nav.popBackStack() }
                )
            }
            composable("visualizer") {
                DevGate(nav) {
                                // v2.5.473 (§4.4): sin auto-arranque de captura tampoco aquí:
                                // el usuario la activa desde CONTROL C1 cuando quiere escuchar
                                // fuera de la app; el botón de abajo sigue ofreciéndola a mano.
                                val labSnapshot by IvannaLabMonitor.snapshot.collectAsState()
                                androidx.compose.foundation.layout.Column(
                                    modifier = Modifier
                                        .fillMaxSize()
                                        .background(Carbon)
                                        .windowInsetsPadding(WindowInsets.systemBars)
                                        .padding(horizontal = 16.dp, vertical = 12.dp),
                                    verticalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(12.dp)
                                ) {
                                    // Header
                                    androidx.compose.foundation.layout.Row(
                                        Modifier.fillMaxWidth(),
                                        horizontalArrangement = androidx.compose.foundation.layout.Arrangement.SpaceBetween,
                                        verticalAlignment = androidx.compose.ui.Alignment.CenterVertically
                                    ) {
                                        androidx.compose.foundation.layout.Column {
                                            androidx.compose.material3.Text(
                                                "ESPECTRO BARK · 64 BANDAS",
                                                color = CyanGlow,
                                                fontWeight = androidx.compose.ui.text.font.FontWeight.ExtraBold,
                                                fontSize = 13.sp,
                                                letterSpacing = 1.5.sp,
                                                fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace
                                            )
                                            androidx.compose.material3.Text(
                                                if (captureActive) "CAPTURA ACTIVA · FFT perceptual · Escala Bark"
                                                else "Aguardando permiso MediaProjection…",
                                                color = if (captureActive) CyanGlow.copy(alpha = 0.7f) else TextSec,
                                                fontSize = 10.sp,
                                                fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace
                                            )
                                        }
                                        if (!captureActive) {
                                            androidx.compose.material3.OutlinedButton(
                                                onClick = { startCapture() },
                                                border = androidx.compose.foundation.BorderStroke(1.dp, CyanGlow)
                                            ) { androidx.compose.material3.Text("ACTIVAR", color = CyanGlow, fontSize = 11.sp) }
                                        }
                                    }

                                    // ── Visualizador Bark64 real ──────────────────────────────────
                                    Bark64VisualizerPanel(
                                        modifier = Modifier
                                            .fillMaxWidth()
                                            .height(180.dp)
                                    )

                                    // ── Métricas Lab en tiempo real (THD · SNR · LUFS) ───────────
                                    androidx.compose.foundation.layout.Row(
                                        Modifier.fillMaxWidth(),
                                        horizontalArrangement = androidx.compose.foundation.layout.Arrangement.spacedBy(8.dp)
                                    ) {
                                        val s = labSnapshot
                                        // v2.5.448: ivannalab.h — todos los campos son -1.0f
                                        // cuando la medición no está implementada o los datos
                                        // son insuficientes ("El caller debe comprobar
                                        // != -1.0f"). Antes el THD se mostraba literal como
                                        // "-1.00%", un porcentaje imposible. Ahora el
                                        // centinela se muestra como "—" (no medido), igual
                                        // que la pantalla IVANNA LAB ("n/d (requiere tono
                                        // de prueba)"). THD% nunca es negativo, así que
                                        // cualquier valor < 0 es centinela.
                                        listOf(
                                            Triple("THD",  if (s == null || s.thd < 0f) "—" else "${"%.2f".format(s.thd)}%",
                                                if (s != null && s.thd > 1f) androidx.compose.ui.graphics.Color(0xFFFF3B30)
                                                else CyanGlow),
                                            Triple("SNR",  if (s == null || s.snr == -1f) "—" else "${"%.1f".format(s.snr)} dB",
                                                if (s != null && s.snr < 60f) androidx.compose.ui.graphics.Color(0xFFFFD600)
                                                else CyanGlow),
                                            Triple("LUFS", if (s == null || s.lufs == -1f) "—" else "${"%.1f".format(s.lufs)}",
                                                CyanGlow)
                                        ).forEach { (label, value, color) ->
                                            androidx.compose.foundation.layout.Column(
                                                modifier = Modifier
                                                    .weight(1f)
                                                    .background(
                                                        androidx.compose.ui.graphics.Color(0xFF111111),
                                                        androidx.compose.foundation.shape.RoundedCornerShape(8.dp)
                                                    )
                                                    .padding(10.dp),
                                                horizontalAlignment = androidx.compose.ui.Alignment.CenterHorizontally
                                            ) {
                                                androidx.compose.material3.Text(
                                                    label,
                                                    color = androidx.compose.ui.graphics.Color(0xFF888888),
                                                    fontSize = 9.sp,
                                                    letterSpacing = 1.sp,
                                                    fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace
                                                )
                                                androidx.compose.material3.Text(
                                                    value,
                                                    color = color,
                                                    fontSize = 14.sp,
                                                    fontWeight = androidx.compose.ui.text.font.FontWeight.Bold,
                                                    fontFamily = androidx.compose.ui.text.font.FontFamily.Monospace
                                                )
                                            }
                                        }
                                    }
                                }
                }
            }
            composable("adaptive") {
                DevGate(nav) {
                                com.ivanna.omega.ui.AdaptiveEngineScreen(
                                    voiceProtectionManager = voiceProtectionManager,
                                    backend = adaptiveBackend,
                                    modifier = Modifier
                                        .fillMaxSize()
                                        .background(Carbon)
                                        .windowInsetsPadding(WindowInsets.systemBars)
                                )
                }
            }
            composable("lab") {
                DevGate(nav) {
                                com.ivanna.omega.ui.IvannaLabScreen(
                                    modifier = Modifier.fillMaxSize().background(Carbon)
                                        .windowInsetsPadding(WindowInsets.systemBars)
                                )
                }
            }
            // FUSIÓN MAGISTRAL (7 operadores, 2026-11-18): FusedOperatorsScreen
            // existía pero sin destino en el NavHost — el usuario nunca la veía.
            // Ruta propia; el tab FUSIÓN navega aquí.
            // Alias legacy: antes redirigía a BRAIN (duplicado de onOpenAdaptive);
            // ahora resuelve al dashboard magistral, que es lo que su nombre dice.

            composable(IvannaRoute.ABX_TEST) {
                DevGate(nav) {
                com.ivanna.omega.ui.AbxTestScreen(onBack = { nav.popBackStack() })
                }
            }
            composable("benchmark") {
                DevGate(nav) {
                com.ivanna.omega.ui.BenchmarkScreen(onBack = { nav.popBackStack() })
                }
            }
            composable("phase7") {
                DevGate(nav) {
                com.ivanna.omega.ui.Phase7Screen(onBack = { nav.popBackStack() })
                }
            }
            // FIX (sub-entorno muerto): CognitiveDashboardActivity estaba
            // registrada en el Manifest pero ninguna ruta la lanzaba —
            // pantalla completa (dashboard cognitivo, perceptual brain) que
            // era inalcanzable desde la UI. Esta ruta la abre con Intent.
            composable("cognitive_dash") {
                DevGate(nav) {
                                val ctx = LocalContext.current
                                LaunchedEffect(Unit) {
                                    ctx.startActivity(
                                        Intent(ctx, com.ivanna.omega.ui.CognitiveDashboardActivity::class.java)
                                    )
                                    nav.popBackStack()
                                }
                }
            }
            // WfsCalibrationPanel existía completo (JNI→daemon→WfsRenderer) pero sin ruta ni acceso desde la UI.
            composable("wfs_calibration") {
                DevGate(nav) {
                com.ivanna.omega.ui.WfsCalibrationPanel()
                }
            }
            composable(IvannaRoute.AUDIO_CONTROL_HUB) {
                DevGate(nav) {
                                com.ivanna.omega.ui.SofaAfRirSafPanelScreen(
                                    onBack = { nav.popBackStack() },
                                    voiceMgr = voiceProtectionManager
                                )
                }
            }
            // ── ESTADO DE MOTORES ─────────────────────────────────────────
            // Motores que corrían en background sin ningún panel visible:
            // RouteDspCalibrator, AudioBackendSelector, UsbAudioProManager,
            // IvannaSpatialManager, AdaptiveEngineModulator, IvannaControlLoop.
            composable("engines_status") {
                DevGate(nav) {
                                com.ivanna.omega.ui.EnginesStatusScreen(onBack = { nav.popBackStack() })
                }
            }

            // ── CENTRO DE CONTROL OEM++ ──────────────────────────────────────
            // 6 pantallas nuevas cableadas a APIs nativas reales.
            // El ViewModel comparte estado entre todas vía viewModels().
            composable("oem_dashboard") {
                DevGate(nav) {
                                val vm: com.ivanna.omega.ui.oem.OemViewModel =
                                    androidx.lifecycle.viewmodel.compose.viewModel()
                                val state by vm.state.collectAsState()
                                val expert by vm.expertMode.collectAsState()
                                com.ivanna.omega.ui.oem.OemDashboardScreen(
                                    state           = state,
                                    expertMode      = expert,
                                    onToggleExpert  = { vm.setExpertMode(!expert) },
                                    onMeasureLatency = { vm.measureLatency() },
                                    onResetClips    = { vm.resetClips() },
                                    onOpenSpatial   = { nav.navigate("oem_spatial") },
                                    onOpenAcoustic  = { nav.navigate("oem_acoustic") },
                                    onOpenAi        = { nav.navigate("oem_ai") },
                                    onOpenThermal   = { nav.navigate("oem_thermal") },
                                    onOpenTelemetry = { nav.navigate("oem_telemetry") },
                                    onToggleAux     = { vm.toggleAuxField() },
                                )
                }
            }
            composable("oem_spatial") {
                DevGate(nav) {
                                val vm: com.ivanna.omega.ui.oem.OemViewModel =
                                    androidx.lifecycle.viewmodel.compose.viewModel()
                                val state by vm.state.collectAsState()
                                val expert by vm.expertMode.collectAsState()
                                com.ivanna.omega.ui.oem.OemSpatialScreen(
                                    state            = state,
                                    expertMode       = expert,
                                    onBack           = { nav.popBackStack() },
                                    onSetWidth       = { vm.setSpatialWidth(it) },
                                    onSetAngle       = { vm.setSpatialAngle(it) },
                                    onSetHrtfEnabled = { vm.setHrtfEnabled(it) },
                                    onSafFeedback    = { dir, pos -> vm.safFeedback(dir, pos) },
                                    onSafReset       = { vm.safReset() },
                                )
                }
            }
            composable("oem_acoustic") {
                DevGate(nav) {
                                val vm: com.ivanna.omega.ui.oem.OemViewModel =
                                    androidx.lifecycle.viewmodel.compose.viewModel()
                                val state by vm.state.collectAsState()
                                com.ivanna.omega.ui.oem.OemAcousticScreen(
                                    state  = state,
                                    onBack = { nav.popBackStack() },
                                )
                }
            }
            composable("oem_ai") {
                DevGate(nav) {
                                val vm: com.ivanna.omega.ui.oem.OemViewModel =
                                    androidx.lifecycle.viewmodel.compose.viewModel()
                                val state by vm.state.collectAsState()
                                val expert by vm.expertMode.collectAsState()
                                com.ivanna.omega.ui.oem.OemAiScreen(
                                    state             = state,
                                    expertMode        = expert,
                                    onBack            = { nav.popBackStack() },
                                    onSetAdaptEnabled = { vm.setAdaptEnabled(it) },
                                )
                }
            }
            composable("oem_thermal") {
                DevGate(nav) {
                                val vm: com.ivanna.omega.ui.oem.OemViewModel =
                                    androidx.lifecycle.viewmodel.compose.viewModel()
                                val state by vm.state.collectAsState()
                                com.ivanna.omega.ui.oem.OemThermalScreen(
                                    state  = state,
                                    onBack = { nav.popBackStack() },
                                )
                }
            }
            composable("oem_telemetry") {
                DevGate(nav) {
                                val vm: com.ivanna.omega.ui.oem.OemViewModel =
                                    androidx.lifecycle.viewmodel.compose.viewModel()
                                val state by vm.state.collectAsState()
                                com.ivanna.omega.ui.oem.OemTelemetryScreen(
                                    state            = state,
                                    onBack           = { nav.popBackStack() },
                                    onResetClips     = { vm.resetClips() },
                                    onMeasureLatency = { vm.measureLatency() },
                                )
                }
            }

            // ── IVANNA Conversational Acoustic Intelligence (FASE 12–19) ─────
            composable(IvannaRoute.IVANNA_ASSISTANT) {
                DevGate(nav) {
                                com.ivanna.omega.ui.IvannaAssistantScreen(
                                    onBack = { nav.popBackStack() }
                                )
                }
            }

            // ── VISTA DE DIOS · FLOTA (v2.5.471 Repair 4) ───────────────────
            // Ciclos reales del consejo leídos del puente. Sin ciclos muestra
            // "Sin ciclos todavía": no se inventa actividad.
            composable(IvannaRoute.FLEET_VIEW) {
                DevGate(nav) {
                                com.ivanna.omega.ui.FleetViewScreen(
                                    onBack = { nav.popBackStack() }
                                )
                }
            }

            // Panel de conectividad WiFi / datos / Gemini Agent
            composable(IvannaRoute.NETWORK) {
                DevGate(nav) {
                                NetworkStatusPanel(
                                    modifier = Modifier.fillMaxSize(),
                                    onBack   = { nav.popBackStack() }
                                )
                }
            }

            // ── 5 EJES DE SUPREMACÍA CUÁNTICO-NEUROMÓRFICA (C++23 Lock-Free) ──
            composable(IvannaRoute.SUPREME_AXES_HUB) {
                DevGate(nav) {
                                com.ivanna.omega.ui.SupremeFiveAxesHubScreen(
                                    onBack      = { nav.popBackStack() },
                                    onOpenAxis1 = { nav.navigate(IvannaRoute.SUPREME_AXIS_1_LATTICE) },
                                    onOpenAxis2 = { nav.navigate(IvannaRoute.SUPREME_AXIS_2_CVNN) },
                                    onOpenAxis3 = { nav.navigate(IvannaRoute.SUPREME_AXIS_3_SNN_HOA) },
                                    onOpenAxis4 = { nav.navigate(IvannaRoute.SUPREME_AXIS_4_PINNA_INR) },
                                    onOpenAxis5 = { nav.navigate(IvannaRoute.SUPREME_AXIS_5_SHM_FARROW) }
                                )
                }
            }
            composable(IvannaRoute.SUPREME_AXIS_1_LATTICE) {
                DevGate(nav) {
                                com.ivanna.omega.ui.WarpedLatticeAxisScreen(onBack = { nav.popBackStack() })
                }
            }
            composable(IvannaRoute.SUPREME_AXIS_2_CVNN) {
                DevGate(nav) {
                                com.ivanna.omega.ui.TransharmonicCvnnAxisScreen(onBack = { nav.popBackStack() })
                }
            }
            composable(IvannaRoute.SUPREME_AXIS_3_SNN_HOA) {
                DevGate(nav) {
                                com.ivanna.omega.ui.SnnNmfHoaAxisScreen(onBack = { nav.popBackStack() })
                }
            }
            composable(IvannaRoute.SUPREME_AXIS_4_PINNA_INR) {
                DevGate(nav) {
                                com.ivanna.omega.ui.PinnaManifoldAxisScreen(onBack = { nav.popBackStack() })
                }
            }
            composable(IvannaRoute.SUPREME_AXIS_5_SHM_FARROW) {
                DevGate(nav) {
                                com.ivanna.omega.ui.ShmFarrowArbitratorAxisScreen(onBack = { nav.popBackStack() })
                }
            }
        }
    }
}

// ── Fichas magistrales v2.5.442 (texto del prototipo aprobado) ─────────────
// Cada ficha tiene las cuatro partes: QUÉ ES / PARA QUÉ SIRVE / ÚSALO
// CUANDO / EVÍTALO CUANDO. Nada vago, nada inventado.
// Nota: en la ficha de Portada el prototipo escribía "versión vigente
// (v2.5.441)"; se omite el número entre paréntesis para no volver a
// hardcodear una versión: la vigente la muestra BuildConfig.VERSION_NAME.
/** Ficha de la Portada (Pantalla I del prototipo). */
private val portadaSpec = SpecInfo(
    title = "Portada",
    what = "La puerta de entrada del producto: identidad, versión vigente y el aviso legal que aceptas antes de que la cadena de audio se active.",
    purpose = "Para dejar constancia de la propiedad del software y de que IVANNA puede modificar el pipeline de audio del sistema con permisos específicos. Sin aceptación no hay acceso al motor.",
    whenUse = "En la primera apertura y tras cada actualización de versión o de términos: lee el aviso y pulsa Aceptar e iniciar.",
    whenAvoid = "No aceptes a ciegas tras reinstalar o restablecer el teléfono: confirma primero que la versión mostrada coincide con la release instalada."
)

/** Ficha de la Experiencia Auditiva (Pantalla II del prototipo). */
private val experienciaSpec = SpecInfo(
    title = "Experiencia Auditiva",
    what = "El selector de perfiles de escucha: siete firmas medidas de bandas de hard rock 70s. No es un ecualizador: cada perfil es una curva completa de destino para la cadena DSP + HRTF + Neuromorphic.",
    purpose = "Para decirle al motor cómo debe sonar el material antes de tocar un solo control: el perfil fija el carácter (pegada, aire, filo, espacio) y los entornos siguientes lo ejecutan.",
    whenUse = "Elige el perfil de la banda que vas a escuchar, o el más cercano en carácter si es otra música: Rush para precisión y transitorios, Steve Miller para el campo más abierto, Grand Funk para potencia con cuerpo.",
    whenAvoid = "No cambies de perfil a mitad de canción para \"arreglar\" una mezcla: el oído necesita unos segundos para asentarse. Y no uses un perfil de filo (Budgie) con grabaciones ya ásperas: suma aspereza."
)

/** Ficha del entorno Control (Pantalla III del prototipo). */
private val controlSpec = SpecInfo(
    title = "Control",
    what = "El entorno de control directo de la cadena: graves, corrección de sala, dinámica espacial y el campo ORBE·HOLOS. Es la vista operativa diaria del motor.",
    purpose = "Para ajustar lo que se oye aquí y ahora: la telemetría ASRC y el espectro confirman que la cadena está viva, y cada módulo actúa sobre una capa distinta sin pisar a las demás.",
    whenUse = "Como punto de partida de cada sesión y tras cambiar de salida (bocina, audífonos, DAC): revisa primero el estado del motor y después toca módulos, de arriba hacia abajo.",
    whenAvoid = "No ajustes todo a la vez: si mueves graves, sala y espacio en la misma escucha no sabrás qué causó qué. Un módulo por escucha, A/B contra su interruptor."
)

@Composable
fun StatusDot(active: Boolean, label: String) {
    Column(horizontalAlignment = Alignment.CenterHorizontally) {
        Box(Modifier.size(8.dp).clip(CircleShape)
            .background(if (active) CyanGlow else Color(0xFF333333)))
        Text(label, color = if (active) CyanGlow else Color(0xFF444444), fontSize = 7.sp)
    }
}

// Nota de auditoria: DspSection/FaderControl/EqFader vivian aqui (50 lineas)
// sin un solo call-site en todo el repo — sliders muertos que nadie renderizaba.
// Los faders reales de la UI viven en los paneles vivos (IvannaControlPanel,
// FusedOperatorsScreen, AdaptiveEngineScreen); los rangos canónicos están en
// CanonicalRanges. SoundScreen/BrainScreen se borraron en la Fase 2 (2026-11-22).
