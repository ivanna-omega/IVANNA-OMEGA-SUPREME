// test_double_floor.cpp — PISO DOUBLE DE LA CADENA ENGANCHADA (labor grande, v2.5.486).
//
// La meta THD+N −200 dB exige double INTERNO (no solo en las fronteras).
// El test de neutro (FusedChain.NeutralThd, ≤ −200 dB o bitExact) NO la
// prueba: en neutro la cadena es transparente y no hay matemática. Y el
// ThdAtEngaged (−5.49/−6.48 dB) mide CARÁCTER intencional (EQ, espacial,
// Phi, compresor), no precisión numérica.
//
// Este archivo mide el piso NUMÉRICO con la cadena ENGANCHADA pero con
// transferencia LINEAL a 1 kHz: los operadores lineales corren su
// matemática double y su transferencia a 1 kHz es lineal — un ajuste por
// mínimos cuadrados de la fundamental (measureThdDouble, piso del medidor
// ~−310 dB) absorbe ganancia y fase, así que el residuo es SOLO error
// numérico + distorsión no lineal. Por eso los operadores NO lineales
// quedan fuera o en unidad:
//   - VOLTERRA·Ω (H2) en 0: su corrección crea armónicos reales (carácter).
//   - NEURO·Ω en MANUAL: el NPE es no lineal (tanh, armónicos).
//   - PULSO con ratio 1.0 y safety 0: sin compresión ni limitador;
//     los crossovers LR4 y el ensanche M/S son lineales y SÍ corren.
// La precisión double de los no lineales la cubren sus propios tests
// (VOLTERRA·Ω 73, NPE 5 + closed-loop 18 + fused_brain 10).
//
// HALLAZGO MEDIDO (2026-11-20, bisección + espectro, ver BisectLinearEngaged):
// el CAMPO (HOLOS·Ω/Phi/OmniField) NO puede entrar en un test de piso con
// tono puro, porque su carácter DOMINA el residuo — no es ruido numérico:
//   - OmniField solo, amount 0.5, convergence=true (default): −19.4 dB.
//     El espectro mostraba un TONO EMERGENTE real a 884 Hz (−17.6 dB) junto
//     al fundamental de 1 kHz: era el tono emergente de la convergencia
//     Kuramoto (OmniParams.convergence), una FUNCIÓN del campo, no un
//     error numérico. Con convergence=false mejoraba a −75.7 dB (el resto es
//     el render de stems sobre tono puro: carácter, no piso).
//     V1 EN VIVO (2026-11-21) confirmó que ese tono es el "piiiii" que se
//     escucha en audífonos (muere al pausar = Kuramoto, no auto-oscilación)
//     y la v2.5.488 MATÓ el 5º stem (kEmergentToneEnabled=false,
//     OmniField.hpp:442): la convergencia Kuramoto SIGUE (r>0.9, bloqueo)
//     pero la línea de 884 Hz desaparece, así que OmniField solo con
//     convergence=true ahora mide limpio (~−75 dB, como convergence=false).
//     Ver OmniFieldEmergentToneKilled.
//   - HOLOS solo (phi 0): −17.6 dB — es OmniField en serie dentro de
//     FusedField (op.amount = controls_.amount), no HolosOperator:
//     HolosOperator directo en su test mide −94.0 dB (retune prod).
//   - Phi solo (HOLOS 0): −123.4 dB (su bucle cerrado modula: carácter).
// Por eso la configuración "lineal enganchada" lleva el campo en 0
// (amount 0 + phi 0 → FusedField en bypass bit-exacto) y el test mide
// ORBE + SALA + ARMONÍA + PULSO-lineal (+ TΩ en SPEAKERS).
//
// PISO MEDIDO de la cadena lineal enganchada (1 kHz, −3 dBFS, 1 s en
// régimen permanente, double de punta a punta):
//   - ORBE + SALA + ARMONÍA: −180.80 dB (ambos canales idénticos).
//   - SALA + ARMONÍA solos: −297.35 dB (piso del medidor: la matemática
//     double de la convolución RIR y del EQ es numéricamente perfecta).
//   - ORBE es quien fijaba el piso en −181 dB antes de SDRF: su convolución
//     HRTF corre en double, pero los datos HRTF/PCA que alimenta son
//     float32 — el piso es el de los DATOS, no el de la aritmética. 40 dB
//     mejor que el piso float32 (~−140 dB), pero la meta −200 dB NO se
//     cumple en la cadena con ORBE activo. Umbral del test: −170 dB
//     (10 dB de margen sobre lo medido). Subir el piso a −200 dB
//     exigiría HRTF en double (datos + interpolación), trabajo futuro,
//     no de este commit.
//   - DESDE v2.5.500 (SDRF dentro de SALA VIVA) el piso lo fija el campo
//     difuso, no ORBE: la red SDRF es variable en el tiempo y no lineal
//     (AURA 0.07 Hz, Kerr dependiente de energía), su THD+N a 1 kHz es
//     −76.66 dB en el campo solo y en la cadena (wet 0.22) da −91.34 dB
//     (peor canal). El umbral del test pasa a −80 dB (~11 dB de margen):
//     es carácter del reverb vivo, no error numérico. Ver el comentario
//     del test ChainLinearEngagedHeadphones (v2.5.501).
//   - TΩ en SPEAKERS (kernel LTI D10, fix-2 #1372/#1426): con el tono
//     re-apuntado a 0.3 (−10.46 dBFS) el piso es −95.42 dB (peor canal,
//     medido por el test; la sonda da −96.17 dB; a 0.1 la sonda da
//     −96.43 dB). A 0.7071 el test medía el TECHO (pico en 0.9886,
//     −26.94 dB), no el piso. La modulación de frame de la TΩ
//     espectral (−40.04 dB, bandas laterales a ±375 Hz) ya no existe:
//     el kernel LTI la mata (bandas −152 dB). Umbral −85 dB
//     (caracterización, ~10 dB de margen). Con TΩ apagado, SPEAKERS
//     mide −296.74 dB (piso del medidor). La modulación de frame ya
//     está reducida por el kernel LTI D10 (ver arriba).
//
// CPU: coste por HOP (128 muestras @ 48 kHz = 2667 µs de audio real),
// cadena lineal enganchada, mediana de 2000 hops tras warm-up.
// Medido en esta VM compartida (1.5 CPU): p50 ~200 µs/hop en régimen
// (primera corrida en frío 886 µs) — 13x de margen sobre el presupuesto
// realtime (el assert es el presupuesto).
// Mismo patrón que FusedChain.TOmegaDspUs (p50; media/p95/max se inflan
// por stalls de planificación en host compartido).
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "../fused/fused_chain.hpp"
#include "../fused/fused_measure.hpp"
#include "../spatial/OmniField.hpp"

using namespace ivanna::fused;
using namespace ivanna::fused::measure;

namespace {
constexpr double kFs = 48000.0;
constexpr double kFreq = 1000.0;
constexpr double kAmp = 0.7071;  // −3 dBFS

// Controles "lineal enganchada": matemática double activa, transferencia
// lineal a 1 kHz (ver cabecera). Campo en 0: su tono emergente dominaría
// el residuo con carácter (−17 dB), no con ruido numérico.
FusedControls linearEngagedControls() {
    FusedControls c;
    c.spatial.amount = 1.0f;
    c.spatial.azimuthDeg = 30.0f;
    c.spatial.distanceM = 1.0f;
    c.spatial.hrtfWet = 0.5f;
    c.spatial.mode = ivanna::OmegaOutputMode::HEADPHONES;
    c.field.amount = 0.0f;        // campo FUERA del test de piso (ver cabecera)
    c.field.omniAmount = 0.0f;  // Unificación 2.5.509 (paso 5): campo FUERA = también OmniField en 0.
    c.field.phiAmount = 0.0f;
    c.field.harmonics = 0.0f;
    c.listenerRoom.roomWet = 0.22f;
    c.transducer.correction = 0.0f;   // Volterra H2 fuera (no lineal)
    c.transducer.temporalAmount = 0.0f;
    c.tonal.curve = 1;                // Harman (lineal)
    c.tonal.lowDb = 0.0f;
    c.tonal.midDb = 0.0f;
    c.tonal.highDb = 0.0f;
    c.tonal.trimDb = 0.0f;
    c.dynamics.compRatio = 1.0f;      // sin compresión
    c.dynamics.compThresholdDb = -12.0f;
    c.dynamics.safety = 0.0f;         // limitador fuera
    c.dynamics.exciter = 0.0f;
    c.dynamics.punch = 0.0f;
    c.dynamics.width = 1.0f;
    c.dynamics.voice = 0.0f;
    c.dynamics.upmixImmersivity = 0.0f;
    c.brain.mode = 1;                 // MANUAL: NPE/bucle en bypass
    c.brain.adaptation = 0.0f;
    c.clamp();
    return c;
}

double measureFloor(const FusedControls& c, bool volumetric, const char* tag, double amp = kAmp) {
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(c);
    chain.setVolumetricEnabled(volumetric);
    const std::size_t n = 96000;      // 2 s
    const std::size_t skip = 48000;   // 1 s de transitorio (colas RIR)
    std::vector<double> l(n), r(n);
    fillToneDouble(l.data(), n, kFs, kFreq, amp);
    fillToneDouble(r.data(), n, kFs, kFreq, amp);
    chain.process(l.data(), r.data(), static_cast<int>(n));
    EXPECT_TRUE(isFiniteBuffer(l.data(), n));
    EXPECT_TRUE(isFiniteBuffer(r.data(), n));
    const ThdDoubleResult thdL = measureThdDouble(l.data() + skip, n - skip, kFs, kFreq);
    const ThdDoubleResult thdR = measureThdDouble(r.data() + skip, n - skip, kFs, kFreq);
    EXPECT_TRUE(thdL.valid);
    EXPECT_TRUE(thdR.valid);
    std::printf("[DoubleFloor] %s: L=%.2f dB R=%.2f dB (1 kHz, %zu muestras, skip %zu)\n",
                tag, thdL.thdPlusNoiseDb, thdR.thdPlusNoiseDb, n - skip, skip);
    return std::max(thdL.thdPlusNoiseDb, thdR.thdPlusNoiseDb);
}
}  // namespace

TEST(DoubleFloor, ChainLinearEngagedHeadphones) {
    // HEADPHONES: TΩ salta (M = I). Mide ORBE + SALA + ARMONÍA +
    // PULSO-lineal en double. Umbral −80 dB (medido −91.34 dB peor
    // canal, −94.95 dB R; ~11 dB de margen).
    //
    // POR QUÉ YA NO ES −170 dB (v2.5.501, decisión de Esse): el umbral
    // −170 dB era un piso de cadena LINEAL, fijado cuando SALA VIVA
    // sin IR era bypass (el piso lo marcaba ORBE en −180.80 dB, por
    // sus datos HRTF float32). Desde la v2.5.500 el campo difuso SDRF
    // vive DENTRO de SALA VIVA (FusedListenerRoom, roomWet 0.22 aquí),
    // y ese campo es una red VARIABLE EN EL TIEMPO Y NO LINEAL (AURA
    // 0.07 Hz, Kerr dependiente de energía, ángulos por envolvente):
    // su THD+N a 1 kHz es −76.66 dB en el campo solo, que en la cadena
    // da los −91.34 dB medidos. Eso es CARÁCTER del reverb (su campo
    // difuso vivo), no error numérico: la matemática double sigue
    // siendo exacta (SALA+ARMONÍA solos miden −297.35 dB, piso del
    // medidor). Un reverb variable en el tiempo no puede dar −170 dB
    // sin destripar su carácter (quitar Kerr/AURA). El umbral −80 dB
    // fija que el campo no se degrade más, sin venderlo como piso de
    // precisión numérica — mismo patrón que SPEAKERS/TΩ (−30 dB).
    const double floor = measureFloor(linearEngagedControls(), true, "HEADPHONES lineal enganchada");
    EXPECT_LT(floor, -80.0) << "Piso double enganchado peor que -80 dB (peor canal " << floor << " dB)";
}

TEST(DoubleFloor, ChainLinearEngagedSpeakers) {
    // SPEAKERS: añade la etapa espectral TΩ (XTC lineal).
    //
    // RE-APUNTADO A 0.3 (fix-2, 2026-11-25, #1426): con el tono a 0.7071
    // (−3 dBFS) el test medía el TECHO, no el piso — la cadena sube el
    // tono +3.5 dB (fundamental a +0.51 dBFS) y el pico se clava en el
    // techo 0.9886 (−0.10 dBFS), dando −26.94 dB. La sonda
    // probe-speakers-floor.cpp lo verifica: a 0.7071 el pico es 0.9886
    // (techo); a 0.3 el pico es 0.4890 (sin clipping) y a 0.1 es 0.1810.
    // El tono a 0.3 (−10.46 dBFS) mide el piso real.
    //
    // PISO MEDIDO con la FIR nueva del kernel LTI (512 taps + shift 256,
    // fix-2 #1372), g++-14 Release: el TEST mide a 0.3 L=−95.42 dB,
    // R=−101.86 dB (peor canal −95.42 dB, determinista en 3 corridas);
    // la sonda probe-speakers-floor.cpp (misma configuración) mide a
    // 0.3 L=−96.17 dB, R=−102.07 dB y a 0.1 L=−96.43 dB, R=−103.02 dB
    // (el piso NO depende del nivel: es piso, no techo; la diferencia
    // sonda/test de 0.75 dB es estado global del proceso — la sonda
    // corre 3 cadenas seguidas y el test una sola — irrelevante frente
    // al margen de ~10 dB).
    // La FIR nueva NO mueve el piso (idéntico a 0.01 dB con la FIR de
    // 128 taps): lo fija el resto de la cadena (SALA VIVA SDRF + ORBE;
    // compárese HEADPHONES −91.34 dB), no la TΩ. La modulación de frame
    // de la TΩ espectral (−40.04 dB, bandas laterales a ±375 Hz) ya no
    // existe: el kernel LTI D10 la mata (bandas −152 dB, ver D10Corona).
    // Umbral −85 dB (~10 dB de margen sobre lo medido): fija que la
    // etapa no se degrade más, sin venderla como piso de precisión.
    FusedControls c = linearEngagedControls();
    c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
    c.clamp();
    const double floor = measureFloor(c, true, "SPEAKERS lineal enganchada (TΩ XTC)", 0.3);
    EXPECT_LT(floor, -85.0) << "Piso TΩ peor que -85 dB (peor canal " << floor << " dB)";
}

TEST(DoubleFloor, BisectLinearEngaged) {
    // DOCUMENTACIÓN VIVA del piso (sin assert de umbral): qué operador
    // domina el residuo y por qué el campo queda fuera del test de piso.
    // Valores medidos 2026-11-20 en host (g++-14, Release).
    FusedControls base = linearEngagedControls();
    // Base del test (campo en 0): el piso de la cadena lineal.
    measureFloor(base, true, "BISECT lineal (campo 0)");
    // Con el campo enganchado (la configuración ORIGINAL del test, antes
    // del hallazgo): el tono emergente domina.
    {
        FusedControls c = base;
        c.field.amount = 0.5f; c.field.omniAmount = 0.5f; c.field.phiAmount = 0.5f;  // Unif. 2.5.509 paso 5
        measureFloor(c, true, "BISECT con campo 0.5/0.5 (tono emergente)");
    }
    {
        FusedControls c = base;
        c.field.amount = 0.5f; c.field.omniAmount = 0.5f; c.field.phiAmount = 0.0f;  // Unif. 2.5.509 paso 5
        measureFloor(c, true, "BISECT HOLOS solo (phi 0)");
    }
    {
        FusedControls c = base;
        c.field.amount = 0.0f; c.field.phiAmount = 0.5f;
        measureFloor(c, true, "BISECT Phi solo (HOLOS 0)");
    }
    // Sin campo, un operador cada vez: quién fija el piso −180.8 dB.
    {
        FusedControls c = base;
        c.spatial.amount = 0.0f; c.spatial.hrtfWet = 0.0f;
        measureFloor(c, true, "BISECT sin ORBE (SALA+ARMONIA)");
    }
    {
        FusedControls c = base;
        c.listenerRoom.roomWet = 0.0f;
        measureFloor(c, true, "BISECT sin SALA (ORBE+ARMONIA)");
    }
    {
        FusedControls c = base;
        c.tonal.curve = 0;
        measureFloor(c, true, "BISECT sin ARMONIA (ORBE+SALA)");
    }
    // SPEAKERS con TΩ apagado: ¿el residuo −40 dB es TΩ o el render
    // SPEAKERS del espacial? (volumetric=false apaga la etapa TΩ.)
    {
        FusedControls c = base;
        c.spatial.mode = ivanna::OmegaOutputMode::SPEAKERS;
        c.clamp();
        measureFloor(c, false, "BISECT SPEAKERS TΩ off");
    }
}

TEST(DoubleFloor, OmniFieldEmergentToneKilled) {
    // El tono emergente de OmniField (5º stem, 884 Hz) está MUERTO desde
    // v2.5.488 (kEmergentToneEnabled=false, OmniField.hpp): V1 en vivo
    // (2026-11-21) confirmó que es el "piiiii" (muere al pausar =
    // Kuramoto, no auto-oscilación) y que regresa en audífonos. Antes era
    // CARÁCTER (convergence=true: −19.4 dB, tono a 884 Hz a −17.6 dB);
    // ahora la convergencia Kuramoto SIGUE (r>0.9, bloqueo) pero la línea
    // de 884 Hz desaparece: el residuo con convergence=true debe ser
    // limpio como con convergence=false (medido −75.7 dB). Si el tono
    // vuelve o la convergencia se rompe, este test lo caza.
    for (bool conv : {true, false}) {
        ivanna::omnifield::OmniField omni;
        omni.prepare(48000.0f);
        ivanna::omnifield::OmniParams op;
        op.amount = 0.5f;
        op.headTracking = false;
        op.convergence = conv;
        omni.setParams(op);
        const std::size_t n = 96000;
        const std::size_t skip = 48000;
        std::vector<double> l(n), r(n);
        fillToneDouble(l.data(), n, kFs, kFreq, kAmp);
        fillToneDouble(r.data(), n, kFs, kFreq, kAmp);
        omni.process(l.data(), r.data(), static_cast<int>(n));
        const ThdDoubleResult thdL = measureThdDouble(l.data() + skip, n - skip, kFs, kFreq);
        const ThdDoubleResult thdR = measureThdDouble(r.data() + skip, n - skip, kFs, kFreq);
        EXPECT_TRUE(thdL.valid);
        EXPECT_TRUE(thdR.valid);
        const double floor = std::max(thdL.thdPlusNoiseDb, thdR.thdPlusNoiseDb);
        std::printf("[DoubleFloor] OmniField 5º stem muerto convergence=%d: L=%.2f dB R=%.2f dB r=%.3f locked=%d\n",
                    static_cast<int>(conv), thdL.thdPlusNoiseDb, thdR.thdPlusNoiseDb,
                    static_cast<double>(omni.convergenceR()),
                    static_cast<int>(omni.convergenceLocked()));
        // El tono a 884 Hz está muerto: el residuo es limpio en ambos casos.
        EXPECT_LT(floor, -60.0) << "5º stem vivo o residuo alto (piso " << floor << " dB, convergence=" << static_cast<int>(conv) << ")";
        if (conv) {
            // La convergencia Kuramoto SIGUE: r>0.9 y bloqueo robusto.
            // (Si alguien mata la convergencia entera en vez del 5º stem,
            // este assert lo caza.)
            EXPECT_GT(omni.convergenceR(), 0.9f) << "convergence=true sin bloqueo (r=" << omni.convergenceR() << ")";
            EXPECT_TRUE(omni.convergenceLocked()) << "convergence=true sin bloqueo robusto";
        }
    }
}

TEST(DoubleFloor, CpuPerHop) {
    // Coste por hop (128 muestras @ 48 kHz = 2667 µs de audio real),
    // cadena lineal enganchada, mediana de 2000 hops tras warm-up.
    // Mismo patrón que FusedChain.TOmegaDspUs (p50; media/p95/max se
    // inflan por stalls de planificación en host compartido).
    constexpr int kHop = 128;
    constexpr int kWarmup = 200;
    constexpr int kMeasured = 2000;
    const std::size_t n = 48000;
    std::vector<double> srcL(n), srcR(n);
    fillToneDouble(srcL.data(), n, kFs, kFreq, kAmp);
    fillToneDouble(srcR.data(), n, kFs, kFreq, kAmp);
    FusedChain chain;
    chain.prepare(kFs);
    chain.setControls(linearEngagedControls());
    chain.setVolumetricEnabled(true);
    std::vector<double> l = srcL, r = srcR;
    std::vector<double> us;
    us.reserve(static_cast<std::size_t>(kMeasured));
    std::size_t off = 0;
    for (int b = 0; b < kWarmup + kMeasured; ++b) {
        if (off + static_cast<std::size_t>(kHop) > n) off = 0;
        const auto t0 = std::chrono::steady_clock::now();
        chain.process(l.data() + off, r.data() + off, kHop);
        const auto t1 = std::chrono::steady_clock::now();
        if (b >= kWarmup) {
            us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
        off += static_cast<std::size_t>(kHop);
    }
    std::sort(us.begin(), us.end());
    const double mean = std::accumulate(us.begin(), us.end(), 0.0) / static_cast<double>(us.size());
    const double p50 = us[us.size() / 2];
    const double p95 = us[(us.size() * 95) / 100];
    const double mx = us.back();
    std::printf("[DoubleFloor] cpu/hop(%d): mean=%.1f p50=%.1f p95=%.1f max=%.1f us (presupuesto 2667 us)\n",
                kHop, mean, p50, p95, mx);
    EXPECT_LT(p50, 2667.0) << "mediana µs/hop supera el periodo realtime (2667 µs)";
}
