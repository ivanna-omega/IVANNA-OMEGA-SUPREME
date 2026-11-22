package com.ivanna.omega.ui

import android.content.Context

/**
 * DevMode — v2.5.473 (§1.1/§1.2 fase 3).
 *
 * Las pantallas internas / de diagnóstico (5 ejes, OEM++, flota, asistente,
 * lab, benchmark, ABX, Phase 7, motores, red, WFS, SOFA/AF/RIR/SAF, visualizador,
 * Music Intelligence, conciertos, Cognitive) NO se borran, pero solo existen
 * detrás de este flag. Se activa con 7 toques en la versión (SISTEMA →
 * "Acerca de / versión"). OFF por defecto; persiste en SharedPreferences.
 * Las rutas viven en el NavHost envueltas en DevGate (MainActivity): sin el
 * flag muestran el aviso y vuelven atrás.
 */
object DevMode {
    private const val PREFS = "ivanna_dev_mode"
    private const val KEY_ENABLED = "dev_mode_enabled"
    const val TAPS_TO_TOGGLE = 7

    fun isEnabled(context: Context): Boolean =
        runCatching {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .getBoolean(KEY_ENABLED, false)
        }.getOrDefault(false)

    fun setEnabled(context: Context, enabled: Boolean) {
        runCatching {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                .edit().putBoolean(KEY_ENABLED, enabled).apply()
        }
    }
}
