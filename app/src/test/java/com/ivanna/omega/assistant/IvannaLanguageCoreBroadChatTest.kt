package com.ivanna.omega.assistant

import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
class IvannaLanguageCoreBroadChatTest {

    @Test
    fun unfamiliarGeneralTopicRoutesToConversationalAgent() {
        val parsed = IvannaLanguageCore.parse("Háblame de la fotosíntesis y cómo funciona")

        assertEquals(IvannaLanguageCore.AcousticIntent.GENERAL_CHAT, parsed.acousticIntent)
    }

    @Test
    fun explicitAudioCommandStillUsesAudioIntent() {
        val parsed = IvannaLanguageCore.parse("sube el volumen un poco")

        assertEquals(IvannaLanguageCore.AcousticIntent.VOLUME_UP, parsed.acousticIntent)
    }

    // v2.5.448: el "repara" pelado ("repara", "repara 6") caía al camino
    // por defecto ("cmd default") porque la lista SELF_HEAL solo tenía las
    // formas largas ("repárate", "repáralo todo"). Ahora dispara SELF_HEAL.
    @Test
    fun bareReparaRoutesToSelfHeal() {
        assertEquals(
            IvannaLanguageCore.AcousticIntent.SELF_HEAL,
            IvannaLanguageCore.parse("repara").acousticIntent
        )
        assertEquals(
            IvannaLanguageCore.AcousticIntent.SELF_HEAL,
            IvannaLanguageCore.parse("repara 6").acousticIntent
        )
        assertEquals(
            IvannaLanguageCore.AcousticIntent.SELF_HEAL,
            IvannaLanguageCore.parse("reparar").acousticIntent
        )
    }
}
