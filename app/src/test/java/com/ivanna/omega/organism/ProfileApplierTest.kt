package com.ivanna.omega.organism

import com.google.gson.JsonParser
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * Tests de la validación fail-closed (C2, release 510).
 * Puro JVM: la validación no toca Android; la cuarentena vive en un
 * directorio temporal. La compilación/ejecución queda para la CI
 * (Gradle no corre en esta VM: conexión cliente→daemon bloqueada).
 */
class ProfileApplierTest {

    private lateinit var dir: File
    private lateinit var applier: ProfileApplier

    @Before
    fun setup() {
        dir = Files.createTempDirectory("profile-applier-test").toFile()
        applier = ProfileApplier(dir)
    }

    @After
    fun teardown() {
        dir.deleteRecursively()
    }

    private fun valid(): ProfileApplier.ValidationResult.Valid {
        val result = ProfileApplier.validate(V11_JSON, version = 2L, appliedVersion = 1L)
        assertTrue("v1.1 debe validar, motivos: ${(result as? ProfileApplier.ValidationResult.Invalid)?.reasons}", result is ProfileApplier.ValidationResult.Valid)
        return result as ProfileApplier.ValidationResult.Valid
    }

    private fun invalidReasons(result: ProfileApplier.ValidationResult): List<String> {
        assertTrue("se esperaba Invalid, fue $result", result is ProfileApplier.ValidationResult.Invalid)
        return (result as ProfileApplier.ValidationResult.Invalid).reasons
    }

    @Test
    fun `perfil v1-1 valido pasa`() {
        val result = valid()
        assertEquals(0.0, result.profile.computedNetGainDb, 1e-9)
        assertEquals(-2.0, result.profile.eqBandsDb.low60_120, 1e-9)
        assertEquals(2.0, result.profile.eqBandsDb.mid500_1k, 1e-9)
        assertEquals(-4.0, result.profile.trimDb, 1e-9)
        assertTrue(result.profile.liveSession)
        assertEquals(0.0, result.profile.meanCoherence, 1e-9)
        assertEquals(10.46, result.profile.spectralErrorRmsDb, 1e-9)
    }

    @Test
    fun `EQ mas-2x4 y trim 0 se rechaza por ganancia neta mas-8 dB`() {
        // v1.1 tiene low -2.0: subirlo a +2.0 deja los 4 slots en +2 (+8.0)
        // y con trim 0.0 la ganancia neta es +8.0 dB (caso del §C2.d).
        val body = V11_JSON
            .replace("\"low_60_120\":-2.0", "\"low_60_120\":2.0")
            .replace("\"trim_db\":-4.0", "\"trim_db\":0.0")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("ganancia neta") && it.contains("8.0") })
    }

    @Test
    fun `NaN se rechaza`() {
        val body = V11_JSON.replace("\"orbe_amount\":0.53", "\"orbe_amount\":NaN")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("no finito") })
    }

    @Test
    fun `Infinity por desbordamiento 1e999 se rechaza`() {
        val body = V11_JSON.replace("\"trim_db\":-4.0", "\"trim_db\":1e999")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("no finito") })
    }

    @Test
    fun `formato incorrecto se rechaza`() {
        val body = V11_JSON.replace("\"format\":\"IVANNA OMEGA\"", "\"format\":\"IVANNA\"")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("format") })
    }

    @Test
    fun `format_version distinto de 1-0 se rechaza`() {
        val body = V11_JSON.replace("\"format_version\":\"1.0\"", "\"format_version\":\"2.0\"")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("format_version") })
    }

    @Test
    fun `version menor sin rollback se rechaza`() {
        val reasons = invalidReasons(ProfileApplier.validate(V11_JSON, version = 1L, appliedVersion = 2L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("menor que la aplicada") })
    }

    @Test
    fun `version igual se ignora como no mas nueva`() {
        val reasons = invalidReasons(ProfileApplier.validate(V11_JSON, version = 2L, appliedVersion = 2L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("ya aplicada") })
    }

    @Test
    fun `rollback explicito permite version menor`() {
        val result = ProfileApplier.validate(V11_JSON, version = 1L, appliedVersion = 2L, rolledBack = true)
        assertTrue("rollback explícito debe validar, fue $result", result is ProfileApplier.ValidationResult.Valid)
    }

    @Test
    fun `campo prohibido token se rechaza`() {
        val body = V11_JSON.replace(
            "\"format_version\":\"1.0\"",
            "\"format_version\":\"1.0\",\"token\":\"abc123\""
        )
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("prohibido") })
    }

    @Test
    fun `campo desconocido en engine se rechaza`() {
        val body = V11_JSON.replace("\"trim_db\":-4.0", "\"trim_db\":-4.0,\"red\":\"x\"")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("desconocida") })
    }

    @Test
    fun `slot EQ fuera de rango se rechaza`() {
        val body = V11_JSON.replace("\"low_60_120\":-2.0", "\"low_60_120\":2.5")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("low_60_120") && it.contains("fuera de") })
    }

    @Test
    fun `duration 0 con source de archivo se rechaza`() {
        val body = V11_JSON.replace(
            "\"source\":\"MuseLink omega-1795857152818-9lec (microfono ERB, 177 sesiones, 2026-11-28 04:09 CST) — CANDIDATO v1.1, NO APLICADO\"",
            "\"source\":\"Archivo FLAC /sdcard/Music/x.flac\""
        )
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("duration_s") })
    }

    @Test
    fun `salto de operador mayor de 0-2 por version se rechaza`() {
        val previous = valid().profile
        val body = V11_JSON.replace("\"orbe_amount\":0.53", "\"orbe_amount\":0.9")
        val reasons = invalidReasons(
            ProfileApplier.validate(body, version = 3L, appliedVersion = 2L, previous = previous)
        )
        assertTrue("motivos: $reasons", reasons.any { it.contains("orbe_amount") && it.contains("salto") })
    }

    @Test
    fun `salto de operador dentro de 0-2 se admite`() {
        val previous = valid().profile
        val body = V11_JSON.replace("\"orbe_amount\":0.53", "\"orbe_amount\":0.7")
        val result = ProfileApplier.validate(body, version = 3L, appliedVersion = 2L, previous = previous)
        assertTrue("salto 0.17 debe validar, fue $result", result is ProfileApplier.ValidationResult.Valid)
    }

    @Test
    fun `body mayor de 64 KB se rechaza`() {
        val body = "x".repeat(65 * 1024)
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("64 KB") })
    }

    @Test
    fun `sin fingerprint ERB se rechaza`() {
        val body = V11_JSON.replace("\"mean_coherence\":0.0,", "")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("mean_coherence") })
    }

    @Test
    fun `sin detected_issues se rechaza`() {
        val obj = JsonParser.parseString(V11_JSON).asJsonObject
        obj.getAsJsonObject("fingerprint").remove("detected_issues")
        val reasons = invalidReasons(ProfileApplier.validate(obj.toString(), version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("detected_issues") })
    }

    @Test
    fun `apply pone en cuarentena el perfil invalido y deja intacto el estable`() {
        val badBody = V11_JSON.replace("\"trim_db\":-4.0", "\"trim_db\":0.0")
        val stable = File(dir, ProfileApplier.STABLE_FILE_NAME)
        stable.writeText("perfil-bueno")
        val result = applier.apply(badBody, version = 2L, appliedVersion = 1L)
        assertTrue("se esperaba Rejected, fue $result", result is ProfileApplier.ApplyResult.Rejected)
        val rejected = result as ProfileApplier.ApplyResult.Rejected
        assertTrue(rejected.quarantineFile.isFile)
        assertTrue(rejected.quarantineFile.readText().contains("\"trim_db\":0.0"))
        assertEquals("perfil-bueno", stable.readText())
        val status = applier.readQuarantineStatus()
        assertTrue(status != null)
        assertEquals(2L, status!!.version)
        assertEquals(rejected.quarantineFile.name, status.fileName)
        assertTrue(status.reasons.any { it.contains("ganancia neta") })
    }

    @Test
    fun `apply con perfil valido no escribe cuarentena`() {
        val result = applier.apply(V11_JSON, version = 2L, appliedVersion = 1L)
        assertTrue("se esperaba Validated, fue $result", result is ProfileApplier.ApplyResult.Validated)
        assertEquals(null, applier.readQuarantineStatus())
        assertTrue(dir.listFiles()?.none { it.name.contains("quarantine") } ?: true)
    }

    companion object {
        /** v1.1 candidata, verbatim del archivo entregado (sin modificar). */
        private const val V11_JSON = """{"artist":"desconocido","derivation":{"compressor":"Sin cambio propuesto en la v1.1. Se heredan los valores del perfil vivo v1.0 (threshold -12.0 dB, ratio 2.0, attack 10.0 ms, release 120.0 ms).","eq":"Corrección v1.1 propuesta (Muse IA, 2026-11-28 04:00, 84 sesiones): subgraves +1, graves -3, medios-graves -2, medios +4, presencia +9, brillo +9 dB. Mapeo a los 4 slots del motor v1.0 con clamp ±2 dB: low_60_120 -2.0 (propuesto -3, residual -1.0), mid_500_1k +2.0 (propuesto +4, residual +2.0), high_2k_4k +2.0 (brillo, propuesto +9, residual +7.0), presence_4k_8k +2.0 (presencia, propuesto +9, residual +7.0). Sin slot en el motor (quedan aquí, no se aplican): subgraves +1.0 y medios-graves -2.0. Neto de los 4 slots +4.0 dB; trim -4.0 dB lo compensa para ganancia neta 0.0 dB (Regla de Oro Matched-LUFS). El residual (+7 dB en presencia/brillo) no lo puede aplicar el motor v1.0 en un solo perfil: se cierra por evoluciones sucesivas validadas por A/B o por una ampliación del esquema.","loudness":"Sin medición LUFS en sesión en vivo (micrófono ERB, no FLAC): no hay loudness_lufs_integrated, peak ni crest medidos. La ganancia neta es 0.0 dB por construcción (trim -4.0 dB compensa el neto +4.0 dB de los slots EQ). Los campos de medición de archivo (loudness, peak, crest, espectro por banda, estéreo) se rellenan desde el perfil v1.0 del servidor al aplicar.","note":"CANDIDATO v1.1, NO APLICADO. Pendiente de validación A/B automática: 3 sesiones idénticas vs v1.0, mejora ERB mínima 20% sin perder coherencia, rollback automático si empeora. Operadores sin cambios (Orbe 0.53, Holos 0.34, Neuro 0.35, SalaViva 0.30; SalaViva sin slot en el engine v1.0, se guarda como nota). Coherencia media 0%: el A/B corre con la guarda de coherencia (ver especificación de refactorización). Fingerprint ERB (no FLAC): solo se incluye el dato verificado (ERB-3 209 Hz, +9.86 dB); la tabla completa de 24 bandas vive en el servidor. duration_s 0.0 = sesión en vivo continua sin duración de pista única. artist 'desconocido' = las sesiones no traen artista.","structural_split":"Sin medición HPSS en sesión en vivo (micrófono ERB, no FLAC). Se heredan los valores del compilador de sesión en vivo (transitorio 0.0, tonal 1.0)."},"duration_s":0.0,"engine":{"compressor":{"attack_ms":10.0,"ratio":2.0,"release_ms":120.0,"threshold_db":-12.0},"eq_bands_db":{"high_2k_4k":2.0,"low_60_120":-2.0,"mid_500_1k":2.0,"presence_4k_8k":2.0},"exciter_amount":0.3,"holos_amount":0.34,"net_gain_db":0.0,"orbe_amount":0.53,"orbe_distance_m":0.35,"phi_amount":0.35,"structural_split":{"tonal_amount":1.0,"tonal_energy_ratio":1.0,"transient_amount":0.55,"transient_energy_ratio":0.0},"trim_db":-4.0,"volterra_enabled":true,"volterra_mix":0.55},"fingerprint":{"detected_issues":["Resonancia ERB-3 209 Hz: +9.86 dB ERB","Caída de -7 a -24 dB entre 1.4 kHz y 14.7 kHz (déficit en medios-altos/agudos)","Exceso en graves/medios-graves","Coherencia media ERB 0%: posible cancelación de fase o filtro peine (sala 4.0x7.0 m, techo 3.0 m, posible pared a ~2.0 m)","Fidelidad 30/100; confianza del Gemelo 10%"],"erb_bands_measured":{"erb3_209hz":{"coherence":0.0,"error_db":9.86}},"mean_coherence":0.0,"spectral_error_rms_db":10.46},"format":"IVANNA OMEGA","format_version":"1.0","source":"MuseLink omega-1795857152818-9lec (microfono ERB, 177 sesiones, 2026-11-28 04:09 CST) — CANDIDATO v1.1, NO APLICADO","track":"Sesión en vivo IVANNA"}"""
    }
}
