package com.ivanna.omega.audio

import android.content.Context
import android.util.Log
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.neuromorphic.IvannaNpeEngine
import com.ivanna.omega.ui.AdaptiveControlsPrefs
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import java.util.Locale

/**
 * Telemetría en vivo de TODOS los efectos de IVANNA, Ruta A (v2.5.407).
 *
 * REGLA #1 DE ESSE (2026-11-11): nada básico, magistral, riguroso, literal.
 * Esse escucha los efectos en Tidal y necesita la lectura en vivo completa
 * para calibrar la "firma magistral": una línea por segundo en logcat,
 * tag `IVANNA.Telemetry`, formato `clave=valor` parseable:
 *
 *   route=A asrc=1 ppm=+12.3 psi_eta=0.73 psi_coh=0.80 psi_active=1
 *   orbe=1.00 holos=0.50 phi=0.50 volterra=1 rms=0.12 peak=0.45
 *   gr_db=-2.10 gain_db=1.50 comp=0.20 exc=0.10 width=1.10 safety=0.90
 *   voice=0.00 applied=1 band_lo=0.30 band_mid=0.50 band_hi=0.20
 *   npe_genre=rock
 *
 * Fuentes (REUTILIZADAS, cero duplicación):
 *  - Telemetría adaptativa: IvannaNativeLib.nativeGetAdaptiveTelemetry()
 *    (10 floats: rms, peak, grDb, targetGain, compAmount, excReduction,
 *    spatialWidth, safetyMargin, voiceProtect, applied).
 *  - Bandas: nativeGetBandEnergies() (low/mid/high).
 *  - Amounts: nativeGetEffectAmounts() (orbe, holos, phi, volterra 1/0).
 *  - Φ / OmniField / NEU·Ω: nativeHyperStatus() (amount, mode, preset,
 *    neuro, spike, frames), nativeGetOmniState() (wet, convergencia,
 *    azimut, difuso, µs/bloque), nativeGetOmniTelemetry() (llamadas,
 *    bloques, frames, diseños, fault) y nativeIsNeuEnabled(). Línea
 *    propia `IVANNA.HyperOmni` (formatHyperOmniLine), no en la principal.
 *  - ASRC/Ψ: RouteAAsrc.isAvailable / clockCorrectionPpm (ppm APLICADO
 *    por el nativo) / psiEta / psiCoherence / psiActive.
 *  - NPE: IvannaNpeEngine.isReady + getDetectedGenre().
 *
 * Seguridad:
 *  - El poller corre en Dispatchers.Default (hilo de fondo), NUNCA en el
 *    hilo de audio RT. Solo mientras Ruta A reproduce: arranca con el
 *    decode loop de IvannaBridgePlayer (se cancela en su `finally`) y con
 *    la captura de PlaybackCaptureService (se cancela en stopEngineLocked).
 *    Desde la v2.5.412 la captura SÍ tiene su propio RouteAAsrc (y Ψ):
 *    el poller recibe el ASRC de la sesión y asrc/psi son vivos en ambas
 *    rutas; el shed sí se lee en ambas rutas.
 *  - Todos los reads con runCatching y fail-closed: si algo falla, la
 *    línea se salta; el audio NUNCA se rompe por la telemetría.
 *  - Locale.US en el formateo: en un teléfono con locale es-ES, "%.2f"
 *    daría "0,73" con coma y rompería el parseo. Con US siempre es punto.
 *  - formatTelemetryLine es top-level y pura (sin Android en su camino):
 *    testeable en host (IvannaTelemetryTest). NO va dentro de un
 *    companion object: los símbolos ahí no resuelven desde los tests
 *    (lección dura del repo).
 */

/** Tag logcat para la lectura en vivo: `logcat -s IVANNA.Telemetry`. */
const val TELEMETRY_TAG = "IVANNA.Telemetry"

/** Tag logcat para Φ / OmniField / NEU·Ω: `logcat -s IVANNA.HyperOmni`.
 *  Línea propia (mismo patrón que StageTimings) para no romper el parseo
 *  de la línea principal que Esse calibra en Termux. */
const val HYPER_OMNI_TAG = "IVANNA.HyperOmni"

/** Tag logcat para la ganancia apilada por banda: `logcat -s IVANNA.EqStack`. */
const val EQ_STACK_TAG = "IVANNA.EqStack"

/** Tag logcat para los tiempos por etapa: `logcat -s IVANNA.StageTimings`. */
const val STAGE_TIMINGS_TAG = "IVANNA.StageTimings"

/** Un snapshot de todos los efectos en un instante. Valores por defecto = silencio/apagado. */
data class TelemetrySnapshot(
    val route: String = "A",
    val asrcAvailable: Boolean = false,
    // Puerta Hi-Fi USB (0030, v2.5.459): 1 = la ruta es direct_pcm +
    // PCM_24_BIT_PACKED (sin mezclador); 0 = ruta float/mezclador actual.
    val hiresDirect: Boolean = false,
    val ppm: Double = 0.0,
    val psiEta: Double = 0.5,
    val psiCoherence: Double = 0.5,
    val psiActive: Boolean = false,
    val orbe: Float = 0f,
    val holos: Float = 0f,
    val phi: Float = 0f,
    val volterra: Float = 0f, // 1.0/0.0 (on/off, no tiene amount)
    // Ω — Operador Universal (v2.5.444, omega_report): activo 0/1, nivel de
    // conocimiento N (0/1/2), E_sig dB RMS (cuánto le falta a la firma) y
    // ε_ILD dB (cuánto ILD le falta). 0 cuando Ω está off.
    val omegaActive: Boolean = false,
    val omegaLevel: Int = 0,
    val omegaESigDb: Float = 0f,
    val omegaEIldDb: Float = 0f,
    // Φ — Hiper-Operador de Estado (nativeHyperStatus, v2.5.445): amount,
    // mode (0 off / 1 aprox / 2 medido), preset, neuroActive, spikeRate,
    // frames recibidos y meanCoherence (v2.5.450, [6]). La coherencia es la
    // evidencia de que Φ corrige de verdad: Γ=0 si es 0 aunque el modo
    // diga MEDIDO (bug Φ, referencia sin alimentar, v2.5.450).
    val hyperAmount: Float = 0f,
    val hyperMode: Int = 0,
    val hyperPreset: Int = 0,
    val hyperNeuroActive: Boolean = false,
    val hyperSpikeRate: Float = 0f,
    val hyperFramesReceived: Float = 0f,
    val hyperMeanCoherence: Float = 0f,
    // Latencia de seguimiento F0 (v2.5.471): end-to-end sensor→bloque
    // renderizado (ms), sensor→JNI (ms) y yaw aplicado (deg). -1 = sin
    // medición todavía. Objetivo ≤80 ms giro-a-sonido.
    val trackingLatencyMs: Float = -1f,
    val trackingJniMs: Float = -1f,
    val trackingYawDeg: Float = 0f,
    // IVANNA / OmniField (nativeGetOmniState + nativeGetOmniTelemetry,
    // v2.5.445): wet efectivo, convergencia, azimut de escena, difuso,
    // media µs/bloque y contadores (llamadas, bloques, frames, diseños,
    // fault). El fault (0/1) es el que delata si OmniField se cayó.
    val omniWet: Float = 0f,
    val omniConvergenceR: Float = 0f,
    val omniAzimuthDeg: Float = 0f,
    val omniDiffuseness: Float = 0f,
    val omniUsPerBlock: Float = 0f,
    val omniProcessCalls: Long = 0L,
    val omniBlocksRendered: Long = 0L,
    val omniFramesRendered: Long = 0L,
    val omniDesignCount: Long = 0L,
    val omniFault: Boolean = false,
    // NEU·Ω (nativeIsNeuEnabled): bandera del compilador espectral.
    // OFF por defecto (fail-closed); 1 solo si alguien lo encendió.
    val neuEnabled: Boolean = false,
    val shedFrac: Float = 0f, // fracción de bloques en passthrough por shed (0..1)
    val shedEmaUs: Float = 0f, // coste medio DSP por bloque procesado (µs)
    val shedPeakUs: Float = 0f, // peor bloque procesado en la ventana de 1 s (µs; v2.5.415, antes pico de sesión)
    val shedPeakSessionUs: Float = 0f, // peor bloque procesado en la sesión (µs; v2.5.415)
    val rms: Float = 0f,
    val peak: Float = 0f,
    val gainReductionDb: Float = 0f,
    val targetGain: Float = 0f,
    val compAmount: Float = 0f,
    val excReduction: Float = 0f,
    val spatialWidth: Float = 0f,
    val safetyMargin: Float = 0f,
    val voiceProtect: Float = 0f,
    val applied: Float = 0f,
    val bandLo: Float = 0f,
    val bandMid: Float = 0f,
    val bandHi: Float = 0f,
    val npeGenre: String = "none",
    // Mezcla de captura aplicada (v2.5.412): ganancia del procesado
    // reinyectado (rampa). NaN = no aplica (BridgePlayer no reinyecta).
    val mix: Float = Float.NaN,
    // Tiempos por etapa (v2.5.411, tarea 4): del getter nativeGetDspShed
    // (extendido a 25 floats). blockBudgetNs = periodo del último bloque
    // (ns); stageStats = 21 floats [mean_ns, p99_ns, max_ns] por etapa en
    // orden hoa, orbe, holos, phi, volterra, npe, asrc. Todo cero = la
    // etapa no corrió en la ventana (bypass por amount=0 o sin bloques).
    // FloatArray (no 21 campos): el snapshot es un valor desechable por
    // segundo; copy() funciona y nadie compara snapshots por igualdad.
    val blockBudgetNs: Float = 0f,
    val stageStats: FloatArray = floatArrayOf(),
    // Internos del servo PLL (v2.5.415, bandera 1 de la 414): la deriva
    // medida, la ocupación real, el error de fase, la demanda sin clampar
    // y si está en el riel. Sin esto el ppm=-500 en el riel se descubrió
    // por accidente; ahora la línea lo dice explícito.
    val pllDriftPpm: Double = 0.0,
    val pllOccupancyFrames: Long = 0L,
    val pllTargetOccupancyFrames: Long = 0L,
    // v2.5.423: capacidad del track (frames). Sin ella la línea no
    // permitía saber si pll_occ era mucho o poco: se adivinaba.
    val pllCapacityFrames: Long = 0L,
    val pllPhaseError: Double = 0.0,
    val pllDemandedPpm: Double = 0.0,
    val pllAtRail: Boolean = false,
    // v2.5.420: cuántas veces el servo congeló su salida por inanición
    // (hold). Si sube, el servo está aguantando sin medida fiable, no
    // corrigiendo un reloj.
    val pllStarvationHolds: Long = 0L,
    // v2.5.455: snap de nivel pendiente (frames; + = soltar, − = duplicar).
    // Dice si el servo está corrigiendo el NIVEL en discreto (el ppm solo
    // corrige la deriva): un snap distinto de 0 con ppm en el riel es la
    // evidencia de que el nivel lo determina el priming, no el servo.
    val pllSnapFrames: Long = 0L,
    // Estado de las puertas de ORBE/HOLOS (v2.5.415, bandera 5 de la 414):
    // tier térmico, coherencia media del estimador, cantidad aplicada y
    // shed térmico. Dice POR QUÉ una etapa está en bypass (~1 µs) en vez
    // de dejarlo a la adivinanza.
    val gateHolosTier: Int = 0,
    val gateHolosCoherence: Float = 1f,
    val gateHolosWet: Float = 0f,
    val gateOrbeCoherence: Float = 1f,
    val gateOrbeShed: Boolean = false,
    val gateOrbeWet: Float = 0f
)

/**
 * Formatea un snapshot como una línea `clave=valor` parseable.
 * Pura y top-level: sin lecturas nativas, sin Android, testeable en host.
 * ppm con signo y 1 decimal; el resto de floats con 2 decimales.
 */
fun formatTelemetryLine(s: TelemetrySnapshot): String {
    val genre = s.npeGenre.ifBlank { "none" }.replace(Regex("[\\s=]+"), "_")
    // mix=-- cuando no aplica (BridgePlayer); mix=0.60 en captura.
    val mixStr = if (s.mix.isFinite()) String.format(Locale.US, "%.2f", s.mix) else "--"
    return String.format(
        Locale.US,
        "route=%s asrc=%d hires_direct=%d ppm=%+.1f psi_eta=%.2f psi_coh=%.2f psi_active=%d " +
            "pll_drift=%+.1f pll_occ=%d pll_tgt=%d pll_cap=%d pll_ph=%+.2f pll_dem=%+.1f pll_rail=%d pll_hold=%d pll_snap=%d " +
            "orbe=%.2f holos=%.2f phi=%.2f volterra=%d omega=%d omega_n=%d omega_esig=%.2f omega_eild=%.2f mix=%s " +
            "gate_holos_tier=%d gate_holos_coh=%.2f gate_holos_wet=%.2f " +
            "gate_orbe_coh=%.2f gate_orbe_shed=%d gate_orbe_wet=%.2f " +
            "shed=%.3f dsp_us=%.0f dsp_pk=%.0f dsp_pk_ses=%.0f " +
            "rms=%.2f peak=%.2f gr_db=%.2f gain_db=%.2f comp=%.2f exc=%.2f " +
            "width=%.2f safety=%.2f voice=%.2f applied=%d " +
            "band_lo=%.2f band_mid=%.2f band_hi=%.2f npe_genre=%s " +
            "trk_ms=%.1f trk_jni=%.1f trk_yaw=%.1f",
        s.route,
        if (s.asrcAvailable) 1 else 0,
        if (s.hiresDirect) 1 else 0,
        s.ppm,
        s.psiEta,
        s.psiCoherence,
        if (s.psiActive) 1 else 0,
        s.pllDriftPpm,
        s.pllOccupancyFrames,
        s.pllTargetOccupancyFrames,
        s.pllCapacityFrames,
        s.pllPhaseError,
        s.pllDemandedPpm,
        if (s.pllAtRail) 1 else 0,
        s.pllStarvationHolds,
        s.pllSnapFrames,
        s.orbe,
        s.holos,
        s.phi,
        if (s.volterra > 0.5f) 1 else 0,
        if (s.omegaActive) 1 else 0,
        s.omegaLevel,
        s.omegaESigDb,
        s.omegaEIldDb,
        mixStr,
        s.gateHolosTier,
        s.gateHolosCoherence,
        s.gateHolosWet,
        s.gateOrbeCoherence,
        if (s.gateOrbeShed) 1 else 0,
        s.gateOrbeWet,
        s.shedFrac,
        s.shedEmaUs,
        s.shedPeakUs,
        s.shedPeakSessionUs,
        s.rms,
        s.peak,
        s.gainReductionDb,
        s.targetGain,
        s.compAmount,
        s.excReduction,
        s.spatialWidth,
        s.safetyMargin,
        s.voiceProtect,
        s.applied.toInt(),
        s.bandLo,
        s.bandMid,
        s.bandHi,
        genre,
        s.trackingLatencyMs,
        s.trackingJniMs,
        s.trackingYawDeg
    )
}

/**
 * Formatea los campos Φ / OmniField / NEU·Ω del snapshot como una línea
 * `clave=valor` parseable (tag IVANNA.HyperOmni). Pura y top-level, como
 * formatTelemetryLine: sin lecturas nativas, sin Android, testeable en host.
 * Línea propia para no romper el contrato exacto de la línea principal.
 */
fun formatHyperOmniLine(s: TelemetrySnapshot): String {
    return String.format(
        Locale.US,
        "hyper=%.2f hyper_mode=%d hyper_preset=%d hyper_neuro=%d hyper_spike=%.2f hyper_frames=%.0f hyper_coh=%.2f " +
            "omni_wet=%.2f omni_conv=%.2f omni_az=%.1f omni_diff=%.2f omni_us=%.1f " +
            "omni_calls=%d omni_blocks=%d omni_frames=%d omni_designs=%d omni_fault=%d neu=%d",
        s.hyperAmount,
        s.hyperMode,
        s.hyperPreset,
        if (s.hyperNeuroActive) 1 else 0,
        s.hyperSpikeRate,
        s.hyperFramesReceived,
        s.hyperMeanCoherence,
        s.omniWet,
        s.omniConvergenceR,
        s.omniAzimuthDeg,
        s.omniDiffuseness,
        s.omniUsPerBlock,
        s.omniProcessCalls,
        s.omniBlocksRendered,
        s.omniFramesRendered,
        s.omniDesignCount,
        if (s.omniFault) 1 else 0,
        if (s.neuEnabled) 1 else 0
    )
}

/**
 * Lee un snapshot de todas las fuentes vivas. null si la librería nativa
 * no está cargada (el llamador salta la línea). Cada read va con
 * runCatching: un getter que falle deja su valor en 0, nunca rompe nada.
 *
 * routeAAsrc es NULLABLE (v2.5.410): la ruta de captura en vivo
 * (PlaybackCaptureService, Tidal por MediaProjection) recibe su propio
 * RouteAAsrc desde la v2.5.412; antes era null (asrc=0, psi en defaults).
 * Si es null (sin ASRC en esta ruta), el poller corre igual con asrc=0 y
 * psi en sus defaults (0.5/0.5/inactivo). El shed sí se lee en ambas rutas:
 * el gobernador es estático en el JNI.
 */
internal fun collectTelemetrySnapshot(
    routeAAsrc: RouteAAsrc?,
    mixGain: Float = Float.NaN,
    pll: AudioTrackBufferPll? = null,
    // Puerta Hi-Fi USB (0030): 1 = ruta direct_pcm + PCM_24_BIT_PACKED.
    hiresDirectProvider: (() -> Boolean)? = null
): TelemetrySnapshot? {
    val lib = IvannaNativeLib
    if (!lib.isLoaded) return null
    val tele = runCatching { lib.nativeGetAdaptiveTelemetry() }.getOrNull()
    val bands = runCatching { lib.nativeGetBandEnergies() }.getOrNull()
    val amounts = runCatching { lib.nativeGetEffectAmounts() }.getOrNull()
    val shed = runCatching { lib.nativeGetDspShed() }.getOrNull()
    val gate = runCatching { lib.nativeGetSpatialGateState() }.getOrNull()
    // Ω (v2.5.444, omega_report): [active, nivel N, E_sig dB, ε_ILD dB].
    // runCatching: si falla, Ω queda en 0 (off), nunca rompe la línea.
    val omega = runCatching { lib.nativeOmegaStatus() }.getOrNull()
    // Φ + OmniField + NEU·Ω (v2.5.445): mismo patrón — un read que falle
    // deja su campo en 0, nunca rompe la línea ni el audio.
    val hyper = runCatching { lib.nativeHyperStatus() }.getOrNull()
    val omniState = runCatching { lib.nativeGetOmniState() }.getOrNull()
    val omniTele = runCatching { lib.nativeGetOmniTelemetry() }.getOrNull()
    // F0 (v2.5.471): latencia de seguimiento end-to-end.
    val trk = runCatching { lib.nativeGetTrackingLatency() }.getOrNull()
    val neuOn = runCatching { lib.nativeIsNeuEnabled() }.getOrDefault(false)
    val npeReady = IvannaNpeEngine.isReady
    val genre = if (npeReady) {
        runCatching { IvannaNpeEngine.getDetectedGenre() }.getOrNull()
    } else {
        null
    }
    return TelemetrySnapshot(
        route = "A",
        asrcAvailable = runCatching { routeAAsrc?.isAvailable ?: false }.getOrDefault(false),
        hiresDirect = runCatching { hiresDirectProvider?.invoke() ?: false }.getOrDefault(false),
        ppm = runCatching { routeAAsrc?.clockCorrectionPpm ?: 0.0 }.getOrDefault(0.0),
        psiEta = runCatching { routeAAsrc?.psiEta ?: 0.5 }.getOrDefault(0.5),
        psiCoherence = runCatching { routeAAsrc?.psiCoherence ?: 0.5 }.getOrDefault(0.5),
        psiActive = runCatching { routeAAsrc?.psiActive ?: false }.getOrDefault(false),
        orbe = amounts?.getOrNull(0) ?: 0f,
        holos = amounts?.getOrNull(1) ?: 0f,
        phi = amounts?.getOrNull(2) ?: 0f,
        volterra = amounts?.getOrNull(3) ?: 0f,
        omegaActive = (omega?.getOrNull(0) ?: 0f) > 0.5f,
        omegaLevel = omega?.getOrNull(1)?.toInt() ?: 0,
        omegaESigDb = omega?.getOrNull(2) ?: 0f,
        omegaEIldDb = omega?.getOrNull(3) ?: 0f,
        hyperAmount = hyper?.getOrNull(0) ?: 0f,
        hyperMode = hyper?.getOrNull(1)?.toInt() ?: 0,
        hyperPreset = hyper?.getOrNull(2)?.toInt() ?: 0,
        hyperNeuroActive = (hyper?.getOrNull(3) ?: 0f) > 0.5f,
        hyperSpikeRate = hyper?.getOrNull(4) ?: 0f,
        hyperFramesReceived = hyper?.getOrNull(5) ?: 0f,
        hyperMeanCoherence = hyper?.getOrNull(6) ?: 0f,
        trackingLatencyMs = trk?.getOrNull(0) ?: -1f,
        trackingJniMs = trk?.getOrNull(1) ?: -1f,
        trackingYawDeg = trk?.getOrNull(2) ?: 0f,
        omniWet = omniState?.getOrNull(0) ?: 0f,
        omniConvergenceR = omniState?.getOrNull(1) ?: 0f,
        omniAzimuthDeg = omniState?.getOrNull(2) ?: 0f,
        omniDiffuseness = omniState?.getOrNull(3) ?: 0f,
        omniUsPerBlock = omniState?.getOrNull(4) ?: 0f,
        omniProcessCalls = omniTele?.getOrNull(0) ?: 0L,
        omniBlocksRendered = omniTele?.getOrNull(1) ?: 0L,
        omniFramesRendered = omniTele?.getOrNull(2) ?: 0L,
        omniDesignCount = omniTele?.getOrNull(3) ?: 0L,
        omniFault = (omniTele?.getOrNull(4) ?: 0L) != 0L,
        neuEnabled = neuOn,
        shedFrac = shed?.getOrNull(0) ?: 0f,
        shedEmaUs = shed?.getOrNull(1) ?: 0f,
        shedPeakUs = shed?.getOrNull(2) ?: 0f,
        shedPeakSessionUs = shed?.getOrNull(25) ?: 0f,
        pllDriftPpm = runCatching { pll?.measuredDriftPpm ?: 0.0 }.getOrDefault(0.0),
        pllOccupancyFrames = runCatching { pll?.occupancyFrames ?: 0L }.getOrDefault(0L),
        pllTargetOccupancyFrames = runCatching { pll?.targetOccupancyFrames ?: 0L }.getOrDefault(0L),
        pllCapacityFrames = runCatching { pll?.lastBufferCapacityFrames ?: 0L }.getOrDefault(0L),
        pllPhaseError = runCatching { pll?.phaseErrorFraction ?: 0.0 }.getOrDefault(0.0),
        pllDemandedPpm = runCatching { pll?.demandedPpm ?: 0.0 }.getOrDefault(0.0),
        pllAtRail = runCatching { pll?.isAtRail ?: false }.getOrDefault(false),
        pllStarvationHolds = runCatching { pll?.starvationHolds ?: 0L }.getOrDefault(0L),
        pllSnapFrames = runCatching { pll?.levelSnapFrames ?: 0L }.getOrDefault(0L),
        gateHolosTier = gate?.getOrNull(0)?.toInt() ?: 0,
        gateHolosCoherence = gate?.getOrNull(1) ?: 1f,
        gateHolosWet = gate?.getOrNull(2) ?: 0f,
        gateOrbeCoherence = gate?.getOrNull(3) ?: 1f,
        gateOrbeShed = (gate?.getOrNull(4) ?: 0f) > 0.5f,
        gateOrbeWet = gate?.getOrNull(5) ?: 0f,
        rms = tele?.getOrNull(0) ?: 0f,
        peak = tele?.getOrNull(1) ?: 0f,
        gainReductionDb = tele?.getOrNull(2) ?: 0f,
        targetGain = tele?.getOrNull(3) ?: 0f,
        compAmount = tele?.getOrNull(4) ?: 0f,
        excReduction = tele?.getOrNull(5) ?: 0f,
        spatialWidth = tele?.getOrNull(6) ?: 0f,
        safetyMargin = tele?.getOrNull(7) ?: 0f,
        voiceProtect = tele?.getOrNull(8) ?: 0f,
        applied = tele?.getOrNull(9) ?: 0f,
        bandLo = bands?.getOrNull(0) ?: 0f,
        bandMid = bands?.getOrNull(1) ?: 0f,
        bandHi = bands?.getOrNull(2) ?: 0f,
        npeGenre = genre?.takeIf { it.isNotBlank() } ?: "none",
        mix = mixGain,
        // El array shed viene extendido (25 floats, v2.5.411): [3] es el
        // presupuesto del bloque y [4..24] las 21 estadísticas de etapa.
        // Se lee UNA vez por segundo aquí: el getter hace snapshot-and-
        // reset, así que una segunda lectura en el mismo segundo daría
        // ceros. Por eso el poller formatea la línea StageTimings desde
        // este snapshot y NO vuelve a llamar al getter.
        blockBudgetNs = shed?.getOrNull(3) ?: 0f,
        stageStats = if (shed != null && shed.size >= 25) shed.copyOfRange(4, 25) else floatArrayOf()
    )
}

/**
 * Arranca el volcado 1 Hz en un hilo de fondo (Dispatchers.Default).
 * El llamador guarda el Job y lo cancela cuando la reproducción termina
 * (IvannaBridgePlayer lo cancela en el `finally` del decode loop;
 * PlaybackCaptureService en stopEngineLocked). Desde la v2.5.412 la
 * captura pasa su propio RouteAAsrc (asrc/psi vivos) y un proveedor de
 * la ganancia de mezcla aplicada (`mix=` en la línea). El Job es hijo
 * del scope que lo lanza: si el scope se cancela, el poller muere con
 * él. La cancelación durante delay() es normal.
 */
internal fun CoroutineScope.startTelemetryPoller(
    routeAAsrc: RouteAAsrc?,
    intervalMs: Long = 1000L,
    appContext: Context? = null,
    mixGainProvider: (() -> Float)? = null,
    pllProvider: (() -> AudioTrackBufferPll?)? = null,
    // Puerta Hi-Fi USB (0030): 1 = ruta direct_pcm + PCM_24_BIT_PACKED.
    hiresDirectProvider: (() -> Boolean)? = null
): Job = launch(Dispatchers.Default) {
    // Diagnóstico de arranque (v2.5.411, tarea 4 — por qué el tag salía
    // vacío): si IVANNA.Telemetry está vacío Y no aparece esta línea, el
    // poller nunca arrancó (build sin telemetría — la 406 no la tenía — o
    // el decode loop / startEngine no llegó a arrancar el poller). Si
    // aparece esta línea pero no las de 1 Hz, el snapshot es null (ver el
    // aviso throttled de abajo: librería nativa no cargada). La causa más
    // probable en el teléfono de Esse es la versión: instaló la 406
    // (2026-11-11 22:43 UTC, ANTES de publicarse la 407 con telemetría);
    // el código del poller en 407/410 es correcto por inspección.
    Log.i(TELEMETRY_TAG, "poller_started route=A interval_ms=$intervalMs")
    var lastNullWarnMs = 0L
    var lastXruns = -1
    while (isActive) {
        runCatching {
            val mix = runCatching { mixGainProvider?.invoke() ?: Float.NaN }.getOrDefault(Float.NaN)
            val pll = runCatching { pllProvider?.invoke() }.getOrNull()
            val snapshot = collectTelemetrySnapshot(
                routeAAsrc, mixGain = mix, pll = pll,
                hiresDirectProvider = hiresDirectProvider
            )
            if (snapshot != null) {
                Log.i(TELEMETRY_TAG, formatTelemetryLine(snapshot))
                Log.i(HYPER_OMNI_TAG, formatHyperOmniLine(snapshot))
                // Tiempos por etapa + xruns (tarea 4), tag propio para no
                // romper el parseo de la línea principal. xruns =
                // AudioTrack.getUnderrunCount() acumulado de la sesión
                // (publicado en OmegaMetrics por la captura y por el
                // BridgePlayer); xruns_1s = delta del último segundo
                // (clamp a 0: al recrearse el track el total vuelve a 0).
                // NO hay AAudio en la app: el I/O es AudioTrack/AudioRecord
                // (verificado por grep); este es el contador equivalente.
                val xruns = OmegaMetrics.shared.value.underrunCount
                val xruns1s = if (lastXruns < 0) 0 else maxOf(0, xruns - lastXruns)
                lastXruns = xruns
                val st = snapshot.stageStats
                if (st.size >= 21) {
                    Log.i(
                        STAGE_TIMINGS_TAG,
                        String.format(
                            Locale.US,
                            "budget_ns=%.0f xruns=%d xruns_1s=%d " +
                                "hoa_mean=%.0f hoa_p99=%.0f hoa_max=%.0f " +
                                "orbe_mean=%.0f orbe_p99=%.0f orbe_max=%.0f " +
                                "holos_mean=%.0f holos_p99=%.0f holos_max=%.0f " +
                                "phi_mean=%.0f phi_p99=%.0f phi_max=%.0f " +
                                "volt_mean=%.0f volt_p99=%.0f volt_max=%.0f " +
                                "npe_mean=%.0f npe_p99=%.0f npe_max=%.0f " +
                                "asrc_mean=%.0f asrc_p99=%.0f asrc_max=%.0f",
                            snapshot.blockBudgetNs, xruns, xruns1s,
                            st[0], st[1], st[2], st[3], st[4], st[5],
                            st[6], st[7], st[8], st[9], st[10], st[11],
                            st[12], st[13], st[14], st[15], st[16], st[17],
                            st[18], st[19], st[20]
                        )
                    )
                }
            } else {
                // Antes el snapshot null se tragaba en silencio y el tag
                // quedaba vacío sin ningún diagnóstico. Ahora avisa (1/30 s)
                // con la causa probable: librería nativa no cargada.
                val now = System.currentTimeMillis()
                if (now - lastNullWarnMs >= 30_000L) {
                    lastNullWarnMs = now
                    Log.w(TELEMETRY_TAG, "snapshot_null: lib nativa no cargada (IvannaNativeLib.isLoaded=false); sin telemetría hasta que cargue")
                }
            }
        }
        // Diagnóstico de ganancia apilada por banda (v2.5.411, tarea 3c).
        // Cuánto EQ se apila en cada banda, en dB, desde sus fuentes de
        // verdad en Kotlin (valores COMANDADOS = prefs; la UI actualiza
        // prefs y nativo a la vez, así que coinciden salvo en el instante
        // del cambio — no es readback nativo). Las 4 capas:
        //   holos: EQ estático Γ de HOLOS (ParameterStore; Esse:
        //     +7.8/−8.1/−8.6, tilt +5.2). presence se suma a la banda alta.
        //   user: EQ de usuario (AdaptiveControlsPrefs eqBass/eqMid/eqTreble;
        //     defaults 2.2/0.8/1.8), aplicado por nativeSetEQParams a g_eq.
        //   trim: hyper_bass_trim de Φ (ParameterStore; ±3 dB, solo graves).
        //   legacy: ganancia master del GainStage (g_params.master, vía
        //     nativeSetEQParams; masterGain lineal → dB), plana en las 3
        //     bandas. tilt es pendiente, no ganancia de banda: se reporta
        //     aparte. Con los valores de Esse: lo ≈ +10.7 dB, mid ≈ −6.6 dB,
        //     hi ≈ −6.1 dB (spread ~17 dB): el stack existe y se ve aquí.
        // El comentario en el JNI (línea ~1168) ya advertía que con boosts
        // grandes el EQ apila >6 dB y el SafetyLimiter arranca en modo pared.
        if (appContext != null) {
            runCatching {
                val ps = ParameterStore(appContext)
                val ac = AdaptiveControlsPrefs.load(appContext)
                val legacyDb =
                    (20.0 * kotlin.math.log10(ac.masterGain.coerceIn(0.1f, 2.0f).toDouble())).toFloat()
                val stackLo = ps.getHolosEqLow() + ac.eqBass + ps.getHyperBassTrimDb() + legacyDb
                val stackMid = ps.getHolosEqMid() + ac.eqMid + legacyDb
                val stackHi = ps.getHolosEqHigh() + ps.getHolosEqPresence() + ac.eqTreble + legacyDb
                Log.i(
                    EQ_STACK_TAG,
                    String.format(
                        Locale.US,
                        "stack_lo=%+.2f stack_mid=%+.2f stack_hi=%+.2f tilt=%+.2f " +
                            "holos_lo=%+.2f holos_mid=%+.2f holos_hi=%+.2f holos_pres=%+.2f " +
                            "user_lo=%+.2f user_mid=%+.2f user_hi=%+.2f trim=%+.2f legacy=%+.2f",
                        stackLo, stackMid, stackHi, ps.getHolosEqTiltDb(),
                        ps.getHolosEqLow(), ps.getHolosEqMid(), ps.getHolosEqHigh(),
                        ps.getHolosEqPresence(),
                        ac.eqBass, ac.eqMid, ac.eqTreble,
                        ps.getHyperBassTrimDb(), legacyDb
                    )
                )
            }
        }
        delay(intervalMs)
    }
}
