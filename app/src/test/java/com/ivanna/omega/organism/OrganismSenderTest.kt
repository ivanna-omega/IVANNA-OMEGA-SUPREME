package com.ivanna.omega.organism

import com.google.gson.JsonObject
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.file.Files

/**
 * Tests del sender HTTPS contra un organismo simulado (v2.5.506).
 * Cubre el mapeo código HTTP → Outcome y el fallback entre hosts, sin
 * tocar la red real ni el organismo. JVM plana (sin Robolectric: el
 * sender usa un logger inyectable, no android.util.Log directo).
 *
 * SIN SOCKETS: un interceptor OkHttp responde con el código de prueba.
 * Un ServerSocket real (incluso anclado a 127.0.0.1) no recibe la
 * conexión en esta VM: el loopback Java dual-stack conecta desde una
 * IPv6 ULA y el accept nunca llega (verificado con programa mínimo).
 * El interceptor prueba exactamente lo mismo (código → Outcome,
 * Bearer, path, fallback) sin depender del loopback.
 */
class OrganismSenderTest {

    private lateinit var dir: File
    private var nextCode: Int = 200
    private var lastAuth: String? = null
    private var lastPath: String? = null
    private var hits: Int = 0

    @Before
    fun setup() {
        dir = Files.createTempDirectory("organism-sender-test").toFile()
    }

    @After
    fun teardown() {
        dir.deleteRecursively()
    }

    /** Cliente OkHttp que NO toca la red: el interceptor simula el organismo. */
    private fun fakeClient(): OkHttpClient = OkHttpClient.Builder()
        .addInterceptor(Interceptor { chain ->
            val req = chain.request()
            // Host caído simulado: puerto 1 = connection refused.
            if (req.url.host == "127.0.0.1" && req.url.port == 1) {
                throw IOException("connection refused (simulado)")
            }
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

    private fun localHost(): String = "http://127.0.0.1:8080"

    private fun sender(
        queue: OrganismQueue,
        token: String? = "test-token-1234567890",
        hosts: List<String> = listOf(localHost())
    ): OrganismSender {
        return OrganismSender(
            tokenStore = InMemoryTokenStore(token),
            queue = queue,
            client = fakeClient(),
            hosts = hosts,
            nowMs = { 1000L },
            jitter = { 0.0 },
            logger = NoOpOrganismLogger(),
            // C9/510: los tests usan el servidor local 127.0.0.1:8080, fuera
            // de la allowlist de hosts de producción; la allowlist queda
            // siempre activa en producción (OrganismController usa el default).
            enforceAllowlist = false
        )
    }

    private fun enqueue(queue: OrganismQueue): OrganismQueue.Item {
        val body = JsonObject().apply {
            addProperty("song", "Sesión en vivo IVANNA")
            addProperty("artist", "IVANNA OMEGA")
        }
        queue.enqueue("s1", 0, body)
        return queue.nextReady(1000L)!!
    }

    @Test
    fun `200 vacia la cola y manda Bearer sin exponer el token en otro lado`() {
        nextCode = 200
        val queue = OrganismQueue(dir)
        val s = sender(queue)
        enqueue(queue)
        val result = s.flush()
        assertEquals(1, result.sent)
        assertEquals(OrganismSender.Outcome.SENT, result.outcome)
        assertTrue(queue.listAll().isEmpty())
        assertEquals("Bearer test-token-1234567890", lastAuth)
        assertEquals("/api/v1/muse/ivanna/session", lastPath)
        assertEquals(1, hits)
    }

    @Test
    fun `401 es TOKEN_INVALID y el chunk queda en PENDING sin consumir intentos`() {
        nextCode = 401
        val queue = OrganismQueue(dir)
        val s = sender(queue)
        enqueue(queue)
        val result = s.flush()
        assertEquals(OrganismSender.Outcome.TOKEN_INVALID, result.outcome)
        assertEquals(0, result.sent)
        val items = queue.listAll()
        assertEquals(1, items.size)
        // Defecto #3: un 401 es configuración, no transitorio: no consume
        // intentos ni va a cuarentena aunque se repita 8 veces.
        assertEquals(OrganismQueue.State.PENDING, items[0].state)
        assertEquals(0, items[0].attempts)
    }

    @Test
    fun `500 y 429 son RETRY`() {
        for (code in listOf(500, 429, 503)) {
            nextCode = code
            val qdir = Files.createTempDirectory("organism-sender-5xx").toFile()
            try {
                val queue = OrganismQueue(qdir)
                val s = OrganismSender(
                    InMemoryTokenStore("test-token-1234567890"), queue,
                    client = fakeClient(),
                    hosts = listOf(localHost()), nowMs = { 1000L }, jitter = { 0.0 },
                    logger = NoOpOrganismLogger(),
                    enforceAllowlist = false  // C9/510: servidor local de test
                )
                val body = JsonObject().apply { addProperty("song", "x") }
                queue.enqueue("s1", 0, body)
                val result = s.flush()
                assertEquals("HTTP $code", OrganismSender.Outcome.RETRY, result.outcome)
                assertEquals(OrganismQueue.State.RETRY, queue.listAll()[0].state)
            } finally {
                qdir.deleteRecursively()
            }
        }
    }

    @Test
    fun `400 es QUARANTINE y no se reintenta`() {
        nextCode = 400
        val queue = OrganismQueue(dir)
        val s = sender(queue)
        enqueue(queue)
        val result = s.flush()
        assertEquals(OrganismSender.Outcome.QUARANTINE, result.outcome)
        assertEquals(OrganismQueue.State.QUARANTINED, queue.listAll()[0].state)
        assertEquals(1, hits)
    }

    @Test
    fun `sin token no se intenta la red`() {
        val queue = OrganismQueue(dir)
        val s = sender(queue, token = null)
        enqueue(queue)
        hits = 0
        val result = s.flush()
        assertEquals(OrganismSender.Outcome.NO_TOKEN, result.outcome)
        assertEquals(0, hits)
        assertEquals(OrganismQueue.State.PENDING, queue.listAll()[0].state)
    }

    @Test
    fun `host caido hace fallback al segundo host`() {
        nextCode = 200
        val queue = OrganismQueue(dir)
        val dead = "http://127.0.0.1:1" // puerto cerrado: connection refused
        val s = sender(queue, hosts = listOf(dead, localHost()))
        enqueue(queue)
        val result = s.flush()
        assertEquals(OrganismSender.Outcome.SENT, result.outcome)
        assertEquals(1, result.sent)
        assertTrue(queue.listAll().isEmpty())
    }
}
