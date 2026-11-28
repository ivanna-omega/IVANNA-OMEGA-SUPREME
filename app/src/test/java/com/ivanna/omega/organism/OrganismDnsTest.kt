package com.ivanna.omega.organism

import com.google.gson.JsonObject
import okhttp3.Dns
import okhttp3.OkHttpClient
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.ByteArrayOutputStream
import java.io.File
import java.net.InetAddress
import java.nio.file.Files
import java.util.concurrent.TimeUnit

/**
 * Tests del Dns propio del Organismo (v2.5.509).
 *
 * El enrutado, el fallback y el protocolo DNS (RFC 1035) se verifican
 * con dobles inyectados y una respuesta enlatada, SIN red real. Solo el
 * test de integración del sender intenta una conexión real (a
 * 127.0.0.1:443, puerto cerrado → connection refused inmediato) para
 * probar que OkHttp consulta el Dns inyectado; con timeout corto para
 * no depender del loopback de la VM. El Dns real (UDP a 8.8.8.8) se
 * valida en el teléfono, donde el DNS del sistema falla por el VPN
 * (MagicDNS) y el directo es el único camino. JVM plana (sin Robolectric).
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
        val addrs = OrganismDnsWire.parseResponse(resp, 1)
        assertEquals(listOf(ip4(34, 143, 76, 2)), addrs)
    }

    @Test
    fun `parseResponse ignora RCODE distinto de 0 y paquetes cortos`() {
        val resp = cannedAResponse("muselink.ai.studio", ip4(34, 143, 76, 2))
        // RCODE=3 (NXDOMAIN): byte 3, bits bajos.
        val nx = resp.copyOf().also { it[3] = (it[3].toInt() or 0x03).toByte() }
        assertTrue(OrganismDnsWire.parseResponse(nx, 1).isEmpty())
        assertTrue(OrganismDnsWire.parseResponse(ByteArray(5), 1).isEmpty())
    }

    /** Respuesta DNS enlatada: 1 pregunta A + 1 respuesta A (NAME por puntero). */
    private fun cannedAResponse(hostname: String, ip: InetAddress): ByteArray {
        val out = ByteArrayOutputStream()
        // Header: ID=0x1234, flags=0x8180 (respuesta, RD+RA, RCODE=0),
        // QDCOUNT=1, ANCOUNT=1, NSCOUNT=0, ARCOUNT=0.
        out.write(0x12); out.write(0x34)
        out.write(0x81); out.write(0x80)
        out.write(0x00); out.write(0x01)
        out.write(0x00); out.write(0x01)
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
        // TTL=300, RDLENGTH=4, RDATA=IP.
        out.write(0xC0); out.write(0x0C)
        out.write(0x00); out.write(0x01)
        out.write(0x00); out.write(0x01)
        out.write(0x00); out.write(0x00); out.write(0x01); out.write(0x2C)
        out.write(0x00); out.write(0x04)
        out.write(ip.address)
        return out.toByteArray()
    }
}
