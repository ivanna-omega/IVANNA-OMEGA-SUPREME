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

    /** Cuerpo completo del POST de sesión para UN chunk agregado. */
    fun sessionBody(
        chunk: SessionAggregator.SessionChunk,
        song: String = DEFAULT_SONG,
        artist: String = DEFAULT_ARTIST,
        room: String = DEFAULT_ROOM,
        equipment: String = DEFAULT_EQUIPMENT
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
        body.add("telemetry", telemetry)
        return body
    }
}
