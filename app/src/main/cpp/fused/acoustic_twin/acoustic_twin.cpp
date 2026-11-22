// acoustic_twin.cpp — Gemelo acústico del trayecto completo (2026-11-21).
//
// Física usada (todo documentado como MODELO, no como medición):
//   · Sabine: RT60 = 0.161·V/A; la absorción media se DERIVA del RT60
//     medido (alpha = 0.161·V/(S·RT60)) y se reparte por banda con un
//     factor nominal (la absorción sube con la frecuencia).
//   · Ganancia total en escucha: 10·log10(1/d² + 4/A) — campo directo
//     (ley del inverso del cuadrado) + campo reverberante. Es la
//     relación estándar de acústica de salas, no un ajuste.
//   · Distancia crítica Dc = 0.057·√(V/RT60); coherencia =
//     1/(1+(d/Dc)²) = directo/(directo+reverberante).
//   · Schroeder fs = 2000·√(RT60/V): por debajo, la sala es modal y el
//     RT60 por banda pierde significado → la confianza baja.
//   · Latencia = buffer del DAC + propagación por aire (d/343).
//   · Rango dinámico del DAC = 6.02·bits + 1.76 dB (teórico).
//   · THD y compresión por MARGEN DE POTENCIA a cada SPL: la potencia
//     requerida para el SPL objetivo sale de sensibilidad + ganancia de
//     trayecto; el headroom (Pmax/Preq) alimenta un modelo suave de THD
//     (0.05 % a 10 dB de margen, se duplica cada 6 dB menos) y una
//     rodilla suave de compresión (3 dB de margen, 0.5 dB/dB, tope 12 dB).
//     Son MODELOS nominales: dan tendencias y órdenes de magnitud, no
//     el THD medido del Genezi (que no está medido).
//   · Confianza: penalizaciones por lo que NO se sabe (potencia y
//     sensibilidad desconocidas, RT60 inconsistente con la sala,
//     distancia fuera de la sala, SPL inalcanzable) y por la región
//     modal. interventionScale = confidence (a confianza baja, se
//     interviene MENOS — corrección del diseño de ivanna-free, que lo
//     tenía invertido).
// Fail-closed: entradas NaN/infinitas se sanean a "desconocido" (0) y
// jamás producen NaN en la salida; si no se puede estimar nada,
// valid=false y confianza 0.
#include "acoustic_twin.hpp"

#include <algorithm>
#include <cmath>

namespace ivanna::fused::acoustic_twin {
namespace {

// Factor de absorción relativa por banda (1.0 a 1 kHz). MODELO nominal:
// la absorción típica de una sala amueblada sube con la frecuencia.
// No es una medición de la sala de Esse.
constexpr double kAlphaFactor[kTwinBands] = {
    0.65, 0.75, 0.85, 0.95, 1.00, 1.05, 1.15, 1.25, 1.35};

// Forma espectral nominal del ruido ambiente (dB relativos a 500 Hz–1 kHz).
// El ruido de sala es grave-pesado. MODELO, no medición.
constexpr double kNoiseShapeDb[kTwinBands] = {
    6.0, 4.0, 2.0, 0.0, 0.0, -2.0, -4.0, -6.0, -8.0};

double clampd(double x, double lo, double hi) noexcept {
    return std::max(lo, std::min(hi, x));
}

// NaN/inf → valor de reserva (0 = "desconocido"). Fail-closed.
double finiteOr(double x, double fallback) noexcept {
    return std::isfinite(x) ? x : fallback;
}

bool isStandardSampleRate(int sr) noexcept {
    switch (sr) {
        case 44100: case 48000: case 88200: case 96000:
        case 176400: case 192000: case 352800: case 384000:
            return true;
        default: return false;
    }
}

// THD estimado por margen de potencia (headroom en dB). MODELO nominal:
// 0.05 % con 10 dB de margen, se duplica cada 6 dB menos de margen.
double thdFromHeadroomDb(double headroomDb) noexcept {
    const double thd = 0.05 * std::pow(2.0, (10.0 - headroomDb) / 6.0);
    return clampd(thd, 0.01, 30.0);
}

// Compresión estimada: rodilla suave a 3 dB de margen, 0.5 dB de
// compresión por cada dB por debajo de la rodilla, tope 12 dB. MODELO.
double compressionFromHeadroomDb(double headroomDb) noexcept {
    return clampd((3.0 - headroomDb) * 0.5, 0.0, 12.0);
}

}  // namespace

AcousticTwinParams AcousticTwinParams::esseGenezi() noexcept {
    AcousticTwinParams p;
    p.dacBits = 24;
    p.dacSampleRateHz = 48000;
    p.dacBufferFrames = 128;
    p.ampMaxPowerW = 0.0;  // desconocida (no medida)
    p.ampGainDb = 0.0;     // flat
    p.subwooferOn = true;  // sub ON neutro
    p.subCrossoverHz = 80.0;
    p.transducer = TwinTransducer::kSpeaker;
    p.sensitivityDb = 0.0;  // desconocida (no medida)
    p.roomWidthM = 4.0;
    p.roomLengthM = 7.0;
    p.roomHeightM = 2.5;
    p.roomRt60Sec = 0.34;
    p.listenerDistanceM = 3.0;
    p.ambientNoiseDbSpl = 30.0;
    p.targetSplDb = 85.0;
    return p;
}

AcousticTwinParams AcousticTwinParams::esseGeneziCalibrated() noexcept {
    // Valores de la calibración P3 ESTIMADA (datasheet Sony MHC-PZ1D +
    // física exacta, genezi_pz1d_estimado_datasheet.json). DERIVADOS, no
    // medidos: usar con estimateCalibrated(), que impone la confianza
    // honesta (0.70). Si se usa con estimate() a secas, la fórmula da
    // 0.926: ese número NO es la confianza de la calibración.
    AcousticTwinParams p = esseGenezi();
    p.ampMaxPowerW = 135.0;  // datasheet (135 W + 135 W, 6 ohm, 1 kHz, 1% THD)
    p.sensitivityDb = 88.0;  // DERIVADO (centro de la clase, frontal)
    p.satellitesOn = false;  // config actual: surround OFF
    p.satelliteF3Hz = 157.5;       // DERIVADO (satélite SS-RSZ999D)
    p.satelliteF3HighHz = 16000.0; // DERIVADO (cono 80 mm)
    p.satelliteSensitivityDb = 84.0;  // DERIVADO (centro de la clase)
    return p;
}

AcousticTwinParams AcousticTwinParams::esseHeadphones() noexcept {
    AcousticTwinParams p = esseGenezi();
    p.transducer = TwinTransducer::kHeadphone;
    p.subwooferOn = false;  // sin sub en audífono
    return p;
}

// ── Calibración P3 ────────────────────────────────────────────────────
// Fuente de verdad:
//   goals/majestuosidad-total-perceptual-de-ivanna/calibration_p3/
//   genezi_pz1d_estimado_datasheet.json
// (formato IVANNA_TWIN_CALIBRATION 1.0, ESTIMADO, 2026-11-21).

bool TwinCalibration::isValid() const noexcept {
    // Confianza honesta y por componente en [0,1] y finitas.
    if (!std::isfinite(honestConfidence) || honestConfidence < 0.0 ||
        honestConfidence > 1.0) {
        return false;
    }
    const double comps[5] = {directPathConfidence, bassConfidence,
                             midConfidence, trebleConfidence, roomConfidence};
    for (double c : comps) {
        if (!std::isfinite(c) || c < 0.0 || c > 1.0) return false;
    }
    // Una calibración estimada NUNCA puede declarar confianza 1.0
    // (sería fingir una medición perfecta).
    if (isEstimated && honestConfidence >= 1.0) return false;
    // Respuesta en rango cuerdo (±60 dB cubre cualquier sala real).
    if (responseKnown) {
        for (double r : responseDb) {
            if (!std::isfinite(r) || r < -60.0 || r > 12.0) return false;
        }
    }
    // Satélite cuerdo si se declara conocido.
    if (satelliteKnown) {
        if (!std::isfinite(satelliteF3Hz) || satelliteF3Hz <= 0.0 ||
            satelliteF3Hz > 20000.0) {
            return false;
        }
        if (!std::isfinite(satelliteF3HighHz) || satelliteF3HighHz <= 0.0 ||
            satelliteF3HighHz > 48000.0 ||
            satelliteF3HighHz <= satelliteF3Hz) {
            return false;
        }
        if (!std::isfinite(satelliteSensitivityDb) ||
            satelliteSensitivityDb <= 0.0 || satelliteSensitivityDb > 120.0) {
            return false;
        }
        for (double r : satelliteResponseDb) {
            if (!std::isfinite(r) || r < -60.0 || r > 12.0) return false;
        }
    }
    return true;
}

TwinCalibration TwinCalibration::geneziPz1dEstimated() noexcept {
    // Transcripción fiel de genezi_pz1d_estimado_datasheet.json
    // (response_twin_bands_9, confidence, speakers.satellite_SS-RSZ999D).
    // Si el JSON cambia, ESTA función cambia con él (misma fuente).
    TwinCalibration c;
    c.responseDb = {-3.24, 1.97, -0.10, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    c.responseKnown = true;
    c.honestConfidence = 0.70;
    c.directPathConfidence = 0.85;
    c.bassConfidence = 0.65;
    c.midConfidence = 0.70;
    c.trebleConfidence = 0.60;
    c.roomConfidence = 0.70;
    c.isEstimated = true;
    c.satelliteKnown = true;
    c.satelliteF3Hz = 157.5;
    c.satelliteF3HighHz = 16000.0;
    c.satelliteSensitivityDb = 84.0;
    c.satelliteResponseDb = {-31.84, -8.66, -0.11, 0.0, 0.0,
                             0.0, -0.02, -0.26, -3.01};
    c.valid = c.isValid();
    return c;
}

TwinCalibration TwinCalibration::geneziPz1dMeasured() noexcept {
    // Respuesta en magnitud MEDIDA desde el barrido Farina de la app
    // (2026-11-22 05:54 CST): ESS limpio 20 Hz-20 kHz capturado por
    // GeneziSweepRecorder (WAV 48 kHz, sin compresión). Método: RMS
    // por ventana de 0.25 s siguiendo la frecuencia del ESS
    // (exponencial 20 Hz -> 20 kHz en 10 s), normalizado por el RMS
    // del ESS fuente en cada ventana, dos barridos independientes
    // (lo/hi) promediados. Lo y hi coinciden dentro de ~1 dB en
    // 125 Hz-2 kHz, así que la medida es coherente.
    //   63 Hz -1.90 | 125 Hz -0.76 | 250 Hz -3.71 | 500 Hz -2.99 |
    //   1000 Hz 0.0 | 2000 Hz -10.51 | 4000 Hz -17.05 | 8000 Hz -7.00
    // 16 kHz: del ESTIMADO (0.0 dB). El barrido mide −30.7 dB a
    // 16 kHz, pero eso es el recorte del micrófono del teléfono,
    // NO el Genezi (su super-tweeter llega a 24 kHz). Usar −30 dB
    // como respuesta del Genezi sería falso.
    // La caída a 2–4 kHz (−10.5/−17 dB) es real en la grabación
    // (lo y hi la muestran igual), pero puede ser el cruce del
    // Genezi o un nulo por la posición del micrófono; no se puede
    // asegurar que sea propiedad del Genezi sin medir en otra
    // posición. Se usa el valor medido (es el mejor dato) y la
    // confianza de agudos (0.60) refleja la duda.
    // Potencia (135 W), sensibilidad (88 dB) y RT60 (0.34 s) siguen
    // del estimado: el barrido no los mide (la deconvolución Farina
    // es inestable con estos datos: RT60 2.7–6.6 s, absurdo para
    // 70 m³; el pico directo salta con la alineación). Por eso la
    // confianza honesta es 0.80, no 0.90+: la respuesta 63 Hz-8 kHz
    // ya es medida en WAV limpio (mejora real sobre la nota de voz
    // 0.75, que era AAC), pero falta RT60, potencia, sensibilidad
    // y 16 kHz medidos.
    // Fuente: goals/majestuosidad-total-perceptual-de-ivanna/
    //   calibration_p3/sweep_2026-11-22_0554/INFORME_BARRIDO_2026-11-22_0554.md
    TwinCalibration c;
    c.responseDb = {-1.90, -0.76, -3.71, -2.99, 0.0, -10.51, -17.05, -7.00, 0.0};
    c.responseKnown = true;
    c.honestConfidence = 0.80;
    c.directPathConfidence = 0.85;
    c.bassConfidence = 0.85;   // medido en WAV limpio, lo/hi a ~2 dB
    c.midConfidence = 0.80;    // medido (250 Hz-1 kHz a ~1 dB; 2 kHz con duda)
    c.trebleConfidence = 0.60; // 4 kHz con duda de posición; 16 kHz no medido
    c.roomConfidence = 0.70;   // RT60 no medido de forma fiable
    c.isEstimated = false;     // la respuesta principal es MEDIDA
    c.satelliteKnown = true;
    c.satelliteF3Hz = 157.5;
    c.satelliteF3HighHz = 16000.0;
    c.satelliteSensitivityDb = 84.0;
    c.satelliteResponseDb = {-31.84, -8.66, -0.11, 0.0, 0.0,
                             0.0, -0.02, -0.26, -3.01};
    c.valid = c.isValid();
    return c;
}

TwinCalibration TwinCalibration::geneziPz1dVoiceNote() noexcept {
    // Respuesta en magnitud MEDIDA desde la nota de voz de Esse
    // (2026-11-22 01:0x CST): 3 barridos ESS grabados con el grabador
    // de notas aparte (AAC 44.1 kHz estéreo). Método: RMS por ventana
    // de 0.25 s en la frecuencia dominante del barrido (FFT), 3
    // barridos promediados, bandas de 1/3 de octava, relativo a 1 kHz.
    //   63 Hz -6.22 | 125 Hz +1.17 | 250 Hz -3.53 | 500 Hz -1.31 |
    //   1000 Hz 0.0 | 2000 Hz -1.72
    // 4-16 kHz: del ESTIMADO (0.0 dB). La nota de voz mide -7.76 dB
    // a 4 kHz y -10.30 dB a 8 kHz, pero eso es el filtro pasa-bajas
    // del grabador de voz / micrófono, NO el Genezi (su
    // super-tweeter llega a 24 kHz según datasheet). Usar esos
    // valores como respuesta del Genezi sería falso.
    // Potencia (135 W), sensibilidad (88 dB) y RT60 (0.34 s) siguen
    // del estimado: la nota de voz no los mide. La deconvolución
    // Farina completa no funciona en AAC (destruye la fase: directo
    // a 450 ms, 63 Hz a +69 dB, artefactos). Por eso la confianza
    // honesta es 0.75, no 0.80+: la respuesta en graves/medios ya
    // es medida (mejora real sobre el 0.70 estimado), pero falta
    // potencia, sensibilidad, RT60 y agudos medidos.
    // Fuente: goals/majestuosidad-total-perceptual-de-ivanna/
    //   calibration_p3/voicenote_2026-11-22/respuesta_rms_voicenote.json
    TwinCalibration c;
    c.responseDb = {-6.22, 1.17, -3.53, -1.31, 0.0, -1.72, 0.0, 0.0, 0.0};
    c.responseKnown = true;
    c.honestConfidence = 0.75;
    c.directPathConfidence = 0.85;
    c.bassConfidence = 0.80;   // medido (nota de voz, 63/125 Hz)
    c.midConfidence = 0.80;    // medido (nota de voz, 250 Hz-2 kHz)
    c.trebleConfidence = 0.60; // NO medido (grabador recorta agudos)
    c.roomConfidence = 0.70;   // RT60 no medido de forma fiable
    c.isEstimated = false;     // la respuesta principal es MEDIDA
    c.satelliteKnown = true;
    c.satelliteF3Hz = 157.5;
    c.satelliteF3HighHz = 16000.0;
    c.satelliteSensitivityDb = 84.0;
    c.satelliteResponseDb = {-31.84, -8.66, -0.11, 0.0, 0.0,
                             0.0, -0.02, -0.26, -3.01};
    c.valid = c.isValid();
    return c;
}

AcousticTwinEstimate AcousticTwin::estimateCalibrated(
    const AcousticTwinParams& p, const TwinCalibration& cal) noexcept {
    // Fail-closed: calibración inválida -> estimación sin calibrar.
    if (!cal.isValid()) return estimate(p);
    // Parámetros con los valores de la calibración (la calibración no
    // cambia la configuración: satellitesOn lo decide el usuario).
    AcousticTwinParams p2 = p;
    // La calibración P3 trae potencia y sensibilidad derivadas. Solo se
    // aplican si el llamador no las dio (0 = desconocido): un valor
    // medido del usuario SIEMPRE manda sobre el derivado.
    if (p2.ampMaxPowerW <= 0.0) p2.ampMaxPowerW = 135.0;
    if (p2.sensitivityDb <= 0.0) p2.sensitivityDb = 88.0;
    if (cal.satelliteKnown) {
        if (p2.satelliteF3Hz <= 0.0) p2.satelliteF3Hz = cal.satelliteF3Hz;
        if (p2.satelliteF3HighHz <= 0.0) {
            p2.satelliteF3HighHz = cal.satelliteF3HighHz;
        }
        if (p2.satelliteSensitivityDb <= 0.0) {
            p2.satelliteSensitivityDb = cal.satelliteSensitivityDb;
        }
    }
    AcousticTwinEstimate out = estimate(p2);
    // Si los parámetros no permiten estimar, la calibración no lo arregla.
    if (!out.valid) return out;
    out.calibrationApplied = true;
    out.calibrationConfidence = cal.honestConfidence;
    // Ganancia por banda: la respuesta de calibración es RELATIVA
    // (0 dB = nivel suave a 1 kHz). Se ancla a la ganancia del gemelo
    // a 1 kHz (banda 4) para no duplicar el refuerzo del sub, que ya
    // está en la ganancia base por debajo del cruce.
    if (cal.responseKnown) {
        const double anchorDb = out.band[4].gainDb;  // 1000 Hz
        for (int i = 0; i < kTwinBands; ++i) {
            out.band[i].gainDb = anchorDb + cal.responseDb[i];
            out.calibratedResponseDb[i] = cal.responseDb[i];
        }
    }
    // Confianza HONESTA (la de la calibración, no la de la fórmula).
    // Mapeo por banda: 63/125 Hz -> graves; 250 Hz-2 kHz -> medios;
    // 4-16 kHz -> agudos. El global es el de la calibración (0.70),
    // no la media de las bandas (el global es la media de los 5
    // componentes, que incluyen directo y sala, no solo bandas).
    for (int i = 0; i < kTwinBands; ++i) {
        double conf = cal.midConfidence;
        if (i <= 1) conf = cal.bassConfidence;
        if (i >= 6) conf = cal.trebleConfidence;
        out.band[i].confidence = clampd(conf, 0.0, 1.0);
        out.band[i].interventionScale = out.band[i].confidence;
    }
    out.overallConfidence = clampd(cal.honestConfidence, 0.0, 1.0);
    out.overallInterventionScale = out.overallConfidence;
    // Satélite (componente separado): solo si está encendido y se conoce.
    if (p2.satellitesOn && cal.satelliteKnown) {
        out.satellitesActive = true;
        for (int i = 0; i < kTwinBands; ++i) {
            out.satelliteGainDb[i] = cal.satelliteResponseDb[i];
        }
    }
    return out;
}

AcousticTwinEstimate AcousticTwin::estimate(const AcousticTwinParams& p) noexcept {
    return estimateAtSpl(p, p.targetSplDb);
}

AcousticTwinEstimate AcousticTwin::estimateAtSpl(const AcousticTwinParams& pIn,
                                                 double splDbIn) noexcept {
    // Saneamiento fail-closed: ninguna entrada no-finita llega a las fórmulas.
    AcousticTwinParams p = pIn;
    p.ampMaxPowerW = finiteOr(p.ampMaxPowerW, 0.0);
    p.ampGainDb = finiteOr(p.ampGainDb, 0.0);
    p.subCrossoverHz = finiteOr(p.subCrossoverHz, 0.0);
    p.sensitivityDb = finiteOr(p.sensitivityDb, 0.0);
    p.roomWidthM = finiteOr(p.roomWidthM, 0.0);
    p.roomLengthM = finiteOr(p.roomLengthM, 0.0);
    p.roomHeightM = finiteOr(p.roomHeightM, 0.0);
    p.roomRt60Sec = finiteOr(p.roomRt60Sec, 0.0);
    p.listenerDistanceM = finiteOr(p.listenerDistanceM, 0.0);
    p.ambientNoiseDbSpl = finiteOr(p.ambientNoiseDbSpl, 0.0);
    p.targetSplDb = finiteOr(p.targetSplDb, 0.0);
    const double splDb = finiteOr(splDbIn, 85.0);

    AcousticTwinEstimate out;
    for (int i = 0; i < kTwinBands; ++i) out.band[i].freqHz = kTwinBandHz[i];

    const bool headphones = (p.transducer == TwinTransducer::kHeadphone);

    // ── Validación dura: sin esto no se puede estimar nada ──────────
    bool hardInvalid = false;
    if (p.dacBits <= 0 || p.dacSampleRateHz <= 0 || p.dacBufferFrames <= 0) {
        hardInvalid = true;
    }
    if (!headphones) {
        if (p.roomWidthM <= 0.0 || p.roomLengthM <= 0.0 || p.roomHeightM <= 0.0) {
            hardInvalid = true;
        }
        if (p.roomRt60Sec <= 0.0) hardInvalid = true;
        if (p.listenerDistanceM <= 0.0) hardInvalid = true;
    }
    if (hardInvalid) {
        out.valid = false;
        out.overallConfidence = 0.0;
        out.overallInterventionScale = 0.0;
        return out;
    }
    out.valid = true;

    // ── Geometría de la sala (solo altavoz) ─────────────────────────
    double volume = 0.0, surface = 0.0, alphaMid = 0.0;
    double schroederHz = 0.0, criticalDistM = 0.0;
    if (!headphones) {
        volume = p.roomWidthM * p.roomLengthM * p.roomHeightM;
        surface = 2.0 * (p.roomWidthM * p.roomLengthM +
                         p.roomWidthM * p.roomHeightM +
                         p.roomLengthM * p.roomHeightM);
        // Sabine invertida: absorción media a partir del RT60 medido.
        alphaMid = kSabineConstant * volume / (surface * p.roomRt60Sec);
        schroederHz = 2000.0 * std::sqrt(p.roomRt60Sec / volume);
        criticalDistM = 0.057 * std::sqrt(volume / p.roomRt60Sec);
        out.roomVolumeM3 = volume;
        out.roomSurfaceM2 = surface;
        out.schroederHz = schroederHz;
        out.criticalDistanceM = criticalDistM;
    }

    // ── Latencia ────────────────────────────────────────────────────
    const double bufferMs = static_cast<double>(p.dacBufferFrames) * 1000.0 /
                            static_cast<double>(p.dacSampleRateHz);
    const double airMs = headphones
                             ? 0.0
                             : p.listenerDistanceM / kSpeedOfSoundMs * 1000.0;
    out.bufferLatencyMs = bufferMs;
    out.airLatencyMs = airMs;

    // ── Rango dinámico teórico del DAC ──────────────────────────────
    const double dacDrDb = 6.02 * static_cast<double>(p.dacBits) + 1.76;

    // ── Confianza base (penalizaciones por lo que no se sabe) ───────
    double baseConfidence = 1.0;
    if (p.ampMaxPowerW <= 0.0) baseConfidence -= 0.25;   // potencia desconocida
    if (p.sensitivityDb <= 0.0) baseConfidence -= 0.25;  // sensibilidad desconocida
    if (!isStandardSampleRate(p.dacSampleRateHz)) baseConfidence -= 0.10;
    if (p.dacBits != 16 && p.dacBits != 24 && p.dacBits != 32) baseConfidence -= 0.10;
    if (p.dacBufferFrames > 8192) baseConfidence -= 0.10;
    if (splDb < 40.0 || splDb > 120.0) baseConfidence -= 0.20;
    if (p.ambientNoiseDbSpl < 0.0 || p.ambientNoiseDbSpl > 80.0) baseConfidence -= 0.10;
    if (p.subwooferOn && p.subCrossoverHz <= 0.0) baseConfidence -= 0.05;
    if (!headphones) {
        if (p.roomRt60Sec > 2.0) baseConfidence -= 0.30;
        if (volume < 5.0 || volume > 10000.0) baseConfidence -= 0.30;
        const double diag = std::sqrt(p.roomWidthM * p.roomWidthM +
                                      p.roomLengthM * p.roomLengthM +
                                      p.roomHeightM * p.roomHeightM);
        if (p.listenerDistanceM > diag) baseConfidence -= 0.30;
        // RT60 inconsistente con la sala (absorción fuera de lo físico).
        if (alphaMid < 0.02 || alphaMid > 0.9) baseConfidence -= 0.20;
    }
    const bool powerKnown = (p.ampMaxPowerW > 0.0 && p.sensitivityDb > 0.0);
    baseConfidence = clampd(baseConfidence, 0.0, 1.0);

    // ── Por banda ───────────────────────────────────────────────────
    double confSum = 0.0;
    for (int i = 0; i < kTwinBands; ++i) {
        AcousticTwinBand& b = out.band[i];
        const double f = kTwinBandHz[i];
        b.latencyMs = bufferMs + airMs;
        b.noiseDbSpl = p.ambientNoiseDbSpl + kNoiseShapeDb[i];

        double conf = baseConfidence;
        if (!headphones) {
            // Absorción por banda (modelo nominal) y RT60 Sabine.
            const double alpha = clampd(alphaMid * kAlphaFactor[i], 0.01, 0.95);
            const double absorption = surface * alpha;  // sabins
            b.rt60Sec = kSabineConstant * volume / absorption;
            // Ganancia total: campo directo (1/d²) + reverberante (4/A).
            const double direct = 1.0 / (p.listenerDistanceM * p.listenerDistanceM);
            const double reverb = 4.0 / absorption;
            b.gainDb = 10.0 * std::log10(direct + reverb);
            // Subwoofer neutro: +3 dB nominal por debajo del cruce.
            if (p.subwooferOn && f <= p.subCrossoverHz) b.gainDb += 3.0;
            // Coherencia: directo / (directo + reverberante).
            const double dOverDc = p.listenerDistanceM / criticalDistM;
            b.coherence = 1.0 / (1.0 + dOverDc * dOverDc);
            // Región modal: por debajo de Schroeder el RT60 es menos fiable.
            if (f < schroederHz) conf *= 0.7;
            if (i == 0) conf *= 0.9;  // 63 Hz, la más modal
        } else {
            b.rt60Sec = 0.0;
            b.gainDb = 0.0;
            b.coherence = 1.0;
        }

        if (powerKnown) {
            // Potencia requerida para el SPL objetivo en esta banda.
            double reqW = 0.0;
            if (!headphones) {
                reqW = std::pow(10.0, (splDb - p.sensitivityDb - b.gainDb) / 10.0);
            } else {
                // Audífono: sensibilidad en dB SPL @ 1 mW.
                const double reqMw = std::pow(10.0, (splDb - p.sensitivityDb) / 10.0);
                reqW = reqMw / 1000.0;
            }
            const double headroomDb = 10.0 * std::log10(p.ampMaxPowerW / reqW);
            b.maxSplKnown = true;
            if (!headphones) {
                b.maxSplDb = p.sensitivityDb +
                             10.0 * std::log10(p.ampMaxPowerW) + b.gainDb;
            } else {
                b.maxSplDb = p.sensitivityDb +
                             10.0 * std::log10(p.ampMaxPowerW * 1000.0);
            }
            b.thdPercent = thdFromHeadroomDb(headroomDb);
            b.compressionDb = compressionFromHeadroomDb(headroomDb);
            b.dynamicRangeDb = b.maxSplDb - b.noiseDbSpl;
            // SPL objetivo inalcanzable: el headroom no es fiable.
            if (headroomDb < 0.0) conf -= 0.20;
        } else {
            b.maxSplKnown = false;
            b.maxSplDb = 0.0;
            b.thdPercent = 0.0;     // no estimable (no cero real)
            b.compressionDb = 0.0;  // no estimable
            b.dynamicRangeDb = dacDrDb;  // techo del DAC, no el usable en sala
        }

        b.confidence = clampd(conf, 0.0, 1.0);
        // CORRECCIÓN del diseño de ivanna-free: a confianza baja se
        // interviene MENOS. interventionScale = confidence.
        b.interventionScale = b.confidence;
        confSum += b.confidence;
    }

    out.overallConfidence = confSum / static_cast<double>(kTwinBands);
    out.overallInterventionScale = out.overallConfidence;
    return out;
}

}  // namespace ivanna::fused::acoustic_twin
