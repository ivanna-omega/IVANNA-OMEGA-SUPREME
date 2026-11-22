package com.ivanna.omega.licensing

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Tests de LicenseKeyUtils (v2.5.446).
 *
 * Funciones puras, sin Android: corren en la JVM sin Robolectric.
 * Las reglas copian `normalizeLicenseKey` de `licenses.mjs` del
 * servidor, para que la app rechace en local lo mismo que el
 * servidor rechazaría con `bad_request`.
 *
 * NOTA: no hay Android SDK en esta VM: estos tests están escritos
 * para correr en CI (`testDebugUnitTest`). No se declaran como
 * pasados sin ejecutarlos.
 */
class LicenseKeyUtilsTest {

    @Test fun normalizeAcceptsCanonicalForm() {
        assertEquals(
            "IVANNA-ABCD-EFGH-JKMN",
            LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABCD-EFGH-JKMN"),
        )
    }

    @Test fun normalizeAcceptsLowercaseWithoutDashesOrSpaces() {
        assertEquals(
            "IVANNA-ABCD-EFGH-JKMN",
            LicenseKeyUtils.normalizeLicenseKey("ivanna abcd efgh jkmn"),
        )
        assertEquals(
            "IVANNA-ABCD-EFGH-JKMN",
            LicenseKeyUtils.normalizeLicenseKey("ivannaabcdefghjkmn"),
        )
    }

    @Test fun normalizeRejectsConfusableCharacters() {
        // Crockford: sin 0/O, 1/I/L.
        assertNull(LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABCD-EFG0-JKMN"))
        assertNull(LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABCD-EFGI-JKMN"))
        assertNull(LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABCD-EFGL-JKMN"))
        assertNull(LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABCD-EFGO-JKMN"))
    }

    @Test fun normalizeRejectsWrongLengthAndGarbage() {
        assertNull(LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABC"))
        assertNull(LicenseKeyUtils.normalizeLicenseKey("IVANNA-ABCD-EFGH-JKMN-NOPQ"))
        assertNull(LicenseKeyUtils.normalizeLicenseKey("OTRA-ABCD-EFGH-JKMN"))
        assertNull(LicenseKeyUtils.normalizeLicenseKey(""))
        assertNull(LicenseKeyUtils.normalizeLicenseKey(null))
    }

    @Test fun hashDeviceIdIsStableSha256Hex() {
        val a = LicenseKeyUtils.hashDeviceId("android-id-123")
        val b = LicenseKeyUtils.hashDeviceId("android-id-123")
        assertEquals(a, b)
        assertEquals(64, a.length)
        assertTrue(a.matches(Regex("[0-9a-f]{64}")))
        assertFalse(a.contains("android-id-123"))
    }

    @Test fun fingerprintIsEightHexCharsNeverTheKey() {
        val fp = LicenseKeyUtils.fingerprint("IVANNA-ABCD-EFGH-JKMN")
        assertEquals(8, fp.length)
        assertTrue(fp.matches(Regex("[0-9a-f]{8}")))
        assertFalse(fp.contains("IVANNA"))
    }

    @Test fun parseExpiresAtHandlesLifetimeNullAndInvalid() {
        assertNull(LicenseKeyUtils.parseExpiresAt(null))
        assertNull(LicenseKeyUtils.parseExpiresAt(""))
        assertNull(LicenseKeyUtils.parseExpiresAt("no-es-una-fecha"))
        assertEquals(
            1797206400000L,
            LicenseKeyUtils.parseExpiresAt("2026-12-14T00:00:00.000Z"),
        )
    }
}
