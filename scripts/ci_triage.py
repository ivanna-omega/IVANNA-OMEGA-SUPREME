#!/usr/bin/env python3
"""ci_triage.py — Auto-reintento discriminado + triaje automático, C6 (release 510).

Clasifica un fallo del job `ctest` de tests-host.yml a partir del artefacto
de C5 (ctest_last.log + ctest_junit.xml, y si existe LastTestsFailed.log) y
decide si la CI puede reintentarlo sola o debe escalarlo a un humano.

REGLAS DEL CLASIFICADOR (conservador: ante duda, NO reintentar y escalar).
Precedencia estricta, en este orden:

  1. FALLO DE ASERCION -> `no-retry-assertion` (NUNCA se reintenta).
     Evidencia: linea de resultado CTest `(Failed|SEGFAULT|Subprocess
     aborted|Exception)`; `ERROR: AddressSanitizer` / `SUMMARY:
     AddressSanitizer`; `runtime error:` / `SUMMARY:
     UndefinedBehaviorSanitizer`; lineas gtest `[  FAILED  ]`; un
     `<failure>` o `<error>` en el JUnit; `Segmentation fault`.
     Gana aunque haya tambien timeout o senales de infra: un fallo real
     disfrazado de timeout no se reintenta.
  2. TEST INDIVIDUAL CON `(Timeout)` -> `no-retry-deadlock-suspect`.
     Un test colgado puede ser un deadlock real, no lentitud del runner
     (mitigacion obligatoria de la propuesta). No se reintenta sin
     revision humana; el comentario lo marca como sospechoso.
  3. TIMEOUT DEL JOB O INFRA -> `retry` (maximo 1 reintento por run,
     estructural: el job ci-triage corre una sola vez por workflow run
     y no se re-dispara a si mismo).
     Timeout del job: el log trae `exceeded the maximum execution time`
     (o el resultado del job es `cancelled` CON esa senal). Un `cancelled`
     sin senal de timeout es cancelacion manual o por concurrencia
     (un push nuevo cancela la corrida vieja): NO se reintenta, se
     escala como `no-retry-unknown`.
     Infra: solo errores de la lista explicita INFRA_PATTERNS
     (shutdown del runner, perdida de comunicacion, disco lleno,
     descarga de acciones, DNS/red, timeout de job). Nada fuera de
     esa lista cuenta como infra.
     Modo de reintento, en orden:
       a. `rerun-failed` si existe LastTestsFailed.log (el comando de
          la propuesta: `ctest --rerun-failed`);
       b. `by-name` si el log muestra tests empezados sin resultado o
          `Not Run` (el job murio a mitad de corrida y no hay
          LastTestsFailed.log): `ctest -R '^(a|b)$'`;
       c. `full` si ctest nunca llego a arrancar (el timeout cayo en
          configurar/compilar: todo esta "no corrido");
       d. `none` si ctest termino entero (el timeout cayo fuera de
          CTest, p. ej. en los chequeos Fase 0): no hay nada que
          reintentar.
  4. Cualquier otro caso -> `no-retry-unknown`: se escala con el
     diagnostico. Incluye: artefacto C5 no disponible, fallo de
     compilacion/configuracion (CTest paso pero el job fallo en otro
     paso, p. ej. Fase 0), y fallo sin evidencia clasificable.

LIMITES DUROS (propuesta C6):
  - La "auto-reparacion" NO escribe codigo ni pushea a main. Main esta
    protegida (PR + aprobacion de Esse).
  - Lo maximo autonomo permitido seria abrir una rama
    `ci-autofix/<job>-<fecha>` con un PR en borrador y el diagnostico.
    Ese camino esta DISENADO en tests-host.yml pero DESACTIVADO por
    defecto (env CI_TRIAGE_AUTOFIX_DRAFT_PR='false'): falta la
    confirmacion de Esse de que un PR en borrador abierto por la CI es
    aceptable. No activar sin su si explicito.

Solo stdlib. Uso:
  python3 scripts/ci_triage.py classify --ctest-result failure|_cancelled
      [--log ctest_last.log] [--junit ctest_junit.xml]
      [--last-tests-failed LastTestsFailed.log]
      [--out verdict.json] [--github-output $GITHUB_OUTPUT]
  python3 scripts/ci_triage.py render --verdict verdict.json
      --run-url URL --sha SHA [--job ctest] [--retry-of]
  `classify` siempre sale 0 (el veredicto va en el JSON); `render`
  escribe el comentario Markdown en stdout.
"""

import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

CTEST_DIR = "build/tests-host"

# --- Patrones de clasificacion ---------------------------------------------

# Linea de resultado CTest, p. ej.:
#   "  5/1835 Test #5: test_x ............***Failed    0.12 sec"
#   "  7/1835 Test #7: test_y ............***Timeout  300.00 sec"
#   "  9/1835 Test #9: test_z ............***Not Run   0.00 sec"
CTEST_RESULT_RE = re.compile(
    r"^\s*\d+/\d+\s+Test\s+#(\d+):\s+(\S+)\s+\.+[*]*\s*"
    r"(Passed|Failed|Timeout|Not Run|Disabled|SEGFAULT|Subprocess aborted|Exception)\b"
)
CTEST_START_RE = re.compile(r"^\s*Start\s+(\d+):\s+(\S+)")
GTEST_FAILED_RE = re.compile(r"\[\s*FAILED\s*\]\s+(\S+)")
CTEST_SUMMARY_RE = re.compile(
    r"(\d+)% tests passed, (\d+) tests failed out of (\d+)"
)

ASAN_RE = re.compile(r"ERROR: AddressSanitizer|SUMMARY: AddressSanitizer")
UBSAN_RE = re.compile(r"runtime error:|SUMMARY: UndefinedBehaviorSanitizer")
SEGFAULT_RE = re.compile(r"Segmentation fault")

# Errores de infra: lista EXPLICITA y cerrada. Nada fuera de ella es infra.
# (nombre, patron). La senal de timeout del job tambien vive aqui porque
# es la evidencia de que el runner mato el job, no un test.
INFRA_PATTERNS = (
    ("runner-shutdown", re.compile(r"The runner has received a shutdown signal")),
    ("lost-communication", re.compile(r"Lost communication with the server")),
    ("disk-full", re.compile(r"No space left on device")),
    ("action-download", re.compile(r"Failed to download action")),
    ("dns", re.compile(r"Could not resolve host|Temporary failure in name resolution")),
    ("network", re.compile(r"Connection timed out|Connection reset by peer|Failed to connect")),
    ("download", re.compile(r"Unable to download")),
    ("job-timeout", re.compile(r"exceeded the maximum execution time")),
)

BUILD_FAILURE_RE = re.compile(r"CMake Error|ninja: build stopped|make: \*\*\*")

# Un fallo de asercion en el JUnit es <failure> o <error> dentro de <testcase>.
MAX_REPRO_NAMES = 20      # nombres en el comando -R de reproduccion
MAX_RETRY_NAMES = 50      # nombres en el -R del reintento by-name


def _short(text, limit=160):
    text = " ".join(text.split())
    return text if len(text) <= limit else text[: limit - 3] + "..."


def _dedup(seq):
    seen, out = set(), []
    for item in seq:
        if item not in seen:
            seen.add(item)
            out.append(item)
    return out


def _regex_alt(names):
    """Nombres de test como alternativa regex segura para ctest -R."""
    return "|".join(re.escape(n) for n in names)


def parse_log(text):
    """Extrae del ctest_last.log todo lo que el clasificador necesita."""
    failed, timed_out, not_run, unfinished = [], [], [], []
    assertion_signals = []
    started = {}   # nº de test -> nombre (lineas "Start N: nombre")
    finished = set()  # nº de test con linea de resultado

    for line in text.splitlines():
        m = CTEST_START_RE.match(line)
        if m:
            started[m.group(1)] = m.group(2)
            continue
        m = CTEST_RESULT_RE.match(line)
        if m:
            num, name, status = m.group(1), m.group(2), m.group(3)
            finished.add(num)
            if status in ("Failed", "SEGFAULT", "Subprocess aborted", "Exception"):
                failed.append(name)
                assertion_signals.append(f"ctest: {name} ({status})")
            elif status == "Timeout":
                timed_out.append(name)
            elif status == "Not Run":
                not_run.append(name)
            continue
        m = GTEST_FAILED_RE.search(line)
        if m:
            # Evidencia corroborante, NO identidad de test: el nombre
            # canonico para `ctest -R` es el de la linea de resultado
            # CTest (test_beta), no el Suite.Test de gtest.
            assertion_signals.append(f"gtest FAILED: {m.group(1)}")
        if ASAN_RE.search(line):
            assertion_signals.append("sanitizer: " + _short(line))
        elif UBSAN_RE.search(line):
            assertion_signals.append("sanitizer: " + _short(line))
        elif SEGFAULT_RE.search(line):
            assertion_signals.append("segfault: " + _short(line))

    for num, name in started.items():
        if num not in finished:
            unfinished.append(name)

    summary = CTEST_SUMMARY_RE.search(text)
    totals = None
    if summary:
        totals = {
            "percent_passed": int(summary.group(1)),
            "failed_count": int(summary.group(2)),
            "total": int(summary.group(3)),
        }

    infra_signals = sorted(
        {name for name, pat in INFRA_PATTERNS if pat.search(text)}
    )
    return {
        "failed_tests": _dedup(failed),
        "timed_out_tests": _dedup(timed_out),
        "not_run_tests": _dedup(not_run),
        "unfinished_tests": _dedup(unfinished),
        "assertion_signals": _dedup(assertion_signals),
        "infra_signals": infra_signals,
        "totals": totals,
        "ctest_started": bool(started),
        "build_failure_hint": bool(BUILD_FAILURE_RE.search(text)),
    }


def parse_junit(path):
    """Nombres de tests con <failure>/<error> en el JUnit de CTest."""
    failed, available = [], False
    try:
        root = ET.parse(path).getroot()
        available = True
        for tc in root.iter("testcase"):
            if tc.find("failure") is not None or tc.find("error") is not None:
                name = tc.get("name") or tc.get("classname") or "?"
                failed.append(name)
    except (ET.ParseError, OSError):
        available = False
    return _dedup(failed), available


def repro_command(names, what="failed"):
    """Comando exacto de reproduccion para unos tests dados."""
    if not names:
        return f"bash scripts/run_ctest.sh  # re-corrida completa de la puerta"
    if len(names) <= MAX_REPRO_NAMES:
        alt = _regex_alt(names)
        return (
            f"ctest --test-dir {CTEST_DIR} -R '^({alt})$' "
            f"--output-on-failure  # reproduce solo los {what}"
        )
    return (
        f"ctest --test-dir {CTEST_DIR} --rerun-failed --output-on-failure"
        f"  # reproduce solo los {what} ({len(names)} tests)"
    )


def classify(args):
    log_text = ""
    log_available = False
    if args.log:
        try:
            log_text = Path(args.log).read_text(errors="replace")
            log_available = True
        except OSError:
            log_available = False

    result = parse_log(log_text) if log_available else {
        "failed_tests": [], "timed_out_tests": [], "not_run_tests": [],
        "unfinished_tests": [], "assertion_signals": [], "infra_signals": [],
        "totals": None, "ctest_started": False, "build_failure_hint": False,
    }

    junit_failed, junit_available = ([], False)
    if args.junit:
        junit_failed, junit_available = parse_junit(args.junit)
    failed = _dedup(result["failed_tests"] + junit_failed)
    for name in junit_failed:
        if name not in result["failed_tests"]:
            result["assertion_signals"].append(f"junit: {name}")

    last_failed_exists = False
    if args.last_tests_failed:
        p = Path(args.last_tests_failed)
        last_failed_exists = p.is_file() and p.stat().st_size > 0

    job_timed_out = "job-timeout" in result["infra_signals"]
    cancelled = args.ctest_result == "cancelled"

    verdict = {
        "ctest_result": args.ctest_result,
        "log_available": log_available,
        "junit_available": junit_available,
        "job_timed_out": job_timed_out,
        "failed_tests": failed,
        "timed_out_tests": result["timed_out_tests"],
        "unfinished_tests": result["unfinished_tests"],
        "not_run_tests": result["not_run_tests"],
        "assertion_signals": _dedup(result["assertion_signals"]),
        "infra_signals": result["infra_signals"],
        "totals": result["totals"],
        "build_failure_hint": result["build_failure_hint"],
        "retry_mode": "none",
        "retry_tests": [],
        "retry_tests_truncated": False,
        "repro_command": "",
        "reason": "",
    }

    # Regla 1 — fallo de asercion: nunca se reintenta, gane lo que gane.
    if failed or result["assertion_signals"]:
        verdict["verdict"] = "no-retry-assertion"
        verdict["reason"] = (
            f"Fallo de asercion real en {len(failed)} test(s)"
            + (f" ({', '.join(failed[:5])})" if failed else "")
            + "; no se reintenta (C6: nunca un fallo de asercion)."
        )
        verdict["repro_command"] = repro_command(failed, "fallidos")
        return verdict

    # Regla 2 — test individual con (Timeout): sospechoso de deadlock.
    if result["timed_out_tests"]:
        verdict["verdict"] = "no-retry-deadlock-suspect"
        verdict["reason"] = (
            "Test(s) individual(es) con (Timeout): "
            + ", ".join(result["timed_out_tests"][:5])
            + ". Un test colgado puede ser un deadlock real, no lentitud "
              "del runner; no se reintenta sin revision humana."
        )
        verdict["repro_command"] = repro_command(
            result["timed_out_tests"], "con timeout"
        )
        return verdict

    # Regla 3 — timeout del job o infra explicita: reintento (max. 1/run).
    if job_timed_out or result["infra_signals"]:
        verdict["verdict"] = "retry"
        if job_timed_out:
            verdict["reason"] = (
                "El job supero su tiempo maximo de ejecucion "
                "(timeout del job, no de un test): la corrida quedo "
                "incompleta sin fallo de asercion."
            )
        else:
            verdict["reason"] = (
                "Error de infraestructura (lista explicita C6): "
                + ", ".join(result["infra_signals"])
                + ". Sin fallo de asercion."
            )
        if last_failed_exists:
            verdict["retry_mode"] = "rerun-failed"
            verdict["repro_command"] = (
                f"ctest --test-dir {CTEST_DIR} --rerun-failed --output-on-failure"
            )
        elif result["unfinished_tests"] or result["not_run_tests"]:
            names = _dedup(result["unfinished_tests"] + result["not_run_tests"])
            verdict["retry_mode"] = "by-name"
            verdict["retry_tests"] = names[:MAX_RETRY_NAMES]
            verdict["retry_tests_truncated"] = len(names) > MAX_RETRY_NAMES
            verdict["repro_command"] = repro_command(
                verdict["retry_tests"], "no corridos"
            )
        elif not result["ctest_started"]:
            verdict["retry_mode"] = "full"
            verdict["repro_command"] = (
                f"ctest --test-dir {CTEST_DIR} --output-on-failure"
                "  # ctest nunca llego a arrancar: todo esta no corrido"
            )
        else:
            verdict["retry_mode"] = "none"
            verdict["reason"] += (
                " CTest termino entero: el timeout cayo fuera de CTest "
                "(p. ej. en los chequeos Fase 0); no hay nada que reintentar."
            )
            verdict["repro_command"] = "bash scripts/run_ctest.sh"
        return verdict

    # Regla 4 — cualquier otro caso: escalar con el diagnostico.
    verdict["verdict"] = "no-retry-unknown"
    if not log_available:
        verdict["reason"] = (
            "Artefacto C5 (ctest_last.log) no disponible: no se puede "
            "clasificar sin evidencia. No se reintenta a ciegas."
        )
    elif cancelled:
        verdict["reason"] = (
            "Job cancelado sin senal de timeout del job: cancelacion "
            "manual o por concurrencia (un push nuevo cancela la corrida "
            "vieja). No se reintenta una corrida sustituida."
        )
    elif result["totals"] and result["totals"]["failed_count"] == 0:
        verdict["reason"] = (
            "CTest paso (0 tests fallidos) pero el job fallo en otro paso "
            "(p. ej. un chequeo Fase 0 o la configuracion). No es un fallo "
            "de test: no hay nada que reintentar en CTest."
        )
    elif result["build_failure_hint"]:
        verdict["reason"] = (
            "El log apunta a un fallo de compilacion/configuracion "
            "(CMake/make), no a un fallo de test. No se reintenta."
        )
    else:
        verdict["reason"] = (
            "Fallo sin evidencia clasificable (ni asercion, ni timeout de "
            "test, ni timeout del job, ni infra listada). Ante duda, no "
            "se reintenta: lo revisa un humano."
        )
    verdict["repro_command"] = "bash scripts/run_ctest.sh"
    return verdict


def render(args):
    verdict = json.loads(Path(args.verdict).read_text())
    labels = {
        "retry": "REINTENTO (timeout del job / infra)",
        "no-retry-assertion": "SIN REINTENTO — fallo de asercion",
        "no-retry-deadlock-suspect": "SIN REINTENTO — timeout individual (sospechoso de deadlock)",
        "no-retry-unknown": "SIN REINTENTO — sin clasificar (escala a humano)",
    }
    lines = []
    title = "Triaje CI (C6)"
    if args.retry_of:
        title += " — tras el reintento"
    lines.append(f"## {title}")
    lines.append("")
    lines.append(f"**Veredicto:** {labels.get(verdict['verdict'], verdict['verdict'])}")
    lines.append(f"**Job ctest:** `{verdict['ctest_result']}` · **SHA:** `{args.sha}`")
    lines.append(f"**Run:** {args.run_url}")
    lines.append("")
    lines.append(f"**Motivo:** {verdict['reason']}")
    lines.append("")

    def _list(header, names):
        if not names:
            return
        lines.append(header)
        for n in names:
            lines.append(f"- `{n}`")
        lines.append("")

    _list(f"### Tests con fallo de asercion ({len(verdict['failed_tests'])})",
          verdict["failed_tests"])
    _list(f"### Tests con timeout individual — sospechosos de deadlock "
          f"({len(verdict['timed_out_tests'])})", verdict["timed_out_tests"])
    _list(f"### Tests empezados sin terminar ({len(verdict['unfinished_tests'])})",
          verdict["unfinished_tests"])
    _list(f"### Tests no corridos ({len(verdict['not_run_tests'])})",
          verdict["not_run_tests"])
    if verdict["assertion_signals"]:
        lines.append("### Senales de asercion / sanitizer")
        for s in verdict["assertion_signals"][:10]:
            lines.append(f"- {s}")
        lines.append("")
    if verdict["infra_signals"]:
        lines.append("### Senales de infra")
        for s in verdict["infra_signals"]:
            lines.append(f"- `{s}`")
        lines.append("")
    if verdict.get("totals"):
        t = verdict["totals"]
        lines.append(
            f"### Resumen CTest: {t['percent_passed']}% pasados, "
            f"{t['failed_count']} fallidos de {t['total']}"
        )
        lines.append("")
    lines.append("### Reproduccion exacta")
    lines.append("```sh")
    lines.append(verdict["repro_command"])
    lines.append("```")
    lines.append("")
    if verdict["verdict"] == "retry":
        mode = verdict["retry_mode"]
        lines.append(
            f"La CI reintenta sola solo lo no corrido/fallado (modo `{mode}`), "
            f"maximo 1 reintento por run. Si el reintento vuelve a fallar con "
            f"asercion, se publica un segundo comentario y no se reintenta mas."
        )
    else:
        lines.append(
            "La CI NO reintenta este fallo (C6: nunca un fallo de asercion, "
            "ni un timeout individual sospechoso de deadlock, ni un fallo "
            "sin clasificar). Lo revisa un humano con el comando de arriba."
        )
    lines.append("")
    lines.append(
        "<!-- ci-triage C6 — generado por scripts/ci_triage.py (solo stdlib); "
        "el PR en borrador ci-autofix sigue DESACTIVADO: falta la "
        "confirmacion de Esse -->"
    )
    print("\n".join(lines))


def main(argv=None):
    parser = argparse.ArgumentParser(prog="ci_triage.py")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("classify", help="clasifica un log CTest y emite el veredicto JSON")
    p.add_argument("--log", help="ctest_last.log del artefacto C5")
    p.add_argument("--junit", help="ctest_junit.xml del artefacto C5")
    p.add_argument("--last-tests-failed", help="LastTestsFailed.log del artefacto C5")
    p.add_argument("--ctest-result", required=True,
                   choices=["failure", "cancelled", "success", "skipped", "timed_out"])
    p.add_argument("--out", help="escribe el JSON del veredicto en este archivo")
    p.add_argument("--github-output", help="ruta de $GITHUB_OUTPUT (anexa verdict=/retry_mode=)")
    p.set_defaults(func="classify")

    p = sub.add_parser("render", help="renderiza el comentario Markdown del veredicto")
    p.add_argument("--verdict", required=True, help="JSON emitido por classify")
    p.add_argument("--run-url", required=True)
    p.add_argument("--sha", required=True)
    p.add_argument("--retry-of", action="store_true",
                   help="el comentario es del reintento, no de la corrida original")
    p.set_defaults(func="render")

    args = parser.parse_args(argv)
    if args.func == "classify":
        verdict = classify(args)
        text = json.dumps(verdict, indent=2, ensure_ascii=False)
        print(text)
        if args.out:
            Path(args.out).write_text(text + "\n", encoding="utf-8")
        if args.github_output:
            with open(args.github_output, "a", encoding="utf-8") as fh:
                fh.write(f"verdict={verdict['verdict']}\n")
                fh.write(f"retry_mode={verdict['retry_mode']}\n")
        return 0
    render(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
