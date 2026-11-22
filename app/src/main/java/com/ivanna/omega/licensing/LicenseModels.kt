package com.ivanna.omega.licensing

/**
 * LICENCIAS — modelos (v2.5.446).
 *
 * Contrato del servidor (fuente de verdad:
 * `monetization/license-server/README.md` + `licenses.mjs`):
 *
 *   POST /v1/licenses/validate {licenseKey, deviceId}
 *     → 200 {valid:true, plan, expiresAt, devicesBound, devicesMax}
 *     → 200 {valid:false, reason}   (not_found | revoked | expired |
 *                                     device_limit)
 *     → 400 {valid:false, reason:"bad_request"}
 *
 * `plan`: `lifetime` (expiresAt null = sin caducidad) o `monthly`
 * (30 días). Máx. 3 dispositivos por defecto. `deviceId` es el
 * ANDROID_ID ya hasheado por la app (el servidor lo vuelve a hashear
 * antes de guardarlo: nunca viaja ni se guarda en claro).
 *
 * Regla de oro: NUNCA bloquear el audio por un fallo de licencia o de
 * red. Sin red, la app sigue con la última validación válida en caché
 * (fail-open). Sin licencia válida, la app sigue funcionando como hoy
 * (modo libre, ver LicenseManager / docs/09-operations/licensing.md).
 */

/** Planes que el servidor reconoce. */
enum class LicensePlan(val wireValue: String) {
    LIFETIME("lifetime"),
    MONTHLY("monthly");

    companion object {
        fun fromWire(value: String?): LicensePlan? =
            entries.firstOrNull { it.wireValue == value }
    }
}

/** Motivos de `valid:false` que devuelve el servidor. */
enum class LicenseInvalidReason(val wireValue: String) {
    NOT_FOUND("not_found"),
    REVOKED("revoked"),
    EXPIRED("expired"),
    DEVICE_LIMIT("device_limit"),
    BAD_REQUEST("bad_request"),
    UNKNOWN("unknown");

    companion object {
        fun fromWire(value: String?): LicenseInvalidReason =
            entries.firstOrNull { it.wireValue == value } ?: UNKNOWN
    }
}

/**
 * Resultado de una llamada a `/v1/licenses/validate`.
 * `NetworkError` y `MalformedResponse` NO son un "no" del servidor:
 * son la señal para el fail-open con caché (ver LicenseManager).
 */
sealed class LicenseValidationOutcome {
    data class Valid(
        val plan: LicensePlan,
        /** ISO-8601 del servidor, o null en lifetime (sin caducidad). */
        val expiresAtMillis: Long?,
        val devicesBound: Int,
        val devicesMax: Int,
    ) : LicenseValidationOutcome()

    data class Invalid(val reason: LicenseInvalidReason) : LicenseValidationOutcome()

    /** Sin red / timeout / el servidor no responde. Fail-open con caché. */
    data object NetworkError : LicenseValidationOutcome()

    /** HTTP 200 con cuerpo que no se puede interpretar. Fail-open con caché. */
    data object MalformedResponse : LicenseValidationOutcome()
}

/** Estado de acceso que ve la UI. */
enum class LicenseAccess {
    /** Sin clave guardada o sin ninguna validación: la app funciona como hoy. */
    UNVALIDATED,

    /** Licencia válida confirmada por el servidor en este arranque. */
    VALID,

    /** Sin red, pero la última validación válida en caché sigue vigente. */
    VALID_OFFLINE,

    /** El servidor dijo que la clave no es válida (motivo en [LicenseState]). */
    INVALID,
}

/**
 * Estado completo de licencia, legible por la UI y por la telemetría.
 * NUNCA contiene la clave en claro (ver [LicenseManager.telemetrySummary]).
 */
data class LicenseState(
    val access: LicenseAccess,
    val plan: LicensePlan?,
    /** Epoch ms, o null en lifetime / sin validación. */
    val expiresAtMillis: Long?,
    val devicesBound: Int?,
    val devicesMax: Int?,
    /** Epoch ms de la última validación válida (servidor o caché). */
    val lastValidatedAtMillis: Long?,
    val invalidReason: LicenseInvalidReason?,
    /** true cuando el estado viene de caché sin contacto con el servidor. */
    val offline: Boolean,
) {
    /** Acceso completo: licencia válida, en vivo o por caché vigente. */
    val hasFullAccess: Boolean get() = access == LicenseAccess.VALID ||
        access == LicenseAccess.VALID_OFFLINE

    companion object {
        val UNVALIDATED = LicenseState(
            access = LicenseAccess.UNVALIDATED,
            plan = null,
            expiresAtMillis = null,
            devicesBound = null,
            devicesMax = null,
            lastValidatedAtMillis = null,
            invalidReason = null,
            offline = false,
        )
    }
}

/** Última validación válida guardada (caché de gracia sin conexión). */
data class CachedValidation(
    val plan: LicensePlan,
    val expiresAtMillis: Long?,
    val devicesBound: Int,
    val devicesMax: Int,
    val validatedAtMillis: Long,
) {
    /** Una mensual en caché solo vale mientras no haya caducado. */
    fun isStillUsable(nowMillis: Long): Boolean =
        expiresAtMillis == null || expiresAtMillis > nowMillis
}
