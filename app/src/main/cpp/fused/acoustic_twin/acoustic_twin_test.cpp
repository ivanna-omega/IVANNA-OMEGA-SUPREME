// acoustic_twin_test.cpp — Tests host del gemelo acústico (2026-11-21).
//
// Standalone (propio main(), sin gtest), mismo patrón que
// test_route_b_neutral_snapshot / test_adaptive_engine: se compila con
// g++-14 y se corre con ctest. Cubre lo que exige el encargo:
//   1. La estimación es estable ante la misma entrada (determinista).
//   2. La confianza baja cuando la entrada es inconsistente.
//   3. El módulo no modifica la señal (no tiene camino de audio).
// Más: el trayecto real de Esse (Genezi, sala 4.0×7.0, 3 m), la
// monotonía con el SPL, el caso audífono, y que jamás sale NaN/inf.
//
// Los valores esperados del trayecto de Esse se calculan a mano en los
// comentarios (no se copian de la salida del programa): si el test pasa,
// la física del módulo coincide con el cálculo independiente.
#include "acoustic_twin.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace ivanna::fused::acoustic_twin;

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

// Compara dos estimaciones campo por campo (determinismo bit a bit).
bool estimatesEqual(const AcousticTwinEstimate& a, const AcousticTwinEstimate& b) {
    if (a.valid != b.valid) return false;
    if (a.roomVolumeM3 != b.roomVolumeM3) return false;
    if (a.roomSurfaceM2 != b.roomSurfaceM2) return false;
    if (a.schroederHz != b.schroederHz) return false;
    if (a.criticalDistanceM != b.criticalDistanceM) return false;
    if (a.bufferLatencyMs != b.bufferLatencyMs) return false;
    if (a.airLatencyMs != b.airLatencyMs) return false;
    if (a.overallConfidence != b.overallConfidence) return false;
    if (a.overallInterventionScale != b.overallInterventionScale) return false;
    if (a.calibrationApplied != b.calibrationApplied) return false;
    if (a.calibrationConfidence != b.calibrationConfidence) return false;
    if (a.satellitesActive != b.satellitesActive) return false;
    for (int i = 0; i < kTwinBands; ++i) {
        const AcousticTwinBand& x = a.band[i];
        const AcousticTwinBand& y = b.band[i];
        if (x.freqHz != y.freqHz || x.gainDb != y.gainDb ||
            x.latencyMs != y.latencyMs || x.dynamicRangeDb != y.dynamicRangeDb ||
            x.thdPercent != y.thdPercent || x.noiseDbSpl != y.noiseDbSpl ||
            x.rt60Sec != y.rt60Sec || x.coherence != y.coherence ||
            x.compressionDb != y.compressionDb || x.maxSplDb != y.maxSplDb ||
            x.maxSplKnown != y.maxSplKnown || x.confidence != y.confidence ||
            x.interventionScale != y.interventionScale) {
            return false;
        }
        if (a.calibratedResponseDb[i] != b.calibratedResponseDb[i]) return false;
        if (a.satelliteGainDb[i] != b.satelliteGainDb[i]) return false;
    }
    return true;
}

// Todas las salidas finitas y en rango (nunca NaN/inf, nunca fuera de [0,1]
// donde el contrato lo exige). Vale para entradas válidas E inválidas.
bool allFiniteAndInRange(const AcousticTwinEstimate& e) {
    if (!std::isfinite(e.roomVolumeM3) || !std::isfinite(e.roomSurfaceM2) ||
        !std::isfinite(e.schroederHz) || !std::isfinite(e.criticalDistanceM) ||
        !std::isfinite(e.bufferLatencyMs) || !std::isfinite(e.airLatencyMs) ||
        !std::isfinite(e.overallConfidence) ||
        !std::isfinite(e.overallInterventionScale) ||
        !std::isfinite(e.calibrationConfidence)) {
        return false;
    }
    if (e.overallConfidence < 0.0 || e.overallConfidence > 1.0) return false;
    if (e.overallInterventionScale < 0.0 || e.overallInterventionScale > 1.0) {
        return false;
    }
    if (e.calibrationConfidence < 0.0 || e.calibrationConfidence > 1.0) return false;
    // Con calibración, la confianza global ES la honesta (no la media).
    if (e.calibrationApplied &&
        e.overallConfidence != e.calibrationConfidence) {
        return false;
    }
    for (int i = 0; i < kTwinBands; ++i) {
        const AcousticTwinBand& b = e.band[i];
        if (!std::isfinite(b.gainDb) || !std::isfinite(b.latencyMs) ||
            !std::isfinite(b.dynamicRangeDb) || !std::isfinite(b.thdPercent) ||
            !std::isfinite(b.noiseDbSpl) || !std::isfinite(b.rt60Sec) ||
            !std::isfinite(b.coherence) || !std::isfinite(b.compressionDb) ||
            !std::isfinite(b.maxSplDb) || !std::isfinite(b.confidence) ||
            !std::isfinite(b.interventionScale)) {
            return false;
        }
        if (b.confidence < 0.0 || b.confidence > 1.0) return false;
        if (b.interventionScale < 0.0 || b.interventionScale > 1.0) return false;
        // Invariante central: a confianza baja se interviene MENOS.
        if (b.interventionScale != b.confidence) return false;
        if (b.coherence < 0.0 || b.coherence > 1.0) return false;
        if (b.thdPercent < 0.0 || b.thdPercent > 30.0) return false;
        if (b.rt60Sec < 0.0 || b.compressionDb < 0.0) return false;
    }
    return true;
}

// Parámetros con potencia y sensibilidad conocidas (para THD/compresión).
AcousticTwinParams fullySpecified() {
    AcousticTwinParams p = AcousticTwinParams::esseGenezi();
    p.ampMaxPowerW = 100.0;  // 100 W por canal (ejemplo, no del Genezi)
    p.sensitivityDb = 88.0;  // 88 dB SPL @ 1 W @ 1 m (ejemplo)
    return p;
}

}  // namespace

int main() {
    std::printf("== acoustic_twin_test ==\n");

    // ── 1. Determinismo: misma entrada → misma salida ────────────────
    std::printf("[determinismo]\n");
    {
        const AcousticTwinParams p = AcousticTwinParams::esseGenezi();
        const AcousticTwinEstimate e1 = AcousticTwin::estimate(p);
        const AcousticTwinEstimate e2 = AcousticTwin::estimate(p);
        check(estimatesEqual(e1, e2), "misma entrada, salida identica");
        check(allFiniteAndInRange(e1), "salida finita y en rango");
    }

    // ── 2. Sin estado oculto: intercalar otra entrada no contamina ───
    std::printf("[sin estado oculto]\n");
    {
        const AcousticTwinParams a = AcousticTwinParams::esseGenezi();
        const AcousticTwinParams b = AcousticTwinParams::esseHeadphones();
        const AcousticTwinEstimate ea1 = AcousticTwin::estimate(a);
        (void)AcousticTwin::estimate(b);
        (void)AcousticTwin::estimateAtSpl(a, 100.0);
        const AcousticTwinEstimate ea2 = AcousticTwin::estimate(a);
        check(estimatesEqual(ea1, ea2), "re-estimar A tras B devuelve lo mismo");
    }

    // ── 3. No modifica la señal ──────────────────────────────────────
    std::printf("[no modifica la senal]\n");
    {
        // Buffer de audio con patrón conocido (seno + ruido determinista).
        float buffer[1024];
        for (int i = 0; i < 1024; ++i) {
            buffer[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.141592653589793 * i / 64.0) +
                                           0.001 * (i % 7));
        }
        float before[1024];
        std::memcpy(before, buffer, sizeof(buffer));
        const AcousticTwinParams p = AcousticTwinParams::esseGenezi();
        (void)AcousticTwin::estimate(p);
        (void)AcousticTwin::estimateAtSpl(p, 70.0);
        (void)AcousticTwin::estimateAtSpl(p, 100.0);
        check(std::memcmp(before, buffer, sizeof(buffer)) == 0,
              "el buffer de audio queda bit-identico tras estimar");
    }

    // ── 4. Trayecto real de Esse (Genezi, sala 4.0x7.0, 3 m) ──────────
    std::printf("[trayecto de Esse: Genezi]\n");
    {
        // Cálculo independiente (a mano):
        //   V = 4*7*2.5 = 70 m3; S = 2*(28+10+17.5) = 111 m2
        //   alpha = 0.161*70/(111*0.34) = 0.2986
        //   Schroeder = 2000*sqrt(0.34/70) = 139.4 Hz
        //   Dc = 0.057*sqrt(70/0.34) = 0.818 m
        //   coherencia a 3 m = 1/(1+(3/0.818)^2) = 0.069
        //   ganancia 1 kHz = 10*log10(1/9 + 4/33.15) = -6.35 dB
        //   latencia = 128/48000*1000 + 3/343*1000 = 2.667 + 8.746 = 11.41 ms
        //   RT60 a 1 kHz = 0.34 s (factor de absorción 1.0 ahí)
        const AcousticTwinEstimate e = AcousticTwin::estimate(
            AcousticTwinParams::esseGenezi());
        check(e.valid, "estimacion valida");
        check(near(e.roomVolumeM3, 70.0, 0.01), "volumen 70 m3");
        check(near(e.roomSurfaceM2, 111.0, 0.01), "superficie 111 m2");
        check(near(e.schroederHz, 139.4, 5.0), "Schroeder ~139 Hz");
        check(near(e.criticalDistanceM, 0.818, 0.05), "distancia critica ~0.82 m");
        check(near(e.bufferLatencyMs, 2.667, 0.05), "latencia buffer 2.67 ms");
        check(near(e.airLatencyMs, 8.746, 0.05), "latencia aire 8.75 ms");
        const AcousticTwinBand& b1k = e.band[4];  // 1000 Hz
        check(near(b1k.latencyMs, 11.41, 0.5), "latencia total ~11.4 ms");
        check(near(b1k.gainDb, -6.35, 1.0), "ganancia 1 kHz ~-6.3 dB");
        check(near(b1k.rt60Sec, 0.34, 0.05), "RT60 1 kHz = 0.34 s");
        check(near(b1k.coherence, 0.069, 0.03),
              "coherencia baja a 3 m (domina la reverberacion)");
        // Potencia y sensibilidad del Genezi NO medidas: el módulo lo dice.
        check(!b1k.maxSplKnown, "SPL maximo desconocido (potencia no medida)");
        check(b1k.thdPercent == 0.0, "THD 0 = no estimable, no cero real");
        // Confianza: 1 - 0.25 (potencia) - 0.25 (sensibilidad) = 0.5 base;
        // las bandas bajo Schroeder bajan a 0.35/0.315. Media ~0.46.
        check(e.overallConfidence > 0.3 && e.overallConfidence < 0.7,
              "confianza intermedia (falta potencia y sensibilidad)");
        check(e.overallInterventionScale == e.overallConfidence,
              "interventionScale == confianza (a confianza baja, menos intervencion)");
        check(allFiniteAndInRange(e), "salida finita y en rango");
    }

    // ── 5. Confianza baja ante entrada inconsistente ─────────────────
    std::printf("[confianza baja ante entrada inconsistente]\n");
    {
        AcousticTwinParams zero{};
        zero.dacBits = 0;
        zero.dacSampleRateHz = 0;
        zero.dacBufferFrames = 0;
        zero.roomRt60Sec = 0.0;
        zero.listenerDistanceM = 0.0;
        const AcousticTwinEstimate e0 = AcousticTwin::estimate(zero);
        check(!e0.valid, "parametros en cero: no valido");
        check(e0.overallConfidence == 0.0, "parametros en cero: confianza 0");
        check(e0.overallInterventionScale == 0.0,
              "parametros en cero: intervencion 0");
        check(allFiniteAndInRange(e0), "parametros en cero: salida finita");

        AcousticTwinParams badRt = AcousticTwinParams::esseGenezi();
        badRt.roomRt60Sec = 5.0;  // imposible en una sala de 70 m3
        const AcousticTwinEstimate e1 = AcousticTwin::estimate(badRt);
        check(e1.overallConfidence < 0.5, "RT60 5 s en 70 m3: confianza < 0.5");
        check(e1.overallInterventionScale < 0.5,
              "RT60 5 s: intervencion < 0.5");

        AcousticTwinParams badDist = AcousticTwinParams::esseGenezi();
        badDist.listenerDistanceM = 50.0;  // fuera de la sala (diagonal ~8.6 m)
        const AcousticTwinEstimate e2 = AcousticTwin::estimate(badDist);
        check(e2.overallConfidence < 0.5, "distancia 50 m: confianza < 0.5");

        AcousticTwinParams nanRt = AcousticTwinParams::esseGenezi();
        nanRt.roomRt60Sec = std::numeric_limits<double>::quiet_NaN();
        const AcousticTwinEstimate e3 = AcousticTwin::estimate(nanRt);
        check(!e3.valid, "RT60 NaN: no valido (fail-closed)");
        check(allFiniteAndInRange(e3), "RT60 NaN: salida finita");
    }

    // ── 6. Comportamiento a distintos SPL (monotonía) ────────────────
    std::printf("[comportamiento a distintos SPL]\n");
    {
        const AcousticTwinParams p = fullySpecified();
        const AcousticTwinEstimate e70 = AcousticTwin::estimateAtSpl(p, 70.0);
        const AcousticTwinEstimate e85 = AcousticTwin::estimateAtSpl(p, 85.0);
        const AcousticTwinEstimate e100 = AcousticTwin::estimateAtSpl(p, 100.0);
        const AcousticTwinBand& b70 = e70.band[4];
        const AcousticTwinBand& b85 = e85.band[4];
        const AcousticTwinBand& b100 = e100.band[4];
        check(b70.maxSplKnown && b100.maxSplKnown, "SPL maximo conocido");
        check(near(b70.maxSplDb, 101.65, 1.0), "SPL maximo 1 kHz ~101.7 dB");
        check(b70.compressionDb == 0.0, "a 70 dB no hay compresion");
        check(b100.compressionDb > 0.0, "a 100 dB si hay compresion");
        check(b100.thdPercent > b85.thdPercent &&
                  b85.thdPercent > b70.thdPercent,
              "THD crece con el SPL (monotono)");
        check(b100.compressionDb > b85.compressionDb &&
                  b85.compressionDb >= b70.compressionDb,
              "compresion crece con el SPL (monotona)");
        // Lo que NO depende del SPL no cambia entre niveles.
        check(b70.gainDb == b100.gainDb, "ganancia independiente del SPL");
        check(b70.latencyMs == b100.latencyMs, "latencia independiente del SPL");
        check(b70.rt60Sec == b100.rt60Sec, "RT60 independiente del SPL");
        check(b70.coherence == b100.coherence, "coherencia independiente del SPL");
        check(allFiniteAndInRange(e100), "salida finita y en rango a 100 dB");
    }

    // ── 7. Audífono: sin sala, sin aire ──────────────────────────────
    std::printf("[audifono]\n");
    {
        const AcousticTwinEstimate e = AcousticTwin::estimate(
            AcousticTwinParams::esseHeadphones());
        check(e.valid, "estimacion valida");
        check(e.airLatencyMs == 0.0, "sin latencia de aire");
        check(near(e.bufferLatencyMs, 2.667, 0.05), "solo latencia de buffer");
        check(e.roomVolumeM3 == 0.0 && e.schroederHz == 0.0, "sin sala");
        bool ok = true;
        for (int i = 0; i < kTwinBands; ++i) {
            if (e.band[i].rt60Sec != 0.0 || e.band[i].coherence != 1.0 ||
                e.band[i].gainDb != 0.0) {
                ok = false;
            }
        }
        check(ok, "RT60 0, coherencia 1, ganancia 0 en todas las bandas");
        check(allFiniteAndInRange(e), "salida finita y en rango");
    }

    // ── 8. Calibración P3 estimada (Genezi, datasheet + física) ─────
    std::printf("[calibracion P3 estimada]\n");
    {
        // La fábrica es la transcripción fiel del JSON
        // (genezi_pz1d_estimado_datasheet.json).
        const TwinCalibration cal = TwinCalibration::geneziPz1dEstimated();
        check(cal.valid, "calibracion P3 valida");
        check(cal.isEstimated, "calibracion marcada como ESTIMADA (no medida)");
        check(cal.responseKnown, "respuesta por banda presente");
        check(near(cal.honestConfidence, 0.70, 0.001),
              "confianza honesta global 0.70");
        check(near(cal.directPathConfidence, 0.85, 0.001), "directo 0.85");
        check(near(cal.bassConfidence, 0.65, 0.001), "graves 0.65");
        check(near(cal.midConfidence, 0.70, 0.001), "medios 0.70");
        check(near(cal.trebleConfidence, 0.60, 0.001), "agudos 0.60");
        check(near(cal.roomConfidence, 0.70, 0.001), "sala 0.70");
        check(near(cal.responseDb[0], -3.24, 0.01), "respuesta 63 Hz -3.24 dB");
        check(near(cal.responseDb[1], 1.97, 0.01), "respuesta 125 Hz +1.97 dB");
        check(near(cal.responseDb[4], 0.0, 0.001), "respuesta 1 kHz 0 dB");
        // Satélite (componente separado, derivado).
        check(cal.satelliteKnown, "satelite conocido");
        check(near(cal.satelliteF3Hz, 157.5, 0.1), "satelite f3 157.5 Hz");
        check(near(cal.satelliteF3HighHz, 16000.0, 1.0),
              "satelite f3 agudo 16 kHz");
        check(near(cal.satelliteSensitivityDb, 84.0, 0.1),
              "satelite sensibilidad 84 dB");
        check(near(cal.satelliteResponseDb[0], -31.84, 0.05),
              "satelite 63 Hz -31.8 dB (HP4)");
        check(near(cal.satelliteResponseDb[8], -3.01, 0.05),
              "satelite 16 kHz -3.0 dB (corte agudo)");

        // Aplicar la calibración al preset calibrado.
        const AcousticTwinParams p =
            AcousticTwinParams::esseGeneziCalibrated();
        const AcousticTwinEstimate e =
            AcousticTwin::estimateCalibrated(p, cal);
        check(e.valid, "estimacion calibrada valida");
        check(e.calibrationApplied, "calibracion aplicada");
        check(near(e.calibrationConfidence, 0.70, 0.001),
              "confianza de calibracion 0.70 (honesta)");
        // LA CLAVE: la confianza es la honesta (0.70), NO la de la
        // fórmula con los valores rellenados (0.926). Si este test
        // falla con 0.926, la calibración se está tratando como medida.
        check(near(e.overallConfidence, 0.70, 0.001),
              "confianza global 0.70, NO 0.926 de la formula");
        check(e.overallConfidence < 0.80,
              "0.70 < 0.80: el gemelo NO controla audio");
        check(e.overallInterventionScale == e.overallConfidence,
              "interventionScale == confianza (0.70)");
        // Confianza por banda: graves 0.65, medios 0.70, agudos 0.60.
        check(near(e.band[0].confidence, 0.65, 0.001), "banda 63 Hz: 0.65");
        check(near(e.band[1].confidence, 0.65, 0.001), "banda 125 Hz: 0.65");
        check(near(e.band[4].confidence, 0.70, 0.001), "banda 1 kHz: 0.70");
        check(near(e.band[8].confidence, 0.60, 0.001), "banda 16 kHz: 0.60");
        // Ganancia por banda: anclada a 1 kHz (-6.35 dB) + respuesta.
        // 1 kHz no cambia; 63 Hz baja por el valle modal (-3.24 dB).
        check(near(e.band[4].gainDb, -6.35, 0.5),
              "ganancia 1 kHz ~-6.35 dB (ancla, sin cambio)");
        check(near(e.band[0].gainDb, -9.59, 0.5),
              "ganancia 63 Hz ~-9.6 dB (-6.35 - 3.24)");
        check(near(e.band[1].gainDb, -4.38, 0.5),
              "ganancia 125 Hz ~-4.4 dB (-6.35 + 1.97)");
        check(near(e.calibratedResponseDb[0], -3.24, 0.01),
              "respuesta calibrada 63 Hz guardada");
        // Con la calibración, la potencia y la sensibilidad ya se conocen.
        check(e.band[4].maxSplKnown,
              "SPL maximo conocido con calibracion");
        check(near(e.band[4].maxSplDb, 102.95, 1.0),
              "SPL maximo 1 kHz ~103 dB (135 W, 88 dB)");
        check(allFiniteAndInRange(e), "salida calibrada finita y en rango");

        // Determinismo de la estimación calibrada.
        const AcousticTwinEstimate e2 =
            AcousticTwin::estimateCalibrated(p, cal);
        check(estimatesEqual(e, e2), "calibrada determinista");

        // Sin calibración, el comportamiento NO cambia: el preset sin
        // calibrar (potencia/sensibilidad en 0) da lo mismo que antes.
        const AcousticTwinEstimate ePlain =
            AcousticTwin::estimate(AcousticTwinParams::esseGenezi());
        check(!ePlain.calibrationApplied, "sin calibracion: no aplicada");
        check(ePlain.calibrationConfidence == 0.0,
              "sin calibracion: confianza de calibracion 0");
        check(!ePlain.band[4].maxSplKnown,
              "sin calibracion: SPL maximo desconocido (como antes)");
        check(near(ePlain.overallConfidence, 0.46, 0.05),
              "sin calibracion: confianza ~0.46 (como antes)");

        // Calibración inválida -> fail-closed: igual que sin calibrar.
        TwinCalibration bad = cal;
        bad.honestConfidence = 1.5;  // fuera de [0,1]
        check(!bad.isValid(), "confianza 1.5: calibracion invalida");
        const AcousticTwinEstimate eBad =
            AcousticTwin::estimateCalibrated(p, bad);
        check(!eBad.calibrationApplied,
              "calibracion invalida: no se aplica");
        check(estimatesEqual(eBad, AcousticTwin::estimate(p)),
              "calibracion invalida: igual que estimate() a secas");
        TwinCalibration bad2 = cal;
        bad2.honestConfidence = 1.0;  // estimado no puede ser 1.0
        check(!bad2.isValid(), "estimado con confianza 1.0: invalido");
        TwinCalibration bad3 = cal;
        bad3.responseDb[0] = std::numeric_limits<double>::quiet_NaN();
        check(!bad3.isValid(), "respuesta NaN: calibracion invalida");

        // Un valor MEDIDO del usuario manda sobre el derivado: si el
        // usuario da potencia 200 W, no se sobrescribe con 135 W.
        AcousticTwinParams pUser = p;
        pUser.ampMaxPowerW = 200.0;
        const AcousticTwinEstimate eUser =
            AcousticTwin::estimateCalibrated(pUser, cal);
        check(eUser.calibrationApplied, "con valor de usuario: calibrada");
        // 88 + 10*log10(200) - 6.35 = 104.66 dB (con 135 W seria 102.95).
        check(near(eUser.band[4].maxSplDb, 104.66, 1.0),
              "potencia medida (200 W) manda sobre la derivada (135 W)");

        // Satélite: apagado -> inactivo; encendido -> activo con su respuesta.
        check(!e.satellitesActive, "satelite apagado: inactivo");
        AcousticTwinParams pSat = p;
        pSat.satellitesOn = true;
        const AcousticTwinEstimate eSat =
            AcousticTwin::estimateCalibrated(pSat, cal);
        check(eSat.satellitesActive, "satelite encendido: activo");
        check(near(eSat.satelliteGainDb[0], -31.84, 0.05),
              "satelite 63 Hz -31.8 dB");
        check(near(eSat.satelliteGainDb[4], 0.0, 0.01),
              "satelite 1 kHz 0 dB");
        check(near(eSat.satelliteGainDb[8], -3.01, 0.05),
              "satelite 16 kHz -3.0 dB");
        // El satélite no cambia la respuesta principal (frontal+sub).
        check(near(eSat.band[0].gainDb, e.band[0].gainDb, 0.001),
              "satelite no cambia la ganancia principal");
        check(near(eSat.overallConfidence, 0.70, 0.001),
              "satelite no cambia la confianza global");
        check(allFiniteAndInRange(eSat), "salida con satelite finita");
    }

    // ── 9. Calibración MEDIDA desde la nota de voz (2026-11-22) ────────
    std::printf("[calibracion nota de voz]\n");
    {
        // Respuesta en magnitud medida por Esse (3 barridos ESS, nota
        // de voz AAC 44.1 kHz). Graves/medios MEDIDOS, agudos del
        // estimado (el grabador de voz recorta agudos).
        const TwinCalibration cal = TwinCalibration::geneziPz1dVoiceNote();
        check(cal.valid, "calibracion nota de voz valida");
        check(!cal.isEstimated, "calibracion marcada como MEDIDA (no estimada)");
        check(cal.responseKnown, "respuesta por banda presente");
        check(near(cal.honestConfidence, 0.75, 0.001),
              "confianza honesta global 0.75");
        check(near(cal.directPathConfidence, 0.85, 0.001), "directo 0.85");
        check(near(cal.bassConfidence, 0.80, 0.001), "graves 0.80 (medidos)");
        check(near(cal.midConfidence, 0.80, 0.001), "medios 0.80 (medidos)");
        check(near(cal.trebleConfidence, 0.60, 0.001),
              "agudos 0.60 (NO medidos, estimado)");
        check(near(cal.roomConfidence, 0.70, 0.001), "sala 0.70");
        // Respuesta medida (nota de voz, relativa a 1 kHz).
        check(near(cal.responseDb[0], -6.22, 0.01), "respuesta 63 Hz -6.22 dB");
        check(near(cal.responseDb[1], 1.17, 0.01), "respuesta 125 Hz +1.17 dB");
        check(near(cal.responseDb[2], -3.53, 0.01), "respuesta 250 Hz -3.53 dB");
        check(near(cal.responseDb[3], -1.31, 0.01), "respuesta 500 Hz -1.31 dB");
        check(near(cal.responseDb[4], 0.0, 0.001), "respuesta 1 kHz 0 dB");
        check(near(cal.responseDb[5], -1.72, 0.01), "respuesta 2 kHz -1.72 dB");
        // Agudos del estimado (0.0 dB), NO de la nota de voz.
        check(near(cal.responseDb[6], 0.0, 0.001),
              "respuesta 4 kHz 0 dB (estimado, no medido)");
        check(near(cal.responseDb[7], 0.0, 0.001),
              "respuesta 8 kHz 0 dB (estimado, no medido)");
        check(near(cal.responseDb[8], 0.0, 0.001),
              "respuesta 16 kHz 0 dB (estimado, no medido)");
        // Mejor que el estimado: 0.75 > 0.70, y por debajo de 0.80
        // (falta potencia, sensibilidad, RT60 y agudos medidos).
        const TwinCalibration est = TwinCalibration::geneziPz1dEstimated();
        check(cal.honestConfidence > est.honestConfidence,
              "nota de voz 0.75 > estimado 0.70");
        check(cal.honestConfidence < 0.80,
              "nota de voz 0.75 < 0.80 (no es medicion completa)");
        // La respuesta medida difiere de la estimada en graves.
        check(cal.responseDb[0] < est.responseDb[0],
              "63 Hz medido (-6.22) mas bajo que estimado (-3.24)");
        check(cal.responseDb[2] < est.responseDb[2],
              "250 Hz medido (-3.53) mas bajo que estimado (-0.10)");

        // Aplicar la calibración al preset calibrado.
        const AcousticTwinParams p =
            AcousticTwinParams::esseGeneziCalibrated();
        const AcousticTwinEstimate e =
            AcousticTwin::estimateCalibrated(p, cal);
        check(e.valid, "estimacion calibrada (nota de voz) valida");
        check(e.calibrationApplied, "calibracion nota de voz aplicada");
        check(near(e.calibrationConfidence, 0.75, 0.001),
              "confianza de calibracion 0.75 (honesta)");
        check(near(e.overallConfidence, 0.75, 0.001),
              "confianza global 0.75, NO 0.926 de la formula");
        check(near(e.overallInterventionScale, 0.75, 0.001),
              "interventionScale == confianza (0.75)");
        // Confianza por banda: graves/medios 0.80, agudos 0.60.
        check(near(e.band[0].confidence, 0.80, 0.001), "banda 63 Hz: 0.80");
        check(near(e.band[1].confidence, 0.80, 0.001), "banda 125 Hz: 0.80");
        check(near(e.band[4].confidence, 0.80, 0.001), "banda 1 kHz: 0.80");
        check(near(e.band[8].confidence, 0.60, 0.001), "banda 16 kHz: 0.60");
        // Ganancia por banda: anclada a 1 kHz (-6.35 dB) + respuesta.
        check(near(e.band[4].gainDb, -6.35, 0.5),
              "ganancia 1 kHz ~-6.35 dB (ancla, sin cambio)");
        check(near(e.band[0].gainDb, -12.57, 0.5),
              "ganancia 63 Hz ~-12.6 dB (-6.35 - 6.22)");
        check(near(e.band[1].gainDb, -5.18, 0.5),
              "ganancia 125 Hz ~-5.2 dB (-6.35 + 1.17)");
        check(near(e.calibratedResponseDb[0], -6.22, 0.01),
              "respuesta calibrada 63 Hz guardada");
        check(allFiniteAndInRange(e), "salida calibrada finita y en rango");
        // Determinismo.
        const AcousticTwinEstimate e2 =
            AcousticTwin::estimateCalibrated(p, cal);
        check(estimatesEqual(e, e2), "calibrada (nota de voz) determinista");
    }

    // ── 10. Calibración MEDIDA desde el barrido Farina de la app ──────
    // (2026-11-22 05:54 CST, ESS limpio WAV 48 kHz)
    std::printf("[calibracion medida farina app]\n");
    {
        // Respuesta en magnitud medida en WAV limpio (2 barridos ESS,
        // lo/hi promediados). Graves/medios/agudos hasta 8 kHz MEDIDOS;
        // 16 kHz del estimado (el micrófono recorta ahí).
        const TwinCalibration cal = TwinCalibration::geneziPz1dMeasured();
        check(cal.valid, "calibracion medida valida");
        check(!cal.isEstimated, "calibracion marcada como MEDIDA (no estimada)");
        check(cal.responseKnown, "respuesta por banda presente");
        check(near(cal.honestConfidence, 0.80, 0.001),
              "confianza honesta global 0.80");
        check(near(cal.directPathConfidence, 0.85, 0.001), "directo 0.85");
        check(near(cal.bassConfidence, 0.85, 0.001), "graves 0.85 (medidos)");
        check(near(cal.midConfidence, 0.80, 0.001), "medios 0.80 (medidos)");
        check(near(cal.trebleConfidence, 0.60, 0.001),
              "agudos 0.60 (4 kHz con duda, 16 kHz no medido)");
        check(near(cal.roomConfidence, 0.70, 0.001), "sala 0.70");
        // Respuesta medida (barrido Farina app, relativa a 1 kHz).
        check(near(cal.responseDb[0], -1.90, 0.01), "respuesta 63 Hz -1.90 dB");
        check(near(cal.responseDb[1], -0.76, 0.01), "respuesta 125 Hz -0.76 dB");
        check(near(cal.responseDb[2], -3.71, 0.01), "respuesta 250 Hz -3.71 dB");
        check(near(cal.responseDb[3], -2.99, 0.01), "respuesta 500 Hz -2.99 dB");
        check(near(cal.responseDb[4], 0.0, 0.001), "respuesta 1 kHz 0 dB");
        check(near(cal.responseDb[5], -10.51, 0.01), "respuesta 2 kHz -10.51 dB");
        check(near(cal.responseDb[6], -17.05, 0.01), "respuesta 4 kHz -17.05 dB");
        check(near(cal.responseDb[7], -7.00, 0.01), "respuesta 8 kHz -7.00 dB");
        // 16 kHz del estimado (0.0 dB), NO del barrido (el micrófono
        // recorta ahí: −30 dB medido es el micrófono, no el Genezi).
        check(near(cal.responseDb[8], 0.0, 0.001),
              "respuesta 16 kHz 0 dB (estimado, no medido)");
        // Mejor que la nota de voz (0.80 > 0.75) y que el estimado.
        const TwinCalibration vn = TwinCalibration::geneziPz1dVoiceNote();
        const TwinCalibration est = TwinCalibration::geneziPz1dEstimated();
        check(cal.honestConfidence > vn.honestConfidence,
              "medida 0.80 > nota de voz 0.75");
        check(cal.honestConfidence > est.honestConfidence,
              "medida 0.80 > estimado 0.70");
        // La respuesta medida difiere de la nota de voz en 2-4 kHz
        // (la nota de voz, AAC, no veía la caída).
        check(cal.responseDb[5] < vn.responseDb[5],
              "2 kHz medido (-10.51) mas bajo que nota de voz (-1.72)");
        check(cal.responseDb[6] < vn.responseDb[6],
              "4 kHz medido (-17.05) mas bajo que nota de voz (0.0)");

        // Aplicar la calibración al preset calibrado.
        const AcousticTwinParams p =
            AcousticTwinParams::esseGeneziCalibrated();
        const AcousticTwinEstimate e =
            AcousticTwin::estimateCalibrated(p, cal);
        check(e.valid, "estimacion calibrada (medida) valida");
        check(e.calibrationApplied, "calibracion medida aplicada");
        check(near(e.calibrationConfidence, 0.80, 0.001),
              "confianza de calibracion 0.80 (honesta)");
        check(near(e.overallConfidence, 0.80, 0.001),
              "confianza global 0.80, NO 0.926 de la formula");
        check(near(e.overallInterventionScale, 0.80, 0.001),
              "interventionScale == confianza (0.80)");
        // Confianza por banda: graves 0.85, medios 0.80, agudos 0.60.
        check(near(e.band[0].confidence, 0.85, 0.001), "banda 63 Hz: 0.85");
        check(near(e.band[1].confidence, 0.85, 0.001), "banda 125 Hz: 0.85");
        check(near(e.band[4].confidence, 0.80, 0.001), "banda 1 kHz: 0.80");
        check(near(e.band[8].confidence, 0.60, 0.001), "banda 16 kHz: 0.60");
        // Ganancia por banda: anclada a 1 kHz (-6.35 dB) + respuesta.
        check(near(e.band[4].gainDb, -6.35, 0.5),
              "ganancia 1 kHz ~-6.35 dB (ancla, sin cambio)");
        check(near(e.band[0].gainDb, -8.25, 0.5),
              "ganancia 63 Hz ~-8.25 dB (-6.35 - 1.90)");
        check(near(e.band[1].gainDb, -7.11, 0.5),
              "ganancia 125 Hz ~-7.11 dB (-6.35 - 0.76)");
        check(near(e.band[6].gainDb, -23.40, 0.5),
              "ganancia 4 kHz ~-23.40 dB (-6.35 - 17.05)");
        check(near(e.calibratedResponseDb[0], -1.90, 0.01),
              "respuesta calibrada 63 Hz guardada");
        check(allFiniteAndInRange(e), "salida calibrada finita y en rango");
        // Determinismo.
        const AcousticTwinEstimate e2 =
            AcousticTwin::estimateCalibrated(p, cal);
        check(estimatesEqual(e, e2), "calibrada (medida) determinista");
    }

    std::printf("== %d checks, %d FALLARON ==\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
