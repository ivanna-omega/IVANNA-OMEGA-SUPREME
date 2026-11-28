#!/usr/bin/env python3
"""Chequeo del bloque `repro:` obligatorio (C12, release 510, Fase 4).

Formato REAL encontrado (2026-11-28, verificado contra el código vivo):
las propuestas de `tools/evolution/proposals/<id>/` NO tienen ningún campo
`repro:` — son directorios con `<id>.patch` + `REPORT.md` +
`validation.json` (+ `DRAFT.md`/`COUNCIL.md` opcionales), y
`tools/evolution/council.py` NUNCA lee esos directorios (recibe un
finding JSON por CLI). No existe YAML ni front-matter en ninguna
propuesta. Por eso el `repro:` de C12 NO se escribe como bloque de texto
en REPORT.md (sería prosa sin parsear): se escribe como sidecar JSON
`repro.json` junto a `validation.json`, con exactamente las claves que
pide la propuesta (`command`, `seed`, `fixtures` con sha256, `expected`),
más `commands` (lista) para repros de varios pasos como la v1.1 (C7+S4).

Formato de `repro.json` (versión 1):

    {
      "proposal": "0034-v1.1-ab-candidate",   // debe coincidir con el dir
      "version": 1,
      "seed": 1795857152818,                  // opcional; entero si existe
      "commands": [                           // o "command" único (ver abajo)
        {
          "name": "S4",
          "command": "python3 scripts/validate_ivanna_profile.py ...",
          "seed": 1795857152818,              // opcional; entero si existe
          "fixtures": [                       // rutas relativas al repo
            {"path": "tools/evolution/fixtures/x.json",
             "sha256": "<64 hex del fixture REAL>"}
          ],
          "expected": "VÁLIDO (schema v1.0 live-session",
                                        // o lista de literales: TODOS deben
                                        // aparecer (C7: PROMOTED + las 5
                                        // puertas con sus números)
          "expected_exit_code": 0             // opcional, por defecto 0
        }
      ],
      "pending_commands": [                   // pasos declarados, NO ejecutados
        {
          "name": "C7",
          "command": "python3 scripts/check_muselink_ab.py ...",
          "seed": 1795857152818,
          "fixtures": [{"path": "fixtures/ab/v1.0_sessions.json"}],
          "expected": "PROMOTED",
          "blocked_by": "C7 en implementación paralela: ..."
        }
      ]
    }

Forma de un solo comando (la literal de la propuesta): claves `command`,
`seed`, `fixtures`, `expected` a nivel superior, sin `commands`. Si
coexisten ambas formas, es error (ambiguo).

Reglas del chequeo (fail-closed):
  - Falta `repro.json` en una propuesta tocada por el PR -> FALLO.
  - Fixture inexistente o sha256 distinto -> FALLO (nunca se fabrica un
    fixture ni un sha: el sha debe ser el del archivo real).
  - El comando sale con código distinto de `expected_exit_code`, o su
    salida (stdout+stderr) NO contiene alguno de los literales de
    `expected` -> FALLO.
  - `pending_commands` NO se ejecuta ni exige fixtures existentes: cada
    paso pendiente exige `blocked_by` no vacío (el motivo por el que aún
    no es ejecutable). Tripwire de promoción: si el programa del comando
    pendiente YA EXISTE en el repo, el chequeo FALLA — el bloqueo caducó
    y el paso debe promoverse a `commands`.
  - Alcance (abuelos): las propuestas anteriores a C12 (0001-0033) NO
    tienen `repro.json`; exigirlo retroactivamente pondría la CI en rojo
    el primer día y rellenarlo a posteriori sería fabricar evidencia.
    Por eso el modo de CI es `--changed`: solo se exige `repro.json` a
    las propuestas tocadas por el PR. `--all` existe para auditoría.

Uso:
  python3 tools/evolution/check_repro.py --proposal 0034-v1.1-ab-candidate
  python3 tools/evolution/check_repro.py --changed --base <sha-del-base>
  python3 tools/evolution/check_repro.py --all [--no-run]
Solo stdlib. Los comandos se ejecutan sin shell (shlex.split) con cwd en
la raíz del repo y un timeout (por defecto 300 s).
"""

import argparse
import hashlib
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path

EVO = Path(__file__).resolve().parent
REPO = EVO.parents[1]
PROPOSALS = EVO / "proposals"

DEFAULT_TIMEOUT_S = 300
# Caracteres que implican shell: sin shell no hacen nada, pero se
# rechazan para que un repro nunca dependa de sintaxis de shell.
_SHELL_META = set(";|&$`><\n")


class ReproError(Exception):
    """Error de formato o de verificación del repro (falla el PR)."""


def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _validate_fixtures(fixtures, where: str, *, require_sha: bool,
                       repo: Path) -> list:
    """Valida una lista de fixtures. Devuelve lista de errores (vacía OK).

    Con require_sha=True (pasos ejecutables): cada fixture exige `path`
    repo-relativo existente y `sha256` de 64 hex que coincida con el
    archivo real. Con require_sha=False (pasos pendientes): el `path`
    basta; si trae sha256 Y el archivo existe, se verifica igualmente.
    """
    errors = []
    if not isinstance(fixtures, list):
        return [f"{where}: 'fixtures' debe ser una lista"]
    for i, fx in enumerate(fixtures):
        fx_where = f"{where}: fixtures[{i}]"
        if not isinstance(fx, dict):
            errors.append(f"{fx_where}: debe ser un objeto")
            continue
        path = fx.get("path")
        if not isinstance(path, str) or not path:
            errors.append(f"{fx_where}: 'path' obligatorio (str no vacío)")
            continue
        if path.startswith("/") or ".." in Path(path).parts:
            errors.append(f"{fx_where}: 'path' debe ser relativo al repo "
                          f"y sin '..': {path!r}")
            continue
        sha = fx.get("sha256")
        fpath = repo / path
        if require_sha:
            if not isinstance(sha, str) or not re.fullmatch(r"[0-9a-f]{64}", sha or ""):
                errors.append(f"{fx_where}: 'sha256' obligatorio (64 hex)")
            if not fpath.is_file():
                errors.append(f"{fx_where}: fixture inexistente: {path}")
            elif isinstance(sha, str) and re.fullmatch(r"[0-9a-f]{64}", sha):
                actual = hashlib.sha256(fpath.read_bytes()).hexdigest()
                if actual != sha:
                    errors.append(f"{fx_where}: sha256 no coincide: {path} "
                                  f"(esperado {sha}, real {actual})")
        else:
            if sha is not None and (not isinstance(sha, str)
                                    or not re.fullmatch(r"[0-9a-f]{64}", sha)):
                errors.append(f"{fx_where}: 'sha256' debe ser 64 hex")
            elif sha is not None and fpath.is_file():
                actual = hashlib.sha256(fpath.read_bytes()).hexdigest()
                if actual != sha:
                    errors.append(f"{fx_where}: sha256 no coincide: {path}")
    return errors


def _validate_command(cmd, where: str, *, require_sha: bool,
                      repo: Path) -> list:
    """Valida el esquema de un paso (ejecutable o pendiente)."""
    errors = []
    if not isinstance(cmd, dict):
        return [f"{where}: debe ser un objeto"]
    if not isinstance(cmd.get("name"), str) or not cmd.get("name"):
        errors.append(f"{where}: 'name' obligatorio (str no vacío)")
    command = cmd.get("command")
    if not isinstance(command, str) or not command.strip():
        errors.append(f"{where}: 'command' obligatorio (str no vacío)")
    else:
        bad = sorted(set(command) & _SHELL_META)
        if bad:
            errors.append(f"{where}: 'command' usa sintaxis de shell "
                          f"(sin shell en el chequeo): {bad}")
    if "seed" in cmd and not _is_int(cmd["seed"]):
        errors.append(f"{where}: 'seed' debe ser entero")
    if "expected" not in cmd:
        errors.append(f"{where}: 'expected' obligatorio (str no vacío, "
                      "o lista no vacía de str)")
    else:
        exp = cmd["expected"]
        if isinstance(exp, str):
            if not exp:
                errors.append(f"{where}: 'expected' no puede ser vacío")
        elif isinstance(exp, list):
            if not exp or any(not isinstance(e, str) or not e for e in exp):
                errors.append(f"{where}: 'expected' como lista debe ser "
                              "no vacía y con solo str no vacíos")
        else:
            errors.append(f"{where}: 'expected' debe ser str o lista de str")
    if "expected_exit_code" in cmd \
            and not _is_int(cmd["expected_exit_code"]):
        errors.append(f"{where}: 'expected_exit_code' debe ser entero")
    errors.extend(_validate_fixtures(cmd.get("fixtures", []),
                                     f"{where}", require_sha=require_sha,
                                     repo=repo))
    return errors


def normalize_repro(data: dict) -> tuple:
    """Normaliza a (commands, pending_commands). Acepta la forma de un
    solo comando (claves a nivel superior) y la forma `commands`."""
    if not isinstance(data, dict):
        raise ReproError("repro.json debe ser un objeto JSON")
    if data.get("version") != 1:
        raise ReproError("'version' debe ser 1")
    if "seed" in data and not _is_int(data["seed"]):
        raise ReproError("'seed' debe ser entero")
    has_single = "command" in data
    has_list = "commands" in data
    if has_single and has_list:
        raise ReproError("ambiguo: 'command' y 'commands' a la vez")
    if has_single:
        single = {k: data[k] for k in
                  ("name", "command", "seed", "fixtures", "expected",
                   "expected_exit_code") if k in data}
        single.setdefault("name", "repro")
        commands = [single]
    elif has_list:
        commands = data["commands"]
        if not isinstance(commands, list) or not commands:
            raise ReproError("'commands' debe ser una lista no vacía")
    else:
        raise ReproError("falta 'command' o 'commands'")
    pending = data.get("pending_commands", [])
    if not isinstance(pending, list):
        raise ReproError("'pending_commands' debe ser una lista")
    return commands, pending


def load_repro(pdir: Path) -> dict:
    """Carga y parsea repro.json; error si falta o no es JSON válido."""
    rpath = pdir / "repro.json"
    if not rpath.is_file():
        raise ReproError(f"falta repro.json en {pdir.name} "
                         "(bloque repro: obligatorio desde C12)")
    try:
        return json.loads(rpath.read_text())
    except (json.JSONDecodeError, OSError) as e:
        raise ReproError(f"repro.json inválido en {pdir.name}: {e}")


def _program_path(command: str, repo: Path):
    """Ruta repo-relativa del programa de un comando, o None si no es
    un archivo del repo (p. ej. `python3` con script relativo aparte)."""
    try:
        argv = shlex.split(command)
    except ValueError:
        return None
    if not argv:
        return None
    # `python3 scripts/x.py ...` -> el programa es argv[1]; en otro
    # caso, el programa es argv[0] si es ruta relativa.
    candidates = argv[1:2] if argv[0] in ("python3", "python") else argv[:1]
    for cand in candidates:
        p = repo / cand
        if not cand.startswith("/") and ".." not in Path(cand).parts \
                and p.is_file():
            return p
    return None


def check_proposal(pid: str, repo: Path = REPO,
                   timeout: int = DEFAULT_TIMEOUT_S,
                   run: bool = True) -> tuple:
    """Verifica el repro de una propuesta.

    Devuelve (ok: bool, messages: [str], pending: [str]). `ok` False =
    el PR debe fallar. Los pasos pendientes nunca hacen fallar por sí
    solos, salvo el tripwire de promoción (programa ya existente).
    """
    pdir = repo / "tools" / "evolution" / "proposals" / pid
    messages, pending_notes = [], []
    if not pdir.is_dir():
        return False, [f"{pid}: no existe el directorio de la propuesta"], []
    try:
        data = load_repro(pdir)
        commands, pending = normalize_repro(data)
    except ReproError as e:
        return False, [f"{pid}: {e}"], []
    if data.get("proposal") not in (None, pid):
        return False, [f"{pid}: 'proposal' en repro.json es "
                       f"{data.get('proposal')!r}, debe ser {pid!r}"], []
    errors = []
    for i, cmd in enumerate(commands):
        errors.extend(_validate_command(cmd, f"{pid}: commands[{i}]",
                                        require_sha=True, repo=repo))
    for i, cmd in enumerate(pending):
        where = f"{pid}: pending_commands[{i}]"
        errors.extend(_validate_command(cmd, where, require_sha=False,
                                        repo=repo))
        if isinstance(cmd, dict):
            if not isinstance(cmd.get("blocked_by"), str) \
                    or not cmd.get("blocked_by", "").strip():
                errors.append(f"{where}: 'blocked_by' obligatorio "
                              "(motivo por el que aún no es ejecutable)")
            prog = _program_path(cmd.get("command", ""), repo) \
                if isinstance(cmd.get("command"), str) else None
            if prog is not None:
                errors.append(f"{where}: el programa ya existe ({prog}) "
                              "y el paso sigue en pending — el bloqueo "
                              "caducó: promueve el paso a 'commands'")
            pending_notes.append(
                f"{pid}: paso pendiente {cmd.get('name')}: "
                f"{cmd.get('blocked_by', '')[:200]}")
    if errors:
        return False, [f"{pid}: " + e for e in errors], pending_notes
    if not run:
        messages.append(f"{pid}: esquema + fixtures OK "
                        f"({len(commands)} paso(s), sin ejecutar)")
        return True, messages, pending_notes
    for cmd in commands:
        argv = shlex.split(cmd["command"])
        try:
            r = subprocess.run(argv, cwd=repo, capture_output=True,
                               timeout=timeout, text=True, errors="replace")
        except subprocess.TimeoutExpired:
            return False, [f"{pid}: paso {cmd['name']}: timeout "
                           f"({timeout} s): {cmd['command']}"], pending_notes
        except OSError as e:
            return False, [f"{pid}: paso {cmd['name']}: no se pudo "
                           f"ejecutar: {e}"], pending_notes
        out = (r.stdout or "") + (r.stderr or "")
        want_rc = cmd.get("expected_exit_code", 0)
        if r.returncode != want_rc:
            return False, [f"{pid}: paso {cmd['name']}: exit "
                           f"{r.returncode} (esperado {want_rc}): "
                           f"{cmd['command']}\nsalida:\n{out[:2000]}"], \
                pending_notes
        # `expected`: un literal, o lista de literales (todos deben
        # aparecer; así el C7 afirma PROMOTED + los números de sus 5
        # puertas, no solo el veredicto).
        wanted = cmd["expected"]
        if isinstance(wanted, str):
            wanted = [wanted]
        missing = [w for w in wanted if w not in out]
        if missing:
            return False, [f"{pid}: paso {cmd['name']}: la salida NO "
                           f"contiene el/los 'expected' literal(es) "
                           f"{missing!r}:\n{out[:2000]}"], \
                pending_notes
        messages.append(f"{pid}: paso {cmd['name']}: OK "
                        f"(exit {r.returncode}, expected presente)")
    return True, messages, pending_notes


def changed_proposals(repo: Path, base: str) -> list:
    """IDs de propuestas con algún archivo cambiado entre base...HEAD."""
    r = subprocess.run(["git", "diff", "--name-only", f"{base}...HEAD"],
                       cwd=repo, capture_output=True, text=True)
    if r.returncode != 0:
        raise ReproError(f"git diff vs {base} falló: {r.stderr.strip()[:300]}")
    ids = []
    for line in r.stdout.splitlines():
        parts = Path(line).parts
        if len(parts) >= 4 and parts[0] == "tools" \
                and parts[1] == "evolution" and parts[2] == "proposals":
            if parts[3] not in ids:
                ids.append(parts[3])
    return sorted(ids)


def all_proposals(repo: Path) -> list:
    pdir = repo / "tools" / "evolution" / "proposals"
    if not pdir.is_dir():
        return []
    return sorted(p.name for p in pdir.iterdir() if p.is_dir())


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(
        description="Chequeo C12: bloque repro: obligatorio en propuestas.")
    ap.add_argument("--proposal", action="append", default=[],
                    help="id de propuesta a verificar (repetible)")
    ap.add_argument("--changed", action="store_true",
                    help="solo propuestas tocadas entre --base...HEAD")
    ap.add_argument("--base", default="",
                    help="sha base para --changed")
    ap.add_argument("--all", action="store_true",
                    help="todas las propuestas (auditoría; las "
                         "anteriores a C12 sin repro.json fallan)")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_S)
    ap.add_argument("--no-run", action="store_true",
                    help="solo esquema + fixtures, sin ejecutar comandos")
    args = ap.parse_args(argv)
    pids = list(args.proposal)
    if args.changed:
        if not args.base:
            print("error: --changed exige --base", file=sys.stderr)
            return 2
        try:
            pids.extend(changed_proposals(REPO, args.base))
        except ReproError as e:
            print(f"error: {e}", file=sys.stderr)
            return 2
    if args.all:
        pids.extend(all_proposals(REPO))
    pids = sorted(set(pids))
    if not pids:
        print("check_repro: ninguna propuesta tocada por el diff "
              "(nada que verificar)")
        return 0
    ok_all, pending_all = True, []
    for pid in pids:
        ok, messages, pending = check_proposal(pid, timeout=args.timeout,
                                               run=not args.no_run)
        pending_all.extend(pending)
        for m in messages:
            print(("OK  " if ok else "FAIL") + " " + m)
        if not ok:
            ok_all = False
    for note in pending_all:
        print("PENDIENTE " + note)
    if ok_all:
        print(f"check_repro: {len(pids)} propuesta(s) con repro válido: "
              + ", ".join(pids))
        return 0
    print(f"check_repro: FALLO en {len(pids)} propuesta(s) revisada(s)",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
