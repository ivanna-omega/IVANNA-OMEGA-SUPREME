package com.ivanna.omega.licensing

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import java.io.IOException

/**
 * Tests de LicenseManager (v2.5.446).
 *
 * Store en memoria + cliente mock: ninguna prueba toca la red real.
 * El "ahora" es fijo (2026-11-14T00:00:00Z = 1794614400000 ms) para
 * que la caducidad de la caché mensual sea determinista.
 *
 * NOTA: no hay Android SDK en esta VM: estos tests están escritos
 * para correr en CI (`testDebugUnitTest`). No se declaran como
 * pasados sin ejecutarlos.
 */
@RunWith(RobolectricTestRunner::class)
class LicenseManagerTest {

    private val nowMillis = 1794614400000L // 2026-11-14T00:00:00Z
    private val monthlyValidUntil = "2026-12-14T00:00:00.000Z"
    private val monthlyExpiredAt = "2026-11-01T00:00:00.000Z"
    private val key = "IVANNA-ABCD-EFGH-JKMN"

    private lateinit var context: Context

    @Before fun setUp() {
        context = ApplicationProvider.getApplicationContext()
    }

    private fun managerWith(
        store: LicenseStore = InMemoryLicenseStore(),
        outcome: LicenseValidationOutcome,
        calls: MutableList<String> = mutableListOf(),
    ): LicenseManager {
        val fakeHttp = object : LicenseHttp {
            override fun post(url: String, body: String): LicenseHttpResponse {
                calls += url
                return when (outcome) {
                    is LicenseValidationOutcome.Valid -> LicenseHttpResponse(
                        200,
                        """{"valid":true,"plan":"${outcome.plan.wireValue}","expiresAt":${
                            outcome.expiresAtMillis?.let {
                                "\"${java.time.Instant.ofEpochMilli(it)}\""
                            } ?: "null"
                        },"devicesBound":${outcome.devicesBound},"devicesMax":${outcome.devicesMax}}""",
                    )
                    is LicenseValidationOutcome.Invalid ->
                        LicenseHttpResponse(200, """{"valid":false,"reason":"${outcome.reason.wireValue}"}""")
                    LicenseValidationOutcome.NetworkError -> throw IOException("sin red")
                    LicenseValidationOutcome.MalformedResponse ->
                        LicenseHttpResponse(200, "esto no es json")
                }
            }
        }
        return LicenseManager(
            context = context,
            store = store,
            clientFactory = { LicenseClient(fakeHttp, "https://mock.invalid") },
            nowMillis = { nowMillis },
            deviceIdProvider = { "devicehash-test" },
        )
    }

    private fun validOutcome(plan: LicensePlan, expiresAt: Long?): LicenseValidationOutcome.Valid =
        LicenseValidationOutcome.Valid(plan, expiresAt, 1, 3)

    private fun storedKeyManager(store: LicenseStore): LicenseManager {
        store.setLicenseKey(key)
        return managerWith(store, validOutcome(LicensePlan.LIFETIME, null))
    }

    // ── Sin clave ──────────────────────────────────────────────────────

    @Test fun noKeyMeansUnvalidatedAndNoNetworkCall() {
        val calls = mutableListOf<String>()
        val manager = managerWith(outcome = validOutcome(LicensePlan.LIFETIME, null), calls = calls)
        val state = manager.refresh()
        assertEquals(LicenseAccess.UNVALIDATED, state.access)
        assertFalse(state.hasFullAccess)
        assertTrue(calls.isEmpty())
    }

    // ── Éxito ──────────────────────────────────────────────────────────

    @Test fun lifetimeValidationIsValidAndCached() {
        val store = InMemoryLicenseStore()
        val manager = storedKeyManager(store)
        val state = manager.refresh()
        assertEquals(LicenseAccess.VALID, state.access)
        assertTrue(state.hasFullAccess)
        assertEquals(LicensePlan.LIFETIME, state.plan)
        assertNull(state.expiresAtMillis)
        assertEquals(1, state.devicesBound)
        assertEquals(3, state.devicesMax)
        val cached = store.getCachedValidation()
        assertEquals(LicensePlan.LIFETIME, cached?.plan)
        assertEquals(nowMillis, cached?.validatedAtMillis)
    }

    @Test fun monthlyValidationKeepsExpiry() {
        val store = InMemoryLicenseStore().also { it.setLicenseKey(key) }
        val manager = managerWith(
            store,
            validOutcome(LicensePlan.MONTHLY, 1797206400000L),
        )
        val state = manager.refresh()
        assertEquals(LicenseAccess.VALID, state.access)
        assertEquals(LicensePlan.MONTHLY, state.plan)
        assertEquals(1797206400000L, state.expiresAtMillis)
    }

    // ── Inválida ───────────────────────────────────────────────────────

    @Test fun revokedIsInvalidAndClearsCache() {
        val store = InMemoryLicenseStore().also {
            it.setLicenseKey(key)
            it.setCachedValidation(
                CachedValidation(LicensePlan.LIFETIME, null, 1, 3, nowMillis),
            )
        }
        val manager = managerWith(
            store,
            LicenseValidationOutcome.Invalid(LicenseInvalidReason.REVOKED),
        )
        val state = manager.refresh()
        assertEquals(LicenseAccess.INVALID, state.access)
        assertFalse(state.hasFullAccess)
        assertEquals(LicenseInvalidReason.REVOKED, state.invalidReason)
        assertNull(store.getCachedValidation())
    }

    @Test fun expiredAndDeviceLimitAreInvalid() {
        for (reason in listOf(LicenseInvalidReason.EXPIRED, LicenseInvalidReason.DEVICE_LIMIT)) {
            val store = InMemoryLicenseStore().also { it.setLicenseKey(key) }
            val manager = managerWith(store, LicenseValidationOutcome.Invalid(reason))
            val state = manager.refresh()
            assertEquals(reason, state.invalidReason)
            assertFalse(state.hasFullAccess)
        }
    }

    // ── Fail-open sin conexión ─────────────────────────────────────────

    @Test fun networkErrorWithLifetimeCacheIsValidOffline() {
        val store = InMemoryLicenseStore().also {
            it.setLicenseKey(key)
            it.setCachedValidation(
                CachedValidation(LicensePlan.LIFETIME, null, 1, 3, nowMillis - 86_400_000L),
            )
        }
        val manager = managerWith(store, LicenseValidationOutcome.NetworkError)
        val state = manager.refresh()
        assertEquals(LicenseAccess.VALID_OFFLINE, state.access)
        assertTrue(state.hasFullAccess)
        assertTrue(state.offline)
        assertEquals(LicensePlan.LIFETIME, state.plan)
    }

    @Test fun networkErrorWithUnexpiredMonthlyCacheIsValidOffline() {
        val store = InMemoryLicenseStore().also {
            it.setLicenseKey(key)
            it.setCachedValidation(
                CachedValidation(LicensePlan.MONTHLY, 1797206400000L, 2, 3, nowMillis),
            )
        }
        val manager = managerWith(store, LicenseValidationOutcome.NetworkError)
        val state = manager.refresh()
        assertEquals(LicenseAccess.VALID_OFFLINE, state.access)
        assertTrue(state.hasFullAccess)
    }

    @Test fun networkErrorWithExpiredMonthlyCacheIsInvalidExpired() {
        val store = InMemoryLicenseStore().also {
            it.setLicenseKey(key)
            it.setCachedValidation(
                CachedValidation(LicensePlan.MONTHLY, 1793491200000L, 1, 3, nowMillis),
            )
        }
        val manager = managerWith(store, LicenseValidationOutcome.NetworkError)
        val state = manager.refresh()
        assertEquals(LicenseAccess.INVALID, state.access)
        assertEquals(LicenseInvalidReason.EXPIRED, state.invalidReason)
        assertFalse(state.hasFullAccess)
        assertTrue(state.offline)
    }

    @Test fun networkErrorWithoutCacheIsUnvalidatedOffline() {
        val store = InMemoryLicenseStore().also { it.setLicenseKey(key) }
        val manager = managerWith(store, LicenseValidationOutcome.NetworkError)
        val state = manager.refresh()
        assertEquals(LicenseAccess.UNVALIDATED, state.access)
        assertFalse(state.hasFullAccess)
        assertTrue(state.offline)
    }

    @Test fun malformedResponseFailsOpenWithCache() {
        val store = InMemoryLicenseStore().also {
            it.setLicenseKey(key)
            it.setCachedValidation(
                CachedValidation(LicensePlan.LIFETIME, null, 1, 3, nowMillis),
            )
        }
        val manager = managerWith(store, LicenseValidationOutcome.MalformedResponse)
        assertEquals(LicenseAccess.VALID_OFFLINE, manager.refresh().access)
    }

    // ── Clave y telemetría ─────────────────────────────────────────────

    @Test fun saveLicenseKeyNormalizesAndRejectsGarbage() {
        val manager = managerWith(outcome = validOutcome(LicensePlan.LIFETIME, null))
        assertTrue(manager.saveLicenseKey("ivanna abcd efgh jkmn"))
        assertFalse(manager.saveLicenseKey("clave-invalida"))
        assertFalse(manager.saveLicenseKey(""))
    }

    @Test fun telemetryNeverExposesTheKey() {
        val store = InMemoryLicenseStore()
        val manager = storedKeyManager(store)
        val state = manager.refresh()
        // telemetrySummary() sin argumento usa currentState(), que tras un
        // refresh exitoso lee de caché y reporta VALID_OFFLINE; aquí se
        // resume el estado vivo VALID que devolvió refresh().
        val summary = manager.telemetrySummary(state)
        assertEquals("VALID", summary["access"])
        assertEquals("lifetime", summary["plan"])
        assertEquals("1/3", summary["devices"])
        val fingerprint = summary["keyFingerprint"]!!
        assertEquals(8, fingerprint.length)
        assertFalse(summary.values.any { it.contains("IVANNA-ABCD") })
        assertFalse(summary.values.any { it.contains(key) })
    }

    @Test fun currentStateReadsCacheWithoutNetwork() {
        val calls = mutableListOf<String>()
        val store = InMemoryLicenseStore().also {
            it.setLicenseKey(key)
            it.setCachedValidation(
                CachedValidation(LicensePlan.MONTHLY, 1797206400000L, 2, 3, nowMillis),
            )
        }
        val manager = managerWith(store, validOutcome(LicensePlan.LIFETIME, null), calls)
        val state = manager.currentState()
        assertEquals(LicenseAccess.VALID_OFFLINE, state.access)
        assertEquals(LicensePlan.MONTHLY, state.plan)
        assertTrue(calls.isEmpty())
    }
}
