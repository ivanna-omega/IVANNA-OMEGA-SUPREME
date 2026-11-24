// fused_listener_room.cpp — OPERADOR #3, MODELO OYENTE+SALA ("SALA VIVA").
// Ver fused_listener_room.hpp para el mapeo de motores.
#include "fused/fused_listener_room.hpp"

#include <algorithm>

namespace ivanna::fused {

namespace {

// Perfil auditivo para HearingAdaptationEngine. El motor solo tiene dos
// mandos reales: el sellado (ganancia de graves) y la pérdida en 4/8 kHz
// (ganancia de agudos); ver recalculate() en HearingAdaptationEngine.hpp:
//   lowGain  = 1 + (1 - seal) * 0.75
//   highGain = clamp(10^(avgLoss4k8k / 40), 0.8, 2.0) * (1 - fatiga*0.25)
// HARMAN es una aproximación con esos dos mandos, no una curva Harman
// medida; CUSTOM es un audiograma moderado fijo hasta que la superficie
// de control tenga entrada de audiograma real.
ivanna::spatial::AudiogramProfile profileFor(int hearingProfile) noexcept {
    ivanna::spatial::AudiogramProfile p{};
    p.ear_tip_seal_factor = 1.0f;  // FLAT: lowGain 1.0, highGain 1.0
    if (hearingProfile == 1) {     // HARMAN
        p.ear_tip_seal_factor = 0.47f;  // lowGain 1.40 (+2.9 dB graves)
        p.loss_4khz_db = 2.0f;
        p.loss_8khz_db = 2.0f;  // highGain 10^(2/40) = 1.12 (+1.0 dB)
    } else if (hearingProfile == 2) {  // CUSTOM (placeholder)
        p.ear_tip_seal_factor = 0.70f;  // lowGain 1.225 (+1.8 dB graves)
        p.loss_4khz_db = 4.0f;
        p.loss_8khz_db = 6.0f;  // highGain 10^(5/40) = 1.33 (+2.5 dB)
    }
    return p;
}

}  // namespace

FusedListenerRoom::FusedListenerRoom() : bio_(0.5, 2.0) {
    controls_.clamp();
    applyControls();
}

void FusedListenerRoom::prepare(double sampleRate) noexcept {
    sampleRate_ = (std::isfinite(sampleRate) && sampleRate >= 8000.0 &&
                   sampleRate <= 384000.0)
                      ? sampleRate
                      : 48000.0;
    // Scratch pre-reservado (hilo de control; process() no aloca).
    dryL_.assign(kFusedMaxBlockFrames, 0.0);
    dryR_.assign(kFusedMaxBlockFrames, 0.0);
    // Scratch del campo difuso SDRF (trozos de kChunkFrames).
    diffuseInL_.assign(kChunkFrames, 0.0);
    diffuseInR_.assign(kChunkFrames, 0.0);
    diffuseOutL_.assign(kChunkFrames, 0.0);
    diffuseOutR_.assign(kChunkFrames, 0.0);
    diffuse_.prepare(sampleRate_);
    rir_.setSampleRate(static_cast<int>(sampleRate_));
    roomEq_.prepare(static_cast<float>(sampleRate_));
    applyControls();
    // FALLO 5: la BRIR sintetizada se horneó con la SR vigente en su
    // momento (p. ej. 48 kHz por defecto en el ctor); si prepare() corre
    // a otra SR, se re-hornea a la SR vigente — igual que
    // HolosOperator::prepare, que sintetiza a su SR de prepare(). Solo
    // pre-stream (!processed_): a mitad de stream la BRIR queda como
    // está, igual que en HOLOS·Ω (que jamás re-sintetiza tras prepare()).
    // Una IR de dataset (setRir) nunca se sobrescribe aquí.
    if (!processed_ && rirSynthesized_ &&
        rir_.needsMasterStudioBrirSynthesis(synthRt60_,
                                            static_cast<int>(sampleRate_))) {
        rir_.synthesizeMasterStudioBrir(synthRt60_,
                                        static_cast<int>(sampleRate_));
        rirWarmed_ = false;
    }
    if (rir_.isLoaded() && !rirWarmed_ && !processed_) warmupRir();
    prepared_ = true;
}

void FusedListenerRoom::setControls(const ListenerRoomControls& c) noexcept {
    controls_ = c;
    controls_.clamp();
    applyControls();
}

void FusedListenerRoom::reset() noexcept {
    hearing_.reset();
    bio_.reset();
    diffuse_.reset();
    diffuseActive_ = false;
    roomEq_.prepare(static_cast<float>(sampleRate_));
    applyControls();  // prepare() resetea los targets del EQ; reponerlos
}

void FusedListenerRoom::setRir(const float* irL, const float* irR, int irLen) noexcept {
    if (!irL || !irR || irLen <= 0) return;
    rir_.load(irL, irR, irLen);
    // Una IR de dataset reemplaza a la BRIR sintetizada: prepare() no
    // debe re-sintetizar por encima de ella (FALLO 5).
    rirSynthesized_ = false;
    synthRt60_ = -1.0f;
    rirWarmed_ = false;
    // Warm-up pre-stream (sin audio real corriendo): asienta wetNow_ a 1.0
    // para que la recuperación del wet sea exacta desde el primer bloque.
    if (!processed_) warmupRir();
}

// FALLO 5 (Fusión Magistral, 2026-11-18): misma BRIR Master Studio
// determinista que HolosOperator::prepare (RirConvolver::
// synthesizeMasterStudioBrir — seeds fijos, taps tempranos ITU-R BS.1116
// y cola con decaimiento exp(-60 dB en rt60)); mismo (rt60, sr) da la
// IR bit-idéntica a la que suena en HOLOS·Ω. Hilo de control.
bool FusedListenerRoom::synthesizeMasterStudioBrir(float rt60S) noexcept {
    const float rt60 = std::clamp(std::isfinite(rt60S) ? rt60S : 0.34f,
                                  0.18f, 1.80f);
    // No-op si esa (rt60, sr) ya está cargada: las llamadas repetidas del
    // snapshot (mismo RT60) no cuestan nada y nunca realocan.
    if (!rir_.needsMasterStudioBrirSynthesis(rt60,
                                             static_cast<int>(sampleRate_))) {
        return rir_.isLoaded();
    }
    rir_.synthesizeMasterStudioBrir(rt60, static_cast<int>(sampleRate_));
    synthRt60_ = rt60;
    rirSynthesized_ = true;
    rirWarmed_ = false;
    // Warm-up pre-stream, igual que setRir().
    if (!processed_) warmupRir();
    return rir_.isLoaded();
}

void FusedListenerRoom::applyControls() noexcept {
    // 1. Perfil auditivo.
    hearing_.setProfile(profileFor(controls_.hearingProfile));

    // 3. Corrección de sala, instancia propia, cut-only (Fase 4).
    roomEq_.setCutOnly(true);
    roomEq_.setTarget(controls_.roomCorrDb);

    // 6. Geometría: roomSizeM = profundidad de la sala; el layout de
    //    referencia (3.83 x 7.00 x 2.83 m) se escala proporcionalmente para
    //    que oyente y altavoces queden dentro de la sala.
    geometry_ = ivanna::spatial::RoomGeometryConfig{};
    const float scale = controls_.roomSizeM / 7.0f;
    geometry_.roomWidthM *= scale;
    geometry_.roomDepthM *= scale;
    geometry_.roomHeightM *= scale;
    geometry_.listenerX *= scale;
    geometry_.listenerY *= scale;
    geometry_.listenerZ *= scale;
    for (int i = 0; i < ivanna::spatial::RoomGeometryConfig::kNumSpeakers; ++i) {
        geometry_.speakerX[static_cast<std::size_t>(i)] *= scale;
        geometry_.speakerY[static_cast<std::size_t>(i)] *= scale;
        geometry_.speakerZ[static_cast<std::size_t>(i)] *= scale;
    }
    schroederHz_ = geometry_.schroederFrequencyHz(controls_.rt60Sec);
    // Limitador físico del wet: sala viva (T60>=1.2 s) -> wet 0; sala seca
    // (T60<=0.5 s) -> wet íntegro; entre medias, rampa lineal.
    effectiveWet_ = ivanna::spatial::RoomGeometryConfig::
        limitSyntheticReverbWetForRoomT60(controls_.roomWet, controls_.rt60Sec);

    // 4. SAF: el latente dorado SOFA (214 sujetos, horneado) acopla el
    //    convolver (claridad temprana, decorrelación tardía, delta RT60,
    //    escala del wet). Idempotente: el mismo latente siempre.
    rir_.applySofaCoupling(ivanna::master::kMasterSafGoldenQ);
    rir_.setSampleRate(static_cast<int>(sampleRate_));
}

void FusedListenerRoom::warmupRir() noexcept {
    if (!rir_.isLoaded()) return;
    // Dos búferes separados: process() escribe L y luego lee R; con el
    // mismo búfer, R leería la salida de L.
    double zL[512] = {};
    double zR[512] = {};
    rir_.setWetDry(1.0f);
    // wetNow_ es un one-pole por muestra con ws = exp(-1/(48000*0.010));
    // 8192 muestras lo asientan a 1 - e^-17 (~4e-8 de residuo).
    for (int i = 0; i < 8192; i += 512) rir_.process(zL, zR, 512);
    rirWarmed_ = true;
}

void FusedListenerRoom::processChunk(double* l, double* r, const double* dryL,
                                     const double* dryR, int n) noexcept {
    // 5. BioControl 5D: sin sensor (Fase 0) es identidad exacta; el lazo se
    //    mantiene alimentado con etaStar = eta0 (su fail-closed §3.4).
    bio_.update(static_cast<double>(n) / sampleRate_, bio_.eta0());

    // 2. RIR + campo difuso SDRF (la sala):
    // El campo difuso es la red unitaria SDRF (guías Weil/GUE + reflexión
    // Householder + all-pass Gray-Markel + Kerr + techo softReflect ≤0.95),
    // alimentada con los controles que YA hay: effectiveWet_ es la puerta
    // y la mezcla de potencia constante (igual que el RIR); rt60Sec es el
    // decaimiento del lazo solo sin IR cargada (con IR la cola RIR ya
    // decae y el campo solo la difunde, g=1); ρ_E = effectiveWet_.
    // Con effectiveWet_ = 0 el campo no corre y su estado se limpia: la
    // cadena con todo apagado sigue bit-exacta (Regla de Oro).
    if (effectiveWet_ > 0.0f) {
        const double ws = rir_.isLoaded()
            ? std::max(1e-6, static_cast<double>(rir_.wetScale())) : 1.0;
        if (rir_.isLoaded()) {
            rir_.setWetDry(1.0f);
            rir_.process(l, r, n);
            // out = 0.32*in + 0.82*ws*wet. Recuperar el wet puro, difundirlo
            // (sin decaimiento añadido) y recomponer con la misma forma.
            diffuseRirWet(l, r, dryL, dryR, n, ws);
        } else {
            // Sin IR cargada el campo difuso ES la sala: el seco alimenta
            // la red y su salida es el wet (con decaimiento RT60 propio).
            // out = 0.32*dry + 0.82*wetDifuso (misma forma que el convolver).
            diffuseFromDry(l, r, dryL, dryR, n);
        }
        // Mezcla de potencia constante gDry^2 + gWet^2 = 1 (sin cambios:
        // la fórmula es la misma porque el wet llega con la misma forma).
        const double gWet = static_cast<double>(effectiveWet_);
        const double gDry = std::sqrt(std::max(0.0, 1.0 - gWet * gWet));
        const double b = gWet / (0.82 * ws);
        const double a = gDry - 0.32 * b;
        for (int i = 0; i < n; ++i) {
            l[i] = a * dryL[i] + b * l[i];
            r[i] = a * dryR[i] + b * r[i];
        }
        diffuseActive_ = true;
    } else if (diffuseActive_) {
        diffuse_.reset();
        diffuseActive_ = false;
    }

    // 3. Corrección de sala cut-only (no-op bit-exacto si targets en 0).
    roomEq_.process(l, r, n);
}

// Recupera el wet puro del convolver, lo difunde con el campo SDRF (sin
// decaimiento añadido: la cola RIR ya decae) y recompone la salida con la
// misma forma (0.32*in + 0.82*ws*wetDifuso), in-place sobre l/r.
void FusedListenerRoom::diffuseRirWet(double* l, double* r, const double* dryL,
                                      const double* dryR, int n, double ws) noexcept {
    diffuse_.setRt60(0.0f);  // difusión pura (g=1, kernel verificado)
    diffuse_.setDensity(effectiveWet_);
    for (int i = 0; i < n; ++i) {
        diffuseInL_[static_cast<std::size_t>(i)] =
            (l[i] - 0.32 * dryL[i]) / (0.82 * ws);
        diffuseInR_[static_cast<std::size_t>(i)] =
            (r[i] - 0.32 * dryR[i]) / (0.82 * ws);
    }
    diffuse_.process(diffuseInL_.data(), diffuseInR_.data(),
                     diffuseOutL_.data(), diffuseOutR_.data(), n);
    for (int i = 0; i < n; ++i) {
        l[i] = 0.32 * dryL[i] + 0.82 * ws * diffuseOutL_[static_cast<std::size_t>(i)];
        r[i] = 0.32 * dryR[i] + 0.82 * ws * diffuseOutR_[static_cast<std::size_t>(i)];
    }
}

// Sin IR: el seco alimenta el campo (con decaimiento RT60 propio) y la
// salida toma la forma del convolver (0.32*dry + 0.82*wetDifuso).
void FusedListenerRoom::diffuseFromDry(double* l, double* r, const double* dryL,
                                       const double* dryR, int n) noexcept {
    diffuse_.setRt60(controls_.rt60Sec);
    diffuse_.setDensity(effectiveWet_);
    diffuse_.process(dryL, dryR, diffuseOutL_.data(), diffuseOutR_.data(), n);
    for (int i = 0; i < n; ++i) {
        l[i] = 0.32 * dryL[i] + 0.82 * diffuseOutL_[static_cast<std::size_t>(i)];
        r[i] = 0.32 * dryR[i] + 0.82 * diffuseOutR_[static_cast<std::size_t>(i)];
    }
}

void FusedListenerRoom::process(double* left, double* right, int frames) noexcept {
    if (!left || !right || frames <= 0) return;
    if (isNeutral()) {
        // Bit-transparente, ganancia exactamente 1.0. Si el campo difuso
        // tenía estado (se apagó el wet), limpiarlo aquí (hilo de audio):
        // nunca libera energía almacenada como golpe al re-enganchar.
        if (diffuseActive_) {
            diffuse_.reset();
            diffuseActive_ = false;
        }
        return;
    }
    if (!prepared_) prepare(48000.0);  // una vez; aloca el scratch
    if (dryL_.size() < static_cast<std::size_t>(kFusedMaxBlockFrames) ||
        dryR_.size() < static_cast<std::size_t>(kFusedMaxBlockFrames) ||
        diffuseInL_.size() < static_cast<std::size_t>(kChunkFrames) ||
        diffuseInR_.size() < static_cast<std::size_t>(kChunkFrames) ||
        diffuseOutL_.size() < static_cast<std::size_t>(kChunkFrames) ||
        diffuseOutR_.size() < static_cast<std::size_t>(kChunkFrames)) {
        return;  // fail-closed: sin scratch no se procesa
    }

    // F2/486: proceso directo en double sobre los buffers del llamador
    // (sin conversión double<->float). El dry post-hearing se guarda
    // en dryL_/dryR_ para la mezcla RIR de potencia constante.
    int remaining = frames;
    int offset = 0;
    while (remaining > 0) {
        const int n = std::min(remaining, kFusedMaxBlockFrames);
        // 1. Perfil auditivo (FLAT = bypass: no se llama al motor).
        if (controls_.hearingProfile != 0) {
            hearing_.process(left + offset, right + offset,
                             static_cast<std::size_t>(n));
        }

        // Dry post-hearing para la mezcla RIR de potencia constante.
        std::memcpy(dryL_.data(), left + offset,
                    static_cast<std::size_t>(n) * sizeof(double));
        std::memcpy(dryR_.data(), right + offset,
                    static_cast<std::size_t>(n) * sizeof(double));

        // Bio 5D + RIR + corrección de sala, en trozos de 128 frames.
        for (int c = 0; c < n; c += kChunkFrames) {
            const int m = std::min(kChunkFrames, n - c);
            processChunk(left + offset + c, right + offset + c,
                         dryL_.data() + c, dryR_.data() + c, m);
        }

        offset += n;
        remaining -= n;
    }
    processed_ = true;
}

}  // namespace ivanna::fused
