package com.ivanna.omega.ui.oem
import androidx.compose.material.icons.automirrored.filled.ArrowBack

import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.ui.theme.*

@Composable
fun OemAcousticScreen(state: OemState, onBack: () -> Unit) {
    Column(
        Modifier.fillMaxSize().background(ObsidianVoid)
            .windowInsetsPadding(WindowInsets.systemBars)
    ) {
        Row(Modifier.fillMaxWidth().background(ObsidianSoft).padding(14.dp),
            verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(10.dp)) {
            IconButton(onClick = onBack, modifier = Modifier.size(32.dp)) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, null, tint = PhosphorGreen)
            }
            Column(Modifier.weight(1f)) {
                Text("ACÚSTICA SAF · RIR", color = PhosphorGreen, fontWeight = FontWeight.ExtraBold,
                    fontSize = 12.sp, letterSpacing = 1.5.sp)
                Text("Simulación acústica basada en respuestas de sala reales",
                    color = TextMuted, fontSize = 9.sp)
            }
        }

        Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)) {

            // Sala activa
            OemCard(Modifier.fillMaxWidth()) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Text("SALA ACTIVA · RIR DATASET", color = TextMuted, fontSize = 8.sp,
                        fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
                    Surface(shape = RoundedCornerShape(4.dp),
                        color = if (state.rirOn && state.rirLoaded) PhosphorGreen.copy(0.15f)
                                else ObsidianEdge) {
                        Text(if (state.rirOn && state.rirLoaded) "ACTIVO" else if (state.rirLoaded) "LISTO" else "DESACTIVADA",
                            color = if (state.rirOn && state.rirLoaded) PhosphorGreen else TextMuted,
                            fontSize = 8.sp, fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(horizontal = 8.dp, vertical = 3.dp))
                    }
                }
                Spacer(Modifier.height(8.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(16.dp)) {
                    Column(Modifier.weight(1f)) {
                        TelRow("Dataset RIR", "200 salas reales", AuroraCyan)
                        TelRow("Longitud IR", "~2048 muestras", TextSecondary)
                        TelRow("Crossfade IR", "32 bloques XFADE", PhosphorGreen)
                        TelRow("Algoritmo", "Overlap-save FFT", TextSecondary)
                    }
                    Column(Modifier.weight(1f)) {
                        TelRow("FFT size", "1024 pt", TextSecondary)
                        TelRow("Precision twiddle", "double (cos/sin directo)", PhosphorGreen)
                        TelRow("Nivel reverb", "${"%.3f".format(state.reverbLevel)}", AuroraCyan)
                        TelRow("Wet smooth", "one-pole @ 500ms", TextSecondary)
                    }
                }
            }

            // RT60 y métricas acústicas
            OemCard(Modifier.fillMaxWidth()) {
                Text("CARACTERÍSTICAS ACÚSTICAS DETECTADAS", color = TextMuted, fontSize = 8.sp,
                    fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
                Spacer(Modifier.height(8.dp))
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    AcousticGauge("REVERB", state.reverbLevel, "nivel",      PhosphorGreen, Modifier.weight(1f))
                    AcousticGauge("PERCUS", state.percussiveness, "transitorio", NeonMagenta,  Modifier.weight(1f))
                    AcousticGauge("TONAL",  state.tonality, "armónica",        AuroraCyan,  Modifier.weight(1f))
                    AcousticGauge("DYN",    (state.dynRange/30f).coerceIn(0f,1f), "${state.dynRange.toInt()}dB", AmberSignal, Modifier.weight(1f))
                }
                Spacer(Modifier.height(6.dp))
                TelRow("Centroide espectral", "${state.spectralCentroid.toInt()} Hz", AuroraCyan)
            }

            // Controles de sala — Fase 2: los sliders LEEN el valor guardado
            // al entrar (antes eran 0.3/0.5/0.5 locales y mentían al abrir).
            // Nivel = rirWet guardado (SpatialAudioPrefs, el que restaura el
            // arranque); tamaño = rirRt60 guardado (0.1–1.5 s -> 0–1);
            // difusión = orbeDiffuse guardado (ParameterStore, ya se reescribía
            // al mover el slider). Cada cambio se vuelve a guardar, así al
            // reabrir se ve lo último que se puso. El envío al motor no cambia.
            val appContext = androidx.compose.ui.platform.LocalContext.current
            val savedRoom = remember(appContext) { com.ivanna.omega.ui.SpatialAudioPrefs.load(appContext) }
            var roomWet   by remember { mutableFloatStateOf(savedRoom.rirWet.coerceIn(0f, 1f)) }
            var roomSize  by remember { mutableFloatStateOf(((savedRoom.rirRt60 - 0.1f) / 1.4f).coerceIn(0f, 1f)) }
            var diffusion by remember { mutableFloatStateOf(runCatching { com.ivanna.omega.core.ParameterStore(appContext).getOrbeDiffuse() }.getOrDefault(0.5f).coerceIn(0f, 1f)) }
            OemCard(Modifier.fillMaxWidth()) {
                Text("CONTROLES DE SALA", color = TextMuted, fontSize = 8.sp,
                    fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
                Spacer(Modifier.height(8.dp))
                OemSliderRow("Nivel de sala", roomWet, 0f, 1f, "${"%.0f".format(roomWet*100)}%") {
                    roomWet = it
                    runCatching { IvannaNativeLib.nativeSetSpatialWet(it) }
                    runCatching { com.ivanna.omega.ui.SpatialAudioPrefs.save(appContext, savedRoom.copy(rirWet = it)) }
                }
                OemSliderRow("Tamaño", roomSize, 0f, 1f, "${"%.0f".format(roomSize*100)}%") {
                    roomSize = it
                    // v2.5.476 — Tamaño cableado: RT60 [0.1, 1.5] s (puerta
                    // SAFMUS) a la sala RIR, app nativa + daemon (Ruta B).
                    runCatching {
                        com.ivanna.omega.magisk.OmegaEngineBridge.setRoom(
                            rt60S = 0.1f + it * 1.4f, wet = roomWet, roomIdx = -1)
                    }
                    runCatching { com.ivanna.omega.ui.SpatialAudioPrefs.save(appContext, savedRoom.copy(rirRt60 = 0.1f + it * 1.4f)) }
                }
                OemSliderRow("Difusión", diffusion, 0f, 1f, "${"%.0f".format(diffusion*100)}%") {
                    diffusion = it
                    // v2.5.476 — Difusión cableada: orbe_diffuse [0,1] a
                    // Ruta A (nativo) + Ruta B (daemon, SET_PERCEPTUAL_STATE).
                    runCatching {
                        val ctx = appContext
                        val store = com.ivanna.omega.core.ParameterStore(ctx)
                        store.setOrbeParams(
                            store.getOrbeAmount(), store.getOrbeDistanceM(),
                            store.getOrbeSpread(), store.getOrbeProximityDb(),
                            it, store.isOrbeHeadTrackingEnabled())
                        com.ivanna.omega.magisk.OmegaEngineBridge.sendCommand(
                            org.json.JSONObject().apply {
                                put("action", "SET_PERCEPTUAL_STATE")
                                put("orbeDiffuse", it.toDouble())
                            })
                    }
                }
            }

            // SAF info card — explicar que NO es reverb simple
            Surface(
                shape = RoundedCornerShape(10.dp),
                border = BorderStroke(1.dp, PhosphorGreen.copy(0.3f)),
                color = PhosphorGreen.copy(0.05f), modifier = Modifier.fillMaxWidth()
            ) {
                Column(Modifier.padding(12.dp)) {
                    Row(horizontalArrangement = Arrangement.spacedBy(6.dp),
                        verticalAlignment = Alignment.CenterVertically) {
                        Icon(Icons.Default.Info, null, tint = PhosphorGreen, modifier = Modifier.size(14.dp))
                        Text("¿Qué es el SAF + RIR?", color = PhosphorGreen,
                            fontWeight = FontWeight.Bold, fontSize = 10.sp)
                    }
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "No es una reverberación sintética. El sistema convoluye el audio con " +
                        "respuestas de impulso grabadas en salas reales (RIR). Cada sala tiene su " +
                        "RT60, patrón de reflexiones tempranas y difusión propios. El cambio de sala " +
                        "usa crossfade de 32 bloques en el dominio de la frecuencia — sin corte audible.",
                        color = TextSecondary, fontSize = 9.sp, lineHeight = 14.sp
                    )
                }
            }
        }
    }
}

@Composable
private fun AcousticGauge(label: String, value: Float, unit: String, color: Color, modifier: Modifier) {
    Column(modifier, horizontalAlignment = Alignment.CenterHorizontally) {
        Text(label, color = TextMuted, fontSize = 7.sp, fontWeight = FontWeight.Bold)
        Spacer(Modifier.height(3.dp))
        Box(Modifier.fillMaxWidth().height(40.dp), contentAlignment = Alignment.BottomCenter) {
            Box(Modifier.fillMaxWidth().height(1.dp).background(ObsidianEdge))
            Box(Modifier.fillMaxWidth().fillMaxHeight(value.coerceIn(0f,1f))
                .background(color.copy(alpha = 0.3f)))
            Box(Modifier.fillMaxWidth().height(2.dp)
                .offset(y = (-(40 * value.coerceIn(0f,1f))).dp).background(color))
        }
        Spacer(Modifier.height(2.dp))
        Text(unit, color = color, fontSize = 7.sp, fontFamily = FontFamily.Monospace)
    }
}
