package com.ivanna.omega.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.audio.FusedControlsBridge
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.ui.theme.*
import kotlinx.coroutines.delay
import org.json.JSONObject
import java.util.Locale
import kotlin.math.log10

// NOTA SAFMUS (regla durable): este archivo NO importa
// androidx.compose.material3.Slider. El `Slider(...)` de abajo resuelve al
// wrapper SAFMUS del mismo paquete (SafmusComposeSlider.kt): sanea y
// clampa el valor antes de renderizar y de despachar al DSP, y deshabilita
// el control si el rango es inválido. Importar el Slider de Material3
// aquí lo sombrearía y rompería la regla.

private data class ConservationTelemetry(
    val active: Boolean = false,
    val appliedGain: Float = 1.0f,
    val engineAmount: Float = 0.0f,
)

/**
 * ConservationPanel — control y telemetría de la capa de conservación
 * perceptual (árbitro de campo, 2026-11-21).
 *
 * Un estado de campo común por banda que los 7 operadores respetan; el
 * solucionador central arbitra sus propuestas bajo energía-fase-causalidad
 * (nunca amplifica = nunca crea energía, sin discontinuidades de fase,
 * estrictamente causal). La capa vive en la FusedChain, después de los 7
 * operadores y antes de la mezcla final.
 *
 * Encendido/apagado: amount 0 = apagada (bypass bit-exacto, el audio pasa
 * idéntico). Cada cambio se guarda en ParameterStore (claves fused_consv_*)
 * y se empuja a Ruta A (JNI, FusedControlsBridge.safeSetFusedControls) y a
 * Ruta B (SET_PERCEPTUAL_STATE al daemon; si el JNI no cargó, se empuja
 * directo al daemon como hace PersistedStateRestorer).
 */
@Composable
fun ConservationPanel(modifier: Modifier = Modifier) {
    val appContext = LocalContext.current.applicationContext
    val store = remember { ParameterStore(appContext) }
    var amount by remember { mutableStateOf(store.getConsvAmount()) }
    var energyCap by remember { mutableStateOf(store.getConsvEnergyCap()) }
    var phaseCoherence by remember { mutableStateOf(store.getConsvPhaseCoherence()) }
    var inertia by remember { mutableStateOf(store.getConsvInertia()) }
    // Última cantidad > 0, para que el interruptor la restaure al encender.
    var lastAmount by remember { mutableStateOf(if (amount > 0f) amount else 0.5f) }
    var telemetry by remember { mutableStateOf(ConservationTelemetry()) }

    val magenta = NeonMagenta
    val emerald = PhosphorGreen
    val gold = AmberSignal
    val bgCard = ObsidianRaised
    val bgSub = ObsidianInset

    // Telemetría REAL del motor (Ruta A): ganancia que la capa aplica ahora.
    // Sin nativo cargado, safeGetConservationStatusJson() devuelve null y la
    // fila muestra "—" (sin lectura), nunca un número inventado.
    LaunchedEffect(Unit) {
        while (true) {
            FusedControlsBridge.safeGetConservationStatusJson()?.let { json ->
                runCatching { JSONObject(json) }.getOrNull()?.let { o ->
                    telemetry = ConservationTelemetry(
                        active = o.optInt("active", 0) != 0,
                        appliedGain = o.optDouble("appliedGain", 1.0).toFloat(),
                        engineAmount = o.optDouble("amount", 0.0).toFloat(),
                    )
                }
            }
            delay(400)
        }
    }

    // Empuja los controles COMPLETOS (los 7 operadores + conservación) desde
    // lo persistido: el panel solo toca las claves fused_consv_*, el resto
    // sale del store tal cual (mismo comportamiento que el restore de arranque).
    fun push() {
        val fused = store.loadFusedControls()
        FusedControlsBridge.safeSetFusedControls(fused)
        if (!FusedControlsBridge.isLoaded) FusedControlsBridge.pushFusedToDaemon(fused)
    }

    Card(
        modifier = modifier.fillMaxWidth().padding(vertical = 6.dp),
        colors = CardDefaults.cardColors(containerColor = bgCard),
        shape = RoundedCornerShape(16.dp)
    ) {
        Column(modifier = Modifier.padding(14.dp)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically
            ) {
                Column(modifier = Modifier.weight(1f)) {
                    Text(
                        "CONSERVACIÓN PERCEPTUAL · ÁRBITRO DE CAMPO",
                        color = magenta, fontWeight = FontWeight.ExtraBold, fontSize = 13.sp
                    )
                    Text(
                        "El campo que los 7 operadores respetan · nunca crea energía · causal",
                        color = TextSecondary, fontSize = 10.sp
                    )
                }
                Switch(
                    checked = amount > 0f,
                    onCheckedChange = { on ->
                        amount = if (on) {
                            if (lastAmount > 0f) lastAmount else 0.5f
                        } else {
                            if (amount > 0f) lastAmount = amount
                            0f
                        }
                        store.setConsvAmount(amount)
                        push()
                    }
                )
            }

            Spacer(Modifier.height(10.dp))

            Row(
                modifier = Modifier.fillMaxWidth().background(bgSub, RoundedCornerShape(10.dp)).padding(10.dp),
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    if (telemetry.active) "ACTIVA" else "APAGADA",
                    color = if (telemetry.active) emerald else TextMuted,
                    fontWeight = FontWeight.Bold, fontSize = 14.sp
                )
                Text(
                    if (FusedControlsBridge.isLoaded) {
                        val gainDb = if (telemetry.appliedGain > 0f) {
                            20f * log10(telemetry.appliedGain)
                        } else {
                            -120f
                        }
                        String.format(
                            Locale.US,
                            "Ganancia %.3f (%.1f dB) · Motor %.2f",
                            telemetry.appliedGain, gainDb, telemetry.engineAmount
                        )
                    } else {
                        "Sin nativo — sin lectura"
                    },
                    color = TextPrimary, fontSize = 11.sp, fontFamily = FontFamily.Monospace
                )
            }

            Spacer(Modifier.height(8.dp))
            Text(
                "Amount 0 = apagada (bypass bit-exacto). La ganancia de la capa nunca pasa de 1: solo puede recortar, nunca amplificar.",
                color = gold, fontSize = 11.sp
            )

            Spacer(Modifier.height(10.dp))
            HorizontalDivider(color = ObsidianEdge)
            Spacer(Modifier.height(10.dp))

            // Cantidad (0..1, default 0 = apagada)
            Text(
                String.format(Locale.US, "Cantidad de conservación: %.2f", amount),
                color = TextPrimary, fontSize = 11.sp
            )
            Slider(
                value = amount, valueRange = 0f..1f,
                onValueChange = {
                    amount = it
                    if (it > 0f) lastAmount = it
                    store.setConsvAmount(it)
                    push()
                }
            )

            // Techo de energía (0..1, default 1.0 = sin techo)
            Text(
                String.format(Locale.US, "Techo de energía por banda: %.2f", energyCap),
                color = TextPrimary, fontSize = 11.sp
            )
            Slider(
                value = energyCap, valueRange = 0f..1f,
                onValueChange = {
                    energyCap = it
                    store.setConsvEnergyCap(it)
                    push()
                }
            )

            // Coherencia de fase (0..1, default 0.5)
            Text(
                String.format(Locale.US, "Coherencia de fase (suavidad del cambio de ganancia): %.2f", phaseCoherence),
                color = TextPrimary, fontSize = 11.sp
            )
            Slider(
                value = phaseCoherence, valueRange = 0f..1f,
                onValueChange = {
                    phaseCoherence = it
                    store.setConsvPhaseCoherence(it)
                    push()
                }
            )

            // Inercia (0..0.99, default 0.82)
            Text(
                String.format(Locale.US, "Inercia del estado de campo: %.2f", inertia),
                color = TextPrimary, fontSize = 11.sp
            )
            Slider(
                value = inertia, valueRange = 0f..0.99f,
                onValueChange = {
                    inertia = it
                    store.setConsvInertia(it)
                    push()
                }
            )
        }
    }
}
