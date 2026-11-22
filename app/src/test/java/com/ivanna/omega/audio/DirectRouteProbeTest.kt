package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import android.media.AudioFormat
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Puerta Hi-Fi USB (0030, v2.5.459): DirectRouteProbe.select es pura
 * (sin Android) y se testea en host con perfiles falsos.
 *
 * El caso real (verificado en vivo 2026-11-16 13:29 CST, DAC JieLi):
 * PCM_24_BIT_PACKED @ 44100/48000 en USB Headset Out → (24-bit, 48000).
 */
class DirectRouteProbeTest {

    private val usb24 = DirectProfile(
        encoding = AudioFormat.ENCODING_PCM_24BIT_PACKED,
        sampleRates = intArrayOf(44_100, 48_000),
        deviceType = AudioDeviceInfo.TYPE_USB_HEADSET
    )
    private val usb16 = DirectProfile(
        encoding = AudioFormat.ENCODING_PCM_16BIT,
        sampleRates = intArrayOf(44_100, 48_000),
        deviceType = AudioDeviceInfo.TYPE_USB_HEADSET
    )
    private val usb32 = DirectProfile(
        encoding = AudioFormat.ENCODING_PCM_32BIT,
        sampleRates = intArrayOf(44_100, 48_000, 96_000),
        deviceType = AudioDeviceInfo.TYPE_USB_DEVICE
    )

    @Test fun realDacSelects24BitAt48k() {
        // El DAC de Esse: 24-bit @ 44100/48000 → 48000 (la más alta).
        val route = DirectRouteProbe.select(
            listOf(usb16, usb24), AudioDeviceInfo.TYPE_USB_HEADSET
        )
        assertEquals(AudioFormat.ENCODING_PCM_24BIT_PACKED, route?.encoding)
        assertEquals(48_000, route?.sampleRate)
    }

    @Test fun prefers24BitOver32Bit() {
        val route = DirectRouteProbe.select(
            listOf(usb32, usb24), AudioDeviceInfo.TYPE_USB_HEADSET
        )
        assertEquals(AudioFormat.ENCODING_PCM_24BIT_PACKED, route?.encoding)
        assertEquals(48_000, route?.sampleRate)
    }

    @Test fun fallsBackTo32BitWhenNo24Bit() {
        val route = DirectRouteProbe.select(
            listOf(usb32), AudioDeviceInfo.TYPE_USB_DEVICE
        )
        assertEquals(AudioFormat.ENCODING_PCM_32BIT, route?.encoding)
        assertEquals(96_000, route?.sampleRate)
    }

    @Test fun nullWhenUsbOnlyAnnounces16Bit() {
        assertNull(DirectRouteProbe.select(listOf(usb16), AudioDeviceInfo.TYPE_USB_HEADSET))
    }

    @Test fun nullWhenNoProfiles() {
        assertNull(DirectRouteProbe.select(emptyList(), AudioDeviceInfo.TYPE_USB_DEVICE))
    }

    @Test fun nullWhenDeviceIsNotUsb() {
        // Un perfil 24-bit en BT/altavoz no habilita la ruta direct USB.
        val bt24 = usb24.copy(deviceType = AudioDeviceInfo.TYPE_BLUETOOTH_A2DP)
        assertNull(DirectRouteProbe.select(listOf(bt24), AudioDeviceInfo.TYPE_BLUETOOTH_A2DP))
        assertNull(DirectRouteProbe.select(listOf(usb24), AudioDeviceInfo.TYPE_BLUETOOTH_A2DP))
    }

    @Test fun nullWhenProfileDeviceTypeDoesNotMatchConnectedUsb() {
        // El perfil es para USB_HEADSET pero el conectado es USB_DEVICE.
        assertNull(DirectRouteProbe.select(listOf(usb24), AudioDeviceInfo.TYPE_USB_DEVICE))
    }

    @Test fun capsRateAt192k() {
        val hi = DirectProfile(
            encoding = AudioFormat.ENCODING_PCM_24BIT_PACKED,
            sampleRates = intArrayOf(44_100, 48_000, 96_000, 192_000, 384_000),
            deviceType = AudioDeviceInfo.TYPE_USB_DEVICE
        )
        val route = DirectRouteProbe.select(listOf(hi), AudioDeviceInfo.TYPE_USB_DEVICE)
        assertEquals(192_000, route?.sampleRate)
    }

    @Test fun nullWhenOnlyRateAboveCap() {
        // Un DAC que solo anuncia 384k no entra en el tope práctico 192k.
        val only384 = DirectProfile(
            encoding = AudioFormat.ENCODING_PCM_24BIT_PACKED,
            sampleRates = intArrayOf(384_000),
            deviceType = AudioDeviceInfo.TYPE_USB_DEVICE
        )
        assertNull(DirectRouteProbe.select(listOf(only384), AudioDeviceInfo.TYPE_USB_DEVICE))
    }

    @Test fun ignoresRatesBelowVoiceFloor() {
        val low = DirectProfile(
            encoding = AudioFormat.ENCODING_PCM_24BIT_PACKED,
            sampleRates = intArrayOf(8000, 16_000, 48_000),
            deviceType = AudioDeviceInfo.TYPE_USB_DEVICE
        )
        val route = DirectRouteProbe.select(listOf(low), AudioDeviceInfo.TYPE_USB_DEVICE)
        assertEquals(48_000, route?.sampleRate)
    }
}
