package com.ivanna.omega.organism

import okhttp3.Dns
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.IOException
import java.net.InetAddress
import java.net.UnknownHostException

/**
 * Tests del diagnóstico de un toque del token (C21, release 510).
 * Mismo patrón que ProfileFetcherTest: un interceptor OkHttp simula el
 * servidor, sin sockets y sin tocar la red real; el DNS es un falso
 * inyectable. JVM plana: el diagnóstico usa un logger inyectable, no
 * android.util.Log directo.
 *
 * La compilación/ejecución queda para la CI (Gradle no corre en esta VM:
 * conexión cliente→daemon bloqueada).
 */
class OrganismTokenDiagnosticsTest {

    /** Host que simula estar caído (el interceptor lanza IOException). */
    private var downHost: String? = null

    /** Código que devuelve el servidor simulado (401 = exige auth). */
    private var nextCode: Int = 401

    private var lastAuth: String? = null
    private var lastPath: String? = null
    private var hits: Int = 0

    /** DNS falso: resuelve, o falla si [dnsDown] es true. */
    private var dnsDown: Boolean = false
    private val fakeDns = object : Dns {
        override fun lookup(hostname: String): List<InetAddress> {
            if (dnsDown) throw UnknownHostException("DNS simulado caído: $hostname")
            return listOf(InetAddress.getByAddress(byteArrayOf(93.toByte(), 184.toByte(), 216.toByte(), 34.toByte())))
        }
    }

    private fun fakeClient(): OkHttpClient = OkHttpClient.Builder()
        .addInterceptor(Interceptor { chain ->
            val req = chain.request()
            if (req.url.host == downHost) throw IOException("connection refused (simulado)")
            hits++
            lastAuth = req.header("Authorization")
            lastPath = req.url.encodedPath
            Response.Builder()
                .request(req)
                .protocol(Protocol.HTTP_1_1)
                .code(nextCode)
                .message("test")
                .body("{}".toResponseBody(null))
                .build()
        })
        .build()

    private fun diagnostics(): OrganismTokenDiagnostics = OrganismTokenDiagnostics(
        dns = fakeDns,
        client = fakeClient(),
        nowMs = { 1_000_000L },
        logger = NoOpOrganismLogger()
    )

    @Test
    fun `hosts vivos con 401 son alcanzables con DNS OK y sin enviar el Bearer`() {
        val diag = diagnostics().diagnose(last401AtMs = 999L)
        assertEquals(2, diag.hosts.size)
        assertEquals(999L, diag.last401AtMs)
        assertEquals(1_000_000L, diag.atMs)
        assertTrue(diag.anyReachable)
        assertTrue(diag.anyDnsOk)
        for (h in diag.hosts) {
            assertTrue("DNS debía resolver en ${h.hostUrl}", h.dnsOk)
            assertTrue("host debía ser alcanzable: ${h.hostUrl}", h.reachable)
            assertEquals(401, h.httpCode)
        }
        assertEquals(2, hits)
        assertNull("el diagnóstico NUNCA envía el token", lastAuth)
        assertEquals("/api/v1/muse/ivanna/profile/current", lastPath)
    }

    @Test
    fun `DNS caido y hosts caidos - nada resuelve ni responde`() {
        dnsDown = true
        downHost = null // el interceptor no se alcanza: el DNS ya falló
        // Sin interceptor alcanzable: simular caída total lanzando en ambos hosts.
        val diag = OrganismTokenDiagnostics(
            dns = fakeDns,
            client = OkHttpClient.Builder()
                .addInterceptor(Interceptor { throw IOException("connection refused (simulado)") })
                .build(),
            nowMs = { 1_000_000L },
            logger = NoOpOrganismLogger()
        ).diagnose(last401AtMs = 0L)
        assertFalse(diag.anyReachable)
        assertFalse(diag.anyDnsOk)
        for (h in diag.hosts) {
            assertFalse(h.dnsOk)
            assertFalse(h.reachable)
            assertNull(h.httpCode)
        }
        assertEquals(0L, diag.last401AtMs)
    }

    @Test
    fun `primario caido y alterno vivo - el diagnostico lo distingue`() {
        downHost = "muselink.ai.studio"
        val diag = diagnostics().diagnose(last401AtMs = 0L)
        assertTrue(diag.anyReachable)
        assertTrue(diag.anyDnsOk)
        val primary = diag.hosts[0]
        val alternate = diag.hosts[1]
        assertEquals("https://muselink.ai.studio", primary.hostUrl)
        assertFalse("el primario caído no responde", primary.reachable)
        assertNull(primary.httpCode)
        assertTrue("el DNS directo sí resolvió el primario", primary.dnsOk)
        assertTrue(alternate.reachable)
        assertEquals(401, alternate.httpCode)
        assertEquals(1, hits) // solo el alterno llegó al interceptor
    }

    @Test
    fun `200 tambien es alcanzable (cualquier codigo HTTP vale)`() {
        nextCode = 200
        val diag = diagnostics().diagnose(last401AtMs = 0L)
        assertTrue(diag.anyReachable)
        assertEquals(200, diag.hosts[0].httpCode)
    }

    @Test
    fun `summary dice host DNS y codigo por linea`() {
        val diag = diagnostics().diagnose(last401AtMs = 0L)
        val text = diag.summary()
        assertTrue(text.contains("muselink.ai.studio"))
        assertTrue(text.contains("muselink-667333806336.us-west1.run.app"))
        assertTrue(text.contains("DNS OK"))
        assertTrue(text.contains("HTTP 401 (alcanzable)"))
        assertEquals(2, text.lines().size)
    }

    @Test
    fun `summary con todo caido dice sin respuesta`() {
        val diag = OrganismTokenDiagnostics(
            dns = fakeDns,
            client = OkHttpClient.Builder()
                .addInterceptor(Interceptor { throw IOException("connection refused (simulado)") })
                .build(),
            nowMs = { 1_000_000L },
            logger = NoOpOrganismLogger()
        ).diagnose(last401AtMs = 0L)
        assertTrue(diag.summary().contains("sin respuesta (revisar red)"))
    }
}
