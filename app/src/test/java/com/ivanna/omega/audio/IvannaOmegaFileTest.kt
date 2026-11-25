// IvannaOmegaFileTest.kt — Unit test del cargador del formato IVANNA OMEGA
// (v2.5.473, feature).
//
// El cargador lee un .ivanna.json por canción y aplica su bloque engine al
// motor (FusedControls + DSPState). Fail-closed: si el archivo no es
// IVANNA OMEGA, si la versión no es la soportada, si el JSON está roto o
// si la ganancia neta viola Matched-LUFS, el parseo falla y NO se aplica
// nada parcial. El parser y el mapeo son puros (sin Android): test JVM
// sin Robolectric, como FusedControlsBridgeTest.
package com.ivanna.omega.audio

import com.ivanna.omega.dsp.DSPState
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class IvannaOmegaFileTest {

    // JSON válido compacto con los mismos valores del engine que el archivo
    // real voodoo_child.ivanna.json (Jimi Hendrix, Voodoo Child).
    private val validJson = """
        {
          "format": "IVANNA OMEGA",
          "format_version": "1.0",
          "track": "Voodoo Child (Slight Return)",
          "artist": "Jimi Hendrix",
          "source": "FLAC 192 kHz / 24-bit estereo",
          "duration_s": 313.07,
          "engine": {
            "orbe_amount": 1.0,
            "orbe_distance_m": 0.35,
            "holos_amount": 0.5,
            "phi_amount": 0.5,
            "volterra_enabled": true,
            "volterra_mix": 0.55,
            "eq_bands_db": {
              "sub_20_60": 0.0, "low_60_120": 1.0, "lowmid_120_250": 0.5,
              "mid_250_500": 0.0, "mid_500_1k": 0.0, "highmid_1k_2k": 0.5,
              "high_2k_4k": 0.0, "presence_4k_8k": 1.5, "air_8k_16k": 1.0,
              "ultra_16k_24k": 0.0
            },
            "compressor": { "threshold_db": -12.0, "ratio": 2.0, "attack_ms": 10.0, "release_ms": 120.0 },
            "exciter_amount": 0.3,
            "trim_db": -0.5,
            "net_gain_db": 0.0,
            "structural_split": { "transient_amount": 0.55, "tonal_amount": 1.0 }
          }
        }
    """.trimIndent()

    private fun parseValid() =
        IvannaOmegaFileParser.parse(validJson).getOrThrow()

    @Test fun validFileParses() {
        val file = parseValid()
        assertEquals("Voodoo Child (Slight Return)", file.track)
        assertEquals("Jimi Hendrix", file.artist)
        assertEquals("Voodoo Child (Slight Return) — Jimi Hendrix", file.displayLabel)
        assertEquals(313.07, file.duration_s, 0.001)
    }

    @Test fun mappingToFusedControls() {
        val file = parseValid()
        // Estado vigente con valores distintos en todo lo que el archivo
        // NO define: deben conservarse.
        val current = FusedControls(
            spatial = SpatialControls(mode = FusedSpatialMode.SPEAKERS, width = 1.5f),
            field = FieldControls(coherence = 0.9f),
            brain = BrainControls(mode = FusedBrainMode.MANUAL, adaptation = 0.0f),
        )
        val mapped = file.toFusedControls(current)
        // Lo que el archivo define:
        assertEquals(1.0f, mapped.spatial.amount)
        assertEquals(0.35f, mapped.spatial.distanceM)
        assertEquals(0.5f, mapped.field.amount)
        assertEquals(0.5f, mapped.field.phiAmount)
        assertEquals(0.55f, mapped.transducer.correction)
        assertEquals(1.0f, mapped.tonal.lowDb)
        assertEquals(0.0f, mapped.tonal.midDb)
        assertEquals(0.0f, mapped.tonal.highDb)
        assertEquals(1.5f, mapped.tonal.presenceDb)
        assertEquals(-0.5f, mapped.tonal.trimDb)
        assertEquals(-12.0f, mapped.dynamics.compThresholdDb)
        assertEquals(2.0f, mapped.dynamics.compRatio)
        assertEquals(0.3f, mapped.dynamics.exciter)
        // Lo que el archivo NO define se conserva:
        assertEquals(FusedSpatialMode.SPEAKERS, mapped.spatial.mode)
        assertEquals(1.5f, mapped.spatial.width)
        assertEquals(0.9f, mapped.field.coherence)
        assertEquals(FusedBrainMode.MANUAL, mapped.brain.mode)
    }

    @Test fun mappingToDspState() {
        val file = parseValid()
        val current = DSPState(master = 2.0f, stereoWidth = 1.5f)
        val mapped = file.toDspState(current)
        assertEquals(0.3f, mapped.wet)
        assertEquals(1.0f, mapped.low)
        assertEquals(0.0f, mapped.mid)
        assertEquals(0.0f, mapped.high)
        assertEquals(1.5f, mapped.presence)
        assertEquals(-12.0f, mapped.compThreshold)
        assertEquals(2.0f, mapped.compRatio)
        // master se conserva: el trim va al tonal fusionado, no a ganancia oculta.
        assertEquals(2.0f, mapped.master)
        assertEquals(1.5f, mapped.stereoWidth)
    }

    @Test fun volterraOffYieldsZeroCorrection() {
        val file = IvannaOmegaFileParser.parse(
            validJson.replace("\"volterra_enabled\": true", "\"volterra_enabled\": false")
        ).getOrThrow()
        assertEquals(0.0f, file.toFusedControls(FusedControls()).transducer.correction)
    }

    @Test fun outOfRangeValuesAreClamped() {
        val file = IvannaOmegaFileParser.parse(
            validJson.replace("\"orbe_amount\": 1.0", "\"orbe_amount\": 5.0")
                .replace("\"trim_db\": -0.5", "\"trim_db\": -99.0")
        ).getOrThrow()
        val mapped = file.toFusedControls(FusedControls())
        assertEquals(1.0f, mapped.spatial.amount)
        assertEquals(-6.0f, mapped.tonal.trimDb)
    }

    @Test fun wrongFormatIsRejected() {
        val result = IvannaOmegaFileParser.parse(
            validJson.replace("\"IVANNA OMEGA\"", "\"Dolby Atmos\"")
        )
        assertTrue(result.isFailure)
    }

    @Test fun wrongVersionIsRejected() {
        val result = IvannaOmegaFileParser.parse(
            validJson.replace("\"1.0\"", "\"2.0\"")
        )
        assertTrue(result.isFailure)
    }

    @Test fun brokenJsonIsRejected() {
        assertTrue(IvannaOmegaFileParser.parse("{ no es json").isFailure)
        assertTrue(IvannaOmegaFileParser.parse("").isFailure)
    }

    @Test fun netGainViolatesMatchedLufsIsRejected() {
        val result = IvannaOmegaFileParser.parse(
            validJson.replace("\"net_gain_db\": 0.0", "\"net_gain_db\": 5.0")
        )
        assertTrue(result.isFailure)
    }

    @Test fun failedParseLeavesStateUntouched() {
        // Fail-closed: un parseo fallido no produce archivo que aplicar, así
        // que el estado vigente queda idéntico (nada se aplica parcial).
        val currentFused = FusedControls()
        val currentDsp = DSPState()
        val result = IvannaOmegaFileParser.parse(
            validJson.replace("\"IVANNA OMEGA\"", "\"Dolby Atmos\"")
        )
        assertTrue(result.isFailure)
        assertFalse(result.isSuccess)
        // El estado que se habría aplicado sigue intacto: no hay archivo.
        assertEquals(FusedControls(), currentFused)
        assertEquals(DSPState(), currentDsp)
    }

    @Test fun missingEngineUsesDefaults() {
        val minimal = """
            {"format": "IVANNA OMEGA", "format_version": "1.0",
             "track": "Prueba", "artist": "Nadie"}
        """.trimIndent()
        val file = IvannaOmegaFileParser.parse(minimal).getOrThrow()
        assertEquals("Prueba — Nadie", file.displayLabel)
        val mapped = file.toFusedControls(FusedControls())
        assertEquals(1.0f, mapped.spatial.amount)
        // Default del engine sin sección engine: HOLOS 1.0 (ATREVIDA 502).
        assertEquals(1.0f, mapped.field.amount)
    }
}
