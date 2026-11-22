// universal_hearing_test.cpp — Tests host del Modelo Universal de Audición
// (P6, 2026-11-21).
//
// Standalone (propio main(), sin gtest), mismo patrón que
// acoustic_twin_test.cpp: se compila con g++-14 y se corre con ctest.
// Cubre lo que exige el encargo:
//   1. Determinismo bit a bit y ausencia de estado oculto.
//   2. El módulo no modifica la señal (no tiene camino de audio).
//   3. El trayecto real de Esse (Genezi, sala 4.0×7.0, 3 m) con valores
//      esperados calculados A MANO en los comentarios.
//   4. Monotonía en la dirección correcta de cada modelo.
//   5. Caso audífono (sin sala: profundidad e imagen ~0, causalidad
//      solo por buffer).
//   6. Fail-closed con NaN y con sala desconocida (valid=false,
//      confianza 0, salida finita).
//   7. Todas las salidas finitas y en [0,1].
//   8. Compatibilidad P[t]: el estado neutro y el orden de las 9 dims
//      coinciden con la semántica documentada del MPC.
//
// Los valores esperados del trayecto de Esse se calculan a mano en los
// comentarios (no se copian de la salida del programa): si el test pasa,
// el modelo coincide con el cálculo independiente.
#include "universal_hearing.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace ivanna::fused::universal_hearing;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const char* name) {
    ++g_checks;
    if (cond) {
        std::printf("  PASS %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL %s\n", name);
    }
}

bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

bool predictionsEqual(const UniversalHearingPrediction& a,
                      const UniversalHearingPrediction& b) {
    if (a.valid != b.valid) return false;
    if (a.confidence != b.confidence) return false;
    if (a.interventionScale != b.interventionScale) return false;
    if (a.imageShiftDegrees != b.imageShiftDegrees) return false;
    if (a.totalLatencyMs != b.totalLatencyMs) return false;
    if (a.effectiveCriticalDistanceM != b.effectiveCriticalDistanceM) return false;
    if (a.effectiveCoherence != b.effectiveCoherence) return false;
    if (a.schroederHz != b.schroederHz) return false;
    for (std::size_t d = 0; d < kHearingDims; ++d) {
        if (a.globalMean[d] != b.globalMean[d]) return false;
    }
    for (std::size_t i = 0; i < kHearingBands; ++i) {
        for (std::size_t d = 0; d < kHearingDims; ++d) {
            if (a.band[i].v[d] != b.band[i].v[d]) return false;
        }
    }
    return true;
}

// Todas las salidas finitas y en rango (nunca NaN/inf, nunca fuera de
// [0,1] donde el contrato lo exige). Vale para entradas válidas E
// inválidas.
bool allFiniteAndInRange(const UniversalHearingPrediction& p) {
    if (!std::isfinite(p.confidence) ||
        !std::isfinite(p.interventionScale) ||
        !std::isfinite(p.imageShiftDegrees) ||
        !std::isfinite(p.totalLatencyMs) ||
        !std::isfinite(p.effectiveCriticalDistanceM) ||
        !std::isfinite(p.effectiveCoherence) ||
        !std::isfinite(p.schroederHz)) {
        return false;
    }
    if (p.confidence < 0.0 || p.confidence > 1.0) return false;
    if (p.interventionScale < 0.0 || p.interventionScale > 1.0) return false;
    if (p.interventionScale != p.confidence) return false;
    if (p.imageShiftDegrees < 0.0) return false;
    if (p.totalLatencyMs < 0.0) return false;
    if (p.effectiveCoherence < 0.0 || p.effectiveCoherence > 1.0) return false;
    for (std::size_t d = 0; d < kHearingDims; ++d) {
        if (!std::isfinite(p.globalMean[d])) return false;
        if (p.globalMean[d] < 0.0 || p.globalMean[d] > 1.0) return false;
    }
    for (std::size_t i = 0; i < kHearingBands; ++i) {
        if (!p.band[i].isFinite()) return false;
        for (std::size_t d = 0; d < kHearingDims; ++d) {
            if (p.band[i].v[d] < 0.0 || p.band[i].v[d] > 1.0) return false;
        }
    }
    return true;
}

double meanDim(const UniversalHearingPrediction& p, Dim d) {
    return p.globalMean[static_cast<std::size_t>(d)];
}

}  // namespace

int main() {
    std::printf("== universal_hearing_test ==\n");

    // ── 1. Determinismo: misma entrada → misma salida ────────────────
    std::printf("[determinismo]\n");
    {
        const UniversalHearingInput in = UniversalHearingInput::esseGenezi();
        const UniversalHearingPrediction p1 =
            UniversalHearingModel::predict(in);
        const UniversalHearingPrediction p2 =
            UniversalHearingModel::predict(in);
        check(predictionsEqual(p1, p2), "misma entrada, salida identica");
        check(p1.valid, "prediccion valida");
        check(allFiniteAndInRange(p1), "salida finita y en rango");
    }

    // ── 2. Sin estado oculto: intercalar otra entrada no contamina ───
    std::printf("[sin estado oculto]\n");
    {
        const UniversalHearingInput a = UniversalHearingInput::esseGenezi();
        const UniversalHearingInput b = UniversalHearingInput::esseHeadphones();
        const UniversalHearingPrediction pa1 =
            UniversalHearingModel::predict(a);
        (void)UniversalHearingModel::predict(b);
        const UniversalHearingPrediction pa2 =
            UniversalHearingModel::predict(a);
        check(predictionsEqual(pa1, pa2), "re-predecir A tras B devuelve lo mismo");
    }

    // ── 3. No modifica la señal ──────────────────────────────────────
    std::printf("[no modifica la senal]\n");
    {
        float buffer[1024];
        for (int i = 0; i < 1024; ++i) {
            buffer[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * i / 64.0) +
                                           0.001 * (i % 7));
        }
        float before[1024];
        std::memcpy(before, buffer, sizeof(buffer));
        const UniversalHearingInput in = UniversalHearingInput::esseGenezi();
        (void)UniversalHearingModel::predict(in);
        (void)UniversalHearingModel::predict(
            UniversalHearingInput::esseHeadphones());
        check(std::memcmp(before, buffer, sizeof(buffer)) == 0,
              "el buffer de audio queda bit-identico tras predecir");
    }

    // ── 4. Trayecto real de Esse (Genezi, sala 4.0x7.0, 3 m) ──────────
    std::printf("[trayecto de Esse: Genezi]\n");
    {
        // Cálculo independiente (a mano):
        //   V = 70 m3; Dc = 0.057*sqrt(70/0.34) = 0.057*14.348 = 0.818 m
        //   coherencia = 1/(1+(3/0.818)^2) = 1/14.45 = 0.069
        //   Schroeder = 2000*sqrt(0.34/70) = 139.4 Hz
        //   latencia = 2.667 + 3/343*1000 = 2.667 + 8.746 = 11.41 ms
        //   RT60 por banda = 0.34/alpha: 0.523, 0.453, 0.400, 0.358,
        //     0.340, 0.324, 0.296, 0.272, 0.252 (media 0.358)
        //   profundidad media = 0.55*0.358 + 0.45*(1-0.069) = 0.62
        //     (1 kHz: 0.55*0.34 + 0.45*0.931 = 0.606)
        //   compresion espacial media = 0.40*0.358 + 0.35*0.931 = 0.47
        //   enmascaramiento 1 kHz = 0.40*0.34*0.5 + 0.25*0.70
        //     + 0.20*0.25 + 0 = 0.068+0.175+0.05 = 0.293 (media 0.30)
        //   inteligibilidad 1 kHz = 0.5*(0.34/0.8)*1.0 + 0.3*0.25
        //     + 0.2*0.931 = 0.2125+0.075+0.186 = 0.474 (media 0.40)
        //   microdinamica = 0.5*(1-12/20) + 0 + 0 = 0.20 (THD y piso
        //     desconocidos aportan 0)
        //   causalidad media = 0.6*(11.41/50) + 0.4*0.358 = 0.137+0.143
        //     = 0.28 (1 kHz: 0.137+0.136 = 0.273)
        //   fatiga = 0 (85 dB no supera el umbral) + 0.10*(30/240)
        //     = 0.0125 (solo el termino de sesion)
        //   confianza base = 1 - 0.10 (THD) - 0.05 (piso) - 0.05 (coh.
        //     derivada) - 0.05 (Dc derivada) = 0.75; bandas 63/125 Hz
        //     bajo Schroeder x0.85 -> 0.6375; media global = 0.725
        const UniversalHearingPrediction p = UniversalHearingModel::predict(
            UniversalHearingInput::esseGenezi());
        check(p.valid, "prediccion valida");
        check(near(p.effectiveCriticalDistanceM, 0.818, 0.02),
              "distancia critica derivada ~0.82 m");
        check(near(p.effectiveCoherence, 0.069, 0.01),
              "coherencia derivada ~0.069 (domina la reverberacion)");
        check(near(p.schroederHz, 139.4, 5.0), "Schroeder ~139 Hz");
        check(near(p.totalLatencyMs, 11.41, 0.1), "latencia total ~11.4 ms");
        check(near(meanDim(p, Dim::kDepthLoss), 0.62, 0.05),
              "perdida de profundidad media ~0.62");
        check(near(p.band[4][Dim::kDepthLoss], 0.606, 0.03),
              "perdida de profundidad 1 kHz ~0.61");
        check(near(meanDim(p, Dim::kSpatialCompression), 0.47, 0.05),
              "compresion espacial media ~0.47");
        check(near(p.band[4][Dim::kTimbralMasking], 0.293, 0.03),
              "enmascaramiento 1 kHz ~0.29");
        check(near(meanDim(p, Dim::kTimbralMasking), 0.30, 0.05),
              "enmascaramiento medio ~0.30");
        check(near(p.band[0][Dim::kTimbralMasking], 0.409, 0.03),
              "enmascaramiento 63 Hz ~0.41 (mas grave, mas mascara)");
        check(near(p.band[4][Dim::kIntelligibilityLoss], 0.474, 0.03),
              "perdida de inteligibilidad 1 kHz ~0.47");
        check(near(meanDim(p, Dim::kIntelligibilityLoss), 0.40, 0.05),
              "perdida de inteligibilidad media ~0.40");
        check(near(meanDim(p, Dim::kMicrodynamicsLoss), 0.20, 0.01),
              "perdida de microdinamica 0.20 (crest 12 dB)");
        check(near(meanDim(p, Dim::kCausalityLoss), 0.28, 0.05),
              "perdida de causalidad media ~0.28");
        check(near(p.band[4][Dim::kCausalityLoss], 0.273, 0.03),
              "perdida de causalidad 1 kHz ~0.27");
        check(near(meanDim(p, Dim::kFatigue), 0.0125, 0.01),
              "fatiga ~0.01 a 85 dB (solo termino de sesion)");
        check(p.imageShiftDegrees == 0.0, "desplazamiento de imagen 0 grados");
        check(meanDim(p, Dim::kImageShift) == 0.0,
              "desplazamiento de imagen normalizado 0");
        // Dims de P[t] derivadas a 1 kHz (a mano):
        //   audibilidad = 0.35+0.65*0.70-0.30*0.293 = 0.717
        //   claridad = 1-0.80*0.474-0.20*0.293 = 0.562
        //   naturalidad = 1-0-0.30*0.293 = 0.912
        //   impacto = 0.35*0.6+0.35*0.6+0.20*0.70+0.10*0.5 = 0.61
        //   espacialidad = 1-0.60*0.606-0.40*0.462 = 0.452
        //   estabilidad = 1-0-0.30*0.1 = 0.97; presupuesto 0.
        check(near(p.band[4][Dim::kAudibility], 0.717, 0.03),
              "audibilidad 1 kHz ~0.72");
        check(near(p.band[4][Dim::kClarity], 0.562, 0.03),
              "claridad 1 kHz ~0.56");
        check(near(p.band[4][Dim::kNaturalness], 0.912, 0.03),
              "naturalidad 1 kHz ~0.91");
        check(near(p.band[4][Dim::kImpact], 0.61, 0.03),
              "impacto 1 kHz ~0.61");
        check(near(p.band[4][Dim::kSpatiality], 0.452, 0.03),
              "espacialidad 1 kHz ~0.45");
        check(near(p.band[4][Dim::kStability], 0.97, 0.01),
              "estabilidad 1 kHz ~0.97");
        check(meanDim(p, Dim::kBudget) == 0.0,
              "presupuesto 0 (el modelo predice, no consume)");
        check(near(p.confidence, 0.725, 0.02),
              "confianza global ~0.73 (THD/piso sin medir, Dc/coh derivadas)");
        check(p.interventionScale == p.confidence,
              "interventionScale == confianza (a confianza baja, menos intervencion)");
        check(p.band[0][Dim::kConfidence] < p.band[4][Dim::kConfidence],
              "confianza menor bajo Schroeder (63 Hz modal)");
        check(allFiniteAndInRange(p), "salida finita y en rango");
    }

    // ── 5. Monotonía: más RT60 → más pérdida ─────────────────────────
    std::printf("[monotonia RT60]\n");
    {
        const UniversalHearingInput base = UniversalHearingInput::esseGenezi();
        UniversalHearingInput wet = base;
        wet.room.rt60Sec = 1.0;  // sala muy reverberante
        const UniversalHearingPrediction pb =
            UniversalHearingModel::predict(base);
        const UniversalHearingPrediction pw =
            UniversalHearingModel::predict(wet);
        check(pw.valid && pb.valid, "ambas predicciones validas");
        check(meanDim(pw, Dim::kDepthLoss) > meanDim(pb, Dim::kDepthLoss),
              "mas RT60 -> mas perdida de profundidad");
        check(meanDim(pw, Dim::kSpatialCompression) >
                  meanDim(pb, Dim::kSpatialCompression),
              "mas RT60 -> mas compresion espacial");
        check(meanDim(pw, Dim::kIntelligibilityLoss) >
                  meanDim(pb, Dim::kIntelligibilityLoss),
              "mas RT60 -> mas perdida de inteligibilidad");
        check(meanDim(pw, Dim::kCausalityLoss) >
                  meanDim(pb, Dim::kCausalityLoss),
              "mas RT60 -> mas perdida de causalidad");
        check(allFiniteAndInRange(pw), "salida finita y en rango con RT60 1 s");
    }

    // ── 6. Monotonía: más distancia → más pérdida ────────────────────
    std::printf("[monotonia distancia]\n");
    {
        const UniversalHearingInput base = UniversalHearingInput::esseGenezi();
        UniversalHearingInput far = base;
        far.listener.distanceM = 6.0;  // el doble de lejos
        const UniversalHearingPrediction pb =
            UniversalHearingModel::predict(base);
        const UniversalHearingPrediction pf =
            UniversalHearingModel::predict(far);
        // A mano: coherencia a 6 m = 1/(1+(6/0.818)^2) = 1/54.8 = 0.018
        // (antes 0.069); latencia aire 17.49 ms (antes 8.75 ms).
        check(near(pf.effectiveCoherence, 0.018, 0.01),
              "coherencia a 6 m ~0.018");
        check(meanDim(pf, Dim::kDepthLoss) > meanDim(pb, Dim::kDepthLoss),
              "mas distancia -> mas perdida de profundidad");
        check(meanDim(pf, Dim::kCausalityLoss) >
                  meanDim(pb, Dim::kCausalityLoss),
              "mas distancia -> mas perdida de causalidad");
    }

    // ── 7. Monotonía: más exposición → más fatiga ────────────────────
    std::printf("[monotonia exposicion/fatiga]\n");
    {
        UniversalHearingInput in = UniversalHearingInput::esseGenezi();
        in.context.targetSplDb = 95.0;  // 10 dB sobre 85 -> factor 0.5
        in.signal.exposureMinutes = 0.0;
        const UniversalHearingPrediction p0 =
            UniversalHearingModel::predict(in);
        in.signal.exposureMinutes = 30.0;
        const UniversalHearingPrediction p30 =
            UniversalHearingModel::predict(in);
        in.signal.exposureMinutes = 120.0;
        const UniversalHearingPrediction p120 =
            UniversalHearingModel::predict(in);
        // A mano a 1 kHz (nivel 0.70): fatiga = 0.5*(0.5+0.5*0.70)=0.425
        // a 120 min (+0.0125 de sesion = 0.4375); a 30 min: 0.25*0.5*
        // 0.85 = 0.106 (+0.0125 = 0.119); a 0 min: solo 0.0125.
        check(near(p0.band[4][Dim::kFatigue], 0.0125, 0.01),
              "fatiga 0 min ~0.01 (solo sesion)");
        check(near(p30.band[4][Dim::kFatigue], 0.119, 0.02),
              "fatiga 30 min a 95 dB ~0.12");
        check(near(p120.band[4][Dim::kFatigue], 0.4375, 0.03),
              "fatiga 120 min a 95 dB ~0.44");
        check(meanDim(p120, Dim::kFatigue) > meanDim(p30, Dim::kFatigue) &&
                  meanDim(p30, Dim::kFatigue) > meanDim(p0, Dim::kFatigue),
              "fatiga monotona con la exposicion");
    }

    // ── 8. Monotonía: latencia, azimut, desbalance, THD ──────────────
    std::printf("[monotonia latencia/azimut/THD]\n");
    {
        const UniversalHearingInput base = UniversalHearingInput::esseGenezi();
        UniversalHearingInput slow = base;
        slow.chain.bufferLatencyMs = 40.0;  // buffer grande
        const UniversalHearingPrediction pb =
            UniversalHearingModel::predict(base);
        const UniversalHearingPrediction ps =
            UniversalHearingModel::predict(slow);
        // A mano: latencia (40+8.746)/50 = 0.975 -> causalidad media =
        // 0.6*0.975+0.4*0.358 = 0.728 (antes 0.28).
        check(near(meanDim(ps, Dim::kCausalityLoss), 0.728, 0.05),
              "causalidad media ~0.73 con buffer 40 ms");
        check(meanDim(ps, Dim::kCausalityLoss) >
                  meanDim(pb, Dim::kCausalityLoss),
              "mas latencia -> mas perdida de causalidad");

        UniversalHearingInput off = base;
        off.listener.azimuthDeg = 20.0;  // 20 grados descentrado
        const UniversalHearingPrediction po =
            UniversalHearingModel::predict(off);
        check(near(po.imageShiftDegrees, 20.0, 0.01),
              "desplazamiento 20 grados con azimut 20");
        check(near(meanDim(po, Dim::kImageShift), 20.0 / 30.0, 0.01),
              "desplazamiento normalizado 20/30 ~0.67");
        check(meanDim(po, Dim::kSpatialCompression) >
                  meanDim(pb, Dim::kSpatialCompression),
              "mas azimut -> mas compresion espacial");

        UniversalHearingInput imb = base;
        imb.chain.channelImbalanceDb = 1.0;  // 1 dB -> 5 grados (nominal)
        const UniversalHearingPrediction pi =
            UniversalHearingModel::predict(imb);
        check(near(pi.imageShiftDegrees, 5.0, 0.01),
              "desbalance 1 dB -> 5 grados");
        check(near(meanDim(pi, Dim::kImageShift), 5.0 / 30.0, 0.01),
              "desbalance 1 dB -> normalizado ~0.17");

        UniversalHearingInput thd = base;
        thd.chain.thdPercent = 5.0;  // THD 5 % -> thdNorm 1.0
        const UniversalHearingPrediction pt =
            UniversalHearingModel::predict(thd);
        // A mano 1 kHz: mascara 0.293+0.15 = 0.443; micro 0.20+0.20 = 0.40.
        check(near(pt.band[4][Dim::kTimbralMasking], 0.443, 0.03),
              "THD 5 %: enmascaramiento 1 kHz ~0.44");
        check(meanDim(pt, Dim::kTimbralMasking) >
                  meanDim(pb, Dim::kTimbralMasking),
              "mas THD -> mas enmascaramiento");
        check(near(meanDim(pt, Dim::kMicrodynamicsLoss), 0.40, 0.01),
              "THD 5 %: microdinamica 0.40");
        check(meanDim(pt, Dim::kMicrodynamicsLoss) >
                  meanDim(pb, Dim::kMicrodynamicsLoss),
              "mas THD -> mas perdida de microdinamica");
    }

    // ── 9. Voz más sensible que música en inteligibilidad ────────────
    std::printf("[contenido: voz vs musica]\n");
    {
        const UniversalHearingInput music = UniversalHearingInput::esseGenezi();
        UniversalHearingInput voice = music;
        voice.context.content = ContentType::kVoice;
        const UniversalHearingPrediction pm =
            UniversalHearingModel::predict(music);
        const UniversalHearingPrediction pv =
            UniversalHearingModel::predict(voice);
        check(meanDim(pv, Dim::kIntelligibilityLoss) >
                  meanDim(pm, Dim::kIntelligibilityLoss),
              "voz pierde mas inteligibilidad que musica (factor 1.2)");
    }

    // ── 10. Valores dados se respetan (no se re-derivan) ─────────────
    std::printf("[valores dados]\n");
    {
        UniversalHearingInput in = UniversalHearingInput::esseGenezi();
        in.room.criticalDistanceM = 2.0;  // dada, no derivada
        in.room.coherence = 0.9;          // dada, no derivada
        const UniversalHearingPrediction p =
            UniversalHearingModel::predict(in);
        check(p.valid, "prediccion valida con Dc y coherencia dadas");
        check(p.effectiveCriticalDistanceM == 2.0,
              "distancia critica dada se respeta");
        check(p.effectiveCoherence == 0.9, "coherencia dada se respeta");
        // A mano: profundidad media = 0.55*0.358+0.45*0.1 = 0.242
        // (antes 0.62 con coherencia derivada 0.069).
        check(near(meanDim(p, Dim::kDepthLoss), 0.242, 0.03),
              "coherencia 0.9 -> profundidad media ~0.24");
        check(p.confidence > 0.80,
              "confianza mayor con Dc y coherencia medidas");
    }

    // ── 11. Audífono: sin sala, sin aire ─────────────────────────────
    std::printf("[audifono]\n");
    {
        const UniversalHearingPrediction p = UniversalHearingModel::predict(
            UniversalHearingInput::esseHeadphones());
        check(p.valid, "prediccion valida");
        check(p.totalLatencyMs == 2.667, "solo latencia de buffer");
        check(p.effectiveCriticalDistanceM == 0.0, "sin distancia critica");
        check(p.effectiveCoherence == 1.0, "coherencia 1");
        check(p.schroederHz == 0.0, "sin Schroeder");
        check(meanDim(p, Dim::kDepthLoss) == 0.0,
              "profundidad 0 (sin sala)");
        check(meanDim(p, Dim::kSpatialCompression) == 0.0,
              "compresion espacial 0 (sin sala)");
        check(p.imageShiftDegrees == 0.0, "imagen 0 (centrado)");
        // A mano: causalidad media = 0.6*(2.667/50) + 0 = 0.032.
        check(near(meanDim(p, Dim::kCausalityLoss), 0.032, 0.01),
              "causalidad ~0.03 (solo buffer)");
        // A mano 1 kHz: mascara = 0+0.25*0.70+0.20*0.25 = 0.225;
        // inteligibilidad = 0+0.3*0.25+0 = 0.075.
        check(near(p.band[4][Dim::kTimbralMasking], 0.225, 0.02),
              "enmascaramiento 1 kHz ~0.23 en audifono");
        check(near(p.band[4][Dim::kIntelligibilityLoss], 0.075, 0.02),
              "inteligibilidad 1 kHz ~0.08 en audifono");
        // Confianza: 1 - 0.10 (THD) - 0.05 (piso) = 0.85, sin
        // penalizacion modal ni de sala.
        check(near(p.confidence, 0.85, 0.01), "confianza 0.85 en audifono");
        check(allFiniteAndInRange(p), "salida finita y en rango");
    }

    // ── 12. Fail-closed ──────────────────────────────────────────────
    std::printf("[fail-closed]\n");
    {
        UniversalHearingInput nanRt = UniversalHearingInput::esseGenezi();
        nanRt.room.rt60Sec = std::numeric_limits<double>::quiet_NaN();
        const UniversalHearingPrediction p1 =
            UniversalHearingModel::predict(nanRt);
        check(!p1.valid, "RT60 NaN: no valido");
        check(p1.confidence == 0.0, "RT60 NaN: confianza 0");
        check(p1.interventionScale == 0.0, "RT60 NaN: intervencion 0");
        check(allFiniteAndInRange(p1), "RT60 NaN: salida finita");

        UniversalHearingInput noRoom = UniversalHearingInput::esseGenezi();
        noRoom.room.rt60Sec = 0.0;  // desconocido
        noRoom.room.roomVolumeM3 = 0.0;  // desconocido
        const UniversalHearingPrediction p2 =
            UniversalHearingModel::predict(noRoom);
        check(!p2.valid, "sala desconocida: no valido");
        check(p2.confidence == 0.0, "sala desconocida: confianza 0");
        check(allFiniteAndInRange(p2), "sala desconocida: salida finita");

        UniversalHearingInput nanRms = UniversalHearingInput::esseGenezi();
        nanRms.signal.rmsDbPerBand[4] =
            std::numeric_limits<double>::quiet_NaN();
        const UniversalHearingPrediction p3 =
            UniversalHearingModel::predict(nanRms);
        check(!p3.valid, "RMS NaN: no valido");
        check(allFiniteAndInRange(p3), "RMS NaN: salida finita");

        UniversalHearingInput noSpl = UniversalHearingInput::esseGenezi();
        noSpl.context.targetSplDb = 0.0;  // desconocido (requerido)
        const UniversalHearingPrediction p4 =
            UniversalHearingModel::predict(noSpl);
        check(!p4.valid, "SPL desconocido: no valido");
        check(allFiniteAndInRange(p4), "SPL desconocido: salida finita");

        UniversalHearingInput noLevel = UniversalHearingInput::esseGenezi();
        noLevel.signal.levelKnown = false;  // nivel requerido
        const UniversalHearingPrediction p5 =
            UniversalHearingModel::predict(noLevel);
        check(!p5.valid, "nivel desconocido: no valido");
        check(allFiniteAndInRange(p5), "nivel desconocido: salida finita");

        UniversalHearingInput badAz = UniversalHearingInput::esseGenezi();
        badAz.listener.azimuthDeg = 200.0;  // fuera de [-180, 180]
        check(!UniversalHearingModel::predict(badAz).valid,
              "azimut 200: no valido");

        UniversalHearingInput badCrest = UniversalHearingInput::esseGenezi();
        badCrest.signal.crestFactorDb = -5.0;
        check(!UniversalHearingModel::predict(badCrest).valid,
              "crest -5 dB: no valido");

        UniversalHearingInput badRms = UniversalHearingInput::esseGenezi();
        badRms.signal.rmsDbPerBand[0] = 5.0;  // sobre 0 dBFS
        check(!UniversalHearingModel::predict(badRms).valid,
              "RMS +5 dBFS: no valido");

        // Audífono con sala desconocida SÍ es válido: la sala no existe.
        UniversalHearingInput hpNoRoom =
            UniversalHearingInput::esseHeadphones();
        hpNoRoom.room.rt60Sec = 0.0;
        hpNoRoom.room.roomVolumeM3 = 0.0;
        const UniversalHearingPrediction p6 =
            UniversalHearingModel::predict(hpNoRoom);
        check(p6.valid, "audifono con sala desconocida: valido");
        check(allFiniteAndInRange(p6), "audifono sin sala: salida finita");

        // Entrada por defecto (todo desconocido) es inválida por
        // construcción: fail-closed desde el primer uso.
        UniversalHearingInput def;
        const UniversalHearingPrediction p7 =
            UniversalHearingModel::predict(def);
        check(!p7.valid, "entrada por defecto: no valida");
        check(p7.confidence == 0.0, "entrada por defecto: confianza 0");
        check(allFiniteAndInRange(p7), "entrada por defecto: salida finita");
    }

    // ── 13. Compatibilidad P[t] (perceptual_mpc) ─────────────────────
    std::printf("[compatibilidad P[t]]\n");
    {
        // El orden de las 9 primeras dims es el de perceptual_vector.hpp:
        // audibilidad 0, claridad 1, naturalidad 2, impacto 3,
        // espacialidad 4, fatiga 5, estabilidad 6, confianza 7,
        // presupuesto 8; las 7 de P6 siguen (9..15).
        check(static_cast<std::size_t>(Dim::kAudibility) == 0, "dim 0 audibilidad");
        check(static_cast<std::size_t>(Dim::kClarity) == 1, "dim 1 claridad");
        check(static_cast<std::size_t>(Dim::kNaturalness) == 2, "dim 2 naturalidad");
        check(static_cast<std::size_t>(Dim::kImpact) == 3, "dim 3 impacto");
        check(static_cast<std::size_t>(Dim::kSpatiality) == 4, "dim 4 espacialidad");
        check(static_cast<std::size_t>(Dim::kFatigue) == 5, "dim 5 fatiga");
        check(static_cast<std::size_t>(Dim::kStability) == 6, "dim 6 estabilidad");
        check(static_cast<std::size_t>(Dim::kConfidence) == 7, "dim 7 confianza");
        check(static_cast<std::size_t>(Dim::kBudget) == 8, "dim 8 presupuesto");
        check(static_cast<std::size_t>(Dim::kDepthLoss) == 9, "dim 9 profundidad");
        check(static_cast<std::size_t>(Dim::kCausalityLoss) == 15,
              "dim 15 causalidad");
        check(kHearingDims == 16, "16 dimensiones en total");
        check(kHearingBands == 9, "9 bandas (la 9a, 16 kHz, sin par en el MPC)");

        // El neutro coincide con PerceptualVector::neutral() en las 9
        // primeras dims: {0.5,0.5,0.5,0.5,0.5,0.2,0.5,0.9,0.0};
        // las 7 pérdidas nuevas en 0 (sin pérdida).
        const UniversalHearingState n = UniversalHearingState::neutral();
        const double expected9[9] = {0.5, 0.5, 0.5, 0.5, 0.5,
                                     0.2, 0.5, 0.9, 0.0};
        bool ok9 = true;
        for (std::size_t d = 0; d < 9; ++d) {
            if (n.v[d] != expected9[d]) ok9 = false;
        }
        check(ok9, "neutro: 9 dims P[t] identicas al MPC");
        bool ok7 = true;
        for (std::size_t d = 9; d < 16; ++d) {
            if (n.v[d] != 0.0) ok7 = false;
        }
        check(ok7, "neutro: 7 perdidas P6 en 0");
        check(n.isFinite(), "neutro finito");

        // clampInPlace / isFinite.
        UniversalHearingState s = UniversalHearingState::neutral();
        s.v[0] = -1.0;
        s.v[1] = 2.0;
        s.v[2] = std::numeric_limits<double>::quiet_NaN();
        check(!s.isFinite(), "NaN invalida el estado");
        s.clampInPlace();
        check(s.isFinite(), "tras clamp, estado finito");
        check(s.v[0] == 0.0 && s.v[1] == 1.0 && s.v[2] == 0.0,
              "clamp: -1->0, 2->1, NaN->0");

        // La media global es la media aritmética de las bandas.
        const UniversalHearingPrediction p = UniversalHearingModel::predict(
            UniversalHearingInput::esseGenezi());
        double sum = 0.0;
        for (std::size_t i = 0; i < kHearingBands; ++i) {
            sum += p.band[i][Dim::kDepthLoss];
        }
        check(near(p.globalMean[static_cast<std::size_t>(Dim::kDepthLoss)],
                   sum / 9.0, 1e-12),
              "media global = media de bandas (profundidad)");
    }

    std::printf("== %d checks, %d FALLARON ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
