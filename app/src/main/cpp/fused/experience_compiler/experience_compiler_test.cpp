// experience_compiler_test.cpp — Tests host de P8.1 + P8.2 + P8.3.
//
// Standalone (propio main(), sin gtest), mismo patrón que P6/P7.
// Cubre lo que exige el plan (PLAN_P7_P10.md §3.2):
//   1. P8.1: ExperienceVector — serialización, límites, distancia.
//   2. P8.2: región alcanzable — rangos sin medir se declaran "sin
//      medir" (Unknown), nunca se fingen como medidos.
//   3. P8.3 (gate): 50 intenciones de prueba compilan sin violar 0.0 dB
//      ni salir de [0,1]; las inalcanzables se declaran como tales y se
//      devuelve el punto más cercano, nunca se fuerzan.
//   4. Fail-closed: NaN y valores fuera de rango no producen amounts
//      fuera de [0,1] ni ganancia neta distinta de 0.0 dB.
#include "experience_compiler.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace ivanna::fused::experience_compiler;

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

ExperienceVector vec(std::array<double, 7> vals, double unc = 0.5) {
    ExperienceVector e;
    e.v = vals;
    e.uncertainty.fill(unc);
    return e;
}

// Generador determinista (LCG) para las 50 intenciones del gate.
std::uint32_t g_seed = 0x12345678u;
double rnd01() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return (g_seed >> 8) / 16777216.0;
}

}  // namespace

int main() {
    std::printf("P8 experience_compiler: %zu componentes, %zu operadores\n",
                kNumComponents, kNumOperators);

    // ── 1. P8.1: límites y distancia ─────────────────────────────────────
    {
        ExperienceVector e = vec({0.0, 0.25, 0.5, 0.75, 1.0, 0.1, 0.9});
        check(e.isValid(), "vector en [0,1]: valido");
        check(e[Component::kImage] == 0.0 &&
                  e[Component::kAttention] == 0.9,
              "acceso por componente");
        ExperienceVector bad = vec({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.5});
        check(!bad.isValid(), "componente 1.5: invalido");
        ExperienceVector nan = vec({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
        nan.v[2] = std::nan("");
        check(!nan.isValid(), "componente NaN: invalido");
        nan.clampInPlace();
        check(nan.isValid() && nan.v[2] == 0.0 &&
                  nan.uncertainty[2] == 1.0,
              "clamp: NaN -> 0 con incertidumbre 1.0");
        ExperienceVector over = vec({-0.2, 1.3, 0.5, 0.5, 0.5, 0.5, 0.5});
        over.clampInPlace();
        check(over.isValid() && over.v[0] == 0.0 && over.v[1] == 1.0,
              "clamp: fuera de rango -> borde [0,1]");
        // Distancia: (1,0,...) vs (0,0,...) = 1.0 exacto.
        ExperienceVector a = vec({1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}, 0.0);
        ExperienceVector b = vec({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}, 0.0);
        check(near(distance(a, b), 1.0, 1e-12), "distancia unitaria = 1.0");
        check(distance(a, a) == 0.0, "distancia a si mismo = 0");
        ExperienceVector nanFresh = vec({0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0});
        nanFresh.v[2] = std::nan("");
        check(std::isinf(distance(a, nanFresh)),
              "distancia con NaN: +inf (no se inventa)");
        check(std::strcmp(componentName(Component::kDepth),
                          "profundidad") == 0,
              "nombre componente profundidad");
    }

    // ── 2. P8.1: serialización ida y vuelta ──────────────────────────────
    {
        ExperienceVector e = vec({0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7}, 0.25);
        std::string s = serialize(e);
        ExperienceVector back;
        check(parse(s, &back), "parse de lo serializado: ok");
        bool same = true;
        for (std::size_t i = 0; i < kNumComponents; ++i) {
            if (!near(back.v[i], e.v[i], 1e-6) ||
                !near(back.uncertainty[i], e.uncertainty[i], 1e-6)) {
                same = false;
            }
        }
        check(same, "serializacion ida y vuelta conserva el vector");
        check(!parse("basura", &back), "parse de basura: false");
        check(!parse("E=0,0,0,0,0,0,2.0;U=0,0,0,0,0,0,0", &back),
              "parse fuera de [0,1]: false");
    }

    // ── 3. P8.2: región alcanzable — nada medido hoy ────────────────────
    {
        ReachableRegion region;  // default: [0,1], measured=false
        bool allUnmeasured = true;
        for (std::size_t i = 0; i < kNumComponents; ++i) {
            if (region.measured[i]) allUnmeasured = false;
            check(region.min[i] == 0.0 && region.max[i] == 1.0,
                  "region por defecto: [0,1] por componente");
        }
        check(allUnmeasured,
              "region 2026-11-24: ningun rango medido (P3/P6/P4)");
        ExperienceVector e = vec({0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5});
        check(classify(e, region) == Reachability::kUnknown,
              "objetivo en [0,1] con rangos sin medir: Unknown");
        ExperienceVector out = vec({1.2, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5});
        check(classify(out, region) == Reachability::kUnreachable,
              "objetivo fuera de [0,1]: Unreachable");
        // Con un rango medido, fuera de él es inalcanzable.
        ReachableRegion measured = region;
        measured.measured[0] = true;
        measured.min[0] = 0.2;
        measured.max[0] = 0.6;
        ExperienceVector inside = vec({0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5});
        check(classify(inside, measured) == Reachability::kUnknown,
              "dentro del rango medido pero otros sin medir: Unknown");
        ExperienceVector outside = vec({0.9, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5});
        check(classify(outside, measured) == Reachability::kUnreachable,
              "fuera del rango medido: Unreachable");
    }

    // ── 4. P8.3 (gate): 50 intenciones compilan, 0.0 dB, [0,1] ──────────
    {
        ReachableRegion region;
        bool allOk = true;
        int unreachable = 0;
        for (int t = 0; t < 50; ++t) {
            ExperienceVector target;
            if (t < 40) {
                // 40 intenciones dentro de [0,1] (pseudoaleatorias).
                for (std::size_t i = 0; i < kNumComponents; ++i) {
                    target.v[i] = rnd01();
                    target.uncertainty[i] = 0.5;
                }
            } else {
                // 10 intenciones fuera de [0,1]: deben declararse
                // inalcanzables y devolver el punto más cercano.
                for (std::size_t i = 0; i < kNumComponents; ++i) {
                    target.v[i] = (t % 2 == 0) ? 1.0 + 0.1 * (i + 1)
                                               : -0.1 * (i + 1);
                    target.uncertainty[i] = 0.5;
                }
            }
            CompileResult r = compile(target, region);
            if (!r.ok) allOk = false;
            if (std::fabs(r.netGainDb) > 1e-9) allOk = false;
            for (std::size_t o = 0; o < kNumOperators; ++o) {
                if (!std::isfinite(r.amounts[o]) || r.amounts[o] < 0.0 ||
                    r.amounts[o] > 1.0) {
                    allOk = false;
                }
            }
            if (!r.nearestPoint.isValid()) allOk = false;
            if (t >= 40) {
                if (r.reachability != Reachability::kUnreachable) {
                    allOk = false;
                } else {
                    ++unreachable;
                    if (r.distanceToTarget <= 0.0) allOk = false;
                }
            } else {
                if (r.reachability != Reachability::kUnknown) allOk = false;
                if (r.distanceToTarget != 0.0) allOk = false;
            }
            // El trim compensa exactamente la ganancia nominal: verificar
            // a mano que trim == -sum(g_i * amount_i).
            double gain = 0.0;
            for (std::size_t o = 0; o < kNumOperators; ++o) {
                gain += nominalGainDbPerUnit(o) * r.amounts[o];
            }
            if (!near(r.trimDb, -gain, 1e-12)) allOk = false;
        }
        check(allOk, "gate P8.3: 50 intenciones, 0.0 dB, amounts en [0,1]");
        check(unreachable == 10,
              "gate P8.3: 10/10 inalcanzables declaradas, ninguna forzada");
    }

    // ── 5. Fail-closed ───────────────────────────────────────────────────
    {
        ReachableRegion region;
        ExperienceVector nan = vec({0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5});
        nan.v[0] = std::nan("");
        CompileResult r = compile(nan, region);
        check(r.ok && r.reachability == Reachability::kUnreachable,
              "intencion con NaN: inalcanzable, no se compila el NaN");
        bool amountsOk = true;
        for (double a : r.amounts) {
            if (!std::isfinite(a) || a < 0.0 || a > 1.0) amountsOk = false;
        }
        check(amountsOk && std::fabs(r.netGainDb) <= 1e-9,
              "intencion con NaN: amounts en [0,1], ganancia 0.0 dB");
        // Intenciones de referencia del plan (§3.2 paso 5): conservar
        // sala / conservar voces / conservar agresividad.
        ExperienceVector sala = vec({0.5, 0.9, 0.8, 0.5, 0.5, 0.8, 0.5});
        CompileResult rs = compile(sala, region);
        check(rs.ok && std::fabs(rs.netGainDb) <= 1e-9,
              "intencion conservar-sala compila con 0.0 dB");
        ExperienceVector voces = vec({0.9, 0.5, 0.3, 0.7, 0.6, 0.9, 0.5});
        CompileResult rv = compile(voces, region);
        check(rv.ok && std::fabs(rv.netGainDb) <= 1e-9,
              "intencion conservar-voces compila con 0.0 dB");
        ExperienceVector agres = vec({0.6, 0.4, 0.7, 0.9, 0.9, 0.7, 0.6});
        CompileResult ra = compile(agres, region);
        check(ra.ok && std::fabs(ra.netGainDb) <= 1e-9,
              "intencion conservar-agresividad compila con 0.0 dB");
    }

    std::printf("P8: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
