// fused_transducer.cpp — OPERADOR #4: CORRECTOR DE TRANSDUCTOR.
// Fusión Magistral (2026-11-18). Ver fused_transducer.hpp para el mapeo
// completo de motores. Sin malloc, locks ni I/O en process().
#include "fused/fused_transducer.hpp"

namespace ivanna::fused {

FusedTransducer::FusedTransducer() {
    // Listo a 48 kHz desde la construcción (prepare() re-prepara a otra
    // frecuencia sin tocar los controles). La construcción del Volterra
    // (memalign de h1/delay lines) ocurre aquí, nunca en process().
    prepare(48000.0);
}

void FusedTransducer::prepare(double sampleRate) noexcept {
    const double sr = (std::isfinite(sampleRate) && sampleRate > 0.0)
                          ? sampleRate
                          : 48000.0;
    sampleRate_ = sr;
    const float fsr = static_cast<float>(sr);
    transient_.prepare(fsr);
    emf_.prepare(fsr, ivanna::spatial::TsParams{});
    temporal_.prepare(fsr);
    temporal_.setAuto(false);
    temporal_.setArc(0.0f, 0.0f);
    temporal_.setParams(false, 0.0f);
    diagonal_.prepare(fsr, kVolterraKernelLength);
    applyDesign();
    reset();
    prepared_ = true;
}

void FusedTransducer::setControls(const TransducerControls& c) noexcept {
    controls_ = c;
    controls_.clamp();
    // El diseño TransducerOperator no depende de los controles (medida
    // plana -> diseño plano a cualquier correction); no se recalcula aquí.
}

// FALLO 5 (Fusión Magistral, 2026-11-18): el .ivk1 medido se carga dentro
// de volterra_ delegando en el cargador existente (VolterraH2Symmetric);
// no hay cargador nuevo. Hilo de CONTROL, nunca el de audio.
ivanna::dsp::VolterraLoadStatus FusedTransducer::loadKernelPackage(
    const std::uint8_t* package, std::size_t packageSize,
    const ivanna::dsp::VolterraLoadPolicy& policy) noexcept {
    return volterra_.loadKernelPackage(package, packageSize, policy);
}

ivanna::dsp::VolterraLoadStatus FusedTransducer::replaceKernelPackage(
    const std::uint8_t* package, std::size_t packageSize,
    const ivanna::dsp::VolterraLoadPolicy& policy) noexcept {
    return volterra_.replaceKernelPackage(package, packageSize, policy);
}

void FusedTransducer::reset() noexcept {
    transient_.reset();
    emf_.reset();
    temporal_.reset();
    diagonal_.reset();
    // VolterraH2Symmetric no expone reset(): sus delay lines conservan el
    // historial a propósito (una calibración posterior entra sin arrancar
    // con memoria vacía); sin kernel efectivo es memcpy de todos modos.
    emfGovernor_ = 1.0f;
    transientHit_ = false;
}

void FusedTransducer::applyDesign() noexcept {
    // Sin respuesta medida del transductor no hay nada que compensar:
    // medida plana (0 dB) vs objetivo plano (0 dB) -> ganancias 0 dB.
    // El número de bandas es arbitrario con entrada plana (plano entra,
    // plano sale); 4 bandas, límite WNG +3 dB. Se calcula en prepare(),
    // nunca en process() (designTransducerCompensation aloca vectores).
    static constexpr int kBands = 4;
    static constexpr float kFlat[kBands] = {0.0f, 0.0f, 0.0f, 0.0f};
    designValid_ = ivanna::psi::designTransducerCompensation(
        kFlat, kFlat, kBands, 3.0f, design_);
    designMeanGainDb_ = 0.0f;
    if (designValid_) {
        double mean = 0.0;
        for (float g : design_.gainsDb) mean += g;
        if (!design_.gainsDb.empty())
            mean /= static_cast<double>(design_.gainsDb.size());
        designMeanGainDb_ = static_cast<float>(mean);
    }
}

void FusedTransducer::process(double* left, double* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0) return;
    // Neutro = bit-transparente: no se toca el buffer ni se avanza
    // ningún estado. Ganancia exactamente 1.0 (Matched-LUFS).
    if (isNeutral()) return;
    if (!prepared_) return;  // fail-closed; el ctor ya prepara a 48 kHz.

    temporal_.setParams(controls_.temporalAmount > 0.0f,
                        controls_.temporalAmount);

    // F2/486: proceso directo en double, en trozos de 128 frames
    // (== Ivanna::BLOCK_SIZE). Sin conversión double<->float.
    int done = 0;
    while (done < frames) {
        const int n = std::min(frames - done, kChunkFrames);
        processSubchunk(left + done, right + done, n);
        done += n;
    }
}

void FusedTransducer::processSubchunk(double* l, double* r, int n) noexcept {
    // 1. SENSOR transientes: mid (L+R)/2 por trozo.
    for (int i = 0; i < n; ++i) mid_[i] = 0.5 * (l[i] + r[i]);
    transientHit_ = transient_.processBlock(mid_, static_cast<std::size_t>(n));

    // 2. SENSOR Back-EMF (si emfEnabled): V = mid (proxy de tensión),
    //    I = V/Re (proxy óhmico). Sin fuente real de V/I, el estimador
    //    solo gobierna; sin sensor, gobernador = 1.0.
    emfGovernor_ = 1.0f;
    if (controls_.emfEnabled) {
        const double re = static_cast<double>(emf_.params().Re);  // 6.0 ohm por defecto (TsParams).
        for (int i = 0; i < n; ++i) emfCurrent_[i] = mid_[i] / re;
        emf_.process(mid_, emfCurrent_, n, emfVel_, emfDisp_);
        double velPower = 0.0;
        for (int i = 0; i < n; ++i) {
            const double v = emfVel_[i];
            velPower += v * v;
        }
        const double velRms = std::sqrt(velPower / static_cast<double>(n));
        // 1/(1+rms) en [0.5, 1.0]: suave, nunca silencia la corrección.
        const double gov = 1.0 / (1.0 + velRms);
        emfGovernor_ = static_cast<float>(std::clamp(gov, 0.5, 1.0));
    }

    // Corrección efectiva del trozo: slider * EMF * (transiente: mitad).
    double effCorrection = static_cast<double>(controls_.correction) *
                           static_cast<double>(emfGovernor_);
    if (transientHit_) effCorrection *= 0.5;

    // 3. VOLTERRA H2 (+ diagonal de memoria larga): dry/wet.
    //    Sin kernel efectivo ambos son bypass (memcpy / no-op); la mezcla
    //    con effCorrection es entonces identidad exacta.
    if (effCorrection > 0.0) {
        for (int i = 0; i < n; ++i) {
            dryL_[i] = l[i];
            dryR_[i] = r[i];
            interIn_[2 * i] = l[i];
            interIn_[2 * i + 1] = r[i];
        }
        volterra_.processInterleaved(interIn_, interOut_,
                                     static_cast<std::uint32_t>(n), 2);
        for (int i = 0; i < n; ++i) {
            wetL_[i] = interOut_[2 * i];
            wetR_[i] = interOut_[2 * i + 1];
        }
        diagonal_.process(wetL_, wetR_, n);
        for (int i = 0; i < n; ++i) {
            const double wl = std::isfinite(wetL_[i]) ? wetL_[i] : dryL_[i];
            const double wr = std::isfinite(wetR_[i]) ? wetR_[i] : dryR_[i];
            l[i] = dryL_[i] + effCorrection * (wl - dryL_[i]);
            r[i] = dryR_[i] + effCorrection * (wr - dryR_[i]);
        }
    }

    // 4. DINÁMICA ESPACIAL TEMPORAL 4D, al final. Con temporalAmount==0
    //    setParams(false, 0) la deja en bypass bit-exacto.
    temporal_.process(l, r, static_cast<std::size_t>(n));

    // Saneo final del trozo (fail-closed): lo no finito -> 0.0.
    for (int i = 0; i < n; ++i) {
        if (!std::isfinite(l[i])) l[i] = 0.0;
        if (!std::isfinite(r[i])) r[i] = 0.0;
    }
}

}  // namespace ivanna::fused
