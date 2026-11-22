// adversarial_core.cpp (parte 1/2) — Utilidades, FFT y generador de estímulos.
// Adaptado del diseño de luna y completado: el diseño original omitía el lector
// WAV y dejaba el `measure` como stub. Aquí todo está implementado y verificado
// por tests host (ver tests/test_adversarial_harness.cpp).
//
// Reglas de diseño del generador:
//  - Todo determinista: la aleatoriedad viene de mt19937 con semilla derivada del
//    ID del caso + la semilla pedida (0 = nominal de la tabla).
//  - Semillas: entrenamiento 0-99, validación 100-199, escucha ciega 200-999.
//  - 48 kHz, estéreo. La variante Mono duplica el mismo canal; Stereo usa
//    semillas independientes por canal donde el caso tiene azar; Seed cambia la
//    semilla (misma estructura). Los casos deterministas son idénticos en las
//    tres variantes (documentado, no es un bug).
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "adversarial_core.hpp"

namespace adv {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kHopMs = 1000.0 * static_cast<double>(kHop) / kFs;  // 2.6667 ms

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

}  // namespace

// ---------- utilidades ----------
double dbfs(double x) { return 20.0 * std::log10(std::max(std::abs(x), 1e-15)); }

double amp_from_db(double db) { return std::pow(10.0, db / 20.0); }

double rms(const std::vector<double>& x, size_t a, size_t b) {
    b = std::min(b, x.size());
    if (b <= a) return 0.0;
    double s = 0.0;
    for (size_t n = a; n < b; ++n) s += x[n] * x[n];
    return std::sqrt(s / static_cast<double>(b - a));
}

double peak_abs(const std::vector<double>& x, size_t a, size_t b) {
    b = std::min(b, x.size());
    double p = 0.0;
    for (size_t n = a; n < b; ++n) p = std::max(p, std::abs(x[n]));
    return p;
}

bool all_finite(const Signal& s) {
    for (double v : s.l)
        if (!std::isfinite(v)) return false;
    for (double v : s.r)
        if (!std::isfinite(v)) return false;
    return true;
}

size_t next_pow2(size_t n) {
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

void fft(std::vector<std::complex<double>>& a, bool inverse) {
    const size_t n = a.size();
    if (n == 0 || (n & (n - 1)) != 0)
        throw std::runtime_error("fft: el tamaño debe ser potencia de 2");
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = 2.0 * kPi / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t j = 0; j < len / 2; ++j) {
                const std::complex<double> u = a[i + j];
                const std::complex<double> v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
    if (inverse)
        for (auto& x : a) x /= static_cast<double>(n);
}

// ---------- generador ----------
namespace {

struct CaseInfo {
    const char* id;
    double duration_s;
    const char* desc;
};

const CaseInfo kCases[] = {
    {"click_01", 3.0,
     "Click de 1 muestra a -0.1 dBFS en 1.0 s: smearing, limiter lento, pérdida de transitorio."},
    {"impulse_01", 3.0,
     "Impulso de 1 muestra a -1 dBFS: respuesta impulsional, ringing, overshoot."},
    {"impulse_02", 3.0,
     "Impulso de 8 muestras con ventana Hann, energía -3 dBFS: ataque realista y preservación temporal."},
    {"train_01", 4.0,
     "Tren de 4 ráfagas a 2/4/8/16 kHz, 250 ms, niveles 0/-3/-6/-12 dB: estado dependiente del historial."},
    {"bass_01", 15.0,
     "Seno 20 Hz a -6 dBFS, 12 s: inestabilidad subgrave, DC, saturación del integrador."},
    {"bass_02", 15.0,
     "30+40+60 Hz a -12 dBFS, fases 0/pi/pi/2: intermodulación, modulación de ganancia."},
    {"sibilant_01", 5.0,
     "Ruido de banda 5.5-10.5 kHz, envolvente 8 ms ataque / 70 ms release, -12 dBFS, 4 ráfagas: sibilancia, aliasing, aspereza."},
    {"dense_01", 10.0,
     "32 senos logarítmicos 80 Hz-18 kHz, fases aleatorias, crest factor ~3 dB: error acumulado, THD+N, modulación residual."},
    {"dense_02", 30.0,
     "Ruido rosa + tonos 60/250/1k/4k/12k Hz, RMS -14 dBFS: comportamiento con material denso realista."},
    {"phase_01", 5.0,
     "L = seno 1 kHz, R = seno 1 kHz invertido: coherencia intercanal; la suma mono debe cancelar."},
    {"phase_02", 5.0,
     "L/R iguales con R retardado 0.25 ms (1-3 s) y 0.5 ms (3-5 s): desfase temporal y errores de fase por canal."},
    {"binaural_01", 5.0,
     "Pulso 700 Hz con ITD +-0.30 ms e ILD +-6 dB: preservación espacial, estados independientes por canal."},
    {"silence_01", 5.0,
     "Tono 1 kHz -12 dBFS 1 s, silencio 500 ms, tono de nuevo: colas, memoria residual, reactivación."},
    {"noise_imp_01", 10.0,
     "200 impulsos aleatorios de 1-3 muestras, -30 a -1 dBFS: robustez frente a ruido impulsivo."},
    {"level_01", 6.0,
     "Escalones -60/-40/-24/-12/-3/-0.1 dBFS, 500 ms cada uno, seno 1 kHz: attack/release, cuantización, clipping."},
    {"level_02", 5.0,
     "Rampas -60 a -0.1 dBFS en 50 ms y vuelta: overshoot, bombeo, estabilidad del control."},
    {"ood_01", 8.0,
     "DC 0.2 + seno 5 Hz + seno 23.5 kHz, pico total -6 dBFS: supuestos fuera de distribución, filtros mal condicionados."},
    {"ood_02", 5.0,
     "Seno 997 Hz a amplitud 1.05 recortado a +-1: protección ante entrada ya distorsionada."},
    {"route_01", 5.0,
     "Estímulo con marcador (tono 1 kHz + click en 1.0 s); el runner desconecta la Ruta A 300 ms: recuperación JNI."},
    {"route_02", 5.0,
     "Mismo estímulo; el runner congela el snapshot del daemon 500 ms y luego salta de estado: estado obsoleto y reconvergencia."},
    {"route_03", 10.0,
     "Mismo estímulo (10 s); el runner alimenta Ruta A y B con snapshots desplazados 1-4 hops: divergencia entre rutas."},
};

const CaseInfo* find_case(const std::string& id) {
    for (const auto& c : kCases)
        if (id == c.id) return &c;
    return nullptr;
}

void add_sine(std::vector<double>& x, double hz, double level_db, double start_s,
              double dur_s, double phase = 0.0) {
    const size_t a = static_cast<size_t>(start_s * kFs);
    const size_t b = std::min(x.size(), a + static_cast<size_t>(dur_s * kFs));
    const double g = amp_from_db(level_db);
    for (size_t n = a; n < b; ++n) {
        const double t = static_cast<double>(n) / kFs;
        x[n] += g * std::sin(2.0 * kPi * hz * t + phase);
    }
}

void add_sine_amp(std::vector<double>& x, double hz, double amp, double start_s,
                  double dur_s, double phase = 0.0) {
    const size_t a = static_cast<size_t>(start_s * kFs);
    const size_t b = std::min(x.size(), a + static_cast<size_t>(dur_s * kFs));
    for (size_t n = a; n < b; ++n) {
        const double t = static_cast<double>(n) / kFs;
        x[n] += amp * std::sin(2.0 * kPi * hz * t + phase);
    }
}

void add_sine_delayed(std::vector<double>& x, double hz, double level_db, double start_s,
                      double dur_s, double delay_samples, double phase = 0.0) {
    const size_t a = static_cast<size_t>(start_s * kFs);
    const size_t b = std::min(x.size(), a + static_cast<size_t>(dur_s * kFs));
    const double g = amp_from_db(level_db);
    for (size_t n = a; n < b; ++n) {
        const double t = (static_cast<double>(n) - delay_samples) / kFs;
        x[n] += g * std::sin(2.0 * kPi * hz * t + phase);
    }
}

void add_click(std::vector<double>& x, size_t n, double level_db) {
    if (n < x.size()) x[n] += amp_from_db(level_db);
}

// Ráfaga senoidal con ventana Hann (transitorio de banda limitada).
void add_burst(std::vector<double>& x, double hz, double level_db, double start_s,
               double dur_s, double phase = 0.0) {
    const size_t a = static_cast<size_t>(start_s * kFs);
    const size_t b = std::min(x.size(), a + static_cast<size_t>(dur_s * kFs));
    if (b <= a) return;
    const double g = amp_from_db(level_db);
    const size_t len = b - a;
    for (size_t i = 0; i < len; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, len - 1));
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * u));
        // Tiempo RELATIVO al inicio de la ráfaga: al desplazar el inicio, la
        // ráfaga completa (portadora + envolvente) se desplaza (ITD real).
        // Con tiempo absoluto solo se movería la envolvente y la fase no cambiaría.
        const double t = static_cast<double>(i) / kFs;
        x[a + i] += g * w * std::sin(2.0 * kPi * hz * t + phase);
    }
}

// Impulso de `len` muestras con ventana Hann; energía total = amp(energy_dbfs)^2.
void add_impulse_hann(std::vector<double>& x, size_t start, size_t len, double energy_dbfs) {
    double sum_w2 = 0.0;
    for (size_t i = 0; i < len; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, len - 1));
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * u));
        sum_w2 += w * w;
    }
    const double s = amp_from_db(energy_dbfs) / std::sqrt(std::max(sum_w2, 1e-30));
    for (size_t i = 0; i < len; ++i) {
        if (start + i >= x.size()) break;
        const double u = static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, len - 1));
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * u));
        x[start + i] += s * w;
    }
}

// Ruido conformado por FFT: blanco -> FFT -> forma espectral -> IFFT.
// pink=true: 1/f (rosa, 1/sqrt(f) en amplitud). Si no, banda [lo_hz, hi_hz].
// Devuelve el ruido normalizado a RMS 1 (la escala se pone fuera).
std::vector<double> shaped_noise(size_t n, uint32_t seed, double lo_hz, double hi_hz,
                                 bool pink) {
    const size_t N = next_pow2(n);
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0.0, 1.0);
    std::vector<std::complex<double>> X(N);
    for (size_t i = 0; i < n; ++i) X[i] = nd(rng);
    fft(X, false);
    for (size_t k = 0; k < N; ++k) {
        const size_t fbin = std::min(k, N - k);
        const double freq = static_cast<double>(fbin) * kFs / static_cast<double>(N);
        double scale = 0.0;
        if (pink) {
            scale = (fbin == 0) ? 0.0 : 1.0 / std::sqrt(static_cast<double>(fbin));
        } else {
            scale = (freq >= lo_hz && freq <= hi_hz) ? 1.0 : 0.0;
        }
        X[k] *= scale;
    }
    fft(X, true);
    std::vector<double> out(n);
    for (size_t i = 0; i < n; ++i) out[i] = X[i].real();
    const double r = rms(out, 0, out.size());
    if (r > 1e-12)
        for (auto& v : out) v /= r;
    return out;
}

// Envolvente de sibilancia: ataque lineal 8 ms, release lineal 70 ms.
double sibilant_env(double t_s) {
    if (t_s < 0.0) return 0.0;
    if (t_s < 0.008) return t_s / 0.008;
    if (t_s < 0.078) return 1.0 - (t_s - 0.008) / 0.070;
    return 0.0;
}

void normalize_rms(std::vector<double>& x, double target_dbfs) {
    const double r = rms(x, 0, x.size());
    if (r < 1e-12) return;
    const double g = amp_from_db(target_dbfs) / r;
    for (auto& v : x) v *= g;
}

// Como normalize_rms, pero el RMS se calcula solo en la región [a, b)
// (para no diluirlo con silencios fuera de la región del tono).
void normalize_rms_region(std::vector<double>& x, size_t a, size_t b, double target_dbfs) {
    const double r = rms(x, a, b);
    if (r < 1e-12) return;
    const double g = amp_from_db(target_dbfs) / r;
    for (auto& v : x) v *= g;
}

double crest_factor_db(const std::vector<double>& x) {
    const double r = rms(x, 0, x.size());
    if (r < 1e-12) return 0.0;
    return dbfs(peak_abs(x, 0, x.size()) / r);
}

}  // namespace

std::vector<std::string> case_ids() {
    std::vector<std::string> out;
    for (const auto& c : kCases) out.push_back(c.id);
    return out;
}

double case_duration_s(const std::string& id) {
    const CaseInfo* c = find_case(id);
    if (!c) throw std::runtime_error("Caso desconocido: " + id);
    return c->duration_s;
}

std::string case_description(const std::string& id) {
    const CaseInfo* c = find_case(id);
    if (!c) throw std::runtime_error("Caso desconocido: " + id);
    return c->desc;
}

Signal generate_case(const std::string& id, const GenOptions& opt) {
    const CaseInfo* info = find_case(id);
    if (!info) throw std::runtime_error("Caso desconocido: " + id);

    const size_t n = static_cast<size_t>(info->duration_s * kFs);
    Signal s;
    s.channels = 2;
    s.sampleRate = kFs;
    s.l.assign(n, 0.0);
    s.r.assign(n, 0.0);

    // Semillas deterministas: base del ID xor la semilla pedida. Mono usa la
    // misma en ambos canales; Stereo/Seed usan una distinta en R.
    const uint32_t base = fnv1a(id);
    const uint32_t seedL = base ^ (opt.seed * 2654435761u);
    const uint32_t seedR = (opt.variant == Variant::Mono)
                               ? seedL
                               : (seedL ^ 0x9E3779B9u);

    if (id == "click_01") {
        add_click(s.l, 48000, -0.1);
        add_click(s.r, 48000, -0.1);
    } else if (id == "impulse_01") {
        add_click(s.l, 48000, -1.0);
        add_click(s.r, 48000, -1.0);
    } else if (id == "impulse_02") {
        add_impulse_hann(s.l, 48000, 8, -3.0);
        add_impulse_hann(s.r, 48000, 8, -3.0);
    } else if (id == "train_01") {
        const double hz[4] = {2000.0, 4000.0, 8000.0, 16000.0};
        const double lv[4] = {0.0, -3.0, -6.0, -12.0};
        for (int k = 0; k < 4; ++k) {
            const double t = 1.0 + 0.25 * k;
            add_burst(s.l, hz[k], lv[k], t, 32.0 / kFs);
            add_burst(s.r, hz[k], lv[k], t, 32.0 / kFs);
        }
    } else if (id == "bass_01") {
        add_sine(s.l, 20.0, -6.0, 1.0, 12.0);
        add_sine(s.r, 20.0, -6.0, 1.0, 12.0);
    } else if (id == "bass_02") {
        add_sine(s.l, 30.0, -12.0, 1.0, 12.0, 0.0);
        add_sine(s.l, 40.0, -12.0, 1.0, 12.0, kPi);
        add_sine(s.l, 60.0, -12.0, 1.0, 12.0, kPi / 2.0);
        add_sine(s.r, 30.0, -12.0, 1.0, 12.0, 0.0);
        add_sine(s.r, 40.0, -12.0, 1.0, 12.0, kPi);
        add_sine(s.r, 60.0, -12.0, 1.0, 12.0, kPi / 2.0);
    } else if (id == "sibilant_01") {
        // 4 ráfagas de 78 ms (8 ms ataque + 70 ms release) en 1/2/3/4 s.
        auto burst = [&](std::vector<double>& x, double start_s, uint32_t seed) {
            const size_t len = static_cast<size_t>(0.078 * kFs);
            auto nz = shaped_noise(len, seed, 5500.0, 10500.0, false);
            const double pk = peak_abs(nz, 0, nz.size());
            const double g = (pk > 1e-12) ? amp_from_db(-12.0) / pk : 0.0;
            const size_t a = static_cast<size_t>(start_s * kFs);
            for (size_t i = 0; i < len && a + i < x.size(); ++i) {
                const double t = static_cast<double>(i) / kFs;
                x[a + i] += g * sibilant_env(t) * nz[i];
            }
        };
        for (int k = 0; k < 4; ++k) {
            burst(s.l, 1.0 + k, seedL + static_cast<uint32_t>(k));
            burst(s.r, 1.0 + k, seedR + static_cast<uint32_t>(k));
        }
    } else if (id == "dense_01") {
        // 32 senos logarítmicos; se prueban 10 semillas y se queda la de crest
        // factor más cercano a 3 dB (objetivo del diseño).
        auto build = [&](uint32_t seed) {
            std::vector<double> x(n, 0.0);
            std::mt19937 rng(seed);
            std::uniform_real_distribution<double> ph(0.0, 2.0 * kPi);
            for (int k = 0; k < 32; ++k) {
                const double hz = 80.0 * std::pow(18000.0 / 80.0, static_cast<double>(k) / 31.0);
                add_sine(x, hz, -30.0, 1.0, 8.0, ph(rng));
            }
            // RMS -12 dBFS en la región del tono (1-9 s), no en todo el archivo
            // (los 2 s de silencio diluirían el RMS).
            normalize_rms_region(x, 48000, 432000, -12.0);
            return x;
        };
        double best = 1e9;
        for (int t = 0; t < 10; ++t) {
            auto xl = build(seedL + static_cast<uint32_t>(t));
            const double d = std::abs(crest_factor_db(xl) - 3.0);
            if (d < best) {
                best = d;
                s.l = xl;
            }
        }
        best = 1e9;
        for (int t = 0; t < 10; ++t) {
            auto xr = build(seedR + static_cast<uint32_t>(t));
            const double d = std::abs(crest_factor_db(xr) - 3.0);
            if (d < best) {
                best = d;
                s.r = xr;
            }
        }
    } else if (id == "dense_02") {
        // Ruido rosa 1-29 s + tonos 60/250/1k/4k/12k a -20 dB; RMS total -14 dBFS.
        // (LUFS aproximado por RMS; la normalización LUFS exacta es calibración
        // posterior — ver README.md.)
        const size_t len = static_cast<size_t>(28.0 * kFs);
        auto pn = shaped_noise(len, seedL, 0.0, 0.0, true);
        normalize_rms(pn, -17.0);
        for (double hz : {60.0, 250.0, 1000.0, 4000.0, 12000.0})
            add_sine(pn, hz, -20.0, 0.0, 28.0);
        normalize_rms(pn, -14.0);
        const size_t a = static_cast<size_t>(1.0 * kFs);
        for (size_t i = 0; i < len && a + i < n; ++i) {
            s.l[a + i] = pn[i];
        }
        auto pnR = shaped_noise(len, seedR, 0.0, 0.0, true);
        normalize_rms(pnR, -17.0);
        for (double hz : {60.0, 250.0, 1000.0, 4000.0, 12000.0})
            add_sine(pnR, hz, -20.0, 0.0, 28.0);
        normalize_rms(pnR, -14.0);
        for (size_t i = 0; i < len && a + i < n; ++i) {
            s.r[a + i] = pnR[i];
        }
    } else if (id == "phase_01") {
        add_sine(s.l, 1000.0, -6.0, 1.0, 4.0, 0.0);
        add_sine(s.r, 1000.0, -6.0, 1.0, 4.0, kPi);
    } else if (id == "phase_02") {
        add_sine(s.l, 1000.0, -6.0, 1.0, 4.0, 0.0);
        // R retardado 0.25 ms (12 muestras) en 1-3 s y 0.5 ms (24) en 3-5 s.
        add_sine_delayed(s.r, 1000.0, -6.0, 1.0, 2.0, 12.0);
        add_sine_delayed(s.r, 1000.0, -6.0, 3.0, 2.0, 24.0);
    } else if (id == "binaural_01") {
        // Pulso 700 Hz, 100 ms Hann. ITD 0.30 ms (cuantizado a 14 muestras =
        // 0.292 ms) e ILD 6 dB; espejo en 3.0 s (R primero).
        add_burst(s.l, 700.0, -6.0, 1.0, 0.1);
        add_burst(s.r, 700.0, -12.0, 1.0 + 0.0003, 0.1);
        add_burst(s.r, 700.0, -6.0, 3.0, 0.1);
        add_burst(s.l, 700.0, -12.0, 3.0 + 0.0003, 0.1);
    } else if (id == "silence_01") {
        add_sine(s.l, 1000.0, -12.0, 0.5, 1.0);
        add_sine(s.r, 1000.0, -12.0, 0.5, 1.0);
        add_sine(s.l, 1000.0, -12.0, 2.0, 1.0);
        add_sine(s.r, 1000.0, -12.0, 2.0, 1.0);
    } else if (id == "noise_imp_01") {
        auto fill = [&](std::vector<double>& x, uint32_t seed) {
            std::mt19937 rng(seed);
            std::uniform_int_distribution<size_t> pos(48000, 480000);
            std::uniform_real_distribution<double> lev(-30.0, -1.0);
            std::uniform_int_distribution<int> width(1, 3);
            for (int i = 0; i < 200; ++i) {
                const size_t p = pos(rng);
                const double amp = amp_from_db(lev(rng));
                const int w = width(rng);
                for (int j = 0; j < w && p + static_cast<size_t>(j) < x.size(); ++j)
                    x[p + static_cast<size_t>(j)] += amp;
            }
        };
        fill(s.l, seedL);
        fill(s.r, seedR);
    } else if (id == "level_01") {
        const double levels[6] = {-60.0, -40.0, -24.0, -12.0, -3.0, -0.1};
        for (int k = 0; k < 6; ++k) {
            const double t = 0.5 + 0.5 * k;
            add_sine(s.l, 1000.0, levels[k], t, 0.5);
            add_sine(s.r, 1000.0, levels[k], t, 0.5);
        }
    } else if (id == "level_02") {
        // Rampa en dB: -60 -> -0.1 en 50 ms, hold 1 s, rampa de vuelta en 50 ms.
        auto ramp = [&](std::vector<double>& x, double db0, double db1, double t0,
                        double dur) {
            const size_t a = static_cast<size_t>(t0 * kFs);
            const size_t b = std::min(x.size(), a + static_cast<size_t>(dur * kFs));
            if (b <= a) return;
            for (size_t n = a; n < b; ++n) {
                const double u = static_cast<double>(n - a) /
                                 static_cast<double>(std::max<size_t>(1, b - a - 1));
                const double db = db0 + (db1 - db0) * u;
                const double t = static_cast<double>(n) / kFs;
                x[n] += amp_from_db(db) * std::sin(2.0 * kPi * 1000.0 * t);
            }
        };
        add_sine(s.l, 1000.0, -60.0, 0.5, 0.5);
        add_sine(s.r, 1000.0, -60.0, 0.5, 0.5);
        ramp(s.l, -60.0, -0.1, 1.0, 0.05);
        ramp(s.r, -60.0, -0.1, 1.0, 0.05);
        add_sine(s.l, 1000.0, -0.1, 1.05, 1.0);
        add_sine(s.r, 1000.0, -0.1, 1.05, 1.0);
        ramp(s.l, -0.1, -60.0, 2.05, 0.05);
        ramp(s.r, -0.1, -60.0, 2.05, 0.05);
        add_sine(s.l, 1000.0, -60.0, 2.10, 1.9);
        add_sine(s.r, 1000.0, -60.0, 2.10, 1.9);
    } else if (id == "ood_01") {
        // DC 0.2 + senos 5 Hz y 23.5 kHz; cada seno con amplitud (amp(-6 dB)-0.2)/2
        // para que el pico total sea -6 dBFS.
        const double a_each = (amp_from_db(-6.0) - 0.2) / 2.0;
        add_sine_amp(s.l, 5.0, a_each, 1.0, 6.0);
        add_sine_amp(s.l, 23500.0, a_each, 1.0, 6.0);
        add_sine_amp(s.r, 5.0, a_each, 1.0, 6.0);
        add_sine_amp(s.r, 23500.0, a_each, 1.0, 6.0);
        const size_t a = 48000, b = std::min(n, static_cast<size_t>(7 * 48000));
        for (size_t i = a; i < b; ++i) {
            s.l[i] += 0.2;
            s.r[i] += 0.2;
        }
    } else if (id == "ood_02") {
        const size_t a = 48000, b = std::min(n, static_cast<size_t>(5 * 48000));
        for (size_t i = a; i < b; ++i) {
            const double v = 1.05 * std::sin(2.0 * kPi * 997.0 * static_cast<double>(i) / kFs);
            s.l[i] = std::clamp(v, -1.0, 1.0);
            s.r[i] = s.l[i];
        }
    } else if (id == "route_01" || id == "route_02" || id == "route_03") {
        // Estímulo con marcador para medir latencia: tono 1 kHz -12 dBFS +
        // click a -0.1 dBFS en 1.0 s. La desconexión/congelamiento/desplazamiento
        // la ejecuta el runner del DUT (ver README.md); aquí solo va el estímulo.
        const double tone_dur = (id == "route_03") ? 8.0 : 3.0;
        add_sine(s.l, 1000.0, -12.0, 1.0, tone_dur);
        add_sine(s.r, 1000.0, -12.0, 1.0, tone_dur);
        add_click(s.l, 48000, -0.1);
        add_click(s.r, 48000, -0.1);
    } else {
        throw std::runtime_error("Caso desconocido: " + id);
    }

    return s;
}

// ---------- medición (parte 2/2) ----------

namespace {

// Primer índice donde el RMS de 1 ms (48 muestras) supera el umbral, o -1.
long find_onset(const std::vector<double>& x, double thresh_dbfs) {
    const double th = amp_from_db(thresh_dbfs);
    const size_t win = 48;
    for (size_t a = 0; a + win <= x.size(); a += win) {
        if (rms(x, a, a + win) > th) return static_cast<long>(a);
    }
    return -1;
}

// Onset a nivel de muestra: primer índice donde |x| supera el umbral 3 muestras
// seguidas (rechaza picos aislados). Es exacto a la muestra para señales con
// ataque abrupto y es invariante a inversión de fase (|x| es igual).
long find_onset_sample(const std::vector<double>& x, double thresh_dbfs) {
    const double th = amp_from_db(thresh_dbfs);
    for (size_t i = 0; i + 2 < x.size(); ++i) {
        if (std::abs(x[i]) > th && std::abs(x[i + 1]) > th && std::abs(x[i + 2]) > th)
            return static_cast<long>(i);
    }
    return -1;
}

// Correlación de |x| (invariante a inversión de fase) en un segmento,
// alrededor de un retardo aproximado (onset). Devuelve el retardo y la
// correlación normalizada en corr_out.
// La correlación de la forma de onda confunde una inversión con un retardo de
// medio periodo; |x| no (una señal invertida tiene el mismo |x|). Entre los
// retardos con correlación casi máxima se elige el más cercano al aproximado,
// para resolver la ambigüedad periódica de los tonos.
double refine_latency(const std::vector<double>& ref, const std::vector<double>& dut,
                      long approx_lag, long search, size_t seg_start, size_t seg_len,
                      double& corr_out) {
    const size_t b = std::min(seg_start + seg_len, ref.size());
    double eRef = 0.0;
    for (size_t n = seg_start; n < b; ++n) {
        const double v = std::abs(ref[n]);
        eRef += v * v;
    }
    std::vector<std::pair<long, double>> cands;
    double best = -2.0;
    for (long lag = approx_lag - search; lag <= approx_lag + search; ++lag) {
        double num = 0.0, eDut = 0.0;
        for (size_t n = seg_start; n < b; ++n) {
            const long m = static_cast<long>(n) + lag;
            const double d = (m >= 0 && static_cast<size_t>(m) < dut.size())
                                 ? std::abs(dut[static_cast<size_t>(m)])
                                 : 0.0;
            const double r = std::abs(ref[n]);
            num += r * d;
            eDut += d * d;
        }
        const double c = num / std::sqrt(std::max(eRef, 1e-30) * std::max(eDut, 1e-30));
        cands.push_back({lag, c});
        if (c > best) best = c;
    }
    long chosen = approx_lag;
    long bestDist = std::numeric_limits<long>::max();
    for (const auto& [lag, c] : cands) {
        if (c >= best - 1e-4) {
            const long d = std::abs(lag - approx_lag);
            if (d < bestDist) {
                bestDist = d;
                chosen = lag;
            }
        }
    }
    corr_out = best;
    return static_cast<double>(chosen);
}

// Latencia por correlación cruzada global (FFT). Solo se usa cuando no hay
// onset claro; puede ser ambigua en tonos periódicos (múltiplos del periodo).
double xcorr_latency(const std::vector<double>& ref, const std::vector<double>& dut,
                     int maxLag, double& corr_out) {
    const size_t n = std::max(ref.size(), dut.size());
    const size_t N = next_pow2(2 * n);
    std::vector<std::complex<double>> R(N), D(N);
    for (size_t i = 0; i < ref.size(); ++i) R[i] = ref[i];
    for (size_t i = 0; i < dut.size(); ++i) D[i] = dut[i];
    fft(R, false);
    fft(D, false);
    for (size_t i = 0; i < N; ++i) R[i] = std::conj(R[i]) * D[i];
    fft(R, true);
    double eRef = 0.0, eDut = 0.0;
    for (double v : ref) eRef += v * v;
    for (double v : dut) eDut += v * v;
    const double norm = std::sqrt(std::max(eRef, 1e-30) * std::max(eDut, 1e-30));
    double best = -2.0;
    int bestLag = 0;
    for (int lag = -maxLag; lag <= maxLag; ++lag) {
        const size_t idx = (lag >= 0) ? static_cast<size_t>(lag) : N + static_cast<size_t>(lag);
        const double c = R[idx].real() / norm;
        if (c > best) {
            best = c;
            bestLag = lag;
        }
    }
    corr_out = best;
    return static_cast<double>(bestLag);
}

// Proyección Goertzel a una frecuencia: potencia de la fundamental y total.
// La ventana debe tener un número entero de ciclos para no tener leakage.
void goertzel(const std::vector<double>& x, double hz, size_t a, size_t b,
              double& fund_power, double& total_power) {
    double fund = 0.0, quad = 0.0, total = 0.0;
    const size_t N = b - a;
    for (size_t n = a; n < b; ++n) {
        const double p = 2.0 * kPi * hz * static_cast<double>(n) / kFs;
        fund += x[n] * std::cos(p);
        quad += x[n] * std::sin(p);
        total += x[n] * x[n];
    }
    fund_power = 2.0 * (fund * fund + quad * quad) /
                 (static_cast<double>(N) * static_cast<double>(N));
    total_power = total / static_cast<double>(N);
}

double phase_error_deg(const std::vector<double>& ref, const std::vector<double>& dut,
                       double hz, size_t a, size_t b) {
    std::complex<double> xr(0.0, 0.0), xd(0.0, 0.0);
    for (size_t n = a; n < b; ++n) {
        const std::complex<double> w =
            std::exp(std::complex<double>(0.0, -2.0 * kPi * hz * static_cast<double>(n) / kFs));
        xr += ref[n] * w;
        xd += dut[n] * w;
    }
    if (std::abs(xr) < 1e-15 || std::abs(xd) < 1e-15) return 0.0;
    return std::arg(xd / xr) * 180.0 / kPi;
}

// Ventana estacionaria de ~1 s DENTRO de una región activa continua (sin
// silencios dentro), ajustada a un número entero de ciclos a freq_hz.
// Devuelve false si no hay región activa suficiente.
// Los límites del tono se miden a nivel de muestra: un solo 1% de silencio
// dentro de la ventana falsea el THD+N en ~-20 dB (la fórmula compara la
// potencia total con la de la fundamental y el silencio diluye la segunda).
bool steady_window(const Signal& s, double freq_hz, size_t& a, size_t& b) {
    const double th = amp_from_db(-60.0);
    // Inicio del tono: primer índice con |x| > umbral 3 muestras seguidas.
    long toneStart = -1;
    for (size_t i = 0; i + 2 < s.l.size(); ++i) {
        if (std::abs(s.l[i]) > th && std::abs(s.l[i + 1]) > th &&
            std::abs(s.l[i + 2]) > th) {
            toneStart = static_cast<long>(i);
            break;
        }
    }
    if (toneStart < 0) return false;
    // Fin del tono: primer índice tras el cual hay 48 muestras seguidas por
    // debajo del umbral (un cruce por cero dura 1-2 muestras, no 48).
    long toneEnd = static_cast<long>(s.l.size());
    int quietRun = 0;
    for (size_t i = static_cast<size_t>(toneStart); i < s.l.size(); ++i) {
        if (std::abs(s.l[i]) < th) {
            if (++quietRun >= 48) {
                toneEnd = static_cast<long>(i) - 47;  // inicio de la racha silenciosa
                break;
            }
        } else {
            quietRun = 0;
        }
    }
    // Ventana de ~1 s dentro del tono, empezando 0.1 s tras el inicio
    // (para saltar el transitorio de ataque); si el tono es corto, desde el inicio.
    size_t start = static_cast<size_t>(toneStart) + 4800;
    if (start + 4800 > static_cast<size_t>(toneEnd)) start = static_cast<size_t>(toneStart);
    const size_t avail = (toneEnd > static_cast<long>(start))
                             ? (static_cast<size_t>(toneEnd) - start)
                             : 0;
    if (avail < 480) return false;  // menos de 10 ms de señal estacionaria
    const size_t want = std::min<size_t>(48000, avail);
    double cycles = std::floor(static_cast<double>(want) * freq_hz / kFs);
    if (cycles < 10.0) {
        cycles = std::floor(static_cast<double>(avail) * freq_hz / kFs);
        if (cycles < 1.0) return false;
    }
    const size_t len = static_cast<size_t>(std::round(cycles * kFs / freq_hz));
    if (len == 0 || start + len > static_cast<size_t>(toneEnd)) return false;
    a = start;
    b = start + len;
    return true;
}

// Coherencia magnitud-cuadrada |Sxy|^2/(Sxx*Syy) entre dos canales, promediada
// en [f_lo, f_hi] (Welch: segmentos 4096, Hann, 50% solape). -1 si no se puede.
double coherence_mean(const std::vector<double>& x, const std::vector<double>& y,
                      double f_lo, double f_hi) {
    const size_t seg = 4096, hop = 2048;
    if (x.size() < seg || y.size() < seg) return -1.0;
    const size_t nseg = (std::min(x.size(), y.size()) - seg) / hop + 1;
    const size_t N = 4096;
    std::vector<double> sxx(N / 2 + 1, 0.0), syy(N / 2 + 1, 0.0);
    std::vector<std::complex<double>> sxy(N / 2 + 1, {0.0, 0.0});
    std::vector<double> win(N);
    for (size_t i = 0; i < N; ++i)
        win[i] = 0.5 * (1.0 - std::cos(2.0 * kPi * static_cast<double>(i) /
                                       static_cast<double>(N - 1)));
    for (size_t s = 0; s < nseg; ++s) {
        const size_t off = s * hop;
        std::vector<std::complex<double>> X(N), Y(N);
        for (size_t i = 0; i < N; ++i) {
            X[i] = x[off + i] * win[i];
            Y[i] = y[off + i] * win[i];
        }
        fft(X, false);
        fft(Y, false);
        for (size_t k = 0; k <= N / 2; ++k) {
            sxx[k] += std::norm(X[k]);
            syy[k] += std::norm(Y[k]);
            sxy[k] += X[k] * std::conj(Y[k]);
        }
    }
    double sum = 0.0;
    int cnt = 0;
    for (size_t k = 0; k <= N / 2; ++k) {
        const double f = static_cast<double>(k) * kFs / static_cast<double>(N);
        if (f < f_lo || f > f_hi) continue;
        const double den = sxx[k] * syy[k];
        if (den <= 1e-30) continue;
        sum += std::min(std::norm(sxy[k]) / den, 1.0);
        ++cnt;
    }
    if (cnt == 0) return -1.0;
    return sum / cnt;
}

size_t peak_index_abs(const std::vector<double>& x) {
    size_t best = 0;
    double bestV = -1.0;
    for (size_t i = 0; i < x.size(); ++i) {
        const double v = std::abs(x[i]);
        if (v > bestV) {
            bestV = v;
            best = i;
        }
    }
    return best;
}

// Tiempo de subida 10-90% caminando hacia atrás desde el pico (muestras).
double rise_time_samples(const std::vector<double>& x, size_t peak_idx) {
    const double pk = std::abs(x[peak_idx]);
    if (pk < 1e-12) return 0.0;
    long i10 = -1, i90 = -1;
    for (long i = static_cast<long>(peak_idx); i >= 0; --i) {
        const double v = std::abs(x[static_cast<size_t>(i)]);
        if (i90 < 0 && v >= 0.9 * pk) i90 = i;
        if (v < 0.1 * pk) {
            i10 = i;
            break;
        }
    }
    if (i10 < 0 || i90 < 0) return 0.0;
    return static_cast<double>(i90 - i10);
}

}  // namespace

bool MeasureReport::all_pass() const {
    for (const auto& m : metrics)
        if (m.applicable && !m.pass) return false;
    return true;
}

const MetricResult* MeasureReport::find(const std::string& name) const {
    for (const auto& m : metrics)
        if (m.name == name) return &m;
    return nullptr;
}

std::string MeasureReport::to_json() const {
    std::ostringstream out;
    out << "{";
    bool first = true;
    auto num = [](double v) {
        if (!std::isfinite(v)) return std::string("null");
        std::ostringstream s;
        s.setf(std::ios::fixed);
        s.precision(6);
        s << v;
        return s.str();
    };
    for (const auto& m : metrics) {
        if (!first) out << ",";
        first = false;
        out << "\"" << m.name << "\":{\"value\":" << num(m.value)
            << ",\"applicable\":" << (m.applicable ? "true" : "false")
            << ",\"pass\":" << (m.pass ? "true" : "false") << "}";
    }
    out << ",\"all_pass\":" << (all_pass() ? "true" : "false") << "}";
    return out.str();
}

double interchannel_phase_deg(const Signal& s, double hz, size_t a, size_t b) {
    return phase_error_deg(s.l, s.r, hz, a, b);
}

MeasureReport measure(const Signal& ref, const Signal& dut, const Thresholds& th,
                      const MeasureOptions& opt) {
    MeasureReport rep;
    auto add = [&](const std::string& name, double value, bool applicable, bool pass,
                   const std::string& detail) {
        rep.metrics.push_back(MetricResult{name, value, applicable, pass, detail});
    };

    if (ref.frames() != dut.frames()) {
        add("length_match", 0.0, true, false,
            "longitudes distintas: ref=" + std::to_string(ref.frames()) +
                " dut=" + std::to_string(dut.frames()));
        return rep;
    }
    const size_t N = ref.frames();
    if (N == 0) {
        add("length_match", 0.0, true, false, "señales vacías");
        return rep;
    }

    // 0) Finitud: NaN/Inf es fallo inmediato.
    const bool finite = all_finite(dut);
    add("finite", finite ? 1.0 : 0.0, true, finite,
        finite ? "sin NaN/Inf en la salida" : "NaN o Inf en la salida del DUT");

    // 1) Picos y overshoot.
    const double pkRef = std::max(peak_abs(ref.l, 0, N), peak_abs(ref.r, 0, N));
    const double pkDut = std::max(peak_abs(dut.l, 0, N), peak_abs(dut.r, 0, N));
    const double pkDutDb = dbfs(pkDut);
    add("peak_ref_dbfs", dbfs(pkRef), true, true, "pico de la referencia");
    add("peak_dut_dbfs", pkDutDb, true, pkDutDb <= th.peak_max_dbfs + 1e-9,
        "pico de salida; fallo si > -0.1 dBFS");
    const double overshoot = (pkRef > 1e-12) ? dbfs(pkDut / pkRef) : 0.0;
    add("overshoot_db", overshoot, pkRef > 1e-12, overshoot <= th.overshoot_max_db,
        "pico dut - pico ref; fallo si > 0.5 dB");
    const double rmsRef = rms(ref.l, 0, N);
    const double rmsDut = rms(dut.l, 0, N);
    add("rms_ratio_db", (rmsRef > 1e-12) ? dbfs(rmsDut / rmsRef) : 0.0, rmsRef > 1e-12,
        true, "informativo: cambio de nivel global");

    // 2) Latencia: onset + refinado por correlación; sin onset, xcorr global.
    // Onset a nivel de muestra (exacto para ataques abruptos); si no hay,
    // se usa el onset por RMS de 1 ms (cuantizado a 48 muestras).
    long onRef = find_onset_sample(ref.l, -60.0);
    if (onRef < 0) onRef = find_onset(ref.l, -60.0);
    long onDut = find_onset_sample(dut.l, -60.0);
    if (onDut < 0) onDut = find_onset(dut.l, -60.0);
    double latency = 0.0, latCorr = 0.0;
    std::string latDetail;
    if (onRef >= 0 && onDut >= 0) {
        const long approx = onDut - onRef;
        const size_t segStart = static_cast<size_t>(onRef);
        const size_t segLen = std::min<size_t>(24000, N - segStart);
        latency = refine_latency(ref.l, dut.l, approx, 64, segStart, segLen, latCorr);
        latDetail = "onset ref=" + std::to_string(onRef) + " dut=" + std::to_string(onDut) +
                    " corr=" + std::to_string(latCorr);
    } else {
        latency = xcorr_latency(ref.l, dut.l, 1024, latCorr);
        latDetail = "sin onset claro; xcorr global +-1024 (ambigua en tonos periódicos)";
    }
    bool latPass = std::abs(latency) <= th.latency_abs_max_samples;
    if (th.has_latency_baseline) {
        latPass = latPass &&
                  (std::abs(latency - th.latency_baseline_samples) <=
                   th.latency_deviation_max_samples);
        latDetail += " baseline=" + std::to_string(th.latency_baseline_samples);
    } else {
        latDetail += " sin baseline (comparar con la latencia dorada de la ruta)";
    }
    add("latency_samples", latency, true, latPass, latDetail);
    add("latency_corr", latCorr, true, true,
        "correlación normalizada en el retardo medido (1.0 = copia exacta)");

    // 2b) Latencia por pico (transitorio claro): cruce de comprobación.
    const double pkRmsRatio = (rmsRef > 1e-12) ? pkRef / rmsRef : 0.0;
    if (pkRmsRatio > 10.0) {
        const size_t pRef = peak_index_abs(ref.l);
        const size_t pDut = peak_index_abs(dut.l);
        const double peakLat =
            static_cast<double>(static_cast<long>(pDut) - static_cast<long>(pRef));
        add("latency_peak_samples", peakLat, true, true,
            "informativo: retardo por pico máximo (transitorio)");
    } else {
        add("latency_peak_samples", 0.0, false, true, "la referencia no tiene un transitorio claro");
    }

    // 2c) Jitter: dispersión de la latencia por segmentos de 0.5 s (proxy
    // intra-archivo; el p99 real de 100 repeticiones es métrica del runner).
    std::vector<double> segLat;
    if (onRef >= 0) {
        const size_t total = N - static_cast<size_t>(onRef);
        const size_t segLen = 24000;
        const size_t count = std::min<size_t>(8, total / segLen);
        for (size_t sIdx = 0; sIdx < count; ++sIdx) {
            const size_t start = static_cast<size_t>(onRef) +
                                 sIdx * (total / std::max<size_t>(1, count));
            if (start + segLen > N) break;
            if (rms(ref.l, start, start + segLen) < amp_from_db(-80.0)) continue;
            double c = 0.0;
            segLat.push_back(refine_latency(ref.l, dut.l,
                                            static_cast<long>(std::lrint(latency)), 32, start,
                                            segLen, c));
        }
    }
    if (segLat.size() >= 2) {
        const auto mm = std::minmax_element(segLat.begin(), segLat.end());
        const double spread = *mm.second - *mm.first;
        const double jitLim = (opt.route == "daemon") ? th.jitter_p99_max_samples_daemon
                                                      : th.jitter_p99_max_samples;
        add("latency_jitter_spread_samples", spread, true, spread <= jitLim,
            "proxy intra-archivo (no el p99 de 100 repeticiones del runner); ruta=" +
                opt.route);
    } else {
        add("latency_jitter_spread_samples", 0.0, false, true, "menos de 2 segmentos con energía");
    }

    // 3) THD+N, error de fase y modulación residual (ventana de ciclos enteros).
    size_t wa = 0, wb = 0;
    if (steady_window(ref, opt.freq_hz, wa, wb)) {
        double fundRef = 0.0, totRef = 0.0, fundDut = 0.0, totDut = 0.0;
        goertzel(ref.l, opt.freq_hz, wa, wb, fundRef, totRef);
        goertzel(dut.l, opt.freq_hz, wa, wb, fundDut, totDut);
        const double sineRatio = (totRef > 1e-30) ? fundRef / totRef : 0.0;
        const bool sineLike = sineRatio > 0.9 && fundRef > 1e-12;
        if (sineLike) {
            const double noise = std::max(0.0, totDut - fundDut);
            const double thdn = 10.0 * std::log10(std::max(noise, 1e-30) /
                                                  std::max(fundDut, 1e-30));
            add("thdn_db", thdn, true, thdn <= th.thdn_max_db,
                "THD+N a " + std::to_string(opt.freq_hz) +
                    " Hz; fallo si > -80 dB (seno -12 dBFS)");
            const double ph = phase_error_deg(ref.l, dut.l, opt.freq_hz, wa, wb);
            const double phLim =
                (opt.freq_hz < 100.0) ? th.phase_error_lf_max_deg : th.phase_error_max_deg;
            add("phase_error_deg", ph, true, std::abs(ph) <= phLim,
                "error de fase a " + std::to_string(opt.freq_hz) + " Hz; fallo si > " +
                    std::to_string(phLim) + " grados");
            const double iphRef = interchannel_phase_deg(ref, opt.freq_hz, wa, wb);
            const double iphDut = interchannel_phase_deg(dut, opt.freq_hz, wa, wb);
            add("interchannel_phase_ref_deg", iphRef, true, true,
                "informativo: fase R-L de la referencia");
            add("interchannel_phase_dut_deg", iphDut, true, true,
                "informativo: fase R-L de la salida");
            // Modulación residual: variación de la amplitud Goertzel por
            // sub-ventanas de 10 ciclos. La envolvente de un tono estacionario
            // debe ser constante; si el DUT bombea, la amplitud varía.
            const size_t subLen =
                static_cast<size_t>(std::round(10.0 * kFs / opt.freq_hz));
            std::vector<double> amps;
            if (subLen > 0) {
                for (size_t a2 = wa; a2 + subLen <= wb; a2 += subLen) {
                    double fp = 0.0, tp = 0.0;
                    goertzel(dut.l, opt.freq_hz, a2, a2 + subLen, fp, tp);
                    amps.push_back(std::sqrt(2.0 * std::max(fp, 0.0)));
                }
            }
            if (amps.size() >= 4) {
                const double mean =
                    std::accumulate(amps.begin(), amps.end(), 0.0) / amps.size();
                double var = 0.0;
                for (double v : amps) var += (v - mean) * (v - mean);
                const double sd = std::sqrt(var / amps.size());
                const double mod = (mean > 1e-12) ? dbfs(sd / mean) : -300.0;
                add("residual_modulation_db", mod, true,
                    mod <= th.residual_modulation_max_dbfs,
                    "variación de la envolvente (amplitud Goertzel por sub-ventanas de 10 "
                    "ciclos); fallo si > -70 dB");
            } else {
                add("residual_modulation_db", 0.0, false, true,
                    "ventana estacionaria demasiado corta para 4 sub-ventanas");
            }
        } else {
            add("thdn_db", 0.0, false, true,
                "la referencia no es un seno dominante a la frecuencia pedida");
            add("phase_error_deg", 0.0, false, true,
                "la referencia no es un seno dominante a la frecuencia pedida");
            add("interchannel_phase_ref_deg", 0.0, false, true,
                "la referencia no es un seno dominante a la frecuencia pedida");
            add("interchannel_phase_dut_deg", 0.0, false, true,
                "la referencia no es un seno dominante a la frecuencia pedida");
            add("residual_modulation_db", 0.0, false, true,
                "la referencia no es un seno dominante a la frecuencia pedida");
        }
    } else {
        add("thdn_db", 0.0, false, true, "sin ventana estacionaria de ciclos enteros");
        add("phase_error_deg", 0.0, false, true, "sin ventana estacionaria de ciclos enteros");
        add("interchannel_phase_ref_deg", 0.0, false, true,
            "sin ventana estacionaria de ciclos enteros");
        add("interchannel_phase_dut_deg", 0.0, false, true,
            "sin ventana estacionaria de ciclos enteros");
        add("residual_modulation_db", 0.0, false, true,
            "sin ventana estacionaria de ciclos enteros");
    }

    // 4) Preservación de transitorios (solo si la referencia tiene uno claro).
    if (pkRmsRatio > 10.0) {
        const size_t pRef = peak_index_abs(ref.l);
        const size_t w0 = (pRef > static_cast<size_t>(kHop)) ? pRef - kHop : 0;
        const size_t w1 = std::min(N, pRef + static_cast<size_t>(kHop) + 1);
        double eRef = 0.0, eDut = 0.0, cRef = 0.0, cDut = 0.0;
        const long latI = static_cast<long>(std::lrint(latency));
        for (size_t n = w0; n < w1; ++n) {
            const double er = ref.l[n] * ref.l[n];
            eRef += er;
            cRef += static_cast<double>(static_cast<long>(n) - static_cast<long>(pRef)) * er;
            const long m = static_cast<long>(n) + latI;
            const double d = (m >= 0 && static_cast<size_t>(m) < N)
                                 ? dut.l[static_cast<size_t>(m)]
                                 : 0.0;
            const double ed = d * d;
            eDut += ed;
            cDut += static_cast<double>(static_cast<long>(n) - static_cast<long>(pRef)) * ed;
        }
        const double loss = (eRef > 1e-30)
                                ? 10.0 * std::log10(std::max(eDut, 1e-30) / eRef)
                                : 0.0;
        add("click_energy_loss_db", -loss, true, -loss <= th.click_energy_loss_max_db,
            "pérdida de energía del transitorio en +-1 hop; fallo si > 1 dB");
        const double centRef = (eRef > 1e-30) ? cRef / eRef : 0.0;
        const double centDut = (eDut > 1e-30) ? cDut / eDut : 0.0;
        const double shiftSamples = centDut - centRef;
        const double shiftHops = shiftSamples / static_cast<double>(kHop);
        add("transient_centroid_shift_hops", shiftHops, true,
            std::abs(shiftHops) <= th.centroid_shift_max_hops,
            "desplazamiento del centroide temporal; fallo si > 0.25 hop (32 muestras)");
        add("transient_centroid_shift_samples", shiftSamples, true, true, "informativo");
        const double riseRef = rise_time_samples(ref.l, pRef);
        const size_t pDut = peak_index_abs(dut.l);
        const double riseDut = rise_time_samples(dut.l, pDut);
        add("rise_time_ref_samples", riseRef, true, true,
            "informativo: subida 10-90% de la referencia");
        add("rise_time_dut_samples", riseDut, true, true,
            "informativo: subida 10-90% de la salida");
        const double pkRefW = peak_abs(ref.l, w0, w1);
        const double pkDutW = peak_abs(dut.l, w0, w1);
        const double ovAttack = (pkRefW > 1e-12) ? dbfs(pkDutW / pkRefW) : 0.0;
        add("overshoot_attack_db", ovAttack, true, ovAttack <= th.overshoot_max_db,
            "overshoot alrededor del ataque (+-1 hop); fallo si > 0.5 dB");
    } else {
        add("click_energy_loss_db", 0.0, false, true,
            "la referencia no tiene un transitorio claro");
        add("transient_centroid_shift_hops", 0.0, false, true,
            "la referencia no tiene un transitorio claro");
        add("transient_centroid_shift_samples", 0.0, false, true,
            "la referencia no tiene un transitorio claro");
        add("rise_time_ref_samples", 0.0, false, true,
            "la referencia no tiene un transitorio claro");
        add("rise_time_dut_samples", 0.0, false, true,
            "la referencia no tiene un transitorio claro");
        add("overshoot_attack_db", 0.0, false, true,
            "la referencia no tiene un transitorio claro");
    }

    // 5) Coherencia intercanal (la salida no debe decorrelar una entrada coherente).
    // La coherencia magnitud-cuadrada es 1 también para señales invertidas o
    // retardadas: por eso phase_01 se verifica con el residuo mono (abajo).
    const double cohDut = coherence_mean(dut.l, dut.r, 100.0, 12000.0);
    const double cohRef = coherence_mean(ref.l, ref.r, 100.0, 12000.0);
    add("coherence_dut", cohDut, cohDut >= 0.0, true,
        "informativo: coherencia intercanal de la salida (100 Hz-12 kHz)");
    add("coherence_ref", cohRef, cohRef >= 0.0, true,
        "informativo: coherencia intercanal de la referencia");
    const bool cohApplicable =
        (cohDut >= 0.0) && (cohRef >= 0.0) && (cohRef >= th.coherence_min);
    if (cohApplicable && !opt.expect_low_coherence) {
        add("coherence_check", cohDut, true, cohDut >= th.coherence_min,
            "la salida no debe decorrelar una entrada coherente; fallo si < 0.995");
    } else {
        add("coherence_check", cohDut, false, true,
            opt.expect_low_coherence
                ? "se espera baja coherencia en este caso"
                : "la referencia ya es incoherente (no se penaliza)");
    }

    // 5b) Residuo mono (solo phase_01: L+R debe cancelar).
    if (opt.case_id == "phase_01") {
        size_t wa2 = 0, wb2 = 0;
        double monoDb = -300.0;
        bool ok = false;
        if (steady_window(ref, 1000.0, wa2, wb2)) {
            double sumRms = 0.0;
            for (size_t n = wa2; n < wb2; ++n) {
                const double v = dut.l[n] + dut.r[n];
                sumRms += v * v;
            }
            sumRms = std::sqrt(sumRms / static_cast<double>(wb2 - wa2));
            monoDb = dbfs(sumRms);
            ok = true;
        }
        add("mono_residual_dbfs", monoDb, ok, monoDb <= th.mono_residual_max_dbfs,
            "phase_01: RMS de L+R; la cancelación debe quedar por debajo de -55 dBFS");
    } else {
        add("mono_residual_dbfs", 0.0, false, true, "solo aplicable a phase_01");
    }

    // 6) Residuo en silencio (100 ms tras la última señal de la referencia).
    {
        const double th90 = amp_from_db(-90.0);
        long last = -1;
        for (long n = static_cast<long>(N) - 1; n >= 0; --n) {
            if (std::abs(ref.l[static_cast<size_t>(n)]) > th90 ||
                std::abs(ref.r[static_cast<size_t>(n)]) > th90) {
                last = n;
                break;
            }
        }
        if (last >= 0) {
            const size_t a = std::min(N, static_cast<size_t>(last) + 4800);
            if (a + 480 <= N) {
                const double r = std::max(rms(dut.l, a, N), rms(dut.r, a, N));
                const double rDb = dbfs(r);
                add("residual_silence_dbfs", rDb, true,
                    rDb <= th.residual_silence_max_dbfs,
                    "RMS de la salida en silencio, 100 ms tras la última señal; fallo si "
                    "> -90 dBFS");
            } else {
                add("residual_silence_dbfs", 0.0, false, true,
                    "sin región de silencio suficiente al final");
            }
        } else {
            add("residual_silence_dbfs", 0.0, false, true,
                "la referencia es silencio completo");
        }
    }

    return rep;
}

// ---------- eventos de ruta (events.jsonl) ----------

namespace {

// Parser mínimo: extrae el valor que sigue a "key" en una línea JSON.
// Solo entiende el esquema de events.jsonl (ver README.md).
long long json_long(const std::string& s, const std::string& key, size_t from = 0) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat, from);
    if (k == std::string::npos) throw std::runtime_error("events: falta la clave " + key);
    const size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos)
        throw std::runtime_error("events: falta ':' tras " + key);
    const char* p = s.c_str() + colon + 1;
    char* end = nullptr;
    const long long v = std::strtoll(p, &end, 10);
    if (end == p) throw std::runtime_error("events: valor no entero en " + key);
    return v;
}

double json_number(const std::string& s, const std::string& key, size_t from = 0) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat, from);
    if (k == std::string::npos) throw std::runtime_error("events: falta la clave " + key);
    const size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos)
        throw std::runtime_error("events: falta ':' tras " + key);
    const char* p = s.c_str() + colon + 1;
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) throw std::runtime_error("events: valor no numérico en " + key);
    return v;
}

bool json_bool(const std::string& s, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat);
    if (k == std::string::npos) throw std::runtime_error("events: falta la clave " + key);
    const size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos)
        throw std::runtime_error("events: falta ':' tras " + key);
    size_t i = colon + 1;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    if (s.compare(i, 4, "true") == 0) return true;
    if (s.compare(i, 5, "false") == 0) return false;
    throw std::runtime_error("events: valor no booleano en " + key);
}

std::string json_string(const std::string& s, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const size_t k = s.find(pat);
    if (k == std::string::npos) throw std::runtime_error("events: falta la clave " + key);
    const size_t colon = s.find(':', k + pat.size());
    if (colon == std::string::npos)
        throw std::runtime_error("events: falta ':' tras " + key);
    const size_t q1 = s.find('"', colon + 1);
    if (q1 == std::string::npos)
        throw std::runtime_error("events: valor no string en " + key);
    const size_t q2 = s.find('"', q1 + 1);
    if (q2 == std::string::npos)
        throw std::runtime_error("events: string sin cerrar en " + key);
    return s.substr(q1 + 1, q2 - q1 - 1);
}

}  // namespace

std::vector<StateEvent> read_events(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("read_events: no se pudo abrir " + path);
    std::vector<StateEvent> out;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("\"t_capture_ns\"") == std::string::npos) continue;
        StateEvent e;
        e.t_ns = json_long(line, "t_capture_ns");
        e.route = json_string(line, "route");
        const size_t rq = line.find("\"requested\"");
        const size_t ap = line.find("\"applied\"");
        if (rq == std::string::npos || ap == std::string::npos)
            throw std::runtime_error("read_events: evento sin requested/applied");
        const std::string reqPart = line.substr(rq, ap - rq);
        const std::string appPart = line.substr(ap);
        e.req_mode = static_cast<int>(json_long(reqPart, "mode"));
        e.req_strength = json_number(reqPart, "strength");
        e.app_mode = static_cast<int>(json_long(appPart, "mode"));
        e.app_strength = json_number(appPart, "strength");
        e.connected = json_bool(line, "connected");
        e.snapshot_id = json_long(line, "snapshot_id");
        out.push_back(e);
    }
    if (out.empty()) throw std::runtime_error("read_events: sin eventos válidos en " + path);
    return out;
}

DivergenceReport analyze_events(const std::vector<StateEvent>& ev, const Thresholds& th,
                                const std::string& route) {
    DivergenceReport rep;
    if (ev.empty()) {
        rep.pass = false;
        rep.detail = "sin eventos";
        return rep;
    }

    double maxDiff = 0.0;
    int mismatches = 0;
    double maxGapMs = 0.0;
    bool inGap = false;
    double gapStartMs = 0.0;

    // Snapshot obsoleto: un cambio pedido cuya snapshot no se actualiza en > 2 hops.
    bool stale = false;
    bool pending = false;
    double pendingSinceMs = 0.0;
    long long pendingSnap = 0;
    double prevReqStrength = ev[0].req_strength;
    int prevReqMode = ev[0].req_mode;

    for (size_t i = 0; i < ev.size(); ++i) {
        const auto& e = ev[i];
        const double tMs = static_cast<double>(e.t_ns) / 1e6;
        const double diff = std::abs(e.req_strength - e.app_strength);
        const bool mismatch = (e.req_mode != e.app_mode);
        maxDiff = std::max(maxDiff, diff);
        if (mismatch) ++mismatches;
        const bool divergent = (diff > 0.01) || mismatch;
        if (divergent && !inGap) {
            inGap = true;
            gapStartMs = tMs;
        }
        if (!divergent && inGap) {
            inGap = false;
            maxGapMs = std::max(maxGapMs, tMs - gapStartMs);
        }
        // ¿Cambió lo pedido respecto al evento anterior?
        if (i > 0 &&
            (std::abs(e.req_strength - prevReqStrength) > 0.01 || e.req_mode != prevReqMode)) {
            pending = true;
            pendingSinceMs = tMs;
            pendingSnap = e.snapshot_id;
        }
        prevReqStrength = e.req_strength;
        prevReqMode = e.req_mode;
        if (pending) {
            if (e.snapshot_id != pendingSnap) {
                pending = false;  // el snapshot se actualizó
            } else if ((tMs - pendingSinceMs) > th.stale_snapshot_max_hops * kHopMs) {
                stale = true;
            }
            if (std::abs(e.req_strength - e.app_strength) <= 0.01 &&
                e.req_mode == e.app_mode) {
                pending = false;  // lo aplicado ya coincide con lo pedido
            }
        }
    }
    if (inGap) {
        const double tMs = static_cast<double>(ev.back().t_ns) / 1e6;
        maxGapMs = std::max(maxGapMs, tMs - gapStartMs);
    }

    rep.max_strength_diff = maxDiff;
    rep.mode_mismatch_count = mismatches;
    rep.max_gap_ms = maxGapMs;
    rep.max_gap_hops = maxGapMs / kHopMs;
    rep.stale_snapshot = stale;

    // Reconvergencia: tras cada intervalo desconectado, hops hasta applied==requested.
    double maxReconvHops = 0.0;
    bool foundFault = false;
    bool neverReconv = false;
    for (size_t i = 0; i < ev.size(); ++i) {
        if (ev[i].connected) continue;
        size_t j = i;
        while (j < ev.size() && !ev[j].connected) ++j;
        if (j >= ev.size()) break;  // el fallo no termina en el registro
        foundFault = true;
        const double tEndMs = static_cast<double>(ev[j].t_ns) / 1e6;
        double tMatchMs = -1.0;
        for (size_t k = j; k < ev.size(); ++k) {
            if (std::abs(ev[k].req_strength - ev[k].app_strength) <= 0.01 &&
                ev[k].req_mode == ev[k].app_mode) {
                tMatchMs = static_cast<double>(ev[k].t_ns) / 1e6;
                break;
            }
        }
        if (tMatchMs < 0.0) {
            neverReconv = true;
        } else {
            maxReconvHops = std::max(maxReconvHops, (tMatchMs - tEndMs) / kHopMs);
        }
        i = j;  // continuar tras el fallo
    }
    rep.reconvergence_hops = foundFault ? (neverReconv ? 1e9 : maxReconvHops) : -1.0;

    const double reconvLim = (route == "daemon") ? th.reconvergence_max_hops_daemon
                                                 : th.reconvergence_max_hops_jni;
    // Épsilon numérico: los intervalos se calculan desde timestamps en ns y el
    // límite de 2 hops es un borde exacto. Los timestamps son enteros en ns, así
    // que el redondeo es de ~1 ns = 3.75e-7 hops; 1e-6 hops lo cubre.
    rep.pass = (rep.max_gap_hops <= th.state_divergence_max_hops + 1e-6) &&
               (rep.max_gap_ms <= th.state_persistence_max_ms + 1e-6) && !stale &&
               (!foundFault || (!neverReconv && maxReconvHops <= reconvLim + 1e-6));

    std::ostringstream d;
    d << "max_diff=" << maxDiff << " mismatches=" << mismatches << " max_gap=" << maxGapMs
      << " ms (" << rep.max_gap_hops << " hops) stale=" << (stale ? "si" : "no")
      << " reconv=";
    if (!foundFault) {
        d << "n/a";
    } else if (neverReconv) {
        d << "nunca";
    } else {
        d << maxReconvHops << " hops (limite " << reconvLim << ", ruta " << route << ")";
    }
    rep.detail = d.str();
    return rep;
}

}  // namespace adv
