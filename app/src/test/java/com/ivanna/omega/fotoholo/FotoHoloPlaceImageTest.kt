package com.ivanna.omega.fotoholo

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/** Solo lógica pura (sin org.json ni red): el prompt, los límites y la extensión de archivo. */
// v2.5.473: FotoHoloPlaceImage es @Deprecated (legacy v2.5.428) pero el código
// NO se borra (regla durable); el test lo ejerce a propósito y suprime el
// aviso de deprecación en vez de migrar o borrar.
@Suppress("DEPRECATION")
class FotoHoloPlaceImageTest {
    @Test
    fun promptCarriesPlaceAndMusicContext() {
        val p = FotoHoloPlaceImage.buildPrompt("Teatro Colón", "luz cálida", "genre: jazz; lowShare 0.4")
        assertTrue(p.contains("Teatro Colón"))
        assertTrue(p.contains("luz cálida"))
        assertTrue(p.contains("genre: jazz; lowShare 0.4"))
    }

    @Test
    fun blankDetailsAndMusicUseHonestDefaultsInsteadOfInventingAudio() {
        val p = FotoHoloPlaceImage.buildPrompt("Faro", "  ", "")
        assertTrue(p.contains("No live music analysis supplied"))
        assertTrue(p.contains("physically plausible lighting"))
    }

    @Test
    fun promptNeverAsksToIdentifyTheSongOrClaimDocumentaryAccuracy() {
        val p = FotoHoloPlaceImage.buildPrompt("Estadio", "", "x")
        assertTrue(p.contains("Do not claim to identify the song"))
        assertTrue(p.contains("not verified survey data"))
    }

    @Test
    fun cleanTrimsAndBoundsInput() {
        assertEquals("abc", FotoHoloPlaceImage.clean("  abc \n", 10))
        assertEquals(240, FotoHoloPlaceImage.clean("x".repeat(1000), FotoHoloPlaceImage.MAX_PLACE).length)
    }

    @Test
    fun extensionFollowsMimeAndDefaultsToPng() {
        assertEquals("jpg", FotoHoloPlaceImage.extensionFor("image/jpeg"))
        assertEquals("webp", FotoHoloPlaceImage.extensionFor("image/webp"))
        assertEquals("png", FotoHoloPlaceImage.extensionFor("image/png"))
        assertFalse(FotoHoloPlaceImage.extensionFor("text/html") == "html")
    }

    @Test
    fun modelChainStartsWithTheFourKModel() {
        assertEquals("gemini-3-pro-image-preview", FotoHoloPlaceImage.MODEL_CHAIN.first())
        assertTrue(FotoHoloPlaceImage.MODEL_CHAIN.size >= 2)
    }
}
