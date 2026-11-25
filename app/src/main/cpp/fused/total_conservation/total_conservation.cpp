// total_conservation.cpp — P7.1 + P7.2 (ver header para el contrato).
//
// Regla de implementación: un término sin medir devuelve estimated=false.
// Solo dos casos devuelven un número medido hoy:
//   · formato sin pérdida (WAV/FLAC) → Δfuente = 0 por definición;
//   · decodificación bit-exacta → Δdecodificación = 0 por definición;
//   · DSP con los 7 amounts en 0 → Δdsp = 0 (nada activo, nada altera).
// Todo lo demás es "sin estimar" hasta P2 audible / P3 calibrado / P10.
#include "total_conservation.hpp"

#include <cmath>

namespace ivanna::fused::total_conservation {

namespace {

// Confianza del gemelo acústico (P3) a 2026-11-24: ~0.46, por debajo del
// umbral 0.80 de Luna. Mientras no llegue a 0.80, ninguna etapa que
// dependa de P3 puede estimarse. La constante documenta el bloqueo; no
// es un número que el certificado use como si fuera una medición.
inline constexpr double kTwinConfidenceGlobal = 0.46;
inline constexpr double kTwinConfidenceThreshold = 0.80;

StageDelta unestimated(const char* reason) noexcept {
    StageDelta d;
    d.estimated = false;
    d.deltaDb = 0.0;
    d.confidence = 0.0;
    d.reason = reason;
    return d;
}

StageDelta zeroMeasured(const char* reason) noexcept {
    StageDelta d;
    d.estimated = true;
    d.deltaDb = 0.0;
    d.confidence = 1.0;
    d.reason = reason;
    return d;
}

}  // namespace

StageDelta estimateDelta(Stage stage, const StageInput& input) noexcept {
    switch (stage) {
        case Stage::kSource:
            switch (input.codec) {
                case Codec::kWav:
                case Codec::kFlac:
                    return zeroMeasured("sin perdida: formato sin perdida");
                case Codec::kMp3:
                case Codec::kAac:
                case Codec::kOpus:
                    return unestimated(
                        "sin estimar: perdida del codec con perdida no "
                        "medida en esta cadena");
                case Codec::kUnknown:
                default:
                    return unestimated(
                        "sin estimar: formato no identificado");
            }
        case Stage::kDecode:
            if (input.decodeBitExact) {
                return zeroMeasured(
                    "bit-exacto: sin resampling ni cambio de sample rate");
            }
            return unestimated(
                "sin estimar: resampling/cambio de sample rate no medido "
                "en host");
        case Stage::kDsp: {
            bool anyActive = false;
            for (double a : input.operatorAmounts) {
                if (!std::isfinite(a)) {
                    // Fail-closed: un amount no finito no permite afirmar
                    // que nada altera; el término queda sin estimar.
                    return unestimated(
                        "sin estimar: amount de operador no finito");
                }
                if (a > 0.0) {
                    anyActive = true;
                }
            }
            if (!anyActive) {
                return zeroMeasured(
                    "sin DSP activo: amounts en 0, nada altera");
            }
            return unestimated(
                "sin estimar: alteracion por operador no medida en el "
                "banco adversarial (P1/P2 audible pendiente)");
        }
        case Stage::kConversion:
            return unestimated(
                "sin estimar: ruido y linealidad del DAC JieLi sin medir "
                "(P3, gemelo 0.46 < 0.80)");
        case Stage::kAmplification:
            return unestimated(
                "sin estimar: ganancia/compresion/ruido del Genezi sin "
                "medir (P3; potencia y sensibilidad SIN MEDIR)");
        case Stage::kTransducer:
            return unestimated(
                "sin estimar: respuesta de las bocinas del Genezi sin "
                "medir (P3; sensibilidad SIN MEDIR)");
        case Stage::kRoom:
            return unestimated(
                "sin estimar: modos y RT60 de la sala 4.0x7.0 sin medir "
                "(P3, gemelo 0.46 < 0.80)");
        case Stage::kPosition:
            return unestimated(
                "sin estimar: delta por distancia/desplazamiento sin "
                "calibrar (P3)");
        case Stage::kPerception:
            return unestimated(
                "sin estimar: fatiga/atencion sin modelo (P10 no existe)");
    }
    return unestimated("sin estimar: etapa desconocida");
}

PlaybackCertificate buildCertificate(const StageInput& input,
                                     std::uint64_t trackIndex,
                                     std::uint32_t ivannaVersionCode) noexcept {
    PlaybackCertificate cert;
    cert.trackIndex = trackIndex;
    cert.ivannaVersionCode = ivannaVersionCode;
    cert.totalDb = 0.0;
    cert.estimatedStages = 0;
    cert.confidence = 1.0;
    for (std::size_t s = 0; s < kNumStages; ++s) {
        Stage stage = static_cast<Stage>(s);
        cert.stages[s] = estimateDelta(stage, input);
        const StageDelta& d = cert.stages[s];
        if (d.estimated) {
            ++cert.estimatedStages;
            cert.totalDb += d.deltaDb;
            if (d.confidence < cert.confidence) {
                cert.confidence = d.confidence;
            }
        }
    }
    cert.totalValid = (cert.estimatedStages == kNumStages);
    if (!cert.totalValid) {
        // Con etapas sin estimar, el total es un piso y la confianza es 0:
        // el certificado no finge saber lo que no sabe.
        cert.confidence = 0.0;
    }
    // Suprimir el warning de variable sin uso si el umbral cambia: el
    // bloqueo P3 está documentado arriba y en el README.
    (void)kTwinConfidenceGlobal;
    (void)kTwinConfidenceThreshold;
    return cert;
}

const char* stageName(Stage stage) noexcept {
    switch (stage) {
        case Stage::kSource: return "fuente";
        case Stage::kDecode: return "decodificacion";
        case Stage::kDsp: return "dsp";
        case Stage::kConversion: return "conversion";
        case Stage::kAmplification: return "amplificacion";
        case Stage::kTransducer: return "transductor";
        case Stage::kRoom: return "sala";
        case Stage::kPosition: return "posicion";
        case Stage::kPerception: return "percepcion";
    }
    return "desconocida";
}

const char* familyName(Family family) noexcept {
    switch (family) {
        case Family::kTimbre: return "timbre";
        case Family::kTime: return "tiempo";
        case Family::kSpace: return "espacio";
        case Family::kDynamics: return "dinamica";
        case Family::kTexture: return "textura";
        case Family::kAttention: return "atencion";
    }
    return "desconocida";
}

}  // namespace ivanna::fused::total_conservation
