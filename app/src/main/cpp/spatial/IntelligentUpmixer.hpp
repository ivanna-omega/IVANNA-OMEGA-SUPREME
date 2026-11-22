#pragma once

#include "HoaGainMatrix.hpp"
#include "TransientDetector.hpp"
#include <array>
#include <vector>
#include <cmath>
#include <cstddef>

namespace Ivanna {

/**
 * IntelligentUpmixer — upmixing estéreo → campo Ambisonics (HOA) de orden ≤2,
 * plano horizontal, con gestión de graves mono-segura y apertura lateral
 * controlada por "inmersividad".
 *
 * DOCTRINA (heredada del encargo HOA): sólo se construye sobre código y física
 * reales. Este motor es 2D (plano horizontal) — cualquier "elevación" es una
 * ilusión espectral, nunca un eje inventado. Documentado así a propósito.
 *
 * DISEÑO VERIFICABLE (no decorativo):
 *  - La imagen estéreo se preserva como par exacto de fuentes virtuales a ±30°
 *    (la codificación canónica estéreo→HOA): HoaGainMatrix::encode() es una
 *    identidad matemática en el plano horizontal, no una aproximación.
 *  - Crossover COMPLEMENTARIO de 2º orden (dos polos, 12 dB/oct) sobre el mid:
 *    bass + agudos == mid EXACTO en cada muestra (suma constante, sin error de
 *    fase en el corte). El grave se codifica al centro (mono-seguro).
 *  - Transientes: detector real sobre el MONO (no sobre un canal suelto).
 *    Cuando dispara, se ensancha la apertura lateral (aire percibido), nunca
 *    elevación real.
 *  - Inmersividad suavizada por muestra (sin zipper). Cero malloc en RT.
 *
 * F2/486 (labor grande, motor double): plantilla con default float +
 * alias. IntelligentUpmixer = float histórico bit-idéntico;
 * IntelligentUpmixerD = núcleo double (lo usa FusedSpatial). Los
 * parámetros (enabled, inmersividad) siguen siendo float = control; el
 * detector de transientes ya era double por dentro (su salida es bool).
 */
template <typename Scalar = float>
class IntelligentUpmixerT {
public:
    static constexpr std::size_t kMaxBlockFrames = 8192;

    IntelligentUpmixerT() = default;
    ~IntelligentUpmixerT() = default;

    void prepare(float sampleRate) noexcept;

    void setUpmixingEnabled(bool enable) noexcept { enabled_ = enable; }
    bool isUpmixingEnabled() const noexcept { return enabled_; }

    void  setImmersivity(float value) noexcept;
    float getImmersivity() const noexcept { return static_cast<float>(targetImmersivity_); }

    /**
     * Sobrecarga RT primaria (cero asignaciones dinámicas): opera sobre un
     * arreglo pre-alojado de `HoaVector` de longitud `numFrames`.
     */
    void processBlock(const Scalar* inL, const Scalar* inR,
                      HoaVectorT<Scalar>* outField, std::size_t numFrames) noexcept;

    /**
     * Sobrecarga compatible con `std::vector<HoaVector>`. Si `outField` ya
     * fue pre-dimensionado a `numFrames` (como en `IvannaFusionEngine` y
     * `test_rt_no_alloc`), no realiza ninguna asignación.
     */
    void processBlock(const Scalar* inL, const Scalar* inR,
                      std::vector<HoaVectorT<Scalar>>& outField, std::size_t numFrames) noexcept {
        if (outField.size() != numFrames) {
            outField.resize(numFrames);
        }
        processBlock(inL, inR, outField.data(), numFrames);
    }

private:
    bool  enabled_             = false;
    Scalar targetImmersivity_   = Scalar(1.0);
    Scalar smoothedImmersivity_ = Scalar(1.0);
    Scalar sampleRate_          = Scalar(48000.0);

    Scalar lpfA_    = Scalar(0);   // coeficiente del polo del crossover
    Scalar smoothA_ = Scalar(0);   // coeficiente de suavizado de inmersividad
    Scalar bassZ1_  = Scalar(0);   // 1er polo del crossover (sobre mid)
    Scalar bassZ2_  = Scalar(0);   // 2º polo del crossover

    // Morfología de bases: mezcla entre el par estrecho (±30°) y el ancho
    // (0°/±90°) según inmersividad, con energía de campo plana.
    Scalar widthMorph_ = Scalar(0);        // 0 = estrecho, 1 = ancho (suavizado)
    Scalar transientWidth_ = Scalar(1.0);    // estrechamiento en el ataque (recupera en rampa)
    Scalar blockMix_     = Scalar(0);      // crossfade seco→upmix en toggle (0=seco,1=upmix)

    alignas(64) std::array<Scalar, kMaxBlockFrames> monoBuf_{};
    ivanna::TransientDetector transientDetector_;
};

// Alias de compat: el float histórico, bit-idéntico; el double es el
// núcleo de la labor grande.
using IntelligentUpmixer = IntelligentUpmixerT<float>;
using IntelligentUpmixerD = IntelligentUpmixerT<double>;

} // namespace Ivanna
