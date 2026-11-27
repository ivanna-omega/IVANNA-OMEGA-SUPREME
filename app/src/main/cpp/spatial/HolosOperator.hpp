// © 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
#pragma once
// HolosOperator — HOLOS · Operador Único de Campo Acústico.
//
// FUSION.md §4–§5: doce funciones, una sola matemática. Por banda ERB b
// y frame t, con X_b(t) el espectro de la banda (mid/side):
//
//   Y_b(t) = G_b(s_b) ⊛ [ K_b(s_b) · X_b(t) ] · Γ_b(s_b)
//
// - G_b: función de Green unificada (HolosGreenKernel): ITD/ILD esféricos,
//   pinna medida, (1/r)^γ, difuso → cola compartida (RirConvolver).
// - K_b: kernel de Volterra por banda (HolosVolterraKernel): polinomio
//   Cheb/GoldenEar/Padé + memoria Heun + Volterra H2 (K=64, sin kernel
//   → identidad). Se evalúa en el dominio temporal sobre la señal ya
//   renderizada (post-OLA): un polinomio por bin no genera armónicos.
// - Γ_b: campo de ganancia único (HolosGainField): estática × dinámica
//   × ancho × techo, desde el mismo vector de estado s_b.
// - s_b: un solo vector de estado por banda (HolosStateEstimator).
//
// Flujo: STFT hop 128 / FFT 512 escalable con SR (misma rejilla que ORBE,
// misma FFTRadix2) → estimación de s_b → render por bin → IFFT +
// overlap-add (Hann, COLA (N/hop)·0.375) → K temporal + softCeil sobre el
// hop renderizado → mezcla wet/dry de potencia constante con el camino
// legacy (crossfade 15 ms, mismo patrón que el upmixer y que ORBE).
// amount = 0 es bit-exacto con la entrada. Latencia total = fftSize
// muestras (512 @48 kHz = 10.7 ms).
//
// Entra en FusionCore EN SERIE tras ORBE (ORBE → HOLOS → OMNI → Φ,
// directiva explícita 2026-11-08; el texto anterior "EN EL LUGAR DE
// ORBE / cuando HOLOS está activo ORBE no corre" quedó obsoleto cuando
// se pasó a serie). El retardo algorítmico se suma: 512 muestras de
// ORBE + 512 de HOLOS ≈ 21.3 ms @48 kHz (más 31 de OMNI).
//
// Tiempo real: cero malloc, cero locks, cero I/O en process(); todo se
// preasigna en prepare(). Estado por instancia, nunca global.
// Falla cerrada (fail-closed): coherencia media < 0.3, parámetros no
// finitos o thermal shed → wet = 0 (camino legacy intacto). Cada término
// cae a identidad por separado; nunca a silencio ni a ruido.
//
// Término difuso (v1): UNA sola convolución particionada compartida
// (RirConvolver, IR sintetizada Master Studio, rt60 = holos_room_rt60),
// alimentada con el mid. La salida del RIR es dry·0.32 + wet·0.82 con
// wetNow=1 (su mezcla interna), así que el wet puro se recupera como
// (out − 0.32·mid)/0.82; el wetNow se lleva a 1.0 en prepare() alimentando
// silencio (si no, los primeros ~10 ms tras activar mezclarían seco en
// la cola). La cola se analiza con una segunda STFT (solo FFT) y se suma
// por bin con (1−coh_b)·diffuse·0.82. El RIR se alimenta siempre que
// HOLOS está activo (no solo con diffuse>0) para que su historia
// overlap/FDL no se quede obsoleta.
//
// Tiers térmicos (FUSION §7.3, en orden): NORMAL todo on (24 bandas, H3
// on, difuso on); WARM 65–70 °C: bandas 24→12, H3 off, diffuse ×0.5;
// LIMITED 70–75 °C: difuso off, gamma fijo 1.0 (Volterra K=64→32 queda
// pendiente de que exista un kernel: sin kernel el término ya es
// identidad); BYPASS >75 °C: HOLOS off → cadena legacy bit-exacta.
// Las tablas de bandas 24 y 12 se construyen AMBAS en prepare() y el
// cambio es inmediato (a diferencia de ORBE, donde bandCount solo se
// aplica en el próximo prepare).
//
// Honestidad de la v1 (mismas reglas que ORBE): sin término de magnitud
// de pinna hasta que se cargue una tabla medida (PinnaTable vacía → 0 dB);
// sin interpolación OT de pinna (lineal, fase 2); sin kernel Volterra por
// banda (K = polinomio memoryless + Volterra banda ancha actual, que hoy
// es identidad); DRR_b es un proxy desde coherencia (su uso como
// modulador queda para fase 2); la equivalencia de fase entre el FIR 256
// taps del EvoEQ y la magnitud por bin de Γ no está medida (§5.5).
// Defaults del struct: amount=0 (bypass) hasta que la app restaure
// estado (ParameterStore: amount 0.5, harmonics 0.0, directiva
// 2026-11-08) o llegue un snapshot en Ruta B. La puerta GREEN ya NO
// existe: la directiva la anuló; "hasta GREEN" quedó obsoleto.
// Head-tracking: el parallax por setHeadPose() solo actúa si alguien
// alimenta la pose. v2.5.471 (F2, parte segura): la pose YA llega en ambas
// rutas — Ruta A vía FusedSpatial.feedRotationVector (sensor) y Ruta B por
// snapshot (daemon SET_HEAD_POSE) vía updateHeadPose().
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "BandPowerConserver.hpp"
#include "HolosGainField.hpp"
#include "HolosGreenKernel.hpp"
#include "HolosStateEstimator.hpp"
#include "HolosVolterraKernel.hpp"
#include "NearFieldHeadModel.hpp"
#include "PowerMix.hpp"  // P3/3.1: mezcla seco/wet de potencia constante
#include "RirConvolver.hpp"
#include "fft_radix2.hpp"

namespace ivanna {

// Fase 1 (conservación de potencia, fix Green 2026-11-25) — equivalente
// del M1 de ORBE (BandPowerConserver.hpp, L1/L2): con el flag ON, el side
// NUNCA se atenúa (wSide = 1.0, como kSideGain en ORBE) y el conservador
// por banda fuerza Σ|Y|² = Σ|X|² por banda (ganancia neta 0.0 dB por
// construcción, Regla de Oro). Sin él, HOLOS pasaba el side a ~0.406 en
// contenido coherente (widthFromState 0.3 × norma 1.354), perdía
// −2.5…−4.1 dB y Green fallaba con todos los spreads (ORBE pasa el side
// a 1.0 siempre por M1). Flag compile-time, ON por defecto (orden Esse
// 2026-11-25: el producto sea magistral; la pieza se cambia por una de
// lujo). P0 se re-congela con el flag ON: cambia el sonido.
#ifndef IVANNA_HOLOS_POWER_CONSERVATION
#define IVANNA_HOLOS_POWER_CONSERVATION 1
#endif

// Guarda de armónicos (v2.5.411, tarea 1): máximo de holos_harmonics
// VALIDADO por test (test_holos_retune_sweep, HolosHarmonicsGuard).
// Medido en host a 48 kHz, tonos −3 dBFS, criterio THD+N ≤ −70 dB
// (@1 kHz y @100 Hz) e IMD SMPTE ≤ −60 dB:
//   amount=0.359 (valor vivo de Esse), harmonics=0.298 → THD+N −41.1/−43.6
//   dB, IMD −37.7 dB: FALLA por ~29 dB. El término K distorsiona muchísimo:
//   a 0.359, 0.005 pasa (−76.5/−77.5/−73.2) y 0.015 ya falla (−67.0).
//   El máximo depende del amount: a 0.5, 0.005 pasa y 0.010 falla; a 1.0,
//   0.003 pasa con solo 0.3 dB de margen y 0.004 falla. El mayor valor que
//   pasa a TODOS los amounts (0.359, 0.5, 1.0) con ≥1 dB de margen en la
//   métrica que manda es 0.002 (a 1.0: −75.3/−71.5/−72.2; margen 1.5 dB
//   @100 Hz). Por encima de eso el K no está validado; por debajo es
//   prácticamente inaudible como mezcla, pero la guarda existe para que un
//   valor guardado sin validar (p. ej. 0.298) nunca llegue al DSP.
//   La guarda se aplica AL LEER (clamp en los 3 sitios que cargan
//   g_holos_harmonics en la ruta viva de Ruta A: JNI ×2, FusionCore ×1),
//   NUNCA reescribiendo la preferencia del usuario: su 0.298 sigue
//   guardado y se restaura; el DSP recibe min(0.298, 0.002).
//   El struct HolosParams conserva su default 0.32f (el operador procesa
//   lo que se le pide; la guarda vive en la lectura, no en el operador).
constexpr float kHolosHarmonicsMaxValidated = 0.002f;

struct HolosParams {
    float amount = 0.0f;       // 0..1 wet/dry vs legacy; 0 = bit-exacto
    float distanceM = 0.35f;   // 0.15..2.0 m, distancia base ("íntimo")
    float spread = 1.2f;       // 0..1.5, escala del azimut estimado (ATREVIDA 502; spread 1.2 por decisión Esse 2026-11-25 — acantilado ≥1.3 espeja la imagen)
    float proximityDb = 6.0f;  // 0..6 dB, techo del realce de proximidad (ATREVIDA 502)
    float diffuse = 0.85f;     // 0..1, cuánto campo difuso va a la cola compartida (ATREVIDA 502)
    float harmonics = 0.32f;   // 0..1, mezcla del término K
    float warmth = 0.5f;       // 0..1, balance par/impar de K
    float dynamics = 1.0f;     // 0..1, cantidad de Γ_dinámico (1 = compresor como hoy)
    float width = 1.0f;        // 0..1.5, escala de Γ_ancho
    float gamma = 1.0f;        // 0.5..1.0, exponente de divergencia (1 esférico, ½ WFS)
    float roomRt60 = 0.34f;    // 0.18..1.8 s, RT60 del término difuso (próximo prepare)
    bool headTracking = true;  // parallax por setHeadPose()
    int bandCount = 24;        // 12 o 24 bandas ERB
    // EQ estático (Γ_estático, plano por defecto = no-op bit-exacto).
    float eqLow = 0.0f, eqMid = 0.0f, eqHigh = 0.0f, eqPresence = 0.0f;
    float eqTiltDb = 0.0f, eqFreq = 1000.0f;
};

enum class HolosThermalTier : int {
    Normal = 0,   // < 65 °C: todo on
    Warm = 1,     // 65–70 °C: 12 bandas, H3 off, diffuse ×0.5
    Limited = 2,  // 70–75 °C: difuso off, gamma fijo 1.0
    Bypass = 3,   // > 75 °C: HOLOS off → legacy bit-exacto
};

class HolosOperator {
public:
    // Conservación de potencia L1/L2: false = width de siempre (el side
    // se atenúa a ~0.406 en contenido coherente); true = wSide=1.0 +
    // conservador por banda. true por defecto (orden Esse 2026-11-25).
    static constexpr bool kHolosPowerConservation = (IVANNA_HOLOS_POWER_CONSERVATION != 0);

    static constexpr int kHop = 128;  // == Ivanna::BLOCK_SIZE
    static constexpr int kMaxBands = 24;

    HolosOperator();
    void prepare(float sr);
    void reset() noexcept;
    void setParams(const HolosParams& p) noexcept;
    void setAmount(float amount) noexcept;
    void setHeadPose(float yawRad) noexcept;
    // RE-SEED Γ (2026-11-26, prototipo): pide re-sembrar el estimador Γ
    // desde el espectro asentado en el próximo frame de runFrame() (la
    // petición la pone FusedChain al detectar fin de rampa; el consumo
    // vive en runFrame, donde el espectro del frame está vivo). No toca
    // rings/wet/blend: un reset() completo en caliente glitchearía.
    // clearEstimatorReseedRequest() cancela una petición pendiente (nueva
    // rampa antes de consumirla). Hilo de audio en ambos casos.
    void requestEstimatorReseed() noexcept;
    void clearEstimatorReseedRequest() noexcept;
    void setThermalTier(HolosThermalTier tier) noexcept;
    void setThermalShed(bool shed) noexcept;  // compat: true = Bypass
    // Ω (v2.5.444) — ganancia de firma por banda (lineal, 10^(C_b/20)) que
    // Ω precalcula (hilo de control). Se pliega en Γ (computeBandGains):
    // gammaLin_[b] *= omegaGainLin_[b]. Default 1.0 (identidad): sin Ω el
    // operador es bit-idéntico. Hilo de control escribe, audio lee
    // (atómicos por banda). Cubre el camino serial (renderBins usa
    // gammaLin_) Y el NEU·Ω (el callback lee bandGammaDb, que lo incluye).
    void setOmegaBandGain(const float* gainsLin24) noexcept;
    void setPinnaTable(const holos::PinnaTable& table);
    // Carga de kernel Volterra IVK1 (hilo de CONTROL, nunca el de audio).
    int loadVolterraKernel(const uint8_t* package, size_t packageSize) noexcept;
    // process() acepta cualquier n > 0; siempre escribe exactamente n
    // muestras. Con amount = 0 y el fade completado es passthrough
    // bit-exacto (sin tocar los buffers).
    // Núcleo double (F2/486): la STFT/OLA, el render por bin, Γ y el K
    // temporal son double 64-bit. El ancla de Fase B (−102.79 dB) se
    // re-mide sobre el núcleo double: el piso solo puede bajar (la
    // conversión elimina el ruido de cuantización float del camino);
    // si la medida empeorara, es un bug, no una propiedad.
    void process(double* l, double* r, int n) noexcept;
    // Envoltorio float (compat: FusedField legacy / JNI / tests).
    void process(float* l, float* r, int n) noexcept;

    // NEU·Ω Fase 6 parte 2: estimar los estados por banda (θ, r, Γ,
    // ancho, diffuseMix) sobre un espectro YA transformado
    // (reL/imL = L, reR/imR = R, n = fftSize). Solo lectura del
    // espectro; no hace FFT ni render. Equivale a lo que runFrame()
    // hace tras su propia FFT (setParams + estimator_.estimate +
    // computeBandGains); aquí lo hace el llamador (el SpectralCore,
    // que ya tiene el espectro) para no pagar una segunda FFT.
    // Núcleo double (F2/486).
    void estimateSpectrum(const double* reL, const double* imL,
                          const double* reR, const double* imR, int n);
    // Envoltorio float (compat: SpectralCore legacy).
    void estimateSpectrum(const float* reL, const float* imL,
                          const float* reR, const float* imR, int n);

    // Diagnóstico / tests.
    int fftSize() const noexcept { return fftSize_; }
    int latencySamples() const noexcept { return fftSize_; }
    float sampleRate() const noexcept { return sampleRate_; }
    int activeBandCount() const noexcept { return activeBands_; }
    HolosThermalTier thermalTier() const noexcept {
        return (HolosThermalTier)tier_.load(std::memory_order_relaxed);
    }
    const holos::HolosBandState& bandState(int b) const noexcept {
        return estimator_.state(b);
    }
    float bandGammaDb(int b) const noexcept;  // Γ aplicado (dB, estático+dinámico)
    float bandWidthGain(int b) const noexcept;
    float bandDiffuseMix(int b) const noexcept;
    // Ganancia del conservador de potencia por banda (Fase 1, M1 de
    // HOLOS): g_b del último frame comprometido. 1.0 si el flag está OFF
    // o la banda no existe. La usa NeuCompiler::holosMatrix para
    // replicar el render vivo.
    float bandConserverGain(int b) const noexcept;
    float meanCoherence() const noexcept { return estimator_.meanCoherence(); }
    float appliedAmount() const noexcept { return static_cast<float>(wet_); }
    bool isActive() const noexcept { return wet_ > 0.0f || blend_ > 0.0f; }
    bool hasVolterraKernel() const noexcept { return kernel_.hasVolterraKernel(); }

private:
    struct BandSet {
        int loBin[kMaxBands] = {};
        int hiBin[kMaxBands] = {};
        float centerHz[kMaxBands] = {};
        std::vector<int> bandOfBin;  // por bin 0..fftSize-1
        int count = 24;
    };

    void rebuildBandTables();
    void applyActiveBandSet();
    void runFrame();
    void computeBandGains();
    void renderBins(double* reL, double* imL, double* reR, double* imR,
                    const double* reTL, const double* imTL,
                    const double* reTR, const double* imTR);

    double sampleRate_ = 48000.0;
    int fftSize_ = 512;
    bool prepared_ = false;

    std::atomic<float> amount_{0.0f};
    std::atomic<float> distanceM_{0.35f};
    std::atomic<float> spread_{1.2f};      // ATREVIDA 502 (spread 1.2, decisión Esse 2026-11-25)
    std::atomic<float> proximityDb_{6.0f};  // ATREVIDA 502
    std::atomic<float> diffuse_{0.85f};     // ATREVIDA 502
    std::atomic<float> harmonics_{0.32f};
    std::atomic<float> warmth_{0.5f};
    std::atomic<float> dynamics_{1.0f};
    std::atomic<float> width_{1.0f};
    std::atomic<float> gamma_{1.0f};
    std::atomic<float> roomRt60_{0.34f};
    std::atomic<bool> headTracking_{true};
    std::atomic<bool> h3Enabled_{true};
    std::atomic<float> yawRad_{0.0f};
    std::atomic<int> bandCount_{24};
    std::atomic<int> tier_{(int)HolosThermalTier::Normal};
    std::atomic<float> eqLow_{0.0f}, eqMid_{0.0f}, eqHigh_{0.0f}, eqPresence_{0.0f};
    std::atomic<float> eqTiltDb_{0.0f}, eqFreq_{1000.0f};
    // Ω (v2.5.444): ganancia de firma por banda (lineal). 1.0 = identidad.
    std::atomic<float> omegaGainLin_[kMaxBands];

    // Núcleos (estado por instancia).
    holos::HolosStateEstimator estimator_;
    holos::HolosVolterraKernel kernel_;
    holos::PinnaTable pinna_;
    Ivanna::RirConvolver rir_;  // término difuso: una sola IR compartida
    // Fase 1 (L1): conservador de potencia por banda + su estado. Solo
    // se usa con kHolosPowerConservation; con el flag OFF nunca se toca
    // y el comportamiento es bit-idéntico al actual.
    ivanna::nf::BandPowerConserver holosCons_;

    // Estado STFT principal (todo preasignado en prepare()) — double (F2/486).
    std::unique_ptr<FFTRadix2d> fft_;
    std::vector<double> window_;
    std::vector<double> ringL_, ringR_;
    int ringPos_ = 0;
    std::vector<double> pendL_, pendR_;
    int pendCount_ = 0;
    std::vector<double> olaL_, olaR_;
    std::vector<double> wetQL_, wetQR_;
    std::vector<double> dryQL_, dryQR_;
    int qHead_ = 0, qTail_ = 0, qCount_ = 0, qCap_ = 0;
    int frameIndex_ = 0;
    int skipEmit_ = 0;
    double colaInv_ = 1.0 / 1.5;
    std::vector<double> reL_, imL_, reR_, imR_;

    // Segunda STFT (solo análisis) para la cola difusa.
    std::vector<double> tailRingL_, tailRingR_;
    int tailRingPos_ = 0;
    std::vector<double> tailReL_, tailImL_, tailReR_, tailImR_;
    std::vector<double> midL_, midR_;  // scratch: mid → RIR → wet recuperado

    // Tablas de bandas (24 y 12; el tier cambia el set activo en caliente).
    BandSet set24_, set12_;
    const BandSet* activeSet_ = &set24_;
    int activeBands_ = 24;

    // Ganancias Γ por banda, calculadas 1×/frame (para render y tests).
    double gammaLin_[kMaxBands] = {};
    double gammaDb_[kMaxBands] = {};
    // Gamma complejo de fase mixta (Ley 1): gammaRe_/gammaIm_ = |Gamma|.(cos phi, sin phi).
    // Con phi=0 es bit-identico al camino real (x.1 - y.0 = x).
    double gammaRe_[kMaxBands] = {};
    double gammaIm_[kMaxBands] = {};
    double widthSide_[kMaxBands] = {};
    double widthNorm_[kMaxBands] = {};
    double diffuseMix_[kMaxBands] = {};
    double dynDbSmooth_[kMaxBands] = {};

    // Fades (15 ms): wet_ = cantidad aplicada, blend_ = 0 seco en vivo,
    // 1 seco retardado (alineado con el STFT).
    double wet_ = 0.0;
    double blend_ = 0.0;
    // RE-SEED Γ: petición pendiente de re-sembrar el estimador (la pone
    // FusedChain al detectar fin de rampa; la consume runFrame() en el
    // próximo frame, con el espectro vivo). Atómica por si el hilo de
    // control la pone (hoy la pone el mismo hilo de audio en process()).
    std::atomic<bool> reseedPending_{false};
    // P3/3.1: mezcladores de potencia constante (uno por fundido; misma
    // regla que en OrbeBinaural: el blend usa extremos exactos, el wet
    // usa el mixer en todo w ∈ [0,1]).
    ivanna::spatial::PowerMixer mixBlend_;
    ivanna::spatial::PowerMixer mixWet_;
    bool flushPending_ = false;
    // Re-sondeo de la puerta de coherencia (v2.5.415, bandera 5): el
    // estimador solo se actualiza en runFrame() (solo cuando la etapa está
    // activa), así que una coherencia < 0.3 cerraba la puerta para siempre
    // (latch). Cada kCohProbePeriodBlocks en bypass por coherencia se
    // sondean kCohProbeBlocks seguidos para re-evaluar el estimador.
    static constexpr int kCohProbePeriodBlocks = 150;  // ~1 s entre sondeos
    static constexpr int kCohProbeBlocks = 8;          // ~53 ms por sondeo
    int cohBypassBlocks_ = 0;
    int cohProbeBlocksRemaining_ = 0;
    size_t totalIn_ = 0;
    size_t totalOut_ = 0;
    // Scratch de los envoltorios float (crece bajo demanda, se reutiliza).
    std::vector<double> wrapL_{};
    std::vector<double> wrapR_{};
    std::vector<double> wrapReL_{};
    std::vector<double> wrapImL_{};
    std::vector<double> wrapReR_{};
    std::vector<double> wrapImR_{};
};

}  // namespace ivanna
