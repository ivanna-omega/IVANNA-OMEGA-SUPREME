package com.ivanna.omega.ui

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import androidx.compose.runtime.*
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.runtime.livedata.observeAsState
import androidx.navigation.NavHostController
import com.ivanna.omega.audio.AdaptiveBackend
import com.ivanna.omega.audio.AntiDolbyController
import com.ivanna.omega.audio.OmegaMetrics
import com.ivanna.omega.audio.IvannaUnifiedPipeline
import com.ivanna.omega.audio.ProfilesLoader
import com.ivanna.omega.audio.VoiceProtectionManager
import com.ivanna.omega.audio.VolterraSwitch
import com.ivanna.omega.audio.toSnapshot
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.dsp.DSPState
import com.ivanna.omega.neuromorphic.PiLstmBridge

/**
 * ControlTabScreen — FIX C (crítico):
 *
 * El tab CONTROL de [MainScaffold] llamaba a [IvannaControlPanel] con sólo 9
 * parámetros; los otros 19 callbacks (anti-Dolby, presets, auto/omega mode,
 * compresor, NHO, spatial, EVO, NPE, Phase Oracle) caían en su valor por
 * defecto `{}`, así que mover esos knobs no producía NINGÚN cambio de audio.
 *
 * Este composable concentra el cableado real —el mismo que ya existía en el
 * DashboardScreen legacy de MainActivity— para que el tab CONTROL controle de
 * verdad el motor. No se borra nada: MainScaffold simplemente delega aquí.
 */
@Composable
fun ControlTabScreen(
    outerNav : NavHostController,
    dsp      : MutableState<DSPState>,
    adaptiveBack : AdaptiveBackend,
    voiceMgr : VoiceProtectionManager,
    metrics  : OmegaMetrics = OmegaMetrics(),
    onOpenAdaptiveTab : () -> Unit = {},
    captureActive   : Boolean = false,
    onToggleCapture : () -> Unit = {},
    onPausePlayback : () -> Unit = {},
    modifier : Modifier = Modifier
) {
    val context = LocalContext.current
    val paramStore = remember { ParameterStore(context) }
    // ── v2.5.473 §2.5: FUSIÓN manda — una sola instancia de FusedControls ──
    // CONTROL renderiza el mismo estado que FUSIÓN (mismo store, mismo
    // update con debounce 120 ms → safeSetFusedControls → saveFusedControls).
    // Antes cada pantalla cargaba su propio loadFusedControls() y dos
    // ediciones simultáneas podían pisarse.
    val fusedScope = rememberCoroutineScope()
    var fusedControls by remember { mutableStateOf(paramStore.loadFusedControls()) }
    var fusedPushJob by remember { mutableStateOf<kotlinx.coroutines.Job?>(null) }
    fun pushFused(next: com.ivanna.omega.audio.FusedControls) {
        fusedControls = next.clamp()
        fusedPushJob?.cancel()
        fusedPushJob = fusedScope.launch {
            kotlinx.coroutines.delay(120)
            com.ivanna.omega.audio.FusedControlsBridge.safeSetFusedControls(fusedControls)
            paramStore.saveFusedControls(fusedControls)
        }
    }
    // C2: el perfil activo se muestra; el picker único es "profiles".
    val currentProfileName = remember {
        val id = paramStore.getCurrentAudioProfileId() ?: "steve_miller"
        runCatching { ProfilesLoader.load(context).firstOrNull { it.id == id }?.name }.getOrNull() ?: id
    }


    // La pestaña CONTROL es la pantalla activa que dibuja AdaptiveEngineStatusCard.
    // No depender de que otra ruta (dashboard legado o AdaptiveEngineScreen) haya
    // iniciado el sondeo: sin este ciclo el panel conserva el snapshot inicial 0/OFFLINE.
    DisposableEffect(adaptiveBack, context) {
        // El panel depende de routeState para seleccionar la telemetría de Ruta A/B.
        // Arrancar el monitor aquí hace que CONTROL sea autosuficiente incluso si
        // IVANNAApplication no llegó a iniciar el singleton por una ruta de arranque
        // alternativa; start() es idempotente y no crea un segundo poller.
        IvannaUnifiedPipeline.start(context.applicationContext)
        adaptiveBack.startTelemetry()
        onDispose { adaptiveBack.stopTelemetry() }
    }
    val routeState by com.ivanna.omega.audio.IvannaUnifiedPipeline.state.collectAsState()
    val voiceActive by voiceMgr.voiceProtectionActive.observeAsState(false)
    // FIX (persistencia): el bypass NPE arrancaba siempre en false aunque el
    // usuario lo hubiera activado y ParameterStore ya lo guardara
    // (KEY_NPE_BYPASS). Ahora se restaura desde prefs y se reaplica al motor
    // nativo — el estado del toggle sobrevive cierre de app y reboot.
    var npeBypassState by remember { mutableStateOf(paramStore.isNpeBypass()) }

    // ── Corrección acústica de sala (medición con el micrófono) ─────────────
    var roomGains by remember { mutableStateOf(paramStore.getRoomCorrectionGains()) }
    var roomEnabled by remember { mutableStateOf(paramStore.isRoomCorrectionEnabled()) }
    var roomBusy by remember { mutableStateOf(false) }
    var roomProgress by remember { mutableStateOf(0f) }
    var roomStatus by remember { mutableStateOf("") }
    val roomScope = rememberCoroutineScope()
    val applyRoom: (FloatArray?, Boolean) -> Unit = { g, en ->
        if (IvannaNativeLib.isLoaded)
            runCatching { IvannaNativeLib.nativeSetRoomCorrection(if (en && g != null) g else FloatArray(10)) }
    }
    val startRoomMeasure: () -> Unit = {
        if (!roomBusy) roomScope.launch {
            val prevGains = roomGains; val prevEnabled = roomEnabled
            roomBusy = true; roomProgress = 0f
            roomStatus = "Prepara: volumen cómodo, teléfono donde te sientas, sala en silencio."
            try {
                when (val out = com.ivanna.omega.audio.RoomMeasurement.runVerified(
                    context, prevGains, prevEnabled, applyRoom
                ) { roomProgress = it }) {
                    is com.ivanna.omega.audio.RoomMeasurement.VerifiedOutcome.Success -> {
                        paramStore.setRoomCorrection(out.gainsDb, true)
                        runCatching { com.ivanna.omega.audio.BassHaptics.setMeasuredLatency(context, out.latencyMs) }
                        roomGains = out.gainsDb; roomEnabled = true
                        applyRoom(out.gainsDb, true)
                        // v2.5.451: si el DAC está conectado, el perfil nuevo
                        // alimenta a Φ de inmediato (CALIBRADO), sin esperar
                        // a reconectar el DAC.
                        runCatching { com.ivanna.omega.audio.AudioRouteManager.syncDacCalibratedProfile() }
                        roomStatus = "Verificado · error tonal ${"%.2f".format(java.util.Locale.ROOT, out.beforeErrorDb)} → ${"%.2f".format(java.util.Locale.ROOT, out.afterErrorDb)} dB · decisión #${out.decisionId}"
                    }
                    is com.ivanna.omega.audio.RoomMeasurement.VerifiedOutcome.Failure -> {
                        applyRoom(prevGains, prevEnabled)
                        roomStatus = "Reversión automática · ${out.reason} · decisión #${out.decisionId}"
                    }
                }
            } catch (t: Throwable) {
                applyRoom(prevGains, prevEnabled)
                roomStatus = "Falló la calibración; se restauró el perfil previo: ${t.message ?: "error desconocido"}"
            } finally {
                roomBusy = false
            }
        }
    }
    // ── Formato IVANNA OMEGA (v2.5.473): cargador de .ivanna.json ─────────
    // El usuario elige el archivo con el selector del sistema (SAF, JSON);
    // se parsea fail-closed y se aplica por la misma vía que un perfil
    // (FusedControls + DSPState). Sin archivo, nada cambia.
    var omegaLabel by remember { mutableStateOf(paramStore.getOmegaFileLabel()) }
    var omegaStatus by remember { mutableStateOf("") }
    val omegaScope = rememberCoroutineScope()
    val omegaLauncher = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.GetContent()
    ) { uri ->
        if (uri == null) return@rememberLauncherForActivityResult
        omegaScope.launch(kotlinx.coroutines.Dispatchers.IO) {
            val result = runCatching {
                val text = context.contentResolver.openInputStream(uri)
                    ?.bufferedReader()?.readText()
                    ?: throw IllegalStateException("No se pudo leer el archivo")
                com.ivanna.omega.audio.IvannaOmegaFileParser.parse(text).getOrThrow()
            }
            withContext(kotlinx.coroutines.Dispatchers.Main) {
                result.onSuccess { file ->
                    val ok = com.ivanna.omega.audio.IvannaOmegaApplier.apply(
                        context, file, dsp.value
                    ) { dsp.value = it }
                    if (ok) {
                        fusedControls = paramStore.loadFusedControls()
                        omegaLabel = file.displayLabel
                        omegaStatus = "Aplicado: ${file.displayLabel}"
                    } else {
                        omegaStatus = "No se pudo aplicar el archivo."
                    }
                }.onFailure { t ->
                    omegaStatus = "Archivo no válido: ${t.message}"
                }
            }
        }
    }
    // ── v2.5.473 §5: barrido Genezi ──────────────────────────────────────
    // Paréntesis neutro: pausa el reproductor, para el sensor, FUSIÓN
    // neutra, bypass DSPState y sala OFF; en `finally` se restaura TODO
    // (fused snapshot + push inmediato, DSPState, sala, sensor). La
    // captura del sistema se deja como está: reiniciarla exige el diálogo
    // de MediaProjection (gesto del usuario) y no toca el micrófono.
    var sweepBusy by remember { mutableStateOf(false) }
    var sweepProgress by remember { mutableStateOf(0f) }
    var sweepStatus by remember { mutableStateOf("") }
    var sweepPending by remember { mutableStateOf(false) }
    // Referencia diferida: micLauncher usa startSweep (declarado después)
    // vía este ref; micLauncher se declara ANTES de startSweep porque
    // startSweep lo usa directamente (Kotlin exige declaración léxica previa).
    var startSweepRef: (() -> Unit)? by remember { mutableStateOf(null) }
    val micLauncher = androidx.activity.compose.rememberLauncherForActivityResult(
        androidx.activity.result.contract.ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted) {
            if (sweepPending) { sweepPending = false; startSweepRef?.invoke() }
            else startRoomMeasure()
        }
        else {
            val msg = com.ivanna.omega.hardware.PermissionPolicy.micDeniedMessage(
                com.ivanna.omega.hardware.PermissionGate.stateFor(context, android.Manifest.permission.RECORD_AUDIO)
            )
            if (sweepPending) { sweepPending = false; sweepStatus = msg } else roomStatus = msg
        }
    }
    val sweepScope = rememberCoroutineScope()
    val startSweep: () -> Unit = {
        if (!sweepBusy) sweepScope.launch {
            val micState = com.ivanna.omega.hardware.PermissionGate.stateFor(context, android.Manifest.permission.RECORD_AUDIO)
            if (micState != com.ivanna.omega.hardware.PermissionState.GRANTED) {
                sweepPending = true
                com.ivanna.omega.hardware.PermissionGate.markRequested(context, android.Manifest.permission.RECORD_AUDIO)
                micLauncher.launch(android.Manifest.permission.RECORD_AUDIO)
                return@launch
            }
            sweepBusy = true; sweepProgress = 0f
            sweepStatus = "Prepara: volumen del Genezi fijo, teléfono frente a las bocinas, sala en silencio."
            val prevFused = fusedControls
            val prevDsp = dsp.value
            val prevRoomGains = roomGains; val prevRoomEnabled = roomEnabled
            val sensorWasRunning = com.ivanna.omega.audio.RealAcousticSensor.isRunning
            try {
                onPausePlayback()
                com.ivanna.omega.audio.RealAcousticSensor.stop()
                com.ivanna.omega.audio.FusedControlsBridge.safeSetFusedNeutral()
                dsp.value = dsp.value.copy(bypass = true); dsp.value.pushToNative()
                applyRoom(null, false)
                when (val out = com.ivanna.omega.audio.GeneziSweepRecorder.run(context) { sweepProgress = it }) {
                    is com.ivanna.omega.audio.GeneziSweepRecorder.Outcome.Success -> {
                        sweepStatus = "Barrido listo: ${out.files.joinToString(", ")} · latencia lo ${out.latencyLoMs} ms / hi ${out.latencyHiMs} ms · pico ${"%.1f".format(java.util.Locale.ROOT, out.peakDbfs)} dBFS"
                    }
                    is com.ivanna.omega.audio.GeneziSweepRecorder.Outcome.Failure -> {
                        sweepStatus = "Barrido fallido: ${out.reason}"
                    }
                }
            } catch (t: Throwable) {
                sweepStatus = "Barrido fallido: ${t.message ?: "error desconocido"}"
            } finally {
                dsp.value = prevDsp; dsp.value.pushToNative()
                applyRoom(prevRoomGains, prevRoomEnabled)
                pushFused(prevFused)
                com.ivanna.omega.audio.FusedControlsBridge.safeSetFusedControls(prevFused)
                if (sensorWasRunning) com.ivanna.omega.audio.RealAcousticSensor.start(context)
                sweepBusy = false
            }
        }
    }
    startSweepRef = startSweep

    LaunchedEffect(Unit) {
        if (npeBypassState && PiLstmBridge.isReady) {
            PiLstmBridge.setBypass(true)
        }
        VolterraSwitch.enabled = paramStore.isNpeManifoldEnabled()
    }

    // Refresco vivo del panel ENGINE (2 Hz) mientras CONTROL está en pantalla.
    LaunchedEffect(Unit) {
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Default) {
            while (true) {
                runCatching { OmegaMetrics.refreshFromNative() }
                kotlinx.coroutines.delay(500)
            }
        }
    }

    val adaptiveTelemetryRaw by adaptiveBack.telemetry.collectAsState()
    val adaptiveTelemetry = adaptiveTelemetryRaw.toSnapshot()

    val antiDolbyController = remember(context) {
        AntiDolbyController(context).also { ctrl ->
            ctrl.onDspUpdate = { exciter, width, eqGainDb ->
                dsp.value = dsp.value.copy(wet = exciter, stereoWidth = width)
                dsp.value.pushToNative()
                if (IvannaNativeLib.isLoaded)
                    IvannaNativeLib.guardedNative(Unit) { IvannaNativeLib.nativeSetEQParams(eqGainDb, eqGainDb, eqGainDb, dsp.value.master) }
            }
        }
    }

    LaunchedEffect(antiDolbyController) {
        kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.Default) {
            antiDolbyController.initialize()
            if (paramStore.isAntiDolbyEnabled()) {
                antiDolbyController.enableAntiDolby()
            }
        }
    }

    IvannaControlPanel(
        initialExciter       = dsp.value.wet,
        initialEq            = dsp.value.mid,
        initialWidth         = dsp.value.stereoWidth,
        initialCompThreshold = dsp.value.alpha,
        initialCompRatio     = dsp.value.beta,
        // El panel muestra el compresor en dB canónicos: el valor que
        // manda es el dB fusionado guardado (no el alpha legacy).
        initialCompThresholdDb = fusedControls.dynamics.compThresholdDb,
        initialCompRatioValue = fusedControls.dynamics.compRatio,
        initialAuxFieldEnabled = paramStore.isAuxFieldEnabled(),
        roomGains = roomGains,
        roomEnabled = roomEnabled,
        roomBusy = roomBusy,
        roomProgress = roomProgress,
        roomStatus = roomStatus,
        onRoomMeasure = {
            // El micrófono solo se pide aquí, cuando el usuario pulsa MEDIR. Si ya quedó denegado para
            // siempre (o se revocó en Ajustes) el sistema no vuelve a mostrar el diálogo: se explica y se
            // abre Ajustes en vez de repetir una petición muda.
            val micState = com.ivanna.omega.hardware.PermissionGate.stateFor(context, android.Manifest.permission.RECORD_AUDIO)
            when (micState) {
                com.ivanna.omega.hardware.PermissionState.GRANTED -> startRoomMeasure()
                com.ivanna.omega.hardware.PermissionState.OPEN_SETTINGS -> {
                    roomStatus = com.ivanna.omega.hardware.PermissionPolicy.micDeniedMessage(micState)
                    com.ivanna.omega.hardware.PermissionGate.openAppSettings(context)
                }
                else -> {
                    com.ivanna.omega.hardware.PermissionGate.markRequested(context, android.Manifest.permission.RECORD_AUDIO)
                    micLauncher.launch(android.Manifest.permission.RECORD_AUDIO)
                }
            }
        },
        onRoomToggle = { en ->
            roomEnabled = en && roomGains != null
            paramStore.setRoomCorrection(roomGains, roomEnabled)
            applyRoom(roomGains, roomEnabled)
        },
        onRoomClear = {
            roomGains = null; roomEnabled = false; roomStatus = ""
            paramStore.setRoomCorrection(null, false)
            applyRoom(null, false)
        },
        sweepBusy = sweepBusy,
        sweepProgress = sweepProgress,
        sweepStatus = sweepStatus,
        onGeneziSweep = startSweep,
        // ── Formato IVANNA OMEGA: archivo por canción ──────────────────
        omegaFileLabel = omegaLabel ?: "",
        omegaStatus = omegaStatus,
        onLoadOmegaFile = { omegaLauncher.launch("application/json") },
        initialAuxXoverHz = paramStore.getAuxXoverHz(),
        initialAuxCoherence = paramStore.getAuxCoherence(),
        // ── v2.5.473 §1.3 C1–C8: estado fusionado hoisteado ────────────────
        initialMasterDb = dsp.value.master,
        initialTonalLowDb = fusedControls.tonal.lowDb,
        initialTonalMidDb = fusedControls.tonal.midDb,
        initialTonalHighDb = fusedControls.tonal.highDb,
        initialTonalPresenceDb = fusedControls.tonal.presenceDb,
        initialTonalTrimDb = fusedControls.tonal.trimDb,
        initialFusedOrbe = fusedControls.spatial.amount,
        initialFusedHolos = fusedControls.field.amount,
        initialFusedPhi = fusedControls.field.phiAmount,
        initialFusedVolterra = fusedControls.transducer.correction > 0f,
        initialSpatialModeSpeakers = fusedControls.spatial.mode == com.ivanna.omega.audio.FusedSpatialMode.SPEAKERS,
        initialCaptureActive = captureActive,
        fusedProfileName = currentProfileName,
        metrics              = metrics,
        adaptiveTelemetry    = adaptiveTelemetry,
        routeState           = routeState,
        // C4/C5/C6: cada slider escribe DSPState (audio vivo ya) Y espeja al
        // estado fusionado (mismo valor en FUSIÓN; un solo estado, §2.5).
        onExciterChange = { v ->
            dsp.value = dsp.value.copy(wet = v); dsp.value.pushToNative()
            pushFused(fusedControls.copy(dynamics = fusedControls.dynamics.copy(exciter = v)))
        },
        onWidthChange = { v ->
            dsp.value = dsp.value.copy(stereoWidth = v); dsp.value.pushToNative()
            pushFused(fusedControls.copy(dynamics = fusedControls.dynamics.copy(width = v)))
        },
        // v: umbral en dB canónicos (−60…0). El alpha legacy del DSP
        // nativo solo cubre −24…0 dB (Compressor.cpp: umbral = −24 + 24·alpha):
        // por debajo de −24 dB el alpha se queda en su piso (0) y el dB
        // completo lo lleva la cadena fusionada.
        onCompThresholdChange = { v ->
            dsp.value = dsp.value.copy(alpha = ((v + 24f) / 24f).coerceIn(0f, 1f)); dsp.value.pushToNative()
            pushFused(fusedControls.copy(dynamics = fusedControls.dynamics.copy(compThresholdDb = v)))
        },
        onCompRatioChange = { v ->
            dsp.value = dsp.value.copy(beta = ((v - 1f) / 19f).coerceIn(0f, 1f)); dsp.value.pushToNative()
            pushFused(fusedControls.copy(dynamics = fusedControls.dynamics.copy(compRatio = v)))
        },
        onTonalEqChange = { low, mid, high, presence ->
            dsp.value = dsp.value.copy(low = low, mid = mid, high = high, presence = presence)
            dsp.value.pushToNative()
            pushFused(fusedControls.copy(tonal = fusedControls.tonal.copy(lowDb = low, midDb = mid, highDb = high, presenceDb = presence)))
        },
        onTonalTrimChange = { trim ->
            pushFused(fusedControls.copy(tonal = fusedControls.tonal.copy(trimDb = trim)))
        },
        onMasterChange = { v ->
            dsp.value = dsp.value.copy(master = v); dsp.value.pushToNative()
            if (PiLstmBridge.isReady) PiLstmBridge.setMasterGain(v)
        },
        onSpatialModeChange = { speakers ->
            pushFused(fusedControls.copy(spatial = fusedControls.spatial.copy(
                mode = if (speakers) com.ivanna.omega.audio.FusedSpatialMode.SPEAKERS
                       else com.ivanna.omega.audio.FusedSpatialMode.HEADPHONES
            )))
        },
        onToggleCapture = onToggleCapture,
        onOpenFusion = onOpenAdaptiveTab,
        onAuxFieldChange = { en, xo, co ->
            paramStore.setAuxField(en, xo, co)
            if (IvannaNativeLib.isLoaded)
                runCatching { IvannaNativeLib.nativeSetAuxField(en, xo.coerceIn(60f, 200f), co.coerceIn(0f, 1f)) }
        },
        // ── Navegación ──────────────────────────────────────────────────
        onOpenHiRes    = { outerNav.navigate("hires") },
        onOpenProfiles = { outerNav.navigate("profiles") },
        onOpenMagisk   = { outerNav.navigate("magisk") },
        modifier       = modifier
    )
}
