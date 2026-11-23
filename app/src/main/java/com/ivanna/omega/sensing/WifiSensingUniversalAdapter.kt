package com.ivanna.omega.sensing

import android.content.Context

/**
 * Adaptador universal de sensing Wi-Fi: hace que la función tenga efecto en
 * cualquier dispositivo y router, no solo donde hay un proveedor CSI verificado.
 *
 * El problema que resuelve: la política de adaptación
 * ([WifiAcousticAdaptationPolicy]) existía pero NADIE la evaluaba — las
 * features de la escena solo llegaban al texto de la franja. Y el sondeo solo
 * tenía sentido con CSI, que ningún equipo trae verificado de fábrica
 * ([WifiSensingCapabilityProbe] siempre reporta `csiAvailable = false`).
 *
 * El adaptador:
 * 1. Elige la mejor fuente que ESTE equipo y ESTE router entregan de verdad
 *    (CSI verificado > escaneo Android por RSSI > respaldo root por RSSI >
 *    nada), sin inventar CSI a partir de RSSI: el nivel de datos se conserva
 *    honesto ([UniversalWifiSensingAdapter.DataLevel]).
 * 2. Evalúa la política sobre cada escena (también con solo metadatos RSSI:
 *    cambio de señal + confianza) y devuelve su decisión.
 * 3. Suprime la petición repetida justo después de medir: si la calibración
 *    acústica tiene menos de 5 minutos, el cambio que la disparó es el mismo
 *    que ya se midió; no se vuelve a pedir hasta que llegue un cambio nuevo.
 *
 * La política sigue siendo la única autoridad de la decisión de radio; el
 * adaptador solo la alimenta y le pone el guard de frescura. El DSP nunca
 * cambia directo por Wi-Fi: la decisión solo pide una medición acústica real
 * ([com.ivanna.omega.ui.RoomMeasureLauncher]).
 */
object WifiSensingUniversalAdapter {
    enum class Source { CSI_PROVIDER, ANDROID_RSSI, ROOT_RSSI, NONE }

    /** Supresión tras medir: no volver a pedir con una calibración recién hecha. */
    const val RECENT_CALIBRATION_SUPPRESS_MS: Long = 5 * 60 * 1000L

    /**
     * Selección pura (testeable sin Android): la mejor fuente que el equipo y
     * el router entregan según sus capacidades declaradas.
     */
    fun selectSource(
        csiAvailable: Boolean,
        standardScanApiAvailable: Boolean,
        wifiPresent: Boolean
    ): Source = when {
        csiAvailable -> Source.CSI_PROVIDER
        standardScanApiAvailable -> Source.ANDROID_RSSI
        wifiPresent -> Source.ROOT_RSSI
        else -> Source.NONE
    }

    /** Selección con el equipo real. */
    fun selectSource(context: Context): Source {
        val csiAvailable = UniversalWifiSensingAdapter.availableCsiProviders(context).isNotEmpty()
        val probe = WifiSensingCapabilityProbe.inspect(context)
        return selectSource(csiAvailable, probe.standardScanApiAvailable, probe.wifiPresent)
    }

    /** Etiqueta honesta de lo que este equipo y router entregan de verdad. */
    fun describeSource(source: Source): String = when (source) {
        Source.CSI_PROVIDER -> "CSI verificado en este equipo"
        Source.ANDROID_RSSI -> "RSSI Android: este router no expone CSI; se usa cambio de señal"
        Source.ROOT_RSSI -> "RSSI por respaldo root: el escaneo Android no está disponible"
        Source.NONE -> "Sin Wi-Fi en este equipo"
    }

    /**
     * Evalúa la escena con la política y aplica el guard de frescura.
     * `acousticCalibrationAgeMs` es null cuando no hay calibración medida.
     */
    fun evaluate(
        scene: UniversalWifiSensingAdapter.SceneFeatures,
        acousticCalibrationAvailable: Boolean,
        acousticCalibrationAgeMs: Long?
    ): WifiAcousticAdaptationPolicy.Decision {
        if (acousticCalibrationAgeMs != null &&
            acousticCalibrationAgeMs >= 0L &&
            acousticCalibrationAgeMs < RECENT_CALIBRATION_SUPPRESS_MS
        ) {
            return WifiAcousticAdaptationPolicy.Decision(
                WifiAcousticAdaptationPolicy.Action.NO_ACTION,
                "Medición acústica reciente; esperando un cambio nuevo."
            )
        }
        return WifiAcousticAdaptationPolicy.evaluate(
            WifiAcousticAdaptationPolicy.Input(
                scene = scene,
                acousticCalibrationAvailable = acousticCalibrationAvailable,
                acousticCalibrationAgeMs = acousticCalibrationAgeMs
            )
        )
    }
}
