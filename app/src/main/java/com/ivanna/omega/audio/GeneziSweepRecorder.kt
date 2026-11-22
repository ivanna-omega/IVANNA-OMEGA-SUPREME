package com.ivanna.omega.audio

import android.content.ContentUris
import android.content.ContentValues
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.os.Environment
import android.provider.MediaStore
import android.util.Log
import com.ivanna.omega.core.IvannaNativeLib
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * GeneziSweepRecorder — graba el barrido de calibración del Genezi (§5, v2.5.473).
 *
 * Secuencia por nivel (lo y hi), UNA sola reproducción continua + UNA sola
 * grabación continua, mismo patrón exacto que [RoomMeasurement]:
 *   1.0 s silencio → piso de ruido
 *   0.3 s ráfaga de 1 kHz → sincronía: mide la LATENCIA REAL de la salida
 *   0.5 s gap
 *   ESS completo (10 s, 20 Hz–20 kHz, 48 kHz mono int16) → la respuesta
 *   2.0 s cola → margen para la latencia de la salida
 *
 * Los ESS ya existen en el teléfono (`ess_lo_48k.wav` amp 0.18 = −14.9 dBFS,
 * `ess_hi_48k.wav` amp 0.36 = −8.9 dBFS, +6 dB exactos, fades 20 ms).
 * Salen `genezi_rec_lo.wav`, `genezi_rec_hi.wav` y `genezi_sweep_log.json`
 * en Download (vía MediaStore), y el resumen en prefs "genezi_sweep"
 * (lo lee la línea "Último barrido" de FUSIÓN · SALA VIVA).
 *
 * Rechazo igual que RoomMeasurement: saturación ≥ 0.98 y ráfaga < 12 dB
 * sobre el ruido. NO toca HAL/daemon/C++: solo el JNI existente
 * `nativeRoomLockIn` (detección síncrona de la ráfaga).
 *
 * El paréntesis neutro (FUSIÓN neutra, bypass DSPState, sala OFF, pausa de
 * reproductor y sensor) lo hace el LLAMADOR (ControlTabScreen.startSweep)
 * y lo restaura en `finally`: este objeto solo hace audio + archivos.
 */
object GeneziSweepRecorder {
    private const val TAG = "GeneziSweepRecorder"
    const val SAMPLE_RATE = 48_000
    private const val SILENCE_S = 1.0f
    private const val BURST_S = 0.3f
    private const val BURST_GAP_S = 0.5f
    private const val TAIL_S = 2.0f
    const val ESS_LO_NAME = "ess_lo_48k.wav"
    const val ESS_HI_NAME = "ess_hi_48k.wav"
    const val REC_LO_NAME = "genezi_rec_lo.wav"
    const val REC_HI_NAME = "genezi_rec_hi.wav"
    const val LOG_NAME = "genezi_sweep_log.json"
    const val PREFS_NAME = "genezi_sweep"

    sealed class Outcome {
        data class Success(
            val latencyLoMs: Int,
            val latencyHiMs: Int,
            val peakDbfs: Float,
            val files: List<String>
        ) : Outcome()

        data class Failure(val reason: String) : Outcome()
    }

    private fun s2n(sec: Float): Int = (sec * SAMPLE_RATE).toInt()
    private fun db(x: Float): Float = 20f * log10(max(x, 1e-9f))

    /** Precondiciones (§5): DAC USB conectado + salida Genezi + ESS presentes. null = todo bien. */
    fun precheck(context: Context): String? {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            ?: return "Sin AudioManager"
        val types = am.getDevices(AudioManager.GET_DEVICES_OUTPUTS).map { it.type }.toIntArray()
        // FIX 2.5.474 — El adaptador DAC JieLi (VID/PID 3654:5678) se reporta
        // en Android como TYPE_USB_HEADSET (usb_headset), NO como
        // TYPE_USB_DEVICE/TYPE_USB_ACCESSORY: hasUsbDac() no lo veía y el
        // barrido fallaba con "Conecta el DAC USB" aunque el DAC estaba
        // conectado (verificado: lsusb 3654:5678 + dumpsys usb_headset).
        // Para el barrido Genezi el DAC es el JieLi, así que USB_HEADSET
        // también cuenta como DAC aquí. No se toca hasUsbDac() (el ruteo
        // general sigue distinguiendo audífono USB puro por precedencia).
        if (!RouteBDeviceGate.hasUsbDac(types) &&
            !types.contains(AudioDeviceInfo.TYPE_USB_HEADSET))
            return "Conecta el DAC USB (JieLi) antes de grabar el barrido"
        val mode = TransducerProfileFactory.resolveMode(types, UsbDacTarget.SPEAKERS)
        if (mode != TransducerMode.SPEAKERS_GENEZI)
            return "La salida no es Genezi (modo $mode): pon la salida en bocinas"
        if (loadEss(context, ESS_LO_NAME) == null)
            return "Falta $ESS_LO_NAME en Download"
        if (loadEss(context, ESS_HI_NAME) == null)
            return "Falta $ESS_HI_NAME en Download"
        return null
    }

    /** Ejecuta lo+hi. El llamador debe tener RECORD_AUDIO concedido. */
    suspend fun run(context: Context, onProgress: (Float) -> Unit): Outcome = withContext(Dispatchers.IO) {
        precheck(context)?.let { return@withContext Outcome.Failure(it) }
        if (!IvannaNativeLib.isLoaded) return@withContext Outcome.Failure("El motor nativo no está cargado")
        val essLo = loadEss(context, ESS_LO_NAME) ?: return@withContext Outcome.Failure("No se pudo leer $ESS_LO_NAME")
        val essHi = loadEss(context, ESS_HI_NAME) ?: return@withContext Outcome.Failure("No se pudo leer $ESS_HI_NAME")

        val recLo = when (val r = recordLevel(context, essLo) { onProgress(it * 0.45f) }) {
            is LevelResult.Ok -> r.samples
            is LevelResult.Fail -> return@withContext Outcome.Failure("lo: ${r.reason}")
        }
        val latLo = analyzeBurst(recLo).let {
            if (it is BurstResult.Fail) return@withContext Outcome.Failure("lo: ${it.reason}")
            (it as BurstResult.Ok).latencyMs
        }
        val recHi = when (val r = recordLevel(context, essHi) { onProgress(0.45f + it * 0.45f) }) {
            is LevelResult.Ok -> r.samples
            is LevelResult.Fail -> return@withContext Outcome.Failure("hi: ${r.reason}")
        }
        val latHi = analyzeBurst(recHi).let {
            if (it is BurstResult.Fail) return@withContext Outcome.Failure("hi: ${it.reason}")
            (it as BurstResult.Ok).latencyMs
        }
        var peak = 0f
        for (v in recLo) peak = max(peak, abs(v))
        for (v in recHi) peak = max(peak, abs(v))
        val peakDbfs = db(peak)

        // Archivos: grabaciones + log JSON (MediaStore, Download).
        val written = ArrayList<String>()
        if (!writeWav16(context, REC_LO_NAME, recLo)) return@withContext Outcome.Failure("No se pudo escribir $REC_LO_NAME")
        written += REC_LO_NAME
        if (!writeWav16(context, REC_HI_NAME, recHi)) return@withContext Outcome.Failure("No se pudo escribir $REC_HI_NAME")
        written += REC_HI_NAME
        val stamp = SimpleDateFormat("yyyy-MM-dd HH:mm", Locale.ROOT).format(Date())
        val log = org.json.JSONObject()
            .put("date", stamp)
            .put("source", "genezi_sweep_v1")
            .put("ess_lo", ESS_LO_NAME)
            .put("ess_hi", ESS_HI_NAME)
            .put("rec_lo", REC_LO_NAME)
            .put("rec_hi", REC_HI_NAME)
            .put("latency_lo_ms", latLo)
            .put("latency_hi_ms", latHi)
            .put("peak_dbfs", peakDbfs.toDouble())
            .put("sample_rate", SAMPLE_RATE)
            .toString()
        if (!writeText(context, LOG_NAME, log)) return@withContext Outcome.Failure("No se pudo escribir $LOG_NAME")
        written += LOG_NAME
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE).edit()
            .putString("date", stamp)
            .putString("files", "$REC_LO_NAME + $REC_HI_NAME")
            .putFloat("peak_db", peakDbfs)
            .putInt("latency_lo_ms", latLo)
            .putInt("latency_hi_ms", latHi)
            .apply()
        onProgress(1f)
        Outcome.Success(latLo, latHi, peakDbfs, written)
    }

    // ── Un nivel: preámbulo + ESS, una reproducción + una grabación ────────
    private sealed class LevelResult {
        class Ok(val samples: FloatArray) : LevelResult()
        class Fail(val reason: String) : LevelResult()
    }

    private suspend fun recordLevel(
        context: Context,
        ess: FloatArray,
        onProgress: (Float) -> Unit
    ): LevelResult = withContext(Dispatchers.IO) {
        val seq = buildSequence(ess)
        val rec = FloatArray(seq.size + s2n(TAIL_S))
        val minRec = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT)
        val minTrk = AudioTrack.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_FLOAT)
        if (minRec <= 0 || minTrk <= 0) return@withContext LevelResult.Fail("Este teléfono no admite la medición (formato de audio)")

        var recorder: AudioRecord? = null
        var track: AudioTrack? = null
        try {
            recorder = try {
                AudioRecord(MediaRecorder.AudioSource.UNPROCESSED, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_FLOAT, max(minRec * 4, SAMPLE_RATE * 4))
            } catch (t: Throwable) { null }
            if (recorder == null || recorder.state != AudioRecord.STATE_INITIALIZED) {
                recorder?.release()
                recorder = AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_FLOAT, max(minRec * 4, SAMPLE_RATE * 4))
            }
            val rcd: AudioRecord = recorder ?: return@withContext LevelResult.Fail("No se pudo abrir el micrófono")
            if (rcd.state != AudioRecord.STATE_INITIALIZED) return@withContext LevelResult.Fail("No se pudo abrir el micrófono")
            // FIX 2.5.492 — Forzar el mic integrado aunque el DAC USB esté
            // conectado. Con el DAC JieLi puesto, Android rutea la entrada
            // al mic del audífono USB (vacío: el aux no trae mic) y la
            // grabación sale en silencio. Se fuerza TYPE_BUILTIN_MIC como
            // dispositivo preferido; si no hay mic integrado o falla, se
            // sigue con el ruteo por defecto (sin romper lo actual).
            forceBuiltinMic(context, rcd)

            val trk: AudioTrack = AudioTrack.Builder()
                .setAudioAttributes(AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
                .setAudioFormat(AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                    .setSampleRate(SAMPLE_RATE)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                .setBufferSizeInBytes(max(minTrk * 2, SAMPLE_RATE * 2))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .build()
            track = trk
            rcd.startRecording()
            trk.play()
            val writer = launch {
                var off = 0
                while (off < seq.size && isActive) {
                    val n = trk.write(seq, off, min(4800, seq.size - off), AudioTrack.WRITE_BLOCKING)
                    if (n <= 0) break
                    off += n
                }
            }
            var got = 0
            while (got < rec.size && isActive) {
                val n = rcd.read(rec, got, min(4800, rec.size - got), AudioRecord.READ_BLOCKING)
                if (n <= 0) break
                got += n
                onProgress(got.toFloat() / rec.size)
            }
            writer.join()
            if (got < rec.size) return@withContext LevelResult.Fail("La grabación se interrumpió")
        } catch (t: Throwable) {
            Log.w(TAG, "barrido falló: ${t.message}")
            return@withContext LevelResult.Fail("Error de audio: ${t.message}")
        } finally {
            runCatching { recorder?.stop() }; runCatching { recorder?.release() }
            runCatching { track?.stop() }; runCatching { track?.release() }
        }
        // Rechazo por saturación (igual que RoomMeasurement.analyze).
        var peak = 0f
        for (v in rec) peak = max(peak, abs(v))
        if (peak >= 0.98f) return@withContext LevelResult.Fail("El micrófono saturó: baja el volumen del Genezi y repite")
        LevelResult.Ok(rec)
    }

    /** Fuerza el micrófono integrado como entrada preferida del AudioRecord.
     * Sin esto, con el DAC USB conectado Android rutea la entrada al mic del
     * audífono USB (vacío) y la grabación sale en silencio. Devuelve true si
     * el mic integrado quedó como preferido; false si no se pudo (entonces el
     * ruteo por defecto sigue vigente, sin romper nada). */
    private fun forceBuiltinMic(context: Context, recorder: AudioRecord): Boolean {
        return try {
            val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
                ?: return false
            val builtin = am.getDevices(AudioManager.GET_DEVICES_INPUTS)
                .firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_MIC }
                ?: return false
            val ok = recorder.setPreferredDevice(builtin)
            Log.i(TAG, "mic integrado forzado: $ok (${builtin.productName})")
            ok
        } catch (t: Throwable) {
            Log.w(TAG, "no se pudo forzar el mic integrado: ${t.message}")
            false
        }
    }

    /** Silencio 1.0 s + ráfaga 1 kHz 0.3 s + gap 0.5 s + ESS. */
    private fun buildSequence(ess: FloatArray): FloatArray {
        val total = s2n(SILENCE_S) + s2n(BURST_S) + s2n(BURST_GAP_S) + ess.size
        val out = FloatArray(total)
        val fade = s2n(0.02f)
        val burstStart = s2n(SILENCE_S)
        val burstLen = s2n(BURST_S)
        for (i in 0 until burstLen) {
            val env = when {
                i < fade -> i / fade.toFloat()
                i > burstLen - fade -> (burstLen - i) / fade.toFloat()
                else -> 1f
            }
            out[burstStart + i] = 0.2f * env * sin(2.0 * PI * 1000f * i / SAMPLE_RATE).toFloat()
        }
        ess.copyInto(out, s2n(SILENCE_S) + s2n(BURST_S) + s2n(BURST_GAP_S))
        return out
    }

    // ── Detección de la ráfaga (mismo algoritmo que RoomMeasurement.analyze) ──
    private sealed class BurstResult {
        class Ok(val latencyMs: Int) : BurstResult()
        class Fail(val reason: String) : BurstResult()
    }

    private fun analyzeBurst(rec: FloatArray): BurstResult {
        val win = s2n(0.02f); val hop = s2n(0.01f)
        val noiseWin = rec.copyOfRange(s2n(0.1f), s2n(0.9f))
        val noise1k = IvannaNativeLib.nativeRoomLockIn(noiseWin, noiseWin.size, SAMPLE_RATE, 1000f)
        var maxLevel = 0f
        val env = ArrayList<Float>()
        var p = 0
        while (p + win <= rec.size - s2n(0.3f)) {
            val seg = rec.copyOfRange(p, p + win)
            val a = IvannaNativeLib.nativeRoomLockIn(seg, seg.size, SAMPLE_RATE, 1000f)
            env.add(a); maxLevel = max(maxLevel, a)
            p += hop
        }
        if (maxLevel < 1e-4f || db(maxLevel) - db(noise1k) < 12f)
            return BurstResult.Fail("No se oyó la señal de prueba: sube un poco el volumen o acerca el teléfono a las bocinas")
        val thr = max(noise1k * 4f, maxLevel * 0.3f)
        val onsetIdx = env.indexOfFirst { it >= thr }
        if (onsetIdx < 0) return BurstResult.Fail("No se detectó el inicio de la señal de prueba")
        val onset = onsetIdx * hop
        val shift = onset - s2n(SILENCE_S)
        val latencyMs = (shift * 1000L / SAMPLE_RATE).toInt()
        if (shift < -s2n(0.05f) || shift > s2n(TAIL_S)) return BurstResult.Fail("Latencia fuera de rango ($latencyMs ms)")
        return BurstResult.Ok(latencyMs)
    }

    // ── ESS: leer WAV 48 kHz mono int16 de Download (MediaStore, fallback File) ──
    internal fun loadEss(context: Context, name: String): FloatArray? {
        val bytes = readDownloadBytes(context, name) ?: return null
        return parseWav16Mono48k(bytes)
    }

    private fun readDownloadBytes(context: Context, name: String): ByteArray? {
        // 1) MediaStore (Download).
        runCatching {
            val uri = MediaStore.Downloads.EXTERNAL_CONTENT_URI
            context.contentResolver.query(
                uri, arrayOf(MediaStore.Downloads._ID),
                "${MediaStore.Downloads.DISPLAY_NAME} = ?", arrayOf(name), null
            )?.use { c ->
                if (c.moveToFirst()) {
                    val id = c.getLong(0)
                    context.contentResolver.openInputStream(ContentUris.withAppendedId(uri, id))?.use {
                        return it.readBytes()
                    }
                }
            }
        }
        // 2) Fallback: archivo directo en Download.
        runCatching {
            val f = File(Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS), name)
            if (f.isFile) return f.readBytes()
        }
        return null
    }

    /** WAV PCM int16, 1 canal, 48 kHz → FloatArray −1..1. null si no coincide. */
    internal fun parseWav16Mono48k(bytes: ByteArray): FloatArray? {
        if (bytes.size < 44) return null
        fun tag(off: Int): String = String(bytes, off, 4, Charsets.US_ASCII)
        if (tag(0) != "RIFF" || tag(8) != "WAVE") return null
        var pos = 12
        var channels = -1; var sampleRate = -1; var bits = -1
        var dataOff = -1; var dataLen = -1
        while (pos + 8 <= bytes.size) {
            val id = tag(pos)
            val len = ByteBuffer.wrap(bytes, pos + 4, 4).order(ByteOrder.LITTLE_ENDIAN).int
            if (len < 0 || pos + 8 + len > bytes.size) return null
            when (id) {
                "fmt " -> {
                    if (len < 16) return null
                    val bb = ByteBuffer.wrap(bytes, pos + 8, 16).order(ByteOrder.LITTLE_ENDIAN)
                    val audioFormat = bb.short.toInt()
                    channels = bb.short.toInt()
                    sampleRate = bb.int
                    bb.int // byteRate
                    bb.short // blockAlign
                    bits = bb.short.toInt()
                    if (audioFormat != 1) return null
                }
                "data" -> { dataOff = pos + 8; dataLen = len }
            }
            pos += 8 + len + (len % 2)
        }
        if (channels != 1 || sampleRate != SAMPLE_RATE || bits != 16 || dataOff < 0) return null
        val n = dataLen / 2
        val out = FloatArray(n)
        val bb = ByteBuffer.wrap(bytes, dataOff, dataLen).order(ByteOrder.LITTLE_ENDIAN)
        for (i in 0 until n) out[i] = bb.short / 32768f
        return out
    }

    // ── Escritura a Download vía MediaStore ────────────────────────────────
    private fun deleteDownload(context: Context, name: String) {
        runCatching {
            val uri = MediaStore.Downloads.EXTERNAL_CONTENT_URI
            context.contentResolver.query(
                uri, arrayOf(MediaStore.Downloads._ID),
                "${MediaStore.Downloads.DISPLAY_NAME} = ?", arrayOf(name), null
            )?.use { c ->
                while (c.moveToNext()) {
                    context.contentResolver.delete(ContentUris.withAppendedId(uri, c.getLong(0)), null, null)
                }
            }
        }
    }

    private fun writeDownload(context: Context, name: String, mime: String, write: (java.io.OutputStream) -> Unit): Boolean {
        return try {
            deleteDownload(context, name)
            val values = ContentValues().apply {
                put(MediaStore.Downloads.DISPLAY_NAME, name)
                put(MediaStore.Downloads.MIME_TYPE, mime)
                put(MediaStore.Downloads.RELATIVE_PATH, Environment.DIRECTORY_DOWNLOADS)
            }
            val uri = context.contentResolver.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, values)
                ?: return false
            context.contentResolver.openOutputStream(uri)?.use { write(it) } ?: return false
            true
        } catch (t: Throwable) {
            Log.w(TAG, "no se pudo escribir $name: ${t.message}")
            false
        }
    }

    /** WAV PCM int16, 1 canal, 48 kHz, cabecera RIFF de 44 bytes. */
    private fun writeWav16(context: Context, name: String, samples: FloatArray): Boolean {
        return writeDownload(context, name, "audio/wav") { out ->
            val dataBytes = samples.size * 2
            val header = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN)
            header.put("RIFF".toByteArray(Charsets.US_ASCII))
            header.putInt(36 + dataBytes)
            header.put("WAVE".toByteArray(Charsets.US_ASCII))
            header.put("fmt ".toByteArray(Charsets.US_ASCII))
            header.putInt(16)
            header.putShort(1) // PCM
            header.putShort(1) // mono
            header.putInt(SAMPLE_RATE)
            header.putInt(SAMPLE_RATE * 2) // byteRate
            header.putShort(2) // blockAlign
            header.putShort(16) // bits
            header.put("data".toByteArray(Charsets.US_ASCII))
            header.putInt(dataBytes)
            out.write(header.array())
            val bb = ByteBuffer.allocate(8192).order(ByteOrder.LITTLE_ENDIAN)
            var i = 0
            while (i < samples.size) {
                bb.clear()
                val chunk = min(4096, samples.size - i)
                for (k in 0 until chunk) {
                    val v = (samples[i + k].coerceIn(-1f, 1f) * 32767f).toInt()
                    bb.putShort(v.toShort())
                }
                out.write(bb.array(), 0, bb.position())
                i += chunk
            }
        }
    }

    private fun writeText(context: Context, name: String, text: String): Boolean {
        return writeDownload(context, name, "application/json") { out ->
            out.write(text.toByteArray(Charsets.UTF_8))
        }
    }
}
