package com.ivanna.omega.audio

import android.util.Log
import com.ivanna.omega.core.NativeLibraryLoader

/**
 * Downsampler sinc polifásico NATIVO para MÚSICA (Fase 4, Ley 3).
 *
 * Reemplaza al remuestreador Kotlin de 64 taps (StereoAudioResampler,
 * Blackman-Harris, ~-92 dB) SOLO para la conversión de tasa entera
 * descendente (192k→48k, 96k→48k, 88.2k→44.1k): sinc de fase lineal con
 * ventana Kaiser, diseño float64, rechazo de alias MEDIDO >130 dB
 * (prototipo: -145 dB a 24 kHz, -163 dB a 30 kHz para 192k→48k).
 *
 * Alcance exacto (no reescribe el ASRC ni el resampler Kotlin):
 *  - Conversión de tasa entera descendente (ratio 2..8). El ASRC de reloj
 *    (RouteAAsrc) sigue siendo una etapa SEPARADA y posterior; el servo
 *    PLL sigue alimentándolo (o a nadie, con el bypass 0033).
 *  - Tasa igual (48k→48k): NO pasa por aquí. El passthrough bit-perfect
 *    ya existe en StereoAudioResampler (inputRate==targetRate y ppm==0
 *    devuelve la entrada sin tocarla; el bypass 0033 deja ppm en 0).
 *  - Ratio no entero o nativo no disponible: fallback al remuestreador
 *    Kotlin (comportamiento actual; el audio nunca se rompe).
 *
 * Una instancia por reproducción (estado FIR + historial son locales).
 * process() devuelve un FloatArray intercalado nuevo por bloque (misma
 * convención que StereoAudioResampler.process); flush() drena la cola de
 * latencia al final del stream.
 *
 * Fail-closed: si la librería nativa no carga o la creación falla,
 * isAvailable=false y process() devuelve la entrada sin tocarla
 * (passthrough). El llamador (IvannaBridgePlayer) debe usar el
 * remuestreador Kotlin en ese caso.
 */
class NativeSincDownsampler {

    companion object {
        private const val TAG = "IVANNA_SINC_DOWNSAMPLER"
    }

    private val loaded = NativeLibraryLoader.ensureLoaded()

    private var handle: Long = 0L
    private var configuredRate: Int = 0
    private var outScratch = FloatArray(0)

    init {
        if (loaded) {
            handle = nativeCreate()
            if (handle == 0L) Log.w(TAG, "nativeCreate falló (handle 0)")
        }
    }

    /** true si el nativo está cargado y la instancia se creó. */
    val isAvailable: Boolean
        get() = loaded && handle != 0L

    /**
     * Configura el downsampler para inputRate→outputRate (ratio entero).
     * Devuelve true si el ratio es entero (2..8) y el filtro se diseñó.
     * Reconfigurar descarta el historial (silencio).
     */
    fun configure(inputRate: Int, outputRate: Int): Boolean {
        if (!isAvailable) return false
        val ok = nativeConfigure(handle, inputRate, outputRate)
        if (ok) configuredRate = outputRate
        return ok
    }

    /** Número de taps del filtro diseñado (0 si no está configurado). */
    val taps: Int
        get() = if (!isAvailable) 0 else nativeTaps(handle)

    /** Retardo de grupo en frames de entrada (0 si no está configurado). */
    val latencyFrames: Int
        get() = if (!isAvailable) 0 else nativeLatencyFrames(handle)

    /**
     * Downsamplea un bloque estéreo intercalado. Devuelve el bloque a la
     * tasa de salida (intercalado). Si no está disponible o no está
     * configurado, devuelve la entrada sin tocarla (passthrough) y el
     * llamador debe usar el remuestreador Kotlin.
     */
    fun process(input: FloatArray): FloatArray {
        if (!isAvailable || configuredRate == 0) return input
        val completeInput = if (input.size % 2 == 0) input else input.copyOf(input.size - 1)
        if (completeInput.isEmpty()) return FloatArray(0)
        val inFrames = completeInput.size / 2
        val maxOutFrames = inFrames + 16
        if (outScratch.size < maxOutFrames * 2) outScratch = FloatArray(maxOutFrames * 2)
        val produced = nativeProcess(handle, completeInput, inFrames, outScratch, maxOutFrames)
        if (produced <= 0) return FloatArray(0)
        return outScratch.copyOf(produced * 2)
    }

    /**
     * Drena la cola de latencia al final del stream. Devuelve los frames
     * restantes (intercalado). Llamar una vez por pista.
     */
    fun flush(): FloatArray {
        if (!isAvailable || configuredRate == 0) return FloatArray(0)
        val maxOutFrames = latencyFrames + 16
        if (outScratch.size < maxOutFrames * 2) outScratch = FloatArray(maxOutFrames * 2)
        val produced = nativeFlush(handle, outScratch, maxOutFrames)
        if (produced <= 0) return FloatArray(0)
        return outScratch.copyOf(produced * 2)
    }

    /** Limpia el historial (silencio) sin reconfigurar el filtro. */
    fun reset() {
        if (!isAvailable) return
        nativeReset(handle)
    }

    fun close() {
        if (handle != 0L) {
            nativeDestroy(handle)
            handle = 0L
        }
        configuredRate = 0
    }

    private external fun nativeCreate(): Long
    private external fun nativeDestroy(handle: Long)
    private external fun nativeConfigure(handle: Long, inputRate: Int, outputRate: Int): Boolean
    private external fun nativeReset(handle: Long)
    private external fun nativeProcess(handle: Long, input: FloatArray, inFrames: Int, output: FloatArray, outCap: Int): Int
    private external fun nativeFlush(handle: Long, output: FloatArray, outCap: Int): Int
    private external fun nativeTaps(handle: Long): Int
    private external fun nativeLatencyFrames(handle: Long): Int
}
