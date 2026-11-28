package com.ivanna.omega.organism

import com.google.gson.JsonObject
import okhttp3.Dns
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.InetAddress
import java.nio.file.Files
import java.util.concurrent.TimeUnit

/**
 * Tests del Dns propio del Organismo (v2.5.509; endurecimiento C9 en v2.5.510).
 *
 * El enrutado, el fallback y el protocolo DNS (RFC 1035) se verifican
 * con dobles inyectados y una respuesta enlatada, SIN red real. Solo el
 * test de integración del sender intenta una conexión real (a
 * 127.0.0.1:443, puerto cerrado → connection refused inmediato) para
 * probar que OkHttp consulta el Dns inyectado; con timeout corto para
 * no depender del loopback de la VM. El Dns real (UDP a 8.8.8.8) se
 * valida en el teléfono, donde el DNS del sistema falla por el VPN
 * (MagicDNS) y el directo es el único camino. JVM plana (sin Robolectric).
 *
 * C9 (v2.5.510): validación de ID de transacción y QDCOUNT en
 * parseResponse, rechazo de TTL absurdo y de exceso de registros, y
 * allowlist cerrada de hosts (MuselinkHosts) antes de conectar.
 */
class OrganismDnsTest {

    private lateinit var dir: File

    @Before
    fun setup() {
        dir = Files.createTempDirectory("organism-dns-test").toFile()
    }

    @After
    fun teardown() {
        dir.deleteRecursively()
    }

    /** Dns doble que registra llamadas y devuelve una IP fija. */
    private class RecordingDns(private val ip: InetAddress) : Dns {
        val asked = mutableListOf<String>()
        override fun lookup(hostname: String): List<InetAddress> {
            asked += hostname
            return listOf(ip)
        }
    }

    private fun ip4(a: Int, b: Int, c: Int, d: Int): InetAddress =
        InetAddress.getByAddress(
            byteArrayOf(a.toByte(), b.toByte(), c.toByte(), d.toByte())
        )

    @Test
    fun `hosts MuseLink van por DNS directo y otros por el sistema`() {
        val system = RecordingDns(ip4(9, 9, 9, 9))
        val directAsked = mutableListOf<Pair<String, InetAddress>>()
        val direct: (String, InetAddress) -> List<InetAddress> = { host, server ->
            directAsked += host to server
            listOf(ip4(34, 143, 76, 2))
        }
        val dns = OrganismDns(systemDns = system, directQuery = direct)
        val addrs = dns.lookup("muselink.ai.studio")
        assertEquals(listOf(ip4(34, 143, 76, 2)), addrs)
        assertEquals(listOf("muselink.ai.studio"), directAsked.map { it.first })
        assertTrue("el sistema no debe resolver hosts MuseLink", system.asked.isEmpty())

        val other = dns.lookup("example.com")
        assertEquals(listOf(ip4(9, 9, 9, 9)), other)
        assertEquals(listOf("example.com"), system.asked)
    }

    @Test
    fun `si el DNS directo falla cae al sistema como ultimo recurso`() {
        val system = RecordingDns(ip4(9, 9, 9, 9))
        val direct: (String, InetAddress) -> List<InetAddress> = { _, _ ->
            throw java.io.IOException("DNS directo caido (simulado)")
        }
        val dns = OrganismDns(systemDns = system, directQuery = direct)
        val addrs = dns.lookup("muselink-667333806336.us-west1.run.app")
        assertEquals(listOf(ip4(9, 9, 9, 9)), addrs)
        assertEquals(listOf("muselink-667333806336.us-west1.run.app"), system.asked)
    }

    @Test
    fun `el sender usa el Dns inyectado en el cliente (no el del sistema)`() {
        // Dns doble: resuelve cualquier host a 127.0.0.1 y registra la consulta.
        val recording = RecordingDns(ip4(127, 0, 0, 1))
        // Cliente SIN interceptor: intenta conectar de verdad (a 127.0.0.1:443,
        // puerto cerrado → connection refused) para que OkHttp tenga que
        // resolver el host vía el Dns inyectado. Timeout corto para que el
        // test no dependa del loopback de la VM.
        val client = OkHttpClient.Builder()
            .dns(recording)
            .connectTimeout(2, TimeUnit.SECONDS)
            .readTimeout(2, TimeUnit.SECONDS)
            .writeTimeout(2, TimeUnit.SECONDS)
            .build()
        val queue = OrganismQueue(dir)
        val s = OrganismSender(
            InMemoryTokenStore("test-token-1234567890"), queue,
            client = client,
            hosts = listOf("https://muselink.ai.studio"),
            nowMs = { 1000L }, jitter = { 0.0 },
            logger = NoOpOrganismLogger()
        )
        val body = JsonObject().apply { addProperty("song", "x") }
        queue.enqueue("s1", 0, body)
        val result = s.flush()
        // La conexión falla (puerto cerrado) → RETRY, pero el Dns inyectado
        // SÍ fue consultado: el sender no usa el DNS del sistema.
        assertEquals(OrganismSender.Outcome.RETRY, result.outcome)
        assertTrue(recording.asked.contains("muselink.ai.studio"))
    }

    @Test
    fun `parseResponse extrae el A de una respuesta enlatada con puntero`() {
        val resp = cannedAResponse("muselink.ai.studio", ip4(34, 143, 76, 2))
        val addrs = OrganismDnsWire.parseResponse(resp, 1, 0x1234)
        assertEquals(listOf(ip4(34, 143, 76, 2)), addrs)
    }

    @Test
    fun `parseResponse ignora RCODE distinto de 0 y paquetes cortos`() {
        val resp = cannedAResponse("muselink.ai.studio", ip4(34, 143, 76, 2))
        // RCODE=3 (NXDOMAIN): byte 3, bits bajos.
        val nx = resp.copyOf().also { it[3] = (it[3].toInt() or 0x03).toByte() }
        assertTrue(OrganismDnsWire.parseResponse(nx, 1, 0x1234).isEmpty())
        assertTrue(OrganismDnsWire.parseResponse(ByteArray(5), 1, 0x1234).isEmpty())
    }

    @Test
    fun `parseResponse rechaza ID de transaccion distinto al de la query`() {
        val resp = cannedAResponse("muselink.ai.studio", ip4(34, 143, 76, 2))
        // ID enlatado 0x1234; una respuesta con otro ID es de otra query
        // (o envenenada): se rechaza entera, no se usa ni un registro.
        assertTrue(OrganismDnsWire.parseResponse(resp, 1, 0x5678).isEmpty())
        assertEquals(
            listOf(ip4(34, 143, 76, 2)),
            OrganismDnsWire.parseResponse(resp, 1, 0x1234)
        )
    }

    @Test
    fun `parseResponse rechaza QDCOUNT distinto de 1`() {
        val ip = ip4(34, 143, 76, 2)
        assertTrue(
            OrganismDnsWire.parseResponse(
                cannedAResponse("muselink.ai.studio", ip, qdcount = 0), 1, 0x1234
            ).isEmpty()
        )
        assertTrue(
            OrganismDnsWire.parseResponse(
                cannedAResponse("muselink.ai.studio", ip, qdcount = 2), 1, 0x1234
            ).isEmpty()
        )
    }

    @Test
    fun `parseResponse rechaza TTL absurdo y acepta TTL en rango`() {
        val ip = ip4(34, 143, 76, 2)
        // TTL 2_000_000 s (~23 días): imposible en este camino, se rechaza.
        assertTrue(
            OrganismDnsWire.parseResponse(
                cannedAResponse("muselink.ai.studio", ip, ttl = 2_000_000), 1, 0x1234
            ).isEmpty()
        )
        // Frontera: 86400 s (1 día, tope de Google Public DNS) pasa; 86401 no.
        assertEquals(
            listOf(ip),
            OrganismDnsWire.parseResponse(
                cannedAResponse("muselink.ai.studio", ip, ttl = 86_400), 1, 0x1234
            )
        )
        assertTrue(
            OrganismDnsWire.parseResponse(
                cannedAResponse("muselink.ai.studio", ip, ttl = 86_401), 1, 0x1234
            ).isEmpty()
        )
        // TTL=0 (sin caché) es válido.
        assertEquals(
            listOf(ip),
            OrganismDnsWire.parseResponse(
                cannedAResponse("muselink.ai.studio", ip, ttl = 0), 1, 0x1234
            )
        )
    }

    @Test
    fun `parseResponse rechaza mas registros de los esperados`() {
        val resp = cannedAResponse(
            "muselink.ai.studio", ip4(34, 143, 76, 2), ancount = 17
        )
        assertTrue(OrganismDnsWire.parseResponse(resp, 1, 0x1234).isEmpty())
    }

    @Test
    fun `lookup rechaza respuesta directa con demasiadas direcciones`() {
        val system = RecordingDns(ip4(9, 9, 9, 9))
        val direct: (String, InetAddress) -> List<InetAddress> = { _, _ ->
            (1..17).map { ip4(10, 0, 0, it) }
        }
        val dns = OrganismDns(systemDns = system, directQuery = direct)
        val addrs = dns.lookup("muselink.ai.studio")
        // La respuesta fabricada se descarta y se cae al DNS del sistema.
        assertEquals(listOf(ip4(9, 9, 9, 9)), addrs)
        assertEquals(listOf("muselink.ai.studio"), system.asked)
    }

    @Test
    fun `sender rechaza host fuera de la allowlist antes de conectar`() {
        val (client, hits) = countingClient()
        val queue = OrganismQueue(dir)
        val s = OrganismSender(
            InMemoryTokenStore("test-token-1234567890"), queue,
            client = client,
            // Host ajeno y ataque por sufijo: ninguno está en MuselinkHosts.
            hosts = listOf("https://evil.example.com", "http://muselink.ai.studio.evil.com"),
            nowMs = { 1000L }, jitter = { 0.0 },
            logger = NoOpOrganismLogger()
        )
        val body = JsonObject().apply { addProperty("song", "x") }
        queue.enqueue("s1", 0, body)
        val result = s.flush()
        // Cero intentos de conexión: el Bearer no salió del teléfono.
        assertEquals(0, hits())
        assertEquals(OrganismSender.Outcome.RETRY, result.outcome)
    }

    @Test
    fun `los dos hosts permitidos pasan la allowlist y el sender conecta`() {
        assertTrue(MuselinkHosts.isAllowedHost("muselink.ai.studio"))
        assertTrue(MuselinkHosts.isAllowedHost("muselink-667333806336.us-west1.run.app"))
        assertTrue(MuselinkHosts.isAllowedHost("MUSELINK.AI.STUDIO")) // DNS no distingue mayúsculas
        assertFalse(MuselinkHosts.isAllowedHost("evil.example.com"))
        assertFalse(MuselinkHosts.isAllowedHost("muselink.ai.studio.evil.com"))
        assertTrue(MuselinkHosts.isAllowedUrl("https://muselink.ai.studio"))
        assertFalse(MuselinkHosts.isAllowedUrl("http://muselink.ai.studio")) // sin https no hay Bearer
        assertFalse(MuselinkHosts.isAllowedUrl("https://evil.example.com"))
        assertFalse(MuselinkHosts.isAllowedUrl("not-a-url"))

        val (client, hits) = countingClient()
        val queue = OrganismQueue(dir)
        val s = OrganismSender(
            InMemoryTokenStore("test-token-1234567890"), queue,
            client = client,
            hosts = MuselinkHosts.DEFAULT_URLS,
            nowMs = { 1000L }, jitter = { 0.0 },
            logger = NoOpOrganismLogger()
        )
        val body = JsonObject().apply { addProperty("song", "x") }
        queue.enqueue("s1", 0, body)
        val result = s.flush()
        assertEquals(OrganismSender.Outcome.SENT, result.outcome)
        assertEquals(1, result.sent)
        assertEquals(1, hits())
    }

    @Test
    fun `defaultClient no sigue redirects`() {
        assertFalse(OrganismSender.defaultClient().followRedirects)
    }

    /** Cliente OkHttp que NO toca la red: el interceptor cuenta y responde 200. */
    private fun countingClient(): Pair<OkHttpClient, () -> Int> {
        var hits = 0
        val client = OkHttpClient.Builder()
            .addInterceptor(Interceptor { chain ->
                hits++
                Response.Builder()
                    .request(chain.request())
                    .protocol(Protocol.HTTP_1_1)
                    .code(200)
                    .message("test")
                    .body("{}".toResponseBody(null))
                    .build()
            })
            .build()
        return client to { hits }
    }

    /** Respuesta DNS enlatada: 1 pregunta A + 1 respuesta A (NAME por puntero). */
    private fun cannedAResponse(
        hostname: String,
        ip: InetAddress,
        ttl: Int = 300,
        id: Int = 0x1234,
        qdcount: Int = 1,
        ancount: Int = 1
    ): ByteArray {
        val out = ByteArrayOutputStream()
        // Header: ID, flags=0x8180 (respuesta, RD+RA, RCODE=0),
        // QDCOUNT, ANCOUNT, NSCOUNT=0, ARCOUNT=0.
        out.write(id shr 8); out.write(id and 0xFF)
        out.write(0x81); out.write(0x80)
        out.write(qdcount shr 8); out.write(qdcount and 0xFF)
        out.write(ancount shr 8); out.write(ancount and 0xFF)
        out.write(0x00); out.write(0x00)
        out.write(0x00); out.write(0x00)
        // Question: QNAME + QTYPE=1 + QCLASS=1.
        for (label in hostname.split(".")) {
            out.write(label.length)
            out.write(label.toByteArray(Charsets.US_ASCII))
        }
        out.write(0x00)
        out.write(0x00); out.write(0x01)
        out.write(0x00); out.write(0x01)
        // Answer: NAME=puntero a offset 12 (0xC00C), TYPE=1, CLASS=1,
        // TTL, RDLENGTH=4, RDATA=IP.
        out.write(0xC0); out.write(0x0C)
        out.write(0x00); out.write(0x01)
        out.write(0x00); out.write(0x01)
        out.write(ttl shr 24)
        out.write((ttl shr 16) and 0xFF)
        out.write((ttl shr 8) and 0xFF)
        out.write(ttl and 0xFF)
        out.write(0x00); out.write(0x04)
        out.write(ip.address)
        return out.toByteArray()
    }
}
