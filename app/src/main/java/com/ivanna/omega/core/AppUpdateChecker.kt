package com.ivanna.omega.core

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.Log
import androidx.core.content.FileProvider
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.MessageDigest

/**
 * Revisor de actualizaciones en la app (v2.5.444).
 *
 * La app se distribuye por GitHub releases (APK), NO está en Play Store.
 * El revisor consulta el latest release de GitHub, compara con la versión
 * instalada, descarga la APK con progreso y lanza el instalador del
 * sistema para que el usuario confirme con UN toque.
 *
 * NO hay instalación silenciosa: Android no lo permite para una app
 * normal y no se intenta por vías raras. Tampoco hay carga dinámica de
 * código para auto-actualizar (eso sería auto-modificación y un riesgo
 * de seguridad). El instalador del sistema verifica la firma: misma
 * keystore = la actualización instala ENCIMA, sin desinstalar.
 *
 * Fail-closed: sin red, servidor sin respuesta, JSON inválido o descarga
 * corrupta/incompleta → la app sigue normal, sin diálogo, sin crash y
 * sin bloquear. La descarga se verifica (tamaño completo y SHA-256
 * cuando el release publica SHA256SUMS.txt) antes de ofrecer instalar.
 *
 * La lógica pura vive en funciones sin Context (testeable en host);
 * la red y las prefs viven en funciones aparte.
 */
object AppUpdateChecker {
    private const val TAG = "IvannaAppUpdate"

    /** Latest release del repo público (fuente de verdad de versiones). */
    const val LATEST_RELEASE_URL: String =
        "https://api.github.com/repos/luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME/releases/latest"

    private const val PREFS = "ivanna_app_update"
    private const val KEY_LAST_CHECK_MS = "last_check_ms"
    private const val KEY_DISMISSED_TAG = "dismissed_tag"
    private const val KEY_DISMISSED_UNTIL_MS = "dismissed_until_ms"

    /** Revisa como mucho una vez al día (no spamear al servidor ni al usuario). */
    const val CHECK_INTERVAL_MS: Long = 24L * 60L * 60L * 1000L
    const val CONNECT_TIMEOUT_MS: Int = 10_000
    const val READ_TIMEOUT_MS: Int = 30_000
    const val APK_DIR: String = "updates"
    const val SHA256SUMS_NAME: String = "SHA256SUMS.txt"

    data class ReleaseInfo(
        val tag: String,
        val versionName: String,
        val name: String,
        val body: String,
        val apkUrl: String,
        val apkName: String,
        val apkSize: Long,
        val sha256Url: String?,
    )

    sealed interface CheckResult {
        data object NoUpdate : CheckResult
        data class UpdateAvailable(val release: ReleaseInfo) : CheckResult
        /** Sin red / servidor no responde / JSON inválido: fail-closed, silencioso. */
        data object Unavailable : CheckResult
    }

    data class UpdateState(
        val lastCheckMs: Long,
        val dismissedTag: String?,
        val dismissedUntilMs: Long,
    )

    // ---------- Lógica pura (testeable en host, sin Context) ----------

    /** "v2.5.444" -> "2.5.444"; sin "v" se devuelve tal cual. */
    fun parseVersionName(tag: String): String = tag.trim().removePrefix("v")

    /**
     * Compara dos nombres de versión numéricos ("2.5.443" vs "2.5.444").
     * Negativo si a < b, 0 si iguales, positivo si a > b. Segmentos no
     * numéricos se tratan como 0 (fail-closed, nunca rompe).
     */
    fun compareVersions(a: String, b: String): Int {
        val pa = parseVersionName(a).split(".")
        val pb = parseVersionName(b).split(".")
        val n = maxOf(pa.size, pb.size)
        for (i in 0 until n) {
            val va = pa.getOrNull(i)?.toIntOrNull() ?: 0
            val vb = pb.getOrNull(i)?.toIntOrNull() ?: 0
            if (va != vb) return va.compareTo(vb)
        }
        return 0
    }

    /** true si el tag del release es más nuevo que la versión instalada. */
    fun isNewer(latestTag: String, currentVersionName: String): Boolean =
        compareVersions(latestTag, currentVersionName) > 0

    /**
     * Parsea el JSON de GitHub `GET /repos/{owner}/{repo}/releases/latest`.
     * Devuelve null si falta el tag, no hay asset APK, o el JSON es inválido
     * (fail-closed: la app sigue normal).
     * El asset APK es el que empieza por "ivanna-omega-supreme-" y termina
     * en ".apk". El SHA256 es opcional: se busca el asset "SHA256SUMS.txt".
     */
    fun parseLatestRelease(json: String): ReleaseInfo? {
        return try {
            val root = JSONObject(json)
            val tag = root.optString("tag_name", "").trim()
            if (tag.isEmpty()) return null
            val name = root.optString("name", tag)
            val body = root.optString("body", "")
            val assets = root.optJSONArray("assets") ?: return null
            var apkUrl: String? = null
            var apkName: String? = null
            var apkSize = 0L
            var sha256Url: String? = null
            for (i in 0 until assets.length()) {
                val a = assets.optJSONObject(i) ?: continue
                val aName = a.optString("name", "")
                val url = a.optString("browser_download_url", "")
                if (url.isEmpty()) continue
                if (aName == SHA256SUMS_NAME) sha256Url = url
                if (aName.startsWith("ivanna-omega-supreme-") && aName.endsWith(".apk")) {
                    apkUrl = url
                    apkName = aName
                    apkSize = a.optLong("size", 0L)
                }
            }
            if (apkUrl == null || apkName == null) return null
            ReleaseInfo(tag, parseVersionName(tag), name, body, apkUrl, apkName, apkSize, sha256Url)
        } catch (e: Exception) {
            Log.w(TAG, "parseLatestRelease falló: ${e.javaClass.simpleName}")
            null
        }
    }

    /**
     * Extrae el SHA256 esperado de un SHA256SUMS.txt para un nombre de archivo.
     * Formato estándar sha256sum: "<sha256>  <nombre>" (dos espacios) o
     * "<sha256> *<nombre>" (modo binario), con o sin ruta delante del nombre.
     * Puro, testeable. Devuelve null si no está.
     */
    fun parseSha256ForFile(sha256sumsText: String, fileName: String): String? {
        for (raw in sha256sumsText.lines()) {
            val line = raw.trim()
            if (line.isEmpty()) continue
            val parts = line.split(Regex("\\s+"))
            if (parts.size < 2) continue
            val hash = parts[0].lowercase()
            val name = parts.last().substringAfterLast('/').removePrefix("*")
            if (hash.length == 64 && hash.all { it in '0'..'9' || it in 'a'..'f' } && name == fileName) {
                return hash
            }
        }
        return null
    }

    /** SHA-256 hex (minúsculas) de un arreglo de bytes. Puro. */
    fun sha256Hex(bytes: ByteArray): String {
        val md = MessageDigest.getInstance("SHA-256")
        return md.digest(bytes).joinToString("") { "%02x".format(it) }
    }

    /**
     * Verifica una descarga: tamaño completo y, si se conoce el SHA256
     * esperado, que coincida. Puro. Si no se conoce el tamaño esperado
     * (<= 0) no se exige; si no se conoce el SHA256 esperado, no se exige.
     * Devuelve false si el tamaño no coincide o el hash (cuando se da)
     * no coincide.
     */
    fun verifyDownload(
        actualSize: Long,
        expectedSize: Long,
        actualSha256: String?,
        expectedSha256: String?,
    ): Boolean {
        if (expectedSize > 0 && actualSize != expectedSize) return false
        if (expectedSha256 != null) {
            if (actualSha256 == null) return false
            if (!actualSha256.equals(expectedSha256, ignoreCase = true)) return false
        }
        return true
    }

    /**
     * ¿Toca revisar? Puro. Revisa como mucho una vez al día y respeta el
     * "Más tarde" (no molestar hasta dismissedUntilMs).
     */
    fun shouldCheck(nowMs: Long, lastCheckMs: Long, dismissedUntilMs: Long): Boolean {
        if (nowMs < dismissedUntilMs) return false
        if (lastCheckMs > 0 && nowMs - lastCheckMs < CHECK_INTERVAL_MS) return false
        return true
    }

    // ---------- Prefs (almacenamiento de la app) ----------

    private fun prefs(context: Context) =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    fun loadState(context: Context): UpdateState {
        val p = prefs(context)
        return UpdateState(
            lastCheckMs = p.getLong(KEY_LAST_CHECK_MS, 0L),
            dismissedTag = p.getString(KEY_DISMISSED_TAG, null),
            dismissedUntilMs = p.getLong(KEY_DISMISSED_UNTIL_MS, 0L),
        )
    }

    fun markChecked(context: Context, nowMs: Long) {
        prefs(context).edit().putLong(KEY_LAST_CHECK_MS, nowMs).apply()
    }

    /** "Más tarde": no volver a molestar con esta versión hasta el próximo día. */
    fun dismissUntilTomorrow(context: Context, tag: String, nowMs: Long) {
        prefs(context).edit()
            .putString(KEY_DISMISSED_TAG, tag)
            .putLong(KEY_DISMISSED_UNTIL_MS, nowMs + CHECK_INTERVAL_MS)
            .apply()
    }

    // ---------- Red (IO) ----------

    private fun httpGetText(url: String, userAgent: String): String? {
        var conn: HttpURLConnection? = null
        return try {
            conn = (URL(url).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                setRequestProperty("User-Agent", userAgent)
                setRequestProperty("Accept", "application/vnd.github+json")
                connectTimeout = CONNECT_TIMEOUT_MS
                readTimeout = READ_TIMEOUT_MS
            }
            if (conn.responseCode != HttpURLConnection.HTTP_OK) return null
            conn.inputStream.bufferedReader().use { it.readText() }
        } catch (e: Exception) {
            Log.w(TAG, "httpGet falló ($url): ${e.javaClass.simpleName}")
            null
        } finally {
            conn?.disconnect()
        }
    }

    /**
     * Consulta el latest release y compara con la versión instalada.
     * Fail-closed: cualquier fallo de red/servidor/parseo devuelve
     * Unavailable (la app sigue normal, sin diálogo ni crash).
     * GitHub API exige User-Agent; sin él responde 403.
     */
    suspend fun checkForUpdate(currentVersionName: String): CheckResult =
        withContext(Dispatchers.IO) {
            val ua = "IVANNA-AppUpdate/$currentVersionName (Android; com.ivanna.omega)"
            val json = httpGetText(LATEST_RELEASE_URL, ua) ?: return@withContext CheckResult.Unavailable
            val release = parseLatestRelease(json) ?: return@withContext CheckResult.Unavailable
            if (isNewer(release.tag, currentVersionName)) CheckResult.UpdateAvailable(release)
            else CheckResult.NoUpdate
        }

    /**
     * Descarga el SHA256SUMS.txt del release (si el release lo publica) y
     * extrae el SHA256 esperado del APK. Devuelve null si no hay asset
     * SHA256SUMS, no responde, o no contiene el APK (en ese caso la
     * verificación es solo por tamaño + firma del instalador).
     */
    suspend fun fetchExpectedSha256(release: ReleaseInfo, currentVersionName: String): String? =
        withContext(Dispatchers.IO) {
            val url = release.sha256Url ?: return@withContext null
            val ua = "IVANNA-AppUpdate/$currentVersionName (Android; com.ivanna.omega)"
            val text = httpGetText(url, ua) ?: return@withContext null
            parseSha256ForFile(text, release.apkName)
        }

    /**
     * Descarga la APK a cacheDir/updates/<apkName> con progreso (0.0..1.0).
     * Devuelve el File o null si falla. Si ya existe un archivo con el
     * tamaño esperado, lo reutiliza (evita bajar ~210 MB otra vez).
     * Si la descarga falla a medias, borra el archivo parcial.
     */
    suspend fun downloadApk(
        context: Context,
        release: ReleaseInfo,
        onProgress: (Float) -> Unit,
    ): File? = withContext(Dispatchers.IO) {
        val dir = File(context.cacheDir, APK_DIR).apply { mkdirs() }
        val outFile = File(dir, release.apkName)
        if (outFile.exists() && release.apkSize > 0 && outFile.length() == release.apkSize) {
            onProgress(1f)
            return@withContext outFile
        }
        var conn: HttpURLConnection? = null
        try {
            conn = (URL(release.apkUrl).openConnection() as HttpURLConnection).apply {
                requestMethod = "GET"
                setRequestProperty("User-Agent", "IVANNA-AppUpdate (Android; com.ivanna.omega)")
                connectTimeout = CONNECT_TIMEOUT_MS
                readTimeout = 60_000
            }
            if (conn.responseCode != HttpURLConnection.HTTP_OK) return@withContext null
            val total = conn.contentLengthLong.takeIf { it > 0 } ?: release.apkSize
            var read = 0L
            conn.inputStream.use { input ->
                FileOutputStream(outFile).use { output ->
                    val buf = ByteArray(64 * 1024)
                    while (true) {
                        val n = input.read(buf)
                        if (n < 0) break
                        output.write(buf, 0, n)
                        read += n
                        if (total > 0) onProgress((read.toFloat() / total.toFloat()).coerceIn(0f, 1f))
                    }
                }
            }
            onProgress(1f)
            outFile
        } catch (e: Exception) {
            Log.w(TAG, "downloadApk falló: ${e.javaClass.simpleName}")
            runCatching { outFile.delete() }
            null
        } finally {
            conn?.disconnect()
        }
    }

    /** SHA-256 hex de un archivo. Null si no se puede leer. */
    fun sha256HexOfFile(file: File): String? {
        return try {
            val md = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buf = ByteArray(64 * 1024)
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    md.update(buf, 0, n)
                }
            }
            md.digest().joinToString("") { "%02x".format(it) }
        } catch (e: Exception) {
            Log.w(TAG, "sha256HexOfFile falló: ${e.javaClass.simpleName}")
            null
        }
    }

    /**
     * Verifica un APK descargado: tamaño completo y, si se conoce el SHA256
     * esperado, que coincida. Si no pasa, borra el archivo (no se ofrece
     * instalar nada corrupto).
     */
    fun verifyApkFile(file: File, release: ReleaseInfo, expectedSha256: String?): Boolean {
        val actualSha = expectedSha256?.let { sha256HexOfFile(file) }
        val ok = verifyDownload(
            actualSize = file.length(),
            expectedSize = release.apkSize,
            actualSha256 = actualSha,
            expectedSha256 = expectedSha256,
        )
        if (!ok) runCatching { file.delete() }
        return ok
    }

    // ---------- Instalación ----------

    /** ¿La app tiene permiso para instalar apps desconocidas? (Android 8+). */
    fun canRequestPackageInstalls(context: Context): Boolean {
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            context.packageManager.canRequestPackageInstalls()
        } else {
            true
        }
    }

    /** Intent a Ajustes para activar "Permitir instalar apps desconocidas" para IVANNA. */
    fun unknownSourcesSettingsIntent(context: Context): Intent =
        Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES).apply {
            data = Uri.parse("package:${context.packageName}")
            addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        }

    /**
     * Intent del instalador del sistema para un APK descargado.
     * Usa FileProvider (el instalador verifica la firma: misma keystore
     * = actualiza encima, sin desinstalar). Null si no se puede construir.
     */
    fun installIntent(context: Context, apkFile: File): Intent? {
        return try {
            val uri = FileProvider.getUriForFile(
                context, "${context.packageName}.fileprovider", apkFile
            )
            Intent(Intent.ACTION_VIEW).apply {
                setDataAndType(uri, "application/vnd.android.package-archive")
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
            }
        } catch (e: Exception) {
            Log.w(TAG, "installIntent falló: ${e.javaClass.simpleName}")
            null
        }
    }
}
