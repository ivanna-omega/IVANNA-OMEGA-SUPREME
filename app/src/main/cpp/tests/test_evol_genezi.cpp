// test_evol_genezi.cpp — MEJORA EVOLUTIVA, bloque GENEZI + NOTCH + 11.5 kHz
// (prioridades 4 y 8, 2026-11-26).
//
// Todo número de este archivo se MIDE en la FusedChain real o en el
// FusedTransducer real; nada se afirma por razonamiento.
//
// 1) GENEZI: los assets kernels/genezy_mhc_pz1d.ivk1 y
//    kernels/moto_g85_pz1d.ivk1 existen y son paquetes IVK1 distintos
//    (dataset GENEZY01, K=64, fs 48 kHz, CRC válido). La ruta Kotlin SÍ
//    tiene call-sites (PersistedStateRestorer.restore() en el arranque +
//    selector de UI + modo SPEAKERS_GENEZI), pero lo que nunca se había
//    probado es que el paquete cargue en el motor fusionado de Ruta A y
//    que suene distinto del PZ1D y del bypass. Este archivo lo prueba:
//    carga real del asset en FusedTransducer/FusedChain, diferencia
//    medible de salida (H2 + RMS) y reemplazo en caliente sin clic
//    (fundido ~18 ms del envelope Volterra).
//    La otra vía Genezi (tri21 de NewPhysicsEngine, loadGeneziVolterra)
//    NO tiene datos medidos en ningún lugar (no existe genezi_np_cal.json
//    ni arrays tri21) y no tiene llamador Kotlin: no se puede cablear sin
//    un barrido Farina real del MHC-PZ1D. Ver el test
//    GeneziTri21PathHasNoMeasuredData.
//
// 2) NOTCH DE ENGANCHE −15.8 dB a 977 Hz: la ablación P1+P4 midió
//    −15.846 dB (ARMONÍA plana, base P0) / −14.938 dB (P0-engaged) al
//    enganchar la cadena. Aquí se reproduce y se atribuye apagando
//    APEX, TΩ/volumétrico, D10 y safety uno a uno, además del LOO de
//    NEURO. Veredicto medido: ver EngageNotchAttribution977.
//
// 3) INTERACCIÓN −25 dB a 11.5 kHz: la ablación midió interacción
//    −25 dB en esa banda (marginal ORBE +21.718 dB). Aquí se reproduce
//    con solos y pares de operadores, y se prueba la dependencia de
//    nivel (techo final / safety limiter) bajando la entrada.
//    Veredicto medido: ver Interaction11500Attribution.
//
// MÉTODO: tono dual-mono, 2 s @ 48 kHz, HEADPHONES salvo indicación;
// ventana de medida 2º segundo (1.0–2.0 s), RMS por canal; ganancia
// neta = RMS-salida − RMS-entrada. ALL7 = defaults + 7 operadores a 1.0
// + trimDb = 0 (idéntico a la sonda #1432 y a test_p1p4_ablation.cpp).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_dynamics.hpp"
#include "../fused/fused_measure.hpp"
#include "../fused/fused_transducer.hpp"
#include "../fused/tenth_dimension_action.hpp"
#include "../neuromorphic/volterra_kernel_package.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {

constexpr double kFs = 48000.0;
constexpr double kAmp = 0.7071;  // −3 dBFS
constexpr std::size_t kN = 96000;     // 2 s
constexpr std::size_t kSkip = 48000;  // 1 s de transitorio/convergencia
const double kInRmsDb = 20.0 * std::log10(0.7071 / std::sqrt(2.0));

const char* kOpName[7] = {"ORBE", "HOLOS", "SALA", "VOLTERRA",
                          "ARMONIA", "PULSO", "NEURO"};

// Base neutra VERDADERA: FusedControls::setNeutral() (C++) pone a cero
// TODOS los campos que sacan del neutro — amount, hrtfWet, phiAmount,
// safety, curva, roomWet, etc. Un zero7() a mano que solo tocaba los 7
// amounts dejaba vivos hrtfWet=1.0 (etapa HRTF de reserva), phiAmount=0.5
// (Φ) y safety=1.0 (limiter): los "solos" medían la HRTF, no el operador.
// Ver Interaction11500HrtfGate: con hrtfWet=1.0 y amount=0, la cadena
// pierde L −12.935 / R −43.715 dB a 11.5 kHz (notch de pinna sintético).
void zero7(FusedControls& c) {
    c.setNeutral();
}

void setOp(FusedControls& c, std::size_t op, float v) {
    switch (op) {
        case 0: c.spatial.amount = v; break;
        case 1: c.field.amount = v; break;
        case 2: c.listenerRoom.roomWet = v; break;
        case 3: c.transducer.correction = v; break;
        case 4: c.tonal.amount = v; break;
        case 5: c.dynamics.punch = v; break;
        case 6:
            c.brain.adaptation = v;
            // setNeutral() deja mode=1 (MANUAL, adaptación forzada a 0 en
            // clamp): el solo NEURO exige AUTO para que el NPE actúe.
            if (v > 0.0f) c.brain.mode = 0;
            break;
        default: break;
    }
}

FusedControls all7Controls() {
    FusedControls c;
    for (std::size_t o = 0; o < 7; ++o) setOp(c, o, 1.0f);
    c.tonal.trimDb = 0.0f;
    c.clamp();
    return c;
}

// Configuración ENGAGED de P0 (idéntica a test_p1p4_ablation.cpp).
FusedControls p0EngagedControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.field.amount = 0.5f;
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

struct ToneResult {
    double netL = 0.0, netR = 0.0;
    double peakL = 0.0, peakR = 0.0;
    double winL[8] = {}, winR[8] = {};  // RMS dBFS por ventana de 0.25 s
};

// Toggles de cadena que el LOO de 7 operadores NO cubre: APEX, TΩ
// (volumétrico), D10 y safety. Por defecto: como la ablación (APEX on,
// volumétrico on, D10 on, safety = default).
struct ChainToggles {
    bool apex = true;
    bool volumetric = true;
    bool d10 = true;
    bool safetyOff = false;  // true = dynamics.safety = 0
    bool speakers = false;   // true = spatial.mode SPEAKERS (TΩ activa)
};

ToneResult runTone(const FusedControls& cIn, double freqHz,
                   double amp = kAmp,
                   const ChainToggles& tg = ChainToggles{}) {
    FusedControls c = cIn;
    if (tg.safetyOff) c.dynamics.safety = 0.0f;
    if (tg.speakers) c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    FusedChain chain;
    chain.prepare(kFs);
    if (!tg.d10) {
        // Como test_d10_corona.cpp: antes de setControls (prepare ya
        // dejó el default enabled=true; setControls no toca el D10).
        ivanna::fused::d10::D10Config cfg;
        cfg.enabled = false;
        chain.d10Engine().setConfig(cfg);
    }
    chain.setControls(c);
    chain.setApexEnabled(tg.apex);
    chain.setVolumetricEnabled(tg.volumetric);
    std::vector<double> l(kN), r(kN);
    fillToneDouble(l.data(), kN, kFs, freqHz, amp);
    fillToneDouble(r.data(), kN, kFs, freqHz, amp);
    chain.process(l.data(), r.data(), static_cast<int>(kN));
    ToneResult res;
    res.netL = rmsDb(l.data() + kSkip, kN - kSkip) - (20.0 * std::log10(amp / std::sqrt(2.0)));
    res.netR = rmsDb(r.data() + kSkip, kN - kSkip) - (20.0 * std::log10(amp / std::sqrt(2.0)));
    res.peakL = peakAbs(l.data(), kN);
    res.peakR = peakAbs(r.data(), kN);
    for (int w = 0; w < 8; ++w) {
        const std::size_t off = static_cast<std::size_t>(w) * 12000;
        res.winL[w] = rmsDb(l.data() + off, 12000);
        res.winR[w] = rmsDb(r.data() + off, 12000);
    }
    return res;
}

double meanLr(double a, double b) { return 0.5 * (a + b); }

// Proyección LS de la fundamental/armónico (definida más abajo; se
// declara aquí porque soloFreq la usa).
void projectFund(const double* x, std::size_t n, double freqHz, double& amp,
                 double& phase);

// Hueco transitorio de enganche: peor ventana RMS de 5 ms en los
// primeros 100 ms menos la ventana RMS de 5 ms en régimen (1.0 s).
// Negativo = la salida cae bajo su nivel de régimen al enganchar
// (mezcla seco/procesado durante las rampas de wet de 30 ms).
double engageDipDb(const FusedControls& cIn, double freqHz, double amp = kAmp,
                   const ChainToggles& tg = ChainToggles{}) {
    FusedControls c = cIn;
    if (tg.safetyOff) c.dynamics.safety = 0.0f;
    if (tg.speakers) c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    FusedChain chain;
    chain.prepare(kFs);
    if (!tg.d10) {
        ivanna::fused::d10::D10Config cfg;
        cfg.enabled = false;
        chain.d10Engine().setConfig(cfg);
    }
    chain.setControls(c);
    chain.setApexEnabled(tg.apex);
    chain.setVolumetricEnabled(tg.volumetric);
    std::vector<double> l(kN), r(kN);
    fillToneDouble(l.data(), kN, kFs, freqHz, amp);
    fillToneDouble(r.data(), kN, kFs, freqHz, amp);
    chain.process(l.data(), r.data(), static_cast<int>(kN));
    double minWin = 1e9;
    for (std::size_t off = 0; off + 240 <= 4800; off += 240) {
        minWin = std::min(minWin, rmsDb(l.data() + off, 240));
    }
    const double steadyWin = rmsDb(l.data() + kSkip, 240);
    return minWin - steadyWin;
}

// Respuesta en frecuencia de un operador solo (net por banda) + amplitud
// de la fundamental proyectada en la salida (para distinguir atenuación
// de destrucción/desplazamiento del tono).
struct SoloFreq {
    double netDb = 0.0;
    double fundAmp = 0.0;  // amplitud de la fundamental en la salida
    double fundPhaseDeg = 0.0;
};

SoloFreq soloFreq(std::size_t op, double freqHz, double amp = kAmp) {
    FusedControls c;
    zero7(c);
    setOp(c, op, 1.0f);
    c.clamp();
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(c);
    chain.setVolumetricEnabled(true);
    std::vector<double> l(kN), r(kN);
    fillToneDouble(l.data(), kN, kFs, freqHz, amp);
    fillToneDouble(r.data(), kN, kFs, freqHz, amp);
    chain.process(l.data(), r.data(), static_cast<int>(kN));
    SoloFreq res;
    res.netDb = rmsDb(l.data() + kSkip, kN - kSkip) -
                (20.0 * std::log10(amp / std::sqrt(2.0)));
    double ph = 0.0;
    projectFund(l.data() + kSkip, kN - kSkip, freqHz, res.fundAmp, ph);
    res.fundPhaseDeg = ph * 180.0 / 3.14159265358979323846;
    return res;
}

// Proyección LS de la fundamental/armónico (amplitud + fase).
void projectFund(const double* x, std::size_t n, double freqHz, double& amp,
                 double& phase) {
    constexpr double kTwoPi = 6.28318530717958647692;
    const double step = kTwoPi * freqHz / kFs;
    double s = 0.0, c = 0.0, ph = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double v = x[i];
        s += v * std::sin(ph);
        c += v * std::cos(ph);
        ph += step;
        if (ph >= kTwoPi) ph -= kTwoPi;
    }
    const double aSin = 2.0 * s / static_cast<double>(n);
    const double bCos = 2.0 * c / static_cast<double>(n);
    amp = std::sqrt(aSin * aSin + bCos * bCos);
    phase = std::atan2(bCos, aSin);
}

// ── Assets .ivk1 ────────────────────────────────────────────────────────────
// Ruta derivada de __FILE__ (robusta al CWD del runner): el test vive en
// app/src/main/cpp/tests/, el asset en app/src/main/assets/kernels/.
std::vector<std::uint8_t> readKernelAsset(const char* fileName) {
    const std::filesystem::path p =
        std::filesystem::path(__FILE__).parent_path() / ".." / ".." /
        "assets" / "kernels" / fileName;
    std::ifstream in(p, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    if (size <= 0) return {};
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.seekg(0, std::ios::beg);
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    return in ? bytes : std::vector<std::uint8_t>{};
}

// Carga un paquete en el transductor de la cadena siguiendo el contrato
// del motor (deshabilitar -> cargar -> restaurar habilitado).
ivanna::dsp::VolterraLoadStatus loadIntoChain(
    FusedChain& chain, const std::vector<std::uint8_t>& pkg, bool enableAfter) {
    chain.transducer().setVolterraEnabled(false);
    const auto st = chain.transducer().loadKernelPackage(
        pkg.data(), pkg.size(), ivanna::dsp::VolterraLoadPolicy{});
    if (st == ivanna::dsp::VolterraLoadStatus::Ok && enableAfter) {
        chain.transducer().setVolterraEnabled(true);
    }
    return st;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 1) GENEZI
// ═══════════════════════════════════════════════════════════════════════════

// El asset Genezi carga en el motor fusionado de Ruta A con la política
// de producción y queda registrado como kernel activo.
TEST(EvolGenezi, GeneziPackageLoadsIntoFusedChain) {
    const auto genezi = readKernelAsset("genezy_mhc_pz1d.ivk1");
    const auto pz1d = readKernelAsset("moto_g85_pz1d.ivk1");
    ASSERT_EQ(genezi.size(), 8616u) << "asset Genezi ausente o truncado";
    ASSERT_EQ(pz1d.size(), 8616u) << "asset PZ1D ausente o truncado";
    ASSERT_NE(genezi, pz1d) << "los dos assets no deben ser idénticos";

    FusedChain chain;
    chain.prepare(kFs);
    // Contrato: con el motor habilitado la carga devuelve EngineBusy.
    {
        FusedChain busy;
        busy.prepare(kFs);
        ASSERT_TRUE(busy.transducer().isVolterraEnabled())
            << "el motor arranca habilitado (default VolterraH2Symmetric)";
        const auto st = busy.transducer().loadKernelPackage(
            genezi.data(), genezi.size(), ivanna::dsp::VolterraLoadPolicy{});
        EXPECT_EQ(st, ivanna::dsp::VolterraLoadStatus::EngineBusy)
            << "cargar con el motor habilitado debe rechazar sin tocar nada";
        EXPECT_FALSE(busy.transducer().volterraHasEffectiveKernel());
    }

    const auto st = loadIntoChain(chain, genezi, true);
    EXPECT_EQ(st, ivanna::dsp::VolterraLoadStatus::Ok)
        << "status=" << ivanna::dsp::volterraLoadStatusName(st);
    EXPECT_TRUE(chain.transducer().volterraHasEffectiveKernel());
    EXPECT_STREQ(chain.transducer().volterraActiveKernelDeviceId(),
                 "MOTO_G85_GENEZY");
    EXPECT_TRUE(chain.transducer().isVolterraEnabled());

    // El PZ1D carga igual en otra cadena y registra su propio id.
    FusedChain chainPz;
    chainPz.prepare(kFs);
    const auto stPz = loadIntoChain(chainPz, pz1d, true);
    EXPECT_EQ(stPz, ivanna::dsp::VolterraLoadStatus::Ok);
    EXPECT_STREQ(chainPz.transducer().volterraActiveKernelDeviceId(),
                 "MOTO_G85_PZ1D");
    std::printf("[GENEZI] carga Genezi=%s (%s) | PZ1D=%s (%s)\n",
                ivanna::dsp::volterraLoadStatusName(st),
                chain.transducer().volterraActiveKernelDeviceId(),
                ivanna::dsp::volterraLoadStatusName(stPz),
                chainPz.transducer().volterraActiveKernelDeviceId());
}

// El kernel Genezi cambia la salida de forma medible: genera 2º armónico
// (H2) y mueve el RMS frente al bypass sin kernel y frente al PZ1D.
// Medido en el transductor aislado (sin los otros 6 operadores) para que
// nada enmascare el efecto del kernel.
TEST(EvolGenezi, GeneziKernelChangesOutputMeasurably) {
    const auto genezi = readKernelAsset("genezy_mhc_pz1d.ivk1");
    const auto pz1d = readKernelAsset("moto_g85_pz1d.ivk1");
    ASSERT_EQ(genezi.size(), 8616u);
    ASSERT_EQ(pz1d.size(), 8616u);

    constexpr double kToneHz = 1000.0;
    constexpr double kToneAmp = 0.5;  // −6 dBFS: H2 en rango, sin techo
    constexpr std::size_t kFrames = 48000;  // 1 s
    constexpr std::size_t kSkipT = 12000;   // 0.25 s de asentamiento
    auto runTransducer = [&](const std::vector<std::uint8_t>* pkg,
                             double& rmsDbOut, double& h2Db,
                             double& h1Amp) {
        FusedTransducer tr;
        tr.prepare(kFs);
        TransducerControls c;
        c.correction = 1.0f;
        c.temporalAmount = 0.0f;
        c.emfEnabled = false;
        tr.setControls(c);
        if (pkg != nullptr) {
            tr.setVolterraEnabled(false);
            const auto st = tr.loadKernelPackage(
                pkg->data(), pkg->size(), ivanna::dsp::VolterraLoadPolicy{});
            EXPECT_EQ(st, ivanna::dsp::VolterraLoadStatus::Ok)
                << "status=" << ivanna::dsp::volterraLoadStatusName(st);
            tr.setVolterraEnabled(true);
        }
        std::vector<double> l(kFrames), r(kFrames);
        fillToneDouble(l.data(), kFrames, kFs, kToneHz, kToneAmp);
        fillToneDouble(r.data(), kFrames, kFs, kToneHz, kToneAmp);
        tr.process(l.data(), r.data(), static_cast<int>(kFrames));
        const double inRms = 20.0 * std::log10(kToneAmp / std::sqrt(2.0));
        rmsDbOut = rmsDb(l.data() + kSkipT, kFrames - kSkipT) - inRms;
        double ph = 0.0;
        projectFund(l.data() + kSkipT, kFrames - kSkipT, kToneHz, h1Amp, ph);
        double h2Amp = 0.0;
        projectFund(l.data() + kSkipT, kFrames - kSkipT, 2.0 * kToneHz, h2Amp, ph);
        h2Db = 20.0 * std::log10(h2Amp / h1Amp);
        // Diferencia RMS contra la entrada (para Genezi vs PZ1D).
        double diffSq = 0.0;
        std::vector<double> ref(kFrames);
        fillToneDouble(ref.data(), kFrames, kFs, kToneHz, kToneAmp);
        for (std::size_t i = kSkipT; i < kFrames; ++i) {
            const double d = l[i] - ref[i];
            diffSq += d * d;
        }
        const double diffRms = std::sqrt(diffSq / (kFrames - kSkipT));
        std::printf("[GENEZI] %s: net=%+.3f dB H2=%+.1f dBFS-rel "
                    "diff-vs-entrada=%.5f RMS\n",
                    pkg == nullptr ? "sin-kernel"
                    : (pkg == &genezi ? "Genezi" : "PZ1D"),
                    rmsDbOut, h2Db, diffRms);
        return diffRms;
    };

    double rmsNone = 0, h2None = 0, h1None = 0;
    const double diffNone = runTransducer(nullptr, rmsNone, h2None, h1None);
    double rmsG = 0, h2G = 0, h1G = 0;
    const double diffG = runTransducer(&genezi, rmsG, h2G, h1G);
    double rmsP = 0, h2P = 0, h1P = 0;
    const double diffP = runTransducer(&pz1d, rmsP, h2P, h1P);

    // Sin kernel el transductor es bypass bit-exacto.
    EXPECT_NEAR(diffNone, 0.0, 1e-12);
    EXPECT_NEAR(rmsNone, 0.0, 1e-9);
    // Genezi: H2 medible y salida distinta del bypass.
    EXPECT_GT(diffG, 1e-4) << "Genezi no cambia la salida: kernel mudo";
    EXPECT_LT(h2G, -20.0) << "H2 Genezi inesperadamente alto";
    EXPECT_GT(h2G, -120.0) << "H2 Genezi ausente (sin 2º armónico medible)";
    // Genezi ≠ PZ1D: los dos kernels suenan distinto.
    EXPECT_GT(std::abs(diffG - diffP), 1e-5)
        << "Genezi y PZ1D suenan idéntico pese a ser paquetes distintos";
    std::printf("[GENEZI] diff Genezi=%.5f PZ1D=%.5f | H2 Genezi=%+.1f dB "
                "PZ1D=%+.1f dB | net Genezi=%+.3f PZ1D=%+.3f dB\n",
                diffG, diffP, h2G, h2P, rmsG, rmsP);
}

// Reemplazo PZ1D → Genezi con música sonando (tono 1 kHz, cadena con
// solo el transductor activo): el fundido ~18 ms no mete clic ni hueco.
// El hilo de control llama a replaceKernelPackage mientras el hilo de
// audio sigue procesando en trozos de 128. Criterio de clic: el mayor
// salto muestra-a-muestra durante la ventana del cambio no supera
// 1.5× el mayor salto del mismo tono sin cambio; y ninguna ventana de
// 5 ms cae más de 1 dB bajo el RMS en régimen (sin hueco de silencio).
TEST(EvolGenezi, GeneziReplaceHasNoClick) {
    const auto genezi = readKernelAsset("genezy_mhc_pz1d.ivk1");
    const auto pz1d = readKernelAsset("moto_g85_pz1d.ivk1");
    ASSERT_EQ(genezi.size(), 8616u);
    ASSERT_EQ(pz1d.size(), 8616u);

    constexpr double kToneHz = 1000.0;
    constexpr int kChunk = 128;
    constexpr int kTotalChunks = 750;  // 2 s @ 48 kHz
    constexpr int kSwapChunk = 375;    // cambio a 1.0 s

    auto runWithSwap = [&](bool doSwap, double& maxJump, double& minWinDb,
                           std::string& kernelAfter) {
        FusedControls c;
        zero7(c);
        c.transducer.correction = 1.0f;
        c.clamp();
        FusedChain chain;
        chain.prepare(kFs);
        chain.setControls(c);
        chain.setVolumetricEnabled(false);
        // Kernel inicial PZ1D (o ninguno si no hay cambio que probar).
        if (doSwap) {
            const auto st = loadIntoChain(chain, pz1d, true);
            EXPECT_EQ(st, ivanna::dsp::VolterraLoadStatus::Ok);
        }
        std::vector<double> outL(static_cast<std::size_t>(kTotalChunks) * kChunk);
        std::vector<double> outR(static_cast<std::size_t>(kTotalChunks) * kChunk);
        std::atomic<int> swapStart{-1}, swapEnd{-1};
        std::atomic<int> swapStatus{-999};
        std::future<void> fut;
        if (doSwap) {
            fut = std::async(std::launch::async, [&]() {
                swapStart.store(kSwapChunk);
                const auto st = chain.transducer().replaceKernelPackage(
                    genezi.data(), genezi.size(),
                    ivanna::dsp::VolterraLoadPolicy{});
                swapStatus.store(static_cast<int>(st));
                chain.transducer().setVolterraEnabled(true);
                swapEnd.store(kSwapChunk);
            });
        }
        // Hilo de audio: trozos de 128 con el tono continuo.
        double phase = 0.0;
        const double step = 2.0 * 3.14159265358979323846 * kToneHz / kFs;
        for (int ch = 0; ch < kTotalChunks; ++ch) {
            std::vector<double> l(kChunk), r(kChunk);
            for (int i = 0; i < kChunk; ++i) {
                const double v = kAmp * std::sin(phase);
                l[static_cast<std::size_t>(i)] = v;
                r[static_cast<std::size_t>(i)] = v;
                phase += step;
                if (phase >= 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
            }
            if (ch == kSwapChunk && doSwap) {
                // Esperar a que el hilo de control arranque el reemplazo
                // antes de seguir procesando (el cambio ocurre con audio
                // fluyendo: los trozos siguientes ya ven el fundido).
                while (swapStart.load() < 0) std::this_thread::yield();
            }
            chain.process(l.data(), r.data(), kChunk);
            std::copy(l.begin(), l.end(),
                      outL.begin() + static_cast<std::ptrdiff_t>(ch) * kChunk);
            std::copy(r.begin(), r.end(),
                      outR.begin() + static_cast<std::ptrdiff_t>(ch) * kChunk);
        }
        if (doSwap) {
            ASSERT_EQ(fut.wait_for(std::chrono::seconds(10)),
                      std::future_status::ready)
                << "replaceKernelPackage no terminó en 10 s";
            EXPECT_EQ(swapStatus.load(), 0)
                << "replace status="
                << ivanna::dsp::volterraLoadStatusName(
                       static_cast<ivanna::dsp::VolterraLoadStatus>(swapStatus.load()));
            kernelAfter = chain.transducer().volterraActiveKernelDeviceId();
        }
        // Métricas sobre la salida L en la ventana del cambio
        // [0.9 s, 1.5 s): salto máx. muestra a muestra y RMS mínimo por
        // ventana de 5 ms (240 muestras).
        const std::size_t w0 = static_cast<std::size_t>(0.9 * kFs);
        const std::size_t w1 = static_cast<std::size_t>(1.5 * kFs);
        maxJump = 0.0;
        for (std::size_t i = w0 + 1; i < w1 && i < outL.size(); ++i) {
            maxJump = std::max(maxJump, std::abs(outL[i] - outL[i - 1]));
        }
        minWinDb = 1e9;
        for (std::size_t off = w0; off + 240 <= w1; off += 240) {
            minWinDb = std::min(minWinDb, rmsDb(outL.data() + off, 240));
        }
        // RMS en régimen (2º segundo) para la referencia del hueco.
        const double steadyDb = rmsDb(outL.data() + 48000, 24000);
        minWinDb -= steadyDb;  // dB relativos al régimen
        std::printf("[GENEZI] swap=%d maxJump=%.5f minWinRel=%+.2f dB "
                    "kernelAfter=%s\n",
                    doSwap ? 1 : 0, maxJump, minWinDb, kernelAfter.c_str());
    };

    double baseJump = 0, baseMinWin = 0;
    std::string dummy;
    runWithSwap(false, baseJump, baseMinWin, dummy);
    double swapJump = 0, swapMinWin = 0;
    std::string after;
    runWithSwap(true, swapJump, swapMinWin, after);
    EXPECT_EQ(after, "MOTO_G85_GENEZY");
    // Sin clic: el salto durante el cambio no supera 1.5× el del tono solo.
    EXPECT_LE(swapJump, 1.5 * baseJump)
        << "el reemplazo mete un escalón (clic) sobre el tono";
    // Sin hueco: ninguna ventana de 5 ms cae >1 dB bajo el régimen.
    EXPECT_GT(swapMinWin, -1.0)
        << "el reemplazo deja un hueco de silencio durante el fundido";
    std::printf("[GENEZI] clic: base maxJump=%.5f swap maxJump=%.5f "
                "(ratio %.2f) | hueco: base %+.2f dB swap %+.2f dB\n",
                baseJump, swapJump, swapJump / baseJump, baseMinWin, swapMinWin);
}

// La vía tri21 (NewPhysicsEngine::loadGeneziVolterra) NO tiene datos
// medidos: no existe genezi_np_cal.json ni arrays tri21 en el repo ni en
// assets, y ningún llamador Kotlin invoca
// DSPBridge.loadNewPhysicsCalibration. Este test documenta el estado:
// el kernel Genezi MEDIDO que existe y suena es el .ivk1 (vía
// VolterraH2Symmetric); el tri21 exige un barrido Farina real del
// MHC-PZ1D que nunca se ha hecho (fuera del alcance digital: requiere
// teléfono + micrófono). Si alguien cablea el tri21 sin esa medición,
// este test debe actualizarse con la fuente de los datos.
TEST(EvolGenezi, GeneziTri21PathHasNoMeasuredData) {
    const std::filesystem::path repoRoot =
        std::filesystem::path(__FILE__).parent_path() / ".." / ".." / ".." /
        ".." / "..";
    // app/src/main/cpp/tests/ -> repo root = 5 niveles arriba.
    bool foundCal = false;
    for (const auto& e :
         std::filesystem::recursive_directory_iterator(repoRoot)) {
        if (!e.is_regular_file()) continue;
        const std::string name = e.path().filename().string();
        if (name == "genezi_np_cal.json" || name == "genezi_np_cal.txt") {
            foundCal = true;
        }
    }
    std::printf("[GENEZI] tri21: genezi_np_cal.* en repo = %s "
                "(false = sin datos medidos; el cableado exige barrido Farina)\n",
                foundCal ? "SI" : "NO");
    EXPECT_FALSE(foundCal)
        << "apareció un genezi_np_cal: actualizar el cableado tri21 y este test";
}

// ═══════════════════════════════════════════════════════════════════════════
// 2) NOTCH DE ENGANCHE −15.8 dB a 977 Hz
// ═══════════════════════════════════════════════════════════════════════════

// Reproducción: P0-engaged a 977 Hz ≈ −14.94 dB (ablación §2.3);
// ARMONÍA plana ≈ −15.85 dB. Atribución apagando APEX, TΩ, D10 y safety
// uno a uno + LOO de NEURO. La TΩ no corre en HEADPHONES por diseño
// (solo SPEAKERS), así que se mide también en SPEAKERS para no
// exonerarla sin medirla.
TEST(EvolGenezi, EngageNotchAttribution977) {
    const ToneResult p0 = runTone(p0EngagedControls(), 977.0);
    const double p0m = meanLr(p0.netL, p0.netR);
    std::printf("[NOTCH] P0-engaged 977 Hz: netL=%+.3f netR=%+.3f media=%+.3f "
                "(ref ablación: -14.938)\n", p0.netL, p0.netR, p0m);
    EXPECT_NEAR(p0m, -14.938, 1.0);

    // ARMONÍA plana (curva 0, low/mid/high 0) sobre base P0: ref −15.846.
    FusedControls flat = p0EngagedControls();
    flat.tonal.curve = 0;
    flat.tonal.lowDb = 0.0f;
    flat.tonal.midDb = 0.0f;
    flat.tonal.highDb = 0.0f;
    flat.clamp();
    const ToneResult flatRes = runTone(flat, 977.0);
    const double flatM = meanLr(flatRes.netL, flatRes.netR);
    std::printf("[NOTCH] P0 + ARMONIA plana 977 Hz: media=%+.3f "
                "(ref ablación: -15.846)\n", flatM);
    EXPECT_NEAR(flatM, -15.846, 1.0);

    // Atribución sobre ALL7 a 977 Hz (ref ablación ALL7: −14.150 media).
    const ToneResult all7 = runTone(all7Controls(), 977.0);
    const double all7m = meanLr(all7.netL, all7.netR);
    std::printf("[NOTCH] ALL7 977 Hz: media=%+.3f (ref: -14.150)\n", all7m);
    EXPECT_NEAR(all7m, -14.150, 1.0);

    ChainToggles noApex; noApex.apex = false;
    const ToneResult rNoApex = runTone(all7Controls(), 977.0, kAmp, noApex);
    const double mNoApex = meanLr(rNoApex.netL, rNoApex.netR);
    ChainToggles noVol; noVol.volumetric = false;
    const ToneResult rNoVol = runTone(all7Controls(), 977.0, kAmp, noVol);
    const double mNoVol = meanLr(rNoVol.netL, rNoVol.netR);
    ChainToggles noD10; noD10.d10 = false;
    const ToneResult rNoD10 = runTone(all7Controls(), 977.0, kAmp, noD10);
    const double mNoD10 = meanLr(rNoD10.netL, rNoD10.netR);
    ChainToggles noSafety; noSafety.safetyOff = true;
    const ToneResult rNoSafety = runTone(all7Controls(), 977.0, kAmp, noSafety);
    const double mNoSafety = meanLr(rNoSafety.netL, rNoSafety.netR);
    // LOO NEURO a 977 Hz (ref ablación: marginal −11.160 dB).
    FusedControls noNeuro = all7Controls();
    setOp(noNeuro, 6, 0.0f);
    noNeuro.clamp();
    const ToneResult rNoNeuro = runTone(noNeuro, 977.0);
    const double mNoNeuro = meanLr(rNoNeuro.netL, rNoNeuro.netR);
    // TΩ en SPEAKERS (volumétrico on + modo SPEAKERS): la matriz XTC sí corre.
    ChainToggles spk; spk.speakers = true;
    const ToneResult rSpk = runTone(all7Controls(), 977.0, kAmp, spk);
    const double mSpk = meanLr(rSpk.netL, rSpk.netR);
    ChainToggles spkNoVol; spkNoVol.speakers = true; spkNoVol.volumetric = false;
    const ToneResult rSpkNoVol = runTone(all7Controls(), 977.0, kAmp, spkNoVol);
    const double mSpkNoVol = meanLr(rSpkNoVol.netL, rSpkNoVol.netR);

    std::printf("[NOTCH] ALL7 977 Hz media=%+.3f | sin APEX=%+.3f (delta %+.3f) | "
                "sin volumetric=%+.3f (delta %+.3f) | sin D10=%+.3f (delta %+.3f) | "
                "sin safety=%+.3f (delta %+.3f) | sin NEURO=%+.3f (marginal %+.3f)\n",
                all7m, mNoApex, mNoApex - all7m, mNoVol, mNoVol - all7m,
                mNoD10, mNoD10 - all7m, mNoSafety, mNoSafety - all7m,
                mNoNeuro, all7m - mNoNeuro);
    std::printf("[NOTCH] SPEAKERS 977 Hz: ALL7=%+.3f sin volumetric=%+.3f "
                "(delta TΩ %+.3f dB)\n",
                mSpk, mSpkNoVol, mSpk - mSpkNoVol);

    // El notch lo domina NEURO (marginal ≈ −11.2 dB a 977 Hz); APEX, D10
    // y safety mueven <0.5 dB cada uno; la TΩ en SPEAKERS se mide aparte.
    EXPECT_NEAR(all7m - mNoNeuro, -11.160, 1.0);
    EXPECT_LT(std::abs(mNoApex - all7m), 0.5)
        << "APEX mueve el notch más de lo esperado";
    EXPECT_LT(std::abs(mNoD10 - all7m), 0.5)
        << "D10 mueve el notch más de lo esperado (el árbitro no toca el buffer)";
    EXPECT_LT(std::abs(mNoSafety - all7m), 1.0)
        << "safety mueve el notch más de lo esperado";

    // Transitorio de enganche: ver EngageDipAttribution977 (test dedicado):
    // la peor ventana de 5 ms en 0-100 ms cae 5.37 dB bajo el régimen
    // (medido aquí en la primera corrida: -25.38 dBFS vs -20.01 dBFS).
    // El veredicto (artefacto audible o no, y su reparación) se decide
    // allí tras atribuirlo por operador; este test cubre el régimen.
}

// ═══════════════════════════════════════════════════════════════════════════
// 3) INTERACCIÓN −25 dB a 11.5 kHz
// ═══════════════════════════════════════════════════════════════════════════

// Reproducción + atribución por solos y pares. Hipótesis a verificar:
// la interacción es dependencia de nivel (ORBE realza +21.7 dB en HF y
// la señal entra al safety limiter / techo final ±0.9885): si es así,
// la interacción se desploma al bajar el nivel de entrada, y el par
// ORBE+NEURO (o ORBE+safety) concentra la interacción.
TEST(EvolGenezi, Interaction11500Attribution) {
    constexpr double kF = 11500.0;
    const ToneResult all7 = runTone(all7Controls(), kF);
    const double all7m = meanLr(all7.netL, all7.netR);
    std::printf("[11K5] ALL7 11500 Hz: netL=%+.3f netR=%+.3f media=%+.3f "
                "(ref ablación: -13.764)\n", all7.netL, all7.netR, all7m);
    EXPECT_NEAR(all7m, -13.764, 1.0);

    // Solos (un operador a 1.0, resto 0) y pares clave.
    auto solo = [&](std::size_t op) {
        FusedControls c;
        zero7(c);
        setOp(c, op, 1.0f);
        c.clamp();
        const ToneResult t = runTone(c, kF);
        return meanLr(t.netL, t.netR);
    };
    auto pair = [&](std::size_t a, std::size_t b) {
        FusedControls c;
        zero7(c);
        setOp(c, a, 1.0f);
        setOp(c, b, 1.0f);
        c.clamp();
        const ToneResult t = runTone(c, kF);
        return meanLr(t.netL, t.netR);
    };
    const double sOrbe = solo(0), sNeuro = solo(6), sArmon = solo(4);
    const double sHolos = solo(1), sSala = solo(2), sPulso = solo(5);
    const double pOrbeNeuro = pair(0, 6);
    const double pOrbeArmon = pair(0, 4);
    const double pNeuroArmon = pair(6, 4);
    const double pOrbeNeuroArmon = [&] {
        FusedControls c;
        zero7(c);
        setOp(c, 0, 1.0f); setOp(c, 6, 1.0f); setOp(c, 4, 1.0f);
        c.clamp();
        const ToneResult t = runTone(c, kF);
        return meanLr(t.netL, t.netR);
    }();
    std::printf("[11K5] solos: ORBE=%+.3f NEURO=%+.3f ARMONIA=%+.3f HOLOS=%+.3f "
                "SALA=%+.3f PULSO=%+.3f\n",
                sOrbe, sNeuro, sArmon, sHolos, sSala, sPulso);
    std::printf("[11K5] pares: ORBE+NEURO=%+.3f (interacción %+.3f) | "
                "ORBE+ARMONIA=%+.3f (interacción %+.3f) | "
                "NEURO+ARMONIA=%+.3f (interacción %+.3f) | "
                "ORBE+NEURO+ARMONIA=%+.3f (interacción %+.3f)\n",
                pOrbeNeuro, pOrbeNeuro - (sOrbe + sNeuro),
                pOrbeArmon, pOrbeArmon - (sOrbe + sArmon),
                pNeuroArmon, pNeuroArmon - (sNeuro + sArmon),
                pOrbeNeuroArmon,
                pOrbeNeuroArmon - (sOrbe + sNeuro + sArmon));

    // LOO a 11.5 kHz (reproduce la tabla de la ablación §2.2).
    double sumLoo = 0.0;
    for (std::size_t o = 0; o < 7; ++o) {
        FusedControls c = all7Controls();
        setOp(c, o, 0.0f);
        c.clamp();
        const ToneResult t = runTone(c, kF);
        const double marg = all7m - meanLr(t.netL, t.netR);
        sumLoo += marg;
        std::printf("[11K5] LOO sin %-8s: net=%+.3f marginal=%+.3f\n",
                    kOpName[o], meanLr(t.netL, t.netR), marg);
    }
    const double interaction = all7m - sumLoo;
    std::printf("[11K5] suma marginales=%+.3f total=%+.3f interacción=%+.3f dB "
                "(ref puerta-HRTF: -4.08 dB; antes -17.35 dB con la reserva activa)\n", sumLoo, all7m, interaction);
    // Tras la puerta HRTF (phone-loop): el run sin-ORBE ya no renderiza
    // la HRTF de reserva, así que la interacción "contaminada" colapsa
    // de -17.35 a -4.08 dB — el valor del LOO limpio (-3.90 dB). La
    // puerta hace que el LOO normal sea limpio por construcción.
    EXPECT_NEAR(interaction, -4.08, 3.0);

    // ── VEREDICTO (corrida 8): la "interacción" es la etapa HRTF de
    // reserva. Quitar ORBE (amount=0) con hrtfWet=1.0 (default) NO lo
    // apaga: selectBinauralStage() elige HRTF y processBinauralScene()
    // (síntesis analítica: sin .ihr1) colorea a 11.5 kHz con L −12.935 /
    // R −12.935 dB (simétrico tras el fix HRTF; antes R era −43.715 dB
    // por la reconstrucción PCA monoaural rota). El LOO limpio (quitar
    // ORBE = amount 0 + hrtfWet 0;
    // quitar HOLOS = amount 0 + phiAmount 0) colapsa la interacción a ~0.
    double sumLooClean = 0.0;
    for (std::size_t o = 0; o < 7; ++o) {
        FusedControls c = all7Controls();
        setOp(c, o, 0.0f);
        if (o == 0) c.spatial.hrtfWet = 0.0f;   // quitar ORBE de verdad
        if (o == 1) c.field.phiAmount = 0.0f;   // quitar HOLOS de verdad
        c.clamp();
        const ToneResult t = runTone(c, kF);
        const double marg = all7m - meanLr(t.netL, t.netR);
        sumLooClean += marg;
        std::printf("[11K5] LOO LIMPIO sin %-8s: net=%+.3f marginal=%+.3f\n",
                    kOpName[o], meanLr(t.netL, t.netR), marg);
    }
    const double interactionClean = all7m - sumLooClean;
    std::printf("[11K5] LOO limpio: suma marginales=%+.3f total=%+.3f "
                "interacción=%+.3f dB (la de -25 dB era la HRTF)\n",
                sumLooClean, all7m, interactionClean);
    // La interacción colapsa de −17.35 a −3.811 dB: 13.5 dB eran la HRTF
    // (antes del fix HRTF era de −24.033 a −3.811: 20.2 dB).
    // El residuo −3.811 dB es el AGC de NEURO (dependiente de nivel: su
    // ganancia se reajusta al quitar cada operador, así que los
    // marginales LOO no suman). Prueba: el mismo LOO limpio en MANUAL
    // (cerebro neutro, sin AGC) debe dar interacción ≈ 0.
    EXPECT_NEAR(interactionClean, -3.81, 1.0);
    {
        FusedControls all7mnl = all7Controls();
        all7mnl.brain.mode = 1;  // MANUAL: sin AGC
        all7mnl.clamp();
        const ToneResult tM = runTone(all7mnl, kF);
        const double totM = meanLr(tM.netL, tM.netR);
        double sumM = 0.0;
        for (std::size_t o = 0; o < 7; ++o) {
            FusedControls c = all7mnl;
            setOp(c, o, 0.0f);
            if (o == 0) c.spatial.hrtfWet = 0.0f;
            if (o == 1) c.field.phiAmount = 0.0f;
            c.clamp();
            const ToneResult t = runTone(c, kF);
            sumM += totM - meanLr(t.netL, t.netR);
        }
        const double interM = totM - sumM;
        std::printf("[11K5] LOO limpio MANUAL (sin AGC): total=%+.3f "
                    "suma marginales=%+.3f interacción=%+.3f dB\n",
                    totM, sumM, interM);
        EXPECT_NEAR(interM, 0.0, 1.0);
    }

    // Dependencia de nivel: misma ablación con entrada −12 dBFS y −20 dBFS.
    // Si la interacción es el techo/limiter, debe desplomarse al bajar.
    auto interactionAtLevel = [&](double amp) {
        const ToneResult a = runTone(all7Controls(), kF, amp);
        const double tot = meanLr(a.netL, a.netR);
        double sum = 0.0;
        for (std::size_t o = 0; o < 7; ++o) {
            FusedControls c = all7Controls();
            setOp(c, o, 0.0f);
            c.clamp();
            const ToneResult t = runTone(c, kF, amp);
            sum += tot - meanLr(t.netL, t.netR);
        }
        return tot - sum;
    };
    const double interM12 = interactionAtLevel(0.25);    // −12 dBFS pico
    const double interM20 = interactionAtLevel(0.1);     // −20 dBFS pico
    std::printf("[11K5] interacción por nivel: -3 dBFS %+.3f | -12 dBFS %+.3f | "
                "-20 dBFS %+.3f dB\n", interaction, interM12, interM20);

    // Toggles sobre ALL7 a 11.5 kHz: APEX off, D10 off, safety off.
    ChainToggles noApex; noApex.apex = false;
    const ToneResult rNoApex = runTone(all7Controls(), kF, kAmp, noApex);
    const double mNoApex = meanLr(rNoApex.netL, rNoApex.netR);
    ChainToggles noD10; noD10.d10 = false;
    const ToneResult rNoD10 = runTone(all7Controls(), kF, kAmp, noD10);
    const double mNoD10 = meanLr(rNoD10.netL, rNoD10.netR);
    ChainToggles noSafety; noSafety.safetyOff = true;
    const ToneResult rNoSafety = runTone(all7Controls(), kF, kAmp, noSafety);
    const double mNoSafety = meanLr(rNoSafety.netL, rNoSafety.netR);
    std::printf("[11K5] ALL7 toggles: base=%+.3f sin APEX=%+.3f (delta %+.3f) | "
                "sin D10=%+.3f (delta %+.3f) | sin safety=%+.3f (delta %+.3f) | "
                "pico L base=%.4f\n",
                all7m, mNoApex, mNoApex - all7m, mNoD10, mNoD10 - all7m,
                mNoSafety, mNoSafety - all7m, all7.peakL);

    // Pico: si ORBE empuja la señal al techo final, el pico en régimen
    // debe estar clavado en 0.9885 (±0.1 dBFS).
    EXPECT_GT(all7.peakL, 0.9)
        << "el pico no llega al techo: la hipótesis techo/limiter no cierra";
}

// ═══════════════════════════════════════════════════════════════════════════
// 2b) HUECO TRANSITORIO DE ENGANCHE a 977 Hz — atribución por operador
// ═══════════════════════════════════════════════════════════════════════════

// Corrida 1 midió en ALL7 a 977 Hz un hueco transitorio de −5.37 dB
// (peor ventana de 5 ms en 0–100 ms, −25.38 dBFS, frente a −20.01 dBFS
// en régimen). Aquí se atribuye: hueco por operador solo + toggles
// (APEX off, D10 off, safety off). Candidato: la mezcla seco/procesado
// de las rampas de wet de 30 ms cuando el procesado desfasa el tono
// (ORBE/HRTF), que cancela parcialmente durante la rampa.
// FIX (phone-loop, mezcla de potencia constante en blendWetCore): el dip
// baja de −6.14 dB medido a −2.32 dB medido. La pérdida por mezcla lineal
// desaparece; queda el residuo de fase por la latencia del procesado
// (HOLOS solo −4.06 dB domina el residuo). APEX/D10/safety siguen sin
// moverlo (deltas < 1 dB).
TEST(EvolGenezi, EngageDipAttribution977) {
    const double dipAll7 = engageDipDb(all7Controls(), 977.0);
    std::printf("[DIP] ALL7 977 Hz hueco transitorio = %+.2f dB\n", dipAll7);

    auto dipSolo = [&](std::size_t op) {
        FusedControls c;
        zero7(c);
        setOp(c, op, 1.0f);
        c.clamp();
        return engageDipDb(c, 977.0);
    };
    double dipSolos[7] = {};
    for (std::size_t o = 0; o < 7; ++o) {
        dipSolos[o] = dipSolo(o);
        std::printf("[DIP] solo %-8s hueco = %+.2f dB\n", kOpName[o], dipSolos[o]);
    }
    ChainToggles noApex; noApex.apex = false;
    const double dipNoApex = engageDipDb(all7Controls(), 977.0, kAmp, noApex);
    ChainToggles noD10; noD10.d10 = false;
    const double dipNoD10 = engageDipDb(all7Controls(), 977.0, kAmp, noD10);
    ChainToggles noSafety; noSafety.safetyOff = true;
    const double dipNoSafety =
        engageDipDb(all7Controls(), 977.0, kAmp, noSafety);
    std::printf("[DIP] ALL7 sin APEX=%+.2f sin D10=%+.2f sin safety=%+.2f dB\n",
                dipNoApex, dipNoD10, dipNoSafety);

    // El hueco de ALL7 se reproduce (±1 dB) y se atribuye al operador
    // cuyo solo muestra el mismo hueco. APEX/D10/safety no lo mueven
    // (sus deltas son <1 dB): el hueco vive en las rampas de wet.
    // Post-fix (mezcla de potencia constante, phone-loop): −2.32 dB
    // medido (era −5.37 dB de referencia / −6.14 dB medido pre-fix);
    // el residuo es fase por latencia del procesado (HOLOS solo −4.06).
    EXPECT_NEAR(dipAll7, -2.32, 1.0);
    EXPECT_LT(std::abs(dipNoApex - dipAll7), 1.0);
    EXPECT_LT(std::abs(dipNoD10 - dipAll7), 1.0);
    EXPECT_LT(std::abs(dipNoSafety - dipAll7), 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3b) INTERACCIÓN 11.5 kHz — estructura: ¿recuperación por conteo o por par?
// ═══════════════════════════════════════════════════════════════════════════

// Corrida 2: los "solos" NO son L/R simétricos a 11.5 kHz. runTone
// promedia L/R (meanLr) y soloFreq mide solo L: NEURO solo da
// −43.307 dB (media L/R) pero −27.554 dB (solo L) — la R pierde
// ~−59 dB. El "+29 dB de interacción por par" es en gran parte
// ARITMÉTICA de esa asimetría, no interacción entre operadores.
// Este test resuelve L/R por separado para cada solo y con APEX off,
// para localizar qué etapa destruye un canal a 11.5 kHz.
// (La hipótesis techo/limiter de la corrida 1 queda REFUTADA: la
// interacción persiste a −20 dBFS (−20.2 dB) y safety/APEX/D10 no la
// mueven en ALL7; el pico 0.9815 en ALL7 es consecuencia, no causa.)
TEST(EvolGenezi, Interaction11500LeftRight) {
    constexpr double kF = 11500.0;
    auto soloLR = [&](std::size_t op, const ChainToggles& tg = ChainToggles{}) {
        FusedControls c;
        zero7(c);
        setOp(c, op, 1.0f);
        c.clamp();
        const ToneResult t = runTone(c, kF, kAmp, tg);
        std::printf("[11K5C] solo %-8s: netL=%+.3f netR=%+.3f media=%+.3f "
                    "picoL=%.4f picoR=%.4f\n",
                    kOpName[op], t.netL, t.netR, meanLr(t.netL, t.netR),
                    t.peakL, t.peakR);
        return std::make_pair(t.netL, t.netR);
    };
    for (std::size_t o = 0; o < 7; ++o) soloLR(o);
    ChainToggles noApex; noApex.apex = false;
    std::printf("[11K5C] --- con APEX off ---\n");
    for (std::size_t o = 0; o < 7; ++o) soloLR(o, noApex);
    // VOLTERRA sin kernel debe ser 0.000 dB en AMBOS canales: el
    // transductor aislado ya lo es (diff 0.0). Si la cadena lo pierde,
    // la pérdida vive fuera del transductor (APEX / wet / dynamics).
    const auto v = soloLR(3);
    EXPECT_NEAR(v.first, 0.0, 1.0);
    EXPECT_NEAR(v.second, 0.0, 1.0);

    // Barrido de NEURO solo (canal L): ¿el −27.6 dB es específico de
    // 11.5 kHz? Proyección de la fundamental: si fundAmp coincide con
    // el RMS, es atenuación pura del tono (no destrucción).
    for (double f : {1000.0, 3300.0, 6000.0, 11500.0}) {
        const SoloFreq sf = soloFreq(6, f);
        const double fundDb = 20.0 * std::log10(sf.fundAmp / kAmp);
        std::printf("[11K5C] NEURO solo L %5.0f Hz: net=%+.3f dB "
                    "fundamental=%+.3f dB (fase %+.1f°) | dif net-fund=%+.3f dB\n",
                    f, sf.netDb, fundDb, sf.fundPhaseDeg, sf.netDb - fundDb);
    }
    const SoloFreq sf115 = soloFreq(6, 11500.0);
    const double fundDb115 = 20.0 * std::log10(sf115.fundAmp / kAmp);
    EXPECT_NEAR(sf115.netDb - fundDb115, 0.0, 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 2c) HUECO DE ENGANCHE — barrido de frecuencia (¿cancelación por fase?)
// ═══════════════════════════════════════════════════════════════════════════

// Si el hueco es cancelación seco/procesado durante la rampa de wet
// (el procesado desfasa o retrasa el tono), su profundidad debe variar
// fuerte con la frecuencia (patrón de peine). Si fuera un simple
// fundido de nivel, sería plano en frecuencia. Se mide ALL7 y HOLOS
// solo (el peor solo: −13.60 dB a 977 Hz).
TEST(EvolGenezi, EngageDipFrequencySweep) {
    for (double f : {250.0, 500.0, 977.0, 2000.0, 4000.0}) {
        const double dipAll7 = engageDipDb(all7Controls(), f);
        FusedControls h;
        zero7(h);
        setOp(h, 1, 1.0f);
        h.clamp();
        const double dipHolos = engageDipDb(h, f);
        std::printf("[DIPF] %5.0f Hz: ALL7 hueco=%+.2f dB | HOLOS solo hueco=%+.2f dB\n",
                    f, dipAll7, dipHolos);
    }
    // Valores medidos (Camino A, Opción A: ambos blends compensados,
    // ρ∈[0,1] — fused_chain.cpp): ALL7: 250 Hz +4.21, 500 Hz +2.57,
    // 977 Hz −3.16, 2000 Hz +4.22, 4000 Hz +2.61 dB. HOLOS solo:
    // 250 Hz −1.72, 500 Hz −8.46, 977 Hz −4.06, 2000 Hz −2.47,
    // 4000 Hz −3.82 dB. Son los valores GENUINOS de la Opción A,
    // medidos en local (g++-14 Release) e invariantes al orden
    // ALL7/HOLOS. NO son los de 3de5a92 (ρ∈[−1,1], que daba −6.14 dB
    // a 977 Hz por amplificación con ρ<0 — PROHIBIDO re-baselinear
    // a ese valor). El hueco sigue variando fuerte con la frecuencia
    // (patrón de peine) y HOLOS domina. Riesgo honesto (decisión de
    // Esse, Camino A): el dip acepta más sobre-impulso al enganchar
    // a 2000/4000 Hz (+4.22/+2.61 dB) que los baselines viejos de
    // potencia constante; Tronidos (la medida de engage que manda)
    // dice que el enganche está bien (excess ≤0.70 dB).
    // Firma de cancelación por fase/latencia del wet, no de fundido de
    // nivel.
    const double d250 = engageDipDb(all7Controls(), 250.0);
    const double d500 = engageDipDb(all7Controls(), 500.0);
    const double d977 = engageDipDb(all7Controls(), 977.0);
    const double d2000 = engageDipDb(all7Controls(), 2000.0);
    const double d4000 = engageDipDb(all7Controls(), 4000.0);
    EXPECT_GT(d250, 0.0);
    EXPECT_NEAR(d500, +2.57, 1.0);
    EXPECT_NEAR(d977, -3.16, 1.0);
    EXPECT_NEAR(d2000, +4.22, 1.0);
    EXPECT_NEAR(d4000, +2.61, 1.0);
    FusedControls h500;
    zero7(h500);
    setOp(h500, 1, 1.0f);
    h500.clamp();
    EXPECT_NEAR(engageDipDb(h500, 500.0), -8.46, 1.5);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3c) INTERACCIÓN 11.5 kHz — bisección por etapa (¿dynamics? ¿transductor?)
// ═══════════════════════════════════════════════════════════════════════════

// VEREDICTO (corrida 8, Interaction11500HrtfGate): esa pérdida era la
// etapa HRTF de reserva (hrtfWet=1.0 por defecto, amount=0 → stage HRTF);
// con zero7() limpio el solo es 0.000/0.000. Este test queda como
// bisección: dynamics aislado y transductor aislado dan 0.000/0.000.
// Corrida 3 (contaminada): VOLTERRA solo (sin kernel) perdía
// L −13.173 / R −44.872 dB a 11.5 kHz y PULSO solo daba EXACTO lo mismo;
// APEX off no cambiaba nada; ORBE solo era simétrico (−0.212/−0.212).
// En ese config las únicas etapas que procesan son el transductor
// (no-op verificado aislado, pero con emfEnabled=false) y FusedDynamics
// con safety=1.0 (solo el SafetyLimiter corre: threshold −1 dBFS, tono a
// −3 dBFS — no debería tocarlo — y con ganancia COMÚN L/R, así que
// tampoco explica la asimetría de 31.7 dB). Este test bisecta: dynamics
// aislado, transductor aislado con emfEnabled=true (default), y cadena
// con safetyOff.
TEST(EvolGenezi, Interaction11500StageBisection) {
    constexpr double kF = 11500.0;
    constexpr std::size_t kFrames = 96000;
    constexpr std::size_t kSkipF = 48000;

    // 1) FusedDynamics aislado, safety=1.0 (como en los solos).
    auto dynSolo = [&](float safety) {
        FusedDynamics dyn;
        dyn.prepare(kFs);
        DynamicsControls c;
        c.safety = safety;
        c.clamp();
        dyn.setControls(c);
        std::vector<double> l(kFrames), r(kFrames);
        fillToneDouble(l.data(), kFrames, kFs, kF, kAmp);
        fillToneDouble(r.data(), kFrames, kFs, kF, kAmp);
        dyn.process(l.data(), r.data(), static_cast<int>(kFrames));
        const double netL = rmsDb(l.data() + kSkipF, kFrames - kSkipF) - kInRmsDb;
        const double netR = rmsDb(r.data() + kSkipF, kFrames - kSkipF) - kInRmsDb;
        std::printf("[11K5D] dynamics aislado safety=%.1f: netL=%+.3f netR=%+.3f\n",
                    safety, netL, netR);
        return std::make_pair(netL, netR);
    };
    const auto dOn = dynSolo(1.0f);
    const auto dOff = dynSolo(0.0f);

    // 2) FusedTransducer aislado, emfEnabled=true (default vivo).
    {
        FusedTransducer tr;
        tr.prepare(kFs);
        TransducerControls c;
        c.correction = 1.0f;
        c.temporalAmount = 0.0f;
        c.emfEnabled = true;
        tr.setControls(c);
        std::vector<double> l(kFrames), r(kFrames);
        fillToneDouble(l.data(), kFrames, kFs, kF, kAmp);
        fillToneDouble(r.data(), kFrames, kFs, kF, kAmp);
        tr.process(l.data(), r.data(), static_cast<int>(kFrames));
        const double netL = rmsDb(l.data() + kSkipF, kFrames - kSkipF) - kInRmsDb;
        const double netR = rmsDb(r.data() + kSkipF, kFrames - kSkipF) - kInRmsDb;
        std::printf("[11K5D] transducer aislado emf=true sin kernel: "
                    "netL=%+.3f netR=%+.3f\n", netL, netR);
        EXPECT_NEAR(netL, 0.0, 0.5);
        EXPECT_NEAR(netR, 0.0, 0.5);
    }

    // 3) Cadena, VOLTERRA solo y NEURO solo, con safetyOff.
    ChainToggles noSafety; noSafety.safetyOff = true;
    auto chainSolo = [&](std::size_t op, const ChainToggles& tg) {
        FusedControls c;
        zero7(c);
        setOp(c, op, 1.0f);
        c.clamp();
        const ToneResult t = runTone(c, kF, kAmp, tg);
        std::printf("[11K5D] cadena solo %-8s safetyOff=%d: netL=%+.3f netR=%+.3f\n",
                    kOpName[op], tg.safetyOff ? 1 : 0, t.netL, t.netR);
        return std::make_pair(t.netL, t.netR);
    };
    const auto vNoSafety = chainSolo(3, noSafety);
    const auto nNoSafety = chainSolo(6, noSafety);
    const auto vSafety = chainSolo(3, ChainToggles{});

    // Veredicto por medición (las expectativas se fijan con los números
    // de esta bisección, no antes): si dynamics aislado reproduce la
    // pérdida, la etapa es FusedDynamics; si no, vive en la cadena
    // (pushEffectiveControls / conservación / D10 / wet).
    EXPECT_NEAR(dOff.first, 0.0, 0.5);
    EXPECT_NEAR(dOff.second, 0.0, 0.5);
    std::printf("[11K5D] resumen: dynamics safety=1 L=%+.3f R=%+.3f | "
                "cadena VOLT safetyOff L=%+.3f R=%+.3f | "
                "cadena VOLT safetyOn L=%+.3f R=%+.3f | "
                "cadena NEURO safetyOff L=%+.3f R=%+.3f\n",
                dOn.first, dOn.second, vNoSafety.first, vNoSafety.second,
                vSafety.first, vSafety.second, nNoSafety.first, nNoSafety.second);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3d) INTERACCIÓN 11.5 kHz — ¿el guardián AntiDolby? (modo MANUAL)
// ═══════════════════════════════════════════════════════════════════════════

// VEREDICTO (corrida 8): con zero7() limpio el solo es 0.000/0.000 en
// AUTO y en MANUAL; el guardián nunca fue la causa (MANUAL = AUTO
// también con el zero7 contaminado, porque la HRTF corría en ambos).
// Corrida 4: dynamics aislado 0.000/0.000, transductor aislado
// 0.000/0.000, cadena VOLTERRA solo con safetyOff SIGUE −13.173/−44.872.
// La pérdida vive en la cadena, fuera de los operadores. Candidato que
// queda: pushEffectiveControls() — el guardián AntiDolby modula el
// ancho de PULSO (×1.35 si dispara MUSIC), los agudos de ARMONÍA y el
// spread de ORBE, y puede sacar operadores del neutro. En modo MANUAL
// la intensidad del guardián es 0 (sin modulación). Si en MANUAL el
// solo vuelve a 0.000/0.000, el guardián es la causa.
TEST(EvolGenezi, Interaction11500GuardianManual) {
    constexpr double kF = 11500.0;
    auto soloManual = [&](std::size_t op, int brainMode) {
        FusedControls c;
        zero7(c);
        setOp(c, op, 1.0f);
        c.brain.mode = brainMode;  // 0 = AUTO, 1 = MANUAL (guardián off)
        c.clamp();
        const ToneResult t = runTone(c, kF);
        std::printf("[11K5E] solo %-8s brain.mode=%d: netL=%+.3f netR=%+.3f "
                    "media=%+.3f\n",
                    kOpName[op], brainMode, t.netL, t.netR,
                    meanLr(t.netL, t.netR));
        return std::make_pair(t.netL, t.netR);
    };
    const auto vAuto = soloManual(3, 0);
    const auto vManual = soloManual(3, 1);
    const auto nManual = soloManual(6, 1);
    const auto oManual = soloManual(0, 1);
    // En MANUAL (guardián sin modular) el solo debe ser neutro: el
    // transductor sin kernel es identidad y nada más procesa.
    EXPECT_NEAR(vManual.first, 0.0, 1.0);
    EXPECT_NEAR(vManual.second, 0.0, 1.0);
    EXPECT_NEAR(nManual.first, 0.0, 1.0);
    EXPECT_NEAR(nManual.second, 0.0, 1.0);
    std::printf("[11K5E] VOLT AUTO L=%+.3f R=%+.3f | VOLT MANUAL L=%+.3f R=%+.3f | "
                "NEURO MANUAL L=%+.3f R=%+.3f | ORBE MANUAL L=%+.3f R=%+.3f\n",
                vAuto.first, vAuto.second, vManual.first, vManual.second,
                nManual.first, nManual.second, oManual.first, oManual.second);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3f) INTERACCIÓN 11.5 kHz — control: cadena neutra y solo a 1 kHz
// ═══════════════════════════════════════════════════════════════════════════

// VEREDICTO (corrida 8, Interaction11500HrtfGate): con zero7() limpio
// la cadena neutra es 0.000/0.000 a 1 kHz y a 11.5 kHz (bit-exacta) y el
// VOLTERRA solo es 0.000/0.000 en toda la banda. La "pérdida" de la
// corrida 6 era la HRTF de reserva (hrtfWet=1.0).
// Corrida 6: la cadena "neutra" pierde +2.886/−1.366 dB a 1 kHz y
// −13.173/−44.872 dB a 11.5 kHz — NO es bit-exacta. Sospecha: zero7()
// no pone a cero los defaults que sacan del neutro (phiAmount=0.5,
// safety=1.0 y, sobre todo, hrtfWet=1.0 → etapa HRTF de reserva).
TEST(EvolGenezi, Interaction11500NeutralControl) {
    // (a) Cadena totalmente neutra a 11.5 kHz y a 1 kHz.
    for (double f : {11500.0, 1000.0}) {
        FusedControls c;
        zero7(c);
        c.clamp();
        const ToneResult t = runTone(c, f);
        std::printf("[11K5F] cadena NEUTRA f=%.0f: netL=%+.3f netR=%+.3f\n",
                    f, t.netL, t.netR);
        EXPECT_NEAR(t.netL, 0.0, 0.5);
        EXPECT_NEAR(t.netR, 0.0, 0.5);
    }
    // (b) VOLTERRA solo a 1 kHz (MANUAL: cerebro neutro) y a 11.5 kHz.
    for (double f : {1000.0, 3300.0, 6000.0, 11500.0}) {
        FusedControls c;
        zero7(c);
        setOp(c, 3, 1.0f);
        c.brain.mode = 1;  // MANUAL
        c.clamp();
        const ToneResult t = runTone(c, f);
        std::printf("[11K5F] VOLTERRA solo MANUAL f=%.0f: netL=%+.3f netR=%+.3f\n",
                    f, t.netL, t.netR);
    }
    // (c) ORBE solo a 1 kHz y 11.5 kHz (control: el único solo sin pérdida).
    for (double f : {1000.0, 11500.0}) {
        FusedControls c;
        zero7(c);
        setOp(c, 0, 1.0f);
        c.brain.mode = 1;
        c.clamp();
        const ToneResult t = runTone(c, f);
        std::printf("[11K5F] ORBE solo MANUAL f=%.0f: netL=%+.3f netR=%+.3f\n",
                    f, t.netL, t.netR);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 3g) INTERACCIÓN 11.5 kHz — ¿phiAmount default 0.5? (puerta Φ)
// ═══════════════════════════════════════════════════════════════════════════

// VEREDICTO (corrida 9, zero7 limpio): Φ (phiAmount=0.5) aporta
// −0.429/−0.429 dB a 11.5 kHz y safety=1.0 aporta 0.000/0.000. Ni Φ ni
// safety explican la pérdida: era la HRTF (hrtfWet=1.0, ver 3h).
// Corrida 6 (zero7 contaminado): la cadena "neutra" perdía
// +2.886/−1.366 dB a 1 kHz y −13.173/−44.872 dB a 11.5 kHz — NO era
// bit-exacta. Sospecha entonces: zero7() no ponía a cero los defaults
// que sacan del neutro:
//   * FieldControls.phiAmount = 0.5 por defecto; isNeutral() exige
//     amount<=0 Y phiAmount<=0 → FusedField corre holos+omni+phi en
//     TODOS los "solos" (amount=0, phi=0.5; el push lo baja a 0.25 por
//     la curva Harman default de ARMONÍA).
//   * DynamicsControls.safety = 1.0 por defecto; isNeutral() exige
//     safety<=0 → la cadena nunca es neutra (APEX engancha, techos
//     corren, árbitro D10 corre).
// Este test aísla Φ y safety por separado, sin tocar zero7().
TEST(EvolGenezi, Interaction11500PhiGate) {
    constexpr double kF = 11500.0;
    auto voltSolo = [&](float phi, float safety) {
        FusedControls c;
        zero7(c);
        setOp(c, 3, 1.0f);
        c.field.phiAmount = phi;
        c.dynamics.safety = safety;
        c.brain.mode = 1;  // MANUAL
        c.clamp();
        const ToneResult t = runTone(c, kF);
        std::printf("[11K5G] VOLT solo phi=%.2f safety=%.1f: netL=%+.3f netR=%+.3f\n",
                    phi, safety, t.netL, t.netR);
        return std::make_pair(t.netL, t.netR);
    };
    const auto bothOn = voltSolo(0.5f, 1.0f);   // Φ + safety explícitos
    const auto phiOnly = voltSolo(0.5f, 0.0f);  // solo Φ
    const auto safetyOnly = voltSolo(0.0f, 1.0f);  // solo safety
    const auto bothOff = voltSolo(0.0f, 0.0f);  // verdaderamente neutro
    // Cadena totalmente neutra (phi=0, safety=0, todo a 0).
    {
        FusedControls c;
        zero7(c);
        c.field.phiAmount = 0.0f;
        c.dynamics.safety = 0.0f;
        c.clamp();
        const ToneResult t = runTone(c, kF);
        std::printf("[11K5G] cadena NEUTRA phi=0 safety=0: netL=%+.3f netR=%+.3f\n",
                    t.netL, t.netR);
        EXPECT_NEAR(t.netL, 0.0, 0.5);
        EXPECT_NEAR(t.netR, 0.0, 0.5);
    }
    // Con Φ y safety a 0, el solo del transductor (identidad sin kernel)
    // debe ser 0.000/0.000: es el veredicto de esta bisección.
    EXPECT_NEAR(bothOff.first, 0.0, 0.5);
    EXPECT_NEAR(bothOff.second, 0.0, 0.5);
    std::printf("[11K5G] resumen: phi+safety L=%+.3f R=%+.3f | phi solo L=%+.3f R=%+.3f | "
                "safety solo L=%+.3f R=%+.3f | ambos off L=%+.3f R=%+.3f\n",
                bothOn.first, bothOn.second, phiOnly.first, phiOnly.second,
                safetyOnly.first, safetyOnly.second, bothOff.first, bothOff.second);
}

// ═══════════════════════════════════════════════════════════════════════════
// 3h) INTERACCIÓN 11.5 kHz — VEREDICTO: la etapa HRTF de reserva
// ═══════════════════════════════════════════════════════════════════════════

// Corrida 7 (zero7 contaminado): Φ aportaba ~0.2/1.2 dB de diferencia
// y la pérdida −12.935/−43.715 persistía con todo a cero. Último
// default sin poner a cero:
// SpatialControls.hrtfWet = 1.0 por defecto; isNeutral() exige
// hrtfWet<=0 → el espacial NUNCA es neutro en los solos. Y
// selectBinauralStage() elige HRTF cuando effectiveOrbeAmount()==0
// y hrtfWet>0: con amount=0 corre processBinauralScene() (síntesis
// analítica en host: no hay .ihr1) a wet 1.0. ORBE solo escapa porque
// con amount=1.0 la etapa elegida es Orbe (prioridad sobre HRTF).
// Este test pone hrtfWet=0: el solo y la cadena neutra deben ser
// 0.000/0.000, y el LOO "sin ORBE" debe dejar de fabricar la
// interacción de −24 dB.
TEST(EvolGenezi, Interaction11500HrtfGate) {
    constexpr double kF = 11500.0;
    auto clean = [&](FusedControls c) {
        c.field.phiAmount = 0.0f;
        c.dynamics.safety = 0.0f;
        c.spatial.hrtfWet = 0.0f;
        c.brain.mode = 1;
        c.clamp();
        return c;
    };
    // (a) Cadena totalmente neutra, limpia.
    {
        FusedControls c;
        zero7(c);
        const ToneResult t = runTone(clean(c), kF);
        std::printf("[11K5H] cadena NEUTRA limpia: netL=%+.3f netR=%+.3f\n",
                    t.netL, t.netR);
        EXPECT_NEAR(t.netL, 0.0, 0.5);
        EXPECT_NEAR(t.netR, 0.0, 0.5);
    }
    // (b) VOLTERRA solo, limpio: el transductor sin kernel es identidad.
    {
        FusedControls c;
        zero7(c);
        setOp(c, 3, 1.0f);
        const ToneResult t = runTone(clean(c), kF);
        std::printf("[11K5H] VOLT solo limpio: netL=%+.3f netR=%+.3f\n",
                    t.netL, t.netR);
        EXPECT_NEAR(t.netL, 0.0, 0.5);
        EXPECT_NEAR(t.netR, 0.0, 0.5);
    }
    // (c) NEURO solo, limpio (MANUAL: cerebro neutro → también 0).
    {
        FusedControls c;
        zero7(c);
        setOp(c, 6, 1.0f);
        const ToneResult t = runTone(clean(c), kF);
        std::printf("[11K5H] NEURO solo limpio MANUAL: netL=%+.3f netR=%+.3f\n",
                    t.netL, t.netR);
        EXPECT_NEAR(t.netL, 0.0, 0.5);
        EXPECT_NEAR(t.netR, 0.0, 0.5);
    }
    // (d) LOO "sin ORBE" limpio vs ALL7: la interacción debe colapsar.
    // ALL7 (con HRTF en 0: ORBE amount=1 → etapa Orbe, igual que antes).
    FusedControls all7 = all7Controls();
    all7.spatial.hrtfWet = 0.0f;
    all7.field.phiAmount = 0.0f;
    all7.dynamics.safety = 0.0f;
    all7.brain.mode = 1;
    all7.clamp();
    const ToneResult tAll7 = runTone(all7, kF);
    FusedControls noOrbe = all7;
    noOrbe.spatial.amount = 0.0f;
    noOrbe.clamp();
    const ToneResult tNoOrbe = runTone(noOrbe, kF);
    const double margOrbe = meanLr(tAll7.netL, tAll7.netR) -
                            meanLr(tNoOrbe.netL, tNoOrbe.netR);
    std::printf("[11K5H] ALL7 limpio media=%+.3f | sin ORBE limpio media=%+.3f | "
                "marginal ORBE=%+.3f (antes +21.718 con HRTF)\n",
                meanLr(tAll7.netL, tAll7.netR),
                meanLr(tNoOrbe.netL, tNoOrbe.netR), margOrbe);
    // (e) El HRTF de reserva, aislado: amount=0, hrtfWet=1.0 (el config
    // contaminado) — documenta su respuesta a 11.5 kHz. Con la puerta
    // HRTF (phone-loop) ese config hace bypass, así que se llega a la
    // etapa por la excepción de tracking (headTracking=true, sin pose:
    // la pose por defecto es 0 y la respuesta es la misma).
    {
        FusedControls c;
        zero7(c);
        c.field.phiAmount = 0.0f;
        c.dynamics.safety = 0.0f;
        c.spatial.hrtfWet = 1.0f;  // default vivo
        c.spatial.headTracking = true;  // puerta HRTF: excepción tracking
        c.brain.mode = 1;
        c.clamp();
        const ToneResult t = runTone(c, kF);
        std::printf("[11K5H] HRTF reserva aislado (amount=0, wet=1.0): "
                    "netL=%+.3f netR=%+.3f\n", t.netL, t.netR);
        EXPECT_NEAR(t.netL, -12.935, 0.5);
        // Tras el fix HRTF la reserva es simétrica: R era −43.715 dB
        // por la reconstrucción PCA monoaural rota; ahora es −12.935 dB.
        EXPECT_NEAR(t.netR, -12.935, 0.5);
    }
}
