package com.ivanna.omega.licensing

/**
 * LICENCIAS — configuración (v2.5.446).
 *
 * URL por defecto: el servidor de Render. **Aún NO desplegado**
 * (2026-11-14): hasta que lo esté, validar contra esta URL da
 * NetworkError y la app cae en fail-open con caché / modo libre.
 * La URL se puede cambiar por preferencia (sin recompilar) para
 * apuntar a un mock o al servidor local (`http://localhost:8788`
 * en el emulador: `http://10.0.2.2:8788`).
 */
object LicenseConfig {
    /** URL del servidor de licencias (Render, plan free). */
    const val DEFAULT_BASE_URL = "https://ivanna-license-server.onrender.com"

    /** Máx. de dispositivos por licencia (contrato del servidor). */
    const val DEFAULT_DEVICES_MAX = 3

    /** Nombre del SharedPreferences de licencias. */
    const val PREFS_NAME = "ivanna_licensing"

    /** Clave guardada por el usuario (forma canónica IVANNA-XXXX-…). */
    const val KEY_LICENSE_KEY = "license_key"

    /** URL base configurada por el usuario (vacía = la de por defecto). */
    const val KEY_BASE_URL_OVERRIDE = "base_url_override"

    /** Última validación válida en caché (JSON). */
    const val KEY_CACHED_VALIDATION = "cached_validation"

    /** Epoch ms de la última validación válida. */
    const val KEY_LAST_VALIDATED_AT = "last_validated_at"

    /** Último motivo de invalidez devuelto por el servidor. */
    const val KEY_LAST_INVALID_REASON = "last_invalid_reason"
}
