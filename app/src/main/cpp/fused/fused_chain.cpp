// fused_chain.cpp — FUSIÓN MAGISTRAL: la cadena única de los 7 operadores.
// Núcleo C++ compartido (Ruta A y Ruta B). Ver fused_chain.hpp.
#include "fused/fused_chain.hpp"
#include "fused/ivanna_apex_kernel.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace ivanna::fused {

void FusedChain::prepare(double sampleRate) noexcept {
    sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
    brain_.prepare(sampleRate_);
    spatial_.prepare(sampleRate_);
    field_.prepare(sampleRate_);
    listenerRoom_.prepare(sampleRate_);
    transducer_.prepare(sampleRate_);
    tonal_.prepare(sampleRate_);
    dynamics_.prepare(sampleRate_);
    // 8. Conservación perceptual: banco de medición por banda.
    conservation_.prepare(sampleRate_);
    scratchL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    scratchR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    // Tronidos 493: seco de entrada por operador para el fundido de
    // enganche (wet). Mismo tamaño que el scratch del puente float.
    wetDryL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    wetDryR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    // Tronidos 493: seco de entrada de la cadena para el wet de cadena
    // (cubre el APEX). Mismo tamaño.
    chainDryL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    chainDryR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    // IVANNA-APEX: splitter + líneas de retardo + scratch de trans/sust.
    apexSplitter_.prepare(sampleRate_);
    apexDelayL_.reset();
    apexDelayR_.reset();
    apexTransL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    apexTransR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    apexSustL_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    apexSustR_.assign(static_cast<std::size_t>(kFusedMaxBlockFrames), 0.0);
    // IVANNA-APEX: matriz volumétrica (W por bin + WOLA).
    prepareVolumetric();
    // TΩ (F1/472): kernel 2×2 64-bit (compilador + pipeline hop 128).
    prepareTOmega();
    // CORONA D10 (2026-11-25): motor D10 ENABLED por defecto (orden
    // "todo prendido"). Distancia de escucha viva 3.0 m — la posición
    // documentada de la sala de Esse (NO el 1.85 m inventado de Gemini).
    d10Engine_.prepare(sampleRate_);
    {
        d10::D10Config d10Cfg;
        d10Cfg.listenerDistanceM = 3.0;
        d10Engine_.setConfig(d10Cfg);  // resto de defaults del header
    }
    d10TelemetryValid_ = false;
    // Línea de retardo del seco D10 (fix-2 #1434): a cero + puntero a 0.
    d10DryDelayL_.fill(0.0);
    d10DryDelayR_.fill(0.0);
    d10DryDelayWp_ = 0;
}

void FusedChain::setControls(const FusedControls& c) noexcept {
    FusedControls nc = c;
    nc.clamp();
    // S12/510: un push explícito nuevo supersede cualquier rampa en curso
    // (gana el último: el motor termina en el objetivo más reciente, nunca
    // queda a medias en valores intermedios).
    cancelParamRamp();
    // ── TRONIDOS 493 — fundido de modo (2026-11-22) ─────────────────────
    // Un cambio de modo o de enganche de TΩ no se aplica de golpe: aplicar
    // SPEAKERS con la FIFO de TΩ vacía dejaba 2 bloques de 0 exacto, y
    // aplicar el modo nuevo con la ganancia en 1.0 mete el escalón en la
    // frontera (la salida TΩ es la señal retrasada ~512 muestras y
    // filtrada por la XTC: distinta de la directa que sonaba). Se deja
    // PENDIENTE: la cadena sigue con los controles viejos mientras la
    // ganancia baja 1→0 (256 muestras); al llegar a 0, process() aplica
    // el pendiente y la ganancia sube 0→1 (512 muestras). Un cambio de
    // parámetros SIN cambio de modo ni de enganche de TΩ se aplica de
    // inmediato, como siempre (el escalón ahí lo cubre el wet por
    // operador, no el fundido de modo).
    // Si ya hay un fundido en curso, el pendiente se reemplaza (gana el
    // último) y el fundido sigue desde la ganancia vigente: nunca se
    // reinicia a 1.0 (eso sería un escalón hacia arriba).
    const bool oldTOmega = tOmegaActive();
    const OmegaOutputMode oldMode = userControls_.spatial.mode;
    // TΩ: trayecto de altavoz = bocina plana o Genezi (evol-holos).
    const bool newTOmega = volumetricEnabled_.load(std::memory_order_relaxed) &&
                           !nc.isNeutral() &&
                           ivanna::omega_output_is_speakers_like(nc.spatial.mode);
    // EXCEPCIÓN (bit-exactitud en neutro): si AMBOS estados son neutros,
    // el modo no cambia nada audible (passthrough en ambos; TΩ exige
    // !neutral) y NO se difiere: aplicar de inmediato preserva la
    // bit-exactitud (TOmegaNeutralBitExactSpeakers) y que el gemelo vea
    // el modo en el mismo bloque (NeutralSpeakersNoControl). El fundido
    // solo existe para cambios que suenan.
    const bool eitherEngaged = !nc.isNeutral() || !userControls_.isNeutral();
    if ((oldTOmega != newTOmega || oldMode != nc.spatial.mode) && eitherEngaged) {
        modeFadePending_ = nc;
        modeFadePendingValid_ = true;
        if (modeFadePhase_ != ModeFadePhase::OUT) modeFadePhase_ = ModeFadePhase::OUT;
        if (!std::isfinite(modeFadeGain_)) modeFadeGain_ = 1.0;
        return;
    }
    userControls_ = nc;
    distributeControls();
}

// Reparte userControls_ a los 7 operadores (sin detección de modo).
void FusedChain::distributeControls() noexcept {
    brain_.setControls(userControls_.brain);
    spatial_.setControls(userControls_.spatial);
    // Puerta Genezi (Fallo 3a, PARCHE v2.6): el modo de HOLOS·Ω vive en
    // FieldControls.mode y sigue a spatial.mode. Se fija aquí antes de
    // repartir, así field_.setControls() ya lleva el modo correcto.
    userControls_.field.mode = userControls_.spatial.mode;
    field_.setControls(userControls_.field);
    listenerRoom_.setControls(userControls_.listenerRoom);
    transducer_.setControls(userControls_.transducer);
    tonal_.setControls(userControls_.tonal);
    dynamics_.setControls(userControls_.dynamics);
}

// ── S12/510 — FUNDIDO DE APLICACIÓN POR HOP (SLEW) ───────────────────────
// Interpolación lineal de UN campo continuo entre extremos clampados.
// t ∈ [0, 1]; con t == 1 el resultado es EXACTAMENTE `b` (sin deriva).
static float lerpParam(float a, float b, double t) noexcept {
    if (t >= 1.0) return b;
    if (t <= 0.0) return a;
    return static_cast<float>(static_cast<double>(a) +
                              (static_cast<double>(b) - static_cast<double>(a)) * t);
}

// Interpola TODOS los campos continuos de FusedControls; los discretos
// (modo, curva, presets, bools) se copian de `from` y cambian con el snap
// final a `to` (nunca a medias). El clamp() final aplica por paso las
// mismas puertas fail-closed que setControls (puerta HRTF, MANUAL→
// adaptation 0): la rampa nunca deja la cadena en un estado que un push
// directo no pudiera dejar.
static FusedControls lerpControls(const FusedControls& from,
                                  const FusedControls& to, double t) noexcept {
    FusedControls out = from;
    out.spatial.amount = lerpParam(from.spatial.amount, to.spatial.amount, t);
    out.spatial.azimuthDeg = lerpParam(from.spatial.azimuthDeg, to.spatial.azimuthDeg, t);
    out.spatial.distanceM = lerpParam(from.spatial.distanceM, to.spatial.distanceM, t);
    out.spatial.width = lerpParam(from.spatial.width, to.spatial.width, t);
    out.spatial.wfsSpread = lerpParam(from.spatial.wfsSpread, to.spatial.wfsSpread, t);
    out.spatial.hrtfWet = lerpParam(from.spatial.hrtfWet, to.spatial.hrtfWet, t);
    out.spatial.upmixImmersivity =
        lerpParam(from.spatial.upmixImmersivity, to.spatial.upmixImmersivity, t);
    out.spatial.parallaxAmount =
        lerpParam(from.spatial.parallaxAmount, to.spatial.parallaxAmount, t);
    out.field.amount = lerpParam(from.field.amount, to.field.amount, t);
    out.field.phiAmount = lerpParam(from.field.phiAmount, to.field.phiAmount, t);
    out.field.pumping = lerpParam(from.field.pumping, to.field.pumping, t);
    out.field.coherence = lerpParam(from.field.coherence, to.field.coherence, t);
    out.field.harmonics = lerpParam(from.field.harmonics, to.field.harmonics, t);
    out.field.gamma = lerpParam(from.field.gamma, to.field.gamma, t);
    out.field.warmth = lerpParam(from.field.warmth, to.field.warmth, t);
    out.field.dynamics = lerpParam(from.field.dynamics, to.field.dynamics, t);
    out.field.roomRt60 = lerpParam(from.field.roomRt60, to.field.roomRt60, t);
    out.field.omniAmount = lerpParam(from.field.omniAmount, to.field.omniAmount, t);
    out.field.diffuse = lerpParam(from.field.diffuse, to.field.diffuse, t);
    out.listenerRoom.rt60Sec = lerpParam(from.listenerRoom.rt60Sec, to.listenerRoom.rt60Sec, t);
    out.listenerRoom.roomSizeM =
        lerpParam(from.listenerRoom.roomSizeM, to.listenerRoom.roomSizeM, t);
    out.listenerRoom.roomWet = lerpParam(from.listenerRoom.roomWet, to.listenerRoom.roomWet, t);
    for (int i = 0; i < 10; ++i) {
        out.listenerRoom.roomCorrDb[i] =
            lerpParam(from.listenerRoom.roomCorrDb[i], to.listenerRoom.roomCorrDb[i], t);
    }
    out.transducer.correction =
        lerpParam(from.transducer.correction, to.transducer.correction, t);
    out.transducer.temporalAmount =
        lerpParam(from.transducer.temporalAmount, to.transducer.temporalAmount, t);
    out.tonal.trimDb = lerpParam(from.tonal.trimDb, to.tonal.trimDb, t);
    out.tonal.lowDb = lerpParam(from.tonal.lowDb, to.tonal.lowDb, t);
    out.tonal.midDb = lerpParam(from.tonal.midDb, to.tonal.midDb, t);
    out.tonal.highDb = lerpParam(from.tonal.highDb, to.tonal.highDb, t);
    out.tonal.presenceDb = lerpParam(from.tonal.presenceDb, to.tonal.presenceDb, t);
    out.tonal.amount = lerpParam(from.tonal.amount, to.tonal.amount, t);
    out.dynamics.punch = lerpParam(from.dynamics.punch, to.dynamics.punch, t);
    out.dynamics.width = lerpParam(from.dynamics.width, to.dynamics.width, t);
    out.dynamics.safety = lerpParam(from.dynamics.safety, to.dynamics.safety, t);
    out.dynamics.compThresholdDb =
        lerpParam(from.dynamics.compThresholdDb, to.dynamics.compThresholdDb, t);
    out.dynamics.compRatio = lerpParam(from.dynamics.compRatio, to.dynamics.compRatio, t);
    out.dynamics.exciter = lerpParam(from.dynamics.exciter, to.dynamics.exciter, t);
    out.dynamics.voice = lerpParam(from.dynamics.voice, to.dynamics.voice, t);
    out.dynamics.upmixImmersivity =
        lerpParam(from.dynamics.upmixImmersivity, to.dynamics.upmixImmersivity, t);
    out.brain.adaptation = lerpParam(from.brain.adaptation, to.brain.adaptation, t);
    out.conservation.amount =
        lerpParam(from.conservation.amount, to.conservation.amount, t);
    out.conservation.energyCap =
        lerpParam(from.conservation.energyCap, to.conservation.energyCap, t);
    out.conservation.phaseCoherence =
        lerpParam(from.conservation.phaseCoherence, to.conservation.phaseCoherence, t);
    out.conservation.inertia =
        lerpParam(from.conservation.inertia, to.conservation.inertia, t);
    out.clamp();
    return out;
}

int FusedChain::rampHopsFor(const FusedControls& from, const FusedControls& to) noexcept {
    // Regla conservadora §S12: ≤0.5 dB por hop sobre los parámetros en dB.
    float maxDbDelta = 0.0f;
    const float dbDeltas[] = {
        std::fabs(to.tonal.trimDb - from.tonal.trimDb),
        std::fabs(to.tonal.lowDb - from.tonal.lowDb),
        std::fabs(to.tonal.midDb - from.tonal.midDb),
        std::fabs(to.tonal.highDb - from.tonal.highDb),
        std::fabs(to.tonal.presenceDb - from.tonal.presenceDb),
        std::fabs(to.dynamics.compThresholdDb - from.dynamics.compThresholdDb),
    };
    for (float d : dbDeltas) maxDbDelta = std::max(maxDbDelta, d);
    for (int i = 0; i < 10; ++i) {
        maxDbDelta = std::max(
            maxDbDelta,
            std::fabs(to.listenerRoom.roomCorrDb[i] - from.listenerRoom.roomCorrDb[i]));
    }
    if (maxDbDelta > 0.0f) {
        const int hops = static_cast<int>(
            std::ceil(static_cast<double>(maxDbDelta) / kParamRampMaxDbPerHop));
        return std::clamp(hops, 1, kParamRampMaxHops);
    }
    // Sin cambio en dB: si algo continuo cambió, el precedente Volterra
    // (~18 ms); si nada cambió, no hace falta rampa.
    FusedControls probe = lerpControls(from, to, 1.0);
    const bool anyChange =
        probe.spatial.amount != from.spatial.amount ||
        probe.spatial.azimuthDeg != from.spatial.azimuthDeg ||
        probe.spatial.distanceM != from.spatial.distanceM ||
        probe.spatial.width != from.spatial.width ||
        probe.spatial.wfsSpread != from.spatial.wfsSpread ||
        probe.spatial.hrtfWet != from.spatial.hrtfWet ||
        probe.spatial.upmixImmersivity != from.spatial.upmixImmersivity ||
        probe.spatial.parallaxAmount != from.spatial.parallaxAmount ||
        probe.field.amount != from.field.amount ||
        probe.field.phiAmount != from.field.phiAmount ||
        probe.field.pumping != from.field.pumping ||
        probe.field.coherence != from.field.coherence ||
        probe.field.harmonics != from.field.harmonics ||
        probe.field.gamma != from.field.gamma ||
        probe.field.warmth != from.field.warmth ||
        probe.field.dynamics != from.field.dynamics ||
        probe.field.roomRt60 != from.field.roomRt60 ||
        probe.field.omniAmount != from.field.omniAmount ||
        probe.field.diffuse != from.field.diffuse ||
        probe.listenerRoom.rt60Sec != from.listenerRoom.rt60Sec ||
        probe.listenerRoom.roomSizeM != from.listenerRoom.roomSizeM ||
        probe.listenerRoom.roomWet != from.listenerRoom.roomWet ||
        probe.transducer.correction != from.transducer.correction ||
        probe.transducer.temporalAmount != from.transducer.temporalAmount ||
        probe.tonal.amount != from.tonal.amount ||
        probe.dynamics.punch != from.dynamics.punch ||
        probe.dynamics.width != from.dynamics.width ||
        probe.dynamics.safety != from.dynamics.safety ||
        probe.dynamics.compRatio != from.dynamics.compRatio ||
        probe.dynamics.exciter != from.dynamics.exciter ||
        probe.dynamics.voice != from.dynamics.voice ||
        probe.dynamics.upmixImmersivity != from.dynamics.upmixImmersivity ||
        probe.brain.adaptation != from.brain.adaptation ||
        probe.conservation.amount != from.conservation.amount ||
        probe.conservation.energyCap != from.conservation.energyCap ||
        probe.conservation.phaseCoherence != from.conservation.phaseCoherence ||
        probe.conservation.inertia != from.conservation.inertia;
    // Los campos discretos (modo, curva, presets, bools) cambian con el
    // snap final; si SOLO cambió uno de ellos, la rampa no tiene nada que
    // interpolar — se aplica de inmediato (hops 0).
    return anyChange ? kParamRampDefaultHops : 0;
}

void FusedChain::setControlsRamped(const FusedControls& target, int hops) noexcept {
    FusedControls nc = target;
    nc.clamp();
    // Longitud 0 (o negativa) = comportamiento anterior exacto: aplicación
    // inmediata, sin fundido. Es el camino de sliders / re-push.
    if (hops <= 0) {
        setControls(nc);
        return;
    }
    // Un cambio de modo o de enganche de TΩ NO se rampea: lo cubre el
    // fundido de modo existente (misma condición que setControls).
    const bool oldTOmega = tOmegaActive();
    const OmegaOutputMode oldMode = userControls_.spatial.mode;
    const bool newTOmega = volumetricEnabled_.load(std::memory_order_relaxed) &&
                           !nc.isNeutral() &&
                           ivanna::omega_output_is_speakers_like(nc.spatial.mode);
    const bool eitherEngaged = !nc.isNeutral() || !userControls_.isNeutral();
    if ((oldTOmega != newTOmega || oldMode != nc.spatial.mode) && eitherEngaged) {
        setControls(nc);
        return;
    }
    // Nunca apilar una rampa sobre un fundido de modo en curso: el
    // pendiente del fundido reescribe userControls_ en ganancia 0 y la
    // rampa quedaría con `from`/`to` obsoletos.
    if (modeFadePhase_ != ModeFadePhase::DONE) {
        setControls(nc);
        return;
    }
    // Re-base: `from` es el valor interpolado VIGENTE (si había una rampa
    // en curso, la nueva es monótona desde donde el motor está de verdad).
    paramRampFrom_ = userControls_;
    paramRampTo_ = nc;
    paramRampTotalHops_ = std::clamp(hops, 1, kParamRampMaxHops);
    paramRampHopsDone_ = 0;
    paramRampHopPos_ = 0;
}

void FusedChain::cancelParamRamp() noexcept {
    paramRampTotalHops_ = 0;
    paramRampHopsDone_ = 0;
    paramRampHopPos_ = 0;
}

void FusedChain::advanceParamRampHop() noexcept {
    if (paramRampTotalHops_ <= 0) return;
    ++paramRampHopsDone_;
    if (paramRampHopsDone_ >= paramRampTotalHops_) {
        // Snap final EXACTO al objetivo (incluye los campos discretos):
        // sin deriva float, sin quedar a medias.
        userControls_ = paramRampTo_;
        cancelParamRamp();
        return;
    }
    const double t = static_cast<double>(paramRampHopsDone_) /
                     static_cast<double>(paramRampTotalHops_);
    userControls_ = lerpControls(paramRampFrom_, paramRampTo_, t);
}

void FusedChain::setNeutral() noexcept {
    userControls_.setNeutral();
    setControls(userControls_);
}

void FusedChain::reset() noexcept {
    brain_.reset();
    spatial_.reset();
    field_.reset();
    listenerRoom_.reset();
    transducer_.reset();
    tonal_.reset();
    dynamics_.reset();
    conservation_.reset();  // 8. Conservación: banco + estado de campo a cero.
    // Gemelo + calibración P3: forzar el refresh en el próximo bloque
    // (la escala vuelve a calcularse desde el modo vivo).
    twinCalDirty_.store(true, std::memory_order_release);
    twinCalReady_ = false;
    twinEstimate_ = acoustic_twin::AcousticTwinEstimate{};
    chainTime_ = 0.0;       // el tiempo perceptual vuelve a cero con el reset.
    stats_ = FusedChainStats{};
    telemetry_ = FusedTelemetry{};
    // IVANNA-APEX: el splitter y las líneas vuelven a cero (sin allocs).
    apexSplitter_.reset();
    apexDelayL_.reset();
    apexDelayR_.reset();
    // IVANNA-APEX: la matriz volumétrica vacía su pipeline (sin allocs).
    resetVolumetric();
    // TΩ (F1/472): la etapa espectral vacía su pipeline y fuerza
    // recompilar con el snapshot vivo en el próximo process().
    resetTOmega();
    // CORONA D10: el motor vuelve a su estado inicial (observador
    // térmico, slew, telemetría). El kernel LTI CONSERVA sus FIR
    // compiladas y su enganche (reset() del header no los borra): el
    // próximo compileTOmega re-compila y re-hace el morph igualmente.
    // La telemetría deja de ser válida hasta el próximo bloque arbitrado.
    d10Engine_.reset();
    d10TelemetryValid_ = false;
    // Línea de retardo del seco D10 (fix-2 #1434): a cero + puntero a 0.
    d10DryDelayL_.fill(0.0);
    d10DryDelayR_.fill(0.0);
    d10DryDelayWp_ = 0;
    // Tronidos 493: el fundido de modo no sobrevive a un reset (un
    // pendiente viejo no debe aplicarse tras reiniciar) y los wets se
    // fijan al objetivo vigente (los controles no los toca reset(), así
    // que el arranque tras reset() suena como antes, sin fundido extra).
    modeFadePhase_ = ModeFadePhase::DONE;
    modeFadeGain_ = 1.0;
    modeFadePendingValid_ = false;
    cancelParamRamp();  // S12/510: sin rampa pendiente tras reset()
    d10DisableFadePending_ = false;  // 6.4: sin fundido de D10 pendiente
    d10EnabledSeen_ = d10Engine_.isEnabled();  // 6.4: re-sincronizar
    opWet_[0] = brain_.isNeutral() ? 0.0 : 1.0;
    opWet_[1] = spatial_.isNeutral() ? 0.0 : 1.0;
    opWet_[2] = field_.isNeutral() ? 0.0 : 1.0;
    opWet_[3] = listenerRoom_.isNeutral() ? 0.0 : 1.0;
    opWet_[4] = transducer_.isNeutral() ? 0.0 : 1.0;
    opWet_[5] = tonal_.isNeutral() ? 0.0 : 1.0;
    opWet_[6] = dynamics_.isNeutral() ? 0.0 : 1.0;
    chainWet_ = isNeutral() ? 0.0 : 1.0;
    // RE-SEED Γ: tras reset() no se pide (ver holosReseedDone_).
    holosReseedDone_ = true;
    field_.clearHolosReseedRequest();
}

void FusedChain::resetStats() noexcept { stats_ = FusedChainStats{}; }

// ── TRONIDOS 493 — fundido de enganche por operador + fundido de modo ────
// Núcleo de la mezcla wet: avanza `wet` por muestra hacia `target` con
// paso `step` y mezcla seco/procesado. Devuelve el wet nuevo. Fail-closed:
// paso inválido, wet no finito o seco nulo → devuelve el objetivo sin mezclar.
// `compensate` (2026-11-27, fix Tronidos, OPCIÓN A): AMBOS blends
// (por operador y de cadena) compensan por correlación. La potencia
// constante (cos/sin) es exacta con seco y procesado INCORRELADOS,
// pero con ρ≈1 da +3 dB de sobre-impulso a mitad de rampa; la ley
// compensada (gDry=cos θ·s, gWet=sin θ·s, s=1/√(1+ρ·sin 2θ),
// ρ ∈ [0, 1]) es exacta en potencia para cualquier ρ y nunca sube
// ganancias por encima de la potencia constante. Camino A (2026-11-27,
// decisión de Esse): 7 variantes medidas y ninguna ley de mezcla pasa
// Tronidos, dip y P8FlagOff a la vez; la Opción A pasa Tronidos
// (excess 0.55–0.70 dB) y el dip se re-baselinea a sus valores genuinos
// (ver test_evol_genezi.cpp::EngageDipFrequencySweep).
static double blendWetCore(double* l, double* r, const double* dryL,
                           const double* dryR, int frames, double wet,
                           double target, double step,
                           bool compensate) noexcept {
    if (!l || !r || frames <= 0) return target;
    if (!std::isfinite(wet)) return target;  // estado inválido: salto al objetivo
    if (wet == target) return target;         // régimen: nada que mezclar
    if (!(step > 0.0) || !std::isfinite(step)) return target;
    if (dryL == nullptr || dryR == nullptr) return target;
    // ── Correlación seco↔procesado del chunk (solo si compensate) ──
    // ρ se estima sobre el chunk (L+R) ANTES del bucle (el bucle
    // sobrescribe l/r). Silencio (energía ~0) → ρ=0 (potencia
    // constante). ρ se fija a [0, 1]: ρ<0 haría s>1 (ganancias por
    // encima de la potencia constante) y el pico del enganche se
    // dispara; la compensación solo REDUCE ganancias, jamás las sube.
    double rho = 0.0;
    if (compensate) {
        double eDry = 0.0, eWet = 0.0, cross = 0.0;
        for (int i = 0; i < frames; ++i) {
            const double dl = dryL[i], dr = dryR[i];
            const double wl = l[i], wr = r[i];
            eDry += dl * dl + dr * dr;
            eWet += wl * wl + wr * wr;
            cross += dl * wl + dr * wr;
        }
        const double denom = std::sqrt(eDry * eWet);
        if (denom > 1e-30 && std::isfinite(denom)) {
            rho = cross / denom;
            if (!std::isfinite(rho)) rho = 0.0;
            rho = std::clamp(rho, 0.0, 1.0);  // OPCIÓN A: solo correlación positiva
        }
    }
    for (int i = 0; i < frames; ++i) {
        if (target > wet) {
            wet += step;
            if (wet >= target) wet = target;
        } else {
            wet -= step;
            if (wet <= target) wet = target;
        }
        if (wet == 1.0) {
            // Wet completo: esta muestra y las que quedan son el
            // procesado tal cual (bit-exacto: no se toca el buffer).
            return 1.0;
        }
        if (wet == 0.0) {
            // Seco completo: esta muestra y las que quedan son el seco
            // guardado (copia exacta, sin aritmética).
            for (int j = i; j < frames; ++j) {
                l[j] = dryL[j];
                r[j] = dryR[j];
            }
            return 0.0;
        }
        // Mezcla de potencia constante, compensada por correlación si
        // `compensate` (Opción A: ambos blends): ρ≈0 → cos/sin puros
        // (sin dip por decorrelación); ρ≈1 → cuasi-lineal (sin
        // sobre-impulso +3 dB en contenido correlado). Sin compensar
        // ρ=0 y esto es la potencia constante de a83738f, bit-idéntica.
        // En wet == 1.0 / 0.0 los cortocircuitos de arriba ya
        // devolvieron; aquí wet está en (0, 1) estricto.
        const double theta = wet * (apex::kPi / 2.0);  // 0 → π/2
        const double sin2 = std::sin(2.0 * theta);     // 0 → 1 → 0
        // Con ρ ∈ [0, 1] el denominador ∈ [1, 2] (s ∈ [0.707, 1]). El
        // piso 0.25 es red de seguridad (nunca se alcanza con ρ ≥ 0).
        double sDen = 1.0 + rho * sin2;
        if (sDen < 0.25) sDen = 0.25;
        const double s = 1.0 / std::sqrt(sDen);
        const double gDry = std::cos(theta) * s;       // 1 → 0
        const double gWet = std::sin(theta) * s;       // 0 → 1
        l[i] = gDry * dryL[i] + gWet * l[i];
        r[i] = gDry * dryR[i] + gWet * r[i];
    }
    return wet;
}

double FusedChain::wetRampStep() const noexcept {
    // 30 ms a la fs vigente (ver kOpWetRampSeconds). fs inválida → 0.0:
    // el blend salta al objetivo sin mezclar (fail-closed, asignación directa).
    if (!(sampleRate_ > 0.0) || !std::isfinite(sampleRate_)) return 0.0;
    return 1.0 / (kOpWetRampSeconds * sampleRate_);
}

void FusedChain::blendOperatorWet(double* l, double* r, const double* dryL,
                                  const double* dryR, int frames, int op,
                                  bool targetActive) noexcept {
    if (!l || !r || frames <= 0 || op < 0 || op >= kNumOperators) return;
    const double target = targetActive ? 1.0 : 0.0;
    opWet_[static_cast<std::size_t>(op)] = blendWetCore(
        l, r, dryL, dryR, frames, opWet_[static_cast<std::size_t>(op)], target,
        wetRampStep(), true);  // OPCIÓN A: operador compensado (ρ∈[0,1])
}

void FusedChain::warmTOmegaHistory(const double* l, const double* r,
                                   int frames) noexcept {
    if (!l || !r || frames <= 0) return;
    if (tOmegaHistL_.size() < static_cast<std::size_t>(kVolFft)) return;
    for (int n = 0; n < frames; ++n) {
        tOmegaHistL_[static_cast<std::size_t>(tOmegaHistWp_)] =
            std::isfinite(l[n]) ? l[n] : 0.0;
        tOmegaHistR_[static_cast<std::size_t>(tOmegaHistWp_)] =
            std::isfinite(r[n]) ? r[n] : 0.0;
        tOmegaHistWp_ = (tOmegaHistWp_ + 1) % kVolFft;
        ++tOmegaInTotal_;
        // Sin calcular frames: solo se adelanta el horario para que el
        // próximo frame caiga cerca del presente (el prime del pendiente
        // lo fuerza a la siguiente muestra al enganchar).
        if (tOmegaInTotal_ == tOmegaNextFrameAt_) tOmegaNextFrameAt_ += kTOmegaHop;
    }
}

void FusedChain::applyModeFadePending() noexcept {
    // 6.4: el fundido de salida por apagado del D10 termina aquí (ganancia
    // 0): la rama TΩ ya puede cambiar a la ruta espectral — su WOLA se
    // rellena durante el fundido de entrada.
    d10DisableFadePending_ = false;
    if (!modeFadePendingValid_) {
        modeFadePhase_ = ModeFadePhase::IN;
        return;
    }
    // S12/510: el pendiente reescribe userControls_ en ganancia 0; una
    // rampa en curso quedaría con `from`/`to` obsoletos (nunca a medias:
    // el motor termina en el pendiente, no en un intermedio de rampa).
    cancelParamRamp();
    userControls_ = modeFadePending_;
    modeFadePendingValid_ = false;
    distributeControls();
    // TΩ se engancha con historia caliente: forzar un frame en la
    // siguiente muestra (sin esto, la FIFO tardaría 512 muestras en
    // llenarse y la salida sería 0 exacto 2 bloques).
    if (tOmegaActive()) tOmegaNextFrameAt_ = tOmegaInTotal_ + 1;
    modeFadePhase_ = ModeFadePhase::IN;
}

// ── IVANNA-APEX: matriz volumétrica (XTC Genezi) ─────────────────────────
// STFT WOLA (N=512, hop=256, Hann solo en análisis). W precalculada por
// bin (spec Genezi). Latencia ~512 muestras solo cuando está activa.
void FusedChain::prepareVolumetric() noexcept {
    volSpec_ = apex::VolumetricSpeakerSpec{};
    for (int n = 0; n < kVolFft; ++n) {
        const double ph = 2.0 * apex::kPi * static_cast<double>(n) / static_cast<double>(kVolFft);
        volWin_[static_cast<std::size_t>(n)] = 0.5 - 0.5 * std::cos(ph);
    }
    for (int k = 0; k < kVolBins; ++k) {
        const double freq = static_cast<double>(k) * sampleRate_ / static_cast<double>(kVolFft);
        volW_[static_cast<std::size_t>(k)] =
            apex::computeVolumetricSpeakersMatrixBin(freq, volSpec_, apexWs_.sphereWs);
    }
    volHistL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    volHistR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    volOlaL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    volOlaR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    const std::size_t fifoCap = static_cast<std::size_t>(kVolFft + kFusedMaxBlockFrames);
    volOutL_.assign(fifoCap, 0.0);
    volOutR_.assign(fifoCap, 0.0);
    resetVolumetric();
}

void FusedChain::resetVolumetric() noexcept {
    std::fill(volHistL_.begin(), volHistL_.end(), 0.0);
    std::fill(volHistR_.begin(), volHistR_.end(), 0.0);
    std::fill(volOlaL_.begin(), volOlaL_.end(), 0.0);
    std::fill(volOlaR_.begin(), volOlaR_.end(), 0.0);
    std::fill(volOutL_.begin(), volOutL_.end(), 0.0);
    std::fill(volOutR_.begin(), volOutR_.end(), 0.0);
    volHistWp_ = 0;
    volInTotal_ = 0;
    volNextFrameAt_ = kVolFft;
    volOutWp_ = 0;
    volOutRp_ = 0;
    volOutLen_ = 0;
}

// LEGACY (F1/472): la matriz volumétrica WOLA ya no se llama — la etapa
// la implementa TΩ (compileTOmega + applyTOmega, mismo rol XTC, un solo
// kernel 2×2 64-bit). Se conserva íntegra (nunca se borra código).
void FusedChain::applyVolumetric(double* l, double* r, int frames) noexcept {
    if (!l || !r || frames <= 0) return;
    // Fail-closed: sin buffers (prepare no corrido), passthrough.
    if (volHistL_.size() < static_cast<std::size_t>(kVolFft) || volOutL_.empty()) return;
    const std::size_t fifoCap = volOutL_.size();
    const int fifoCapI = static_cast<int>(fifoCap);
    for (int n = 0; n < frames; ++n) {
        volHistL_[static_cast<std::size_t>(volHistWp_)] = std::isfinite(l[n]) ? l[n] : 0.0;
        volHistR_[static_cast<std::size_t>(volHistWp_)] = std::isfinite(r[n]) ? r[n] : 0.0;
        volHistWp_ = (volHistWp_ + 1) % kVolFft;
        ++volInTotal_;
        if (volInTotal_ != volNextFrameAt_) continue;
        volNextFrameAt_ += kVolHop;
        // Frame: últimos N muestras (en orden), ventana Hann.
        for (int i = 0; i < kVolFft; ++i) {
            const int idx = (volHistWp_ + i) % kVolFft;
            const double w = volWin_[static_cast<std::size_t>(i)];
            volFftL_[static_cast<std::size_t>(i)] = {volHistL_[static_cast<std::size_t>(idx)] * w, 0.0};
            volFftR_[static_cast<std::size_t>(i)] = {volHistR_[static_cast<std::size_t>(idx)] * w, 0.0};
        }
        apex::fftDouble(volFftL_.data(), kVolFft, false);
        apex::fftDouble(volFftR_.data(), kVolFft, false);
        // W por bin + espejo hermítico (señal real).
        for (int k = 0; k < kVolBins; ++k) {
            const apex::Complex2x2& W = volW_[static_cast<std::size_t>(k)];
            const std::complex<double> xL = volFftL_[static_cast<std::size_t>(k)];
            const std::complex<double> xR = volFftR_[static_cast<std::size_t>(k)];
            const std::complex<double> yL = W.m00 * xL + W.m01 * xR;
            const std::complex<double> yR = W.m10 * xL + W.m11 * xR;
            volFftL_[static_cast<std::size_t>(k)] = yL;
            volFftR_[static_cast<std::size_t>(k)] = yR;
            if (k > 0 && k < kVolFft / 2) {
                volFftL_[static_cast<std::size_t>(kVolFft - k)] = std::conj(yL);
                volFftR_[static_cast<std::size_t>(kVolFft - k)] = std::conj(yR);
            }
        }
        apex::fftDouble(volFftL_.data(), kVolFft, true);
        apex::fftDouble(volFftR_.data(), kVolFft, true);
        // Overlap-add al acumulador (sin ventana de síntesis; COLA Hann).
        for (int i = 0; i < kVolFft; ++i) {
            volOlaL_[static_cast<std::size_t>(i)] += volFftL_[static_cast<std::size_t>(i)].real();
            volOlaR_[static_cast<std::size_t>(i)] += volFftR_[static_cast<std::size_t>(i)].real();
        }
        // Emitir los primeros H del acumulador a la FIFO de salida.
        for (int i = 0; i < kVolHop; ++i) {
            if (volOutLen_ < fifoCapI) {
                volOutL_[static_cast<std::size_t>(volOutWp_)] = volOlaL_[static_cast<std::size_t>(i)];
                volOutR_[static_cast<std::size_t>(volOutWp_)] = volOlaR_[static_cast<std::size_t>(i)];
                volOutWp_ = (volOutWp_ + 1) % fifoCapI;
                ++volOutLen_;
            }
        }
        // Desplazar el acumulador H a la izquierda (cola a cero).
        for (int i = 0; i < kVolFft - kVolHop; ++i) {
            volOlaL_[static_cast<std::size_t>(i)] = volOlaL_[static_cast<std::size_t>(i + kVolHop)];
            volOlaR_[static_cast<std::size_t>(i)] = volOlaR_[static_cast<std::size_t>(i + kVolHop)];
        }
        for (int i = kVolFft - kVolHop; i < kVolFft; ++i) {
            volOlaL_[static_cast<std::size_t>(i)] = 0.0;
            volOlaR_[static_cast<std::size_t>(i)] = 0.0;
        }
    }
    // Sacar `frames` muestras de la FIFO (retrasadas ~N). Si la FIFO no
    // tiene suficientes (arranque), rellenar con ceros (transitorio ≤10.7 ms).
    for (int n = 0; n < frames; ++n) {
        if (volOutLen_ > 0) {
            l[n] = volOutL_[static_cast<std::size_t>(volOutRp_)];
            r[n] = volOutR_[static_cast<std::size_t>(volOutRp_)];
            volOutRp_ = (volOutRp_ + 1) % fifoCapI;
            --volOutLen_;
        } else {
            l[n] = 0.0;
            r[n] = 0.0;
        }
    }
}

// ── TΩ (F1/472): kernel 2×2 64-bit conectado a FusedChain ───────────────
// Reloj 2 (compile, double) + Reloj 1 (apply, hop 128 empaquetado L+jR).
// Ver fused_chain.hpp para el contrato completo (orden M[k], puertas,
// COLA, hermítica). La ventana Hann es la misma que la volumétrica
// (volWin_, N=512 periódica).
void FusedChain::prepareTOmega() noexcept {
    tOmegaHistL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    tOmegaHistR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    tOmegaOlaL_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    tOmegaOlaR_.assign(static_cast<std::size_t>(kVolFft), 0.0);
    const std::size_t fifoCap = static_cast<std::size_t>(kVolFft + kFusedMaxBlockFrames);
    tOmegaOutL_.assign(fifoCap, 0.0);
    tOmegaOutR_.assign(fifoCap, 0.0);
    tOmegaFdlL_.fill(std::complex<double>(0.0, 0.0));
    tOmegaFdlR_.fill(std::complex<double>(0.0, 0.0));
    tOmegaWs_.outMatrix.fill(apex::ComplexMatrix2x2{});
    tOmegaWs_.minPhaseBuf.fill(std::complex<double>(1.0, 0.0));
    resetTOmega();
}

void FusedChain::resetTOmega() noexcept {
    std::fill(tOmegaHistL_.begin(), tOmegaHistL_.end(), 0.0);
    std::fill(tOmegaHistR_.begin(), tOmegaHistR_.end(), 0.0);
    std::fill(tOmegaOlaL_.begin(), tOmegaOlaL_.end(), 0.0);
    std::fill(tOmegaOlaR_.begin(), tOmegaOlaR_.end(), 0.0);
    std::fill(tOmegaOutL_.begin(), tOmegaOutL_.end(), 0.0);
    std::fill(tOmegaOutR_.begin(), tOmegaOutR_.end(), 0.0);
    tOmegaHistWp_ = 0;
    tOmegaInTotal_ = 0;
    tOmegaNextFrameAt_ = kVolFft;
    tOmegaOutWp_ = 0;
    tOmegaOutRp_ = 0;
    tOmegaOutLen_ = 0;
    tOmegaCompiled_ = false;  // fuerza recompilar con el snapshot vivo
    // tOmegaCompiles_ NO se resetea: contador acumulado (telemetría).
}

void FusedChain::compileTOmega() noexcept {
    // Geometría Genezi (la misma spec que usaba la matriz volumétrica).
    apex::SpeakerPairPhysics sp;
    sp.spanM = volSpec_.spanM;
    sp.distanceM = volSpec_.distanceM;
    sp.headRadiusM = volSpec_.headRadiusM;
    sp.speedOfSoundMps = volSpec_.speedOfSoundMps;

    // T_Eq: magnitud neta PROPIA de TΩ, fase mínima cepstral en double.
    // En F1 es plana (0 dB): el tonal lo aplica ARMONÍA serial y
    // duplicarlo colorearía dos veces. El cepstrum corre igual (g_eq = 1
    // exacto) para que la estructura M = T_XTC · (g_eq·I) quede cableada.
    std::array<double, kVolBins> magDb{};
    magDb.fill(0.0);
    apex::buildMinimumPhaseSpectrum64(magDb.data(), kVolFft,
                                      tOmegaWs_.minPhaseBuf.data(), tOmegaWs_);

    const bool speakers = ivanna::omega_output_is_speakers_like(tOmegaSnapshot_.mode);
    for (int k = 0; k < kVolBins; ++k) {
        const double freq = static_cast<double>(k) * sampleRate_ / static_cast<double>(kVolFft);
        const std::complex<double> gEq =
            tOmegaWs_.minPhaseBuf[static_cast<std::size_t>(k)];
        // T_XTC: Kirkeby mono-renormalizado en SPEAKERS; identidad en
        // HEADPHONES. T_Transd = T_HOLOS/LGN = T_ORBE = I en F1 (ver hpp).
        const apex::ComplexMatrix2x2 xtc =
            speakers ? apex::computeSpeakersMatrixBin(freq, sp, tOmegaWs_.sphereWs)
                     : apex::ComplexMatrix2x2{};
        apex::ComplexMatrix2x2& M = tOmegaWs_.outMatrix[static_cast<std::size_t>(k)];
        M.m00 = xtc.m00 * gEq;
        M.m01 = xtc.m01 * gEq;
        M.m10 = xtc.m10 * gEq;
        M.m11 = xtc.m11 * gEq;
    }
    tOmegaCompiled_ = true;
    ++tOmegaCompiles_;
    // CORONA D10: compilar el kernel LTI desde la matriz recién
    // compilada (M[k] = outMatrix). Solo si el D10 está habilitado: con
    // D10 OFF el kernel no se engancha y la ruta espectral LEGACY sigue
    // siendo la que suena. El morph (512 muestras) arranca aquí.
    // ESTABILIDAD (verificado en process()): compileTOmega solo se llama
    // si cambió el snapshot (modo/fs/geometría Genezi) o tras reset; en
    // régimen la matriz NO cambia por bloque, así que el morph NO se
    // reinicia constantemente y el kernel LTI es correcto en régimen.
    if (d10Engine_.isEnabled()) {
        d10Engine_.ltiKernel().updateFromCompiledSpectrum(tOmegaWs_.outMatrix);
    }
}

void FusedChain::applyTOmega(double* l, double* r, int frames) noexcept {
    if (!l || !r || frames <= 0) return;
    // Fail-closed: sin buffers (prepare no corrido) o sin matriz
    // compilada, passthrough (no se aplica nada a ciegas).
    if (tOmegaHistL_.size() < static_cast<std::size_t>(kVolFft) || tOmegaOutL_.empty()) return;
    if (!tOmegaCompiled_) return;
    const std::size_t fifoCap = tOmegaOutL_.size();
    const int fifoCapI = static_cast<int>(fifoCap);
    for (int n = 0; n < frames; ++n) {
        tOmegaHistL_[static_cast<std::size_t>(tOmegaHistWp_)] =
            std::isfinite(l[n]) ? l[n] : 0.0;
        tOmegaHistR_[static_cast<std::size_t>(tOmegaHistWp_)] =
            std::isfinite(r[n]) ? r[n] : 0.0;
        tOmegaHistWp_ = (tOmegaHistWp_ + 1) % kVolFft;
        ++tOmegaInTotal_;
        if (tOmegaInTotal_ != tOmegaNextFrameAt_) continue;
        tOmegaNextFrameAt_ += kTOmegaHop;
        // Frame: últimos N muestras (en orden), ventana Hann, empaquetado
        // z = L + jR en UN solo buffer (1 FFT en vez de 2).
        for (int i = 0; i < kVolFft; ++i) {
            const int idx = (tOmegaHistWp_ + i) % kVolFft;
            const double w = volWin_[static_cast<std::size_t>(i)];
            tOmegaPack_[static_cast<std::size_t>(i)] = {
                tOmegaHistL_[static_cast<std::size_t>(idx)] * w,
                tOmegaHistR_[static_cast<std::size_t>(idx)] * w};
        }
        apex::fftRadix2Double(tOmegaPack_.data(), kVolFft, false);
        // Desempaquetar X_L/X_R (Z[k] = XL[k] + j·XR[k]):
        //   XL = (Z[k] + conj(Z[N-k])) / 2
        //   XR = (Z[k] - conj(Z[N-k])) / (2j) = -0.5j · (Z[k] - conj(Z[N-k]))
        // Aplicar Y[k] = M[k]·X[k] + T_FDL[k]; DC y Nyquist reales.
        for (int k = 0; k < kVolBins; ++k) {
            const std::complex<double> zk = tOmegaPack_[static_cast<std::size_t>(k)];
            const int nk = (k == 0) ? 0 : (kVolFft - k);
            const std::complex<double> znk = tOmegaPack_[static_cast<std::size_t>(nk)];
            const std::complex<double> diff = zk - std::conj(znk);
            const std::complex<double> xL = 0.5 * (zk + std::conj(znk));
            const std::complex<double> xR =
                std::complex<double>(0.0, -0.5) * diff;
            const apex::ComplexMatrix2x2& M =
                tOmegaWs_.outMatrix[static_cast<std::size_t>(k)];
            std::complex<double> yL = M.m00 * xL + M.m01 * xR +
                                      tOmegaFdlL_[static_cast<std::size_t>(k)];
            std::complex<double> yR = M.m10 * xL + M.m11 * xR +
                                      tOmegaFdlR_[static_cast<std::size_t>(k)];
            // DC (k=0) y Nyquist (k=256) de una señal real DEBEN ser
            // reales: se conserva la parte real (la imaginaria es espuria
            // para una señal real; mismo forzado que
            // applyUnifiedMatrix2x2, residual <1e-14 tras IFFT).
            if (k == 0 || k == kVolFft / 2) {
                yL = std::complex<double>(yL.real(), 0.0);
                yR = std::complex<double>(yR.real(), 0.0);
            }
            tOmegaYl_[static_cast<std::size_t>(k)] = yL;
            tOmegaYr_[static_cast<std::size_t>(k)] = yR;
        }
        // Reempaquetar W[k] = YL[k] + j·YR[k] (hermítico:
        // W[N-k] = conj(YL[k] - j·YR[k])) y UN solo IFFT.
        for (int k = 0; k < kVolBins; ++k) {
            const std::complex<double> yL = tOmegaYl_[static_cast<std::size_t>(k)];
            const std::complex<double> yR = tOmegaYr_[static_cast<std::size_t>(k)];
            tOmegaPack_[static_cast<std::size_t>(k)] = std::complex<double>(
                yL.real() - yR.imag(), yL.imag() + yR.real());
            if (k > 0 && k < kVolFft / 2) {
                const std::complex<double> d =
                    yL - std::complex<double>(0.0, 1.0) * yR;
                tOmegaPack_[static_cast<std::size_t>(kVolFft - k)] = std::conj(d);
            }
        }
        apex::fftRadix2Double(tOmegaPack_.data(), kVolFft, true);
        // Overlap-add al acumulador (Re = L, Im = R del IFFT empaquetado).
        for (int i = 0; i < kVolFft; ++i) {
            tOmegaOlaL_[static_cast<std::size_t>(i)] += tOmegaPack_[static_cast<std::size_t>(i)].real();
            tOmegaOlaR_[static_cast<std::size_t>(i)] += tOmegaPack_[static_cast<std::size_t>(i)].imag();
        }
        // Emitir los primeros H del acumulador a la FIFO de salida, con
        // ganancia COLA 0.5 (Hann análisis, hop 128, N=512: Σw = 2.0).
        for (int i = 0; i < kTOmegaHop; ++i) {
            if (tOmegaOutLen_ < fifoCapI) {
                tOmegaOutL_[static_cast<std::size_t>(tOmegaOutWp_)] =
                    0.5 * tOmegaOlaL_[static_cast<std::size_t>(i)];
                tOmegaOutR_[static_cast<std::size_t>(tOmegaOutWp_)] =
                    0.5 * tOmegaOlaR_[static_cast<std::size_t>(i)];
                tOmegaOutWp_ = (tOmegaOutWp_ + 1) % fifoCapI;
                ++tOmegaOutLen_;
            }
        }
        // Desplazar el acumulador H a la izquierda (cola a cero).
        for (int i = 0; i < kVolFft - kTOmegaHop; ++i) {
            tOmegaOlaL_[static_cast<std::size_t>(i)] =
                tOmegaOlaL_[static_cast<std::size_t>(i + kTOmegaHop)];
            tOmegaOlaR_[static_cast<std::size_t>(i)] =
                tOmegaOlaR_[static_cast<std::size_t>(i + kTOmegaHop)];
        }
        for (int i = kVolFft - kTOmegaHop; i < kVolFft; ++i) {
            tOmegaOlaL_[static_cast<std::size_t>(i)] = 0.0;
            tOmegaOlaR_[static_cast<std::size_t>(i)] = 0.0;
        }
    }
    // Sacar `frames` muestras de la FIFO (retrasadas ~N). Si la FIFO no
    // tiene suficientes (arranque), rellenar con ceros (transitorio ≤10.7 ms).
    for (int n = 0; n < frames; ++n) {
        if (tOmegaOutLen_ > 0) {
            l[n] = tOmegaOutL_[static_cast<std::size_t>(tOmegaOutRp_)];
            r[n] = tOmegaOutR_[static_cast<std::size_t>(tOmegaOutRp_)];
            tOmegaOutRp_ = (tOmegaOutRp_ + 1) % fifoCapI;
            --tOmegaOutLen_;
        } else {
            l[n] = 0.0;
            r[n] = 0.0;
        }
    }
}

void FusedChain::setRouteTag(const char* tag) noexcept {
    if (!tag) {
        routeTag_[0] = '\0';
        return;
    }
    std::size_t i = 0;
    for (; i + 1 < sizeof(routeTag_) && tag[i] != '\0'; ++i) routeTag_[i] = tag[i];
    routeTag_[i] = '\0';
}

void FusedChain::setTwinCalibration(
    const acoustic_twin::TwinCalibration& cal) noexcept {
    twinCalibrationOverride_ = cal;
    twinCalibrationHasOverride_.store(true, std::memory_order_release);
    twinCalDirty_.store(true, std::memory_order_release);
}

// ── CORONA P7–P10 — cableado (2026-11-24, rama corona-cableado) ──────────
// P7: solo lectura. El certificado se construye desde userControls_
// (controles efectivos vigentes); process() no lo llama.
total_conservation::PlaybackCertificate
FusedChain::totalConservationCertificate(
    total_conservation::Codec codec, bool decodeBitExact,
    std::uint64_t trackIndex, std::uint32_t ivannaVersionCode) const noexcept {
    total_conservation::StageInput in;
    in.codec = codec;
    in.decodeBitExact = decodeBitExact;
    // estimateDelta() solo distingue amount == 0 de amount > 0, así que
    // la presencia se codifica 0.0/1.0 con el predicado verdadero de cada
    // operador (isNeutral()), no con un escalar parcial: ORBE altera
    // también vía width/wfsSpread/xtc/upmix aunque amount sea 0, y PULSO
    // vía exciter/compresor aunque punch sea 0. Orden canónico: ORBE,
    // HOLOS, SALA VIVA, VOLTERRA, ARMONÍA, PULSO, NEURO.
    in.operatorAmounts = {
        userControls_.spatial.isNeutral() ? 0.0 : 1.0,
        userControls_.field.isNeutral() ? 0.0 : 1.0,
        userControls_.listenerRoom.isNeutral() ? 0.0 : 1.0,
        userControls_.transducer.isNeutral() ? 0.0 : 1.0,
        userControls_.tonal.isNeutral() ? 0.0 : 1.0,
        userControls_.dynamics.isNeutral() ? 0.0 : 1.0,
        userControls_.brain.isNeutral() ? 0.0 : 1.0,
    };
    in.conservationActive = !userControls_.conservation.isNeutral();
    total_conservation::PlaybackCertificate cert =
        total_conservation::buildCertificate(in, trackIndex,
                                             ivannaVersionCode);
    // P7 — Ξ_10 (CORONA D10): acción hamiltoniana MEDIDA del último
    // bloque arbitrado. Las 9 etapas y totalValid NO se tocan. Si el D10
    // está deshabilitado o aún no arbitró ningún bloque, el campo queda
    // sin estimar (hasTenthDimension=false, acción NaN por default del
    // struct) — nunca inventado.
    if (d10Engine_.isEnabled() && d10TelemetryValid_) {
        cert.hasTenthDimension = true;
        cert.tenthDimensionAction = d10Engine_.telemetry().hamiltonianAction;
    }
    return cert;
}

// P8: compilar es puro (no depende del flag); aplicar exige flag ON.
experience_compiler::CompileResult FusedChain::compileExperience(
    const experience_compiler::ExperienceVector& target,
    const experience_compiler::ReachableRegion& region) const noexcept {
    return experience_compiler::compile(target, region);
}

bool FusedChain::applyCompiledExperience(
    const experience_compiler::CompileResult& compiled) noexcept {
    if (!experienceCompilerEnabled_.load(std::memory_order_relaxed)) {
        return false;  // Flag OFF: no-op, la cadena no cambia.
    }
    if (!compiled.ok) return false;
    FusedControls nc = userControls_;
    nc.spatial.amount =
        static_cast<float>(compiled.amounts[0]);  // ORBE
    nc.field.amount = static_cast<float>(compiled.amounts[1]);  // HOLOS
    nc.listenerRoom.roomWet =
        static_cast<float>(compiled.amounts[2]);  // SALA VIVA
    nc.transducer.correction =
        static_cast<float>(compiled.amounts[3]);  // VOLTERRA
    // [4] ARMONÍA: SÍ SE APLICA (CORONA D10, 2026-11-25): el escalar
    // tonal continuo tonal.amount cierra ARMONÍA 7/7 en P8. Su ganancia
    // nominal (+0.06 dB/unidad) SÍ entra en el trim de lo aplicado: el
    // trim se recomputa sobre los 7 aplicados, así la ganancia neta
    // nominal de lo aplicado es 0.0 dB.
    nc.tonal.amount = static_cast<float>(compiled.amounts[4]);  // ARMONÍA
    nc.dynamics.punch = static_cast<float>(compiled.amounts[5]);  // PULSO
    nc.brain.adaptation =
        static_cast<float>(compiled.amounts[6]);  // NEURO
    double appliedGain = 0.0;
    for (std::size_t o = 0; o < experience_compiler::kNumOperators; ++o) {
        appliedGain += experience_compiler::nominalGainDbPerUnit(o) *
                       compiled.amounts[o];
    }
    nc.tonal.trimDb = static_cast<float>(-appliedGain);
    setControls(nc);  // clamp + fundido de modo + reparto existentes.
    return true;
}

// P9: delegación pura al simulador con la geometría por defecto. Ningún
// modo se aplica como efecto: NO hay camino de audio hacia living_room.
living_room::RoomGeometry FusedChain::livingRoomGeometry() const noexcept {
    return living_room::RoomGeometry{};
}

living_room::RoomDeltaResult FusedChain::livingRoomDeltaDb(
    double listenerDistanceM) const noexcept {
    return living_room::roomDeltaDb(living_room::RoomGeometry{},
                                    listenerDistanceM);
}

living_room::DeltaChangeResult FusedChain::predictLivingRoomDeltaChange(
    double fromM, double toM) const noexcept {
    return living_room::predictDeltaChange(living_room::RoomGeometry{},
                                           fromM, toM);
}

living_room::ModeInfo FusedChain::livingRoomModeInfo(
    living_room::RoomMode mode) const noexcept {
    return living_room::modeInfo(mode);
}

void FusedChain::refreshTwinCalibration(OmegaOutputMode mode) noexcept {
    // Solo recomputar cuando cambia el modo o la calibración (o la
    // primera vez): la estimación es función pura de parámetros
    // estáticos, por bloque sería trabajo inútil.
    const bool dirty =
        twinCalDirty_.exchange(false, std::memory_order_acq_rel);
    if (twinCalReady_ && !dirty && mode == twinCalMode_) return;
    twinCalMode_ = mode;
    twinCalReady_ = true;

    if (ivanna::omega_output_is_speakers_like(mode)) {
        // Trayecto Genezi: calibración con la respuesta MEDIDA desde el
        // barrido Farina de la app (2026-11-22 05:54, TwinCalibration::
        // geneziPz1dMeasured(), confianza honesta 0.80), o la
        // alimentada por la app. Calibración inválida -> estimado SIN
        // calibrar (~0.46) con el preset sin valores derivados, para
        // que la fórmula no infle la confianza a 0.926.
        // La P3 estimada (geneziPz1dEstimated(), 0.70) y la nota de voz
        // (geneziPz1dVoiceNote(), 0.75) quedan como respaldo
        // documentado; la medida en WAV limpio manda porque su
        // respuesta 63 Hz-8 kHz es MEDIDA en limpio, no AAC.
        const acoustic_twin::TwinCalibration cal =
            twinCalibrationHasOverride_.load(std::memory_order_acquire)
                ? twinCalibrationOverride_
                : acoustic_twin::TwinCalibration::geneziPz1dMeasured();
        acoustic_twin::AcousticTwinEstimate est;
        if (cal.isValid()) {
            est = acoustic_twin::AcousticTwin::estimateCalibrated(
                acoustic_twin::AcousticTwinParams::esseGeneziCalibrated(), cal);
        } else {
            est = acoustic_twin::AcousticTwin::estimate(
                acoustic_twin::AcousticTwinParams::esseGenezi());
        }
        twinEstimate_ = est;
        applyTwinToConservation(conservation_, est);
        telemetry_.twinCalibrationApplied = est.calibrationApplied;
        telemetry_.twinCalibrationConfidence =
            static_cast<float>(est.calibrationConfidence);
    } else {
        // Audífonos: la calibración del Genezi no aplica. Escala 1.0
        // (sin cambio) y telemetría en 0. Si se venía de SPEAKERS, el
        // reset de la escala es obligatorio: si no, la conservación en
        // audífonos quedaría escalada por la confianza del Genezi.
        twinEstimate_ = acoustic_twin::AcousticTwinEstimate{};
        conservation_.setTwinInterventionScale(1.0);
        telemetry_.twinCalibrationApplied = false;
        telemetry_.twinCalibrationConfidence = 0.0f;
    }
}

void FusedChain::pushEffectiveControls() noexcept {
    // El guardián AntiDolby (NEURO·Ω) ya se actualizó con el bloque actual
    // (el cerebro corre primero). Su modulación se aplica en este bloque:
    // widener → ancho de PULSO, EQ 2-4 kHz → agudos de ARMONÍA,
    // spread → ancho de ORBE·Ω. Intensidad 0 = sin modulación.
    const float gW = brain_.antiDolbyWidener();
    const float gE = brain_.antiDolbyEqBoostDb();
    const float gS = brain_.antiDolbySpreadMul();
    const float gI = brain_.antiDolbyIntensity();

    // Widener/spread se interpolan por intensidad: con intensidad 0
    // (MANUAL) no hay modulación (factor 1.0, el ancho queda en 1.0 y el
    // operador sigue neutro); con intensidad 1, modulación completa.
    // (Sin esta ponderación, el guardián sacaba al operador del neutro
    // aunque el usuario no pidiera nada: width != 1.0f rompe isNeutral().)
    const float spreadFactor = 1.0f + (gS - 1.0f) * gI;
    const float widenFactor = 1.0f + (gW - 1.0f) * gI;
    FusedControls eff = userControls_;
    // ORBE·Ω: ancho efectivo = ancho del usuario × spread del guardián.
    eff.spatial.width = clampf(eff.spatial.width * spreadFactor, 0.0f, 2.0f);
    // ARMONÍA: agudos efectivos = agudos del usuario + boost del guardián.
    eff.tonal.highDb = clampf(eff.tonal.highDb + gE * gI, -12.0f, 12.0f);
    // Enrutado del upmix: la inmersión la gobierna PULSO pero el upmixer
    // HOA vive en ORBE·Ω. Efectivo = el mayor de los dos paneles. El
    // upmixer se enciende si el usuario lo activó en el panel espacial O si
    // la inmersión de PULSO es > 0 (enrutado). NO se deriva de la
    // inmersividad espacial cruda: su default es 1.25 y setNeutral() la
    // pone a 0, pero un valor residual no debe encender el upmixer solo.
    eff.spatial.upmixImmersivity =
        std::max(eff.spatial.upmixImmersivity, eff.dynamics.upmixImmersivity);
    eff.spatial.upmixEnabled =
        eff.spatial.upmixEnabled || eff.dynamics.upmixImmersivity > 0.0f;

    // ── Fallo 3b: el guardián NO inyecta StereoWidener (M/S) sobre un
    // render binaural 3D ni en SPEAKERS ──────────────────────────────────
    // El widener M/S remezcla mid/side y destruye el ITD/ILD que ORBE·Ω,
    // HOLOS·Ω (GreenKernel), el HRTF o el upmix HOA acaban de codificar;
    // en SPEAKERS tampoco hay escena binaural que ensanchar (la ruta de
    // altavoz es XTC + AuxField). Solo se suprime el FACTOR del guardián:
    // el ancho que el usuario puso a mano en PULSO se respeta (elección
    // explícita, no inyección automática). Sin render binaural activo y
    // en audífonos, el guardián modula como siempre (MUSIC widener 1.35).
    const bool speakers =
        eff.spatial.mode == OmegaOutputMode::SPEAKERS;
    const bool binaural3d =
        !speakers &&
        (eff.spatial.amount > 0.0f || eff.spatial.hrtfWet > 0.0f ||
         eff.spatial.upmixEnabled || eff.spatial.wfsSpread > 0.0f ||
         eff.field.amount > 0.0f);
    const float widenFactorEff =
        (speakers || binaural3d) ? 1.0f : widenFactor;
    // PULSO: ancho efectivo = ancho del usuario × widener del guardián
    // (×1.0 cuando el arbitraje lo suprime: el ancho queda en el del
    // usuario y PULSO sigue neutro si era 1.0).
    eff.dynamics.width = clampf(eff.dynamics.width * widenFactorEff, 0.0f, 2.0f);

    // ── Fallo 3c: presupuesto tonal único ────────────────────────────────
    // Tres fuentes empujaban hacia Harman a la vez: ARMONÍA (EvoEQ Harman
    // 2018 ×0.5, o élite-lean), Φ dentro de HOLOS·Ω (target Harman AE/OE
    // 2018 por defecto en HyperStateOperator, corrección cut-only hacia
    // esa curva) y SALA VIVA (hearingProfile==1, aproximación Harman:
    // +2.9 dB graves / +1.0 dB agudos). Dueño único por prioridad:
    //   1. ARMONÍA (curve != 0) posee la curva: SALA VIVA pierde su
    //      Harman (solo su aproximación; su RIR y su corrección de sala
    //      cut-only no se tocan) y Φ —cuya corrección Harman NO se puede
    //      separar de su campo por controles— se atenúa a la mitad de su
    //      wet (sigue vivo: Volterra, EMF, Γ; solo tira la mitad hacia
    //      la curva).
    //   2. Sin ARMONÍA, Φ posee la curva: SALA VIVA pierde su Harman.
    //   3. Sin ARMONÍA ni Φ, SALA VIVA puede ser la única fuente Harman.
    // hearingProfile==2 (CUSTOM, audiograma personal) NO es Harman y no
    // se suprime nunca. Solo se tocan los controles EFECTIVOS; los del
    // usuario quedan intactos en userControls_.
    float phiScale = 1.0f;
    bool hearingSuppressed = false;
    if (eff.tonal.curve != 0) {
        if (eff.listenerRoom.hearingProfile == 1) {
            eff.listenerRoom.hearingProfile = 0;
            hearingSuppressed = true;
        }
        phiScale = 0.5f;
    } else if (eff.field.phiAmount > 0.0f) {
        if (eff.listenerRoom.hearingProfile == 1) {
            eff.listenerRoom.hearingProfile = 0;
            hearingSuppressed = true;
        }
    }
    eff.field.phiAmount = clampf(eff.field.phiAmount * phiScale, 0.0f, 1.0f);
    eff.clamp();

    // Puerta Genezi (Fallo 3a, PARCHE v2.6): el modo efectivo de HOLOS·Ω
    // sigue a spatial.mode en cada bloque (por si cambió sin setControls).
    eff.field.mode = eff.spatial.mode;
    brain_.setControls(eff.brain);
    spatial_.setControls(eff.spatial);
    field_.setControls(eff.field);
    listenerRoom_.setControls(eff.listenerRoom);
    transducer_.setControls(eff.transducer);
    tonal_.setControls(eff.tonal);
    dynamics_.setControls(eff.dynamics);

    // ── GEMELO ACÚSTICO + CALIBRACIÓN P3 — CALL SITE (2026-11-21) ────────
    // En runtime, la cadena consulta la estimación calibrada del gemelo
    // y alimenta su interventionScale a la conservación (el camino vivo
    // que ya la consume: process() la multiplica por el amount). Solo se
    // recomputa al cambiar el modo o la calibración; el primer bloque
    // tras prepare() ya la aplica. En SPEAKERS la escala es 0.70
    // (confianza honesta de la P3 estimada); en HEADPHONES es 1.0.
    // Corre ANTES de la sección 8: los defaults del gemelo para la
    // conservación (amount y techo) dependen de si la calibración se
    // aplicó (telemetry_.twinCalibrationApplied).
    refreshTwinCalibration(eff.spatial.mode);

    // ── 8. CONSERVACIÓN PERCEPTUAL (árbitro de campo, 2026-11-21) ────────
    // La capa recibe la config del usuario y los AMOUNTS EFECTIVOS de los
    // 7 operadores (de `eff`, no de userControls_: el guardián y el
    // arbitraje ya los modularon). El amount es la confianza de la
    // propuesta de cada operador: 0 = inactivo (no propone nada).
    // Proxies (documentados, no medidos): ARMONÍA no tiene un amount único
    // — su actividad es el mayor |dB| de su EQ (o 1.0 si lleva curva); PULSO
    // usa el mayor de sus amounts (punch/exciter/voice/safety).
    {
        ConservationConfig cc;
        // ── Control del gemelo (orden de Esse, 2026-11-21) ──
        // Con la calibración P3 aplicada (SPEAKERS, válida), el gemelo
        // CONTROLA la conservación: si el usuario no fijó amount ni
        // techo, se usan los defaults del gemelo
        // (kTwinCalibratedDefaultConservation*). Sin el amount, la escala
        // 0.70 no haría nada (0 × 0.70 = 0); sin el techo, el
        // solucionador nunca constriñe (ganancia siempre 1.0: solo baja
        // si el techo < energía medida) y el control sería un placebo.
        // El amount del usuario (> 0) y un techo fijado por el usuario
        // (< 1.0) siempre mandan. En audífonos no se aplica nada (la
        // calibración no está aplicada).
        float consAmount = eff.conservation.amount;
        float consEnergyCap = eff.conservation.energyCap;
        // Solo cuando la cadena está ACTIVA (no neutra): en neutro el
        // usuario pide bypass bit-exacto y el gemelo no debe intervenir
        // (TOmegaNeutralBitExactSpeakers y ChainNeutralStillBitExact lo
        // exigen). En escucha real (operadores encendidos) el gemelo sí
        // controla.
        if (telemetry_.twinCalibrationApplied && !isNeutral()) {
            if (consAmount <= 0.0f) {
                consAmount = kTwinCalibratedDefaultConservationAmount;
            }
            if (consEnergyCap >= 1.0f) {
                consEnergyCap = kTwinCalibratedDefaultConservationEnergyCap;
            }
        }
        cc.amount = consAmount;
        cc.energyCap = consEnergyCap;
        cc.phaseCoherence = eff.conservation.phaseCoherence;
        cc.inertia = eff.conservation.inertia;
        conservation_.setConfig(cc);
        const float tonalDb = std::max(
            std::max(std::abs(eff.tonal.trimDb), std::abs(eff.tonal.lowDb)),
            std::max(std::abs(eff.tonal.midDb), std::abs(eff.tonal.highDb)));
        // P8 — CORONA D10: el proxy tonal refleja la actividad EFECTIVA
        // (escalada por tonal.amount): con amount 0.5 y curva, la
        // actividad es 0.5, no 1.0. Se guarda para el árbitro D10
        // (arbitrateAction la recibe como tonalAmount en process()).
        const float tonalAmt = ((eff.tonal.curve != 0)
            ? 1.0f
            : clampf(tonalDb / 12.0f, 0.0f, 1.0f)) *
            clampf(eff.tonal.amount, 0.0f, 1.0f);
        effectiveTonalAmount_ = tonalAmt;
        const float pulseAmt = clampf(
            std::max(std::max(eff.dynamics.punch, eff.dynamics.exciter),
                     std::max(eff.dynamics.voice, eff.dynamics.safety)),
            0.0f, 1.0f);
        const float amounts[PerceptualConservation::kNumOperators] = {
            clampf(eff.brain.adaptation, 0.0f, 1.0f),           // 0 NEURO
            clampf(eff.spatial.amount, 0.0f, 1.0f),            // 1 ORBE
            clampf(eff.field.amount, 0.0f, 1.0f),              // 2 HOLOS
            clampf(eff.listenerRoom.roomWet, 0.0f, 1.0f),      // 3 SALA VIVA
            clampf(eff.transducer.correction, 0.0f, 1.0f),     // 4 VOLTERRA
            tonalAmt,                                          // 5 ARMONÍA
            pulseAmt,                                          // 6 PULSO
        };
        conservation_.setOperatorAmounts(amounts, PerceptualConservation::kNumOperators);
    }

    // (El call site del gemelo corre ANTES de la sección 8: los defaults
    // del gemelo para la conservación dependen de si la calibración se
    // aplicó.)

    // Telemetría para FotoHolo (ancho espacial + guardián + género).
    telemetry_.spatialWidth = eff.spatial.width;
    telemetry_.guardianWidener = gW;
    telemetry_.guardianEqBoostDb = gE * gI;
    telemetry_.guardianSpread = gS;
    telemetry_.guardianIntensity = gI;
    telemetry_.tonalBudgetPhiScale = phiScale;
    telemetry_.hearingHarmanSuppressed = hearingSuppressed;
    telemetry_.dominantGenre = brain_.dominantGenre();
    telemetry_.genreConfidence = brain_.confidence();
    telemetry_.brainAuto = (userControls_.brain.mode == 0);
}

void FusedChain::process(double* left, double* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    // 6.4 (refinamiento 2026-11-26) — FUNDIDO AL APAGAR EL D10 EN CALIENTE.
    // Apagar el D10 conmuta la rama TΩ de LTI (anillo caliente) a espectral
    // (WOLA con acumulador frío): sin fundido, la salida caía ~-25 dB RMS
    // ~512 muestras (medido: excess -22.8 dB en hot_d10_disable). El
    // fundido de modo (256 fuera / 512 dentro, el mismo que cubre los
    // cambios de modo) tapa la conmutación; la rama sigue en LTI durante
    // la salida (d10DisableFadePending_) y cambia a espectral en ganancia
    // 0, rellenando el WOLA durante la entrada. Solo al APAGAR con el
    // kernel enganchado y TΩ activa: encender solo no cambia la ruta
    // (el kernel no se engancha sin recompilar) y no necesita fundido.
    // No corre en neutro (tOmegaActive() exige !neutral).
    {
        const bool d10Now = d10Engine_.isEnabled();
        if (d10EnabledSeen_ && !d10Now && d10Engine_.ltiKernel().engaged() &&
            tOmegaActive() && modeFadePhase_ == ModeFadePhase::DONE) {
            modeFadePhase_ = ModeFadePhase::OUT;
            if (!std::isfinite(modeFadeGain_)) modeFadeGain_ = 1.0;
            d10DisableFadePending_ = true;
        }
        d10EnabledSeen_ = d10Now;
    }
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        // ── TRONIDOS 493 — el pendiente se aplica en frontera de chunk ──
        // Si un fundido de salida está en curso, el chunk se corta justo
        // donde la ganancia llega a 0: el pendiente se aplica al final de
        // ese chunk y el chunk siguiente ya corre con los controles nuevos
        // (TΩ, gemelo y compilación Reloj 2 ven el modo nuevo EN LA MISMA
        // llamada a process()). Sin esto, el pendiente se aplicaba a mitad
        // de chunk y la etapa nueva no arrancaba hasta el chunk siguiente:
        // los tests que ponen controles y miden en la misma llamada veían
        // el modo viejo (13 fallos en test_fused_chain).
        int fadeRemain = kFusedMaxBlockFrames;
        if (modeFadePhase_ == ModeFadePhase::OUT) {
            const double remain =
                modeFadeGain_ * static_cast<double>(kModeFadeOutSamples);
            fadeRemain = static_cast<int>(std::ceil(remain));
            if (fadeRemain < 1) fadeRemain = 1;
        }
        int chunk = std::min({remaining, kFusedMaxBlockFrames, fadeRemain});
        // ── S12/510 — fundido de aplicación por hop (slew) ─────────────
        // Mientras la rampa está activa, el chunk se parte en fronteras de
        // hop (128 muestras): cada sub-chunk corre con UN paso de la
        // interpolación, y el paso se aplica ANTES de que nada lea los
        // controles de este sub-chunk (apex/TΩ/puertas/pushEffectiveControls).
        // Sin rampa, chunk queda como antes (coste cero, sin cambio de
        // comportamiento). El primer hop usa t = 1/N y el hop N usa el
        // objetivo exacto (snap final en advanceParamRampHop).
        if (paramRampTotalHops_ > 0) {
            // max(1, …): defensa del invariante hopPos_ ∈ [0,127] — un
            // chunk de 0 muestras colgaría el bucle (remaining no avanza).
            chunk = std::min(chunk,
                             std::max(1, kParamRampHopSamples - paramRampHopPos_));
            if (paramRampHopPos_ == 0) advanceParamRampHop();
        }
        double* l = left + offset;
        double* r = right + offset;

        // ── APEX / TΩ POR CHUNK (no por llamada) ─────────────────────────
        // El pendiente del fundido de modo se aplica a mitad de llamada
        // (al llegar la ganancia a 0); el chunk siguiente ya debe ver el
        // modo nuevo. Por eso apex/tOmega/ITD se calculan por chunk.
        // ── IVANNA-APEX: el camino de transitorios solo engancha cuando hay ───
        // procesamiento que proteger. Si la cadena es NEUTRA (los 7 operadores
        // en bypass), el APEX se salta por completo: process() es bit-idéntico
        // al anterior (sin splitter ni suma), preservando la garantía de
        // neutralidad bit-exacta del chain (NeutralIsBitExactFloat). El APEX
        // protege los ataques DE los operadores; sin operadores activos no hay
        // nada que proteger, y el retardo de 2.0 muestras rompería la identidad.
        const bool apex = apexEnabled_.load(std::memory_order_relaxed) && !isNeutral();
        // ── TΩ (F1/472): matriz 2×2 64-bit (Kirkeby Genezi) ──────────────────
        // SOLO en SPEAKERS (470, crítica externa verificada): la matriz es
        // cancelación de diafonía para altavoces Genezi; en audífonos no hay
        // diafonía acústica que cancelar y aplicarla colorearía la señal.
        // La puerta exige: flag on + no neutro + modo SPEAKERS. El modo llega
        // a userControls_.spatial.mode por setControls (Ruta A vía JNI, Ruta B
        // vía setFusedOutputMode; el sync por atomics no lo toca, así que
        // persiste). Si nunca se puso, el default HEADPHONES deja la matriz
        // apagada (fail-closed). En neutro es passthrough bit-exacto.
        // La implementación es la TΩ (compileTOmega + applyTOmega, hop 128
        // empaquetado, double); el WOLA volumétrico queda LEGACY. Sin toggle
        // propio de TΩ: el interruptor de la etapa es setVolumetricEnabled().
        // Trayecto de altavoz = bocina plana o Genezi (evol-holos).
        const bool tOmega = volumetricEnabled_.load(std::memory_order_relaxed) && !isNeutral() &&
                            ivanna::omega_output_is_speakers_like(userControls_.spatial.mode);
        if (tOmega) {
            // Reloj 2: recompilar solo si cambió lo que la matriz refleja.
            TOmegaSnapshot snap;
            snap.mode = userControls_.spatial.mode;
            snap.sampleRate = sampleRate_;
            snap.spanM = volSpec_.spanM;
            snap.distanceM = volSpec_.distanceM;
            if (!tOmegaCompiled_ || snap != tOmegaSnapshot_) {
                tOmegaSnapshot_ = snap;
                compileTOmega();
            }
        }
        // ── IVANNA-APEX: ITD fraccionario del azimut de ORBE ──────────────────
        // Por chunk (el azimut del tracker puede cambiar entre bloques).
        // theta: mismo convenio que HrtfManager (azimut efectivo negado).
        // CORONA D10: la distancia la aporta el motor D10 (3.0 m, sala
        // documentada) con la ley de curvatura verificada — sustituye el
        // 1.0 m fijo de escena. Base causal 2.0 muestras (la línea de
        // Lagrange la exige >= 1.0, óptimo >= 2.0).
        double apexDelayL = 2.0, apexDelayR = 2.0;
        if (apex) {
            const SpatialControls sc = spatial_.controls();
            float azDeg = sc.azimuthDeg;
            if (sc.headTracking) azDeg += spatial_.trackerYawDeg();
            const double theta = -static_cast<double>(azDeg) *
                                 (3.14159265358979323846 / 180.0);
            const double itdSec = d10Engine_.coupledApexItdSeconds(theta);
            const double itdSamples = itdSec * sampleRate_;
            apexDelayL = 2.0 + std::max(0.0, itdSamples);
            apexDelayR = 2.0 + std::max(0.0, -itdSamples);
        }

        // ── TRONIDOS 493 — wet de cadena (cubre el APEX) ─────────────────
        // Objetivo: 1 si la cadena no es neutra, 0 si lo es. Solo durante
        // la rampa se guarda el seco de entrada del chunk (la entrada
        // cruda, antes del splitter APEX: el wet mezcla la salida final
        // con el passthrough que sonaba antes de enganchar).
        const bool chainTargetActive = !isNeutral();
        const bool chainRamping =
            chainWet_ != (chainTargetActive ? 1.0 : 0.0);
        const double* chainDryPtrL = nullptr;
        const double* chainDryPtrR = nullptr;
        if (chainRamping &&
            chainDryL_.size() >= static_cast<std::size_t>(chunk) &&
            chainDryR_.size() >= static_cast<std::size_t>(chunk)) {
            for (int i = 0; i < chunk; ++i) {
                chainDryL_[static_cast<std::size_t>(i)] = l[i];
                chainDryR_[static_cast<std::size_t>(i)] = r[i];
            }
            chainDryPtrL = chainDryL_.data();
            chainDryPtrR = chainDryR_.data();
        }

        // ── CORONA D10 — seco de referencia del árbitro ─────────────────
        // Copia de la entrada cruda del chunk (antes de APEX 1 y de
        // ningún operador) como referencia seca LUFS de arbitrateAction.
        // Solo si el D10 está habilitado y la cadena NO es neutra: en
        // neutro el árbitro no corre y no se paga la copia. Punteros
        // nulos = sin referencia seca.
        const double* d10DryL = nullptr;
        const double* d10DryR = nullptr;
        if (d10Engine_.isEnabled() && !isNeutral()) {
            // FIX-2 #1434: el kernel LTI retrasa la salida
            // kFirCenterShift muestras (desplazamiento circular del IR);
            // el seco de referencia lleva el MISMO retardo o el árbitro
            // compara bloques desalineados (acción con batido de 3
            // bloques en tono de 1 kHz: dispersión 0.134 vs umbral 0.02).
            // La línea se alimenta siempre (queda cebada para el
            // enganche); el seco retardado solo se usa cuando el kernel
            // procesa este chunk (misma condición que la llamada al
            // kernel más abajo). La latencia FIFO de la ruta espectral
            // LEGACY no se compensa (sin test que la mida).
            const bool kernelActive = tOmega && d10Engine_.ltiKernel().engaged();
            constexpr int kDelay = d10::TimeInvariantMatrixKernel2x2::kFirCenterShift;
            for (int i = 0; i < chunk; ++i) {
                const std::size_t w = static_cast<std::size_t>(d10DryDelayWp_);
                const double delayedL = d10DryDelayL_[w];
                const double delayedR = d10DryDelayR_[w];
                d10DryDelayL_[w] = l[i];
                d10DryDelayR_[w] = r[i];
                d10DryDelayWp_ = (d10DryDelayWp_ + 1) % kDelay;
                const std::size_t o = static_cast<std::size_t>(i);
                d10DryL_[o] = kernelActive ? delayedL : l[i];
                d10DryR_[o] = kernelActive ? delayedR : r[i];
            }
            d10DryL = d10DryL_.data();
            d10DryR = d10DryR_.data();
        }

        if (apex) {
            // ── APEX 1: separar transitorio/sostenido (muestra a muestra) ──
            // El SOSTENIDO es lo que procesan los 7 operadores; el
            // TRANSITORIO se guarda para sumarlo al final sin procesar.
            for (int i = 0; i < chunk; ++i) {
                double tL, tR, sL, sR;
                apexSplitter_.splitSample(l[i], r[i], tL, tR, sL, sR);
                apexTransL_[static_cast<std::size_t>(i)] = tL;
                apexTransR_[static_cast<std::size_t>(i)] = tR;
                apexSustL_[static_cast<std::size_t>(i)] = sL;
                apexSustR_[static_cast<std::size_t>(i)] = sR;
            }
            for (int i = 0; i < chunk; ++i) {
                l[i] = apexSustL_[static_cast<std::size_t>(i)];
                r[i] = apexSustR_[static_cast<std::size_t>(i)];
            }
        }

        // ── TRONIDOS 493 — fundido de enganche por operador ─────────────
        // Cada operador corre con su wet (seco→procesado en 30 ms por
        // muestra). En régimen el wet ya es el objetivo y no se mezcla
        // nada (bit-exacto, sin coste): solo durante la rampa se guarda
        // el seco y se mezcla. El objetivo es la neutralidad EFECTIVA
        // (tras pushEffectiveControls): el cerebro usa su neutralidad
        // pre-push (corre antes del push); los demás, la post-push.
        auto runOperator = [&](auto& opObj, int op, bool targetActive) noexcept {
            const bool ramping =
                opWet_[static_cast<std::size_t>(op)] != (targetActive ? 1.0 : 0.0);
            const double* dryL = nullptr;
            const double* dryR = nullptr;
            if (ramping &&
                wetDryL_.size() >= static_cast<std::size_t>(chunk) &&
                wetDryR_.size() >= static_cast<std::size_t>(chunk)) {
                for (int i = 0; i < chunk; ++i) {
                    wetDryL_[static_cast<std::size_t>(i)] = l[i];
                    wetDryR_[static_cast<std::size_t>(i)] = r[i];
                }
                dryL = wetDryL_.data();
                dryR = wetDryR_.data();
            }
            opObj.process(l, r, chunk);
            if (ramping) blendOperatorWet(l, r, dryL, dryR, chunk, op, targetActive);
            ++stats_.blocksProcessed[static_cast<std::size_t>(op)];
            if (targetActive) ++stats_.activeBlocks[static_cast<std::size_t>(op)];
        };

        // 0. NEURO·Ω primero: observa y actúa; actualiza el guardián.
        const bool brainActive = !brain_.isNeutral();
        runOperator(brain_, 0, brainActive);

        // Modulación del guardián + enrutado del upmix (con el cerebro ya actualizado).
        pushEffectiveControls();

        // 1..6 en orden canónico. Cada operador es bit-transparente en neutro.
        const bool active[7] = {
            brainActive,
            !spatial_.isNeutral(),
            !field_.isNeutral(),
            !listenerRoom_.isNeutral(),
            !transducer_.isNeutral(),
            !tonal_.isNeutral(),
            !dynamics_.isNeutral(),
        };
        runOperator(spatial_, 1, active[1]);
        runOperator(field_, 2, active[2]);
        runOperator(listenerRoom_, 3, active[3]);
        runOperator(transducer_, 4, active[4]);
        runOperator(tonal_, 5, active[5]);
        runOperator(dynamics_, 6, active[6]);

        if (apex) {
            // ── APEX 2: el TRANSITORIO (sin operadores) se retrasa ────────
            // por la línea de Lagrange (ITD fraccionario) y se SUMA a la
            // salida de los operadores. Los ataques no pasan por STFT ni
            // por compresor: no se emborronan ni se comprimen.
            for (int i = 0; i < chunk; ++i) {
                l[i] += apexDelayL_.process(apexTransL_[static_cast<std::size_t>(i)], apexDelayL);
                r[i] += apexDelayR_.process(apexTransR_[static_cast<std::size_t>(i)], apexDelayR);
            }
        }

        // ── 8. CONSERVACIÓN PERCEPTUAL (árbitro de campo, 2026-11-21) ────
        // Tras los 7 operadores + transitorios APEX, antes de TΩ. Mide el
        // estado de campo por banda, arbitra las propuestas de los 7
        // operadores y aplica la ganancia de conservación (<= 1, con rampa
        // por muestra). Con amount=0 no toca el buffer (bit-exacto) y el
        // coste es un atomic + una rama. El tiempo perceptual es el tiempo
        // monotónico de la cadena.
        conservation_.process(l, r, chunk, chainTime_);
        chainTime_ += static_cast<double>(chunk) / sampleRate_;

        // ── TΩ (F1/472) / D10 LTI (CORONA D10) ───────────────────────────
        // Corrección de altavoz al final (tras operadores + transitorios),
        // en el lugar que ocupaba la matriz volumétrica WOLA (LEGACY).
        // CORONA D10: el kernel LTI SUSTITUYE la ruta espectral TΩ cuando
        // está enganchado — la matriz fija no modula a la tasa de hop y
        // mata las bandas laterales a ±375 Hz (esa es la gracia del D10).
        // La ruta espectral (applyTOmega) queda LEGACY: solo se usa si el
        // kernel NO está enganchado (antes del primer compile, o con el
        // D10 deshabilitado). Con TΩ apagado, la historia se sigue
        // alimentando (warm): al engancharse, el primer frame usa señal
        // real y no ceros (sin esto, el cambio de modo dejaba 2 bloques
        // de 0 exacto). En HEADPHONES (tOmega false) nada cambia: M=I.
        // NOTA DE LATENCIA: al conmutar de la ruta espectral (FIFO ~512
        // muestras) al kernel LTI (FIR causal de 512 taps con
        // desplazamiento circular de 256 — retardo puro de 5.33 ms en
        // serie, fix-2 #1372) la latencia de la etapa cambia; el morph
        // de 512 muestras suaviza la ganancia pero no el salto de
        // retardo. En régimen el kernel ya está enganchado desde el
        // primer chunk con TΩ (compileTOmega corre antes, en el mismo
        // chunk), así que la ruta espectral prácticamente no llega a
        // sonar con D10 ON: la conmutación solo se ve al habilitar el
        // D10 en caliente o tras un reset.
        // 6.4: mientras el fundido por apagado del D10 está en salida, la
        // rama sigue en LTI (d10DisableFadePending_): el cambio a espectral
        // ocurre en ganancia 0 (applyModeFadePending limpia el flag).
        const bool ltiHold = d10DisableFadePending_ &&
                             modeFadePhase_ == ModeFadePhase::OUT;
        if (tOmega && ((d10Engine_.isEnabled() &&
                        d10Engine_.ltiKernel().engaged()) ||
                       ltiHold)) {
            // 6.4 (refinamiento 2026-11-26) — HISTORIA ESPECTRAL CALIENTE
            // MIENTRAS EL LTI CORRE: si el D10 se apaga en caliente, la rama
            // cae a applyTOmega; sin este warm, su FIFO llevaba fría desde
            // que el kernel se enganchó. El warm va ANTES del kernel (l/r
            // aún son la entrada pre-TΩ: lo que applyTOmega habría recibido)
            // y solo alimenta la historia (no toca el buffer): con el D10
            // encendido es no-op audible.
            warmTOmegaHistory(l, r, chunk);
            d10Engine_.ltiKernel().process(l, r, chunk);
        } else if (tOmega) {
            applyTOmega(l, r, chunk);  // LEGACY (ver arriba)
        } else {
            warmTOmegaHistory(l, r, chunk);
        }

        // ── CORONA D10 — árbitro (tras Conservación + TΩ/LTI) ───────────
        // FIX-ARBITER (2026-11-25): el árbitro YA NO TOCA EL BUFFER
        // (energyCorrectionEnabled=false por defecto): solo observa el
        // estado térmico y calcula telemetría (acción, coherencia,
        // lufsDelta). La ganancia por muestra con slew es LEGACY
        // desactivada: medida, modulaba el tono, degradaba los pisos y
        // sumaba +2.34 dB que rompía P0. Corre ANTES del techo final (el
        // techo sigue después y sigue siendo la última barrera). No-op si
        // el D10 está deshabilitado o la cadena es neutra (en neutro
        // d10DryL es nulo y además no se invoca). tonalAmount = amount
        // tonal EFECTIVO del bloque (pushEffectiveControls). La telemetría
        // del árbitro alimenta Ξ_10 del certificado P7
        // (d10TelemetryValid_ = true).
        if (d10Engine_.isEnabled() && !isNeutral()) {
            d10Engine_.arbitrateAction(l, r, d10DryL, d10DryR, chunk,
                                       static_cast<double>(effectiveTonalAmount_));
            d10TelemetryValid_ = true;
        }

        // ── TECHO FINAL (P1 2026-11-22): el safety limiter de PULSO ya no
        // es la última barrera — APEX 2 suma el transitorio DESPUÉS de él
        // (hotmix_01: APEX ON +0.81 dBFS, APEX OFF −0.14 dBFS) y TΩ también
        // puede subir el pico. Este techo es la última barrera absoluta:
        // clava la salida a ±0.9885531f (−0.1 dBFS exacto a float32, el
        // MISMO techo del safety limiter, para que ambos no diverjan). Transparente bajo el techo
        // (bit-idéntico: solo actúa cuando |x| > techo) y fail-closed.
        // Solo cuando la cadena NO es neutra: en bypass no corre y se
        // preserva la bit-exactitud. No cambia la ganancia neta (Regla de
        // Oro Matched-LUFS: 0.0 dB).
        if (!isNeutral()) {
            const double ceil = static_cast<double>(FusedDynamics::kLimiterCeiling);
            for (int i = 0; i < chunk; ++i) {
                l[i] = std::clamp(l[i], -ceil, ceil);
                r[i] = std::clamp(r[i], -ceil, ceil);
            }
        }

        // ── TRONIDOS 493 — wet de cadena ───────────────────────────────
        // Mezcla la salida final con el seco de entrada (cubre el enganche
        // del splitter APEX, fuera del alcance de los wets por operador:
        // con APEX apagado el enganche ya es ~0 dB; con APEX era
        // +1.1…1.7 dB). En régimen (wet == objetivo) no se mezcla nada.
        if (chainRamping) {
            chainWet_ = blendWetCore(l, r, chainDryPtrL, chainDryPtrR, chunk,
                                     chainWet_,
                                     chainTargetActive ? 1.0 : 0.0,
                                     wetRampStep(), true);  // OPCIÓN A: cadena compensada (ρ∈[0,1])
        }

        // ── TRONIDOS 493 — fundido de modo ───────────────────────────────
        // Ganancia de salida 1→0 (256 muestras) y 0→1 (512 muestras) en
        // los cambios de modo / enganche de TΩ. Solo corre durante el
        // fundido (fase DONE = sin coste, sin tocar el buffer). Al llegar
        // a 0 se aplica el pendiente (modo nuevo + prime TΩ) y el fundido
        // pasa a IN. Al llegar a 1.0 el fundido termina y el resto del
        // chunk queda intacto (bit-exacto). Fail-closed: ganancia no
        // finita → 1.0 y DONE (la salida sigue, sin fundido).
        if (modeFadePhase_ != ModeFadePhase::DONE) {
            if (!std::isfinite(modeFadeGain_)) {
                modeFadeGain_ = 1.0;
                modeFadePhase_ = ModeFadePhase::DONE;
            } else {
                const double outStep = 1.0 / static_cast<double>(kModeFadeOutSamples);
                const double inStep = 1.0 / static_cast<double>(kModeFadeInSamples);
                for (int i = 0; i < chunk; ++i) {
                    if (modeFadePhase_ == ModeFadePhase::OUT) {
                        modeFadeGain_ -= outStep;
                        if (modeFadeGain_ <= 0.0) {
                            modeFadeGain_ = 0.0;
                            applyModeFadePending();  // pasa a IN
                        }
                    } else if (modeFadePhase_ == ModeFadePhase::IN) {
                        modeFadeGain_ += inStep;
                        if (modeFadeGain_ >= 1.0) {
                            // Fundido completo: esta muestra y las que
                            // quedan ya están a ganancia 1.0 (sin tocar).
                            modeFadeGain_ = 1.0;
                            modeFadePhase_ = ModeFadePhase::DONE;
                            break;
                        }
                    } else {
                        break;  // DONE a mitad de chunk: el resto intacto
                    }
                    l[i] *= modeFadeGain_;
                    r[i] *= modeFadeGain_;
                }
            }
        }

        // ── TECHO FINAL, 2ª PASADA (rampa de enganche, CORONA 2026-11-25) ──
        // El wet de cadena (TRONIDOS 493) mezcla el seco DESPUÉS del techo
        // de arriba: durante la rampa de enganche (30 ms, chainWet_ 0→1) el
        // seco caliente pasaba sin clavar — un seno 1 kHz a 0 dBFS salía a
        // −0.001 dBFS con APEX ON y OFF (el hotmix a 0 dBFS sí quedaba en
        // −0.100/−0.139 porque su pico cae tras la rampa). Esta 2ª pasada
        // es la ÚLTIMA barrera: clava la salida a ±0.9885531f (−0.1 dBFS
        // exacto) también durante la rampa. Transparente bajo el techo
        // (bit-idéntico: solo actúa cuando |x| > techo) y solo cuando la
        // cadena NO es neutra: en bypass no corre y se preserva la
        // bit-exactitud. El fundido de modo solo multiplica por ≤1, así
        // que no puede crear violación; la pasada va después de él para
        // que NINGUNA etapa posterior al techo pueda subir el pico.
        if (!isNeutral()) {
            const double ceilFinal =
                static_cast<double>(FusedDynamics::kLimiterCeiling);
            for (int i = 0; i < chunk; ++i) {
                l[i] = std::clamp(l[i], -ceilFinal, ceilFinal);
                r[i] = std::clamp(r[i], -ceilFinal, ceilFinal);
            }
        }

        ++stats_.totalBlocks;
        for (int i = 0; i < kNumOperators; ++i) telemetry_.active[i] = active[i];

        // ── RE-SEED Γ (HOLOS, 2026-11-26) ───────────────────────────────
        // Al completar el enganche (todos los wets en objetivo y fundido
        // de modo terminado), el estimador Γ de HOLOS retiene el
        // transitorio de la rampa de 30 ms (envolventes de 260 ms) y la
        // Regla de Oro sale +0.104 dB en el 2º segundo. Se pide UN re-seed
        // (el próximo frame de HOLOS siembra sus envolventes desde el
        // espectro asentado): el blend queda bit-idéntico (dip + barrido
        // intactos) y Γ nunca aprende el transitorio. Se re-arme en cada
        // rampa nueva o fundido de modo (el siguiente fin de rampa vuelve
        // a pedirlo). No se pide en neutro ni con HOLOS inactivo. Una
        // petición pendiente de un enganche anterior que no se consumió
        // (HOLOS en bypass por coherencia) se cancela al re-armar; si se
        // consumiera en el primer frame de un enganche nuevo, sembraría
        // desde la rampa viva — el re-seed propio del enganche nuevo la
        // corrige al terminar su rampa.
        {
            bool allAtTarget = (chainWet_ == (chainTargetActive ? 1.0 : 0.0));
            for (int i = 0; i < kNumOperators; ++i) {
                if (opWet_[static_cast<std::size_t>(i)] != (active[i] ? 1.0 : 0.0)) {
                    allAtTarget = false;
                    break;
                }
            }
            if (!isNeutral() && active[2] && allAtTarget &&
                modeFadePhase_ == ModeFadePhase::DONE) {
                if (!holosReseedDone_) {
                    field_.requestHolosReseed();
                    holosReseedDone_ = true;
                }
            } else if (!allAtTarget || modeFadePhase_ != ModeFadePhase::DONE) {
                holosReseedDone_ = false;
                field_.clearHolosReseedRequest();
            }
        }

        // S12/510: avance de la posición dentro del hop vigente (solo
        // mientras la rampa sigue activa; al completarse en el avance de
        // arriba, hopPos_ ya está a 0 y no hay nada que acumular).
        if (paramRampTotalHops_ > 0) {
            paramRampHopPos_ += chunk;
            if (paramRampHopPos_ >= kParamRampHopSamples) paramRampHopPos_ = 0;
        }

        offset += chunk;
        remaining -= chunk;
    }
}

void FusedChain::processFloat(float* left, float* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    if (scratchL_.size() < static_cast<std::size_t>(kFusedMaxBlockFrames) ||
        scratchR_.size() < static_cast<std::size_t>(kFusedMaxBlockFrames)) {
        prepare(sampleRate_);
    }
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int chunk = std::min(remaining, kFusedMaxBlockFrames);
        for (int i = 0; i < chunk; ++i) {
            scratchL_[static_cast<std::size_t>(i)] = static_cast<double>(left[offset + i]);
            scratchR_[static_cast<std::size_t>(i)] = static_cast<double>(right[offset + i]);
        }
        process(scratchL_.data(), scratchR_.data(), chunk);
        for (int i = 0; i < chunk; ++i) {
            const double vl = scratchL_[static_cast<std::size_t>(i)];
            const double vr = scratchR_[static_cast<std::size_t>(i)];
            left[offset + i] = std::isfinite(vl) ? static_cast<float>(vl) : 0.0f;
            right[offset + i] = std::isfinite(vr) ? static_cast<float>(vr) : 0.0f;
        }
        offset += chunk;
        remaining -= chunk;
    }
}

}  // namespace ivanna::fused
