package com.ivanna.omega.audio

import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * Tests de la escritura atómica del override (C8, release 510).
 * Puro JVM: [ProfilesLoader.writeOverrideAtomic] y [ProfilesLoader.restoreBackup]
 * operan sobre un directorio cualquiera, sin Context ni Android. La
 * compilación/ejecución queda para la CI (Gradle no corre en esta VM:
 * conexión cliente→daemon bloqueada).
 */
class ProfilesLoaderAtomicTest {

    private lateinit var dir: File

    @Before
    fun setup() {
        dir = Files.createTempDirectory("profiles-loader-atomic-test").toFile()
    }

    @After
    fun teardown() {
        dir.deleteRecursively()
    }

    private fun mainFile() = File(dir, "audio_profiles.json")
    private fun bakFile() = File(dir, "audio_profiles.json.bak")
    private fun tmpFile() = File(dir, "audio_profiles.json.tmp")

    @Test
    fun `tmp huerfano no reemplaza al principal sin rename`() {
        ProfilesLoader.writeOverrideAtomic(dir, container("v1"))
        // Corte simulado a mitad de escritura: un .tmp con basura que
        // nunca llegó a renombrarse.
        tmpFile().writeText("{basura-corrupta", Charsets.UTF_8)
        assertEquals("el principal debe seguir intacto", container("v1"), mainFile().readText())
        // La siguiente escritura buena lo reemplaza y deja el .tmp limpio.
        ProfilesLoader.writeOverrideAtomic(dir, container("v2"))
        assertEquals(container("v2"), mainFile().readText())
        assertEquals("el anterior queda en .bak", container("v1"), bakFile().readText())
        assertFalse("no debe quedar .tmp tras una escritura completa", tmpFile().exists())
    }

    @Test
    fun `segunda escritura guarda el anterior en bak`() {
        ProfilesLoader.writeOverrideAtomic(dir, container("v1"))
        ProfilesLoader.writeOverrideAtomic(dir, container("v2"))
        assertEquals(container("v2"), mainFile().readText())
        assertEquals(container("v1"), bakFile().readText())
    }

    @Test
    fun `principal corrupto restaura el bak`() {
        ProfilesLoader.writeOverrideAtomic(dir, container("v1"))
        ProfilesLoader.writeOverrideAtomic(dir, container("v2"))
        mainFile().writeText("{corrupto", Charsets.UTF_8)
        assertTrue(ProfilesLoader.restoreBackup(dir))
        assertEquals("el principal debe volver a ser v1", container("v1"), mainFile().readText())
    }

    @Test
    fun `bak corrupto no restaura ni toca el principal`() {
        ProfilesLoader.writeOverrideAtomic(dir, container("v1"))
        ProfilesLoader.writeOverrideAtomic(dir, container("v2"))
        bakFile().writeText("{bak-corrupto", Charsets.UTF_8)
        mainFile().writeText("{corrupto", Charsets.UTF_8)
        assertFalse(ProfilesLoader.restoreBackup(dir))
        assertEquals("el principal corrupto no se toca", "{corrupto", mainFile().readText())
    }

    @Test
    fun `sin bak no hay restauracion`() {
        ProfilesLoader.writeOverrideAtomic(dir, container("v1"))
        bakFile().delete()
        mainFile().writeText("{corrupto", Charsets.UTF_8)
        assertFalse(ProfilesLoader.restoreBackup(dir))
        assertEquals("{corrupto", mainFile().readText())
    }

    /** Contenedor mínimo válido (lista vacía, solo cambia la versión del metadata). */
    private fun container(version: String): String =
        """{"audioProfiles":[],"metadata":{"version":"$version","createdDate":"","lastModified":"","totalProfiles":0,"trainingSequence":[],"trainingNotes":""}}"""
}
