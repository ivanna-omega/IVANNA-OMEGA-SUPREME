package com.ivanna.omega.organism

import com.google.gson.JsonObject
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * Tests de la cola durable (estados + reintento + degradación, v2.5.506).
 * Puro JVM: la cola vive en un directorio temporal.
 */
class OrganismQueueTest {

    private lateinit var dir: File
    private lateinit var queue: OrganismQueue

    @Before
    fun setup() {
        dir = Files.createTempDirectory("organism-queue-test").toFile()
        queue = OrganismQueue(dir)
    }

    @After
    fun teardown() {
        dir.deleteRecursively()
    }

    private fun body(chunkId: Int = 0): JsonObject =
        JsonObject().apply { addProperty("chunkId", chunkId) }

    @Test
    fun `enqueue y nextReady devuelven el chunk pendiente`() {
        assertTrue(queue.enqueue("s1", 0, body(0)))
        assertTrue(queue.enqueue("s1", 1, body(1)))
        val next = queue.nextReady(1000L)
        assertNotNull(next)
        assertEquals("s1", next!!.sessionId)
        assertEquals(OrganismQueue.State.PENDING, next.state)
        assertEquals(2, queue.counts().getValue(OrganismQueue.State.PENDING))
    }

    @Test
    fun `la misma clave no se encola dos veces (idempotencia)`() {
        assertTrue(queue.enqueue("s1", 0, body(0)))
        assertFalse(queue.enqueue("s1", 0, body(0)))
        assertEquals(1, queue.listAll().size)
    }

    @Test
    fun `markSent borra el chunk`() {
        queue.enqueue("s1", 0, body(0))
        val item = queue.nextReady(1000L)!!
        queue.markSending(item)
        assertEquals(OrganismQueue.State.SENDING, queue.listAll()[0].state)
        queue.markSent(item)
        assertTrue(queue.listAll().isEmpty())
    }

    @Test
    fun `markRetry aplica backoff exponencial con tope`() {
        assertEquals(30_000L, queue.backoffMs(1))
        assertEquals(60_000L, queue.backoffMs(2))
        assertEquals(120_000L, queue.backoffMs(3))
        assertEquals(900_000L, queue.backoffMs(99)) // tope 15 min
        queue.enqueue("s1", 0, body(0))
        val item = queue.nextReady(1000L)!!
        val retried = queue.markRetry(item, 10_000L, "red caída")
        assertEquals(OrganismQueue.State.RETRY, retried.state)
        assertEquals(1, retried.attempts)
        assertEquals(10_000L + 30_000L, retried.nextAttemptAtMs)
        assertEquals("red caída", retried.lastError)
        // Aún no vencido: no está listo.
        assertNull(queue.nextReady(20_000L))
        // Vencido: vuelve a estar listo.
        assertNotNull(queue.nextReady(50_000L))
    }

    @Test
    fun `tras maxAttempts el chunk va a cuarentena`() {
        val small = OrganismQueue(dir, maxAttempts = 2)
        small.enqueue("s1", 0, body(0))
        var item = small.nextReady(1000L)!!
        item = small.markRetry(item, 1000L, "500")
        assertEquals(OrganismQueue.State.RETRY, item.state)
        val quarantined = small.markRetry(item, 2000L, "500 otra vez")
        assertEquals(OrganismQueue.State.QUARANTINED, quarantined.state)
        assertTrue(quarantined.lastError!!.contains("max-intentos"))
        // En cuarentena ya no está listo para envío.
        assertNull(small.nextReady(1_000_000L))
    }

    @Test
    fun `markQuarantined conserva el chunk fuera del flujo`() {
        queue.enqueue("s1", 0, body(0))
        val item = queue.nextReady(1000L)!!
        queue.markQuarantined(item, "400 payload inválido")
        val all = queue.listAll()
        assertEquals(1, all.size)
        assertEquals(OrganismQueue.State.QUARANTINED, all[0].state)
        assertNull(queue.nextReady(1_000_000L))
    }

    @Test
    fun `la cola sobrevive a una nueva instancia (durable en disco)`() {
        queue.enqueue("s1", 0, body(0))
        queue.enqueue("s1", 1, body(1))
        val reopened = OrganismQueue(dir)
        assertEquals(2, reopened.listAll().size)
        assertEquals(2, reopened.counts().getValue(OrganismQueue.State.PENDING))
    }

    @Test
    fun `SENDING huerfano vuelve a PENDING al reabrir la cola`() {
        queue.enqueue("s1", 0, body(0))
        val item = queue.nextReady(1000L)!!
        queue.markSending(item)
        assertEquals(OrganismQueue.State.SENDING, queue.listAll()[0].state)
        // La app "muere" con el chunk en SENDING: al reabrir, vuelve a
        // PENDING sin consumir intentos (defecto #2).
        val reopened = OrganismQueue(dir)
        val recovered = reopened.listAll()
        assertEquals(1, recovered.size)
        assertEquals(OrganismQueue.State.PENDING, recovered[0].state)
        assertEquals(0, recovered[0].attempts)
        assertNotNull(reopened.nextReady(1000L))
    }

    @Test
    fun `retencion de 7 dias descarta chunks viejos`() {
        // Chunk viejo escrito a mano (createdAtMs de hace 8 días).
        val oldJson = """{"sessionId":"s1","chunkId":0,"state":"PENDING","attempts":0,"nextAttemptAtMs":0,"bodyJson":"{\"song\":\"x\"}","lastError":null,"createdAtMs":${System.currentTimeMillis() - 8L * 24 * 60 * 60 * 1000}}"""
        File(dir, "s1_0.json").writeText(oldJson)
        val reopened = OrganismQueue(dir)
        assertTrue("el chunk de 8 días debe descartarse", reopened.listAll().isEmpty())
    }

    @Test
    fun `clearAll borra toda la cola`() {
        queue.enqueue("s1", 0, body(0))
        queue.enqueue("s1", 1, body(1))
        assertEquals(2, queue.clearAll())
        assertTrue(queue.listAll().isEmpty())
    }

    @Test
    fun `prune descarta RETRY viejos antes que PENDING`() {
        val tiny = OrganismQueue(dir, maxItems = 2)
        tiny.enqueue("s1", 0, body(0))
        tiny.enqueue("s1", 1, body(1))
        tiny.enqueue("s1", 2, body(2))
        // 3 PENDING con tope 2: el más viejo cae (último recurso).
        var items = tiny.listAll()
        assertEquals(2, items.size)
        assertEquals(1, items[0].chunkId)
        // Ahora el chunk 1 pasa a RETRY y entra un cuarto: el RETRY cae
        // antes que cualquier PENDING.
        val retry = tiny.markRetry(items[0], 1000L, "500")
        assertEquals(OrganismQueue.State.RETRY, retry.state)
        tiny.enqueue("s1", 3, body(3))
        items = tiny.listAll()
        assertEquals(2, items.size)
        assertEquals(listOf(2, 3), items.map { it.chunkId })
        assertTrue(items.all { it.state == OrganismQueue.State.PENDING })
    }

    // T10/510 — el prune deja de ser silencioso: la auditoría IDA
    // cuenta los podados como disposición, no como pérdida.

    @Test
    fun `prunedTotal cuenta los podados por tope de cola`() {
        val tinyDir = Files.createTempDirectory("organism-queue-prune-test").toFile()
        try {
            val tiny = OrganismQueue(tinyDir, maxItems = 2)
            assertEquals(0, tiny.prunedTotal)
            tiny.enqueue("s1", 0, body(0))
            tiny.enqueue("s1", 1, body(1))
            assertEquals(0, tiny.prunedTotal)
            tiny.enqueue("s1", 2, body(2)) // 3 con tope 2: cae el más viejo.
            assertEquals(1, tiny.prunedTotal)
            tiny.enqueue("s1", 3, body(3))
            assertEquals(2, tiny.prunedTotal)
            assertEquals(2, tiny.listAll().size)
        } finally {
            tinyDir.deleteRecursively()
        }
    }

    @Test
    fun `prunedTotal cuenta el podado por retencion al reabrir`() {
        // Chunk viejo escrito a mano (createdAtMs de hace 8 días).
        val oldDir = Files.createTempDirectory("organism-queue-retention-test").toFile()
        try {
            val oldJson = """{"sessionId":"s1","chunkId":0,"state":"PENDING","attempts":0,"nextAttemptAtMs":0,"bodyJson":"{\"song\":\"x\"}","lastError":null,"createdAtMs":${System.currentTimeMillis() - 8L * 24 * 60 * 60 * 1000}}"""
            File(oldDir, "s1_0.json").writeText(oldJson)
            val reopened = OrganismQueue(oldDir)
            assertTrue("el chunk de 8 días debe descartarse", reopened.listAll().isEmpty())
            assertEquals(1, reopened.prunedTotal)
        } finally {
            oldDir.deleteRecursively()
        }
    }

    // C24/510 — cuarentena versionada.

    private fun versionedBody(version: Int): JsonObject =
        JsonObject().apply {
            add("telemetry", JsonObject().apply { addProperty("payloadVersion", version) })
        }

    @Test
    fun `markQuarantined estampa la fecha y lastQuarantinedError devuelve el mas reciente`() {
        queue.enqueue("s1", 0, body(0))
        queue.enqueue("s1", 1, body(1))
        val first = queue.listAll().first { it.chunkId == 0 }
        queue.markQuarantined(first, "400 viejo", nowMs = 1000L)
        val second = queue.listAll().first { it.chunkId == 1 }
        queue.markQuarantined(second, "422 nuevo", nowMs = 2000L)
        assertEquals(2, queue.counts().getValue(OrganismQueue.State.QUARANTINED))
        assertEquals("422 nuevo", queue.lastQuarantinedError())
        // La estampa sobrevive a reabrir la cola (disco, no memoria).
        val reopened = OrganismQueue(dir)
        assertEquals("422 nuevo", reopened.lastQuarantinedError())
    }

    @Test
    fun `lastQuarantinedError es null sin cuarentena`() {
        queue.enqueue("s1", 0, body(0))
        assertNull(queue.lastQuarantinedError())
    }

    @Test
    fun `discardPayloadOlderThan borra solo los de version vieja o sin versionar`() {
        queue.enqueue("s1", 0, versionedBody(1)) // vigente
        queue.enqueue("s1", 1, versionedBody(0)) // versionado a 0
        queue.enqueue("s1", 2, body(2)) // sin telemetry (pre-C24)
        queue.enqueue("s1", 3, versionedBody(2)) // nuevo
        assertEquals(2, queue.discardPayloadOlderThan(2L))
        assertEquals(listOf(0, 3), queue.listAll().map { it.chunkId })
        // Idempotente: el segundo pase no borra nada.
        assertEquals(0, queue.discardPayloadOlderThan(2L))
        // Un mínimo igual a la versión no descarta nada.
        assertEquals(0, queue.discardPayloadOlderThan(1L))
    }

    @Test
    fun `discardPayloadOlderThan no toca un chunk en SENDING`() {
        queue.enqueue("s1", 0, versionedBody(0))
        val item = queue.nextReady(1000L)!!
        queue.markSending(item)
        assertEquals(0, queue.discardPayloadOlderThan(2L))
        assertEquals(1, queue.listAll().size)
        // Al resolverse el envío (aquí, cuarentena por 4xx), el próximo
        // descarte sí lo alcanza: no queda huérfano obsoleto.
        queue.markQuarantined(item, "400 esquema viejo", nowMs = 5000L)
        assertEquals(1, queue.discardPayloadOlderThan(2L))
        assertTrue(queue.listAll().isEmpty())
    }

    @Test
    fun `el prune de retencion no borra cuarentena joven (solo la edad manda)`() {
        queue.enqueue("s1", 0, body(0))
        val item = queue.nextReady(1000L)!!
        queue.markQuarantined(item, "400", nowMs = System.currentTimeMillis())
        // Prune inmediato: la cuarentena joven sobrevive (la edad, no el
        // estado, decide; el descarte por esquema es discardPayloadOlderThan).
        assertEquals(0, queue.pruneIfOverCap())
        assertEquals(1, queue.listAll().size)
    }
}
