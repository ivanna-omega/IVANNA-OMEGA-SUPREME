package com.ivanna.omega.ui

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.slideInVertically
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.audio.FusedControlsBridge
import com.ivanna.omega.audio.IvannaBridgePlayer
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.magisk.OmegaEngineBridge
import com.ivanna.omega.ui.control.NewPhysicsCardsColumn
import com.ivanna.omega.ui.theme.AmberSignal
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.CoralWarn
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
 * Pantallas nuevas magistrales — Fase 2 de la refactorización visual
 * (orden de Esse: "todo se refactoriza y se crean las nuevas animadas").
 *
 * Estructura (§3 Fase 4 del inventario): Escucha magistral, Física Nueva
 * por secciones, Mi sala, Mi equipo, Salud del sistema y Aprende. La
 * FUSIÓN por operadores ya existe (FusedOperatorsScreen: 7 tarjetas con su
 * ficha visible y su A/B por operador); aquí solo se le da acceso desde
 * sus botones. Las animaciones son del sistema de animación de Compose
 * (animate*AsState, AnimatedVisibility, infiniteTransition): el orbe
 * respira, el campo se abre con ORBE, las tarjetas entran con transición
 * escalonada. Los prototipos raea (~/workspace/raea-workspace/ivanna-ui)
 * son la referencia visual; ningún código raea se copia aquí.
 *
 * Nada de este archivo toca el motor de audio: solo lee y escribe el mismo
 * estado fusionado guardado que FusedOperatorsScreen (ParameterStore +
 * FusedControlsBridge), con el mismo debounce de 120 ms.
 */

// ═══════════════════════════════════════════════════════════════════════════
//  Componentes animados compartidos
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Orbe vivo: respira siempre (ciclo 2.4 s) y su campo se abre con la
 * cantidad de ORBE; el brillo interior sigue a HOLOS. Es el mismo orbe
 * del prototipo raea de la pantalla principal.
 */
@Composable
fun MagistralOrbe(
    orbeAmount: Float,
    holosAmount: Float,
    modifier: Modifier = Modifier,
) {
    val infinite = rememberInfiniteTransition(label = "orbe-respira")
    val breath by infinite.animateFloat(
        initialValue = 0.96f, targetValue = 1.05f,
        animationSpec = infiniteRepeatable(
            animation = tween(2400, easing = FastOutSlowInEasing),
            repeatMode = RepeatMode.Reverse
        ),
        label = "respiracion"
    )
    val targetRadius = 0.55f + 0.35f * orbeAmount.coerceIn(0f, 1f)
    val radius by animateFloatAsState(targetValue = targetRadius, animationSpec = tween(600), label = "campo")
    val targetGlow = 0.25f + 0.55f * holosAmount.coerceIn(0f, 1f)
    val glow by animateFloatAsState(targetValue = targetGlow, animationSpec = tween(600), label = "brillo")
    val ringColor by animateColorAsState(
        targetValue = if (orbeAmount > 0f) AuroraCyan else TextMuted,
        animationSpec = tween(600), label = "color-orbe"
    )
    Canvas(modifier.size(220.dp)) {
        val r = size.minDimension / 2f * radius * breath
        drawCircle(color = ringColor.copy(alpha = glow * 0.22f), radius = r * 1.28f)
        drawCircle(color = ringColor.copy(alpha = glow), radius = r, style = Stroke(width = 2.5f))
        drawCircle(color = ringColor.copy(alpha = glow * 0.45f), radius = r * 0.72f, style = Stroke(width = 1.5f))
        drawCircle(color = PhosphorGreen.copy(alpha = 0.85f), radius = r * 0.42f * breath)
        drawCircle(color = PhosphorGreen.copy(alpha = 0.35f), radius = r * 0.20f)
    }
}

/** Tarjeta con entrada animada escalonada (fade + deslizamiento). */
@Composable
fun MagistralAnimatedCard(
    visible: Boolean,
    index: Int,
    modifier: Modifier = Modifier,
    content: @Composable ColumnScope.() -> Unit,
) {
    AnimatedVisibility(
        visible = visible,
        enter = fadeIn(animationSpec = tween(350, delayMillis = index * 90)) +
            slideInVertically(animationSpec = tween(350, delayMillis = index * 90)) { it / 3 },
        modifier = modifier,
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .background(ObsidianSoft)
                .border(1.dp, ObsidianEdge, RoundedCornerShape(12.dp))
                .padding(14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            content = content
        )
    }
}

/** Cabecera de sección dentro de una pantalla magistral. */
@Composable
private fun MagistralSectionTitle(text: String) {
    Text(text, color = TextMuted, fontSize = 10.sp, fontWeight = FontWeight.Bold, letterSpacing = 1.sp)
}

/**
 * Andamiaje de pantalla magistral: cabecera con atrás + título, contenido
 * con scroll y entrada animada. `content` recibe el `visible` de entrada
 * para escalonar sus tarjetas con [MagistralAnimatedCard].
 */
@Composable
fun MagistralScreenScaffold(
    title: String,
    subtitle: String,
    onBack: () -> Unit,
    content: @Composable ColumnScope.(visible: Boolean) -> Unit,
) {
    var visible by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) { visible = true }
    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(ObsidianVoid)
            .windowInsetsPadding(WindowInsets.systemBars)
    ) {
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
                Text(title, color = AuroraCyan, fontWeight = FontWeight.ExtraBold, fontSize = 13.sp, letterSpacing = 1.5.sp)
                Text(subtitle, color = TextMuted, fontSize = 10.sp)
            }
        }
        Column(
            modifier = Modifier
                // weight(1f), no fillMaxSize(): el viewport es justo lo que
                // queda tras la cabecera; con fillMaxSize() el viewport se
                // extendía una cabecera por debajo de la pantalla y los
                // últimos ~64 dp de contenido no se alcanzaban nunca.
                .weight(1f)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            content(visible)
        }
    }
}

private fun formatMmSs(ms: Long): String {
    if (ms <= 0L) return "0:00"
    val s = ms / 1000L
    return "${s / 60}:${"%02d".format(s % 60)}"
}

// ═══════════════════════════════════════════════════════════════════════════
//  1 · ESCUCHA MAGISTRAL — pantalla principal rediseñada
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Escucha magistral: orbe vivo + archivo Ivanna aplicado + 4 sliders
 * maestros (ORBE, HOLOS, Φ, Volterra). Lee y escribe el mismo estado
 * fusionado guardado que FUSIÓN (un solo estado): mover un maestro aquí
 * se ve igual en FUSIÓN y viceversa.
 */
@Composable
fun EscuchaMagistralScreen(
    player: IvannaBridgePlayer,
    onBack: () -> Unit,
    onOpenMusic: () -> Unit,
    onOpenFusion: () -> Unit,
) {
    val context = LocalContext.current
    val fusedStore = remember { ParameterStore(context.applicationContext) }
    var controls by remember { mutableStateOf(fusedStore.loadFusedControls()) }
    val scope = rememberCoroutineScope()
    var pushJob by remember { mutableStateOf<Job?>(null) }
    fun pushSoon(next: com.ivanna.omega.audio.FusedControls) {
        pushJob?.cancel()
        pushJob = scope.launch {
            delay(120L)
            FusedControlsBridge.safeSetFusedControls(next)
            fusedStore.saveFusedControls(next)
        }
    }
    fun update(next: com.ivanna.omega.audio.FusedControls) {
        controls = next.clamp()
        pushSoon(controls)
    }
    val omegaLabel = remember { fusedStore.getOmegaFileLabel() ?: "" }
    val profileId = remember { fusedStore.getCurrentAudioProfileId() ?: "rock_70s" }
    val posMs by player.currentPositionMs.collectAsState()
    val durMs by player.durationMs.collectAsState()
    var playing by remember { mutableStateOf(false) }
    LaunchedEffect(Unit) {
        while (true) {
            playing = player.state == IvannaBridgePlayer.State.PLAYING
            delay(1000L)
        }
    }

    MagistralScreenScaffold(
        title = "ESCUCHA MAGISTRAL",
        subtitle = "Tu música, con el campo IVANNA",
        onBack = onBack,
    ) { visible ->
        MagistralAnimatedCard(visible = visible, index = 0) {
            Box(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                MagistralOrbe(
                    orbeAmount = controls.spatial.amount,
                    holosAmount = controls.field.amount,
                )
            }
            Text(
                text = if (playing) "● SONANDO" else "○ EN PAUSA",
                color = if (playing) PhosphorGreen else TextMuted,
                fontSize = 11.sp, fontWeight = FontWeight.Bold,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )
            if (durMs > 0L) {
                Text(
                    text = "${formatMmSs(posMs)} / ${formatMmSs(durMs)}",
                    color = TextSecondary, fontSize = 11.sp,
                    modifier = Modifier.align(Alignment.CenterHorizontally)
                )
            }
            Text(
                text = if (omegaLabel.isNotEmpty()) "Archivo IVANNA: $omegaLabel" else "Sin archivo IVANNA aplicado",
                color = TextSecondary, fontSize = 11.sp,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )
            Text(
                text = "Perfil: $profileId",
                color = TextMuted, fontSize = 11.sp,
                modifier = Modifier.align(Alignment.CenterHorizontally)
            )
        }
        MagistralAnimatedCard(visible = visible, index = 1) {
            MagistralSectionTitle("4 MAESTROS")
            AuroraSlider(
                "ORBE · Ω", controls.spatial.amount, 0f..1f,
                spec = FusedControlSpecs.orbeAmount, showSpecInline = true,
                displayValue = { "%.2f".format(it) }
            ) { update(controls.copy(spatial = controls.spatial.copy(amount = it))) }
            AuroraSlider(
                "HOLOS · Ω", controls.field.amount, 0f..1f,
                spec = FusedControlSpecs.holosAmount, showSpecInline = true,
                displayValue = { "%.2f".format(it) }
            ) { update(controls.copy(field = controls.field.copy(amount = it))) }
            AuroraSlider(
                "Φ · HIPER-OPERADOR", controls.field.phiAmount, 0f..1f,
                spec = FusedControlSpecs.phiAmount, showSpecInline = true,
                displayValue = { "%.2f".format(it) }
            ) { update(controls.copy(field = controls.field.copy(phiAmount = it))) }
            AuroraSlider(
                "VOLTERRA · Ω", controls.transducer.correction, 0f..1f,
                spec = FusedControlSpecs.volterraCorrection, showSpecInline = true,
                displayValue = { "%.2f".format(it) }
            ) { update(controls.copy(transducer = controls.transducer.copy(correction = it))) }
        }
        MagistralAnimatedCard(visible = visible, index = 2) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Button(
                    onClick = onOpenMusic, modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan)
                ) { Text("ABRIR MÚSICA", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = ObsidianVoid) }
                OutlinedButton(onClick = onOpenFusion, modifier = Modifier.weight(1f)) {
                    Text("ABRIR FUSIÓN", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = AuroraCyan)
                }
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  2 · FÍSICA NUEVA POR SECCIONES
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Física Nueva por secciones: las dos tarjetas ya existentes
 * (NewPhysicsCardsColumn, antes sin anfitrión: nadie las montaba), con la
 * advertencia Ruta A / Ruta B SIEMPRE visible arriba — no se esconde.
 */
@Composable
fun FisicaNuevaScreen(onBack: () -> Unit) {
    MagistralScreenScaffold(
        title = "FÍSICA NUEVA",
        subtitle = "Ω · Σ · LGN · GENEZI por secciones",
        onBack = onBack,
    ) { visible ->
        MagistralAnimatedCard(visible = visible, index = 0) {
            Text(
                "⚠ RUTA A / RUTA B — LÉELO SIEMPRE",
                color = AmberSignal, fontSize = 11.sp, fontWeight = FontWeight.Bold
            )
            Text(
                "Estos sliders mueven solo la Ruta A en vivo (música dentro de " +
                    "IVANNA). En Ruta B (Tidal, Qobuz y demás apps) suenan los " +
                    "valores por defecto hasta la extensión ABI del snapshot. Si " +
                    "un ajuste no se oye fuera de la app, es por esto: el control " +
                    "no está roto.",
                color = TextSecondary, fontSize = 11.sp, lineHeight = 16.sp
            )
        }
        // Las tarjetas se agrupan por secciones dentro de NewPhysicsCardsColumn
        // (maestro/equipo · LGN/pinna · Σ/distancia · Genezi/Volterra · v3).
        // Cada slider ya explica para qué sirve debajo del control (v2.5.484).
        NewPhysicsCardsColumn()
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  3 · MI SALA — medición + corrección + barrido + RIR en un solo flujo
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Mi sala: el calibrador ISO 226 (antes sin anfitrión: solo SoundScreen lo
 * usaba) + el estado de la corrección de sala + el último barrido Genezi +
 * el camino a RIR/SAF y al barrido.
 */
@Composable
fun MiSalaScreen(
    onBack: () -> Unit,
    onOpenRirSaf: () -> Unit,
    onOpenControl: () -> Unit,
) {
    val context = LocalContext.current
    val paramStore = remember { ParameterStore(context.applicationContext) }
    val roomEnabled = remember { paramStore.isRoomCorrectionEnabled() }
    val roomGains = remember { paramStore.getRoomCorrectionGains() }
    val sweepSummary = remember {
        val sp = context.getSharedPreferences("genezi_sweep", android.content.Context.MODE_PRIVATE)
        val date = sp.getString("date", null)
        val files = sp.getString("files", null)
        val peak = sp.getFloat("peak_db", Float.NaN)
        if (date == null) "Sin barrido registrado"
        else "Último barrido: $date · ${files ?: "—"}" +
            if (peak.isFinite()) " · pico ${"%.1f".format(peak)} dBFS" else ""
    }
    MagistralScreenScaffold(
        title = "MI SALA",
        subtitle = "Medición · corrección · barrido · RIR",
        onBack = onBack,
    ) { visible ->
        MagistralAnimatedCard(visible = visible, index = 0) {
            MagistralSectionTitle("CORRECCIÓN DE SALA (CUT-ONLY)")
            Text(
                text = if (roomEnabled && roomGains != null)
                    "ACTIVA · ${roomGains.size} bandas medidas · solo corta, nunca refuerza"
                else "Apagada: sin medición válida o apagada a mano. Sin medición no hay nada que corregir.",
                color = if (roomEnabled && roomGains != null) PhosphorGreen else TextSecondary,
                fontSize = 11.sp, lineHeight = 16.sp
            )
            Text(
                text = sweepSummary,
                color = TextSecondary, fontSize = 11.sp
            )
        }
        MagistralAnimatedCard(visible = visible, index = 1) {
            Iso226CalibratorPanel()
        }
        MagistralAnimatedCard(visible = visible, index = 2) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Button(
                    onClick = onOpenRirSaf, modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan)
                ) { Text("RIR · SAF · SOFA", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = ObsidianVoid) }
                OutlinedButton(onClick = onOpenControl, modifier = Modifier.weight(1f)) {
                    Text("GRABAR BARRIDO", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = AuroraCyan)
                }
            }
            Text(
                "El barrido Genezi se graba desde CONTROL (Sala): reproduce los " +
                    "dos barridos ESS al Genezi y guarda la grabación en Download.",
                color = TextMuted, fontSize = 10.sp, lineHeight = 14.sp
            )
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  4 · MI EQUIPO — salida + bocinas + perfil Genezi + Hi-Res
// ═══════════════════════════════════════════════════════════════════════════

/** Mi equipo: bocinas declaradas + perfil Genezi + camino a Hi-Res. */
@Composable
fun MiEquipoScreen(
    onBack: () -> Unit,
    onOpenHiRes: () -> Unit,
    onOpenControl: () -> Unit,
) {
    MagistralScreenScaffold(
        title = "MI EQUIPO",
        subtitle = "Salida · bocinas · Genezi · Hi-Res",
        onBack = onBack,
    ) { visible ->
        MagistralAnimatedCard(visible = visible, index = 0) {
            SpeakerLayoutPanel()
        }
        MagistralAnimatedCard(visible = visible, index = 1) {
            GenezyMhcpz1ProfileCard()
        }
        MagistralAnimatedCard(visible = visible, index = 2) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Button(
                    onClick = onOpenHiRes, modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan)
                ) { Text("AUDIO HI-RES", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = ObsidianVoid) }
                OutlinedButton(onClick = onOpenControl, modifier = Modifier.weight(1f)) {
                    Text("CAMBIAR SALIDA", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = AuroraCyan)
                }
            }
            Text(
                "La salida (audífonos / Genezi) se cambia en CONTROL: la cadena " +
                    "adapta lo binaural y lo general según lo que suena.",
                color = TextMuted, fontSize = 10.sp, lineHeight = 14.sp
            )
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  5 · SALUD DEL SISTEMA — Magisk/Ruta B + térmica + latencia/clips
// ═══════════════════════════════════════════════════════════════════════════

/**
 * Salud del sistema: estado Magisk/Ruta B + rendimiento en vivo
 * (NeonProfiler) + camino a telemetría y térmico OEM.
 */
@Composable
fun SaludSistemaScreen(
    omegaBridge: OmegaEngineBridge,
    onBack: () -> Unit,
    onOpenTelemetry: () -> Unit,
    onOpenThermal: () -> Unit,
) {
    MagistralScreenScaffold(
        title = "SALUD DEL SISTEMA",
        subtitle = "Magisk · Ruta B · térmica · latencia",
        onBack = onBack,
    ) { visible ->
        MagistralAnimatedCard(visible = visible, index = 0) {
            // FIX (crash 497): sin scroll propio — el panel es contenido del
            // scroll del scaffold; con su scroll interno se medía con altura
            // infinita y Compose lanzaba IllegalStateException.
            MagiskStatusPanel(omegaBridge = omegaBridge, onBack = onBack, scrollable = false)
        }
        MagistralAnimatedCard(visible = visible, index = 1) {
            MagistralSectionTitle("RENDIMIENTO EN VIVO")
            NeonProfilerPanel()
        }
        MagistralAnimatedCard(visible = visible, index = 2) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Button(
                    onClick = onOpenTelemetry, modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan)
                ) { Text("TELEMETRÍA OEM", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = ObsidianVoid) }
                OutlinedButton(onClick = onOpenThermal, modifier = Modifier.weight(1f)) {
                    Text("CONTROL TÉRMICO", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = AuroraCyan)
                }
            }
            Text(
                "Si algo falla, pulsa EXPORTAR DIAGNÓSTICO en Telemetría OEM y " +
                    "comparte el archivo: trae motor, pipeline, térmico y latencia.",
                color = TextMuted, fontSize = 10.sp, lineHeight = 14.sp
            )
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════════
//  6 · APRENDE — la guía en español, navegable por control
// ═══════════════════════════════════════════════════════════════════════════

/** Glosario de siglas en palabras simples (inventario Apéndice B). */
private val glosarioMagistral: List<Pair<String, String>> = listOf(
    "HRTF" to "Cómo tu cabeza y tus orejas cambian el sonido según de dónde viene. Sirve para poner sonidos en 3D con audífonos.",
    "SOFA" to "El formato de archivo que guarda mediciones de HRTF de cabezas reales.",
    "RIR" to "La «foto acústica» de una sala: cómo responde la sala a un chasquido. Con ella se recrea esa sala.",
    "RT60" to "Cuánto tarda en apagarse el sonido en una sala (en segundos). 0.34 s = sala viva pero controlada.",
    "SAF" to "El sistema que aprende de tu sala y ajusta el sonido a ella.",
    "ITD / ILD" to "La diferencia de tiempo / de volumen entre lo que oye tu oído izquierdo y el derecho. Así sabes de dónde viene un sonido.",
    "XTC" to "Cancelar que el sonido de la bocina izquierda llegue a tu oído derecho (y viceversa). Solo tiene sentido con bocinas.",
    "ABX" to "Prueba ciega: escuchas A y B sin saber cuál es cuál y dices si notas diferencia. Quita el sesgo.",
    "LUFS" to "La medida de qué tan fuerte se percibe la música (no el pico, lo que el oído siente).",
    "THD+N" to "Cuánta distorsión y ruido añade la cadena. Menos es más limpio.",
    "DSP" to "El procesamiento digital: todo lo que IVANNA le hace al sonido antes de que suene.",
    "DAC" to "El aparato que convierte el sonido digital en sonido analógico para las bocinas o audífonos.",
    "HAL" to "La capa de Android que habla con el hardware de audio del teléfono.",
    "ASRC" to "El corrector que mantiene el reloj del audio sincronizado cuando el DAC y el teléfono no van exactamente igual.",
    "RMS / Peak" to "RMS = el nivel promedio (lo que se siente fuerte); Peak = el pico más alto (lo que puede saturar).",
    "GR" to "Cuánto está reduciendo el compresor en ese momento (en dB).",
    "FFT / Bark" to "FFT = descomponer el sonido en frecuencias; Bark = agruparlas como las agrupa el oído humano (64 bandas).",
    "HOA" to "Audio de orden superior: repartir el estéreo en un campo espacial más rico.",
    "WFS" to "Síntesis de frente de onda: crear un frente de sonido con muchos altavoces virtuales. Necesita altavoces reales.",
    "CMA-ES" to "Un método evolutivo que prueba muchas soluciones y se queda con la mejor (para el EQ evolutivo).",
    "NPE / Φ" to "NPE = el motor neuronal que escucha con el micrófono; Φ = el Hiper-Operador de Estado, el bucle cerrado que afina el sonido muestra a muestra.",
    "ORBE / HOLOS" to "ORBE = el campo cercano esférico que abre la escena en 3D; HOLOS = el campo acústico unificado que da cuerpo y coherencia de sala.",
)

/**
 * Aprende: la guía completa en español (las 87 entradas del catálogo, ya
 * visibles al entrar) + el glosario de siglas + atajos a FUSIÓN y Física
 * Nueva. Regla de oro, repetida en todas las fichas: cambia UNA cosa por
 * vez, compara con el bypass al mismo volumen percibido, y si empeora,
 * vuelve al valor anterior.
 */
@Composable
fun AprendeScreen(
    onBack: () -> Unit,
    onOpenFusion: () -> Unit,
    onOpenFisicaNueva: () -> Unit,
) {
    MagistralScreenScaffold(
        title = "APRENDE",
        subtitle = "La guía en español · qué es cada control",
        onBack = onBack,
    ) { visible ->
        MagistralAnimatedCard(visible = visible, index = 0) {
            Text(
                "REGLA DE ORO",
                color = AmberSignal, fontSize = 11.sp, fontWeight = FontWeight.Bold
            )
            Text(
                "Cambia UNA cosa por vez, compara con el bypass al mismo volumen " +
                    "percibido, y si empeora, vuelve al valor anterior. Un guion «—» " +
                    "o «sin dato» significa que no hay lectura válida, no cero.",
                color = TextSecondary, fontSize = 11.sp, lineHeight = 16.sp
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Button(
                    onClick = onOpenFusion, modifier = Modifier.weight(1f),
                    colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan)
                ) { Text("IR A FUSIÓN", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = ObsidianVoid) }
                OutlinedButton(onClick = onOpenFisicaNueva, modifier = Modifier.weight(1f)) {
                    Text("FÍSICA NUEVA", fontSize = 11.sp, fontWeight = FontWeight.Bold, color = AuroraCyan)
                }
            }
        }
        MagistralAnimatedCard(visible = visible, index = 1) {
            MagistralSectionTitle("GLOSARIO · SIGLAS EN PALABRAS SIMPLES")
            glosarioMagistral.forEach { (sigla, texto) ->
                Column(verticalArrangement = Arrangement.spacedBy(2.dp)) {
                    Text(sigla, color = AuroraCyan, fontSize = 11.sp, fontWeight = FontWeight.Bold)
                    Text(texto, color = TextSecondary, fontSize = 11.sp, lineHeight = 15.sp)
                }
                Spacer(Modifier.height(6.dp))
            }
        }
        MagistralAnimatedCard(visible = visible, index = 2) {
            // La guía completa (87 entradas): abierta al entrar, no detrás
            // de un botón. Cada entrada dice qué hace y cómo usarlo.
            ControlHelpCatalogPanel(startExpanded = true)
        }
    }
}
