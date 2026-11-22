package com.ivanna.omega.agent

import com.ivanna.omega.assistant.IvannaConversationalCore
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test

class IvannaAgentCorePolicyTest {

    @Before
    fun resetPreferences() {
        IvannaConversationalCore.clear()
    }

    @Test
    fun musicPolicyUsesNeutralDefaultsWhenNoPreferenceWasExpressed() {
        val policy = IvannaAgentCore.DecisionAgent.decide(
            IvannaAgentCore.PerceptionSnapshot(scene = IvannaAgentCore.AcousticScene.MUSIC),
            IvannaAgentCore.HealthSnapshot()
        )

        assertEquals("music-adaptive", policy.name)
        assertEquals(1.0f, policy.targetGain, 0.001f)
        // v2.5.486: el default de comp en MUSIC es 0.0, no 0.25 — el agente
        // re-empujaba 0.25 en cada flip MUSIC y el compresor causaba bombeo
        // y micro-cortes (IvannaAgentCore.kt). El test seguía fijando el
        // valor viejo y tumbaba la CI sobre 0e6806b1.
        assertEquals(0.0f, policy.compAmount, 0.001f)
        assertEquals(0.15f, policy.excReduction, 0.001f)
        assertEquals(1.15f, policy.spatialWidth, 0.001f)
    }

    @Test
    fun musicPolicyAdaptsToExplicitListeningPreferences() {
        IvannaConversationalCore.updateTemporalPreferences(
            "prefiero escuchar suave, más ancho y más cálido"
        )

        val policy = IvannaAgentCore.DecisionAgent.decide(
            IvannaAgentCore.PerceptionSnapshot(scene = IvannaAgentCore.AcousticScene.MUSIC),
            IvannaAgentCore.HealthSnapshot()
        )

        assertEquals(0.90f, policy.targetGain, 0.001f)
        assertEquals(0.32f, policy.compAmount, 0.001f)
        assertEquals(0.28f, policy.excReduction, 0.001f)
        assertEquals(1.30f, policy.spatialWidth, 0.001f)
    }

    @Test
    fun clippingProtectionOverridesMusicalPreferences() {
        IvannaConversationalCore.updateTemporalPreferences("prefiero escuchar suave y más ancho")

        val policy = IvannaAgentCore.DecisionAgent.decide(
            IvannaAgentCore.PerceptionSnapshot(scene = IvannaAgentCore.AcousticScene.MUSIC),
            IvannaAgentCore.HealthSnapshot(clipping = true, clipCountDelta = 9)
        )

        assertEquals("protect", policy.name)
        assertEquals(0.75f, policy.targetGain, 0.001f)
        assertEquals(1.0f, policy.spatialWidth, 0.001f)
    }

    @Test
    fun silenceLeavesProcessingNeutral() {
        val policy = IvannaAgentCore.DecisionAgent.decide(
            IvannaAgentCore.PerceptionSnapshot(scene = IvannaAgentCore.AcousticScene.SILENCE),
            IvannaAgentCore.HealthSnapshot()
        )

        assertEquals("neutral", policy.name)
        assertEquals(1.0f, policy.spatialWidth, 0.001f)
        assertEquals(0.0f, policy.compAmount, 0.001f)
    }
}
