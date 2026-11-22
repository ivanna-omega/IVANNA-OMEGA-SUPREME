#!/usr/bin/env python3
r"""
check_docs_claims.py — Fase A5 Gate de afirmaciones documentales contra código real y herramientas A3/A4.

Falla (exit 1) si `README.md` o `LÉAME.md`:
  1. Declaran un estándar `C++XX` distinto de `CMAKE_CXX_STANDARD` en `app/src/main/cpp/CMakeLists.txt`.
  2. Afirman `"0 locks"` / `"lock-free"` cuando A3 (`check_rt_safety.py`)
     reporta violaciones de MUTEX en el path RT, o `"0 malloc"` /
     `"zero-allocation"` cuando A3 reporta violaciones de ALOCACIÓN o A4
     (`test_rt_no_alloc.cpp`) no está registrado en CTest. Las categorías
     se exigen por separado: un LOG en RT no falsifica "lock-free".
  3. Afirman `"128-tap"` / `"128 taps"` para el motor híbrido cuando `kActiveTaps` en
     `app/src/main/cpp/spatial/HybridRenderer.hpp` no es 128 (también audita `SpatialAudioPanel.kt`).
  4. Contienen afirmaciones absolutas no respaldadas por telemetría:
     - `"0.00 ms"`
     - `"THD = 0"` o `"THD=0"`
     - Conteo `N/N` de CTest que no coincida con `telemetry/ctest_summary.json`.
       NO son conteos CTest y se ignoran: ratios en líneas SOFA/HRTF
       ("239/239 readable" es inventario SOFA), ratios precedidos por
       "evolution suite" (la suite Python de evolución, no CTest) y ratios
       dentro de paréntesis (anotación histórica, p. ej. "(998/998 at
       v2.5.442)").
     - `\operatorname` (inconsistente en MathJax de GitHub)

Modo `--generate-from-log <path>`:
  Parsea la salida real de `ctest` y escribe `telemetry/ctest_summary.json`.
"""

from __future__ import annotations
import argparse
import json
import re
import sys
from pathlib import Path
from typing import List, Tuple

import check_rt_safety

ROOT = Path(__file__).resolve().parent.parent
DOCS = [ROOT / "README.md", ROOT / "LÉAME.md"]
MAIN_CMAKE = ROOT / "app" / "src" / "main" / "cpp" / "CMakeLists.txt"
TESTS_CMAKE = ROOT / "app" / "src" / "main" / "cpp" / "tests" / "CMakeLists.txt"
TEST_RT_NO_ALLOC = ROOT / "app" / "src" / "main" / "cpp" / "tests" / "test_rt_no_alloc.cpp"
HYBRID_RENDERER_HPP = ROOT / "app" / "src" / "main" / "cpp" / "spatial" / "HybridRenderer.hpp"
SPATIAL_AUDIO_PANEL_KT = ROOT / "app" / "src" / "main" / "java" / "com" / "ivanna" / "omega" / "ui" / "SpatialAudioPanel.kt"
CTEST_SUMMARY_JSON = ROOT / "telemetry" / "ctest_summary.json"


# Menciones que, cuando aparecen ANTES de un ratio N/N en la misma línea,
# demuestran que el ratio es el conteo de OTRA suite/inventario, no de CTest.
# Posicional a propósito: solo vale lo que precede al ratio (un conteo CTest
# antes de la mención sí se exige, igual que la regla SOFA/HRTF).
_NON_CTEST_BEFORE_RES = (
    re.compile(r'\bsofa\b|\bhrtf\b', re.IGNORECASE),
    re.compile(r'\bevolution\s+suite\b', re.IGNORECASE),
)


def ratio_is_ctest_count(line: str, m_ratio, expected_label: str,
                         has_current_outside: bool) -> bool:
    """¿Es este ratio N/N un conteo CTest exigible contra telemetry?

    Devuelve False (no es conteo CTest, se ignora) cuando:
      - n1 != n2 (no es un conteo "todo pasó"), n1 < 50 (no es escala
        de suite) o el ratio coincide con el label vigente de telemetry;
      - el ratio está dentro de paréntesis y la misma línea trae otro
        ratio fuera de ellos (anotación histórica);
      - una mención de otra suite/inventario (SOFA/HRTF, evolution suite)
        aparece ANTES del ratio en la misma línea.
    En cualquier otro caso devuelve True: el ratio es un conteo CTest
    que debe coincidir con `telemetry/ctest_summary.json`.
    """
    n1, n2 = m_ratio.group(1), m_ratio.group(2)
    if n1 != n2 or int(n1) < 50 or f"{n1}/{n2}" == expected_label:
        return False
    depth = line[: m_ratio.start()].count("(") - line[: m_ratio.start()].count(")")
    if depth > 0 and has_current_outside:
        return False
    before = line[: m_ratio.start()]
    if any(rx.search(before) for rx in _NON_CTEST_BEFORE_RES):
        return False
    return True


def generate_from_log(log_path: Path) -> int:
    if not log_path.exists():
        print(f"[ERROR] No existe log de CTest: {log_path}", file=sys.stderr)
        return 1
    txt = log_path.read_text(encoding="utf-8", errors="replace")
    # CTest 4.4.4 imprime "100% tests passed, 0 tests failed out of N";
    # CTest 4.4.5 omite la cláusula cuando no hay fallos:
    # "100% tests passed out of N". La cláusula es opcional (failed=0).
    m = re.search(r'(\d+)%\s+tests\s+passed(?:,\s+(\d+)\s+tests\s+failed)?\s+out\s+of\s+(\d+)', txt)
    if not m:
        print("[ERROR] No se encontró línea resumen de CTest en el log.", file=sys.stderr)
        return 1
    pct = int(m.group(1))
    failed = int(m.group(2)) if m.group(2) is not None else 0
    total = int(m.group(3))
    passed = total - failed
    t_match = re.search(r'Total Test time \(real\)\s*=\s*([0-9.]+)\s*sec', txt)
    duration_sec = float(t_match.group(1)) if t_match else 0.0

    CTEST_SUMMARY_JSON.parent.mkdir(parents=True, exist_ok=True)
    payload = {
        "passed": passed,
        "failed": failed,
        "total": total,
        "pass_rate_percent": pct,
        "duration_sec": duration_sec,
        "badge_label": f"{passed}/{total}",
    }
    CTEST_SUMMARY_JSON.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"[OK] Generado {CTEST_SUMMARY_JSON.relative_to(ROOT)}: {passed}/{total} tests passed ({duration_sec:.2f}s)")
    return 0


def read_cmake_cxx_standard() -> str:
    if not MAIN_CMAKE.exists():
        return "26"
    txt = MAIN_CMAKE.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'set\s*\(\s*CMAKE_CXX_STANDARD\s+(\d+)\s*\)', txt)
    return m.group(1) if m else "20"


def read_hybrid_active_taps() -> int:
    if not HYBRID_RENDERER_HPP.exists():
        return 0
    txt = HYBRID_RENDERER_HPP.read_text(encoding="utf-8", errors="replace")
    m = re.search(r'\bkActiveTaps\s*=\s*(\d+)', txt)
    return int(m.group(1)) if m else 0


def check_a3_and_a4_status() -> Tuple[bool, bool, Dict[str, int]]:
    """Devuelve (a3_ok, a4_ok, violaciones A3 por categoría).

    a3_ok = los 36 puntos de entrada RT existen y no hay NINGUNA violación.
    Las categorías (mutex/alloc/log/throw) permiten que cada afirmación
    documental se exija solo sobre la propiedad que asevera:
      - "lock-free" / "0 locks" -> categoría mutex
      - "0 malloc" / "zero-allocation" -> categoría alloc (+ A4)
    Un LOGI en omega_process no falsifica que la cola SPSC del sensor sea
    lock-free (verificada por separado: solo atómicos en push/drain); la
    violación de log la sigue reportando A3 por su cuenta.
    """
    audit = check_rt_safety.run_audit()
    a3_ok = (
        int(audit["analyzed"]) == int(audit["total"])
        and len(audit["violations"]) == 0  # type: ignore[arg-type]
    )
    by_category = dict(audit["by_category"])  # type: ignore[arg-type]

    a4_ok = (
        TEST_RT_NO_ALLOC.exists()
        and TESTS_CMAKE.exists()
        and "test_rt_no_alloc" in TESTS_CMAKE.read_text(encoding="utf-8", errors="replace")
    )
    return a3_ok, a4_ok, by_category


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--generate-from-log", type=Path, default=None)
    parser.add_argument("--ratchet", action="store_true")
    args = parser.parse_args()

    if args.generate_from_log is not None:
        return generate_from_log(args.generate_from_log)

    findings: List[str] = []

    cmake_cxx = read_cmake_cxx_standard()
    active_taps = read_hybrid_active_taps()
    a3_ok, a4_ok, a3_by_category = check_a3_and_a4_status()
    a3_mutex_ok = a3_by_category.get("mutex", 0) == 0 and a3_by_category.get("missing", 0) == 0
    a3_alloc_ok = a3_by_category.get("alloc", 0) == 0 and a3_by_category.get("missing", 0) == 0

    ctest_data = None
    if CTEST_SUMMARY_JSON.exists():
        try:
            ctest_data = json.loads(CTEST_SUMMARY_JSON.read_text(encoding="utf-8"))
        except Exception:
            ctest_data = None

    cxx_re = re.compile(r'\bC\+\+(\d{2})\b')
    # "lock-free" asevera ausencia de bloqueos; "0 malloc"/"zero-allocation"
    # asevera ausencia de alocación. Se exigen por separado (ver
    # check_a3_and_a4_status): un LOG de A3 no falsifica ninguna de las dos.
    lock_claim_re = re.compile(r'\b(?:0\s*locks|lock-free)\b', re.IGNORECASE)
    alloc_claim_re = re.compile(r'\b(?:0\s*malloc|zero-allocation)\b', re.IGNORECASE)
    tap128_re = re.compile(r'\b128(?:-|\s+)taps?\b', re.IGNORECASE)

    for doc in DOCS:
        if not doc.exists():
            continue
        rel = doc.relative_to(ROOT)
        lines = doc.read_text(encoding="utf-8", errors="replace").splitlines()
        has_rt_claim_reported = False
        for idx, line in enumerate(lines, start=1):
            # 1) Estándar C++ vs CMAKE_CXX_STANDARD
            for m_cxx in cxx_re.finditer(line):
                doc_std = m_cxx.group(1)
                if doc_std != cmake_cxx:
                    findings.append(
                        f"  - {rel}:{idx} declara 'C++{doc_std}' pero CMAKE_CXX_STANDARD={cmake_cxx} en CMakeLists.txt"
                    )

            # 2) Afirmaciones RT, exigidas por categoría (no por A3 en bloque):
            #    "lock-free"/"0 locks" requieren 0 violaciones mutex en A3;
            #    "0 malloc"/"zero-allocation" requieren 0 violaciones alloc + A4.
            if not has_rt_claim_reported:
                m_lock = lock_claim_re.search(line)
                if m_lock and not a3_mutex_ok:
                    findings.append(
                        f"  - {rel}:{idx} afirma '{m_lock.group(0)}' pero A3 tiene "
                        f"{a3_by_category.get('mutex', 0)} violacion(es) mutex"
                    )
                    has_rt_claim_reported = True
                m_alloc = alloc_claim_re.search(line)
                if m_alloc and (not a3_alloc_ok or not a4_ok):
                    findings.append(
                        f"  - {rel}:{idx} afirma '{m_alloc.group(0)}' pero A3_alloc_ok={a3_alloc_ok}, A4_ok={a4_ok}"
                    )
                    has_rt_claim_reported = True

            # 3) Afirmación "128-tap" / "128 taps" requiere kActiveTaps == 128 en HybridRenderer.hpp
            if tap128_re.search(line) and active_taps != 128:
                findings.append(
                    f"  - {rel}:{idx} afirma '128-tap'/'128 taps' pero HybridRenderer.hpp define kActiveTaps={active_taps}"
                )

            # 4) Reglas existentes
            if "0.00 ms" in line:
                findings.append(f"  - {rel}:{idx} contiene afirmación '0.00 ms' sin medición empírica adjunta")
            if "THD = 0" in line or "THD=0" in line:
                findings.append(
                    f"  - {rel}:{idx} contiene afirmación 'THD = 0' / 'THD=0' (usar cifra medida en dB de telemetry/thd_chain.json)"
                )
            if "150/150" in line and ctest_data is None:
                findings.append(f"  - {rel}:{idx} contiene '150/150' hardcodeado sin telemetry/ctest_summary.json generado por CI")
            elif ctest_data is not None:
                expected_label = ctest_data.get("badge_label", "")
                # Un ratio N/N solo es un conteo CTest si NO es inventario
                # SOFA/HRTF ("239/239 readable" es SOFA, no CTest), NO es
                # el conteo de otra suite ("evolution suite 117/117" es la
                # suite Python de evolución) y NO es una anotación
                # histórica entre paréntesis junto al conteo vigente
                # ("(998/998 at v2.5.442 + 10 Ω tests)" en la línea del
                # 1008/1008). La decisión vive en ratio_is_ctest_count()
                # (módulo, testeable); el salto por paréntesis solo aplica
                # si la MISMA línea trae otro ratio fuera de ellos (un
                # "(N/N)" solitario sí se exige); el salto por mención
                # solo aplica si la mención aparece ANTES del ratio en la
                # línea (un conteo CTest antes de la mención se exige).
                ratios = list(re.finditer(r'\b(\d+)/(\d+)\b', line))
                has_current_outside = any(
                    (line[: m.start()].count("(") - line[: m.start()].count(")") == 0)
                    for m in ratios
                )
                for m_ratio in ratios:
                    if not ratio_is_ctest_count(
                            line, m_ratio, expected_label, has_current_outside):
                        continue
                    n1, n2 = m_ratio.group(1), m_ratio.group(2)
                    findings.append(
                        f"  - {rel}:{idx} conteo CTest '{n1}/{n2}' no coincide con telemetry/ctest_summary.json ('{expected_label}')"
                    )
            if "\\operatorname" in line:
                findings.append(f"  - {rel}:{idx} contiene '\\operatorname' (usar '\\mathrm{{sgn}}')")

    if SPATIAL_AUDIO_PANEL_KT.exists():
        rel_ui = SPATIAL_AUDIO_PANEL_KT.relative_to(ROOT)
        for idx, line in enumerate(SPATIAL_AUDIO_PANEL_KT.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
            if tap128_re.search(line) and active_taps != 128:
                findings.append(
                    f"  - {rel_ui}:{idx} afirma '128-tap'/'128 taps' en UI pero HybridRenderer.hpp define kActiveTaps={active_taps}"
                )

    print("=== [A5] CHECK DOCS CLAIMS REPORT ===")
    print(f"CMAKE_CXX_STANDARD={cmake_cxx} | HybridRenderer kActiveTaps={active_taps} | A3_ok={a3_ok} | A4_ok={a4_ok} | A3_by_category={a3_by_category}")
    if findings:
        print(f"[FAIL] Afirmaciones documentales/UI no verificadas ({len(findings)}):")
        for f in findings:
            print(f)
        print(f"[SUMMARY] Total hallazgos docs: {len(findings)}")
        if args.ratchet and len(findings) <= 11:
            print(f"[RATCHET-OK] Hallazgos documentales ({len(findings)}) <= baseline (11).")
            return 0
        return 1

    print("[PASS] 0 afirmaciones prohibidas o desincronizadas en README.md / LÉAME.md / UI.")
    print("[SUMMARY] Total hallazgos docs: 0")
    return 0


if __name__ == "__main__":
    sys.exit(main())
