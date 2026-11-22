package com.ivanna.omega.ui

import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import com.ivanna.omega.BuildConfig
import com.ivanna.omega.core.AppUpdateChecker
import kotlinx.coroutines.launch

/**
 * Revisor de actualizaciones (v2.5.444): diálogo "Nueva versión disponible"
 * + host que revisa al abrir la app (como mucho una vez al día).
 *
 * El diálogo es lo único que ve el usuario: versión nueva, qué trae,
 * y botones Instalar / Más tarde. "Más tarde" no vuelve a molestar
 * hasta el próximo día. Sin red o sin versión nueva: silencioso,
 * fail-closed (la app sigue normal).
 */
@Composable
fun AppUpdateHost() {
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    var release by remember { mutableStateOf<AppUpdateChecker.ReleaseInfo?>(null) }
    var downloading by remember { mutableStateOf(false) }
    var progress by remember { mutableFloatStateOf(0f) }
    var downloadFailed by remember { mutableStateOf(false) }
    var needUnknownSources by remember { mutableStateOf(false) }

    LaunchedEffect(Unit) {
        val state = AppUpdateChecker.loadState(context)
        val now = System.currentTimeMillis()
        if (!AppUpdateChecker.shouldCheck(now, state.lastCheckMs, state.dismissedUntilMs)) {
            return@LaunchedEffect
        }
        AppUpdateChecker.markChecked(context, now)
        val result = runCatching { AppUpdateChecker.checkForUpdate(BuildConfig.VERSION_NAME) }
            .getOrNull() ?: return@LaunchedEffect
        if (result is AppUpdateChecker.CheckResult.UpdateAvailable) {
            // Si el usuario ya dijo "Más tarde" a esta versión hoy, no insistir.
            if (state.dismissedTag == result.release.tag && now < state.dismissedUntilMs) {
                return@LaunchedEffect
            }
            release = result.release
        }
        // NoUpdate / Unavailable: silencioso, fail-closed.
    }

    val current = release
    if (current != null) {
        AppUpdateDialog(
            release = current,
            downloading = downloading,
            progress = progress,
            downloadFailed = downloadFailed,
            needUnknownSources = needUnknownSources,
            onInstall = {
                needUnknownSources = false
                downloadFailed = false
                if (!AppUpdateChecker.canRequestPackageInstalls(context)) {
                    // Android 8+: hay que activar "Permitir instalar apps
                    // desconocidas" para IVANNA antes de poder instalar.
                    needUnknownSources = true
                    return@AppUpdateDialog
                }
                downloading = true
                progress = 0f
                scope.launch {
                    val expectedSha = runCatching {
                        AppUpdateChecker.fetchExpectedSha256(current, BuildConfig.VERSION_NAME)
                    }.getOrNull()
                    val file = AppUpdateChecker.downloadApk(context, current) { p -> progress = p }
                    downloading = false
                    if (file == null) {
                        downloadFailed = true
                        return@launch
                    }
                    if (!AppUpdateChecker.verifyApkFile(file, current, expectedSha)) {
                        downloadFailed = true
                        return@launch
                    }
                    val intent = AppUpdateChecker.installIntent(context, file)
                    if (intent != null) {
                        // El instalador del sistema verifica la firma: misma
                        // keystore = actualiza encima, sin desinstalar.
                        release = null
                        context.startActivity(intent)
                    } else {
                        downloadFailed = true
                    }
                }
            },
            onOpenUnknownSources = {
                context.startActivity(AppUpdateChecker.unknownSourcesSettingsIntent(context))
                needUnknownSources = false
            },
            onLater = {
                AppUpdateChecker.dismissUntilTomorrow(
                    context, current.tag, System.currentTimeMillis()
                )
                release = null
            },
            onDismiss = { release = null },
        )
    }
}

/** Notas del release recortadas para el diálogo (el body puede ser largo). */
internal fun AppUpdateChecker.ReleaseInfo.shortNotes(maxChars: Int = 600): String {
    val clean = body.trim()
    if (clean.isEmpty()) return "Mejoras y correcciones."
    return if (clean.length <= maxChars) clean else clean.take(maxChars).trimEnd() + "…"
}

@Composable
fun AppUpdateDialog(
    release: AppUpdateChecker.ReleaseInfo,
    downloading: Boolean,
    progress: Float,
    downloadFailed: Boolean,
    needUnknownSources: Boolean,
    onInstall: () -> Unit,
    onOpenUnknownSources: () -> Unit,
    onLater: () -> Unit,
    onDismiss: () -> Unit,
) {
    val sizeMb = if (release.apkSize > 0) {
        "%.0f MB".format(release.apkSize / 1_000_000.0)
    } else {
        ""
    }
    AlertDialog(
        onDismissRequest = { if (!downloading) onDismiss() },
        title = { Text("Nueva versión disponible") },
        text = {
            Column(modifier = Modifier.fillMaxWidth()) {
                Text("IVANNA ${release.versionName} (${release.tag})" + if (sizeMb.isNotEmpty()) " · $sizeMb" else "")
                Spacer(modifier = Modifier.height(8.dp))
                Text(release.shortNotes())
                if (needUnknownSources) {
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(
                        "Para instalarla, activa \"Permitir instalar apps desconocidas\" " +
                            "para IVANNA en Ajustes y vuelve a tocar Instalar."
                    )
                }
                if (downloading) {
                    Spacer(modifier = Modifier.height(12.dp))
                    LinearProgressIndicator(
                        progress = { progress },
                        modifier = Modifier.fillMaxWidth(),
                    )
                    Spacer(modifier = Modifier.height(4.dp))
                    Text("Descargando… ${(progress * 100).toInt()}%")
                }
                if (downloadFailed) {
                    Spacer(modifier = Modifier.height(12.dp))
                    Text(
                        "La descarga falló o no se pudo verificar. Revisa tu conexión " +
                            "e inténtalo de nuevo."
                    )
                }
            }
        },
        confirmButton = {
            if (needUnknownSources) {
                TextButton(onClick = onOpenUnknownSources) { Text("Abrir ajustes") }
            } else {
                TextButton(onClick = onInstall, enabled = !downloading) {
                    Text(if (downloading) "Descargando…" else "Instalar")
                }
            }
        },
        dismissButton = {
            TextButton(onClick = onLater, enabled = !downloading) { Text("Más tarde") }
        },
    )
}
