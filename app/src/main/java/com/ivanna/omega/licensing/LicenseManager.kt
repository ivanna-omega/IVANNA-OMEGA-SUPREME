package com.ivanna.omega.licensing

import android.content.Context
import android.provider.Settings
import android.util.Log

/**
 * LICENCIAS — orquestador (v2.5.446).
 *
 * `refresh()` se llama al arrancar (MainActivity, en Dispatchers.IO,
 * sin bloquear la UI ni el audio). Flujo:
 *
 *   sin clave guardada        → UNVALIDATED (modo libre, como hoy)
 *   servidor: valid:true      → VALID + guarda caché de gracia
 *   servidor: valid:false     → INVALID (motivo) + borra la caché
 *   sin red / respuesta rota  → FAIL-OPEN: caché vigente → VALID_OFFLINE;
 *                               sin caché o caché caducada → UNVALIDATED
 *                               (o INVALID/EXPIRED si la caché caducó)
 *
 * El audio NUNCA se bloquea por licencia: el modo libre es la app
 * funcionando como hoy. El estado solo marca el acceso; qué se limita
 * exactamente en modo libre es decisión de producto de Esse PENDIENTE
 * (ver docs/09-operations/licensing.md). Hoy: nada se bloquea.
 *
 * `deviceId` = SHA-256 del ANDROID_ID (el servidor lo vuelve a
 * hashear). La clave NUNCA sale en logs ni en telemetría: solo su
 * huella de 8 hex ([LicenseKeyUtils.fingerprint]).
 */
class LicenseManager(
    private val context: Context,
    private val store: LicenseStore = SharedPreferencesLicenseStore(context),
    private val clientFactory: (String) -> LicenseClient =
        { baseUrl -> LicenseClient(UrlConnectionLicenseHttp(), baseUrl) },
    private val nowMillis: () -> Long = { System.currentTimeMillis() },
    private val deviceIdProvider: () -> String = { defaultDeviceId(context) },
) {
    companion object {
        private const val TAG = "IVANNA.License"

        /** SHA-256 del ANDROID_ID; fallback estable si no hay ANDROID_ID. */
        fun defaultDeviceId(context: Context): String {
            val androidId = Settings.Secure.getString(
                context.contentResolver,
                Settings.Secure.ANDROID_ID,
            )
            return LicenseKeyUtils.hashDeviceId(androidId?.takeIf { it.isNotBlank() } ?: "unknown-device")
        }
    }

    fun baseUrl(): String =
        store.getBaseUrlOverride()?.trimEnd('/') ?: LicenseConfig.DEFAULT_BASE_URL

    /** Guarda la clave que escribe el usuario. false = forma inválida. */
    fun saveLicenseKey(rawInput: String): Boolean {
        val canonical = LicenseKeyUtils.normalizeLicenseKey(rawInput) ?: return false
        store.setLicenseKey(canonical)
        store.setLastInvalidReason(null)
        Log.i(TAG, "Clave guardada (huella=${LicenseKeyUtils.fingerprint(canonical)})")
        return true
    }

    fun clearLicenseKey() {
        store.setLicenseKey(null)
        store.setCachedValidation(null)
        store.setLastInvalidReason(null)
    }

    /** Estado actual sin tocar la red (para la UI antes del refresh). */
    fun currentState(): LicenseState {
        val key = store.getLicenseKey() ?: return LicenseState.UNVALIDATED
        if (LicenseKeyUtils.normalizeLicenseKey(key) == null) {
            return LicenseState.UNVALIDATED.copy(
                invalidReason = LicenseInvalidReason.BAD_REQUEST,
            )
        }
        val cached = store.getCachedValidation()
        if (cached != null && cached.isStillUsable(nowMillis())) {
            return LicenseState(
                access = LicenseAccess.VALID_OFFLINE,
                plan = cached.plan,
                expiresAtMillis = cached.expiresAtMillis,
                devicesBound = cached.devicesBound,
                devicesMax = cached.devicesMax,
                lastValidatedAtMillis = cached.validatedAtMillis,
                invalidReason = null,
                offline = true,
            )
        }
        val reason = store.getLastInvalidReason()
        if (reason != null) {
            return LicenseState.UNVALIDATED.copy(
                access = LicenseAccess.INVALID,
                invalidReason = reason,
            )
        }
        return LicenseState.UNVALIDATED
    }

    /**
     * Valida contra el servidor. Síncrono: llamar desde Dispatchers.IO.
     * Nunca lanza: cualquier fallo inesperado es NetworkError → fail-open.
     */
    fun refresh(): LicenseState {
        val key = store.getLicenseKey()
            ?.let { LicenseKeyUtils.normalizeLicenseKey(it) }
            ?: return LicenseState.UNVALIDATED
        val outcome = runCatching {
            clientFactory(baseUrl()).validate(key, deviceIdProvider())
        }.getOrElse {
            Log.w(TAG, "Validación falló sin respuesta del servidor", it)
            LicenseValidationOutcome.NetworkError
        }
        val now = nowMillis()
        return when (outcome) {
            is LicenseValidationOutcome.Valid -> {
                val cached = CachedValidation(
                    plan = outcome.plan,
                    expiresAtMillis = outcome.expiresAtMillis,
                    devicesBound = outcome.devicesBound,
                    devicesMax = outcome.devicesMax,
                    validatedAtMillis = now,
                )
                store.setCachedValidation(cached)
                store.setLastInvalidReason(null)
                Log.i(TAG, "Licencia válida: plan=${outcome.plan} " +
                    "devices=${outcome.devicesBound}/${outcome.devicesMax}")
                LicenseState(
                    access = LicenseAccess.VALID,
                    plan = outcome.plan,
                    expiresAtMillis = outcome.expiresAtMillis,
                    devicesBound = outcome.devicesBound,
                    devicesMax = outcome.devicesMax,
                    lastValidatedAtMillis = now,
                    invalidReason = null,
                    offline = false,
                )
            }
            is LicenseValidationOutcome.Invalid -> {
                // Un "no" explícito del servidor NO es fail-open: la caché
                // no puede resucitar una licencia revocada o expirada.
                store.setCachedValidation(null)
                store.setLastInvalidReason(outcome.reason)
                Log.i(TAG, "Licencia inválida: reason=${outcome.reason}")
                LicenseState.UNVALIDATED.copy(
                    access = LicenseAccess.INVALID,
                    invalidReason = outcome.reason,
                )
            }
            LicenseValidationOutcome.NetworkError,
            LicenseValidationOutcome.MalformedResponse -> {
                val cached = store.getCachedValidation()
                if (cached != null && cached.isStillUsable(now)) {
                    Log.i(TAG, "Sin servidor; gracia sin conexión con caché vigente " +
                        "(plan=${cached.plan})")
                    LicenseState(
                        access = LicenseAccess.VALID_OFFLINE,
                        plan = cached.plan,
                        expiresAtMillis = cached.expiresAtMillis,
                        devicesBound = cached.devicesBound,
                        devicesMax = cached.devicesMax,
                        lastValidatedAtMillis = cached.validatedAtMillis,
                        invalidReason = null,
                        offline = true,
                    )
                } else if (cached != null) {
                    Log.i(TAG, "Sin servidor y la caché mensual caducó")
                    LicenseState.UNVALIDATED.copy(
                        access = LicenseAccess.INVALID,
                        invalidReason = LicenseInvalidReason.EXPIRED,
                        offline = true,
                    )
                } else {
                    Log.i(TAG, "Sin servidor y sin caché: modo libre")
                    LicenseState.UNVALIDATED.copy(offline = true)
                }
            }
        }
    }

    /**
     * Resumen para telemetría/UI: acceso, plan y contadores, SIN la
     * clave (solo su huella de 8 hex). Seguro para logs y paneles.
     */
    fun telemetrySummary(state: LicenseState = currentState()): Map<String, String> {
        val key = store.getLicenseKey()
        return mapOf(
            "access" to state.access.name,
            "plan" to (state.plan?.wireValue ?: "none"),
            "devices" to "${state.devicesBound ?: 0}/${state.devicesMax ?: LicenseConfig.DEFAULT_DEVICES_MAX}",
            "keyFingerprint" to (key?.let { LicenseKeyUtils.fingerprint(it) } ?: "none"),
            "lastValidatedAtMillis" to (state.lastValidatedAtMillis?.toString() ?: "never"),
            "offline" to state.offline.toString(),
            "invalidReason" to (state.invalidReason?.wireValue ?: "none"),
        )
    }
}
