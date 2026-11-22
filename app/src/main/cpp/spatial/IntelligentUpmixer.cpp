#include "IntelligentUpmixer.hpp"
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace Ivanna {

namespace {
// F2/486: constantes del diseño en double exacto; cada instanciación las
// castea a su Scalar (en float dan exactamente los literales f históricos).
constexpr double kCrossoverHz = 140.0;    // graves por debajo → centro (mono-seguro)
constexpr double kSmoothMs    = 15.0;     // suavizado de inmersividad / morphing
constexpr double kTransRecoverMs = 20.0;  // recuperación del ancho tras un transiente

template <typename Scalar>
inline Scalar sanitize(Scalar v) noexcept { return std::isfinite(v) ? v : Scalar(0); }
template <typename Scalar>
inline Scalar clamp01(Scalar v) noexcept {
    if (!std::isfinite(v)) return Scalar(0);
    return v < Scalar(0) ? Scalar(0) : (v > Scalar(1) ? Scalar(1) : v);
}
} // namespace

template <typename Scalar>
void IntelligentUpmixerT<Scalar>::prepare(float sampleRate) noexcept {
    sampleRate_ = (std::isfinite(sampleRate) && sampleRate > 0.0f) ? static_cast<Scalar>(sampleRate) : Scalar(48000.0);
    transientDetector_.prepare(static_cast<float>(sampleRate_));
    transientDetector_.setThreshold(3.0f);

    const Scalar dt = Scalar(1.0) / sampleRate_;
    const Scalar RC = Scalar(1.0) / (Scalar(2.0) * static_cast<Scalar>(M_PI) * static_cast<Scalar>(kCrossoverHz));
    lpfA_    = dt / (RC + dt);
    smoothA_ = Scalar(1.0) - std::exp(-dt / (static_cast<Scalar>(kSmoothMs) * Scalar(0.001)));

    bassZ1_ = bassZ2_ = Scalar(0);
    smoothedImmersivity_ = targetImmersivity_;
    widthMorph_ = targetImmersivity_;
    transientWidth_ = Scalar(1.0);
}

template <typename Scalar>
void IntelligentUpmixerT<Scalar>::setImmersivity(float value) noexcept {
    targetImmersivity_ = clamp01<Scalar>(static_cast<Scalar>(value));
}

template <typename Scalar>
void IntelligentUpmixerT<Scalar>::processBlock(const Scalar* inL, const Scalar* inR,
                                      HoaVectorT<Scalar>* outField,
                                      std::size_t numFrames) noexcept {
    if (numFrames == 0 || inL == nullptr || inR == nullptr || outField == nullptr) return;
    const std::size_t frames = std::min(numFrames, kMaxBlockFrames);

    // Bases con ENERGÍA UNITARIA exacta (encodeUnitPower): la energía del campo
    // no depende del azimut ni de cuántas fuentes se mezclen — la inmersividad
    // cambia la ANCHURA, nunca el NIVEL.
    // REFINAMIENTO (2026-09-17): las 5 bases de codificación son constantes
    // del sistema — antes se recalculaban en CADA bloque de audio (5
    // evaluaciones de encodeUnitPower por llamada a processBlock). Ahora se
    // evalúan una sola vez por proceso (static const, inicialización
    // thread-safe garantizada por C++11) y el hot path solo las lee.
    static const HoaVectorT<Scalar> encNarrowL = HoaGainMatrix::encodeUnitPower<Scalar>( static_cast<Scalar>(M_PI) / Scalar(6.0)); // +30°
    static const HoaVectorT<Scalar> encNarrowR = HoaGainMatrix::encodeUnitPower<Scalar>(-static_cast<Scalar>(M_PI) / Scalar(6.0)); // -30°
    static const HoaVectorT<Scalar> encWideL   = HoaGainMatrix::encodeUnitPower<Scalar>( static_cast<Scalar>(M_PI) / Scalar(2.0)); // +90°
    static const HoaVectorT<Scalar> encWideR   = HoaGainMatrix::encodeUnitPower<Scalar>(-static_cast<Scalar>(M_PI) / Scalar(2.0)); // -90°
    static const HoaVectorT<Scalar> encCenter  = HoaGainMatrix::encodeUnitPower<Scalar>(Scalar(0.0));                              // 0°

    // FIX REAL (eco/desface al activar/desactivar el toggle, reporte del
    // propietario 2026-09-17 — el intento anterior en este mismo archivo
    // declaraba blockMix_/mixStep en comentarios pero NUNCA los usaba para
    // mezclar nada; la rama de bypass seguia siendo el mismo salto duro).
    //
    // Causa: al activar/desactivar, la señal saltaba de golpe entre la ruta
    // directa (sin latencia) y la ruta HOA+HRTF (con su propia latencia
    // FIR) — la cola del filtro y el cambio de fase se oian como un eco
    // breve y un desface entre canales. Ahora TODA transicion (activar,
    // desactivar, y tambien el paso por w=0 de inmersividad, que antes
    // tenia su propio salto a un camino de calculo distinto) se resuelve
    // con un crossfade por muestra entre la señal seca (blockMix_=0) y la
    // procesada (blockMix_=1), avanzando ~1/15ms por muestra — ambas rutas
    // suenan superpuestas y en fase durante la transicion, nunca una
    // reemplazando a la otra de un bloque a otro.
    const Scalar mixStep   = Scalar(1.0) / (sampleRate_ * Scalar(0.015));
    const Scalar mixTarget = enabled_ ? Scalar(1.0) : Scalar(0.0);

    // Camino rapido: completamente asentado en bypass sin transicion en
    // curso — evita el coste del crossover/detector de transientes en el
    // caso comun (upmixing apagado, que es el valor por defecto real).
    if (!enabled_ && blockMix_ <= Scalar(0)) {
        for (std::size_t i = 0; i < frames; ++i) {
            HoaVectorT<Scalar> out{};
            HoaGainMatrix::accumulate(out, encNarrowL, sanitize<Scalar>(inL[i]));
            HoaGainMatrix::accumulate(out, encNarrowR, sanitize<Scalar>(inR[i]));
            outField[i] = out;
        }
        return;
    }

    // Detección de transientes sobre el pico estéreo max(|L|,|R|) — no sobre el
    // mid (que cancela golpes paneados) ni sobre un solo canal. El resultado SÍ
    // se usa: en el ataque se estrecha el ancho (transiente localizado al
    // frente) y se recupera en rampa de ~20 ms.
    for (std::size_t i = 0; i < frames; ++i) {
        const Scalar a = std::fabs(sanitize<Scalar>(inL[i]));
        const Scalar b = std::fabs(sanitize<Scalar>(inR[i]));
        monoBuf_[i] = a > b ? a : b;
    }
    const bool hasTransients = transientDetector_.processBlock(monoBuf_.data(), frames);
    const Scalar transTarget  = hasTransients ? Scalar(0.55) : Scalar(1.0); // estrechar en el ataque
    const Scalar transA       = Scalar(1.0) - std::exp(-Scalar(1.0) / (sampleRate_ * static_cast<Scalar>(kTransRecoverMs) * Scalar(0.001)));

    for (std::size_t i = 0; i < frames; ++i) {
        const Scalar l = sanitize<Scalar>(inL[i]);
        const Scalar r = sanitize<Scalar>(inR[i]);

        // Avanza el crossfade seco<->upmix un paso por muestra hacia su destino.
        if (blockMix_ < mixTarget)      blockMix_ = std::min(blockMix_ + mixStep, mixTarget);
        else if (blockMix_ > mixTarget) blockMix_ = std::max(blockMix_ - mixStep, mixTarget);

        const Scalar mid  = Scalar(0.5) * (l + r);
        const Scalar side = Scalar(0.5) * (l - r);

        // Crossover complementario de 2º orden sobre el mid: bass + midHi == mid
        // EXACTO en cada muestra (suma constante, sin error de fase en el corte).
        bassZ1_ += lpfA_ * (mid - bassZ1_);
        bassZ2_ += lpfA_ * (bassZ1_ - bassZ2_);
        const Scalar bass  = bassZ2_;
        const Scalar midHi = mid - bass;

        // Suavizado por muestra de inmersividad y morphing (sin zipper).
        smoothedImmersivity_ += smoothA_ * (targetImmersivity_ - smoothedImmersivity_);
        widthMorph_          += smoothA_ * (smoothedImmersivity_ - widthMorph_);
        transientWidth_      += transA  * (transTarget - transientWidth_);

        const Scalar w = smoothedImmersivity_;
        // Morfología de bases: el par directo se abre de ±30° a ±90° según la
        // inmersividad, con energía de campo plana (bases unitarias). Nota:
        // esta fórmula converge EXACTAMENTE a la mezcla seca cuando w→0
        // (gN→1, gW→0, midHi/bass/side ponderados por w→0) — por eso ya no
        // hace falta un branch aparte para "inmersividad ~0": el propio
        // crossfade blockMix_ cubre activar/desactivar, y esta continuidad
        // matemática cubre el barrido del slider de inmersividad hasta 0.
        const Scalar m = widthMorph_ * transientWidth_;
        const Scalar gN = Scalar(1.0) - Scalar(0.5) * m;   // peso del par estrecho
        const Scalar gW = Scalar(0.5) * m;          // peso del par ancho

        HoaVectorT<Scalar> wet{};
        // 1) Par estéreo directo (estrecho ↔ ancho, energía plana).
        HoaGainMatrix::accumulate(wet, encNarrowL, l * gN);
        HoaGainMatrix::accumulate(wet, encNarrowR, r * gN);
        HoaGainMatrix::accumulate(wet, encWideL,   l * gW);
        HoaGainMatrix::accumulate(wet, encWideR,   r * gW);
        // 2) Expansión inmersiva: presencia central + graves mono-seguros al
        //    centro + apertura lateral difusa.
        HoaGainMatrix::accumulate(wet, encCenter, midHi * w);
        HoaGainMatrix::accumulate(wet, encCenter, bass  * w);
        HoaGainMatrix::accumulate(wet, encWideL,   side  * w * Scalar(0.5));
        HoaGainMatrix::accumulate(wet, encWideR,  -side  * w * Scalar(0.5));

        if (blockMix_ >= Scalar(0.999999)) {
            // Upmix pleno, sin transición en curso: no hay nada que mezclar.
            outField[i] = wet;
        } else {
            // Señal seca EXACTAMENTE igual a la del camino rápido de bypass
            // (misma codificación ±30°, ganancia unitaria) — continuidad
            // garantizada en blockMix_==0 con el otro extremo del crossfade.
            HoaVectorT<Scalar> dry{};
            HoaGainMatrix::accumulate(dry, encNarrowL, l);
            HoaGainMatrix::accumulate(dry, encNarrowR, r);
            HoaVectorT<Scalar> out{};
            for (int ch = 0; ch < kHoaNumChannels; ++ch) {
                out[static_cast<size_t>(ch)] =
                    dry[static_cast<size_t>(ch)] * (Scalar(1.0) - blockMix_) +
                    wet[static_cast<size_t>(ch)] * blockMix_;
            }
            outField[i] = out;
        }
    }
}

// Instanciaciones explícitas (las definiciones viven en este .cpp):
// float = ruta legacy bit-idéntica; double = núcleo de la labor grande.
template class IntelligentUpmixerT<float>;
template class IntelligentUpmixerT<double>;

} // namespace Ivanna
