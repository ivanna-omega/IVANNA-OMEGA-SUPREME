package com.ivanna.omega.audio

/**
 * Perfil de transductor (FASE 2, v2.5.462).
 *
 * Un solo punto de verdad para "¿qué recibe el daemon según lo que está
 * conectado?". RouteBDeviceGate decide audífonos/bocinas; este módulo
 * traduce esa decisión (más el selector de DAC USB) en los valores
 * efectivos que PersistedStateRestorer envía al daemon (Ruta B).
 *
 * TRES MODOS (no dos): el perfil Genezi solo aplica a la cadena DAC USB
 * → Genezi. Cualquier otra bocina (integrada, BT, HDMI, AUX) es
 * SPEAKERS_GENERIC = neutro (comportamiento vigente, fail-closed): el
 * kernel Volterra Genezi y el XTC calibrado para los dos bafles del
 * Genezi NO deben aplicarse a una bocina que no es el Genezi.
 *
 * HEADPHONES (Holografía Binaural Pura): HRTF real (IHR1/SOFA) + ORBE
 * 1.0 + HOLOS 0.5 + Φ 0.5 + Volterra del usuario, cero EvoEQ genérico
 * encima. XTC APAGADO: en audífonos no hay diafonía acústica que
 * cancelar; aplicar XTC colapsaría la imagen binaural.
 *
 * SPEAKERS_GENEZI (Trinnov + Meyer GALAXY): PROHIBIDO HRTF binaural,
 * WFS de array virtual y BRIR artificial. ORBE/HOLOS SÍ pasan con los
 * montos del usuario (v2.5.483: el holograma del Genezi es el del
 * usuario, 1.0/0.5 por defecto; antes iban a 0 y el daemon re-sembraba
 * ORBE 0 tras cada cambio de sesión — hallazgo Esse 2026-11-20 05:18
 * CST). SÍ: AuxFieldCoherence (graves en fase coherente bajo el cruce
 * del Genezi), CrosstalkCanceller (hyperXtc = 1, calibrado a la
 * geometría real de los dos bafles y el punto de escucha) y
 * VolterraH2Symmetric con el kernel genezy_mhc_pz1d.ivk1. Φ sigue con
 * el monto del usuario (es un operador de estado, no binaural). Los
 * montos GENERALES (compressor/loudness/exciterRed) NO pasan: los
 * neutraliza RouteBDeviceGate.gateGeneral en modo GENEZI (0032).
 *
 * VALORES POR DEFECTO (no mediciones): el cruce y la coherencia del Aux
 * Field se leen de ParameterStore (ajustables por el usuario, defaults
 * 120 Hz / 1.0) y se clampan al rango del Genezi 80–120 Hz. La
 * calibración medida (Ley 2 / Ley 4: barridos H2/H3 reales, geometría
 * medida) queda para validación en el G85; estos defaults solo fijan un
 * punto de partida seguro dentro del rango especificado.
 *
 * Todo es PURO (sin Android) y se testea en host.
 */
enum class TransducerMode { HEADPHONES, SPEAKERS_GENEZI, SPEAKERS_GENERIC }

/** Qué kernel Volterra carga el modo (el on/off lo decide el usuario). */
enum class VolterraKernel { GENEZI, PZ1D }

/**
 * Valores efectivos que el daemon recibe para un modo. Solo campos de
 * Ruta B (SET_PERCEPTUAL_STATE / snapshot): lo de Ruta A (HRTF wet,
 * WFS, EvoEQ) lo gobierna ExternalStereoPolicy en restore() y queda
 * fuera de este perfil.
 */
data class TransducerProfile(
    val mode: TransducerMode,
    val orbeAmount: Float,
    val holosAmount: Float,
    val hyperAmount: Float,
    val hyperPreset: Int,
    val hyperXtcEnabled: Boolean,
    val volterraEnabled: Boolean,
    val volterraKernel: VolterraKernel,
    val auxFieldEnabled: Boolean,
    val auxXoverHz: Float,
    val auxCoherence: Float
)

object TransducerProfileFactory {
    // Cruce del Genezi (FASE 2): 80–120 Hz. Fuera de ese rango el Aux
    // Field dejaría de ser coherente con los bafles reales.
    const val GENEZI_XOVER_MIN_HZ = 80f
    const val GENEZI_XOVER_MAX_HZ = 120f

    /** Resolución pura del modo desde tipos + selector (FASE 2). */
    fun resolveMode(deviceTypes: IntArray, usbDacTarget: UsbDacTarget): TransducerMode {
        if (RouteBDeviceGate.isHeadphonesOutput(deviceTypes, usbDacTarget)) {
            return TransducerMode.HEADPHONES
        }
        // DAC USB con selector a bocinas = la cadena del Genezi. Cualquier
        // otra bocina es genérica (neutro, sin kernel ni XTC del Genezi).
        if (RouteBDeviceGate.hasUsbDac(deviceTypes) && usbDacTarget == UsbDacTarget.SPEAKERS) {
            return TransducerMode.SPEAKERS_GENEZI
        }
        return TransducerMode.SPEAKERS_GENERIC
    }

    /**
     * Perfil efectivo para un modo. Los montos del usuario (orbe/holos/
     * hyper/volterra) se respetan donde el modo los admite; donde el modo
     * los prohíbe van a 0. El cruce/coherencia del Aux Field vienen del
     * ParameterStore (ajustables) clampados al rango del Genezi.
     */
    fun profileFor(
        mode: TransducerMode,
        orbeUser: Float,
        holosUser: Float,
        hyperUser: Float,
        hyperPresetUser: Int,
        volterraUser: Boolean,
        auxXoverStoreHz: Float,
        auxCoherenceStore: Float
    ): TransducerProfile = when (mode) {
        TransducerMode.HEADPHONES -> TransducerProfile(
            mode = mode,
            orbeAmount = orbeUser,
            holosAmount = holosUser,
            hyperAmount = hyperUser,
            hyperPreset = hyperPresetUser,
            hyperXtcEnabled = false,
            volterraEnabled = volterraUser,
            volterraKernel = VolterraKernel.PZ1D,
            auxFieldEnabled = false,
            auxXoverHz = 0f,
            auxCoherence = 0f
        )
        TransducerMode.SPEAKERS_GENEZI -> TransducerProfile(
            mode = mode,
            // v2.5.483: ORBE/HOLOS del usuario (antes 0f). El perfil
            // Genezi es el último escritor en restore()/pushRouteBState:
            // con 0f envenenaba desired.orbe del daemon y cada re-seed
            // por cambio de sesión devolvía ORBE 0 (05:18 CST).
            orbeAmount = orbeUser,
            holosAmount = holosUser,
            hyperAmount = hyperUser,
            hyperPreset = hyperPresetUser,
            hyperXtcEnabled = true,
            volterraEnabled = volterraUser,
            volterraKernel = VolterraKernel.GENEZI,
            auxFieldEnabled = true,
            auxXoverHz = auxXoverStoreHz.coerceIn(GENEZI_XOVER_MIN_HZ, GENEZI_XOVER_MAX_HZ),
            auxCoherence = auxCoherenceStore.coerceIn(0f, 1f)
        )
        TransducerMode.SPEAKERS_GENERIC -> TransducerProfile(
            mode = mode,
            orbeAmount = 0f,
            holosAmount = 0f,
            hyperAmount = 0f,
            hyperPreset = 0,
            hyperXtcEnabled = false,
            volterraEnabled = false,
            volterraKernel = VolterraKernel.PZ1D,
            auxFieldEnabled = false,
            auxXoverHz = 0f,
            auxCoherence = 0f
        )
    }
}
