package com.ivanna.omega.ui
import androidx.compose.material.icons.automirrored.filled.KeyboardArrowRight

import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.livedata.observeAsState
import androidx.compose.ui.Alignment
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.PasswordVisualTransformation
import com.ivanna.omega.audio.IvannaBridgePlayer
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.NavHostController
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController
import com.ivanna.omega.audio.AdaptiveBackend
import com.ivanna.omega.audio.AdaptiveMode
import com.ivanna.omega.audio.OmegaMetrics
import com.ivanna.omega.audio.PipelineState
import com.ivanna.omega.audio.VoiceProtectionManager
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.dsp.DSPState
import com.ivanna.omega.ui.theme.*

// ── Tab descriptors ──────────────────────────────────────────────────────────
private data class NavTab(val route: String, val label: String, val icon: androidx.compose.ui.graphics.vector.ImageVector)

private val TABS = listOf(
    // v2.5.473 (§1.1): 4 tabs, 4+2 superficies. BRAIN/ADAPTIVE/SPATIAL eran
    // hubs de telemetría y duplicados de FUSIÓN; sus contenidos viven ahora
    // en FUSIÓN (operadores) o detrás del Modo desarrollador (diagnóstico).
    NavTab("tab_control", "CONTROL", Icons.Filled.Tune),
    NavTab("tab_music",   "MÚSICA",  Icons.Filled.MusicNote),
    NavTab("tab_fused",   "FUSIÓN",  Icons.Filled.Hub),
    NavTab("tab_system",  "SYSTEM",  Icons.Filled.Settings)
)

/**
 * MainScaffold — BottomNavigation de 4 tabs (v2.5.473: CONTROL, MÚSICA, FUSIÓN, SYSTEM).
 * Reemplaza DashboardScreen() en el NavHost de MainActivity.
 * outerNav → push de sub-pantallas (magisk, profiles, hires, etc.)
 */
@Composable
fun MainScaffold(
    outerNav   : NavHostController,
    dsp        : MutableState<DSPState>,
    adaptiveBack : AdaptiveBackend,
    voiceMgr   : VoiceProtectionManager,
    metrics    : OmegaMetrics      = OmegaMetrics(),
    adaptiveMode : AdaptiveMode    = AdaptiveMode.NATURAL,
    onAdaptiveModeChange : (AdaptiveMode) -> Unit = {},
    adaptiveIntensity    : Float   = 50f,
    onAdaptiveIntensityChange : (Float) -> Unit = {},
    routeState : PipelineState     = PipelineState(),
    // MÚSICA LOCAL (v2.5.447): el reproductor vive en el ámbito de la
    // actividad (MainActivity) y se pasa aquí para la pestaña MÚSICA.
    // (TIDAL, v2.5.416–v2.5.446, quedó legacy: ya no se pasa su cliente.)
    player     : IvannaBridgePlayer,
    captureActive : Boolean = false,
    onToggleCapture : () -> Unit = {},
    onPausePlayback : () -> Unit = {},
) {
    val tabNav  = rememberNavController()
    val entry   by tabNav.currentBackStackEntryAsState()
    val current  = entry?.destination?.route

    Scaffold(
        containerColor = ObsidianVoid,
        bottomBar = {
            // Dock flotante de cristal: separa la navegación del contenido,
            // conserva contraste OLED y evita la banda plana típica de Material.
            Surface(
                modifier = Modifier.fillMaxWidth().padding(start = 10.dp, end = 10.dp, top = 4.dp, bottom = 7.dp),
                shape = androidx.compose.foundation.shape.RoundedCornerShape(23.dp),
                color = ObsidianSoft.copy(alpha = 0.98f),
                border = BorderStroke(1.dp, GlassOutline.copy(alpha = 0.55f)),
                shadowElevation = 12.dp
            ) {
                NavigationBar(
                    modifier = Modifier.fillMaxWidth(),
                    containerColor = Color.Transparent,
                    tonalElevation = 0.dp,
                    windowInsets = WindowInsets(0.dp)
                ) {
                    TABS.forEach { tab ->
                        val selected = current == tab.route
                        NavigationBarItem(
                            selected = selected,
                            onClick = {
                                if (current != tab.route) {
                                    tabNav.navigate(tab.route) {
                                        popUpTo(tabNav.graph.findStartDestination().id) { saveState = true }
                                        launchSingleTop = true
                                        restoreState = true
                                    }
                                }
                            },
                            icon = {
                                Icon(
                                    tab.icon,
                                    contentDescription = tab.label,
                                    modifier = Modifier.size(if (selected) 23.dp else 21.dp)
                                )
                            },
                            label = { Text(tab.label, style = MaterialTheme.typography.labelSmall, maxLines = 1) },
                            colors = NavigationBarItemDefaults.colors(
                                selectedIconColor = AuroraIce,
                                selectedTextColor = AuroraCyan,
                                indicatorColor = AuroraCyan.copy(alpha = 0.16f),
                                unselectedIconColor = TextMuted,
                                unselectedTextColor = TextMuted
                            )
                        )
                    }
                }
            }
        }
    ) { padding ->
        NavHost(
            navController    = tabNav,
            startDestination = TABS[0].route,
            modifier         = Modifier.padding(padding)
        ) {

            // ── CONTROL ─────────────────────────────────────────────────
            composable(TABS[0].route) {
                ControlTabScreen(
                    outerNav          = outerNav,
                    dsp               = dsp,
                    adaptiveBack      = adaptiveBack,
                    voiceMgr          = voiceMgr,
                    metrics           = metrics,
                    // v2.5.473: BRAIN/ADAPTIVE/SPATIAL ya no son tabs; los
                    // botones que apuntaban ahí van a FUSIÓN (operadores).
                    onOpenAdaptiveTab = { tabNav.navigate(TABS[2].route) { launchSingleTop = true } },
                    captureActive   = captureActive,
                    onToggleCapture = onToggleCapture,
                    onPausePlayback = onPausePlayback,
                )
            }





            // ── MÚSICA LOCAL (v2.5.447) ─────────────────────────────────
            // Reemplaza a TIDAL: la música descargada/guardada del celular
            // (Download y Música) va por el BridgePlayer (cadena magistral
            // al 100%), nunca por captura ni por el reproductor del sistema.
            composable(TABS[1].route) {
                LocalMusicScreen(
                    player = player,
                    onBack = { tabNav.navigate(TABS[0].route) { launchSingleTop = true } }
                )
            }

            // ── FUSIÓN MAGISTRAL · 7 OPERADORES (v2.5.468) ───────────────
            // La pantalla existía pero era huérfana (sin ruta ni tab). Ahora
            // es ventana propia: el usuario la ve al abrir la app.
            composable(TABS[2].route) {
                FusedOperatorsScreen(
                    onBack = { tabNav.navigate(TABS[0].route) { launchSingleTop = true } },
                    onOpenHeadMeasure = { outerNav.navigate(IvannaRoute.HEAD_MEASURE) }
                )
            }

            // ── SYSTEM (reducido, §1.1) ──────────────────────────────────
            // Solo: Perfiles · Estado Magisk/Ruta B · Audio Hi-Res ·
            // Acerca de/versión + Ajustes colapsados + Modo desarrollador.
            composable(TABS[3].route) {
                SystemHubScreen(
                    voiceProtectionEnabled = voiceMgr.voiceProtectionActive.observeAsState(false).value,
                    onVoiceProtectionChange = { voiceMgr.toggle() },
                    onOpenMagisk    = { outerNav.navigate("magisk") },
                    onOpenProfiles  = { outerNav.navigate("profiles") },
                    onOpenHiRes     = { outerNav.navigate("hires") },
                    onOpenDevRoute  = { route -> outerNav.navigate(route) }
                )
            }
        }
    }
}

// ── SystemHubScreen ───────────────────────────────────────────────────────────
// v2.5.473 (§1.1): SYSTEM reducido a 4 entradas (Perfiles · Magisk/Ruta B ·
// Hi-Res · Acerca de/versión) + Ajustes colapsados + Modo desarrollador.
// Los 15 HubCards (5 ejes, OEM++, flota, asistente, lab, motores, red,
// perfiles adaptativos, Music Intelligence, conciertos, diagnóstico vivo)
// salen de la UI de usuario: viven detrás del Modo desarrollador (7 toques
// en la versión) o no existen como ruta de usuario. La función conserva su
// nombre y los HubCard/HubHeader compartidos.
@Composable
fun SystemHubScreen(
    onOpenMagisk: () -> Unit,
    onOpenProfiles: () -> Unit,
    onOpenHiRes: () -> Unit = {},
    onOpenDevRoute: (String) -> Unit = {},
    voiceProtectionEnabled: Boolean = true,
    onVoiceProtectionChange: (Boolean) -> Unit = {},
) {
    val context = LocalContext.current
    var showSettings by remember { mutableStateOf(false) }
    var devEnabled by remember { mutableStateOf(DevMode.isEnabled(context)) }
    var versionTaps by remember { mutableStateOf(0) }
    var resetMessage by remember { mutableStateOf("") }
    val versionName = runCatching { com.ivanna.omega.BuildConfig.VERSION_NAME }.getOrDefault("?")
    val versionCode = runCatching { com.ivanna.omega.BuildConfig.VERSION_CODE }.getOrDefault(0)

    Column(
        modifier = Modifier.fillMaxSize().background(ObsidianVoid)
            .verticalScroll(rememberScrollState())
            .padding(horizontal = 16.dp, vertical = 20.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp)
    ) {
        HubHeader("SISTEMA", "IVANNA · Perfiles · Magisk · Hi-Res · Versión", AmberSignal)
        HubCard("PERFILES DE USUARIO", "Bandas auditivas · EQ precalibrado · Perfil activo", AmberSignal, onOpenProfiles)
        HubCard("MAGISK MODULE STATUS", "Daemon RT · Ruta B · Sesión", AmberSignal, onOpenMagisk)
        HubCard("AUDIO HI-RES", "Selector de frecuencia 16..384 kHz y profundidad de bits", AuroraCyan, onOpenHiRes)
        // FIX 2.5.474 — VISTA DE DIOS · FLOTA visible sin Modo desarrollador:
        // la flota es del propietario (ciclos reales del consejo) y estaba
        // escondida detrás de 7 toques en la versión; además los veredictos
        // ya se entregan al teléfono (filesDir/fleet_cycles), así que la
        // pantalla muestra ciclos reales, no "Sin ciclos todavía".
        HubCard("VISTA DE DIOS · FLOTA", "Ciclos del consejo · Rondas · Veredictos", AuroraCyan) { onOpenDevRoute(IvannaRoute.FLEET_VIEW) }
        // Acerca de / versión: 7 toques activan o desactivan el Modo desarrollador.
        HubCard(
            "ACERCA DE · IVANNA v$versionName ($versionCode)",
            if (devEnabled) "Modo desarrollador: ACTIVADO · toca 7 veces para apagarlo"
            else "Toca 7 veces para activar el Modo desarrollador",
            AuroraCyan
        ) {
            versionTaps++
            if (versionTaps >= DevMode.TAPS_TO_TOGGLE) {
                versionTaps = 0
                val next = !DevMode.isEnabled(context)
                DevMode.setEnabled(context, next)
                devEnabled = next
            }
        }

        // ── Ajustes (colapsados) ──────────────────────────────────────────
        HubCard(
            if (showSettings) "▼ AJUSTES" else "▶ AJUSTES",
            "Sensor acústico · VoiceProtection · Restablecer valores",
            TextMuted
        ) { showSettings = !showSettings }
        if (showSettings) {
            Text(
                "Sensor acústico: se activa con el permiso de micrófono al medir la sala " +
                    "(CONTROL → Sala) o desde el ajuste del sensor. Sin permiso, Φ corre " +
                    "en APROXIMADO fail-closed.",
                color = TextSecondary, style = MaterialTheme.typography.bodySmall
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text(
                    "VoiceProtection: protege la voz al cambiar de app.",
                    color = TextSecondary, style = MaterialTheme.typography.bodySmall,
                    modifier = Modifier.weight(1f)
                )
                androidx.compose.material3.Switch(
                    checked = voiceProtectionEnabled,
                    onCheckedChange = { onVoiceProtectionChange(it) }
                )
            }
            androidx.compose.material3.OutlinedButton(onClick = {
                runCatching {
                    val store = com.ivanna.omega.core.ParameterStore(context)
                    store.saveFusedControls(com.ivanna.omega.audio.FusedControls())
                    store.setCurrentAudioProfileId(null)
                }
                resetMessage = "Valores restablecidos: FUSIÓN a fábrica y perfil borrado (al abrir, vuelve Steve Miller)."
            }) { Text("RESTABLECER VALORES") }
            if (resetMessage.isNotEmpty()) {
                Text(resetMessage, color = TextSecondary, style = MaterialTheme.typography.bodySmall)
            }
        }

        // ── Modo desarrollador: diagnósticos internos ─────────────────────
        if (devEnabled) {
            HubHeader("MODO DESARROLLADOR", "Diagnóstico interno · no es escucha diaria", PhosphorGreen)
            HubCard("LABORATORIO DSP", "Sweep · LUFS · THD+N · SNR", PhosphorGreen) { onOpenDevRoute("lab") }
            HubCard("ESTADO DE MOTORES", "RouteDspCalibrator · USB Pro · HRTF · Backend · Daemon", AuroraCyan) { onOpenDevRoute("engines_status") }
            HubCard("CENTRO DE CONTROL OEM++", "Dashboard · HRTF · SAF/RIR · IA · Térmico · Telemetría", AuroraCyan) { onOpenDevRoute("oem_dashboard") }
            HubCard("✦ 5 EJES DE SUPREMACÍA", "Celosía · CVNN · SNN-HOA · Pinna INR · Farrow", PhosphorGreen) { onOpenDevRoute(IvannaRoute.SUPREME_AXES_HUB) }
            // FIX 2.5.474 — La flota vive ahora en el SYSTEM principal (visible
            // sin Modo desarrollador); esta entrada se retira para no duplicarla.
            HubCard("IVANNA ASSISTANT", "Inteligencia acústica conversacional", AuroraCyan) { onOpenDevRoute(IvannaRoute.IVANNA_ASSISTANT) }
            HubCard("ESTADO DE RED / TELEMETRÍA", "Latencia · Socket IPC · Puertos", AuroraCyan) { onOpenDevRoute(IvannaRoute.NETWORK) }
            HubCard("PRUEBA ABX", "Validación perceptual", NeonMagenta) { onOpenDevRoute(IvannaRoute.ABX_TEST) }
            HubCard("BENCHMARK & EVIDENCE", "Telemetría y validación acústica", NeonMagenta) { onOpenDevRoute("benchmark") }
            HubCard("FASE 7: HEGEMONÍA", "Computer Vision & AutoEQ", NeonMagenta) { onOpenDevRoute("phase7") }
            HubCard("WFS · CALIBRACIÓN DE SALA", "Posición X/Y/Z · Preset 7ch", NeonMagenta) { onOpenDevRoute("wfs_calibration") }
            HubCard("SOFA · AF · RIR · SAF", "Panel de motores avanzados", AuroraCyan) { onOpenDevRoute(IvannaRoute.AUDIO_CONTROL_HUB) }
            HubCard("VISUALIZADOR DE ESPECTRO", "FFT 64-Band · Bark Perceptual", AuroraCyan) { onOpenDevRoute("visualizer") }
            HubCard("MUSIC INTELLIGENCE", "Análisis musical en tiempo real", PhosphorGreen) { onOpenDevRoute("music_intelligence") }
            HubCard("CONCIERTOS LEGENDARIOS", "Los 30 mejores conciertos de rock", AmberSignal) { onOpenDevRoute(IvannaRoute.ROCK_CONCERTS) }
            HubCard("ADAPTATIVO (MOTOR)", "AdaptiveEngineScreen · motor adaptativo", AuroraCyan) { onOpenDevRoute("adaptive") }
            HubCard("COGNITIVO (ACTIVITY)", "Dashboard cognitivo en Activity separada", AuroraCyan) { onOpenDevRoute("cognitive_dash") }
        }
    }
}

@Composable
private fun HubHeader(title: String, subtitle: String, accent: Color) {
    Column(
        modifier = Modifier.fillMaxWidth().padding(top = 4.dp, bottom = 6.dp),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Box(Modifier.size(7.dp).background(accent, androidx.compose.foundation.shape.CircleShape))
            Text(
                "IVANNA  /  OMEGA SYSTEMS",
                color = TextMuted,
                style = MaterialTheme.typography.labelSmall,
                letterSpacing = 1.25.sp
            )
        }
        Text(
            title,
            color = TextPrimary,
            style = MaterialTheme.typography.headlineLarge,
            fontWeight = FontWeight.Bold
        )
        Text(
            subtitle,
            color = TextSecondary,
            style = MaterialTheme.typography.bodyMedium
        )
        Box(
            Modifier.fillMaxWidth().height(1.dp).background(
                Brush.horizontalGradient(listOf(accent.copy(alpha = 0.85f), accent.copy(alpha = 0.18f), Color.Transparent))
            )
        )
    }
}

// v2.5.369 (refactor visual): cada tarjeta del hub tenía el mismo punto
// de color como "icono" — 30 tarjetas indistinguibles de un vistazo.
// Ahora cada superficie tiene su icono Material real, derivado del título
// (los títulos son literales en este mismo archivo). El fallback es un
// círculo, como antes, para cualquier título futuro sin mapear.
private fun hubIconFor(title: String): androidx.compose.ui.graphics.vector.ImageVector = when {
    title.contains("5 EJES") -> Icons.Filled.Hub
    title.contains("EJE 1") -> Icons.Filled.GridOn
    title.contains("EJE 2") -> Icons.Filled.Waves
    title.contains("EJE 3") -> Icons.Filled.GraphicEq
    title.contains("EJE 4") -> Icons.Filled.Hearing
    title.contains("EJE 5") -> Icons.Filled.Speed
    title.contains("COCLEAR") -> Icons.Filled.Psychology
    title.contains("SOFA") -> Icons.Filled.Folder
    title.contains("SAF") -> Icons.Filled.Person
    title.contains("CONTROL ESPACIAL") -> Icons.Filled.ViewInAr
    title.contains("VISUALIZADOR") -> Icons.Filled.BarChart
    title.contains("OPE") -> Icons.Filled.Tune
    title.contains("BINAURAL") -> Icons.Filled.Headphones
    title.contains("AUDITIVA") -> Icons.Filled.MusicNote
    title.contains("ABX") -> Icons.Filled.Science
    title.contains("BENCHMARK") -> Icons.Filled.Assessment
    title.contains("FASE 7") -> Icons.Filled.AutoAwesome
    title.contains("WFS") -> Icons.Filled.Speaker
    title.contains("COGNITIVO") -> Icons.Filled.Psychology
    title.contains("DIAGNÓSTICO") -> Icons.Filled.BugReport
    title.contains("ASSISTANT") -> Icons.Filled.Chat
    title.contains("FLOTA") -> Icons.Filled.Visibility
    title.contains("OEM") -> Icons.Filled.Dashboard
    title.contains("MAGISK") -> Icons.Filled.Security
    title.contains("PERFILES") -> Icons.Filled.Person
    title.contains("LABORATORIO") -> Icons.Filled.Science
    title.contains("MOTORES") -> Icons.Filled.Memory
    title.contains("RED") -> Icons.Filled.Wifi
    title.contains("HI-RES") -> Icons.Filled.HighQuality
    title.contains("MUSIC INTELLIGENCE") -> Icons.Filled.LibraryMusic
    title.contains("CONCIERTOS") -> Icons.Filled.MusicVideo
    else -> Icons.Filled.Circle
}

@Composable
private fun HubCard(title: String, subtitle: String, accent: Color, onClick: () -> Unit) {
    Surface(
        onClick = onClick,
        modifier = Modifier.fillMaxWidth(),
        color = ObsidianRaised,
        shape = RoundedCornerShape(18.dp),
        border = BorderStroke(1.dp, GlassOutline.copy(alpha = 0.58f)),
        shadowElevation = 2.dp
    ) {
        Row(
            modifier = Modifier.padding(horizontal = 15.dp, vertical = 14.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(13.dp)
        ) {
            Box(
                modifier = Modifier.size(42.dp).background(
                    Brush.linearGradient(listOf(accent.copy(alpha = 0.23f), accent.copy(alpha = 0.06f))),
                    RoundedCornerShape(13.dp)
                ).border(1.dp, accent.copy(alpha = 0.38f), RoundedCornerShape(13.dp)),
                contentAlignment = Alignment.Center
            ) {
                Icon(
                    hubIconFor(title),
                    contentDescription = null,
                    tint = accent,
                    modifier = Modifier.size(22.dp)
                )
            }
            Column(Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(4.dp)) {
                Text(
                    title,
                    color = TextPrimary,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 2
                )
                Text(
                    subtitle,
                    color = TextSecondary,
                    style = MaterialTheme.typography.bodySmall,
                    maxLines = 2
                )
            }
            Icon(
                Icons.AutoMirrored.Filled.KeyboardArrowRight,
                contentDescription = "Abrir $title",
                tint = accent,
                modifier = Modifier.size(23.dp)
            )
        }
    }
}
