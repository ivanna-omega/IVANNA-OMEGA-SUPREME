package com.ivanna.omega.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.SwitchDefaults
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.audio.AntiDolbyTelemetry
import com.ivanna.omega.audio.FusedBrainMode
import com.ivanna.omega.audio.FusedControls
import com.ivanna.omega.audio.FusedControlsBridge
import com.ivanna.omega.audio.FusedHearingProfile
import com.ivanna.omega.audio.FusedPhiPreset
import com.ivanna.omega.audio.FusedSpatialMode
import com.ivanna.omega.audio.FusedTonalCurve
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.ui.theme.AmberSignal
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.ObsidianEdge
import com.ivanna.omega.ui.theme.ObsidianSoft
import com.ivanna.omega.ui.theme.ObsidianVoid
import com.ivanna.omega.ui.theme.PhosphorGreen
import com.ivanna.omega.ui.theme.TextMuted
import com.ivanna.omega.ui.theme.TextPrimary
import com.ivanna.omega.ui.theme.TextSecondary
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch

/**
 * FusedOperatorsScreen — cuadrante magistral de los 7 operadores fusionados
 * (FUSIÓN MAGISTRAL, 2026-11-18).
 *
 * Un cuadrante por operador, en cuadrícula de 2 columnas; NEURO·Ω a lo ancho
 * abajo. Cada cuadrante muestra SOLO los controles de
 * fused_controls.hpp (nada de los efectos fusionados, que quedan legacy):
 * sliders/toggles con valor actual y rango, y una mini-lectura de estado
 * (ACTIVO / NEUTRO, según isNeutral() del C++).
 *
 * Estilo copiado de EnginesStatusScreen (header, tarjetas Surface con borde
 * y expansibles) y de MagiskStatusPanel (botones de acción con borde).
 * Estado con remember + mutableStateOf (el repo no tiene un ViewModel para
 * estos controles). Cada cambio actualiza el FusedControls y empuja al
 * nativo con debounce de 120 ms — el repo ya usa debounce para empujar
 * parámetros (ParameterStore.saveParametersDebounced, LearningBias
 * PERSIST_DEBOUNCE_MS); sin él, arrastrar un slider dispararía un JNI por
 * cada frame. Los botones (Todo neutro / Restablecer) empujan de inmediato.
 *
 * El JNI fusionado aún no está implementado en C++: los envoltorios safe*
 * de FusedControlsBridge absorben el UnsatisfiedLinkError (log + no-op) y
 * la telemetría muestra "—" hasta que exista. Nada se inventa.
 */
@Composable
fun FusedOperatorsScreen(onBack: () -> Unit, onOpenHeadMeasure: () -> Unit = {}) {
    // 470: los controles FUSIÓN se persisten (ParameterStore, claves
    // "fused_*") y se cargan al abrir la pantalla; antes vivían solo en
    // memoria (remember) y un reinicio los revertía a los legacy.
    val context = LocalContext.current
    val fusedStore = remember { ParameterStore(context.applicationContext) }
    var controls by remember { mutableStateOf(fusedStore.loadFusedControls()) }
    var thdDb by remember { mutableStateOf(Float.NaN) }
    var antiDolby by remember { mutableStateOf(AntiDolbyTelemetry()) }
    // v2.5.473 §2.3: último barrido Genezi (prefs "genezi_sweep", las
    // escribe GeneziSweepRecorder; sin barrido, "Sin barrido registrado").
    val sweepSummary = remember {
        val sp = context.getSharedPreferences("genezi_sweep", android.content.Context.MODE_PRIVATE)
        val date = sp.getString("date", null)
        val files = sp.getString("files", null)
        val peak = sp.getFloat("peak_db", Float.NaN)
        if (date == null) "Sin barrido registrado"
        else "Último barrido: $date · ${files ?: "—"}" +
            if (peak.isFinite()) " · pico ${"%.1f".format(peak)} dBFS" else ""
    }
    // v2.5.473 §2.4: valores neuro del perfil activo, solo lectura (P1–P6).
    val neuroProfile = remember {
        val id = fusedStore.getCurrentAudioProfileId() ?: "steve_miller"
        runCatching { com.ivanna.omega.audio.ProfilesLoader.load(context).firstOrNull { it.id == id } }.getOrNull()
    }
    val scope = rememberCoroutineScope()
    var pushJob by remember { mutableStateOf<Job?>(null) }

    // Empuje al nativo con debounce: el arrastre de un slider no dispara un
    // JNI por frame; el último valor gana a los 120 ms.
    fun pushSoon(next: FusedControls) {
        pushJob?.cancel()
        pushJob = scope.launch {
            delay(120L)
            FusedControlsBridge.safeSetFusedControls(next)
            // 470: persistir con el mismo debounce que el empuje (el
            // último valor gana); así el reinicio no revierte a legacy.
            fusedStore.saveFusedControls(next)
        }
    }

    fun update(next: FusedControls) {
        controls = next.clamp()
        pushSoon(controls)
    }

    // ── Telemetría en vivo (poll 500 ms, como EnginesStatusScreen) ───────
    LaunchedEffect(Unit) {
        while (true) {
            thdDb = runCatching { FusedControlsBridge.safeGetFusedThdDb() }.getOrDefault(Float.NaN)
            antiDolby = runCatching { FusedControlsBridge.safeGetAntiDolbyTelemetry() }
                .getOrDefault(AntiDolbyTelemetry())
            delay(500L)
        }
    }

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(ObsidianVoid)
            .windowInsetsPadding(WindowInsets.systemBars)
    ) {
        // Header (patrón EnginesStatusScreen)
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(ObsidianSoft)
                .padding(horizontal = 16.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(10.dp)
        ) {
            IconButton(onClick = onBack, modifier = Modifier.size(36.dp)) {
                Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = "Atrás", tint = AuroraCyan)
            }
            Column(Modifier.weight(1f)) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                SpecInfoButtonSmall(spec = FusedControlSpecs.thd)
                    Text(
                        "OPERADORES FUSIONADOS", color = AuroraCyan,
                        fontWeight = FontWeight.ExtraBold, fontSize = 13.sp, letterSpacing = 1.5.sp
                    )
                    Text(
                        "Cuadrante magistral · 7 operadores · Fusión Magistral",
                        color = TextMuted, fontSize = 10.sp
                    )
                }
                Box(
                    Modifier.size(10.dp).clip(CircleShape)
                        .background(if (controls.isNeutral()) AmberSignal else PhosphorGreen)
                )
            }
        }
    
            Column(
                modifier = Modifier
                    .fillMaxSize()
                    .verticalScroll(rememberScrollState())
                    .padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                // THD+N de la cadena (telemetría; "—" sin lectura)
                Text(
                    text = "THD+N cadena: " +
                        if (thdDb.isFinite()) "${"%.1f".format(thdDb)} dB" else "— dB",
                    color = TextSecondary, fontSize = 11.sp, fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold
                )
            Text(
                text = if (controls.isNeutral()) "Cadena neutra · bit-transparente (ganancia 1.0 exacta)"
                    else "Cadena activa · Matched-LUFS (ganancia neta 0.0 dB)",
                color = TextMuted, fontSize = 10.sp, fontFamily = FontFamily.Monospace
            )

            // Acciones globales
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                FusedActionButton(
                    label = "TODO NEUTRO",
                    enabled = true,
                    modifier = Modifier.weight(1f)
                ) {
                    pushJob?.cancel()
                    val neutral = controls.setNeutral()
                    controls = neutral
                    FusedControlsBridge.safeSetFusedNeutral()
                    fusedStore.saveFusedControls(neutral)
                }
                FusedActionButton(
                    label = "RESTABLECER",
                    enabled = true,
                    modifier = Modifier.weight(1f)
                ) {
                    pushJob?.cancel()
                    controls = FusedControls()
                    FusedControlsBridge.safeSetFusedControls(controls)
                    fusedStore.saveFusedControls(controls)
                }
                SpecInfoButtonSmall(spec = FusedControlSpecs.neutralReset)
            }

            // ── Fila 1: ORBE·Ω + HOLOS·Ω ─────────────────────────────────
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OperatorCard(
                    title = "ORBE·Ω",
                    subtitle = "Espacial único",
                    neutral = controls.spatial.isNeutral(),
                    modifier = Modifier.weight(1f)
                ) {
                    val s = controls.spatial
                    FusedSliderRow(
                        label = "Cantidad", value = s.amount, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "ORBE 0..1 (0 = bypass)",
                        spec = FusedControlSpecs.orbeAmount,
                    ) { update(controls.copy(spatial = s.copy(amount = it))) }
                    FusedSliderRow(
                        label = "Azimut (°)", value = s.azimuthDeg, range = -180f..180f,
                        format = { "%.0f°".format(it) },
                        hint = "Azimut −180..180°",
                        spec = FusedControlSpecs.orbeAzimuth,
                    ) { update(controls.copy(spatial = s.copy(azimuthDeg = it))) }
                    FusedSliderRow(
                        label = "Distancia (m)", value = s.distanceM, range = 0.15f..2.0f,
                        format = { "%.2f m".format(it) },
                        hint = "Distancia 0.15..2.0 m",
                        spec = FusedControlSpecs.orbeDistance,
                    ) { update(controls.copy(spatial = s.copy(distanceM = it))) }
                    FusedSliderRow(
                        label = "Ancho", value = s.width, range = 0f..2f,
                        format = { "%.2f".format(it) },
                        hint = "Ancho 0..2 (1 = neutro)",
                        spec = FusedControlSpecs.orbeWidth,
                    ) { update(controls.copy(spatial = s.copy(width = it))) }
                    FusedOptionRow(
                        label = "Modo",
                        options = FusedSpatialMode.entries.map { it.label },
                        selected = s.mode.ordinal,
                        spec = FusedControlSpecs.outputMode,
                    ) {
                        update(controls.copy(spatial = s.copy(mode = FusedSpatialMode.entries[it])))
                    }
                    FusedSliderRow(
                        label = "Apertura WFS", value = s.wfsSpread, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Apertura WFS 0..1 (0 = apagado)",
                        spec = FusedControlSpecs.orbeWfs,
                    ) { update(controls.copy(spatial = s.copy(wfsSpread = it))) }
                    FusedSliderRow(
                        label = "Wet HRTF", value = s.hrtfWet, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Wet HRTF 0..1",
                        spec = FusedControlSpecs.orbeHrtfWet,
                    ) { update(controls.copy(spatial = s.copy(hrtfWet = it))) }
                    FusedSwitchRow(
                        label = "Seguimiento de cabeza (SO(3))", checked = s.headTracking,
                        spec = FusedControlSpecs.orbeHeadTracking,
                    ) { update(controls.copy(spatial = s.copy(headTracking = it))) }
                    FusedSwitchRow(
                        label = "Cancelación de diafonía (altavoces)", checked = s.xtcEnabled,
                        spec = FusedControlSpecs.orbeXtc,
                    ) { update(controls.copy(spatial = s.copy(xtcEnabled = it))) }
                    FusedSwitchRow(
                        label = "Upmixer HOA", checked = s.upmixEnabled,
                        spec = FusedControlSpecs.orbeUpmix,
                    ) { update(controls.copy(spatial = s.copy(upmixEnabled = it))) }
                    FusedSliderRow(
                        label = "Inmersividad upmix", value = s.upmixImmersivity, range = 0f..2f,
                        format = { "%.2f".format(it) },
                        hint = "Inmersividad 0..2",
                        spec = FusedControlSpecs.orbeUpmix,
                    ) { update(controls.copy(spatial = s.copy(upmixImmersivity = it))) }
                    FusedSliderRow(
                        label = "Parallax (head-tracking)", value = s.parallaxAmount, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Parallax 0..1 (1 = el tracker mueve el sonido)",
                        spec = FusedControlSpecs.orbeParallax,
                    ) { update(controls.copy(spatial = s.copy(parallaxAmount = it))) }
                    Text(
                        "Radio de cabeza: ${"%.4f".format(s.headRadiusM)} m (foto medida · default KEMAR 0.0875 m)",
                        color = TextMuted, fontSize = 9.sp, fontFamily = FontFamily.Monospace
                    )
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        SpecInfoButtonSmall(spec = FusedControlSpecs.orbeMeasureHead)
                        FusedActionButton(label = "MEDIR CABEZA", enabled = true, modifier = Modifier.weight(1f)) {
                        onOpenHeadMeasure()
                    }
                    }
                }
                OperatorCard(
                    title = "HOLOS·Ω",
                    subtitle = "Campo unificado",
                    neutral = controls.field.isNeutral(),
                    modifier = Modifier.weight(1f)
                ) {
                    val f = controls.field
                    FusedSliderRow(
                        label = "Cantidad", value = f.amount, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "HOLOS 0..1",
                        spec = FusedControlSpecs.holosAmount,
                    ) { update(controls.copy(field = f.copy(amount = it))) }
                    FusedSliderRow(
                        label = "Cantidad Φ", value = f.phiAmount, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Phi 0..1",
                        spec = FusedControlSpecs.phiAmount,
                    ) { update(controls.copy(field = f.copy(phiAmount = it))) }
                    FusedOptionRow(
                        label = "Preset Φ",
                        options = FusedPhiPreset.entries.map { it.label },
                        selected = f.phiPreset.ordinal,
                        spec = FusedControlSpecs.phiPreset,
                    ) {
                        update(controls.copy(field = f.copy(phiPreset = FusedPhiPreset.entries[it])))
                    }
                    FusedSliderRow(
                        label = "Bombeo (0 = cero)", value = f.pumping, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Bombeo 0..1 · objetivo 0.0",
                        spec = FusedControlSpecs.holosPumping,
                    ) { update(controls.copy(field = f.copy(pumping = it))) }
                    FusedSliderRow(
                        label = "Coherencia", value = f.coherence, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Coherencia 0..1",
                        spec = FusedControlSpecs.holosCoherence,
                    ) { update(controls.copy(field = f.copy(coherence = it))) }
                    FusedSliderRow(
                        label = "Armónicos", value = f.harmonics, range = 0f..0.002f,
                        format = { "%.4f".format(it) },
                        hint = "Armónicos 0..0.002",
                        spec = FusedControlSpecs.holosHarmonics,
                    ) { update(controls.copy(field = f.copy(harmonics = it))) }
                    FusedSliderRow(
                        label = "Campo Γ", value = f.gamma, range = 0f..2f,
                        format = { "%.2f".format(it) },
                        hint = "Campo Γ (Ley 1) 0..2",
                        spec = FusedControlSpecs.holosGamma,
                    ) { update(controls.copy(field = f.copy(gamma = it))) }
                    FusedSliderRow(
                        label = "Calidez", value = f.warmth, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Calidez 0..1",
                        spec = FusedControlSpecs.holosWarmth,
                    ) { update(controls.copy(field = f.copy(warmth = it))) }
                    FusedSliderRow(
                        label = "Dinámica", value = f.dynamics, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Dinámica 0..1 (1 = sin compresión extra)",
                        spec = FusedControlSpecs.holosDynamics,
                    ) { update(controls.copy(field = f.copy(dynamics = it))) }
                    FusedSliderRow(
                        label = "RT60 sala del campo (s)", value = f.roomRt60, range = 0.05f..1.5f,
                        format = { "%.2f s".format(it) },
                        hint = "RT60 0.05..1.5 s",
                        spec = FusedControlSpecs.holosRt60,
                    ) { update(controls.copy(field = f.copy(roomRt60 = it))) }
                }
            }

            // ── Fila 2: SALA VIVA + VOLTERRA·Ω ───────────────────────────
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OperatorCard(
                    title = "SALA VIVA",
                    subtitle = "Oyente + sala",
                    neutral = controls.listenerRoom.isNeutral(),
                    modifier = Modifier.weight(1f)
                ) {
                    val r = controls.listenerRoom
                    FusedSliderRow(
                        label = "RT60 (s)", value = r.rt60Sec, range = 0.05f..1.5f,
                        format = { "%.2f s".format(it) },
                        hint = "RT60 0.05..1.5 s",
                        spec = FusedControlSpecs.salaRoom,
                    ) { update(controls.copy(listenerRoom = r.copy(rt60Sec = it))) }
                    FusedSliderRow(
                        label = "Tamaño de sala (m)", value = r.roomSizeM, range = 2f..12f,
                        format = { "%.1f m".format(it) },
                        hint = "Tamaño 2..12 m",
                        spec = FusedControlSpecs.salaRoom,
                    ) { update(controls.copy(listenerRoom = r.copy(roomSizeM = it))) }
                    FusedSliderRow(
                        label = "Wet RIR", value = r.roomWet, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Wet RIR 0..1",
                        spec = FusedControlSpecs.salaRoom,
                    ) { update(controls.copy(listenerRoom = r.copy(roomWet = it))) }
                    FusedOptionRow(
                        label = "Perfil auditivo",
                        options = FusedHearingProfile.entries.map { it.label },
                        selected = r.hearingProfile.ordinal,
                        spec = FusedControlSpecs.salaHearing,
                    ) {
                        update(controls.copy(listenerRoom = r.copy(hearingProfile = FusedHearingProfile.entries[it])))
                    }
                    Text(
                        "Corrección de sala (10 bandas, solo corte −12..0 dB): sin control " +
                            "en esta pantalla — el JNI fusionado no tiene slot para ella.",
                        color = TextMuted, fontSize = 9.sp, fontFamily = FontFamily.Monospace
                    )
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        SpecInfoButtonSmall(spec = FusedControlSpecs.salaSweep)
                        Text(
                            sweepSummary,
                            color = TextSecondary, fontSize = 9.sp, fontFamily = FontFamily.Monospace
                        )
                    }
                }
                OperatorCard(
                    title = "VOLTERRA·Ω",
                    subtitle = "Corrector de transductor",
                    neutral = controls.transducer.isNeutral(),
                    modifier = Modifier.weight(1f)
                ) {
                    val t = controls.transducer
                    FusedSliderRow(
                        label = "Corrección", value = t.correction, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Corrección 0..1",
                        spec = FusedControlSpecs.volterraCorrection,
                    ) { update(controls.copy(transducer = t.copy(correction = it))) }
                    FusedSliderRow(
                        label = "Dinámica temporal", value = t.temporalAmount, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Dinámica espacial temporal 0..1",
                        spec = FusedControlSpecs.volterraTemporal,
                    ) { update(controls.copy(transducer = t.copy(temporalAmount = it))) }
                    FusedSwitchRow(
                        label = "Sensado Back-EMF", checked = t.emfEnabled,
                        spec = FusedControlSpecs.volterraEmf,
                    ) { update(controls.copy(transducer = t.copy(emfEnabled = it))) }
                }
            }

            // ── Fila 3: ARMONÍA + PULSO ──────────────────────────────────
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                OperatorCard(
                    title = "ARMONÍA",
                    subtitle = "Balance tonal único",
                    neutral = controls.tonal.isNeutral(),
                    modifier = Modifier.weight(1f)
                ) {
                    val t = controls.tonal
                    FusedOptionRow(
                        label = "Curva",
                        options = FusedTonalCurve.entries.map { it.label },
                        selected = t.curve.ordinal,
                        spec = FusedControlSpecs.tonalCurve,
                    ) {
                        update(controls.copy(tonal = t.copy(curve = FusedTonalCurve.entries[it])))
                    }
                    FusedSliderRow(
                        label = "Trim (dB)", value = t.trimDb, range = -6f..6f,
                        format = { "${"%+.1f".format(it)} dB" },
                        hint = "Trim −6..+6 dB (neto 0.0 dB)",
                        spec = FusedControlSpecs.tonalTrim,
                    ) { update(controls.copy(tonal = t.copy(trimDb = it))) }
                    FusedSliderRow(
                        label = "Graves (dB)", value = t.lowDb, range = CanonicalRanges.eqBandDb,
                        format = { "${"%+.1f".format(it)} dB" },
                        hint = "Graves −18..+18 dB",
                        spec = FusedControlSpecs.tonalEq,
                    ) { update(controls.copy(tonal = t.copy(lowDb = it))) }
                    FusedSliderRow(
                        label = "Medios (dB)", value = t.midDb, range = CanonicalRanges.eqBandDb,
                        format = { "${"%+.1f".format(it)} dB" },
                        hint = "Medios −18..+18 dB",
                        spec = FusedControlSpecs.tonalEq,
                    ) { update(controls.copy(tonal = t.copy(midDb = it))) }
                    FusedSliderRow(
                        label = "Agudos (dB)", value = t.highDb, range = CanonicalRanges.eqBandDb,
                        format = { "${"%+.1f".format(it)} dB" },
                        hint = "Agudos −18..+18 dB",
                        spec = FusedControlSpecs.tonalEq,
                    ) { update(controls.copy(tonal = t.copy(highDb = it))) }
                    FusedSliderRow(
                        label = "Presencia (dB)", value = t.presenceDb, range = CanonicalRanges.eqPresenceDb,
                        format = { "${"%+.1f".format(it)} dB" },
                        hint = "Presencia 8–12 kHz −12..+12 dB",
                        spec = FusedControlSpecs.tonalEq,
                    ) { update(controls.copy(tonal = t.copy(presenceDb = it))) }
                    FusedSwitchRow(
                        label = "Compensación de loudness (ISO 226)", checked = t.loudnessComp,
                        spec = FusedControlSpecs.tonalLoudness,
                    ) { update(controls.copy(tonal = t.copy(loudnessComp = it))) }
                }
                OperatorCard(
                    title = "PULSO",
                    subtitle = "Dinámica maestra",
                    neutral = controls.dynamics.isNeutral(),
                    modifier = Modifier.weight(1f)
                ) {
                    val d = controls.dynamics
                    FusedSliderRow(
                        label = "Pegada", value = d.punch, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Pegada 0..1",
                        spec = FusedControlSpecs.dynPunch,
                    ) { update(controls.copy(dynamics = d.copy(punch = it))) }
                    FusedSliderRow(
                        label = "Ancho M/S", value = d.width, range = 0f..2f,
                        format = { "%.2f".format(it) },
                        hint = "Ancho 0..2 (1 = neutro)",
                        spec = FusedControlSpecs.dynWidth,
                    ) { update(controls.copy(dynamics = d.copy(width = it))) }
                    FusedSliderRow(
                        label = "Seguridad (limitador)", value = d.safety, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Seguridad 0..1 (1 = activo)",
                        spec = FusedControlSpecs.dynSafety,
                    ) { update(controls.copy(dynamics = d.copy(safety = it))) }
                    FusedSliderRow(
                        label = "Umbral compresor (dB)", value = d.compThresholdDb, range = CanonicalRanges.compressorThresholdDb,
                        format = { "%.0f dB".format(it) },
                        hint = "Umbral −60..0 dB",
                        spec = FusedControlSpecs.dynComp,
                    ) { update(controls.copy(dynamics = d.copy(compThresholdDb = it))) }
                    FusedSliderRow(
                        label = "Ratio compresor", value = d.compRatio, range = CanonicalRanges.compressorRatio,
                        format = { "%.1f:1".format(it) },
                        hint = "Ratio 1..20 (1 = sin compresión)",
                        spec = FusedControlSpecs.dynComp,
                    ) { update(controls.copy(dynamics = d.copy(compRatio = it))) }
                    FusedSliderRow(
                        label = "Exciter", value = d.exciter, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Exciter 0..1",
                        spec = FusedControlSpecs.dynExciter,
                    ) { update(controls.copy(dynamics = d.copy(exciter = it))) }
                    FusedSliderRow(
                        label = "Voz / prosodia", value = d.voice, range = 0f..1f,
                        format = { "%.2f".format(it) },
                        hint = "Voz 0..1",
                        spec = FusedControlSpecs.dynVoice,
                    ) { update(controls.copy(dynamics = d.copy(voice = it))) }
                }
            }

            // ── Fila 4: NEURO·Ω a lo ancho ───────────────────────────────
            OperatorCard(
                title = "NEURO·Ω",
                subtitle = "Cerebro neural · observa y gobierna, no procesa la señal",
                neutral = controls.brain.isNeutral(),
                modifier = Modifier.fillMaxWidth()
            ) {
                val b = controls.brain
                FusedOptionRow(
                    label = "Modo",
                    options = FusedBrainMode.entries.map { it.label },
                    selected = b.mode.ordinal,
                    spec = FusedControlSpecs.brainMode,
                ) {
                    update(controls.copy(brain = b.copy(mode = FusedBrainMode.entries[it])))
                }
                FusedSliderRow(
                    label = "Adaptación", value = b.adaptation, range = 0f..1f,
                    format = { "%.2f".format(it) },
                    hint = "Adaptación 0..1 (en MANUAL se fuerza a 0)",
                    spec = FusedControlSpecs.brainAdaptation,
                ) { update(controls.copy(brain = b.copy(adaptation = it))) }
                HorizontalDivider(color = ObsidianEdge)
                Text(
                    "PERFIL NEURO (SOLO LECTURA — lo manda el perfil activo)",
                    color = AuroraCyan, fontWeight = FontWeight.Bold,
                    fontSize = 10.sp, letterSpacing = 0.8.sp
                )
                if (neuroProfile != null) {
                    val n = neuroProfile.neuromorphic
                    Text(
                        "P1 armónicos ${"%.2f".format(n.harmonicGain)} · P2 inhibición lateral ${"%.2f".format(n.lateralInhibition)}",
                        color = TextSecondary, fontSize = 10.sp, fontFamily = FontFamily.Monospace
                    )
                    Text(
                        "P3 compresión OHC ${"%.2f".format(n.ohcCompression)} · P4 ganancia máster ${"%+.1f".format(n.masterGainDb)} dB",
                        color = TextSecondary, fontSize = 10.sp, fontFamily = FontFamily.Monospace
                    )
                    Text(
                        "P5 ancho coclear ${n.cochlearBandwidth} · P6 modo cerebro ${if (neuroProfile.brain.mode == 0) "AUTO" else "MANUAL"}",
                        color = TextSecondary, fontSize = 10.sp, fontFamily = FontFamily.Monospace
                    )
                } else {
                    Text(
                        "Sin perfil activo.",
                        color = TextMuted, fontSize = 10.sp, fontFamily = FontFamily.Monospace
                    )
                }
                HorizontalDivider(color = ObsidianEdge)
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    SpecInfoButtonSmall(spec = FusedControlSpecs.antiDolby)
                    Text(
                        "GUARDIÁN ANTIDOLBY · TELEMETRÍA (SOLO LECTURA)",
                        color = AuroraCyan, fontWeight = FontWeight.Bold,
                        fontSize = 10.sp, letterSpacing = 0.8.sp
                    )
                }
                AntiDolbyRow("Widener", antiDolby.widener) { "%.2f".format(it) }
                AntiDolbyRow("Boost EQ (dB)", antiDolby.eqBoostDb) { "${"%+.1f".format(it)} dB" }
                AntiDolbyRow("Spread", antiDolby.spread) { "%.2f".format(it) }
                AntiDolbyRow("Intensidad", antiDolby.intensity) { "%.2f".format(it) }
                if (!antiDolby.isAvailable) {
                    Text(
                        "Sin lectura del guardián (nativo sin implementar o sin señal).",
                        color = TextMuted, fontSize = 9.sp, fontFamily = FontFamily.Monospace
                    )
                }
            }

            Spacer(Modifier.height(8.dp))
            }
    }
}

/** Tarjeta de un operador (patrón EngineCard de EnginesStatusScreen). */
@Composable
private fun OperatorCard(
    title: String,
    subtitle: String,
    neutral: Boolean,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    var expanded by remember { mutableStateOf(true) }
    Surface(
        onClick = { expanded = !expanded },
        modifier = modifier,
        color = ObsidianSoft,
        shape = RoundedCornerShape(10.dp),
        border = androidx.compose.foundation.BorderStroke(
            1.dp,
            (if (neutral) AmberSignal else PhosphorGreen).copy(alpha = 0.35f)
        )
    ) {
        Column(Modifier.padding(14.dp)) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(10.dp)
            ) {
                Box(
                    Modifier.size(8.dp).clip(CircleShape)
                        .background(if (neutral) AmberSignal else PhosphorGreen)
                )
                Column(Modifier.weight(1f)) {
                    Text(
                        title, color = TextPrimary, fontWeight = FontWeight.Bold,
                        fontSize = 12.sp, letterSpacing = 0.8.sp
                    )
                    Text(subtitle, color = TextMuted, fontSize = 9.sp)
                }
                Text(
                    if (neutral) "NEUTRO" else "ACTIVO",
                    color = if (neutral) AmberSignal else PhosphorGreen,
                    fontSize = 9.sp, fontFamily = FontFamily.Monospace,
                    fontWeight = FontWeight.Bold
                )
                Icon(
                    if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                    contentDescription = null, tint = TextMuted, modifier = Modifier.size(18.dp)
                )
            }
            if (expanded) {
                Spacer(Modifier.height(8.dp))
                HorizontalDivider(color = ObsidianEdge)
                Spacer(Modifier.height(8.dp))
                Column(verticalArrangement = Arrangement.spacedBy(10.dp)) {
                    content()
                }
            }
        }
    }
}

/** Fila slider: etiqueta + valor actual, slider con su rango, pista del rango. */
@Composable
private fun FusedSliderRow(
    label: String,
    value: Float,
    range: ClosedFloatingPointRange<Float>,
    format: (Float) -> String,
    hint: String,
    spec: SpecInfo? = null, // v2.5.473 §6: "i" junto al control
    onValueChange: (Float) -> Unit,
) {
    Column {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(label, color = TextPrimary, fontSize = 11.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            if (spec != null) SpecInfoButtonSmall(spec = spec)
            Text(
                format(value), color = AuroraCyan, fontSize = 11.sp,
                fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold
            )
        }
        // SAFMUS — wrapper del repo (SafmusComposeSlider.kt, MISMO paquete,
        // se resuelve sin importar): sanea y clampa el valor. NO es el
        // Slider de Material3.
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = range,
            modifier = Modifier.fillMaxWidth(),
            colors = androidx.compose.material3.SliderDefaults.colors(
                thumbColor = AuroraCyan,
                activeTrackColor = AuroraCyan,
                inactiveTrackColor = ObsidianEdge
            )
        )
        Text(hint, color = TextMuted, fontSize = 9.sp, fontFamily = FontFamily.Monospace)
    }
}

/** Fila toggle: etiqueta + Switch. */
@Composable
private fun FusedSwitchRow(
    label: String,
    checked: Boolean,
    spec: SpecInfo? = null, // v2.5.473 §6
    onCheckedChange: (Boolean) -> Unit,
) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            label, color = TextPrimary, fontSize = 11.sp,
            fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f)
        )
        if (spec != null) SpecInfoButtonSmall(spec = spec)
        Switch(
            checked = checked,
            onCheckedChange = onCheckedChange,
            colors = SwitchDefaults.colors(
                checkedThumbColor = ObsidianVoid,
                checkedTrackColor = AuroraCyan,
                uncheckedThumbColor = TextMuted,
                uncheckedTrackColor = ObsidianEdge
            )
        )
    }
}

/** Fila de opciones excluyentes (modo, curva, perfil, preset, modo cerebro). */
@Composable
private fun FusedOptionRow(
    label: String,
    options: List<String>,
    selected: Int,
    spec: SpecInfo? = null, // v2.5.473 §6
    onSelected: (Int) -> Unit,
) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(6.dp)) {
            Text(label, color = TextPrimary, fontSize = 11.sp, fontWeight = FontWeight.Bold, modifier = Modifier.weight(1f))
            if (spec != null) SpecInfoButtonSmall(spec = spec)
        }
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            options.forEachIndexed { index, option ->
                val isSelected = index == selected
                Box(
                    modifier = Modifier
                        .weight(1f)
                        .clip(RoundedCornerShape(8.dp))
                        .background(
                            if (isSelected) AuroraCyan.copy(alpha = 0.18f)
                            else ObsidianEdge.copy(alpha = 0.4f)
                        )
                        .border(
                            1.dp,
                            if (isSelected) AuroraCyan else ObsidianEdge,
                            RoundedCornerShape(8.dp)
                        )
                        .clickable { onSelected(index) }
                        .padding(horizontal = 6.dp, vertical = 8.dp),
                    contentAlignment = Alignment.Center
                ) {
                    Text(
                        text = option,
                        color = if (isSelected) AuroraCyan else TextMuted,
                        fontFamily = FontFamily.Monospace,
                        fontSize = 10.sp,
                        fontWeight = FontWeight.Bold,
                        maxLines = 1
                    )
                }
            }
        }
    }
}

/** Botón de acción (patrón ActionButton de MagiskStatusPanel). */
@Composable
private fun FusedActionButton(
    label: String,
    enabled: Boolean,
    modifier: Modifier = Modifier,
    onClick: () -> Unit,
) {
    val containerColor = if (enabled) AuroraCyan.copy(alpha = 0.18f) else ObsidianEdge.copy(alpha = 0.4f)
    val borderColor = if (enabled) AuroraCyan else ObsidianEdge
    val textColor = if (enabled) AuroraCyan else TextMuted
    Box(
        modifier = modifier
            .clip(RoundedCornerShape(8.dp))
            .background(containerColor)
            .border(1.dp, borderColor, RoundedCornerShape(8.dp))
            .clickable(enabled = enabled, onClick = onClick)
            .padding(horizontal = 12.dp, vertical = 10.dp),
        contentAlignment = Alignment.Center
    ) {
        Text(
            text = label,
            color = textColor,
            fontFamily = FontFamily.Monospace,
            fontSize = 11.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1
        )
    }
}

/** Una lectura del guardián AntiDolby; "—" sin lectura (nunca se inventa). */
@Composable
private fun AntiDolbyRow(label: String, value: Float, format: (Float) -> String) {
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(label, color = TextSecondary, fontSize = 11.sp, fontFamily = FontFamily.Monospace)
        Text(
            if (value.isFinite()) format(value) else "—",
            color = if (value.isFinite()) TextPrimary else TextMuted,
            fontSize = 11.sp, fontFamily = FontFamily.Monospace, fontWeight = FontWeight.Bold
        )
    }
}
