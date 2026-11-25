// human_twin_test.cpp — Tests host de P10.1.
//
// Standalone (propio main(), sin gtest), mismo patrón que P6/P7/P8/P9.
// Cubre lo que exige el plan (PLAN_P7_P10.md §5.3 paso 1):
//   1. Banco de pruebas: las 8 pruebas existen, cada una con su eje
//      (nunca mezclados) y su duración nominal.
//   2. Registro por prueba: estímulo, respuesta, tiempo, eje; el log
//      rechaza registros con eje mezclado (prueba invalidada).
//   3. Serialización ida y vuelta del registro.
//   4. Modelo de sensibilidad: sin baseline devuelve prior de azar
//      (0.5) con calibrated=false; con <20 ensayos se niega a calibrar;
//      con >= 20 ensayos calibra con la tasa empírica.
//   5. Fail-closed: registros inválidos no entran al log ni al modelo.
#include "human_twin.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace ivanna::fused::human_twin;

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

TrialRecord rec(TestType t, std::uint32_t stim, bool correct,
                double timeMs = 1200.0) {
    TrialRecord r;
    r.type = t;
    r.axis = testAxis(t);
    r.stimulusId = stim;
    r.response = correct ? 1 : 0;
    r.responseTimeMs = timeMs;
    r.correct = correct;
    r.timestampMs = 1000000 + stim;
    return r;
}

}  // namespace

int main() {
    std::printf("P10 human_twin: %zu pruebas, 4 ejes\n", kNumTestTypes);

    // ── 1. Banco de pruebas: 8 tipos, eje único, duración ───────────────
    {
        check(kNumTestTypes == 8, "8 pruebas breves de calibracion");
        // Cada prueba tiene nombre, eje y duración nominal del plan.
        const int kDurations[8] = {15, 10, 10, 10, 15, 10, 10, 5};
        bool allOk = true;
        for (std::size_t t = 0; t < kNumTestTypes; ++t) {
            TestType type = static_cast<TestType>(t);
            if (testName(type)[0] == '\0') allOk = false;
            if (axisName(testAxis(type))[0] == '\0') allOk = false;
            if (testDurationMin(type) != kDurations[t]) allOk = false;
        }
        check(allOk, "8 pruebas con nombre, eje y duracion del plan");
        // Mapa prueba → eje (documentado en el header).
        check(testAxis(TestType::kAbxAdaptive) == Axis::kConservation,
              "ABX -> conservacion");
        check(testAxis(TestType::kDetection) == Axis::kConservation,
              "Deteccion -> conservacion");
        check(testAxis(TestType::kDepthComparison) == Axis::kConservation,
              "Profundidad -> conservacion");
        check(testAxis(TestType::kTexturePerception) == Axis::kConservation,
              "Textura -> conservacion");
        check(testAxis(TestType::kFatigueSensitivity) == Axis::kContextual,
              "Fatiga -> adaptacion contextual");
        check(testAxis(TestType::kTimbralPreference) == Axis::kPreference,
              "Preferencia timbrica -> preferencia");
        check(testAxis(TestType::kLatencyTolerance) ==
                  Axis::kPhysicalAccuracy,
              "Latencia -> exactitud fisica");
        check(testAxis(TestType::kReferenceMemory) == Axis::kPreference,
              "Memoria de referencia -> preferencia");
        check(std::strcmp(axisName(Axis::kPreference),
                          "preferencia del oyente") == 0,
              "nombre eje preferencia");
    }

    // ── 2. Registro: estímulo, respuesta, tiempo, eje ───────────────────
    {
        TrialLog log;
        check(log.append(rec(TestType::kAbxAdaptive, 1, true)),
              "ensayo ABX valido: se registra");
        check(log.append(rec(TestType::kDetection, 2, false, 850.0)),
              "ensayo Deteccion valido: se registra");
        check(log.size() == 2, "log con 2 ensayos");
        check(log.countByTest(TestType::kAbxAdaptive) == 1,
              "conteo por prueba ABX = 1");
        check(log.countByAxis(Axis::kConservation) == 2,
              "conteo por eje conservacion = 2");
        check(log.at(0).stimulusId == 1 && log.at(0).correct &&
                  near(log.at(0).responseTimeMs, 1200.0, 1e-9),
              "registro guarda estimulo, respuesta y tiempo");
        // Eje mezclado: la prueba queda invalidada, no se guarda.
        TrialRecord mixed = rec(TestType::kAbxAdaptive, 3, true);
        mixed.axis = Axis::kPreference;  // ABX es conservacion, no preferencia
        check(!isValidRecord(mixed), "registro con eje mezclado: invalido");
        check(!log.append(mixed), "log rechaza eje mezclado");
        check(log.size() == 2, "log sigue con 2 ensayos tras el rechazo");
        // Tiempo negativo o NaN: inválido.
        TrialRecord badTime = rec(TestType::kDetection, 4, true, -5.0);
        check(!log.append(badTime), "log rechaza tiempo negativo");
        TrialRecord nanTime = rec(TestType::kDetection, 5, true,
                                  std::nan(""));
        check(!log.append(nanTime), "log rechaza tiempo NaN");
    }

    // ── 3. Serialización ida y vuelta ────────────────────────────────────
    {
        TrialLog log;
        log.append(rec(TestType::kAbxAdaptive, 10, true, 1100.0));
        log.append(rec(TestType::kTimbralPreference, 11, false, 2300.0));
        log.append(rec(TestType::kLatencyTolerance, 12, true, 950.5));
        std::string s = log.serialize();
        TrialLog back;
        check(back.parse(s), "parse del log serializado: ok");
        check(back.size() == 3, "log restaurado con 3 ensayos");
        bool same = back.at(0).stimulusId == 10 && back.at(0).correct &&
                    near(back.at(0).responseTimeMs, 1100.0, 1e-9) &&
                    back.at(1).type == TestType::kTimbralPreference &&
                    !back.at(1).correct &&
                    back.at(2).type == TestType::kLatencyTolerance &&
                    back.at(2).axis == Axis::kPhysicalAccuracy;
        check(same, "serializacion conserva estimulo/respuesta/tiempo/eje");
        TrialLog bad;
        check(!bad.parse("99,0,1,1,100.0,1,1000\n"),
              "parse con tipo 99: false");
        check(bad.size() == 0, "parse fallido no modifica el log");
        check(!bad.parse("0,2,1,1,100.0,1,1000\n"),
              "parse con eje mezclado (ABX/preferencia): false");
    }

    // ── 4. Modelo de sensibilidad: prior de azar sin baseline ───────────
    {
        SensitivityModel model;
        check(!model.isCalibrated(), "modelo sin baseline: no calibrado");
        SensitivityEstimate e = model.predict(TestType::kAbxAdaptive, 3.0);
        check(!e.calibrated && e.confidence == 0.0 &&
                  e.pDetect == 0.5 && e.pBothers == 0.5,
              "sin baseline: prior de azar 0.5/0.5, confianza 0");
        // Con <20 ensayos se niega a calibrar (fail-closed).
        TrialLog few;
        for (int i = 0; i < 5; ++i) {
            few.append(rec(TestType::kAbxAdaptive, 100 + i, true));
        }
        check(!model.fit(TestType::kAbxAdaptive, few),
              "fit con 5 ensayos: se niega");
        check(!model.isCalibrated(),
              "tras fit rechazado: sigue sin calibrar");
        // Con 30 ensayos (24 correctos) calibra con la tasa empírica.
        TrialLog many;
        for (int i = 0; i < 30; ++i) {
            many.append(rec(TestType::kAbxAdaptive, 200 + i, i < 24));
        }
        check(model.fit(TestType::kAbxAdaptive, many),
              "fit con 30 ensayos: calibra");
        check(model.isCalibrated(), "modelo calibrado tras fit");
        SensitivityEstimate e2 =
            model.predict(TestType::kAbxAdaptive, 3.0);
        check(e2.calibrated && near(e2.pDetect, 0.8, 1e-12),
              "pDetect calibrado = 24/30 = 0.8");
        check(e2.confidence > 0.0 && e2.confidence <= 0.9,
              "confianza calibrada en (0, 0.9]");
        // El modelo no calibrado de otro tipo sigue en azar si no se
        // ajustó ese tipo: el fit es por tipo de prueba.
        SensitivityModel fresh;
        TrialLog mixed;
        for (int i = 0; i < 25; ++i) {
            mixed.append(rec(TestType::kDetection, 300 + i, true));
        }
        check(fresh.fit(TestType::kDetection, mixed),
              "fit Deteccion con 25 ensayos: calibra");
        check(near(fresh.predict(TestType::kDetection, 1.0).pDetect, 1.0,
                   1e-12),
              "pDetect Deteccion 25/25 = 1.0");
    }

    std::printf("P10: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
