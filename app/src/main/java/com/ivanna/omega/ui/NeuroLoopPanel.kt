/*
 * © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
 * Tarjeta de control del bucle cerrado neuro-acústico (v2.5.376, modo
 * micrófono v2.5.449).
 */
package com.ivanna.omega.ui

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Row
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.core.content.ContextCompat
import com.ivanna.omega.neuromorphic.NeuroMicDriver
import com.ivanna.omega.neuromorphic.NeuroSensorGateway
import com.ivanna.omega.neuromorphic.NeuroUdpDriver
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.TextMuted
import kotlinx.coroutines.delay
import kotlin.math.roundToInt

/** Modo de sensor del bucle cerrado. MICRÓFONO es el default (directiva
 *  de Esse, v2.5.449): funciona sin root y sin hardware extra. */
internal enum class NeuroSensorMode { MIC, UDP }

private const val NEURO_PREFS = "ivanna_neuro_prefs"
private const val NEURO_MODE_KEY = "neuro_sensor_mode"

private fun loadNeuroMode(context: Context): NeuroSensorMode {
    val saved = context.getSharedPreferences(NEURO_PREFS, Context.MODE_PRIVATE)
        .getString(NEURO_MODE_KEY, "mic")
    return if (saved == "udp") NeuroSensorMode.UDP else NeuroSensorMode.MIC
}

private fun saveNeuroMode(context: Context, mode: NeuroSensorMode) {
    context.getSharedPreferences(NEURO_PREFS, Context.MODE_PRIVATE).edit()
        .putString(NEURO_MODE_KEY, if (mode == NeuroSensorMode.UDP) "udp" else "mic")
        .apply()
}

private fun hasMicPermission(context: Context): Boolean =
    ContextCompat.checkSelfPermission(context, Manifest.permission.RECORD_AUDIO) ==
        PackageManager.PERMISSION_GRANTED

/** Texto de estado honesto a partir de lo que el bucle nativo reporta. */
internal fun neuroLoopStatusLine(
    enabled: Boolean, active: Boolean, frames: Int,
    mode: NeuroSensorMode, micPermission: Boolean
): String = when {
    !enabled -> "APAGADO · el audio no cambia"
    mode == NeuroSensorMode.MIC && !micPermission ->
        "SIN PERMISO DE MICRÓFONO · el bucle no aprende"
    active && mode == NeuroSensorMode.MIC -> "MICRÓFONO ACTIVO · $frames frames aprendidos"
    active -> "SENSOR ACTIVO · $frames frames aprendidos"
    frames > 0 -> "FRAMES VIEJOS O DE BAJA CALIDAD · fail-closed"
    mode == NeuroSensorMode.MIC -> "MICRÓFONO DEL TELÉFONO · esperando música audible"
    else -> "ESPERANDO SENSOR REAL en 127.0.0.1:${NeuroUdpDriver.DEFAULT_PORT}"
}

@Composable
fun NeuroLoopPanel() {
    val context = LocalContext.current
    val micDriver = remember { NeuroMicDriver(context) }
    val udpDriver = remember { NeuroUdpDriver() }
    var mode by remember { mutableStateOf(loadNeuroMode(context)) }
    var enabled by remember { mutableStateOf(NeuroSensorGateway.isEnabled) }
    var micPermission by remember { mutableStateOf(hasMicPermission(context)) }
    var status by remember { mutableStateOf(floatArrayOf(0f, 0f, 0f, 0f)) }

    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        micPermission = granted
        if (granted && mode == NeuroSensorMode.MIC && !enabled) {
            NeuroSensorGateway.registerDriver(micDriver)
            enabled = NeuroSensorGateway.enable()
        }
    }

    LaunchedEffect(enabled) {
        while (enabled) {
            status = NeuroSensorGateway.status()
            delay(500)
        }
    }

    fun tryEnable() {
        if (mode == NeuroSensorMode.MIC) {
            if (!hasMicPermission(context)) {
                micPermission = false
                permissionLauncher.launch(Manifest.permission.RECORD_AUDIO)
                return
            }
            micPermission = true
            NeuroSensorGateway.registerDriver(micDriver)
            enabled = NeuroSensorGateway.enable()
        } else {
            NeuroSensorGateway.registerDriver(udpDriver)
            enabled = NeuroSensorGateway.enable()
        }
    }

    GlassCard(
        title = "NEURO-ACÚSTICA · BUCLE CERRADO",
        accent = AuroraCyan,
        subtitle = neuroLoopStatusLine(
            enabled, status[0] > 0.5f, status[2].roundToInt(), mode, micPermission
        ),
        rightSlot = {
            Switch(enabled, onCheckedChange = { want ->
                if (want) {
                    tryEnable()
                } else {
                    NeuroSensorGateway.disable()
                    enabled = false
                }
            })
        }
    ) {
        Row {
            TextButton(onClick = {
                if (mode != NeuroSensorMode.MIC) {
                    if (enabled) {
                        NeuroSensorGateway.disable()
                        enabled = false
                    }
                    mode = NeuroSensorMode.MIC
                    saveNeuroMode(context, mode)
                }
            }) {
                Text(
                    "MICRÓFONO",
                    color = if (mode == NeuroSensorMode.MIC) AuroraCyan else TextMuted
                )
            }
            TextButton(onClick = {
                if (mode != NeuroSensorMode.UDP) {
                    if (enabled) {
                        NeuroSensorGateway.disable()
                        enabled = false
                    }
                    mode = NeuroSensorMode.UDP
                    saveNeuroMode(context, mode)
                }
            }) {
                Text(
                    "SENSOR EXTERNO",
                    color = if (mode == NeuroSensorMode.UDP) AuroraCyan else TextMuted
                )
            }
        }
        if (enabled) {
            val counters = if (mode == NeuroSensorMode.MIC) {
                "MIC recibidos ${micDriver.received.get()} · aceptados ${micDriver.accepted.get()} · rechazados ${micDriver.rejected.get()}"
            } else {
                "UDP recibidos ${udpDriver.received.get()} · aceptados ${udpDriver.accepted.get()} · rechazados ${udpDriver.rejected.get()}"
            }
            Text(
                "Confianza ${(status[1] * 100).roundToInt()} % · wet ${"%.2f".format(status[3])} · $counters",
                style = MaterialTheme.typography.labelSmall, color = TextMuted
            )
        }
        Text(
            if (mode == NeuroSensorMode.MIC) {
                "El teléfono escucha su propia salida con el micrófono (sin root, sin hardware extra) y corrige el balance tonal hacia el target. Es un bucle acústico, no lee el cerebro. Ajustes acotados (±6 dB, con slew); la firma vive solo en RAM. No es un dispositivo médico."
            } else {
                "Solo datos reales: un procesador de sensor externo (EEG/ABR/fNIRS) envía frames por UDP local. El Moto G85 no trae ninguno; sin él no se inventa nada y el audio queda intacto. Ajustes acotados (±6 dB, con slew); la firma vive solo en RAM. No es un dispositivo médico."
            },
            style = MaterialTheme.typography.labelSmall, color = TextMuted
        )
    }
}
