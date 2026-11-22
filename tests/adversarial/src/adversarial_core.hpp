// adversarial_core.hpp — Núcleo del banco adversarial de escucha y medición.
// Generador de estímulos + harness de medición, C++17, sin dependencias externas.
// Adaptado del diseño de luna (gpt-5.6-luna, 2026-11-21) y completado aquí:
// el diseño original omitía el lector WAV y dejaba el `measure` como stub.
//
// El banco NO procesa audio: genera WAV de entrada y mide pares (entrada, salida)
// que produce el DUT (el motor). Corre en host, sin teléfono.
#pragma once

#include <complex>
#include <cstdint>
#include <string>
#include <vector>

#include "wav.hpp"

namespace adv {

// ---------- utilidades ----------
double dbfs(double x);               // 20*log10(max(|x|, 1e-15))
double amp_from_db(double db);       // 10^(db/20)
double rms(const std::vector<double>& x, size_t a, size_t b);
double peak_abs(const std::vector<double>& x, size_t a, size_t b);
bool all_finite(const Signal& s);

// FFT radix-2 in-place (iterativa). inverse=true → IFFT con escala 1/N.
void fft(std::vector<std::complex<double>>& a, bool inverse);
size_t next_pow2(size_t n);

// ---------- generador ----------
// Variantes por caso (el diseño exige tres por caso):
//   Mono   — mismo canal duplicado en L y R.
//   Stereo — canales independientes donde el caso lo permite (fases/ruido distintos).
//   Seed   — misma estructura, semilla distinta (para validación / escucha ciega).
enum class Variant { Mono, Stereo, Seed };

struct GenOptions {
    uint32_t seed = 0;                 // 0 = nominal de la tabla; 1..999 = variantes
    Variant variant = Variant::Mono;   // (ver arriba)
};

std::vector<std::string> case_ids();
double case_duration_s(const std::string& id);   // duración nominal de la tabla (s)
std::string case_description(const std::string& id);  // una línea: qué falla busca
Signal generate_case(const std::string& id, const GenOptions& opt = {});

// ---------- umbrales de fallo ----------
// Umbrales OPERATIVOS iniciales (no metas de calidad): un DUT correcto pasa con
// margen y uno roto falla con claridad. Cada uno se justifica en README.md.
// La latencia algorítmica fija del motor NO se penaliza: se compara contra una
// latencia dorada (baseline) de la misma build y ruta; fallo = desvío > 1 muestra.
struct Thresholds {
    double latency_baseline_samples = 0.0;  // latencia dorada; 0 = sin baseline
    bool has_latency_baseline = false;      // true = comparar contra la baseline
    double latency_deviation_max_samples = 1.0;
    double latency_abs_max_samples = 8192.0;  // >170 ms es un DUT roto, con o sin baseline
    double jitter_p99_max_samples = 32.0;     // JNI (0.667 ms)
    double jitter_p99_max_samples_daemon = 64.0;  // daemon (1.333 ms)
    double phase_error_max_deg = 2.0;         // 100 Hz–18 kHz
    double phase_error_lf_max_deg = 5.0;      // 20–100 Hz
    double overshoot_max_db = 0.5;            // sobre el baseline / sobre la referencia
    double peak_max_dbfs = -0.1;              // pico absoluto de salida
    double thdn_max_db = -80.0;               // seno a -12 dBFS
    double residual_silence_max_dbfs = -90.0;  // en silencio, 100 ms tras la última señal
    double residual_modulation_max_dbfs = -70.0;  // entrada constante
    double click_energy_loss_max_db = 1.0;    // pérdida de energía del transitorio
    double centroid_shift_max_hops = 0.25;    // 0.25 hop = 32 muestras
    double coherence_min = 0.995;             // 100 Hz–12 kHz
    double mono_residual_max_dbfs = -55.0;    // phase_01: la suma mono debe cancelar
    double reconvergence_max_hops_jni = 3.0;
    double reconvergence_max_hops_daemon = 8.0;
    double state_divergence_max_hops = 2.0;
    double state_persistence_max_ms = 100.0;
    double stale_snapshot_max_hops = 2.0;
};

// ---------- medición ----------
struct MeasureOptions {
    double freq_hz = 1000.0;   // fundamental para THD+N y error de fase
    std::string case_id;       // "" = genérico; habilita chequeos del caso (p. ej. phase_01)
    std::string route = "jni";  // "jni" o "daemon" (umbrales de jitter/reconvergencia)
    bool expect_low_coherence = false;  // no penalizar baja coherencia (decorrelación intencionada)
};

struct MetricResult {
    std::string name;
    double value = 0.0;
    bool applicable = false;  // false = no se pudo medir en este par (no es fallo)
    bool pass = true;
    std::string detail;
};

struct MeasureReport {
    std::vector<MetricResult> metrics;
    bool all_pass() const;
    const MetricResult* find(const std::string& name) const;
    std::string to_json() const;  // un objeto JSON, una línea por métrica
};

// Mide (ref, dut). No lanza por contenido: los fallos van en el reporte.
// Lanza std::runtime_error solo si las longitudes difieren (par inválido).
MeasureReport measure(const Signal& ref, const Signal& dut, const Thresholds& th,
                      const MeasureOptions& opt);

// Diferencia de fase entre L y R a una frecuencia, en [a, b) (grados).
// Útil para verificar phase_02 (R retardado) y binaural_01 (ITD).
double interchannel_phase_deg(const Signal& s, double hz, size_t a, size_t b);

// ---------- eventos de ruta (events.jsonl) ----------
// Esquema que el DUT debe producir (una línea JSON por evento):
//   {"t_capture_ns":123, "route":"jni", "requested":{"mode":3,"strength":0.8},
//    "applied":{"mode":2,"strength":0.75}, "connected":true, "snapshot_id":1842}
// El parser es mínimo y solo entiende ese esquema (documentado en README.md).
struct StateEvent {
    long long t_ns = 0;
    std::string route;
    int req_mode = 0;
    double req_strength = 0.0;
    int app_mode = 0;
    double app_strength = 0.0;
    bool connected = true;
    long long snapshot_id = 0;
};

std::vector<StateEvent> read_events(const std::string& path);

struct DivergenceReport {
    double max_strength_diff = 0.0;
    int mode_mismatch_count = 0;
    double max_gap_ms = 0.0;     // mayor intervalo divergente (ms)
    double max_gap_hops = 0.0;   // mismo intervalo en hops de 128 muestras
    bool stale_snapshot = false;  // snapshot congelado > 2 hops con estado pedido distinto
    double reconvergence_hops = -1.0;  // -1 = n/a (sin fallo en el registro)
    bool pass = true;
    std::string detail;
};

DivergenceReport analyze_events(const std::vector<StateEvent>& ev, const Thresholds& th,
                                const std::string& route);

}  // namespace adv
