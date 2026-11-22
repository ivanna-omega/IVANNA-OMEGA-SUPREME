// WfsRenderer.hpp
// ============================================================================
// IVANNA — Wave Field Synthesis (WFS) Renderer
// ============================================================================
// (c) 2026 Luis Uriel Pimentel Pérez — GORE TNS. All rights reserved.
//
// Síntesis de campo de ondas sobre un array circular de altavoces
// VIRTUALES que luego se renderiza binauralmente (ITD/ILD por altavoz).
// Cada objeto de audio actúa como fuente primaria cuyo frente de onda se
// reconstruye por superposición de las ondas secundarias de cada altavoz
// del array (principio de Huygens):
//
//   driving(o, s) = gain_o * WFS_ATTENUATION(d_os) * focusWeight(o, s)
//   señal_s(t)   = Σ_o  src_o(t − d_os/c) * driving(o, s)
//
// con atenuación 1/sqrt(d) (onda cilíndrica, régimen de campo del array)
// y peso de focalización coseno (la contribución de un altavoz se atenúa
// cuando el objeto queda fuera de su semiespacio frontal — aproximación
// estándar de la driving function de WFS 2.5D).
//
// ── REFINAMIENTO MAGISTRAL: eliminación de micro-cortes (clicks/pops) ──
// CAUSA RAÍZ del defecto: la versión anterior llamaba a rebuildDelays()
// desde setObject(), y ese método hacía assign() de TODAS las líneas de
// delay → cada cambio de posición de una fuente BORRABA el historial de
// audio (líneas a cero, cursores a cero) → discontinuidad instantánea de
// amplitud = micro-corte audible. Además reasignaba memoria en caliente.
//
// Arquitectura glitch-free:
//  1. UNA línea de delay persistente POR FUENTE (no por par fuente×altavoz:
//     todos los taps de una fuente leen la misma señal — mismo resultado
//     físico con 1/16 de memoria y de escrituras). Jamás se resetea cuando
//     la fuente se MUEVE: el audio en propagación sigue su curso natural.
//  2. Retardo fraccionario SUAVIZADO: el delay objetivo se recalcula al
//     mover la fuente, pero el delay EFECTIVO lo persigue con un one-pole
//     por muestra (~5 ms) → glide sin salto de fase (Doppler físico leve,
//     perceptualmente correcto, cero click).
//  3. Ganancia de driving SUAVIZADA con la misma envolvente → sin salto de
//     amplitud al cruzar semiespacios del array.
//  4. Fade-out de un bloque al ELIMINAR una fuente (la salida natural por
//     historial-a-ceros cubre el nacimiento; el corte de una fuente sonando
//     era el otro click clásico).
//  5. CERO asignaciones de memoria en process(): toda la memoria se
//     preasigna en init() o al registrar una fuente (hilo de control).
//
// Coste: O(objetos × altavoces × frames), ~25k MACs por bloque de 384
// frames con 4 objetos × 16 altavoces — viable en el audio thread.
//
// Sin dependencias externas: solo <cmath>, <vector>, <array>, <cstdint>.
// ============================================================================
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace ivanna::wfs {
// ═══ Guarda de anti-aliasing espacial ═══════════════════════════════════
// WFS con array discreto de N altavoces sufre aliasing espacial por encima de
// f_alias = c / (2 * dx), dx = separación entre altavoces. Por encima, el
// frente de onda se reconstruye con artefactos (fantasmas). Solución de
// referencia (Spors & Ahrens): atenuación suave por encima de f_alias con un
// one-pole por canal, calculada desde la geometría real del array. No
// degrada el audio por debajo de f_alias — solo evita que la reconstrucción
// mienta arriba.
inline float spatialAliasCutoffHz(float speakerSpacingM, float c = 343.0f) noexcept {
    return (speakerSpacingM > 0.f) ? c / (2.f * speakerSpacingM) : 8000.f;
}
// One-pole lowpass por canal: y += a(x - y), a = 1 - exp(-2π f_c / sr)
// F2/486: plantilla con default float + alias (señal Scalar; la geometría
// —espaciado y SR— sigue siendo float = control).
template <typename Scalar = float>
struct SpatialAliasGuardT {
    Scalar a = Scalar(1.0), yL = Scalar(0.0), yR = Scalar(0.0);
    void init(float spacingM, float sampleRate) noexcept {
        const float fc = spatialAliasCutoffHz(spacingM);
        a = Scalar(1.0) - std::exp(-Scalar(2.0) * Scalar(3.141592653589793) * static_cast<Scalar>(fc) / static_cast<Scalar>(sampleRate));
    }
    inline void process(Scalar& L, Scalar& R) noexcept {
        yL += a * (L - yL); yR += a * (R - yR); L = yL; R = yR;
    }
    void reset() noexcept { yL = yR = Scalar(0.0); }
};
using SpatialAliasGuard = SpatialAliasGuardT<float>;
using SpatialAliasGuardD = SpatialAliasGuardT<double>;

// ═══ Motor de protección de audio profesional (salida WFS) ═══════════════════
// Cadena determinista aplicada a TODA salida WFS antes del mixer:
//   DC removal -> denormal guard -> soft limiter (tanh suave) -> true-peak hold
// Garantiza: cero clipping, cero NaN/Inf, cero DC, cero saturacion por suma
// coherente de altavoces — una mala geometria WFS nunca rompe el audio.
// FALLO 6 (2026-11-18): esta struct vivía al final de este header y se
// instanciaba como thread_local compartido en WfsRenderer.cpp; se mueve
// aquí arriba para que WfsRenderer pueda tenerla como MIEMBRO por instancia
// (ver wfsProtection_): cada renderer tiene su cadena propia, sin estado
// compartido entre instancias del mismo hilo. La struct en sí no cambia.
// F2/486: plantilla con default float + alias (señal Scalar).
template <typename Scalar = float>
struct WfsProtectionChainT {
    // DC removal: one-pole highpass y = x - x_prev + r*y_prev, r ~ 0.995
    Scalar r_ = Scalar(0.995), xL_ = Scalar(0.0), yL_ = Scalar(0.0), xR_ = Scalar(0.0), yR_ = Scalar(0.0);
    // True-peak hold con decaimiento lento (evita pumping)
    Scalar peakHold_ = Scalar(0.0);

    void init(float sampleRate) noexcept {
        // Fc ~ 5 Hz para DC removal sin tocar graves
        r_ = std::exp(-Scalar(2.0) * Scalar(3.141592653589793) * Scalar(5.0) / static_cast<Scalar>(sampleRate));
        xL_ = yL_ = xR_ = yR_ = peakHold_ = Scalar(0.0);
    }
    void reset() noexcept { xL_ = yL_ = xR_ = yR_ = peakHold_ = Scalar(0.0); }

    // Soft limiter tanh con normalizacion: |x|<=1 pasa casi lineal, picos
    // se comprimen suavemente a <= ~0.99. Knee suave, sin discontinuidad.
    static inline Scalar softLimit(Scalar x) noexcept {
        // Knee en 0.99: por debajo pasa BIT-EXACTO (transparente de verdad).
        // Por encima: tanh asintotico a 1.0 -> imposible clipear, continuo en
        // el knee (tanh(0)=0), sin salto de valor ni de pendiente audible.
        const Scalar a = std::fabs(x);
        if (a <= Scalar(0.99)) return x;
        const Scalar lim = Scalar(0.99) + Scalar(0.01) * std::tanh((a - Scalar(0.99)) * Scalar(20.0));
        return std::copysign(lim, x);
    }

    inline Scalar protectChannel(Scalar x, Scalar& xp, Scalar& yp) noexcept {
        // 1. NaN/Inf -> silencio (nunca propagar)
        if (!std::isfinite(x)) { xp = Scalar(0.0); yp = Scalar(0.0); return Scalar(0.0); }
        // 2. Denormal guard: flush subnormales a cero
        if (std::fabs(x) < Scalar(1e-30)) x = Scalar(0.0);
        // 3. DC removal (one-pole highpass)
        const Scalar y = x - xp + r_ * yp; xp = x; yp = y;
        // 4. Denormal en la recursion
        Scalar out = (std::fabs(y) < Scalar(1e-30)) ? Scalar(0.0) : y;
        // 5. True-peak tracking (para telemetria/diagnostico)
        const Scalar a = std::fabs(out);
        peakHold_ = a > peakHold_ ? a : peakHold_ * Scalar(0.9995);
        // 6. Soft limiter siempre: identidad bajo 0.99, compresion arriba
        out = softLimit(out);
        return out;
    }
    inline void process(Scalar& L, Scalar& R) noexcept {
        L = protectChannel(L, xL_, yL_);
        R = protectChannel(R, xR_, yR_);
    }
    // Salud del motor para la UI (FASE 6): true-peak sostenido
    Scalar truePeak() const noexcept { return peakHold_; }
};
using WfsProtectionChain = WfsProtectionChainT<float>;
using WfsProtectionChainD = WfsProtectionChainT<double>;
} // namespace ivanna::wfs

namespace ivanna::spatial {

// F2/486 (labor grande, motor double): plantilla con default float +
// alias. WfsRenderer = float histórico bit-idéntico;
// WfsRendererD = núcleo double (lo usa FusedSpatial).
// El plano de SEÑAL (líneas de delay, taps suavizados, envolventes,
// historiales ITD, ganancias de altavoz, mezcla bypass) es Scalar; la
// geometría (posiciones, dimensiones de sala, azimutes) sigue float.
template <typename Scalar = float>
class WfsRendererT {
public:
    static constexpr int   kMaxSpeakers   = 32;
    static constexpr int   kMaxObjects    = 32;   // ranuras fijas de fuentes
    static constexpr float kSpeedOfSound  = 343.0f;    // m/s
    static constexpr float kArrayRadiusM  = 1.5f;      // radio del array virtual
    static constexpr float kRefDistanceM  = 1.0f;      // distancia de referencia 0 dB
    static constexpr float kMaxObjectDist = 8.0f;      // clamp de distancia de fuente
    static constexpr float kMaxItdS       = 0.00063f;  // ITD máximo interaural
    // Constante del one-pole de suavizado de delay/ganancia (~5 ms @48k):
    // lo bastante rápida para seguir gestos de UI, lo bastante lenta para
    // que el cambio de delay sea un glide inaudible en vez de un salto.
    static constexpr float kSmoothCoeff   = 0.004f;

    bool init(float sampleRate, int blockSize, int numSpeakers = 16) noexcept;
    void reset() noexcept;

    // Fase 2 (geometría 3D real): sustituye el array circular generado por
    // 7 posiciones explícitas relativas al oyente (metros; dyFwd=frente+,
    // dz=altura respecto al oído). Distancia/retardo/ganancia usan la
    // distancia 3D real (incluye altura) — no solo el plano horizontal.
    void setSpeakerLayout3D(const float* dx, const float* dyFwd, const float* dz,
                             int count) noexcept;

    // Activación/desactivación del renderer completo (independiente de las
    // fuentes individuales) — ver blendWithBypass(). Arranca en 1/1
    // (activo y asentado): quien use process() sin tocar setEnabled()
    // obtiene el comportamiento de siempre (compatibilidad con el uso
    // existente de esta clase).
    void setEnabled(bool enabled) noexcept { enabledTarget_ = enabled ? Scalar(1.0) : Scalar(0.0); }
    Scalar aliasGuardCoefficient() const noexcept { return aliasGuard_.a; }
    bool isEnabled() const noexcept { return enabledTarget_ > Scalar(0.5); }
    bool isSettledBypassed() const noexcept {
        return enabledTarget_ <= Scalar(0.0) && enabledMix_ <= Scalar(0.0);
    }
    // Mezcla bypass<->WFS con transición suave (~15 ms, smoothstep: derivada
    // cero en ambos extremos). wetL/R = salida de process() para el MISMO
    // bloque (buffer inicializado a 0 antes de process()). in-place seguro.
    void blendWithBypass(const Scalar* dryL, const Scalar* dryR,
                          const Scalar* wetL, const Scalar* wetR,
                          Scalar* outL, Scalar* outR, int frames) noexcept;

    // Posición/actualización de fuente primaria. x/y en metros relativos al
    // oyente (x: derecha+, y: frente+). gain lineal. thread-safety: llamar
    // desde el hilo de control entre bloques. NUNCA resetea historiales:
    // mover una fuente suaviza delay/ganancia sin tocar el audio en vuelo.
    void setObject(int id, float x, float y, float gain) noexcept;

    // ── FASE 6: 4D FIELD SYNTHESIS SUPREME ──────────────────────────────────
    // Objeto dinámico 4D con profundidad variable (y), altura perceptual (z),
    // vector de velocidad cinemática continua (vx, vy, vz) e interacción física
    // con las fronteras de la habitación (roomCoupling).
    void setObject4D(int id, float x, float y, float z,
                     float vx, float vy, float vz,
                     float gain, float roomCoupling = 0.25f) noexcept;

    // Configura las dimensiones físicas del recinto para acoplamiento de
    // reflexiones de primer orden dentro del campo WFS 4D.
    void setRoomDimensions(float widthM, float depthM, float heightM, float wallAbsorption = 0.25f) noexcept;

    void removeObject(int id) noexcept;   // fade-out de un bloque, sin click
    void clearObjects() noexcept;

    // Procesa UN bloque acumulando en outL/outR (suma, no sobrescribe,
    // para componer con el resto de la cadena espacial). objectInputs[i]
    // corresponde a la i-ésima fuente viva en orden de registro — misma
    // semántica de orden que la versión anterior.
    void process(const Scalar* const* objectInputs, int numObjects,
                 Scalar* outL, Scalar* outR, int frames) noexcept;

    int  numSpeakers() const noexcept { return numSpeakers_; }
    int  numObjects()  const noexcept { return numActiveObjects_; }

private:
    // Estado de UNA fuente primaria (ranura persistente, índice fijo).
    // La línea de delay es POR FUENTE: todos los altavoces leen la misma
    // señal retardada cada uno con su propio delay fraccionario.
    struct SourceSlot {
        int   id = -1;
        bool  active = false;
        Scalar x = Scalar(0.0), y = Scalar(0.0), z = Scalar(0.0);
        Scalar vx = Scalar(0.0), vy = Scalar(0.0), vz = Scalar(0.0);
        Scalar gain = Scalar(1.0);
        Scalar roomCoupling = Scalar(0.0);
        Scalar elevState = Scalar(0.0);
        // Envolvente de salida (fade-out glitch-free al eliminar). 0..1.
        Scalar actEnv = Scalar(1.0);
        Scalar actTarget = Scalar(1.0);
        Scalar envFrom = Scalar(1.0);            // envolvente al inicio del bloque
        Scalar envDelta = Scalar(0.0);           // target - from (transicion de 1 bloque)
        std::vector<Scalar> line;        // historial circular de la fuente
        int   writePos = 0;
    };

    // Par (fuente × altavoz): solo estado suavizado (la señal vive en el
    // slot de la fuente — compartida por los 32 taps de esa fuente).
    struct DelayTap {
        Scalar delayTarget = Scalar(0.0);   // delay físico objetivo (muestras, frac)
        Scalar delaySmooth = Scalar(0.0);   // delay efectivo suavizado (muestras, frac)
        Scalar gainTarget  = Scalar(0.0);   // driving gain objetivo
        Scalar gainSmooth  = Scalar(0.0);   // driving gain suavizada
    };

    Scalar sampleRate_ = Scalar(48000.0);
    int   blockSize_  = 384;
    int   numSpeakers_ = 16;

    std::array<SourceSlot, kMaxObjects> slots_{};
    int numActiveObjects_ = 0;

    // Geometría del array.
    std::array<float, kMaxSpeakers> speakerAzimuth_{};
    std::array<Scalar, kMaxSpeakers> speakerGainL_{}, speakerGainR_{};
    std::array<int,   kMaxSpeakers> speakerItdL_{}, speakerItdR_{}; // en muestras
    // Fase 2 (geometría 3D real): posición relativa al oyente por altavoz,
    // en uso cuando explicitLayout_==true (setSpeakerLayout3D). dz = altura
    // respecto al oído — entra en la distancia 3D real (no solo horizontal).
    bool  explicitLayout_ = false;
    std::array<float, kMaxSpeakers> speakerRelX_{}, speakerRelY_{}, speakerRelZ_{};

    // Activación/desactivación del renderer completo — ver setEnabled()/
    // blendWithBypass(). Arranca en 1/1: process() sin tocar setEnabled()
    // se comporta exactamente como antes de esta feature (compatibilidad).
    Scalar enabledTarget_ = Scalar(1.0);
    Scalar enabledMix_    = Scalar(1.0);

    // Fase 6: Geometría física de la habitación para interacción de bordes 4D
    float roomWidthM_     = 6.5f;
    float roomDepthM_     = 8.2f;
    float roomHeightM_    = 3.4f;
    float wallAbsorption_ = 0.25f;

    // Taps (fuente × altavoz) — matriz plana [slot*kMaxSpeakers + spk],
    // preasignada en init() (kMaxObjects × kMaxSpeakers entradas fijas).
    std::vector<DelayTap> taps_;
    int maxDelayTap_ = 0;

    // 256 muestras cubren el ITD máximo (242 muestras) a 384 kHz.
    std::array<std::array<Scalar, 256>, kMaxSpeakers> itdHist_{};
    std::array<int, kMaxSpeakers>                   itdPos_{};

    // FIX (2026-09-18 → cableado real): antes definido en el .cpp, fuera de
    // esta clase y de su namespace — matemática correcta, jamás llamado
    // desde process(). Verificado: con la geometría por defecto (16
    // altavoces, radio 1.5 m) la separación es ~0.59 m → f_alias ≈ 291 Hz,
    // muy por debajo del rango donde vive la mayoría de la energía de
    // música/voz real — sin este filtro, prácticamente todo el contenido
    // audible se reconstruye por encima de la frecuencia de aliasing
    // espacial del array (imágenes fantasma / coloración tipo comb).
    ivanna::wfs::SpatialAliasGuardT<Scalar> aliasGuard_;

    // FALLO 6 (2026-11-18): la cadena de protección vivía como thread_local
    // compartido en WfsRenderer.cpp (g_wfsProtection): todas las instancias
    // del mismo hilo compartían DC-removal, true-peak y estado — dos
    // renderers a distinta SR se re-inicializaban mutuamente en cada bloque
    // (pérdida de estado) y el estado de uno contaminaba al otro. Ahora es
    // miembro por instancia: cada renderer tiene su cadena propia.
    // ensureWfsProtection() re-inicializa solo si cambia la SR (misma
    // semántica lazy que el thread_local: init()/reset() NO la tocan).
    ivanna::wfs::WfsProtectionChainT<Scalar> wfsProtection_;
    float wfsProtectionSampleRate_ = 0.0f;
    bool  wfsProtectionInit_ = false;
    void ensureWfsProtection(float sampleRate) noexcept;

    // Limitador suave de seguridad (anti-clip => anti-tronidos): por debajo
    // de |1.0| es EXACTAMENTE lineal (cero coloracion); por encima comprime
    // de forma continua hacia un techo asintotico de 2.0. La suma coherente
    // de N altavoces frontales (focus=cos, ILD x1.41, gain<=2) podia superar
    // +-2.8 -> clip duro en el destino = tronido. Esto lo vuelve imposible.
    static inline Scalar softLimit(Scalar x) noexcept {
        const Scalar a = std::fabs(x);
        if (a <= Scalar(0.99)) return x;                       // identidad bit-exacta
        // Asintota 0.999999 < 1.0: imposible clipear, continuo en el knee
        const Scalar lim = Scalar(0.99) + Scalar(0.009999) * std::tanh((a - Scalar(0.99)) * Scalar(20.0));
        return std::copysign(lim, x);
    }

    void rebuildGeometry() noexcept;            // altavoces: azimut, ILD, ITD
    void computeTapTargets(int slot) noexcept;  // delays/ganancias objetivo
    int  findSlot(int id) const noexcept;       // -1 si no existe
    int  allocSlot() noexcept;                  // -1 si está lleno
};

// Alias de compat: el float histórico, bit-idéntico; el double es el
// núcleo de la labor grande.
using WfsRenderer = WfsRendererT<float>;
using WfsRendererD = WfsRendererT<double>;

} // namespace ivanna::spatial

// NOTA FALLO 6 (2026-11-18): WfsProtectionChain vivía definida aquí abajo;
// se movió arriba, junto a SpatialAliasGuard, para que WfsRenderer la tenga
// como miembro por instancia. La struct no cambió, solo su ubicación.
