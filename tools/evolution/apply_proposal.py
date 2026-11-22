#!/usr/bin/env python3
"""Aplica una propuesta APROBADA por el consejo al árbol de trabajo.

El eslabón que faltaba (2026-11-15): el motor de evolución
(`evolution_engine.py`) termina en una propuesta VALIDADA en scratch
("NUNCA aplica al árbol ni publica") y el puente GitHub
(`github_bridge.py publish_proposal`) solo se alcanza por el ciclo
`autonomous` — que nada invoca — o a mano, y además commitea + pushea
de inmediato. Entre validar y publicar no había ningún paso que dejara
el cambio aplicado en el árbol, verificado y LISTO para commit. Este
script es ese paso:

    propuesta VERDE (consejo APROBADO + validación fresca en scratch)
        → contención (límites, prohibidos, secretos)
        → git apply al árbol real
        → verificación post-aplicación (tests afectados)
        → queda en el working tree, SIN commit, SIN push, SIN bump.

El operador (Muse + Esse) revisa el diff y hace el commit. La
publicación a main sigue exigiendo el gancho de contención
(.ivanna-publish-approval) y IVANNA_ALLOW_PUBLISH=1: este script NO
los toca ni los evita.

REGLA DE ORO DE ESSE (2026-11-15 19:51 CST): el listón no es
"perfecto estándar" ni "cumple el estándar", es PERFECTO MEJOR QUE
LOS ÉLITE, MEJOR QUE LA TECNOLOGÍA EXISTENTE YA CREADA. Este script
verifica CORRECTITUD (el parche no rompe nada y hace lo que dice);
la ELEVACIÓN por encima de la élite la valora el operador con esa
regla, no la inventa la máquina: el reporte registra solo hechos
medidos (tests nuevos, suite, defecto medido que corrige) y deja la
valoración de elevación al humano. Un parche que solo deja el
producto "bien" o "a nivel" pasa las puertas de correctitud, pero el
reporte NO lo presenta como elevación.

Uso:
  python3 tools/evolution/apply_proposal.py --proposal 0001
      Re-valida en scratch contra el árbol ACTUAL, aplica al árbol
      real, verifica los tests afectados y deja el cambio listo.
  python3 tools/evolution/apply_proposal.py --proposal 0001 --dry-run
      Solo puertas + re-validación; no toca el árbol.
  python3 tools/evolution/apply_proposal.py --proposal 0001 \\
      --allow-no-council-record
      Permite aplicar una propuesta anterior al registro de consejo
      (0001-0005 no tienen COUNCIL.md: el consejo solo revisó la
      0006). Queda registrado en el reporte y en la auditoría: la
      responsabilidad la asume el operador, no el consejo.

Puertas (fail-closed, en orden):
  0. La propuesta existe (parche + validation.json).
  1. Aprobación del consejo (COUNCIL.md/DRAFT.md con APROBADO), salvo
     --allow-no-council-record explícito del operador.
  2. Contención (límites, prohibidos, secretos). Va antes del
     ya-aplicado: un parche prohibido se rechaza aunque su objetivo
     ya no exista.
  3. Ya aplicado: si el parche ya está en el árbol (reverse-check
     limpio, o contenido presente aunque el contexto derivó, o hunk
     moot porque su objetivo fue eliminado) no se toca nada: se
     reporta "already_applied". Esta puerta es solo lectura y no
     exige validation.json (no se va a cambiar nada).
  4. Re-validación FRESCA en scratch (validate_proposal): el veredicto
     viejo es evidencia de otra base; solo un VERDE contra el árbol
     actual habilita la aplicación. Aquí sí se exige validation.json
     previo (vamos a cambiar el árbol).
  5. El parche aplica limpio sobre el árbol actual (git apply --check).
  6. Verificación post-aplicación: tests afectados (Python: suite del
     motor si el parche toca tools/evolution o tools/autorepair; C++:
     solo los targets nuevos del parche). Si falla, se REVIERTE
     (checkout de los tocados + unlink de los nuevos) y se reporta
     el fallo.

Nunca borra código: el revert restaura el estado previo exacto.
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EVO = REPO / "tools" / "evolution"
PROPOSALS = EVO / "proposals"

SECRET_PATTERNS = [
    re.compile(r"ghp_[A-Za-z0-9]{20,}"),
    re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
    re.compile(r"(?i)password\s*=\s*['\"][^'\"]{4,}"),
    re.compile(r"(?i)token\s*=\s*['\"][^'\"]{8,}"),
]


def _load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _engine():
    return _load_module("ivanna_evolution_engine", EVO / "evolution_engine.py")


def _audit():
    return _load_module("ivanna_audit", EVO / "audit.py")


def _run(cmd, cwd=REPO, timeout=None):
    return subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                          timeout=timeout)


# ── Puerta 1: aprobación del consejo ──────────────────────────────────────
def council_approval(pdir: Path):
    """Busca el registro de aprobación del consejo en la propuesta.

    Devuelve (aprobado: bool, evidencia: str). COUNCIL.md manda; si no
    existe, DRAFT.md. Las propuestas 0001-0005 no tienen ese registro
    (el consejo solo revisó la 0006): en ese caso devuelve False y el
    operador decide con --allow-no-council-record.
    """
    for fname in ("COUNCIL.md", "DRAFT.md"):
        f = pdir / fname
        if not f.exists():
            continue
        text = f.read_text(errors="ignore")
        m = re.search(r"Veredicto:\s*\**\s*(APROBADO|RECHAZADO)",
                      text, re.IGNORECASE)
        if m:
            return (m.group(1).upper() == "APROBADO",
                    f"{fname}: veredicto {m.group(1).upper()}")
        if "APROBADO" in text.upper():
            return True, f"{fname}: contiene APROBADO"
    return False, "sin registro de aprobación del consejo en la propuesta"


def is_verdict_green(result: dict) -> bool:
    """Misma lógica que github_bridge.is_verdict_green y cmd_evolve."""
    passed = bool(result.get("applied"))
    fs = result.get("full_suite")
    if isinstance(fs, dict):
        passed = passed and (fs.get("passed") is True or fs.get("skipped") is True)
    pc = result.get("python_checks")
    if isinstance(pc, dict):
        passed = passed and all(
            v.get("passed") is True or v.get("skipped") is True
            for v in pc.values())
    return passed


# ── Puerta 2: ¿ya está aplicado? ──────────────────────────────────────────
def _parse_patch_sections(patch_text: str):
    """Divide el parche por archivo: {rel: (added, removed, is_new)}.

    added/removed son las líneas `+`/`-` sin el prefijo (se ignoran
    las cabeceras `+++`/`---`). is_new: el archivo se crea
    (`--- /dev/null`).
    """
    sections = {}
    current = None
    for line in patch_text.splitlines():
        if line.startswith("--- /dev/null"):
            current = None
            continue
        if line.startswith("--- a/"):
            current = None
            continue
        if line.startswith("+++ b/"):
            rel = line[6:].strip()
            sections.setdefault(rel, {"added": [], "removed": [],
                                      "is_new": False})
            current = rel
            continue
        if line.startswith("+++ /dev/null"):
            current = None
            continue
        if current is None:
            continue
        if line.startswith("+") and not line.startswith("+++"):
            sections[current]["added"].append(line[1:])
        elif line.startswith("-") and not line.startswith("---"):
            sections[current]["removed"].append(line[1:])
    # Marcar creaciones: el `--- /dev/null` precede al `+++ b/`.
    for m in re.finditer(r"--- /dev/null\n\+\+\+ b/([^\n]+)", patch_text):
        rel = m.group(1).strip()
        if rel in sections:
            sections[rel]["is_new"] = True
    return sections


def _file_lines(path: Path):
    try:
        return set(l.strip() for l in path.read_text(errors="ignore").splitlines())
    except OSError:
        return None


def patch_already_applied(patch_text: str, repo: Path):
    """¿El parche ya está en el árbol? Devuelve (estado, detalle).

    Estados: "exact" (reverse-check limpio), "content" (todo el
    contenido añadido ya está presente aunque el contexto derivó),
    "moot" (algún hunk apunta a código que ya no existe: su objetivo
    fue eliminado por otra refactorización; el resto está aplicado),
    "partial" (parte aplicada, parte no: NO se toca, lo revisa un
    humano), "absent" (no aplicado).
    """
    r = subprocess.run(["git", "apply", "--check", "--reverse"],
                       input=patch_text, cwd=repo, capture_output=True,
                       text=True)
    if r.returncode == 0:
        return "exact", "git apply --check --reverse limpio"
    sections = _parse_patch_sections(patch_text)
    if not sections:
        return "absent", "el parche no toca archivos reconocibles"
    per_file = {}
    for rel, sec in sections.items():
        target = repo / rel
        if not target.exists():
            if sec["is_new"]:
                per_file[rel] = "missing"  # archivo nuevo que no existe
            else:
                per_file[rel] = "moot"  # modificación de algo inexistente
            continue
        present = _file_lines(target)
        added = [a.strip() for a in sec["added"] if a.strip()]
        removed = [x.strip() for x in sec["removed"] if x.strip()]
        added_ok = all(a in present for a in added)
        removed_gone = all(x not in present for x in removed)
        if added_ok:
            per_file[rel] = "applied"
        elif removed_gone and not added_ok:
            # Ni lo viejo ni lo nuevo: el código objetivo ya no está.
            per_file[rel] = "moot"
        elif any(a in present for a in added):
            per_file[rel] = "partial"
        else:
            per_file[rel] = "missing"
    states = set(per_file.values())
    detail = "; ".join(f"{f}: {s}" for f, s in per_file.items())
    if states == {"applied"}:
        return "content", detail
    if states <= {"applied", "moot"} and "moot" in states:
        return "moot", detail
    if "partial" in states or ("applied" in states and "missing" in states):
        return "partial", detail
    return "absent", detail


# ── Revert ────────────────────────────────────────────────────────────────
def _revert_files(repo: Path, files):
    """Restaura el estado previo: checkout de los trackeados, unlink de
    los nuevos (sin seguimiento). Nunca borra nada que no haya tocado
    el parche."""
    tracked = _run(["git", "ls-files", "--"] + files, cwd=repo).stdout.split()
    if tracked:
        _run(["git", "checkout", "--"] + tracked, cwd=repo)
    for f in files:
        if f not in tracked:
            p = repo / f
            if p.exists() and p.is_file():
                p.unlink()


# ── Verificación post-aplicación ──────────────────────────────────────────
def _post_apply_verify(repo: Path, files, patch_text: str):
    """Verifica el árbol real tras aplicar. Devuelve (ok, detalle).

    Python tocado → suite del motor (rápida). C++ tocado → solo los
    targets NUEVOS del parche (add_executable) en build-tests real;
    si el parche no añade targets, no hay nada nuevo que compilar (la
    suite C++ completa ya corrió en scratch en la re-validación).
    """
    py_touched = [f for f in files
                  if f.endswith(".py")
                  and (f.startswith("tools/evolution/")
                       or f.startswith("tools/autorepair/"))]
    cpp_touched = [f for f in files
                   if f.endswith((".cpp", ".c", ".h", ".hpp"))
                   or f == "app/src/main/cpp/tests/CMakeLists.txt"]
    tests_dir = repo / "tools" / "evolution" / "tests"
    if py_touched and tests_dir.exists():
        r = _run(["python3", "-m", "unittest", "discover",
                  "-s", "tools/evolution/tests"], cwd=repo, timeout=300)
        tail = (r.stdout + r.stderr)[-500:]
        if r.returncode != 0:
            return False, f"suite Python del motor falló tras aplicar:\n{tail}"
    if cpp_touched:
        cpp_tests_dir = repo / "app/src/main/cpp/tests"
        if not (cpp_tests_dir / "CMakeLists.txt").exists():
            return True, ("C++ tocado pero sin tests/CMakeLists.txt en "
                          "este árbol; la suite ya pasó en scratch")
        new_targets = sorted(set(re.findall(r"add_executable\((\w+)",
                                            patch_text)))
        if not new_targets:
            return True, ("sin targets C++ nuevos en el parche; la suite "
                          "completa ya pasó en scratch")
        cmake = os.environ.get("IVANNA_CMAKE",
                               str(Path.home() / ".local/bin/cmake"))
        tests_dir = repo / "app/src/main/cpp/tests"
        build_dir = tests_dir / "build-tests"
        if not (build_dir / "CMakeCache.txt").exists():
            r = _run([cmake, "-S", "app/src/main/cpp/tests",
                      "-B", "build-tests"], cwd=repo, timeout=600)
            if r.returncode != 0:
                return False, ("no se pudo configurar build-tests real "
                               f"tras aplicar:\n{(r.stdout + r.stderr)[-500:]}")
        for t in new_targets:
            b = _run([cmake, "--build", "build-tests", "--target", t, "-j4"],
                     cwd=tests_dir, timeout=900)
            if b.returncode != 0:
                return False, (f"compilación del target {t} falló tras "
                               f"aplicar:\n{(b.stdout + b.stderr)[-500:]}")
            tr = _run([f"./{t}"], cwd=build_dir, timeout=300)
            if tr.returncode != 0:
                return False, (f"test {t} falló tras aplicar:\n"
                               f"{tr.stdout[-500:]}")
        return True, (f"targets nuevos OK tras aplicar: "
                      f"{', '.join(new_targets)}")
    return True, "verificación post-aplicación OK"


# ── Aplicación ────────────────────────────────────────────────────────────
def apply_proposal(pid: str, repo: Path = None, proposals_dir: Path = None,
                   verify: bool = True, allow_no_council_record: bool = False,
                   dry_run: bool = False) -> dict:
    """Aplica la propuesta pid al árbol real. Ver docstring del módulo.

    Devuelve un dict de reporte y escribe apply_report.json en la
    propuesta. NUNCA commitea, pushea ni hace bump.
    """
    repo = Path(repo).resolve() if repo else REPO
    proposals_dir = Path(proposals_dir) if proposals_dir else repo / "tools" / "evolution" / "proposals"
    pdir = proposals_dir / pid
    report = {"proposal": pid, "status": "refused", "applied": False,
              "dry_run": dry_run}
    engine = _engine()
    audit = _audit()

    def _fail(reason, **extra):
        report.update({"status": "refused", "reason": reason, **extra})
        _write_report(pdir, report)
        audit.record("apply_decision",
                     f"{pid}: REFUSADO — {reason}")
        return report

    patch_file = next(pdir.glob("*.patch"), None) if pdir.exists() else None
    if patch_file is None:
        return _fail("sin parche en la propuesta")
    patch_text = patch_file.read_text()

    # Puerta 1: aprobación del consejo.
    approved, council_evidence = council_approval(pdir)
    report["council"] = {"approved": approved, "evidence": council_evidence}
    if not approved and not allow_no_council_record:
        return _fail("sin registro de aprobación del consejo "
                     "(usa --allow-no-council-record si el operador "
                     "asume la responsabilidad)")
    if not approved and allow_no_council_record:
        report["council_override"] = True

    # Puerta 2: contención (límites, prohibidos, secretos). Va ANTES
    # del ya-aplicado: un parche prohibido se rechaza aunque su objetivo
    # ya no exista (moot) o ya esté aplicado.
    try:
        files = engine.check_patch_limits(patch_text)
    except SystemExit as e:
        return _fail(f"GATE límites: {e}")
    bad = [f for f in files if engine.is_forbidden(f)]
    if bad:
        return _fail(f"GATE: el parche toca archivos prohibidos: {bad}")
    secrets = [p.pattern for p in SECRET_PATTERNS if p.search(patch_text)]
    if secrets:
        return _fail(f"GATE: el parche contiene secretos potenciales: {secrets}")
    report["files"] = files

    # Puerta 3: ¿ya está aplicado?
    state, detail = patch_already_applied(patch_text, repo)
    report["already_applied"] = {"state": state, "detail": detail}
    if state in ("exact", "content", "moot"):
        report.update({"status": "already_applied",
                       "reason": f"el cambio ya está en el árbol ({state}): {detail}"})
        _write_report(pdir, report)
        audit.record("apply_decision",
                     f"{pid}: YA APLICADO ({state}) — no se tocó el árbol")
        return report
    if state == "partial":
        return _fail("parche PARCIALMENTE aplicado: lo revisa un humano, "
                     f"no se fuerza ({detail})")

    # Puerta 4: re-validación FRESCA en scratch contra el árbol actual.
    # El veredicto viejo es evidencia de otra base; solo un VERDE
    # fresco habilita la aplicación. Sin validation.json previo no hay
    # nada que re-validar: se rechaza (aquí sí, porque vamos a CAMBIAR
    # el árbol; el ya-aplicado de arriba no necesitaba este archivo).
    validation_path = pdir / "validation.json"
    if not validation_path.exists():
        return _fail("sin validation.json: no se aplica nada sin verificación")
    try:
        fresh = engine.validate_proposal(pid)
    except SystemExit as e:
        return _fail(f"re-validación rechazada por puerta: {e}")
    except Exception as e:  # noqa: BLE001 - cualquier fallo es rechazo
        return _fail(f"re-validación falló con excepción: {e}")
    report["fresh_validation"] = {
        "applied": fresh.get("applied"),
        "tree_untouched": fresh.get("tree_untouched"),
        "full_suite": fresh.get("full_suite"),
        "python_checks": bool(fresh.get("python_checks")),
    }
    if not is_verdict_green(fresh):
        return _fail("re-validación FRESCA no verde: no se aplica",
                     fresh_validation_detail=fresh)

    if dry_run:
        report.update({"status": "dry_run_green",
                       "reason": "puertas + re-validación en verde; "
                                 "no se tocó el árbol (dry-run)"})
        _write_report(pdir, report)
        audit.record("apply_decision",
                     f"{pid}: DRY-RUN VERDE — listo para aplicar")
        return report

    # Puerta 5: aplica limpio sobre el árbol actual.
    r = _run(["git", "apply", "--check", str(patch_file)], cwd=repo)
    if r.returncode != 0:
        return _fail("el parche ya no aplica limpio sobre el árbol actual "
                     f"(el árbol se movió):\n{r.stderr[:800]}")
    r = _run(["git", "apply", str(patch_file)], cwd=repo)
    if r.returncode != 0:
        return _fail(f"git apply falló:\n{r.stderr[:800]}")

    # Puerta 6: verificación post-aplicación; si falla, revertir.
    if verify:
        ok, detail = _post_apply_verify(repo, files, patch_text)
        if not ok:
            _revert_files(repo, files)
            return _fail("verificación post-aplicación falló: cambio "
                         f"REVERTIDO\n{detail}")
        report["post_apply_verify"] = detail
    else:
        report["post_apply_verify"] = "omitida (--no-verify)"

    # Éxito: queda en el working tree, SIN commit. El operador revisa
    # y commitea (un commit por cambio, `tipo(area): descripción`).
    status = _run(["git", "status", "--short"], cwd=repo).stdout.strip()
    report.update({
        "status": "applied",
        "applied": True,
        "reason": "aplicado al árbol real y verificado; listo para commit",
        "tree_status": status,
        "commit_commands": [
            f"git add -- {' '.join(files)}",
            "git commit -m \"tipo(area): descripción\"  # un commit por cambio",
        ],
        # Regla de oro: la máquina verifica correctitud; la elevación
        # por encima de la élite la valora el operador. Solo hechos.
        "criterio_oro": ("PERFECTO MEJOR QUE LOS ÉLITE (Esse 2026-11-15): "
                         "la elevación la valora el operador; este reporte "
                         "solo registra hechos medidos."),
        "evidencia_medida": {
            "validacion_fresca_verde": True,
            "tests_nuevos_en_parche": sorted(set(re.findall(
                r"add_executable\((\w+)", patch_text))),
            "archivos": files,
        },
    })
    _write_report(pdir, report)
    audit.record("apply_decision",
                 f"{pid}: APLICADO al árbol (sin commit) — {len(files)} archivos")
    return report


def _write_report(pdir: Path, report: dict):
    try:
        pdir.mkdir(parents=True, exist_ok=True)
        (pdir / "apply_report.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False))
    except OSError as e:
        print(f"apply: no se pudo escribir apply_report.json: {e}",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(
        description="Aplica una propuesta APROBADA por el consejo al árbol "
                    "real (validada en scratch, verificada, SIN commit).")
    ap.add_argument("--proposal", required=True, help="id de la propuesta")
    ap.add_argument("--allow-no-council-record", action="store_true",
                    help="el operador asume la responsabilidad: la propuesta "
                         "no tiene registro de aprobación del consejo")
    ap.add_argument("--no-verify", action="store_true",
                    help="omite la verificación post-aplicación (no recomendado)")
    ap.add_argument("--dry-run", action="store_true",
                    help="solo puertas + re-validación; no toca el árbol")
    ap.add_argument("--repo", help="repo (por defecto, el de este script)")
    args = ap.parse_args()
    repo = Path(args.repo).resolve() if args.repo else None
    rep = apply_proposal(args.proposal, repo=repo,
                         verify=not args.no_verify,
                         allow_no_council_record=args.allow_no_council_record,
                         dry_run=args.dry_run)
    print(json.dumps(rep, indent=2, ensure_ascii=False))
    if rep["status"] == "applied":
        print("\nListo para commit (lo hace el operador):")
        for cmd in rep["commit_commands"]:
            print(f"  {cmd}")
    return 0 if rep["status"] in ("applied", "already_applied",
                                  "dry_run_green") else 1


if __name__ == "__main__":
    raise SystemExit(main())
