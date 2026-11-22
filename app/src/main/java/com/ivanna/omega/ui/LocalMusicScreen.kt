package com.ivanna.omega.ui

import android.Manifest
import android.content.ContentUris
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.MediaStore
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.MusicNote
import androidx.compose.material.icons.filled.Pause
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.SkipNext
import androidx.compose.material.icons.filled.SkipPrevious
import androidx.compose.material.icons.filled.Stop
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.FilterChip
import androidx.compose.material3.FilterChipDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.OutlinedTextField
// v2.5.448: NO importar androidx.compose.material3.Slider — el contrato
// SAFMUS (scripts/check_safmus_transversal_wiring.py) prohíbe el Slider
// directo de Material3 en este paquete. Se usa el wrapper compartido
// `Slider` de SafmusComposeSlider.kt (mismo paquete, sin import).
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
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
import androidx.core.content.ContextCompat
import com.ivanna.omega.audio.IvannaBridgePlayer
import com.ivanna.omega.ui.theme.AuroraCyan
import com.ivanna.omega.ui.theme.ObsidianVoid
import com.ivanna.omega.ui.theme.TextMuted
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext

/**
 * LocalMusicScreen — v2.5.447. Reproductor de música LOCAL (reemplaza a TIDAL).
 *
 * Directiva de Esse (2026-11-15): quitar TIDAL para evitar temas (el
 * programa oficial no incluye reproducción completa y la vía no oficial
 * es fuera de términos y revocable) y reemplazarlo por un reproductor
 * nativo que lea la música descargada/guardada del celular (Download y
 * Música) y la pase por el BridgePlayer — la cadena magistral al 100% —
 * para escuchar si el producto funciona de verdad, sin red ni cuentas.
 *
 * TIDAL queda como legacy (código conservado, marcado @Deprecated, sin
 * borrar — regla de Esse); la pestaña TIDAL se sustituye por esta.
 *
 * Fuente: MediaStore.Audio (content:// URIs). Permiso READ_MEDIA_AUDIO
 * (Android 13+) o READ_EXTERNAL_STORAGE (anterior). El BridgePlayer ya
 * abre content:// con MediaExtractor (setDataSource(context, uri, null)),
 * así que no hace falta copiar archivos ni FileProvider.
 */
data class LocalTrack(
    val uri: Uri,
    val title: String,
    val artist: String,
    val durationMs: Long,
    val sizeBytes: Long,
    val folder: String,
    val dateAdded: Long
)

private enum class LocalFilter { ALL, DOWNLOAD, MUSIC }

/** Lee los audios locales (Download + Música + resto). Puro I/O, sin UI. */
fun loadLocalTracks(context: Context): List<LocalTrack> {
    val projection = arrayOf(
        MediaStore.Audio.Media._ID,
        MediaStore.Audio.Media.TITLE,
        MediaStore.Audio.Media.ARTIST,
        MediaStore.Audio.Media.DURATION,
        MediaStore.Audio.Media.SIZE,
        MediaStore.Audio.Media.DATA,
        MediaStore.Audio.Media.DATE_ADDED,
        MediaStore.Audio.Media.RELATIVE_PATH
    )
    val out = mutableListOf<LocalTrack>()
    val cursor = runCatching {
        context.contentResolver.query(
            MediaStore.Audio.Media.EXTERNAL_CONTENT_URI,
            projection,
            "${MediaStore.Audio.Media.IS_MUSIC} != 0",
            null,
            "${MediaStore.Audio.Media.DATE_ADDED} DESC"
        )
    }.getOrNull() ?: return emptyList()
    cursor.use { c ->
        val idCol = c.getColumnIndexOrThrow(MediaStore.Audio.Media._ID)
        val titleCol = c.getColumnIndexOrThrow(MediaStore.Audio.Media.TITLE)
        val artistCol = c.getColumnIndexOrThrow(MediaStore.Audio.Media.ARTIST)
        val durCol = c.getColumnIndexOrThrow(MediaStore.Audio.Media.DURATION)
        val sizeCol = c.getColumnIndexOrThrow(MediaStore.Audio.Media.SIZE)
        val dataCol = c.getColumnIndex(MediaStore.Audio.Media.DATA)
        val addedCol = c.getColumnIndex(MediaStore.Audio.Media.DATE_ADDED)
        val relCol = c.getColumnIndex(MediaStore.Audio.Media.RELATIVE_PATH)
        while (c.moveToNext()) {
            val duration = if (durCol >= 0) c.getLong(durCol) else 0L
            val size = if (sizeCol >= 0) c.getLong(sizeCol) else 0L
            if (duration <= 0L || size <= 0L) continue
            val id = c.getLong(idCol)
            val uri = ContentUris.withAppendedId(
                MediaStore.Audio.Media.EXTERNAL_CONTENT_URI, id
            )
            val title = c.getString(titleCol)?.takeIf { it.isNotBlank() } ?: "Sin título"
            val artist = c.getString(artistCol)?.takeIf { it.isNotBlank() && it != "<unknown>" } ?: "Desconocido"
            val data = if (dataCol >= 0) c.getString(dataCol).orEmpty() else ""
            val rel = if (relCol >= 0) c.getString(relCol).orEmpty() else ""
            val folder = when {
                rel.startsWith("Download") || data.contains("/Download/") -> "Download"
                rel.startsWith("Music") || data.contains("/Music/") -> "Música"
                data.isNotBlank() -> data.substringAfterLast("/", "").let {
                    // Carpeta padre como etiqueta (sin ruta completa).
                    val parent = data.substringBeforeLast("/", "")
                    parent.substringAfterLast("/", "").takeIf { it.isNotBlank() } ?: "Local"
                }
                else -> "Local"
            }
            val added = if (addedCol >= 0) c.getLong(addedCol) else 0L
            out.add(LocalTrack(uri, title, artist, duration, size, folder, added))
        }
    }
    // Download primero (lo que pidió Esse), luego por fecha (lo más nuevo).
    return out.sortedWith(
        compareByDescending<LocalTrack> { it.folder == "Download" }
            .thenByDescending { it.dateAdded }
    )
}

/** "3:45" — minutos:segundos. */
fun formatTrackDuration(ms: Long): String {
    val totalSec = (ms / 1000L).coerceAtLeast(0L)
    return "${totalSec / 60}:${(totalSec % 60).toString().padStart(2, '0')}"
}

/** "12.4 MB" — tamaño legible. */
fun formatTrackSize(bytes: Long): String {
    if (bytes < 1024L) return "$bytes B"
    val kb = bytes / 1024.0
    if (kb < 1024.0) return "%.1f KB".format(kb)
    return "%.1f MB".format(kb / 1024.0)
}

@Composable
fun LocalMusicScreen(
    player: IvannaBridgePlayer,
    onBack: () -> Unit = {}
) {
    val context = LocalContext.current
    val audioPermission = if (Build.VERSION.SDK_INT >= 33)
        Manifest.permission.READ_MEDIA_AUDIO
    else Manifest.permission.READ_EXTERNAL_STORAGE

    var hasPermission by remember {
        mutableStateOf(
            ContextCompat.checkSelfPermission(context, audioPermission) ==
                PackageManager.PERMISSION_GRANTED
        )
    }
    val permissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted -> hasPermission = granted }

    var tracks by remember { mutableStateOf<List<LocalTrack>>(emptyList()) }
    var loading by remember { mutableStateOf(false) }
    var query by remember { mutableStateOf("") }
    var filter by remember { mutableStateOf(LocalFilter.ALL) }
    var currentIndex by remember { mutableStateOf(-1) }

    // Estado del reproductor (flujos en vivo del BridgePlayer).
    val positionMs by player.currentPositionMs.collectAsState()
    val durationMs by player.durationMs.collectAsState()
    var playerState by remember { mutableStateOf(player.state) }
    // El State del BridgePlayer es un @Volatile var (no un Flow): se sondea.
    LaunchedEffect(Unit) {
        while (true) {
            playerState = player.state
            delay(500)
        }
    }
    // Avance automático de cola (fin de tema): actualiza el tema actual.
    DisposableEffect(player) {
        player.onQueueAdvance = { uri ->
            val idx = tracks.indexOfFirst { it.uri == uri }
            if (idx >= 0) currentIndex = idx
        }
        onDispose { player.onQueueAdvance = null }
    }

    // Carga la lista al tener permiso (y al volver a la pantalla).
    LaunchedEffect(hasPermission) {
        if (!hasPermission) return@LaunchedEffect
        loading = true
        val loaded = withContext(Dispatchers.IO) { loadLocalTracks(context) }
        tracks = loaded
        loading = false
    }

    val visible = remember(tracks, query, filter) {
        val q = query.trim().lowercase()
        tracks.filter { t ->
            val byFolder = when (filter) {
                LocalFilter.ALL -> true
                LocalFilter.DOWNLOAD -> t.folder == "Download"
                LocalFilter.MUSIC -> t.folder == "Música"
            }
            val byQuery = q.isBlank() ||
                t.title.lowercase().contains(q) || t.artist.lowercase().contains(q)
            byFolder && byQuery
        }
    }
    val currentTrack = tracks.getOrNull(currentIndex)
    val isPlaying = playerState == IvannaBridgePlayer.State.PLAYING

    fun playAt(index: Int) {
        if (index < 0 || index >= visible.size) return
        // La cola es la lista VISIBLE (respetando filtro/búsqueda).
        val uris = visible.map { it.uri }
        val globalIndex = tracks.indexOf(visible[index])
        currentIndex = globalIndex
        player.playQueue(uris, index)
    }

    Column(
        modifier = Modifier.fillMaxSize().background(ObsidianVoid)
            .padding(horizontal = 16.dp, vertical = 12.dp)
    ) {
        // ── Cabecera ───────────────────────────────────────────────────
        Row(verticalAlignment = Alignment.CenterVertically) {
            IconButton(onClick = onBack) {
                Icon(Icons.Filled.ArrowBack, contentDescription = "Atrás", tint = AuroraCyan)
            }
            Column(Modifier.weight(1f)) {
                Text("MÚSICA LOCAL", color = AuroraCyan, fontWeight = FontWeight.Bold, fontSize = 18.sp)
                Text(
                    "Download y Música del celular — el audio pasa por la cadena IVANNA al 100%",
                    color = TextMuted, fontSize = 12.sp
                )
            }
            Icon(Icons.Filled.MusicNote, contentDescription = null, tint = AuroraCyan)
        }
        Spacer(Modifier.height(8.dp))

        if (!hasPermission) {
            // ── Sin permiso: pedirlo ───────────────────────────────────
            Box(
                Modifier.fillMaxWidth().background(Color(0xFF0B1220), RoundedCornerShape(12.dp))
                    .padding(16.dp)
            ) {
                Column {
                    Text(
                        "IVANNA necesita permiso para leer tu música.",
                        color = Color.White, fontWeight = FontWeight.Bold
                    )
                    Spacer(Modifier.height(6.dp))
                    Text(
                        "Solo lee los archivos de audio (Download y Música) para listarlos y reproducirlos por la cadena IVANNA. Nada se sube ni se comparte.",
                        color = TextMuted, fontSize = 13.sp
                    )
                    Spacer(Modifier.height(12.dp))
                    Button(
                        onClick = { permissionLauncher.launch(audioPermission) },
                        colors = ButtonDefaults.buttonColors(containerColor = AuroraCyan)
                    ) {
                        Text("Permitir leer música", color = Color.Black, fontWeight = FontWeight.Bold)
                    }
                }
            }
            return@Column
        }

        // ── Búsqueda + filtros ─────────────────────────────────────────
        OutlinedTextField(
            value = query,
            onValueChange = { query = it },
            modifier = Modifier.fillMaxWidth(),
            placeholder = { Text("Buscar por título o artista…", color = TextMuted) },
            leadingIcon = { Icon(Icons.Filled.Search, contentDescription = null, tint = TextMuted) },
            singleLine = true
        )
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            FilterChip(
                selected = filter == LocalFilter.ALL,
                onClick = { filter = LocalFilter.ALL },
                label = { Text("Todo (${tracks.size})") },
                colors = FilterChipDefaults.filterChipColors(
                    selectedContainerColor = AuroraCyan,
                    selectedLabelColor = Color.Black
                )
            )
            FilterChip(
                selected = filter == LocalFilter.DOWNLOAD,
                onClick = { filter = LocalFilter.DOWNLOAD },
                label = { Text("Download (${tracks.count { it.folder == "Download" }})") },
                colors = FilterChipDefaults.filterChipColors(
                    selectedContainerColor = AuroraCyan,
                    selectedLabelColor = Color.Black
                )
            )
            FilterChip(
                selected = filter == LocalFilter.MUSIC,
                onClick = { filter = LocalFilter.MUSIC },
                label = { Text("Música (${tracks.count { it.folder == "Música" }})") },
                colors = FilterChipDefaults.filterChipColors(
                    selectedContainerColor = AuroraCyan,
                    selectedLabelColor = Color.Black
                )
            )
        }
        Spacer(Modifier.height(8.dp))

        if (loading) {
            Box(Modifier.fillMaxWidth().weight(1f), contentAlignment = Alignment.Center) {
                CircularProgressIndicator(color = AuroraCyan)
            }
        } else if (visible.isEmpty()) {
            Box(Modifier.fillMaxWidth().weight(1f), contentAlignment = Alignment.Center) {
                Column(horizontalAlignment = Alignment.CenterHorizontally) {
                    Text("No hay música aquí todavía.", color = Color.White, fontWeight = FontWeight.Bold)
                    Spacer(Modifier.height(6.dp))
                    Text(
                        if (query.isNotBlank() || filter != LocalFilter.ALL)
                            "Prueba otra búsqueda o filtro."
                        else
                            "Guarda archivos de audio (MP3, FLAC, WAV, M4A, OGG) en Download o Música y vuelve.",
                        color = TextMuted, fontSize = 13.sp
                    )
                }
            }
        } else {
            LazyColumn(
                modifier = Modifier.fillMaxWidth().weight(1f),
                verticalArrangement = Arrangement.spacedBy(6.dp)
            ) {
                itemsIndexed(visible, key = { _, t -> t.uri.toString() }) { idx, track ->
                    val isCurrent = tracks.indexOf(track) == currentIndex
                    Row(
                        modifier = Modifier.fillMaxWidth()
                            .background(
                                if (isCurrent) Color(0xFF12303A) else Color(0xFF0B1220),
                                RoundedCornerShape(10.dp)
                            )
                            .clickable { playAt(idx) }
                            .padding(horizontal = 12.dp, vertical = 10.dp),
                        verticalAlignment = Alignment.CenterVertically
                    ) {
                        Icon(
                            Icons.Filled.MusicNote,
                            contentDescription = null,
                            tint = if (isCurrent) AuroraCyan else TextMuted,
                            modifier = Modifier.size(22.dp)
                        )
                        Spacer(Modifier.width(10.dp))
                        Column(Modifier.weight(1f)) {
                            Text(
                                track.title,
                                color = if (isCurrent) AuroraCyan else Color.White,
                                fontWeight = if (isCurrent) FontWeight.Bold else FontWeight.Normal,
                                fontSize = 14.sp, maxLines = 1
                            )
                            Text(
                                "${track.artist} · ${track.folder} · ${formatTrackSize(track.sizeBytes)}",
                                color = TextMuted, fontSize = 12.sp, maxLines = 1
                            )
                        }
                        Spacer(Modifier.width(8.dp))
                        Text(formatTrackDuration(track.durationMs), color = TextMuted, fontSize = 12.sp)
                    }
                }
            }
        }

        // ── Ahora suena ────────────────────────────────────────────────
        if (currentTrack != null) {
            Spacer(Modifier.height(8.dp))
            Column(
                Modifier.fillMaxWidth().background(Color(0xFF0B1220), RoundedCornerShape(12.dp))
                    .padding(horizontal = 12.dp, vertical = 10.dp)
            ) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(currentTrack.title, color = Color.White, fontWeight = FontWeight.Bold, fontSize = 14.sp, maxLines = 1)
                        Text(
                            "${currentTrack.artist} · por la cadena IVANNA",
                            color = AuroraCyan, fontSize = 12.sp, maxLines = 1
                        )
                    }
                    Text(
                        "${formatTrackDuration(positionMs)} / ${formatTrackDuration(durationMs)}",
                        color = TextMuted, fontSize = 12.sp
                    )
                }
                Slider(
                    value = if (durationMs > 0L) positionMs.toFloat() / durationMs.toFloat() else 0f,
                    onValueChange = { frac ->
                        if (durationMs > 0L) player.seekTo((frac * durationMs).toLong())
                    },
                    modifier = Modifier.fillMaxWidth()
                )
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.Center,
                    verticalAlignment = Alignment.CenterVertically
                ) {
                    IconButton(onClick = {
                        if (player.skipToPrevious()) {
                            val prev = tracks.indexOf(currentTrack) - 1
                            if (prev >= 0) currentIndex = prev
                        }
                    }) {
                        Icon(Icons.Filled.SkipPrevious, contentDescription = "Anterior", tint = Color.White, modifier = Modifier.size(30.dp))
                    }
                    IconButton(onClick = {
                        if (isPlaying) player.pause() else player.resume()
                    }) {
                        Icon(
                            if (isPlaying) Icons.Filled.Pause else Icons.Filled.PlayArrow,
                            contentDescription = if (isPlaying) "Pausa" else "Reproducir",
                            tint = AuroraCyan, modifier = Modifier.size(38.dp)
                        )
                    }
                    IconButton(onClick = {
                        if (player.skipToNext()) {
                            val next = tracks.indexOf(currentTrack) + 1
                            if (next < tracks.size) currentIndex = next
                        }
                    }) {
                        Icon(Icons.Filled.SkipNext, contentDescription = "Siguiente", tint = Color.White, modifier = Modifier.size(30.dp))
                    }
                    IconButton(onClick = { player.stop(); currentIndex = -1 }) {
                        Icon(Icons.Filled.Stop, contentDescription = "Parar", tint = TextMuted, modifier = Modifier.size(26.dp))
                    }
                }
            }
        }
    }
}
