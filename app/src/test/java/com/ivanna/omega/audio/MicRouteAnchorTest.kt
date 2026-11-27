package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

/**
 * Tests del ancla de micrófono interno (mic con DAC conectado, v2.5.506).
 * Puerto falso: sin hardware, sin AudioRecord real.
 */
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class MicRouteAnchorTest {

    private class FakePort(
        var builtin: MicRouteAnchor.MicDevice? =
            MicRouteAnchor.MicDevice(AudioDeviceInfo.TYPE_BUILTIN_MIC, 1, "Built-in Microphone"),
        var routed: MicRouteAnchor.MicDevice? = builtin,
        var preferred: MicRouteAnchor.MicDevice? = null,
        var setPreferredWorks: Boolean = true
    ) : MicRouteAnchor.MicDevicePort {
        var setPreferredCalls = 0
        override fun builtinMic(): MicRouteAnchor.MicDevice? = builtin
        override fun setPreferred(device: MicRouteAnchor.MicDevice?): Boolean {
            setPreferredCalls++
            if (!setPreferredWorks) return false
            preferred = device
            return true
        }

        override fun preferredDevice(): MicRouteAnchor.MicDevice? = preferred
        override fun routedDevice(): MicRouteAnchor.MicDevice? = routed
    }

    private fun usbMic() =
        MicRouteAnchor.MicDevice(AudioDeviceInfo.TYPE_USB_DEVICE, 7, "JieLi USB Audio")

    @Test
    fun `anchor fija el mic interno como preferido`() {
        val port = FakePort()
        val outcome = MicRouteAnchor.anchor(port)
        assertTrue(outcome is MicRouteAnchor.AnchorOutcome.Anchored)
        assertEquals(1, port.setPreferredCalls)
        assertEquals(AudioDeviceInfo.TYPE_BUILTIN_MIC, port.preferred?.type)
    }

    @Test
    fun `anchor sin mic interno no bloquea (devuelve NoBuiltinMic)`() {
        val port = FakePort(builtin = null, routed = null)
        val outcome = MicRouteAnchor.anchor(port)
        assertTrue(outcome is MicRouteAnchor.AnchorOutcome.NoBuiltinMic)
        assertEquals(0, port.setPreferredCalls)
    }

    @Test
    fun `anchor fallido se reporta sin lanzar`() {
        val port = FakePort(setPreferredWorks = false)
        val outcome = MicRouteAnchor.anchor(port)
        assertTrue(outcome is MicRouteAnchor.AnchorOutcome.AnchorFailed)
    }

    @Test
    fun `verify Ok con mic interno enrutado`() {
        val port = FakePort()
        val outcome = MicRouteAnchor.verify(port)
        assertTrue(outcome is MicRouteAnchor.VerifyOutcome.Ok)
    }

    @Test
    fun `verify detecta deriva al USB`() {
        val port = FakePort(routed = usbMic())
        val outcome = MicRouteAnchor.verify(port)
        assertTrue(outcome is MicRouteAnchor.VerifyOutcome.Drifted)
        val drifted = outcome as MicRouteAnchor.VerifyOutcome.Drifted
        assertEquals(AudioDeviceInfo.TYPE_USB_DEVICE, drifted.device?.type)
        assertTrue(drifted.device!!.isUsb)
    }

    @Test
    fun `verify con routed null es Unknown (no se afirma ruta)`() {
        val port = FakePort(routed = null)
        assertTrue(MicRouteAnchor.verify(port) is MicRouteAnchor.VerifyOutcome.Unknown)
    }

    @Test
    fun `reanchor tras deriva vuelve al interno y cuenta`() {
        val port = FakePort(routed = usbMic())
        val before = MicRouteAnchor.RouteStatus(anchored = true, reanchorCount = 2)
        // El re-ancla fija la preferencia; la ruta leída sigue en USB
        // hasta que Android la mueva (el estado refleja lo leído).
        val after = MicRouteAnchor.reanchor(port, before, 12345L)
        assertEquals(3, after.reanchorCount)
        assertEquals(12345L, after.lastCheckMs)
        assertEquals(1, port.setPreferredCalls)
        assertEquals(AudioDeviceInfo.TYPE_BUILTIN_MIC, port.preferred?.type)
    }

    @Test
    fun `nombres de dispositivo en espanol`() {
        assertEquals("Mic interno", MicRouteAnchor.deviceTypeName(AudioDeviceInfo.TYPE_BUILTIN_MIC))
        assertEquals("Mic USB", MicRouteAnchor.deviceTypeName(AudioDeviceInfo.TYPE_USB_DEVICE))
        assertEquals("Mic Bluetooth", MicRouteAnchor.deviceTypeName(AudioDeviceInfo.TYPE_BLUETOOTH_SCO))
    }
}
