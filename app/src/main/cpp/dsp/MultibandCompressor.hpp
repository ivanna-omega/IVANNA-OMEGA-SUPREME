#pragma once
#include "../include/Compressor.h"
#include "../include/dsp_types.h"
#include <cstdint>
#include <vector>

namespace ivanna {

// Compresor multibanda 3 bandas (0027, elevación omnipotente del DSP).
//
// El compresor de Ruta A (g_comp) es MONOBANDA: un solo detector de pico
// y un solo threshold/ratio para TODO el espectro. Un golpe de bajo
// dispara la compresión de todo el espectro (las voces y los agudos se
// hunden con cada bombo = pumping); un sibilante agudo no se controla
// sin aplastar los graves. Todos los referentes mundiales (SHARC, Lake,
// GALAXY, Trinnov, Helix, HDX, UAD) tienen dinámica multibanda.
//
// Diseño (exigido por el revisor del consejo, 0027):
// - 3 bandas con crossovers Linkwitz-Riley de 4º orden (2x Butterworth
//   2º orden Q=0.7071 en serie por rama): graves <200 Hz, medios
//   200 Hz–4 kHz, agudos >4 kHz. La suma LR4 es plana en magnitud
//   (all-pass): sin compresión, la reconstrucción es plana ±0.5 dB.
// - Un Compressor (el existente, con detector link estéreo max(L,R),
//   soft knee y makeup acoplado) por banda. Cada banda comprime
//   independiente: el grave pega sin hundir la voz.
// - Misma interfaz de runtime que Compressor (setRuntimeAmount) para
//   que el motor adaptativo no cambie: el amount se reenvía a las 3
//   bandas. setParams/setThreshold/setRatio/setAttack/setRelease también
//   se reenvían (mismos valores en las 3 bandas por defecto).
// - Bypass bit-exacto: con enabled=false el proceso retorna sin tocar
//   el buffer (el monobanda queda como fallback real). El split de
//   crossovers corre SIEMPRE (aunque esté en bypass) para mantener los
//   filtros cebados; al activar/desactivar hay un fundido de 20 ms
//   (sin clic) entre seco y procesado.
// - Sin asignaciones en el hot path: scratch por banda como miembros
//   (3 bandas x 2 canales x 2048 frames). RT-safe.
//
// F2/486 (2026-11-20) — MOTOR DOUBLE: estado y matemática en double 64-bit.
// process(double*, double*, int) es el núcleo; process(float*, float*, int)
// es un envoltorio de compat.
class MultibandCompressor {
public:
    static constexpr int kNumBands = 3;
    static constexpr int kMaxFrames = 2048;
    static constexpr double kCrossoverLowHz = 200.0;    // graves / medios
    static constexpr double kCrossoverHighHz = 4000.0;  // medios / agudos

    MultibandCompressor();

    void setParams(const DSPParams& p);
    void setThreshold(float db);
    void setRatio(float ratio);
    void setAttack(float ms);
    void setRelease(float ms);
    // Misma interfaz que Compressor: el motor adaptativo fija el objetivo
    // por bloque; se reenvía a las 3 bandas.
    void setRuntimeAmount(float amount01) noexcept;
    // false = bypass bit-exacto (el monobanda es el fallback).
    void setEnabled(bool on) noexcept;
    bool isEnabled() const noexcept { return enabled_; }
    void setCrossovers(float lowHz, float highHz);
    void process(double* left, double* right, int frames);
    void process(float* left, float* right, int frames);
    void reset();

private:
    void updateCrossovers() noexcept;

    double sr_ = 48000.0;
    double lowHz_ = kCrossoverLowHz;
    double highHz_ = kCrossoverHighHz;
    bool crossoversDirty_ = true;

    // Crossover Linkwitz-Riley 4º orden = 2x Butterworth 2º orden en
    // serie (Q=0.7071). Por canal: low = LP,LP; high = HP,HP;
    // mid = HP,HP,LP,LP.
    struct Crossover {
        Biquad lp1a, lp1b;  // lowpass @ lowHz (x2 en serie)
        Biquad hp1a, hp1b;  // highpass @ lowHz (x2 en serie)
        Biquad lp2a, lp2b;  // lowpass @ highHz (x2 en serie)
        Biquad hp2a, hp2b;  // highpass @ highHz (x2 en serie)
    };
    Crossover xL_, xR_;
    Compressor band_[kNumBands];

    bool enabled_ = true;
    double mix_ = 1.0;        // fundido 20 ms al activar/desactivar
    double mixTarget_ = 1.0;

    // Scratch por banda (miembro, no stack: 3 x 2 x 2048 doubles).
    double bandL_[kNumBands][kMaxFrames];
    double bandR_[kNumBands][kMaxFrames];

    // Scratch del envoltorio float (crece bajo demanda).
    std::vector<double> wrapL_, wrapR_;
};

}  // namespace ivanna
