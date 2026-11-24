package com.ivanna.omega.ui

import android.Manifest
import android.content.Context
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import com.ivanna.omega.audio.AudioRouteManager
import com.ivanna.omega.audio.BassHaptics
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.audio.RoomMeasurement
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.hardware.PermissionGate
import com.ivanna.omega.hardware.PermissionPolicy
import com.ivanna.omega.hardware.PermissionState
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Punto único de entrada para la medición acústica de sala (RoomProbe).
 *
 * Extraído VERBATIM de ControlTabScreen (misma medición, mismo permiso, misma
 * reversión, mismos textos): la pestaña CONTROL y la franja Wi-Fi comparten
 * este launcher para que la decisión del adaptador Wi-Fi tenga efecto real —
 * antes la política de adaptación no tenía llamador y el Wi-Fi no hacía nada.
 *
 * Guarda global: la franja y CONTROL instancian el launcher por separado,
 * pero una medición en curso bloquea a la otra instancia (dos barridos de
 * tonos a la vez serían un desastre). La segunda llamada solo deja un aviso.
 */
class RoomMeasureLauncher internal constructor(
    private val context: Context,
    private val scope: CoroutineScope,
    private val paramStore: ParameterStore
) {
    var gains by mutableStateOf(paramStore.getRoomCorrectionGains())
        private set
    var enabled by mutableStateOf(paramStore.isRoomCorrectionEnabled())
        private set
    var busy by mutableStateOf(false)
        private set
    var progress by mutableStateOf(0f)
        private set
    var status by mutableStateOf("")
        private set

    /** Lo pide el composable que crea el launcher (permiso de micrófono). */
    internal var requestMicPermission: (() -> Unit)? = null

    fun applyRoom(g: FloatArray?, en: Boolean) {
        if (IvannaNativeLib.isLoaded)
            runCatching { IvannaNativeLib.nativeSetRoomCorrection(if (en && g != null) g else FloatArray(10)) }
    }

    /**
     * El micrófono solo se pide aquí, cuando el usuario pulsa MEDIR. Si ya quedó denegado para
     * siempre (o se revocó en Ajustes) el sistema no vuelve a mostrar el diálogo: se explica y se
     * abre Ajustes en vez de repetir una petición muda.
     */
    fun launch() {
        if (busy) return
        val micState = PermissionGate.stateFor(context, Manifest.permission.RECORD_AUDIO)
        when (micState) {
            PermissionState.GRANTED -> runMeasurement()
            PermissionState.OPEN_SETTINGS -> {
                status = PermissionPolicy.micDeniedMessage(micState)
                PermissionGate.openAppSettings(context)
            }
            else -> {
                PermissionGate.markRequested(context, Manifest.permission.RECORD_AUDIO)
                requestMicPermission?.invoke()
            }
        }
    }

    /** Callback del permiso de micrófono: con permiso se mide; sin él, se explica. */
    fun onMicPermissionResult(granted: Boolean) {
        if (granted) runMeasurement()
        else status = PermissionPolicy.micDeniedMessage(
            PermissionGate.stateFor(context, Manifest.permission.RECORD_AUDIO)
        )
    }

    fun runMeasurement() {
        if (busy) return
        scope.launch {
            if (!measurementInFlight.compareAndSet(false, true)) {
                status = "Ya hay una medición en curso."
                return@launch
            }
            val prevGains = gains; val prevEnabled = enabled
            busy = true; progress = 0f
            status = "Prepara: volumen cómodo, teléfono donde te sientas, sala en silencio."
            try {
                when (val out = RoomMeasurement.runVerified(
                    context, prevGains, prevEnabled, ::applyRoom
                ) { progress = it }) {
                    is RoomMeasurement.VerifiedOutcome.Success -> {
                        paramStore.setRoomCorrection(out.gainsDb, true)
                        runCatching { BassHaptics.setMeasuredLatency(context, out.latencyMs) }
                        gains = out.gainsDb; enabled = true
                        applyRoom(out.gainsDb, true)
                        // v2.5.451: si el DAC está conectado, el perfil nuevo
                        // alimenta a Φ de inmediato (CALIBRADO), sin esperar
                        // a reconectar el DAC.
                        runCatching { AudioRouteManager.syncDacCalibratedProfile() }
                        status = "Verificado · error tonal ${"%.2f".format(Locale.ROOT, out.beforeErrorDb)} → ${"%.2f".format(Locale.ROOT, out.afterErrorDb)} dB · decisión #${out.decisionId}"
                    }
                    is RoomMeasurement.VerifiedOutcome.Failure -> {
                        applyRoom(prevGains, prevEnabled)
                        status = "Reversión automática · ${out.reason} · decisión #${out.decisionId}"
                    }
                }
            } catch (t: Throwable) {
                applyRoom(prevGains, prevEnabled)
                status = "Falló la calibración; se restauró el perfil previo: ${t.message ?: "error desconocido"}"
            } finally {
                busy = false
                measurementInFlight.set(false)
            }
        }
    }

    fun toggle(en: Boolean) {
        enabled = en && gains != null
        paramStore.setRoomCorrection(gains, enabled)
        applyRoom(gains, enabled)
    }

    fun clear() {
        gains = null; enabled = false; status = ""
        paramStore.setRoomCorrection(null, false)
        applyRoom(null, false)
    }

    companion object {
        /** Una sola medición de sala a la vez en todo el proceso. */
        private val measurementInFlight = AtomicBoolean(false)
    }
}

/** Crea el launcher con su permiso de micrófono cableado. */
@Composable
fun rememberRoomMeasureLauncher(): RoomMeasureLauncher {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val paramStore = remember(context) { ParameterStore(context) }
    val launcher = remember(context, scope) { RoomMeasureLauncher(context, scope, paramStore) }
    val micLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> launcher.onMicPermissionResult(granted) }
    SideEffect { launcher.requestMicPermission = { micLauncher.launch(Manifest.permission.RECORD_AUDIO) } }
    return launcher
}
