#!/usr/bin/env python3
"""check_acoustic_golden.py — compara la salida del harness acústico offline
(test_acoustic_harness) con el Golden-Master
tests/golden/acoustic_harness_baseline.json y falla si F0/F/L se desvían
más que la tolerancia.

Uso:
    python3 scripts/check_acoustic_golden.py <log> [golden.json]
    python3 scripts/check_acoustic_golden.py build/tests-host/ctest_last.log

El <log> es la salida del binario test_acoustic_harness o el log de ctest
(ctest --test-dir build/tests-host -R AcousticHarness --output-on-failure).

Qué compara (S7/510):
  * Filas HARNESS <cfg> <estímulo> ... : peak L/R, True Peak, THD+N (tono),
    tono emergente (tono), crest L/R, balance, lag, corr@0, ringing
    (impulso). clip y overCeil deben ser exactamente 0. El valor -320.00
    es el centinela «no medido en ese estímulo» y se ignora.
  * Filas HARNESS ERB <cfg> banda <n> <fc> Hz lvl <L>/<R> dB corr <c> :
    firma espectral por banda (barrido) + correlación intercanal.
  * Filas HARNESS <cfg> cpu musica ... : SOLO presupuesto (media y p95
    <= 2667 us/hop). El CPU NO se compara contra el valor golden: la media
    varía con la carga de la máquina (p. ej. F: 621 us en la primera
    corrida, 866 us en la 509 g++-14, 1278 us en clang-18) y pincharla
    sería un falso positivo. El valor golden queda como referencia.

Tolerancias: viven en el JSON (campo "tolerances"). Son CONSERVADORAS y
pendientes de medir: las métricas no-CPU fueron bit-idénticas (a 2
decimales) en 3 corridas x 2 toolchains (g++-14, clang++-18, 2026-11-27),
así que ±0.5 dB / ±0.02 corr / ±1.0 ms ringing es holgado frente a la
dispersión observada (0.00). Antes de endurecerlas hay que medir la
dispersión en 5 corridas por toolchain (o fijar baselines por compilador).

Actualización del golden: SOLO vía UPDATE_GOLDEN=1 + PR con justificación.
Nunca automática silenciosa. Con UPDATE_GOLDEN=1 el script reescribe los
valores medidos del JSON desde el log (tolerancias y metadatos intactos)
y avisa de que hace falta el PR.

Solo stdlib. Exit: 0 PASS, 1 FAIL (desviación o filas que faltan),
2 error de uso / archivos.
"""

import json
import math
import os
import re
import sys

DEFAULT_GOLDEN = "tests/golden/acoustic_harness_baseline.json"

ROW_RE = re.compile(
    r"^HARNESS\s+(N|F0|F|L)\s+(tono1k|barrido|rosa|impulso|musica)\s+"
    r"peak\s+([+-][0-9.]+)/([+-][0-9.]+)\s+dBFS\s+clip\s+(\d+)\s+overCeil\s+(\d+)\s+"
    r"tp\s+([+-][0-9.]+)\s+dBTP\s+thdn\s+([+-][0-9.]+)/([+-][0-9.]+)\s+dB\s+"
    r"emerg\s+([+-][0-9.]+)\s+dB\s+@\s+([0-9.]+)\s+Hz\s+"
    r"crest\s+([+-][0-9.]+)/([+-][0-9.]+)\s+dB\s+\(in\s+[+-][0-9.]+\)\s+"
    r"bal\s+([+-][0-9.]+)\s+dB\s+lag\s+([+-]\d+)\s+corr\s+([0-9.]+)\s+"
    r"ring\s+([0-9.]+)\s+ms\s+cpu\s+([0-9.]+)/([0-9.]+)/([0-9.]+)\s+us"
)
ERB_RE = re.compile(
    r"^HARNESS\s+ERB\s+(N|F0|F|L)\s+banda\s+(\d+)\s+([0-9.]+)\s+Hz\s+"
    r"lvl\s+([+-][0-9.]+)\s*/\s*([+-][0-9.]+)\s+dB\s+corr\s+([+-][0-9.]+)"
)
CPU_RE = re.compile(
    r"^HARNESS\s+(F|L)\s+cpu\s+musica\s+media\s+([0-9.]+)\s+us\s+"
    r"p95\s+([0-9.]+)\s+us\s+max\s+([0-9.]+)\s+us\s+\((\d+)\s+chunks\)"
)
PREDICT_RE = re.compile(
    r"^HARNESS\s+PREDICT\s+(\S+)\s+rms\s+predicho\s+([+-][0-9.]+)\s+dB\s+"
    r"\(base\s+([+-][0-9.]+)\s+dB,\s+objetivo\s+<=\s+([0-9.]+)\s+dB\)\s+"
    r"puerta\s+(PASA|RECHAZA)"
)

NOT_MEASURED = -320.0


def parse_log(text):
    rows = {}
    erb = {}
    cpu = {}
    predicts = []
    for line in text.splitlines():
        line = line.strip()
        m = ROW_RE.match(line)
        if m:
            cfg, stim = m.group(1), m.group(2)
            rows[(cfg, stim)] = {
                "peak_l_db": float(m.group(3)),
                "peak_r_db": float(m.group(4)),
                "clip": int(m.group(5)),
                "over_ceiling": int(m.group(6)),
                "true_peak_dbtp": float(m.group(7)),
                "thdn_l_db": float(m.group(8)),
                "thdn_r_db": float(m.group(9)),
                "emergent_db": float(m.group(10)),
                "emergent_hz": float(m.group(11)),
                "crest_l_db": float(m.group(12)),
                "crest_r_db": float(m.group(13)),
                "balance_db": float(m.group(14)),
                "lag": int(m.group(15)),
                "corr0": float(m.group(16)),
                "ring_ms": float(m.group(17)),
            }
            continue
        m = ERB_RE.match(line)
        if m:
            cfg, band = m.group(1), int(m.group(2))
            erb[(cfg, band)] = {
                "center_hz": float(m.group(3)),
                "lvl_l_db": float(m.group(4)),
                "lvl_r_db": float(m.group(5)),
                "corr": float(m.group(6)),
            }
            continue
        m = CPU_RE.match(line)
        if m:
            cpu[m.group(1)] = {
                "mean_us": float(m.group(2)),
                "p95_us": float(m.group(3)),
                "max_us": float(m.group(4)),
                "chunks": int(m.group(5)),
            }
            continue
        m = PREDICT_RE.match(line)
        if m:
            predicts.append({
                "candidate": m.group(1),
                "predicted_rms_db": float(m.group(2)),
                "baseline_rms_db": float(m.group(3)),
                "target_rms_db": float(m.group(4)),
                "gate": m.group(5),
            })
    return rows, erb, cpu, predicts


def check_value(name, got, want, tol, failures, notes):
    if want is None or got is None:
        return
    if abs(want - NOT_MEASURED) < 1e-9 or abs(got - NOT_MEASURED) < 1e-9:
        return  # centinela «no medido en ese estímulo»
    dev = abs(got - want)
    if dev > tol:
        failures.append(
            f"  FAIL {name}: medido {got:+.2f} vs golden {want:+.2f} "
            f"(desviación {dev:.2f} > tolerancia {tol})"
        )
    else:
        notes.append(f"  ok {name}: {got:+.2f} vs {want:+.2f} (tol {tol})")


def main(argv):
    if len(argv) < 2 or len(argv) > 3:
        print(__doc__)
        print("Uso: check_acoustic_golden.py <log> [golden.json]", file=sys.stderr)
        return 2
    log_path, golden_path = argv[1], argv[2] if len(argv) == 3 else DEFAULT_GOLDEN
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError as e:
        print(f"ERROR: no se puede leer el log {log_path}: {e}", file=sys.stderr)
        return 2
    try:
        with open(golden_path, encoding="utf-8") as f:
            golden = json.load(f)
    except (OSError, json.JSONDecodeError) as e:
        print(f"ERROR: no se puede leer el golden {golden_path}: {e}", file=sys.stderr)
        return 2

    update = os.environ.get("UPDATE_GOLDEN") == "1"
    rows, erb, cpu, predicts = parse_log(text)
    tol = golden.get("tolerances", {})
    failures, notes = [], []
    checked = 0

    # --- Filas de métricas -------------------------------------------------
    for entry in golden.get("rows", []):
        key = (entry["cfg"], entry["stimulus"])
        got = rows.get(key)
        if got is None:
            failures.append(f"  FAIL fila {key[0]} {key[1]}: no aparece en el log")
            continue
        checked += 1
        label = f"{key[0]} {key[1]}"
        if update:
            for field in ("peak_l_db", "peak_r_db", "true_peak_dbtp", "thdn_l_db",
                          "thdn_r_db", "emergent_db", "emergent_hz", "crest_l_db",
                          "crest_r_db", "balance_db", "corr0", "ring_ms"):
                entry[field] = got[field]
            entry["lag"] = got["lag"]
            entry["clip"] = got["clip"]
            entry["over_ceiling"] = got["over_ceiling"]
            continue
        for field, tkey in (
            ("peak_l_db", "peak_db"), ("peak_r_db", "peak_db"),
            ("true_peak_dbtp", "true_peak_dbtp"),
            ("thdn_l_db", "thdn_db"), ("thdn_r_db", "thdn_db"),
            ("emergent_db", "emergent_db"),
            ("crest_l_db", "crest_db"), ("crest_r_db", "crest_db"),
            ("balance_db", "balance_db"), ("corr0", "corr0"),
            ("ring_ms", "ring_ms"),
        ):
            check_value(f"{label} {field}", got[field], entry.get(field),
                        tol.get(tkey, 0.5), failures, notes)
        if got["lag"] != entry.get("lag"):
            failures.append(f"  FAIL {label} lag: medido {got['lag']} vs golden {entry.get('lag')} (exacto)")
        if got["clip"] != 0 or entry.get("clip", 0) != 0:
            failures.append(f"  FAIL {label} clip: medido {got['clip']} (debe ser 0)")
        if got["over_ceiling"] != 0 or entry.get("over_ceiling", 0) != 0:
            failures.append(f"  FAIL {label} overCeil: medido {got['over_ceiling']} (debe ser 0)")

    # --- Firma ERB por banda ------------------------------------------------
    for band in golden.get("erb_signature", {}).get("bands", []):
        b = band["band"]
        for cfg in ("N", "F0", "F", "L"):
            got = erb.get((cfg, b))
            if got is None:
                failures.append(f"  FAIL ERB {cfg} banda {b}: no aparece en el log")
                continue
            checked += 1
            if update:
                # En modo update la entrada por cfg puede no existir aún
                # (primera vez que se congela la firma): se crea.
                want = band.setdefault(cfg, {})
                want["lvl_l_db"], want["lvl_r_db"], want["corr"] = (
                    got["lvl_l_db"], got["lvl_r_db"], got["corr"])
                want["center_hz"] = got["center_hz"]
                continue
            want = band.get(cfg)
            if want is None:
                continue
            check_value(f"ERB {cfg} banda {b} lvl_l", got["lvl_l_db"], want.get("lvl_l_db"),
                        tol.get("erb_level_db", 0.5), failures, notes)
            check_value(f"ERB {cfg} banda {b} lvl_r", got["lvl_r_db"], want.get("lvl_r_db"),
                        tol.get("erb_level_db", 0.5), failures, notes)
            check_value(f"ERB {cfg} banda {b} corr", got["corr"], want.get("corr"),
                        tol.get("erb_corr", 0.02), failures, notes)

    # --- CPU: solo presupuesto, nunca valor golden --------------------------
    budget = golden.get("cpu", {}).get("budget_us", 2667)
    for cfg, want in golden.get("cpu", {}).get("configs", {}).items():
        got = cpu.get(cfg)
        if got is None:
            failures.append(f"  FAIL CPU {cfg}: no aparece la fila 'cpu musica' en el log")
            continue
        checked += 1
        if update:
            # UPDATE_GOLDEN no toca la referencia CPU: es solo referencia
            # (el check es contra presupuesto) y su valor queda ligado a
            # la medición que lo fijó; cambiarla es edición manual con
            # justificación en el PR, no reescritura automática.
            continue
        for field in ("mean_us", "p95_us"):
            if got[field] > budget:
                failures.append(
                    f"  FAIL CPU {cfg} {field}: {got[field]:.0f} us > presupuesto {budget} us/hop")
            else:
                notes.append(f"  ok CPU {cfg} {field}: {got[field]:.0f} us <= {budget} us "
                             f"(referencia golden {want.get(field)} us, no se compara)")
    if cpu and not golden.get("cpu", {}).get("configs"):
        notes.append("  (sin referencia CPU en el golden; solo se aplica el presupuesto)")

    # --- PREDICT: coherencia de la puerta (no es golden) --------------------
    for p in predicts:
        target = 0.8 * p["baseline_rms_db"]
        expect = "PASA" if p["predicted_rms_db"] <= target else "RECHAZA"
        if p["gate"] != expect:
            failures.append(
                f"  FAIL PREDICT {p['candidate']}: puerta impresa {p['gate']} pero "
                f"rms {p['predicted_rms_db']:.2f} vs objetivo {target:.2f} exige {expect}")
        elif p["candidate"] not in ("SINT-PLANO",):
            notes.append(f"  ok PREDICT {p['candidate']}: rms {p['predicted_rms_db']:.2f} "
                         f"puerta {p['gate']} (coherente)")

    if update:
        with open(golden_path, "w", encoding="utf-8") as f:
            json.dump(golden, f, indent=2, ensure_ascii=False)
            f.write("\n")
        print(f"UPDATE_GOLDEN=1: {golden_path} actualizado desde {log_path}.")
        print("AVISO: esto NO es un cambio silencioso — hay que abrir un PR con "
              "justificación (qué cambió y por qué) antes de darlo por bueno.")
        return 0

    print(f"Golden: {golden_path}  |  Log: {log_path}  |  entradas comparadas: {checked}")
    for line in notes:
        print(line)
    if failures:
        print(f"RESULTADO: FAIL ({len(failures)} desviaciones)")
        for line in failures:
            print(line)
        return 1
    print("RESULTADO: PASS — F0/F/L dentro de tolerancia del Golden-Master")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
