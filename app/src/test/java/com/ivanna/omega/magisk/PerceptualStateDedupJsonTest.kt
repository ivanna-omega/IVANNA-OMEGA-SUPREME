// PerceptualStateDedupJsonTest.kt — Unit test de la conversión
// JSON→mapa de la deduplicación (micro-cortes TIDAL, 2026-11-21).
//
// Robolectric porque JSONObject es clase del framework Android.
// Si la conversión perdiera un campo, el dedup ignoraría sus cambios y el
// daemon dejaría de recibirlos: este test fija que todos los campos
// llegan al comparador, con los números como Double y los anidados
// convertidos recursivamente. El comparador en sí se testea en
// PerceptualStateDedupTest (JUnit puro, sin Android).
package com.ivanna.omega.magisk

import org.json.JSONArray
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class PerceptualStateDedupJsonTest {

    @Test
    fun `conversion preserva todos los campos como Double`() {
        val json = JSONObject().apply {
            put("action", "SET_PERCEPTUAL_STATE")
            put("orbeAmount", 0.386)
            put("holosAmount", 0.0)
            put("outputMode", 2)
            put("spatialWidth", 1.2)
        }
        val map = perceptualPayloadToMap(json)
        assertEquals(5, map.size)
        assertEquals("SET_PERCEPTUAL_STATE", map["action"])
        assertEquals(0.386, map["orbeAmount"] as Double, 0.0)
        assertEquals(0.0, map["holosAmount"] as Double, 0.0)
        assertEquals(2.0, map["outputMode"] as Double, 0.0)
        assertEquals(1.2, map["spatialWidth"] as Double, 0.0)
    }

    @Test
    fun `conversion anida objetos y arrays recursivamente`() {
        val json = JSONObject().apply {
            put("action", "SEND_PERCEPTUAL_STATE")
            put("state", JSONObject().apply {
                put("orbeAmount", 0.5)
                put("nested", JSONObject().apply { put("x", 1.0) })
            })
            put("args", JSONArray().apply { put(1.0); put(2.0) })
        }
        val map = perceptualPayloadToMap(json)
        val state = map["state"] as Map<*, *>
        assertEquals(0.5, state["orbeAmount"] as Double, 0.0)
        val nested = state["nested"] as Map<*, *>
        assertEquals(1.0, nested["x"] as Double, 0.0)
        val args = map["args"] as List<*>
        assertEquals(2, args.size)
        assertEquals(1.0, args[0] as Double, 0.0)
        assertEquals(2.0, args[1] as Double, 0.0)
    }

    @Test
    fun `conversion null JSON pasa a null`() {
        val json = JSONObject().apply {
            put("action", "SET_PERCEPTUAL_STATE")
            put("orbeAmount", JSONObject.NULL)
        }
        val map = perceptualPayloadToMap(json)
        assertTrue(map.containsKey("orbeAmount"))
        assertNull(map["orbeAmount"])
    }

    @Test
    fun `conversion objeto vacio da mapa vacio`() {
        assertTrue(perceptualPayloadToMap(JSONObject()).isEmpty())
    }

    @Test
    fun `mapa convertido decide igual que el mapa manual`() {
        // El puente entre JSON y dedup: el mismo payload en JSON y en mapa
        // manual debe producir la misma decisión (enviar la primera vez,
        // suprimir la segunda).
        val dedup = PerceptualStateDedup()
        val json = JSONObject().apply {
            put("action", "SET_PERCEPTUAL_STATE")
            put("orbeAmount", 0.386)
            put("holosAmount", 0.0)
        }
        val first = perceptualPayloadToMap(json)
        val second = perceptualPayloadToMap(
            JSONObject().apply {
                put("action", "SET_PERCEPTUAL_STATE")
                put("orbeAmount", 0.386)
                put("holosAmount", 0.0)
            }
        )
        assertTrue(dedup.shouldSend("SET_PERCEPTUAL_STATE", first))
        assertFalse(dedup.shouldSend("SET_PERCEPTUAL_STATE", second))
    }
}
