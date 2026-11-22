// SignatureRooms.kt — v2.5.472 Fase 2 (Sala Viva): catálogo de salas firma.
//
// Las 3 salas son MEDICIONES free-field (anecoicas) del catálogo SOFA: su
// T60 real está bajo el piso de clamp de ListenerRoomControls (0.05 s), así
// que el rt60Sec aplicado es 0.05 en las 3 y el valor medido va documentado
// en measuredRt60Ms. roomSizeM = clamp(r medido, 2, 12) m. roomWet es la
// fracción de energía reverberante medida (ventana directa 2.5 ms, frontal).
//
// La medición de la habitación con micrófono es OTRO paso (necesita al
// dueño): el código está listo para recibir una RIR medida —
// HrtfManager::installMeasuredRoomRir() la guarda como sala instalada
// (slot de staging; nada la convoluciona aún).
//
// Neutro bit-exacto: signatureRoom = 0 es el comportamiento anterior
// (sin sala firma). Espejo exacto del C++ (ListenerRoomControls::
// signatureRoom, fused_controls.hpp) y de HrtfManager::setSignatureRoom,
// que busca estos mismos archivos .sofa en el dispositivo.
package com.ivanna.omega.audio

/**
 * Una sala firma: un SOFA medido + los parámetros de sala que aplica.
 *
 * @param id 1..3 (0 = ninguna, no está en la lista).
 * @param sofaFile nombre del .sofa en app/src/main/assets/ivanna_omega/sofa/
 *   (la APK/el módulo lo extraen al dispositivo; ver HrtfManager).
 * @param rt60Sec RT60 aplicado (s): 0.05 en las 3 (piso de clamp; el T60
 *   real medido está por debajo y va en [measuredRt60Ms]).
 * @param roomSizeM tamaño de sala aplicado (m): clamp(r medido, 2, 12).
 * @param roomWet wet RIR aplicado: fracción de energía reverberante medida.
 * @param measuredRt60Ms T60 medido (Schroeder -5..-35 dB, frontal, ms).
 * @param measuredDistanceM radio de medición (m).
 * @param measuredWet fracción de energía reverberante medida (frontal).
 * @param measuredDrrDb DRR medido, directa/reverberante (dB, frontal).
 * @param sampleRateHz SR nativa del SOFA.
 * @param positions posiciones medidas en el SOFA.
 * @param taps taps por oído del SOFA.
 */
data class SignatureRoom(
    val id: Int,
    val name: String,
    val sofaFile: String,
    val rt60Sec: Float,
    val roomSizeM: Float,
    val roomWet: Float,
    val measuredRt60Ms: Float,
    val measuredDistanceM: Float,
    val measuredWet: Float,
    val measuredDrrDb: Float,
    val sampleRateHz: Int,
    val positions: Int,
    val taps: Int,
)

object SignatureRooms {
    val rooms: List<SignatureRoom> = listOf(
        SignatureRoom(
            id = 1,
            name = "TU-Berlin QU KEMAR",
            sofaFile = "TU-Berlin_QU_KEMAR_anechoic_radius_0.5m.sofa",
            rt60Sec = 0.05f,
            roomSizeM = 2.0f,
            roomWet = 0.013f,
            measuredRt60Ms = 10.6f,
            measuredDistanceM = 0.5f,
            measuredWet = 0.013f,
            measuredDrrDb = 18.8f,
            sampleRateHz = 44100,
            positions = 360,
            taps = 2048,
        ),
        SignatureRoom(
            id = 2,
            name = "MIT KEMAR normal pinna",
            sofaFile = "MIT_KEMAR_normal_pinna.sofa",
            rt60Sec = 0.05f,
            roomSizeM = 2.0f,
            roomWet = 0.043f,
            measuredRt60Ms = 16.7f,
            measuredDistanceM = 1.4f,
            measuredWet = 0.043f,
            measuredDrrDb = 13.5f,
            sampleRateHz = 44100,
            positions = 710,
            taps = 512,
        ),
        SignatureRoom(
            id = 3,
            name = "THK KU100",
            sofaFile = "demo_FreeFieldHRTF_1_IR.sofa",
            rt60Sec = 0.05f,
            roomSizeM = 3.25f,
            roomWet = 0.001f,
            measuredRt60Ms = 3.1f,
            measuredDistanceM = 3.25f,
            measuredWet = 0.0009f,
            measuredDrrDb = 30.6f,
            sampleRateHz = 48000,
            positions = 2354,
            taps = 128,
        ),
    )

    fun byId(id: Int): SignatureRoom? = rooms.firstOrNull { it.id == id }

    /**
     * Aplica la sala [id] a [controls] y devuelve la copia con signatureRoom
     * + rt60/size/wet, ya con clamp(). id 0 solo pone signatureRoom = 0
     * (sin sala firma; el resto de controles queda intacto). Un id
     * desconocido se trata como 0: nunca se aplica una sala que no existe.
     */
    fun applyTo(controls: ListenerRoomControls, id: Int): ListenerRoomControls {
        if (id == 0) return controls.copy(signatureRoom = 0).clamp()
        val room = byId(id) ?: return controls.copy(signatureRoom = 0).clamp()
        return controls.copy(
            signatureRoom = room.id,
            rt60Sec = room.rt60Sec,
            roomSizeM = room.roomSizeM,
            roomWet = room.roomWet,
        ).clamp()
    }
}
