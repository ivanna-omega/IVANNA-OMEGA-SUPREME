#pragma once
// fused_chain.hpp — FUSIÓN MAGISTRAL (2026-11-18, directiva de Esse).
//
// La cadena ÚNICA de los 7 operadores fusionados. Núcleo C++ compartido:
// la MISMA clase corre en Ruta A (app, nativeProcess/JNI, DSPBridge) y en
// Ruta B (efecto en el HAL, omega_process / IvannaFusionEngine). Cada ruta
// instancia su propio FusedChain; los OPERADORES (el código) son los mismos
// en ambas — nada de un operador que solo exista en una ruta.
//
// Orden canónico de la cadena (flujo de señal, igual en ambas rutas):
//   0 NEURO·Ω (cerebro) → 1 ORBE·Ω (espacial) → 2 HOLOS·Ω (campo) →
//   3 SALA VIVA (oyente+sala) → 4 VOLTERRA·Ω (transductor) →
//   5 ARMONÍA (tonal) → 6 PULSO (dinámica)
//
// El cerebro corre primero: su guardián AntiDolby se actualiza con el
// bloque actual y su modulación (widener/EQ/spread) se aplica a los
// operadores que siguen en ese mismo bloque. El control de inmersión del
// upmixer (PULSO.upmixImmersivity) se enruta a ORBE·Ω (el upmixer HOA vive
// ahí, unidad indivisible con el decoder).
//
// Matched-LUFS: ganancia neta 0.0 dB. En neutro toda la cadena es
// bit-transparente (cada operador no toca el buffer; el puente float↔double
// es exacto en ambos sentidos para valores float).
//
// NOTA HONESTA — DOUBLE INTERNO EN LOS 8 OPERADORES (v2.5.486, 2026-11-20;
// reemplaza la nota "DOUBLE SOLO EN LAS FRONTERAS" del 2026-11-18, que ya
// NO es cierta): la labor grande pasó el plano de SEÑAL a double 64-bit
// en los 7 operadores + NEURO·Ω (commits b10266c2 + 4c04b26f): ORBE·Ω,
// HOLOS·Ω, SALA VIVA, VOLTERRA·Ω, ARMONÍA, PULSO, Φ/OmniField y NEURO·Ω
// corren su matemática interna en double; los envoltorios float de compat
// (X = float histórico bit-idéntico, Xd = double) quedan para la API
// externa y para la ruta Hexagon/DSP (el DSP es float por hardware). El
// plano de CONTROL queda float/cuantizado POR DISEÑO (TinyML,
// clasificador, LSTM int8, tracker, NPE/LIF/EEG de Φ, audiograma,
// geometría, ClosedLoopController, LIFPool32, BiquadEnvelopeBank).
//
// PISOS MEDIDOS (2026-11-20, tests/test_double_floor.cpp, 1 kHz, −3 dBFS,
// 1 s en régimen permanente, medidor con piso ~−310 dB). La meta THD+N
// −200 dB NO se cumple en toda la cadena — y este es el desglose honesto:
//   - Cadena lineal enganchada HEADPHONES (ORBE+SALA+ARMONÍA+PULSO-lineal,
//     campo/VOLTERRA/NEURO fuera por carácter): −180.80 dB. El límite lo
//     pone ORBE: su convolución HRTF corre en double pero los DATOS
//     HRTF/PCA que alimenta son float32 — el piso es el de los datos, no
//     el de la aritmética. 40 dB mejor que el piso float32 (~−140 dB).
//     Subir a −200 dB exigiría HRTF en double (datos + interpolación).
//   - SALA + ARMONÍA solos: −297.35 dB (piso del medidor): la matemática
//     double de la convolución RIR y del EQ es numéricamente perfecta.
//   - SPEAKERS con TΩ apagado: −296.74 dB (el render SPEAKERS es perfecto).
//   - TΩ en SPEAKERS: −40.04 dB — bandas laterales a ±375 Hz (tasa de hop
//     128) con periodo de 3 hops: la etapa espectral modula el tono a la
//     tasa de frame (carácter en tono estacionario, no ruido numérico).
//   - Campo enganchado (amount 0.5/phi 0.5): −17.32 dB — el TONO EMERGENTE
//     de OmniField (convergencia Kuramoto, default ON): un tono real a
//     884 Hz (−17.6 dB) junto al fundamental. Es FUNCIÓN del campo, no
//     error numérico (con convergence=false baja a −75.7 dB; Phi solo
//     −123.37 dB por su bucle cerrado). Por eso el campo queda FUERA del
//     test de piso: con tono puro su carácter domina cualquier medida.
//   - CPU por hop (128 muestras = 2667 µs): p50 ~200 µs/hop en host
//     (13x de margen) — el double cabe sobrado en realtime.
// El −303.89 dB en neutro sigue siendo cadena transparente sin matemática;
// el THD+N activo −5.49/−6.48 dB (ThdAtEngaged) sigue siendo carácter
// intencional, no precisión. Nada de esto vende double donde hay carácter.
//
// F1/472 (2026-11-18) — TΩ CONECTADO, DOBLE EN LA CADENA:
// La etapa espectral TΩ (ver miembros tOmega* abajo) corre en double
// 64-bit de extremo a extremo: Reloj 2 compila M[k] (T_XTC Kirkeby
// mono-renormalizado × fase mínima cepstral × espacial) y Reloj 1 la
// aplica con 1 FFT + 1 IFFT empaquetadas L+jR, hop 128, 257 bins. En
// SPEAKERS sustituye a la matriz volumétrica WOLA (mismo rol XTC, un
// solo kernel); en HEADPHONES M = I y la etapa se salta a coste cero
// (el render espacial vive en ORBE·Ω/HOLOS·Ω seriales; migrarlo al
// compilador espectral es F3). Los 7 operadores seriales SIGUEN con
// matemática interna float32: sus archivos están fuera del ámbito de
// este ítem (los tiene otro hijo) y NO se tocaron. El THD+N activo NO
// se mueve con el double interno: −5.49/−6.48 dB es carácter intencional
// del procesado (el piso float32 está ~−140 dB), no cuantización; el
// test ThdAtEngaged lo mide y lo reporta tal cual, sin vender mejora.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include "fused/fused_controls.hpp"
#include "fused/acoustic_twin/acoustic_twin.hpp"  // gemelo + calibración P3 (call site)
#include "fused/fused_spatial.hpp"
#include "fused/fused_field.hpp"
#include "fused/fused_listener_room.hpp"
#include "fused/fused_transducer.hpp"
#include "fused/fused_tonal.hpp"
#include "fused/fused_dynamics.hpp"
#include "fused/fused_brain.hpp"
#include "fused/ivanna_apex_kernel.hpp"  // IVANNA-APEX: splitter + delay Lagrange
#include "fused/perceptual_conservation.hpp"  // 8. Árbitro de campo (2026-11-21)
#include "fused/unified_field_kernel.hpp"  // TΩ: kernel 2×2 64-bit (F1/472)

namespace ivanna::fused {

// Telemetría para FotoHolo (módulo propio en Ruta B, NO fusionado — es foto
// holográfica, no audio): lee el ancho espacial de ORBE·Ω, las salidas del
// guardián AntiDolby y el estado por operador. FotoHoloBarkMeter corre su
// propio banco Bark sobre el audio; esta telemetría le da el resto.
struct FusedTelemetry {
    float spatialWidth = 1.0f;       // ancho efectivo de ORBE·Ω
    float guardianWidener = 1.0f;    // guardián AntiDolby (NEURO·Ω)
    float guardianEqBoostDb = 0.0f;
    float guardianSpread = 1.0f;
    float guardianIntensity = 0.0f;
    // Arbitraje del Fallo 3 (pushEffectiveControls): escala de Phi aplicada
    // por el presupuesto tonal (1.0 = sin recorte) y si la aproximación
    // Harman de SALA VIVA quedó suprimida por tener dueño la curva.
    float tonalBudgetPhiScale = 1.0f;
    bool hearingHarmanSuppressed = false;
    int dominantGenre = 0;           // género del clasificador (NEURO·Ω)
    float genreConfidence = 0.0f;
    bool active[7] = {};             // por operador, en el último bloque
    bool brainAuto = true;           // NEURO·Ω en AUTO
    // ── GEMELO ACÚSTICO + CALIBRACIÓN P3 (call site, 2026-11-21) ─────────
    // true si la calibración P3 estimada del Genezi se aplicó en el
    // último bloque (modo SPEAKERS, calibración válida). false en
    // audífonos o si la calibración era inválida (fallback sin calibrar).
    bool twinCalibrationApplied = false;
    // Confianza HONESTA de la calibración aplicada (0.70 en la P3
    // estimada). 0.0 si no se aplicó ninguna calibración.
    float twinCalibrationConfidence = 0.0f;
};

// Contadores de ejecución por operador — verificación dual-ruta: cada
// operador debe ejecutar y medirse en Ruta A y en Ruta B.
struct FusedChainStats {
    std::uint64_t blocksProcessed[7] = {};
    std::uint64_t activeBlocks[7] = {};  // bloques donde el operador NO era neutro
    std::uint64_t totalBlocks = 0;
};

class FusedChain {
public:
    static constexpr int kNumOperators = 7;
    // 0 NEURO·Ω, 1 ORBE·Ω, 2 HOLOS·Ω, 3 SALA VIVA, 4 VOLTERRA·Ω, 5 ARMONÍA, 6 PULSO.
    static const char* operatorName(int i) noexcept {
        switch (i) {
            case 0: return FusedBrain::name();
            case 1: return FusedSpatial::name();
            case 2: return FusedField::name();
            case 3: return FusedListenerRoom::name();
            case 4: return FusedTransducer::name();
            case 5: return FusedTonal::name();
            case 6: return FusedDynamics::name();
            default: return "?";
        }
    }

    FusedChain() = default;
    // No copiable (los operadores llevan estado y atomics).
    FusedChain(const FusedChain&) = delete;
    FusedChain& operator=(const FusedChain&) = delete;

    void prepare(double sampleRate) noexcept;
    void setControls(const FusedControls& c) noexcept;
    FusedControls controls() const noexcept { return userControls_; }
    void setNeutral() noexcept;
    bool isNeutral() const noexcept { return userControls_.isNeutral(); }
    void reset() noexcept;
    void setRouteTag(const char* tag) noexcept;
    const char* routeTag() const noexcept { return routeTag_; }

    // ── IVANNA-APEX (2026-11-18) — camino de transitorios ──────────────────
    // El splitter NLMS separa cada muestra en transitorio/sostenido. El
    // SOSTENIDO pasa por los 7 operadores (orden canónico, sin tocarlos).
    // El TRANSITORIO NO pasa por los operadores (ni STFT ni compresor):
    // se retrasa por la línea de Lagrange con el ITD fraccionario del
    // azimut de ORBE y se SUMA a la salida. Así los ataques no se
    // emborronan por la ventana FFT ni se comprimen. Default TRUE (Esse
    // ordenó cablearlo). Con apexEnabled=false, process() es bit-idéntico
    // al anterior (sin splitter ni suma).
    // La matriz volumétrica SÍ se cablea (2026-11-18, orden de Esse):
    // STFT WOLA al final de la cadena (ver setVolumetricEnabled). El oído
    // híbrido queda como componente verificado por test_ivanna_apex_kernel,
    // listo para la siguiente fase.
    void setApexEnabled(bool on) noexcept { apexEnabled_.store(on, std::memory_order_relaxed); }
    bool apexEnabled() const noexcept { return apexEnabled_.load(std::memory_order_relaxed); }

    // ── IVANNA-APEX: matriz volumétrica (XTC Genezi) ─────────────────────
    // Default TRUE. Con volumetricEnabled=false, process() es bit-idéntico
    // al anterior (sin STFT). La puerta isNeutral() tiene prioridad: en
    // neutro la matriz se salta aunque el flag esté en true.
    // SOLO SPEAKERS (470, crítica externa verificada): la matriz es XTC
    // para altavoces Genezi; en HEADPHONES no se aplica aunque el flag
    // esté en true (no hay diafonía que cancelar). El modo se lee de
    // userControls_.spatial.mode (Ruta A vía JNI, Ruta B vía
    // setFusedOutputMode); default HEADPHONES = matriz apagada (fail-closed).
    // F1/472: la implementación de esta etapa es la TΩ (compileTOmega +
    // applyTOmega, kernel 2×2 64-bit, hop 128 empaquetado); el WOLA
    // volumétrico (applyVolumetric) queda LEGACY — se conserva el código,
    // no se llama. Este flag SIGUE siendo el interruptor de la etapa: no
    // hay toggle propio de TΩ (sin physics.tOmegaEnabled — contrato
    // eliminado en la reconciliación Física Nueva — ni bool nuevo).
    void setVolumetricEnabled(bool on) noexcept { volumetricEnabled_.store(on, std::memory_order_relaxed); }
    bool volumetricEnabled() const noexcept { return volumetricEnabled_.load(std::memory_order_relaxed); }

    // ── TΩ (F1/472): telemetría de la etapa espectral ────────────────────
    // Compilaciones Reloj 2 acumuladas (0 = la matriz aún no se compiló)
    // y si la etapa está enganchada ahora mismo (puerta: volumetricEnabled
    // && !neutral && SPEAKERS). Solo lectura; sin locks.
    int tOmegaCompiles() const noexcept { return tOmegaCompiles_; }
    bool tOmegaActive() const noexcept {
        return volumetricEnabled_.load(std::memory_order_relaxed) && !isNeutral() &&
               userControls_.spatial.mode == OmegaOutputMode::SPEAKERS;
    }

    // Camino en double (núcleo). En neutro ningún operador toca el buffer.
    void process(double* left, double* right, int frames) noexcept;
    // Puente float para las rutas vivas (FusionEngine Ruta B, JNI Ruta A).
    // En neutro es bit-exacto en float (float→double→float es exacto).
    void processFloat(float* left, float* right, int frames) noexcept;

    FusedChainStats stats() const noexcept { return stats_; }
    FusedTelemetry telemetry() const noexcept { return telemetry_; }
    void resetStats() noexcept;

    // Acceso directo a los operadores (telemetría / tests).
    FusedBrain& brain() noexcept { return brain_; }
    FusedSpatial& spatial() noexcept { return spatial_; }
    FusedField& field() noexcept { return field_; }
    FusedListenerRoom& listenerRoom() noexcept { return listenerRoom_; }
    FusedTransducer& transducer() noexcept { return transducer_; }
    FusedTonal& tonal() noexcept { return tonal_; }
    FusedDynamics& dynamics() noexcept { return dynamics_; }
    // Acceso a la capa de conservación (telemetría / Ruta B / tests).
    PerceptualConservation& conservation() noexcept { return conservation_; }
    const PerceptualConservation& conservation() const noexcept { return conservation_; }

    // ── GEMELO ACÚSTICO + CALIBRACIÓN — CALL SITE (2026-11-21/22) ────────
    // La calibración del Genezi vive embebida en el gemelo. Desde la
    // 2026-11-22 la de por defecto es la MEDIDA desde el barrido Farina
    // de la app (TwinCalibration::geneziPz1dMeasured(), confianza
    // honesta 0.80); la nota de voz (geneziPz1dVoiceNote(), 0.75) y la
    // P3 estimada (geneziPz1dEstimated(), 0.70) quedan como respaldo
    // documentado. FusedChain la consulta en runtime
    // (refreshTwinCalibration, en pushEffectiveControls, por bloque y
    // solo cuando cambia el modo o la calibración) y alimenta su
    // interventionScale a la conservación (applyTwinToConservation), que
    // es el camino vivo que ya consume esa escala en el hilo de audio.
    // El MPC sigue en sombra (sin instancia en el path vivo): su puente
    // (applyTwinToField) queda disponible para cuando el MPC se encienda;
    // la estimación queda expuesta aquí (twinEstimate()) para ese uso.
    //
    // setTwinCalibration: alimenta una calibración distinta (p. ej. una
    // MEDIDA futura: la app carga el JSON en Kotlin y puebla el struct).
    // Sin llamar, se usa la medida en WAV limpio del Genezi. Llamar antes de
    // process() o con la cadena parada (mismo patrón que setControls:
    // el struct se escribe en el hilo de UI y lo lee el de audio).
    // Una calibración inválida NO rompe nada: el refresh cae al estimado
    // sin calibrar (~0.46) y la conservación sigue con esa escala.
    void setTwinCalibration(const acoustic_twin::TwinCalibration& cal) noexcept;
    // Última estimación del gemelo aplicada (o vacía si aún no se
    // refrescó). Solo la escribe el hilo de audio; lectura best-effort.
    const acoustic_twin::AcousticTwinEstimate& twinEstimate() const noexcept {
        return twinEstimate_;
    }

    // ── Control del gemelo (orden de Esse, 2026-11-21) ───────────────────
    // Con la calibración aplicada, el gemelo CONTROLA la conservación:
    // si el usuario no fijó amount ni energyCap, se usan estos defaults
    // del gemelo. Sin el amount, la escala no haría nada
    // (0 × escala = 0); sin el techo, el solucionador nunca constriñe
    // (la ganancia es siempre 1.0: solo baja si el techo < energía
    // medida) y el control sería un placebo. El amount del usuario
    // (> 0) y un techo fijado por el usuario (< 1.0) siempre mandan.
    // Efectivo con la medida en WAV limpio (2026-11-22): 0.5 × 0.80 = 0.40.
    // El techo 0.8 solo recorta bandas por encima del 80% de energía
    // (exceso genuino); el recorte máximo a fondo de escala es ~3.7%
    // (no recorta de más).
    static constexpr float kTwinCalibratedDefaultConservationAmount = 0.5f;
    static constexpr float kTwinCalibratedDefaultConservationEnergyCap = 0.8f;

private:
    // Calcula los controles efectivos del bloque (usuario + modulación del
    // guardián + enrutado del upmix) y los empuja a los operadores.
    void pushEffectiveControls() noexcept;

    // ── GEMELO ACÚSTICO + CALIBRACIÓN — CALL SITE (2026-11-21/22) ────────
    // Consulta la estimación del gemelo para el modo efectivo y alimenta
    // su interventionScale a la conservación. Solo recomputa cuando
    // cambia el modo o la calibración (la estimación es función pura de
    // parámetros estáticos; por bloque sería trabajo inútil).
    //   SPEAKERS: estimateCalibrated(esseGeneziCalibrated(),
    //     geneziPz1dMeasured()) -> escala 0.80 (confianza honesta).
    //     Calibración inválida -> estimate(esseGenezi()) sin calibrar
    //     (~0.46): la fórmula NO ve los valores derivados, así que no
    //     infla la confianza (eso daría 0.926 y sería mentir).
    //   HEADPHONES (u otro): la calibración del Genezi no aplica;
    //     escala 1.0 (sin cambio, comportamiento actual intacto).
    // Fail-closed en todas las capas: estimación inválida ->
    // applyTwinToConservation pone escala 1.0; amount=0 -> la escala no
    // toca el sonido (0 x escala = 0). Sin malloc/locks/I-O; seguro en
    // el hilo de audio.
    void refreshTwinCalibration(OmegaOutputMode mode) noexcept;
    // Calibración alimentada por la app (setTwinCalibration). Sin ella,
    // se usa la estimada del Genezi. El struct lo escribe el hilo de UI
    // y lo lee el de audio; el flag atómico ordena el traspaso.
    acoustic_twin::TwinCalibration twinCalibrationOverride_{};
    std::atomic<bool> twinCalibrationHasOverride_{false};
    std::atomic<bool> twinCalDirty_{true};  // true = refrescar en el próximo bloque
    bool twinCalReady_ = false;             // solo hilo de audio
    OmegaOutputMode twinCalMode_ = OmegaOutputMode::HEADPHONES;  // solo hilo de audio
    acoustic_twin::AcousticTwinEstimate twinEstimate_{};  // solo hilo de audio

    FusedBrain brain_;
    FusedSpatial spatial_;
    FusedField field_;
    FusedListenerRoom listenerRoom_;
    FusedTransducer transducer_;
    FusedTonal tonal_;
    FusedDynamics dynamics_;
    // ── 8. CONSERVACIÓN PERCEPTUAL (árbitro de campo, 2026-11-21) ─────────
    // NO es un operador: arbitra tras los 7 + APEX, antes de TΩ. Apagado
    // por defecto (amount 0 = no toca el buffer, bit-exacto).
    PerceptualConservation conservation_;

    FusedControls userControls_;
    double sampleRate_ = 48000.0;
    double chainTime_ = 0.0;  // tiempo monotónico de la cadena (s), para el tiempo perceptual de la conservación.
    char routeTag_[16] = "shared";
    FusedChainStats stats_;
    FusedTelemetry telemetry_;
    std::vector<double> scratchL_, scratchR_;  // puente float (processFloat)
    // ── IVANNA-APEX: camino de transitorios (ver setApexEnabled) ─────────
    // Splitter NLMS + 2 líneas de retardo Lagrange + workspace APEX.
    // Scratch por chunk (máx kFusedMaxBlockFrames) para trans/sust.
    apex::TransientOrthogonalSplitter64 apexSplitter_;
    apex::FractionalDelayLine64 apexDelayL_, apexDelayR_;
    apex::ApexWorkspace apexWs_;
    std::atomic<bool> apexEnabled_{true};
    std::vector<double> apexTransL_, apexTransR_, apexSustL_, apexSustR_;
    // ── IVANNA-APEX: matriz volumétrica (XTC Genezi, 2026-11-18) ─────────
    // STFT WOLA (N=512, hop=256, Hann solo en análisis, COLA exacta) que
    // aplica la matriz W por bin al FINAL de la cadena (corrección de
    // altavoz, última etapa, tras los 7 operadores y tras la suma de
    // transitorios APEX). W precalculada por bin en prepareVolumetric()
    // (spec Genezi: span 0.90 m, distancia 0.85 m, hw 0.02). Latencia
    // ~512 muestras (~10.7 ms @ 48 kHz) SOLO cuando está activa; en
    // neutro o con volumetricEnabled=false es passthrough bit-exacto sin
    // STFT ni retardo (la puerta se evalúa antes de tocar el buffer).
    // La FIFO de salida desacopla los bloques de hop (256) del tamaño de
    // chunk arbitrario del llamador (hasta kFusedMaxBlockFrames).
    static constexpr int kVolFft = 512;
    static constexpr int kVolHop = 256;
    static constexpr int kVolBins = kVolFft / 2 + 1;
    apex::VolumetricSpeakerSpec volSpec_;
    std::array<apex::Complex2x2, kVolBins> volW_{};
    std::array<double, kVolFft> volWin_{};
    std::array<std::complex<double>, kVolFft> volFftL_, volFftR_;
    std::vector<double> volHistL_, volHistR_;   // anillo historia (N)
    std::vector<double> volOlaL_, volOlaR_;     // acumulador OLA (N)
    std::vector<double> volOutL_, volOutR_;     // FIFO salida (anillo)
    int volHistWp_ = 0;
    long volInTotal_ = 0;        // muestras de entrada recibidas (acumulado)
    long volNextFrameAt_ = 512;  // inTotal al que se procesa el próximo frame
    int volOutWp_ = 0, volOutRp_ = 0, volOutLen_ = 0;
    std::atomic<bool> volumetricEnabled_{true};
    void prepareVolumetric() noexcept;
    void resetVolumetric() noexcept;
    // LEGACY (F1/472): la matriz volumétrica WOLA ya no se llama —
    // la etapa la implementa TΩ (compileTOmega + applyTOmega). Se
    // conserva el código (nunca se borra) para rollback / referencia.
    void applyVolumetric(double* l, double* r, int frames) noexcept;

    // ── TΩ (F1/472): kernel 2×2 64-bit conectado a FusedChain ───────────
    // Reloj 2 (compileTOmega, double 64-bit, SOLO al cambiar controles):
    //   M[k] = T_Transd · T_XTC · T_Eq · T_HOLOS/LGN · T_ORBE (orden no
    //   conmutativo §2.2, de la señal al transductor):
    //     T_Transd = I (sin kernel .ivk1 medido — F4; fail-closed).
    //     T_XTC[k] = Kirkeby computeSpeakersMatrixBin (SPEAKERS, mono
    //       renormalizado <80 Hz) / I (HEADPHONES, sin diafonía).
    //     T_Eq[k] = g_eq[k]·I, fase mínima cepstral en double
    //       (buildMinimumPhaseSpectrum64) de la magnitud neta PROPIA de
    //       TΩ — en F1 plana (0 dB): el tonal lo aplica ARMONÍA serial y
    //       duplicarlo colorearía dos veces. La maquinaria queda cableada
    //       y verificada (tests MinimumPhase); F3 solo alimenta la
    //       magnitud neta Γ cuando el tonal migre a M[k].
    //     T_HOLOS/LGN = T_ORBE = I (el campo y el espacial viven en sus
    //       operadores seriales; migrarlos al compilador es F3).
    //   β(ω): alto <120 Hz y >6 kHz (regMid 0.06, regHigh 0.35, lowRoll
    //   LR4 +4.0 en subgraves — ver computeSpeakersMatrixBin); el soporte
    //   temporal queda |τ| ≤ 64 muestras por fase mínima + regularización
    //   (sin FFT 1024: no hizo falta).
    // Reloj 1 (applyTOmega, cada hop 128, N=512, 257 bins): 1 FFT + 1 IFFT
    //   empaquetadas z = L + jR; Y[k] = M[k]·X[k] + T_FDL[k] (T_FDL = 0 en
    //   F1: la cola difusa >80 ms vive en HOLOS·Ω serial); OLA Hann con
    //   ganancia COLA 2.0 a hop 128 (×0.5 al emitir). Simetría hermítica
    //   exacta en DC y Nyquist (Im{Y[0]} = Im{Y[256]} = 0, residual <1e-14).
    //   En host Reloj 1 corre en double (no hay NEON); la especialización
    //   float32-NEON es trabajo F3 para el G85, no F1.
    // Puerta: volumetricEnabled && !neutral && SPEAKERS (mismo interruptor
    //   que la matriz volumétrica; sin toggle propio de TΩ).
    static constexpr int kTOmegaHop = 128;
    apex::UnifiedFieldWorkspace tOmegaWs_;  // M[k] vive en ws.outMatrix
    std::array<std::complex<double>, kVolBins> tOmegaYl_{}, tOmegaYr_{};
    std::array<std::complex<double>, kVolFft> tOmegaPack_{};
    std::array<std::complex<double>, kVolBins> tOmegaFdlL_{}, tOmegaFdlR_{};
    std::vector<double> tOmegaHistL_, tOmegaHistR_;  // anillo historia (N)
    std::vector<double> tOmegaOlaL_, tOmegaOlaR_;    // acumulador OLA (N)
    std::vector<double> tOmegaOutL_, tOmegaOutR_;    // FIFO salida (anillo)
    int tOmegaHistWp_ = 0;
    long tOmegaInTotal_ = 0;        // muestras de entrada recibidas (acumulado)
    long tOmegaNextFrameAt_ = 512;  // inTotal al que se procesa el próximo frame
    int tOmegaOutWp_ = 0, tOmegaOutRp_ = 0, tOmegaOutLen_ = 0;
    // Snapshot de lo que la matriz compilada refleja (modo, fs, geometría
    // Genezi). Si cambia, Reloj 2 recompila en el próximo process().
    struct TOmegaSnapshot {
        OmegaOutputMode mode = OmegaOutputMode::HEADPHONES;
        double sampleRate = 48000.0;
        double spanM = 0.90;
        double distanceM = 0.85;
        bool operator==(const TOmegaSnapshot& o) const noexcept {
            return mode == o.mode && sampleRate == o.sampleRate &&
                   spanM == o.spanM && distanceM == o.distanceM;
        }
        bool operator!=(const TOmegaSnapshot& o) const noexcept { return !(*this == o); }
    };
    TOmegaSnapshot tOmegaSnapshot_{};
    bool tOmegaCompiled_ = false;
    int tOmegaCompiles_ = 0;  // acumulado (telemetría; reset() no lo borra)
    void prepareTOmega() noexcept;
    void resetTOmega() noexcept;
    void compileTOmega() noexcept;
    void applyTOmega(double* l, double* r, int frames) noexcept;
};

}  // namespace ivanna::fused
