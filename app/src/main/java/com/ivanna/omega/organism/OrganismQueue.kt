package com.ivanna.omega.organism

import com.google.gson.Gson
import com.google.gson.JsonObject
import java.io.File

/**
 * OrganismQueue — cola durable de envío al organismo (v2.5.506).
 *
 * Sin Room ni WorkManager (no son dependencias del proyecto y añadirlas
 * es un cambio de build mayor): la durabilidad la da el sistema de
 * archivos, un JSON por chunk, con estados explícitos:
 *
 *   PENDING → SENDING → SENT (se borra)
 *                    → RETRY (con nextAttemptAt, backoff con jitter)
 *                    → QUARANTINED (tras N intentos, o 4xx no reintentable)
 *
 * IDEMPOTENCIA: cada chunk lleva (sessionId, chunkId) en el payload
 * (telemetry) y en el nombre del archivo; un reintento reenvía el MISMO
 * archivo, nunca duplica la sesión en la cola.
 *
 * RECUPERACIÓN (consejo 2026-11-26, defecto #2): un chunk en SENDING
 * es un envío a medias — si la app muere ahí, al reabrir la cola vuelve
 * a PENDING (sin consumir intentos). Sin esto quedaba huérfano para
 * siempre: nextReady() solo mira PENDING y RETRY.
 *
 * RETENCIÓN (defecto #9): los chunks viven como máximo 7 días
 * ([MAX_RETENTION_MS]); los más viejos se descartan en el prune. El
 * usuario puede además borrar toda la cola ([clearAll], botón en la
 * tarjeta del organismo). Los JSON viven en filesDir (privado de la
 * app, sin cifrar en reposo — se documenta, no se esconde).
 *
 * CUARENTENA VERSIONADA (C24/510): cada chunk lleva su versión de
 * payload en el body (`telemetry.payloadVersion`, ver
 * [OrganismPayload.PAYLOAD_VERSION]; 0 = sin versionar, anterior a
 * C24). Cuando el servidor anuncia `minSupportedPayloadVersion` mayor
 * que la del teléfono, [discardPayloadOlderThan] borra EXPLÍCITAMENTE
 * (con registro en el controlador) los chunks viejos en vez de
 * dejarlos pudrirse 7 días en cuarentena: un cambio de esquema del
 * servidor ya no tira sesiones en silencio. La cuarentena en sí no
 * cambia: [markQuarantined] estampa además cuándo entró el chunk
 * ([Item.quarantinedAtMs]) para que [lastQuarantinedError] devuelva
 * el error del más reciente, no de uno cualquiera.
 *
 * DEGRADACIÓN POR ESPACIO: si la cola supera el tope, primero se
 * descartan los chunks RETRY más viejos y solo después los PENDING más
 * viejos. El AUDIO nunca se toca: sin espacio, se degrada el envío,
 * no la reproducción. (v2.5.506 es solo-ERB: no hay WAV que degradar;
 * defecto #12.)
 *
 * Puro Kotlin + Gson + java.io.File: testeable en JVM con un directorio
 * temporal, sin Android.
 */
class OrganismQueue(
    private val dir: File,
    private val gson: Gson = Gson(),
    val maxItems: Int = MAX_ITEMS,
    val maxBytes: Long = MAX_BYTES,
    val maxAttempts: Int = MAX_ATTEMPTS
) {
    companion object {
        const val MAX_ITEMS = 200
        const val MAX_BYTES = 50L * 1024L * 1024L  // 50 MB
        const val MAX_ATTEMPTS = 8
        const val BASE_BACKOFF_MS = 30_000L       // 30 s
        const val MAX_BACKOFF_MS = 15 * 60_000L   // 15 min
        /** Retención máxima de un chunk en cola: 7 días (defecto #9). */
        const val MAX_RETENTION_MS = 7L * 24L * 60L * 60L * 1000L
    }

    enum class State { PENDING, SENDING, RETRY, QUARANTINED }

    /** Un chunk encolado. `body` es el JSON del POST (sin token, nunca). */
    data class Item(
        val sessionId: String,
        val chunkId: Int,
        val state: State,
        val attempts: Int,
        val nextAttemptAtMs: Long,
        val body: JsonObject,
        val lastError: String? = null,
        /** Creación (ms epoch). 0 = desconocido (archivo viejo). */
        val createdAtMs: Long = 0L,
        /**
         * C24/510 — cuándo entró en QUARANTINED (ms epoch). 0 = nunca
         * estuvo en cuarentena o archivo de antes de C24 (entonces manda
         * [createdAtMs] como aproximación en [lastQuarantinedError]).
         */
        val quarantinedAtMs: Long = 0L
    ) {
        /** Clave de idempotencia: el mismo chunk nunca se encola dos veces. */
        val key: String get() = "${sessionId}_$chunkId"
    }

    init {
        dir.mkdirs()
        recoverSending()
        pruneIfOverCap()
    }

    /**
     * Recuperación de SENDING huérfano (defecto #2): un envío a medias
     * por muerte del proceso vuelve a PENDING sin consumir intentos.
     */
    private fun recoverSending() {
        for (item in listAll()) {
            if (item.state == State.SENDING) {
                writeItem(item.copy(state = State.PENDING, nextAttemptAtMs = 0L))
            }
        }
    }

    /** Encola un chunk; si la clave ya existe, no duplica (devuelve false). */
    @Synchronized
    fun enqueue(sessionId: String, chunkId: Int, body: JsonObject): Boolean {
        val key = "${sessionId}_$chunkId"
        if (findFile(key) != null) return false
        val item = Item(
            sessionId, chunkId, State.PENDING, 0, 0L, body,
            createdAtMs = System.currentTimeMillis()
        )
        writeItem(item)
        pruneIfOverCap()
        return true
    }

    /** Próximo chunk listo para enviar (PENDING, o RETRY vencido), el más viejo. */
    @Synchronized
    fun nextReady(nowMs: Long): Item? {
        return listAll()
            .filter { it.state == State.PENDING || (it.state == State.RETRY && it.nextAttemptAtMs <= nowMs) }
            .minByOrNull { it.nextAttemptAtMs }
    }

    @Synchronized
    fun markSending(item: Item) {
        writeItem(item.copy(state = State.SENDING))
    }

    /**
     * Vuelta a PENDING sin consumir intentos (defecto #3): un 401 es un
     * error de configuración (token muerto), no transitorio. El chunk
     * queda listo para reintentarse en cuanto se corrija el token, sin
     * acercarse a la cuarentena por maxAttempts.
     */
    @Synchronized
    fun markPending(item: Item): Item {
        val next = item.copy(state = State.PENDING, nextAttemptAtMs = 0L)
        writeItem(next)
        return next
    }

    @Synchronized
    fun markSent(item: Item) {
        findFile(item.key)?.delete()
    }

    /**
     * Fallo reintentable (red, 429, 5xx): RETRY con backoff exponencial
     * con jitter ±25% (el jitter lo pone el llamador en nextAttemptAtMs
     * si quiere; aquí se calcula con el reloj dado).
     */
    @Synchronized
    fun markRetry(item: Item, nowMs: Long, error: String?, jitterRatio: Double = 0.0): Item {
        val attempts = item.attempts + 1
        if (attempts >= maxAttempts) return markQuarantined(item, "max-intentos: $error")
        val backoff = backoffMs(attempts)
        val jitter = (backoff * jitterRatio).toLong()
        val next = item.copy(
            state = State.RETRY, attempts = attempts,
            nextAttemptAtMs = nowMs + backoff + jitter, lastError = error
        )
        writeItem(next)
        return next
    }

    /** Fallo no reintentable (4xx salvo 429, payload inválido): cuarentena. */
    @Synchronized
    fun markQuarantined(item: Item, error: String?, nowMs: Long = System.currentTimeMillis()): Item {
        // C24/510: se estampa cuándo entró en cuarentena; el estado
        // expone el lastError del chunk en cuarentena MÁS RECIENTE.
        val next = item.copy(state = State.QUARANTINED, lastError = error, quarantinedAtMs = nowMs)
        writeItem(next)
        return next
    }

    /**
     * C24/510 — `lastError` del chunk en cuarentena más reciente
     * (por [Item.quarantinedAtMs]; sin estampa —archivo de antes de
     * C24— manda `createdAtMs`). null si no hay nada en cuarentena.
     * Es lo que el estado expone como `lastQuarantineError`.
     */
    @Synchronized
    fun lastQuarantinedError(): String? =
        listAll().filter { it.state == State.QUARANTINED }
            .maxByOrNull { if (it.quarantinedAtMs > 0L) it.quarantinedAtMs else it.createdAtMs }
            ?.lastError

    /**
     * C24/510 — descarte EXPLÍCITO por payload obsoleto (lo llama el
     * controlador cuando el servidor anuncia `minSupportedPayloadVersion`
     * mayor que [OrganismPayload.PAYLOAD_VERSION]). Borra los chunks
     * cuya versión de payload ([OrganismPayload.payloadVersionOf]; 0 =
     * sin versionar) es menor que [minSupportedVersion], en CUALQUIER
     * estado salvo SENDING: un chunk en vuelo no se toca (su outcome lo
     * reescribe al terminar y el próximo sondeo lo reevalúa). Devuelve
     * cuántos se descartaron. El registro (qué versión exigía el
     * servidor, cuántos cayeron) lo hace el llamador: aquí solo se
     * borra. No es el prune de retención: este descarte es por esquema,
     * no por edad, y no espera 7 días.
     */
    @Synchronized
    fun discardPayloadOlderThan(minSupportedVersion: Long): Int {
        var discarded = 0
        for (item in listAll()) {
            if (item.state == State.SENDING) continue
            if (OrganismPayload.payloadVersionOf(item.body) < minSupportedVersion) {
                if (findFile(item.key)?.delete() == true) discarded++
            }
        }
        return discarded
    }

    @Synchronized
    fun counts(): Map<State, Int> =
        listAll().groupingBy { it.state }.eachCount().withDefault { 0 }

    @Synchronized
    fun pendingBytes(): Long = listAll().sumOf { fileSize(it) }

    @Synchronized
    fun listAll(): List<Item> {
        val files = dir.listFiles()?.filter { it.name.endsWith(".json") } ?: return emptyList()
        return files.mapNotNull { f ->
            runCatching {
                val raw = f.readText()
                val item = gson.fromJson(raw, StoredItem::class.java)
                // createdAtMs: el guardado, o la fecha del archivo si el
                // JSON es de antes de la retención (compat hacia atrás).
                item.toItem().let {
                    if (it.createdAtMs > 0) it else it.copy(createdAtMs = f.lastModified())
                }
            }.getOrNull()
        }.sortedBy { it.sessionId + "_" + it.chunkId.toString().padStart(6, '0') }
    }

    /** Backoff exponencial: 30 s, 60 s, 120 s… con tope de 15 min. */
    fun backoffMs(attempts: Int): Long {
        var backoff = BASE_BACKOFF_MS
        repeat((attempts - 1).coerceAtLeast(0)) { backoff = (backoff * 2).coerceAtMost(MAX_BACKOFF_MS) }
        return backoff.coerceAtMost(MAX_BACKOFF_MS)
    }

    /**
     * Degradación por espacio y retención (defectos #9 y #12): primero se
     * descartan los chunks más viejos que la retención (7 días); después
     * los RETRY más viejos; último recurso, los PENDING más viejos. Nunca
     * se descarta un chunk con ERB por un WAV (v2.5.506 es solo-ERB).
     * Devuelve cuántos items se descartaron.
     */
    @Synchronized
    fun pruneIfOverCap(nowMs: Long = System.currentTimeMillis()): Int {
        var degraded = 0
        var items = listAll()
        var bytes = items.sumOf { fileSize(it) }
        // 0) Retención: fuera los chunks más viejos que 7 días.
        val expired = items.filter { it.createdAtMs > 0 && nowMs - it.createdAtMs > MAX_RETENTION_MS }
        for (victim in expired) {
            findFile(victim.key)?.delete()
            degraded++
        }
        if (expired.isNotEmpty()) {
            items = items - expired.toSet()
            bytes = items.sumOf { fileSize(it) }
        }
        // 1) Descartar RETRY más viejos.
        while ((items.size > maxItems || bytes > maxBytes) &&
            items.any { it.state == State.RETRY }) {
            val victim = items.filter { it.state == State.RETRY }
                .minByOrNull { it.nextAttemptAtMs } ?: break
            findFile(victim.key)?.delete()
            degraded++
            items = items - victim
            bytes = items.sumOf { fileSize(it) }
        }
        // 2) Último recurso: PENDING más viejo.
        while (items.size > maxItems || bytes > maxBytes) {
            val victim = items.minByOrNull { it.nextAttemptAtMs } ?: break
            findFile(victim.key)?.delete()
            degraded++
            items = items - victim
            bytes = items.sumOf { fileSize(it) }
        }
        return degraded
    }

    /** Borrado total por el usuario (defecto #9): vacía la cola en disco. */
    @Synchronized
    fun clearAll(): Int {
        val files = dir.listFiles()?.filter {
            it.name.endsWith(".json") || it.name.endsWith(".tmp")
        } ?: return 0
        var deleted = 0
        for (f in files) if (f.delete()) deleted++
        return deleted
    }

    private fun fileSize(item: Item): Long = findFile(item.key)?.length() ?: 0L

    private fun findFile(key: String): File? {
        val f = File(dir, "$key.json")
        return if (f.exists()) f else null
    }

    private fun writeItem(item: Item) {
        val stored = StoredItem.from(item)
        val tmp = File(dir, "${item.key}.tmp")
        tmp.writeText(gson.toJson(stored))
        val dest = File(dir, "${item.key}.json")
        if (!tmp.renameTo(dest)) {
            tmp.copyTo(dest, overwrite = true)
            tmp.delete()
        }
    }

    /** Forma serializada (el body va como string JSON, sin token jamás). */
    private data class StoredItem(
        val sessionId: String = "",
        val chunkId: Int = 0,
        val state: String = "PENDING",
        val attempts: Int = 0,
        val nextAttemptAtMs: Long = 0L,
        val bodyJson: String = "{}",
        val lastError: String? = null,
        val createdAtMs: Long = 0L,
        /** C24/510 — ver [Item.quarantinedAtMs]; 0 en archivos viejos. */
        val quarantinedAtMs: Long = 0L
    ) {
        fun toItem(): Item {
            val body = Gson().fromJson(bodyJson, JsonObject::class.java) ?: JsonObject()
            return Item(
                sessionId, chunkId,
                runCatching { State.valueOf(state) }.getOrDefault(State.PENDING),
                attempts, nextAttemptAtMs, body, lastError, createdAtMs, quarantinedAtMs
            )
        }

        companion object {
            fun from(item: Item): StoredItem {
                return StoredItem(
                    item.sessionId, item.chunkId, item.state.name, item.attempts,
                    item.nextAttemptAtMs, Gson().toJson(item.body),
                    item.lastError, item.createdAtMs, item.quarantinedAtMs
                )
            }
        }
    }
}
