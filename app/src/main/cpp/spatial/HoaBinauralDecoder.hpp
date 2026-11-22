#pragma once

#include "HoaGainMatrix.hpp"
#include "hrtf_convolver.hpp"
#include <vector>
#include <memory>
#include <cstddef>
#include <array>

namespace Ivanna {

/**
 * HoaBinauralDecoder - Decodes a 2D HOA field to Binaural output via HRTF.
 * Implements N virtual speakers arranged in a horizontal ring (elevation=0).
 */
// F2/486 (labor grande, motor double): plantilla con default float +
// alias. HoaBinauralDecoder = float histórico bit-idéntico;
// HoaBinauralDecoderD = núcleo double (lo usa FusedSpatial).
// El plano de SEÑAL (decodeGains, monoBuffer, mix) es Scalar; los
// azimutes y los pesos max-rE son control → float.
template <typename Scalar = float>
class HoaBinauralDecoderT {
public:
    static constexpr std::size_t kMaxBlockFrames = 8192;

    HoaBinauralDecoderT() = default;
    ~HoaBinauralDecoderT() = default;

    /**
     * @param sampleRate Hz
     * @param numSpeakers Number of virtual speakers on the horizontal ring (e.g., 8)
     */
    void prepare(float sampleRate, size_t numSpeakers = 8) noexcept;
    
    void setHrtfProfile(std::shared_ptr<ivanna::SyntheticHRTF> profile) noexcept;

    /**
     * Pesos de decodificación por orden armónico (0, 1, 2) para el decoder
     * de muestreo horizontal: peso_base(orden) * max-rE(orden) * renorm,
     * donde la renormalización deja la ganancia en eje (fuente alineada
     * con un altavoz virtual) IDÉNTICA a la del decoder de muestreo plano
     * (peso_base solo, sin max-rE) — max-rE solo cambia la forma fuera de
     * eje (Daniel & Nicol, decodificación estándar para auriculares).
     * Público y puro (sin estado) para poder verificarse en tests sin
     * pasar por HRTFConvolver.
     */
    static std::array<float, 3> computeMaxReOrderWeights() noexcept;

    /**
     * @param inField Array of HOA vectors for each frame
     * @param outL Left output buffer
     * @param outR Right output buffer
     * @param numFrames Number of frames in block
     */
    void processBlock(const std::vector<HoaVectorT<Scalar>>& inField, Scalar* outL, Scalar* outR, std::size_t numFrames) noexcept;

private:
    float sampleRate_ = 48000.0f;
    size_t numSpeakers_ = 8;
    std::shared_ptr<ivanna::SyntheticHRTF> hrtfProfile_;
    
    struct VirtualSpeaker {
        float azimuthRad;
        HoaVectorT<Scalar> decodeGains;
        ivanna::HRTFConvolverT<Scalar> convolver;
        
        // Buffer mono pre-alojado de capacidad fija para decodificar HOA -> Speaker sin malloc en RT
        alignas(64) std::array<Scalar, kMaxBlockFrames> monoBuffer{};
    };
    
    // unique_ptr: HRTFConvolver contiene std::atomic (ni copiable ni movible),
    // asi que vector<VirtualSpeaker> plano no compila (resize exige move).
    std::vector<std::unique_ptr<VirtualSpeaker>> speakers_;
    
    // Internal mix buffers pre-alojados de capacidad fija (cero resize en RT)
    alignas(64) std::array<Scalar, kMaxBlockFrames> mixL_{};
    alignas(64) std::array<Scalar, kMaxBlockFrames> mixR_{};
};

// Alias de compat: el float histórico, bit-idéntico; el double es el
// núcleo de la labor grande.
using HoaBinauralDecoder = HoaBinauralDecoderT<float>;
using HoaBinauralDecoderD = HoaBinauralDecoderT<double>;

} // namespace Ivanna
