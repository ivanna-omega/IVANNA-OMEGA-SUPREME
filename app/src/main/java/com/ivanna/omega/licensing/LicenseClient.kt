package com.ivanna.omega.licensing

import org.json.JSONObject
import java.io.BufferedReader
import java.io.IOException
import java.io.InputStreamReader
import java.io.OutputStreamWriter
import java.net.HttpURLConnection
import java.net.URL
import java.nio.charset.StandardCharsets

/**
 * LICENCIAS — cliente de validación (v2.5.446).
 *
 * POST `{baseUrl}/v1/licenses/validate` con `{licenseKey, deviceId}`.
 * La URL base por defecto es el servidor de Render (aún SIN desplegar:
 * ver [LicenseConfig]); es configurable por preferencia para apuntar
 * a un mock/local en pruebas.
 *
 * El transporte es inyectable ([LicenseHttp]) para que los tests usen
 * un mock sin tocar la red real (mismo patrón que `TidalHttp`).
 * Sin OkHttp/Retrofit: HttpURLConnection estándar, como el resto de
 * la app.
 *
 * La clave NUNCA se registra en logs: solo su huella de 8 hex.
 */
data class LicenseHttpResponse(val status: Int, val body: String)

interface LicenseHttp {
    @Throws(IOException::class)
    fun post(url: String, body: String): LicenseHttpResponse
}

/** Transporte real por HttpURLConnection (10 s connect / 10 s read). */
class UrlConnectionLicenseHttp(
    private val connectTimeoutMillis: Int = 10_000,
    private val readTimeoutMillis: Int = 10_000,
) : LicenseHttp {
    override fun post(url: String, body: String): LicenseHttpResponse {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            requestMethod = "POST"
            connectTimeout = connectTimeoutMillis
            readTimeout = readTimeoutMillis
            doOutput = true
            setRequestProperty("Content-Type", "application/json")
            setRequestProperty("Accept", "application/json")
        }
        try {
            OutputStreamWriter(conn.outputStream, StandardCharsets.UTF_8).use {
                it.write(body)
                it.flush()
            }
            val status = conn.responseCode
            val stream = if (status in 200..299) conn.inputStream else conn.errorStream
            val text = stream?.let {
                BufferedReader(InputStreamReader(it, StandardCharsets.UTF_8)).use { r -> r.readText() }
            }.orEmpty()
            return LicenseHttpResponse(status, text)
        } finally {
            conn.disconnect()
        }
    }
}

class LicenseClient(
    private val http: LicenseHttp,
    private val baseUrl: String = LicenseConfig.DEFAULT_BASE_URL,
) {
    companion object {
        const val VALIDATE_PATH = "/v1/licenses/validate"
    }

    /**
     * Valida la clave contra el servidor.
     * - `licenseKey` debe venir ya normalizada (ver [LicenseKeyUtils]).
     * - `deviceId` es el ANDROID_ID ya hasheado por la app.
     * - Un 400 con `bad_request`, o un 200 con `valid:false`, es una
     *   respuesta del servidor (Invalid), NO un error de red.
     * - IOException / timeout → NetworkError (fail-open con caché).
     * - Cuerpo no-JSON, `valid` ausente, o `plan` desconocido en un
     *   `valid:true` → MalformedResponse (fail-open con caché).
     */
    fun validate(licenseKey: String, deviceId: String): LicenseValidationOutcome {
        val url = baseUrl.trimEnd('/') + VALIDATE_PATH
        val requestBody = JSONObject().apply {
            put("licenseKey", licenseKey)
            put("deviceId", deviceId)
        }.toString()
        val response = try {
            http.post(url, requestBody)
        } catch (e: IOException) {
            return LicenseValidationOutcome.NetworkError
        } catch (e: RuntimeException) {
            return LicenseValidationOutcome.NetworkError
        }
        return parseResponse(response)
    }

    /**
     * Parsea la respuesta del servidor. Fail-open: CUALQUIER cuerpo que no
     * se pueda interpretar (no-JSON, campos ausentes, tipos mal) es
     * MalformedResponse, y cualquier excepción inesperada al parsear
     * (p. ej. NPE de org.json con claves ausentes) también lo es.
     * Nunca lanza: un parseo roto no debe tumbar la validación.
     */
    internal fun parseResponse(response: LicenseHttpResponse): LicenseValidationOutcome {
        return runCatching { parseResponseStrict(response) }
            .getOrElse { LicenseValidationOutcome.MalformedResponse }
    }

    private fun parseResponseStrict(response: LicenseHttpResponse): LicenseValidationOutcome {
        val json = runCatching { JSONObject(response.body) }.getOrNull()
            ?: return LicenseValidationOutcome.MalformedResponse
        if (!json.has("valid")) return LicenseValidationOutcome.MalformedResponse
        val valid = runCatching { json.getBoolean("valid") }.getOrNull()
            ?: return LicenseValidationOutcome.MalformedResponse
        if (!valid) {
            val reason = LicenseInvalidReason.fromWire(optString(json, "reason"))
            return LicenseValidationOutcome.Invalid(reason)
        }
        val plan = LicensePlan.fromWire(optString(json, "plan"))
            ?: return LicenseValidationOutcome.MalformedResponse
        val devicesBound = if (json.has("devicesBound")) {
            runCatching { json.getInt("devicesBound") }.getOrNull()
                ?: return LicenseValidationOutcome.MalformedResponse
        } else 0
        val devicesMax = if (json.has("devicesMax")) {
            runCatching { json.getInt("devicesMax") }.getOrNull()
                ?: return LicenseValidationOutcome.MalformedResponse
        } else LicenseConfig.DEFAULT_DEVICES_MAX
        val expiresAt = LicenseKeyUtils.parseExpiresAt(optString(json, "expiresAt"))
        return LicenseValidationOutcome.Valid(plan, expiresAt, devicesBound, devicesMax)
    }

    /**
     * Lectura de un campo string que nunca lanza ni devuelve basura:
     * ausente, null JSON o no-string → null. (org.json `optString(key,
     * null)` no es fiable con claves ausentes según la versión.)
     */
    private fun optString(json: JSONObject, key: String): String? =
        (json.opt(key) as? String)?.takeIf { it.isNotEmpty() }
}
