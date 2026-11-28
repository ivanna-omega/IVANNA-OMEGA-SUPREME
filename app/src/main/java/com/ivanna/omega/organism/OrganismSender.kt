package com.ivanna.omega.organism

import android.util.Log
import com.google.gson.JsonObject
import okhttp3.Dns
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetAddress
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
            .dns(OrganismDns()) // DNS propio: no depende de netd/MagicDNS
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

// ── DNS PROPIO DEL ORGANISMO ──────────────────────────────────────────────
// Por qué existe: la app corre bajo el VPN de Tailscale y su MagicDNS
// (100.100.100.100) NO resuelve hosts públicos para la app
// (UnknownHostException aunque la red esté bien); el /etc/hosts del
// teléfono no lo ve netd (que es quien resuelve para las apps), así que
// ningún arreglo por /etc/hosts es fiable. El sender DEBE resolver por sí
// mismo: pregunta directo por UDP a 8.8.8.8 (respaldo 1.1.1.1) con un
// cliente DNS mínimo (RFC 1035, sin dependencias). Si el DNS directo
// falla, cae al DNS del sistema como ÚLTIMO recurso (no al revés). Los
// hosts que no son de MuseLink usan el DNS del sistema siempre.
//
// El Dns es inyectable (constructor) para que los tests verifiquen el
// enrutado y el fallback sin tocar la red real.
/**
 * Resuelve los hosts de MuseLink por DNS directo (UDP a 8.8.8.8/1.1.1.1)
 * sin pasar por netd/MagicDNS; resto de hosts y fallback usan el DNS del
 * sistema. IPv4 primero (el camino que se sabe que pasa por el VPN).
 */
class OrganismDns(
    private val systemDns: Dns = Dns.SYSTEM,
    private val directServers: List<InetAddress> = DEFAULT_DNS_SERVERS,
    private val timeoutMs: Int = 3000,
    private val directQuery: (hostname: String, server: InetAddress) -> List<InetAddress> =
        { host, server -> queryDirectDns(host, server, timeoutMs) }
) : Dns {

    companion object {
        /** Hosts que se resuelven por DNS directo (no por el sistema). */
        val MUSELINK_HOSTS: Set<String> = setOf(
            "muselink.ai.studio",
            "muselink-667333806336.us-west1.run.app"
        )

        /** Servidores DNS directos (IP literal, sin resolver). */
        val DEFAULT_DNS_SERVERS: List<InetAddress> = listOf(
            InetAddress.getByAddress(byteArrayOf(8, 8, 8, 8)),
            InetAddress.getByAddress(byteArrayOf(1, 1, 1, 1))
        )
    }

    override fun lookup(hostname: String): List<InetAddress> {
        if (hostname !in MUSELINK_HOSTS) return systemDns.lookup(hostname)
        // DNS directo primero (no depende de netd/MagicDNS).
        for (server in directServers) {
            try {
                val addrs = directQuery(hostname, server)
                if (addrs.isNotEmpty()) return addrs
            } catch (e: Exception) {
                // Probar el siguiente servidor directo.
            }
        }
        // Último recurso: DNS del sistema.
        return systemDns.lookup(hostname)
    }
}

/** Protocolo DNS mínimo (RFC 1035) para el Dns propio. Sin dependencias. */
internal object OrganismDnsWire {

    /** Construye un query DNS estándar (RD=1) para A (1) o AAAA (28). */
    fun buildQuery(hostname: String, qtype: Int, id: Int): ByteArray {
        val out = java.io.ByteArrayOutputStream()
        out.write(id shr 8)
        out.write(id and 0xFF)
        out.write(0x01) // flags: RD=1
        out.write(0x00)
        out.write(0x00)
        out.write(0x01) // QDCOUNT=1
        out.write(0x00)
        out.write(0x00) // ANCOUNT=0
        out.write(0x00)
        out.write(0x00) // NSCOUNT=0
        out.write(0x00)
        out.write(0x00) // ARCOUNT=0
        for (label in hostname.split(".")) {
            out.write(label.length)
            out.write(label.toByteArray(Charsets.US_ASCII))
        }
        out.write(0x00) // fin de QNAME
        out.write(qtype shr 8)
        out.write(qtype and 0xFF)
        out.write(0x00)
        out.write(0x01) // QCLASS=IN
        return out.toByteArray()
    }

    /**
     * Extrae las direcciones del tipo pedido (1=A, 28=AAAA) de una
     * respuesta DNS. Tolera CNAME (el resolver recursivo ya trae los
     * A/AAAA del destino en la misma respuesta) y punteros de compresión
     * (0xC0xx). RCODE != 0 o paquete corto → lista vacía.
     */
    fun parseResponse(data: ByteArray, wantType: Int): List<InetAddress> {
        if (data.size < 12) return emptyList()
        val rcode = data[3].toInt() and 0x0F
        if (rcode != 0) return emptyList()
        val qd = ((data[4].toInt() and 0xFF) shl 8) or (data[5].toInt() and 0xFF)
        val an = ((data[6].toInt() and 0xFF) shl 8) or (data[7].toInt() and 0xFF)
        var off = 12
        repeat(qd) {
            off = skipName(data, off)
            off += 4 // QTYPE + QCLASS
        }
        val out = mutableListOf<InetAddress>()
        repeat(an) {
            if (off >= data.size) return out
            off = skipName(data, off)
            if (off + 10 > data.size) return out
            val type = ((data[off].toInt() and 0xFF) shl 8) or
                (data[off + 1].toInt() and 0xFF)
            val rdlen = ((data[off + 8].toInt() and 0xFF) shl 8) or
                (data[off + 9].toInt() and 0xFF)
            val rdataOff = off + 10
            if (rdataOff + rdlen <= data.size && type == wantType) {
                if (wantType == 1 && rdlen == 4) {
                    out += InetAddress.getByAddress(data.copyOfRange(rdataOff, rdataOff + 4))
                } else if (wantType == 28 && rdlen == 16) {
                    out += InetAddress.getByAddress(data.copyOfRange(rdataOff, rdataOff + 16))
                }
            }
            off = rdataOff + rdlen
        }
        return out
    }

    /** Salta un nombre DNS (etiquetas o puntero de compresión 0xC0xx). */
    fun skipName(data: ByteArray, off: Int): Int {
        var i = off
        while (i < data.size) {
            val len = data[i].toInt() and 0xFF
            if (len == 0) return i + 1
            if (len and 0xC0 == 0xC0) return i + 2 // puntero: 2 bytes
            i += 1 + len
        }
        return i
    }
}

/** Query DNS directo por UDP (A y luego AAAA; IPv4 primero). */
private fun queryDirectDns(
    hostname: String,
    server: InetAddress,
    timeoutMs: Int
): List<InetAddress> {
    val out = mutableListOf<InetAddress>()
    for (qtype in intArrayOf(1, 28)) {
        try {
            val id = (Math.random() * 65536).toInt()
            val query = OrganismDnsWire.buildQuery(hostname, qtype, id)
            DatagramSocket().use { sock ->
                sock.soTimeout = timeoutMs
                sock.connect(server, 53)
                sock.send(DatagramPacket(query, query.size))
                val buf = ByteArray(2048)
                val pkt = DatagramPacket(buf, buf.size)
                sock.receive(pkt)
                out += OrganismDnsWire.parseResponse(pkt.data.copyOf(pkt.length), qtype)
            }
        } catch (e: Exception) {
            // Un tipo falló (timeout): el otro aún puede responder.
        }
    }
    return out
}
