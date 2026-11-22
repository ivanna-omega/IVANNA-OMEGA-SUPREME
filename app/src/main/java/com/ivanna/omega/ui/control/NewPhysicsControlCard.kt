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
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.ivanna.omega.dsp.NewPhysicsState
import com.ivanna.omega.dsp.NewPhysicsStore
import com.ivanna.omega.dsp.NewPhysicsTelemetryUi
import com.ivanna.omega.dsp.NewPhysicsV3TelemetryUi
import kotlinx.coroutines.delay

/**
 * FÍSICA NUEVA · Ω·Σ·LGN·GENEZY — Tarjeta CONTROL (Fase 2, v2.5.472).
 * Todos los controles son REALES y CABLEADOS: cada cambio guarda en
 * SharedPreferences ("ivanna_new_physics_v1") y empuja por
 * NewPhysicsStore.pushToNative -> JNI -> atomics g_np_* -> NewPhysicsEngine
 * (Ruta A). Fail-closed: amount=0 = bypass bit-exacto.
 *
 * v2.5.483: estado ELEVADO — la tarjeta recibe `state` y `onApply` del
 * padre (`NewPhysicsCardsColumn`, que también renderiza la tarjeta v3);
 * así las dos tarjetas nunca divergen. El padre guarda + empuja.
 *
 * HONESTIDAD DE CABLEADO:
 * - Ruta B (ABI v14, v2.5.472): la app ya empuja los 17 controles al
 *   daemon por SET_PERCEPTUAL_STATE (NewPhysicsStore.pushToDaemon, mismo
 *   canal que los fusionados de la 469). Que el efecto los aplique
 *   depende del lado daemon (parseo de las claves np_* + publicación en
 *   el snapshot v14); la tarjeta lo dice en pantalla.
 * - Botón "Calibrar Genezi (ESS $0)": NO está en esta tarjeta a propósito.
 *   La calibración necesita reproducir el ESS por el Genezi y grabar con el
 *   mic — la app no tiene ese grabador cableado, así que un botón aquí sería
 *   un control muerto. La calibración real corre offline en
 *   tools/genezi_calibrate.py y su salida se aplica con
 *   NewPhysicsStore.applyCalibration(). El estado del kernel (MEDIDO /
 *   APROXIMADO / IDENTIDAD) sí se muestra en vivo abajo.
 */
@Composable
fun NewPhysicsControlCard(
    state: NewPhysicsState,
    onApply: (NewPhysicsState) -> Unit,
    modifier: Modifier = Modifier
) {
    var telemetry by remember { mutableStateOf(NewPhysicsTelemetryUi()) }
    var telemetryV3 by remember { mutableStateOf(NewPhysicsV3TelemetryUi()) }

    // Telemetría en vivo cada 500 ms (Ruta A; ceros si el nativo no cargó).
    LaunchedEffect(Unit) {
        while (true) {
            delay(500)
            telemetry = runCatching { NewPhysicsStore.readTelemetry() }
                .getOrDefault(NewPhysicsTelemetryUi())
            telemetryV3 = runCatching { NewPhysicsStore.readV3Telemetry() }
                .getOrDefault(NewPhysicsV3TelemetryUi())
        }
    }

    fun apply(next: NewPhysicsState) = onApply(next)

    Card(
        modifier = modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceVariant
        )
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Text(
                    text = "FÍSICA NUEVA · Ω·Σ·LGN·GENEZY",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )
                Switch(
                    checked = state.amount > 0.5f,
                    onCheckedChange = { on ->
                        apply(state.copy(amount = if (on) 1.0f else 0.0f))
                    }
                )
            }
            // v2.5.484: para qué sirve el interruptor maestro.
            ControlHelpText("interruptor maestro omega")
            Text(
                text = if (state.amount > 0.001f)
                    "ACTIVO · Ruta A · ${"%.0f".format(telemetry.cpuUsPerHop)} µs/hop (presupuesto 2667 µs @ 48 kHz)"
                else
                    "BYPASS BIT-EXACTO (OFF)",
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.primary
            )
            Text(
                text = "Ruta B (ABI v14): la app empuja los 17 controles al daemon por SET_PERCEPTUAL_STATE; el efecto los aplica al publicarse el snapshot v14.",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
            Spacer(modifier = Modifier.height(8.dp))

            // Telemetría honesta en vivo.
            Text(
                text = "E_sig ${"%.2f".format(telemetry.sigErrorRmsDb)} dB · " +
                    "D_max ${"%.1f".format(telemetry.xtalkCeilingDb)} dB · " +
                    "γ(σ) ${"%.2f".format(telemetry.scaleGamma)} · " +
                    "α(L) ${"%.2f".format(telemetry.ildAlpha)}",
                style = MaterialTheme.typography.bodySmall
            )
            // K2: MEDIDO (genezi_calibrate.py) o APROXIMADO (v2.5.483, H2
            // 0.50% / H3 0.20% @85 dB SPL, valores típicos documentados en
            // ~/workspace/np483/APPROXIMATIONS.md). Sin kernel = identidad.
            Text(
                text = when {
                    telemetryV3.geneziKernelMeasured -> "K2: MEDIDO · Genezi calibrado activo"
                    telemetryV3.kernelLoaded -> "K2: APROXIMADO v2.5.483 · H2 0.50% / H3 0.20% @85 dB (ver APPROXIMATIONS.md)"
                    else -> "K2: IDENTIDAD (sin kernel — los órganos v3 despiertan al preparar el motor)"
                },
                style = MaterialTheme.typography.bodySmall
            )
            Spacer(modifier = Modifier.height(8.dp))

            // Selector de equipo Ω (7 clases del motor; E5 no existe en
            // EquipmentClass: 0=E1 audífonos, 1=E2 bocina G85, 2=E3 BT,
            // 3=E4 Genezi, 4=E6 auto, 5=E7 PA/club, 6=E8 estadio).
            Text(text = "Equipo Ω", style = MaterialTheme.typography.labelLarge)
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                EqChip("Audífonos", 0, state.eqClass) { apply(state.copy(eqClass = 0)) }
                EqChip("Bocina G85", 1, state.eqClass) { apply(state.copy(eqClass = 1)) }
                EqChip("BT", 2, state.eqClass) { apply(state.copy(eqClass = 2)) }
                EqChip("Genezi PZ1D", 3, state.eqClass) { apply(state.copy(eqClass = 3)) }
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                EqChip("Auto", 4, state.eqClass) { apply(state.copy(eqClass = 4)) }
                EqChip("PA / Club", 5, state.eqClass) { apply(state.copy(eqClass = 5)) }
                EqChip("Estadio", 6, state.eqClass) { apply(state.copy(eqClass = 6)) }
            }
            // v2.5.484: para qué sirve elegir el equipo.
            ControlHelpText("equipo omega")
            Spacer(modifier = Modifier.height(8.dp))

            NpSlider("Mezcla Maestra", state.amount, 0f..1.5f, "%.2f") {
                apply(state.copy(amount = it))
            }
            NpSlider("LGN 3D Pinna", state.lgnPinnaMix, 0f..1f, "%.2f") {
                apply(state.copy(lgnPinnaMix = it))
            }
            NpSlider("Amortiguación Bright-Spot", state.brightSpotDamp, 0f..1f, "%.2f") {
                apply(state.copy(brightSpotDamp = it))
            }
            // v2.5.483: los 5 sliders que faltaban del struct NewPhysicsControls.
            NpSlider("Eco de Torso (retardo torso)", state.torsoEchoMix, 0f..0.5f, "%.2f") {
                apply(state.copy(torsoEchoMix = it))
            }
            NpSlider(
                "Azimut Ω (${"%.0f".format(state.azimuthDeg)}°)",
                state.azimuthDeg, -180f..180f, "%.0f"
            ) { apply(state.copy(azimuthDeg = it)) }
            NpSlider(
                "Elevación Ω (${"%.0f".format(state.elevationDeg)}°)",
                state.elevationDeg, -40f..70f, "%.0f"
            ) { apply(state.copy(elevationDeg = it)) }
            NpSlider(
                "Tope de Refuerzo Ω (${"%.1f".format(state.boostCapDb)} dB)",
                state.boostCapDb, 0f..9f, "%.1f"
            ) { apply(state.copy(boostCapDb = it)) }
            NpSlider("Compensación de Aire", state.airCompMix, 0f..1f, "%.2f") {
                apply(state.copy(airCompMix = it))
            }
            NpSlider(
                "Radio Cabeza Personal (${"%.1f".format(state.headRadiusM * 100f)} cm)",
                state.headRadiusM, 0.065f..0.115f, "%.3f"
            ) { apply(state.copy(headRadiusM = it)) }
            NpSlider("Ω Firma Élite Harman2018 − H_b", state.omegaSignatureMix, 0f..1f, "%.2f") {
                apply(state.copy(omegaSignatureMix = it))
            }
            NpSlider("Pre-Compensación Diafonía Ec.7", state.crosstalkCompMix, 0f..1.5f, "%.2f") {
                apply(state.copy(crosstalkCompMix = it))
            }
            NpSlider(
                "Σ Distancia / Escala (${"%.2f".format(state.sigmaDistanceM)} m)",
                state.sigmaDistanceM, 0.15f..5.0f, "%.2f",
                helpLabel = "sigma distancia"
            ) { apply(state.copy(sigmaDistanceM = it)) }
            NpSlider(
                "Σ Nivel SPL Estimado (${"%.0f".format(state.splEstimateDb)} dB)",
                state.splEstimateDb, 65f..115f, "%.0f"
            ) { apply(state.copy(splEstimateDb = it)) }
            NpSlider("Genezi Volterra Laguerre H2", state.volterraMix, 0f..1f, "%.2f") {
                apply(state.copy(volterraMix = it))
            }
            NpSlider("Lazo Variacional F · MOC + Onsets", state.freeEnergyMix, 0f..1f, "%.2f") {
                apply(state.copy(freeEnergyMix = it))
            }

            Spacer(modifier = Modifier.height(8.dp))
            // ABX cíclico: Off -> A (Bypass) -> B (Física Nueva) -> Off.
            val abxLabel = when (state.abxOverrideMode) {
                1 -> "ABX: A · Bypass"
                2 -> "ABX: B · Física Nueva"
                else -> "ABX: Off"
            }
            Button(
                onClick = {
                    val next = (state.abxOverrideMode + 1) % 3
                    apply(state.copy(abxOverrideMode = next))
                },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(abxLabel)
            }
            // v2.5.484: para qué sirve el ABX.
            ControlHelpText("abx")
        }
    }
}

@Composable
private fun EqChip(label: String, value: Int, selected: Int, onClick: () -> Unit) {
    if (selected == value) {
        Button(onClick = onClick) { Text(label) }
    } else {
        OutlinedButton(onClick = onClick) { Text(label) }
    }
}

@Composable
private fun NpSlider(
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
        // Por defecto la clave es su propia etiqueta; helpLabel la anula
        // cuando la etiqueta no casa con el catálogo (Σ usa "sigma", no σ).
        ControlHelpText(helpLabel ?: label)
    }
}
