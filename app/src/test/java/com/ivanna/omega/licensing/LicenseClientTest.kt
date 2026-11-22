package com.ivanna.omega.licensing

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import java.io.IOException

/**
 * Tests de LicenseClient (v2.5.446).
 *
 * Transporte mock inyectado: ninguna prueba toca la red real (el
 * servidor aún no está desplegado). Robolectric porque el cliente
 * usa org.json.JSONObject, que es un stub en el android.jar de los
 * unit tests (mismo patrón que OmniRouteOrchestratorTest).
 *
 * NOTA: no hay Android SDK en esta VM: estos tests están escritos
 * para correr en CI (`testDebugUnitTest`). No se declaran como
 * pasados sin ejecutarlos.
 */
@RunWith(RobolectricTestRunner::class)
class LicenseClientTest {

    private class FakeHttp(
        private val handler: (url: String, body: String) -> LicenseHttpResponse,
    ) : LicenseHttp {
        var lastUrl: String? = null
        var lastBody: String? = null
        override fun post(url: String, body: String): LicenseHttpResponse {
            lastUrl = url
            lastBody = body
            return handler(url, body)
        }
    }

    private fun clientWith(body: String, status: Int = 200): Pair<LicenseClient, FakeHttp> {
        val fake = FakeHttp { _, _ -> LicenseHttpResponse(status, body) }
        return LicenseClient(fake, "https://mock.invalid") to fake
    }

    @Test fun lifetimeSuccessHasNullExpiry() {
        val (client, _) = clientWith(
            """{"valid":true,"plan":"lifetime","expiresAt":null,"devicesBound":1,"devicesMax":3}""",
        )
        val result = client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash")
        assertTrue(result is LicenseValidationOutcome.Valid)
        val valid = result as LicenseValidationOutcome.Valid
        assertEquals(LicensePlan.LIFETIME, valid.plan)
        assertNull(valid.expiresAtMillis)
        assertEquals(1, valid.devicesBound)
        assertEquals(3, valid.devicesMax)
    }

    @Test fun monthlySuccessParsesExpiry() {
        val (client, _) = clientWith(
            """{"valid":true,"plan":"monthly","expiresAt":"2026-12-14T00:00:00.000Z","devicesBound":2,"devicesMax":3}""",
        )
        val valid = client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash")
            as LicenseValidationOutcome.Valid
        assertEquals(LicensePlan.MONTHLY, valid.plan)
        assertEquals(1797206400000L, valid.expiresAtMillis)
        assertEquals(2, valid.devicesBound)
    }

    @Test fun invalidReasonsMapExactly() {
        val cases = mapOf(
            "not_found" to LicenseInvalidReason.NOT_FOUND,
            "revoked" to LicenseInvalidReason.REVOKED,
            "expired" to LicenseInvalidReason.EXPIRED,
            "device_limit" to LicenseInvalidReason.DEVICE_LIMIT,
            "bad_request" to LicenseInvalidReason.BAD_REQUEST,
        )
        for ((wire, expected) in cases) {
            val (client, _) = clientWith("""{"valid":false,"reason":"$wire"}""")
            val result = client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash")
            assertEquals(expected, (result as LicenseValidationOutcome.Invalid).reason)
        }
    }

    @Test fun badRequestHttp400IsInvalidNotNetworkError() {
        val (client, _) = clientWith("""{"valid":false,"reason":"bad_request"}""", status = 400)
        val result = client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash")
        assertEquals(
            LicenseInvalidReason.BAD_REQUEST,
            (result as LicenseValidationOutcome.Invalid).reason,
        )
    }

    @Test fun networkFailureIsNetworkError() {
        val fake = FakeHttp { _, _ -> throw IOException("sin red") }
        val client = LicenseClient(fake, "https://mock.invalid")
        assertEquals(
            LicenseValidationOutcome.NetworkError,
            client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash"),
        )
    }

    @Test fun malformedBodiesAreMalformedResponse() {
        val badBodies = listOf(
            "esto no es json",
            "",
            """{"plan":"lifetime"}""", // sin "valid"
            """{"valid":true,"plan":"gold"}""", // plan desconocido
            """{"valid":true}""", // sin plan
            """{"valid":true,"plan":"lifetime","devicesBound":"uno"}""",
        )
        for (body in badBodies) {
            val (client, _) = clientWith(body)
            assertEquals(
                "cuerpo: $body",
                LicenseValidationOutcome.MalformedResponse,
                client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash"),
            )
        }
    }

    @Test fun requestPostsToValidatePathWithKeyAndDeviceId() {
        val (client, fake) = clientWith(
            """{"valid":true,"plan":"lifetime","expiresAt":null,"devicesBound":1,"devicesMax":3}""",
        )
        client.validate("IVANNA-ABCD-EFGH-JKMN", "devicehash-abc")
        assertEquals("https://mock.invalid/v1/licenses/validate", fake.lastUrl)
        val body = JSONObject(fake.lastBody!!)
        assertEquals("IVANNA-ABCD-EFGH-JKMN", body.getString("licenseKey"))
        assertEquals("devicehash-abc", body.getString("deviceId"))
    }
}
