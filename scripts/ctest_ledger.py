#!/usr/bin/env python3
r"""ctest_ledger.py — C13/510: ledger de tiempos por test + reparto por etiqueta.

Lee el JUnit que genera CTest (`ctest --output-junit`, C5) y publica:
  * total de tests, pasados/fallados, tiempo total de ejecucion;
  * top-N de los tests mas lentos (con su etiqueta de area);
  * reparto por etiqueta de area (dsp/acoustic/daemon/spatial/fused):
    conteo y tiempo acumulado;
  * balance de los shards definidos en tests-host.yml (conteo y tiempo
    por shard, con el test mas lento de cada uno).

La etiqueta de cada test se lee del propio JUnit (propiedad cmake_labels
que CTest escribe en cada <testcase>); --test-list es solo un respaldo
para JUnit sin propiedades. Sin ninguna de las dos, los tests quedan con
etiqueta UNKNOWN y el reparto por etiqueta se marca incompleto — el
script NO inventa etiquetas.

Uso:
  # Ledger completo de una corrida (JUnit + lista de tests del build):
  ctest --test-dir build/tests-host --output-junit build/tests-host/ctest_junit.xml \
        -LE QUARANTINED -j4 --timeout 300
  ctest --test-dir build/tests-host --show-only=json-v1 > build/tests-host/ctest_list.json
  python3 scripts/ctest_ledger.py --junit build/tests-host/ctest_junit.xml \
      --test-list build/tests-host/ctest_list.json

  # Solo top-20 (sin reparto por etiqueta):
  python3 scripts/ctest_ledger.py --junit build/tests-host/ctest_junit.xml --top 20

  # Linea resumen fusionada para check_docs_claims.py --generate-from-log
  # (un log con varios shards trae una linea resumen por shard y el
  # generador toma la primera: este modo escribe UNA linea con los
  # totales reales de todos los JUnit):
  python3 scripts/ctest_ledger.py --junit shard1.xml shard2.xml shard3.xml \
      --merged-summary build/tests-host/ctest_merged.log

Solo stdlib. No toca la red. Exit 1 si el JUnit no existe o no parsea.
"""

from __future__ import annotations

import argparse
import json
import sys
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path

# Shards de .github/workflows/tests-host.yml (job ctest, matriz shard).
# Mantener sincronizado a mano con el workflow: si cambia un shard ahi,
# cambia aqui (el script avisa si un shard del workflow no aparece o
# si una etiqueta no pertenece a ningun shard).
# Shards de .github/workflows/tests-host.yml (job ctest, matriz shard).
# La etiqueta PRIMARIA de un test es su slow-label si la tiene (slow-a /
# slow-b, los 5 binarios 'fused' que concentran el 65% del tiempo) y si no
# su etiqueta de area. Asi cada test cae en EXACTAMENTE un shard.
SHARDS = {
    "shard-1-light": {"dsp", "acoustic", "daemon"},
    "shard-2-spatial-fused": {"spatial", "fused"},
    "shard-3-slow-a": {"slow-a"},
    "shard-4-slow-b": {"slow-b"},
}
AREA_LABELS = {"dsp", "acoustic", "daemon", "spatial", "fused"}
SLOW_LABELS = {"slow-a", "slow-b"}
# slow-a/slow-b SON area fused (segundo nivel de sharding); el area de un
# test slow es fused aunque su etiqueta primaria sea slow-a/slow-b.
AREA_OF = {"slow-a": "fused", "slow-b": "fused"}


def parse_junit(path: Path):
    """Devuelve (tests, wall_time): lista de dicts y wall-clock del JUnit.

    La etiqueta de area se lee del propio JUnit: CTest escribe en cada
    <testcase> una propiedad cmake_labels con las LABELS del test. Es la
    fuente autorizada (no depende de adivinar el binario por el nombre).
    El wall-clock es el atributo time del <testsuite> raiz (lo que CTest
    imprime como "Total Test time (real)"), NO la suma de tiempos por
    test: con shards en paralelo, el wall del total es el del shard mas
    lento, no la suma.
    """
    try:
        root = ET.parse(path).getroot()
    except (ET.ParseError, OSError) as exc:
        print(f"[ERROR] No se pudo parsear {path}: {exc}", file=sys.stderr)
        sys.exit(1)
    wall_time = 0.0
    for ts in root.iter("testsuite"):
        try:
            wall_time = max(wall_time, float(ts.get("time", "0") or 0))
        except ValueError:
            pass
    tests = []
    for tc in root.iter("testcase"):
        name = tc.get("name", "?")
        try:
            t = float(tc.get("time", "0") or 0)
        except ValueError:
            t = 0.0
        status = "passed"
        if tc.find("failure") is not None:
            status = "failed"
        elif tc.find("error") is not None:
            status = "error"
        elif tc.find("skipped") is not None:
            status = "skipped"
        label = None
        props = tc.find("properties")
        if props is not None:
            for prop in props.iter("property"):
                if prop.get("name") == "cmake_labels":
                    # Primaria: slow-a/slow-b mandan sobre el area (un test
                    # slow pertenece a su shard slow, no al shard de area).
                    got = set((prop.get("value") or "").split(";"))
                    slow = sorted(got & SLOW_LABELS)
                    areas = sorted(got & AREA_LABELS)
                    label = slow[0] if slow else (areas[0] if areas else None)
        tests.append({"name": name, "time": t, "status": status, "label": label})
    return tests, wall_time


def load_labels(test_list_path: Path | None):
    """name del test -> etiqueta de area (o None). Desde --show-only=json-v1."""
    if test_list_path is None:
        return {}
    try:
        data = json.loads(test_list_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"[ERROR] No se pudo leer {test_list_path}: {exc}", file=sys.stderr)
        sys.exit(1)
    labels = {}
    for t in data.get("tests", []):
        name = t.get("name", "")
        # json-v1 no trae clave "labels": las LABELS viven en properties.
        got = set()
        for prop in t.get("properties", []):
            if prop.get("name") == "LABELS":
                got.update(prop.get("value", []))
        slow = sorted(got & SLOW_LABELS)
        area = sorted(got & AREA_LABELS)
        labels[name] = slow[0] if slow else (area[0] if area else None)
    return labels


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--junit", nargs="+", required=True,
                    help="uno o mas ctest_junit.xml (varios = varios shards)")
    ap.add_argument("--test-list", default=None,
                    help="ctest --show-only=json-v1 del build (para etiquetas)")
    ap.add_argument("--top", type=int, default=20, help="top-N mas lentos")
    ap.add_argument("--merged-summary", default=None,
                    help="escribe log con UNA linea resumen fusionada")
    args = ap.parse_args()

    labels = load_labels(Path(args.test_list) if args.test_list else None)
    all_tests = []
    walls = []
    for j in args.junit:
        ts, w = parse_junit(Path(j))
        all_tests.extend(ts)
        walls.append(w)
    wall_max = max(walls) if walls else 0.0
    total = len(all_tests)
    failed = [t for t in all_tests if t["status"] in ("failed", "error")]
    total_time = sum(t["time"] for t in all_tests)
    for t in all_tests:
        # cmake_labels del JUnit manda; el --test-list es solo respaldo
        # (p. ej. JUnit viejos sin propiedades).
        if t["label"] is None:
            t["label"] = labels.get(t["name"])
        t["area"] = AREA_OF.get(t["label"], t["label"])

    print(f"Tests: {total} ({total - len(failed)} pasados, {len(failed)} fallados) "
          f"en {total_time:.2f} s (suma de tiempos por test, no wall-clock)")
    if wall_max:
        print(f"Wall-clock (maximo por JUnit, no suma): {wall_max:.2f} s")
    if failed:
        print("Fallados:")
        for t in failed:
            print(f"  FAIL {t['name']} ({t['time']:.2f} s)")

    print(f"\nTop-{args.top} tests mas lentos:")
    for i, t in enumerate(sorted(all_tests, key=lambda x: -x["time"])[: args.top], 1):
        lab = t["label"] or "UNKNOWN"
        print(f"  {i:2d}. {t['time']:8.2f} s  [{lab:8s}] {t['name']}")

    unlabeled = [t for t in all_tests if t["label"] is None]
    print("\nReparto por etiqueta de area (slow-a/slow-b cuentan como fused):")
    by_label: dict[str, list] = defaultdict(list)
    for t in all_tests:
        by_label[t["area"] or "UNKNOWN"].append(t)
    for lab in sorted(by_label, key=lambda l: -sum(x["time"] for x in by_label[l])):
        ts = by_label[lab]
        print(f"  {lab:8s}: {len(ts):5d} tests, {sum(x['time'] for x in ts):9.2f} s")
    if unlabeled and args.test_list:
        print(f"  AVISO: {len(unlabeled)} tests sin etiqueta de area en la "
              f"lista de CTest (revisar CMakeLists.txt).")
    elif unlabeled:
        print("  AVISO: sin --test-list no hay reparto por etiqueta "
              "(todos UNKNOWN); pasar el json-v1 del build.")

    print("\nBalance de shards (segun tests-host.yml):")
    for shard, labs in SHARDS.items():
        ts = [t for t in all_tests if t["label"] in labs]
        slowest = max((t["time"] for t in ts), default=0.0)
        print(f"  {shard}: {len(ts):5d} tests, "
              f"{sum(x['time'] for x in ts):9.2f} s, test mas lento {slowest:.2f} s")
    covered = [t for t in all_tests if t["label"] in
               {l for labs in SHARDS.values() for l in labs}]
    print(f"  Cobertura de shards: {len(covered)}/{total} tests "
          f"({100.0 * len(covered) / total if total else 0:.1f}%)")
    missing_labels = (AREA_LABELS | SLOW_LABELS) - {l for labs in SHARDS.values() for l in labs}
    if missing_labels:
        print(f"  AVISO: etiquetas sin shard: {sorted(missing_labels)}")

    if args.merged_summary:
        passed = total - len(failed)
        pct = 100 * passed // total if total else 0
        # Duracion = wall del shard mas lento (max), NO la suma: los shards
        # corren en paralelo y la suma inflaria el tiempo ~4x (1498 s en
        # vez de ~380 s). Si no hay wall en el JUnit, se usa la suma.
        duration = wall_max if wall_max else total_time
        # El job Ledger de la CI no compila (solo baja artefactos): build/
        # no existe ahi. Crear el padre o write_text levanta FileNotFoundError
        # y el paso muere (reproducido en local 2026-11-28).
        Path(args.merged_summary).parent.mkdir(parents=True, exist_ok=True)
        Path(args.merged_summary).write_text(
            f"{pct}% tests passed, {len(failed)} tests failed out of {total}\n"
            f"Total Test time (real) = {duration:.2f} sec\n",
            encoding="utf-8",
        )
        print(f"\nResumen fusionado escrito en {args.merged_summary} "
              f"({passed}/{total}, wall {duration:.2f} s)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
