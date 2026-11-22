#!/usr/bin/env python3
"""Exportación de estado del motor de evolución para la Vista de Dios.

La Vista de Dios (artefactos web externos, ver tools/evolution/README.md
§ Vista de Dios) muestra releases, runs de CI y salud del gateway con
datos REALES de la API pública de GitHub y del probe 401 de OmniRoute —
pero NO ve el interior del motor: qué propuestas existen, qué dijo el
consejo, qué bloqueó la contención. Este módulo es la pieza que faltaba
en el repo para alimentarla: un JSON con el estado del motor, sin
secretos (nunca incluye la API key ni tokens), que el operador puede
publicar o adjuntar a la sonda de la Vista de Dios.

Uso:
  python3 tools/evolution/status_export.py [--out PATH]
  python3 tools/evolution/evolution_engine.py --status [--status-out PATH]

Sin --out imprime el JSON por stdout. Con --out lo escribe en PATH.
El health del gateway hace una llamada real a localhost:20128 (si el
gateway está caído, gateway.ok=false: es un dato, no un error).
"""
import argparse
import json
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

EVO = Path(__file__).resolve().parent
REPO = EVO.parents[1]
PROPOSALS = EVO / "proposals"

# Los 6 modelos gratuitos del combo ivanna-free (verificados con
# /v1/models; ver OMNIROUTE_VERIFICATION.md).
IVANNA_FREE_MODELS = (
    "ddgw/claude-haiku-4-5",
    "ddgw/gpt-5.6-luna",
    "ddgw/gpt-5.4-mini",
    "ddgw/mistral-small-2603",
    "ddgw/tinfoil/gpt-oss-120b",
    "ddgw/tinfoil/gemma4-31b",
)


def _git(*args, cwd=REPO):
    r = subprocess.run(["git", *args], cwd=cwd,
                       capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def _gateway_status():
    """Health real del gateway (llm_client.health). Nunca lanza."""
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "ivanna_llm_client", EVO / "llm_client.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        ok, ms, models = mod.health()
        present = [m for m in IVANNA_FREE_MODELS if m in models]
        return {
            "ok": bool(ok),
            "latency_ms": round(ms, 1),
            "models_total": len(models),
            "ivanna_free_present": present,
            "ivanna_free_missing": [m for m in IVANNA_FREE_MODELS
                                    if m not in models],
        }
    except Exception as exc:  # noqa: BLE001 - gateway caído es un dato
        return {"ok": False, "latency_ms": 0.0, "models_total": 0,
                "ivanna_free_present": [], "ivanna_free_missing":
                list(IVANNA_FREE_MODELS), "error": str(exc)[:200]}


def _proposal_rows(proposals_dir=PROPOSALS):
    """Una fila por propuesta: veredicto medido, consejo, archivos."""
    rows = []
    if not proposals_dir.exists():
        return rows
    for pdir in sorted(proposals_dir.iterdir()):
        if not pdir.is_dir():
            continue
        row = {"id": pdir.name}
        vpath = pdir / "validation.json"
        if vpath.exists():
            try:
                v = json.loads(vpath.read_text())
            except (json.JSONDecodeError, OSError):
                v = {}
            row["applied"] = v.get("applied")
            fs = v.get("full_suite") or {}
            row["suite"] = ("passed" if fs.get("passed") is True
                            else "skipped" if fs.get("skipped") is True
                            else "failed" if fs else "unknown")
            row["files"] = v.get("files", [])
            row["tree_untouched"] = v.get("tree_untouched")
        council_path = pdir / "COUNCIL.md"
        if council_path.exists():
            text = council_path.read_text(errors="ignore")
            m = re.search(r"\*\*Veredicto:\*\* (\w+)", text)
            row["council"] = m.group(1) if m else "unknown"
            m = re.search(r"\*\*Rondas:\*\* (\d+)", text)
            row["council_rounds"] = int(m.group(1)) if m else None
        report_path = pdir / "REPORT.md"
        if report_path.exists():
            text = report_path.read_text(errors="ignore")
            m = re.search(r"\*\*Veredicto:\*\* (.+)$", text, re.M)
            row["verdict"] = m.group(1).strip() if m else "unknown"
        try:
            row["updated"] = datetime.fromtimestamp(
                pdir.stat().st_mtime, tz=timezone.utc).isoformat()
        except OSError:
            row["updated"] = None
        rows.append(row)
    return rows


def _audit_tail(n=20):
    try:
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "ivanna_audit", EVO / "audit.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        return mod.tail(n)
    except Exception:  # noqa: BLE001 - sin registro no hay cola
        return []


def export_status(repo=REPO, proposals_dir=PROPOSALS,
                  include_gateway=True) -> dict:
    """Foto del estado del motor. Sin secretos, sin escribir nada."""
    head = _git("log", "--oneline", "-1", cwd=repo)
    branch = _git("rev-parse", "--abbrev-ref", "HEAD", cwd=repo)
    dirty = _git("status", "--porcelain", cwd=repo)
    return {
        "generated_at": datetime.now(timezone.utc).isoformat(),
        "source": "tools/evolution/status_export.py (repo, sin secretos)",
        "repo": {
            "branch": branch,
            "head": head,
            "dirty_files": len(dirty.splitlines()) if dirty else 0,
        },
        "gateway": _gateway_status() if include_gateway else {"ok": None},
        "council": {
            "proposer": "ddgw/tinfoil/gpt-oss-120b",
            "implementer": "ddgw/claude-haiku-4-5",
            "reviewer": "ddgw/gpt-5.6-luna",
            "arbiter": "ddgw/gpt-5.4-mini",
            "max_rounds": 3,
        },
        "proposals": _proposal_rows(proposals_dir),
        "audit_tail": _audit_tail(20),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", help="archivo JSON donde escribir el estado")
    ap.add_argument("--no-gateway", action="store_true",
                    help="no probar el gateway (solo estado del repo)")
    args = ap.parse_args()
    status = export_status(include_gateway=not args.no_gateway)
    text = json.dumps(status, indent=2, ensure_ascii=False)
    if args.out:
        Path(args.out).write_text(text, encoding="utf-8")
        print(f"estado del motor: {args.out}")
    else:
        print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
