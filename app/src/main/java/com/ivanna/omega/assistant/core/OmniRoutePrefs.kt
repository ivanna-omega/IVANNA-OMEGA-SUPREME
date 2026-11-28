package com.ivanna.omega.assistant.core

import android.content.Context

/**
 * OmniRoutePrefs — ajustes del proveedor LLM OmniRoute (NO secretos).
 *
 * OmniRoute es el gateway de IA gratuito que corre en el propio servidor
 * del usuario (https://github.com/ChrisCompton/omniroute, MIT, endpoint
 * OpenAI-compatible). La app lo usa como cerebro LLM alternativo a
 * Gemini para el chat.
 *
 * - Gemini sigue siendo el DEFAULT; OmniRoute es opt-in (flag
 *   `omniroute_enabled`, apagado por defecto).
 * - La API key de OmniRoute NO vive aquí: va en
 *   SecureConfigurationManager (EncryptedSharedPreferences), igual que la
 *   key Gemini.
 * - SIN URL POR DEFECTO (v2.5.471, Repair 2): la dirección del gateway
 *   del dueño NO va en el APK — ni como default, ni como placeholder,
 *   ni en comentarios. DEFAULT_BASE_URL es "" y la app pide la URL en
 *   primera apertura (diálogo "Configura tu gateway"). El placeholder
 *   de la UI muestra SOLO el formato (BASE_URL_PLACEHOLDER).
 * - Modelo por defecto: el combo `ivanna-free` (fallback automático entre
 *   los proveedores gratuitos conectados en el gateway). Si el combo aún
 *   no existe en el gateway, el usuario puede poner un modelo directo
 *   (p. ej. `pol/claude`, sin key ni registro).
 * - Migración v2.5.471: si lo guardado es una URL de la red personal
 *   (host del rango CGNAT), migrateLegacyBaseUrl() la borra y el
 *   diálogo de primera apertura vuelve a pedir la URL.
 *
 * Fail-closed: si OmniRoute falla o no responde, el agente devuelve
 * Offline con la última respuesta en caché (ver IvannaGeminiAgent).
 * Sin URL configurada, OmniRoute ni se intenta (el orquestador falla
 * con "sin configurar" y el chat sigue con Gemini). El audio NUNCA
 * depende del gateway.
 */
object OmniRoutePrefs {
    private const val PREFS_NAME = "ivanna_llm_provider"
    private const val KEY_GATEWAY_SETUP_SHOWN = "gateway_setup_shown"

    /**
     * Base URL por defecto: VACÍA. No hay ninguna dirección hardcodeada
     * en el APK; cada usuario escribe la URL de su propio gateway.
     */
    const val DEFAULT_BASE_URL = ""

    /**
     * Placeholder SOLO de formato para los campos de URL de la UI.
     * Nunca una dirección real.
     */
    const val BASE_URL_PLACEHOLDER = "http://TU-GATEWAY:20128/v1"

    /** Modelo/combo por defecto del gateway. */
    const val DEFAULT_MODEL = "ivanna-free"

    /**
     * Descubrimiento de la URL viva del gateway (v2.5.509, auto-enlace).
     *
     * La dirección pública del túnel rota cada pocos minutos y NO puede
     * ir en el APK (v2.5.471: la dirección del gateway del dueño no va
     * hardcodeada, ni como default, ni en comentarios) ni pedirse al
     * usuario en cada rotación — ese era el fallo que obligaba a copiar
     * URL y key a mano y a reintentar sin éxito. El vigilante del túnel
     * publica la URL vigente en `omniroute.json` (rama `omniroute-url`
     * del repo, archivo público, sin PRs porque no es main); la app la
     * lee al arrancar y la guarda como URL vigente. Si el descubrimiento
     * falla (sin red, archivo caído), se usa la URL guardada. Aquí solo
     * vive el puntero público; la dirección del túnel NUNCA va en el APK.
     */
    const val DISCOVERY_URL =
        "https://raw.githubusercontent.com/ivanna-omega/IVANNA-OMEGA-SUPREME/omniroute-url/omniroute.json"

    /**
     * Normaliza una URL de gateway: quita espacios y barra final y exige
     * el sufijo `/v1` (lo añade si falta). Acepta https siempre, y http
     * solo para localhost (pruebas locales del gateway). Null si no es
     * una URL de gateway válida. Función pura (sin Context) para poder
     * testearla sin Android.
     */
    fun normalizeGatewayUrl(url: String): String? {
        val clean = url.trim().trimEnd('/')
        if (clean.isEmpty()) return null
        val lower = clean.lowercase()
        val afterScheme = when {
            lower.startsWith("https://") -> clean.substring("https://".length)
            lower.startsWith("http://localhost") ||
                lower.startsWith("http://127.0.0.1") -> clean.substring("http://".length)
            else -> return null
        }
        val host = afterScheme.substringBefore('/').substringBefore(':')
        if (host.isBlank()) return null
        if (host != "localhost" && !host.contains('.')) return null
        return if (clean.endsWith("/v1")) clean else "$clean/v1"
    }

    /**
     * Parsea el archivo de descubrimiento `{"omniroute_url": "https://.../v1"}`.
     * Null si el JSON no trae una URL de gateway válida. Función pura.
     */
    fun parseDiscoveredBaseUrl(json: String): String? = runCatching {
        val raw = org.json.JSONObject(json).optString("omniroute_url").orEmpty()
        normalizeGatewayUrl(raw)
    }.getOrNull()

    /**
     * true si la URL apunta al túnel público (localhost.run). Solo esas
     * URLs las puede sustituir el descubrimiento: una URL propia del
     * usuario (su propio gateway) se respeta siempre. Función pura.
     */
    fun isTunnelUrl(url: String): Boolean {
        val host = url.trim().lowercase()
            .removePrefix("http://")
            .removePrefix("https://")
            .substringBefore('/')
            .substringBefore(':')
        return host.endsWith(".app.lhr.life")
    }

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /** true = el chat usa OmniRoute; false (default) = Gemini. */
    fun isOmniRouteEnabled(context: Context): Boolean =
        prefs(context).getBoolean("omniroute_enabled", false)

    fun setOmniRouteEnabled(context: Context, enabled: Boolean) {
        prefs(context).edit().putBoolean("omniroute_enabled", enabled).apply()
    }

    /** Base URL del gateway (incluye /v1). Sin barra final. "" = sin configurar. */
    fun getBaseUrl(context: Context): String =
        prefs(context).getString("omniroute_base_url", null)
            ?.trim()?.trimEnd('/') ?: ""

    fun setBaseUrl(context: Context, url: String) {
        val clean = url.trim().trimEnd('/')
        val edit = prefs(context).edit()
        if (clean.isBlank()) {
            // Vacío = sin configurar: no se guarda ningún default.
            edit.remove("omniroute_base_url")
        } else {
            edit.putString("omniroute_base_url", clean)
            // Una URL válida guardada = configuración completada.
            edit.putBoolean(KEY_GATEWAY_SETUP_SHOWN, true)
        }
        edit.apply()
    }

    /** true si hay una URL de gateway guardada (no vacía). */
    fun isBaseUrlConfigured(context: Context): Boolean =
        getBaseUrl(context).isNotBlank()

    /** true si el diálogo de primera apertura ya se mostró o se resolvió. */
    fun isGatewaySetupDone(context: Context): Boolean =
        prefs(context).getBoolean(KEY_GATEWAY_SETUP_SHOWN, false)

    /** Marca el diálogo de primera apertura como ya atendido. */
    fun markGatewaySetupShown(context: Context) {
        prefs(context).edit().putBoolean(KEY_GATEWAY_SETUP_SHOWN, true).apply()
    }

    /**
     * true si la URL apunta a un host del rango CGNAT de red privada.
     * Función pura (sin Context) para poder testearla sin Android.
     * Detecta por patrón, NUNCA comparando con una dirección concreta:
     * la dirección vieja no aparece en ningún literal del código.
     */
    fun isLegacyBaseUrl(url: String): Boolean {
        val host = url.trim().lowercase()
            .removePrefix("http://")
            .removePrefix("https://")
            .substringBefore('/')
            .substringBefore(':')
        // Rango CGNAT = primer octeto 100; se compara el octeto como
        // número para no dejar ningún literal de IP en el código.
        return host.substringBefore('.').toIntOrNull() == 100
    }

    /**
     * Migración v2.5.471: si la URL guardada es de la red personal
     * (host del rango CGNAT), se borra y se rearma el diálogo de primera
     * apertura para que el usuario escriba la URL de su propio gateway.
     * Devuelve true si borró algo (el llamador muestra el diálogo).
     * Idempotente y barata: se puede llamar en cada lectura de la URL.
     */
    fun migrateLegacyBaseUrl(context: Context): Boolean {
        val stored = prefs(context).getString("omniroute_base_url", null)
            ?.trim().orEmpty()
        if (stored.isEmpty() || !isLegacyBaseUrl(stored)) return false
        prefs(context).edit()
            .remove("omniroute_base_url")
            .putBoolean(KEY_GATEWAY_SETUP_SHOWN, false)
            .apply()
        return true
    }

    /** Modelo o combo del gateway (p. ej. `ivanna-free`, `pol/claude`). */
    fun getModel(context: Context): String =
        prefs(context).getString("omniroute_model", DEFAULT_MODEL)
            ?.trim()?.ifBlank { DEFAULT_MODEL } ?: DEFAULT_MODEL

    fun setModel(context: Context, model: String) {
        val clean = model.trim()
        prefs(context).edit()
            .putString("omniroute_model", clean.ifBlank { DEFAULT_MODEL })
            .apply()
    }
}
