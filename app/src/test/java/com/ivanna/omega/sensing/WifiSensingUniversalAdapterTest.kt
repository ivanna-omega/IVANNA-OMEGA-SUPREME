package com.ivanna.omega.sensing

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Test

/**
 * El adaptador universal hace que el Wi-Fi tenga efecto en cualquier equipo y
 * router: elige la mejor fuente real (CSI > RSSI Android > RSSI root > nada)
 * y evalúa la política sobre cada escena, también con solo metadatos RSSI.
 * El efecto en el sonido llega por la medición acústica que la decisión
 * dispara, nunca por DSP directo desde la radio.
 */
class WifiSensingUniversalAdapterTest {
    private fun scene(
        level: UniversalWifiSensingAdapter.DataLevel =
            UniversalWifiSensingAdapter.DataLevel.WIFI_METADATA_ONLY,
        movement: Double? = 0.7,
        confidence: Double = 0.5,
        occupancy: UniversalWifiSensingAdapter.OccupancyHint =
            UniversalWifiSensingAdapter.OccupancyHint.SIGNAL_CHANGED
    ) = UniversalWifiSensingAdapter.SceneFeatures(
        level = level,
        observationCount = 8,
        medianRssiDbm = -54,
        signalVariabilityDb = 4.0,
        movementIndicator = movement,
        occupancy = occupancy,
        confidence = confidence,
        source = "test",
        timestampMs = 1000L,
        limitations = listOf("test")
    )

    @Test
    fun csiProviderWinsWhenVerified() {
        assertEquals(
            WifiSensingUniversalAdapter.Source.CSI_PROVIDER,
            WifiSensingUniversalAdapter.selectSource(
                csiAvailable = true, standardScanApiAvailable = true, wifiPresent = true
            )
        )
    }

    @Test
    fun androidRssiIsTheFallbackOnAnyRouterWithoutCsi() {
        assertEquals(
            WifiSensingUniversalAdapter.Source.ANDROID_RSSI,
            WifiSensingUniversalAdapter.selectSource(
                csiAvailable = false, standardScanApiAvailable = true, wifiPresent = true
            )
        )
    }

    @Test
    fun rootRssiIsTheFallbackWhenAndroidScanIsUnavailable() {
        assertEquals(
            WifiSensingUniversalAdapter.Source.ROOT_RSSI,
            WifiSensingUniversalAdapter.selectSource(
                csiAvailable = false, standardScanApiAvailable = false, wifiPresent = true
            )
        )
    }

    @Test
    fun noWifiMeansNoSource() {
        assertEquals(
            WifiSensingUniversalAdapter.Source.NONE,
            WifiSensingUniversalAdapter.selectSource(
                csiAvailable = false, standardScanApiAvailable = false, wifiPresent = false
            )
        )
    }

    @Test
    fun rssiOnlySceneStillRequestsARealAcousticMeasurement() {
        val result = WifiSensingUniversalAdapter.evaluate(
            scene(), acousticCalibrationAvailable = false, acousticCalibrationAgeMs = null
        )
        assertEquals(
            WifiAcousticAdaptationPolicy.Action.REQUEST_ACOUSTIC_REMEASUREMENT,
            result.action
        )
        assertFalse(result.dspMayChange)
    }

    @Test
    fun weakRadioEvidenceRequestsNothing() {
        val result = WifiSensingUniversalAdapter.evaluate(
            scene(movement = 0.1, confidence = 0.1),
            acousticCalibrationAvailable = true,
            acousticCalibrationAgeMs = 1000L
        )
        assertEquals(
            WifiAcousticAdaptationPolicy.Action.NO_ACTION,
            result.action
        )
    }

    @Test
    fun recentCalibrationSuppressesTheRepeatRequest() {
        val result = WifiSensingUniversalAdapter.evaluate(
            scene(), acousticCalibrationAvailable = true, acousticCalibrationAgeMs = 60_000L
        )
        assertEquals(
            WifiAcousticAdaptationPolicy.Action.NO_ACTION,
            result.action
        )
    }

    @Test
    fun staleCalibrationRequestsAgainAfterSuppressionWindow() {
        val result = WifiSensingUniversalAdapter.evaluate(
            scene(),
            acousticCalibrationAvailable = true,
            acousticCalibrationAgeMs = WifiSensingUniversalAdapter.RECENT_CALIBRATION_SUPPRESS_MS + 1000L
        )
        assertEquals(
            WifiAcousticAdaptationPolicy.Action.REQUEST_ACOUSTIC_REMEASUREMENT,
            result.action
        )
    }

    @Test
    fun noWifiKeepsCurrentAudioState() {
        val result = WifiSensingUniversalAdapter.evaluate(
            scene(level = UniversalWifiSensingAdapter.DataLevel.NO_WIFI, movement = null),
            acousticCalibrationAvailable = true,
            acousticCalibrationAgeMs = 1000L
        )
        assertEquals(
            WifiAcousticAdaptationPolicy.Action.NO_ACTION,
            result.action
        )
    }
}
