package com.ivanna.omega.organism

import com.google.gson.JsonObject
import okhttp3.Interceptor
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Response
import okhttp3.ResponseBody.Companion.toResponseBody
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.Signature
import java.util.Base64

/**
 * Tests del fetcher GET/POST del perfil vigente (T1, release 510).
 * Mismo patrón que OrganismSenderTest: un interceptor OkHttp simula el
 * servidor, sin sockets (el loopback Java no recibe la conexión en esta
 * VM) y sin tocar la red real. JVM plana: el fetcher usa un logger
 * inyectable, no android.util.Log directo.
 *
 * La compilación/ejecución queda para la CI (Gradle no corre en esta VM:
 * conexión cliente→daemon bloqueada).
 *
 * C3/510 — verificación de firma Ed25519: los tests de firma usan pares
 * de claves de PRUEBA generados aquí mismo (nunca una clave de
 * producción) y los vectores conocidos de RFC 8032 §7.1. La clave
 * pública de prueba se deriva del par generado (últimos 32 bytes del
 * X.509), igual que el servidor derivaría la suya.
 */
class ProfileFetcherTest {

    companion object {
        /**
         * C3/510 — par de claves de PRUEBA (generado en el test, nunca
         * de producción). `TEST_KEY_PAIR` es la clave vigente y
         * `TEST_KEY_PAIR_OLD` la anterior (rotación).
         */
        private val TEST_KEY_PAIR: KeyPair =
            KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
        private val TEST_KEY_PAIR_OLD: KeyPair =
            KeyPairGenerator.getInstance("Ed25519").generateKeyPair()
        private const val TEST_KEY_ID = "test-key-1"
        private const val TEST_KEY_ID_OLD = "test-key-0"

        /**
         * C3/510 — el `.ivanna.json` del sobre de prueba, como cadena
         * compacta exacta. Es el mensaje que se firma: el fetcher
         * verifica `profileEl.toString()` y Gson lo reproduce byte a
         * byte (mismo orden, mismos literales, sin escapes HTML).
         */
        private const val PROFILE_JSON =
            """{"format":"IVANNA OMEGA","track":"Sesión en vivo IVANNA"}"""

        /** Clave pública cruda (32 bytes) del par, en Base64 estándar. */
        private fun rawPublicKeyB64(keyPair: KeyPair): String {
            val x509 = keyPair.public.encoded
            require(x509.size == 44) {
                "se esperaba X.509 Ed25519 de 44 bytes, fue ${x509.size}"
            }
            return Base64.getEncoder().encodeToString(x509.copyOfRange(12, 44))
        }

        /** Firma Ed25519 del mensaje UTF-8, en Base64 estándar. */
        private fun signB64(keyPair: KeyPair, message: String): String {
            val signer = Signature.getInstance("Ed25519")
            signer.initSign(keyPair.private)
            signer.update(message.toByteArray(Charsets.UTF_8))
            return Base64.getEncoder().encodeToString(signer.sign())
        }

        private fun hexToBytes(hex: String): ByteArray {
            val clean = hex.filterNot { it.isWhitespace() }
            require(clean.length % 2 == 0) { "hex de longitud impar" }
            return ByteArray(clean.length / 2) { i ->
                clean.substring(2 * i, 2 * i + 2).toInt(16).toByte()
            }
        }

        private fun hexToB64(hex: String): String =
            Base64.getEncoder().encodeToString(hexToBytes(hex))
    }

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
        llmDrafted: Boolean? = null,
        // C3/510: firma Ed25519 opcional del sobre (null = sin firmar).
        signature: String? = null,
        signerKeyId: String? = null
    ): String {
        val minSupported = minSupportedPayloadVersion?.let { ",\"minSupportedPayloadVersion\":$it" } ?: ""
        val llm = (llmStatus?.let { ",\"llmStatus\":\"$it\"" } ?: "") +
            (llmModel?.let { ",\"llmModel\":\"$it\"" } ?: "") +
            (llmDrafted?.let { ",\"llmDrafted\":$it" } ?: "")
        val sig = (signature?.let { ",\"signature\":\"$it\"" } ?: "") +
            (signerKeyId?.let { ",\"signerKeyId\":\"$it\"" } ?: "")
        return """{"profileId":"$profileId","version":$version,"status":"$status","abState":"$abState"$minSupported$llm,"compiledAt":"2026-11-28T10:03:03Z","sessionsCount":177,"profile":$PROFILE_JSON$sig}"""
    }

    private fun fetcher(
        token: String? = "test-token-1234567890",
        hosts: List<String> = listOf(localHost()),
        enforceAllowlist: Boolean = false,
        // C3/510: enforcement OFF por defecto (igual que en producción).
        enforceSignature: Boolean = false,
        signerPublicKeys: Map<String, String> = emptyMap()
    ): ProfileFetcher = ProfileFetcher(
        tokenStore = InMemoryTokenStore(token),
        client = fakeClient(),
        hosts = hosts,
        // Los tests usan el servidor local 127.0.0.1:8080, fuera de la
        // allowlist de producción; la allowlist queda siempre activa en
        // producción (OrganismController usa el default).
        enforceAllowlist = enforceAllowlist,
        logger = NoOpOrganismLogger(),
        enforceSignature = enforceSignature,
        signerPublicKeys = signerPublicKeys
    )

    /** Sobre firmado con la clave de PRUEBA vigente sobre PROFILE_JSON. */
    private fun signedEnvelope(
        version: Long = 3L,
        status: String = "stable",
        abState: String = "promoted",
        keyPair: KeyPair = TEST_KEY_PAIR,
        keyId: String = TEST_KEY_ID
    ): String = envelope(
        version = version, status = status, abState = abState,
        signature = signB64(keyPair, PROFILE_JSON), signerKeyId = keyId
    )

    /** Mapa de claves de PRUEBA: una entrada (vigente) o dos (rotación). */
    private fun testKeys(rotation: Boolean = false): Map<String, String> =
        if (rotation) {
            mapOf(
                TEST_KEY_ID to rawPublicKeyB64(TEST_KEY_PAIR),
                TEST_KEY_ID_OLD to rawPublicKeyB64(TEST_KEY_PAIR_OLD)
            )
        } else {
            mapOf(TEST_KEY_ID to rawPublicKeyB64(TEST_KEY_PAIR))
        }

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

    // C3/510 — verificación de firma Ed25519 (gated OFF por defecto).

    // Vectores conocidos: RFC 8032 §7.1, tests 1–3 (Ed25519 puro).
    // Clave pública y firma en hex del RFC; el mensaje, en bytes.

    @Test
    fun `RFC 8032 vector 1 verifica (mensaje vacio)`() {
        assertTrue(
            ProfileFetcher.verifyEd25519Signature(
                publicKeyBase64 = hexToB64("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
                message = byteArrayOf(),
                signatureBase64 = hexToB64(
                    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155" +
                        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
                )
            )
        )
    }

    @Test
    fun `RFC 8032 vector 2 verifica (mensaje 0x72)`() {
        assertTrue(
            ProfileFetcher.verifyEd25519Signature(
                publicKeyBase64 = hexToB64("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"),
                message = hexToBytes("72"),
                signatureBase64 = hexToB64(
                    "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da" +
                        "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"
                )
            )
        )
    }

    @Test
    fun `RFC 8032 vector 3 verifica (mensaje af82)`() {
        assertTrue(
            ProfileFetcher.verifyEd25519Signature(
                publicKeyBase64 = hexToB64("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025"),
                message = hexToBytes("af82"),
                signatureBase64 = hexToB64(
                    "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac" +
                        "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"
                )
            )
        )
    }

    @Test
    fun `firma RFC 8032 con un bit cambiado no verifica`() {
        val sig = hexToBytes(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155" +
                "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
        )
        sig[0] = (sig[0].toInt() xor 1).toByte()
        assertFalse(
            ProfileFetcher.verifyEd25519Signature(
                publicKeyBase64 = hexToB64("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"),
                message = byteArrayOf(),
                signatureBase64 = Base64.getEncoder().encodeToString(sig)
            )
        )
    }

    @Test
    fun `verificacion pura rechaza mensaje distinto, otra clave y entradas malformadas`() {
        val publicKey = hexToB64("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")
        val signature = hexToB64(
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155" +
                "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"
        )
        // Mensaje distinto.
        assertFalse(ProfileFetcher.verifyEd25519Signature(publicKey, byteArrayOf(0x00), signature))
        // Otra clave pública.
        assertFalse(
            ProfileFetcher.verifyEd25519Signature(
                hexToB64("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c"),
                byteArrayOf(), signature
            )
        )
        // Base64 inválido, clave de 31 bytes, firma de 63 bytes: false, sin lanzar.
        assertFalse(ProfileFetcher.verifyEd25519Signature("!!!no-base64!!!", byteArrayOf(), signature))
        assertFalse(
            ProfileFetcher.verifyEd25519Signature(
                Base64.getEncoder().encodeToString(ByteArray(31)), byteArrayOf(), signature
            )
        )
        assertFalse(
            ProfileFetcher.verifyEd25519Signature(
                publicKey, byteArrayOf(),
                Base64.getEncoder().encodeToString(ByteArray(63))
            )
        )
    }

    @Test
    fun `enforcement OFF (defecto) - perfil sin firma se acepta igual`() {
        nextCode = 200
        val result = fetcher().fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
    }

    @Test
    fun `enforcement OFF - firma valida con claves configuradas se acepta (monitor)`() {
        nextCode = 200
        nextBody = signedEnvelope()
        val result = fetcher(signerPublicKeys = testKeys()).fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
    }

    @Test
    fun `enforcement OFF - firma INVALIDA con claves configuradas se acepta igual (monitor, no cuarentena)`() {
        nextCode = 200
        // Firma válida de OTRO cuerpo: el fetcher verifica PROFILE_JSON.
        nextBody = envelope(
            version = 3L, status = "stable", abState = "promoted",
            signature = signB64(TEST_KEY_PAIR, "otro cuerpo"), signerKeyId = TEST_KEY_ID
        )
        val result = fetcher(signerPublicKeys = testKeys()).fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current (gated OFF), fue $result", result is ProfileFetcher.Result.Current)
    }

    @Test
    fun `enforcement ON - firma valida devuelve Current`() {
        nextCode = 200
        nextBody = signedEnvelope()
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Current, fue $result", result is ProfileFetcher.Result.Current)
        assertEquals(3L, (result as ProfileFetcher.Result.Current).version)
    }

    @Test
    fun `enforcement ON - firma valida en NotModified no bloquea el sondeo`() {
        nextCode = 200
        nextBody = signedEnvelope()
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 3L)
        assertTrue("se esperaba NotModified, fue $result", result is ProfileFetcher.Result.NotModified)
    }

    @Test
    fun `enforcement ON - perfil sin firma es Quarantine`() {
        nextCode = 200
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
        assertTrue((result as ProfileFetcher.Result.Quarantine).reason.contains("firma Ed25519"))
    }

    @Test
    fun `enforcement ON - firma invalida es Quarantine`() {
        nextCode = 200
        nextBody = envelope(
            version = 3L, status = "stable", abState = "promoted",
            signature = signB64(TEST_KEY_PAIR, "otro cuerpo"), signerKeyId = TEST_KEY_ID
        )
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `enforcement ON - firma corrupta (un byte) es Quarantine`() {
        nextCode = 200
        val sig = Base64.getDecoder().decode(signB64(TEST_KEY_PAIR, PROFILE_JSON))
        sig[0] = (sig[0].toInt() xor 1).toByte()
        nextBody = envelope(
            version = 3L, status = "stable", abState = "promoted",
            signature = Base64.getEncoder().encodeToString(sig), signerKeyId = TEST_KEY_ID
        )
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `enforcement ON - signerKeyId desconocido es Quarantine`() {
        nextCode = 200
        nextBody = signedEnvelope(keyId = "clave-que-no-existe")
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `enforcement ON - sin claves provisionadas todo es Quarantine (fail-closed)`() {
        nextCode = 200
        nextBody = signedEnvelope()
        val result = fetcher(enforceSignature = true, signerPublicKeys = emptyMap())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `enforcement ON - firma sin signerKeyId es Quarantine`() {
        nextCode = 200
        nextBody = envelope(
            version = 3L, status = "stable", abState = "promoted",
            signature = signB64(TEST_KEY_PAIR, PROFILE_JSON)
        )
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `enforcement ON - NotModified sin firma es Quarantine (el sobre no se acepta en ningun caso)`() {
        nextCode = 200
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys())
            .fetch(appliedVersion = 3L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }

    @Test
    fun `rotacion - con dos claves, firman la vieja y la nueva (enforcement ON)`() {
        nextCode = 200
        val keys = testKeys(rotation = true)
        nextBody = signedEnvelope(keyPair = TEST_KEY_PAIR_OLD, keyId = TEST_KEY_ID_OLD)
        val oldResult = fetcher(enforceSignature = true, signerPublicKeys = keys)
            .fetch(appliedVersion = 2L)
        assertTrue("firma con clave vieja: se esperaba Current, fue $oldResult", oldResult is ProfileFetcher.Result.Current)
        nextBody = signedEnvelope(keyPair = TEST_KEY_PAIR, keyId = TEST_KEY_ID)
        val newResult = fetcher(enforceSignature = true, signerPublicKeys = keys)
            .fetch(appliedVersion = 2L)
        assertTrue("firma con clave nueva: se esperaba Current, fue $newResult", newResult is ProfileFetcher.Result.Current)
    }

    @Test
    fun `rotacion - clave vieja retirada del mapa ya no verifica`() {
        nextCode = 200
        nextBody = signedEnvelope(keyPair = TEST_KEY_PAIR_OLD, keyId = TEST_KEY_ID_OLD)
        val result = fetcher(enforceSignature = true, signerPublicKeys = testKeys(rotation = false))
            .fetch(appliedVersion = 2L)
        assertTrue("se esperaba Quarantine, fue $result", result is ProfileFetcher.Result.Quarantine)
    }
}
