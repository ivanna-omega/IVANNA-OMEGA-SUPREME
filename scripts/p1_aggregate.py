#!/usr/bin/env python3
"""p1_aggregate.py — P1 (2026-11-22): agrega los reportes JSON de la matriz
adversarial (p1_matrix_<caso>.json) en una tabla consolidada: qué operador
degrada qué, y cuánto.

Uso:
    python3 scripts/p1_aggregate.py [dir_con_json] [--out reporte.md]

Por defecto lee p1_matrix_*.json del directorio actual (el binario de test
los escribe en el cwd, típicamente build/tests-host). Imprime una tabla
Markdown por caso y un resumen de defectos por operador.

P0 (REFERENCE_LOCKED) sigue sin existir: P1 detecta regresiones, no
sustituye la referencia congelada. Ver PLAN_P7_P10.md §P1.
"""
import glob
import json
import os
import sys

CONFIGS = [
    "bypass", "ivanna",
    "op_neuro", "op_orbe", "op_holos", "op_sala_viva",
    "op_volterra", "op_armonia", "op_pulso",
]
SHORT = {
    "bypass": "bypass", "ivanna": "IVANNA",
    "op_neuro": "NEURO", "op_orbe": "ORBE", "op_holos": "HOLOS",
    "op_sala_viva": "SALA", "op_volterra": "VOLT",
    "op_armonia": "ARMON", "op_pulso": "PULSO",
}


def fmt(v, nd=2):
    if v is None:
        return "—"
    return f"{v:.{nd}f}"


def load_reports(d):
    reps = {}
    for path in sorted(glob.glob(os.path.join(d, "p1_matrix_*.json"))):
        with open(path) as f:
            data = json.load(f)
        reps[data["case"]] = data
    return reps


def main():
    d = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else "."
    out = None
    if "--out" in sys.argv:
        out = sys.argv[sys.argv.index("--out") + 1]
    reps = load_reports(d)
    if not reps:
        print(f"sin reportes p1_matrix_*.json en {d}")
        return 1
    lines = []
    lines.append("# P1 — Matriz adversarial: qué operador degrada qué")
    lines.append("")
    lines.append(f"Casos: {len(reps)} ({', '.join(sorted(reps))}). "
                 "Configuraciones por caso: bypass, IVANNA, y cada operador "
                 "solo (con el safety limiter siempre activo).")
    lines.append("")

    # ── Tabla de defectos por caso/config ──
    lines.append("## Defectos (falla = ✗)")
    lines.append("")
    hdr = "| caso | " + " | ".join(SHORT[c] for c in CONFIGS) + " |"
    lines.append(hdr)
    lines.append("|" + "---|" * (len(CONFIGS) + 1))
    for case in sorted(reps):
        row = [case]
        for cfg in CONFIGS:
            c = next((x for x in reps[case]["configs"] if x["config"] == cfg), None)
            if c is None:
                row.append("?")
            elif c["defect_pass"]:
                row.append("✓")
            else:
                row.append("✗ " + c["defect"].strip())
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")

    # ── Pico de salida por caso/config (clipping) ──
    lines.append("## Pico de salida (dBFS; > 0.0 = clipping digital)")
    lines.append("")
    lines.append("| caso | " + " | ".join(SHORT[c] for c in CONFIGS) + " |")
    lines.append("|" + "---|" * (len(CONFIGS) + 1))
    for case in sorted(reps):
        row = [case]
        for cfg in CONFIGS:
            c = next((x for x in reps[case]["configs"] if x["config"] == cfg), None)
            row.append(fmt(c["peak_dut_dbfs"]) if c else "—")
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")

    # ── RMS ratio por caso/config (cambio de nivel) ──
    lines.append("## RMS ratio vs entrada (dB; el procesado intencional cambia el nivel)")
    lines.append("")
    lines.append("| caso | " + " | ".join(SHORT[c] for c in CONFIGS) + " |")
    lines.append("|" + "---|" * (len(CONFIGS) + 1))
    for case in sorted(reps):
        row = [case]
        for cfg in CONFIGS:
            c = next((x for x in reps[case]["configs"] if x["config"] == cfg), None)
            row.append(fmt(c["rms_ratio_db"]) if c else "—")
        lines.append("| " + " | ".join(row) + " |")
    lines.append("")

    # ── Resumen de defectos por operador ──
    lines.append("## Defectos por operador (conteo de casos con falla)")
    lines.append("")
    for cfg in CONFIGS:
        fails = []
        for case in sorted(reps):
            c = next((x for x in reps[case]["configs"] if x["config"] == cfg), None)
            if c and not c["defect_pass"]:
                fails.append(f"{case}:{c['defect'].strip()}")
        lines.append(f"- **{SHORT[cfg]}**: {len(fails)} caso(s) con falla")
        for f in fails:
            lines.append(f"  - {f}")
    lines.append("")

    # ── Totales ──
    total = sum(len(r["configs"]) for r in reps.values())
    passed = sum(1 for r in reps.values() for c in r["configs"] if c["defect_pass"])
    lines.append(f"**Total: {passed}/{total} configuraciones sin defecto.**")
    lines.append("")
    text = "\n".join(lines)
    print(text)
    if out:
        with open(out, "w") as f:
            f.write(text + "\n")
        print(f"(escrito en {out})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
