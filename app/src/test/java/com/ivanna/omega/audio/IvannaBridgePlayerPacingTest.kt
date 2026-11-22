package com.ivanna.omega.audio

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class IvannaBridgePlayerPacingTest {
    @Test fun noAnchorNeverPacesEvenWhenEstimateExceedsTarget0039() {
        // El bug (0039): sin ancla la estima es lo enviado (monótona) y
        // el gate se bloqueaba para siempre tras ~512 ms en la ruta
        // directa. Sin ancla, jamás se paces, por grande que sea la estima.
        assertFalse(shouldPaceProducer(30_000L, 24_576L, false))
        assertFalse(shouldPaceProducer(100_000L, 24_576L, false))
    }

    @Test fun anchoredEstimateAboveTargetPaces0039() {
        assertTrue(shouldPaceProducer(30_000L, 24_576L, true))
    }

    @Test fun anchoredEstimateAtOrBelowTargetDoesNotPace0039() {
        assertFalse(shouldPaceProducer(24_576L, 24_576L, true))
        assertFalse(shouldPaceProducer(10_000L, 24_576L, true))
    }
}
