package com.ivanna.omega.organism

import java.util.UUID
import kotlin.math.sqrt

/**
 * SessionAggregator — agregador local de sesión (v2.5.506, consejo Luna/Haiku).
 *
 * El sensor publica frames cada ~85 ms (FloatArray[76]: mag[0..23],
 * err[24..47], coh[48..71], level[72], valid[73], referenceFresh[74],
 * referenceLevelDb[75]). Enviar cada frame
 * sería ruido de red, batería y exposición de datos. Aquí se agregan en
 * VENTANAS de 1–5 s (por defecto 5 s): por ventana, media y dispersión
 * por banda ERB, coherencia media, nivel medio/máximo, frames válidos y
 * timestamps. Al cierre de la sesión se emite el payload final.
 *
 * La latencia de red NO participa en el lazo DSP: el agregador solo
 * observa (corre en el hilo del consumidor, nunca en el de audio).
 * v2.5.506 es solo-ERB: no hay captura WAV (defecto #12 del consejo —
 * el código del WAV era muerto, ningún llamador lo alimentaba; se
 * eliminó el camino en vez de dejarlo como adorno).
 *
 * Puro Kotlin (sin Android): testeable en JVM con frames sintéticos.
 * Frames inválidos (valid=0, silencio ≤ −70 dBFS, no finitos) NO entran
 * en las estadísticas, pero SÍ cuentan en framesTotal (para saber cuánto
 * se midió de verdad).
 */
class SessionAggregator(
    val sessionId: String = UUID.randomUUID().toString(),
    windowSeconds: Float = DEFAULT_WINDOW_SECONDS,
    val engineVersion: String = "2.5.506"
) {
    companion object {
        const val BANDS = 24
        const val DEFAULT_WINDOW_SECONDS = 5f
        const val MIN_WINDOW_SECONDS = 1f
        const val MAX_WINDOW_SECONDS = 5f
        const val SILENCE_FLOOR_DBFS = -70f

        /** Centros ERB (Hz), misma escala que RealAcousticSensor (60 Hz–16 kHz). */
        fun erbCenterHz(sampleRate: Float = 48000f): FloatArray {
            val fTop = minOf(16000f, 0.45f * sampleRate)
            val erbRate = { f: Float -> 21.4f * Math.log10(1.0 + 0.00437 * f).toFloat() }
            val erbInv = { r: Float -> ((Math.pow(10.0, (r / 21.4f).toDouble()) - 1.0) / 0.00437).toFloat() }
            val rLo = erbRate(60f)
            val rHi = erbRate(fTop)
            return FloatArray(BANDS) { b ->
                val lo = erbInv(rLo + (rHi - rLo) * b / BANDS)
                val hi = erbInv(rLo + (rHi - rLo) * (b + 1) / BANDS)
                sqrt(lo * hi)
            }
        }
    }

    /** Ventana de agregación en µs (clamp 1–5 s por diseño del consejo). */
    val windowUs: Long =
        (windowSeconds.coerceIn(MIN_WINDOW_SECONDS, MAX_WINDOW_SECONDS) * 1_000_000f).toLong()

    /** Un lote agregado de una ventana (lo que viaja al organismo). */
    data class SessionChunk(
        val sessionId: String,
        val chunkId: Int,
        val startedAtUs: Long,
        val endedAtUs: Long,
        /** Media por banda ERB (dBFS RMS medido). */
        val erbMeanDb: FloatArray,
        /** Desviación estándar por banda del error (dispersión, dB). */
        val erbStdDb: FloatArray,
        /** Error medio por banda (balance tonal medido − target, dB). */
        val errorMeanDb: FloatArray,
        /** Coherencia media mic/referencia por banda [0..1]. */
        val coherenceMean: FloatArray,
        val levelMeanDbfs: Float,
        val levelMaxDbfs: Float,
        val framesValid: Int,
        val framesTotal: Int,
        /** Fracción de frames válidos de la ventana con referencia fresca [0..1] (S1/510). */
        val referenceFreshFrac: Float,
        /** Nivel medio de referencia (media del mic en 250 Hz–2 kHz, dBFS) en la ventana (S1/510). */
        val referenceLevelMeanDbfs: Float,
        /** Frames válidos de la ventana con referencia fresca (conteo, no fracción) (S1/510). */
        val framesReferenceFresh: Int,
        val engineVersion: String
    ) {
        /** Media de coherencia de todas las bandas [0..1]. */
        val meanCoherence: Float get() = coherenceMean.average().toFloat()
        /** RMS del error medio (qué tan lejos del target, dB). */
        val spectralErrorRmsDb: Float
            get() = sqrt(errorMeanDb.sumOf { (it * it).toDouble() } / BANDS).toFloat()
    }

    /** Resumen al cierre de la sesión. */
    data class SessionSummary(
        val sessionId: String,
        val startedAtUs: Long,
        val endedAtUs: Long,
        val chunks: Int,
        val framesValid: Int,
        val framesTotal: Int,
        val levelMeanDbfs: Float
    )

    @Volatile
    var onChunk: ((SessionChunk) -> Unit)? = null

    private val lock = Any()
    private var chunkId = 0
    private var windowStartUs: Long = -1L
    private var windowEndUs: Long = -1L
    private val magSum = FloatArray(BANDS)
    private val errSum = FloatArray(BANDS)
    private val errSqSum = FloatArray(BANDS)
    private val cohSum = FloatArray(BANDS)
    private var levelSum = 0f
    private var levelMax = Float.NEGATIVE_INFINITY
    private var referenceLevelSum = 0f
    private var framesReferenceFresh = 0
    private var windowValid = 0
    private var windowTotal = 0

    private var sessionStartUs: Long = -1L
    private var sessionEndUs: Long = -1L
    private var totalValid = 0
    private var totalFrames = 0
    private var totalLevelSum = 0f
    private var emittedChunks = 0

    val chunksEmitted: Int get() = emittedChunks
    val framesSeen: Int get() = totalFrames
    val framesValidSeen: Int get() = totalValid

    /**
     * Añade UN frame del sensor. Devuelve el chunk emitido si la ventana
     * se cerró con este frame, o null. Los frames inválidos no agregan.
     */
    fun addFrame(frame74: FloatArray, timestampUs: Long, frameIndex: Long): SessionChunk? {
        synchronized(lock) {
            totalFrames++
            if (frame74.size != 76) return null
            if (frame74[73] < 0.5f) {
                windowTotal++
                return null
            }
            if (frame74.any { !it.isFinite() }) {
                windowTotal++
                return null
            }
            val level = frame74[72]
            if (level <= SILENCE_FLOOR_DBFS) {
                windowTotal++
                return null
            }
            if (sessionStartUs < 0) sessionStartUs = timestampUs
            sessionEndUs = timestampUs
            if (windowStartUs < 0) windowStartUs = timestampUs
            windowEndUs = timestampUs
            windowTotal++
            windowValid++
            totalValid++
            totalLevelSum += level
            levelSum += level
            if (level > levelMax) levelMax = level
            // S1/510: frescura de referencia de este frame válido.
            if (frame74[74] >= 0.5f) framesReferenceFresh++
            referenceLevelSum += frame74[75]
            for (b in 0 until BANDS) {
                val mag = frame74[b]
                val err = frame74[24 + b]
                val coh = frame74[48 + b].coerceIn(0f, 1f)
                magSum[b] += mag
                errSum[b] += err
                errSqSum[b] += err * err
                cohSum[b] += coh
            }
            if (windowEndUs - windowStartUs >= windowUs) {
                return emitLocked()
            }
            return null
        }
    }

    /** Cierra la sesión: emite la ventana parcial si tiene frames válidos. */
    fun close(): SessionSummary {
        synchronized(lock) {
            if (windowValid > 0) emitLocked()
            return SessionSummary(
                sessionId = sessionId,
                startedAtUs = if (sessionStartUs < 0) 0L else sessionStartUs,
                endedAtUs = if (sessionEndUs < 0) 0L else sessionEndUs,
                chunks = emittedChunks,
                framesValid = totalValid,
                framesTotal = totalFrames,
                levelMeanDbfs = if (totalValid > 0) totalLevelSum / totalValid else -120f
            )
        }
    }

    private fun emitLocked(): SessionChunk {
        val n = windowValid.toFloat()
        val erbMean = FloatArray(BANDS) { magSum[it] / n }
        val errMean = FloatArray(BANDS) { errSum[it] / n }
        val erbStd = FloatArray(BANDS) { b ->
            val mean = errSum[b] / n
            val variance = (errSqSum[b] / n - mean * mean).coerceAtLeast(0f)
            sqrt(variance)
        }
        val cohMean = FloatArray(BANDS) { (cohSum[it] / n).coerceIn(0f, 1f) }
        val chunk = SessionChunk(
            sessionId = sessionId,
            chunkId = chunkId++,
            startedAtUs = windowStartUs,
            endedAtUs = windowEndUs,
            erbMeanDb = erbMean,
            erbStdDb = erbStd,
            errorMeanDb = errMean,
            coherenceMean = cohMean,
            levelMeanDbfs = if (windowValid > 0) levelSum / n else -120f,
            levelMaxDbfs = if (levelMax.isFinite()) levelMax else -120f,
            framesValid = windowValid,
            framesTotal = windowTotal,
            // S1/510: fracción y media sobre los frames válidos de la
            // ventana; −120 dB si no hay frames válidos. meanCoherence
            // deja de interpretarse sola: su lectura válida exige
            // referenceFreshFrac (ver S1-reference-audit.md §5.4).
            referenceFreshFrac = if (windowValid > 0) framesReferenceFresh.toFloat() / windowValid else 0f,
            referenceLevelMeanDbfs = if (windowValid > 0) referenceLevelSum / n else -120f,
            framesReferenceFresh = framesReferenceFresh,
            engineVersion = engineVersion
        )
        emittedChunks++
        windowStartUs = -1L
        windowEndUs = -1L
        magSum.fill(0f)
        errSum.fill(0f)
        errSqSum.fill(0f)
        cohSum.fill(0f)
        levelSum = 0f
        levelMax = Float.NEGATIVE_INFINITY
        referenceLevelSum = 0f
        framesReferenceFresh = 0
        windowValid = 0
        windowTotal = 0
        onChunk?.invoke(chunk)
        return chunk
    }
}
