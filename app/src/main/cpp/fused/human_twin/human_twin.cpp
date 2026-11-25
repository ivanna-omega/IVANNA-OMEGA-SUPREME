// human_twin.cpp — P10.1 banco de pruebas + modelo de sensibilidad.
#include "human_twin.hpp"

#include <cmath>
#include <cstdio>

namespace ivanna::fused::human_twin {

Axis testAxis(TestType type) noexcept {
    switch (type) {
        case TestType::kAbxAdaptive:
        case TestType::kDetection:
        case TestType::kDepthComparison:
        case TestType::kTexturePerception:
            return Axis::kConservation;
        case TestType::kFatigueSensitivity:
            return Axis::kContextual;
        case TestType::kTimbralPreference:
        case TestType::kReferenceMemory:
            return Axis::kPreference;
        case TestType::kLatencyTolerance:
            return Axis::kPhysicalAccuracy;
    }
    return Axis::kConservation;
}

const char* testName(TestType type) noexcept {
    switch (type) {
        case TestType::kAbxAdaptive: return "ABX adaptativo";
        case TestType::kDetection: return "Deteccion de diferencias";
        case TestType::kDepthComparison: return "Comparacion de profundidad";
        case TestType::kTexturePerception: return "Percepcion de textura";
        case TestType::kFatigueSensitivity: return "Sensibilidad a fatiga";
        case TestType::kTimbralPreference: return "Preferencia timbrica";
        case TestType::kLatencyTolerance: return "Tolerancia a latencia";
        case TestType::kReferenceMemory: return "Memoria de referencia";
    }
    return "Desconocida";
}

const char* axisName(Axis axis) noexcept {
    switch (axis) {
        case Axis::kConservation: return "conservacion de la obra";
        case Axis::kPhysicalAccuracy: return "exactitud fisica";
        case Axis::kPreference: return "preferencia del oyente";
        case Axis::kContextual: return "adaptacion contextual";
    }
    return "desconocido";
}

int testDurationMin(TestType type) noexcept {
    switch (type) {
        case TestType::kAbxAdaptive: return 15;
        case TestType::kDetection: return 10;
        case TestType::kDepthComparison: return 10;
        case TestType::kTexturePerception: return 10;
        case TestType::kFatigueSensitivity: return 15;
        case TestType::kTimbralPreference: return 10;
        case TestType::kLatencyTolerance: return 10;
        case TestType::kReferenceMemory: return 5;
    }
    return 0;
}

bool isValidRecord(const TrialRecord& r) noexcept {
    if (r.axis != testAxis(r.type)) return false;  // eje mezclado
    if (!std::isfinite(r.responseTimeMs) || r.responseTimeMs < 0.0) {
        return false;
    }
    return true;
}

bool TrialLog::append(const TrialRecord& r) noexcept {
    if (!isValidRecord(r)) return false;
    try {
        trials_.push_back(r);
    } catch (...) {
        return false;
    }
    return true;
}

std::size_t TrialLog::countByTest(TestType type) const noexcept {
    std::size_t n = 0;
    for (const TrialRecord& r : trials_) {
        if (r.type == type) ++n;
    }
    return n;
}

std::size_t TrialLog::countByAxis(Axis axis) const noexcept {
    std::size_t n = 0;
    for (const TrialRecord& r : trials_) {
        if (r.axis == axis) ++n;
    }
    return n;
}

std::string TrialLog::serialize() const {
    std::string out;
    char buf[256];
    for (const TrialRecord& r : trials_) {
        std::snprintf(buf, sizeof(buf), "%u,%u,%u,%d,%.3f,%d,%llu\n",
                      static_cast<unsigned>(r.type),
                      static_cast<unsigned>(r.axis), r.stimulusId,
                      r.response, r.responseTimeMs,
                      r.correct ? 1 : 0,
                      static_cast<unsigned long long>(r.timestampMs));
        out += buf;
    }
    return out;
}

bool TrialLog::parse(const std::string& text) noexcept {
    std::vector<TrialRecord> parsed;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        std::string line = text.substr(
            pos, (eol == std::string::npos) ? std::string::npos : eol - pos);
        if (!line.empty()) {
            unsigned typeU = 0, axisU = 0, stim = 0, correctU = 0;
            int response = 0;
            double timeMs = 0.0;
            unsigned long long ts = 0;
            int n = std::sscanf(line.c_str(), "%u,%u,%u,%d,%lf,%u,%llu",
                                &typeU, &axisU, &stim, &response, &timeMs,
                                &correctU, &ts);
            if (n != 7 || typeU >= kNumTestTypes || axisU > 3 ||
                correctU > 1) {
                return false;
            }
            TrialRecord r;
            r.type = static_cast<TestType>(typeU);
            r.axis = static_cast<Axis>(axisU);
            r.stimulusId = stim;
            r.response = response;
            r.responseTimeMs = timeMs;
            r.correct = correctU != 0;
            r.timestampMs = ts;
            if (!isValidRecord(r)) return false;
            parsed.push_back(r);
        }
        if (eol == std::string::npos) break;
        pos = eol + 1;
    }
    trials_ = parsed;
    return true;
}

SensitivityEstimate SensitivityModel::predict(
    TestType type, double alterationDb) const noexcept {
    (void)type;
    (void)alterationDb;
    SensitivityEstimate e;
    if (!calibrated_) {
        // Prior de azar: sin baseline no se sabe nada de Esse.
        e.pDetect = 0.5;
        e.pBothers = 0.5;
        e.confidence = 0.0;
        e.calibrated = false;
        return e;
    }
    e.pDetect = pDetect_;
    e.pBothers = pBothers_;
    e.confidence = confidence_;
    e.calibrated = true;
    return e;
}

bool SensitivityModel::fit(TestType type, const TrialLog& log) noexcept {
    std::size_t n = log.countByTest(type);
    if (n < kMinTrialsToCalibrate) return false;  // fail-closed
    std::size_t correct = 0;
    for (std::size_t i = 0; i < log.size(); ++i) {
        const TrialRecord& r = log.at(i);
        if (r.type == type && r.correct) ++correct;
    }
    double rate = static_cast<double>(correct) / static_cast<double>(n);
    // Estimador crudo: tasa empírica de acierto. La confianza crece con
    // el número de ensayos (máx. 0.9): con 20 ensayos es 0.2, con 100
    // es 0.9. No es un ajuste psicométrico completo; es el piso que el
    // plan exige antes de afirmar nada sobre Esse.
    fittedType_ = type;
    pDetect_ = rate;
    pBothers_ = rate;  // sin datos de molestia separados, no se distingue
    confidence_ = 0.9 * (static_cast<double>(n) < 100.0
                             ? static_cast<double>(n) / 100.0
                             : 1.0);
    calibrated_ = true;
    return true;
}

}  // namespace ivanna::fused::human_twin
