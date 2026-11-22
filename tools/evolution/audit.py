"""Registro auditable del motor de evolución (tools/evolution/audit.py).

Cada violación de contención y cada decisión de publicación deja una
línea JSONL con fecha, evento y detalle. El registro es SOLO append:
nunca se reescribe ni se borra desde el motor. Si no se puede escribir
(disco lleno, permisos), la acción que se audita NO se bloquea por eso:
auditar nunca debe romper el fail-closed del ciclo; el fallo de
escritura queda en stderr.

Ubicación: `tools/evolution/audit.jsonl` (en el repo, SIN commitear:
está en .gitignore; es evidencia local del operador, no parte del
producto). Para tests o para otro destino: `IVANNA_AUDIT_FILE`.

Formato por línea: {"ts": ISO8601 UTC, "event": str, "detail": str}.
Eventos: "containment_violation" (lo registra automáticamente
ContainmentViolation al construirse), "publish_decision",
"autonomous_decision", "hook_blocked" (lo registra el pre-push hook).
"""
import json
import os
import sys
from datetime import datetime, timezone
from pathlib import Path

EVO = Path(__file__).resolve().parent
DEFAULT_FILE = EVO / "audit.jsonl"


def audit_file() -> Path:
    override = os.environ.get("IVANNA_AUDIT_FILE")
    return Path(override) if override else DEFAULT_FILE


def record(event: str, detail: str) -> None:
    """Añade una línea al registro. Nunca lanza."""
    try:
        line = json.dumps({
            "ts": datetime.now(timezone.utc).isoformat(),
            "event": event,
            "detail": str(detail)[:2000],
        }, ensure_ascii=False)
        with open(audit_file(), "a", encoding="utf-8") as f:
            f.write(line + "\n")
    except Exception as exc:  # noqa: BLE001 - auditar nunca rompe el ciclo
        print(f"audit: no se pudo registrar {event}: {exc}", file=sys.stderr)


def tail(n: int = 20) -> list:
    """Últimas n líneas del registro (para la Vista de Dios / revisión)."""
    try:
        lines = audit_file().read_text(encoding="utf-8").splitlines()
    except OSError:
        return []
    out = []
    for line in lines[-n:]:
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return out
