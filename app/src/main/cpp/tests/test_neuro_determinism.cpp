// test_neuro_determinism.cpp — Diagnóstico + guarda de la no-determinismo
// de NEURO·Ω (hallazgo mayor de P0, 2026-11-22).
//
// HIPÓTESIS (verificada por lectura de código, ver informe):
// la única entrada asíncrona que afecta al audio en el camino NEURO es
// IvannaAudioClassifier (hilo de inferencia, sleep 5 ms, frames de 512
// muestras decimadas) -> AntiDolbyState (guardián) ->
// FusedChain::pushEffectiveControls (dynamics.width / tonal.highDb /
// spatial.width). En AUTO (intensidad del guardián = adaptation = 1.0)
// el guardián modula; en neutro (intensidad 0) no modula nada. Por eso
// SOLO op_neuro difiere entre corridas (medido: maxDiff 0.33) y el resto
// de operadores son bit-idénticos. Todo lo demás en la actuación NEURO
// es determinista: NPE (estado inicializado + reset), NHO (idem), pool
// LIF (idem), BiquadEnvelopeBank (idem; su phase_oracle_tick global solo
// alimenta el cue T, que el NPE no usa para audio),
// ClosedLoopController::compute(0) (contador interno de bloques,
// identidad sin frames de sensor), NeuroBandEq (bypass bit-exacto a 0 dB).
//
// TEST 1 (guarda): op_neuro con dos cadenas frescas debe ser
// BIT-IDÉNTICO (maxDiff 0.0). Antes del fix falla (~0.1-0.3).
// TEST 2 (diagnóstico, solo imprime): sonda directa clasificador +
// guardián: el chunk del primer output válido y la trayectoria del
// guardián varían entre corridas por el timing del hilo de inferencia.
// TEST 3 (línea base): IVANNA completa con dos cadenas frescas dentro
// de la tolerancia P0 (1e-5), como el test Deterministic de P0.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../IvannaAudioClassifier.hpp"
#include "../anti_dolby.h"

#include "ivanna_test_seed.hpp"

// C10: semilla global — imprime `seed=…` al inicio del log.
namespace { [[maybe_unused]] const bool kIvannaSeedLogged =
    [] { ivanna::test::PrintIvannaTestSeed(); return true; }(); }  // namespace

using namespace ivanna::fused;

namespace {

constexpr double kFs = 48000.0;
constexpr int kHop = 128;
constexpr size_t kN = 192000;  // 4 s @ 48 kHz
constexpr double kTwoPi = 6.28318530717958647692;

// xorshift64* determinista (misma familia que el ruido de P0).
struct XorShift {
    uint64_t s;
    explicit XorShift(uint64_t seed) : s(seed ? seed : 0x9e3779b97f4a7c15ULL) {}
    double nextUniform() noexcept {  // [-1, 1)
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        const uint64_t v = s * 0x2545F4914F6CDD1DULL;
        return static_cast<double>(v >> 11) * (1.0 / 9007199254740992.0) * 2.0 - 1.0;
    }
};

struct Signal {
    std::vector<double> l, r;
};

// Señal fija: seno 1 kHz @ -12 dBFS + ruido blanco RMS ~-20 dBFS.
Signal generateSignal() {
    Signal s;
    s.l.resize(kN);
    s.r.resize(kN);
    XorShift rng(ivanna::test::IvannaTestSeedFor(1));
    for (size_t i = 0; i < kN; ++i) {
        const double t = static_cast<double>(i) / kFs;
        const double sine = 0.251189 * std::sin(kTwoPi * 1000.0 * t);  // -12 dBFS
        const double noise = 0.05 * rng.nextUniform();
        s.l[i] = sine + noise;
        s.r[i] = 0.9 * sine + 0.8 * noise;
    }
    return s;
}

// Solo NEURO·Ω activo (idéntico a operatorOnlyControls(0) de la matriz
// P1): cerebro AUTO adaptation 1.0, resto neutro, safety limiter activo.
FusedControls opNeuroControls() {
    FusedControls c;
    c.setNeutral();
    c.brain.mode = 0;
    c.brain.adaptation = 1.0f;
    c.dynamics.safety = 1.0f;
    c.clamp();
    return c;
}

// IVANNA completa (idéntica a engagedControls() de la matriz P1).
FusedControls engagedControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.field.amount = 0.5f;
    c.field.omniAmount = 0.5f;  // Unificación 2.5.509 (paso 5): OmniField ya NO sigue a HOLOS —
    // su amount es propio; el helper fija el valor explícito para
    // preservar la configuración medida (antes lo ponía field.amount).
    c.field.phiAmount = 0.5f;
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 1.0f;
    c.tonal.curve = 1;
    c.tonal.lowDb = 2.0f;
    c.dynamics.compRatio = 2.0f;
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.safety = 1.0f;
    c.brain.mode = 0;
    c.brain.adaptation = 1.0f;
    c.clamp();
    return c;
}

Signal runChain(const Signal& ref, const FusedControls& controls) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(controls);
    std::vector<double> l = ref.l, r = ref.r;
    for (size_t off = 0; off < kN; off += static_cast<size_t>(kHop)) {
        chain.process(l.data() + off, r.data() + off, kHop);
    }
    Signal out;
    out.l = std::move(l);
    out.r = std::move(r);
    return out;
}

double maxAbsDiff(const Signal& a, const Signal& b, size_t* firstIdx = nullptr) {
    double d = 0.0;
    size_t idx = 0;
    for (size_t i = 0; i < kN; ++i) {
        const double dl = std::fabs(a.l[i] - b.l[i]);
        const double dr = std::fabs(a.r[i] - b.r[i]);
        if (dl > d) { d = dl; idx = i; }
        if (dr > d) { d = dr; idx = i; }
    }
    if (firstIdx) *firstIdx = idx;
    return d;
}

// Sonda directa del mecanismo: clasificador + guardián alimentados con la
// misma señal en trozos de 128, como hace FusedBrain::runAnalysisChunk.
// Devuelve el chunk del primer output válido y la trayectoria del guardián.
struct GuardianTrace {
    int firstValidChunk = -1;
    // widener / spread / eqBoost en los chunks de muestreo.
    float wid[6] = {};
    float spr[6] = {};
    float eq[6] = {};
};

GuardianTrace probeClassifierGuardian(const Signal& ref) {
    Ivanna::IvannaAudioClassifier classifier;
    AntiDolbyState guardian;
    guardian.reset();
    guardian.setAntiDolbyIntensity(1.0f);  // AUTO, adaptation 1.0
    GuardianTrace tr;
    const int kSampleChunks[6] = {32, 64, 128, 256, 512, 1024};
    std::vector<float> fl(kHop), fr(kHop);
    int chunk = 0;
    for (size_t off = 0; off < kN; off += static_cast<size_t>(kHop), ++chunk) {
        for (int i = 0; i < kHop; ++i) {
            fl[static_cast<size_t>(i)] = static_cast<float>(ref.l[off + static_cast<size_t>(i)]);
            fr[static_cast<size_t>(i)] = static_cast<float>(ref.r[off + static_cast<size_t>(i)]);
        }
        classifier.ingestAudioFrame(fl.data(), fr.data(), static_cast<size_t>(kHop));
        Ivanna::AIModelOutput out{};
        if (classifier.getModelOutput(out) && out.is_valid) {
            if (tr.firstValidChunk < 0) tr.firstValidChunk = chunk;
            guardian.updateFromNeuralContext(
                static_cast<uint8_t>(out.dominant_class), out.confidence,
                out.scene_energy);
        }
        guardian.tick(static_cast<float>(kHop) / static_cast<float>(kFs));
        for (int s = 0; s < 6; ++s) {
            if (chunk == kSampleChunks[s]) {
                tr.wid[s] = guardian.currentWidener();
                tr.spr[s] = guardian.currentSpreadMultiplier();
                tr.eq[s] = guardian.currentEqBoost();
            }
        }
    }
    return tr;
}

void printTrace(const char* tag, const GuardianTrace& tr) {
    std::printf("[NEURO-DET] %s primer chunk valido=%d | widener@32/64/128/256/512/1024 = "
                "%.4f/%.4f/%.4f/%.4f/%.4f/%.4f | spread = %.4f/%.4f/%.4f/%.4f/%.4f/%.4f | "
                "eqBoost = %.3f/%.3f/%.3f/%.3f/%.3f/%.3f\n",
                tag, tr.firstValidChunk, tr.wid[0], tr.wid[1], tr.wid[2], tr.wid[3],
                tr.wid[4], tr.wid[5], tr.spr[0], tr.spr[1], tr.spr[2], tr.spr[3],
                tr.spr[4], tr.spr[5], tr.eq[0], tr.eq[1], tr.eq[2], tr.eq[3],
                tr.eq[4], tr.eq[5]);
}

}  // namespace

TEST(NeuroDeterminism, OpNeuroTwoFreshChainsBitIdentical) {
    const Signal ref = generateSignal();
    const Signal out1 = runChain(ref, opNeuroControls());
    const Signal out2 = runChain(ref, opNeuroControls());
    size_t idx = 0;
    const double d = maxAbsDiff(out1, out2, &idx);
    std::printf("[NEURO-DET] op_neuro maxDiff entre 2 cadenas frescas = %.3e "
                "(primera divergencia muestra %zu, %.1f ms)\n",
                d, idx, 1000.0 * static_cast<double>(idx) / kFs);
    EXPECT_EQ(d, 0.0) << "op_neuro no es bit-determinista entre corridas";
}

TEST(NeuroDeterminism, ClassifierGuardianTiming) {
    // Diagnóstico (no impone igualdad): documenta que el chunk del primer
    // output válido del clasificador y la trayectoria del guardián varían
    // entre corridas por el timing del hilo de inferencia.
    const Signal ref = generateSignal();
    const GuardianTrace t1 = probeClassifierGuardian(ref);
    const GuardianTrace t2 = probeClassifierGuardian(ref);
    printTrace("corrida1", t1);
    printTrace("corrida2", t2);
    if (t1.firstValidChunk != t2.firstValidChunk) {
        std::printf("[NEURO-DET] MECANISMO CONFIRMADO: el primer chunk valido "
                    "difiere (%d vs %d): el guardián arranca en momentos "
                    "distintos por el timing del hilo de inferencia\n",
                    t1.firstValidChunk, t2.firstValidChunk);
    } else {
        std::printf("[NEURO-DET] primer chunk valido igual (%d) en esta "
                    "corrida: el timing coincidio (intermitente)\n",
                    t1.firstValidChunk);
    }
}

TEST(NeuroDeterminism, IvannaTwoFreshChainsWithinTolerance) {
    const Signal ref = generateSignal();
    const Signal out1 = runChain(ref, engagedControls());
    const Signal out2 = runChain(ref, engagedControls());
    size_t idx = 0;
    const double d = maxAbsDiff(out1, out2, &idx);
    std::printf("[NEURO-DET] IVANNA maxDiff entre 2 cadenas frescas = %.3e "
                "(primera divergencia muestra %zu)\n",
                d, idx);
    EXPECT_LE(d, 1e-5) << "IVANNA completa fuera de la tolerancia P0 (1e-5)";
}
