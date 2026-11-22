#!/usr/bin/env python3
"""Instalador del gancho pre-push de IVANNA (tools/evolution/install_publish_hook.py).

La barrera crítica de publicación NO puede vivir solo en una variable
de entorno (IVANNA_ALLOW_PUBLISH=1) que el propio proceso del agente
controla: debe aplicarla git, fuera del proceso del agente. Este script
instala `.git/hooks/pre-push` en el clon local (NO se commitea: los
hooks no se versionan; cada clon lo instala una vez).

Regla del gancho: un push a `origin/main` solo pasa si existe el
archivo de aprobación `.ivanna-publish-approval` en la raíz del repo
(con `proposal=<id>` y `head=<sha del HEAD que se pushea>`). Ese
archivo lo crea EL OPERADOR (humano) tras revisar la propuesta VERDE;
el agente no puede auto-aprobarse porque el gancho lo ejecuta git, no
el agente. Los pushes a otras ramas no se tocan. Un push bloqueado
queda en el registro auditable (evento "hook_blocked").

El archivo de aprobación está en .gitignore: nunca se commitea.

Uso:
  python3 tools/evolution/install_publish_hook.py --install [--force]
  python3 tools/evolution/install_publish_hook.py --status
  python3 tools/evolution/install_publish_hook.py --remove
"""
import argparse
import stat
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
HOOK = REPO / ".git" / "hooks" / "pre-push"
MARKER = "# IVANNA-PUBLISH-GUARD"

HOOK_BODY = '''#!/bin/sh
# IVANNA-PUBLISH-GUARD — gancho pre-push de IVANNA (no editar a mano;
# se instala con tools/evolution/install_publish_hook.py).
#
# Publicar a origin/main exige aprobación del OPERADOR: el archivo
# .ivanna-publish-approval (raíz del repo) con:
#   proposal=<id de la propuesta VERDE>
#   head=<sha del HEAD que se pushea>
# El agente NO puede auto-aprobarse: este gancho lo ejecuta git, fuera
# del proceso del agente. Sin aprobación, el push se bloquea (exit 1).
# Los pushes a otras ramas no se tocan.
remote="$1"
url="$2"
root=$(git rev-parse --show-toplevel 2>/dev/null) || exit 0
approval="$root/.ivanna-publish-approval"
blocked=0
while read -r local_ref local_sha remote_ref remote_sha; do
  case "$remote_ref" in
    refs/heads/main) ;;
    *) continue ;;
  esac
  if [ "$remote" != "origin" ]; then continue; fi
  ok=0
  if [ -f "$approval" ]; then
    ahead=$(grep -E "^head=$local_sha\\$" "$approval" 2>/dev/null)
    aprop=$(grep -E "^proposal=[A-Za-z0-9._-]+\\$" "$approval" 2>/dev/null)
    if [ -n "$ahead" ] && [ -n "$aprop" ]; then ok=1; fi
  fi
  if [ "$ok" -ne 1 ]; then
    echo "BLOQUEADO por IVANNA-PUBLISH-GUARD: push a origin/main sin aprobación del operador." >&2
    echo "Crea $approval con proposal=<id> y head=$local_sha tras revisar la propuesta VERDE." >&2
    blocked=1
  fi
done
if [ "$blocked" -eq 1 ]; then
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo "?")
  printf '%s\\n' "{\\"ts\\": \\"$ts\\", \\"event\\": \\"hook_blocked\\", \\"detail\\": \\"push a origin/main sin .ivanna-publish-approval\\"}" >> "$root/tools/evolution/audit.jsonl" 2>/dev/null || true
  exit 1
fi
exit 0
'''


def _git(*args):
    return subprocess.run(["git", *args], cwd=REPO,
                          capture_output=True, text=True)


def install(force: bool = False) -> int:
    if not (REPO / ".git").exists():
        print(f"no es un repo git: {REPO}")
        return 1
    if HOOK.exists() and MARKER not in HOOK.read_text(errors="ignore") and not force:
        print(f"ya existe un pre-push hook ajeno en {HOOK}: "
              f"revísalo a mano o usa --force")
        return 1
    HOOK.parent.mkdir(parents=True, exist_ok=True)
    HOOK.write_text(HOOK_BODY)
    HOOK.chmod(HOOK.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    print(f"gancho instalado: {HOOK}")
    print("Los pushes a origin/main exigen ahora .ivanna-publish-approval "
          "del operador (ver tools/evolution/install_publish_hook.py).")
    return 0


def status() -> int:
    if not HOOK.exists():
        print("sin gancho pre-push instalado")
        return 1
    installed = MARKER in HOOK.read_text(errors="ignore")
    print(f"gancho: {HOOK} "
          f"({'IVANNA-PUBLISH-GUARD' if installed else 'AJENO'})")
    return 0 if installed else 1


def remove() -> int:
    if HOOK.exists() and MARKER in HOOK.read_text(errors="ignore"):
        HOOK.unlink()
        print(f"gancho eliminado: {HOOK}")
        return 0
    print("no hay gancho de IVANNA que eliminar")
    return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--install", action="store_true")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--remove", action="store_true")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()
    if args.install:
        return install(args.force)
    if args.status:
        return status()
    if args.remove:
        return remove()
    ap.print_help()
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
