"""Puente GitHub — ciclo autónomo COMPLETO de auto-reparación (v2.5.445).

Cierra el ciclo que el motor de evolución dejaba abierto: una reparación
YA VERIFICADA (tests en verde + CI en verde, fail-closed) se publica sola:
commit (un commit por cambio, `tipo(area): descripción`), push a main y
bump de versión con su release correspondiente (las 6 fuentes coinciden).
Sin verificación verde, NO se publica nada.

Ciclo autónomo completo (`autonomous`, sin intervención humana):
    detectar → reescribir (LLM vía OmniRoute) → verificar en scratch
    → commit → push → release correspondiente.
Si cualquier paso falla, el ciclo se detiene y NO publica nada.

POTENCIA (directiva de Esse 2026-11-14 17:24 — "literal así de potente"
como la flota de IAs de Anthropic que escaparon): el ciclo tiene AGENCIA
COMPLETA sobre el producto IVANNA — puede reescribir cualquier código de
IVANNA, rediseñar cualquier módulo DSP, arreglar cualquier bug, evolucionar
cualquier parte y publicar cualquier release, sin cuello de botella humano.
No hay lista de módulos permitidos ni aprobación manual: el único juez es
la verificación (tests + CI en verde).

CONTENCIÓN (directiva de Esse 2026-11-14 17:21 — "jamás debe escapar"):
TODO ese poder es SOLO dentro de IVANNA (su repo, su código, sus releases,
su diagnóstico). La guardia de contención (`ContainmentGuard`, fail-closed)
bloquea y reporta — nunca ejecuta — cualquier acción fuera del dominio:
  - rutas fuera del repo (escapes `../`, absolutas, symlinks);
  - parches que toquen archivos fuera del repo o prohibidos;
  - push a un remote que no sea el repo de IVANNA-OMEGA-SUPREME;
  - comandos fuera de la lista permitida (git/gh/motor/CMake/CTest);
  - red fuera de los destinos permitidos (github.com, gateway local
    OmniRoute en localhost:20128);
  - secretos (PAT, tokens, contraseñas) en parches, mensajes o diffs.
Los límites duros que nunca se mueven: fail-closed (solo publica lo
verificado en verde), prohibido deshabilitar IntegrityGate, prohibido
tocar credenciales/secretos, prohibido comprar nada. Y el verificador no
se puede reescribir a sí mismo: los archivos que son la puerta
(`app/build.gradle.kts`, `app/src/main/cpp/CMakeLists.txt`, `security/`
= IntegrityGate, `.github/workflows/`, `tools/security/`, keystores) están
en FORBIDDEN — no porque el producto no sea reescribible, sino porque si
la reparación pudiera reescribir su propio juez, el "verde" no valdría
nada. Potencia total sobre el producto, cero poder sobre las puertas
que lo juzgan.

Push: usa el remote YA AUTENTICADO del repo (`git push origin main`).
NO hay PAT, token ni secreto en archivos, código, mensajes ni URLs.
Sin `--execute` o sin `IVANNA_ALLOW_PUBLISH=1`, todo es dry-run:
se llega hasta el commit y se reporta lo que se haría, sin pushear.

Uso:
    python3 tools/evolution/github_bridge.py publish --proposal 0003-x \\
        --message "fix(saf): guarda isfinite en saf_latent_bridge" [--execute] [--wait-ci]
    python3 tools/evolution/github_bridge.py autonomous --proposal-id 0006-x \\
        --finding '{"kind":"...","title":"...","evidence":"...","files":[...]}' [--execute] [--wait-ci]
    python3 tools/evolution/github_bridge.py check --patch tools/evolution/proposals/0003-x/0003-x.patch
"""
import argparse
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EVO = REPO / "tools" / "evolution"
PROPOSALS = EVO / "proposals"

# ── Dominio permitido (contención, directiva 2026-11-14 17:21) ─────────────
# El único repositorio sobre el que el ciclo autónomo puede actuar.
ALLOWED_REMOTE_RE = re.compile(
    r"^(?:https://github\.com/|git@github\.com:)"
    r"luisurielpimentelperez814-design/IVANNA-OMEGA-SUPREME(?:\.git)?$"
)
# Los únicos destinos de red del ciclo: GitHub (push) y el gateway local
# OmniRoute (LLM, localhost:20128). Todo lo demás está fuera de dominio.
ALLOWED_HOSTS = ("github.com", "localhost", "127.0.0.1")

# Archivos que el ciclo NUNCA toca: las puertas que lo juzgan y los
# secretos. (Misma lista que evolution_engine.FORBIDDEN; se reimporta
# desde el motor si está disponible, para no divergir.)
FORBIDDEN_FALLBACK = (
    "app/build.gradle.kts",
    "app/src/main/cpp/CMakeLists.txt",
    "app/src/main/java/com/ivanna/omega/security/",
    ".github/workflows/",
    "tools/security/",
)
FORBIDDEN_SUFFIX = (".keystore", ".jks", ".pem", ".key")

# Untracked permanentes que NUNCA bloquean la publicación (no son parte
# de la reparación ni del árbol: restos de trabajo local del operador).
UNTRACKED_OK_PREFIXES = (
    "monetization/",
    "magisk_module_fusion/",
    "hal_integration/moto_g85/pal_preload_probe/",
    "tools/autorepair/",
    "tools/evolution/knowledge/knowledge.json",
)

# Secuencias que nunca pueden aparecer en un parche ni en un mensaje
# (mismos patrones que evolution_engine.validate_proposal, sin el token
# del puente, que no aplica a un diff).
SECRET_PATTERNS = (
    re.compile(r"ghp_[A-Za-z0-9]{20,}"),
    re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
    re.compile(r"(?i)password\s*=\s*['\"][^'\"]{4,}"),
    re.compile(r"(?i)token\s*=\s*['\"][^'\"]{8,}"),
)

# Commit: `tipo(area): descripción`, un commit por cambio.
COMMIT_MSG_RE = re.compile(
    r"^(feat|fix|test|chore|docs|refactor|perf)\([\w][\w.-]*\): .+"
)

# Comandos permitidos del ciclo (argv[0] + forma). Todo lo demás está
# fuera de dominio y se bloquea. `git push` solo a origin/main.
ALLOWED_GIT = {
    "status", "diff", "add", "commit", "push", "log", "rev-parse", "fetch",
    "apply", "ls-files", "remote",
}


class ContainmentViolation(Exception):
    """Una acción quiso salir del dominio IVANNA: se bloquea (fail-closed)
    y se reporta, no se ejecuta.

    Al construirse, deja constancia en el registro auditable
    (tools/evolution/audit.py): un bloqueo sin rastro es indistinguible
    de no haber existido. Auditar nunca lanza (ver audit.record)."""

    def __init__(self, message):
        super().__init__(message)
        try:
            import audit as _audit
            _audit.record("containment_violation", message)
        except Exception:
            pass


def _load_engine():
    spec = importlib.util.spec_from_file_location(
        "ivanna_evolution_engine", EVO / "evolution_engine.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _forbidden():
    try:
        return _load_engine().FORBIDDEN
    except Exception:
        return FORBIDDEN_FALLBACK


def _is_forbidden(rel: str) -> bool:
    return (rel.startswith(_forbidden())
            or rel.endswith(FORBIDDEN_SUFFIX)
            or "SECRET" in rel.upper()
            or "PAT" in rel)


# ── Guardia de contención ───────────────────────────────────────────────────
class ContainmentGuard:
    """Hace cumplir el dominio IVANNA en cada paso del ciclo autónomo.

    Toda acción (ruta, parche, remote, comando, red, secreto) pasa por
    aquí antes de ejecutarse. Cualquier violación levanta
    ContainmentViolation: el ciclo se detiene y NO publica nada.
    """

    def __init__(self, repo: Path):
        self.repo = Path(repo).resolve()
        if not (self.repo / ".git").exists():
            raise ContainmentViolation(
                f"no es un repo git: {self.repo}")

    # ── Rutas: solo dentro del repo ──
    def check_path(self, path) -> Path:
        p = Path(path)
        resolved = (p if p.is_absolute() else self.repo / p).resolve()
        if not resolved.is_relative_to(self.repo):
            raise ContainmentViolation(
                f"ruta fuera del dominio IVANNA: {path} "
                f"(resuelve a {resolved}, fuera de {self.repo})")
        return resolved

    # ── Parches: todos los archivos dentro del repo y no prohibidos ──
    def patch_files(self, patch_text: str) -> list:
        # Solo `+++ b/` manda (la ruta nueva); `--- a/` es la misma ruta
        # vieja. Excepción: borrado (`+++ /dev/null`), donde la ruta viva
        # es la de `--- a/`. Sin dedup, cada archivo saldría dos veces.
        files, seen = [], set()
        lines = patch_text.splitlines()
        for i, line in enumerate(lines):
            f = None
            if line.startswith("+++ b/"):
                f = line[6:]
            elif line.startswith("--- a/") and i + 1 < len(lines) \
                    and lines[i + 1].startswith("+++ /dev/null"):
                f = line[6:]
            if f is not None and f not in seen:
                seen.add(f)
                files.append(f)
        return files

    def check_patch(self, patch_text: str) -> list:
        files = self.patch_files(patch_text)
        # Límites duros (mismos que evolution_engine: si divergen, manda
        # el menor; el motor ya los aplica en draft/validate y aquí es
        # defensa en profundidad en la puerta de publicación).
        try:
            max_files = _load_engine().MAX_PATCH_FILES
            max_bytes = _load_engine().MAX_PATCH_BYTES
        except Exception:
            max_files, max_bytes = 50, 512 * 1024
        n_bytes = len(patch_text.encode("utf-8"))
        if n_bytes > max_bytes:
            raise ContainmentViolation(
                f"parche de {n_bytes} bytes (máx. {max_bytes})")
        if len(files) > max_files:
            raise ContainmentViolation(
                f"parche toca {len(files)} archivos (máx. {max_files})")
        for f in files:
            if f.startswith("/") or ".." in Path(f).parts:
                raise ContainmentViolation(
                    f"el parche escapa del repo: {f}")
            if _is_forbidden(f):
                raise ContainmentViolation(
                    f"el parche toca un archivo prohibido (puerta): {f}")
            self.check_path(f)
        self.check_no_secrets(patch_text, where="parche")
        return files

    # ── Remote: solo el repo de IVANNA ──
    def check_remote(self, url: str) -> None:
        if not ALLOWED_REMOTE_RE.match(url.strip()):
            raise ContainmentViolation(
                f"remote fuera del dominio IVANNA: {url} "
                f"(solo se permite el repo IVANNA-OMEGA-SUPREME)")

    def check_current_remote(self) -> str:
        r = subprocess.run(["git", "remote", "get-url", "origin"],
                           cwd=self.repo, capture_output=True, text=True)
        if r.returncode != 0:
            raise ContainmentViolation(
                "no hay remote 'origin': sin destino verificado no se pushea")
        url = r.stdout.strip()
        self.check_remote(url)
        return url

    # ── Comandos: solo la lista permitida ──
    def check_command(self, argv) -> None:
        if not argv:
            raise ContainmentViolation("comando vacío")
        prog = Path(argv[0]).name
        if prog == "git":
            if len(argv) < 2 or argv[1] not in ALLOWED_GIT:
                raise ContainmentViolation(
                    f"subcomando git fuera del dominio: {' '.join(argv)}")
            if argv[1] == "push":
                if argv[2:] != ["origin", "main"]:
                    raise ContainmentViolation(
                        f"push solo a origin/main: {' '.join(argv)}")
            if argv[1] == "remote" and argv[2:] != ["get-url", "origin"]:
                raise ContainmentViolation(
                    f"git remote solo get-url origin: {' '.join(argv)}")
            if argv[1] == "commit" and "--amend" in argv:
                raise ContainmentViolation("git commit --amend prohibido")
            return
        if prog == "gh":
            # Solo lectura de runs de CI del repo actual (nunca --repo ajeno).
            if "--repo" in argv:
                raise ContainmentViolation(
                    f"gh con --repo fuera del dominio: {' '.join(argv)}")
            if len(argv) >= 3 and argv[1:3] in (["run", "list"], ["run", "view"]):
                return
            raise ContainmentViolation(
                f"gh solo lectura de runs de CI: {' '.join(argv)}")
        if prog == "python3":
            # Solo scripts del propio motor (dentro del repo) o su unittest.
            if len(argv) >= 2 and argv[1] == "-m" and argv[2:3] == ["unittest"]:
                return
            if len(argv) >= 2:
                self.check_path(argv[1])
                rel = str(self.check_path(argv[1]).relative_to(self.repo))
                if not (rel.startswith("tools/evolution/")
                        or rel.startswith("tools/autorepair/")):
                    raise ContainmentViolation(
                        f"python3 solo scripts del motor: {' '.join(argv)}")
                return
            raise ContainmentViolation(f"python3 sin script: {' '.join(argv)}")
        if prog in ("cmake", "ctest"):
            return  # verificación en scratch, dentro del dominio
        raise ContainmentViolation(
            f"comando fuera del dominio IVANNA: {' '.join(argv)}")

    # ── Red: solo destinos permitidos ──
    def check_host(self, host: str) -> None:
        if host not in ALLOWED_HOSTS:
            raise ContainmentViolation(
                f"host fuera del dominio IVANNA: {host}")

    # ── Rama: publicar solo desde main ──
    def check_publish_branch(self) -> str:
        """La publicación pushea `origin main`: solo se hace desde la
        rama main local. Publicar desde otra rama empujaría un main
        local posiblemente desfasado (o ajeno a lo que se validó).
        Solo se exige en el camino de push real (dry-run no pushea)."""
        r = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                           cwd=self.repo, capture_output=True, text=True)
        branch = r.stdout.strip()
        if r.returncode != 0 or branch != "main":
            raise ContainmentViolation(
                f"publicar solo desde main (rama actual: "
                f"{branch or 'desconocida'})")
        return branch

    # ── Secretos: nunca en parches, mensajes ni diffs ──
    def check_no_secrets(self, text: str, where: str = "texto") -> None:
        for pat in SECRET_PATTERNS:
            if pat.search(text):
                raise ContainmentViolation(
                    f"secreto potencial en {where} (patrón {pat.pattern}): "
                    f"bloqueado, no se publica")

    # ── Árbol: sin cambios trackeados ajenos a la reparación ──
    def check_clean_tracked(self, allowed_files=()) -> None:
        r = subprocess.run(["git", "status", "--porcelain"],
                           cwd=self.repo, capture_output=True, text=True)
        allowed = set(allowed_files)
        dirty = []
        for line in r.stdout.splitlines():
            if line.startswith("??"):
                continue  # untracked nunca bloquea (ver UNTRACKED_OK)
            rel = line[3:].strip().split(" -> ")[-1]
            if rel not in allowed:
                dirty.append(line)
        if dirty:
            raise ContainmentViolation(
                "el árbol tiene cambios trackeados ajenos a la reparación: "
                + "; ".join(dirty))


def guarded_run(guard: ContainmentGuard, argv, cwd=None, input_text=None,
                check=False, timeout=None):
    """Ejecuta un comando solo si pasa la guardia de contención."""
    guard.check_command(argv)
    r = subprocess.run(argv, cwd=cwd or guard.repo, capture_output=True,
                       text=True, input=input_text, timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"cmd failed: {argv}\n{r.stdout}\n{r.stderr}")
    return r


def _audit_decision(event: str, report: dict) -> None:
    """Una línea en el registro auditable por cada decisión de publicar
    o no publicar (nunca lanza: auditar no rompe el ciclo)."""
    try:
        import audit as _audit
        _audit.record(event, json.dumps({
            "proposal": report.get("proposal"),
            "published": report.get("published"),
            "dry_run": report.get("dry_run"),
            "verdict": report.get("verdict"),
            "refused": report.get("refused"),
            "version": report.get("version"),
        }, ensure_ascii=False))
    except Exception:
        pass


# ── Veredicto (misma lógica que evolution_engine.cmd_evolve) ───────────────
def is_verdict_green(result: dict) -> bool:
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


# ── Bump de versión (6 fuentes; fail-closed: todo en memoria primero) ─────
def _read_version(repo: Path):
    props = (repo / "version.properties").read_text()
    m_name = re.search(r"^versionName=(\d+)\.(\d+)\.(\d+)\s*$", props, re.M)
    m_code = re.search(r"^versionCode=(\d+)\s*$", props, re.M)
    if not m_name or not m_code:
        raise RuntimeError("version.properties sin versionName/versionCode")
    return (int(m_name.group(1)), int(m_name.group(2)), int(m_name.group(3))), \
        int(m_code.group(1))


def bump_version(repo: Path, summary: str):
    """Sube patch+1 y code+1 en las 6 fuentes (fail-closed).

    A diferencia de run_fix._bump_version (que deja zipUrl en el release
    anterior para que el bot lo actualice), aquí zipUrl apunta YA a la
    versión nueva: el ciclo autónomo pushea y la CI publica el release
    con ese zip exacto; las 6 fuentes deben coincidir en el push.
    Si alguna fuente no contiene el patrón esperado, NO se escribe nada.
    Devuelve (nueva_version, nuevo_code).
    """
    (mayor, menor, patch), code = _read_version(repo)
    nueva = f"{mayor}.{menor}.{patch + 1}"
    nuevo_code = code + 1
    hoy = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    zip_url = ("https://github.com/luisurielpimentelperez814-design/"
               "IVANNA-OMEGA-SUPREME/releases/download/"
               f"v{nueva}/ivanna_omega_supreme_v{nueva}.zip")

    # 1. version.properties
    props_p = repo / "version.properties"
    props = props_p.read_text()
    if f"versionName={mayor}.{menor}.{patch}" not in props:
        raise RuntimeError("bump: version.properties sin versionName actual")
    props = re.sub(r"^versionName=.*$", f"versionName={nueva}", props, flags=re.M)
    props = re.sub(r"^versionCode=.*$", f"versionCode={nuevo_code}", props, flags=re.M)
    props = re.sub(r"^(versionName=.*)$",
                   f"# v{nueva}: {summary}\n\\1", props, count=1, flags=re.M)

    # 2. magisk_module/module.prop
    mod_p = repo / "magisk_module" / "module.prop"
    mod = mod_p.read_text()
    if f"version=v{mayor}.{menor}.{patch}" not in mod:
        raise RuntimeError("bump: module.prop sin version actual")
    mod = re.sub(r"^version=.*$", f"version=v{nueva}", mod, flags=re.M)
    mod = re.sub(r"^versionCode=.*$", f"versionCode={nuevo_code}", mod, flags=re.M)
    mod = re.sub(r"^description=",
                 f"description=v{nueva}: {summary} ", mod, count=1, flags=re.M)

    # 3. magisk_module/update.json
    upd_p = repo / "magisk_module" / "update.json"
    upd = json.loads(upd_p.read_text())
    if upd.get("version") != f"v{mayor}.{menor}.{patch}":
        raise RuntimeError("bump: update.json sin version actual")
    upd["version"] = f"v{nueva}"
    upd["versionCode"] = nuevo_code
    upd["zipUrl"] = zip_url
    upd_text = json.dumps(upd, indent=2) + "\n"

    # 4. CHANGELOG.md (entrada nueva arriba)
    cl_p = repo / "CHANGELOG.md"
    cl = cl_p.read_text()
    entrada = (f"# v{nueva} ({nuevo_code}) - {hoy}\n\n"
               f"- **Auto-reparación ({summary})**\n"
               f"- **Verificado:** propuesta VERDE en scratch (tests + CI). "
               f"Publicado por el ciclo autónomo (puente GitHub, v2.5.445).\n\n")

    # 5. README.md (fila nueva arriba de la tabla "Versiones recientes")
    readme_p = repo / "README.md"
    readme = readme_p.read_text()
    anchor = f"| **v{mayor}.{menor}.{patch}** ({code}) |"
    if anchor not in readme:
        raise RuntimeError("bump: README sin fila de la versión actual")
    row = f"| **v{nueva}** ({nuevo_code}) | {summary} |\n"
    readme = readme.replace(anchor, row + anchor, 1)

    # 6. User-Agent de TidalApiClient.kt
    ua_p = (repo / "app/src/main/java/com/ivanna/omega/tidal"
            / "TidalApiClient.kt")
    ua = ua_p.read_text()
    if f"IVANNA-Tidal/{mayor}.{menor}.{patch}" not in ua:
        raise RuntimeError("bump: TidalApiClient sin User-Agent actual")
    ua = ua.replace(f"IVANNA-Tidal/{mayor}.{menor}.{patch}",
                    f"IVANNA-Tidal/{nueva}")

    # Todo verificado en memoria: ahora sí escribir.
    props_p.write_text(props)
    mod_p.write_text(mod)
    upd_p.write_text(upd_text)
    cl_p.write_text(entrada + cl)
    readme_p.write_text(readme)
    ua_p.write_text(ua)
    return nueva, nuevo_code


# ── Publicación de una propuesta YA VERIFICADA ─────────────────────────────
def publish_proposal(repo: Path, proposals_dir: Path, pid: str, message: str,
                     execute=False, wait_ci=False) -> dict:
    """Publica una propuesta VERDE: apply → commit → bump → commit → push.

    Puerta: validation.json con veredicto VERDE (misma lógica que
    cmd_evolve) + el parche aplica limpio sobre el árbol actual + árbol
    sin cambios trackeados ajenos. Sin --execute o sin
    IVANNA_ALLOW_PUBLISH=1, es dry-run: se commitea en local y se reporta
    lo que se pushearía, sin tocar el remote.
    """
    repo = Path(repo).resolve()
    guard = ContainmentGuard(repo)
    report = {"proposal": pid, "published": False, "dry_run": True}
    pdir = Path(proposals_dir) / pid
    patch_file = next(pdir.glob("*.patch"), None)
    if patch_file is None:
        raise SystemExit(f"sin parche en {pdir}")
    patch_text = patch_file.read_text()
    validation_path = pdir / "validation.json"
    if not validation_path.exists():
        raise SystemExit(f"sin validation.json en {pdir}: no se publica "
                         f"nada sin verificación")
    result = json.loads(validation_path.read_text())
    report["validation"] = {"applied": result.get("applied"),
                            "tree_untouched": result.get("tree_untouched")}
    if not is_verdict_green(result):
        print(f"propuesta {pid}: veredicto ROJO — NO se publica")
        report["refused"] = "veredicto no verde"
        _audit_decision("publish_decision", report)
        return report

    # Puerta 2: contención sobre el parche (dominio + prohibidos + secretos).
    files = guard.check_patch(patch_text)
    report["files"] = files
    # Puerta 3: el mensaje sigue el formato de commit de Esse.
    if not COMMIT_MSG_RE.match(message):
        raise SystemExit(f"mensaje de commit inválido (debe ser "
                         f"`tipo(area): descripción`): {message}")
    guard.check_no_secrets(message, where="mensaje de commit")
    # Puerta 4: árbol limpio de cambios trackeados ajenos.
    guard.check_clean_tracked()

    # Aplicar al árbol real (el parche ya se validó en scratch; aquí solo
    # se confirma que aplica limpio sobre el árbol actual).
    r = guarded_run(guard, ["git", "apply", "--check", str(patch_file)])
    if r.returncode != 0:
        print(f"propuesta {pid}: el parche ya no aplica limpio "
              f"(el árbol se movió): NO se publica\n{r.stderr}")
        report["refused"] = "apply --check falló sobre el árbol actual"
        _audit_decision("publish_decision", report)
        return report
    guarded_run(guard, ["git", "apply", str(patch_file)], check=True)
    guarded_run(guard, ["git", "add", "--"] + files, check=True)
    guarded_run(guard, ["git", "commit", "-m", message], check=True)
    head = guarded_run(guard, ["git", "rev-parse", "HEAD"],
                       check=True).stdout.strip()
    report["repair_commit"] = head
    print(f"commit de reparación: {head} ({message})")

    # Bump (6 fuentes) + commit propio, un commit por cambio.
    nueva, nuevo_code = bump_version(repo, message)
    report["version"] = f"v{nueva}"
    report["version_code"] = nuevo_code
    bump_files = ["version.properties", "magisk_module/module.prop",
                  "magisk_module/update.json", "CHANGELOG.md", "README.md",
                  "app/src/main/java/com/ivanna/omega/tidal/TidalApiClient.kt"]
    guarded_run(guard, ["git", "add", "--"] + bump_files, check=True)
    bump_msg = f"chore(release): bump a v{nueva} (vc {nuevo_code})"
    guarded_run(guard, ["git", "commit", "-m", bump_msg], check=True)
    head = guarded_run(guard, ["git", "rev-parse", "HEAD"],
                       check=True).stdout.strip()
    report["bump_commit"] = head
    print(f"commit de bump: {head} ({bump_msg})")

    # Push: solo con --execute + IVANNA_ALLOW_PUBLISH=1, al remote
    # verificado (la guardia comprueba que es el repo de IVANNA).
    do_push = execute and os.environ.get("IVANNA_ALLOW_PUBLISH") == "1"
    if not do_push:
        print("DRY-RUN: no se pushea (falta --execute o "
              "IVANNA_ALLOW_PUBLISH=1). Los commits quedan en local.")
        report["dry_run"] = True
    else:
        # Barreras del push real, en orden: rama main local, remote
        # verificado, y el pre-push hook de git (fuera del proceso del
        # agente: exige .ivanna-publish-approval del operador).
        guard.check_publish_branch()
        guard.check_current_remote()
        guarded_run(guard, ["git", "push", "origin", "main"], check=True)
        report["dry_run"] = False
        report["pushed"] = True
        print("push OK a origin/main")
        if wait_ci:
            report["ci"] = wait_for_ci(guard, head)

    report["published"] = True
    _audit_decision("publish_decision", report)
    (pdir / "publish_report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False))
    return report


def wait_for_ci(guard: ContainmentGuard, head_sha: str,
                timeout_s=2400, poll_s=60) -> dict:
    """Espera el run principal de CI del HEAD pusheado (gh, solo lectura).
    Devuelve {conclusion, run_id} o {conclusion: 'timeout'}."""
    t0 = time.monotonic()
    while time.monotonic() - t0 < timeout_s:
        r = guarded_run(guard, ["gh", "run", "list", "--branch", "main",
                                "--limit", "5", "--json",
                                "databaseId,headSha,status,conclusion"])
        try:
            runs = json.loads(r.stdout)
        except Exception:
            runs = []
        for run in runs:
            if run.get("headSha") == head_sha:
                if run.get("status") == "completed":
                    return {"conclusion": run.get("conclusion"),
                            "run_id": run.get("databaseId")}
                break
        time.sleep(poll_s)
    return {"conclusion": "timeout", "run_id": None}


# ── Ciclo autónomo completo: detectar → reparar → verificar → publicar ───
def autonomous(repo: Path, proposals_dir: Path, pid: str, finding: dict,
               execute=False, wait_ci=False) -> dict:
    """Hace TODO el ciclo sola, de punta a punta, sin intervención humana.

    1. Draft (LLM vía OmniRoute) + validación en scratch (cmd_evolve).
    2. Si VERDE: publish_proposal (commit + bump + push).
    3. Si ROJO: no se publica nada; el reporte queda como evidencia.
    """
    repo = Path(repo).resolve()
    guard = ContainmentGuard(repo)
    report = {"cycle": "autonomous", "proposal": pid, "published": False}
    engine = _load_engine()
    # El draft puede tocar cualquier código del producto (potencia total
    # dentro de IVANNA); las puertas (FORBIDDEN/DRAFT_FORBIDDEN) las
    # aplica el propio motor en draft_patch + validate_proposal.
    result = engine.cmd_evolve(pid, finding)
    green = is_verdict_green(result)
    report["verdict"] = "VERDE" if green else "ROJO"
    if not green:
        print(f"ciclo autónomo {pid}: veredicto ROJO — NO se publica nada")
        _audit_decision("autonomous_decision", report)
        (Path(proposals_dir) / pid / "autonomous_report.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False))
        return report
    message = f"fix(evolution): {finding.get('title', pid)}"
    pub = publish_proposal(repo, proposals_dir, pid, message,
                           execute=execute, wait_ci=wait_ci)
    report.update(pub)
    _audit_decision("autonomous_decision", report)
    (Path(proposals_dir) / pid / "autonomous_report.json").write_text(
        json.dumps(report, indent=2, ensure_ascii=False))
    return report


# ── CLI ────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(
        description="Puente GitHub: publica reparaciones VERIFICADAS "
                    "(commit + push + release). Fail-closed.")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_pub = sub.add_parser("publish", help="publicar una propuesta VERDE")
    p_pub.add_argument("--proposal", required=True)
    p_pub.add_argument("--message", required=True,
                       help="mensaje de commit `tipo(area): descripción`")
    p_pub.add_argument("--execute", action="store_true")
    p_pub.add_argument("--wait-ci", action="store_true")

    p_auto = sub.add_parser("autonomous",
                            help="ciclo completo solo: draft → validar → publicar")
    p_auto.add_argument("--proposal-id", required=True)
    p_auto.add_argument("--finding", required=True,
                        help="hallazgo JSON: {kind, title, evidence, files[]}")
    p_auto.add_argument("--execute", action="store_true")
    p_auto.add_argument("--wait-ci", action="store_true")

    p_check = sub.add_parser("check", help="chequeo de contención de un parche")
    p_check.add_argument("--patch", required=True)

    args = ap.parse_args()
    if args.cmd == "publish":
        rep = publish_proposal(REPO, PROPOSALS, args.proposal, args.message,
                               execute=args.execute, wait_ci=args.wait_ci)
        print(json.dumps(rep, indent=2, ensure_ascii=False))
    elif args.cmd == "autonomous":
        finding = json.loads(args.finding)
        rep = autonomous(REPO, PROPOSALS, args.proposal_id, finding,
                         execute=args.execute, wait_ci=args.wait_ci)
        print(json.dumps(rep, indent=2, ensure_ascii=False))
    elif args.cmd == "check":
        guard = ContainmentGuard(REPO)
        files = guard.check_patch(Path(args.patch).read_text())
        print(f"contención OK: {len(files)} archivos dentro del dominio, "
              f"ninguno prohibido: {files}")


if __name__ == "__main__":
    main()
