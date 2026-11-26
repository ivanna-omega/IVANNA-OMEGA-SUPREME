package com.ivanna.omega.core

import android.content.Context
import android.media.AudioManager
import android.util.Log
import com.ivanna.omega.audio.EffectsOffTransition
import com.ivanna.omega.audio.FusedControlsBridge
import com.ivanna.omega.audio.RouteBDeviceGate
import com.ivanna.omega.audio.RouteBGeneralAmounts
import com.ivanna.omega.audio.RouteBOutput
import com.ivanna.omega.audio.TransducerMode
import com.ivanna.omega.audio.TransducerProfile
import com.ivanna.omega.audio.TransducerProfileFactory
import com.ivanna.omega.audio.UsbDacTarget
import com.ivanna.omega.magisk.OmegaEngineBridge
import com.ivanna.omega.spatial.IvannaSpatialEngine
import com.ivanna.omega.spatial.IvannaSpatialManager
import com.ivanna.omega.ui.AdaptiveControlsPrefs
import com.ivanna.omega.ui.SpatialAudioPrefs
import org.json.JSONObject

object PersistedStateRestorer {
    private const val TAG = "PersistedStateRestorer"

    /**
     * Resolución única del transductor (FASE 2, tercer modo v2.5.483):
     * selector de DAC USB + tipos de dispositivo → (salida, modo).
     * Pura salvo la lectura de prefs/AudioManager; fail-closed a
     * (SPEAKERS, SPEAKERS_GENERIC). GENEZI = DAC USB con selector
     * SPEAKERS (la cadena JieLi → Genezi): binaural del usuario,
     * general neutro.
     */
    private fun resolveRouteBMode(context: Context): Pair<RouteBOutput, TransducerMode> {
        val ctx = context.applicationContext
        val usbDacTarget = runCatching { SpatialAudioPrefs.load(ctx).usbDacTarget }
            .getOrDefault(UsbDacTarget.SPEAKERS)
        val types = runCatching {
            val am = ctx.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            am?.getDevices(AudioManager.GET_DEVICES_OUTPUTS)?.map { it.type }?.toIntArray()
        }.getOrNull() ?: intArrayOf()
        return RouteBDeviceGate.routeBOutput(types, usbDacTarget) to
            TransducerProfileFactory.resolveMode(types, usbDacTarget)
    }

    /** Perfil efectivo para un modo, desde los montos del usuario. */
    private fun profileForMode(mode: TransducerMode, store: ParameterStore): TransducerProfile =
        TransducerProfileFactory.profileFor(
            mode,
            store.getOrbeAmount(),
            store.getHolosAmount(),
            store.getHyperAmount(),
            store.getHyperPreset(),
            store.isVolterraEnabled(),
            store.getAuxXoverHz(),
            store.getAuxCoherence()
        )

    /**
     * Kernel Volterra del modo (FASE 2): Genezi → genezy_mhc_pz1d.ivk1,
     * audífonos → moto_g85_pz1d.ivk1. Los cargadores son no-op si el
     * kernel ya es el activo; en bocina genérica no se toca el kernel.
     * Hilo de CONTROL, nunca el de audio.
     */
    private fun applyTransducerVolterraKernel(context: Context, mode: TransducerMode) {
        if (!IvannaNativeLib.isLoaded) return
        runCatching {
            when (mode) {
                TransducerMode.SPEAKERS_GENEZI ->
                    IvannaNativeLib.loadGeneziVolterraKernel(context)
                TransducerMode.HEADPHONES ->
                    IvannaNativeLib.loadMotoG85Pz1dVolterraKernel(context)
                TransducerMode.SPEAKERS_GENERIC -> Unit
            }
        }
    }

    suspend fun restore(context: Context) {
        runCatching { com.ivanna.omega.audio.HiResAudioManager.restore(context) }
        val ctx = context.applicationContext
        val saPrefs = SpatialAudioPrefs.load(ctx)
        val route = com.ivanna.omega.audio.AudioRouteManager.detectOutputRoute()
        val externalStereo = com.ivanna.omega.audio.ExternalStereoPolicy.usesExternalSpeakers(
            route, saPrefs.externalStereoLineOut)
        val hrtfWet = com.ivanna.omega.audio.ExternalStereoPolicy.hrtfWetForRoute(
            route, saPrefs.externalStereoLineOut, saPrefs.hrtfEnabled)
        val hrtfAllowed = hrtfWet > 0f
        val safAllowed = saPrefs.safEnabled &&
            com.ivanna.omega.audio.ExternalStereoPolicy.allowHeadphoneHrtf(
                route, saPrefs.externalStereoLineOut, saPrefs.hrtfEnabled)

        // 0032 — Puerta de Ruta B por dispositivo (ver RouteBDeviceGate):
        // audífonos → los montos binaurales Y generales del usuario llegan
        // al daemon; GENEZI (DAC USB del Genezi) → binaural del usuario,
        // general neutro (v2.5.483); bocinas planas → neutro total. Se
        // resuelve UNA VEZ aquí y se reutiliza en los comandos de abajo.
        // Fail-closed: sin AudioManager o sin dispositivo determinable →
        // neutro (SPEAKERS).
        // FASE 2: el selector usbDacTarget decide cuando hay DAC USB
        // (sin él, un audífono en el adaptador JieLi seguiría en neutro).
        // routeBMode es la fuente del perfil (TransducerProfile).
        val (routeBOutput, routeBMode) = resolveRouteBMode(ctx)

        // 1. Restaurar AdaptiveControlsPrefs
        val acPrefs = AdaptiveControlsPrefs.load(ctx)
        // El motor híbrido es estado global nativo compartido por varias pantallas.
        // Restaurarlo aquí evita que dependa de abrir ninguna pantalla.
        NativeBridge.safeSetHybridMagistralParams(
            enabled = acPrefs.hybridMagistralEnabled,
            binauralWet = if (hrtfAllowed) acPrefs.hybridBinauralWet * hrtfWet else 0f,
            virtualAzimuthDeg = acPrefs.binauralAzimuth.takeIf { kotlin.math.abs(it) > 1f } ?: 30f,
            virtualElevationDeg = acPrefs.binauralElevation,
            roomSize = acPrefs.hybridRoomSize,
            roomAbsorption = acPrefs.hybridRoomAbsorption,
            roomDampening = acPrefs.hybridRoomDampening,
            roomWetMix = if (externalStereo) 0f else acPrefs.hybridRoomWetMix
        )
        if (IvannaNativeLib.isLoaded) {
            runCatching { com.ivanna.omega.audio.AudioEngine.nativeSetHrtfWetDryStatic(hrtfWet) }
            runCatching { IvannaNativeLib.nativeSetEta(acPrefs.nhoEta) }
            runCatching { IvannaNativeLib.nativeSetHarmonicGain(acPrefs.nhoHarmonicGain) }
            runCatching { IvannaNativeLib.nativeSetBeta(acPrefs.nhoLateralInhib) }
            runCatching { IvannaNativeLib.nativeSetAlpha(acPrefs.nhoOhcGain) }
            
            // EQ evolutivo 512 (Ruta A): el nativo arranca en ON; se reaplica la eleccion guardada.
            runCatching { EvolutionaryEqPrefs.restore(ctx) }

            // Anti-Dolby & Spatial
            runCatching { IvannaNativeLib.nativeInitializeEvolution(acPrefs.evoPopSize, acPrefs.evoGenerations) }
            runCatching { IvannaNativeLib.nativeSetMutationRate(acPrefs.evoMutationRate) }
            runCatching { IvannaNativeLib.nativeSetPhaseParameters(acPrefs.phaseOracleIntensity, acPrefs.phaseOracleIntensity * 0.7f, acPrefs.phaseOracleIntensity * 0.5f) }
            runCatching { IvannaNativeLib.nativeSetAntiDolbyIntensity(acPrefs.antiDolbyThreshold * acPrefs.tinymlInferenceGain.coerceIn(0f, 2f)) }
            runCatching { IvannaNativeLib.nativeSetSpatialWet(acPrefs.spatialSuppression) }
            
            // NPE Controls
            // npeMasterGain is mapped to ETA (wet NHO) via PiLstmBridge in some places, but let's just make sure PiLstmBridge gets the state if it exists.
            runCatching { 
                com.ivanna.omega.neuromorphic.PiLstmBridge.setMasterGain(acPrefs.npeMasterGain)
                com.ivanna.omega.neuromorphic.PiLstmBridge.setAgc(acPrefs.agcTarget, acPrefs.agcRate)
            }
            
            // Azimuth/Elevation
            val aziRad = acPrefs.binauralAzimuth * Math.PI.toFloat() / 180f
            val eleRad = acPrefs.binauralElevation * Math.PI.toFloat() / 180f
            IvannaSpatialEngine.setAzimuth(aziRad)
            IvannaSpatialEngine.setElevation(eleRad)
            runCatching { IvannaNativeLib.nativeSetSpatialAngleRad(aziRad) }
            
            // Intelligent Upmixing (HOA)
            val paramStore = com.ivanna.omega.core.ParameterStore(ctx)
            // FASE 2: perfil de transductor (audífonos / Genezi / genérico)
            // desde el modo resuelto arriba + los montos del usuario.
            val routeBProfile = profileForMode(routeBMode, paramStore)
            runCatching { IvannaNativeLib.nativeSetIntelligentUpmixingEnabled(paramStore.isHoaUpmixingEnabled() && hrtfAllowed) }
            runCatching { IvannaNativeLib.nativeSetUpmixingImmersivity(paramStore.getHoaImmersivity()) }
            // Tronidos 493 (c): el upmix también pasa por el throttle
            // compartido (OmegaEngineBridge.sendCommand: puerta + dedup +
            // throttle), no directo por MagiskBridge.
            runCatching { OmegaEngineBridge.sendCommand(JSONObject("{\"action\":\"SET_PERCEPTUAL_STATE\",\"upmixingEnabled\":${if (paramStore.isHoaUpmixingEnabled() && hrtfAllowed) 1.0f else 0.0f},\"upmixingImmersivity\":${paramStore.getHoaImmersivity()}}")) }

            // Wave Field Synthesis (WFS) & SPSC Ring Buffer
            runCatching { IvannaNativeLib.nativeSetWfsEnabled(paramStore.isWfsEnabled() && !externalStereo) }
            runCatching { IvannaNativeLib.nativeSetWfsSpread(paramStore.getWfsSpread()) }
            // Modo AUX/altavoces: restaura estado, crossover y coherencia (ruta A + daemon/ruta B).
            runCatching { IvannaNativeLib.nativeSetAuxField(paramStore.isAuxFieldEnabled(), paramStore.getAuxXoverHz(), paramStore.getAuxCoherence()) }
            // NPE neuromórfico: estado al módulo (Ruta B). Sin esto el módulo arrancaba con su valor por defecto.
            // PRECEDENCIA (v2.5.411, tarea 3b): el motor NPE VIVO se alimenta
            // de AdaptiveControlsPrefs (ivanna_adaptive_controls), NO de
            // ParameterStore: los sliders de la UI solo escriben ahí y los
            // valores NPE de ParameterStore (ivanna_omega_params) difieren
            // de estos en el teléfono de Esse. ParameterStore NPE solo
            // alimenta el sync al cDSP (OemViewModel). No se re-apunta nada:
            // cambiar la fuente cambiaría el sonido sin evidencia ni ABX.
            runCatching {
                com.ivanna.omega.neuromorphic.PiLstmBridge.restoreModuleNpe(paramStore.isNpeBypass())
                // Los valores que la UI MUESTRA (guardados o por defecto) son los que debe usar el motor desde el arranque;
                // antes solo se aplicaban al mover un slider, y el motor corría con otros valores internos.
                val npe = com.ivanna.omega.ui.AdaptiveControlsPrefs.load(context)
                val pi = com.ivanna.omega.neuromorphic.PiLstmBridge
                pi.setHarmonicGain(npe.npeHarmonic)
                pi.setBeta(npe.npeLateralInhib)
                pi.setAlpha(npe.npeOhcCompression)
                pi.setMasterGain(npe.npeMasterGain)
                if (paramStore.isNpeBypass()) pi.setBypass(true)   // bypass guardado: aplicado desde el arranque, sin abrir la pestaña
            }
            // Corrección acústica de sala medida: si estaba activa, la restaura (ruta A + daemon/ruta B); si no, ceros.
            runCatching {
                val roomG = paramStore.getRoomCorrectionGains()
                IvannaNativeLib.nativeSetRoomCorrection(if (paramStore.isRoomCorrectionEnabled() && roomG != null) roomG else FloatArray(10))
            }
            // Impacto predictivo (PunchEngine): restaura la cantidad (ruta A + daemon/ruta B).
            runCatching { IvannaNativeLib.nativeSetPunch(paramStore.getPunchAmount()) }
            // Crossfeed de graves con ITD (Ruta A): restaura la intensidad (0 = apagado).
            runCatching { IvannaNativeLib.nativeSetCrossfeed(paramStore.getCrossfeedStrength()) }
            // ORBE — Binaural Esférico de Campo Cercano (v2.5.363): restaura los 6
            // parámetros (amount 0 = apagado por defecto) en la Ruta A por JNI y en
            // la Ruta B por SET_PERCEPTUAL_STATE al daemon (snapshot ABI v9).
            // v2.5.386 — MIGRACIÓN DEL SOBRE 380: antes de reenviar nada al
            // daemon, clampa los valores persistidos por la 384 (HOLOS 0.75 /
            // width 1.25 / diffuse 0.65, ORBE spread 1.2) al sobre 380.
            runCatching { paramStore.migrateTo386Envelope() }
            runCatching { paramStore.migrateTo437DirectiveAmounts() }
            runCatching {
                IvannaNativeLib.nativeSetOrbeParams(
                    paramStore.getOrbeAmount(), paramStore.getOrbeDistanceM(),
                    paramStore.getOrbeSpread(), paramStore.getOrbeProximityDb(),
                    paramStore.getOrbeDiffuse(), paramStore.isOrbeHeadTrackingEnabled()
                )
            }
            // Ψ (v2.5.388): radio de cabeza personalizado medido en
            // HeadMeasureScreen (KEMAR 0.0875 si nunca se midió).
            runCatching {
                IvannaNativeLib.nativeSetOrbeHeadRadiusM(paramStore.getOrbeHeadRadiusM())
            }
            // 0032/483: el amount pasa por el gate (0 en bocinas planas;
            // del usuario en audífonos y en GENEZI); el resto de
            // parámetros es inerte con amount 0. (El envío al daemon es
            // atómico y va abajo, tras hyper/Volterra/aux: ver
            // sendRouteBStateAtomic, tronidos 493.)
            // HOLOS — Operador Único de Campo Acústico (v2.5.364): restaura los 13
            // parámetros (amount 0 = apagado por defecto) en la Ruta A por JNI y en
            // la Ruta B por SET_PERCEPTUAL_STATE al daemon (snapshot ABI v10).
            // El EQ estático de Γ se restaura de ParameterStore (plano = no-op).
            runCatching {
                IvannaNativeLib.nativeSetHolosParams(
                    paramStore.getHolosAmount(), paramStore.getHolosDistanceM(),
                    paramStore.getHolosSpread(), paramStore.getHolosProximityDb(),
                    paramStore.getHolosDiffuse(), paramStore.getHolosHarmonics(),
                    paramStore.getHolosWarmth(), paramStore.getHolosDynamics(),
                    paramStore.getHolosWidth(), paramStore.getHolosGamma(),
                    paramStore.getHolosRoomRt60(), paramStore.isHolosHeadTrackingEnabled(),
                    paramStore.getHolosBandCount()
                )
                IvannaNativeLib.nativeSetHolosEq(
                    paramStore.getHolosEqLow(), paramStore.getHolosEqMid(),
                    paramStore.getHolosEqHigh(), paramStore.getHolosEqPresence(),
                    paramStore.getHolosEqTiltDb(), paramStore.getHolosEqFreq()
                )
            }
            // Ω — Operador Universal (v2.5.444): restaura enabled/equipment/
            // boostMax/ildPrecomp en la Ruta A por JNI. OFF por defecto
            // (fail-closed): con Ω off la cadena es HOLOS-Σ bit-exacta.
            // Auto se resuelve por la ruta de salida real; sin ruta, N0
            // (identidad). Sin Ruta B todavía (sin snapshot ABI).
            runCatching { OmegaEquipment.applyToNative(paramStore) }
            // 0032/483: el amount pasa por el gate (0 en bocinas planas;
            // del usuario en audífonos y en GENEZI); el resto de
            // parámetros es inerte con amount 0. (El envío al daemon es
            // atómico y va abajo, tras hyper/Volterra/aux.)
            // IVANNA (v2.5.375): restaura amount/scenePull/distanceM/
            // headTracking en la Ruta A por JNI. ACTIVO por defecto
            // (amount 1.0). Sin Ruta B todavía (sin snapshot ABI). El
            // audiograma se restaura plano (0 dB); la integración con el
            // audiograma medido queda pendiente.
            runCatching {
                IvannaNativeLib.nativeSetOmniParams(
                    paramStore.getOmniAmount(), paramStore.getOmniScenePull(),
                    paramStore.getOmniDistanceM(), paramStore.isOmniHeadTrackingEnabled()
                )
                IvannaNativeLib.nativeSetOmniAudiogram(
                    FloatArray(6), FloatArray(6)
                )
            }
            // Hiper-Operador de Estado Φ (v2.5.369): restaura amount/preset/
            // XTC y la curva objetivo élite (Harman 2018 por defecto) en la
            // Ruta A por JNI. La parte daemon (Ruta B) queda pendiente: el
            // snapshot ABI v10 no tiene campos hyper_* (mismo precedente
            // que HOLOS antes de v10); Ruta B usa los defaults del efecto
            // (off) hasta el bump de ABI.
            runCatching {
                IvannaNativeLib.nativeSetHyperAmount(
                    paramStore.getHyperAmount(), paramStore.getHyperPreset(),
                    paramStore.isHyperXtcEnabled()
                )
                IvannaNativeLib.nativeSetHyperTarget(
                    paramStore.getHyperTargetKind(), paramStore.getHyperBassTrimDb()
                )
            }
            // Volterra (v2.5.369): kernel H1/H2 ACTIVO por defecto
            // (directiva: todo lo nuevo suena desde la primera canción).
            // Primero se aplica el on/off persistido (default ON) y luego
            // se carga el kernel SELECCIONADO persistido (PZ1D refinado
            // por defecto; Genezi alternativo si el usuario lo eligió en
            // CONTROL). Fail-closed: si el asset falta o la puerta lo
            // rechaza, el motor queda en bypass bit-exacto, sin
            // silenciar. El usuario puede apagarlo en CONTROL y persiste.
            runCatching {
                IvannaNativeLib.nativeSetVolterraEnabled(paramStore.isVolterraEnabled())
                IvannaNativeLib.setVolterraKernel(ctx, paramStore.getVolterraKernelId())
            }
            // FASE 2: el perfil completo (Φ/Volterra/Aux) también al daemon
            // en el arranque, no solo en cambio de ruta (pushRouteBState):
            // sin esto, en el arranque el daemon corría con el perfil
            // viejo hasta el primer cambio de ruta. Y el kernel sigue al
            // modo (Genezi → genezy_mhc_pz1d.ivk1; audífonos → pz1d).
            // Tronidos 493 (d): orbe+holos+hyper/aux en UN solo comando
            // atómico (sendRouteBStateAtomic), no en 4 envíos separados.
            sendRouteBStateAtomic(paramStore, routeBProfile)
            applyTransducerVolterraKernel(ctx, routeBMode)
            // Sensor acústico real (v2.5.369): arranca la captura del
            // micrófono por defecto para que Φ corra en MEDIDO (el operador
            // lee el SensorFrame vivo nativamente en cada bloque). start()
            // devuelve false sin permiso RECORD_AUDIO o si el mic falla; en
            // ese caso Φ cae a APROXIMADO (fail-closed, sin crash, sin
            // silencio). La corrección de sala en vivo (RoomCorrectionEq)
            // sigue apagada por defecto y requiere acción explícita.
            runCatching { com.ivanna.omega.audio.RealAcousticSensor.start(ctx) }
            // 4D temporal: restaura estado y profundidad (ruta A + daemon/ruta B).
            runCatching { IvannaNativeLib.nativeSetTemporalField(paramStore.isTemporalFieldEnabled(), paramStore.getTemporalFieldAmount(), paramStore.isTemporalFieldAdaptive()) }
            // Háptica de graves (4D): vive en Kotlin (ruta A), no depende de la librería nativa.
            runCatching { com.ivanna.omega.audio.BassHaptics.configure(ctx, paramStore.isBassHapticEnabled(), paramStore.getBassHapticStrength(), paramStore.getBassHapticRumble(), paramStore.getBassHapticTrimMs().toInt()) }
            runCatching { IvannaNativeLib.nativeSetSpscRingFactor(acPrefs.spscRingFactor) }
            // Tronidos 493 (c): el factor SPSC pasa por la vía única de
            // envío (OmegaEngineBridge.sendCommand: chequeo de finitud +
            // coalescencia + reintento), no directo por MagiskBridge.
            runCatching { OmegaEngineBridge.sendCommand(JSONObject("{\"action\":\"SET_SPSC_RING_FACTOR\",\"factor\":${acPrefs.spscRingFactor}}")) }
        }

        // 2. Restaurar HarmonicExciterPrefs (perceptual state — Calibración Magistral activa por defecto)
        val hePrefs = ctx.getSharedPreferences("harmonic_exciter_prefs", Context.MODE_PRIVATE)
        val exciterEnabled = hePrefs.getBoolean("exciterEnabled", true)
        val harmonicGain = hePrefs.getFloat("harmonicGain", 0.78f)
        val antiDolby = hePrefs.getFloat("antiDolby", 0.85f)
        
        // 0032: spatialWidth/harmonicGain/antiDolby/compressor/
        // loudnessTarget/exciterRed pasan por el gate (neutro en bocinas
        // planas; binaural del usuario + general neutro en GENEZI).
        sendGeneralToDaemon(
            harmonicGain = if (exciterEnabled) harmonicGain else 0f,
            antiDolby = antiDolby,
            output = routeBOutput
        )

        // 3. Restaurar SpatialAudioPrefs (HRTF, RIR, SAF — Root y Non-Root)

        // Eje Supremo: Inversión Biomecánica Coclear (Cochlear-PINN)
        runCatching {
            NativeBridge.setCochlearInverseEnabled(saPrefs.cochlearInverseEnabled)
            NativeBridge.setCochlearIntensity(saPrefs.cochlearIntensity)
        }
        
        IvannaSpatialEngine.enabled = hrtfAllowed
        if (hrtfAllowed) {
            IvannaSpatialManager.setHrtfSubject(saPrefs.hrtfSubject)
        }

        // Asegurar dataset RIR extraído y registrado tanto en Non-Root como en Root
        runCatching { OmegaEngineBridge.ensureRirDataset(ctx) }

        // Restore the same validated personal snapshot used by calibration UI. No synthetic
        // room optimization, double intensity scaling or destructive reset during startup/bypass.
        val saf = com.ivanna.omega.saf.SaFEngine(ctx)
        try {
            saf.initialize().join()
        } finally {
            saf.release()
        }
        if (!safAllowed) {
            OmegaEngineBridge.pushSafLatentQ(FloatArray(7), gain = 0f)
        }

        if (saPrefs.rirEnabled && !externalStereo) {
            OmegaEngineBridge.setRoom(saPrefs.rirRt60, saPrefs.rirWet)
        } else {
            OmegaEngineBridge.disableRoom()
        }

        // 4. Restaurar los 5 Ejes de Supremacía Cuántico-Neuromórfica (C++23 Lock-Free)
        runCatching {
            val supremeState = com.ivanna.omega.ui.SupremeAxesPrefs.load(ctx)
            com.ivanna.omega.ui.SupremeAxesPrefs.applyToNative(supremeState)
        }.onFailure { Log.w(TAG, "Fallo al restaurar SupremeAxesPrefs: ${it.message}") }

        // 5. Restaurar calibración de layout de altavoces WFS (7 altavoces virtuales)
        runCatching {
            if (!externalStereo) com.ivanna.omega.spatial.WfsCalibrationManager.restore(ctx)
        }.onFailure { Log.w(TAG, "Fallo al restaurar WfsCalibrationManager: ${it.message}") }

        // 6. Cadena 4D (arco, háptica, diafonía, dosímetro). La superficie nativa retiene los valores y
        //    los reaplica cuando se crea la cadena, así que restaurar aquí es válido antes de reproducir.
        runCatching {
            if (Ivanna4DNative.isAvailable) {
                val ps = ParameterStore(ctx)
                Ivanna4DNative.nativeFourDSetLeadMs(ps.get4dLeadMs())
            runCatching { Ivanna4DNative.nativeFourDSetOutputLatencyMs(ps.get4dOutputLatencyMs()) }
                Ivanna4DNative.nativeFourDSetDepth(ps.get4dDepth())
                Ivanna4DNative.nativeFourDSetHapticScale(ps.get4dHapticScale())
                Ivanna4DNative.nativeFourDSetDosimeterMode(ps.get4dDosimeterMode())
                Ivanna4DNative.nativeFourDSetDosimeterCalibration(ps.get4dDosimeterCal())
                Ivanna4DNative.nativeFourDSetArcDepth(ps.get4dArcDepth())
                Ivanna4DNative.nativeFourDSetArc(ps.is4dArcOn())
                Ivanna4DNative.nativeFourDSetHaptics(ps.is4dHapticsOn())
                Ivanna4DNative.nativeFourDSetCrosstalk(com.ivanna.omega.audio.ExternalStereoPolicy.allowTransauralXtc(route, saPrefs.externalStereoLineOut, ps.is4dCrosstalkOn()))
                Ivanna4DNative.nativeFourDSetEnabled(ps.is4dEnabled())
            }
            FourDRouteB.push(ctx, ParameterStore(ctx), route)   // Ruta B (demonio), en hilo propio
        }.onFailure { Log.w(TAG, "Fallo al restaurar la cadena 4D: ${it.message}") }

        // P2 (2.5.461): "todo apagado" real al arrancar: si todo está
        // apagado en lo persistido, la cadena base queda transparente.
        runCatching { EffectsOffTransition.evaluate(ctx) }

        // ── 470: restaurar los controles FUSIÓN persistidos ─────────────
        // Sin esto, un reinicio revertía los sliders de FUSIÓN a los
        // legacy: el restore legacy (arriba) sobrescribe el daemon y la
        // pantalla arrancaba con FusedControls() por defecto. Se restaura
        // AL FINAL para que los valores fusionados sean los últimos
        // aplicados al daemon (merge: último en escribir gana en los
        // campos solapados) y a la Ruta A (JNI). safeSetFusedControls
        // empuja los 23 campos al daemon (v2.5.469); si el JNI no estaba
        // cargado, safeSetFusedControls retorna antes del push y se
        // empuja directo al daemon para que la Ruta B no se quede con
        // los legacy (idempotente: mismos valores).
        runCatching {
            // v2.5.473 §2.1: migración única legacy→fused ANTES de cargar:
            // un usuario que viene de la 472 trae sus valores en claves
            // orbe_*/holos_*/hyper_*; sin esto el restore cargaría defaults.
            ParameterStore(ctx).migrateLegacyToFused()

            val fused = ParameterStore(ctx).loadFusedControls()
            FusedControlsBridge.safeSetFusedControls(fused)
            if (!FusedControlsBridge.isLoaded) {
                FusedControlsBridge.pushFusedToDaemon(fused)
            }
        }.onFailure { Log.w(TAG, "Fallo al restaurar FusedControls: ${it.message}") }

        Log.i(TAG, "✅ Todos los controles descableados restaurados (Persistencia total conectada)")
    }

    // ── 0032: envío de estado binaural a Ruta B con gate por dispositivo ──
    // Cada helper manda UN comando al daemon; pushRouteBState() los manda
    // los 3 juntos (re-envío al cambiar la salida). restore() los llama en
    // sus puntos actuales para no reordenar el resto del arranque.
    // ORBE/HOLOS, spatialWidth/harmonicGain/antiDolby Y compressor/
    // loudnessTarget/exciterRed pasan por el gate (extensión 0032: la
    // caída de volumen inmediata al prender la APK era el compresor y el
    // loudness, no solo ORBE/HOLOS/widener). La Ruta A NO se toca aquí:
    // los JNI nativeSet* siguen con los montos del usuario siempre.

    // FIX 1 (bofo Genezi, v2.5.465): modo de salida para el snapshot
    // (OmegaOutputMode en omega_control_bus.h: 0 = desconocido, 1 =
    // audífonos, 2 = bocinas planas, 3 = Genezi). Lo resuelve
    // RouteBDeviceGate + usbDacTarget (resolveRouteBMode); el EFECTO lo
    // hace cumplir en omega_apply_snapshot. TERCER MODO — GENEZI
    // (evol-holos, 2026-11-26): el Genezi ya NO codifica como bocina
    // plana (2): con 3 el efecto deja pasar el holograma del usuario y
    // solo neutraliza lo general. Un daemon viejo decodifica 3 como
    // UNKNOWN (aplica el snapshot tal cual: compatible).
    private fun outputModeFor(mode: TransducerMode): Int =
        when (mode) {
            TransducerMode.HEADPHONES -> 1
            TransducerMode.SPEAKERS_GENEZI -> 3
            TransducerMode.SPEAKERS_GENERIC -> 2
        }

    // Tronidos 493 (d): ORBE y HOLOS viajan en UN SOLO comando atómico.
    // Antes eran 2 SET_PERCEPTUAL_STATE separados (cada uno con su
    // runCatching que tragaba el fallo): si el de holos fallaba o el
    // restore se interrumpía entre ambos, orbe quedaba en 1.0 y holos en
    // su valor viejo (0 envenenado por el gate de SPEAKERS, que pone
    // holosAmount a 0), y el merge pegajoso del daemon lo re-sembraba en
    // cada cambio de sesión (ivanna_daemon.cpp, copyDesiredPerceptual).
    // Ahora pushRouteBState funde los 4 grupos (orbe/holos/hyper/aux) en
    // un solo JSONObject y lo envía una vez por OmegaEngineBridge
    // .sendCommand (puerta + dedup + throttle). O el estado entero llega,
    // o no llega nada; el restore es idempotente (se re-corre al cambiar
    // de ruta), así que un fallo se autocorrige en el próximo cambio.
    private fun orbeJson(store: ParameterStore, profile: TransducerProfile): String =
        """{"action":"SET_PERCEPTUAL_STATE","outputMode":${outputModeFor(profile.mode)},"orbeAmount":${profile.orbeAmount},"orbeDistanceM":${store.getOrbeDistanceM()},"orbeSpread":${store.getOrbeSpread()},"orbeProximityDb":${store.getOrbeProximityDb()},"orbeDiffuse":${store.getOrbeDiffuse()},"orbeHeadTracking":${if (store.isOrbeHeadTrackingEnabled()) 1.0f else 0.0f}}"""

    private fun holosJson(store: ParameterStore, profile: TransducerProfile): String =
        """{"action":"SET_PERCEPTUAL_STATE","outputMode":${outputModeFor(profile.mode)},"holosAmount":${profile.holosAmount},"holosDistanceM":${store.getHolosDistanceM()},"holosSpread":${store.getHolosSpread()},"holosProximityDb":${store.getHolosProximityDb()},"holosDiffuse":${store.getHolosDiffuse()},"holosHarmonics":${store.getHolosHarmonics()},"holosWarmth":${store.getHolosWarmth()},"holosDynamics":${store.getHolosDynamics()},"holosWidth":${store.getHolosWidth()},"holosGamma":${store.getHolosGamma()},"holosRoomRt60":${store.getHolosRoomRt60()},"holosHeadTracking":${if (store.isHolosHeadTrackingEnabled()) 1.0f else 0.0f},"holosBandCount":${store.getHolosBandCount()},"holosEqLow":${store.getHolosEqLow()},"holosEqMid":${store.getHolosEqMid()},"holosEqHigh":${store.getHolosEqHigh()},"holosEqPresence":${store.getHolosEqPresence()},"holosEqTiltDb":${store.getHolosEqTiltDb()},"holosEqFreq":${store.getHolosEqFreq()}}"""

    // Funde varios JSONObject parciales en uno (las claves repetidas —
    // "action" y "outputMode" — llevan el mismo valor en todos los grupos).
    private fun mergePerceptualJson(vararg parts: JSONObject): JSONObject {
        val out = JSONObject()
        for (part in parts) {
            val keys = part.keys()
            while (keys.hasNext()) {
                val key = keys.next()
                out.put(key, part.opt(key))
            }
        }
        return out
    }

    // Tronidos 493 (c+d): UN solo SET_PERCEPTUAL_STATE atómico
    // (orbe+holos+hyper/Volterra+aux) por el throttle compartido. Lo usan
    // el restore de arranque y pushRouteBState (cambio de ruta).
    private fun sendRouteBStateAtomic(store: ParameterStore, profile: TransducerProfile) {
        runCatching {
            OmegaEngineBridge.sendCommand(
                mergePerceptualJson(
                    JSONObject(orbeJson(store, profile)),
                    JSONObject(holosJson(store, profile)),
                    JSONObject(hyperVolterraJson(profile)),
                    JSONObject(auxFieldJson(profile)),
                )
            )
        }
    }

    // Φ + Volterra (ABI v13, 0038+0040): paridad Ruta B. Sin este envío, el
    // daemon nunca recibía hyper_amount ni volterra_enabled y Ruta B no
    // inyectaba Φ ni Volterra aunque la directiva los tiene ACTIVOS.
    // FASE 2: los cuatro valores vienen del perfil de transductor (el
    // XTC es del modo, no del toggle global: en audífonos no hay
    // diafonía que cancelar; en Genezi el XTC es el del modo).
    // FASE A: el JSON vive en hyperVolterraJson() (única fuente de verdad
    // del payload) para que el envío en caliente del deslizador use
    // EXACTAMENTE el mismo comando que el restore.
    private fun hyperVolterraJson(profile: TransducerProfile): String =
        """{"action":"SET_PERCEPTUAL_STATE","hyperAmount":${profile.hyperAmount},"hyperPreset":${profile.hyperPreset},"hyperXtcEnabled":${if (profile.hyperXtcEnabled) 1.0f else 0.0f},"volterraEnabled":${if (profile.volterraEnabled) 1.0f else 0.0f}}"""

    // (sendHyperVolterraToDaemon ya no existe: el grupo hyper/Volterra
    // viaja dentro del comando atómico de pushRouteBState; el envío en
    // caliente usa pushHyperVolterraToDaemon, abajo.)


    /**
     * FASE A (2026-11-18) — comando Φ/Volterra en caliente para el modo
     * vigente. El deslizador Φ persistía (hyper_amount=0.0) pero el
     * snapshot del daemon seguía con hyper 0.94 hasta arrancar la app:
     * onHyperChange nunca mandaba al daemon. Este comando se construye
     * desde el PERFIL del modo vigente (Genezi: hyper 0 / Volterra off;
     * audífonos: valores del usuario), NO desde el valor crudo del
     * deslizador: en Genezi, mandar el crudo inyectaría Φ donde el perfil
     * lo tiene apagado. Puro salvo la lectura de prefs/AudioManager.
     */
    fun hyperVolterraCommand(context: Context): String {
        val ctx = context.applicationContext
        val store = ParameterStore(ctx)
        val (_, mode) = resolveRouteBMode(ctx)
        return hyperVolterraJson(profileForMode(mode, store))
    }

    /**
     * FASE A — re-envía Φ/Volterra al daemon EN CALIENTE (sin esperar al
     * arranque ni al cambio de salida). Lo llama el deslizador Φ
     * (ControlTabScreen, vía el canal CONFLATED perceptualCommands) y el
     * toggle VOLTERRA H2 (IvannaControlPanel, envío directo: es un toggle
     * discreto, no un arrastre). Idempotente; fail-closed si el daemon no
     * responde (el restore del arranque lo re-envía de todos modos).
     */
    fun pushHyperVolterraToDaemon(context: Context) {
        // Tronidos 493 (c): el envío en caliente de Φ/Volterra también
        // pasa por el throttle compartido (no directo por MagiskBridge).
        runCatching {
            OmegaEngineBridge.sendCommand(JSONObject(hyperVolterraCommand(context)))
        }
    }

    // Aux Field (FASE 2): el daemon YA acepta auxEnabled/auxXoverHz/
    // auxCoherence (command_server.cpp, clamp xover 60–200, coherencia
    // 0–1, encode a snapshot.flags), pero nadie se lo mandaba: solo
    // nativeSetAuxField en Ruta A. En Genezi, graves en fase coherente
    // bajo el cruce; en audífonos/bocina genérica, apagado.
    private fun auxFieldJson(profile: TransducerProfile): String =
        """{"action":"SET_PERCEPTUAL_STATE","auxEnabled":${if (profile.auxFieldEnabled) 1.0f else 0.0f},"auxXoverHz":${profile.auxXoverHz},"auxCoherence":${profile.auxCoherence}}"""

    private fun sendGeneralToDaemon(harmonicGain: Float, antiDolby: Float, output: RouteBOutput) {
        if (!OmegaEngineBridge.isConnected) return
        // 0032 extensión: compressor/loudnessTarget/exciterRed también
        // pasan por el gate (neutro en bocinas). Sin esto, el volumen
        // caía al prender la APK aunque ORBE/HOLOS/widener ya fueran
        // neutros (hallazgo Esse 2026-11-16 18:26 CST).
        // v2.5.483 tercer modo: en GENEZI, spatialWidth/harmonicGain/
        // antiDolby pasan (binaural del usuario) pero compressor/
        // loudnessTarget/exciterRed quedan en neutro (gateGeneral solo
        // pasa lo general en HEADPHONES).
        val general = RouteBDeviceGate.gateGeneral(
            RouteBGeneralAmounts(
                compressor = -5.5f,
                exciterRed = 0.15f,
                loudnessTarget = -16.0f
            ),
            output
        )
        val binaural = output != RouteBOutput.SPEAKERS
        runCatching {
            OmegaEngineBridge.sendPerceptualState(
                compressor     = general.compressor,
                exciterRed     = general.exciterRed,
                highCut        = 19500f,
                spatialWidth   = if (binaural) 1.55f else 1.0f,
                loudnessTarget = general.loudnessTarget,
                harmonicGain   = if (binaural) harmonicGain else 0f,
                antiDolby      = if (binaural) antiDolby else 0f
            )
        }.onFailure { Log.w(TAG, "Fallo al restaurar HarmonicExciterPrefs: ${it.message}") }
    }

    /**
     * Re-envía el estado binaural de Ruta B al daemon con el gate por
     * dispositivo vigente. Lo llama AudioRouteManager al cambiar la salida
     * (conectar/desconectar audífonos o DAC): si el usuario conecta
     * audífonos, ORBE/HOLOS vuelven; si cambia al DAC USB del Genezi,
     * ORBE/HOLOS vuelven con los montos del usuario y lo general queda
     * en neutro (v2.5.483); si cambia a bocinas planas, se neutraliza
     * todo sin reiniciar la app. Idempotente; fail-closed a neutro
     * si no se puede determinar el dispositivo. No toca la Ruta A.
     */
    fun pushRouteBState(context: Context) {
        val ctx = context.applicationContext
        val store = ParameterStore(ctx)
        // FASE 2: el selector usbDacTarget decide cuando hay DAC USB; el
        // perfil lleva los valores efectivos al daemon (Genezi: ORBE/HOLOS
        // del usuario + XTC + Aux + kernel genezi; audífonos: binaural;
        // genérico: neutro).
        val (output, mode) = resolveRouteBMode(ctx)
        val profile = profileForMode(mode, store)
        // Tronidos 493 (c+d): el estado de Ruta B viaja en UN solo
        // SET_PERCEPTUAL_STATE atómico (ver sendRouteBStateAtomic).
        sendRouteBStateAtomic(store, profile)
        applyTransducerVolterraKernel(ctx, mode)
        val hePrefs = ctx.getSharedPreferences("harmonic_exciter_prefs", Context.MODE_PRIVATE)
        val exciterEnabled = hePrefs.getBoolean("exciterEnabled", true)
        val harmonicGain = hePrefs.getFloat("harmonicGain", 0.78f)
        val antiDolby = hePrefs.getFloat("antiDolby", 0.85f)
        sendGeneralToDaemon(
            harmonicGain = if (exciterEnabled) harmonicGain else 0f,
            antiDolby = antiDolby,
            output = output
        )
    }
}
