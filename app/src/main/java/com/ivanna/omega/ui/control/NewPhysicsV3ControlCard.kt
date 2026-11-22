package com.ivanna.omega.ui.control

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import com.ivanna.omega.ui.ControlHelpText
import com.ivanna.omega.ui.Slider
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.ivanna.omega.dsp.NewPhysicsState
import com.ivanna.omega.dsp.NewPhysicsStore
import com.ivanna.omega.dsp.NewPhysicsV3TelemetryUi
import com.ivanna.omega.dsp.RirEntry
import com.ivanna.omega.dsp.SubBassStageState
import kotlinx.coroutines.delay

/**
 * FÍSICA NUEVA v3 — Padre común (v2.5.483).
 *
 * Eleva el estado: las dos tarjetas (la de controles Ω y esta v3)
 * comparten el MISMO NewPhysicsState, así nunca divergen. El padre
 * guarda en prefs y empuja (Ruta A por JNI + Ruta B por daemon) en
 * cada cambio; al entrar, re-empuja el estado guardado y, si la RIR
 * elegida no es el default, la carga (el wake del motor carga rir_0101).
 *
 * Esta es la tarjeta que IvannaControlPanel coloca en el panel CONTROL.
 * La tarjeta v3 mueve SOLO Ruta A en vivo: la Ruta B suena con los
 * mismos defaults compilados (np483_defaults.hpp) hasta la extensión
 * ABI del snapshot. La tarjeta lo dice en pantalla; ningún control
 * finge más de lo que hace.
 */
@Composable
fun NewPhysicsCardsColumn(modifier: Modifier = Modifier) {
    val context = LocalContext.current
    var state by remember { mutableStateOf(NewPhysicsStore.load(context)) }

    LaunchedEffect(Unit) {
        NewPhysicsStore.pushToNative(state)
        if (state.roomRirFile != "rir_0101.wav") {
            runCatching {
                NewPhysicsStore.loadRoomRir(context, state.roomRirFile, state.roomTailMix)
            }
        }
    }

    fun apply(next: NewPhysicsState) {
        state = next
        NewPhysicsStore.save(context, next)
        NewPhysicsStore.pushToNative(next)
    }

    Column(
        modifier = modifier.fillMaxWidth(),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        NewPhysicsControlCard(state = state, onApply = { next -> apply(next) })
        NewPhysicsV3ControlCard(state = state, onApply = { next -> apply(next) })
    }
}

/**
 * FÍSICA NUEVA v3 · ÓRGANOS DESPIERTOS — Tarjeta CONTROL (v2.5.483).
 *
 * Tres órganos que en la 482 estaban dormidos y sin cablear:
 *  (a) Sub-graves modal: 6 etapas (la 0 es el diferencial Ω automático y
 *      no se edita); aquí las 5 editables: polo Genezi 68.5 Hz + 4 modos
 *      axiales de la sala rir_0101 (3.5 x 2.9 x 4.6 m). Cut-only (Fase 4).
 *  (b) Cola de sala UPOLA: mezcla + selector de RIR (dataset de 200
 *      habitaciones reales, ordenado por distancia a los 3.5 m de Esse).
 *  (c) Klippel H3: mezcla + temperatura de bobina en vivo + estado térmico.
 *
 * VALORES APROXIMADOS (no medidos, sin equipo): cada default tiene su
 * fuente en ~/workspace/np483/APPROXIMATIONS.md — H2 0.50% / H3 0.20%
 * @85 dB SPL (típico bocina pequeña), modos f = c/2L, térmica dual-RC
 * Klippel (Tv 6.3 s, Tm 264 s). Nada inventado sin fuente.
 */
@Composable
fun NewPhysicsV3ControlCard(
    state: NewPhysicsState,
    onApply: (NewPhysicsState) -> Unit,
    modifier: Modifier = Modifier
) {
    val context = LocalContext.current
    var telemetryV3 by remember { mutableStateOf(NewPhysicsV3TelemetryUi()) }
    var rirs by remember { mutableStateOf<List<RirEntry>>(emptyList()) }
    var rirStatus by remember { mutableStateOf("") }

    LaunchedEffect(Unit) {
        rirs = runCatching { NewPhysicsStore.listRirs(context) }.getOrDefault(emptyList())
        while (true) {
            delay(500)
            telemetryV3 = runCatching { NewPhysicsStore.readV3Telemetry() }
                .getOrDefault(NewPhysicsV3TelemetryUi())
        }
    }

    fun apply(next: NewPhysicsState) = onApply(next)

    fun setStage(idx: Int, next: SubBassStageState) {
        val stages = state.subBassStages.toMutableList()
        stages[idx] = next
        apply(state.copy(subBassStages = stages))
    }

    fun selectRir(file: String) {
        apply(state.copy(roomRirFile = file))
        val ok = runCatching {
            NewPhysicsStore.loadRoomRir(context, file, state.roomTailMix)
        }.getOrDefault(false)
        rirStatus = if (ok) "Cola cargada: $file"
        else "Falló la carga — sigue sonando la cola anterior"
    }

    val stageNames = listOf(
        "Etapa 1 · Polo Genezi",
        "Etapa 2 · Axial largo 4.6 m",
        "Etapa 3 · Axial ancho 3.5 m",
        "Etapa 4 · Axial alto 2.9 m",
        "Etapa 5 · 2º armónico largo"
    )
    val activeStages = state.subBassStages.count { it.active }

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        )
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = "FÍSICA NUEVA v3 · ÓRGANOS DESPIERTOS",
                style = MaterialTheme.typography.titleMedium,
                fontWeight = FontWeight.Bold
            )
            Text(
                text = "Cola ${if (telemetryV3.roomTailActive) "ACTIVA" else "inactiva"} · " +
                    "Sub-graves $activeStages/5 · " +
                    "K2 ${if (telemetryV3.geneziKernelMeasured) "MEDIDO" else if (telemetryV3.kernelLoaded) "APROXIMADO" else "sin kernel"} · " +
                    "${"%.0f".format(telemetryV3.cpuUsPerHop)} µs/hop",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary
            )
            Text(
                text = "Ruta B suena con estos mismos defaults (compilados); los sliders v3 mueven solo Ruta A hasta la extensión ABI del snapshot.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.height(8.dp))

            // ── (a) Sub-graves modal ──────────────────────────────────────
            Text(text = "Sub-graves modal (cut-only)", style = MaterialTheme.typography.labelLarge)
            Text(
                text = "Polo Genezi 68.5 Hz + modos axiales de la sala (f = c/2L). " +
                    "La etapa 0 es el diferencial Ω automático y no se edita.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            // v2.5.484: para qué sirven los sub-graves modales.
            ControlHelpText("sub-graves modal")
            for (i in 0 until 5) {
                val st = state.subBassStages[i]
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text(
                        text = "${stageNames[i]} (${"%.1f".format(st.freqHz)} Hz)",
                        style = MaterialTheme.typography.bodySmall
                    )
                    Switch(
                        checked = st.active,
                        onCheckedChange = { on -> setStage(i, st.copy(active = on)) }
                    )
                }
                V3Slider(
                    "Frecuencia", st.freqHz.toFloat(), 15f..120f, "%.1f Hz",
                    helpLabel = "sub-graves etapa frecuencia"
                ) { setStage(i, st.copy(freqHz = it.toDouble())) }
                V3Slider("Q", st.q.toFloat(), 0.3f..18f, "%.1f",
                    helpLabel = "sub-graves etapa q") {
                    setStage(i, st.copy(q = it.toDouble()))
                }
                V3Slider("Ganancia", st.gainDb.toFloat(), -12f..6f, "%+.1f dB",
                    helpLabel = "sub-graves etapa ganancia") {
                    setStage(i, st.copy(gainDb = it.toDouble()))
                }
            }
            Spacer(modifier = Modifier.height(8.dp))

            // ── (b) Cola de sala UPOLA ────────────────────────────────────
            Text(text = "Cola de sala (RIR real)", style = MaterialTheme.typography.labelLarge)
            V3Slider(
                "Mezcla de cola", state.roomTailMix, 0f..1f, "%.2f"
            ) { apply(state.copy(roomTailMix = it)) }
            val currentIdx = rirs.indexOfFirst { it.file == state.roomRirFile }
                .takeIf { it >= 0 } ?: 0
            val current = rirs.getOrNull(currentIdx)
            Text(
                text = current?.let {
                    "${it.file} · sala ${"%.1f".format(it.widthM)}×${"%.1f".format(it.heightM)}×${"%.1f".format(it.depthM)} m · " +
                        "distancia ${"%.2f".format(it.distanceM)} m · RT60 ${"%.2f".format(it.rt60)} s"
                } ?: state.roomRirFile,
                style = MaterialTheme.typography.bodySmall
            )
            // v2.5.484: para qué sirve elegir la sala.
            ControlHelpText("selector rir")
            if (rirStatus.isNotEmpty()) {
                Text(text = rirStatus, style = MaterialTheme.typography.labelSmall)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                OutlinedButton(
                    onClick = {
                        if (rirs.isNotEmpty()) {
                            val prev = (currentIdx - 1 + rirs.size) % rirs.size
                            selectRir(rirs[prev].file)
                        }
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("◀ RIR anterior") }
                OutlinedButton(
                    onClick = {
                        if (rirs.isNotEmpty()) {
                            val next = (currentIdx + 1) % rirs.size
                            selectRir(rirs[next].file)
                        }
                    },
                    modifier = Modifier.weight(1f)
                ) { Text("RIR siguiente ▶") }
            }
            Button(
                onClick = { selectRir("rir_0101.wav") },
                modifier = Modifier.fillMaxWidth()
            ) { Text("Restablecer rir_0101 (sala de Esse)") }
            Spacer(modifier = Modifier.height(8.dp))

            // ── (c) Klippel H3 + térmica ──────────────────────────────────
            Text(text = "Klippel H3 + térmica de bobina", style = MaterialTheme.typography.labelLarge)
            V3Slider("Mezcla H3", state.klippelH3Mix, 0f..1f, "%.2f") {
                apply(state.copy(klippelH3Mix = it))
            }
            Text(
                text = "Bobina L ${"%.1f".format(telemetryV3.coilTempL)} °C · " +
                    "R ${"%.1f".format(telemetryV3.coilTempR)} °C · " +
                    "pico Volterra ${"%.4f".format(telemetryV3.volterraPeak)}",
                style = MaterialTheme.typography.bodySmall
            )
            Text(
                text = if (state.eqClass == 0)
                    "Térmica en espera: la bobina solo se modela con equipo de bocinas (Genezi, bocina, BT, auto, PA, estadio)."
                else
                    "Térmica activa (equipo de bocinas): protección sobre 115 °C, límite 135 °C.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@Composable
private fun V3Slider(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    format: String,
    helpLabel: String? = null,
    onChange: (Float) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Text(
            text = "$label: ${format.format(value)}",
            style = MaterialTheme.typography.bodySmall
        )
        Slider(
            value = value,
            onValueChange = onChange,
            valueRange = range,
            modifier = Modifier.fillMaxWidth()
        )
        // v2.5.484: cada slider explica para qué sirve, debajo del control.
        // Las etiquetas "Frecuencia"/"Q"/"Ganancia" son genéricas: los
        // call sites pasan helpLabel con la clave del catálogo.
        ControlHelpText(helpLabel ?: label)
    }
}
