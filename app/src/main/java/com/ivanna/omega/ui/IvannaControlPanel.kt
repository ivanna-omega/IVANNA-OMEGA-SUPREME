package com.ivanna.omega.ui

import android.util.Log
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.tween
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.expandVertically
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.ivanna.omega.audio.ActiveRoute
import com.ivanna.omega.audio.EffectsOffTransition
import com.ivanna.omega.audio.IvannaEffectProfile
import com.ivanna.omega.audio.OmegaMetrics
import com.ivanna.omega.audio.PipelineState
import com.ivanna.omega.audio.RealAcousticSensor
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.PersistedStateRestorer
import com.ivanna.omega.hardware.PermissionGate
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import com.ivanna.omega.neuromorphic.IvannaNpeEngine
import com.ivanna.omega.ui.control.NewPhysicsCardsColumn
import com.ivanna.omega.ui.theme.*
import kotlin.math.log10
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalLifecycleOwner
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver

// FIX (crash 506 - ConcurrentModificationException en Compose, 3x el 2026-11-27
// 02:11): instantaneas inmutables para cruzar del hilo de fondo
// (Dispatchers.Default, JNI) al hilo principal, donde vive TODA escritura de
// estado de Compose. Sin ellas, los bucles de telemetria publicaban estado
// desde Default mientras el layout lo leia y tiraba CME.
private data class UnityTelemetrySnap(
    val loaded: Boolean,
    val unityTelemetry: FloatArray,
    val punchTelemetry: FloatArray,
    val evoEqActive: Boolean,
    val volterraKernelLoaded: Boolean,
    val hyperMeanCoherence: Float,
    val hyperMode: Int,
    val volterraKernelId: String,
    val sensorRunning: Boolean,
    val sensorFrameIndex: Long,
    val sensorSource: String,
)

private data class NpeHudSnap(
    val genre: String,
    val rmsDb: Float,
    val agcDb: Float,
    val confidence: Float,
    val thd: Float,
    val inferenceUs: Long,
)


// ── Fichas magistrales · Control (prototipo aprobado, Pantalla III · Motor) ─
// Texto exacto del prototipo; nada inventado. SpecInfo / ScreenSpecCard /
// SpecInfoButton viven en SpecSheet.kt, mismo paquete. Los controles del
// prototipo se anclan a su equivalente más cercano en este panel (ver notas):
// «Etapa de graves» = tarjeta MODO AUX · ALTAVOCES; «Profundidad máxima» =
// slider de profundidad del campo temporal 4D; «ORBE · Intensidad» /
// «HOLOS · Intensidad» = sliders CANTIDAD ORBE / CANTIDAD HOLOS.

/** Ficha del entorno · Control. */
private val controlScreenSpec = SpecInfo(
    title = "Control",
    what = "El entorno de control directo de la cadena: graves, corrección de sala, dinámica espacial y el campo ORBE·HOLOS. Es la vista operativa diaria del motor.",
    purpose = "Para ajustar lo que se oye aquí y ahora: la telemetría ASRC y el espectro confirman que la cadena está viva, y cada módulo actúa sobre una capa distinta sin pisar a las demás.",
    whenUse = "Como punto de partida de cada sesión y tras cambiar de salida (bocina, audífonos, DAC): revisa primero el estado del motor y después toca módulos, de arriba hacia abajo.",
    whenAvoid = "No ajustes todo a la vez: si mueves graves, sala y espacio en la misma escucha no sabrás qué causó qué. Un módulo por escucha, A/B contra su interruptor."
)

/** A · Etapa de graves — Frecuencia de cruce. */
private val auxXoverSpec = SpecInfo(
    title = "Frecuencia de cruce",
    what = "La frecuencia a partir de la cual la etapa de graves de IVANNA deja de gestionar el grave y lo entrega a la cadena principal. Por debajo del cruce manda la gestión propia de IVANNA; por encima, el grave pasa sin ella.",
    purpose = "Para que el grave tenga un solo dueño. Si tu receptor o amplificador también aplica su crossover, dos cruces en cascada abren un hueco o un bulto en el empalme que se oye como grave «bofo», inflado y desconectado del resto.",
    whenUse = "Con bocinas o AUX y un receptor con crossover propio: iguala el valor al del receptor (lo típico es 80 Hz) o desactiva uno de los dos. Con bocinas pequeñas sube a 100–120 Hz para aliviarlas del grave profundo.",
    whenAvoid = "Con audífonos o la bocina del teléfono no hay etapa externa: déjalo en 80 Hz. Evita los extremos salvo que tu sistema los pida: a 40 Hz el grave gestionado casi desaparece; a 160 Hz el grave se vuelve localizable y embarra la escena."
)

/** A · Etapa de graves — Coherencia de graves. */
private val auxCoherenceSpec = SpecInfo(
    title = "Coherencia de graves",
    what = "El grado de consistencia temporal con que la etapa de graves entrega la baja frecuencia en ambos canales. Al 100 % el grave llega alineado y compacto; al bajarlo, la etapa relaja esa alineación y el grave se vuelve más difuso y envolvente.",
    purpose = "Es la herramienta directa contra el grave «bofo»: un grave coherente se percibe seco, con golpe y con filo; uno incoherente se percibe grande pero sin definición. No cambia cuántos graves hay: cambia su orden.",
    whenUse = "Déjalo en 100 % como punto de partida siempre. Bájalo solo con bocinas en sala pequeña, si el grave se concentra en un único punto y quieres repartirlo — en pasos de 10 % y comparando.",
    whenAvoid = "Valores bajos con audífonos: el bombo pierde el centro y se desdibuja. Si al bajarlo el grave pierde definición en vez de ganar espacio, regresa a 100 %: no es un control de cantidad, es de coherencia."
)

/**
 * B · Corrección acústica · Sala / Bocinas.
 *
 * SIN botón «Generar vista geométrica 3D 4K» con ese nombre exacto en el
 * código: la vista geométrica es RoomGeometryPreviewPanel(), ya renderizada
 * dentro de esta misma tarjeta. La ficha cubre el módulo completo
 * (medición + vista geométrica + límites honestos del micrófono) y el botón
 * se ancla a la tarjeta, junto a su interruptor.
 */
private val roomCorrectionSpec = SpecInfo(
    title = "Corrección acústica · Sala / Bocinas",
    what = "Una medición acústica con el micrófono del teléfono: emite un barrido de 50 Hz a 6 kHz, captura cómo tu sala y tus bocinas alteran el balance tonal y genera una corrección de balance amplio que lo compensa. La vista 3D 4K renderiza la geometría estimada de ese espacio.",
    purpose = "La sala manda más que las bocinas: esquinas que inflan el grave (boom), paredes que se comen la presencia. La corrección aplana ese balance para que escuches la mezcla, no el cuarto.",
    whenUse = "Escuchas por bocinas en un punto fijo: mide con el teléfono en tu punto de escucha, a la altura del oído, en silencio. Repite la medición si mueves bocinas, muebles o cambias de cuarto.",
    whenAvoid = "Con audífonos no hay sala que corregir: déjala apagada. No apliques la corrección de un cuarto en otro, ni midas con el teléfono pegado a una pared. Y sin medición previa, el interruptor no aplica nada: el audio queda intacto."
)

/** C · Dinámica espacial · 4D — Profundidad máxima. */
private val temporalDepthSpec = SpecInfo(
    title = "Profundidad máxima",
    what = "El techo de apertura de la dinámica espacial 4D: en cada golpe la imagen se enfoca hacia el centro y en las colas se abre, como máximo, hasta este porcentaje. El centro vocal y los graves quedan fuera de este movimiento.",
    purpose = "Para que la escena respire con la música sin volverse inestable: los ataques quedan enfocados y los pasajes abiertos ganan escenario, sin que la imagen «nada» de un lado a otro.",
    whenUse = "50–60 % es el punto magistral para rock, bocinas y audífonos abiertos. Sube a 70–80 % solo con grabaciones muy cerradas o mono antiguas que quieras abrir.",
    whenAvoid = "Al 100 % con material ya muy espacial (binaural, directos con mucha sala) la imagen se deshace y el centro se adelgaza. Si la voz pierde cuerpo, baja 10 %."
)

/**
 * D · ORBE · HOLOS — ORBE · Intensidad.
 *
 * El control se llama «CANTIDAD ORBE» en el código; es la cantidad de ORBE
 * en la mezcla, es decir, su intensidad. No se renombra el control.
 */
private val orbeAmountSpec = SpecInfo(
    title = "ORBE · Intensidad",
    what = "ORBE es el campo binaural esférico de campo cercano: cada banda ERB —las bandas en que el oído divide el espectro— se coloca a un azimut y una distancia continuos entre 0.15 y 2.0 m, con parallax al mover la cabeza. Este control es su cantidad en la mezcla.",
    purpose = "Para sacar el sonido de «dentro de la cabeza» y ponerlo frente a ti, a distancias reales: una voz a 40 cm no suena como una orquesta a 2 m, y ORBE respeta esa diferencia banda a banda.",
    whenUse = "Al 100 %, su valor de diseño y el fijado por directiva del producto: es el modo completo. Baja a 60–80 % solo si una grabación concreta te resulta demasiado «fuera» y quieres acercarla.",
    whenAvoid = "No lo apagues si buscas la experiencia IVANNA completa: sin ORBE la cadena suena plana y pegada al oído. Y no lo muevas canción a canción: el oído tarda unos segundos en asentarse en el campo."
)

/**
 * D · ORBE · HOLOS — HOLOS · Intensidad.
 *
 * El control se llama «CANTIDAD HOLOS» en el código; es el peso de HOLOS
 * en el campo, es decir, su intensidad. No se renombra el control.
 */
private val holosAmountSpec = SpecInfo(
    title = "HOLOS · Intensidad",
    what = "HOLOS es el operador único de campo acústico: en vez de sumar efectos por separado, mantiene un solo vector de estado por banda (STFT hop 128 / FFT 512) y gobierna el campo completo de forma coherente. Este control es su peso en el campo.",
    purpose = "Para que el espacio no sea una suma de efectos peleando entre sí: una sola física decide el campo, con THD+N de −94 dB a 1 kHz — coherencia sin distorsión añadida.",
    whenUse = "Al 50 %, su punto retuneado de fábrica (armónicos en 0.0): es el equilibrio medido entre presencia del campo y transparencia total.",
    whenAvoid = "Por encima de 70 % el campo empieza a imponerse sobre material delicado (clásica, acústico íntimo). Si en A/B no notas mejora en tu sala, déjalo en 50 %: más no es mejor, es otro balance."
)

/**
 * J · Kernel Volterra · Pre-distorsión (prototipo aprobado, Pantalla V ·
 * Adaptive). El selector PZ1D/Genezi vive aquí (SingleChoiceSegmentedButtonRow
 * "KERNEL VOLTERRA"); la ficha J de AdaptiveEngineScreen quedó anclada a
 * "Modo adaptativo" como equivalente, y aquí va en su sitio exacto.
 */
private val volterraKernelSpec = SpecInfo(
    title = "Kernel Volterra · Pre-distorsión",
    what = "El kernel Volterra modela la no-linealidad real de tu salida (cómo el DAC y el transductor deforman la señal, empezando por el segundo armónico, H2) y aplica la pre-distorsión inversa: la señal sale torcida en sentido contrario para llegar recta a tu oído. El control regula cuánta de esa inversa se aplica.",
    purpose = "Para quitar la aspereza y el «velo» que no vienen de la grabación sino de tu propio hardware reproduciéndola. Es corrección a la medida del dispositivo, no un efecto de sabor.",
    whenUse = "Elige el kernel que corresponde a tu salida física y cambia con la música sonando: el fundido de ~18 ms hace la comparación A/B inmediata y honesta. Nivel H2 moderado (30–40 %) como punto de partida.",
    whenAvoid = "No fuerces el kernel de un DAC sobre otro: la pre-distorsión equivocada añade exactamente la distorsión que debía quitar. Si no sabes qué DAC tienes conectado, PZ1D Refinado es la opción segura."
)

/**
 * Aplica las preferencias persistidas del sensor al arrancar la captura
 * (graves ±3 dB, corrección de sala en vivo). Sin esto, los wrappers
 * RealAcousticSensor.setBassTrimDb / setRoomCorrectionEnabled /
 * setRoomCorrectionAmount no tenían llamador: la voz los persiste y el
 * arranque los empuja al sensor. No-op sin librería nativa (guarded).
 */
private fun applySensorPrefs(context: android.content.Context) {
    val store = com.ivanna.omega.core.ParameterStore(context)
    runCatching {
        RealAcousticSensor.setBassTrimDb(store.getSensorBassTrimDb())
        RealAcousticSensor.setRoomCorrectionAmount(store.getSensorRoomCorrectionAmount())
        RealAcousticSensor.setRoomCorrectionEnabled(store.isSensorRoomCorrectionEnabled())
    }
}

/**
 * Interruptor con acento dorado magistral (v2.5.442): misma semántica que
 * Switch — reenvía checked / enabled / onCheckedChange sin tocar el
 * cableado — con pista dorada en ON y pulgar oscuro, según el lenguaje
 * visual del prototipo (dorado con jerarquía; cian solo donde se toca o
 * donde algo está vivo).
 */
@Composable
private fun MagistralSwitch(
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
    enabled: Boolean = true,
    modifier: Modifier = Modifier
) {
    Switch(
        checked = checked,
        onCheckedChange = onCheckedChange,
        enabled = enabled,
        modifier = modifier,
        colors = SwitchDefaults.colors(
            checkedThumbColor = MagistralVoid,
            checkedTrackColor = MagistralGold,
            checkedBorderColor = MagistralGold,
            uncheckedThumbColor = MagistralMuted,
            uncheckedTrackColor = Color.Transparent,
            uncheckedBorderColor = MagistralLineStrong
        )
    )
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun IvannaControlPanel(
    initialExciter: Float,
    initialEq: Float,
    initialWidth: Float,
    // v2.5.473 (§1.3 C1–C8): CONTROL renderiza el estado fusionado (misma
    // instancia que FUSIÓN, §2.5) pero no lo posee: estos valores llegan ya
    // hoisteados desde ControlTabScreen.
    initialMasterDb: Float = 0f,
    initialTonalLowDb: Float = 0f,
    initialTonalMidDb: Float = 0f,
    initialTonalHighDb: Float = 0f,
    initialTonalPresenceDb: Float = 0f,
    initialTonalTrimDb: Float = 0f,
    initialFusedOrbe: Float = 1f,
    initialFusedHolos: Float = 0.5f,
    initialFusedPhi: Float = 0.5f,
    initialFusedVolterra: Boolean = true,
    initialSpatialModeSpeakers: Boolean = false,
    initialCaptureActive: Boolean = false,
    fusedProfileName: String = "",
    onMasterChange: (Float) -> Unit = {},
    onTonalEqChange: (Float, Float, Float, Float) -> Unit = { _, _, _, _ -> },
    onTonalTrimChange: (Float) -> Unit = {},
    onSpatialModeChange: (Boolean) -> Unit = {},
    onToggleCapture: () -> Unit = {},
    onOpenFusion: () -> Unit = {},
    initialAntiDolby: Boolean = true,
    initialPreset: String = "IVANNA OMEGA",
    initialAutoMode: Boolean = true,
    initialOmegaMode: Int = 2,
    initialCompThreshold: Float = 0.35f,
    initialCompRatio: Float = 0.08f,
    // Umbral/ratio del compresor EN dB (canónico, CanonicalRanges). Por
    // defecto derivan del alpha/beta normalizado legacy con el mapeo
    // histórico del motor (umbral = −24 + 24·alpha; ratio = 1 + 19·beta),
    // así el valor guardado no cambia: solo cambia el rango del control.
    // El llamador (ControlTabScreen) pasa el dB fusionado guardado.
    initialCompThresholdDb: Float = -24f + initialCompThreshold * 24f,
    initialCompRatioValue: Float = 1f + initialCompRatio * 19f,
    initialNhoHarmonic: Float = 0.28f,
    initialSpatialAngle: Float = 0.5f,
    initialSpatialWidth: Float = 0.68f,
    initialEvoEnabled: Boolean = true,
    initialNpeBypass: Boolean = false,
    initialNpeHarmonic: Float = 0.60f,
    initialNpeLateralInhib: Float = 0.50f,
    initialNpeOhcCompression: Float = 0.40f,
    initialNpeMasterGain: Float = 2.5f,
    initialNpeAgcTarget: Float = -14.0f,
    initialNpeAgcRate: Float = 0.62f,
    initialNpeHrtf: Boolean = true,
    initialNpeCochlear: Boolean = true,
    initialNpeAdapt: Boolean = true,
    initialNpeManifold: Boolean = true,
    initialSpatialEnabled: Boolean = true,
    initialHoaUpmixingEnabled: Boolean = true,
    initialHoaImmersivity: Float = 1.25f,
    initialWfsEnabled: Boolean = true,
    initialWfsSpread: Float = 1.18f,
    initialAuxFieldEnabled: Boolean = false,
    roomGains: FloatArray? = null,
    roomEnabled: Boolean = false,
    roomBusy: Boolean = false,
    roomProgress: Float = 0f,
    roomStatus: String = "",
    onRoomMeasure: () -> Unit = {},
    onRoomToggle: (Boolean) -> Unit = {},
    onRoomClear: () -> Unit = {},
    // v2.5.473 §5: barrido Genezi (C7). El estado vive en ControlTabScreen.
    sweepBusy: Boolean = false,
    sweepProgress: Float = 0f,
    sweepStatus: String = "",
    onGeneziSweep: () -> Unit = {},
    // Formato IVANNA OMEGA: archivo .ivanna.json por canción. El estado
    // vive en ControlTabScreen (selector SAF + parse + apply).
    omegaFileLabel: String = "",
    omegaStatus: String = "",
    onLoadOmegaFile: () -> Unit = {},
    initialTemporalFieldEnabled: Boolean = false,
    initialTemporalFieldAmount: Float = 0.6f,
    initialTemporalFieldAdaptive: Boolean = true,
    initialBassHapticEnabled: Boolean = false,
    initialBassHapticStrength: Float = 0.7f,
    initialBassHapticRumble: Float = 0f,
    initialBassHapticTrimMs: Float = 0f,
    bassHapticSupported: Boolean = true,
    initialAuxXoverHz: Float = 120f,
    initialAuxCoherence: Float = 1f,
    // ── Organismo (v2.5.506, cableado mic → organismo) ─────────────────────
    // UI mínima: UN interruptor maestro + UN indicador de estado. El
    // estado vive en OrganismController (StateFlow); ControlTabScreen lo
    // observa y lo pasa aquí. Defaults = organismo apagado en el panel
    // si el llamador no lo cablea (el controlador sigue autónomo).
    organismState: com.ivanna.omega.organism.OrganismController.State =
        com.ivanna.omega.organism.OrganismController.State(),
    onOrganismToggle: (Boolean) -> Unit = {},
    onOrganismSaveToken: (String) -> Unit = {},
    onOrganismClearQueue: () -> Unit = {},
    initialPunchAmount: Float = 0f,
    initialCrossfeed: Float = 0f,
    // ORBE — Binaural Esférico de Campo Cercano (v2.5.363). ACTIVO por defecto en v2.5.369 (amount 1.0).
    initialOrbeAmount: Float = 1.0f,
    initialOrbeDistanceM: Float = 0.35f,
    initialOrbeSpread: Float = 1.0f,
    initialOrbeProximityDb: Float = 3.0f,
    initialOrbeDiffuse: Float = 0.5f,
    initialOrbeHeadTracking: Boolean = true,
    // HOLOS — Operador Único de Campo Acústico (v2.5.364). ACTIVO por defecto en v2.5.369, retuneado (amount 0.5, harmonics 0.0).
    initialHolosAmount: Float = 0.5f,
    initialHolosDistanceM: Float = 0.35f,
    initialHolosSpread: Float = 1.0f,
    initialHolosProximityDb: Float = 3.0f,
    initialHolosDiffuse: Float = 0.5f,
    initialHolosHarmonics: Float = 0.0f,
    initialHolosWarmth: Float = 0.5f,
    initialHolosDynamics: Float = 1.0f,
    initialHolosWidth: Float = 1.0f,
    initialHolosGamma: Float = 1.0f,
    initialHolosRoomRt60: Float = 0.34f,
    initialHolosHeadTracking: Boolean = true,
    initialHolosBandCount: Int = 24,
    // EQ estático de Γ (dB -12..12, frecuencia del tilt en Hz). Plano = no-op.
    initialHolosEqLow: Float = 0f,
    initialHolosEqMid: Float = 0f,
    initialHolosEqHigh: Float = 0f,
    initialHolosEqPresence: Float = 0f,
    initialHolosEqTiltDb: Float = 0f,
    initialHolosEqFreq: Float = 1000f,
    // Hiper-Operador de Estado Φ (v2.5.369). amount 0.5 = ACTIVO por defecto.
    // preset 0 teléfono / 1 sala Genezi / 2 auto. target kind 0 Harman 2018
    // (default) / 1 élite-lean / 2 campo difuso H&M / 3 altavoces en sala.
    initialHyperAmount: Float = 0.5f,
    initialHyperPreset: Int = 0,
    initialHyperXtc: Boolean = true,
    initialHyperTargetKind: Int = 0,
    initialHyperBassTrimDb: Float = 0f,
    onExciterChange: (Float) -> Unit,
    onEqChange: (Float) -> Unit = {}, // v2.5.473: sin uso en CONTROL (la EQ es por banda, onTonalEqChange); se conserva la firma.
    onWidthChange: (Float) -> Unit,
    onAntiDolbyChange: (Boolean) -> Unit = {},
    onPresetSelected: (String) -> Unit = {},
    onAutoModeChange: (Boolean) -> Unit = {},
    onOmegaModeChange: (Int) -> Unit = {},
    onCompThresholdChange: (Float) -> Unit = {},
    onCompRatioChange: (Float) -> Unit = {},
    onNhoHarmonicChange: (Float) -> Unit = {},
    onSpatialAngleChange: (Float) -> Unit = {},
    onSpatialWidthChange: (Float) -> Unit = {},
    onEvoEnabledChange: (Boolean) -> Unit = {},
    onNpeBypassChange: (Boolean) -> Unit = {},
    onNpeHarmonicChange: (Float) -> Unit = {},
    onNpeLateralInhibChange: (Float) -> Unit = {},
    onNpeOhcCompressionChange: (Float) -> Unit = {},
    onNpeMasterGainChange: (Float) -> Unit = {},
    onNpeAgcChange: (Float, Float) -> Unit = { _, _ -> },
    onNpeFlagsChange: (Boolean, Boolean, Boolean) -> Unit = { _, _, _ -> },
    onNpeManifoldChange: (Boolean) -> Unit = {},
    onSpatialEnabledChange: (Boolean) -> Unit = {},
    onHoaUpmixingEnabledChange: (Boolean) -> Unit = {},
    onHoaImmersivityChange: (Float) -> Unit = {},
    onWfsEnabledChange: (Boolean) -> Unit = {},
    onWfsSpreadChange: (Float) -> Unit = {},
    onAuxFieldChange: (Boolean, Float, Float) -> Unit = { _, _, _ -> },
    onPunchChange: (Float) -> Unit = {},
    onCrossfeedChange: (Float) -> Unit = {},
    // ORBE: (amount, distanceM, spread, proximityDb, diffuse, headTracking)
    onOrbeChange: (Float, Float, Float, Float, Float, Boolean) -> Unit = { _, _, _, _, _, _ -> },
    // HOLOS: (amount, distanceM, spread, proximityDb, diffuse, harmonics,
    // warmth, dynamics, width, gamma, roomRt60, headTracking, bandCount)
    onHolosChange: (Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Boolean, Int) -> Unit = { _, _, _, _, _, _, _, _, _, _, _, _, _ -> },
    // HOLOS EQ de Γ: (low, mid, high, presence, tiltDb, freq)
    onHolosEqChange: (Float, Float, Float, Float, Float, Float) -> Unit = { _, _, _, _, _, _ -> },
    // Hiper-Operador de Estado Φ: (amount, preset, xtcEnabled, targetKind, bassTrimDb)
    onHyperChange: (Float, Int, Boolean, Int, Float) -> Unit = { _, _, _, _, _ -> },
    onTemporalFieldChange: (Boolean, Float, Boolean) -> Unit = { _, _, _ -> },
    onBassHapticChange: (Boolean, Float, Float, Float) -> Unit = { _, _, _, _ -> },
    onOpenVisualizer: () -> Unit = {},
    onOpenHiRes: () -> Unit = {},
    onOpenAdaptive: () -> Unit = {},
    onOpenAdaptiveEngineManual: () -> Unit = {},
    onOpenOpe: () -> Unit = {},
    onOpenBinaural: () -> Unit = {},
    onOpenHeadMeasure: () -> Unit = {},
    onOpenTelemetry: () -> Unit = {},
    onOpenAdaptiveProfiles: () -> Unit = {},
    onOpenProfiles: () -> Unit = {},
    onOpenMagisk: () -> Unit = {},
    onOpenSupremeAxesHub: () -> Unit = {},
    metrics: OmegaMetrics = OmegaMetrics(),
    onMetricsUpdate: ((OmegaMetrics) -> Unit)? = null,
    adaptiveTelemetry: com.ivanna.omega.ui.AdaptiveTelemetrySnapshot = com.ivanna.omega.ui.AdaptiveTelemetrySnapshot(),
    adaptiveMode: com.ivanna.omega.audio.AdaptiveMode = com.ivanna.omega.audio.AdaptiveMode.STUDIO,
    onAdaptiveModeChange: (com.ivanna.omega.audio.AdaptiveMode) -> Unit = {},
    adaptiveIntensity: Float = 82f,
    onAdaptiveIntensityChange: (Float) -> Unit = {},
    voiceProtectionEnabled: Boolean = true,
    onVoiceProtectionChange: (Boolean) -> Unit = {},
    routeState: PipelineState = PipelineState(),
    // Phase Oracle — intensidad global de coherencia de fase (0=off, 1=max)
    initialPhaseOracleIntensity: Float = 0.72f,
    onPhaseOracleChange: (Float) -> Unit = {},
    // FIX (scroll 413): el panel es contenido raíz de ControlTabScreen (ahí
    // necesita su propio scroll interno, default true) y también hijo del
    // Dashboard, donde va seguido de BridgePlayerCard. Con fillMaxSize() +
    // scroll interno, en el Dashboard el panel se comía TODA la altura y la
    // tarjeta quedaba con 0 px — inalcanzable. Con scrollable=false el panel
    // se mide a su contenido y el scroll lo pone el padre (un solo scroller,
    // sin scroll vertical anidado).
    scrollable: Boolean = true,
    modifier: Modifier = Modifier
) {
    // FIX (build): 'context' y 'savedState' estaban duplicados — una copia
    // quedó pegada DENTRO del lambda por defecto de onAntiDolbyChange (línea
    // 87-90), donde LocalContext.current/remember son ilegales por no ser
    // contexto @Composable. Se elimina esa copia y se deja una sola aquí.
    // Además phaseOracleIntensity se declaraba ANTES que savedState
    // (Unresolved reference: savedState) — ahora va después.
    val context = LocalContext.current
    val savedState = remember { AdaptiveControlsPrefs.load(context) }
    // Socket al daemon fuera del hilo UI; en un slider rápido solo interesa
    // el último valor, no una cola de cientos de comandos intermedios.
    val daemonCommands = remember {
        kotlinx.coroutines.channels.Channel<String>(kotlinx.coroutines.channels.Channel.CONFLATED)
    }
    LaunchedEffect(daemonCommands) {
        for (command in daemonCommands) {
            kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                runCatching { com.ivanna.omega.magisk.MagiskBridge.sendCommand(command) }
            }
        }
    }

    var phaseOracleIntensity by remember { mutableFloatStateOf(savedState.phaseOracleIntensity) }
    var antiDolbyThreshold by remember { mutableFloatStateOf(savedState.antiDolbyThreshold) }
    var spatialSuppression by remember { mutableFloatStateOf(savedState.spatialSuppression) }
    var spscRingFactor by remember {
        mutableFloatStateOf(
            if (IvannaNativeLib.isLoaded)
                runCatching { IvannaNativeLib.nativeGetSpscRingFactor() }.getOrDefault(savedState.spscRingFactor)
            else savedState.spscRingFactor
        )
    }
    var tinymlInferenceGain by remember { mutableFloatStateOf(savedState.tinymlInferenceGain) }
    var volterraEnabled by remember {
        mutableStateOf(
            if (IvannaNativeLib.isLoaded) runCatching { IvannaNativeLib.nativeIsVolterraEnabled() }.getOrDefault(true) else true
        )
    }
    // v2.5.369: ¿hay un kernel Volterra cargado de verdad? El toggle dice la
    // intención del usuario; esto dice el estado real del motor. Se refresca
    // en el bucle de telemetría de 500 ms junto a unity/punch/evoEq.
    var volterraKernelLoaded by remember { mutableStateOf(false) }
    // v2.5.369 (selector PZ1D/Genezi): device id del kernel que suena de
    // verdad ("MOTO_G85_PZ1D" / "MOTO_G85_GENEZY" / "" si no hay kernel).
    // Se refresca en el mismo bucle de 500 ms. Mientras cambia,
    // volterraSwitching bloquea el selector para no solapar cambios.
    var volterraKernelId by remember { mutableStateOf("") }
    var volterraSwitching by remember { mutableStateOf(false) }
    // Petición de cambio de kernel (selector): la ejecuta el
    // LaunchedEffect de abajo en el hilo de CONTROL (la vía nativa puede
    // dormir ~300 ms esperando silencio); nunca bloquear la UI.
    var volterraSwitchRequest by remember { mutableStateOf<String?>(null) }
    // Sensor acústico en vivo (v2.5.369): alimenta Φ en MEDIDO. Se lee del
    // sensor real (RealAcousticSensor), no se supone. sensorMicDenied marca
    // que el permiso quedó denegado (el botón pasa a abrir Ajustes).
    var sensorRunning by remember { mutableStateOf(false) }
    var sensorFrameIndex by remember { mutableLongStateOf(-1L) }
    var sensorSource by remember { mutableStateOf("none") }
    var sensorMicDenied by remember { mutableStateOf(false) }
    // Coherencia media mic/salida de Φ (v2.5.450, nativeHyperStatus [6]):
    // es la evidencia de que Φ corrige de verdad. MEDIDO exige coh > 0.
    var hyperMeanCoherence by remember { mutableFloatStateOf(0f) }
    // Modo de Φ (v2.5.451, nativeHyperStatus [1]): 0 apagado, 1 APROXIMADO,
    // 2 MEDIDO, 3 CALIBRADO (perfil guardado con DAC USB, sin mic en vivo).
    var hyperMode by remember { mutableIntStateOf(0) }
    // Permiso de micrófono para el sensor acústico. Si ya está concedido,
    // el sistema responde al instante sin mostrar diálogo.
    val micPermissionLauncher = rememberLauncherForActivityResult(
        ActivityResultContracts.RequestPermission()
    ) { granted ->
        sensorMicDenied = !granted
        if (granted) runCatching {
            RealAcousticSensor.start(context)
            applySensorPrefs(context)
        }
    }
    // EQ evolutivo de 512 bandas en Ruta A (paridad con Ruta B). El interruptor es la eleccion PERSISTIDA del
    // usuario (ON por defecto). No se pinta con nativeIsEvolutionaryEqActive(): ese devuelve false mientras
    // la calibracion corre en su hilo (unos segundos tras arrancar) y mostraria "apagado" estando encendido.
    var evoEqEnabled by remember { mutableStateOf(com.ivanna.omega.core.EvolutionaryEqPrefs.isEnabled(context)) }
    // Estado del filtro ya calibrado/validado; separado de la intención persistida del interruptor.
    var evoEqActive by remember { mutableStateOf(false) }
    var fastRpcEnabled by remember {
        mutableStateOf(
            if (IvannaNativeLib.isLoaded) runCatching { IvannaNativeLib.nativeIsFastRpcEnabled() }.getOrDefault(false) else false
        )
    }
    var atiEnabled by remember {
        mutableStateOf(
            if (IvannaNativeLib.isLoaded) runCatching { IvannaNativeLib.nativeIsAtiEnabled() }.getOrDefault(true) else true
        )
    }
    // v2.5.15: Acoustic Unity Engine — interruptor persistente (su propio SharedPreferences) que se
    // reaplica al nativo al componer, y telemetría MEDIDA (original vs salida) refrescada cada 500 ms.
    val unityPrefs = remember { context.getSharedPreferences("ivanna_unity", android.content.Context.MODE_PRIVATE) }
    var unityEnabled by remember { mutableStateOf(unityPrefs.getBoolean("enabled", true)) }
    var unityTelemetry by remember { mutableStateOf(FloatArray(0)) }
    var punchAmount by remember { mutableFloatStateOf(initialPunchAmount) }
    var crossfeed by remember { mutableFloatStateOf(initialCrossfeed) }
    // Mezcla de captura AJUSTABLE (v2.5.412): cuánto del procesado se
    // reinyecta frente al original de Tidal (100%). Default 0.60 = el
    // valor vivo actual (DELTA_BLEND_SOLO). El engine la aplica con
    // rampa per-sample (sin zipper noise) en ≤250 ms.
    var captureMix by remember {
        mutableFloatStateOf(
            com.ivanna.omega.audio.PlaybackCaptureService.getCaptureMixGain(context)
        )
    }
    // ORBE — Binaural Esférico de Campo Cercano (v2.5.363), ACTIVO por defecto en v2.5.369 (amount 1.0).
    var orbeAmount by remember { mutableFloatStateOf(initialOrbeAmount) }
    var orbeDistanceM by remember { mutableFloatStateOf(initialOrbeDistanceM) }
    var orbeSpread by remember { mutableFloatStateOf(initialOrbeSpread) }
    var orbeProximityDb by remember { mutableFloatStateOf(initialOrbeProximityDb) }
    var orbeDiffuse by remember { mutableFloatStateOf(initialOrbeDiffuse) }
    var orbeHeadTracking by remember { mutableStateOf(initialOrbeHeadTracking) }
    var holosAmount by remember { mutableFloatStateOf(initialHolosAmount) }
    var holosDistanceM by remember { mutableFloatStateOf(initialHolosDistanceM) }
    var holosSpread by remember { mutableFloatStateOf(initialHolosSpread) }
    var holosProximityDb by remember { mutableFloatStateOf(initialHolosProximityDb) }
    var holosDiffuse by remember { mutableFloatStateOf(initialHolosDiffuse) }
    var holosHarmonics by remember { mutableFloatStateOf(initialHolosHarmonics) }
    var holosWarmth by remember { mutableFloatStateOf(initialHolosWarmth) }
    var holosDynamics by remember { mutableFloatStateOf(initialHolosDynamics) }
    var holosWidth by remember { mutableFloatStateOf(initialHolosWidth) }
    var holosGamma by remember { mutableFloatStateOf(initialHolosGamma) }
    var holosRoomRt60 by remember { mutableFloatStateOf(initialHolosRoomRt60) }
    var holosHeadTracking by remember { mutableStateOf(initialHolosHeadTracking) }
    var holosBandCount by remember { mutableIntStateOf(initialHolosBandCount) }
    var holosEqLow by remember { mutableFloatStateOf(initialHolosEqLow) }
    var holosEqMid by remember { mutableFloatStateOf(initialHolosEqMid) }
    var holosEqHigh by remember { mutableFloatStateOf(initialHolosEqHigh) }
    var holosEqPresence by remember { mutableFloatStateOf(initialHolosEqPresence) }
    var holosEqTiltDb by remember { mutableFloatStateOf(initialHolosEqTiltDb) }
    var holosEqFreq by remember { mutableFloatStateOf(initialHolosEqFreq) }
    // Hiper-Operador de Estado Φ (v2.5.369).
    var hyperAmount by remember { mutableFloatStateOf(initialHyperAmount) }
    var hyperPreset by remember { mutableIntStateOf(initialHyperPreset) }
    var hyperXtc by remember { mutableStateOf(initialHyperXtc) }
    var hyperTargetKind by remember { mutableIntStateOf(initialHyperTargetKind) }
    var hyperBassTrimDb by remember { mutableFloatStateOf(initialHyperBassTrimDb) }
    var punchTelemetry by remember { mutableStateOf(FloatArray(0)) }
    LaunchedEffect(Unit) {
        // FIX (crash 506 - ConcurrentModificationException en Compose, 3x el
        // 2026-11-27 02:11): este bucle corria ENTERO en Dispatchers.Default
        // y escribia estado de Compose desde un hilo de fondo mientras el
        // hilo principal lo leia en layout. Ahora lo pesado (JNI) corre en
        // Default y TODA escritura de estado corre en Main, el hilo de este
        // LaunchedEffect.
        if (IvannaNativeLib.isLoaded) {
            val unityFallback = unityEnabled
            withContext(Dispatchers.Default) {
                runCatching { IvannaNativeLib.nativeSetUnityEnabled(unityFallback) }
            }
            // La UI muestra el estado REAL del motor nativo, no el supuesto.
            unityEnabled = withContext(Dispatchers.Default) {
                runCatching { IvannaNativeLib.nativeIsUnityEnabled() }.getOrDefault(unityFallback)
            }
        }
        while (isActive) {
            val switching = volterraSwitching
            val snap = withContext(Dispatchers.Default) {
                val loaded = IvannaNativeLib.isLoaded
                val unityTel = if (loaded) runCatching { IvannaNativeLib.nativeGetUnityTelemetry() }.getOrDefault(FloatArray(0)) else FloatArray(0)
                val punchTel = if (loaded) runCatching { IvannaNativeLib.nativeGetPunchTelemetry() }.getOrDefault(FloatArray(0)) else FloatArray(0)
                val evoActive = if (loaded) runCatching { IvannaNativeLib.nativeIsEvolutionaryEqActive() }.getOrDefault(false) else false
                val kernelLoaded = if (loaded) runCatching { IvannaNativeLib.nativeHasVolterraKernel() }.getOrDefault(false) else false
                // Coherencia media de Phi (v2.5.450): sin .so o sin lectura, 0.
                val meanCoherence = if (loaded) runCatching { IvannaNativeLib.nativeHyperStatus().getOrNull(6) ?: 0f }.getOrDefault(0f) else 0f
                // Modo de Phi (v2.5.451): 3 = CALIBRADO (perfil guardado, DAC).
                val mode = if (loaded) runCatching { (IvannaNativeLib.nativeHyperStatus().getOrNull(1) ?: 0f).toInt() }.getOrDefault(0) else 0
                val kernelId = if (loaded && !switching) runCatching { IvannaNativeLib.activeVolterraKernelId(context) }.getOrDefault("") else ""
                // Sensor acustico: se lee del objeto real, independiente de la
                // biblioteca nativa (frameIndex() devuelve -1 sin .so).
                UnityTelemetrySnap(
                    loaded = loaded,
                    unityTelemetry = unityTel,
                    punchTelemetry = punchTel,
                    evoEqActive = evoActive,
                    volterraKernelLoaded = kernelLoaded,
                    hyperMeanCoherence = meanCoherence,
                    hyperMode = mode,
                    volterraKernelId = kernelId,
                    sensorRunning = RealAcousticSensor.isRunning,
                    sensorFrameIndex = RealAcousticSensor.frameIndex(),
                    sensorSource = RealAcousticSensor.audioSourceUsed,
                )
            }
            if (snap.loaded) {
                unityTelemetry = snap.unityTelemetry
                punchTelemetry = snap.punchTelemetry
            }
            evoEqActive = snap.evoEqActive
            volterraKernelLoaded = snap.volterraKernelLoaded
            hyperMeanCoherence = snap.hyperMeanCoherence
            hyperMode = snap.hyperMode
            if (!switching) volterraKernelId = snap.volterraKernelId
            sensorRunning = snap.sensorRunning
            sensorFrameIndex = snap.sensorFrameIndex
            sensorSource = snap.sensorSource
            delay(500)
        }
    }
    // v2.5.369 (selector PZ1D/Genezi): ejecuta el cambio de kernel pedido
    // por el selector. Corre en el hilo de CONTROL (la vía nativa puede
    // dormir ~300 ms esperando silencio); la UI nunca se bloquea. Al
    // terminar se lee el id REAL del motor: si el cambio falló, sigue el
    // kernel anterior y el selector vuelve a él solo.
    LaunchedEffect(volterraSwitchRequest) {
        val target = volterraSwitchRequest ?: return@LaunchedEffect
        withContext(Dispatchers.Default) {
            runCatching { IvannaNativeLib.setVolterraKernel(context, target) }
        }
        volterraKernelId = runCatching { IvannaNativeLib.activeVolterraKernelId(context) }.getOrDefault(volterraKernelId)
        volterraSwitchRequest = null
        volterraSwitching = false
    }

    // Persistencia inmediata en cada cambio para todos los controles del panel.
    // FIX: phaseOracleIntensity y omegaMode sólo se guardaban en DisposableEffect
    // ON_STOP — si la app crasheaba o era matada por el sistema, el valor se perdía.
    // FIX (evo kernel descableado + sin persistencia, 2026-09-07): el toggle
    // KERNEL EVOLUTIVO persistía en AdaptiveControlsPrefs y cambiaba el hilo
    // nativo en caliente (onEvoEnabledChange → nativeStart/StopEvoThread),
    // pero al reabrir la app el panel restauraba initialEvoEnabled SIN
    // reaplicarlo al nativo — si el usuario lo apagó y reinició, el hilo
    // corría (init llama start_evo_thread) mientras la UI mostraba OFF, y
    // el estado persistido nunca llegaba al motor. Se reaplica una vez al
    // componer: OFF persistido → stop explícito; ON → no-op (ya corre).
    LaunchedEffect(Unit) {
        if (!initialEvoEnabled && IvannaNativeLib.isLoaded) {
            runCatching { IvannaNativeLib.guardedNative(Unit) { IvannaNativeLib.nativeStopEvoThread() } }
        }
    }

    LaunchedEffect(antiDolbyThreshold, spatialSuppression, spscRingFactor, tinymlInferenceGain) {
        val cur = AdaptiveControlsPrefs.load(context)
        AdaptiveControlsPrefs.save(context, cur.copy(
            antiDolbyThreshold = antiDolbyThreshold,
            spatialSuppression = spatialSuppression,
            spscRingFactor = spscRingFactor,
            tinymlInferenceGain = tinymlInferenceGain
        ))
    }
    LaunchedEffect(phaseOracleIntensity) {
        val cur = AdaptiveControlsPrefs.load(context)
        AdaptiveControlsPrefs.save(context, cur.copy(phaseOracleIntensity = phaseOracleIntensity))
    }
    var tonalLow by remember { mutableFloatStateOf(initialTonalLowDb) }
    var tonalMid by remember { mutableFloatStateOf(initialTonalMidDb) }
    var tonalHigh by remember { mutableFloatStateOf(initialTonalHighDb) }
    var tonalPresence by remember { mutableFloatStateOf(initialTonalPresenceDb) }
    var tonalTrim by remember { mutableFloatStateOf(initialTonalTrimDb) }
    var masterDb by remember { mutableFloatStateOf(initialMasterDb) }
    var spatialModeSpeakers by remember { mutableStateOf(initialSpatialModeSpeakers) }
    var captureActiveState by remember { mutableStateOf(initialCaptureActive) }
    var exciter by remember { mutableFloatStateOf(initialExciter) }
    var width by remember { mutableFloatStateOf(initialWidth) }

    var antiDolbyEnabled by remember { mutableStateOf(initialAntiDolby) }
    var selectedPreset by remember { mutableStateOf(initialPreset) }
    var autoMode by remember { mutableStateOf(initialAutoMode) }
    var omegaMode by remember { mutableIntStateOf(savedState.omegaMode) }
    var compThresholdDb by remember { mutableFloatStateOf(initialCompThresholdDb) }
    var compRatioValue by remember { mutableFloatStateOf(initialCompRatioValue) }
    var nhoHarmonic by remember { mutableFloatStateOf(savedState.nhoHarmonic) }
    var spatialAngle by remember { mutableFloatStateOf(savedState.spatialAngle) }
    var spatialWidth by remember { mutableFloatStateOf(savedState.spatialWidth) }
    var evoEnabled by remember { mutableStateOf(initialEvoEnabled) }
    var evoFitness by remember { mutableFloatStateOf(0f) }
    var evoGeneration by remember { mutableIntStateOf(0) }
    var npeBypass by remember { mutableStateOf(initialNpeBypass) }
    var npeHarmonic by remember { mutableFloatStateOf(savedState.npeHarmonic) }
    var npeLateralInhib by remember { mutableFloatStateOf(savedState.npeLateralInhib) }
    var npeOhcCompression by remember { mutableFloatStateOf(savedState.npeOhcCompression) }
    var npeMasterGain by remember { mutableFloatStateOf(savedState.npeMasterGain) }
    var npeAgcTarget by remember { mutableFloatStateOf(savedState.npeAgcTarget) }
    var npeAgcRate by remember { mutableFloatStateOf(savedState.npeAgcRate) }
    // FIX (build): estos 3 LaunchedEffect estaban ANTES de las declaraciones
    // de omegaMode/nhoHarmonic/spatialAngle/spatialWidth/npeHarmonic/etc.
    // (usados antes de declararse -> "Unresolved reference" en compileDebugKotlin,
    // build del APK roto). Se mueven aqui, justo despues de que todas las
    // variables que referencian ya existen.
    LaunchedEffect(omegaMode) {
        val cur = AdaptiveControlsPrefs.load(context)
        AdaptiveControlsPrefs.save(context, cur.copy(omegaMode = omegaMode))
    }
    LaunchedEffect(nhoHarmonic, spatialAngle, spatialWidth) {
        val cur = AdaptiveControlsPrefs.load(context)
        AdaptiveControlsPrefs.save(context, cur.copy(
            nhoHarmonic  = nhoHarmonic,
            spatialAngle = spatialAngle,
            spatialWidth = spatialWidth
        ))
    }
    LaunchedEffect(npeHarmonic, npeLateralInhib, npeOhcCompression, npeMasterGain, npeAgcTarget, npeAgcRate) {
        val cur = AdaptiveControlsPrefs.load(context)
        AdaptiveControlsPrefs.save(context, cur.copy(
            npeHarmonic       = npeHarmonic,
            npeLateralInhib   = npeLateralInhib,
            npeOhcCompression = npeOhcCompression,
            npeMasterGain     = npeMasterGain,
            npeAgcTarget      = npeAgcTarget,
            npeAgcRate        = npeAgcRate
        ))
    }
    var npeHrtf by remember { mutableStateOf(initialNpeHrtf) }
    var npeCochlear by remember { mutableStateOf(initialNpeCochlear) }
    var npeAdapt by remember { mutableStateOf(initialNpeAdapt) }
    var npeManifold by remember { mutableStateOf(initialNpeManifold) }
    var npeGenre by remember { mutableStateOf("\u2014") }
    var npeRmsDb by remember { mutableFloatStateOf(-60f) }
    var npeAgcGainDb by remember { mutableFloatStateOf(0f) }
    var npeClassifyConfidence by remember { mutableFloatStateOf(0f) }
    var npeClassifyThd by remember { mutableFloatStateOf(0f) }
    var npeInferenceUs by remember { mutableLongStateOf(-1L) }
    var lastAutoAppliedGenre by remember { mutableStateOf<String?>(null) }
    var spatialEnabled by remember { mutableStateOf(initialSpatialEnabled) }
    var hoaUpmixingEnabled by remember { mutableStateOf(initialHoaUpmixingEnabled) }
    var hoaImmersivity by remember { mutableFloatStateOf(initialHoaImmersivity) }
    var wfsEnabled by remember { mutableStateOf(initialWfsEnabled) }
    var wfsSpread by remember { mutableFloatStateOf(initialWfsSpread) }
    var auxEnabled by remember { mutableStateOf(initialAuxFieldEnabled) }
    var temporalEnabled by remember { mutableStateOf(initialTemporalFieldEnabled) }
    var temporalAmount by remember { mutableStateOf(initialTemporalFieldAmount) }
    var temporalAdaptive by remember { mutableStateOf(initialTemporalFieldAdaptive) }
    var hapticEnabled by remember { mutableStateOf(initialBassHapticEnabled && bassHapticSupported) }
    var hapticStrength by remember { mutableStateOf(initialBassHapticStrength) }
    var hapticRumble by remember { mutableStateOf(initialBassHapticRumble) }
    var hapticTrim by remember { mutableStateOf(initialBassHapticTrimMs) }
    var hapticSyncText by remember { mutableStateOf(com.ivanna.omega.audio.BassHaptics.describeSync()) }
    LaunchedEffect(hapticEnabled) {
        while (hapticEnabled) {
            hapticSyncText = com.ivanna.omega.audio.BassHaptics.describeSync()
            kotlinx.coroutines.delay(1000L)
        }
    }
    var auxXover by remember { mutableFloatStateOf(initialAuxXoverHz) }
    var auxCoherence by remember { mutableFloatStateOf(initialAuxCoherence) }

    
    // ── Persistencia automática al salir de la pantalla ──
    // FIX (build): LocalLifecycleOwner.current se leía DENTRO del cuerpo de
    // DisposableEffect y del onDispose — ninguno es contexto @Composable.
    // Se captura una sola vez aquí (sí composable) y se usa el lifecycle
    // capturado dentro del efecto; así addObserver/removeObserver operan
    // sobre el MISMO lifecycle (antes podían resolver a owners distintos).
    val lifecycleOwner = LocalLifecycleOwner.current
    DisposableEffect(lifecycleOwner) {
        val observer = LifecycleEventObserver { _, event ->
            if (event == Lifecycle.Event.ON_STOP) {
                val current = AdaptiveControlsPrefs.load(context)
                AdaptiveControlsPrefs.save(context, current.copy(
                    antiDolbyEnabled = antiDolbyEnabled,
                    selectedPreset = selectedPreset,
                    autoMode = autoMode,
                    omegaMode = omegaMode,
                    nhoHarmonic = nhoHarmonic,
                    spatialAngle = spatialAngle,
                    spatialWidth = spatialWidth,
                    evoEnabled = evoEnabled,
                    npeBypass = npeBypass,
                    npeHarmonic = npeHarmonic,
                    npeLateralInhib = npeLateralInhib,
                    npeOhcCompression = npeOhcCompression,
                    npeMasterGain = npeMasterGain,
                    npeAgcTarget = npeAgcTarget,
                    npeAgcRate = npeAgcRate,
                    npeHrtf = npeHrtf,
                    npeCochlear = npeCochlear,
                    npeAdapt = npeAdapt,
                    npeManifold = npeManifold,
                    spatialEnabled = spatialEnabled,
                    phaseOracleIntensity = phaseOracleIntensity
                ))
            }
        }
        lifecycleOwner.lifecycle.addObserver(observer)
        onDispose { lifecycleOwner.lifecycle.removeObserver(observer) }
    }

    val rmsHistory = remember { mutableStateListOf<Float>().apply { repeat(32) { add(-60f) } } }

    LaunchedEffect(Unit) {
        // FIX (crash 506 - ConcurrentModificationException en Compose, 3x el
        // 2026-11-27 02:11): este bucle corria ENTERO en Dispatchers.Default
        // y mutaba rmsHistory (removeAt/add) y el resto del HUD desde un hilo
        // de fondo mientras LiveTelemetryHud iteraba la lista en layout. Ahora
        // lo pesado (JNI + lectura del bus) corre en Default y TODA escritura
        // de estado corre en Main.
        delay(800)
        while (true) {
            // FIX (HUD "en vivo" mostraba datos congelados de la ultima
            // sesion de captura, para siempre, incluso con MediaProjection
            // ya terminado): sin este guard, npeGenre/npeRmsDb/etc. nunca se
            // reseteaban al morir la captura - el titulo dice "tiempo real"
            // pero el estado era "ultimo valor conocido, sin caducidad".
            // FIX (HUD muerto fuera de captura): la telemetria solo se daba por viva con
            // MediaProjection activo; con el motor NPE/nativo procesando por otra via
            // (modulo Magisk, bridge) quedaba clavada en -60 dB / "-". Ahora se considera
            // viva si hay captura, inferencia NPE reciente o nivel en el bus compartido.
            val snap = withContext(Dispatchers.Default) {
                val sharedRmsLin = com.ivanna.omega.audio.OmegaMetrics.shared.value.rmsLevel
                val npeLive = IvannaNpeEngine.isReady && IvannaNpeEngine.lastInferenceUs >= 0L
                if (!com.ivanna.omega.audio.PlaybackCaptureService.isCapturing.value &&
                    !npeLive && sharedRmsLin <= 1e-6f) {
                    NpeHudSnap(genre = "\u2014", rmsDb = -60f, agcDb = 0f, confidence = 0f, thd = 0f, inferenceUs = -1L)
                } else {
                    val genre = if (IvannaNpeEngine.isReady) IvannaNpeEngine.getDetectedGenre() else "\u2014"
                    val m = if (IvannaNpeEngine.isReady) IvannaNpeEngine.getMetrics() else FloatArray(0)
                    val rmsLin = m.getOrElse(1) { 0f }.takeIf { it > 1e-6f } ?: sharedRmsLin
                    val rmsDb = if (rmsLin > 1e-6f) (20f * log10(rmsLin)) else -60f
                    val agcLin = m.getOrElse(2) { 1f }
                    val agcDb = if (agcLin > 1e-6f) (20f * log10(agcLin)) else 0f
                    val c = if (IvannaNpeEngine.isReady) IvannaNpeEngine.getSynthClassify() else FloatArray(0)
                    NpeHudSnap(
                        genre = genre,
                        rmsDb = rmsDb,
                        agcDb = agcDb,
                        confidence = c.getOrElse(1) { 0f },
                        thd = c.getOrElse(2) { 0f },
                        inferenceUs = IvannaNpeEngine.lastInferenceUs,
                    )
                }
            }
            npeGenre = snap.genre
            npeRmsDb = snap.rmsDb
            npeAgcGainDb = snap.agcDb
            npeClassifyConfidence = snap.confidence
            npeClassifyThd = snap.thd
            npeInferenceUs = snap.inferenceUs
            rmsHistory.removeAt(0)
            rmsHistory.add(snap.rmsDb)

            // FIX ("presets automaticos" nunca existia pese a que autoMode ya
            // prometia "Auto IA seleccionando" en el subtitulo - solo subia
            // un multiplicador visual (omniLevel) y bloqueaba los controles
            // manuales; ningun codigo seleccionaba realmente un preset).
            // Mapeo honesto a los 8 generos reales que autonomous_brain.hpp
            // puede devolver (ratios espectrales medidos, no adivinados),
            // sobre el catalogo de 14 presets ya construido - con debounce
            // para no recargar el mismo preset en cada bloque de 750ms.
            if (autoMode && npeGenre != lastAutoAppliedGenre && npeGenre != "\u2014") {
                val profile = when (npeGenre) {
                    "Hip-Hop / EDM"    -> com.ivanna.omega.audio.IvannaEffectProfile.PUNCH
                    "Pop"              -> com.ivanna.omega.audio.IvannaEffectProfile.IVANNA_OMEGA
                    "Reggaet\u00f3n"   -> com.ivanna.omega.audio.IvannaEffectProfile.PUNCH
                    "Rock / Metal"     -> com.ivanna.omega.audio.IvannaEffectProfile.ROCK_70S
                    "Jazz / Cl\u00e1sica" -> com.ivanna.omega.audio.IvannaEffectProfile.STUDIO_PRO
                    "Ac\u00fastica"    -> com.ivanna.omega.audio.IvannaEffectProfile.ABBEY_ROAD
                    "Electr\u00f3nica" -> com.ivanna.omega.audio.IvannaEffectProfile.SPATIAL
                    else               -> com.ivanna.omega.audio.IvannaEffectProfile.WARM  // "Mixto"
                }
                withContext(Dispatchers.Default) {
                    runCatching {
                        com.ivanna.omega.dsp.DSPState.globalEffectManager?.applyProfile(profile)
                    }
                }
                lastAutoAppliedGenre = npeGenre
            }

            delay(750)
        }
    }

    LaunchedEffect(Unit) {
        // FIX (crash 506 - ConcurrentModificationException en Compose, 3x el
        // 2026-11-27 02:11): este bucle escribia evoFitness/evoGeneration
        // desde Dispatchers.Default. Ahora el JNI corre en Default y la
        // escritura de estado corre en Main.
        delay(1200)
        while (true) {
            try {
                // FIX (kernel evolutivo "no jalaba"): nativeGetBestFitness()
                // solo tenia implementacion nativa en evolutionary_kernel.cpp
                // (v1) - archivo excluido del CMakeLists activo desde hace
                // tiempo. El simbolo JNI no existe en el .so real: esta
                // llamada lanzaba UnsatisfiedLinkError en CADA tick, atrapado
                // aqui y logueado como "no disponible todavia" para siempre.
                // El kernel v2 (evolutionary_kernel_v2.cpp, el que SI compila
                // y SI evoluciona en background via pd_engine.hpp) expone su
                // fitness real por nativeGetEvoBestFitness() - ya usado
                // correctamente mas abajo en este mismo archivo (linea ~820).
                val (fitness, generation) = withContext(Dispatchers.Default) {
                    IvannaNativeLib.nativeGetEvoBestFitness() to IvannaNativeLib.nativeGetGeneration()
                }
                evoFitness = fitness
                evoGeneration = generation
            } catch (e: Throwable) {
                Log.w("IvannaControlPanel", "Kernel evolutivo no disponible todavía", e)
            }
            delay(2000)
        }
    }

    val omniLevel by remember(omegaMode, npeBypass, spatialEnabled, evoEnabled, antiDolbyEnabled, autoMode) {
        mutableFloatStateOf(
            listOf(
                if (omegaMode > 0) 1f else 0.3f,
                if (!npeBypass) 1f else 0.15f,
                if (spatialEnabled) 1f else 0.2f,
                if (evoEnabled) 1f else 0.2f,
                if (antiDolbyEnabled) 1f else 0.4f,
                if (autoMode) 1f else 0.6f
            ).average().toFloat()
        )
    }

    // El scroll del panel es condicional (ver param scrollable): con
    // scrollable=true es el scroller de la pantalla (ControlTabScreen);
    // con false el padre pone el scroll y este estado no se usa.
    val panelScrollState = rememberScrollState()
    Column(
        modifier = if (scrollable) {
            modifier
                .fillMaxSize()
                .background(
                    Brush.radialGradient(
                        colors = listOf(
                            MagistralPanel.copy(alpha = 0.65f),
                            MagistralVoid.copy(alpha = 0.92f)
                        ),
                        radius = 1400f
                    )
                )
                .verticalScroll(panelScrollState)
                .padding(horizontal = 16.dp, vertical = 20.dp)
        } else {
            modifier
                .fillMaxWidth()
                .background(
                    Brush.radialGradient(
                        colors = listOf(
                            MagistralPanel.copy(alpha = 0.65f),
                            MagistralVoid.copy(alpha = 0.92f)
                        ),
                        radius = 1400f
                    )
                )
                .padding(horizontal = 16.dp, vertical = 20.dp)
        },
        verticalArrangement = Arrangement.spacedBy(16.dp)
    ) {
        // Ficha del entorno · Control (prototipo aprobado, Pantalla III · Motor)
        ScreenSpecCard(spec = controlScreenSpec)

        RouteModeBanner()
        OmniHeroHeader(
            omegaMode = omegaMode,
            npeActive = !npeBypass,
            spatialActive = spatialEnabled,
            autoMode = autoMode,
            omniLevel = omniLevel,
            routeState = routeState,
            orbeActive = orbeAmount > 0f,
            holosActive = holosAmount > 0f,
            hyperActive = hyperAmount > 0f,
            volterraActive = volterraEnabled
        )

        EngineStatusCard(metrics = metrics)

        // ── v2.5.506 · ORGANISMO ───────────────────────────────────────────
        // Interruptor maestro + indicador de estado (nada más). El cableado
        // mic → organismo arranca solo al abrir la app (OrganismController).
        OrganismCard(
            state = organismState,
            onToggle = onOrganismToggle,
            onSaveToken = onOrganismSaveToken,
            onClearQueue = onOrganismClearQueue
        )

        // ── v2.5.483 · FÍSICA NUEVA v3 ─────────────────────────────────────
        // Las tarjetas NP estaban HUÉRFANAS (definidas pero nunca colocadas:
        // por eso Esse no veía la pantalla ni los controles en la 482).
        // Aquí van: controles Ω completos (16 sliders + 7 equipos + ABX)
        // y órganos v3 (sub-graves modal, cola RIR, Klippel H3 + térmica).
        NewPhysicsCardsColumn()

        // ── v2.5.484 · GUÍA DE CONTROLES ───────────────────────────────────
        // La ayuda existía en ControlHelpCatalog.kt pero nunca se montaba:
        // por eso Esse no veía para qué sirve cada control.
        ControlHelpCatalogPanel()

        // ── v2.5.473 §1.3 C1/C2/C3/C8 ─────────────────────────────────────
        // C2 · Perfil activo (el picker único es ProfileSelectorScreen, ruta
        // "profiles"): aquí solo se muestra y se abre.
        GlassCard(title = "PERFIL ACTIVO", accent = MagistralGold, subtitle = "El perfil manda: EQ, ORBE, HOLOS, Φ y Volterra", rightSlot = { SpecInfoButton(spec = FusedControlSpecs.profileBand) }) {
            Text(
                if (fusedProfileName.isNotBlank()) fusedProfileName else "Sin perfil aplicado",
                style = MaterialTheme.typography.titleMedium,
                color = TextPrimary
            )
            Spacer(modifier = Modifier.height(4.dp))
            Text(
                "Referencia: «Fly Like an Eagle» — Steve Miller Band (1976). Neto 0.0 dB.",
                style = MaterialTheme.typography.labelSmall,
                color = TextMuted
            )
            Spacer(modifier = Modifier.height(8.dp))
            Button(onClick = { onOpenProfiles() }) { Text("CAMBIAR PERFIL") }
            // Formato IVANNA OMEGA: archivo .ivanna.json por canción. El
            // archivo trae su perfil (ORBE/HOLOS/Φ/Volterra, EQ, compresor,
            // exciter, trim) y el motor lo aplica al elegirlo. Si no es
            // válido, no se aplica nada y se muestra el motivo.
            Spacer(modifier = Modifier.height(12.dp))
            Text(
                "ARCHIVO IVANNA OMEGA",
                style = MaterialTheme.typography.labelMedium,
                color = TextMuted
            )
            if (omegaFileLabel.isNotBlank()) {
                Text(
                    omegaFileLabel,
                    style = MaterialTheme.typography.labelSmall,
                    color = TextPrimary
                )
            }
            if (omegaStatus.isNotEmpty()) {
                Text(
                    omegaStatus,
                    style = MaterialTheme.typography.labelSmall,
                    color = TextSecondary
                )
            }
            Spacer(modifier = Modifier.height(4.dp))
            OutlinedButton(onClick = onLoadOmegaFile) { Text("CARGAR ARCHIVO IVANNA OMEGA") }
        }

        // C1 · Salida y captura (default OFF: nada suena hasta que el usuario
        // lo enciende, §4.1).
        GlassCard(title = "SALIDA Y CAPTURA", accent = AuroraCyan, subtitle = "Dónde suena y qué se captura") {
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("Salida: ${if (spatialModeSpeakers) "Genezi (bocinas)" else "Audífonos"}", color = TextPrimary)
                SpecInfoButtonSmall(spec = FusedControlSpecs.outputMode)
                MagistralSwitch(checked = spatialModeSpeakers, onCheckedChange = {
                    spatialModeSpeakers = it; onSpatialModeChange(it)
                })
            }
            Spacer(modifier = Modifier.height(8.dp))
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.SpaceBetween
            ) {
                Text("Captura del sistema", color = TextPrimary)
                SpecInfoButtonSmall(spec = FusedControlSpecs.systemCapture)
                MagistralSwitch(checked = captureActiveState, onCheckedChange = {
                    captureActiveState = it; onToggleCapture()
                })
            }
        }

        // C3 · Volumen maestro (dB, un solo sitio).
        GlassCard(title = "VOLUMEN MAESTRO", accent = AmberSignal, subtitle = "Un solo sitio para el volumen", rightSlot = { SpecInfoButton(spec = FusedControlSpecs.masterVolume) }) {
            AuroraSlider("MASTER", masterDb, -24f..12f, unit = "dB") { masterDb = it; onMasterChange(it) }
        }

        // C8 · Indicadores fusionados, solo lectura (tap → FUSIÓN, donde se
        // editan: ORBE #1, HOLOS #2, Φ #3, Volterra #4).
        Row(
            modifier = Modifier.fillMaxWidth().clickable { onOpenFusion() }.padding(vertical = 4.dp),
            horizontalArrangement = Arrangement.Center
        ) {
            Text(
                "ORBE %.2f · HOLOS %.2f · Φ %.2f · Volterra %s → FUSIÓN".format(
                    initialFusedOrbe, initialFusedHolos, initialFusedPhi,
                    if (initialFusedVolterra) "ON" else "OFF"
                ),
                style = MaterialTheme.typography.labelMedium,
                color = TextMuted
            )
        }

        LiveTelemetryHud(
            rmsDb = npeRmsDb,
            rmsHistory = rmsHistory,
            agcDb = npeAgcGainDb,
            genre = npeGenre,
            confidence = npeClassifyConfidence,
            thd = npeClassifyThd,
            evoFitness = evoFitness,
            evoGeneration = evoGeneration
        )

        // ── NAEL / ISO 226:2023 — Equal Loudness Compensation ─────────
        SectionLabel("CADENA DSP", MagistralGold)

        GlassCard(title = "DSP CORE", accent = AuroraCyan, subtitle = "EQ · Exciter · Widener · Gain") {
            AuroraSlider("EXCITER", exciter, 0f..1f, unit = "×", spec = FusedControlSpecs.dynExciter, showSpecInline = true) { exciter = it; onExciterChange(it) }
            AuroraSlider("GRAVES", tonalLow, CanonicalRanges.eqBandDb, unit = "dB", spec = FusedControlSpecs.tonalEq, showSpecInline = true) { tonalLow = it; onTonalEqChange(tonalLow, tonalMid, tonalHigh, tonalPresence) }
            AuroraSlider("MEDIOS", tonalMid, CanonicalRanges.eqBandDb, unit = "dB", spec = FusedControlSpecs.tonalEq, showSpecInline = true) { tonalMid = it; onTonalEqChange(tonalLow, tonalMid, tonalHigh, tonalPresence) }
            AuroraSlider("AGUDOS", tonalHigh, CanonicalRanges.eqBandDb, unit = "dB", spec = FusedControlSpecs.tonalEq, showSpecInline = true) { tonalHigh = it; onTonalEqChange(tonalLow, tonalMid, tonalHigh, tonalPresence) }
            AuroraSlider("PRESENCIA", tonalPresence, CanonicalRanges.eqPresenceDb, unit = "dB", spec = FusedControlSpecs.tonalEq, showSpecInline = true) { tonalPresence = it; onTonalEqChange(tonalLow, tonalMid, tonalHigh, tonalPresence) }
            AuroraSlider("TRIM · CIERRE LUFS", tonalTrim, -6f..6f, unit = "dB", spec = FusedControlSpecs.tonalTrim, showSpecInline = true) { tonalTrim = it; onTonalTrimChange(it) }
            AuroraSlider("STEREO WIDTH", width, 0f..1.5f, unit = "γ", spec = FusedControlSpecs.dynWidth) { width = it; onWidthChange(it) }
            // MEZCLA CAPTURA (v2.5.412): cuánto del procesado se reinyecta
            // frente al original de Tidal, que suena al 100% (no hay API
            // pública para silenciarlo). En captura los efectos SÍ corren;
            // lo que faltaba era presencia. Default 0.60 = el valor vivo
            // actual. El engine aplica el cambio con rampa per-sample
            // (cero zipper noise) en ≤250 ms; ver `mix=` en la telemetría.
            AuroraSlider(
                "MEZCLA CAPTURA", captureMix, 0.20f..1.00f, unit = "×", spec = FusedControlSpecs.captureMix,
                displayValue = { "%.0f%%".format(it * 100f) }
            ) {
                captureMix = it
                com.ivanna.omega.audio.PlaybackCaptureService.setCaptureMixGain(context, it)
            }
            if (captureMix > 0.50f) {
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    "Por encima de 0.50 el eco / comb filtering vuelve: el original suena al 100% y la copia procesada ya no se funde como cuerpo.",
                    style = MaterialTheme.typography.labelSmall, color = CoralWarn
                )
            }
        }

        GlassCard(title = "COMPRESOR", accent = AmberSignal, subtitle = "g_comp · dinámica lock-free") {
            // Compresor en dB canónicos (−60…0 dB, ratio 1…20): el valor
            // guardado (dB fusionado) no cambia, solo el rango del control.
            // El alpha/beta normalizado legacy se deriva en el llamador.
            AuroraSlider("THRESHOLD", compThresholdDb, CanonicalRanges.compressorThresholdDb, spec = FusedControlSpecs.dynComp, showSpecInline = true,
                displayValue = { "%.1f dB".format(it) }) {
                compThresholdDb = it; onCompThresholdChange(it)
            }
            AuroraSlider("RATIO", compRatioValue, CanonicalRanges.compressorRatio, spec = FusedControlSpecs.dynComp, showSpecInline = true,
                displayValue = { "%.1f:1".format(it) }) {
                compRatioValue = it; onCompRatioChange(it)
            }
        }

        SectionLabel("ESPACIAL Y NEUROMÓRFICO", MagistralGold)
        
        // A · Etapa de graves (prototipo, Pantalla III · Motor): la tarjeta
        // MODO AUX es el equivalente en código de la etapa de graves.
        GlassCard(
            title = "MODO AUX · ALTAVOCES",
            accent = MagistralGold,
            subtitle = if (auxEnabled)
                "Activo: graves en fase y misma presión en ambos lados · HRTF, HOA y WFS binaurales en pausa"
            else
                "Para salida por AUX/amplificador: graves coherentes (Linkwitz-Riley 4º orden). Apagado = audio intacto",
            rightSlot = {
                MagistralSwitch(checked = auxEnabled, onCheckedChange = {
                    auxEnabled = it
                    onAuxFieldChange(it, auxXover, auxCoherence)
                })
            }
        ) {
            Text(
                "Cruce 80–120 Hz y coherencia: valores del perfil de transductor (Genezi), no un slider (§2.2).",
                style = MaterialTheme.typography.labelSmall, color = TextMuted
            )
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                SpecInfoButton(spec = auxXoverSpec)
                SpecInfoButton(spec = auxCoherenceSpec)
            }
        }

        // B · Corrección acústica · Sala / Bocinas (prototipo, Pantalla III ·
        // Motor): medición de sala + vista geométrica 3D (RoomGeometryPreviewPanel
        // dentro de esta tarjeta) + nota de límites honestos.
        GlassCard(
            title = "CORRECCIÓN ACÚSTICA · SALA / BOCINAS",
            accent = MagistralGold,
            subtitle = when {
                roomBusy -> "Midiendo… no hables ni te muevas. Tonos de prueba por tus bocinas (~35 s)"
                roomGains == null -> "Mide tu sala con el micrófono del teléfono y corrige el balance tonal de 50 Hz a 6 kHz. Sin medición = audio intacto"
                roomEnabled -> "Activa: corrección medida aplicada en todas las salidas (AUX, USB-C, Bluetooth)"
                else -> "Medición guardada, corrección apagada"
            },
            rightSlot = {
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    SpecInfoButton(spec = roomCorrectionSpec)
                    MagistralSwitch(checked = roomEnabled, enabled = roomGains != null && !roomBusy, onCheckedChange = { onRoomToggle(it) })
                }
            }
        ) {
            if (roomBusy) {
                LinearProgressIndicator(progress = { roomProgress }, modifier = Modifier.fillMaxWidth())
            }
            if (roomGains != null && !roomBusy) {
                val labels = arrayOf("31.5", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k")
                val parts = (0 until 10).filter { kotlin.math.abs(roomGains[it]) >= 0.05f }
                    .joinToString(" · ") { "${labels[it]} Hz ${if (roomGains[it] > 0) "+" else ""}${"%.1f".format(java.util.Locale.US, roomGains[it])} dB" }
                Text(
                    if (parts.isEmpty()) "Tu sala ya suena pareja: no hace falta corrección" else parts,
                    style = MaterialTheme.typography.labelSmall, color = TextSecondary
                )
            }
            if (roomStatus.isNotEmpty()) {
                Text(roomStatus, style = MaterialTheme.typography.labelSmall, color = TextSecondary)
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                OutlinedButton(enabled = !roomBusy, onClick = onRoomMeasure) {
                    Text(if (roomGains == null) "MEDIR SALA" else "MEDIR DE NUEVO")
                }
                if (roomGains != null) {
                    OutlinedButton(enabled = !roomBusy, onClick = onRoomClear) { Text("BORRAR MEDICIÓN") }
                }
            }
            // v2.5.473 §5: barrido Genezi — ESS lo+hi por las bocinas,
            // grabado con el micrófono. ~25 s por nivel, paréntesis neutro.
            if (sweepBusy) {
                LinearProgressIndicator(progress = { sweepProgress }, modifier = Modifier.fillMaxWidth())
            }
            if (sweepStatus.isNotEmpty()) {
                Text(sweepStatus, style = MaterialTheme.typography.labelSmall, color = TextSecondary)
            }
            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                SpecInfoButtonSmall(spec = FusedControlSpecs.salaSweep)
            OutlinedButton(enabled = !roomBusy && !sweepBusy, onClick = onGeneziSweep) {
                Text("GRABAR BARRIDO GENEZI")
            }
            }
            com.ivanna.omega.spatial.RoomGeometryPreviewPanel()
            Text(
                "Límites honestos: el micrófono del teléfono no está calibrado; corrige el balance amplio (boom, caída de graves, presencia), no modos estrechos ni la fase, y mide el punto donde está el teléfono.",
                style = MaterialTheme.typography.labelSmall, color = TextSecondary
            )
        }

        // C · Dinámica espacial · 4D (prototipo, Pantalla III · Motor).
        Spacer(Modifier.height(4.dp))
        Text(
            "IVANNA-OMEGA-SUPREME · GORE TNS / LUPP-OR9 © 2026",
            style = MaterialTheme.typography.labelSmall,
            color = TextMuted,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth()
        )
        Spacer(Modifier.height(8.dp))
    }
}

@Composable
private fun SectionLabel(text: String, accent: Color) {
    Row(
        modifier = Modifier.fillMaxWidth().padding(top = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Text(
            text,
            style = MaterialTheme.typography.titleSmall,
            color = MagistralGoldHi,
            fontFamily = MagistralSerif,
            fontWeight = FontWeight.Bold
        )
        Spacer(
            Modifier
                .weight(1f)
                .height(1.dp)
                .background(Brush.horizontalGradient(listOf(accent.copy(alpha = 0.5f), Color.Transparent)))
        )
    }
}
