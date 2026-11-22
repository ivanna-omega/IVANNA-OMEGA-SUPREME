/*
 * © 2026 Luis Uriel Pimentel Pérez — IVANNA N-P-E
 * AppUpdateCheckerTest.kt — Revisor de actualizaciones (v2.5.444).
 *
 * Prueba la lógica pura del revisor: comparación de versiones, parseo
 * del JSON de GitHub releases/latest, extracción de SHA-256 de
 * SHA256SUMS.txt, verificación de descarga y la cadencia de revisión.
 * Sin red (fixtures con la forma real de la API de GitHub).
 * org.json es un stub en la JVM sin Robolectric (mismo patrón que
 * TidalApiParsingTest): por eso esta clase usa Robolectric.
 */
package com.ivanna.omega.core

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class AppUpdateCheckerTest {

    companion object {
        const val RELEASE_JSON = """{
          "tag_name": "v2.5.444",
          "name": "IVANNA OMEGA SUPREME v2.5.444",
          "body": "REVISOR DE ACTUALIZACIONES EN LA APP.",
          "assets": [
            {"name": "ivanna-omega-supreme-abc123.apk", "browser_download_url": "https://github.com/luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME/releases/download/v2.5.444/ivanna-omega-supreme-abc123.apk", "size": 209783622},
            {"name": "ivanna_omega_supreme_v2.5.444.zip", "browser_download_url": "https://github.com/luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME/releases/download/v2.5.444/ivanna_omega_supreme_v2.5.444.zip", "size": 97144380},
            {"name": "SHA256SUMS.txt", "browser_download_url": "https://github.com/luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME/releases/download/v2.5.444/SHA256SUMS.txt", "size": 200}
          ]
        }"""

        const val SHA256SUMS = "2bc17176a7c07c92ba439285a4ff62541e76de9301431ff67d68dffe08c49722  ivanna_omega_supreme_v2.5.444.zip\n" +
            "f498d502b796043f74127d8f7b62afe0b54ee32d1cbe9681657eeb800e979521  ivanna-omega-supreme-abc123.apk\n"
    }

    // ---------- Comparación de versiones ----------

    @Test fun parseVersionNameStripsV() {
        assertEquals("2.5.444", AppUpdateChecker.parseVersionName("v2.5.444"))
        assertEquals("2.5.444", AppUpdateChecker.parseVersionName("2.5.444"))
        assertEquals("2.5.444", AppUpdateChecker.parseVersionName("  v2.5.444  "))
    }

    @Test fun compareVersionsNumericNotLexicographic() {
        assertTrue(AppUpdateChecker.compareVersions("2.5.443", "2.5.444") < 0)
        assertTrue(AppUpdateChecker.compareVersions("2.5.444", "2.5.443") > 0)
        assertEquals(0, AppUpdateChecker.compareVersions("2.5.444", "2.5.444"))
        assertEquals(0, AppUpdateChecker.compareVersions("v2.5.444", "2.5.444"))
        // Numérico, no lexicográfico: 9 < 10 aunque "9" > "1" como texto.
        assertTrue(AppUpdateChecker.compareVersions("2.5.9", "2.5.10") < 0)
        assertTrue(AppUpdateChecker.compareVersions("2.5.10", "2.5.9") > 0)
        // Distinto número de segmentos: 2.5 == 2.5.0.
        assertEquals(0, AppUpdateChecker.compareVersions("2.5", "2.5.0"))
        assertTrue(AppUpdateChecker.compareVersions("2.5.1", "2.5") > 0)
    }

    @Test fun isNewerOnlyWhenStrictlyGreater() {
        assertTrue(AppUpdateChecker.isNewer("v2.5.444", "2.5.443"))
        assertFalse(AppUpdateChecker.isNewer("v2.5.443", "2.5.443"))
        assertFalse(AppUpdateChecker.isNewer("v2.5.442", "2.5.443"))
    }

    // ---------- Parseo del JSON de GitHub ----------

    @Test fun parseLatestReleaseFindsApkAndSha256() {
        val r = AppUpdateChecker.parseLatestRelease(RELEASE_JSON)
        assertTrue(r != null)
        r!!
        assertEquals("v2.5.444", r.tag)
        assertEquals("2.5.444", r.versionName)
        assertEquals("IVANNA OMEGA SUPREME v2.5.444", r.name)
        assertEquals("ivanna-omega-supreme-abc123.apk", r.apkName)
        assertEquals(
            "https://github.com/luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME/releases/download/v2.5.444/ivanna-omega-supreme-abc123.apk",
            r.apkUrl
        )
        assertEquals(209783622L, r.apkSize)
        assertEquals(
            "https://github.com/luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME/releases/download/v2.5.444/SHA256SUMS.txt",
            r.sha256Url
        )
    }

    @Test fun parseLatestReleaseFailsClosed() {
        // Sin tag.
        assertNull(AppUpdateChecker.parseLatestRelease("""{"name": "x", "assets": []}"""))
        // Sin asset APK (solo zip).
        assertNull(
            AppUpdateChecker.parseLatestRelease(
                """{"tag_name": "v2.5.444", "assets": [{"name": "ivanna_omega_supreme_v2.5.444.zip", "browser_download_url": "https://example.com/z.zip", "size": 1}]}"""
            )
        )
        // Sin assets.
        assertNull(AppUpdateChecker.parseLatestRelease("""{"tag_name": "v2.5.444"}"""))
        // JSON inválido.
        assertNull(AppUpdateChecker.parseLatestRelease("esto no es json"))
        assertNull(AppUpdateChecker.parseLatestRelease(""))
        // APK sin URL de descarga.
        assertNull(
            AppUpdateChecker.parseLatestRelease(
                """{"tag_name": "v2.5.444", "assets": [{"name": "ivanna-omega-supreme-x.apk", "browser_download_url": "", "size": 1}]}"""
            )
        )
    }

    @Test fun parseLatestReleaseWithoutSha256Asset() {
        val json = """{"tag_name": "v2.5.444", "assets": [
          {"name": "ivanna-omega-supreme-abc123.apk", "browser_download_url": "https://example.com/a.apk", "size": 100}
        ]}"""
        val r = AppUpdateChecker.parseLatestRelease(json)
        assertTrue(r != null)
        assertNull(r!!.sha256Url)
    }

    // ---------- SHA256SUMS.txt ----------

    @Test fun parseSha256ForFileStandardFormat() {
        assertEquals(
            "f498d502b796043f74127d8f7b62afe0b54ee32d1cbe9681657eeb800e979521",
            AppUpdateChecker.parseSha256ForFile(SHA256SUMS, "ivanna-omega-supreme-abc123.apk")
        )
        assertEquals(
            "2bc17176a7c07c92ba439285a4ff62541e76de9301431ff67d68dffe08c49722",
            AppUpdateChecker.parseSha256ForFile(SHA256SUMS, "ivanna_omega_supreme_v2.5.444.zip")
        )
    }

    @Test fun parseSha256ForFileBinaryMarkerAndPath() {
        // Modo binario ("*") y nombre con ruta delante.
        val text = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa *./ivanna-omega-supreme-x.apk\n"
        assertEquals(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            AppUpdateChecker.parseSha256ForFile(text, "ivanna-omega-supreme-x.apk")
        )
        // No está.
        assertNull(AppUpdateChecker.parseSha256ForFile(SHA256SUMS, "otro.apk"))
        // Hash corto (no son 64 hex) no vale.
        assertNull(AppUpdateChecker.parseSha256ForFile("abc123  ivanna-omega-supreme-x.apk\n", "ivanna-omega-supreme-x.apk"))
        assertNull(AppUpdateChecker.parseSha256ForFile("", "ivanna-omega-supreme-x.apk"))
    }

    @Test fun sha256HexKnownVectors() {
        // Vectores conocidos de SHA-256.
        assertEquals(
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            AppUpdateChecker.sha256Hex("abc".toByteArray())
        )
        assertEquals(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            AppUpdateChecker.sha256Hex(ByteArray(0))
        )
    }

    // ---------- Verificación de descarga ----------

    @Test fun verifyDownloadSizeAndHash() {
        val sha = "f498d502b796043f74127d8f7b62afe0b54ee32d1cbe9681657eeb800e979521"
        // Tamaño + hash correctos.
        assertTrue(AppUpdateChecker.verifyDownload(100L, 100L, sha, sha))
        // Tamaño distinto.
        assertFalse(AppUpdateChecker.verifyDownload(99L, 100L, sha, sha))
        // Hash distinto.
        assertFalse(AppUpdateChecker.verifyDownload(100L, 100L, "00$sha".take(64), sha))
        // Sin SHA esperado: basta el tamaño.
        assertTrue(AppUpdateChecker.verifyDownload(100L, 100L, null, null))
        assertTrue(AppUpdateChecker.verifyDownload(100L, 100L, sha, null))
        // SHA esperado pero no se pudo calcular el real.
        assertFalse(AppUpdateChecker.verifyDownload(100L, 100L, null, sha))
        // Tamaño esperado desconocido (0): no se exige.
        assertTrue(AppUpdateChecker.verifyDownload(12345L, 0L, null, null))
        // El hash no distingue mayúsculas.
        assertTrue(AppUpdateChecker.verifyDownload(100L, 100L, sha.uppercase(), sha))
    }

    // ---------- Cadencia de revisión ----------

    @Test fun shouldCheckOnceADayAndRespectsLater() {
        val day = AppUpdateChecker.CHECK_INTERVAL_MS
        val now = 1_000_000_000_000L
        // Nunca se ha revisado: sí.
        assertTrue(AppUpdateChecker.shouldCheck(now, 0L, 0L))
        // Se revisó hace 1 hora: no.
        assertFalse(AppUpdateChecker.shouldCheck(now, now - 3_600_000L, 0L))
        // Se revisó hace 25 horas: sí.
        assertTrue(AppUpdateChecker.shouldCheck(now, now - 25 * 3_600_000L, 0L))
        // "Más tarde" hasta dentro de 1 hora: no, aunque toque revisar.
        assertFalse(AppUpdateChecker.shouldCheck(now, 0L, now + 3_600_000L))
        // "Más tarde" ya vencido: sí.
        assertTrue(AppUpdateChecker.shouldCheck(now, 0L, now - 1L))
    }
}
