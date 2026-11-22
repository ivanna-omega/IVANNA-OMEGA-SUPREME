// adversarial_harness.cpp — CLI del harness de medición del banco adversarial.
// Mide un par (referencia, salida del DUT) y opcionalmente un events.jsonl.
// Imprime un JSON por stdout y un resumen humano por stderr.
// Salida: 0 = todo pasa, 1 = algún fallo, 2 = error de uso/IO.
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

#include "adversarial_core.hpp"

namespace {
using namespace adv;

void usage(const char* prog) {
    std::cerr << "Uso:\n"
              << "  " << prog << " measure <ref.wav> <dut.wav> [opciones]\n"
              << "Opciones:\n"
              << "  --freq HZ             fundamental para THD+N y fase (def. 1000)\n"
              << "  --case ID             caso del banco (habilita chequeos del caso)\n"
              << "  --route jni|daemon    ruta del DUT (def. jni)\n"
              << "  --baseline N          latencia dorada en muestras (activa la comparación)\n"
              << "  --jitter-limit N      límite de jitter en muestras (def. 32 JNI / 64 daemon)\n"
              << "  --thdn-limit DB       límite de THD+N en dB (def. -80)\n"
              << "  --events FILE         events.jsonl del DUT (divergencia/reconvergencia)\n"
              << "  --expect-latency N    fallo si la latencia medida != N (para tests)\n"
              << "  --expect-phase DEG    fallo si |fase - DEG| > 5 (para tests)\n"
              << "  --expect-low-coherence  no penalizar baja coherencia\n";
}

std::string fmt_num(double v) {
    if (!std::isfinite(v)) return "null";
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 4 || std::string(argv[1]) != "measure") {
            usage(argv[0]);
            return 2;
        }
        const std::string refPath = argv[2];
        const std::string dutPath = argv[3];
        MeasureOptions opt;
        Thresholds th;
        std::string eventsPath;
        bool hasExpectLatency = false;
        double expectLatency = 0.0;
        bool hasExpectPhase = false;
        double expectPhase = 0.0;
        for (int i = 4; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--freq" && i + 1 < argc)
                opt.freq_hz = std::stod(argv[++i]);
            else if (a == "--case" && i + 1 < argc)
                opt.case_id = argv[++i];
            else if (a == "--route" && i + 1 < argc)
                opt.route = argv[++i];
            else if (a == "--baseline" && i + 1 < argc) {
                th.latency_baseline_samples = std::stod(argv[++i]);
                th.has_latency_baseline = true;
            } else if (a == "--jitter-limit" && i + 1 < argc) {
                th.jitter_p99_max_samples = std::stod(argv[++i]);
                th.jitter_p99_max_samples_daemon = std::stod(argv[i]);
            } else if (a == "--thdn-limit" && i + 1 < argc) {
                th.thdn_max_db = std::stod(argv[++i]);
            } else if (a == "--events" && i + 1 < argc) {
                eventsPath = argv[++i];
            } else if (a == "--expect-latency" && i + 1 < argc) {
                hasExpectLatency = true;
                expectLatency = std::stod(argv[++i]);
            } else if (a == "--expect-phase" && i + 1 < argc) {
                hasExpectPhase = true;
                expectPhase = std::stod(argv[++i]);
            } else if (a == "--expect-low-coherence") {
                opt.expect_low_coherence = true;
            } else {
                std::cerr << "opción desconocida: " << a << "\n";
                usage(argv[0]);
                return 2;
            }
        }
        if (opt.route != "jni" && opt.route != "daemon") {
            std::cerr << "route debe ser jni o daemon\n";
            return 2;
        }

        Signal ref = read_wav(refPath);
        Signal dut = read_wav(dutPath);
        MeasureReport report = measure(ref, dut, th, opt);
        bool allPass = report.all_pass();

        // Chequeos de expectativa (para tests del propio harness).
        if (hasExpectLatency) {
            const MetricResult* m = report.find("latency_samples");
            const double got = m ? m->value : 0.0;
            if (!m || !m->applicable || std::abs(got - expectLatency) > 0.5) {
                std::cerr << "EXPECT-FAIL: latencia medida " << got << ", esperada "
                          << expectLatency << "\n";
                allPass = false;
            }
        }
        if (hasExpectPhase) {
            const MetricResult* m = report.find("phase_error_deg");
            const double got = m ? m->value : 0.0;
            double d = std::abs(got - expectPhase);
            while (d > 180.0) d -= 360.0;
            if (!m || !m->applicable || std::abs(d) > 5.0) {
                std::cerr << "EXPECT-FAIL: fase medida " << got << ", esperada " << expectPhase
                          << "\n";
                allPass = false;
            }
        }

        // Eventos de ruta (divergencia / reconvergencia / snapshot obsoleto).
        const bool haveEvents = !eventsPath.empty();
        DivergenceReport div;
        if (haveEvents) {
            auto ev = read_events(eventsPath);
            div = analyze_events(ev, th, opt.route);
            if (!div.pass) allPass = false;
        }

        // JSON por stdout.
        std::cout << "{\"measure\":" << report.to_json();
        if (haveEvents) {
            std::cout << ",\"events\":{\"max_strength_diff\":" << fmt_num(div.max_strength_diff)
                      << ",\"mode_mismatch_count\":" << div.mode_mismatch_count
                      << ",\"max_gap_ms\":" << fmt_num(div.max_gap_ms)
                      << ",\"max_gap_hops\":" << fmt_num(div.max_gap_hops)
                      << ",\"stale_snapshot\":" << (div.stale_snapshot ? "true" : "false")
                      << ",\"reconvergence_hops\":" << fmt_num(div.reconvergence_hops)
                      << ",\"pass\":" << (div.pass ? "true" : "false") << "}";
        }
        std::cout << ",\"all_pass\":" << (allPass ? "true" : "false") << "}\n";

        // Resumen humano por stderr.
        for (const auto& m : report.metrics) {
            if (!m.applicable) continue;
            std::cerr << (m.pass ? "PASS " : "FAIL ") << m.name << " = " << fmt_num(m.value)
                      << "  (" << m.detail << ")\n";
        }
        if (haveEvents) {
            std::cerr << (div.pass ? "PASS " : "FAIL ") << "events: " << div.detail << "\n";
        }
        std::cerr << "RESULT: " << (allPass ? "PASS" : "FAIL") << "\n";
        return allPass ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 2;
    }
}
