package com.ivanna.omega.ui

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Tests de los formateadores puros de LocalMusicScreen (v2.5.447).
 * loadLocalTracks (MediaStore) no se testea aquí: en Robolectric el
 * MediaStore está vacío y la consulta real solo existe en el teléfono.
 */
class LocalMusicScreenTest {

    @Test
    fun formatTrackDurationSeconds() {
        assertEquals("0:00", formatTrackDuration(0L))
        assertEquals("0:05", formatTrackDuration(5_000L))
        assertEquals("1:00", formatTrackDuration(60_000L))
        assertEquals("3:45", formatTrackDuration(225_000L))
        assertEquals("10:09", formatTrackDuration(609_000L))
    }

    @Test
    fun formatTrackDurationClampsNegative() {
        assertEquals("0:00", formatTrackDuration(-1_000L))
    }

    @Test
    fun formatTrackSizeBytes() {
        assertEquals("0 B", formatTrackSize(0L))
        assertEquals("512 B", formatTrackSize(512L))
        assertEquals("1.0 KB", formatTrackSize(1024L))
        assertEquals("12.4 MB", formatTrackSize(13_000_000L))
    }
}
