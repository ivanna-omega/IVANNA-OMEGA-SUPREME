package com.ivanna.omega.core

import android.content.Context
import android.util.Log
import com.ivanna.omega.BuildConfig
import com.ivanna.omega.agent.IvannaAgentCore
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicLong

/**
 * Privacy-minimized remote diagnostics. Starts locally at app boot but sends nothing
 * unless the owner explicitly enables it and configures an HTTPS endpoint + bearer token.
 * No audio, app names, hardware IDs, or repair commands are sent.
 */
object LiveDiagnosticConnector {
    private const val TAG = "IvannaDiagConnector"
    private const val PREFS = "ivanna_live_diagnostics"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_ENDPOINT = "endpoint"
    private const val KEY_TOKEN = "token"
    private const val PERIOD_MS = 15_000L

    // ── Latido PUSH F8 (v2.5.446) ──────────────────────────────────────────
    // Señal ligera "estoy vivo + salud mínima" al MISMO puente Floot que los
    // snapshots de diagnóstico (mismo endpoint, mismo opt-in, mismo token).
    // El latido NO lleva detalle: solo tipo, versión y 4 booleanos de salud.
    // Cadencia más lenta que el snapshot (60 s vs 15 s) y contador de
    // secuencia PROPIO, para no entrelazarlo con la secuencia de snapshots.
    const val HEARTBEAT_PERIOD_MS = 60_000L
    private val heartbeatSequence = AtomicLong(0)

    // Alcance del gateway OmniRoute: OmniRouteOrchestrator es una clase
    // instanciada dentro de IvannaGeminiAgent (no un singleton), así que el
    // conector no puede leer su salud directamente. El agente la reporta
    // aquí con reportGatewayReachable() desde su health-check (GET /v1/models).
    // Sin reporte, gatewayKnown = false y gatewayReachable = false, que
    // significa "sin evidencia de alcance", NO "gateway caído".
    @Volatile private var gatewayReachable = false
    @Volatile private var gatewayKnown = false

    /** Salud mínima que lleva el latido. Sin secretos, sin identificadores. */
    data class HeartbeatHealth(
        val gatewayReachable: Boolean,
        val gatewayKnown: Boolean,
        val dspAlive: Boolean,
        val sessionActive: Boolean
    )

    // v2.5.443 — Puente de diagnóstico en Floot (verificado 2026-11-14):
    // https://ivanna-diagnostic-bridge.floot.app — el prefijo /_api es
    // obligatorio. El contrato es EL MISMO que el puente de Render (mismo
    // body, mismas respuestas): GET /_api/healthz, POST /_api/v1/diagnostics
    // (Bearer si hay token), GET /_api/v1/diagnostics/latest,
    // GET /_api/v1/diagnostics/recent, POST /_api/v1/diagnostics/analyze.
    // El análisis en Floot es por REGLAS en español (no LLM; Pollinations
    // no responde a tiempo desde Floot).
    const val DEFAULT_ENDPOINT: String =
        "https://ivanna-diagnostic-bridge.floot.app/_api/v1/diagnostics"
    // URL vieja de Render (el puente vivía en el plan free de Render).
    // Si un teléfono la tiene guardada, se migra sola a Floot (ver
    // resolveEndpoint): Esse no tiene que re-escribir nada.
    const val LEGACY_RENDER_ENDPOINT: String =
        "https://ivanna-diagnostic-bridge.onrender.com/v1/diagnostics"

    /**
     * Resuelve el endpoint efectivo. Pura (testeable en host).
     *   - vacío/nulo → el default de Floot.
     *   - la URL vieja de Render → el default de Floot (migración automática).
     *   - cualquier otra → la guardada (sin barra final).
     */
    fun resolveEndpoint(stored: String?): String {
        val normalized = stored?.trim()?.trimEnd('/') ?: ""
        if (normalized.isEmpty()) return DEFAULT_ENDPOINT
        if (normalized == LEGACY_RENDER_ENDPOINT) return DEFAULT_ENDPOINT
        return normalized
    }
    private val sequence = AtomicLong(0)
    @Volatile private var started = false
    private var scope: CoroutineScope? = null
    private lateinit var appContext: Context

    fun start(context: Context) {
        if (started) return
        started = true
        appContext = context.applicationContext
        scope = CoroutineScope(SupervisorJob() + Dispatchers.IO).also { s ->
            s.launch {
                while (isActive) {
                    runCatching { uploadIfConfigured() }
                        .onFailure { Log.w(TAG, "Diagnostic upload skipped: ${it.javaClass.simpleName}") }
                    delay(PERIOD_MS)
                }
            }
            // Latido PUSH F8: misma corrutina de vida, cadencia propia (60 s).
            // Fail-closed: cualquier fallo se registra y se sigue; jamás se
            // reintenta en bucle ni se toca el hilo de audio.
            s.launch {
                while (isActive) {
                    runCatching { uploadHeartbeatIfConfigured() }
                        .onFailure { Log.w(TAG, "Heartbeat skipped: ${it.javaClass.simpleName}") }
                    delay(HEARTBEAT_PERIOD_MS)
                }
            }
        }
        Log.i(TAG, "Local diagnostic connector initialized; remote upload is opt-in")
    }

    /**
     * Reporta el alcance del gateway OmniRoute (llamar desde el health-check
     * de IvannaGeminiAgent / OmniRouteOrchestrator tras cada GET /v1/models).
     * Es el único insumo del latido que no sale del estado del AgentCore.
     */
    fun reportGatewayReachable(reachable: Boolean) {
        gatewayReachable = reachable
        gatewayKnown = true
    }

    /** Salud mínima actual, leída del estado en memoria del AgentCore. Pura. */
    fun collectHeartbeatHealth(): HeartbeatHealth {
        val agent = IvannaAgentCore.state.value
        return HeartbeatHealth(
            gatewayReachable = gatewayReachable,
            gatewayKnown = gatewayKnown,
            dspAlive = agent.health.adaptiveEngineRunning,
            sessionActive = agent.perception.playbackActive
        )
    }

    /**
     * Construye el latido. Pura (testeable en host): la versión, la salud y
     * los contadores entran por parámetro; nada secreto entra nunca.
     * Formato exacto:
     *   {"type":"heartbeat","schemaVersion":1,"sequence":N,"timestampMs":<epoch>,
     *    "uptimeMs":<elapsedRealtime>,"appVersion":"2.5.x",
     *    "health":{"gatewayReachable":bool,"gatewayKnown":bool,
     *             "dspAlive":bool,"sessionActive":bool}}
     */
    fun buildHeartbeat(
        appVersion: String,
        health: HeartbeatHealth,
        sequence: Long,
        uptimeMs: Long,
        timestampMs: Long = System.currentTimeMillis()
    ): JSONObject = JSONObject().apply {
        put("type", "heartbeat")
        put("schemaVersion", 1)
        put("sequence", sequence)
        put("timestampMs", timestampMs)
        put("uptimeMs", uptimeMs)
        put("appVersion", appVersion)
        put("health", JSONObject().apply {
            put("gatewayReachable", health.gatewayReachable)
            put("gatewayKnown", health.gatewayKnown)
            put("dspAlive", health.dspAlive)
            put("sessionActive", health.sessionActive)
        })
    }

    /**
     * Call only from an explicit user-controlled settings action.
     * Endpoint must be HTTPS; token remains in private app preferences and is never logged.
     */
    fun configure(context: Context, enabled: Boolean, endpoint: String, bearerToken: String): Boolean {
        val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val effectiveToken = bearerToken.ifBlank { prefs.getString(KEY_TOKEN, "") ?: "" }
        // v2.5.443 — si se guarda la URL vieja de Render, se guarda Floot
        // en su lugar (migración; el contrato es el mismo).
        val normalized = resolveEndpoint(endpoint.trim().trimEnd('/'))
        val valid = runCatching {
            val url = URL(normalized)
            url.protocol.equals("https", ignoreCase = true) &&
                url.host.isNotBlank() && url.userInfo == null &&
                normalized.endsWith("/v1/diagnostics")
        }.getOrDefault(false)
        if (enabled && (!valid || effectiveToken.length < 32)) return false
        prefs.edit()
            .putBoolean(KEY_ENABLED, enabled)
            .putString(KEY_ENDPOINT, normalized)
            .putString(KEY_TOKEN, effectiveToken)
            .apply()
        return true
    }

    fun isEnabled(context: Context): Boolean =
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(KEY_ENABLED, false)

    /** Endpoint efectivo (resuelto: default Floot si no hay nada o si era Render). */
    fun configuredEndpoint(context: Context): String =
        resolveEndpoint(
            context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getString(KEY_ENDPOINT, null)
        )

    fun disable(context: Context) {
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putBoolean(KEY_ENABLED, false).remove(KEY_TOKEN).apply()
    }

    /** Requests an on-demand AI analysis of the latest uploaded snapshot. Recommendations only. */
    fun requestAnalysis(context: Context, onResult: (String) -> Unit) {
        val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(KEY_ENABLED, false)) {
            onResult("Activa primero el envío remoto de diagnósticos.")
            return
        }
        val endpoint = resolveEndpoint(prefs.getString(KEY_ENDPOINT, null))
        val token = prefs.getString(KEY_TOKEN, null) ?: run {
            onResult("Falta configurar el token de diagnóstico.")
            return
        }
        val worker = scope ?: run {
            onResult("El conector local todavía no está iniciado.")
            return
        }
        worker.launch {
            val result = runCatching {
                val analysisUrl = endpoint.trimEnd('/') + "/analyze"
                val connection = (URL(analysisUrl).openConnection() as HttpURLConnection)
                try {
                    connection.requestMethod = "POST"
                    connection.connectTimeout = 5_000
                    connection.readTimeout = 15_000
                    connection.doOutput = true
                    connection.setRequestProperty("Authorization", "Bearer $token")
                    connection.setRequestProperty("Content-Type", "application/json")
                    connection.outputStream.use { it.write(byteArrayOf()) }
                    val status = connection.responseCode
                    val stream = if (status in 200..299) connection.inputStream else connection.errorStream
                    val text = stream?.bufferedReader(Charsets.UTF_8)?.use { it.readText() }.orEmpty()
                    val json = runCatching { JSONObject(text) }.getOrNull()
                    if (status in 200..299) json?.optString("analysis")?.takeIf { it.isNotBlank() }
                        ?: "El servicio no devolvió un análisis legible."
                    else when (json?.optString("error")) {
                        "ai_not_configured" -> "El servidor no tiene disponible el análisis por reglas."
                        "no_snapshot_yet" -> "Todavía no hay una telemetría recibida; espera un ciclo de envío."
                        else -> "No se pudo obtener el análisis (HTTP $status)."
                    }
                } finally {
                    connection.disconnect()
                }
            }.getOrElse { "No se pudo conectar al servicio de análisis (${it.javaClass.simpleName})." }
            withContext(Dispatchers.Main) { onResult(result) }
        }
    }

    /**
     * Latido PUSH F8: POST al mismo endpoint del puente Floot que el snapshot.
     * Misma puerta opt-in (enabled + HTTPS + token ≥ 32). Fail-closed: si el
     * puente no responde, no hay reintento ni bloqueo; solo se registra.
     * No se llama nunca desde el hilo de audio (corre en Dispatchers.IO).
     */
    private fun uploadHeartbeatIfConfigured() {
        val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(KEY_ENABLED, false)) return
        val endpoint = resolveEndpoint(prefs.getString(KEY_ENDPOINT, null))
        val token = prefs.getString(KEY_TOKEN, null) ?: return
        if (!endpoint.startsWith("https://") || token.length < 32) return

        val body = buildHeartbeat(
            appVersion = BuildConfig.VERSION_NAME,
            health = collectHeartbeatHealth(),
            sequence = heartbeatSequence.incrementAndGet(),
            uptimeMs = android.os.SystemClock.elapsedRealtime()
        )

        val connection = (URL(endpoint).openConnection() as HttpURLConnection)
        try {
            connection.requestMethod = "POST"
            connection.connectTimeout = 4_000
            connection.readTimeout = 4_000
            connection.doOutput = true
            connection.setRequestProperty("Authorization", "Bearer $token")
            connection.setRequestProperty("Content-Type", "application/json")
            connection.setRequestProperty("Cache-Control", "no-store")
            connection.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
            val status = connection.responseCode
            if (status in 200..299) Log.d(TAG, "Heartbeat accepted (HTTP $status)")
            else Log.w(TAG, "Heartbeat rejected by endpoint (HTTP $status)")
        } finally {
            connection.disconnect()
        }
    }

    private fun uploadIfConfigured() {
        val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean(KEY_ENABLED, false)) return
        val endpoint = resolveEndpoint(prefs.getString(KEY_ENDPOINT, null))
        val token = prefs.getString(KEY_TOKEN, null) ?: return
        if (!endpoint.startsWith("https://") || token.length < 32) return

        val metrics = OEMTelemetry.metrics.value
        val agent = IvannaAgentCore.state.value
        val health = agent.health
        val body = JSONObject().apply {
            put("schemaVersion", 1)
            put("sequence", sequence.incrementAndGet())
            put("uptimeMs", android.os.SystemClock.elapsedRealtime())
            put("metrics", JSONObject().apply {
                put("dspProcessingUs", metrics.dspProcessingUs)
                put("frameDrops", metrics.frameDrops)
                put("underruns", metrics.underruns)
                put("thermalThrottling", metrics.thermalThrottling)
                put("memoryPressure", when (metrics.memoryPressure.lowercase()) {
                    "normal", "moderate", "high" -> metrics.memoryPressure.lowercase()
                    else -> "unknown"
                })
            })
            put("agent", JSONObject().apply {
                put("running", agent.running)
                put("daemonConnected", health.daemonConnected)
                put("adaptiveEngineRunning", health.adaptiveEngineRunning)
                put("clipping", health.clipping)
                put("latencyDegraded", health.latencyDegraded)
                put("clipCount", health.clipCount)
                put("roundTripLatencyUs", health.roundTripLatencyUs)
            })
        }

        val connection = (URL(endpoint).openConnection() as HttpURLConnection)
        try {
            connection.requestMethod = "POST"
            connection.connectTimeout = 4_000
            connection.readTimeout = 4_000
            connection.doOutput = true
            connection.setRequestProperty("Authorization", "Bearer $token")
            connection.setRequestProperty("Content-Type", "application/json")
            connection.setRequestProperty("Cache-Control", "no-store")
            connection.outputStream.use { it.write(body.toString().toByteArray(Charsets.UTF_8)) }
            val status = connection.responseCode
            if (status in 200..299) Log.d(TAG, "Diagnostic snapshot accepted (HTTP $status)")
            else Log.w(TAG, "Diagnostic endpoint rejected snapshot (HTTP $status)")
        } finally {
            connection.disconnect()
        }
    }

    fun shutdown() {
        scope?.cancel()
        scope = null
        started = false
    }
}
