package com.ivanna.omega.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
// v2.5.448: NO importar androidx.compose.material3.Slider — el contrato
// SAFMUS (scripts/check_safmus_transversal_wiring.py) prohíbe el Slider
// directo de Material3 en este paquete. Se usa el wrapper compartido
// `Slider` de SafmusComposeSlider.kt (mismo paquete, sin import), que
// sanea los valores con SafmusUniversalKernel.sanitizeControlValue.
import androidx.compose.material3.SliderDefaults
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.core.FourDRouteB
import com.ivanna.omega.core.Ivanna4DNative
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.spatial.WfsCalibrationManager
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.GlassOutline
import com.ivanna.omega.ui.theme.ObsidianSoft
import com.ivanna.omega.ui.theme.ObsidianVoid
import com.ivanna.omega.ui.theme.TextMuted
import java.util.Locale

/**
 * WFS · CALIBRACIÓN DE SALA — v2.5.448 (refinamiento magistral + sin root).
 *
 * Directiva de Esse (2026-11-15): este sub-entorno no estaba refinado y debe
 * funcionar SIN root. Antes usaba el tema Material por defecto (morado) y el
 * 4D dependía del daemon (Ruta B, root) como vía principal.
 *
 * Ahora:
 * - Tema IVANNA magistral (ObsidianVoid, AuroraCyan, tarjetas ObsidianSoft).
 * - SIN ROOT: la geometría WFS y el 4D se aplican por el motor NATIVO EN
 *   PROCESO (libivanna_omega.so, Ruta A) — que es la vía primaria. El envío
 *   al daemon (SET_4D / Ruta B) queda como refuerzo best-effort, no como
 *   requisito. Al abrir el panel se carga la librería nativa y se restaura
 *   la geometría guardada, así los controles actúan aunque nunca se haya
 *   reproducido audio antes.
 * - Estado honesto: dice si se aplicó en el motor de la app o si solo quedó
 *   guardado (motor no cargado).
 */
@Composable
fun WfsCalibrationPanel() {
    val ctx = LocalContext.current
    val store = remember { ParameterStore(ctx.applicationContext) }
    var fdOn by remember { mutableStateOf(store.is4dEnabled()) }
    var fdDepth by remember { mutableStateOf(store.get4dDepth()) }
    var fdLead by remember { mutableStateOf(store.get4dLeadMs()) }
    var fdArc by remember { mutableStateOf(store.is4dArcOn()) }
    var fdArcDepth by remember { mutableStateOf(store.get4dArcDepth()) }
    var lx by remember { mutableStateOf(WfsCalibrationManager.listenerX) }
    var ly by remember { mutableStateOf(WfsCalibrationManager.listenerY) }
    var lz by remember { mutableStateOf(WfsCalibrationManager.listenerZ) }
    var yaw by remember { mutableStateOf(WfsCalibrationManager.listenerYawDegrees) }
    var nativeReady by remember { mutableStateOf(false) }
    var status by remember {
        mutableStateOf("Ajusta la geometría de escucha. Al soltar un control, IVANNA guarda la posición y la aplica en el motor de la app (sin root).")
    }

    // Al abrir: cargar la librería nativa (en proceso, sin root) y restaurar
    // la geometría guardada, para que los controles actúen desde el primer
    // toque aunque el motor aún no se haya usado en esta sesión.
    LaunchedEffect(Unit) {
        nativeReady = IvannaNativeLib.isLoaded || Ivanna4DNative.isAvailable
        WfsCalibrationManager.restore(ctx)
        lx = WfsCalibrationManager.listenerX
        ly = WfsCalibrationManager.listenerY
        lz = WfsCalibrationManager.listenerZ
        yaw = WfsCalibrationManager.listenerYawDegrees
        if (nativeReady) {
            // Reaplicar el 4D guardado al motor en proceso (sin root).
            applyFourDInProcess(store, fdOn, fdDepth, fdLead, fdArc, fdArcDepth)
        }
    }

    fun applyPosition() {
        val sent = WfsCalibrationManager.setListener(ctx, lx, ly, lz, yaw)
        status = if (sent) {
            "Geometría aplicada en el motor de la app (sin root). Esto confirma el envío al motor, no una medición acústica ni una mejora audible automática."
        } else {
            "Motor nativo no cargado: los valores quedaron guardados y se aplicarán cuando el motor arranque. No hace falta root."
        }
    }

    fun fmt(value: Float) = String.format(Locale.getDefault(), "%.2f", value)

    Column(
        Modifier.fillMaxSize().background(ObsidianVoid)
            .verticalScroll(rememberScrollState()).padding(horizontal = 16.dp, vertical = 12.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        // ── Cabecera ───────────────────────────────────────────────────
        Text(
            "WFS · CALIBRACIÓN DE SALA",
            color = AuroraCyan, fontWeight = FontWeight.ExtraBold, fontSize = 16.sp
        )
        Text(
            "Indica dónde está tu cabeza y hacia dónde miras respecto al conjunto virtual de siete altavoces. Funciona sin root: la geometría se aplica en el motor de la app. WFS solo ofrece el resultado esperado cuando la salida y el sistema de altavoces son compatibles; no convierte unos audífonos estéreo en un arreglo WFS físico.",
            color = TextMuted, fontSize = 13.sp
        )
        Text(
            if (nativeReady) "Motor en proceso: LISTO (sin root)" else "Motor en proceso: cargando…",
            color = if (nativeReady) AuroraCyan else TextMuted, fontSize = 12.sp, fontWeight = FontWeight.Bold
        )

        WfsCard("1 · Posición izquierda / derecha", "Oyente X: ${fmt(lx)} m de 0 a 3,5 m") {
            WfsSlider(lx, 0f..3.5f, { lx = it }, { applyPosition() })
            WfsHelp("Qué hace: mueve el punto de escucha a lo ancho de la sala. Úsalo para corregir si tu asiento queda desplazado del centro. Cuándo sí: asiento descentrado. Cuándo no: si escuchas centrado, déjalo en 1.75 m.")
        }

        WfsCard("2 · Altura del oyente", "Oyente Y: ${fmt(ly)} m de 0 a 3,5 m") {
            WfsSlider(ly, 0f..3.5f, { ly = it }, { applyPosition() })
            WfsHelp("Qué hace: ajusta la altura de referencia de los altavoces virtuales respecto a tus oídos. Mide desde el suelo aproximadamente. Cuándo sí: si tus oídos quedan muy por encima o por debajo de los altavoces. Cuándo no: en audífonos, la altura la manda el HRTF, no este control.")
        }

        WfsCard("3 · Distancia al frente / fondo", "Oyente Z: ${fmt(lz)} m de 0 a 7 m") {
            WfsSlider(lz, 0f..7f, { lz = it }, { applyPosition() })
            WfsHelp("Qué hace: sitúa el punto de escucha desde la parte frontal hacia el fondo. Ajústalo a la distancia real entre el asiento y el frente de la sala. Cuándo sí: sala real con medidas conocidas. Cuándo no: si no conoces la distancia, usa el preset 7ch.")
        }

        WfsCard("4 · Orientación del oyente", "Giro horizontal: ${fmt(yaw)}°") {
            WfsSlider(yaw, 0f..360f, { yaw = it }, { applyPosition() })
            WfsHelp("Qué hace: gira el marco de referencia alrededor del eje vertical y recalcula las coordenadas relativas de los siete altavoces. Úsalo si escuchas mirando hacia un lado; 0° conserva la orientación original. Cuándo no: si miras al frente, déjalo en 0°.")
        }

        WfsCard("Acciones rápidas", null) {
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                WfsButton("Aplicar preset 7ch", Modifier.weight(1f)) {
                    val sent = WfsCalibrationManager.applyLayout(ctx, "7ch-default")
                    status = if (sent) "Preset 7ch aplicado en el motor de la app (sin root) con la geometría actual. Aún requiere escucha y validación en la sala."
                        else "No se pudo aplicar el preset: motor nativo no disponible. Quedó guardado."
                }
                WfsButton("Centrar", Modifier.weight(1f)) {
                    lx = 1.75f
                    ly = 1.20f
                    lz = 3.50f
                    yaw = 0f
                    applyPosition()
                }
            }
            WfsHelp("Preset 7ch: reenvía el diseño de siete altavoces virtuales. Centrar: restaura una posición de referencia y orientación frontal.")
        }

        WfsCard("4D · Campo anticipado", null) {
            // Mismo cableado que FourDPredictionPanel, pero SIN ROOT como vía
            // primaria: primero el motor nativo en proceso (Ivanna4DNative),
            // que es lo que suena; el envío al daemon (FourDRouteB, Ruta B)
            // es solo un refuerzo best-effort cuando hay daemon.
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Switch(
                    checked = fdOn,
                    onCheckedChange = {
                        fdOn = it
                        store.set4dEnabled(it)
                        applyFourDInProcess(store, fdOn, fdDepth, fdLead, fdArc, fdArcDepth)
                        FourDRouteB.push(ctx, store)
                    },
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = Color.Black,
                        checkedTrackColor = AuroraCyan,
                        uncheckedThumbColor = TextMuted,
                        uncheckedTrackColor = ObsidianSoft
                    )
                )
                Text(if (fdOn) "4D activo" else "4D apagado: no altera el audio", color = Color.White, fontWeight = FontWeight.Bold)
            }
            Text("Profundidad 4D: ${fmt(fdDepth)}", color = Color.White, fontSize = 14.sp)
            WfsSlider(fdDepth, 0f..1f, {
                fdDepth = it
                store.set4dDepth(it)
                applyFourDInProcess(store, fdOn, fdDepth, fdLead, fdArc, fdArcDepth)
                FourDRouteB.push(ctx, store)
            }, null)
            Text("Antelación (lead): ${String.format(Locale.getDefault(), "%.1f", fdLead)} ms", color = Color.White, fontSize = 14.sp)
            WfsSlider(fdLead, 5f..10f, {
                fdLead = it
                store.set4dLeadMs(it)
                applyFourDInProcess(store, fdOn, fdDepth, fdLead, fdArc, fdArcDepth)
                FourDRouteB.push(ctx, store)
            }, null)
            Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                Switch(
                    checked = fdArc,
                    onCheckedChange = {
                        fdArc = it
                        store.set4dArcOn(it)
                        store.set4dArcDepth(fdArcDepth)
                        applyFourDInProcess(store, fdOn, fdDepth, fdLead, fdArc, fdArcDepth)
                        FourDRouteB.push(ctx, store)
                    },
                    colors = SwitchDefaults.colors(
                        checkedThumbColor = Color.Black,
                        checkedTrackColor = AuroraCyan,
                        uncheckedThumbColor = TextMuted,
                        uncheckedTrackColor = ObsidianSoft
                    )
                )
                Text("Arco narrativo", color = Color.White, fontWeight = FontWeight.Bold)
            }
            Text("Profundidad del arco: ${fmt(fdArcDepth)}", color = Color.White, fontSize = 14.sp)
            WfsSlider(fdArcDepth, 0f..1f, {
                fdArcDepth = it
                store.set4dArcDepth(it)
                applyFourDInProcess(store, fdOn, fdDepth, fdLead, fdArc, fdArcDepth)
                FourDRouteB.push(ctx, store)
            }, null)
            WfsHelp("Los controles se aplican en el motor de la app (sin root). El envío al daemon es solo un refuerzo cuando hay Ruta B. Aplicar no equivale a una mejora audible verificada.")
        }

        WfsCard("Estado de aplicación", null) {
            Text(status, color = Color.White, fontSize = 14.sp)
        }

        Text(
            "Recomendaciones: en audífonos, usa el modo binaural/HRTF en lugar de WFS físico; en estéreo por AUX o DAC USB-C, elige la salida estéreo normal salvo que conectes un arreglo multicanal real. Para una sala 7.1, confirma primero el número de canales que acepta tu receptor. Haz los ajustes a volumen moderado y cambia una variable por vez.",
            color = TextMuted, fontSize = 12.sp
        )
        Spacer(Modifier.height(16.dp))
    }
}

/**
 * Aplica el 4D al motor NATIVO EN PROCESO (sin root) — la vía primaria.
 * El daemon (FourDRouteB) es solo refuerzo y se llama aparte, nunca como
 * requisito. Sin esto, sin root el 4D no sonaba: solo se enviaba al daemon.
 */
private fun applyFourDInProcess(
    store: ParameterStore,
    enabled: Boolean,
    depth: Float,
    leadMs: Float,
    arcOn: Boolean,
    arcDepth: Float
) {
    if (!Ivanna4DNative.isAvailable) return
    runCatching {
        Ivanna4DNative.nativeFourDSetEnabled(enabled)
        Ivanna4DNative.nativeFourDSetDepth(depth)
        Ivanna4DNative.nativeFourDSetLeadMs(leadMs)
        Ivanna4DNative.nativeFourDSetArcDepth(arcDepth)
        Ivanna4DNative.nativeFourDSetArc(arcOn)
    }
}

@Composable
private fun WfsCard(title: String, value: String?, content: @Composable () -> Unit) {
    Column(
        Modifier.fillMaxWidth()
            .background(ObsidianSoft, RoundedCornerShape(14.dp))
            .border(1.dp, GlassOutline.copy(alpha = 0.5f), RoundedCornerShape(14.dp))
            .padding(16.dp),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Text(title, color = AuroraCyan, fontWeight = FontWeight.Bold, fontSize = 15.sp)
        if (value != null) Text(value, color = Color.White, fontSize = 14.sp)
        content()
    }
}

@Composable
private fun WfsSlider(
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    onValueChange: (Float) -> Unit,
    onValueChangeFinished: (() -> Unit)?
) {
    Slider(
        value = value,
        onValueChange = onValueChange,
        onValueChangeFinished = onValueChangeFinished,
        valueRange = range,
        modifier = Modifier.fillMaxWidth(),
        colors = SliderDefaults.colors(
            thumbColor = AuroraCyan,
            activeTrackColor = AuroraCyan,
            inactiveTrackColor = GlassOutline
        )
    )
}

@Composable
private fun WfsHelp(text: String) {
    Text(text, color = TextMuted, fontSize = 12.sp)
}

@Composable
private fun WfsButton(label: String, modifier: Modifier = Modifier, onClick: () -> Unit) {
    Button(
        onClick = onClick,
        modifier = modifier,
        colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan, contentColor = Color.Black)
    ) {
        Text(label, fontWeight = FontWeight.Bold, fontSize = 14.sp)
    }
}
