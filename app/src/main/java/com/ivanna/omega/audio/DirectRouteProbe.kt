package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import android.media.AudioFormat
import android.util.Log

/**
 * Puerta Hi-Fi USB (0030, v2.5.459): ruta direct_pcm + PCM_24_BIT_PACKED.
 *
 * EL PROBLEMA (verificado en código): el BridgePlayer abre hoy un AudioTrack
 * PCM_FLOAT que AudioPolicy manda al mixPort primary output (PCM_16_BIT @
 * 48 kHz): el mezclador trunca a 16-bit/48k aunque el DAC USB sea 24-bit.
 * La vía hi-res es el mixPort `direct_pcm` (DIRECT): PCM_24_BIT_PACKED
 * 8k–384k, ruta direct_pcm → USB Device Out en el XML, sin mezclador, sin
 * remuestreo de AudioFlinger.
 *
 * EL DAC DE ESSE (verificado en vivo 2026-11-16 13:29 CST, ALSA card 1 =
 * JieLi Technology USB Audio): S24_3LE 24-bit @ 44100/48000 + S16_LE @
 * 44100/48000. `dumpsys media.audio_policy`: "USB Headset Out" anuncia el
 * perfil dinámico PCM_24_BIT_PACKED @ 44100/48000. Meta real: 24-bit/48 kHz.
 * El Riesgo 1 del diseño (el devicePort USB es 16-bit en el XML estático)
 * queda RESUELTO: el perfil DINÁMICO del DAC anuncia 24-bit y la policy lo
 * acepta.
 *
 * ESTE ARCHIVO: lee los formatos que el propio DAC USB anuncia
 * (`AudioDeviceInfo.encodings` + `sampleRates`, API pública 23+) y los
 * convierte a DirectProfile (uno por encoding). La selección (`select`)
 * es pura (sin Android): se testea en host con perfiles falsos. Si algo
 * falla → lista vacía → el BridgePlayer usa la ruta float actual
 * (fail-closed, con log).
 *
 * CORRECCIÓN (2026-11-16, CI 459): la v1 de este archivo usaba
 * `AudioManager.directProfiles` con `AudioDescriptor.device/encodings/
 * sampleRates` — esa API NO EXISTE. El `AudioDescriptor` público es otra
 * clase (descriptor EDID/SADB por bytes, sin device ni tasas) y
 * `getDirectProfilesForAttributes()` devuelve `AudioProfile` SIN device
 * asociado (no se puede filtrar por USB). La CI lo cazó
 * (`compileReleaseKotlin` FAILED). La vía real es el propio
 * `AudioDeviceInfo` del DAC: sus `encodings`/`sampleRates` son los
 * perfiles dinámicos que anuncia el DAC (los mismos que muestra
 * `dumpsys media.audio_policy`), y el fallback del BridgePlayer
 * (track direct no inicializa → float) cubre cualquier exceso.
 *
 * Diseño completo (privado, NO va al repo):
 * ~/workspace/analysis-ivanna/HIRES_RUTA_456_DISENO.md
 */

/** Un perfil directo tal como lo reporta el SO (un encoding, sus tasas, su device). */
data class DirectProfile(
    val encoding: Int,
    val sampleRates: IntArray,
    val deviceType: Int
)

/** La ruta direct elegida: encoding + tasa. */
data class DirectRoute(
    val encoding: Int,
    val sampleRate: Int
)

object DirectRouteProbe {
    private const val TAG = "IVANNA.DirectRouteProbe"

    val USB_DEVICE_TYPES = intArrayOf(
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_ACCESSORY
    )

    // Encodings preferidos, en orden. 24-bit packed primero (el DAC de Esse
    // lo anuncia); 32-bit como fallback (direct_pcm también lo acepta).
    val PREFERRED_ENCODINGS = intArrayOf(
        AudioFormat.ENCODING_PCM_24BIT_PACKED,
        AudioFormat.ENCODING_PCM_32BIT
    )

    // Tope práctico: direct_pcm llega a 384k, el devicePort USB a 192k.
    const val MAX_RATE_HZ = 192_000
    // Mínimo sensato para música (por debajo no es hi-res, es voz).
    const val MIN_RATE_HZ = 44_100

    fun isUsbDeviceType(type: Int): Boolean = type in USB_DEVICE_TYPES

    /**
     * Selección pura (sin Android): dados los perfiles directos que reporta
     * el SO y el tipo del device USB conectado, devuelve (encoding, rate)
     * o null si no hay ruta direct viable para ese DAC.
     *
     * Reglas: solo USB; encoding en orden de preferencia (24-bit packed,
     * luego 32-bit); tasa = la más alta anunciada para ese encoding dentro
     * de [44.1k, 192k]. Si el DAC solo anuncia 16-bit (o nada), null.
     */
    fun select(profiles: List<DirectProfile>, usbDeviceType: Int): DirectRoute? {
        if (!isUsbDeviceType(usbDeviceType)) return null
        val usbProfiles = profiles.filter { it.deviceType == usbDeviceType }
        for (encoding in PREFERRED_ENCODINGS) {
            for (p in usbProfiles) {
                if (p.encoding != encoding) continue
                val rate = p.sampleRates
                    .filter { it in MIN_RATE_HZ..MAX_RATE_HZ }
                    .maxOrNull() ?: continue
                return DirectRoute(encoding, rate)
            }
        }
        return null
    }

    /**
     * Adaptador: lee los formatos que el propio DAC USB anuncia
     * (`AudioDeviceInfo.encodings` + `sampleRates`, API pública 23+; el
     * minSdk del proyecto es 28, sin guarda de versión necesaria) y los
     * convierte a DirectProfile (un DirectProfile por encoding del device).
     * Vacío si el device no es USB, si no reporta formatos o si la
     * lectura falla. Nunca lanza.
     *
     * Las tasas son las del device COMPLETO (no por encoding): `select`
     * toma la más alta en [44.1k, 192k] para el encoding elegido. Si un
     * DAC anunciara 24-bit solo a una tasa baja y 16-bit hasta 192k, se
     * elegiría 192k para 24-bit y el track no inicializaría — el fallback
     * del BridgePlayer (direct → float) lo cubre, fail-closed.
     */
    fun query(usbDevice: AudioDeviceInfo): List<DirectProfile> {
        if (!isUsbDeviceType(usbDevice.type)) return emptyList()
        // OJO: IntArray? NO tiene .orEmpty() en la stdlib (solo String,
        // Collection y Array<T> lo tienen) — se usa ?: intArrayOf().
        // La CI 459 (2º intento) lo cazó: 'Unresolved reference orEmpty'.
        val encodings = runCatching { usbDevice.encodings }.getOrNull() ?: intArrayOf()
        val rates = runCatching { usbDevice.sampleRates }.getOrNull() ?: intArrayOf()
        if (encodings.isEmpty() || rates.isEmpty()) {
            Log.i(TAG, "USB ${usbDevice.productName}: sin encodings/rates reportados")
            return emptyList()
        }
        return encodings.map { DirectProfile(it, rates, usbDevice.type) }
    }
}
