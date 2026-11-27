package com.ivanna.omega.organism

import android.util.Log
import com.google.gson.JsonObject
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.util.concurrent.TimeUnit

/**
 * Logger mínimo inyectable. El sender NO depende de android.util.Log
 * directo: así los tests corren en JVM plana (sin Robolectric) y el
 * loopback del stub no depende del sandbox. En la app se usa el logger
 * Android real; en tests, un no-op o un recolector.
 */
interface OrganismLogger {
    fun i(message: String)
    fun w(message: String)
}

class AndroidOrganismLogger(private val tag: String) : OrganismLogger {
    override fun i(message: String) {
        Log.i(tag, message)
    }

    override fun w(message: String) {
        Log.w(tag, message)
    }
}

class NoOpOrganismLogger : OrganismLogger {
    override fun i(message: String) = Unit
    override fun w(message: String) = Unit
}

/**
 * OrganismSender — POST HTTPS al organismo, desde Kotlin (v2.5.506).
 *
 * C++ solo entrega snapshots a una JNI mínima; TODO el transporte vive
 * aquí: OkHttp + cola durable + backoff. Nunca en el hilo de audio ni
 * en la UI (el controlador lo corre en Dispatchers.IO).
 *
 * DESTINO (verificado contra el conector Muselink):
 *   POST /api/v1/muse/ivanna/session
 *   host primario: https://muselink.ai.studio
 *   host alterno:  https://muselink-667333806336.us-west1.run.app
 *   auth: Bearer <token del almacén cifrado> (nunca en logs).
 *
 * RESULTADO del envío (el controlador decide la transición de la cola):
 *   Sent        2xx: el organismo aceptó el chunk.
 *   Retry       red caída, timeout, 429, 5xx: reintento con backoff.
 *   Quarantine  400/403/404/422: el payload no va a pasar; cuarentena.
 *   TokenInvalid 401: el token expiró o es inválido; reautorizar una vez.
 *   NoToken     no hay token provisionado; no se intenta la red.
 *
 * Concurrencia: 1 envío a la vez (el controlador serializa). Timeout
 * 15 s por intento: un organismo caído no debe retener el hilo.
 */
class OrganismSender(
    private val tokenStore: TokenStore,
    private val queue: OrganismQueue,
    private val client: OkHttpClient = defaultClient(),
    private val hosts: List<String> = DEFAULT_HOSTS,
    private val nowMs: () -> Long = { System.currentTimeMillis() },
    private val jitter: () -> Double = { (Math.random() * 0.5) - 0.25 }, // ±25%
    private val logger: OrganismLogger = AndroidOrganismLogger(TAG)
) {
    companion object {
        private const val TAG = "OrganismSender"
        val DEFAULT_HOSTS = listOf(
            "https://muselink.ai.studio",
            "https://muselink-667333806336.us-west1.run.app"
        )
        const val SESSION_PATH = "/api/v1/muse/ivanna/session"
        private val JSON = "application/json; charset=utf-8".toMediaType()

        fun defaultClient(): OkHttpClient = OkHttpClient.Builder()
            .connectTimeout(15, TimeUnit.SECONDS)
            .readTimeout(15, TimeUnit.SECONDS)
            .writeTimeout(15, TimeUnit.SECONDS)
            .followRedirects(false) // nunca seguir redirects con el Bearer
            .build()
    }

    enum class Outcome { SENT, RETRY, QUARANTINE, TOKEN_INVALID, NO_TOKEN }

    /** Resultado de un intento de vaciado (para el indicador de estado). */
    data class FlushResult(
        val sent: Int,
        val retried: Int,
        val quarantined: Int,
        val outcome: Outcome
    )

    /**
     * Vacía lo que esté listo en la cola (PENDING o RETRY vencido), un
     * chunk por intento, hasta que no quede nada listo o un fallo diga
     * que hay que parar (token inválido, red caída). Devuelve el resumen.
     * Sincronizado: dos vaciados concurrentes (timer + chunk nuevo) no
     * deben mandar el mismo chunk dos veces.
     */
    @Synchronized
    fun flush(): FlushResult {
        val token = tokenStore.getToken()
        if (token.isNullOrBlank()) return FlushResult(0, 0, 0, Outcome.NO_TOKEN)
        var sent = 0
        var retried = 0
        var quarantined = 0
        var lastOutcome = Outcome.SENT
        while (true) {
            val item = queue.nextReady(nowMs()) ?: break
            queue.markSending(item)
            val outcome = sendOne(item, token)
            lastOutcome = outcome
            when (outcome) {
                Outcome.SENT -> {
                    queue.markSent(item)
                    sent++
                }
                Outcome.RETRY -> {
                    queue.markRetry(item, nowMs(), "reintentable", jitter())
                    retried++
                    break // la red está mal: no seguir golpeándola
                }
                Outcome.QUARANTINE -> {
                    queue.markQuarantined(item, "no reintentable")
                    quarantined++
                }
                Outcome.TOKEN_INVALID -> {
                    // Defecto #3: un 401 es configuración (token muerto),
                    // no un fallo transitorio. NO consume intentos ni va a
                    // cuarentena: vuelve a PENDING con los mismos intentos
                    // y queda listo para cuando se corrija el token.
                    queue.markPending(item)
                    break // no gastar intentos con un token muerto
                }
                Outcome.NO_TOKEN -> break
            }
        }
        return FlushResult(sent, retried, quarantined, lastOutcome)
    }

    /** Un intento de POST para un chunk. Sin logs del token ni del body. */
    fun sendOne(item: OrganismQueue.Item, token: String): Outcome {
        val json = item.body.toString()
        var lastOutcome: Outcome = Outcome.RETRY
        for (host in hosts) {
            val request = Request.Builder()
                .url(host + SESSION_PATH)
                .header("Authorization", "Bearer $token")
                .header("Accept", "application/json")
                .post(json.toRequestBody(JSON))
                .build()
            try {
                client.newCall(request).execute().use { resp ->
                    val code = resp.code
                    logger.i("POST $host$SESSION_PATH → HTTP $code (chunk ${item.key})")
                    return when {
                        code in 200..299 -> Outcome.SENT
                        code == 401 -> Outcome.TOKEN_INVALID
                        code == 429 || code in 500..599 -> Outcome.RETRY
                        else -> Outcome.QUARANTINE
                    }
                }
            } catch (e: Exception) {
                logger.w("POST $host falló: ${e.javaClass.simpleName} (chunk ${item.key})")
                lastOutcome = Outcome.RETRY
                // Probar el host alterno antes de rendirse.
            }
        }
        return lastOutcome
    }
}
