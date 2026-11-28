package com.ivanna.omega.organism

import com.google.gson.JsonArray
import com.google.gson.JsonObject

/**
 * OrganismPayload — cuerpo del POST /api/v1/muse/ivanna/session (v2.5.506).
 *
 * Schema verificado contra el fuente de Muselink
 * (~/workspace/muselink-source/src/lib/museTypes.ts + ivannaDsp.ts):
 *   audioFeatures: { bands: [{band, freqHz, magnitudeDb, errorDb,
 *     coherence}], meanCoherence, spectralErrorRmsDb,
 *     source: "microphone-erb" }
 *   telemetry: { sessionId, chunkId, ... } (fuente cruda aceptada)
 *   song/artist: requeridos (1–140 caracteres).
 *
 * Sin canción conocida, song/artist son la etiqueta de la sesión en vivo
 * ("Sesión en vivo IVANNA" / "IVANNA OMEGA"), NO una canción inventada:
 * el organismo compila perfiles por canción solo cuando hay metadata
 * real; aquí el dato crudo es el ERB medido, que es lo que compila.
 *
 * Puro Kotlin (Gson): testeable en JVM sin Android.
 */
object OrganismPayload {
    const val DEFAULT_SONG = "Sesión en vivo IVANNA"
    const val DEFAULT_ARTIST = "IVANNA OMEGA"
    const val DEFAULT_ROOM = "Sala 4.0x7.0 m, escucha a 3 m"
    const val DEFAULT_EQUIPMENT = "Genezi MHC-PZ1D"

    /**
     * T2/510 — atribución A/B: valores neutros cuando aún no hay ningún
     * perfil de Muselink aplicado. `id = "factory"` = perfil de fábrica;
     * `version = 0` = ninguna versión del servidor (el sobre de
     * `GET /profile/current` usa `version: Long >= 1`, igual que
     * `ProfileApplier`); `abState = "none"` = vocabulario del §3.1 de la
     * especificación de la VUELTA (`none | candidate | ab_running |
     * promoted | rolled_back`); `hash = ""` = sin hash conocido.
     */
    const val DEFAULT_APPLIED_PROFILE_ID = "factory"
    const val DEFAULT_APPLIED_PROFILE_VERSION = 0L
    const val DEFAULT_AB_STATE = "none"

    /**
     * C24/510 — versión del esquema del POST /session (campo
     * `payloadVersion` en `telemetry`). El servidor puede anunciar
     * `minSupportedPayloadVersion` en el sobre del GET de perfil (ver
     * [ProfileFetcher]): si es mayor que esta, la cola vieja se
     * descarta explícitamente ([OrganismQueue.discardPayloadOlderThan])
     * en vez de pudrirse 7 días en cuarentena. Subir aquí SOLO cuando
     * cambie el esquema del POST (y el servidor lo anuncie primero).
     */
    const val PAYLOAD_VERSION = 1

    /**
     * C24/510 — versión de payload de un cuerpo encolado (para el
     * descarte por obsolescencia). 0 = sin versionar (chunk de antes
     * de C24) o campo inválido: un chunk sin versión nunca puede
     * probar que el servidor lo acepte, así que cuenta como el más
     * viejo. Puro JVM.
     */
    fun payloadVersionOf(body: JsonObject): Int {
        return runCatching {
            val telemetry = body.getAsJsonObject("telemetry") ?: return 0
            val v = telemetry.get("payloadVersion") ?: return 0
            v.asInt.coerceAtLeast(0)
        }.getOrDefault(0)
    }

    /**
     * T2/510 — perfil aplicado que sonó en este chunk. Lo mantiene
     * `OrganismController` tras cada aplicación (`recordAppliedProfile`)
     * y lo lee `enqueueChunk` para etiquetar cada chunk; `OrganismPayload`
     * no toca el controlador directamente para seguir siendo puro JVM.
     */
    data class AppliedProfile(
        val id: String = DEFAULT_APPLIED_PROFILE_ID,
        val version: Long = DEFAULT_APPLIED_PROFILE_VERSION,
        val abState: String = DEFAULT_AB_STATE,
        val hash: String = ""
    )

    private val erbCenters: FloatArray by lazy { SessionAggregator.erbCenterHz() }

    /** audioFeatures en el schema que normalizeAudioFeatures acepta. */
    fun audioFeatures(chunk: SessionAggregator.SessionChunk): JsonObject {
        val bands = JsonArray()
        for (b in 0 until SessionAggregator.BANDS) {
            val band = JsonObject()
            band.addProperty("band", "ERB-${b + 1}")
            band.addProperty("freqHz", erbCenters[b].toDouble())
            band.addProperty("magnitudeDb", chunk.erbMeanDb[b].toDouble())
            band.addProperty("errorDb", chunk.errorMeanDb[b].toDouble())
            band.addProperty("coherence", chunk.coherenceMean[b].toDouble())
            bands.add(band)
        }
        val out = JsonObject()
        out.add("bands", bands)
        out.addProperty("meanCoherence", chunk.meanCoherence.toDouble())
        out.addProperty("spectralErrorRmsDb", chunk.spectralErrorRmsDb.toDouble())
        out.addProperty("source", "microphone-erb")
        return out
    }

    /** Cuerpo completo del POST de sesión para UN chunk agregado.
     *
     * T2/510: `applied` etiqueta el chunk con el perfil que sonó
     * (id, versión, abState, hash). Sin perfil aplicado lleva los
     * valores neutros (`factory` / 0 / `none` / `""`): el chunk sigue
     * siendo válido y el servidor lo atribuye a fábrica, sin inferir
     * la versión por correlación temporal con el `POST applied`.
     */
    fun sessionBody(
        chunk: SessionAggregator.SessionChunk,
        song: String = DEFAULT_SONG,
        artist: String = DEFAULT_ARTIST,
        room: String = DEFAULT_ROOM,
        equipment: String = DEFAULT_EQUIPMENT,
        applied: AppliedProfile = AppliedProfile()
    ): JsonObject {
        val body = JsonObject()
        body.addProperty("song", song.take(140).ifBlank { DEFAULT_SONG })
        body.addProperty("artist", artist.take(140).ifBlank { DEFAULT_ARTIST })
        body.add("audioFeatures", audioFeatures(chunk))
        body.addProperty("room", room)
        body.addProperty("equipment", equipment)
        val telemetry = JsonObject()
        telemetry.addProperty("sessionId", chunk.sessionId)
        telemetry.addProperty("chunkId", chunk.chunkId)
        telemetry.addProperty("chunkStartedAtUs", chunk.startedAtUs)
        telemetry.addProperty("chunkEndedAtUs", chunk.endedAtUs)
        telemetry.addProperty("framesValid", chunk.framesValid)
        telemetry.addProperty("framesTotal", chunk.framesTotal)
        telemetry.addProperty("levelMeanDbfs", chunk.levelMeanDbfs.toDouble())
        telemetry.addProperty("levelMaxDbfs", chunk.levelMaxDbfs.toDouble())
        // S1/510: frescura de referencia de la ventana. Sin estos campos
        // la coherencia media es uninterpretable (0% puede ser sala o
        // instrumentación). referenceLevelMeanDbfs es media del mic en
        // 250 Hz–2 kHz, NO nivel de la señal de referencia.
        telemetry.addProperty("referenceFreshFrac", chunk.referenceFreshFrac.toDouble())
        telemetry.addProperty("framesReferenceFresh", chunk.framesReferenceFresh)
        telemetry.addProperty("referenceLevelMeanDbfs", chunk.referenceLevelMeanDbfs.toDouble())
        telemetry.addProperty("engineVersion", chunk.engineVersion)
        // C24/510 — versión del esquema del payload. El servidor la lee
        // para rechazar (4xx → cuarentena) lo que ya no acepta, y anuncia
        // su mínimo en el sobre del GET de perfil para el descarte
        // explícito (sin esperar a que el prune de 7 días lo borre).
        telemetry.addProperty("payloadVersion", PAYLOAD_VERSION)
        // T2/510 — atribución A/B: con qué perfil sonó este chunk.
        // Campos nuevos; el servidor debe ignorarlos si no los conoce
        // (pendiente de verificar: si los rechaza con 4xx, los chunks
        // irían a cuarentena — verificar con un chunk de prueba en vivo).
        telemetry.addProperty("appliedProfileId", applied.id)
        telemetry.addProperty("appliedProfileVersion", applied.version)
        telemetry.addProperty("abState", applied.abState)
        telemetry.addProperty("appliedProfileHash", applied.hash)
        body.add("telemetry", telemetry)
        return body
    }

    /**
     * T2/510 — cuerpo del `POST /api/v1/muse/ivanna/profile/applied`
     * (VUELTA §3.1): `{ profileId, version, appliedAt, engineVersion,
     * previousVersion }`, más `profileHash` (extensión T2: el hash del
     * perfil aplicado, para que el servidor correlacione versión
     * aplicada ↔ ERB medido sin depender solo de la versión).
     * `appliedAt` en ISO-8601 UTC, igual que `compiledAt` del GET.
     * PUNTO DE ENGANCHE — el POST existe en `ProfileFetcher.confirmApplied()`
     * (T1, Fase 2): quien lo envía construye el cuerpo con esta función a
     * partir del `AppliedProfile` que devuelve
     * `OrganismController.recordAppliedProfile`.
     */
    fun appliedConfirmation(
        applied: AppliedProfile,
        previousVersion: Long,
        engineVersion: String,
        appliedAtMs: Long = System.currentTimeMillis()
    ): JsonObject {
        val body = JsonObject()
        body.addProperty("profileId", applied.id)
        body.addProperty("version", applied.version)
        body.addProperty("appliedAt", java.time.Instant.ofEpochMilli(appliedAtMs).toString())
        body.addProperty("engineVersion", engineVersion)
        body.addProperty("previousVersion", previousVersion)
        body.addProperty("profileHash", applied.hash)
        return body
    }
}
