// universal_hearing.cpp — Modelo Universal de Audición, P6 (2026-11-21).
//
// Modelos usados (TODO NOMINAL: calibración inicial por razonamiento, NO
// medición del Genezi ni de la sala de Esse; ver README). Cada fórmula
// da tendencias y órdenes de magnitud con la monotonía correcta:
//
//   · Pérdida de profundidad: 0.55·RT60norm + 0.45·(1-coherencia).
//     Crece con RT60 y con distancia/distancia-crítica (coherencia baja).
//     Referencia: RT60norm = RT60banda/1.0 s (1 s = pérdida plena por
//     reverberación); la coherencia es directo/(directo+reverberante).
//   · Compresión espacial: 0.40·RT60norm + 0.35·(1-coherencia)
//     + 0.25·(|azimut|/90°). Crece con RT60 alto + coherencia baja +
//     azimut descentrado (90° = contribución plena).
//   · Enmascaramiento tímbrico: 0.40·RT60norm·pesoGrave + 0.25·nivel
//     + 0.20·ruidoAmbiente + 0.15·THDnorm. Crece con RT60 en graves
//     (pesoGrave 1.0 a 63–125 Hz → 0.3 en agudos), nivel alto y piso de
//     ruido. Referencias nominales: nivel 0 dBFS = 1 (−60 dBFS = 0);
//     ambiente 20 dB SPL = 0, 60 dB SPL = 1; THD 5 % = 1.
//   · Fatiga predicha (dim 5 de P[t], NO duplicada): fatigaPrevia +
//     exposición·nivelSobre85·(0.5+0.5·nivelBanda)·(1+0.5·THDnorm)
//     + 0.10·(sesión/240 min). Referencias: 120 min de exposición = 1,
//     85 dB SPL = 0 (nivel seguro de referencia), 105 dB = 1; la
//     sesión larga suma hasta 0.10 aunque el nivel sea seguro.
//   · Pérdida de inteligibilidad: factorContenido·(0.50·RT60medios
//     + 0.30·ruidoAmbiente + 0.20·(1-coherencia)). RT60medios =
//     min(RT60banda/0.8, 1)·pesoMedios (pico 500 Hz–2 kHz). La voz es
//     más sensible que la música (factor 1.2; mixto 1.1).
//   · Desplazamiento de imagen: |azimut| + desbalance·5°/dB (NOMINAL:
//     1 dB de desbalance ≈ 5° de desplazamiento lateral). Normalizado:
//     30° = 1.0. También se expone en grados (imageShiftDegrees).
//   · Pérdida de microdinámica: 0.50·(1-crestNorm) + 0.30·ruidoPiso
//     + 0.20·THDnorm. Crece con crest bajo (compresión), piso de ruido
//     y THD. Referencias: crest 20 dB = dinámica plena; piso −120 dBFS
//     = 0, −60 dBFS = 1; THD 5 % = 1.
//   · Pérdida de causalidad: 0.60·latenciaNorm + 0.40·RT60norm.
//     Latencia total = buffer + aire (d/343); 50 ms = pérdida plena.
//     El RT60 aporta smearing temporal.
//   · Las 9 dims de P[t] se DERIVAN de las pérdidas (no se inventan
//     aparte): audibilidad del nivel y el enmascaramiento; claridad de
//     inteligibilidad y enmascaramiento; naturalidad de THD y
//     enmascaramiento; impacto de transitorios, crest y nivel;
//     espacialidad de profundidad y compresión espacial; estabilidad
//     de desplazamiento de imagen y movimiento de cabeza; confianza
//     del modelo; presupuesto 0 (el modelo solo predice, no consume).
//
// RT60 por banda: RT60/alphaFactor, mismo factor nominal de absorción
// que el gemelo acústico (la absorción sube con la frecuencia).
//
// CONFIANZA. Base 1.0 menos penalizaciones por lo que NO se sabe (cada
// una NOMINAL, documentada aquí y en el README):
//   THD desconocido −0.10 · piso de ruido desconocido −0.05 ·
//   crest desconocido −0.05 · transitorios desconocidos −0.05 ·
//   tonal desconocido −0.03 · exposición desconocida −0.05 ·
//   ambiente desconocido −0.05 · coherencia derivada −0.05 ·
//   distancia crítica derivada −0.05 · sample rate no estándar −0.05 ·
//   bits no 16/24/32 −0.05 · RT60 > 2 s −0.20 · volumen < 5 o
//   > 10000 m³ −0.20 · SPL fuera de [40,120] −0.10 ·
//   desbalance > 3 dB −0.10.
// Por banda (solo altavoz): ×0.85 por debajo de Schroeder
// (fs = 2000·√(RT60/V)), donde la sala es modal y el RT60 por banda
// pierde significado. interventionScale = confidence (a confianza baja
// se interviene MENOS — igual que el gemelo, nunca al revés).
//
// FAIL-CLOSED. Entrada inválida (NaN/inf, fuera de rango, dato
// requerido desconocido) → valid=false, confianza 0,
// interventionScale 0, predicciones en 0.0 ("sin predicción").
// NUNCA NaN/inf en la salida. Un dato opcional desconocido aporta 0
// a su término y baja la confianza (la predicción es un piso).
#include "universal_hearing.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::fused::universal_hearing {
namespace {

// Factor de absorción relativa por banda (1.0 a 1 kHz). MISMO modelo
// nominal que el gemelo acústico: la absorción típica de una sala
// amueblada sube con la frecuencia. No es una medición de la sala.
constexpr double kAlphaFactor[kHearingBands] = {
    0.65, 0.75, 0.85, 0.95, 1.00, 1.05, 1.15, 1.25, 1.35};

// Peso grave del enmascaramiento tímbrico (1.0 en graves → 0.3 agudos).
// NOMINAL: el enmascaramiento por reverberación pesa más en graves.
constexpr double kLowWeight[kHearingBands] = {
    1.0, 1.0, 0.9, 0.7, 0.5, 0.4, 0.3, 0.3, 0.3};

// Peso medios de la inteligibilidad (pico 500 Hz–2 kHz). NOMINAL: la
// inteligibilidad vive en los medios.
constexpr double kMidWeight[kHearingBands] = {
    0.3, 0.4, 0.6, 1.0, 1.0, 1.0, 0.8, 0.5, 0.3};

constexpr double kSpeedOfSoundMs = 343.0;  // m/s, 20 °C.
constexpr double kImageShiftFullDeg = 30.0;  // 30° de desplazamiento = 1.0
// NOMINAL: 1 dB de desbalance de canal ≈ 5° de desplazamiento lateral.
constexpr double kDegPerDbImbalance = 5.0;

double clampd(double x, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, x));
}

bool allFinite(const std::array<double, kHearingBands>& a) noexcept {
    for (double x : a) {
        if (!std::isfinite(x)) return false;
    }
    return true;
}

bool isStandardSampleRate(int sr) noexcept {
    switch (sr) {
        case 44100: case 48000: case 88200: case 96000:
        case 176400: case 192000: case 352800: case 384000:
            return true;
        default: return false;
    }
}

}  // namespace

UniversalHearingInput UniversalHearingInput::esseGenezi() noexcept {
    UniversalHearingInput in;
    // Señal: música típica, RMS por banda en dBFS (espectro con caída
    // en agudos), crest 12 dB, 30 min de exposición al nivel actual.
    in.signal.rmsDbPerBand = {-24.0, -22.0, -20.0, -19.0, -18.0,
                              -19.0, -21.0, -24.0, -28.0};
    in.signal.levelKnown = true;
    in.signal.crestFactorDb = 12.0;
    in.signal.crestKnown = true;
    in.signal.transientDensity = 0.6;
    in.signal.transientKnown = true;
    in.signal.tonalFraction = 0.5;
    in.signal.tonalKnown = true;
    in.signal.exposureMinutes = 30.0;
    in.signal.exposureKnown = true;
    // Cadena: DAC JieLi 24-bit/48 kHz, buffer 128 frames (2.667 ms).
    // THD y piso de ruido NO medidos (0 = desconocidos).
    in.chain.dacBits = 24;
    in.chain.dacSampleRateHz = 48000;
    in.chain.thdPercent = 0.0;  // desconocido
    in.chain.noiseFloorDb = 0.0;  // desconocido
    in.chain.bufferLatencyMs = 2.667;
    in.chain.transducer = Transducer::kSpeaker;
    in.chain.channelImbalanceDb = 0.0;
    // Sala 4.0×7.0 m (V = 70 m³), RT60 0.34 s, ambiente 30 dB SPL.
    // Distancia crítica y coherencia no medidas: se derivan.
    in.room.rt60Sec = 0.34;
    in.room.roomVolumeM3 = 70.0;
    in.room.criticalDistanceM = 0.0;  // desconocida → derivada
    in.room.coherence = -1.0;         // desconocida → derivada
    in.room.ambientNoiseDbSpl = 30.0;
    // Oyente a 3 m, centrado, casi quieto.
    in.listener.distanceM = 3.0;
    in.listener.azimuthDeg = 0.0;
    in.listener.headMovement = 0.1;
    // Música a 85 dB SPL, sesión de 30 min, sin fatiga previa.
    in.context.content = ContentType::kMusic;
    in.context.targetSplDb = 85.0;
    in.context.priorFatigue = 0.0;
    in.context.sessionMinutes = 30.0;
    return in;
}

UniversalHearingInput UniversalHearingInput::esseHeadphones() noexcept {
    UniversalHearingInput in = esseGenezi();
    in.chain.transducer = Transducer::kHeadphone;
    return in;
}

UniversalHearingPrediction UniversalHearingModel::predict(
    const UniversalHearingInput& in) noexcept {
    UniversalHearingPrediction out;  // todo en 0, valid=false por defecto.
    for (std::size_t i = 0; i < kHearingBands; ++i) {
        out.band[i].v.fill(0.0);
    }
    out.globalMean.fill(0.0);

    const bool headphones = (in.chain.transducer == Transducer::kHeadphone);

    // ── Validación dura (fail-closed) ───────────────────────────────
    bool invalid = false;
    // Señal: nivel REQUERIDO, resto opcional con flag.
    if (!in.signal.levelKnown) invalid = true;
    if (!allFinite(in.signal.rmsDbPerBand)) invalid = true;
    if (in.signal.levelKnown) {
        for (double rms : in.signal.rmsDbPerBand) {
            if (rms < -120.0 || rms > 0.0) invalid = true;
        }
    }
    if (!std::isfinite(in.signal.crestFactorDb) ||
        !std::isfinite(in.signal.transientDensity) ||
        !std::isfinite(in.signal.tonalFraction) ||
        !std::isfinite(in.signal.exposureMinutes)) {
        invalid = true;
    }
    if (in.signal.crestKnown &&
        (in.signal.crestFactorDb < 0.0 || in.signal.crestFactorDb > 40.0)) {
        invalid = true;
    }
    if (in.signal.transientKnown &&
        (in.signal.transientDensity < 0.0 || in.signal.transientDensity > 1.0)) {
        invalid = true;
    }
    if (in.signal.tonalKnown &&
        (in.signal.tonalFraction < 0.0 || in.signal.tonalFraction > 1.0)) {
        invalid = true;
    }
    if (in.signal.exposureKnown &&
        (in.signal.exposureMinutes < 0.0 || in.signal.exposureMinutes > 600.0)) {
        invalid = true;
    }
    // Cadena.
    if (in.chain.dacBits <= 0 || in.chain.dacBits > 32) invalid = true;
    if (in.chain.dacSampleRateHz < 8000 || in.chain.dacSampleRateHz > 768000) {
        invalid = true;
    }
    if (!std::isfinite(in.chain.thdPercent) ||
        !std::isfinite(in.chain.noiseFloorDb) ||
        !std::isfinite(in.chain.bufferLatencyMs) ||
        !std::isfinite(in.chain.channelImbalanceDb)) {
        invalid = true;
    }
    if (in.chain.thdPercent < 0.0 || in.chain.thdPercent > 100.0) invalid = true;
    if (in.chain.noiseFloorDb < -160.0 || in.chain.noiseFloorDb > 0.0) {
        invalid = true;
    }
    if (in.chain.bufferLatencyMs < 0.0 || in.chain.bufferLatencyMs > 1000.0) {
        invalid = true;
    }
    if (in.chain.channelImbalanceDb < 0.0 ||
        in.chain.channelImbalanceDb > 20.0) {
        invalid = true;
    }
    if (in.chain.transducer != Transducer::kSpeaker &&
        in.chain.transducer != Transducer::kHeadphone) {
        invalid = true;
    }
    // Sala (solo altavoz; en audífono se ignora por completo).
    if (!headphones) {
        if (!std::isfinite(in.room.rt60Sec) ||
            !std::isfinite(in.room.roomVolumeM3) ||
            !std::isfinite(in.room.criticalDistanceM) ||
            !std::isfinite(in.room.coherence) ||
            !std::isfinite(in.room.ambientNoiseDbSpl)) {
            invalid = true;
        }
        // RT60 y volumen REQUERIDOS en altavoz (0 = desconocido).
        if (in.room.rt60Sec <= 0.0 || in.room.rt60Sec > 10.0) invalid = true;
        if (in.room.roomVolumeM3 <= 0.0 || in.room.roomVolumeM3 > 100000.0) {
            invalid = true;
        }
        // Distancia crítica: 0 = desconocida (se deriva), si se da debe
        // ser física.
        if (in.room.criticalDistanceM < 0.0 || in.room.criticalDistanceM > 100.0) {
            invalid = true;
        }
        // Coherencia: -1 = desconocida (se deriva), si se da en [0,1].
        if (in.room.coherence < -1.0 || in.room.coherence > 1.0) invalid = true;
        if (in.room.coherence < 0.0 && in.room.coherence != -1.0) invalid = true;
        // Ambiente: 0 = desconocido, si se da en (0, 90].
        if (in.room.ambientNoiseDbSpl < 0.0 ||
            in.room.ambientNoiseDbSpl > 90.0) {
            invalid = true;
        }
    }
    // Oyente.
    if (!std::isfinite(in.listener.distanceM) ||
        !std::isfinite(in.listener.azimuthDeg) ||
        !std::isfinite(in.listener.headMovement)) {
        invalid = true;
    }
    if (!headphones) {
        // Distancia REQUERIDA en altavoz (0 = desconocida).
        if (in.listener.distanceM <= 0.0 || in.listener.distanceM > 100.0) {
            invalid = true;
        }
    } else if (in.listener.distanceM < 0.0) {
        invalid = true;
    }
    if (in.listener.azimuthDeg < -180.0 || in.listener.azimuthDeg > 180.0) {
        invalid = true;
    }
    if (in.listener.headMovement < 0.0 || in.listener.headMovement > 1.0) {
        invalid = true;
    }
    // Contexto: SPL objetivo REQUERIDO (0 = desconocido).
    if (in.context.content != ContentType::kMusic &&
        in.context.content != ContentType::kVoice &&
        in.context.content != ContentType::kMixed) {
        invalid = true;
    }
    if (!std::isfinite(in.context.targetSplDb) ||
        !std::isfinite(in.context.priorFatigue) ||
        !std::isfinite(in.context.sessionMinutes)) {
        invalid = true;
    }
    if (in.context.targetSplDb <= 0.0 || in.context.targetSplDb > 140.0) {
        invalid = true;
    }
    if (in.context.priorFatigue < 0.0 || in.context.priorFatigue > 1.0) {
        invalid = true;
    }
    if (in.context.sessionMinutes < 0.0 || in.context.sessionMinutes > 1440.0) {
        invalid = true;
    }

    if (invalid) {
        // Fail-closed: out ya está en 0.0 con valid=false, confianza 0.
        return out;
    }

    // ── Valores efectivos (dados o derivados) ────────────────────────
    const bool thdKnown = in.chain.thdPercent > 0.0;
    const bool noiseKnown = in.chain.noiseFloorDb < 0.0;
    const bool ambientKnown = in.room.ambientNoiseDbSpl > 0.0;
    const double thdNorm = thdKnown
        ? clampd(in.chain.thdPercent / 5.0, 0.0, 1.0) : 0.0;
    const double noiseNorm = noiseKnown
        ? clampd((in.chain.noiseFloorDb + 120.0) / 60.0, 0.0, 1.0) : 0.0;
    const double ambientNorm = ambientKnown
        ? clampd((in.room.ambientNoiseDbSpl - 20.0) / 40.0, 0.0, 1.0) : 0.0;
    const double crestNorm = in.signal.crestKnown
        ? clampd(in.signal.crestFactorDb / 20.0, 0.0, 1.0) : 0.0;
    const double transient = in.signal.transientKnown
        ? in.signal.transientDensity : 0.0;
    const double tonal = in.signal.tonalKnown ? in.signal.tonalFraction : 0.0;

    double criticalDistM = 0.0;
    double coherence = 1.0;
    double schroederHz = 0.0;
    bool dcDerived = false;
    bool cohDerived = false;
    if (!headphones) {
        if (in.room.criticalDistanceM > 0.0) {
            criticalDistM = in.room.criticalDistanceM;
        } else {
            // Derivada (Sabine, misma fórmula que el gemelo).
            criticalDistM = 0.057 * std::sqrt(in.room.roomVolumeM3 /
                                              in.room.rt60Sec);
            dcDerived = true;
        }
        if (in.room.coherence >= 0.0) {
            coherence = in.room.coherence;
        } else {
            // Derivada (misma fórmula que el gemelo).
            const double dOverDc = in.listener.distanceM / criticalDistM;
            coherence = 1.0 / (1.0 + dOverDc * dOverDc);
            cohDerived = true;
        }
        schroederHz = 2000.0 * std::sqrt(in.room.rt60Sec /
                                         in.room.roomVolumeM3);
    }
    out.effectiveCriticalDistanceM = criticalDistM;
    out.effectiveCoherence = coherence;
    out.schroederHz = schroederHz;

    // Latencia total: buffer + aire (0 aire en audífono).
    const double airMs = headphones
        ? 0.0
        : in.listener.distanceM / kSpeedOfSoundMs * 1000.0;
    const double totalLatencyMs = in.chain.bufferLatencyMs + airMs;
    out.totalLatencyMs = totalLatencyMs;
    const double latencyNorm = clampd(totalLatencyMs / 50.0, 0.0, 1.0);

    // Desplazamiento de imagen (global, mismo en todas las bandas).
    const double shiftDeg = std::fabs(in.listener.azimuthDeg) +
                            in.chain.channelImbalanceDb * kDegPerDbImbalance;
    out.imageShiftDegrees = shiftDeg;
    const double shiftNorm = clampd(shiftDeg / kImageShiftFullDeg, 0.0, 1.0);

    // Fatiga: términos globales (el término por banda solo escala por nivel).
    const double exposureFactor = in.signal.exposureKnown
        ? clampd(in.signal.exposureMinutes / 120.0, 0.0, 1.0) : 0.0;
    const double levelFactor =
        clampd((in.context.targetSplDb - 85.0) / 20.0, 0.0, 1.0);
    const double sessionTerm =
        0.10 * clampd(in.context.sessionMinutes / 240.0, 0.0, 1.0);
    double contentFactor = 1.0;
    if (in.context.content == ContentType::kVoice) contentFactor = 1.2;
    if (in.context.content == ContentType::kMixed) contentFactor = 1.1;

    // ── Confianza base ──────────────────────────────────────────────
    double baseConfidence = 1.0;
    if (!thdKnown) baseConfidence -= 0.10;
    if (!noiseKnown) baseConfidence -= 0.05;
    if (!in.signal.crestKnown) baseConfidence -= 0.05;
    if (!in.signal.transientKnown) baseConfidence -= 0.05;
    if (!in.signal.tonalKnown) baseConfidence -= 0.03;
    if (!in.signal.exposureKnown) baseConfidence -= 0.05;
    if (!headphones) {
        if (!ambientKnown) baseConfidence -= 0.05;
        if (cohDerived) baseConfidence -= 0.05;
        if (dcDerived) baseConfidence -= 0.05;
        if (in.room.rt60Sec > 2.0) baseConfidence -= 0.20;
        if (in.room.roomVolumeM3 < 5.0 || in.room.roomVolumeM3 > 10000.0) {
            baseConfidence -= 0.20;
        }
    }
    if (!isStandardSampleRate(in.chain.dacSampleRateHz)) baseConfidence -= 0.05;
    if (in.chain.dacBits != 16 && in.chain.dacBits != 24 &&
        in.chain.dacBits != 32) {
        baseConfidence -= 0.05;
    }
    if (in.context.targetSplDb < 40.0 || in.context.targetSplDb > 120.0) {
        baseConfidence -= 0.10;
    }
    if (in.chain.channelImbalanceDb > 3.0) baseConfidence -= 0.10;
    baseConfidence = clampd(baseConfidence, 0.0, 1.0);

    // ── Por banda ───────────────────────────────────────────────────
    double confSum = 0.0;
    for (std::size_t i = 0; i < kHearingBands; ++i) {
        UniversalHearingState& s = out.band[i];
        const double f = kHearingBandHz[i];
        const double rt60Band = headphones
            ? 0.0 : in.room.rt60Sec / kAlphaFactor[i];
        const double rt60Norm = clampd(rt60Band / 1.0, 0.0, 1.0);
        const double levelNorm =
            clampd((in.signal.rmsDbPerBand[i] + 60.0) / 60.0, 0.0, 1.0);
        const double oneMinusCoh = 1.0 - coherence;

        const double depthLoss =
            clampd(0.55 * rt60Norm + 0.45 * oneMinusCoh, 0.0, 1.0);
        const double spatialComp = clampd(
            0.40 * rt60Norm + 0.35 * oneMinusCoh +
                0.25 * (std::fabs(in.listener.azimuthDeg) / 90.0),
            0.0, 1.0);
        const double masking = clampd(
            0.40 * rt60Norm * kLowWeight[i] + 0.25 * levelNorm +
                0.20 * ambientNorm + 0.15 * thdNorm,
            0.0, 1.0);
        const double intelligLoss = clampd(
            contentFactor *
                (0.50 * std::min(rt60Band / 0.8, 1.0) * kMidWeight[i] +
                 0.30 * ambientNorm + 0.20 * oneMinusCoh),
            0.0, 1.0);
        const double microLoss = clampd(
            0.50 * (1.0 - crestNorm) + 0.30 * noiseNorm + 0.20 * thdNorm,
            0.0, 1.0);
        const double causality =
            clampd(0.60 * latencyNorm + 0.40 * rt60Norm, 0.0, 1.0);
        const double fatigue = clampd(
            in.context.priorFatigue +
                exposureFactor * levelFactor * (0.5 + 0.5 * levelNorm) *
                    (1.0 + 0.5 * thdNorm) +
                sessionTerm,
            0.0, 1.0);

        // Confianza por banda: penalización modal bajo Schroeder.
        double conf = baseConfidence;
        if (!headphones && f < schroederHz) conf *= 0.85;

        // Las 9 dims de P[t] derivadas de las pérdidas.
        s[Dim::kAudibility] =
            clampd(0.35 + 0.65 * levelNorm - 0.30 * masking, 0.0, 1.0);
        s[Dim::kClarity] =
            clampd(1.0 - 0.80 * intelligLoss - 0.20 * masking, 0.0, 1.0);
        s[Dim::kNaturalness] =
            clampd(1.0 - 0.40 * thdNorm - 0.30 * masking, 0.0, 1.0);
        s[Dim::kImpact] = clampd(0.35 * transient + 0.35 * crestNorm +
                                     0.20 * levelNorm + 0.10 * (1.0 - tonal),
                                 0.0, 1.0);
        s[Dim::kSpatiality] =
            clampd(1.0 - 0.60 * depthLoss - 0.40 * spatialComp, 0.0, 1.0);
        s[Dim::kFatigue] = fatigue;
        s[Dim::kStability] =
            clampd(1.0 - 0.70 * shiftNorm - 0.30 * in.listener.headMovement,
                   0.0, 1.0);
        s[Dim::kConfidence] = clampd(conf, 0.0, 1.0);
        s[Dim::kBudget] = 0.0;  // el modelo predice; no consume presupuesto.
        // Las 7 pérdidas nuevas de P6.
        s[Dim::kDepthLoss] = depthLoss;
        s[Dim::kSpatialCompression] = spatialComp;
        s[Dim::kTimbralMasking] = masking;
        s[Dim::kIntelligibilityLoss] = intelligLoss;
        s[Dim::kImageShift] = shiftNorm;
        s[Dim::kMicrodynamicsLoss] = microLoss;
        s[Dim::kCausalityLoss] = causality;

        s.clampInPlace();
        confSum += s[Dim::kConfidence];
    }

    // Medias globales por dimensión.
    for (std::size_t d = 0; d < kHearingDims; ++d) {
        double sum = 0.0;
        for (std::size_t i = 0; i < kHearingBands; ++i) sum += out.band[i].v[d];
        out.globalMean[d] = sum / static_cast<double>(kHearingBands);
    }

    out.confidence = confSum / static_cast<double>(kHearingBands);
    out.interventionScale = out.confidence;
    out.valid = true;
    return out;
}

}  // namespace ivanna::fused::universal_hearing
