package com.ivanna.omega.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.OmegaEquipment
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.MagistralGold
import com.ivanna.omega.ui.theme.MagistralLineStrong
import com.ivanna.omega.ui.theme.MagistralMuted
import com.ivanna.omega.ui.theme.MagistralVoid
import com.ivanna.omega.ui.theme.PhosphorGreen
import com.ivanna.omega.ui.theme.TextMuted
import com.ivanna.omega.ui.theme.TextPrimary
import com.ivanna.omega.ui.theme.TextSecondary

// ── Ficha magistral · Ω (prototipo aprobado: qué es / para qué sirve /
// cuándo usarlo / cuándo NO) ─────────────────────────────────────────────
private val omegaSpec = SpecInfo(
    title = "Ω · UNIVERSAL",
    what = "Ω es el operador universal: generaliza a Σ (alto SPL) con dos dimensiones nuevas, el equipo E y la firma S (Harman 2018 + refinamiento insignia). Caracteriza el equipo (su respuesta, su diafonía κ, su ancho de banda), invierte lo invertible, corrige la salida hacia la firma élite y preserva la holofonía hasta el techo que el equipo permite. Cuando el equipo no puede llegar, hace lo mejor posible sin degradar y dice exactamente cuánto falta (E_sig, ε_ILD).",
    purpose = "Que el equipo que sea suene magistral: lo más cerca posible de la firma élite y con la holofonía intacta. En audífonos, la firma sin techo de diafonía; en bocinas del teléfono, la firma sin graves que no existen y el ILD pre-compensado hasta ~3.1 dB; en un PA, Ω es Σ (WFS, γ → ½).",
    whenUse = "Cuando quieras que un equipo concreto (audífonos, bocina Bluetooth, auto, PA) suene lo más cerca posible de la firma élite sin perder el campo 3D. Empieza en Automático (la app detecta la salida) y con el tope de +3 dB: es el modo seguro sin medición.",
    whenAvoid = "No lo enciendas si no sabes qué equipo está sonando (sin ruta detectable Ω no corrige nada: N0, identidad). No subas el refuerzo por encima de +3 dB sin haber medido el equipo: sin medición no hay headroom conocido y reforzar a ciegas distorsiona. En bocina mono no hay ILD posible (κ=1, techo 0 dB): Ω no lo intenta."
)

/**
 * Tarjeta Ω · UNIVERSAL — MAGISTRAL EN CUALQUIER EQUIPO (v2.5.444).
 *
 * Autocontenida: lee y escribe ParameterStore directamente y aplica al
 * nativo por OmegaEquipment.applyToNative(). Se invoca desde
 * IvannaControlPanel tras la tarjeta HOLOS (Σ no tiene tarjeta propia).
 * OFF por defecto (fail-closed): con Ω off la cadena es HOLOS-Σ
 * bit-exacta. La telemetría (E_sig, ε_ILD) se lee del nativo al cambiar
 * cualquier parámetro (solo depende de equipo/perfil, no del audio vivo).
 */
@Composable
fun OmegaControlCard() {
    val context = LocalContext.current
    val paramStore = remember { ParameterStore(context) }

    var omegaEnabled by remember { mutableStateOf(paramStore.isOmegaEnabled()) }
    var omegaEquipment by remember { mutableIntStateOf(paramStore.getOmegaEquipment()) }
    var omegaBoostMax by remember { mutableFloatStateOf(paramStore.getOmegaBoostMaxDb()) }
    var omegaIldPrecomp by remember { mutableStateOf(paramStore.isOmegaIldPrecomp()) }
    var omegaStatus by remember { mutableStateOf(FloatArray(4)) }
    var menuExpanded by remember { mutableStateOf(false) }

    fun apply(en: Boolean, eq: Int, boost: Float, ild: Boolean) {
        paramStore.setOmegaParams(en, eq, boost, ild)
        OmegaEquipment.applyToNative(paramStore)
    }

    // Telemetría Ω: se relee al cambiar cualquier parámetro (E_sig y ε_ILD
    // solo dependen del equipo/perfil, no del audio vivo).
    LaunchedEffect(omegaEnabled, omegaEquipment, omegaBoostMax, omegaIldPrecomp) {
        omegaStatus = runCatching {
            if (IvannaNativeLib.isLoaded) IvannaNativeLib.nativeOmegaStatus() else FloatArray(4)
        }.getOrElse { FloatArray(4) }
    }

    val resolvedEq = OmegaEquipment.resolve(omegaEquipment)
    val knowledgeLabel = when (omegaStatus.getOrNull(1)?.toInt()) {
        1 -> "N1 · clase conocida"
        2 -> "N2 · medido"
        else -> "N0 · sin info"
    }
    val eSig = omegaStatus.getOrNull(2) ?: 0f
    val eIld = omegaStatus.getOrNull(3) ?: 0f

    GlassCard(
        title = "Ω · UNIVERSAL — MAGISTRAL EN CUALQUIER EQUIPO",
        accent = MagistralGold,
        subtitle = if (omegaEnabled)
            "Activo (${OmegaEquipment.label(resolvedEq)} · $knowledgeLabel): la salida se acerca a la firma Harman 2018 con la holofonía intacta hasta el techo del equipo"
        else
            "El mismo campo magistral en el teléfono, los audífonos, el auto y el estadio. Apagado = audio intacto (HOLOS-Σ bit-exacto)"
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Box(modifier = Modifier.weight(1f)) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    Text("ACTIVAR Ω", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
                    Switch(
                        checked = omegaEnabled,
                        onCheckedChange = {
                            omegaEnabled = it
                            apply(it, omegaEquipment, omegaBoostMax, omegaIldPrecomp)
                        },
                        colors = SwitchDefaults.colors(
                            checkedThumbColor = MagistralVoid,
                            checkedTrackColor = MagistralGold,
                            checkedBorderColor = MagistralGold,
                            uncheckedThumbColor = MagistralMuted,
                            uncheckedTrackColor = Color.Transparent,
                            uncheckedBorderColor = MagistralLineStrong
                        )
                    )
                }
            }
            SpecInfoButton(spec = omegaSpec)
        }

        // Equipo: selector (Auto resuelto por la salida real).
        Text("EQUIPO", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
        Box(modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(
                onClick = { menuExpanded = true },
                modifier = Modifier.fillMaxWidth()
            ) {
                Text(OmegaEquipment.label(omegaEquipment), color = TextPrimary)
            }
            DropdownMenu(
                expanded = menuExpanded,
                onDismissRequest = { menuExpanded = false }
            ) {
                for (eq in 0..8) {
                    DropdownMenuItem(
                        text = { Text(OmegaEquipment.label(eq), color = TextPrimary) },
                        onClick = {
                            menuExpanded = false
                            omegaEquipment = eq
                            apply(omegaEnabled, eq, omegaBoostMax, omegaIldPrecomp)
                        }
                    )
                }
            }
        }
        if (omegaEquipment == OmegaEquipment.AUTO) {
            Text(
                "Automático: la app detecta la salida (${OmegaEquipment.label(resolvedEq)}). Sin ruta detectable, Ω no corrige (N0).",
                style = MaterialTheme.typography.labelSmall,
                color = TextMuted
            )
        }

        AuroraSlider("REFUERZO MÁX. DE FIRMA", omegaBoostMax, 0f..12f, unit = " dB",
            displayValue = { "${"%.1f".format(it)} dB" }) {
            omegaBoostMax = it
            apply(omegaEnabled, omegaEquipment, it, omegaIldPrecomp)
        }
        Text(
            "Sin medición del equipo (N1) el refuerzo real se limita a +3 dB aunque pidas más: sin headroom conocido, reforzar a ciegas distorsiona.",
            style = MaterialTheme.typography.labelSmall,
            color = TextMuted
        )

        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text("PRE-COMPENSACIÓN DE DIAFONÍA (ILD)", style = MaterialTheme.typography.labelMedium, color = TextSecondary)
            Switch(
                checked = omegaIldPrecomp,
                onCheckedChange = {
                    omegaIldPrecomp = it
                    apply(omegaEnabled, omegaEquipment, omegaBoostMax, it)
                },
                colors = SwitchDefaults.colors(
                    checkedThumbColor = MagistralVoid,
                    checkedTrackColor = MagistralGold,
                    checkedBorderColor = MagistralGold,
                    uncheckedThumbColor = MagistralMuted,
                    uncheckedTrackColor = Color.Transparent,
                    uncheckedBorderColor = MagistralLineStrong
                )
            )
        }

        // Telemetría Ω (omega_report): cuánto le falta al equipo.
        Spacer(modifier = Modifier.height(4.dp))
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            StatBlock("NIVEL", knowledgeLabel, AuroraCyan, Modifier.weight(1f))
            StatBlock("E_SIG (FIRMA)", "${"%.2f".format(eSig)} dB", PhosphorGreen, Modifier.weight(1f))
            StatBlock("ε_ILD (ESPACIO)", "${"%.2f".format(eIld)} dB", MagistralGold, Modifier.weight(1f))
        }
        Text(
            "E_sig: lo que le falta al equipo para llegar a la firma Harman 2018 (0 dB = llega). ε_ILD: lo que le falta al ILD de diseño por la diafonía de la vía (0 dB = llega). Si un equipo no llega, aquí se ve: no se esconde.",
            style = MaterialTheme.typography.labelSmall,
            color = TextMuted
        )
        Text(
            "v1: perfil de clase (N1). La medición del equipo (barrido con el micrófono) y la Ruta B (daemon) quedan para una fase posterior; Φ también persigue Harman en bucle cerrado y ambas correcciones componen con el tope N1.",
            style = MaterialTheme.typography.labelSmall,
            color = TextMuted
        )
    }
}
