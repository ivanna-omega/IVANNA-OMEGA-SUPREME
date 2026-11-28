package com.ivanna.omega.organism

import com.google.gson.JsonObject
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.file.Files

/**
 * Tests del fetcher GET/POST del perfil vigente (T1, release 510).
 * Mismo patrón que OrganismSenderTest: un interceptor OkHttp simula el
 * servidor, sin sockets (el loopback Java no recibe la conexión en esta
 * VM) y sin tocar la red real. JVM plana: el fetcher usa un logger
 * inyectable, no android.util.Log directo.
 *
 * La compilación/ejecución queda para la CI (Gradle no corre en esta VM:
 * conexión cliente→daemon bloqueada).
 */
class ProfileFetcherTest {

    private lateinit var dir: File
    private var nextCode: Int = 200
    private var nextBody: String = ""
    private var lastMethod: String? = null
    private var lastAuth: String? = null
    private var lastPath: String? = null
    private var hits: Int = 0

    @Before
    fun setup() {
        dir = Files.createTempDirectory("profile-fetcher-test").toFile()
        nextBody = envelope(version = 3L, status = "stable", abState = "promoted")
    }

    @After
    fun teardown() {
        dir.deleteRecursively()
    }

    /** Cliente OkHttp que NO toca la red: el interceptor simula el servidor. */
    private fun fakeClient(): OkHttpClient = OkHttpClient.Builder()
        .addInterceptor(Interceptor { chain ->
            val req = chain.request()
            // Host caído simulado: puerto 1 = connection refused.
            if (req.url.host == "127.0.0.1" && req.url.port == 1) {
                throw IOException("connection refused (simulado)")
            }
            hits++
            lastMethod = req.method
            lastAuth = req.header("Authorization")
            lastPath = req.url.encodedPath
            Response.Builder()
                .request(req)
                .protocol(Protocol.HTTP_1_1)
                .code(nextCode)
                .message("test")
                .body(nextBody.toResponseBody(null))
                .build()
        })
        .build()

    private fun localHost(): String = "http://127.0.0.1:8080"

    private fun envelope(
        version: Long,
        status: String,
        abState: String,
        profileId: String = "omega-1795857152818-9lec",
        // C24/510: señal opcional del servidor (null = no la anuncia).
        minSupportedPayloadVersion: Long? = null,
        // C16/510: señal LLM opcional del servidor (null = no la anuncia).
        llmStatus: String? = null,
        llmModel: String? = null,
        llmDrafted: Boolean? = null
    ): String {
        val minSupported = minSupportedPayloadVersion?.let { ",\"minSupportedPayloadVersion\":$it" } ?: ""
        val llm = (llmStatus?.let { ",\"llmStatus\":\"$it\"" } ?: "") +
            (llmModel?.let { ",\"llmModel\":\"$it\"" } ?: "") +
            (llmDrafted?.let { ",\"llmDrafted\":$it" } ?: "")
        return """{"profileId":"$profileId","version":$version,"status":"$status","abState":"$abState"$minSupported$llm,"compiledAt":"2026-11-28T10:03:03Z","sessionsCount":177,"profile":{"format":"IVANNA OMEGA","track":"Sesión en vivo IVANNA"}}"""
    }

    private fun fetcher(
        token: String? = "test-token-1234567890",
        hosts: List<String> = listOf(localHost()),
        enforceAllowlist: Boolean = false
    ): ProfileFetcher = ProfileFetcher(
        tokenStore = InMemoryTokenStore(token),
        client = fakeClient(),
        hosts = hosts,
        // Los tests usan el servidor local 127.0.0.1:8080, fuera de la
        // allowlist de producción; la allowlist queda siempre activa en
        // producción (OrganismController usa el default).
        enforceAllowlist = enforceAllowlist,
        logger = NoOpOrganismLogger()
    )

    @Test
    fun `GET current con perfil nuevo devuelve Current con Bearer y el path correcto`() {
        nextCode = 200
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
        val current = result as ProfileFetcher.Result.Current
        assertEquals("omega-1795857152818-9lec", current.profileId)
        assertEquals(3L, current.version)
        assertEquals("stable", current.status)
        assertEquals("promoted", current.abState)
        assertTrue(current.profileBody.contains("IVANNA OMEGA"))
        assertEquals("GET", lastMethod)
        assertEquals("Bearer test-token-1234567890", lastAuth)
        assertEquals("/api/v1/muse/ivanna/profile/current", lastPath)
        assertEquals(1, hits)
    }

    @Test
    fun `version igual es NotModified sin descargar el perfil`() {
        nextCode = 200
        val result = fetcher().fetch(appliedVersion = 3L)
        assertTrue("se esperaba NotModified, fue $result", result is ProfileFetcher.Result.NotModified)
        assertEquals(1, hits) // el GET se hace (es la comprobación de versión); no hay nada más que hacer
    }

    @Test
    fun `version menor tambien llega como Current (el controlador decide)`() {
        nextCode = 200
        val result = fetcher().fetch(appliedVersion = 5L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
        assertEquals(3L, (result as ProfileFetcher.Result.Current).version)
    }

    @Test
    fun `401 es TokenInvalid`() {
        nextCode = 401
        assertEquals(ProfileFetcher.Result.TokenInvalid, fetcher().fetch(appliedVersion = 2L))
    }

    @Test
    fun `429 es Retry`() {
        nextCode = 429
        assertEquals(ProfileFetcher.Result.Retry, fetcher().fetch(appliedVersion = 2L))
    }

    @Test
    fun `500 es Retry`() {
        nextCode = 500
        assertEquals(ProfileFetcher.Result.Retry, fetcher().fetch(appliedVersion = 2L))
    }

    @Test
    fun `404 es Quarantine (aun no hay perfil compilado)`() {
        nextCode = 404
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
        assertTrue((result as ProfileFetcher.Result.Quarantine).reason.contains("404"))
    }

    @Test
    fun `403 es Quarantine`() {
        nextCode = 403
        assertTrue(fetcher().fetch(appliedVersion = 2L) is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `sin token no se intenta la red`() {
        val result = fetcher(token = null).fetch(appliedVersion = 2L)
        assertEquals(ProfileFetcher.Result.NoToken, result)
        assertEquals(0, hits)
    }

    @Test
    fun `host fuera de la allowlist no se conecta ni recibe el Bearer`() {
        val result = fetcher(hosts = listOf("http://evil.example.com"), enforceAllowlist = true)
            .fetch(appliedVersion = 2L)
        assertEquals(ProfileFetcher.Result.Retry, result)
        assertEquals(0, hits)
        assertEquals(null, lastAuth)
    }

    @Test
    fun `host caido pasa al alterno`() {
        nextCode = 200
        val f = fetcher(hosts = listOf("http://127.0.0.1:1", localHost()))
        val result = f.fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
        assertEquals(1, hits)
        assertEquals("/api/v1/muse/ivanna/profile/current", lastPath)
    }

    @Test
    fun `todos los hosts caidos es Retry`() {
        val f = fetcher(hosts = listOf("http://127.0.0.1:1"))
        assertEquals(ProfileFetcher.Result.Retry, f.fetch(appliedVersion = 2L))
        assertEquals(0, hits)
    }

    @Test
    fun `sobre sin profile es Quarantine`() {
        nextCode = 200
        nextBody = """{"profileId":"omega-1","version":3,"status":"stable","abState":"promoted"}"""
        assertTrue(fetcher().fetch(appliedVersion = 2L) is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `status desconocido es Quarantine`() {
        nextCode = 200
        nextBody = envelope(version = 3L, status = "experimental", abState = "promoted")
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `abState desconocido es Quarantine`() {
        nextCode = 200
        nextBody = envelope(version = 3L, status = "stable", abState = "weird")
        assertTrue(fetcher().fetch(appliedVersion = 2L) is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `version menor de 1 es Quarantine`() {
        nextCode = 200
        nextBody = envelope(version = 0L, status = "stable", abState = "promoted")
        assertTrue(fetcher().fetch(appliedVersion = null) is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `JSON roto es Quarantine`() {
        nextCode = 200
        nextBody = "{no-json"
        assertTrue(fetcher().fetch(appliedVersion = 2L) is ProfileFetcher.Result.Quarantine)
    }

    // C24/510 — señal minSupportedPayloadVersion del servidor.

    @Test
    fun `Current lleva minSupportedPayloadVersion cuando el servidor la anuncia`() {
        nextCode = 200
        nextBody = envelope(version = 3L, status = "stable", abState = "promoted", minSupportedPayloadVersion = 2L)
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
        assertEquals(2L, (result as ProfileFetcher.Result.Current).minSupportedPayloadVersion)
    }

    @Test
    fun `sin senal minSupportedPayloadVersion es null en Current y NotModified`() {
        nextCode = 200
        val current = fetcher().fetch(appliedVersion = 2L)
        assertTrue(current is ProfileFetcher.Result.Current)
        assertNull((current as ProfileFetcher.Result.Current).minSupportedPayloadVersion)
        val notModified = fetcher().fetch(appliedVersion = 3L)
        assertTrue("se esperaba NotModified, fue $notModified", notModified is ProfileFetcher.Result.NotModified)
        assertNull((notModified as ProfileFetcher.Result.NotModified).minSupportedPayloadVersion)
    }

    @Test
    fun `NotModified tambien lleva la senal del servidor`() {
        nextCode = 200
        nextBody = envelope(version = 3L, status = "stable", abState = "promoted", minSupportedPayloadVersion = 2L)
        val result = fetcher().fetch(appliedVersion = 3L)
        assertTrue("se esperaba NotModified, fue $result", result is ProfileFetcher.Result.NotModified)
        assertEquals(2L, (result as ProfileFetcher.Result.NotModified).minSupportedPayloadVersion)
    }

    @Test
    fun `minSupportedPayloadVersion no numerica o menor de 1 es Quarantine`() {
        nextCode = 200
        nextBody = """{"profileId":"omega-1","version":3,"status":"stable","abState":"promoted","minSupportedPayloadVersion":"dos","profile":{"format":"IVANNA OMEGA"}}"""
        assertTrue(fetcher().fetch(appliedVersion = 2L) is ProfileFetcher.Result.Quarantine)
        nextBody = envelope(version = 3L, status = "stable", abState = "promoted", minSupportedPayloadVersion = 0L)
        assertTrue(fetcher().fetch(appliedVersion = 2L) is ProfileFetcher.Result.Quarantine)
    }

    // C16/510 — señal LLM del cerebro (llmStatus / llmModel / llmDrafted).

    @Test
    fun `Current lleva la senal LLM cuando el servidor la anuncia`() {
        nextCode = 200
        nextBody = envelope(
            version = 3L, status = "stable", abState = "promoted",
            llmStatus = "degraded", llmModel = "duckduckgo-web/claude-haiku-4-5", llmDrafted = true
        )
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
        val current = result as ProfileFetcher.Result.Current
        assertEquals("degraded", current.llmStatus)
        assertEquals("duckduckgo-web/claude-haiku-4-5", current.llmModel)
        assertEquals(true, current.llmDrafted)
    }

    @Test
    fun `NotModified tambien lleva la senal LLM (la salud cambia sin perfil nuevo)`() {
        nextCode = 200
        nextBody = envelope(
            version = 3L, status = "stable", abState = "promoted",
            llmStatus = "unavailable", llmDrafted = false
        )
        val result = fetcher().fetch(appliedVersion = 3L)
        assertTrue("se esperaba NotModified, fue $result", result is ProfileFetcher.Result.NotModified)
        val notModified = result as ProfileFetcher.Result.NotModified
        assertEquals("unavailable", notModified.llmStatus)
        assertNull(notModified.llmModel)
        assertEquals(false, notModified.llmDrafted)
    }

    @Test
    fun `sin senal LLM es null en Current y NotModified`() {
        nextCode = 200
        val current = fetcher().fetch(appliedVersion = 2L)
        assertTrue(current is ProfileFetcher.Result.Current)
        assertNull((current as ProfileFetcher.Result.Current).llmStatus)
        assertNull(current.llmModel)
        assertNull(current.llmDrafted)
        val notModified = fetcher().fetch(appliedVersion = 3L)
        assertTrue("se esperaba NotModified, fue $notModified", notModified is ProfileFetcher.Result.NotModified)
        assertNull((notModified as ProfileFetcher.Result.NotModified).llmStatus)
        assertNull(notModified.llmModel)
        assertNull(notModified.llmDrafted)
    }

    @Test
    fun `llmStatus desconocido se ignora, no pone el perfil en cuarentena`() {
        nextCode = 200
        nextBody = envelope(version = 3L, status = "stable", abState = "promoted", llmStatus = "muerto")
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current (senal ignorada), fue $result", result is ProfileFetcher.Result.Current)
        assertNull((result as ProfileFetcher.Result.Current).llmStatus)
    }

    @Test
    fun `llmDrafted no booleano y llmModel vacio se ignoran`() {
        nextCode = 200
        nextBody = """{"profileId":"omega-1","version":3,"status":"stable","abState":"promoted","llmStatus":"ok","llmModel":"","llmDrafted":"si","profile":{"format":"IVANNA OMEGA"}}"""
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current (senales invalidas ignoradas), fue $result", result is ProfileFetcher.Result.Current)
        val current = result as ProfileFetcher.Result.Current
        assertEquals("ok", current.llmStatus)
        assertNull(current.llmModel)
        assertNull(current.llmDrafted)
    }

    @Test
    fun `POST applied 200 es Confirmed con el path correcto`() {
        nextCode = 200
        val body = JsonObject().apply {
            addProperty("profileId", "omega-1795857152818-9lec")
            addProperty("version", 3L)
        }
        assertEquals(ProfileFetcher.ConfirmOutcome.Confirmed, fetcher().confirmApplied(body))
        assertEquals("POST", lastMethod)
        assertEquals("/api/v1/muse/ivanna/profile/applied", lastPath)
        assertEquals("Bearer test-token-1234567890", lastAuth)
    }

    @Test
    fun `POST applied 401 es TokenInvalid`() {
        nextCode = 401
        assertEquals(
            ProfileFetcher.ConfirmOutcome.TokenInvalid,
            fetcher().confirmApplied(JsonObject())
        )
    }

    @Test
    fun `POST applied 500 es Retry`() {
        nextCode = 500
        assertEquals(
            ProfileFetcher.ConfirmOutcome.Retry,
            fetcher().confirmApplied(JsonObject())
        )
    }

    @Test
    fun `POST applied 404 es Quarantine (el endpoint aun no existe)`() {
        nextCode = 404
        assertEquals(
            ProfileFetcher.ConfirmOutcome.Quarantine,
            fetcher().confirmApplied(JsonObject())
        )
    }
}
