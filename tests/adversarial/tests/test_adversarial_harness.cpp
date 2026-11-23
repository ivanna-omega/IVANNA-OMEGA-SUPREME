// test_adversarial_harness.cpp — Tests host del generador y el harness.
// Sin framework externo: checks con conteo de fallos; salida 0 si todo pasa.
// Cubre: WAV round-trip, generación de los 21 casos, passthrough bit-exacto,
// latencia (delay 128), fase (inversión 180°), overshoot (+6 dB), preservación
// de transitorios, detección de NaN, y análisis de events.jsonl.
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

#include "adversarial_core.hpp"

namespace {
using namespace adv;
namespace fs = std::filesystem;

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const std::string& name) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::cout << "FAIL: " << name << "\n";
    }
}

void check_near(double got, double want, double tol, const std::string& name) {
    ++g_checks;
    if (!(std::abs(got - want) <= tol)) {
        ++g_failures;
        std::cout << "FAIL: " << name << " (got " << got << ", want " << want << " +- "
                  << tol << ")\n";
    }
}

Signal shifted(const Signal& in, long delay) {
    Signal out = in;
    std::vector<double> nl(in.l.size(), 0.0), nr(in.r.size(), 0.0);
    for (size_t n = 0; n < in.l.size(); ++n) {
        const long src = static_cast<long>(n) - delay;
        if (src >= 0 && static_cast<size_t>(src) < in.l.size()) {
            nl[n] = in.l[static_cast<size_t>(src)];
            nr[n] = in.r[static_cast<size_t>(src)];
        }
    }
    out.l = std::move(nl);
    out.r = std::move(nr);
    return out;
}

Signal scaled(const Signal& in, double gain_db) {
    Signal out = in;
    const double g = amp_from_db(gain_db);
    for (auto& v : out.l) v *= g;
    for (auto& v : out.r) v *= g;
    return out;
}

Signal inverted(const Signal& in) {
    Signal out = in;
    for (auto& v : out.l) v = -v;
    for (auto& v : out.r) v = -v;
    return out;
}

// Eventos sintéticos: un hop = 2_666_667 ns (128 muestras a 48 kHz).
std::vector<StateEvent> make_events(int count, double req_strength, double app_strength,
                                    int req_mode = 3, int app_mode = 3,
                                    bool connected = true, long long snap_start = 1000) {
    std::vector<StateEvent> ev;
    for (int i = 0; i < count; ++i) {
        StateEvent e;
        e.t_ns = static_cast<long long>(i) * 2666667LL;
        e.route = "jni";
        e.req_mode = req_mode;
        e.req_strength = req_strength;
        e.app_mode = app_mode;
        e.app_strength = app_strength;
        e.connected = connected;
        e.snapshot_id = snap_start + i;
        ev.push_back(e);
    }
    return ev;
}

void test_wav_roundtrip(const fs::path& tmp) {
    Signal s = generate_case("click_01");
    const fs::path p = tmp / "click_01.wav";
    write_wav(p.string(), s);
    Signal back = read_wav(p.string());
    check(back.frames() == s.frames(), "wav: frames iguales");
    check(back.channels == 2, "wav: estéreo");
    double maxErr = 0.0;
    for (size_t i = 0; i < s.frames(); ++i) {
        maxErr = std::max(maxErr, std::abs(back.l[i] - s.l[i]));
        maxErr = std::max(maxErr, std::abs(back.r[i] - s.r[i]));
    }
    check(maxErr <= 1.0 / 32768.0 + 1e-12, "wav: error de cuantización <= 1 LSB");
    check_near(back.l[48000], amp_from_db(-0.1), 1.0 / 32768.0 + 1e-9,
               "wav: pico del click a -0.1 dBFS");
}

void test_gen_all_cases() {
    const auto ids = case_ids();
    check(ids.size() == 26, "gen: 26 casos");
    for (const auto& id : ids) {
        Signal s = generate_case(id);
        const size_t want = static_cast<size_t>(case_duration_s(id) * kFs);
        check(s.frames() == want, "gen: duración de " + id);
        check(all_finite(s), "gen: finito " + id);
        check(peak_abs(s.l, 0, s.frames()) > 1e-6, "gen: contenido no nulo " + id);
    }
    // Spot checks con valores exactos.
    Signal click = generate_case("click_01");
    check(click.l[48000] == amp_from_db(-0.1) && click.l[47999] == 0.0 &&
              click.l[48001] == 0.0,
          "gen: click_01 es una sola muestra en 48000");
    Signal imp2 = generate_case("impulse_02");
    double e = 0.0;
    for (double v : imp2.l) e += v * v;
    check_near(e, amp_from_db(-3.0) * amp_from_db(-3.0), 1e-9,
               "gen: impulse_02 energía = amp(-3 dB)^2");
    Signal lvl = generate_case("level_01");
    const double r0 = rms(lvl.l, 24000, 48000);    // escalón -60 dB (0.5-1.0 s)
    const double r5 = rms(lvl.l, 144000, 168000);  // escalón -0.1 dB (3.0-3.5 s)
    check(dbfs(r5 / r0) > 40.0, "gen: level_01 los escalones suben > 40 dB");
    Signal sil = generate_case("silence_01");
    check(dbfs(rms(sil.l, 76800, 96000)) < -100.0,
          "gen: silence_01 hay silencio en 1.6-2.0 s");
    Signal d1 = generate_case("dense_01");
    // Crest factor en la región del tono (1-9 s). El objetivo de 3 dB del diseño
    // original no es alcanzable con 32 senos logarítmicos de fases aleatorias
    // (una multiseno gaussiana tiene ~11 dB); medido 11.03 dB.
    const double cf = dbfs(peak_abs(d1.l, 48000, 432000) / rms(d1.l, 48000, 432000));
    check(cf >= 8.0 && cf <= 13.0, "gen: dense_01 crest factor en [8, 13] dB");
    check_near(dbfs(rms(d1.l, 48000, 432000)), -12.0, 0.1,
               "gen: dense_01 RMS -12 dBFS en la región del tono");
    Signal d2 = generate_case("dense_02");
    check_near(dbfs(rms(d2.l, 48000, 48000 + 48000 * 28)), -14.0, 0.5,
               "gen: dense_02 RMS -14 dBFS");
    Signal ood = generate_case("ood_01");
    double mean = 0.0;
    for (size_t i = 96000; i < 288000; ++i) mean += ood.l[i];
    mean /= 192000.0;
    check_near(mean, 0.2, 0.01, "gen: ood_01 DC 0.2");
    check(peak_abs(ood.l, 0, ood.frames()) <= amp_from_db(-6.0) + 0.01,
          "gen: ood_01 pico <= -6 dBFS");
    Signal ph1 = generate_case("phase_01");
    double mono = 0.0;
    for (size_t i = 0; i < ph1.frames(); ++i) {
        const double v = ph1.l[i] + ph1.r[i];
        mono += v * v;
    }
    mono = std::sqrt(mono / ph1.frames());
    check(dbfs(mono) < -120.0, "gen: phase_01 L+R cancela");
    Signal ph2 = generate_case("phase_02");
    const double iph = interchannel_phase_deg(ph2, 1000.0, 48000, 144000);
    check_near(iph, -90.0, 2.0, "gen: phase_02 R retrasa 90 grados en 1-3 s");
    Signal bin = generate_case("binaural_01");
    // ITD de 14 muestras (0.292 ms) a 700 Hz = fase R-L de -73.5 grados.
    // (El pico local no sirve: depende de la fase de muestreo del seno.)
    const double iphBin = interchannel_phase_deg(bin, 700.0, 48000, 52800);
    check_near(iphBin, -73.5, 5.0, "gen: binaural_01 ITD 14 muestras = fase -73.5 grados");
    for (const auto& id : {"route_01", "route_02", "route_03"}) {
        Signal s = generate_case(id);
        check_near(s.l[48000], amp_from_db(-0.1), 1e-9,
                   "gen: " + std::string(id) + " marcador");
    }
    // ── P1 (2026-11-22): spot checks de los 5 casos nuevos, con valores
    // medidos al generarlos (no inventados; ver medición del commit).
    Signal v1 = generate_case("voice_01");
    check_near(dbfs(rms(v1.l, 24000, 38400)), -8.95, 0.5,
               "gen: voice_01 sílaba 0.5-0.8 s RMS ~-9 dBFS");
    check(dbfs(peak_abs(v1.l, 192000, 195744)) > -30.0,
          "gen: voice_01 sibilante presente a 4.0 s");
    Signal dr = generate_case("drums_01");
    check_near(dbfs(peak_abs(dr.l, 24000, 29760)), -4.17, 0.5,
               "gen: drums_01 bombo pico ~-4 dBFS");
    check(dbfs(rms(dr.l, 29760, 48000)) < -100.0,
          "gen: drums_01 silencio entre bombo y caja");
    Signal hm = generate_case("hotmix_01");
    check_near(dbfs(peak_abs(hm.l, 24000, 456000)), -0.50, 0.05,
               "gen: hotmix_01 pico limitado a -0.5 dBFS");
    check_near(dbfs(rms(hm.l, 24000, 456000)), -6.39, 0.5,
               "gen: hotmix_01 RMS activo ~-6.4 dBFS");
    const double hm_cf = dbfs(peak_abs(hm.l, 24000, 456000) / rms(hm.l, 24000, 456000));
    check(hm_cf >= 5.0 && hm_cf <= 8.0, "gen: hotmix_01 crest factor en [5, 8] dB");
    Signal pa = generate_case("pause_01");
    check(dbfs(rms(pa.l, 124800, 163200)) < -100.0,
          "gen: pause_01 silencio en la pausa 2.6-3.4 s");
    check_near(dbfs(rms(pa.l, 24000, 120000)), dbfs(rms(pa.l, 168000, 264000)), 0.1,
               "gen: pause_01 mismo nivel antes y después de la pausa");
    Signal sr = generate_case("srchange_01");
    check_near(dbfs(rms(sr.l, 48000, 96000)), dbfs(rms(sr.l, 144000, 192000)), 0.1,
               "gen: srchange_01 mismo nivel a ambos lados de 2.5 s");
    Signal a = generate_case("noise_imp_01", GenOptions{0, Variant::Mono});
    Signal b = generate_case("noise_imp_01", GenOptions{0, Variant::Mono});
    check(a.l == b.l && a.r == b.r, "gen: determinista con la misma semilla");
    Signal c = generate_case("noise_imp_01", GenOptions{150, Variant::Seed});
    check(a.l != c.l, "gen: otra semilla da otro estímulo");
}

void test_passthrough_sine() {
    Signal s = generate_case("silence_01");
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    opt.case_id = "silence_01";
    MeasureReport r = measure(s, s, Thresholds{}, opt);
    const MetricResult* lat = r.find("latency_samples");
    check(lat && lat->applicable && std::abs(lat->value) <= 0.5,
          "harness: passthrough latencia 0");
    const MetricResult* ph = r.find("phase_error_deg");
    check(ph && ph->applicable && std::abs(ph->value) <= 0.01,
          "harness: passthrough fase 0");
    const MetricResult* th = r.find("thdn_db");
    check(th && th->applicable && th->value < -120.0,
          "harness: passthrough THD+N muy bajo");
    const MetricResult* ov = r.find("overshoot_db");
    check(ov && ov->applicable && std::abs(ov->value) <= 0.001,
          "harness: passthrough overshoot 0");
    const MetricResult* co = r.find("coherence_dut");
    check(co && co->applicable && std::abs(co->value - 1.0) <= 0.001,
          "harness: passthrough coherencia 1");
    check(r.all_pass(), "harness: passthrough pasa todo");
}

void test_delay_128() {
    // Latencia por forma de onda (seno): 128 muestras.
    Signal s = generate_case("silence_01");
    Signal d = shifted(s, 128);
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    MeasureReport r = measure(s, d, Thresholds{}, opt);
    const MetricResult* lat = r.find("latency_samples");
    check(lat && lat->applicable, "harness: delay tiene latencia aplicable");
    check_near(lat->value, 128.0, 0.5, "harness: delay 128 medido");
    // Latencia por pico (transitorio): el click es el que tiene pico claro.
    Signal c = generate_case("click_01");
    Signal cd = shifted(c, 128);
    MeasureReport rc = measure(c, cd, Thresholds{}, opt);
    const MetricResult* pl = rc.find("latency_peak_samples");
    check(pl && pl->applicable, "harness: delay tiene latencia por pico");
    check_near(pl->value, 128.0, 0.5, "harness: delay 128 por pico");
    const MetricResult* latc = rc.find("latency_samples");
    check(latc && latc->applicable, "harness: delay click tiene latencia aplicable");
    check_near(latc->value, 128.0, 0.5, "harness: delay 128 click medido");
}

void test_inverted() {
    Signal s = generate_case("silence_01");
    Signal d = inverted(s);
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    MeasureReport r = measure(s, d, Thresholds{}, opt);
    const MetricResult* ph = r.find("phase_error_deg");
    check(ph && ph->applicable, "harness: inversión tiene fase aplicable");
    check(ph && std::abs(std::abs(ph->value) - 180.0) <= 1.0,
          "harness: inversión da fase 180");
    check(!r.all_pass(), "harness: inversión no pasa (fase 180 > 2)");
}

void test_gain() {
    Signal s = generate_case("silence_01");
    Signal d = scaled(s, 6.0);
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    MeasureReport r = measure(s, d, Thresholds{}, opt);
    const MetricResult* ov = r.find("overshoot_db");
    check(ov && ov->applicable, "harness: ganancia tiene overshoot aplicable");
    check_near(ov->value, 6.0, 0.05, "harness: ganancia +6 dB medida");
    check(!r.all_pass(), "harness: ganancia +6 dB no pasa (overshoot)");
}

void test_transient_passthrough() {
    Signal s = generate_case("click_01");
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    opt.case_id = "click_01";
    MeasureReport r = measure(s, s, Thresholds{}, opt);
    const MetricResult* loss = r.find("click_energy_loss_db");
    check(loss && loss->applicable, "harness: click tiene pérdida de energía aplicable");
    check_near(loss->value, 0.0, 0.001, "harness: click passthrough pérdida 0 dB");
    const MetricResult* shift = r.find("transient_centroid_shift_hops");
    check(shift && shift->applicable, "harness: click tiene centroide aplicable");
    check_near(shift->value, 0.0, 0.001, "harness: click passthrough centroide 0");
    const MetricResult* rise = r.find("rise_time_dut_samples");
    check(rise && rise->applicable, "harness: click tiene rise time aplicable");
    check_near(rise->value, 1.0, 0.5, "harness: click rise time 1 muestra");
    check(r.all_pass(), "harness: click passthrough pasa todo");
}

void test_nan() {
    Signal s = generate_case("silence_01");
    Signal d = s;
    d.l[100000] = std::numeric_limits<double>::quiet_NaN();
    check(!all_finite(d), "harness: all_finite detecta NaN");
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    MeasureReport r = measure(s, d, Thresholds{}, opt);
    const MetricResult* f = r.find("finite");
    check(f && f->applicable && !f->pass, "harness: NaN marca finite como fallo");
    check(!r.all_pass(), "harness: NaN no pasa");
}

void test_phase_01() {
    Signal s = generate_case("phase_01");
    MeasureOptions opt;
    opt.freq_hz = 1000.0;
    opt.case_id = "phase_01";
    MeasureReport r = measure(s, s, Thresholds{}, opt);
    const MetricResult* mono = r.find("mono_residual_dbfs");
    check(mono && mono->applicable, "harness: phase_01 tiene residuo mono aplicable");
    check(mono && mono->value < -120.0, "harness: phase_01 residuo mono muy bajo");
    check(r.all_pass(), "harness: phase_01 passthrough pasa todo");
}

void test_events_clean() {
    auto ev = make_events(100, 0.8, 0.8);
    DivergenceReport r = analyze_events(ev, Thresholds{}, "jni");
    check(r.pass, "events: limpio pasa");
    check_near(r.max_gap_hops, 0.0, 0.01, "events: limpio gap 0");
    check(r.reconvergence_hops < 0.0, "events: limpio reconvergencia n/a");
}

void test_events_divergent() {
    auto ev = make_events(200, 0.8, 0.8);
    for (int i = 50; i < 125; ++i) ev[i].app_strength = 0.3;
    DivergenceReport r = analyze_events(ev, Thresholds{}, "jni");
    check(!r.pass, "events: divergencia 200 ms no pasa");
    check_near(r.max_gap_ms, 200.0, 5.0, "events: divergencia gap 200 ms");
    check_near(r.max_gap_hops, 75.0, 2.0, "events: divergencia gap 75 hops");
    check_near(r.max_strength_diff, 0.5, 0.01, "events: divergencia max_diff 0.5");
}

void test_events_stale() {
    auto ev = make_events(100, 0.8, 0.8);
    for (int i = 50; i < 100; ++i) {
        ev[i].req_strength = 0.2;
        ev[i].snapshot_id = 1049;  // congelado (el del evento 49)
    }
    DivergenceReport r = analyze_events(ev, Thresholds{}, "jni");
    check(r.stale_snapshot, "events: snapshot congelado detectado");
    check(!r.pass, "events: snapshot obsoleto no pasa");
}

void test_events_reconvergence() {
    auto ev = make_events(100, 0.8, 0.8);
    for (int i = 20; i < 30; ++i) ev[i].connected = false;
    for (int i = 30; i < 32; ++i) ev[i].app_strength = 0.3;
    DivergenceReport r = analyze_events(ev, Thresholds{}, "jni");
    check(r.pass, "events: reconvergencia en 2 hops pasa (JNI, límite 3)");
    check_near(r.reconvergence_hops, 2.0, 0.5, "events: reconvergencia 2 hops");
    auto ev2 = make_events(100, 0.8, 0.8);
    for (int i = 20; i < 30; ++i) ev2[i].connected = false;
    for (int i = 30; i < 35; ++i) ev2[i].app_strength = 0.3;
    DivergenceReport r2 = analyze_events(ev2, Thresholds{}, "jni");
    check(!r2.pass, "events: reconvergencia en 5 hops no pasa (JNI, límite 3)");
    check_near(r2.reconvergence_hops, 5.0, 0.5, "events: reconvergencia 5 hops");
}

void test_events_roundtrip(const fs::path& tmp) {
    auto ev = make_events(10, 0.8, 0.75);
    const fs::path p = tmp / "events.jsonl";
    {
        std::ofstream f(p);
        for (const auto& e : ev) {
            f << "{\"t_capture_ns\":" << e.t_ns << ",\"route\":\"" << e.route
              << "\",\"requested\":{\"mode\":" << e.req_mode
              << ",\"strength\":" << e.req_strength << "},\"applied\":{\"mode\":" << e.app_mode
              << ",\"strength\":" << e.app_strength
              << "},\"connected\":" << (e.connected ? "true" : "false")
              << ",\"snapshot_id\":" << e.snapshot_id << "}\n";
        }
    }
    auto back = read_events(p.string());
    check(back.size() == ev.size(), "events: roundtrip cuenta");
    check(back[3].t_ns == ev[3].t_ns, "events: roundtrip t_ns");
    check(back[3].req_strength == ev[3].req_strength, "events: roundtrip req_strength");
    check(back[3].app_strength == ev[3].app_strength, "events: roundtrip app_strength");
    check(back[3].snapshot_id == ev[3].snapshot_id, "events: roundtrip snapshot_id");
}

}  // namespace

int main() {
    const fs::path tmp = fs::temp_directory_path() / "adversarial_tests";
    fs::create_directories(tmp);
    test_wav_roundtrip(tmp);
    test_gen_all_cases();
    test_passthrough_sine();
    test_delay_128();
    test_inverted();
    test_gain();
    test_transient_passthrough();
    test_nan();
    test_phase_01();
    test_events_clean();
    test_events_divergent();
    test_events_stale();
    test_events_reconvergence();
    test_events_roundtrip(tmp);
    fs::remove_all(tmp);
    std::cout << "adversarial_tests: " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed\n";
    return g_failures == 0 ? 0 : 1;
}
