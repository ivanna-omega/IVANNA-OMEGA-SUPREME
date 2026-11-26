// perceptual_state.h — Estado perceptual pendiente app -> daemon -> efecto.
//
// CAUSA RAÍZ (Ruta B neutra, 2026-11-14, auditoría estática):
// la app envía ORBE/HOLOS y el resto de ajustes SOLO por
// SET_PERCEPTUAL_STATE al socket @omega_daemon_socket
// (PersistedStateRestorer.kt: tres comandos — upmixing, ORBE, HOLOS).
// El daemon v2 (daemon_command_responder.h, desde la 440) solo servía
// HELLO/GET_STATUS y respondía {"ok":false,"error":"unsupported"} a
// cualquier SET_*. El CommandServer viejo (command_server.cpp), que sí
// aplicaba SET_PERCEPTUAL_STATE, NO se compila en el binario v2
// (daemon/CMakeLists.txt: solo ivanna_daemon.cpp + omega_control_bus.cpp).
// Y la app NUNCA abre el bus SHM como escritora (publish() exige
// m_isWriter; ningún JNI llama a openWriter), así que el bucle del
// daemon solo re-lee sus propias publicaciones: lastSnap nace de
// makeDefault() (ORBE 0 / HOLOS 0, bypass bit-exacto) y se republica
// 20 veces por segundo. El efecto SE ACOPLA, corre el DSP y el beacon
// crece — pero con amounts 0 la salida es ≈ la entrada: "acoplado pero
// no procesa (efectivamente)". El doctor decía HEALTHY_PROCESSING.
//
// ESTE ARCHIVO cierra ese canal en mínimo, sin resucitar el
// CommandServer viejo:
//   - PendingPerceptual: los campos de UN comando SET_PERCEPTUAL_STATE.
//     NaN = ausente (cada comando de la app trae solo un subconjunto:
//     upmixing, ORBE o HOLOS). El merge conserva lo anterior.
//   - applyPendingPerceptual(): aplica los campos presentes a un
//     OmegaDspSnapshot con LOS MISMOS clamps del servidor viejo
//     (command_server.cpp, SET_PERCEPTUAL_STATE). Pura, testeable.
//   - El hilo de comandos hace stash (ver daemon_command_responder.h);
//     el bucle principal hace take + apply antes de publicar.
//
// Fail-closed: sin ningún SET_PERCEPTUAL_STATE, el snapshot sigue
// siendo makeDefault() neutro (ORBE 0 / HOLOS 0). Un comando parcial
// nunca borra los campos que no trae. Un valor fuera de rango se
// clampa, nunca se rechaza el comando entero.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "include/omega_control_bus.h"
#include "include/omega_user_flags.hpp"

namespace ivanna::daemon {

// NaN = campo ausente en el comando (no tocar el valor vigente).
inline constexpr float kPerceptualAbsent =
    std::numeric_limits<float>::quiet_NaN();

struct PendingPerceptual {
    uint64_t gen{0};  // nº de SET_PERCEPTUAL_STATE recibidos (0 = ninguno)

    // Estado perceptual general (mismos nombres JSON que el servidor viejo).
    float compressor{kPerceptualAbsent};
    float exciterRed{kPerceptualAbsent};
    float highCutHz{kPerceptualAbsent};
    float spatialWidth{kPerceptualAbsent};
    float loudnessTarget{kPerceptualAbsent};
    float harmonicGain{kPerceptualAbsent};
    float antiDolby{kPerceptualAbsent};
    float upmixingEnabled{kPerceptualAbsent};
    float upmixingImmersivity{kPerceptualAbsent};

    // ORBE (ABI v9).
    float orbeAmount{kPerceptualAbsent};
    float orbeDistanceM{kPerceptualAbsent};
    float orbeSpread{kPerceptualAbsent};
    float orbeProximityDb{kPerceptualAbsent};
    float orbeDiffuse{kPerceptualAbsent};
    float orbeHeadTracking{kPerceptualAbsent};

    // HOLOS (ABI v10) + EQ estático.
    float holosAmount{kPerceptualAbsent};
    float holosDistanceM{kPerceptualAbsent};
    float holosSpread{kPerceptualAbsent};
    float holosProximityDb{kPerceptualAbsent};
    float holosDiffuse{kPerceptualAbsent};
    float holosHarmonics{kPerceptualAbsent};
    float holosWarmth{kPerceptualAbsent};
    float holosDynamics{kPerceptualAbsent};
    float holosWidth{kPerceptualAbsent};
    float holosGamma{kPerceptualAbsent};
    float holosRoomRt60{kPerceptualAbsent};
    float holosHeadTracking{kPerceptualAbsent};
    float holosBandCount{kPerceptualAbsent};
    float holosEqLow{kPerceptualAbsent};
    float holosEqMid{kPerceptualAbsent};
    float holosEqHigh{kPerceptualAbsent};
    float holosEqPresence{kPerceptualAbsent};
    float holosEqTiltDb{kPerceptualAbsent};
    float holosEqFreq{kPerceptualAbsent};

    // Φ (HyperState) + Volterra (ABI v13, 0038+0040): paridad Ruta B.
    float hyperAmount{kPerceptualAbsent};
    float hyperPreset{kPerceptualAbsent};
    float hyperXtcEnabled{kPerceptualAbsent};
    float volterraEnabled{kPerceptualAbsent};

    // Física Nueva (ABI v14, Fase 2, v2.5.472): NewPhysicsControls.
    // Mismos nombres JSON que el snapshot (np_*) en camelCase, como
    // orbeAmount/holosAmount/hyperAmount. Rangos = NewPhysicsControls::
    // clamp() (fused/new_physics_controls.hpp); el clamp se hace al
    // aplicar, no aquí. NaN = ausente (se conserva el vigente).
    float npAmount{kPerceptualAbsent};
    float npLgnMix{kPerceptualAbsent};
    float npBsDamp{kPerceptualAbsent};
    float npTorsoMix{kPerceptualAbsent};
    float npHeadRadiusM{kPerceptualAbsent};
    float npAzDeg{kPerceptualAbsent};
    float npElDeg{kPerceptualAbsent};
    float npEqClass{kPerceptualAbsent};
    float npOmegaSigMix{kPerceptualAbsent};
    float npBoostCapDb{kPerceptualAbsent};
    float npXtalkMix{kPerceptualAbsent};
    float npSigmaDistM{kPerceptualAbsent};
    float npSplDb{kPerceptualAbsent};
    float npAirMix{kPerceptualAbsent};
    float npVolterraMix{kPerceptualAbsent};
    float npFreeEnergyMix{kPerceptualAbsent};
    float npAbxMode{kPerceptualAbsent};

    // 5 Ejes Supremos (ABI v15, v2.5.481): los 12 escalares del
    // FusedChain que el responder aplica por SET_SUPREME_AXES.
    // NaN = ausente (se conserva el vigente). Rangos = los del
    // snapshot (omega_control_bus.h). Los flags de habilitado
    // (OMEGA_FLAG_SUPREME_*) NO van aquí: el drive en 0 deja el
    // eje inerte aunque el flag siga puesto.
    float supremeLatticeBlDrive{kPerceptualAbsent};
    float supremeLatticeLambda{kPerceptualAbsent};
    float supremeCvnnHarmonicGain{kPerceptualAbsent};
    float supremeCvnnImdCancel{kPerceptualAbsent};
    float supremeCvnnTapeDrive{kPerceptualAbsent};
    float supremeSnnImmersivity{kPerceptualAbsent};
    float supremeSnnSpikeThreshold{kPerceptualAbsent};
    float supremePinnaWetMix{kPerceptualAbsent};
    float supremePinnaConchaDepth{kPerceptualAbsent};
    float supremePinnaHelixCurl{kPerceptualAbsent};
    float supremePinnaHeadWidth{kPerceptualAbsent};
    float supremeMsoItdNs{kPerceptualAbsent};

    // ABI v16 (v2.5.486): los 16 comandos portados del handler muerto.
    // FotoHolo / coclear / reality / route-profile vivían solo en el
    // estado del servidor viejo; SPSC / YAMNet no tenían slot en el
    // snapshot (ahora sí: spsc_ring_factor + yamnet_*).
    float fotoholoEnabled{kPerceptualAbsent};
    float fotoholoIntensity{kPerceptualAbsent};
    float cochlearEnabled{kPerceptualAbsent};
    float cochlearIntensity{kPerceptualAbsent};
    float realityReconEnabled{kPerceptualAbsent};
    float bassBoostDb{kPerceptualAbsent};
    float dialogBoostDb{kPerceptualAbsent};
    float widenerMult{kPerceptualAbsent};
    float spscRingFactor{kPerceptualAbsent};
    float yamnetSpeech{kPerceptualAbsent};
    float yamnetMusic{kPerceptualAbsent};
    float yamnetClassId{kPerceptualAbsent};
    float yamnetConfidence{kPerceptualAbsent};

    // Conservación perceptual (ABI v17, 2026-11-21): el árbitro de campo.
    // NaN = ausente (se conserva el vigente, como el resto de campos).
    // El daemon los aplica al snapshot (consv_*) y el efecto los empuja
    // a la FusedChain de Ruta B vía IvannaFusionEngine::setConservationParams.
    float consvAmount{kPerceptualAbsent};
    float consvEnergyCap{kPerceptualAbsent};
    float consvPhaseCoherence{kPerceptualAbsent};
    float consvInertia{kPerceptualAbsent};

    // Comandos adicionales enviados por OmegaEngineBridge / PersistedStateRestorer / DSPState
    // (Fallo 1: el respondedor los rechazaba como "unsupported"; aquí viven
    // los campos que el daemon aplica al snapshot en applyPendingPerceptual).
    float intensity{kPerceptualAbsent};
    float targetGain{kPerceptualAbsent};
    float compAmount{kPerceptualAbsent};
    float excRed{kPerceptualAbsent};

    // Sala / RIR (SET_ROOM_RT60)
    float roomRt60S{kPerceptualAbsent};
    float roomWet{kPerceptualAbsent};
    float roomIdx{kPerceptualAbsent};

    // WFS (SET_WFS / SET_WFS_LAYOUT)
    float wfsEnabled{kPerceptualAbsent};
    float wfsSpread{kPerceptualAbsent};
    bool hasWfsLayout{false};
    float wfsSpeakerX[7]{};
    float wfsSpeakerY[7]{};
    float wfsSpeakerZ[7]{};

    // AUX / XTC / Temporal / NPE / Punch / 4D / AudioFeatures
    float auxEnabled{kPerceptualAbsent};
    float auxXoverHz{kPerceptualAbsent};
    float auxCoherence{kPerceptualAbsent};
    float xtcEnabled{kPerceptualAbsent};
    float temporalEnabled{kPerceptualAbsent};
    float temporalAmount{kPerceptualAbsent};
    float temporalAuto{kPerceptualAbsent};
    float npeEnabled{kPerceptualAbsent};
    float npeLateralInhib{kPerceptualAbsent};
    float npeOhcCompression{kPerceptualAbsent};
    float npeMasterDb{kPerceptualAbsent};
    float punchAmount{kPerceptualAbsent};
    float fourdOn{kPerceptualAbsent};
    float fourdArc{kPerceptualAbsent};
    float fourdXtc{kPerceptualAbsent};
    float fourdArcDepth{kPerceptualAbsent};
    float fourdDepth{kPerceptualAbsent};
    float fourdLeadMs{kPerceptualAbsent};
    float hrtfEnabled{kPerceptualAbsent};
    float adaptiveEnabled{kPerceptualAbsent};
    float masterBypass{kPerceptualAbsent};

    // FIX 1 (bofo Genezi, v2.5.465): modo de salida (OmegaOutputMode:
    // 0 = desconocido, 1 = audífonos, 2 = bocinas/Genezi), puesto por la
    // app en SET_PERCEPTUAL_STATE ("outputMode", PersistedStateRestorer).
    // NaN = ausente (se conserva el vigente, como el resto de campos).
    // El daemon NO neutraliza nada aquí: solo lo escribe en
    // temporal_field bits 18..19; el EFECTO lo hace cumplir.
    float outputMode{kPerceptualAbsent};

    // Pose / escena
    float headYawRad{kPerceptualAbsent};
    float headPitchRad{kPerceptualAbsent};
    // v2.5.472 (Fase 2, ítem E): escala del parallax 0..1 (SpatialControls).
    // NaN = ausente (se conserva la vigente). El responder la aplica a la
    // pose de SET_HEAD_POSE / SET_SCENE_POSE al parsear (el snapshot ABI
    // v13 NO cambia: no hay campo de parallax en el snapshot).
    float parallaxAmount{kPerceptualAbsent};
    float sceneDistance{kPerceptualAbsent};

    // SAF / RoomCorrection / EQ 10 bandas
    float safDeltaEnergy{kPerceptualAbsent};
    float safMetricNorm{kPerceptualAbsent};
    float safMemory{kPerceptualAbsent};
    float safGain{kPerceptualAbsent};
    bool hasSafQ{false};
    float safQ[7]{};
    bool hasRoomCorr{false};
    float roomCorrDb[10]{};
    bool hasEqBands{false};
    float eqGains[10]{};
    float listenPhon{kPerceptualAbsent};
    float refPhon{kPerceptualAbsent};

    // User flags (SET_USER_FLAGS: fastrpc/volterra/ati/etc.)
    uint32_t userFlagsMask{0u};
    uint32_t userFlagsValue{0u};

    // PF params (SET_PF_PARAMS -> compressor/comp_amount). PF NO lo lee
    // omega_effect hoy (command_server.cpp:279), pero el protocolo lo
    // transporta y el snapshot lo lleva; se aplica por fidelidad.
    bool hasPfParams{false};
    float pfParams[13]{};
    // PF de un solo slot (SET_PF_<NOMBRE>:<v> por texto: DRIVE/WET/MIX/...).
    // Se aplica DESPUES del array completo si ambos vienen en el mismo
    // drenaje (el slot unico es el comando mas especifico/reciente).
    int pfSingleIdx{-1};
    float pfSingleValue{kPerceptualAbsent};

    // ¿Trae al menos un campo presente? Se comprueban uno a uno para no
    // depender del layout del struct.
    bool any() const noexcept {
        return !std::isnan(compressor) || !std::isnan(exciterRed) ||
               !std::isnan(highCutHz) || !std::isnan(spatialWidth) ||
               !std::isnan(loudnessTarget) || !std::isnan(harmonicGain) ||
               !std::isnan(antiDolby) || !std::isnan(upmixingEnabled) ||
               !std::isnan(upmixingImmersivity) || !std::isnan(orbeAmount) ||
               !std::isnan(orbeDistanceM) || !std::isnan(orbeSpread) ||
               !std::isnan(orbeProximityDb) || !std::isnan(orbeDiffuse) ||
               !std::isnan(orbeHeadTracking) || !std::isnan(holosAmount) ||
               !std::isnan(holosDistanceM) || !std::isnan(holosSpread) ||
               !std::isnan(holosProximityDb) || !std::isnan(holosDiffuse) ||
               !std::isnan(holosHarmonics) || !std::isnan(holosWarmth) ||
               !std::isnan(holosDynamics) || !std::isnan(holosWidth) ||
               !std::isnan(holosGamma) || !std::isnan(holosRoomRt60) ||
               !std::isnan(holosHeadTracking) || !std::isnan(holosBandCount) ||
               !std::isnan(holosEqLow) || !std::isnan(holosEqMid) ||
               !std::isnan(holosEqHigh) || !std::isnan(holosEqPresence) ||
               !std::isnan(holosEqTiltDb) || !std::isnan(holosEqFreq) ||
               !std::isnan(hyperAmount) || !std::isnan(hyperPreset) ||
               !std::isnan(hyperXtcEnabled) || !std::isnan(volterraEnabled) ||
               !std::isnan(npAmount) || !std::isnan(npLgnMix) ||
               !std::isnan(npBsDamp) || !std::isnan(npTorsoMix) ||
               !std::isnan(npHeadRadiusM) || !std::isnan(npAzDeg) ||
               !std::isnan(npElDeg) || !std::isnan(npEqClass) ||
               !std::isnan(npOmegaSigMix) || !std::isnan(npBoostCapDb) ||
               !std::isnan(npXtalkMix) || !std::isnan(npSigmaDistM) ||
               !std::isnan(npSplDb) || !std::isnan(npAirMix) ||
               !std::isnan(npVolterraMix) || !std::isnan(npFreeEnergyMix) ||
               !std::isnan(npAbxMode) ||
               !std::isnan(supremeLatticeBlDrive) || !std::isnan(supremeLatticeLambda) ||
               !std::isnan(supremeCvnnHarmonicGain) || !std::isnan(supremeCvnnImdCancel) ||
               !std::isnan(supremeCvnnTapeDrive) || !std::isnan(supremeSnnImmersivity) ||
               !std::isnan(supremeSnnSpikeThreshold) || !std::isnan(supremePinnaWetMix) ||
               !std::isnan(supremePinnaConchaDepth) || !std::isnan(supremePinnaHelixCurl) ||
               !std::isnan(supremePinnaHeadWidth) || !std::isnan(supremeMsoItdNs) ||
               !std::isnan(fotoholoEnabled) || !std::isnan(fotoholoIntensity) ||
               !std::isnan(cochlearEnabled) || !std::isnan(cochlearIntensity) ||
               !std::isnan(realityReconEnabled) || !std::isnan(bassBoostDb) ||
               !std::isnan(dialogBoostDb) || !std::isnan(widenerMult) ||
               !std::isnan(spscRingFactor) || !std::isnan(yamnetSpeech) ||
               !std::isnan(yamnetMusic) || !std::isnan(yamnetClassId) ||
               !std::isnan(yamnetConfidence) ||
               !std::isnan(consvAmount) || !std::isnan(consvEnergyCap) ||
               !std::isnan(consvPhaseCoherence) || !std::isnan(consvInertia) ||
               !std::isnan(intensity) || !std::isnan(targetGain) ||
               !std::isnan(compAmount) || !std::isnan(excRed) ||
               !std::isnan(roomRt60S) || !std::isnan(roomWet) || !std::isnan(roomIdx) ||
               !std::isnan(wfsEnabled) || !std::isnan(wfsSpread) || hasWfsLayout ||
               !std::isnan(auxEnabled) || !std::isnan(auxXoverHz) || !std::isnan(auxCoherence) ||
               !std::isnan(xtcEnabled) || !std::isnan(temporalEnabled) ||
               !std::isnan(temporalAmount) || !std::isnan(temporalAuto) ||
               !std::isnan(npeEnabled) || !std::isnan(npeLateralInhib) ||
               !std::isnan(npeOhcCompression) || !std::isnan(npeMasterDb) ||
               !std::isnan(punchAmount) || !std::isnan(fourdOn) ||
               !std::isnan(fourdArc) || !std::isnan(fourdXtc) ||
               !std::isnan(fourdArcDepth) || !std::isnan(fourdDepth) || !std::isnan(fourdLeadMs) ||
               !std::isnan(hrtfEnabled) || !std::isnan(adaptiveEnabled) ||
               !std::isnan(masterBypass) || !std::isnan(outputMode) || !std::isnan(headYawRad) ||
               !std::isnan(headPitchRad) || !std::isnan(parallaxAmount) || !std::isnan(sceneDistance) ||
               !std::isnan(safDeltaEnergy) || !std::isnan(safMetricNorm) ||
               !std::isnan(safMemory) || !std::isnan(safGain) ||
               hasSafQ || hasRoomCorr || hasEqBands ||
               !std::isnan(listenPhon) || !std::isnan(refPhon) ||
               userFlagsMask != 0u || hasPfParams || pfSingleIdx >= 0;
    }

    // Merge: los campos presentes de `in` sobrescriben; los ausentes
    // conservan el valor vigente. La generación la pone el stash.
    void merge(const PendingPerceptual& in) noexcept {
#define IVANNA_PERCEPTUAL_MERGE(field) \
    if (!std::isnan(in.field)) field = in.field
        IVANNA_PERCEPTUAL_MERGE(compressor);
        IVANNA_PERCEPTUAL_MERGE(exciterRed);
        IVANNA_PERCEPTUAL_MERGE(highCutHz);
        IVANNA_PERCEPTUAL_MERGE(spatialWidth);
        IVANNA_PERCEPTUAL_MERGE(loudnessTarget);
        IVANNA_PERCEPTUAL_MERGE(harmonicGain);
        IVANNA_PERCEPTUAL_MERGE(antiDolby);
        IVANNA_PERCEPTUAL_MERGE(upmixingEnabled);
        IVANNA_PERCEPTUAL_MERGE(upmixingImmersivity);
        IVANNA_PERCEPTUAL_MERGE(orbeAmount);
        IVANNA_PERCEPTUAL_MERGE(orbeDistanceM);
        IVANNA_PERCEPTUAL_MERGE(orbeSpread);
        IVANNA_PERCEPTUAL_MERGE(orbeProximityDb);
        IVANNA_PERCEPTUAL_MERGE(orbeDiffuse);
        IVANNA_PERCEPTUAL_MERGE(orbeHeadTracking);
        IVANNA_PERCEPTUAL_MERGE(holosAmount);
        IVANNA_PERCEPTUAL_MERGE(holosDistanceM);
        IVANNA_PERCEPTUAL_MERGE(holosSpread);
        IVANNA_PERCEPTUAL_MERGE(holosProximityDb);
        IVANNA_PERCEPTUAL_MERGE(holosDiffuse);
        IVANNA_PERCEPTUAL_MERGE(holosHarmonics);
        IVANNA_PERCEPTUAL_MERGE(holosWarmth);
        IVANNA_PERCEPTUAL_MERGE(holosDynamics);
        IVANNA_PERCEPTUAL_MERGE(holosWidth);
        IVANNA_PERCEPTUAL_MERGE(holosGamma);
        IVANNA_PERCEPTUAL_MERGE(holosRoomRt60);
        IVANNA_PERCEPTUAL_MERGE(holosHeadTracking);
        IVANNA_PERCEPTUAL_MERGE(holosBandCount);
        IVANNA_PERCEPTUAL_MERGE(holosEqLow);
        IVANNA_PERCEPTUAL_MERGE(holosEqMid);
        IVANNA_PERCEPTUAL_MERGE(holosEqHigh);
        IVANNA_PERCEPTUAL_MERGE(holosEqPresence);
        IVANNA_PERCEPTUAL_MERGE(holosEqTiltDb);
        IVANNA_PERCEPTUAL_MERGE(holosEqFreq);
        IVANNA_PERCEPTUAL_MERGE(hyperAmount);
        IVANNA_PERCEPTUAL_MERGE(hyperPreset);
        IVANNA_PERCEPTUAL_MERGE(hyperXtcEnabled);
        IVANNA_PERCEPTUAL_MERGE(volterraEnabled);
        IVANNA_PERCEPTUAL_MERGE(npAmount);
        IVANNA_PERCEPTUAL_MERGE(npLgnMix);
        IVANNA_PERCEPTUAL_MERGE(npBsDamp);
        IVANNA_PERCEPTUAL_MERGE(npTorsoMix);
        IVANNA_PERCEPTUAL_MERGE(npHeadRadiusM);
        IVANNA_PERCEPTUAL_MERGE(npAzDeg);
        IVANNA_PERCEPTUAL_MERGE(npElDeg);
        IVANNA_PERCEPTUAL_MERGE(npEqClass);
        IVANNA_PERCEPTUAL_MERGE(npOmegaSigMix);
        IVANNA_PERCEPTUAL_MERGE(npBoostCapDb);
        IVANNA_PERCEPTUAL_MERGE(npXtalkMix);
        IVANNA_PERCEPTUAL_MERGE(npSigmaDistM);
        IVANNA_PERCEPTUAL_MERGE(npSplDb);
        IVANNA_PERCEPTUAL_MERGE(npAirMix);
        IVANNA_PERCEPTUAL_MERGE(npVolterraMix);
        IVANNA_PERCEPTUAL_MERGE(npFreeEnergyMix);
        IVANNA_PERCEPTUAL_MERGE(npAbxMode);
        IVANNA_PERCEPTUAL_MERGE(supremeLatticeBlDrive);
        IVANNA_PERCEPTUAL_MERGE(supremeLatticeLambda);
        IVANNA_PERCEPTUAL_MERGE(supremeCvnnHarmonicGain);
        IVANNA_PERCEPTUAL_MERGE(supremeCvnnImdCancel);
        IVANNA_PERCEPTUAL_MERGE(supremeCvnnTapeDrive);
        IVANNA_PERCEPTUAL_MERGE(supremeSnnImmersivity);
        IVANNA_PERCEPTUAL_MERGE(supremeSnnSpikeThreshold);
        IVANNA_PERCEPTUAL_MERGE(supremePinnaWetMix);
        IVANNA_PERCEPTUAL_MERGE(supremePinnaConchaDepth);
        IVANNA_PERCEPTUAL_MERGE(supremePinnaHelixCurl);
        IVANNA_PERCEPTUAL_MERGE(supremePinnaHeadWidth);
        IVANNA_PERCEPTUAL_MERGE(supremeMsoItdNs);
        // ABI v16 (v2.5.486): FotoHolo / coclear / reality / route-profile /
        // SPSC / YAMNet.
        IVANNA_PERCEPTUAL_MERGE(fotoholoEnabled);
        IVANNA_PERCEPTUAL_MERGE(fotoholoIntensity);
        IVANNA_PERCEPTUAL_MERGE(cochlearEnabled);
        IVANNA_PERCEPTUAL_MERGE(cochlearIntensity);
        IVANNA_PERCEPTUAL_MERGE(realityReconEnabled);
        IVANNA_PERCEPTUAL_MERGE(bassBoostDb);
        IVANNA_PERCEPTUAL_MERGE(dialogBoostDb);
        IVANNA_PERCEPTUAL_MERGE(widenerMult);
        IVANNA_PERCEPTUAL_MERGE(spscRingFactor);
        IVANNA_PERCEPTUAL_MERGE(yamnetSpeech);
        IVANNA_PERCEPTUAL_MERGE(yamnetMusic);
        IVANNA_PERCEPTUAL_MERGE(yamnetClassId);
        IVANNA_PERCEPTUAL_MERGE(yamnetConfidence);
        // ABI v17 (2026-11-21): conservación perceptual.
        IVANNA_PERCEPTUAL_MERGE(consvAmount);
        IVANNA_PERCEPTUAL_MERGE(consvEnergyCap);
        IVANNA_PERCEPTUAL_MERGE(consvPhaseCoherence);
        IVANNA_PERCEPTUAL_MERGE(consvInertia);
        IVANNA_PERCEPTUAL_MERGE(intensity);
        IVANNA_PERCEPTUAL_MERGE(targetGain);
        IVANNA_PERCEPTUAL_MERGE(compAmount);
        IVANNA_PERCEPTUAL_MERGE(excRed);
        IVANNA_PERCEPTUAL_MERGE(roomRt60S);
        IVANNA_PERCEPTUAL_MERGE(roomWet);
        IVANNA_PERCEPTUAL_MERGE(roomIdx);
        IVANNA_PERCEPTUAL_MERGE(wfsEnabled);
        IVANNA_PERCEPTUAL_MERGE(wfsSpread);
        IVANNA_PERCEPTUAL_MERGE(auxEnabled);
        IVANNA_PERCEPTUAL_MERGE(auxXoverHz);
        IVANNA_PERCEPTUAL_MERGE(auxCoherence);
        IVANNA_PERCEPTUAL_MERGE(xtcEnabled);
        IVANNA_PERCEPTUAL_MERGE(temporalEnabled);
        IVANNA_PERCEPTUAL_MERGE(temporalAmount);
        IVANNA_PERCEPTUAL_MERGE(temporalAuto);
        IVANNA_PERCEPTUAL_MERGE(npeEnabled);
        IVANNA_PERCEPTUAL_MERGE(npeLateralInhib);
        IVANNA_PERCEPTUAL_MERGE(npeOhcCompression);
        IVANNA_PERCEPTUAL_MERGE(npeMasterDb);
        IVANNA_PERCEPTUAL_MERGE(punchAmount);
        IVANNA_PERCEPTUAL_MERGE(fourdOn);
        IVANNA_PERCEPTUAL_MERGE(fourdArc);
        IVANNA_PERCEPTUAL_MERGE(fourdXtc);
        IVANNA_PERCEPTUAL_MERGE(fourdArcDepth);
        IVANNA_PERCEPTUAL_MERGE(fourdDepth);
        IVANNA_PERCEPTUAL_MERGE(fourdLeadMs);
        IVANNA_PERCEPTUAL_MERGE(hrtfEnabled);
        IVANNA_PERCEPTUAL_MERGE(adaptiveEnabled);
        IVANNA_PERCEPTUAL_MERGE(masterBypass);
        IVANNA_PERCEPTUAL_MERGE(outputMode);
        IVANNA_PERCEPTUAL_MERGE(headYawRad);
        IVANNA_PERCEPTUAL_MERGE(headPitchRad);
        IVANNA_PERCEPTUAL_MERGE(parallaxAmount);
        IVANNA_PERCEPTUAL_MERGE(sceneDistance);
        IVANNA_PERCEPTUAL_MERGE(safDeltaEnergy);
        IVANNA_PERCEPTUAL_MERGE(safMetricNorm);
        IVANNA_PERCEPTUAL_MERGE(safMemory);
        IVANNA_PERCEPTUAL_MERGE(safGain);
        IVANNA_PERCEPTUAL_MERGE(listenPhon);
        IVANNA_PERCEPTUAL_MERGE(refPhon);
        IVANNA_PERCEPTUAL_MERGE(pfSingleValue);
        if (in.hasWfsLayout) {
            hasWfsLayout = true;
            for (int i = 0; i < 7; ++i) {
                wfsSpeakerX[i] = in.wfsSpeakerX[i];
                wfsSpeakerY[i] = in.wfsSpeakerY[i];
                wfsSpeakerZ[i] = in.wfsSpeakerZ[i];
            }
        }
        if (in.hasSafQ) {
            hasSafQ = true;
            for (int i = 0; i < 7; ++i) safQ[i] = in.safQ[i];
        }
        if (in.hasRoomCorr) {
            hasRoomCorr = true;
            for (int i = 0; i < 10; ++i) roomCorrDb[i] = in.roomCorrDb[i];
        }
        if (in.hasEqBands) {
            hasEqBands = true;
            for (int i = 0; i < 10; ++i) eqGains[i] = in.eqGains[i];
        }
        if (in.userFlagsMask != 0u) {
            userFlagsMask |= in.userFlagsMask;
            userFlagsValue = (userFlagsValue & ~in.userFlagsMask) |
                             (in.userFlagsValue & in.userFlagsMask);
        }
        if (in.hasPfParams) {
            hasPfParams = true;
            for (int i = 0; i < 13; ++i) pfParams[i] = in.pfParams[i];
        }
        if (in.pfSingleIdx >= 0) pfSingleIdx = in.pfSingleIdx;
#undef IVANNA_PERCEPTUAL_MERGE
    }
};

// Aplica los campos presentes de `p` a `s` con los clamps del servidor
// viejo (command_server.cpp, SET_PERCEPTUAL_STATE). Los ausentes no se
// tocan. Pura (sin I/O, sin locks): la usa el bucle del daemon y el test.
inline void applyPendingPerceptual(ivanna::OmegaDspSnapshot& s,
                                   const PendingPerceptual& p) noexcept {
    if (!std::isnan(p.compressor)) s.compressor = p.compressor;
    // exciterRed va al slot perceptual (exciter_reduction); exc_red es
    // salida adaptativa aparte y no se toca aquí.
    if (!std::isnan(p.exciterRed)) s.exciter_reduction = p.exciterRed;
    if (!std::isnan(p.highCutHz)) s.high_cut_hz = p.highCutHz;
    if (!std::isnan(p.spatialWidth)) s.spatial_width = p.spatialWidth;
    if (!std::isnan(p.loudnessTarget)) s.loudness_target = p.loudnessTarget;
    if (!std::isnan(p.harmonicGain)) s.harmonic_gain = p.harmonicGain;
    if (!std::isnan(p.antiDolby)) s.anti_dolby = p.antiDolby;
    if (!std::isnan(p.upmixingEnabled))
        s.upmixing_enabled = (p.upmixingEnabled >= 0.5f) ? 1u : 0u;
    if (!std::isnan(p.upmixingImmersivity))
        s.upmixing_immersivity = p.upmixingImmersivity;

    if (!std::isnan(p.orbeAmount))
        s.orbe_amount = ivanna::omega_clamp_orbe_amount(p.orbeAmount);
    if (!std::isnan(p.orbeDistanceM))
        s.orbe_distance_m = std::clamp(p.orbeDistanceM, 0.15f, 2.0f);
    if (!std::isnan(p.orbeSpread))
        s.orbe_spread = ivanna::omega_clamp_orbe_spread(p.orbeSpread);
    if (!std::isnan(p.orbeProximityDb))
        s.orbe_proximity_db = std::clamp(p.orbeProximityDb, 0.f, 6.f);
    if (!std::isnan(p.orbeDiffuse))
        s.orbe_diffuse = std::clamp(p.orbeDiffuse, 0.f, 1.f);
    if (!std::isnan(p.orbeHeadTracking))
        s.orbe_head_tracking = (p.orbeHeadTracking >= 0.5f) ? 1u : 0u;

    if (!std::isnan(p.holosAmount))
        s.holos_amount = ivanna::omega_clamp_holos_amount(p.holosAmount);
    if (!std::isnan(p.holosDistanceM))
        s.holos_distance_m = std::clamp(p.holosDistanceM, 0.15f, 2.0f);
    if (!std::isnan(p.holosSpread))
        s.holos_spread = std::clamp(p.holosSpread, 0.f, 1.5f);
    if (!std::isnan(p.holosProximityDb))
        s.holos_proximity_db = std::clamp(p.holosProximityDb, 0.f, 6.f);
    if (!std::isnan(p.holosDiffuse))
        s.holos_diffuse = ivanna::omega_clamp_holos_diffuse(p.holosDiffuse);
    if (!std::isnan(p.holosHarmonics))
        s.holos_harmonics = std::clamp(p.holosHarmonics, 0.f, 1.f);
    if (!std::isnan(p.holosWarmth))
        s.holos_warmth = std::clamp(p.holosWarmth, 0.f, 1.f);
    if (!std::isnan(p.holosDynamics))
        s.holos_dynamics = std::clamp(p.holosDynamics, 0.f, 1.f);
    if (!std::isnan(p.holosWidth))
        s.holos_width = std::clamp(p.holosWidth, 0.f, 1.5f);
    if (!std::isnan(p.holosGamma))
        s.holos_gamma = std::clamp(p.holosGamma, 0.5f, 1.f);
    if (!std::isnan(p.holosRoomRt60))
        s.holos_room_rt60 = std::clamp(p.holosRoomRt60, 0.18f, 1.8f);
    if (!std::isnan(p.holosHeadTracking))
        s.holos_head_tracking = (p.holosHeadTracking >= 0.5f) ? 1u : 0u;
    if (!std::isnan(p.holosBandCount))
        s.holos_band_count = (p.holosBandCount <= 18.f) ? 12u : 24u;
    if (!std::isnan(p.holosEqLow))
        s.holos_eq_low = std::clamp(p.holosEqLow, -12.f, 12.f);
    if (!std::isnan(p.holosEqMid))
        s.holos_eq_mid = std::clamp(p.holosEqMid, -12.f, 12.f);
    if (!std::isnan(p.holosEqHigh))
        s.holos_eq_high = std::clamp(p.holosEqHigh, -12.f, 12.f);
    if (!std::isnan(p.holosEqPresence))
        s.holos_eq_presence = std::clamp(p.holosEqPresence, -12.f, 12.f);
    if (!std::isnan(p.holosEqTiltDb))
        s.holos_eq_tilt_db = std::clamp(p.holosEqTiltDb, -12.f, 12.f);
    if (!std::isnan(p.holosEqFreq))
        s.holos_eq_freq = std::clamp(p.holosEqFreq, 20.f, 20000.f);

    // Φ + Volterra (ABI v13, 0038+0040): paridad Ruta B. Volterra se
    // gobierna por el bit OMEGA_FLAG_VOLTERRA_ON en flags (el efecto lo
    // lee en omega_process); se sincroniza aquí con el campo canónico
    // volterra_enabled para que la app lo vea por GET_STATUS.
    if (!std::isnan(p.hyperAmount))
        s.hyper_amount = std::clamp(p.hyperAmount, 0.f, 1.f);
    if (!std::isnan(p.hyperPreset))
        s.hyper_preset = static_cast<int32_t>(std::clamp(p.hyperPreset, 0.f, 2.f));
    if (!std::isnan(p.hyperXtcEnabled))
        s.hyper_xtc_enabled = (p.hyperXtcEnabled >= 0.5f) ? 1u : 0u;
    if (!std::isnan(p.volterraEnabled)) {
        const bool on = p.volterraEnabled >= 0.5f;
        s.volterra_enabled = on ? 1u : 0u;
        if (on) s.flags |= ivanna::OMEGA_FLAG_VOLTERRA_ON;
        else s.flags &= ~ivanna::OMEGA_FLAG_VOLTERRA_ON;
    }

    // Física Nueva (ABI v14, Fase 2, v2.5.472): los 17 controles
    // NewPhysicsControls al snapshot. Rangos = NewPhysicsControls::
    // clamp() (fused/new_physics_controls.hpp); el NaN ya está
    // descartado por el guarda isnan de cada campo (ausente = se
    // conserva el vigente, nunca basura). np_amount 0 = bypass
    // bit-exacto en el efecto; los int (eq_class 0..6, abx 0..2) se
    // clampean como float y se castean, igual que hyper_preset.
    // OVERDRIVE v2.5.480 (Esse): np_amount y np_xtalk_mix admiten hasta
    // 1.5 — 1.0 es el 100% (mezcla wet/dry completa), 1.5 extrapola la
    // diferencia wet−dry ×1.5 (más holograma; puede sobre-excitar, se
    // prueba en escucha). El tope 1.0 era el cap, no un límite físico.
    if (!std::isnan(p.npAmount))
        s.np_amount = std::clamp(p.npAmount, 0.f, 1.5f);
    if (!std::isnan(p.npLgnMix))
        s.np_lgn_mix = std::clamp(p.npLgnMix, 0.f, 1.f);
    if (!std::isnan(p.npBsDamp))
        s.np_bs_damp = std::clamp(p.npBsDamp, 0.f, 1.f);
    if (!std::isnan(p.npTorsoMix))
        s.np_torso_mix = std::clamp(p.npTorsoMix, 0.f, 0.5f);
    if (!std::isnan(p.npHeadRadiusM))
        s.np_head_radius_m = std::clamp(p.npHeadRadiusM, 0.065f, 0.115f);
    if (!std::isnan(p.npAzDeg))
        s.np_az_deg = std::clamp(p.npAzDeg, -180.f, 180.f);
    if (!std::isnan(p.npElDeg))
        s.np_el_deg = std::clamp(p.npElDeg, -40.f, 70.f);
    if (!std::isnan(p.npEqClass))
        s.np_eq_class = static_cast<int32_t>(std::clamp(p.npEqClass, 0.f, 6.f));
    if (!std::isnan(p.npOmegaSigMix))
        s.np_omega_sig_mix = std::clamp(p.npOmegaSigMix, 0.f, 1.f);
    if (!std::isnan(p.npBoostCapDb))
        s.np_boost_cap_db = std::clamp(p.npBoostCapDb, 0.f, 9.f);
    if (!std::isnan(p.npXtalkMix))
        s.np_xtalk_mix = std::clamp(p.npXtalkMix, 0.f, 1.5f);
    if (!std::isnan(p.npSigmaDistM))
        s.np_sigma_dist_m = std::clamp(p.npSigmaDistM, 0.15f, 80.f);
    if (!std::isnan(p.npSplDb))
        s.np_spl_db = std::clamp(p.npSplDb, 65.f, 115.f);
    if (!std::isnan(p.npAirMix))
        s.np_air_mix = std::clamp(p.npAirMix, 0.f, 1.f);
    if (!std::isnan(p.npVolterraMix))
        s.np_volterra_mix = std::clamp(p.npVolterraMix, 0.f, 1.f);
    if (!std::isnan(p.npFreeEnergyMix))
        s.np_free_energy_mix = std::clamp(p.npFreeEnergyMix, 0.f, 1.f);
    if (!std::isnan(p.npAbxMode))
        s.np_abx_mode = static_cast<int32_t>(std::clamp(p.npAbxMode, 0.f, 2.f));

    // 5 Ejes Supremos (ABI v15, v2.5.481): del responder al snapshot.
    // Rangos = los del snapshot (omega_control_bus.h). El NaN ya está
    // descartado por el guarda isnan (ausente = se conserva el vigente).
    if (!std::isnan(p.supremeLatticeBlDrive))
        s.supreme_lattice_bl_drive = std::clamp(p.supremeLatticeBlDrive, 0.f, 2.f);
    if (!std::isnan(p.supremeLatticeLambda))
        s.supreme_lattice_lambda = std::clamp(p.supremeLatticeLambda, -0.85f, 0.85f);
    if (!std::isnan(p.supremeCvnnHarmonicGain))
        s.supreme_cvnn_harmonic_gain = std::clamp(p.supremeCvnnHarmonicGain, 0.f, 1.f);
    if (!std::isnan(p.supremeCvnnImdCancel))
        s.supreme_cvnn_imd_cancel = std::clamp(p.supremeCvnnImdCancel, 0.f, 1.f);
    if (!std::isnan(p.supremeCvnnTapeDrive))
        s.supreme_cvnn_tape_drive = std::clamp(p.supremeCvnnTapeDrive, 0.f, 1.f);
    if (!std::isnan(p.supremeSnnImmersivity))
        s.supreme_snn_immersivity = std::clamp(p.supremeSnnImmersivity, 0.f, 1.f);
    if (!std::isnan(p.supremeSnnSpikeThreshold))
        s.supreme_snn_spike_threshold = std::clamp(p.supremeSnnSpikeThreshold, 0.15f, 1.5f);
    if (!std::isnan(p.supremePinnaWetMix))
        s.supreme_pinna_wet_mix = std::clamp(p.supremePinnaWetMix, 0.f, 1.f);
    if (!std::isnan(p.supremePinnaConchaDepth))
        s.supreme_pinna_concha_depth = std::clamp(p.supremePinnaConchaDepth, -1.f, 1.f);
    if (!std::isnan(p.supremePinnaHelixCurl))
        s.supreme_pinna_helix_curl = std::clamp(p.supremePinnaHelixCurl, -1.f, 1.f);
    if (!std::isnan(p.supremePinnaHeadWidth))
        s.supreme_pinna_head_width = std::clamp(p.supremePinnaHeadWidth, -1.f, 1.f);
    if (!std::isnan(p.supremeMsoItdNs))
        s.supreme_mso_itd_ns = std::clamp(p.supremeMsoItdNs, -50000.f, 50000.f);

    // ABI v16 (v2.5.486): los 16 comandos portados del handler muerto.
    // FotoHolo: bits 6..15 de temporal_field (omega_fotoholo_encode/decode);
    // se preserva la parte no tocada (actualización parcial: enabled solo
    // no borra la intensidad vigente, ni al revés).
    if (!std::isnan(p.fotoholoEnabled) || !std::isnan(p.fotoholoIntensity)) {
        bool curOn = false; float curInt = 0.f;
        ivanna::omega_fotoholo_decode(s.temporal_field, curOn, curInt);
        const bool en = !std::isnan(p.fotoholoEnabled) ? (p.fotoholoEnabled >= 0.5f) : curOn;
        const float in = !std::isnan(p.fotoholoIntensity) ? std::clamp(p.fotoholoIntensity, 0.f, 1.f) : curInt;
        s.temporal_field = (s.temporal_field & ~(ivanna::OMEGA_FOTOHOLO_ON_BIT |
                                                 ivanna::OMEGA_FOTOHOLO_INTENSITY_MASK)) |
                           ivanna::omega_fotoholo_encode(en, in);
    }
    // Coclear: intensidad al campo propio; el bit de habilitado va en flags
    // (OMEGA_FLAG_COCHLEAR_ON), igual que VOLTERRA_ON arriba.
    if (!std::isnan(p.cochlearIntensity))
        s.cochlear_intensity = std::clamp(p.cochlearIntensity, 0.f, 1.f);
    if (!std::isnan(p.cochlearEnabled)) {
        if (p.cochlearEnabled >= 0.5f) s.flags |= ivanna::OMEGA_FLAG_COCHLEAR_ON;
        else s.flags &= ~ivanna::OMEGA_FLAG_COCHLEAR_ON;
    }
    // Reconstrucción de realidad: solo el flag (OMEGA_FLAG_REALITY_RECON_ON);
    // la intensidad va por el slot p.intensity, que ya aplica la sección
    // Fallo 1 de abajo (el servidor viejo también escribía s.intensity).
    if (!std::isnan(p.realityReconEnabled)) {
        if (p.realityReconEnabled >= 0.5f) s.flags |= ivanna::OMEGA_FLAG_REALITY_RECON_ON;
        else s.flags &= ~ivanna::OMEGA_FLAG_REALITY_RECON_ON;
    }
    // Perfil de ruta (SET_ROUTE_PROFILE): boost de graves / diálogo /
    // ensanchador. El servidor viejo no clampeaba; aquí se acota al rango
    // útil (el preset "warm" pide +3 dB de graves).
    if (!std::isnan(p.bassBoostDb))
        s.bass_boost_db = std::clamp(p.bassBoostDb, -12.f, 12.f);
    if (!std::isnan(p.dialogBoostDb))
        s.dialog_boost_db = std::clamp(p.dialogBoostDb, -12.f, 12.f);
    if (!std::isnan(p.widenerMult))
        s.widener_mult = std::clamp(p.widenerMult, 0.f, 2.f);
    // SPSC + YAMNet (ABI v16): campos nuevos del snapshot.
    if (!std::isnan(p.spscRingFactor))
        s.spsc_ring_factor = std::clamp(p.spscRingFactor, 0.5f, 2.0f);
    if (!std::isnan(p.yamnetSpeech))
        s.yamnet_speech = std::clamp(p.yamnetSpeech, 0.f, 1.f);
    if (!std::isnan(p.yamnetMusic))
        s.yamnet_music = std::clamp(p.yamnetMusic, 0.f, 1.f);
    if (!std::isnan(p.yamnetClassId))
        s.yamnet_class_id = static_cast<int32_t>(std::clamp(p.yamnetClassId, 0.f, 520.f));
    if (!std::isnan(p.yamnetConfidence))
        s.yamnet_confidence = std::clamp(p.yamnetConfidence, 0.f, 1.f);
    // Conservación perceptual (ABI v17): campos nuevos del snapshot.
    if (!std::isnan(p.consvAmount))
        s.consv_amount = std::clamp(p.consvAmount, 0.f, 1.f);
    if (!std::isnan(p.consvEnergyCap))
        s.consv_energy_cap = std::clamp(p.consvEnergyCap, 0.f, 1.f);
    if (!std::isnan(p.consvPhaseCoherence))
        s.consv_phase_coherence = std::clamp(p.consvPhaseCoherence, 0.f, 1.f);
    if (!std::isnan(p.consvInertia))
        s.consv_inertia = std::clamp(p.consvInertia, 0.f, 0.99f);

    // ── Fallo 1: el resto del protocolo real de la app ─────────────────
    if (!std::isnan(p.intensity)) s.intensity = std::clamp(p.intensity, 0.f, 1.f);
    if (!std::isnan(p.targetGain)) s.target_gain = std::clamp(p.targetGain, -24.f, 24.f);
    if (!std::isnan(p.compAmount)) s.comp_amount = std::clamp(p.compAmount, 0.f, 1.f);
    if (!std::isnan(p.excRed)) s.exc_red = std::clamp(p.excRed, 0.f, 1.f);

    if (!std::isnan(p.roomRt60S)) s.room_rt60_s = std::clamp(p.roomRt60S, 0.f, 6.f);  // servidor viejo: [0,6] s
    if (!std::isnan(p.roomWet)) s.room_wet = std::clamp(p.roomWet, 0.f, 1.f);
    if (!std::isnan(p.roomIdx)) s.room_idx = static_cast<int32_t>(p.roomIdx);

    if (!std::isnan(p.wfsEnabled) || !std::isnan(p.wfsSpread) || p.hasWfsLayout) {
        if (!std::isnan(p.wfsEnabled)) s.wfs_enabled = p.wfsEnabled >= 0.5f ? 1u : 0u;
        if (!std::isnan(p.wfsSpread)) s.wfs_spread = std::clamp(p.wfsSpread, 0.f, 1.f);
        if (p.hasWfsLayout) {
            for (int i = 0; i < 7; ++i) {
                s.wfs_speaker_x[i] = p.wfsSpeakerX[i];
                s.wfs_speaker_y[i] = p.wfsSpeakerY[i];
                s.wfs_speaker_z[i] = p.wfsSpeakerZ[i];
            }
        }
    }

    if (!std::isnan(p.auxEnabled) || !std::isnan(p.auxXoverHz) || !std::isnan(p.auxCoherence)) {
        // 0044 (coord.): al actualizar parcial se CONSERVAN xo/co actuales
        // (decodificados de flags), no defaults fijos 120 Hz / 1.0.
        bool curEn = false; float curXo = 120.f, curCo = 1.f;
        ivanna::omega_aux_field_decode(s.flags, curEn, curXo, curCo);
        const bool en = !std::isnan(p.auxEnabled) ? (p.auxEnabled >= 0.5f) : curEn;
        const float xo = !std::isnan(p.auxXoverHz) ? std::clamp(p.auxXoverHz, 60.f, 200.f) : curXo;
        const float co = !std::isnan(p.auxCoherence) ? std::clamp(p.auxCoherence, 0.f, 1.f) : curCo;
        s.flags = (s.flags & ~(ivanna::OMEGA_FLAG_AUX_FIELD_ON |
                               ivanna::OMEGA_FLAG_AUX_XOVER_MASK |
                               ivanna::OMEGA_FLAG_AUX_COH_MASK))
                | ivanna::omega_aux_field_encode(en, xo, co);
    }

    // XTC (diafonía, altavoces): vive en temporal_field bit 5
    // (OMEGA_TEMPORAL_FIELD_XTC_BIT), NO en flags OMEGA_FLAG_ATI_ON
    // (ATI = Active Transducer Inversion, otra cosa). El efecto lo lee
    // en omega_effect.cpp:616. Ver publishCurrentState (command_server:227).
    if (!std::isnan(p.xtcEnabled)) {
        if (p.xtcEnabled >= 0.5f) s.temporal_field |= ivanna::OMEGA_TEMPORAL_FIELD_XTC_BIT;
        else s.temporal_field &= ~ivanna::OMEGA_TEMPORAL_FIELD_XTC_BIT;
    }
    if (!std::isnan(p.temporalEnabled) || !std::isnan(p.temporalAmount) || !std::isnan(p.temporalAuto)) {
        // 0044 (coord.): el campo temporal vive en s.temporal_field
        // (bits 0..4: on/amount/auto), NO en s.flags ni en un
        // s.temporal_amount inexistente. Se preservan bits 5..31
        // (XTC, FotoHolo 6..15, features 16..17).
        bool curOn = false; float curAmt = 0.6f; bool curAuto = false;
        ivanna::omega_temporal_field_decode(s.temporal_field, curOn, curAmt, curAuto);
        const bool en = !std::isnan(p.temporalEnabled) ? (p.temporalEnabled >= 0.5f) : curOn;
        const float amt = !std::isnan(p.temporalAmount) ? std::clamp(p.temporalAmount, 0.f, 1.f) : curAmt;
        const bool au = !std::isnan(p.temporalAuto) ? (p.temporalAuto >= 0.5f) : curAuto;
        s.temporal_field = (s.temporal_field & ~0x1Fu) |
                           ivanna::omega_temporal_field_encode(en, amt, au);
    }

    if (!std::isnan(p.npeEnabled) || !std::isnan(p.npeLateralInhib) ||
        !std::isnan(p.npeOhcCompression) || !std::isnan(p.npeMasterDb)) {
        // 0044 (coord.): omega_npe_ctl_encode SOLO toma (on, masterDb);
        // lateral_inhib y ohc_compression son campos propios del snapshot
        // (npe_lateral_inhib / npe_ohc_compression). Gemini pasaba 4 args.
        bool curOn = false; float curDb = 0.f;
        ivanna::omega_npe_ctl_decode(s.npe_ctl, curOn, curDb);
        const bool en = !std::isnan(p.npeEnabled) ? (p.npeEnabled >= 0.5f) : curOn;
        const float db = !std::isnan(p.npeMasterDb) ? std::clamp(p.npeMasterDb, -6.f, 6.f) : curDb;
        const uint32_t distBits = s.npe_ctl & 0xFFFF0000u;  // distancia de escena: no tocarla aquí
        s.npe_ctl = distBits | ivanna::omega_npe_ctl_encode(en, db);
        if (!std::isnan(p.npeLateralInhib)) s.npe_lateral_inhib = std::clamp(p.npeLateralInhib, 0.f, 1.f);
        if (!std::isnan(p.npeOhcCompression)) s.npe_ohc_compression = std::clamp(p.npeOhcCompression, 0.f, 1.f);
    }

    // PUNCH: palabra extendida ext_flags16 bits 0..3 (omega_punch_encode);
    // no existe s.punch_amount (Gemini lo inventó).
    if (!std::isnan(p.punchAmount)) {
        s.ext_flags16 = static_cast<uint16_t>(
            (s.ext_flags16 & ~ivanna::OMEGA_EXT_PUNCH_MASK) |
            ivanna::omega_punch_encode(std::clamp(p.punchAmount, 0.f, 1.f)));
    }
    // 4D: palabra extendida ext_flags16 bits 4..15 (omega_4d_encode);
    // no existen s.four_d_* (Gemini los inventó). Al actualizar parcial
    // se decodifica el estado vigente y se conservan los demás campos.
    if (!std::isnan(p.fourdOn) || !std::isnan(p.fourdArc) || !std::isnan(p.fourdXtc) ||
        !std::isnan(p.fourdArcDepth) || !std::isnan(p.fourdDepth) || !std::isnan(p.fourdLeadMs)) {
        ivanna::Omega4DExt e = ivanna::omega_4d_decode(s.ext_flags16);
        if (!std::isnan(p.fourdOn)) e.on = p.fourdOn >= 0.5f;
        if (!std::isnan(p.fourdArc)) e.arc = p.fourdArc >= 0.5f;
        if (!std::isnan(p.fourdXtc)) e.xtc = p.fourdXtc >= 0.5f;
        if (!std::isnan(p.fourdArcDepth)) e.arcDepth = std::clamp(p.fourdArcDepth, 0.f, 1.f);
        if (!std::isnan(p.fourdDepth)) e.depth = std::clamp(p.fourdDepth, 0.f, 1.f);
        if (!std::isnan(p.fourdLeadMs)) e.leadMs = std::clamp(p.fourdLeadMs, 5.f, 10.f);  // servidor viejo: [5,10] ms
        s.ext_flags16 = static_cast<uint16_t>(
            (s.ext_flags16 & ~ivanna::OMEGA_EXT_4D_MASK) | ivanna::omega_4d_encode(e));
    }

    // HRTF / Adaptive: el efecto los lee de temporal_field bits 16/17
    // (OMEGA_EFFECT_HRTF_ENABLED_BIT / ADAPTIVE_ENABLED_BIT,
    // omega_effect.cpp:461,1247), NO de flags OMEGA_FLAG_HRTF_ON /
    // OMEGA_FLAG_ADAPTIVE_ON (que el efecto no lee). Ver
    // publishCurrentState (command_server.cpp:230).
    if (!std::isnan(p.hrtfEnabled)) {
        if (p.hrtfEnabled >= 0.5f) s.temporal_field |= ivanna::OMEGA_EFFECT_HRTF_ENABLED_BIT;
        else s.temporal_field &= ~ivanna::OMEGA_EFFECT_HRTF_ENABLED_BIT;
    }
    if (!std::isnan(p.adaptiveEnabled)) {
        if (p.adaptiveEnabled >= 0.5f) s.temporal_field |= ivanna::OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT;
        else s.temporal_field &= ~ivanna::OMEGA_EFFECT_ADAPTIVE_ENABLED_BIT;
    }
    if (!std::isnan(p.masterBypass)) {
        if (p.masterBypass >= 0.5f) s.flags |= ivanna::OMEGA_FLAG_MASTER_BYPASS;
        else s.flags &= ~ivanna::OMEGA_FLAG_MASTER_BYPASS;
    }
    // FIX 1 (bofo Genezi, v2.5.465): modo de salida a temporal_field
    // bits 18..19 (omega_output_mode_encode). Solo esos dos bits se
    // tocan: el resto de temporal_field (temporal 0..4, XTC, FotoHolo,
    // features 16..17) se conserva. Fuera de rango (>= 3.5 o < 0) =
    // UNKNOWN (fail-closed: el efecto aplica el snapshot tal cual).
    // TERCER MODO — GENEZI (evol-holos, 2026-11-26): la app lo emite
    // como outputMode 3 (PersistedStateRestorer.outputModeFor); 2 sigue
    // siendo bocina plana.
    if (!std::isnan(p.outputMode)) {
        ivanna::OmegaOutputMode mode = ivanna::OmegaOutputMode::UNKNOWN;
        if (p.outputMode >= 0.5f && p.outputMode < 1.5f) mode = ivanna::OmegaOutputMode::HEADPHONES;
        else if (p.outputMode >= 1.5f && p.outputMode < 2.5f) mode = ivanna::OmegaOutputMode::SPEAKERS;
        else if (p.outputMode >= 2.5f && p.outputMode < 3.5f) mode = ivanna::OmegaOutputMode::GENEZI;
        s.temporal_field = (s.temporal_field & ~ivanna::OMEGA_OUTPUT_MODE_MASK) |
                           ivanna::omega_output_mode_encode(mode);
    }

    if (!std::isnan(p.headYawRad)) s.head_yaw_rad = std::clamp(p.headYawRad, -3.2f, 3.2f);
    if (!std::isnan(p.headPitchRad)) s.head_pitch_rad = std::clamp(p.headPitchRad, -1.6f, 1.6f);
    if (!std::isnan(p.sceneDistance)) {
        // No existe s.scene_distance: la distancia vive como word en
        // npe_ctl bits 16..31 (omega_scene_distance_encode; 0 = sin
        // distancia). Se conserva el rango del servidor viejo (0.5..2.0).
        const float dist = std::clamp(p.sceneDistance, 0.5f, 2.f);
        const uint32_t dw = ivanna::omega_scene_distance_encode(dist);
        if (dw != 0u) s.npe_ctl = (s.npe_ctl & 0x0000FFFFu) | dw;
    }

    if (!std::isnan(p.safDeltaEnergy)) s.saf_delta_energy = p.safDeltaEnergy;
    if (!std::isnan(p.safMetricNorm)) s.saf_metric_norm = std::clamp(p.safMetricNorm, 0.f, 1.f);
    if (!std::isnan(p.safMemory)) s.saf_memory = std::clamp(p.safMemory, 0.f, 1.f);
    if (!std::isnan(p.safGain)) s.saf_gain = std::clamp(p.safGain, -18.f, 18.f);
    if (p.hasSafQ) {
        for (int i = 0; i < 7; ++i) s.saf_q[i] = std::clamp(p.safQ[i], 0.f, 1.f);
    }
    if (p.hasRoomCorr) {
        // 0044 (coord.): cut-only de Fase 4 (RoomProbe.hpp: kRoomCorrMinDb
        // -12, kRoomCorrMaxDb 0) — nunca realzar; Gemini proponía [-18, 0].
        for (int i = 0; i < 10; ++i) {
            s.room_corr_db[i] = std::clamp(p.roomCorrDb[i], -12.f, 0.f);
        }
    }
    if (p.hasEqBands) {
        for (int i = 0; i < 10; ++i) s.eq_gains[i] = std::clamp(p.eqGains[i], -15.f, 15.f);
        s.listen_phon = std::isnan(p.listenPhon) ? 60.f : std::clamp(p.listenPhon, 20.f, 90.f);
        s.ref_phon = std::isnan(p.refPhon) ? 80.f : std::clamp(p.refPhon, 20.f, 90.f);
        s.flags |= ivanna::OMEGA_FLAG_EQ_CALIBRATED;
    }

    if (p.userFlagsMask != 0u) {
        s.flags = (s.flags & ~p.userFlagsMask) | (p.userFlagsValue & p.userFlagsMask);
    }

    if (p.hasPfParams) {
        for (int i = 0; i < 13; ++i) s.pf_params[i] = p.pfParams[i];
        if (s.pf_params[3] != 0.f || s.pf_params[4] != 0.f) {
            s.compressor = std::min(-0.5f, -24.f + s.pf_params[3] * 24.f);
            s.comp_amount = std::min(1.f, std::max(0.f, s.pf_params[4] * 19.f / 7.f));
        }
    }
    // PF de un solo slot (texto SET_PF_<NOMBRE>:<v>): se aplica DESPUÉS
    // del array completo. Rangos del servidor viejo (command_server.cpp).
    if (p.pfSingleIdx >= 0 && p.pfSingleIdx < 13 && !std::isnan(p.pfSingleValue)) {
        static const float kPfLo[13] = {0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 20.f, 0.1f, -18.f, -18.f, -18.f, -18.f, -18.f};
        static const float kPfHi[13] = {4.f, 1.f, 1.f, 1.f, 1.f, 1.f, 20000.f, 10.f, 18.f, 18.f, 18.f, 18.f, 18.f};
        s.pf_params[p.pfSingleIdx] = std::clamp(p.pfSingleValue, kPfLo[p.pfSingleIdx], kPfHi[p.pfSingleIdx]);
        if (p.pfSingleIdx == 3) s.compressor = std::min(-0.5f, -24.f + s.pf_params[3] * 24.f);
        if (p.pfSingleIdx == 4) s.comp_amount = std::min(1.f, std::max(0.f, s.pf_params[4] * 19.f / 7.f));
    }
}

}  // namespace ivanna::daemon
