// fused_tonal.cpp — OPERADOR #5: BALANCE TONAL ÚNICO "ARMONÍA".
// Ver fused_tonal.hpp para el orden de la cadena y las decisiones.
//
// Motores envueltos (verificados en host 2026-11-18: los 5 enlazan y
// corren; Psychoacoustics.cpp y EvolutionaryEQ.cpp compilan standalone
// con solo sus headers, sin IvannaFusionCore.cpp):
//   1. Ivanna::Psychoacoustics (Psychoacoustics.cpp) — primero, solo si
//      loudnessComp. Expansión máx +1.0 dB en micro-detalle y LPF de
//      fatiga con alpha en [0.85, 1.0]: sobre tonos fuertes es ≈ identidad.
//   2. Ivanna::EvolutionaryEQ (EvolutionaryEQ.cpp) — solo si curve != 0
//      y quedó calibrado (prepare() calibra; sin calibrar el propio
//      motor es identidad). Objetivo fijo Harman 2018 ×0.5.
//   3. ivanna::spatial::EliteTargetCurves (header-only) — anclajes Harman
//      2018 y élite-lean; el delta élite−Harman ×0.5 se pliega en la
//      etapa 4 (el EvoEQ no expone setter de objetivo).
//   4. ivanna::dsp::RoomCorrectionEq (header-only: AutoEqFilter +
//      RoomProbe, sin .cpp) — EQ de usuario de 10 bandas, instancia
//      propia, modo completo (±12 dB). El auto_eq_filter vive dentro de
//      RoomCorrectionEq y NO se envuelve por separado.
//   5. ivanna::LoudnessMeter (perceptual_loudness.hpp, header-only) —
//      medición K-weighted por trozo + trim estático trimDb al final
//      (única ganancia neta; ver nota del auto-trim en el header).

#include "fused/fused_tonal.hpp"

#include <algorithm>
#include <cstring>

namespace ivanna::fused {

void FusedTonal::prepare(double sampleRate) noexcept {
    sampleRate_ = (std::isfinite(sampleRate) && sampleRate > 0.0)
                      ? sampleRate
                      : 48000.0;
    const float sr = static_cast<float>(sampleRate_);
    roomEq_.prepare(sr);
    meter_.init(sampleRate_);
    meter_.reset();
    psycho_ = Ivanna::Psychoacoustics{};
    // Calibración del EvoEQ: determinista e idempotente (resiembre 1337,
    // genoma neutral), ~10 ms a -O2; sin ella processNEON es identidad.
    // No asigna (todo en miembros fijos), así que es segura en noexcept.
    evoEq_.calibrate(sr);
    evoEq_.resetHistory();
    carryIn_ = 0;   // FALLO 1: el resto acarreado es de la tasa anterior.
    carryOut_ = 0;  // (las muestras pendientes se descartan, ≤127).
    applyControls();
}

void FusedTonal::setControls(const TonalControls& c) noexcept {
    controls_ = c;
    controls_.clamp();
    applyControls();
}

void FusedTonal::applyControls() noexcept {
    trimGain_ = std::pow(10.0, static_cast<double>(controls_.trimDb) / 20.0);
    // EQ de usuario, 10 bandas RoomProbe (31.5 Hz … 16 kHz):
    //   bandas 0-2 = lowDb, 3-6 = midDb, 7-9 = highDb, y S3/510:
    //   presencia 4-8 kHz (bandas 7-8: 4 kHz y 8 kHz) ADITIVA sobre
    //   highDb. La banda 9 (16 kHz) queda solo con highDb.
    // curve 2 (élite-lean): el EvoEQ sigue apuntando a Harman ×0.5, así
    // que el delta (élite − Harman) ×0.5 se suma aquí; el total queda
    // élite-lean ×0.5, a la misma escala que Harman ×0.5 en curve 1.
    // curve 0: sin delta (plana); curve 1: sin delta (Harman la da el EvoEQ).
    float gains[ivanna::room::kBands]{};
    for (int b = 0; b < ivanna::room::kBands; ++b) {
        float g = (b <= 2) ? controls_.lowDb
                           : (b <= 6) ? controls_.midDb : controls_.highDb;
        // S3/510 — presenceDb ADITIVO (no sustitutivo): las bandas
        // 7-8 (4 kHz y 8 kHz) suman presenceDb ENCIMA de highDb. Con
        // presenceDb = 0 las ganancias son idénticas a antes
        // (bit-exacto para perfiles existentes); la guarda != 0.0f lo
        // hace exacto incluso ante -0.0f.
        if (controls_.presenceDb != 0.0f && (b == 7 || b == 8))
            g += controls_.presenceDb;
        if (controls_.curve == 2) {
            const float f = ivanna::room::kBandHz[b];
            const float harman =
                ivanna::spatial::eliteTargetDbAt(
                    ivanna::spatial::EliteTargetKind::Harman2018, f);
            const float elite =
                ivanna::spatial::eliteTargetDbAt(
                    ivanna::spatial::EliteTargetKind::HarmanEliteLean, f);
            g += (elite - harman) * kCurveTargetScale;
        }
        // P8 — CORONA D10: el escalar tonal continuo escala las 10
        // ganancias de banda. amount=1.0 → idéntico a antes; amount=0 →
        // ganancias 0 (bypass). El trimGain_ (trimDb) NO se escala: es
        // el trim maestro / compensador P8.
        gains[b] = g * controls_.amount;
    }
    roomEq_.setTarget(gains);
}

void FusedTonal::reset() noexcept {
    psycho_ = Ivanna::Psychoacoustics{};
    evoEq_.resetHistory();
    roomEq_.prepare(static_cast<float>(sampleRate_));
    meter_.reset();
    carryIn_ = 0;   // FALLO 1: el reset vacía el carry (sin muestras
    carryOut_ = 0;  // fantasma de un flujo anterior en el estado DSP).
    applyControls();  // RoomCorrectionEq::prepare reseteó los objetivos.
}

// Un trozo exacto de 128 frames REALES (workL_/workR_) por psico/evo → EQ de
// usuario → medidor → trim, volcando el resultado al carry de salida.
// En neutro es un mero acarreo (copia work_ → carry de salida): el
// retardo constante del carry se conserva al conmutar a neutro, sin
// perder ni duplicar muestras.
// F2/486 — MOTOR DOUBLE: todo el trozo corre en double 64-bit (los motores
// usan sus núcleos double; el puente float↔double desaparece de aquí).
void FusedTonal::processBlock128() noexcept {
    const bool neutral = isNeutral();
    const bool usePsycho = !neutral && controls_.loudnessComp;
    const bool useEvo = !neutral && controls_.curve != 0 && evoEq_.isReady();
    if (usePsycho) {
        psycho_.predictAndMitigateFatigue(workL_, workR_, kProcessChunkFrames);
        psycho_.applyMaskingCompensation(workL_, workR_, kProcessChunkFrames);
    }
    // P8 — CORONA D10: el EvoEQ apunta a Harman ×0.5 con objetivo FIJO
    // (sin setter), así que amount lo escala por mezcla wet/dry:
    // work = dry + amount·(evo − dry). amount=1.0 corre el EvoEQ
    // directo (bit-idéntico al comportamiento actual: la mezcla
    // dry + 1·(evo − dry) NO es bit-idéntica en coma flotante);
    // 0 < amount < 1 mezcla; amount=0 es inalcanzable aquí (neutral →
    // useEvo false) y queda como guarda fail-closed (se salta el EvoEQ).
    if (useEvo) {
        const float amt = controls_.amount;
        if (amt >= 1.0f) {
            evoEq_.processPlanar(workL_, workR_, kProcessChunkFrames);
        } else if (amt > 0.0f) {
            for (int j = 0; j < kProcessChunkFrames; ++j) {
                evoDryL_[j] = workL_[j];
                evoDryR_[j] = workR_[j];
            }
            evoEq_.processPlanar(workL_, workR_, kProcessChunkFrames);
            const double a = static_cast<double>(amt);
            for (int j = 0; j < kProcessChunkFrames; ++j) {
                workL_[j] = evoDryL_[j] + a * (workL_[j] - evoDryL_[j]);
                workR_[j] = evoDryR_[j] + a * (workR_[j] - evoDryR_[j]);
            }
        }
        // amt <= 0.0f: sin EvoEQ (guarda; en neutro useEvo ya es false).
    }
    if (!neutral) {
        // EQ de usuario (bypass bit-exacto si los 10 objetivos son 0)
        // + medición K-weighted del trozo + trim estático al final.
        roomEq_.process(workL_, workR_, kProcessChunkFrames);
        meter_.measure_block(workL_, workR_, kProcessChunkFrames);
        const double trim = trimGain_;
        if (trim != 1.0) {
            for (int j = 0; j < kProcessChunkFrames; ++j) {
                workL_[j] *= trim;
                workR_[j] *= trim;
            }
        }
    }
    for (int j = 0; j < kProcessChunkFrames; ++j) {
        carryOutL_[carryOut_] = workL_[j];
        carryOutR_[carryOut_] = workR_[j];
        ++carryOut_;
    }
}

void FusedTonal::process(double* left, double* right, int frames) noexcept {
    if (left == nullptr || right == nullptr || frames <= 0) return;
    // Vía rápida bit-transparente: sin resto pendiente no hay nada que
    // emitir ni acarrear. (isNeutral() excluye loudnessComp por contrato;
    // en neutro el psicoacústico NO corre, como antes.)
    if (isNeutral() && carryIn_ == 0 && carryOut_ == 0) return;

    // FALLO 1 (2026-11-18, HAL 192 frames — CRÍTICO): los motores
    // de 128 fijos (Psychoacoustics / EvolutionaryEQ) exigen 128 frames
    // exactos. El código anterior rellenaba la cola de un trozo parcial
    // con CEROS y solo devolvía n muestras ("el estado avanza 128
    // frames, como un breve silencio — inaudible y finito"):
    // FALSO en régimen periódico. Con bloques HAL de 192 (128+64) el
    // segundo sub-trozo metía 64 ceros REALES en el historial FIR del
    // EvoEQ y en las envolventes IIR del psicoacústico en CADA bloque →
    // ruido/modulación periódica a 48000/192 = 250 Hz.
    //
    // Corrección: carry-over de 128 exactos. La entrada se ve como un
    // flujo virtual [resto pendiente (carryIn_) + trozo actual]: solo
    // trozos COMPLETOS de 128 frames reales llegan a processBlock128();
    // la cola (< 128) se acarrea como entrada real a la próxima llamada
    // y la salida procesada se acarrea igual (carryOut_). El flujo sale
    // con retardo constante ≤127 frames y un cebado inicial de ceros
    // SOLO a la salida (posiciones sin muestra procesada aún, típico
    // del primer bloque no múltiplo), jamás al estado DSP. Con entrada
    // ya alineada a 128 el carry es cero y el retardo es 0: el camino
    // de 48000 frames en un solo bloque es bit-idéntico al anterior.
    int done = 0;
    while (done < frames) {
        const int outer = std::min(kFusedMaxBlockFrames, frames - done);
        // F2/486 — MOTOR DOUBLE: se sanea la entrada (un NaN aquí
        // envenenaría el estado persistente de los motores); ya no hay
        // bajada a float.
        for (int i = 0; i < outer; ++i) {
            const double l = left[done + i];
            const double r = right[done + i];
            scratchL_[i] = std::isfinite(l) ? l : 0.0;
            scratchR_[i] = std::isfinite(r) ? r : 0.0;
        }
        // Flujo virtual: [carryIn_ pendientes][scratch outer]. Solo
        // trozos completos de 128 frames reales se procesan.
        const int total = carryIn_ + outer;
        const int nBlocks = total / kProcessChunkFrames;
        for (int b = 0; b < nBlocks; ++b) {
            const int base = b * kProcessChunkFrames;
            for (int j = 0; j < kProcessChunkFrames; ++j) {
                const int p = base + j;
                if (p < carryIn_) {
                    workL_[j] = carryInL_[p];
                    workR_[j] = carryInR_[p];
                } else {
                    workL_[j] = scratchL_[p - carryIn_];
                    workR_[j] = scratchR_[p - carryIn_];
                }
            }
            processBlock128();
        }
        // Cola real (< 128) al carry de entrada — JAMÁS ceros.
        const int consumed = nBlocks * kProcessChunkFrames;
        const int tail = total - consumed;
        for (int j = 0; j < tail; ++j) {
            const int p = consumed + j;
            if (p < carryIn_) {
                carryInL_[j] = carryInL_[p];
                carryInR_[j] = carryInR_[p];
            } else {
                carryInL_[j] = scratchL_[p - carryIn_];
                carryInR_[j] = scratchR_[p - carryIn_];
            }
        }
        carryIn_ = tail;
        // Emitir las posiciones de ESTE trozo desde el carry de salida
        // (saneado final fail-closed + techo ±1.5).
        // El shortfall (take < outer) solo ocurre en el cebado inicial:
        // esas posiciones salen a cero y sus muestras reales ya están
        // en el carry, procesadas en la próxima llamada.
        const int take = std::min(carryOut_, outer);
        for (int j = 0; j < take; ++j) {
            const double l = carryOutL_[j];
            const double r = carryOutR_[j];
            double ol = std::isfinite(l) ? l : 0.0;
            double orr = std::isfinite(r) ? r : 0.0;
            ol = std::clamp(ol, -kOutputCeiling, kOutputCeiling);
            orr = std::clamp(orr, -kOutputCeiling, kOutputCeiling);
            left[done + j] = ol;
            right[done + j] = orr;
        }
        if (take > 0) {
            const int remain = carryOut_ - take;
            if (remain > 0) {
                std::memmove(carryOutL_, carryOutL_ + take,
                             sizeof(double) * static_cast<std::size_t>(remain));
                std::memmove(carryOutR_, carryOutR_ + take,
                             sizeof(double) * static_cast<std::size_t>(remain));
            }
            carryOut_ = remain;
        }
        for (int j = take; j < outer; ++j) {
            left[done + j] = 0.0;
            right[done + j] = 0.0;
        }
        done += outer;
    }
}

}  // namespace ivanna::fused
