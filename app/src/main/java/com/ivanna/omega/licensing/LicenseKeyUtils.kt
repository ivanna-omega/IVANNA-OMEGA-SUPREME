package com.ivanna.omega.licensing

import java.security.MessageDigest

/**
 * LICENCIAS — utilidades puras (v2.5.446).
 *
 * Sin dependencias de Android: testeable en la JVM sin Robolectric.
 * Las reglas copian las del servidor (`licenses.mjs`) para que la app
 * rechace en local lo que el servidor rechazaría con `bad_request`,
 * sin gastar una llamada de red.
 */
object LicenseKeyUtils {

    /** Alfabeto Crockford sin caracteres confusos (sin 0/O, 1/I/L). */
    private val KEY_BODY_REGEX =
        Regex("^IVANNA[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{12}$")

    /**
     * Normaliza lo que escribe el usuario: mayúsculas, con o sin
     * guiones/espacios. Devuelve la forma canónica
     * `IVANNA-XXXX-XXXX-XXXX` o null si no tiene la forma válida.
     * Idéntica a `normalizeLicenseKey` del servidor.
     */
    fun normalizeLicenseKey(input: String?): String? {
        if (input.isNullOrBlank()) return null
        val clean = input.uppercase().filter { it.isLetterOrDigit() }
        if (!KEY_BODY_REGEX.matches(clean)) return null
        val body = clean.removePrefix("IVANNA")
        return "IVANNA-${body.substring(0, 4)}-${body.substring(4, 8)}-${body.substring(8, 12)}"
    }

    /** SHA-256 en hex del ANDROID_ID (lo que se envía como `deviceId`). */
    fun hashDeviceId(androidId: String): String = sha256Hex(androidId)

    /**
     * Huella corta de la clave para logs y telemetría (igual que el
     * `fingerprint` del servidor: 8 primeros hex del SHA-256).
     * Nunca la clave completa, nunca el email en claro.
     */
    fun fingerprint(licenseKey: String): String =
        sha256Hex(licenseKey).substring(0, 8)

    fun sha256Hex(value: String): String {
        val digest = MessageDigest.getInstance("SHA-256")
        val bytes = digest.digest(value.toByteArray(Charsets.UTF_8))
        return bytes.joinToString("") { "%02x".format(it) }
    }

    /**
     * Parsea `expiresAt` ISO-8601 del servidor (`2026-12-14T…Z`) a
     * epoch ms. null → sin caducidad (lifetime). Inválido → null:
     * un `expiresAt` corrupto no debe tumbar una validación válida.
     */
    fun parseExpiresAt(iso: String?): Long? {
        if (iso.isNullOrBlank()) return null
        return runCatching {
            java.time.Instant.parse(iso).toEpochMilli()
        }.getOrNull()
    }
}
