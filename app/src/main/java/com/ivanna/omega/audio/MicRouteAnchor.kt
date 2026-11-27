package com.ivanna.omega.audio

import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.media.AudioRecord
import android.util.Log

/**
 * MicRouteAnchor — el micrófono interno SE QUEDA aunque el DAC USB esté
 * conectado (requisito de Esse, 2026-11-26, v2.5.506).
 *
 * EL PROBLEMA: al conectar el DAC USB (JieLi, salida al Genezi), Android
 * puede enrutar la ENTRADA al USB y el micrófono del teléfono se cancela
 * (o queda en silencio). El bucle cerrado depende de ese micrófono justo
 * cuando suena música por el Genezi: sin mic, Φ cae a APROXIMADO y el
 * organismo no recibe nada.
 *
 * LA GARANTÍA (tres capas, ninguna toca el audio):
 *   1. ANCLA al crear el AudioRecord: `setPreferredDevice` al
 *      `TYPE_BUILTIN_MIC` que devuelve `getDevices(GET_DEVICES_INPUTS)`.
 *      Si no hay mic interno (o el ancla falla), la captura sigue con la
 *      fuente que Android elija (UNPROCESSED/MIC) y el estado lo reporta
 *      — nunca se bloquea la captura por el ancla.
 *   2. VERIFICACIÓN periódica en el hilo de captura (~1 s):
 *      `getRoutedDevice()` debe seguir siendo el mic interno. Si la ruta
 *      se fue al USB (o a otro lado), se RE-ANCLA automáticamente y se
 *      cuenta en [RouteStatus.reanchorCount] (visible en el indicador
 *      del organismo).
 *   3. REPORTE: [RouteStatus] dice en cada momento qué mic está
 *      enrutado; el frame del sensor sigue siendo la verdad final
 *      (valid=true + ERB medido = mic vivo con música sonando).
 *
 * La lógica vive sobre [MicDevicePort] (interfaz fina, sin Android en la
 * firma salvo tipos planos) para que los tests unitarios la ejerciten con
 * un puerto falso, sin Robolectric ni hardware.
 */
object MicRouteAnchor {
    private const val TAG = "MicRouteAnchor"

    /** Representación plana de un dispositivo de entrada (testeable en JVM). */
    data class MicDevice(val type: Int, val id: Int, val name: String) {
        val isBuiltinMic: Boolean get() = type == AudioDeviceInfo.TYPE_BUILTIN_MIC
        val isUsb: Boolean
            get() = type == AudioDeviceInfo.TYPE_USB_DEVICE ||
                type == AudioDeviceInfo.TYPE_USB_HEADSET
    }

    /** Puerto fino sobre AudioRecord + AudioManager. */
    interface MicDevicePort {
        /** Mic interno, o null si el dispositivo no expone ninguno. */
        fun builtinMic(): MicDevice?

        /** Fija el dispositivo preferido (null = limpiar preferencia). */
        fun setPreferred(device: MicDevice?): Boolean

        /** Dispositivo preferido actual, o null si no hay. */
        fun preferredDevice(): MicDevice?

        /** Dispositivo por el que REALMENTE está entrando el audio. */
        fun routedDevice(): MicDevice?
    }

    /** Implementación real sobre las APIs de Android (minSdk 33). */
    class AndroidMicDevicePort(
        private val record: AudioRecord,
        private val audioManager: AudioManager
    ) : MicDevicePort {
        private fun AudioDeviceInfo.toMicDevice(): MicDevice =
            MicDevice(type, id, productName?.toString() ?: "mic")

        private fun inputDevices(): List<AudioDeviceInfo> = runCatching {
            audioManager.getDevices(AudioManager.GET_DEVICES_INPUTS).toList()
        }.getOrElse { emptyList() }

        override fun builtinMic(): MicDevice? =
            inputDevices().firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_MIC }
                ?.toMicDevice()

        override fun setPreferred(device: MicDevice?): Boolean {
            if (device == null) return runCatching { record.setPreferredDevice(null) }.getOrDefault(false)
            val info = inputDevices().firstOrNull { it.id == device.id } ?: return false
            return runCatching { record.setPreferredDevice(info) }.getOrDefault(false)
        }

        override fun preferredDevice(): MicDevice? =
            runCatching { record.preferredDevice?.toMicDevice() }.getOrNull()

        override fun routedDevice(): MicDevice? =
            runCatching { record.routedDevice?.toMicDevice() }.getOrNull()
    }

    sealed interface AnchorOutcome {
        data class Anchored(val device: MicDevice) : AnchorOutcome
        data object NoBuiltinMic : AnchorOutcome
        data class AnchorFailed(val reason: String) : AnchorOutcome
    }

    sealed interface VerifyOutcome {
        data class Ok(val device: MicDevice?) : VerifyOutcome
        data class Drifted(val device: MicDevice?) : VerifyOutcome
        /** Ruta desconocida: no se pudo leer el dispositivo enrutado. */
        data object Unknown : VerifyOutcome
    }

    /** Estado vivo de la ruta del mic, para el indicador del organismo. */
    data class RouteStatus(
        val anchored: Boolean = false,
        val routedType: Int = AudioDeviceInfo.TYPE_UNKNOWN,
        val routedTypeName: String = "desconocido",
        val reanchorCount: Int = 0,
        val lastCheckMs: Long = 0L,
        val lastError: String? = null
    )

    fun deviceTypeName(type: Int): String = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC -> "Mic interno"
        AudioDeviceInfo.TYPE_USB_DEVICE -> "Mic USB"
        AudioDeviceInfo.TYPE_USB_HEADSET -> "Mic USB (auricular)"
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> "Mic Bluetooth"
        AudioDeviceInfo.TYPE_WIRED_HEADSET -> "Mic auricular cableado"
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> "Auricular interno"
        AudioDeviceInfo.TYPE_UNKNOWN -> "desconocido"
        else -> "mic tipo $type"
    }

    /**
     * Ancla el mic interno como dispositivo preferido. No lanza: cualquier
     * fallo se devuelve como [AnchorOutcome] y la captura SIGUE con la
     * fuente que Android elija (el ancla nunca bloquea el audio).
     * Acepta el ancla si `setPreferredDevice` devolvió true Y el
     * dispositivo preferido leído es el mic interno.
     */
    fun anchor(port: MicDevicePort): AnchorOutcome {
        val builtin = runCatching { port.builtinMic() }.getOrNull()
            ?: return AnchorOutcome.NoBuiltinMic
        val setOk = runCatching { port.setPreferred(builtin) }.getOrDefault(false)
        if (!setOk) {
            Log.w(TAG, "setPreferredDevice(mic interno) devolvió false")
            return AnchorOutcome.AnchorFailed("setPreferredDevice=false")
        }
        val preferred = runCatching { port.preferredDevice() }.getOrNull()
        if (preferred == null || !preferred.isBuiltinMic) {
            Log.w(TAG, "preferencia leída no es mic interno: $preferred")
            return AnchorOutcome.AnchorFailed("preferred=$preferred")
        }
        Log.i(TAG, "Mic anclado al interno: ${builtin.name} (id=${builtin.id})")
        return AnchorOutcome.Anchored(builtin)
    }

    /**
     * Verificación periódica (~1 s, hilo de captura): ¿sigue entrando el
     * audio por el mic interno? [VerifyOutcome.Drifted] = la ruta se fue
     * (típico: al USB al conectar el DAC) y hay que re-anclar.
     * Un routedDevice null (aún sin rutear o no legible) es DESCONOCIDO
     * (defecto #6): NO es Ok — el llamador conserva el último estado
     * conocido y no afirma una ruta que no ha visto.
     */
    fun verify(port: MicDevicePort): VerifyOutcome {
        val routed = runCatching { port.routedDevice() }.getOrNull()
        if (routed == null) return VerifyOutcome.Unknown
        return if (routed.isBuiltinMic) VerifyOutcome.Ok(routed)
        else VerifyOutcome.Drifted(routed)
    }

    /**
     * Re-ancla tras una deriva y devuelve el estado actualizado. Si el
     * re-ancla falla, el estado conserva el error y la captura SIGUE:
     * el sensor reportará frames inválidos y Φ fallará cerrado.
     */
    fun reanchor(port: MicDevicePort, previous: RouteStatus, nowMs: Long): RouteStatus {
        val outcome = anchor(port)
        val routed = runCatching { port.routedDevice() }.getOrNull()
        return when (outcome) {
            is AnchorOutcome.Anchored -> previous.copy(
                anchored = true,
                routedType = routed?.type ?: AudioDeviceInfo.TYPE_BUILTIN_MIC,
                routedTypeName = deviceTypeName(routed?.type ?: AudioDeviceInfo.TYPE_BUILTIN_MIC),
                reanchorCount = previous.reanchorCount + 1,
                lastCheckMs = nowMs,
                lastError = null
            )
            is AnchorOutcome.AnchorFailed -> previous.copy(
                anchored = false, lastCheckMs = nowMs, lastError = outcome.reason
            )
            AnchorOutcome.NoBuiltinMic -> previous.copy(
                anchored = false, lastCheckMs = nowMs,
                lastError = "sin mic interno"
            )
        }
    }
}
