package com.ivanna.omega.organism

import okhttp3.Dns
import okhttp3.OkHttpClient
import okhttp3.Request
import java.util.concurrent.TimeUnit

/**
 * OrganismTokenDiagnostics — diagnóstico de un toque del token (C21,
 * release 510; «diagnóstico de re-auth del token, no auto-renovación
 * mágica»).
 *
 * Si el token muere, el estado dice «Token inválido» y todo se para
 * hasta que Esse pegue otro. No hay endpoint de refresh (documentado en
 * [OrganismTokenStore]) y NO se inventa uno: lo que hace este
 * diagnóstico es que el re-pegado sea de un minuto y no una
 * investigación. Por host (primario y alterno, [MuselinkHosts.
 * DEFAULT_URLS]) comprueba:
 *   1. DNS directo OK/fallo (el mismo [OrganismDns] del sender: sin
 *      netd/MagicDNS; un fallo aquí con red en general viva apunta al
 *      DNS, no al token).
 *   2. Alcanzabilidad: un GET sin Authorization a
 *      [ProfileFetcher.CURRENT_PATH]. Cualquier código HTTP (típicamente
 *      401 por falta de auth) prueba que el host está vivo y exige auth;
 *      sin respuesta HTTP, el host no es alcanzable. El token NUNCA se
 *      envía en el diagnóstico: distinguir «token muerto» de «red
 *      caída» no necesita el Bearer, y no sale de los dos hosts
 *      permitidos ni siquiera en el diagnóstico.
 *   3. Fecha del último 401 (la registra el controlador; aquí solo se
 *      arrastra para mostrarla junto al resto).
 *
 * Puro Kotlin + OkHttp, sin `android.util.Log` directo (mismo patrón que
 * [OrganismSender] / [ProfileFetcher]): testeable en JVM plana con un
 * interceptor que simule el servidor y un [Dns] falso. El cliente tiene
 * timeouts cortos (5 s por intento) porque es un diagnóstico bajo
 * demanda, no el vaciado: con los 15 s del sender, dos hosts caídos
 * tardarían más de medio minuto.
 *
 * Lo que NO hace: renovar el token, reintentarlo, ni guardarlo. El
 * token solo entra pegado por Esse, una vez, en
 * [OrganismController.saveToken]; ningún endpoint puede entregar un
 * token al teléfono.
 */
class OrganismTokenDiagnostics(
    private val dns: Dns = OrganismDns(),
    private val client: OkHttpClient = defaultClient(dns),
    private val nowMs: () -> Long = { System.currentTimeMillis() },
    private val logger: OrganismLogger = AndroidOrganismLogger(TAG)
) {
    companion object {
        private const val TAG = "OrganismTokenDiagnostics"

        /** Timeout por intento del diagnóstico (un GET por host, sin reintento). */
        const val DIAGNOSIS_TIMEOUT_SECONDS = 5L

        fun defaultClient(dns: Dns = OrganismDns()): OkHttpClient = OkHttpClient.Builder()
            .dns(dns) // DNS propio: el mismo del sender
            .connectTimeout(DIAGNOSIS_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .readTimeout(DIAGNOSIS_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .writeTimeout(DIAGNOSIS_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            .followRedirects(false) // nunca seguir redirects, ni en diagnóstico
            .build()
    }

    /** Diagnóstico de UN host (primario o alterno). */
    data class HostDiagnosis(
        /** URL base del host (p. ej. `https://muselink.ai.studio`). */
        val hostUrl: String,
        /** true si el DNS directo resolvió el hostname. */
        val dnsOk: Boolean,
        /** true si el host devolvió alguna respuesta HTTP. */
        val reachable: Boolean,
        /** Código HTTP recibido, o null si no hubo respuesta. */
        val httpCode: Int?
    )

    /** Resultado del diagnóstico de un toque (un host por URL permitida). */
    data class Diagnosis(
        val atMs: Long,
        val hosts: List<HostDiagnosis>,
        /** Fecha del último 401 con el token actual (0 = nunca hubo uno). */
        val last401AtMs: Long
    ) {
        /** true si al menos un host respondió por HTTP (red y servidor vivos). */
        val anyReachable: Boolean get() = hosts.any { it.reachable }
        /** true si algún host resolvió por DNS directo. */
        val anyDnsOk: Boolean get() = hosts.any { it.dnsOk }

        /**
         * Texto para la tarjeta (una línea por host): host, DNS y
         * alcanzabilidad. Puro (testeable en JVM); la fecha del último
         * 401 la muestra la tarjeta aparte, con su propio formato.
         */
        fun summary(): String = hosts.joinToString("\n") { h ->
            val host = runCatching { java.net.URI(h.hostUrl).host }.getOrNull() ?: h.hostUrl
            val dns = if (h.dnsOk) "DNS OK" else "DNS falló"
            val reach = when {
                h.reachable && h.httpCode != null -> "HTTP ${h.httpCode} (alcanzable)"
                h.reachable -> "alcanzable"
                else -> "sin respuesta (revisar red)"
            }
            "$host: $dns · $reach"
        }
    }

    /** Diagnóstico de un toque sobre los dos hosts permitidos. */
    fun diagnose(last401AtMs: Long): Diagnosis {
        val perHost = MuselinkHosts.DEFAULT_URLS.map { url -> diagnoseHost(url) }
        return Diagnosis(atMs = nowMs(), hosts = perHost, last401AtMs = last401AtMs)
    }

    private fun diagnoseHost(url: String): HostDiagnosis {
        val hostname = runCatching { java.net.URI(url).host }.getOrNull()
            ?: return HostDiagnosis(url, dnsOk = false, reachable = false, httpCode = null)
        // 1. DNS directo (el mismo camino que el sender; sin netd).
        val dnsOk = runCatching { dns.lookup(hostname) }.getOrDefault(emptyList()).isNotEmpty()
        // 2. Alcanzabilidad SIN Authorization: un 401 prueba que el host
        // está vivo y exige auth; cualquier código HTTP vale. El Bearer
        // no sale del teléfono en el diagnóstico.
        var code: Int? = null
        var reachable = false
        try {
            val request = Request.Builder()
                .url(url + ProfileFetcher.CURRENT_PATH)
                .header("Accept", "application/json")
                .get()
                .build()
            client.newCall(request).execute().use { resp ->
                code = resp.code
                reachable = true
            }
        } catch (e: Exception) {
            logger.w("diagnóstico $url falló: ${e.javaClass.simpleName}")
        }
        return HostDiagnosis(url, dnsOk, reachable, code)
    }
}
