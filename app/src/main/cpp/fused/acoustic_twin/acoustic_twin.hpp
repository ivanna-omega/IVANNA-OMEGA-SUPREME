// acoustic_twin.hpp — Gemelo acústico del trayecto completo (2026-11-21).
//
// Próximo salto magistral, parte 2 (plan de luna, cxa/gpt-5.6-luna,
// 2026-11-21): modelar el trayecto físico completo
//   fuente → DSP → mezcla → DAC → amplificador → transductor → sala → oído
// para que el motor sepa QUÉ está oyendo el usuario en realidad, no solo
// qué señal le entrega al DAC.
//
// Este módulo SOLO ESTIMA. No procesa audio, no toca ningún buffer, no
// tiene estado global, no hace I/O: es una función pura (parámetros de
// entrada → estimación de salida). Por eso es seguro llamarlo desde
// cualquier hilo, incluido el de audio, y por eso su test puede probar que
// la señal no se modifica (no hay camino de audio que modificar).
//
// ADAPTACIÓN HONESTA del diseño de ivanna-free (omniroute/auto/best-free,
// 2026-11-21, 17.7 s): su esqueleto era correcto en estructura (SystemParams
// → BandMetrics, Sabine + ley del inverso del cuadrado, confianza por
// validación de entrada) pero tenía TRES defectos que aquí se corrigen:
//   1. Su `interventionFactor = 1 - confidence` estaba INVERTIDO: con
//      confianza baja hay que intervenir MENOS, no más. Aquí
//      `interventionScale = confidence` (0 = no intervenir, 1 = intervenir
//      pleno). El que lo use debe multiplicar su corrección por este factor.
//   2. Su `noiseDbSpl = 20 + 10*log10(roomVol)` y su THD por tramos eran
//      números sin física detrás. Aquí el ruido es un parámetro de entrada
//      con forma espectral documentada, y el THD/compresión salen del
//      margen de potencia (headroom) real a cada SPL.
//   3. No distinguía altavoz de audífono. Aquí el audífono no tiene
//      distancia, ni aire, ni sala (coherencia 1, RT60 0, ganancia 0).
//
// TODO LO QUE ES MODELO, NO MEDICIÓN, VA DOCUMENTADO COMO TAL. La
// absorción por banda, la forma espectral del ruido, el THD por headroom
// y la compresión son MODELOS con constantes nominales: dan órdenes de
// magnitud y tendencias (monotonía con el SPL, caída de coherencia con la
// distancia), no valores calibrados del Genezi real. La potencia del
// Genezi y la sensibilidad de sus bocinas NO están medidas: en el
// preset de Esse quedan en 0 (desconocidas) y la confianza lo refleja
// (el módulo no finge saber lo que no sabe).
//
// Parámetros del trayecto real de Esse (MEMORY.md): DAC USB JieLi
// 24-bit/48 kHz, Sony Genezi MHC-PZ1D (estéreo, flat, sub ON neutro,
// surround OFF), sala 4.0×7.0 m, escucha a 3 m, SALA VIVA 0.34 s / 7.0 m
// / 0.22. El preset `esseGenezi()` los trae como entrada; el módulo los
// acepta como parámetros, no los hardcodea (cualquier otro equipo/sala
// se estima con los mismos campos).
#pragma once

#include <array>
#include <cstdint>

namespace ivanna::fused::acoustic_twin {

// 9 bandas logarítmicas, 63 Hz .. 16 kHz.
inline constexpr int kTwinBands = 9;
inline constexpr double kTwinBandHz[kTwinBands] = {
    63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0};

// Velocidad del sonido en aire (m/s, 20 °C). Constante física estándar.
inline constexpr double kSpeedOfSoundMs = 343.0;
// Constante de Sabine (s·m⁻¹): RT60 = 0.161 · V / A.
inline constexpr double kSabineConstant = 0.161;

enum class TwinTransducer : std::uint8_t {
    kSpeaker = 0,    // Altavoz/bocina: hay distancia, aire y sala.
    kHeadphone = 1,  // Audífono: sin distancia, sin aire, sin sala.
};

// Parámetros del trayecto real. Todo es ENTRADA. Un campo numérico en 0
// (o negativo) donde se marca "0 = desconocido" significa "no se conoce":
// baja la confianza, NO se inventa un valor.
struct AcousticTwinParams {
    // ── DAC ──────────────────────────────────────────────────────────
    int dacBits{24};            // 16 / 24 / 32
    int dacSampleRateHz{48000}; // 44100 / 48000 / 96000 / ...
    int dacBufferFrames{128};  // latencia de buffer (hop STFT = 128)

    // ── Amplificador ─────────────────────────────────────────────────
    double ampMaxPowerW{0.0};  // potencia máxima por canal (W). 0 = desconocida.
    double ampGainDb{0.0};     // ganancia del amplificador (dB)
    bool subwooferOn{false};   // subwoofer activo (neutro = sin procesar)
    double subCrossoverHz{80.0};

    // ── Satélites (surround, componente separado) ────────────────────
    bool satellitesOn{false};  // surround activo (hoy OFF en Esse)
    // Corte grave del satélite (Hz). 0 = desconocido.
    double satelliteF3Hz{0.0};
    // Corte agudo del satélite (Hz). 0 = desconocido.
    double satelliteF3HighHz{0.0};
    // Sensibilidad del satélite (dB SPL @ 1 W @ 1 m). 0 = desconocida.
    double satelliteSensitivityDb{0.0};

    // ── Transductor ──────────────────────────────────────────────────
    TwinTransducer transducer{TwinTransducer::kSpeaker};
    // Altavoz: dB SPL @ 1 W @ 1 m. Audífono: dB SPL @ 1 mW en el oído.
    // 0 = desconocida.
    double sensitivityDb{0.0};

    // ── Sala (solo altavoz; en audífono se ignora) ───────────────────
    double roomWidthM{4.0};
    double roomLengthM{7.0};
    double roomHeightM{2.5};
    double roomRt60Sec{0.34};  // RT60 medido/referencia (500 Hz–1 kHz)
    double listenerDistanceM{3.0};
    double ambientNoiseDbSpl{30.0};  // ruido ambiente de la sala (estimación)

    // ── Señal ────────────────────────────────────────────────────────
    double targetSplDb{85.0};  // SPL que se pide en la posición de escucha

    // Preset del trayecto real de Esse (MEMORY.md): DAC USB JieLi
    // 24-bit/48 kHz → Sony Genezi MHC-PZ1D (estéreo, flat, sub ON neutro,
    // surround OFF) → sala 4.0×7.0 m, escucha a 3 m, RT60 0.34 s.
    // La potencia del Genezi y la sensibilidad de sus bocinas NO están
    // medidas: quedan en 0 (desconocidas) y la confianza lo refleja.
    static AcousticTwinParams esseGenezi() noexcept;
    // Mismo trayecto que esseGenezi() pero con los valores de la
    // calibración P3 ESTIMADA (datasheet Sony MHC-PZ1D + física exacta,
    // genezi_pz1d_estimado_datasheet.json): amp 135 W, sensibilidad
    // frontal 88 dB, satélite modelado (f3 157.5 Hz, 16 kHz, 84 dB).
    // Los valores son DERIVADOS, no medidos: la confianza honesta es
    // 0.70 (ver TwinCalibration), no la que da la fórmula con los
    // valores rellenados (0.926). Usar con estimateCalibrated().
    static AcousticTwinParams esseGeneziCalibrated() noexcept;
    // Mismo DAC, audífono en vez de bocinas (sin sala, sin aire).
    static AcousticTwinParams esseHeadphones() noexcept;
};

// Calibración del gemelo (P3). Entrada por STRUCT: el gemelo no hace
// I/O y no lee archivos. Quien carga el JSON (la app, o el test) puebla
// este struct y lo pasa a AcousticTwin::estimateCalibrated().
// La fuente de verdad es
//   goals/majestuosidad-total-perceptual-de-ivanna/calibration_p3/
//   genezi_pz1d_estimado_datasheet.json
// y el preset TwinCalibration::geneziPz1dEstimated() es su transcripción
// fiel (con la cita de la fuente en el .cpp).
//
// ADVERTENCIA DE CONFIANZA (documentada en el JSON): la fórmula del
// gemelo solo penaliza valores AUSENTES; si se le alimentan valores
// derivados como si fueran medidos, sobreestima la confianza (0.926
// con estos valores). Por eso la calibración trae su confianza HONESTA
// (honestConfidence = 0.70) y estimateCalibrated() la impone: la
// confianza que sale con calibración es la honesta, nunca la de la
// fórmula. Con la calibración aplicada el gemelo CONTROLA la
// conservación (FusedChain: defaults del gemelo cuando el usuario no
// fijó amount ni techo; efectivo 0.5 × 0.70 = 0.35). El 0.80 era el
// umbral que exigía control con medición; con lo que hay (estimado
// 0.70) el control va escalado por la confianza honesta, no bloqueado.
struct TwinCalibration {
    // Respuesta estimada por banda (dB, relativa, 0 dB = nivel suave
    // a 1 kHz). Se ancla a la ganancia del gemelo a 1 kHz (no se suma
    // al gainDb por banda: eso duplicaría el refuerzo del sub).
    std::array<double, kTwinBands> responseDb{};
    bool responseKnown{false};
    // Confianza HONESTA global de la calibración (0..1). Es la que
    // manda cuando la calibración se aplica.
    double honestConfidence{0.0};
    // Confianza honesta por componente (0..1). Mapeo por banda:
    //   63/125 Hz -> bass; 250 Hz-2 kHz -> mid; 4-16 kHz -> treble.
    double directPathConfidence{0.0};
    double bassConfidence{0.0};
    double midConfidence{0.0};
    double trebleConfidence{0.0};
    double roomConfidence{0.0};
    // true = estimado (datasheet + física), false = medido (barrido).
    // Solo un estimado trae honestConfidence < 1 por construcción.
    bool isEstimated{true};
    // Satélite (componente separado). Solo se usa si satellitesOn.
    bool satelliteKnown{false};
    double satelliteF3Hz{0.0};
    double satelliteF3HighHz{0.0};
    double satelliteSensitivityDb{0.0};
    // Respuesta anechoica del satélite por banda (dB, relativa).
    std::array<double, kTwinBands> satelliteResponseDb{};
    // true si la calibración pasó la validación (isValid()).
    bool valid{false};

    // Validación fail-closed: valores finitos, confianza en [0,1],
    // respuesta en rango cuerdo, satélite cuerdo si se declara.
    bool isValid() const noexcept;
    // Calibración P3 estimada del Genezi (transcripción fiel del JSON).
    static TwinCalibration geneziPz1dEstimated() noexcept;
    // Calibración del Genezi con la respuesta en magnitud MEDIDA desde
    // la nota de voz de Esse (2026-11-22 01:0x CST, 3 barridos ESS
    // grabados con el grabador de notas aparte, AAC 44.1 kHz).
    // Respuesta 63 Hz-2 kHz MEDIDA (RMS por ventana 0.25 s en la
    // frecuencia dominante del barrido, 3 barridos promediados);
    // 4-16 kHz del ESTIMADO (la nota de voz no los mide: el grabador
    // recorta agudos, -7.8/-10.3 dB a 4/8 kHz son artefacto del
    // grabador, no del Genezi). Potencia, sensibilidad y RT60 siguen
    // sin medirse (derivan del estimado). Confianza honesta 0.75:
    // mejor que el estimado 0.70 (respuesta en graves/medios ya es
    // medida, no derivada) pero por debajo de 0.80 (falta potencia,
    // sensibilidad, RT60 y agudos medidos). isEstimated=false porque
    // la respuesta principal es medida; el comentario documenta que
    // los agudos siguen siendo estimado.
    // Fuente: goals/.../calibration_p3/voicenote_2026-11-22/
    //   respuesta_rms_voicenote.json, INFORME_VOICENOTE_2026-11-22.md
    static TwinCalibration geneziPz1dVoiceNote() noexcept;
    // Calibración del Genezi con la respuesta en magnitud MEDIDA desde
    // el barrido Farina de la app (2026-11-22 05:54 CST, ESS limpio
    // 20 Hz-20 kHz capturado por GeneziSweepRecorder, WAV 48 kHz sin
    // compresión). Método: RMS por ventana de 0.25 s siguiendo la
    // frecuencia del ESS, normalizado por el RMS del ESS fuente, dos
    // barridos independientes (lo/hi) promediados; coinciden dentro de
    // ~1 dB en 125 Hz-2 kHz. Respuesta 63 Hz-8 kHz MEDIDA; 16 kHz del
    // ESTIMADO (0.0 dB: el micrófono del teléfono recorta ahí, −30 dB
    // medido es el micrófono, no el Genezi). La caída a 2–4 kHz
    // (−10.5/−17 dB) es real en la grabación pero puede ser el cruce
    // del Genezi o un nulo por posición del micrófono; la confianza
    // de agudos (0.60) lo refleja. Potencia, sensibilidad y RT60
    // siguen sin medirse (derivan del estimado). Confianza honesta
    // 0.80: medición completa en WAV (mejor que la nota de voz 0.75,
    // que era AAC), pero falta RT60, potencia, sensibilidad y 16 kHz.
    // isEstimated=false porque la respuesta principal es medida.
    // Fuente: goals/.../calibration_p3/sweep_2026-11-22_0554/
    //   INFORME_BARRIDO_2026-11-22_0554.md
    static TwinCalibration geneziPz1dMeasured() noexcept;
};

// Estimación por banda. Todo es un ESTIMADO (modelo), no una medición.
struct AcousticTwinBand {
    double freqHz{};
    double gainDb{};         // Respuesta espectral efectiva en escucha:
                             // altavoz = 10·log10(1/d² + 4/A) (+ sub en
                             // graves); audífono = 0.
    double latencyMs{};      // Buffer del DAC + propagación por aire
                             // (0 aire en audífono).
    double dynamicRangeDb{}; // Rango dinámico usable en escucha =
                             // SPL máximo − ruido ambiente. Si el SPL
                             // máximo es desconocido, es el rango
                             // dinámico teórico del DAC (techo, no usable).
    double thdPercent{};     // THD estimado por margen de potencia.
                             // 0 con !maxSplKnown = NO ESTIMABLE, no cero.
    double noiseDbSpl{};     // Piso de ruido ambiente por banda.
    double rt60Sec{};        // RT60 Sabine por banda (0 en audífono).
    double coherence{};      // 0..1: campo directo / (directo+reverberante).
                             // 1 en audífono.
    double compressionDb{};  // Compresión estimada a targetSplDb.
                             // 0 con !maxSplKnown = no estimable.
    double maxSplDb{};       // SPL máximo alcanzable en la banda.
    bool maxSplKnown{false}; // false si potencia o sensibilidad desconocidas.
    double confidence{};     // 0..1: confianza de ESTA banda.
    double interventionScale{};  // 0..1 = confidence: a confianza baja, el
                                 // sistema debe intervenir MENOS. Multiplicar
                                 // cualquier corrección por este factor.
};

// Estimación completa del trayecto.
struct AcousticTwinEstimate {
    std::array<AcousticTwinBand, kTwinBands> band{};
    double roomVolumeM3{};       // 0 en audífono (sin sala)
    double roomSurfaceM2{};      // 0 en audífono
    double schroederHz{};        // Frecuencia de Schroeder (0 en audífono)
    double criticalDistanceM{};  // Distancia crítica (0 en audífono)
    double bufferLatencyMs{};
    double airLatencyMs{};       // 0 en audífono
    double overallConfidence{};  // media de las confianzas por banda
    double overallInterventionScale{};  // = overallConfidence
    bool valid{false};           // false si los parámetros no permiten estimar
    // ── Calibración (solo cuando se aplicó una válida) ──────────────
    bool calibrationApplied{false};  // true si estimateCalibrated() aplicó una
    double calibrationConfidence{0.0};  // confianza HONESTA de la calibración
                                        // (0.70 en la P3 estimada; 0 si no aplicada)
    // Respuesta de calibración por banda (dB, relativa). 0 si no aplicada.
    std::array<double, kTwinBands> calibratedResponseDb{};
    // ── Satélites (componente separado) ─────────────────────────────
    bool satellitesActive{false};  // true si satellitesOn y satélite conocido
    // Respuesta del satélite por banda (dB, relativa). 0 si inactivo.
    std::array<double, kTwinBands> satelliteGainDb{};
};

// El gemelo acústico. Función pura: sin estado global, sin I/O, sin
// tocar audio. Determinista: misma entrada → misma salida, bit a bit.
class AcousticTwin {
public:
    static AcousticTwinEstimate estimate(const AcousticTwinParams& p) noexcept;
    // Re-estima a otro SPL (mismo trayecto, otro nivel). Solo THD,
    // compresión y rango dinámico cambian con el nivel; ganancia,
    // latencia, RT60 y coherencia no dependen del SPL.
    static AcousticTwinEstimate estimateAtSpl(const AcousticTwinParams& p,
                                              double splDb) noexcept;
    // Estima con una calibración aplicada (P3). Fail-closed: si la
    // calibración no es válida, devuelve la estimación SIN calibrar
    // (mismo resultado que estimate(p)); si los parámetros no permiten
    // estimar, devuelve inválido (la calibración no lo arregla).
    // Con calibración válida: la ganancia por banda se ancla a la
    // ganancia a 1 kHz + la respuesta de calibración (sin duplicar el
    // refuerzo del sub), y la confianza es la HONESTA de la calibración
    // (0.70), nunca la de la fórmula (0.926). El satélite se reporta
    // aparte y solo si satellitesOn y satélite conocido.
    static AcousticTwinEstimate estimateCalibrated(
        const AcousticTwinParams& p, const TwinCalibration& cal) noexcept;
};

}  // namespace ivanna::fused::acoustic_twin
