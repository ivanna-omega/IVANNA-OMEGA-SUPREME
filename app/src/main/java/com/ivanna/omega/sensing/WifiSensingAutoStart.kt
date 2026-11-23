package com.ivanna.omega.sensing

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.core.content.ContextCompat
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.compose.ui.platform.LocalLifecycleOwner
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.hardware.PermissionGate
import com.ivanna.omega.hardware.PermissionState
import com.ivanna.omega.ui.rememberRoomMeasureLauncher
import kotlinx.coroutines.delay

/**
 * Starts a consented Wi-Fi survey automatically when the app enters foreground.
 * It never starts when opt-in is false, and pauses on backgrounding. The visible
 * status strip doubles as an immediate stop/revoke control.
 */
@Composable
fun WifiSensingAutoStart(content: @Composable () -> Unit) {
    val context = LocalContext.current
    val lifecycleOwner = LocalLifecycleOwner.current
    val prefs = remember(context) {
        context.getSharedPreferences("acoustic_consent_v1", Context.MODE_PRIVATE)
    }
    var status by remember { mutableStateOf("Medición Wi-Fi inactiva.") }
    var active by remember { mutableStateOf(false) }
    var optedIn by remember { mutableStateOf(prefs.getBoolean("wifi_sensing_opt_in", false)) }
    var observations by remember { mutableStateOf(0) }
    var networkRows by remember { mutableStateOf<List<WifiObservation>>(emptyList()) }
    var signalSummary by remember { mutableStateOf("Sin datos de señal") }
    var userStopped by remember { mutableStateOf(false) }
    var askedPermission by remember { mutableStateOf(false) }
    // Ventana plegada por defecto; se despliega sola al llegar las primeras
    // redes reales tras conceder el permiso (ver callback de la sesión).
    var expanded by remember { mutableStateOf(false) }
    // Adaptador universal: la fuente que este equipo/router entrega y la
    // decisión de la política sobre cada escena. La decisión es lo que le da
    // EFECTO EN EL SONIDO al Wi-Fi: cuando pide re-medición, la sala se vuelve
    // a medir (RoomProbe real) y la corrección aplicada cambia el sonido.
    // Nada de DSP directo por radio: el Wi-Fi solo dispara la medición.
    val roomLauncher = rememberRoomMeasureLauncher()
    val paramStore = remember(context) { ParameterStore(context) }
    var wifiSource by remember { mutableStateOf(WifiSensingUniversalAdapter.Source.NONE) }
    var sourceReady by remember { mutableStateOf(false) }
    var adaptationDecision by remember { mutableStateOf<WifiAcousticAdaptationPolicy.Decision?>(null) }
    var autoCountdown by remember { mutableStateOf<Int?>(null) }
    var autoDismissedUntilMs by remember { mutableStateOf(0L) }
    val session = remember(context) {
        WifiScanSession(context) { update ->
            status = update.message
            active = update.sessionRunning
            observations = update.observations.size
            if (update.observations.isNotEmpty()) {
                // Al llegar las primeras redes reales, la ventana se
                // despliega sola para mostrarlas; si el usuario la pliega,
                // no se le vuelve a abrir encima mientras haya datos.
                val wasEmpty = networkRows.isEmpty()
                networkRows = update.observations.sortedByDescending { it.rssiDbm }.take(12)
                if (wasEmpty) expanded = true
            }
            val scene = update.sceneFeatures
            signalSummary = when (scene.level) {
                UniversalWifiSensingAdapter.DataLevel.NO_WIFI -> "Sin señal Wi-Fi nueva"
                UniversalWifiSensingAdapter.DataLevel.WIFI_METADATA_ONLY -> "RSSI · cambio=${scene.movementIndicator?.let { "%.2f".format(it) } ?: "sin referencia"} · confianza=${"%.0f".format(scene.confidence * 100)}%"
                UniversalWifiSensingAdapter.DataLevel.CSI_VERIFIED -> "CSI verificado · proveedor CSI activo"
            }
            // Adaptador universal: qué fuente entrega este equipo/router y qué
            // decide la política sobre esta escena. La fuente se evalúa una
            // vez por sesión (la capacidad del hardware no cambia en vivo).
            if (!sourceReady) {
                wifiSource = WifiSensingUniversalAdapter.selectSource(context)
                sourceReady = true
            }
            val calibrationAvailable = paramStore.isRoomCorrectionEnabled()
            val calibrationAgeMs = paramStore.getRoomCorrectionMeasuredAt()?.let { measuredAt ->
                (System.currentTimeMillis() - measuredAt).takeIf { it >= 0L }
            }
            adaptationDecision = WifiSensingUniversalAdapter.evaluate(
                update.sceneFeatures, calibrationAvailable, calibrationAgeMs
            )
        }
    }
    val locationPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        if (granted && optedIn && lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) {
            active = session.start(consentGranted = true)
            if (!active) status = "Permiso concedido, pero el escaneo no pudo iniciarse. Comprueba Ubicación y Wi-Fi."
        } else {
            active = false
            status = if (granted) "Permiso concedido; activa la medición Wi-Fi desde la aplicación."
                else "Permiso de ubicación denegado; el escaneo Android no se iniciará ni se eludirá con root."
        }
    }

    DisposableEffect(lifecycleOwner, session, prefs) {
        fun syncAndStart() {
            optedIn = prefs.getBoolean("wifi_sensing_opt_in", false)
            if (optedIn && !userStopped) {
                val hasLocation = ContextCompat.checkSelfPermission(
                    context, Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED
                if (!hasLocation && !askedPermission) {
                    // Con el consentimiento ya dado, se pide el permiso de ubicación una vez (Android
                    // lo exige para listar redes Wi-Fi) en lugar de esperar a que se toque un botón oculto.
                    askedPermission = true
                    active = false
                    status = "Solicitando el permiso de ubicación que Android exige para escanear Wi-Fi."
                    locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
                } else {
                    active = session.start(consentGranted = true)
                }
            } else {
                session.stop()
                active = false
                status = if (userStopped) "Medición detenida por el usuario."
                    else "Medición Wi-Fi inactiva: consentimiento no habilitado."
            }
        }
        val observer = LifecycleEventObserver { _, event ->
            when (event) {
                Lifecycle.Event.ON_RESUME -> syncAndStart()
                Lifecycle.Event.ON_PAUSE -> {
                    session.stop()
                    active = false
                    status = "Medición pausada al salir de primer plano."
                }
                else -> Unit
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) {
            syncAndStart()
        }
        val listener = android.content.SharedPreferences.OnSharedPreferenceChangeListener { _, key ->
            if (key == "wifi_sensing_opt_in") {
                val consentEnabled = prefs.getBoolean("wifi_sensing_opt_in", false)
                optedIn = consentEnabled
                if (consentEnabled) {
                    // Un consentimiento nuevo explícito debe poder reactivar la sesión
                    // en el mismo ciclo de vida; userStopped solo bloquea hasta esta acción.
                    userStopped = false
                    if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) {
                        active = session.start(consentGranted = true)
                        if (!active) status = "Consentimiento guardado; el sondeo no pudo iniciarse."
                    }
                } else {
                    session.stop()
                    active = false
                    status = "Consentimiento Wi-Fi revocado; medición detenida."
                }
            }
        }
        prefs.registerOnSharedPreferenceChangeListener(listener)
        onDispose {
            prefs.unregisterOnSharedPreferenceChangeListener(listener)
            lifecycleOwner.lifecycle.removeObserver(observer)
            session.stop()
        }
    }

    // Cuenta atrás para la medición automática: cuando el adaptador pide
    // re-medición y el micrófono ya está concedido, la sala se mide sola en
    // 15 s para que el sonido cambie (efecto real, medido por RoomProbe).
    // El usuario puede cancelarla o medir ya. Sin micrófono concedido no hay
    // auto-medición: se muestra el botón y el permiso se pide al pulsarlo.
    val autoMeasureCountdownS = 15
    val autoMeasureDismissMs = 30 * 60 * 1000L
    LaunchedEffect(adaptationDecision, autoCountdown, roomLauncher.busy) {
        val decision = adaptationDecision
        if (decision?.action != WifiAcousticAdaptationPolicy.Action.REQUEST_ACOUSTIC_REMEASUREMENT) {
            autoCountdown = null
            return@LaunchedEffect
        }
        if (roomLauncher.busy) return@LaunchedEffect
        val countdown = autoCountdown
        if (countdown == null) {
            val micGranted = PermissionGate.stateFor(context, Manifest.permission.RECORD_AUDIO) ==
                PermissionState.GRANTED
            if (!micGranted) return@LaunchedEffect
            if (System.currentTimeMillis() < autoDismissedUntilMs) return@LaunchedEffect
            autoCountdown = autoMeasureCountdownS
            return@LaunchedEffect
        }
        if (countdown <= 0) {
            autoCountdown = null
            roomLauncher.launch()
            return@LaunchedEffect
        }
        delay(1000)
        autoCountdown = countdown - 1
    }

    // FIX (panel blanco que tapaba la app): esta franja vive FUERA del tema oscuro de OmegaApp, así que
    // MaterialTheme.colorScheme.surface era el blanco por defecto, con texto negro, y además se dibujaba
    // ENCIMA del contenido (tapaba la barra inferior y parte del scroll). Ahora ocupa su propio espacio
    // debajo del contenido (el contenido se encoge en vez de quedar tapado), usa colores oscuros
    // explícitos y el gráfico queda plegado por defecto.
    Column(Modifier.fillMaxSize()) {
        Box(Modifier.weight(1f).fillMaxWidth()) { content() }
        // FIX (franja inalcanzable): la franja es la única vía en la app para
        // activar la medición Wi-Fi; si solo se muestra con opt-in, quien no
        // marcó la casilla en el consentimiento no puede activarla nunca
        // (tampoco hay interruptor en Ajustes). Siempre visible; compacta
        // cuando está inactiva, con su botón de activación a la vista.
        Surface(
            modifier = Modifier.fillMaxWidth(),
            color = Color(0xFF0B1220),
            contentColor = Color(0xFFDDE6F0),
            tonalElevation = 0.dp
        ) {
            Column(Modifier.padding(horizontal = 12.dp, vertical = 8.dp)) {
                Text(
                    if (active) "FotoHolo Wi-Fi · sesión en primer plano · $observations observaciones"
                    else "FotoHolo Wi-Fi · inactiva",
                    style = MaterialTheme.typography.labelMedium
                )
                Text(status, style = MaterialTheme.typography.bodySmall)
                Text(signalSummary, style = MaterialTheme.typography.bodySmall)
                // Fuente real de este equipo/router (adaptador universal) y efecto
                // en el sonido: si el entorno cambió, la sala se vuelve a medir
                // y la corrección nueva cambia el sonido. Sin medición no hay
                // efecto; por eso aquí se pide y se ejecuta, no solo se informa.
                Text(
                    "Fuente Wi-Fi: ${WifiSensingUniversalAdapter.describeSource(wifiSource)}",
                    style = MaterialTheme.typography.bodySmall
                )
                if (adaptationDecision?.action == WifiAcousticAdaptationPolicy.Action.REQUEST_ACOUSTIC_REMEASUREMENT) {
                    Text(adaptationDecision?.reason.orEmpty(), style = MaterialTheme.typography.bodySmall)
                    if (autoCountdown != null) {
                        Text(
                            "La sala cambió: midiendo en ${autoCountdown} s (pausa la música).",
                            style = MaterialTheme.typography.bodySmall
                        )
                    }
                    Row(
                        modifier = Modifier.fillMaxWidth(),
                        horizontalArrangement = Arrangement.spacedBy(8.dp)
                    ) {
                        OutlinedButton(
                            onClick = { autoCountdown = null; roomLauncher.launch() },
                            enabled = !roomLauncher.busy
                        ) { Text("Medir ahora") }
                        if (autoCountdown != null) {
                            androidx.compose.material3.TextButton(
                                onClick = {
                                    autoCountdown = null
                                    autoDismissedUntilMs = System.currentTimeMillis() + autoMeasureDismissMs
                                }
                            ) { Text("Cancelar") }
                        }
                    }
                }
                if (roomLauncher.busy) {
                    Text(
                        "Midiendo sala… ${(roomLauncher.progress * 100).toInt()}%",
                        style = MaterialTheme.typography.bodySmall
                    )
                }
                if (roomLauncher.status.isNotBlank()) {
                    Text(roomLauncher.status, style = MaterialTheme.typography.labelSmall)
                }
                // Visualización real del sondeo: cada barra representa una red observada y su RSSI.
                // No presenta RSSI como CSI ni dibuja una reconstrucción espacial ficticia.
                if (expanded) Canvas(
                    modifier = Modifier.fillMaxWidth().height(76.dp)
                ) {
                    val left = 8f
                    val top = 8f
                    val bottom = size.height - 8f
                    val chartWidth = (size.width - 16f).coerceAtLeast(1f)
                    for (step in 0..4) {
                        val y = top + (bottom - top) * step / 4f
                        drawLine(
                            color = Color(0x337F8C9A),
                            start = androidx.compose.ui.geometry.Offset(left, y),
                            end = androidx.compose.ui.geometry.Offset(size.width - 8f, y),
                            strokeWidth = 1f
                        )
                    }
                    val rows = networkRows
                    if (rows.isNotEmpty()) {
                        val slot = chartWidth / rows.size
                        rows.forEachIndexed { index, network ->
                            val normalized = ((network.rssiDbm + 100f) / 70f).coerceIn(0f, 1f)
                            val barHeight = (bottom - top) * normalized
                            val x = left + index * slot + slot * 0.16f
                            val width = (slot * 0.68f).coerceAtLeast(2f)
                            val barColor = when {
                                network.rssiDbm >= -55 -> Color(0xFF37C987)
                                network.rssiDbm >= -70 -> Color(0xFFFFC857)
                                else -> Color(0xFFFF7A7A)
                            }
                            drawRect(
                                color = barColor,
                                topLeft = androidx.compose.ui.geometry.Offset(x, bottom - barHeight),
                                size = androidx.compose.ui.geometry.Size(width, barHeight)
                            )
                        }
                    } else {
                        drawCircle(
                            color = Color(0x667F8C9A),
                            radius = 4f,
                            center = androidx.compose.ui.geometry.Offset(size.width / 2f, size.height / 2f),
                            style = Stroke(width = 2f)
                        )
                    }
                }
                if (expanded) if (networkRows.isNotEmpty()) {
                    // Scroll real de la ventana: todas las redes observadas
                    // (hasta 12), no solo 5 en 2 líneas. Altura acotada para
                    // que la franja no se coma la pantalla; el scroll es
                    // propio de la franja, no anidado en el contenido.
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(max = 120.dp)
                            .verticalScroll(rememberScrollState())
                    ) {
                        networkRows.forEach { network ->
                            Text(
                                "${network.ssid.ifBlank { "<red oculta>" }} · ${network.rssiDbm} dBm",
                                style = MaterialTheme.typography.labelSmall,
                                maxLines = 1
                            )
                        }
                    }
                } else {
                    Text("El gráfico aparecerá cuando el sondeo entregue redes reales.", style = MaterialTheme.typography.labelSmall)
                }
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.End
                ) {
                    androidx.compose.material3.TextButton(onClick = { expanded = !expanded }) {
                        Text(if (expanded) "Ocultar gráfico" else "Ver gráfico")
                    }
                    if (active) {
                        OutlinedButton(onClick = {
                            userStopped = true
                            prefs.edit().putBoolean("wifi_sensing_opt_in", false).apply()
                            session.stop()
                            active = false
                            status = "Consentimiento Wi-Fi revocado; medición detenida."
                        }) {
                            Text("Detener y revocar")
                        }
                    } else if (ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED) {
                        OutlinedButton(onClick = {
                            userStopped = false
                            prefs.edit().putBoolean("wifi_sensing_opt_in", true).apply()
                            optedIn = true
                            locationPermissionLauncher.launch(Manifest.permission.ACCESS_FINE_LOCATION)
                        }) {
                            Text(if (optedIn) "Conceder permiso Wi-Fi" else "Activar escaneo Wi-Fi")
                        }
                    } else if (!optedIn) {
                        OutlinedButton(onClick = {
                            userStopped = false
                            prefs.edit().putBoolean("wifi_sensing_opt_in", true).apply()
                            optedIn = true
                            if (lifecycleOwner.lifecycle.currentState.isAtLeast(Lifecycle.State.RESUMED)) {
                                active = session.start(consentGranted = true)
                                if (!active) status = "No se pudo iniciar. Comprueba Wi-Fi, Ubicación y permiso preciso."
                            }
                        }) {
                            Text("Activar escaneo Wi-Fi")
                        }
                    } else {
                        OutlinedButton(onClick = {
                            userStopped = false
                            active = session.start(consentGranted = true)
                            if (!active) status = "No se pudo iniciar. Comprueba Wi-Fi, Ubicación y permiso preciso."
                        }) {
                            Text("Reintentar escaneo")
                        }
                    }
                }
            }
        }
    }
}
