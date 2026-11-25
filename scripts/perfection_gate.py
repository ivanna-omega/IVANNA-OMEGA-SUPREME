#!/usr/bin/env python3
"""perfection_gate.py — PUERTA DE PERFECCIÓN (Sección L2, prompt maestro v3).

Veredicto binario: IVANNA hace EXACTAMENTE lo que dice que hace, cada
capacidad verificada en funcionamiento real. NI MENOS (prohibido recortar /
ocultar / desactivar) NI MÁS (prohibido añadir funciones fuera del inventario).

La puerta lee el inventario, ejecuta los verificadores y las suites de tests,
cuenta TODO/FIXME/stub, pruebas en skip/xfail/disabled, controles sin
llamador y flags apagadas, y calcula 4 contadores:

    PENDIENTES = 0, FALLOS = 0, A_MEDIAS = 0, BLOQUEOS_ABIERTOS = 0

Sale con código 0 SOLO si los cuatro son cero. Nadie escribe los contadores
a mano: el script los calcula siempre.

CORRECCIÓN L4 (aprobada por Esse, 2026-11-14): el v3 decía "toda capacidad:
verificada en dispositivo como mínimo". Eso es un error de categoría para
capacidades que NO corren en el teléfono (consejo, motor de evolución,
puente GitHub, Vista de Dios). El estado objetivo es POR TIPO, campo
`target_state` por capacidad en docs/capabilities/inventory.yaml:

    audio/DSP (sonido)  -> "VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE"
    audio/DSP (resto)   -> "VERIFICADA EN DISPOSITIVO"
    herramientas/infra  -> "VERIFICADA EN HOST/CI"

La validación perceptual la hace SOLO Esse. Sin esta distinción la puerta
nunca llega a cero. Equivalencia única admitida: el `estado`
"VERIFICADA EN HOST" satisface el `target_state` "VERIFICADA EN HOST/CI"
(mismo lugar de verificación).

DIRECTIVA DE ESSE (2026-11-14 20:01 CST) — ELEVAR, JAMÁS DEGRADAR: cada
cambio debe dejar el producto mejor de lo que estaba; nunca peor, nunca a
medias, nunca un retroceso. Esta puerta eleva: añade verificación sin tocar
código de producto, sin bajar umbrales, sin eliminar pruebas. Si algo no
eleva, no se hace.

REGLAS DE CONTEO (versionadas; cambiarlas requiere a Esse):
  - FALLOS: un verificador con exit != 0 (1 por verificador) + cada test
    que falla (unittest, CTest) + cada violación RT confirmada + conteo
    CTest/SOFA distinto del umbral + inventario sin `target_state`.
  - A_MEDIAS: cada hallazgo individual de los verificadores (código sin
    llamador, envoltorio sin integrar, docs que afirman lo que el código
    no cumple) + cada capacidad con estado por debajo de su `target_state`
    (ni PENDIENTE ni BLOQUEADA) + cada TODO/FIXME/XXX estricto + cada
    prueba en skip/xfail/disabled + cada flag apagada que debería estar
    encendida (atómicos del efecto en 0.0, NEU·Ω OFF) + CTest no ejecutado.
    NOTA: un mismo defecto puede aparecer en FALLOS (la corrida que lo
    detecta) y en A_MEDIAS (el ítem concreto): miden cosas distintas
    (corridas vs ítems), no es doble conteo encubierto.
  - PENDIENTES: capacidades en estado PENDIENTE + trabajo en
    `pendientes_resumen` sin evidencia de cierre y no contado ya como
    BLOQUEADA (p. ej. la validación perceptual ABX, sin ejecutar).
  - BLOQUEOS_ABIERTOS: capacidades en estado BLOQUEADA sin un campo
    `decision_esse:` registrado en su bloque del inventario. Una decisión
    mencionada en prosa no cierra el bloqueo: debe quedar registrada con
    ese campo para que la puerta la vea.

UMBRALES (versionados en THRESHOLDS; cambiar un umbral requiere a Esse):
  - CTest esperado: 1008/1008. SOFA esperado: 694 archivos en el repo.

La puerta funciona SIN NDK/SDK: solo Python 3 estándar + los verificadores.
CTest es opcional en entornos sin cmake o con toolchain inadecuada
(cmake < 3.30.3, exigido por app/src/main/cpp/tests/CMakeLists.txt:1):
se registra "no ejecutado", que cuenta como A_MEDIAS, sin inventar un
resultado. Un CTest que configura pero no produce resumen sí es FALLO.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

GATE_VERSION = "1.0.0"
THRESHOLDS_VERSION = "perfection-gate-thresholds-v1"

# Umbrales versionados ANTES de medir. Cambiar un umbral requiere a Esse.
# NO se bajan para que la puerta dé cero: si el conteo real es menor, la
# puerta lo reporta como FALLO y lo decide Esse, no la puerta.
# 2026-11-21 (v2.5.490): la suite CTest creció de 1008 a 1416 (conservación
# 9 + gemelo 46 + MPC 11 + adversarial 134 + resto). El umbral CTest se
# SUBE a 1416/1416 para igualar el conteo real medido (1416/1416 en host
# y en CI); NO se baja. El umbral SOFA se queda en 694 (el conteo real
# del gate, que excluye directorios build, es 694; un conteo ingenuo de
# 1149 incluye 455 .sofa dentro de build/). Si el conteo real cae por
# debajo, la puerta sigue fallando. Cambiar un umbral exige actualizar
# también tools/evolution/tests/test_perfection_gate.py
# (test_umbrales_versionados), que es el candado que lo hace explícito.
# 2026-11-21 (Majestuosidad Total P6): la suite CTest crece de 1416 a
# 1417 con el test standalone test_universal_hearing (1 entrada ctest,
# 119 checks internos). El umbral CTest se SUBE a 1417/1417 para igualar
# el conteo real medido; el SOFA se queda en 694. Mismo procedimiento:
# actualizar también el candado test_umbrales_versionados.
# 2026-11-21 (v2.5.492): la suite CTest crece de 1417 a 1423 con el call
# site del gemelo (6 tests nuevos FusedChainTwinCallSite en
# test_fused_chain). El umbral CTest se SUBE a 1423/1423 para igualar el
# conteo real medido (1423/1423 en host); el SOFA se queda en 694. Mismo
# procedimiento: actualizar también el candado test_umbrales_versionados.
# 2026-11-21 (v2.5.493): la suite CTest crece de 1423 a 1430 con el control
# del gemelo (7 tests nuevos FusedChainTwinControl en test_fused_chain).
# El umbral CTest se SUBE a 1430/1430 para igualar el conteo real medido
# (1430/1430 en host); el SOFA se queda en 694. Mismo procedimiento:
# actualizar también el candado test_umbrales_versionados.
# 2026-11-22 (salto/control-cerrado): la suite crece de 1430 a 1607
# con el sistema de control cerrado (177 tests nuevos).
# 2026-11-22 (P1 banco adversarial permanente): la suite crece de 1607
# a 1619 con la matriz bypass vs IVANNA vs cada operador (11 casos +
# 1 diagnóstico APEX/limiter en test_p1_adversarial_matrix).
# El umbral CTest se SUBE a 1619/1619 para igualar el conteo real medido
# (1619 en host); el SOFA se queda en 694. Mismo procedimiento:
# actualizar también el candado test_umbrales_versionados.
# 2026-11-22 (fix no-determinismo NEURO): la suite crece de 1628 a 1631
# con test_neuro_determinism (3 TESTs: OpNeuroTwoFreshChainsBitIdentical,
# ClassifierGuardianTiming, IvannaTwoFreshChainsWithinTolerance).
# 2026-11-22 (v2.5.498, fix del tronido): la suite crece de 1631 a 1634
# con test_tronidos_layer_step (3 TESTs: AllLayers,
# FixEngageNoStepAndModeNoSilence, ApexOffProbe).
# 2026-11-23 (pan Poincaré de campo cercano, flag OFF por defecto): la
# suite crece de 1634 a 1639 con test_poincare_pan (4 TESTs: FlagIsOn,
# OperatorSelfTest, CueAccuracyWired, NetGainAndSilence) y
# test_fused_spatial.WfsStageGoldenWithPoincareOff (1 TEST: clava la
# salida WFS con el flag apagado).
THRESHOLDS = {
    # 2026-11-24 (CORONA, cableado P7–P10): la suite crece de 1639 a 1675
    # (+12 CORONA: 4 entradas standalone test_total_conservation,
    # test_experience_compiler, test_living_room, test_human_twin + 8 TESTs
    # FusedChainCoronaWiring) sobre una base ya en 1663: el umbral 1639
    # llevaba estancado desde la v2.5.498 (2026-11-23) y el Frente A midió
    # 1,663 en el CTest completo sin subir el candado (+24). 1663 + 12 = 1675
    # medido con `ctest -N` en el build clang++-18 completo.
    "ctest_expected_total": 1675,
    "ctest_expected_passed": 1675,
    "sofa_expected_total": 694,
}

TARGET_STATES_VALIDOS = {
    "VERIFICADA EN DISPOSITIVO",
    "VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE",
    "VERIFICADA EN HOST/CI",
}

# "VERIFICADA EN HOST" (vocabulario de `estado` en el inventario) y
# "VERIFICADA EN HOST/CI" (vocabulario de `target_state`, corrección L4)
# denotan el mismo lugar de verificación; se tratan como equivalentes al
# comparar estado con objetivo. No es una leniencia oculta: es la única
# equivalencia admitida y está documentada aquí y en el README de la puerta.
ESTADO_EQUIVALENTE = {"VERIFICADA EN HOST": "VERIFICADA EN HOST/CI"}


def estado_satisface_objetivo(estado, target_state) -> bool:
    if estado == target_state:
        return True
    return ESTADO_EQUIVALENTE.get(estado) == target_state


# Segmentos de código VENDOREADO (no es código del producto; sus TODOs,
# skips y docs no cuentan): googletest vendoreado, nlohmann/json, etc.
VENDORED_SEGMENTS = {"third_party", "vendor", "nlohmann", "googletest"}

# Un CTest que ni siquiera configura por toolchain inadecuada (cmake
# demasiado viejo, compilador ausente) NO es un fallo del producto: es
# "no ejecutado en este entorno" (A_MEDIAS, sin inventar resultado).
TOOLCHAIN_FAILURE_RES = [
    re.compile(r"cmake_minimum_required.*is required", re.S),
    re.compile(r"You are running version \d+\.\d+"),
    re.compile(r"No CMAKE_CXX_COMPILER could be found"),
    re.compile(r"cmake: command not found"),
    re.compile(r"Could not find a package configuration file.*CMake"),
]


def is_toolchain_failure(output: str) -> bool:
    return any(rx.search(output) for rx in TOOLCHAIN_FAILURE_RES)


def cmake_version():
    try:
        proc = subprocess.run(["cmake", "--version"], capture_output=True,
                              text=True, timeout=30)
        m = re.search(r"(\d+\.\d+\.\d+)", proc.stdout)
        return m.group(1) if m else None
    except Exception:
        return None


CHECKERS = [
    ("check_jni_wiring", ["python3", "scripts/check_jni_wiring.py"]),
    ("check_kotlin_wrapper_wiring", ["python3", "scripts/check_kotlin_wrapper_wiring.py"]),
    ("check_rt_safety", ["python3", "scripts/check_rt_safety.py"]),
    ("check_docs_claims", ["python3", "scripts/check_docs_claims.py"]),
]

# Marcadores TODO estrictos: SOLO la marca (TODO, FIXME o XXX), con ámbito
# opcional entre paréntesis, seguida de dos puntos al inicio de un comentario. Un grep ingenuo de "TODO" es inválido en
# este repo: "TODO" es palabra española ("por defecto TODO apagado") y la
# marca XXX casa con placeholders como "1:XXXXXXXXXXXX:android:..."
# (CloudSyncManager). Por eso la marca debe ir anclada a un comentario.
TODO_RE = re.compile(r"(?:^|[\s/*#])\s*(TODO|FIXME|XXX)(\([^)]*\))?\s*:\s*(.+)$")

SKIP_RES = [
    re.compile(r"@unittest\.skip"),
    re.compile(r"pytest\.mark\.(skip|xfail)"),
    re.compile(r"@Disabled\b"),
    re.compile(r"@Ignore\b"),
    re.compile(r"GTEST_SKIP\s*\("),
    re.compile(r"\bTEST(_F|_P)?\s*\(\s*\w+\s*,\s*DISABLED_"),
]

PRODUCT_SCAN_DIRS = ["app/src/main/cpp", "app/src/main/java", "tools/evolution", "scripts"]
PRODUCT_SCAN_EXTS = {".cpp", ".hpp", ".h", ".c", ".kt", ".py"}
TEST_SCAN_DIRS = ["app/src/main/cpp/tests", "tools/evolution/tests", "app/src/test",
                  "app/src/androidTest", "tests"]


def _rel(root: Path, path: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


# ---------------------------------------------------------------- inventario
def parse_inventory(path: Path):
    """Parser mínimo del inventario (sin dependencia PyYAML).

    Devuelve (capacidades, pendientes_resumen). Cada capacidad es un dict
    con id, estado, target_state, decision_esse, bloque (texto) y linea.
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    caps = []
    cur = None
    in_pendientes = False
    pendientes = []
    for i, line in enumerate(lines, 1):
        m = re.match(r"^  - id: (cap-\d+)\s*$", line)
        if m:
            cur = {"id": m.group(1), "estado": None, "target_state": None,
                   "decision_esse": None, "bloque": "", "linea": i}
            caps.append(cur)
            in_pendientes = False
            continue
        if line.startswith("pendientes_resumen:"):
            in_pendientes = True
            cur = None
            continue
        if line.startswith("bloqueados_resumen:"):
            in_pendientes = False
            cur = None
            continue
        if in_pendientes:
            m2 = re.match(r"^  - (.+)$", line)
            if m2:
                pendientes.append(m2.group(1).strip())
            continue
        if cur is not None:
            cur["bloque"] += line + "\n"
            m3 = re.match(r"^    estado: (.+?)\s*$", line)
            if m3:
                cur["estado"] = m3.group(1)
            m4 = re.match(r"^    target_state: (.+?)\s*$", line)
            if m4:
                cur["target_state"] = m4.group(1).strip().strip('"')
            m5 = re.match(r"^    decision_esse: (.+?)\s*$", line)
            if m5:
                cur["decision_esse"] = m5.group(1)
    return caps, pendientes


def inventory_items(root: Path, items: list) -> dict:
    """Evalúa el inventario y devuelve {pendientes, bloqueos, a_medias, fallos}."""
    inv_path = root / "docs/capabilities/inventory.yaml"
    counts = {"pendientes": 0, "fallos": 0, "a_medias": 0, "bloqueos": 0}
    if not inv_path.exists():
        counts["fallos"] += 1
        items.append({"counter": "FALLOS", "kind": "inventario_ausente",
                      "file": "docs/capabilities/inventory.yaml",
                      "detail": "No existe el inventario; la puerta no puede verificar nada."})
        return counts
    caps, pendientes_resumen = parse_inventory(inv_path)
    blocked_ids = set()
    pending_ids = set()
    # Primera pasada: clasificar por estado aunque falte target_state, para
    # que pendientes_resumen no duplique lo ya contado como BLOQUEADA.
    for cap in caps:
        if cap["estado"] == "PENDIENTE":
            pending_ids.add(cap["id"])
        elif cap["estado"] == "BLOQUEADA":
            blocked_ids.add(cap["id"])
    for cap in caps:
        base = {"capability": cap["id"], "file": "docs/capabilities/inventory.yaml",
                "line": cap["linea"]}
        if cap["target_state"] is None:
            counts["fallos"] += 1
            items.append({**base, "counter": "FALLOS", "kind": "target_state_ausente",
                          "detail": f"{cap['id']} no tiene target_state (corrección L4)."})
            # Sin target_state no se puede medir el gap, pero el estado sí
            # se clasifica abajo (PENDIENTE / BLOQUEADA).
        elif cap["target_state"] not in TARGET_STATES_VALIDOS:
            counts["fallos"] += 1
            items.append({**base, "counter": "FALLOS", "kind": "target_state_invalido",
                          "detail": f"{cap['id']} target_state={cap['target_state']!r} no válido."})
        estado = cap["estado"]
        if estado == "PENDIENTE":
            counts["pendientes"] += 1
            items.append({**base, "counter": "PENDIENTES", "kind": "capacidad_pendiente",
                          "detail": f"{cap['id']} en estado PENDIENTE (objetivo: {cap['target_state']})."})
        elif estado == "BLOQUEADA":
            if cap["decision_esse"]:
                items.append({**base, "counter": "—", "kind": "bloqueada_con_decision",
                              "detail": f"{cap['id']} BLOQUEADA con decisión de Esse registrada; "
                                        "no cuenta como bloqueo abierto."})
            else:
                counts["bloqueos"] += 1
                items.append({**base, "counter": "BLOQUEOS_ABIERTOS", "kind": "capacidad_bloqueada",
                              "detail": f"{cap['id']} BLOQUEADA sin decision_esse registrada."})
        elif estado_satisface_objetivo(estado, cap["target_state"]):
            pass  # En objetivo: nada que contar.
        elif cap["target_state"] is not None:
            counts["a_medias"] += 1
            items.append({**base, "counter": "A_MEDIAS", "kind": "capacidad_bajo_objetivo",
                          "detail": f"{cap['id']} estado={estado} por debajo de su objetivo "
                                    f"{cap['target_state']}."})
        # Flags apagadas que deberían estar encendidas (A_MEDIAS).
        for m in re.finditer(r"defecto_efecto_atomico: 0\.0", cap["bloque"]):
            counts["a_medias"] += 1
            items.append({**base, "counter": "A_MEDIAS", "kind": "flag_apagada_efecto",
                          "detail": f"{cap['id']}: atómico del efecto arranca en 0.0 (bypass) "
                                    "mientras la app declara un default > 0."})
        if "neu_omega_enabled: { defecto: false }" in cap["bloque"]:
            counts["a_medias"] += 1
            items.append({**base, "counter": "A_MEDIAS", "kind": "flag_apagada_neu",
                          "detail": f"{cap['id']}: NEU·Ω OFF por defecto; su criterio exige "
                                    "activación en dispositivo."})
    # pendientes_resumen: trabajo documentado sin evidencia de cierre.
    for bullet in pendientes_resumen:
        refs = re.findall(r"cap-\d+", bullet)
        if any(r in blocked_ids for r in refs):
            continue  # Ya contado como BLOQUEO_ABIERTO.
        if any(r in pending_ids for r in refs):
            continue  # Ya contado como PENDIENTE (la capacidad).
        counts["pendientes"] += 1
        items.append({"counter": "PENDIENTES", "kind": "pendiente_documentado",
                      "file": "docs/capabilities/inventory.yaml",
                      "detail": f"Pendiente documentado sin evidencia de cierre: {bullet}"})
    return counts


# ------------------------------------------------------------------ checkers
FAIL_ITEM_RE = re.compile(r"^\s+-\s+(.+)$")
FILE_LINE_RE = re.compile(r"^(\S+:\d+)\s*(?:->\s*)?(.*)$")


def classify_finding(header: str) -> str:
    h = header.lower()
    if "sin llamador" in h:
        return "sin_llamador"
    if "sin `external" in h or "sin external" in h:
        return "cpp_sin_external"
    if "afirmaciones" in h or "documental" in h:
        return "docs_sin_respaldo"
    if "rt" in h or "violacion" in h:
        return "rt_violation"
    if "fuera de cmake" in h:
        return "cpp_fuera_de_cmake"
    return "hallazgo_verificador"


def parse_checker_output(name: str, output: str):
    """Extrae los ítems [FAIL] con su sección, archivo:línea y símbolo."""
    findings = []
    header = ""
    in_fail = False
    for line in output.splitlines():
        if line.startswith("[FAIL]"):
            header = line[len("[FAIL]"):].strip()
            in_fail = True
            continue
        if line.startswith("[PASS]") or line.startswith("[SUMMARY]") or line.startswith("==="):
            if line.startswith("[SUMMARY]"):
                in_fail = False
            continue
        m = FAIL_ITEM_RE.match(line)
        if in_fail and m:
            raw = m.group(1).strip()
            fm = FILE_LINE_RE.match(raw)
            if fm:
                f, sym = fm.group(1), fm.group(2).strip()
            else:
                f, sym = "", raw
            findings.append({"checker": name, "header": header, "file": f,
                             "symbol": sym, "raw": raw,
                             "kind": classify_finding(header)})
    return findings


def run_checkers(root: Path, items: list, detail: dict):
    counts = {"fallos": 0, "a_medias": 0}
    results = []
    for name, argv in CHECKERS:
        try:
            proc = subprocess.run(argv, cwd=root, capture_output=True, text=True, timeout=600)
            out = proc.stdout + proc.stderr
            rc = proc.returncode
        except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
            rc, out = -1, f"no se pudo ejecutar {name}: {exc}"
        findings = parse_checker_output(name, out)
        results.append({"name": name, "exit": rc, "findings": len(findings)})
        detail[name] = {"exit": rc, "findings": findings,
                        "output_tail": out.strip().splitlines()[-5:]}
        if rc != 0:
            counts["fallos"] += 1
            items.append({"counter": "FALLOS", "kind": "verificador_fallido",
                          "file": f"scripts/{name}.py",
                          "detail": f"{name} exit={rc} con {len(findings)} hallazgo(s)."})
        for f in findings:
            if f["kind"] == "rt_violation":
                counts["fallos"] += 1
                items.append({"counter": "FALLOS", "kind": "rt_violation",
                              "file": f["file"], "symbol": f["symbol"],
                              "detail": f"[{name}] {f['header']}: {f['raw']}"})
            else:
                counts["a_medias"] += 1
                items.append({"counter": "A_MEDIAS", "kind": f["kind"],
                              "file": f["file"], "symbol": f["symbol"],
                              "detail": f"[{name}] {f['header']}: {f['raw']}"})
    return counts, results


# -------------------------------------------------------------------- suites
def run_unittest(root: Path, items: list):
    """Suite Python tools/evolution/tests (unittest). Cada test que falla = 1 FALLO."""
    result = {"executed": False, "ran": 0, "failures": []}
    try:
        proc = subprocess.run(
            [sys.executable, "-m", "unittest", "discover", "-s", "tools/evolution/tests"],
            cwd=root, capture_output=True, text=True, timeout=900)
        out = proc.stdout + proc.stderr
        result["executed"] = True
        m = re.search(r"Ran (\d+) tests?", out)
        result["ran"] = int(m.group(1)) if m else 0
        result["ok"] = "OK" in out.splitlines()[-3:] if out.strip() else False
        failed = re.findall(r"^(?:FAIL|ERROR): (\S+)", out, re.M)
        result["failures"] = failed
        result["exit"] = proc.returncode
        for t in failed:
            items.append({"counter": "FALLOS", "kind": "test_python_fallido",
                          "file": "tools/evolution/tests",
                          "symbol": t, "detail": f"Test Python en fallo: {t}."})
    except (subprocess.TimeoutExpired, FileNotFoundError) as exc:
        result["error"] = str(exc)
        items.append({"counter": "FALLOS", "kind": "test_python_no_ejecutado",
                      "file": "tools/evolution/tests",
                      "detail": f"No se pudo ejecutar la suite Python: {exc}."})
    return result


def run_ctest(root: Path, items: list, regenerate_telemetry: bool):
    """CTest host vía scripts/run_ctest.sh.

    - Si CTest corre y produce resumen: se compara con el umbral (1008/1008).
    - Si ni siquiera configura por toolchain inadecuada (cmake viejo,
      compilador ausente): 'no ejecutado en este entorno' (A_MEDIAS, sin
      inventar un resultado). Requisito: cmake >= 3.30.3
      (app/src/main/cpp/tests/CMakeLists.txt:1).
    - Si configura/compila pero no produce resumen: FALLO (el código no
      compila o el harness se rompió: eso sí es del producto)."""
    result = {"executed": False, "total": None, "passed": None, "failed": [],
              "cmake_version": cmake_version()}
    build_dir = root / "build" / "tests-host"
    log_path = build_dir / "ctest_last.log"
    if not (root / "scripts" / "run_ctest.sh").exists():
        result["reason"] = "scripts/run_ctest.sh ausente"
    else:
        try:
            env = dict(os.environ, CTEST_JOBS="4")
            proc = subprocess.run(["bash", "scripts/run_ctest.sh"], cwd=root,
                                  capture_output=True, text=True, timeout=1500, env=env)
            out = proc.stdout + proc.stderr
            result["exit"] = proc.returncode
            result["log"] = str(log_path) if log_path.exists() else None
            text = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else out
            # CTest 4.4.4 imprime "100% tests passed, 0 tests failed out of N";
            # CTest 4.4.5 omite la cláusula cuando no hay fallos:
            # "100% tests passed out of N" (misma corrección que
            # scripts/check_docs_claims.py:generate_from_log). Sin el grupo
            # opcional, un CTest 4.4.5 con todo verde caía en
            # "ctest_sin_resumen" aunque exit=0 (runs 43883575683,
            # 43887050955, 43891138351).
            m = re.search(r"(\d+)%\s+tests\s+passed(?:,\s+(\d+)\s+tests\s+failed)?\s+out\s+of\s+(\d+)", text)
            if m:
                failed = int(m.group(2)) if m.group(2) is not None else 0
                result["executed"] = True
                result["passed"] = int(m.group(3)) - failed
                result["total"] = int(m.group(3))
                result["failed"] = re.findall(
                    r"^\s*\d+ - (\S+) \((?:Failed|Timeout|SEGFAULT|Subprocess aborted|Exception)",
                    text, re.M)
            elif is_toolchain_failure(out):
                cause = next((ln.strip() for ln in out.splitlines()
                              if "required" in ln or "running version" in ln
                              or "could be found" in ln or "not found" in ln),
                             "toolchain inadecuada")
                result["reason"] = (f"toolchain inadecuada (cmake {result['cmake_version']}): "
                                    f"{cause[:160]}")
            else:
                result["executed"] = True
                result["reason"] = "sin línea resumen de ctest en el log"
            if regenerate_telemetry and log_path.exists():
                subprocess.run(
                    [sys.executable, "scripts/check_docs_claims.py",
                     "--generate-from-log", str(log_path)],
                    cwd=root, capture_output=True, text=True, timeout=300)
                result["telemetry_regenerated"] = True
        except subprocess.TimeoutExpired:
            result["reason"] = "timeout (1500 s) ejecutando CTest"
        except FileNotFoundError as exc:
            result["reason"] = f"ctest/cmake no disponible: {exc}"
    if not result["executed"]:
        items.append({"counter": "A_MEDIAS", "kind": "ctest_no_ejecutado",
                      "file": "scripts/run_ctest.sh",
                      "detail": f"CTest no ejecutado en este entorno: {result.get('reason')}. "
                                "Sin resultado inventado."})
        return result
    if result.get("total") is None:
        items.append({"counter": "FALLOS", "kind": "ctest_sin_resumen",
                      "file": "scripts/run_ctest.sh",
                      "detail": f"CTest corrió (exit={result.get('exit')}) pero no se pudo "
                                f"leer su resumen: {result.get('reason')}."})
        return result
    if result["total"] != THRESHOLDS["ctest_expected_total"] or \
            result["passed"] != THRESHOLDS["ctest_expected_passed"]:
        items.append({"counter": "FALLOS", "kind": "ctest_conteo_distinto_umbral",
                      "file": "scripts/run_ctest.sh",
                      "detail": f"CTest {result['passed']}/{result['total']} != umbral "
                                f"{THRESHOLDS['ctest_expected_passed']}/"
                                f"{THRESHOLDS['ctest_expected_total']} (umbral versionado; "
                                "no se baja para dar cero)."})
    for t in result["failed"]:
        items.append({"counter": "FALLOS", "kind": "test_ctest_fallido",
                      "file": "scripts/run_ctest.sh", "symbol": t,
                      "detail": f"Test CTest en fallo: {t}."})
    return result


# ------------------------------------------------------------------- escaneos
def _iter_files(root: Path, dirs, exts, skip_tests_dir=True):
    for d in dirs:
        base = root / d
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if not path.is_file() or path.suffix not in exts:
                continue
            rel = _rel(root, path)
            if VENDORED_SEGMENTS & set(path.parts):
                continue
            if skip_tests_dir and "/tests/" in rel:
                continue
            yield path


def scan_todo(root: Path, items: list) -> int:
    n = 0
    for path in _iter_files(root, PRODUCT_SCAN_DIRS, PRODUCT_SCAN_EXTS):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            m = TODO_RE.search(line)
            if m:
                n += 1
                items.append({"counter": "A_MEDIAS", "kind": "todo_fixme_stub",
                              "file": _rel(root, path), "line": i,
                              "symbol": m.group(1) + (m.group(2) or ""),
                              "detail": f"{m.group(1)}: {m.group(3).strip()[:160]}"})
    return n


def scan_skips(root: Path, items: list) -> int:
    n = 0
    for path in _iter_files(root, TEST_SCAN_DIRS, {".cpp", ".hpp", ".h", ".kt", ".py"},
                            skip_tests_dir=False):
        if "third_party" in path.parts:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for i, line in enumerate(text.splitlines(), 1):
            for rx in SKIP_RES:
                if rx.search(line):
                    n += 1
                    items.append({"counter": "A_MEDIAS", "kind": "prueba_en_skip",
                                  "file": _rel(root, path), "line": i,
                                  "detail": line.strip()[:160]})
                    break
    return n


def count_sofa(root: Path) -> int:
    n = 0
    for path in root.rglob("*.sofa"):
        if ".git" in path.parts or "build" in path.parts:
            continue
        n += 1
    return n


# ---------------------------------------------------------------------- main
def git_info(root: Path):
    def _run(args):
        try:
            proc = subprocess.run(["git"] + args, cwd=root, capture_output=True,
                                  text=True, timeout=30)
            return proc.stdout.strip() if proc.returncode == 0 else None
        except Exception:
            return None
    return {"commit": _run(["rev-parse", "HEAD"]),
            "branch": _run(["branch", "--show-current"])}


def run_gate(root: Path, *, skip_checkers=False, skip_ctest=False,
             skip_unittest=False, skip_sofa=False, regenerate_telemetry=False):
    items: list = []
    counters = {"PENDIENTES": 0, "FALLOS": 0, "A_MEDIAS": 0, "BLOQUEOS_ABIERTOS": 0}
    detail: dict = {}

    inv = inventory_items(root, items)
    counters["PENDIENTES"] += inv["pendientes"]
    counters["FALLOS"] += inv["fallos"]
    counters["A_MEDIAS"] += inv["a_medias"]
    counters["BLOQUEOS_ABIERTOS"] += inv["bloqueos"]

    todo_n = scan_todo(root, items)
    skip_n = scan_skips(root, items)
    counters["A_MEDIAS"] += todo_n + skip_n
    detail["todo_scan"] = {"hallazgos": todo_n}
    detail["skip_scan"] = {"hallazgos": skip_n}

    if skip_sofa:
        detail["sofa"] = {"executed": False, "reason": "omitido por --skip-sofa"}
    else:
        sofa_n = count_sofa(root)
        detail["sofa"] = {"executed": True, "total": sofa_n,
                          "expected": THRESHOLDS["sofa_expected_total"]}
        if sofa_n != THRESHOLDS["sofa_expected_total"]:
            counters["FALLOS"] += 1
            items.append({"counter": "FALLOS", "kind": "sofa_conteo_distinto_umbral",
                          "file": "docs/capabilities/inventory.yaml",
                          "detail": f"SOFA en repo: {sofa_n} != umbral "
                                    f"{THRESHOLDS['sofa_expected_total']}."})

    if skip_ctest:
        detail["ctest"] = {"executed": False, "reason": "omitido por --skip-ctest"}
    else:
        before = len(items)
        detail["ctest"] = run_ctest(root, items, regenerate_telemetry)
        counters["FALLOS"] += sum(1 for it in items[before:] if it["counter"] == "FALLOS")
        counters["A_MEDIAS"] += sum(1 for it in items[before:] if it["counter"] == "A_MEDIAS")

    if skip_checkers:
        detail["checkers"] = {"executed": False, "reason": "omitidos por --skip-checkers"}
    else:
        before = len(items)
        c_counts, c_results = run_checkers(root, items, detail)
        detail["checkers"] = {"executed": True, "results": c_results}
        counters["FALLOS"] += c_counts["fallos"]
        counters["A_MEDIAS"] += c_counts["a_medias"]

    if skip_unittest:
        detail["unittest"] = {"executed": False, "reason": "omitido por --skip-unittest"}
    else:
        detail["unittest"] = run_unittest(root, items)
        counters["FALLOS"] += len(detail["unittest"].get("failures", []))
        if not detail["unittest"].get("executed"):
            counters["FALLOS"] += 1  # Ya se añadió el ítem en run_unittest.

    verdict = "PERFECTA" if all(v == 0 for v in counters.values()) else "NO PERFECTA"
    report = {
        "gate": "perfection_gate",
        "gate_version": GATE_VERSION,
        "thresholds_version": THRESHOLDS_VERSION,
        "thresholds": THRESHOLDS,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "repo": str(root),
        **git_info(root),
        "counters": counters,
        "verdict": verdict,
        "exit_code": 0 if verdict == "PERFECTA" else 1,
        "checks": detail,
        "items": items,
        "not_verifiable_automatically": [
            "Parche sin rollback probado: la puerta no puede verificar rollback; "
            "cada parche debe documentar su rollback y probarlo (ver README).",
            "Controles UI sin efecto medible: la puerta usa como proxy los wrappers "
            "sin llamador de check_kotlin_wrapper_wiring.py; la medición perceptual "
            "de cada control la hace solo Esse.",
            "Validación perceptual (ABX): solo Esse puede darla; la puerta solo "
            "registra su ausencia como PENDIENTE.",
        ],
    }
    return report


def human_summary(report: dict) -> str:
    c = report["counters"]
    lines = [
        "=== PUERTA DE PERFECCIÓN v%s (umbrales %s) ===" % (
            report["gate_version"], report["thresholds_version"]),
        f"Commit: {report.get('commit')}  Rama: {report.get('branch')}",
        "",
        f"  PENDIENTES       : {c['PENDIENTES']}",
        f"  FALLOS           : {c['FALLOS']}",
        f"  A_MEDIAS         : {c['A_MEDIAS']}",
        f"  BLOQUEOS_ABIERTOS: {c['BLOQUEOS_ABIERTOS']}",
        "",
        f"VEREDICTO: {report['verdict']}",
    ]
    if report["verdict"] != "PERFECTA":
        lines.append("")
        lines.append("La flota NO puede decir \"terminado / perfecto / resuelto\" "
                     "sobre este commit.")
    for counter in ("PENDIENTES", "FALLOS", "A_MEDIAS", "BLOQUEOS_ABIERTOS"):
        group = [it for it in report["items"] if it["counter"] == counter]
        if not group:
            continue
        lines.append("")
        lines.append(f"--- {counter} ({len(group)}) ---")
        for it in group[:15]:
            loc = it.get("file", "")
            if it.get("line"):
                loc += f":{it['line']}"
            sym = f" {it['symbol']}" if it.get("symbol") else ""
            lines.append(f"  [{it['kind']}] {loc}{sym} — {it['detail'][:200]}")
        if len(group) > 15:
            lines.append(f"  … y {len(group) - 15} más (ver JSON).")
    return "\n".join(lines) + "\n"


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Puerta de perfección (v3, L2).")
    ap.add_argument("--root", default=".", help="Raíz del repo (default: .)")
    ap.add_argument("--output", default="perfection_gate_report.json",
                    help="Ruta del JSON (default: perfection_gate_report.json)")
    ap.add_argument("--skip-checkers", action="store_true")
    ap.add_argument("--skip-ctest", action="store_true")
    ap.add_argument("--skip-unittest", action="store_true")
    ap.add_argument("--skip-sofa", action="store_true")
    ap.add_argument("--regenerate-telemetry", action="store_true",
                    help="Regenera telemetry/ctest_summary.json desde el log CTest "
                         "(como tests-host.yml). Por defecto NO: no ensucia el árbol.")
    args = ap.parse_args(argv)
    root = Path(args.root).resolve()
    report = run_gate(root, skip_checkers=args.skip_checkers,
                      skip_ctest=args.skip_ctest, skip_unittest=args.skip_unittest,
                      skip_sofa=args.skip_sofa,
                      regenerate_telemetry=args.regenerate_telemetry)
    out_path = Path(args.output)
    if not out_path.is_absolute():
        out_path = root / out_path
    out_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n",
                        encoding="utf-8")
    print(human_summary(report))
    print(f"JSON: {out_path}")
    return report["exit_code"]


if __name__ == "__main__":
    raise SystemExit(main())
