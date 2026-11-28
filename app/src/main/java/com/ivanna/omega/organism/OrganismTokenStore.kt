package com.ivanna.omega.organism

import android.content.Context
import android.content.SharedPreferences
import android.util.Log
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey

/**
 * OrganismTokenStore — el token del organismo vive cifrado en el teléfono.
 *
 * El token es el MUSE_CONNECTOR_TOKEN de Muselink (Bearer). Se guarda UNA
 * vez (lo pega Esse en la tarjeta del organismo, solo cuando el estado es
 * "Sin token") en EncryptedSharedPreferences (Android Keystore). NUNCA
 * va en un literal, en archivos planos, en URLs, en excepciones ni en
 * logs. Si expira y no hay endpoint de refresh, el sender pasa a
 * TOKEN_EXPIRADO y hay que reautorizar una vez (límite honesto del
 * consejo: Android no puede renovar lo que el backend no ofrece).
 *
 * La interfaz [TokenStore] permite probar el sender con un falso en JVM.
 */
interface TokenStore {
    fun getToken(): String?
    fun setToken(raw: String): Boolean
    fun clearToken()
}

class OrganismTokenStore(context: Context) : TokenStore {
    companion object {
        private const val TAG = "OrganismTokenStore"
        private const val PREFS_FILE = "ivanna_organism_token"
        private const val KEY_TOKEN = "muselink_bearer_token"
    }

    private val prefs: SharedPreferences? = runCatching {
        val masterKey = MasterKey.Builder(context.applicationContext)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        EncryptedSharedPreferences.create(
            context.applicationContext,
            PREFS_FILE,
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM
        )
    }.onFailure { e ->
        Log.e(TAG, "No se pudo abrir el almacén cifrado: ${e.message}")
    }.getOrNull()

    override fun getToken(): String? =
        runCatching { prefs?.getString(KEY_TOKEN, null)?.takeIf { it.isNotBlank() } }.getOrNull()

    override fun setToken(raw: String): Boolean {
        val token = raw.trim().trim('"', '\'', '`').filter { !it.isWhitespace() }
        if (token.isEmpty()) {
            Log.w(TAG, "Token rechazado: vacío")
            return false
        }
        // Sin piso de longitud: el servidor valida el token (401 si es
        // incorrecto). El MUSE_CONNECTOR_TOKEN real es corto (6 caracteres)
        // y el piso viejo de 10 lo rechazaba antes de guardarlo, así que el
        // pegado correcto nunca llegaba al almacén cifrado.
        return runCatching {
            prefs?.edit()?.putString(KEY_TOKEN, token)?.apply() ?: return false
            Log.i(TAG, "Token del organismo guardado cifrado")
            true
        }.getOrDefault(false)
    }

    override fun clearToken() {
        runCatching { prefs?.edit()?.remove(KEY_TOKEN)?.apply() }
    }
}

/** Falso en memoria para tests (nunca toca Keystore ni disco). */
class InMemoryTokenStore(initialToken: String? = null) : TokenStore {
    // Nombre distinto de `token`: una propiedad `var token` generaría
    // getToken() y chocaría con el método de la interfaz (mismo JVM
    // signature). El compilador lo rechaza como platform declaration clash.
    private var storedToken: String? = initialToken

    override fun getToken(): String? = storedToken
    override fun setToken(raw: String): Boolean {
        storedToken = raw.trim()
        // Sin piso de longitud (igual que el almacén real): cualquier token
        // no vacío se guarda; el servidor lo valida (401 si es incorrecto).
        return storedToken!!.isNotEmpty()
    }

    override fun clearToken() {
        storedToken = null
    }
}
