// StereoObjectDecomposer.hpp — EJE 1: descomposición estéreo→objetos WFS.
// (c) 2026 LUPP / GORE TNS.
// De una mezcla estéreo deriva 4 objetos espaciales reales en tiempo real:
//   CENTER (mid, frente), L/R FLANK (side escalado a ±x), AMBIENTE (residual
//   decorrelado). Posiciones desde correlación intercanal + energía de bandas
//   (filtros 1 polo, sin FFT). Cero malloc/locks en process(); memoria
//   preasignada en prepare(). Pensado para alimentar WfsRenderer::process.
#pragma once
#include <cstddef>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <array>

namespace ivanna { namespace spatial {

struct ObjectPosition {
    float x{0.0f};
    float y{1.5f};
    float z{0.0f};
};

struct DecomposedObject {
    ObjectPosition position{};
    float spread{1.0f};
    float gain{1.0f};
    float energy{0.0f};
};

// F2/486 (labor grande, motor double): plantilla con default float +
// alias. StereoObjectDecomposer = float histórico bit-idéntico;
// StereoObjectDecomposerD = núcleo double (lo usa FusedSpatial).
// Los objetos (mid/side) y sus buffers son señal → Scalar; sideRatio_ /
// lowRatio_ y el layout canónico son estimaciones de control → float.
template <typename Scalar = float>
class StereoObjectDecomposerT {
public:
    static constexpr int kObjects = 4;  // 0=CENTER 1=LEFT 2=RIGHT 3=AMBIENT
    static constexpr int kMaxBlock = 4096;

    bool prepare(float sampleRate, int maxBlock) noexcept {
        if (sampleRate <= 0.f || maxBlock <= 0 || maxBlock > kMaxBlock) return false;
        sr_ = static_cast<Scalar>(sampleRate); block_ = maxBlock;
        // LPF 1 polo ~250 Hz para energía de graves (L/R comparten coef).
        lpC_ = Scalar(1.0) - std::exp(Scalar(-2.0) * Scalar(3.14159265358979323846) * Scalar(250.0) / sr_);
        posSmooth_ = Scalar(1.0) - std::exp(Scalar(-2.0) * Scalar(3.14159265358979323846) * Scalar(2.0) / sr_);
        reset();
        return true;
    }
    void reset() noexcept {
        lpL_ = lpR_ = Scalar(0); eMid_ = eSide_ = eLow_ = Scalar(0); n_ = 0;
        sideRatio_ = 0.5f; lowRatio_ = 0.25f;
        std::memset(objL_, 0, sizeof(objL_)); std::memset(objR_, 0, sizeof(objR_));
        updateObjectsArray();
    }

    // RT-safe: planar L y R en buffers de entrada separados; escribe en outObjects[4].
    void decompose(const Scalar* bufferL, const Scalar* bufferR, Scalar* const* outObjects, size_t numSamples) noexcept {
        if (!bufferL || !bufferR || !outObjects || numSamples == 0) return;
        size_t frames = (numSamples > static_cast<size_t>(kMaxBlock)) ? static_cast<size_t>(kMaxBlock) : numSamples;
        Scalar eMid = Scalar(0), eSide = Scalar(0), eLow = Scalar(0);
        for (size_t i = 0; i < frames; ++i) {
            const Scalar l = bufferL[i];
            const Scalar r = bufferR[i];
            const Scalar mid   = Scalar(0.5) * (l + r);
            const Scalar sideL = Scalar(0.5) * (l - r);
            const Scalar sideR = -sideL;
            lpL_ += lpC_ * (mid - lpL_);
            const Scalar lowM = lpL_;
            eMid  += mid * mid;
            eSide += sideL * sideL;
            eLow  += lowM * lowM;
            if (outObjects[0]) outObjects[0][i] = mid;
            if (outObjects[1]) outObjects[1][i] = sideL;
            if (outObjects[2]) outObjects[2][i] = sideR;
            // FIX BALANCE R: outObjects[3] (ambiente difuso) no debe inyectar una señal mono asimétrica (+sideL)
            // al renderizador espacial porque al ser paneada al centro cancela el canal derecho.
            if (outObjects[3]) outObjects[3][i] = Scalar(0.0);
            objL_[0 * kMaxBlock + i] = mid;
            objR_[0 * kMaxBlock + i] = mid;
            objL_[1 * kMaxBlock + i] = sideL;
            objR_[1 * kMaxBlock + i] = 0.f;
            objL_[2 * kMaxBlock + i] = 0.f;
            objR_[2 * kMaxBlock + i] = sideR;
            objL_[3 * kMaxBlock + i] = sideL * Scalar(0.35);
            objR_[3 * kMaxBlock + i] = sideR * Scalar(0.35);
        }
        const Scalar tot = eMid + Scalar(2.0) * eSide + Scalar(1e-9);
        const Scalar instSide = (Scalar(2.0) * eSide) / tot;
        const Scalar instLow  = eLow / (eMid + Scalar(1e-9));
        sideRatio_ += posSmooth_ * frames * (instSide - sideRatio_);
        lowRatio_  += posSmooth_ * frames * (instLow  - lowRatio_);
        n_ += static_cast<int>(frames);
        updateObjectsArray();
    }

    const std::array<DecomposedObject, kObjects>& getObjects() const noexcept {
        return objectsArray_;
    }

    // RT-safe: interleaved [L0,R0,...]; frames = muestras por canal.
    // Escribe 4 objetos (cada uno frames muestras) en objL_/objR_ internos.
    void processBlock(const Scalar* interleaved, int frames) noexcept {
        if (!interleaved || frames <= 0) return;
        if (frames > block_) frames = block_;
        Scalar eMid = Scalar(0), eSide = Scalar(0), eLow = Scalar(0);
        for (int i = 0; i < frames; ++i) {
            const Scalar l = interleaved[2 * i];
            const Scalar r = interleaved[2 * i + 1];
            const Scalar mid   = Scalar(0.5) * (l + r);
            const Scalar sideL = Scalar(0.5) * (l - r);
            const Scalar sideR = -sideL;
            // graves por LPF sobre mid
            lpL_ += lpC_ * (mid - lpL_);
            const Scalar lowM = lpL_;
            eMid  += mid * mid;
            eSide += sideL * sideL;
            eLow  += lowM * lowM;
            // objetos: center=mid (voz/bombo/bajo), flanks=side, ambient=side suavizado
            objL_[0 * kMaxBlock + i] = mid;
            objR_[0 * kMaxBlock + i] = mid;
            objL_[1 * kMaxBlock + i] = sideL;   // LEFT  = +side
            objR_[1 * kMaxBlock + i] = 0.f;
            objL_[2 * kMaxBlock + i] = 0.f;     // RIGHT = -side
            objR_[2 * kMaxBlock + i] = sideR;
            objL_[3 * kMaxBlock + i] = sideL * Scalar(0.35); // AMBIENTE residual
            objR_[3 * kMaxBlock + i] = sideR * Scalar(0.35);
        }
        const Scalar tot = eMid + Scalar(2.0) * eSide + Scalar(1e-9);
        const Scalar instSide = (Scalar(2.0) * eSide) / tot;   // 0=mono puro .. 1=muy ancho
        const Scalar instLow  = eLow / (eMid + Scalar(1e-9)); // 0..1 aprox
        sideRatio_ += posSmooth_ * frames * (instSide - sideRatio_);
        lowRatio_  += posSmooth_ * frames * (instLow  - lowRatio_);
        n_ += frames;
        updateObjectsArray();
    }

    // Punteros de entrada para WfsRenderer::process (planar por objeto).
    // fillObjectPtrs llena in[i] con el canal L de cada objeto (o R vía objR_).
    const Scalar* objectL(int id) const noexcept { return &objL_[id * kMaxBlock]; }
    const Scalar* objectR(int id) const noexcept { return &objR_[id * kMaxBlock]; }

    // Posiciones 3D canónicas según el análisis (x lateral, y distancia, gain).
    // width 0..1 (sideRatio) abre los flancos; lowRatio acerca el bajo al frente.
    void canonicalLayout(float x[4], float y[4], float g[4]) const noexcept {
        const float w = 0.3f + 0.9f * sideRatio_;      // apertura real medida
        const float lowPull = 0.3f * lowRatio_;         // bajo → más cerca
        x[0] = 0.0f;   y[0] = 2.0f - lowPull; g[0] = 1.0f;  // CENTER frente
        x[1] = -w;     y[1] = 1.6f;           g[1] = 0.9f;  // LEFT flank
        x[2] =  w;     y[2] = 1.6f;           g[2] = 0.9f;  // RIGHT flank
        x[3] = 0.0f;   y[3] = 3.0f;           g[3] = 0.5f;  // AMBIENTE atrás
    }

    float sideRatio() const noexcept { return sideRatio_; }
    float lowRatio()  const noexcept { return lowRatio_; }

    void updateObjectsArray() noexcept {
        float x[4], y[4], g[4];
        canonicalLayout(x, y, g);
        for (int i = 0; i < kObjects; ++i) {
            objectsArray_[i].position.x = x[i];
            objectsArray_[i].position.y = y[i];
            objectsArray_[i].position.z = 0.0f;
            objectsArray_[i].gain = g[i];
            objectsArray_[i].spread = 1.0f;
        }
    }

private:
    Scalar sr_ = Scalar(48000.0); int block_ = 0;
    Scalar lpC_ = Scalar(0), posSmooth_ = Scalar(0);
    Scalar lpL_ = Scalar(0), lpR_ = Scalar(0), eMid_ = Scalar(0), eSide_ = Scalar(0), eLow_ = Scalar(0);
    int   n_ = 0;
    float sideRatio_ = 0.5f, lowRatio_ = 0.25f;
    std::array<DecomposedObject, kObjects> objectsArray_{};
    // buffers planares preasignados (4 objetos × kMaxBlock) — cero malloc
    Scalar objL_[kObjects * kMaxBlock]{};
    Scalar objR_[kObjects * kMaxBlock]{};
}; 

// Alias de compat: el float histórico, bit-idéntico (IvannaAudioPipeline,
// ObjectSpatialRenderer, tests); el double es el núcleo de la labor grande.
using StereoObjectDecomposer = StereoObjectDecomposerT<float>;
using StereoObjectDecomposerD = StereoObjectDecomposerT<double>;

}} // namespace ivanna::spatial
