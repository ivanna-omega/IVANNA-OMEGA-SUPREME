package com.ivanna.omega.ui

import android.content.Context
import androidx.compose.foundation.background
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.Velocity
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.R
import com.ivanna.omega.ui.theme.AmberSignal
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.GlassOutline
import com.ivanna.omega.ui.theme.ObsidianRaised
import com.ivanna.omega.ui.theme.ObsidianVoid
import com.ivanna.omega.ui.theme.PhosphorGreen
import com.ivanna.omega.ui.theme.TextMuted
import com.ivanna.omega.ui.theme.TextPrimary
import com.ivanna.omega.ui.theme.TextSecondary
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * VISTA DE DIOS · FLOTA (v2.5.471 Repair 4).
 *
 * Muestra los ciclos REALES del consejo (detector → verificador → revisor →
 * árbitro) con su hallazgo, sus rondas, su veredicto y su fecha. Nada de lo
 * que se muestra aquí es inventado: cada tarjeta es un archivo de veredicto
 * leído del puente.
 *
 * FUENTE REAL — [FleetCycleStore.load]: archivos `council_verdict_*.json`
 * en el directorio privado de la app `filesDir/fleet_cycles/`, con el
 * esquema que escribe `tools/evolution/council.py` en la VM
 * (`finding`, `approved`, `patch`, `rounds[]`, `detector`, `verifier`,
 * `reviewer`, `arbiter`). La fecha de cada ciclo es el `lastModified` del
 * archivo — los veredictos no llevan timestamp propio.
 *
 * VERDAD HOY (2026-11-18): ese directorio está vacío porque el puente
 * (tools/evolution, Python externo en la VM) escribe los veredictos solo en
 * la raíz del repositorio y NADA los entrega al teléfono: no hay canal de
 * entrega (ni archivo en /data/adb/ivanna_omega, ni SharedPreferences, ni
 * socket, ni API) que la app pueda leer. Por eso la pantalla muestra
 * "Sin ciclos todavía", que es la verdad, y NO actividad falsa.
 *
 * LO QUE FALTA CONECTAR: un paso del puente que copie cada
 * `council_verdict_*.json` nuevo al teléfono (p. ej. vía adb/Tailscale al
 * directorio `filesDir/fleet_cycles/` de la app, o a un archivo en
 * /data/adb/ivanna_omega legible con root). En cuanto el primer archivo
 * llegue, esta pantalla lo muestra sin tocar código: el lector ya está.
 *
 * REFRESCO: relectura automática cada [FLEET_POLL_INTERVAL_MS] (5 min),
 * pull-to-refresh (arrastrar hacia abajo en el tope) y botón Actualizar.
 * material3 1.2.0 no trae PullToRefreshBox (llegó en 1.3.0) ni hay
 * accompanist en las dependencias, así que el gesto se implementa con un
 * NestedScrollConnection propio; el botón es la vía equivalente accesible.
 * PROHIBIDO: texto fijo que aparente actividad ("sistema activo",
 * "todo bien", contadores inventados). La única marca temporal que se
 * muestra además de las de los ciclos es la hora real de la última lectura.
 */

// Intervalo de relectura automática del puente (5 minutos).
const val FLEET_POLL_INTERVAL_MS = 300_000L

// Un ciclo del consejo, tal como lo escribió el puente. Sin adornos.
data class FleetCycle(
    val id: String,            // nombre del archivo de veredicto
    val finding: String,       // hallazgo que originó el ciclo
    val approved: Boolean?,    // null = el consejo no emitió veredicto
    val rounds: Int,           // rondas registradas en el veredicto
    val roles: List<String>,   // detector/verifier/reviewer/arbiter presentes
    val patchPresent: Boolean, // el veredicto trae parche o no
    val timestampMs: Long      // lastModified del archivo (fecha del ciclo)
)

data class FleetSnapshot(
    val cycles: List<FleetCycle>,
    val unreadable: Int // archivos council_verdict_*.json que no se pudieron parsear
)

/**
 * Lee el estado real del puente desde el almacenamiento privado de la app.
 * Debe llamarse desde Dispatchers.IO: hace I/O de archivos.
 * Si el directorio no existe o está vacío, devuelve una lista vacía — esa es
 * la señal de "Sin ciclos todavía", no un error.
 */
object FleetCycleStore {

    private const val CYCLES_DIR = "fleet_cycles"
    private val FILE_REGEX = Regex("""council_verdict_.*\.json""")
    private val ROLES = listOf("detector", "verifier", "reviewer", "arbiter")

    fun load(context: Context): FleetSnapshot {
        val dir = File(context.filesDir, CYCLES_DIR)
        if (!dir.isDirectory) return FleetSnapshot(emptyList(), 0)
        val files = dir.listFiles { f -> FILE_REGEX.matches(f.name) }
            ?.sortedByDescending { it.lastModified() }
            ?: return FleetSnapshot(emptyList(), 0)
        val cycles = mutableListOf<FleetCycle>()
        var unreadable = 0
        for (file in files) {
            try {
                val json = JSONObject(file.readText())
                cycles += FleetCycle(
                    id = file.name,
                    finding = json.optString("finding", "").ifBlank { file.name },
                    approved = if (json.isNull("approved")) null else json.optBoolean("approved"),
                    rounds = json.optJSONArray("rounds")?.length() ?: 0,
                    roles = ROLES.filter { !json.isNull(it) },
                    patchPresent = !json.isNull("patch"),
                    timestampMs = file.lastModified()
                )
            } catch (_: Exception) {
                // Archivo corrupto o con esquema distinto: se cuenta, no se inventa.
                unreadable++
            }
        }
        return FleetSnapshot(cycles, unreadable)
    }

    fun formatTimestamp(timestampMs: Long): String =
        SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.getDefault()).format(Date(timestampMs))
}

@Composable
fun FleetViewScreen(onBack: () -> Unit) {
    val context = LocalContext.current
    val appContext = context.applicationContext
    val scope = rememberCoroutineScope()
    val scrollState = rememberScrollState()
    val density = LocalDensity.current
    val pullThresholdPx = with(density) { 96.dp.toPx() }

    var snapshot by remember { mutableStateOf<FleetSnapshot?>(null) }
    var refreshing by remember { mutableStateOf(false) }
    var lastReadMs by remember { mutableStateOf<Long?>(null) }
    var pullOffset by remember { mutableStateOf(0f) }

    suspend fun loadOnce() {
        val loaded = withContext(Dispatchers.IO) { FleetCycleStore.load(appContext) }
        snapshot = loaded
        lastReadMs = System.currentTimeMillis()
    }

    fun refresh() {
        if (refreshing) return
        scope.launch {
            refreshing = true
            try {
                loadOnce()
            } finally {
                refreshing = false
            }
        }
    }

    // Lectura inicial + relectura automática cada FLEET_POLL_INTERVAL_MS.
    LaunchedEffect(Unit) {
        loadOnce()
        while (true) {
            delay(FLEET_POLL_INTERVAL_MS)
            loadOnce()
        }
    }

    // Pull-to-refresh propio (material3 1.2.0 no trae PullToRefreshBox):
    // tirar hacia abajo en el tope acumula arrastre; al cruzar el umbral
    // dispara la relectura. Al soltar (fling) el offset vuelve a cero.
    val pullConnection = remember {
        object : NestedScrollConnection {
            override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
                if (available.y > 0f && scrollState.value == 0 && !refreshing) {
                    pullOffset += available.y
                    if (pullOffset >= pullThresholdPx) {
                        pullOffset = 0f
                        refresh()
                    }
                    return available
                }
                return Offset.Zero
            }

            override suspend fun onPostFling(consumed: Velocity, available: Velocity): Velocity {
                pullOffset = 0f
                return super.onPostFling(consumed, available)
            }
        }
    }

    val cycles = snapshot?.cycles.orEmpty()
    val pullProgress = (pullOffset / pullThresholdPx).coerceIn(0f, 1f)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(ObsidianVoid)
            .nestedScroll(pullConnection)
    ) {
        // ── Cabecera ────────────────────────────────────────────────────
        Row(
            modifier = Modifier.fillMaxWidth().padding(horizontal = 8.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically
        ) {
            IconButton(onClick = onBack) {
                Icon(
                    Icons.AutoMirrored.Filled.ArrowBack,
                    contentDescription = "Volver",
                    tint = AuroraCyan
                )
            }
            Column(Modifier.weight(1f)) {
                Text(
                    stringResource(R.string.fleet_title),
                    color = TextPrimary,
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold
                )
                Text(
                    stringResource(R.string.fleet_subtitle),
                    color = TextSecondary,
                    style = MaterialTheme.typography.bodySmall
                )
            }
            IconButton(onClick = { refresh() }) {
                Icon(
                    Icons.Filled.Refresh,
                    contentDescription = stringResource(R.string.fleet_refresh),
                    tint = AuroraCyan
                )
            }
        }

        // Indicador de refresco: indeterminado mientras lee, progreso del
        // gesto mientras se tira hacia abajo. Nada más.
        if (refreshing) {
            LinearProgressIndicator(
                modifier = Modifier.fillMaxWidth(),
                color = AuroraCyan,
                trackColor = Color.Transparent
            )
        } else if (pullProgress > 0f) {
            LinearProgressIndicator(
                progress = pullProgress,
                modifier = Modifier.fillMaxWidth(),
                color = AuroraCyan,
                trackColor = Color.Transparent
            )
        }

        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(scrollState)
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            if (snapshot == null) {
                // Primera lectura aún en curso: sin texto que aparente nada.
                Box(Modifier.fillMaxWidth().padding(top = 48.dp), contentAlignment = Alignment.Center) {
                    Text("…", color = TextMuted, fontSize = 20.sp)
                }
            } else if (cycles.isEmpty()) {
                // ESTADO REAL HOY: sin ciclos. No se inventa actividad.
                Column(
                    modifier = Modifier.fillMaxWidth().padding(top = 48.dp),
                    horizontalAlignment = Alignment.CenterHorizontally,
                    verticalArrangement = Arrangement.spacedBy(12.dp)
                ) {
                    Icon(
                        Icons.Filled.Visibility,
                        contentDescription = null,
                        tint = TextMuted,
                        modifier = Modifier.size(44.dp)
                    )
                    Text(
                        stringResource(R.string.fleet_no_cycles),
                        color = TextPrimary,
                        style = MaterialTheme.typography.titleMedium,
                        fontWeight = FontWeight.Bold
                    )
                    Text(
                        stringResource(R.string.fleet_no_cycles_hint),
                        color = TextSecondary,
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
            } else {
                for (cycle in cycles) {
                    FleetCycleCard(cycle)
                }
            }

            val snap = snapshot
            if (snap != null && snap.unreadable > 0) {
                Text(
                    "${snap.unreadable} ${stringResource(R.string.fleet_unreadable)}",
                    color = AmberSignal,
                    style = MaterialTheme.typography.bodySmall
                )
            }

            // Hora real de la última lectura del puente (no del ciclo).
            lastReadMs?.let { readMs ->
                Spacer(Modifier.height(8.dp))
                Text(
                    "${stringResource(R.string.fleet_last_read)}: " +
                        FleetCycleStore.formatTimestamp(readMs),
                    color = TextMuted,
                    style = MaterialTheme.typography.bodySmall
                )
            }
        }
    }
}

@Composable
private fun FleetCycleCard(cycle: FleetCycle) {
    val verdictText = when (cycle.approved) {
        true -> stringResource(R.string.fleet_verdict_approved)
        false -> stringResource(R.string.fleet_verdict_rejected)
        null -> stringResource(R.string.fleet_verdict_pending)
    }
    val verdictColor = when (cycle.approved) {
        true -> PhosphorGreen
        false -> AmberSignal
        null -> TextMuted
    }
    val patchYes = stringResource(R.string.fleet_patch_yes)
    val patchNo = stringResource(R.string.fleet_patch_no)

    Surface(
        modifier = Modifier.fillMaxWidth(),
        color = ObsidianRaised,
        shape = RoundedCornerShape(18.dp),
        border = BorderStroke(1.dp, GlassOutline.copy(alpha = 0.58f)),
        shadowElevation = 2.dp
    ) {
        Column(
            modifier = Modifier.padding(horizontal = 15.dp, vertical = 14.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp)
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Text(
                    verdictText,
                    color = verdictColor,
                    style = MaterialTheme.typography.labelLarge,
                    fontWeight = FontWeight.Bold,
                    modifier = Modifier
                        .background(verdictColor.copy(alpha = 0.14f), RoundedCornerShape(8.dp))
                        .padding(horizontal = 8.dp, vertical = 4.dp)
                )
                Text(
                    FleetCycleStore.formatTimestamp(cycle.timestampMs),
                    color = TextMuted,
                    style = MaterialTheme.typography.bodySmall
                )
            }
            Text(
                cycle.finding,
                color = TextPrimary,
                style = MaterialTheme.typography.bodyMedium
            )
            Text(
                "${stringResource(R.string.fleet_rounds)}: ${cycle.rounds}  ·  " +
                    "${stringResource(R.string.fleet_patch)}: ${if (cycle.patchPresent) patchYes else patchNo}",
                color = TextSecondary,
                style = MaterialTheme.typography.bodySmall
            )
            if (cycle.roles.isNotEmpty()) {
                Text(
                    "${stringResource(R.string.fleet_roles)}: ${cycle.roles.joinToString(" · ")}",
                    color = TextSecondary,
                    style = MaterialTheme.typography.bodySmall
                )
            }
            Text(
                "${stringResource(R.string.fleet_source)}: ${cycle.id}",
                color = TextMuted,
                style = MaterialTheme.typography.bodySmall
            )
        }
    }
}
