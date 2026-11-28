package com.ivanna.omega.organism

import com.google.gson.JsonObject
import com.google.gson.JsonParser
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response
import java.security.KeyFactory
import java.security.Signature
import java.security.spec.X509EncodedKeySpec
import java.util.Base64

/**
 * ProfileFetcher — GET del perfil vigente + POST de confirmación (T1,
 * release 510; la VUELTA de la refactorización §3).
 *
 * Hoy el Organismo solo sube (OrganismSender solo hace POST; ningún GET
 * existe). Este cliente cierra el ciclo por el lado de la descarga:
 *   GET  /api/v1/muse/ivanna/profile/current   — perfil versionado
 *   POST /api/v1/muse/ivanna/profile/applied   — confirmación de aplicación
 *
 * Reutiliza [OrganismSender.defaultClient] (mismo [OrganismDns], 15 s,
 * `followRedirects(false)`) y el [TokenStore], contra los mismos
 * [OrganismSender.DEFAULT_HOSTS] vía [MuselinkHosts] — NO define su
 * propia lista de hosts. Prueba el host primario y luego el alterno
 * (igual que `OrganismSender.sendOne`). Maneja los errores con la misma
 * disciplina que el sender: RETRY en red/timeout/429/5xx, TOKEN_INVALID
 * en 401, QUARANTINE en 400/403/404/422 (404 = «aún no hay perfil
 * compilado», no es un error: el teléfono sigue con el último bueno).
 *
 * C24/510 — el sobre del GET puede traer además
 * `minSupportedPayloadVersion` (opcional): la versión mínima de
 * payload que el servidor acepta. Va en [Result.Current] y en
 * [Result.NotModified] (el servidor puede subir su mínimo sin publicar
 * un perfil nuevo); el controlador descarta la cola vieja con ella.
 *
 * C16/510 — el sobre puede traer además la salud del LLM del cerebro
 * (`llmStatus` / `llmModel` / `llmDrafted`, opcionales): es la parte
 * Android de C16. La parte del servidor (probar el modelo elegido
 * antes de cada tarea LLM, caer al siguiente del combo, y si no hay
 * ninguno seguir solo con el analizador determinista marcando el
 * informe «sin redacción LLM») vive en el servidor (OmniRoute /
 * MuseLink), NO en este repo: el teléfono no llama a ningún LLM en el
 * ciclo del organismo, así que no puede hacer el health-check él
 * mismo — solo consume y muestra la señal que publica el servidor.
 * Va en [Result.Current] y en [Result.NotModified] (la salud del LLM
 * puede cambiar sin publicar un perfil nuevo). Es SOLO telemetría de
 * display: un valor inválido se IGNORA (se trata como ausente, con
 * aviso en el log), jamás pone el perfil en cuarentena — un campo de
 * display no puede matar un perfil válido (al contrario que
 * `minSupportedPayloadVersion`, que sí dispara un descarte y por eso
 * es fail-closed). Sin señal, todo es null/desconocido y nada cambia.
 * Contrato de la señal (lo que debe publicar el servidor): `llmStatus`
 * en `ok | degraded | unavailable` (`ok` = la última prueba del
 * modelo elegido pasó; `degraded` = algún modelo del combo falló y
 * corre en fallback; `unavailable` = ningún LLM respondió, informes
 * solo deterministas); `llmModel` = modelo que redactó el perfil
 * vigente (p. ej. `duckduckgo-web/claude-haiku-4-5`); `llmDrafted` =
 * false si el perfil se compiló solo con el analizador determinista
 * («sin redacción LLM»).
 *
 * C3/510 — verificación de firma Ed25519 (Fase 6): el servidor firma
 * el `.ivanna.json` canónico con Ed25519 y sirve
 * `{profile, version, signature, signerKeyId}` — en concreto el
 * sobre trae `signature` (firma Ed25519, 64 bytes, Base64 estándar o
 * URL-safe) y `signerKeyId` (qué clave firmó). [parseCurrent] verifica
 * la firma contra la clave pública ANTES de devolver [Result.Current]
 * (y antes de [Result.NotModified]: un sobre sin firma válida no se
 * acepta en ningún caso cuando el enforcement está ON). Lo que se
 * verifica es exactamente lo que se aplica: los bytes UTF-8 de
 * `profileEl.toString()` (la misma cadena que viaja como
 * `profileBody` a la validación fail-closed de [ProfileApplier]).
 *
 * CONTRATO DE FIRMA (lo que debe hacer el servidor al firmar):
 *  - Firmar los bytes UTF-8 exactos del valor del campo `profile` tal
 *    como se sirve en el sobre: JSON compacto, mismo orden de claves y
 *    mismos literales numéricos que en el sobre. El teléfono re-deriva
 *    esos bytes con Gson (`JsonObject.toString()`), que preserva orden
 *    y literales y NO escapa `<>&='` (verificado en JVM con Gson
 *    2.15.0); el servidor DEBE firmar la cadena exacta que embebe en
 *    el sobre, no una re-serialización (una re-serialización que
 *    reescriba `1.0` como `1` o reordene claves rompe la verificación).
 *  - `signature` = firma Ed25519 pura (RFC 8032, sin contexto) de 64
 *    bytes, en Base64 estándar (con padding) o Base64 URL-safe.
 *  - `signerKeyId` = identificador de la clave que firmó; el teléfono
 *    busca la clave pública en [signerPublicKeys] por ese id.
 *  - Clave pública = 32 bytes crudos Ed25519 en Base64 (estándar o
 *    URL-safe). El teléfono la envuelve en SubjectPublicKeyInfo X.509
 *    (prefijo DER de Ed25519) para `KeyFactory`.
 *  - Los metadatos del sobre (`version`, `status`, `abState`) NO van
 *    cubiertos por la firma: la firma autentica el contenido del
 *    perfil (un perfil firmado es salida genuina del servidor, sea
 *    cual sea el sobre que lo acompañe); la política de versiones
 *    (solo `rolled_back` aplica una versión menor) la decide el
 *    controlador. Si en el futuro se quiere ligar versión y perfil, el
 *    contrato de firma debe cambiar en ambos lados a la vez.
 *
 * ⛔ GATED OFF POR DEFECTO — LEER ANTES DE ACTIVAR ([enforceSignature]
 * = false): el servidor AÚN NO firma y Esse NO ha decidido dónde vive
 * la clave privada. Si el enforcement se activara ahora, TODOS los
 * perfiles irían a cuarentena y la VUELTA se rompería. Por eso la
 * verificación existe y se prueba, pero NO se exige: con enforcement
 * OFF los perfiles sin firma se aceptan igual (la validación C2 de
 * [ProfileApplier] se aplica siempre) y, si el sobre trae firma y hay
 * claves configuradas, el resultado solo se registra en el log (modo
 * monitor: sirve para desplegar la firma en el servidor y ver que
 * verifica antes de exigirla). PARA ACTIVAR EL ENFORCEMENT falta, en
 * este orden: (1) decisión de Esse sobre la custodia de la clave
 * privada del servidor; (2) servidor firmando cada perfil con esa
 * clave y sirviendo `signature` + `signerKeyId`; (3) clave(s)
 * pública(s) REAL(es) provisionada(s) en [signerPublicKeys] (vacío
 * por defecto: sin claves no hay nada que verificar y con
 * enforcement ON todo iría a cuarentena); (4) poner
 * `enforceSignature = true` donde se construye el fetcher. La clave
 * privada del servidor NO existe en este repo ni debe existir jamás:
 * aquí solo vive la pública.
 *
 * Rotación: [signerPublicKeys] es un mapa `signerKeyId → clave
 * pública Base64`. En operación normal tiene UNA entrada; durante una
 * rotación tiene DOS (clave vieja + clave nueva) y el servidor indica
 * con `signerKeyId` cuál firmó cada perfil. Un `signerKeyId`
 * desconocido no verifica (fail-closed con enforcement ON).
 *
 * Implementación: API de plataforma (`java.security.Signature` con
 * «Ed25519», `KeyFactory` «Ed25519»; minSdk 33) sin dependencias
 * nuevas. Si el algoritmo no existiera en el dispositivo, la
 * verificación devuelve false (fail-closed con enforcement ON, sin
 * efecto con OFF). Toda la verificación es pura y testeable en JVM:
 * [verifyEd25519Signature] se prueba con los vectores de RFC 8032
 * §7.1 (ver ProfileFetcherTest).
 *
 * PENDIENTE END-TO-END: el endpoint del servidor (`GET current` +
 * `POST applied` + analizador determinista) NO está en este repo (vive
 * en la app MuseLink de AI Studio) y NO se implementa aquí. Hasta que
 * exista, el GET devolverá 404 → [Result.Quarantine] («aún no hay
 * perfil») y el teléfono seguirá con el último bueno; el POST
 * confirmará contra un 404 del mismo modo. Nada de esto toca el audio.
 *
 * Puro Kotlin + OkHttp + Gson, sin `android.util.Log` directo (mismo
 * patrón que [OrganismSender]): testeable en JVM plana con un
 * interceptor que simule el servidor.
 */
class ProfileFetcher(
    private val tokenStore: TokenStore,
    private val client: OkHttpClient = OrganismSender.defaultClient(),
    private val hosts: List<String> = OrganismSender.DEFAULT_HOSTS,
    private val enforceAllowlist: Boolean = true,
    private val logger: OrganismLogger = AndroidOrganismLogger(TAG),
    /**
     * C3/510 — enforcement de la firma Ed25519. ⛔ OFF POR DEFECTO y
     * debe seguir así hasta que se cumplan las 4 condiciones de la
     * cabecera (decisión de custodia de Esse + servidor firmando +
     * clave pública real provisionada en [signerPublicKeys] + este
     * flag a true): con enforcement ON, un sobre sin firma, con firma
     * inválida, con `signerKeyId` desconocido o sin claves
     * provisionadas va a [Result.Quarantine] (mismo camino que C2).
     * Con OFF, los perfiles sin firma se aceptan igual (C2 se aplica
     * siempre) y una firma presente solo se verifica en modo monitor
     * (resultado en el log, sin cuarentena).
     */
    private val enforceSignature: Boolean = false,
    /**
     * C3/510 — claves públicas Ed25519 del servidor, por
     * `signerKeyId`: `signerKeyId → clave pública de 32 bytes crudos
     * en Base64 (estándar o URL-safe)`. Vacío por defecto: la clave
     * pública real NO existe aún (el servidor aún no firma) y NO debe
     * hardcodearse aquí ninguna clave «real». Una entrada en operación
     * normal; DOS durante una rotación (vieja + nueva). Sin entrada
     * para el `signerKeyId` del sobre, la firma no verifica.
     */
    private val signerPublicKeys: Map<String, String> = emptyMap()
) {
    companion object {
        private const val TAG = "ProfileFetcher"
        const val CURRENT_PATH = "/api/v1/muse/ivanna/profile/current"
        const val APPLIED_PATH = "/api/v1/muse/ivanna/profile/applied"
        private val JSON = "application/json; charset=utf-8".toMediaType()

        /**
         * Tope de la respuesta del GET: un `.ivanna.json` legítimo pesa
         * ~2 KB y el sobre añade poco; 128 KB deja margen de sobra y
         * evita que una respuesta hostil llene la memoria. Por encima,
         * cuarentena (no se reintenta: el servidor no va a encoger).
         */
        const val MAX_RESPONSE_BYTES = 128 * 1024

        /** Vocabulario de `status` del sobre (VUELTA §3.1). */
        val KNOWN_STATUSES: Set<String> = setOf("stable", "candidate", "ab_running", "rolled_back")

        /** Vocabulario de `abState` del sobre (VUELTA §3.1). */
        val KNOWN_AB_STATES: Set<String> = setOf("none", "candidate", "ab_running", "promoted", "rolled_back")

        /**
         * C16/510 — vocabulario de `llmStatus` del sobre (salud del LLM
         * del cerebro, según la última prueba del servidor). Un valor
         * fuera de este vocabulario se IGNORA (no es cuarentena: es
         * solo display, ver cabecera).
         */
        val KNOWN_LLM_STATUSES: Set<String> = setOf("ok", "degraded", "unavailable")

        /**
         * C3/510 — prefijo DER de SubjectPublicKeyInfo para Ed25519
         * (OID 1.3.101.112): `SEQUENCE { SEQUENCE { OID Ed25519 }
         * BIT STRING }`. La clave pública configurada son 32 bytes
         * crudos; `KeyFactory` «Ed25519» exige el envoltorio X.509
         * completo (44 bytes), así que se antepone este prefijo de 12
         * bytes. Verificado en JVM (JDK 17): el `encoded` X.509 de
         * una clave generada con `KeyPairGenerator` «Ed25519» es
         * exactamente este prefijo + los 32 bytes crudos.
         */
        private val ED25519_X509_PREFIX = byteArrayOf(
            0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00
        )

        /**
         * C3/510 — verifica una firma Ed25519 pura (RFC 8032, sin
         * contexto) sobre [message].
         *
         * [publicKeyBase64] es la clave pública de 32 bytes crudos en
         * Base64 (estándar con padding, o URL-safe con o sin padding);
         * [signatureBase64] es la firma de 64 bytes en el mismo
         * formato. Cualquier entrada malformada (Base64 inválido,
         * longitudes distintas de 32/64, clave que `KeyFactory`
         * rechaza, algoritmo ausente en el dispositivo) devuelve
         * false — jamás lanza. Pura y sin estado: testeable en JVM
         * con los vectores de RFC 8032 §7.1 (ver ProfileFetcherTest).
         */
        fun verifyEd25519Signature(
            publicKeyBase64: String,
            message: ByteArray,
            signatureBase64: String
        ): Boolean {
            return try {
                val publicKeyBytes = decodeBase64Lenient(publicKeyBase64)
                    ?: return false
                if (publicKeyBytes.size != 32) return false
                val signatureBytes = decodeBase64Lenient(signatureBase64)
                    ?: return false
                if (signatureBytes.size != 64) return false
                val x509 = ED25519_X509_PREFIX + publicKeyBytes
                val publicKey = KeyFactory.getInstance("Ed25519")
                    .generatePublic(X509EncodedKeySpec(x509))
                val verifier = Signature.getInstance("Ed25519")
                verifier.initVerify(publicKey)
                verifier.update(message)
                verifier.verify(signatureBytes)
            } catch (e: Exception) {
                false
            }
        }

        /**
         * C3/510 — Base64 tolerante: acepta estándar (con padding) y
         * URL-safe (con o sin padding), ignorando espacios en blanco.
         * null = no es Base64 válido en ninguno de los dos alfabetos.
         */
        private fun decodeBase64Lenient(value: String): ByteArray? {
            val compact = value.filterNot { it.isWhitespace() }
            if (compact.isEmpty()) return null
            try {
                return Base64.getDecoder().decode(compact)
            } catch (ignored: IllegalArgumentException) {
                // No es Base64 estándar: probar URL-safe.
            }
            return try {
                Base64.getUrlDecoder().decode(compact)
            } catch (ignored: IllegalArgumentException) {
                null
            }
        }
    }

    sealed class Result {
        /**
         * Perfil nuevo en el servidor. [profileBody] es el `.ivanna.json`
         * completo como texto JSON (re-serializado compacto desde el
         * campo `profile` del sobre): es lo que recibe
         * [ProfileApplier.validate]. La versión vive en el sobre, no
         * dentro del `.ivanna.json`.
         *
         * C24/510 — [minSupportedPayloadVersion] es la señal del
         * servidor: la versión mínima de payload (`telemetry.
         * payloadVersion`) que acepta. null = el servidor aún no la
         * anuncia (comportamiento previo: nada que descartar).
         *
         * C16/510 — [llmStatus] / [llmModel] / [llmDrafted] es la
         * salud del LLM del cerebro según el último sondeo (ver
         * cabecera y [KNOWN_LLM_STATUSES]): null = el servidor aún no
         * la anuncia (desconocido, nada que mostrar). `llmDrafted`
         * false = el perfil se compiló solo con el analizador
         * determinista («sin redacción LLM»).
         */
        data class Current(
            val profileId: String,
            val version: Long,
            val status: String,
            val abState: String,
            val profileBody: String,
            val minSupportedPayloadVersion: Long? = null,
            val llmStatus: String? = null,
            val llmModel: String? = null,
            val llmDrafted: Boolean? = null
        ) : Result()

        /**
         * La versión del servidor es la misma que la aplicada: nada que
         * hacer. C24/510 — lleva igualmente la señal
         * [minSupportedPayloadVersion]: el servidor puede subir su
         * mínimo sin publicar un perfil nuevo, y el teléfono debe
         * enterarse en este sondeo (no solo cuando llegue un Current).
         * C16/510 — lleva igualmente la señal LLM por el mismo motivo:
         * la salud del cerebro puede cambiar sin perfil nuevo.
         */
        data class NotModified(
            val minSupportedPayloadVersion: Long? = null,
            val llmStatus: String? = null,
            val llmModel: String? = null,
            val llmDrafted: Boolean? = null
        ) : Result()

        /** Red caída, timeout, 429, 5xx: reintentable en el próximo sondeo. */
        data object Retry : Result()

        /** 401: el token expiró o es inválido; reautorizar una vez. */
        data object TokenInvalid : Result()

        /** No hay token provisionado; no se intenta la red. */
        data object NoToken : Result()

        /**
         * Resto de 4xx (404 incluido: «aún no hay perfil compilado») o
         * sobre malformado: no se reintenta; el teléfono sigue con el
         * último bueno. [reason] dice por qué, sin el cuerpo.
         */
        data class Quarantine(val reason: String) : Result()
    }

    sealed class ConfirmOutcome {
        /** 2xx: el servidor registró la confirmación. */
        data object Confirmed : ConfirmOutcome()

        /** Red caída, timeout, 429, 5xx: la confirmación queda pendiente. */
        data object Retry : ConfirmOutcome()

        /** 401: el token expiró o es inválido. */
        data object TokenInvalid : ConfirmOutcome()

        /** Resto de 4xx: la confirmación no va a pasar; se descarta. */
        data object Quarantine : ConfirmOutcome()

        /** No hay token provisionado; no se intenta la red. */
        data object NoToken : ConfirmOutcome()
    }

    /**
     * Pide el perfil vigente. [appliedVersion] es la versión aplicada
     * actualmente, o null si aún no hay ninguna: si el servidor trae la
     * misma, devuelve [Result.NotModified] sin descargar nada más. Una
     * versión MENOR que la aplicada también se devuelve como
     * [Result.Current] (el controlador decide: solo se aplica con
     * `rolled_back` explícito; el fetcher no inventa política).
     */
    fun fetch(appliedVersion: Long?): Result {
        val token = tokenStore.getToken()
        if (token.isNullOrBlank()) return Result.NoToken
        var lastOutcome: Result = Result.Retry
        for (host in hosts) {
            // C9: host fuera de la allowlist → ni se intenta la conexión
            // (el Bearer no sale de los dos hosts permitidos). Igual que
            // OrganismSender.sendOne: es error de configuración y el
            // resultado es RETRY (se conserva todo para el próximo ciclo).
            if (enforceAllowlist && !MuselinkHosts.isAllowedUrl(host)) {
                logger.w("Host fuera de la allowlist, no se conecta: $host (GET current)")
                continue
            }
            val request = Request.Builder()
                .url(host + CURRENT_PATH)
                .header("Authorization", "Bearer $token")
                .header("Accept", "application/json")
                .get()
                .build()
            try {
                client.newCall(request).execute().use { resp ->
                    val code = resp.code
                    logger.i("GET $host$CURRENT_PATH → HTTP $code")
                    return when {
                        code == 200 -> parseCurrent(resp, appliedVersion)
                        code == 401 -> Result.TokenInvalid
                        code == 429 || code in 500..599 -> Result.Retry
                        code == 404 -> Result.Quarantine("GET current → HTTP 404 (aún no hay perfil compilado)")
                        else -> Result.Quarantine("GET current → HTTP $code")
                    }
                }
            } catch (e: Exception) {
                logger.w("GET $host falló: ${e.javaClass.simpleName} (GET current)")
                lastOutcome = Result.Retry
                // Probar el host alterno antes de rendirse.
            }
        }
        return lastOutcome
    }

    /**
     * Confirma una aplicación: POST del cuerpo que construye
     * [OrganismPayload.appliedConfirmation]. Idempotente por
     * (`profileId`, `version`) en el servidor: reenviar la misma
     * confirmación no duplica.
     */
    fun confirmApplied(body: com.google.gson.JsonObject): ConfirmOutcome {
        val token = tokenStore.getToken()
        if (token.isNullOrBlank()) return ConfirmOutcome.NoToken
        var lastOutcome: ConfirmOutcome = ConfirmOutcome.Retry
        for (host in hosts) {
            if (enforceAllowlist && !MuselinkHosts.isAllowedUrl(host)) {
                logger.w("Host fuera de la allowlist, no se conecta: $host (POST applied)")
                continue
            }
            val request = Request.Builder()
                .url(host + APPLIED_PATH)
                .header("Authorization", "Bearer $token")
                .header("Accept", "application/json")
                .post(body.toString().toRequestBody(JSON))
                .build()
            try {
                client.newCall(request).execute().use { resp ->
                    val code = resp.code
                    logger.i("POST $host$APPLIED_PATH → HTTP $code")
                    return when {
                        code in 200..299 -> ConfirmOutcome.Confirmed
                        code == 401 -> ConfirmOutcome.TokenInvalid
                        code == 429 || code in 500..599 -> ConfirmOutcome.Retry
                        else -> ConfirmOutcome.Quarantine
                    }
                }
            } catch (e: Exception) {
                logger.w("POST $host falló: ${e.javaClass.simpleName} (POST applied)")
                lastOutcome = ConfirmOutcome.Retry
            }
        }
        return lastOutcome
    }

    /**
     * Parsea el sobre del GET. Fail-closed: cualquier campo obligatorio
     * ausente, con tipo incorrecto o fuera de vocabulario es cuarentena
     * (el servidor no va a arreglar el sobre reintentando).
     */
    private fun parseCurrent(resp: Response, appliedVersion: Long?): Result {
        val bytes = try {
            resp.body?.bytes()
        } catch (e: Exception) {
            return Result.Quarantine("GET current: no se pudo leer el cuerpo (${e.javaClass.simpleName})")
        } ?: return Result.Quarantine("GET current: cuerpo vacío")
        if (bytes.size > MAX_RESPONSE_BYTES) {
            return Result.Quarantine("GET current: respuesta de ${bytes.size} bytes supera el tope de $MAX_RESPONSE_BYTES")
        }
        val root = try {
            JsonParser.parseString(bytes.toString(Charsets.UTF_8))
        } catch (e: Exception) {
            return Result.Quarantine("GET current: JSON inválido (${e.message})")
        }
        if (!root.isJsonObject) return Result.Quarantine("GET current: la raíz debe ser un objeto")
        val data = root.asJsonObject
        val profileId = data.get("profileId")?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
            ?.asString?.takeIf { it.isNotBlank() }
            ?: return Result.Quarantine("GET current: falta profileId (string no vacío)")
        val versionEl = data.get("version")
        val version = if (versionEl != null && versionEl.isJsonPrimitive && versionEl.asJsonPrimitive.isNumber) {
            val v = versionEl.asDouble
            if (!v.isFinite()) return Result.Quarantine("GET current: version no finita")
            v.toLong()
        } else {
            return Result.Quarantine("GET current: falta version (número)")
        }
        if (version < 1L) return Result.Quarantine("GET current: version $version debe ser >= 1")
        val status = data.get("status")?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
            ?.asString
            ?: return Result.Quarantine("GET current: falta status (string)")
        if (status !in KNOWN_STATUSES) {
            return Result.Quarantine("GET current: status desconocido '$status'")
        }
        val abState = data.get("abState")?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
            ?.asString
            ?: return Result.Quarantine("GET current: falta abState (string)")
        if (abState !in KNOWN_AB_STATES) {
            return Result.Quarantine("GET current: abState desconocido '$abState'")
        }
        val profileEl = data.get("profile")
        if (profileEl == null || !profileEl.isJsonObject) {
            return Result.Quarantine("GET current: falta profile (objeto .ivanna.json)")
        }
        // C24/510 — señal OPCIONAL del servidor: versión mínima de
        // payload que acepta. Ausente = aún no la anuncia (null). Si
        // viene, debe ser un número >= 1: un valor inválido es sobre
        // malformado (fail-closed, igual que version < 1) y va a
        // cuarentena, no se ignora en silencio.
        val minSupportedPayloadVersion: Long? = when (val minEl = data.get("minSupportedPayloadVersion")) {
            null -> null
            else -> {
                if (!minEl.isJsonPrimitive || !minEl.asJsonPrimitive.isNumber) {
                    return Result.Quarantine("GET current: minSupportedPayloadVersion debe ser un número")
                }
                val minV = minEl.asDouble
                if (!minV.isFinite() || minV < 1.0) {
                    return Result.Quarantine("GET current: minSupportedPayloadVersion $minV debe ser >= 1")
                }
                minV.toLong()
            }
        }
        // C16/510 — señal OPCIONAL del servidor: salud del LLM del
        // cerebro. A diferencia de minSupportedPayloadVersion, un valor
        // inválido NO es sobre malformado: es solo display y se IGNORA
        // (null = desconocido) con aviso en el log, para que un campo
        // de display jamás ponga en cuarentena un perfil válido.
        // `llmStatus` fuera de vocabulario, `llmModel` vacío y
        // `llmDrafted` no booleano se tratan como ausentes.
        val llmStatus: String? = when (val llmEl = data.get("llmStatus")) {
            null -> null
            else -> {
                val raw = if (llmEl.isJsonPrimitive && llmEl.asJsonPrimitive.isString) llmEl.asString else null
                if (raw != null && raw in KNOWN_LLM_STATUSES) raw else {
                    logger.w("GET current: llmStatus '$raw' fuera de vocabulario; se ignora (display)")
                    null
                }
            }
        }
        val llmModel: String? = when (val modelEl = data.get("llmModel")) {
            null -> null
            else -> {
                val raw = if (modelEl.isJsonPrimitive && modelEl.asJsonPrimitive.isString) modelEl.asString else null
                if (!raw.isNullOrBlank()) raw else {
                    logger.w("GET current: llmModel vacío o no string; se ignora (display)")
                    null
                }
            }
        }
        val llmDrafted: Boolean? = when (val draftedEl = data.get("llmDrafted")) {
            null -> null
            else -> {
                if (draftedEl.isJsonPrimitive && draftedEl.asJsonPrimitive.isBoolean) draftedEl.asBoolean else {
                    logger.w("GET current: llmDrafted no booleano; se ignora (display)")
                    null
                }
            }
        }
        // C3/510 — verificación de firma Ed25519, ANTES de devolver
        // Current o NotModified: un sobre sin firma válida no se acepta
        // en ningún caso cuando el enforcement está ON. Lo firmado son
        // los bytes UTF-8 de `profileBody` (la misma cadena que recibe
        // ProfileApplier.validate: lo verificado es lo aplicado).
        // Con enforcement OFF (defecto: el servidor aún no firma) un
        // sobre sin firma se acepta igual y una firma presente solo se
        // verifica en modo monitor (log, sin cuarentena).
        val profileBody = profileEl.toString()
        val signaturePresent = data.has("signature") || data.has("signerKeyId")
        val signatureValid = verifyEnvelopeSignature(data, profileBody)
        if (enforceSignature && !signatureValid) {
            val keyId = data.get("signerKeyId")
                ?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
                ?.asString
            return Result.Quarantine(
                "GET current: firma Ed25519 ausente o inválida " +
                    "(enforcement ON; signerKeyId=${keyId ?: "ausente"})"
            )
        }
        if (!enforceSignature && signaturePresent && signerPublicKeys.isNotEmpty()) {
            logger.i(
                "GET current: firma Ed25519 " +
                    (if (signatureValid) "VÁLIDA" else "INVÁLIDA") +
                    " (monitor: enforcement OFF, el perfil se acepta igual)"
            )
        }
        if (appliedVersion != null && version == appliedVersion) {
            return Result.NotModified(
                minSupportedPayloadVersion,
                llmStatus = llmStatus,
                llmModel = llmModel,
                llmDrafted = llmDrafted
            )
        }
        return Result.Current(
            profileId = profileId,
            version = version,
            status = status,
            abState = abState,
            profileBody = profileBody,
            minSupportedPayloadVersion = minSupportedPayloadVersion,
            llmStatus = llmStatus,
            llmModel = llmModel,
            llmDrafted = llmDrafted
        )
    }

    /**
     * C3/510 — resuelve la clave pública por `signerKeyId` y verifica
     * la `signature` del sobre sobre los bytes UTF-8 de [profileBody].
     * Devuelve false si falta `signature` o `signerKeyId` (o no son
     * strings no vacíos), si el `signerKeyId` no está en
     * [signerPublicKeys] (rotación: clave desconocida) o si la firma
     * no verifica. No lanza nunca.
     */
    private fun verifyEnvelopeSignature(data: JsonObject, profileBody: String): Boolean {
        val signature = data.get("signature")
            ?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
            ?.asString?.takeIf { it.isNotBlank() }
            ?: return false
        val signerKeyId = data.get("signerKeyId")
            ?.takeIf { it.isJsonPrimitive && it.asJsonPrimitive.isString }
            ?.asString?.takeIf { it.isNotBlank() }
            ?: return false
        val publicKeyBase64 = signerPublicKeys[signerKeyId] ?: return false
        return verifyEd25519Signature(
            publicKeyBase64,
            profileBody.toByteArray(Charsets.UTF_8),
            signature
        )
    }
}
