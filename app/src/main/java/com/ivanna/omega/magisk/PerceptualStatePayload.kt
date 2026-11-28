package com.ivanna.omega.magisk

import com.ivanna.omega.audio.FusedControls
import com.ivanna.omega.audio.FusedControlsBridge
import com.ivanna.omega.audio.RouteBOutput
import org.json.JSONObject

/**
 * PerceptualStatePayload — UN SOLO CONSTRUCTOR de payloads perceptuales
 * (unificación espacial 2.5.509, paso 3).
 *
 * EL PROBLEMA: SET_PERCEPTUAL_STATE se construía a mano en ≥5 sitios
 * (JSONObject().apply { put("action", ...) } en pushFusedToDaemon,
 * sendRouteBStateAtomic, hyperVolterraCommand, NewPhysicsBridge,
 * OemAcousticScreen, EffectsOffTransition) y cada uno estampaba su
 * propio "outputMode" (o no lo estampaba). El constructor es el ÚNICO
 * sitio que pone "action" y "outputMode" en un payload perceptual.
 *
 * REGLAS:
 *  - "action" se pone UNA sola vez, aquí (un solo put("action", …) en
 *    main/java fuera de tests — criterio del paso 3).
 *  - "outputMode" lo pone el constructor DESDE EL MODO PUBLICADO
 *    (AudioRouteManager.routeBStateFlow): 1 audífonos / 2 bocina plana /
 *    3 Genezi (mismo sello que RouteBDeviceGate). Si el mapa de campos
 *    trajera un outputMode, el publicado GANA (se estampa después).
 *  - Los campos llegan como Map<String, Float> (de
 *    FusedControlsBridge.buildFusedPerceptualFields o de un mapa legacy
 *    con los campos sin slot fused); el constructor no sabe de dónde
 *    vienen, solo los vuelca.
 *  - La puerta (RouteBDeviceGate.gatePerceptualCommandJson) sigue
 *    aplicando en sendCommand: re-estampa el mismo modo (idempotente)
 *    y neutraliza por modo. Constructor ≠ puerta: el primero arma, la
 *    segunda protege.
 */
object PerceptualStatePayload {

    /** Valor del daemon para el modo publicado (1/2/3, ver RouteBDeviceGate). */
    fun outputModeValue(output: RouteBOutput): Int = when (output) {
        RouteBOutput.HEADPHONES -> 1
        RouteBOutput.GENEZI -> 3
        RouteBOutput.SPEAKERS -> 2
    }

    /**
     * Construye un payload perceptual completo. [action] es
     * "SET_PERCEPTUAL_STATE" o "SEND_PERCEPTUAL_STATE" (nunca otra).
     */
    fun build(action: String, fields: Map<String, Float>, output: RouteBOutput): JSONObject {
        return JSONObject().apply {
            put("action", action)
            for ((key, value) in fields) {
                put(key, value.toDouble())
            }
            // El modo publicado gana SIEMPRE (se estampa después del mapa).
            put("outputMode", outputModeValue(output).toDouble())
        }
    }

    /** SET_PERCEPTUAL_STATE (escritura fusionada / restore / neutro). */
    fun buildSet(fields: Map<String, Float>, output: RouteBOutput): JSONObject =
        build("SET_PERCEPTUAL_STATE", fields, output)

    /** SEND_PERCEPTUAL_STATE (re-publicación del cerebro, paso 6). */
    fun buildSend(fields: Map<String, Float>, output: RouteBOutput): JSONObject =
        build("SEND_PERCEPTUAL_STATE", fields, output)

    /** SET neutro completo (FusedControls().setNeutral() + modo publicado). */
    fun buildNeutral(output: RouteBOutput): JSONObject =
        buildSet(FusedControlsBridge.buildFusedPerceptualFields(FusedControls().setNeutral()), output)
}
