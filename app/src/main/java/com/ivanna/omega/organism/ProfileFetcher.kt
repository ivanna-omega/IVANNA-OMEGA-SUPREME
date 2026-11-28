package com.ivanna.omega.organism

import com.google.gson.JsonParser
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import okhttp3.Response

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
 * PUNTO DE ENGANCHE (C3, Fase 2 — verificación de firma Ed25519): en
 * [fetch], tras parsear el sobre y ANTES de devolver [Result.Current],
 * irá la verificación de la firma del sobre contra la clave pública del
 * servidor. Hoy el sobre no trae firma y la verificación NO existe: un
 * servidor comprometido o un token robado podrían servir un perfil
 * válido pero no firmado. NO se implementa en T1 por diseño (C3 es
 * Fase 2 posterior); el perfil que se devuelve aquí pasa de todos modos
 * por la validación fail-closed de [ProfileApplier] antes de tocar el
 * motor.
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
    private val logger: OrganismLogger = AndroidOrganismLogger(TAG)
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
         */
        data class Current(
            val profileId: String,
            val version: Long,
            val status: String,
            val abState: String,
            val profileBody: String,
            val minSupportedPayloadVersion: Long? = null
        ) : Result()

        /**
         * La versión del servidor es la misma que la aplicada: nada que
         * hacer. C24/510 — lleva igualmente la señal
         * [minSupportedPayloadVersion]: el servidor puede subir su
         * mínimo sin publicar un perfil nuevo, y el teléfono debe
         * enterarse en este sondeo (no solo cuando llegue un Current).
         */
        data class NotModified(
            val minSupportedPayloadVersion: Long? = null
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
        // PUNTO DE ENGANCHE (C3, Fase 2 — verificación de firma Ed25519):
        // aquí se verificará la firma del sobre (campo `signature`,
        // clave pública del servidor) ANTES de devolver Current. Hoy el
        // sobre no trae firma y la verificación no existe (ver cabecera).
        if (appliedVersion != null && version == appliedVersion) {
            return Result.NotModified(minSupportedPayloadVersion)
        }
        return Result.Current(
            profileId = profileId,
            version = version,
            status = status,
            abState = abState,
            profileBody = profileEl.toString(),
            minSupportedPayloadVersion = minSupportedPayloadVersion
        )
    }
}
