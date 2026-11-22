package com.ivanna.omega.licensing

import android.content.Context
import android.content.SharedPreferences
import org.json.JSONObject

/**
 * LICENCIAS — persistencia (v2.5.446).
 *
 * La interfaz existe para que los tests inyecten un store en memoria
 * sin Android. La implementación real es SharedPreferences privado
 * de la app. Solo se guarda:
 * - la clave en claro (la guarda el usuario; es SU clave, no un
 *   secreto de la app; el servidor solo guarda su hash SHA-256),
 * - la última validación válida (plan, caducidad, dispositivos),
 * - la URL base configurada y el último motivo de invalidez.
 *
 * NUNCA se registra la clave en logs ni se envía a telemetría.
 */
interface LicenseStore {
    fun getLicenseKey(): String?
    fun setLicenseKey(canonicalKey: String?)
    fun getBaseUrlOverride(): String?
    fun setBaseUrlOverride(url: String?)
    fun getCachedValidation(): CachedValidation?
    fun setCachedValidation(cached: CachedValidation?)
    fun getLastInvalidReason(): LicenseInvalidReason?
    fun setLastInvalidReason(reason: LicenseInvalidReason?)
}

class InMemoryLicenseStore : LicenseStore {
    private var licenseKey: String? = null
    private var baseUrlOverride: String? = null
    private var cached: CachedValidation? = null
    private var lastInvalidReason: LicenseInvalidReason? = null

    override fun getLicenseKey(): String? = licenseKey
    override fun setLicenseKey(canonicalKey: String?) { licenseKey = canonicalKey }
    override fun getBaseUrlOverride(): String? = baseUrlOverride
    override fun setBaseUrlOverride(url: String?) { baseUrlOverride = url }
    override fun getCachedValidation(): CachedValidation? = cached
    override fun setCachedValidation(cached: CachedValidation?) { this.cached = cached }
    override fun getLastInvalidReason(): LicenseInvalidReason? = lastInvalidReason
    override fun setLastInvalidReason(reason: LicenseInvalidReason?) { lastInvalidReason = reason }
}

class SharedPreferencesLicenseStore(context: Context) : LicenseStore {
    private val prefs: SharedPreferences =
        context.getSharedPreferences(LicenseConfig.PREFS_NAME, Context.MODE_PRIVATE)

    override fun getLicenseKey(): String? =
        prefs.getString(LicenseConfig.KEY_LICENSE_KEY, null)

    override fun setLicenseKey(canonicalKey: String?) {
        prefs.edit().putString(LicenseConfig.KEY_LICENSE_KEY, canonicalKey).apply()
    }

    override fun getBaseUrlOverride(): String? =
        prefs.getString(LicenseConfig.KEY_BASE_URL_OVERRIDE, null)?.takeIf { it.isNotBlank() }

    override fun setBaseUrlOverride(url: String?) {
        prefs.edit().putString(LicenseConfig.KEY_BASE_URL_OVERRIDE, url).apply()
    }

    override fun getCachedValidation(): CachedValidation? {
        val raw = prefs.getString(LicenseConfig.KEY_CACHED_VALIDATION, null) ?: return null
        return runCatching {
            val json = JSONObject(raw)
            val plan = LicensePlan.fromWire(json.optString("plan", null)) ?: return null
            val validatedAt = json.getLong("validatedAtMillis")
            val expiresAt = if (json.isNull("expiresAtMillis")) null
                else json.getLong("expiresAtMillis")
            CachedValidation(
                plan = plan,
                expiresAtMillis = expiresAt,
                devicesBound = json.optInt("devicesBound", 0),
                devicesMax = json.optInt("devicesMax", LicenseConfig.DEFAULT_DEVICES_MAX),
                validatedAtMillis = validatedAt,
            )
        }.getOrNull()
    }

    override fun setCachedValidation(cached: CachedValidation?) {
        if (cached == null) {
            prefs.edit().remove(LicenseConfig.KEY_CACHED_VALIDATION).apply()
            return
        }
        val json = JSONObject().apply {
            put("plan", cached.plan.wireValue)
            if (cached.expiresAtMillis == null) put("expiresAtMillis", JSONObject.NULL)
            else put("expiresAtMillis", cached.expiresAtMillis)
            put("devicesBound", cached.devicesBound)
            put("devicesMax", cached.devicesMax)
            put("validatedAtMillis", cached.validatedAtMillis)
        }
        prefs.edit().putString(LicenseConfig.KEY_CACHED_VALIDATION, json.toString()).apply()
    }

    override fun getLastInvalidReason(): LicenseInvalidReason? {
        val raw = prefs.getString(LicenseConfig.KEY_LAST_INVALID_REASON, null) ?: return null
        return LicenseInvalidReason.fromWire(raw).takeIf { it != LicenseInvalidReason.UNKNOWN }
    }

    override fun setLastInvalidReason(reason: LicenseInvalidReason?) {
        prefs.edit().putString(LicenseConfig.KEY_LAST_INVALID_REASON, reason?.wireValue).apply()
    }
}
