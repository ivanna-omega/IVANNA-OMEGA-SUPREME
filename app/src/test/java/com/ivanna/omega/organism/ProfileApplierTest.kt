package com.ivanna.omega.organism

import com.google.gson.JsonParser
import com.ivanna.omega.audio.IvannaAntiDolbyParams
import com.ivanna.omega.audio.IvannaAudioEngineParams
import com.ivanna.omega.audio.IvannaAudioProfile
import com.ivanna.omega.audio.IvannaNeuromorphicParams
import com.ivanna.omega.audio.IvannaRouteParams
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
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
        // S9: la v1.1 es el primer candidato de medición del A/B
        // (coherencia 0.0); pasa la guarda solo por la marca.
        assertTrue(result.profile.abMeasurementOnly)
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

    // ------------------------------------------------------------------
    // C8: diario + estable verificado + rollback.
    // ------------------------------------------------------------------

    private fun installOk(profile: ValidatedProfile): Boolean = true

    @Test
    fun `applyAndCommit con read-back ok commitea y promueve el estable`() {
        val result = applier.applyAndCommit(
            V11_JSON, version = 2L, appliedVersion = 1L,
            install = ::installOk,
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        assertTrue("se esperaba Committed, fue $result", result is ProfileApplier.CommitResult.Committed)
        val journal = applier.readJournal()
        assertTrue(journal != null)
        assertEquals(2L, journal!!.version)
        assertEquals(ProfileApplier.JournalState.COMMITTED, journal.estado)
        assertEquals(ProfileApplier.sha256Hex(V11_JSON), journal.hashSha256)
        val stable = File(dir, ProfileApplier.STABLE_FILE_NAME)
        assertTrue(stable.isFile)
        assertEquals(V11_JSON, stable.readText())
        assertTrue("el estable debe verificar su hash", applier.verifyStableHash())
        assertEquals(V11_JSON, applier.readStableBody())
    }

    @Test
    fun `read-back distinto deja PENDING y no promueve estable`() {
        val result = applier.applyAndCommit(
            V11_JSON, version = 2L, appliedVersion = 1L,
            install = ::installOk,
            readAppliedProfileId = { "rock_70s" }
        )
        assertTrue("se esperaba NotCommitted, fue $result", result is ProfileApplier.CommitResult.NotCommitted)
        assertEquals(ProfileApplier.JournalState.PENDING, applier.readJournal()!!.estado)
        assertFalse("sin read-back no hay estable", File(dir, ProfileApplier.STABLE_FILE_NAME).exists())
    }

    @Test
    fun `install que falla deja PENDING sin estable`() {
        val result = applier.applyAndCommit(
            V11_JSON, version = 2L, appliedVersion = 1L,
            install = { false },
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        assertTrue("se esperaba NotCommitted, fue $result", result is ProfileApplier.CommitResult.NotCommitted)
        assertEquals(ProfileApplier.JournalState.PENDING, applier.readJournal()!!.estado)
        assertFalse(File(dir, ProfileApplier.STABLE_FILE_NAME).exists())
        // El candidato sí quedó escrito (es lo que se estaba instalando).
        assertEquals(V11_JSON, File(dir, ProfileApplier.candidateFileName(2L)).readText())
    }

    @Test
    fun `applyAndCommit invalido va a cuarentena sin diario`() {
        val badBody = V11_JSON.replace("\"trim_db\":-4.0", "\"trim_db\":0.0")
        val result = applier.applyAndCommit(
            badBody, version = 2L, appliedVersion = 1L,
            install = ::installOk,
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        assertTrue("se esperaba Rejected, fue $result", result is ProfileApplier.CommitResult.Rejected)
        assertEquals(null, applier.readJournal())
    }

    @Test
    fun `diario PENDING al arrancar restaura el estable`() {
        assertTrue(applier.promoteStable(V11_JSON, 1L))
        applier.installCandidate(V11_JSON, 2L)
        // Candidato a medias (la app murió durante la instalación).
        File(dir, ProfileApplier.candidateFileName(2L)).writeText("a-medias")
        val check = applier.checkPendingRollback()
        assertTrue("se esperaba Ready, fue $check", check is ProfileApplier.RollbackCheck.Ready)
        assertEquals(V11_JSON, (check as ProfileApplier.RollbackCheck.Ready).stableBody)
        assertTrue(applier.restoreStableCandidate())
        assertEquals("el candidato debe volver a ser el estable", V11_JSON, File(dir, ProfileApplier.candidateFileName(2L)).readText())
        // El diario sigue PENDING hasta que T1 re-aplique con read-back.
        assertEquals(ProfileApplier.JournalState.PENDING, applier.readJournal()!!.estado)
    }

    @Test
    fun `hash del estable distinto no restaura`() {
        assertTrue(applier.promoteStable(V11_JSON, 1L))
        applier.installCandidate(V11_JSON, 2L)
        File(dir, ProfileApplier.STABLE_FILE_NAME).appendText("x")
        val check = applier.checkPendingRollback()
        assertTrue("se esperaba StableHashMismatch, fue $check", check is ProfileApplier.RollbackCheck.StableHashMismatch)
        assertFalse(applier.restoreStableCandidate())
        assertEquals("el candidato no se toca", V11_JSON, File(dir, ProfileApplier.candidateFileName(2L)).readText())
        assertEquals(null, applier.readStableBody())
    }

    @Test
    fun `diario PENDING sin estable no restaura`() {
        applier.installCandidate(V11_JSON, 2L)
        val check = applier.checkPendingRollback()
        assertTrue("se esperaba StableMissing, fue $check", check is ProfileApplier.RollbackCheck.StableMissing)
        assertFalse(applier.restoreStableCandidate())
    }

    @Test
    fun `sin diario pendiente no hay nada que restaurar`() {
        assertTrue(applier.checkPendingRollback() is ProfileApplier.RollbackCheck.None)
        assertFalse(applier.restoreStableCandidate())
        assertTrue(applier.promoteStable(V11_JSON, 1L))
        applier.writeJournal(
            ProfileApplier.ApplyJournal(1L, ProfileApplier.sha256Hex(V11_JSON), ProfileApplier.JournalState.COMMITTED, 0L)
        )
        assertTrue(applier.checkPendingRollback() is ProfileApplier.RollbackCheck.None)
    }

    @Test
    fun `tope de archivos solo estable mas candidato vigente mas diario`() {
        assertTrue(applier.promoteStable(V11_JSON, 1L))
        applier.installCandidate(V11_JSON, 2L)
        applier.installCandidate(V11_JSON, 3L)
        applier.installCandidate(V11_JSON, 4L)
        val names = dir.listFiles()!!.map { it.name }.toSet()
        assertFalse(names.contains(ProfileApplier.candidateFileName(2L)))
        assertFalse(names.contains(ProfileApplier.candidateFileName(3L)))
        assertTrue(names.contains(ProfileApplier.candidateFileName(4L)))
        assertTrue(names.contains(ProfileApplier.STABLE_FILE_NAME))
        assertTrue(names.contains(ProfileApplier.JOURNAL_FILE_NAME))
    }

    @Test
    fun `C22 borrar candidatos no toca estable ni diario ni cuarentena`() {
        assertTrue(applier.promoteStable(V11_JSON, 1L))
        applier.installCandidate(V11_JSON, 2L)
        // Candidatos acumulados (p. ej. de versiones viejas o de un corte
        // antes del prune): se escriben directo para simular el montón.
        File(dir, ProfileApplier.candidateFileName(3L)).writeText(V11_JSON)
        File(dir, ProfileApplier.candidateFileName(4L)).writeText(V11_JSON)
        File(dir, "${ProfileApplier.candidateFileName(4L)}.tmp").writeText("{basura")
        val quarantined = applier.quarantine(V11_JSON, 9L, listOf("motivo de prueba"))
        assertEquals("deben borrarse los 3 candidatos (2, 3 y 4)", 3, applier.deleteCandidates())
        val names = dir.listFiles()!!.map { it.name }.toSet()
        assertFalse(names.contains(ProfileApplier.candidateFileName(2L)))
        assertFalse(names.contains(ProfileApplier.candidateFileName(3L)))
        assertFalse(names.contains(ProfileApplier.candidateFileName(4L)))
        assertFalse(names.contains("${ProfileApplier.candidateFileName(4L)}.tmp"))
        assertTrue(names.contains(ProfileApplier.STABLE_FILE_NAME))
        assertTrue(names.contains(ProfileApplier.STABLE_HASH_FILE_NAME))
        assertTrue(names.contains(ProfileApplier.JOURNAL_FILE_NAME))
        assertTrue(names.contains(quarantined.name))
        assertTrue(names.contains(ProfileApplier.QUARANTINE_STATUS_FILE_NAME))
        assertEquals("el estable sigue verificando", V11_JSON, applier.readStableBody())
    }

    @Test
    fun `C22 borrar estable borra estable mas sidecar mas diario y conserva candidatos`() {
        assertTrue(applier.promoteStable(V11_JSON, 1L))
        applier.installCandidate(V11_JSON, 2L)
        assertTrue(applier.deleteStable())
        val names = dir.listFiles()!!.map { it.name }.toSet()
        assertFalse(names.contains(ProfileApplier.STABLE_FILE_NAME))
        assertFalse(names.contains(ProfileApplier.STABLE_HASH_FILE_NAME))
        assertFalse(names.contains(ProfileApplier.JOURNAL_FILE_NAME))
        assertTrue("el candidato no se toca (lo borra clearQueue o el prune)", names.contains(ProfileApplier.candidateFileName(2L)))
        assertFalse("sin estable ya no hay nada que borrar", applier.deleteStable())
    }

    @Test
    fun `C22 borrar estable sin estable devuelve false`() {
        assertFalse(applier.deleteStable())
        applier.installCandidate(V11_JSON, 2L)
        assertFalse(applier.deleteStable())
    }

    @Test
    fun `corte simulado tmp huerfano no reemplaza al candidato`() {
        applier.installCandidate(V11_JSON, 2L)
        File(dir, "${ProfileApplier.candidateFileName(2L)}.tmp").writeText("{basura")
        assertEquals(V11_JSON, File(dir, ProfileApplier.candidateFileName(2L)).readText())
        assertEquals(ProfileApplier.JournalState.PENDING, applier.readJournal()!!.estado)
    }

    @Test
    fun `completeRollback marca ROLLED_BACK solo con version y PENDING`() {
        applier.installCandidate(V11_JSON, 2L)
        applier.completeRollback(99L)
        assertEquals(ProfileApplier.JournalState.PENDING, applier.readJournal()!!.estado)
        applier.completeRollback(2L)
        assertEquals(ProfileApplier.JournalState.ROLLED_BACK, applier.readJournal()!!.estado)
        assertTrue(applier.checkPendingRollback() is ProfileApplier.RollbackCheck.None)
    }

    // ------------------------------------------------------------------
    // S9: guarda de coherencia antes de aplicar EQ.
    // ------------------------------------------------------------------

    /** Quita la marca de solo-medición de un cuerpo (la v1.1 la trae). */
    private fun withoutMeasurementOnly(body: String): String =
        body.replace("\"ab_measurement_only\":true,", "")

    private fun withCoherence(body: String, coherence: Double): String =
        body.replace("\"mean_coherence\":0.0", "\"mean_coherence\":$coherence")

    @Test
    fun `coherencia 0 sin marca se rechaza por coherencia`() {
        val body = withoutMeasurementOnly(V11_JSON)
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("mean_coherence") && it.contains("coherencia media") })
    }

    @Test
    fun `coherencia 0 sin marca va a cuarentena por el camino existente`() {
        val body = withoutMeasurementOnly(V11_JSON)
        val result = applier.apply(body, version = 2L, appliedVersion = 1L)
        assertTrue("se esperaba Rejected, fue $result", result is ProfileApplier.ApplyResult.Rejected)
        val rejected = result as ProfileApplier.ApplyResult.Rejected
        assertTrue(rejected.quarantineFile.isFile)
        assertTrue(rejected.quarantineFile.readText().contains("\"mean_coherence\":0.0"))
        val status = applier.readQuarantineStatus()
        assertTrue(status != null)
        assertEquals(2L, status!!.version)
        assertTrue(status.reasons.any { it.contains("coherencia media") })
    }

    @Test
    fun `coherencia 0 con marca solo-medicion pasa la guarda`() {
        // La v1.1 trae la marca: es el primer candidato de medición del A/B.
        val result = ProfileApplier.validate(V11_JSON, version = 2L, appliedVersion = 1L)
        assertTrue("con ab_measurement_only debe validar, fue $result", result is ProfileApplier.ValidationResult.Valid)
        assertTrue((result as ProfileApplier.ValidationResult.Valid).profile.abMeasurementOnly)
    }

    @Test
    fun `coherencia 0-25 sin marca pasa`() {
        val body = withCoherence(withoutMeasurementOnly(V11_JSON), 0.25)
        val result = ProfileApplier.validate(body, version = 2L, appliedVersion = 1L)
        assertTrue("coherencia 0.25 debe validar, fue $result", result is ProfileApplier.ValidationResult.Valid)
        assertFalse((result as ProfileApplier.ValidationResult.Valid).profile.abMeasurementOnly)
    }

    @Test
    fun `coherencia 0-20 sin marca pasa (umbral inclusivo)`() {
        val body = withCoherence(withoutMeasurementOnly(V11_JSON), 0.20)
        val result = ProfileApplier.validate(body, version = 2L, appliedVersion = 1L)
        assertTrue("coherencia 0.20 debe validar (no es < 0.20), fue $result", result is ProfileApplier.ValidationResult.Valid)
    }

    @Test
    fun `coherencia 0-19 sin marca se rechaza`() {
        val body = withCoherence(withoutMeasurementOnly(V11_JSON), 0.19)
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("mean_coherence") && it.contains("0.19") })
    }

    @Test
    fun `coherencia 0-19 con marca pasa como solo-medicion`() {
        val body = withCoherence(V11_JSON, 0.19)
        val result = ProfileApplier.validate(body, version = 2L, appliedVersion = 1L)
        assertTrue("con marca debe validar, fue $result", result is ProfileApplier.ValidationResult.Valid)
        assertTrue((result as ProfileApplier.ValidationResult.Valid).profile.abMeasurementOnly)
    }

    @Test
    fun `ab_measurement_only no booleano se rechaza`() {
        val body = V11_JSON.replace("\"ab_measurement_only\":true", "\"ab_measurement_only\":\"si\"")
        val reasons = invalidReasons(ProfileApplier.validate(body, version = 2L, appliedVersion = 1L))
        assertTrue("motivos: $reasons", reasons.any { it.contains("ab_measurement_only") && it.contains("boolean") })
    }

    @Test
    fun `sha256Hex es estable y distingue cuerpos`() {
        val h1 = ProfileApplier.sha256Hex(V11_JSON)
        assertEquals(64, h1.length)
        assertEquals(h1, ProfileApplier.sha256Hex(V11_JSON))
        assertTrue(h1 != ProfileApplier.sha256Hex("$V11_JSON "))
    }

    // T1/510 — traducción ValidatedProfile → IvannaAudioProfile.

    private fun baseProfile(): IvannaAudioProfile = IvannaAudioProfile(
        id = "rock_70s",
        name = "Rock 70s",
        description = "base",
        category = "rock",
        priority = 1,
        audioEngine = IvannaAudioEngineParams(gain = 0.5f, exciterAmount = 0.3f, eqGain = 0.0f, widthAmount = 1.0f),
        antiDolby = IvannaAntiDolbyParams(speechThreshold = 0.33f, bassThreshold = 0.33f, eqBoost2k4k = 0.0f, widenerMultiplier = 1.0f),
        neuromorphic = IvannaNeuromorphicParams(harmonicGain = 0.3f, lateralInhibition = 0.5f, ohcCompression = 0.4f, masterGainDb = 0.0f),
        route = IvannaRouteParams(bassBoostDb = 0.0f, dialogBoostDb = 0.0f, widenerMult = 1.0f),
        tags = listOf("rock"),
        recommendedFor = "test"
    )

    @Test
    fun `traduccion escribe el EQ en ambos destinos con el mismo valor sin dividir entre 2`() {
        val validated = valid().profile
        val ivanna = ProfileApplier.translate(validated, status = "stable", profileId = "omega-1795857152818-9lec", base = baseProfile())
        // v1.1: low -2.0, mid/high/presence +2.0. El mismo valor en el
        // destino legacy (audioEngine) y en el fusionado (tonal): NO es
        // la mitad en cada uno (eso sería -1.0/+1.0).
        assertEquals(-2.0f, ivanna.audioEngine.eqLowDb!!)
        assertEquals(-2.0f, ivanna.tonal.lowDb!!)
        assertEquals(2.0f, ivanna.audioEngine.eqMidDb!!)
        assertEquals(2.0f, ivanna.tonal.midDb!!)
        assertEquals(2.0f, ivanna.audioEngine.eqHighDb!!)
        assertEquals(2.0f, ivanna.tonal.highDb!!)
        assertEquals(2.0f, ivanna.audioEngine.eqPresenceDb!!)
        assertEquals(2.0f, ivanna.tonal.presenceDb!!)
        assertEquals(ivanna.audioEngine.eqLowDb, ivanna.tonal.lowDb)
        assertEquals(ivanna.audioEngine.eqMidDb, ivanna.tonal.midDb)
        assertEquals(ivanna.audioEngine.eqHighDb, ivanna.tonal.highDb)
        assertEquals(ivanna.audioEngine.eqPresenceDb, ivanna.tonal.presenceDb)
    }

    @Test
    fun `traduccion fija muselink-live la sala de Esse y hereda lo no cubierto`() {
        val validated = valid().profile
        val base = baseProfile()
        val ivanna = ProfileApplier.translate(validated, status = "stable", profileId = "omega-1795857152818-9lec", base = base)
        assertEquals(ProfileApplier.LIVE_PROFILE_ID, ivanna.id)
        assertTrue(ivanna.name.contains("Sesión en vivo IVANNA"))
        assertTrue(ivanna.name.contains("v2"))
        assertTrue(ivanna.description.contains("omega-1795857152818-9lec"))
        assertEquals("Sesión en vivo IVANNA", ivanna.referenceTrack)
        assertEquals(0.0f, ivanna.targetLufsDeltaDb)
        // Sala fija de la VUELTA (4.0x7.0 m, escucha 3 m).
        assertEquals(0.34f, ivanna.sala.rt60Sec)
        assertEquals(7.0f, ivanna.sala.roomSizeM)
        assertEquals(0.22f, ivanna.sala.roomWet)
        // Operadores y compresor del perfil.
        assertEquals(0.53f, ivanna.spatial.orbeAmount)
        assertEquals(0.35f, ivanna.spatial.orbeDistanceM)
        assertEquals(0.34f, ivanna.field.holosAmount)
        assertEquals(0.35f, ivanna.field.phiAmount)
        assertEquals(0.3f, ivanna.audioEngine.exciterAmount)
        assertEquals(-4.0f, ivanna.tonal.trimDb!!)
        assertEquals(-12.0f, ivanna.dynamics.compThresholdDb!!)
        assertEquals(2.0f, ivanna.dynamics.compRatio!!)
        assertEquals(1.0f, ivanna.volterra.correction) // volterra_enabled=true → on
        // Herencia de la base: nada inventado.
        assertEquals(0.5f, ivanna.audioEngine.gain)
        assertEquals(1.0f, ivanna.audioEngine.widthAmount)
        assertEquals(0.33f, ivanna.antiDolby.speechThreshold)
        assertEquals(0.3f, ivanna.neuromorphic.harmonicGain)
        assertEquals(0.0f, ivanna.route.bassBoostDb)
    }

    @Test
    fun `volterra apagado traduce correction a 0`() {
        val body = V11_JSON.replace("\"volterra_enabled\":true", "\"volterra_enabled\":false")
        val result = ProfileApplier.validate(body, version = 2L, appliedVersion = 1L)
        assertTrue("debe validar, motivos: ${(result as? ProfileApplier.ValidationResult.Invalid)?.reasons}", result is ProfileApplier.ValidationResult.Valid)
        val ivanna = ProfileApplier.translate(
            (result as ProfileApplier.ValidationResult.Valid).profile,
            status = "stable", profileId = "omega-1", base = baseProfile()
        )
        assertEquals(0.0f, ivanna.volterra.correction)
    }

    @Test
    fun `traduccion sin profileId omite el id de la descripcion (rollback)`() {
        val validated = valid().profile
        val ivanna = ProfileApplier.translate(validated, status = "rolled_back", profileId = null, base = baseProfile())
        assertEquals(ProfileApplier.LIVE_PROFILE_ID, ivanna.id)
        assertFalse(ivanna.description.contains("omega-"))
        assertTrue(ivanna.description.contains("rolled_back"))
    }

    // T1/510 — diario: el candidato commitea sin promover el estable.

    @Test
    fun `applyAndCommit de candidato no promueve el estable pero registra el diario`() {
        val result = applier.applyAndCommit(
            V11_JSON, version = 2L, appliedVersion = 1L,
            profileId = "omega-1795857152818-9lec",
            promoteToStable = false,
            install = ::installOk,
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        assertTrue("se esperaba Committed, fue $result", result is ProfileApplier.CommitResult.Committed)
        assertFalse("el candidato no es estable", File(dir, ProfileApplier.STABLE_FILE_NAME).exists())
        assertEquals(null, applier.readStableBody())
        val journal = applier.readJournal()!!
        assertEquals(2L, journal.version)
        assertEquals(ProfileApplier.JournalState.COMMITTED, journal.estado)
        assertEquals("omega-1795857152818-9lec", journal.profileId)
        assertEquals(null, journal.stableVersion)
    }

    @Test
    fun `applyAndCommit estable promueve y registra stableVersion en el diario`() {
        val result = applier.applyAndCommit(
            V11_JSON, version = 2L, appliedVersion = 1L,
            profileId = "omega-1795857152818-9lec",
            install = ::installOk,
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        assertTrue("se esperaba Committed, fue $result", result is ProfileApplier.CommitResult.Committed)
        assertEquals(V11_JSON, applier.readStableBody())
        val journal = applier.readJournal()!!
        assertEquals(2L, journal.version)
        assertEquals("omega-1795857152818-9lec", journal.profileId)
        assertEquals(2L, journal.stableVersion)
    }

    @Test
    fun `candidato tras estable conserva la stableVersion anterior en el diario`() {
        applier.applyAndCommit(
            V11_JSON, version = 2L, appliedVersion = 1L,
            profileId = "omega-1",
            install = ::installOk,
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        val v3 = V11_JSON.replace("\"orbe_amount\":0.53", "\"orbe_amount\":0.7")
        val result = applier.applyAndCommit(
            v3, version = 3L, appliedVersion = 2L,
            profileId = "omega-1",
            promoteToStable = false,
            install = ::installOk,
            readAppliedProfileId = { ProfileApplier.LIVE_PROFILE_ID }
        )
        assertTrue("se esperaba Committed, fue $result", result is ProfileApplier.CommitResult.Committed)
        // El estable sigue siendo la v2 (no la candidata v3).
        assertEquals(V11_JSON, applier.readStableBody())
        assertEquals(2L, applier.readJournal()!!.stableVersion)
        assertEquals(3L, applier.readJournal()!!.version)
    }

    @Test
    fun `diario viejo sin profileId ni stableVersion se lee con defaults`() {
        applier.writeJournal(
            ProfileApplier.ApplyJournal(1L, ProfileApplier.sha256Hex(V11_JSON), ProfileApplier.JournalState.COMMITTED, 0L)
        )
        // Reescribir sin los campos nuevos, como lo haría el código C8.
        File(dir, ProfileApplier.JOURNAL_FILE_NAME).writeText(
            """{"version":1,"hashSha256":"${ProfileApplier.sha256Hex(V11_JSON)}","estado":"COMMITTED","atMs":0}"""
        )
        val journal = applier.readJournal()!!
        assertEquals("", journal.profileId)
        assertEquals(null, journal.stableVersion)
    }

    companion object {
        /**
         * v1.1 candidata, verbatim del archivo entregado salvo la marca
         * S9 `ab_measurement_only: true` (raíz): la v1.1 es el primer
         * candidato de medición del A/B y su coherencia media es 0.0,
         * así que sin la marca la guarda S9 la rechazaría. El archivo
         * v1.1 entregado por el servidor (pre-S9) NO trae la marca:
         * el analizador debe emitirla al re-emitir la v1.1 (y el
         * validador Python espejo debe admitirla en la raíz), o la
         * v1.1 real será rechazada por coherencia en el teléfono.
         */
        private const val V11_JSON = """{"ab_measurement_only":true,"artist":"desconocido","derivation":{"compressor":"Sin cambio propuesto en la v1.1. Se heredan los valores del perfil vivo v1.0 (threshold -12.0 dB, ratio 2.0, attack 10.0 ms, release 120.0 ms).","eq":"Corrección v1.1 propuesta (Muse IA, 2026-11-28 04:00, 84 sesiones): subgraves +1, graves -3, medios-graves -2, medios +4, presencia +9, brillo +9 dB. Mapeo a los 4 slots del motor v1.0 con clamp ±2 dB: low_60_120 -2.0 (propuesto -3, residual -1.0), mid_500_1k +2.0 (propuesto +4, residual +2.0), high_2k_4k +2.0 (brillo, propuesto +9, residual +7.0), presence_4k_8k +2.0 (presencia, propuesto +9, residual +7.0). Sin slot en el motor (quedan aquí, no se aplican): subgraves +1.0 y medios-graves -2.0. Neto de los 4 slots +4.0 dB; trim -4.0 dB lo compensa para ganancia neta 0.0 dB (Regla de Oro Matched-LUFS). El residual (+7 dB en presencia/brillo) no lo puede aplicar el motor v1.0 en un solo perfil: se cierra por evoluciones sucesivas validadas por A/B o por una ampliación del esquema.","loudness":"Sin medición LUFS en sesión en vivo (micrófono ERB, no FLAC): no hay loudness_lufs_integrated, peak ni crest medidos. La ganancia neta es 0.0 dB por construcción (trim -4.0 dB compensa el neto +4.0 dB de los slots EQ). Los campos de medición de archivo (loudness, peak, crest, espectro por banda, estéreo) se rellenan desde el perfil v1.0 del servidor al aplicar.","note":"CANDIDATO v1.1, NO APLICADO. Pendiente de validación A/B automática: 3 sesiones idénticas vs v1.0, mejora ERB mínima 20% sin perder coherencia, rollback automático si empeora. Operadores sin cambios (Orbe 0.53, Holos 0.34, Neuro 0.35, SalaViva 0.30; SalaViva sin slot en el engine v1.0, se guarda como nota). Coherencia media 0%: el A/B corre con la guarda de coherencia (ver especificación de refactorización). Fingerprint ERB (no FLAC): solo se incluye el dato verificado (ERB-3 209 Hz, +9.86 dB); la tabla completa de 24 bandas vive en el servidor. duration_s 0.0 = sesión en vivo continua sin duración de pista única. artist 'desconocido' = las sesiones no traen artista.","structural_split":"Sin medición HPSS en sesión en vivo (micrófono ERB, no FLAC). Se heredan los valores del compilador de sesión en vivo (transitorio 0.0, tonal 1.0)."},"duration_s":0.0,"engine":{"compressor":{"attack_ms":10.0,"ratio":2.0,"release_ms":120.0,"threshold_db":-12.0},"eq_bands_db":{"high_2k_4k":2.0,"low_60_120":-2.0,"mid_500_1k":2.0,"presence_4k_8k":2.0},"exciter_amount":0.3,"holos_amount":0.34,"net_gain_db":0.0,"orbe_amount":0.53,"orbe_distance_m":0.35,"phi_amount":0.35,"structural_split":{"tonal_amount":1.0,"tonal_energy_ratio":1.0,"transient_amount":0.55,"transient_energy_ratio":0.0},"trim_db":-4.0,"volterra_enabled":true,"volterra_mix":0.55},"fingerprint":{"detected_issues":["Resonancia ERB-3 209 Hz: +9.86 dB ERB","Caída de -7 a -24 dB entre 1.4 kHz y 14.7 kHz (déficit en medios-altos/agudos)","Exceso en graves/medios-graves","Coherencia media ERB 0%: posible cancelación de fase o filtro peine (sala 4.0x7.0 m, techo 3.0 m, posible pared a ~2.0 m)","Fidelidad 30/100; confianza del Gemelo 10%"],"erb_bands_measured":{"erb3_209hz":{"coherence":0.0,"error_db":9.86}},"mean_coherence":0.0,"spectral_error_rms_db":10.46},"format":"IVANNA OMEGA","format_version":"1.0","source":"MuseLink omega-1795857152818-9lec (microfono ERB, 177 sesiones, 2026-11-28 04:09 CST) — CANDIDATO v1.1, NO APLICADO","track":"Sesión en vivo IVANNA"}"""
    }
}
