#!/usr/bin/env python3
"""
Motor de Auto-Evolución en Código de IVANNA — MVP (2026-11-09).

Ciclo completo:
  1. ESTADO     lee el repo (git log/status, nº de tests, cobertura por módulo).
  2. CONOCIMIENTO construye tools/evolution/knowledge/knowledge.json:
     índice del codebase, patrón SAF_GUARD de referencia, candidatos.
  3. DETECTAR   reglas deterministas (sin LLM): funciones SAF sin guardas,
     funciones muertas (definidas, nunca llamadas), módulos sin test,
     chequeos host de Fase 0 en rojo (scripts/check_*.py, 0003) y
     TODO/FIXME en el C++ nativo (0003; --scan ~30 s con los 6 chequeos).
  4. PROPONER   emite la propuesta (parche + justificación) para el candidato.
  5. VALIDAR    valida el parche en un git worktree desechable (scratch):
     compila el/los tests, corre la suite host completa (o las auto-pruebas
     del motor si el parche no toca C++), registra el resultado y ELIMINA
     el scratch. El árbol real nunca se toca. El entregable es el diff +
     validation.json + REPORT.md, no el cambio aplicado.
  6. REPORTE    tools/evolution/proposals/<id>/REPORT.md + validation.json.

Dónde vive el motor (decisión de arquitectura, ver README.md):
  HÍBRIDO. El teléfono solo recolecta telemetría/logs y propone vía el
  daemon; el motor que ESCRIBE código vive en la VM/servidor, donde está
  el repo, el compilador host y la CI. La app nunca reescribe su fuente.

Generación de propuestas (2026-11-14, motor completo):
  La detección es determinista y la REDACCIÓN del parche la hace el LLM
  vía OmniRoute (tools/evolution/llm_client.py, modelo "ivanna-free",
  key solo en ~/.omniroute/ivanna-app-key). El LLM recibe solo código
  relevante + diagnóstico; nunca credenciales ni secretos. La validación
  ocurre en un git worktree desechable (scratch): el árbol real nunca se
  toca. Ver tools/evolution/PIPELINE.md.

Uso:
  python3 tools/evolution/evolution_engine.py --scan
      Solo fases 1-3: estado + conocimiento + detección (no toca el árbol).
  python3 tools/evolution/evolution_engine.py --proposal 0001 --validate
      Ciclo completo para la propuesta 0001 (valida en scratch, no aplica).
  python3 tools/evolution/evolution_engine.py --draft 0002 --finding '{...}'
      El LLM redacta el parche para el hallazgo JSON (sin validar).
  python3 tools/evolution/evolution_engine.py --evolve 0002 --finding '{...}'
      Detectar → reescribir (LLM) → verificar en scratch → REPORT.md.
      No aplica al árbol, no commitea, no publica.
  python3 tools/evolution/evolution_engine.py --apply 0002
      Aplica una propuesta VERDE aprobada por el consejo al árbol real
      (ver tools/evolution/apply_proposal.py): re-valida en scratch
      contra el árbol actual, aplica, verifica los tests afectados y
      deja el cambio listo para commit. NUNCA commitea ni publica.
      La elevación por encima de la élite (regla de oro de Esse
      2026-11-15) la valora el operador, no la máquina.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
EVO = REPO / "tools" / "evolution"
KNOWLEDGE = EVO / "knowledge" / "knowledge.json"
PROPOSALS = EVO / "proposals"

# Archivos que el motor NUNCA toca (gate de seguridad).
FORBIDDEN = (
    "app/build.gradle.kts",
    "app/src/main/cpp/CMakeLists.txt",
    "app/src/main/java/com/ivanna/omega/security/",
    ".github/workflows/",
    "tools/security/",
)

# Archivos que el LLM NUNCA redacta en un draft (además de FORBIDDEN).
# - Los archivos de versión los mueve SOLO el publish de
#   tools/autorepair/run_fix.py (bump + CHANGELOG); un draft no los toca.
# - app/src/main/java/com/ivanna/omega/audio/ es el área del doctor runtime
#   que construye el agente principal (2026-11-14): el motor no la toca.
DRAFT_FORBIDDEN = FORBIDDEN + (
    "app/src/main/java/com/ivanna/omega/audio/",
    "version.properties",
    "magisk_module/module.prop",
    "magisk_module/update.json",
    "CHANGELOG.md",
    "README.md",
)

FORBIDDEN_SUFFIX = (".keystore", ".jks", ".pem", ".key")


# Límites duros de un parche (contención: un draft no puede reescribir
# medio producto de una tacada; un parche gigante es sospechoso o un
# error del LLM, y se revisa en trozos).
MAX_PATCH_FILES = 50
MAX_PATCH_BYTES = 512 * 1024


def patch_touched_files(patch_text: str) -> list:
    """Archivos que un unified diff toca, INCLUIDOS los borrados.

    Solo `+++ b/` manda, salvo borrado (`+++ /dev/null`), donde la ruta
    viva es la de `--- a/`. Sin esto, un parche que BORRA un archivo
    prohibido (p. ej. app/build.gradle.kts) pasaba el Gate 1: su línea
    `+++` es `/dev/null` y el borrado nunca se revisaba (hallazgo de la
    auditoría 2026-11-14, verificado en test_patch_gates.py). Misma
    lógica que github_bridge.ContainmentGuard.patch_files (gemela: si
    cambia una, cambiar la otra).
    """
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


def check_patch_limits(patch_text: str) -> list:
    """Límites de tamaño/nº de archivos de un parche. Lanza SystemExit.

    Devuelve los archivos tocados (para que el llamador no los
    re-parsee). Fail-closed: un parche fuera de límites no se valida
    ni se publica; se trocea y se revisa por partes.
    """
    n_bytes = len(patch_text.encode("utf-8"))
    if n_bytes > MAX_PATCH_BYTES:
        raise SystemExit(
            f"GATE: parche de {n_bytes} bytes (máx. {MAX_PATCH_BYTES}); "
            f"trocear la reescritura")
    files = patch_touched_files(patch_text)
    if len(files) > MAX_PATCH_FILES:
        raise SystemExit(
            f"GATE: parche toca {len(files)} archivos "
            f"(máx. {MAX_PATCH_FILES}); trocear la reescritura")
    return files


_HUNK_HEADER_RE = re.compile(
    r"^@@(?: -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))?)?(?: @@)?$")


def normalize_patch_hunks(patch_text: str, base_dir) -> str | None:
    """Repara hunks `@@` sin números de línea (los que emite a veces el
    proposer del consejo).

    `git apply` exige `@@ -a,b +c,d @@`; un hunk pelado es "garbage" y la
    validación lo rechaza aunque el consejo haya aprobado el parche
    (medido en el ensayo 0006-rehearsal-seeded: consejo APROBADO en 1
    ronda, validación ROJO por `patch with only garbage at line 4`).
    Aquí se resuelven los números por coincidencia EXACTA del contexto
    (líneas ` ` + `-`) en el archivo actual — la misma regla que
    llm_client._apply_patch_to_unified — y se reescribe la cabecera.
    Los hunks que ya traen números se dejan tal cual. Devuelve el parche
    normalizado, o None si algún hunk no se puede ubicar (0 o >1
    coincidencias, archivo inexistente, archivo nuevo sin `+`): sin
    adivinar, fail-closed.
    """
    lines = patch_text.splitlines(keepends=True)
    out = []
    i = 0
    current_file = None
    current_old = None  # líneas actuales del archivo (se actualizan hunk a hunk)
    while i < len(lines):
        line = lines[i]
        m = re.match(r"^diff --git a/(.*) b/(.*)$", line.rstrip("\n"))
        if m:
            current_file = m.group(2)
            current_old = None
            out.append(line)
            i += 1
            continue
        if line.startswith("--- /dev/null"):
            out.append(line)
            i += 1
            continue
        if line.startswith("--- a/") and current_file is not None:
            out.append(line)
            i += 1
            continue
        if line.startswith("+++ b/") and current_file is not None:
            out.append(line)
            i += 1
            continue
        hm = _HUNK_HEADER_RE.match(line.rstrip("\n"))
        if hm is not None and current_file is not None:
            # Recoger el cuerpo del hunk (líneas ' ', '+', '-', '\').
            j = i + 1
            body = []
            while j < len(lines) and lines[j][:1] in (" ", "+", "-", "\\"):
                body.append(lines[j])
                j += 1
            old_seq = [h[1:].rstrip("\n") for h in body if h.startswith((" ", "-"))]
            new_seq = [h[1:].rstrip("\n") for h in body if h.startswith((" ", "+"))]
            if hm.group(1) is not None:
                out.append(line)  # ya trae números: no tocar
                out.extend(body)
                if current_old is not None and old_seq:
                    hits = [k for k in range(len(current_old) - len(old_seq) + 1)
                            if current_old[k:k + len(old_seq)] == old_seq]
                    if len(hits) == 1:
                        s0 = hits[0]
                        current_old = (current_old[:s0] + new_seq
                                       + current_old[s0 + len(old_seq):])
                i = j
                continue
            # Hunk pelado: ubicar el contexto en el archivo actual.
            if current_old is None:
                fpath = Path(base_dir) / current_file
                if not fpath.exists():
                    return None
                current_old = fpath.read_text(errors="ignore").splitlines()
            if not old_seq and not new_seq:
                return None
            if not old_seq:
                # Solo inserciones sin contexto: no hay dónde anclarlas.
                return None
            hits = [k for k in range(len(current_old) - len(old_seq) + 1)
                    if current_old[k:k + len(old_seq)] == old_seq]
            if len(hits) != 1:
                return None
            s0 = hits[0]
            out.append(f"@@ -{s0 + 1},{len(old_seq)} +{s0 + 1},{len(new_seq)} @@\n")
            out.extend(body)
            current_old = (current_old[:s0] + new_seq
                           + current_old[s0 + len(old_seq):])
            i = j
            continue
        out.append(line)
        i += 1
    return "".join(out)


def run(cmd, cwd=REPO, check=False, timeout=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       shell=isinstance(cmd, str), timeout=timeout)
    if check and r.returncode != 0:
        raise RuntimeError(f"cmd failed: {cmd}\n{r.stdout}\n{r.stderr}")
    return r


def is_forbidden(rel: str) -> bool:
    return (rel.startswith(FORBIDDEN)
            or rel.endswith(FORBIDDEN_SUFFIX)
            or "SECRET" in rel.upper()
            or "PAT" in rel)


def _strip_cmake_comments(text: str) -> str:
    """Quita comentarios `#` de CMake sin tocar el contenido entre comillas.

    Basta para los escaneos por regex: un `#` dentro de comillas no es
    comentario y un `#` fuera corta la línea.
    """
    out = []
    for line in text.splitlines():
        buf = []
        in_quote = False
        escaped = False
        for ch in line:
            if escaped:
                buf.append(ch)
                escaped = False
                continue
            if ch == "\\":
                buf.append(ch)
                escaped = True
                continue
            if ch == '"':
                in_quote = not in_quote
                buf.append(ch)
                continue
            if ch == "#" and not in_quote:
                break
            buf.append(ch)
        out.append("".join(buf))
    return "\n".join(out)


def _count_ctest_targets(cmake_text: str) -> int:
    """Cuenta los targets CTest registrados en tests/CMakeLists.txt.

    El conteo viejo (`add_test\\(NAME`) daba 36 cuando el total real es
    ~104: los tests se registran sobre todo con el helper
    `ivanna_add_test(name)` (que usa `gtest_discover_tests`), no con
    `add_test(NAME ...)` directo. Se cuentan los nombres únicos de:
      - invocaciones `ivanna_add_test(<nombre>)` (sin la definición de la
        función: la línea `function(ivanna_add_test name)` casa el regex
        con el nombre `name` y hay que excluirla explícitamente);
      - `add_test(NAME <nombre> ...)` directos.
    Los comentarios se quitan antes (una línea `# ivanna_add_test(fake)`
    no registra nada).
    """
    text = _strip_cmake_comments(cmake_text)
    names = set()
    for m in re.finditer(r"\bivanna_add_test\s*\(\s*([A-Za-z0-9_./:-]+)", text):
        line_start = text.rfind("\n", 0, m.start()) + 1
        line = text[line_start:text.find("\n", m.start())]
        if line.strip().startswith("function("):
            continue  # la definición del helper, no un registro
        names.add(m.group(1))
    for m in re.finditer(r"\badd_test\s*\(\s*NAME\s+([A-Za-z0-9_./:-]+)",
                         text, flags=re.IGNORECASE):
        names.add(m.group(1))
    return len(names)


# ── Fase 1: estado ──────────────────────────────────────────────────────────
def repo_state():
    log = run(["git", "log", "--oneline", "-5"], check=True).stdout.strip()
    status = run(["git", "status", "--short"]).stdout.strip()
    cmake_text = (REPO / "app/src/main/cpp/tests/CMakeLists.txt").read_text(errors="ignore")
    n_tests = _count_ctest_targets(cmake_text)
    return {"head": log.splitlines(), "dirty": status.splitlines(), "ctest_targets": n_tests}


# ── Fase 2+3: conocimiento + detección ──────────────────────────────────────
# --- Detector de guardas SAF por DEFINICIÓN (0007, 2026-11-16) ---------------
# Una "guarda" es una FUNCIÓN cuya definición tiene `std::isfinite` en el
# cuerpo. No un substring en el archivo: el nombre de la guarda
# (`isFiniteLatent`) no contiene el literal `isfinite` y un substring
# tampoco distingue una guarda real de una mención en un comentario.
# El extractor usa brace-matching (no regex de firmas) para soportar
# firmas multilínea, inline/static/constexpr, noexcept, trailing return
# y llaves anidadas en el cuerpo. La resolución de #include es transitiva
# con normalización de paths. Sin fallback legacy de substring.
_CPP_KEYWORDS = frozenset({
    "if", "for", "while", "switch", "catch", "return", "sizeof",
    "decltype", "else", "do", "case", "default", "try",
})


def _strip_cpp_noise(text):
    """Quita comentarios y literales de cadena/caracter, reemplazándolos
    por espacios (preserva saltos de línea y longitud) para que el
    extractor no vea llaves ni paréntesis falsos."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            if j == -1:
                j = n
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            if j == -1:
                j = n
            else:
                j += 2
            seg = text[i:j]
            out.append("".join("\n" if ch == "\n" else " " for ch in seg))
            i = j
        elif c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    j += 1
                    break
                j += 1
            seg = text[i:j]
            out.append("".join("\n" if ch == "\n" else " " for ch in seg))
            i = j
        elif c == "'":
            # Separador de dígitos C++14 (1'000) no es un literal.
            if i > 0 and (text[i - 1].isdigit() or text[i - 1] == "_"):
                out.append(c)
                i += 1
                continue
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    j += 1
                    break
                j += 1
            seg = text[i:j]
            out.append("".join("\n" if ch == "\n" else " " for ch in seg))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _matching_paren(text, open_idx):
    """Índice del ')' que cierra el '(' en open_idx, o -1."""
    depth = 0
    i, n = open_idx, len(text)
    while i < n:
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _matching_brace(text, open_idx):
    """Índice del '}' que cierra el '{' en open_idx, o -1."""
    depth = 0
    i, n = open_idx, len(text)
    while i < n:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


_NAME_PAREN_RE = re.compile(r"([A-Za-z_~][\w:]*)\s*\(")
_WORD_RE = re.compile(r"[A-Za-z_~][\w:]*")
_POST_QUALIFIERS = ("const", "noexcept", "override", "final")


def _skip_post_param_qualifiers(text, j):
    """Tras el ')' de los parámetros, avanza hasta el '{' del cuerpo.
    Salta const, noexcept(...), override/final, ref-qualifiers (&/&&),
    trailing return type (-> ...) y lista de inicializadores (: ...).
    Devuelve el índice del '{' o -1 si no hay cuerpo."""
    n = len(text)
    while True:
        while j < n and text[j].isspace():
            j += 1
        if j >= n:
            return -1
        matched = False
        for qual in _POST_QUALIFIERS:
            if text.startswith(qual, j):
                after = j + len(qual)
                if after < n and (text[after].isalnum() or text[after] == "_"):
                    continue
                j = after
                matched = True
                if qual == "noexcept":
                    while j < n and text[j].isspace():
                        j += 1
                    if j < n and text[j] == "(":
                        j = _matching_paren(text, j)
                        if j == -1:
                            return -1
                        j += 1
                break
        if matched:
            continue
        if text.startswith("&&", j) or text[j] == "&":
            j += 2 if text.startswith("&&", j) else 1
            continue
        if text.startswith("->", j):
            j += 2
            while j < n and text[j] not in "{;":
                j += 1
            if j >= n or text[j] != "{":
                return -1
            return j
        if text[j] == ":":
            j += 1
            while True:
                while j < n and text[j].isspace():
                    j += 1
                if j >= n:
                    return -1
                if text[j] == "{":
                    return j
                m = _WORD_RE.match(text, j)
                if not m:
                    return -1
                j = m.end()
                while j < n and text[j].isspace():
                    j += 1
                if j < n and text[j] == "(":
                    j = _matching_paren(text, j)
                    if j == -1:
                        return -1
                    j += 1
                elif j < n and text[j] == "{":
                    j = _matching_brace(text, j)
                    if j == -1:
                        return -1
                    j += 1
                while j < n and text[j].isspace():
                    j += 1
                if j < n and text[j] == ",":
                    j += 1
                    continue
                if j < n and text[j] == "{":
                    return j
                return -1
        if text[j] == "{":
            return j
        return -1


def _extract_function_definitions(text):
    """Extrae definiciones de función C++ con brace-matching.

    Devuelve una lista de dicts {name, body, start, end}. Maneja firmas
    multilínea, inline/static/constexpr, noexcept, trailing return type,
    ref-qualifiers, listas de inicializadores y llaves anidadas.
    Las llamadas (sin '{' tras los parámetros y calificadores) y las
    palabras de control (if/for/while/...) no son definiciones.
    """
    clean = _strip_cpp_noise(text)
    defs = []
    for m in _NAME_PAREN_RE.finditer(clean):
        name = m.group(1)
        last = name.split("::")[-1]
        if last in _CPP_KEYWORDS:
            continue
        open_paren = m.end() - 1
        close_paren = _matching_paren(clean, open_paren)
        if close_paren == -1:
            continue
        brace = _skip_post_param_qualifiers(clean, close_paren + 1)
        if brace == -1:
            continue
        close_brace = _matching_brace(clean, brace)
        if close_brace == -1:
            continue
        defs.append({
            "name": name,
            "body": clean[brace + 1:close_brace],
            "start": m.start(),
            "end": close_brace + 1,
        })
    return defs


_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*(?P<delim>[<"])(?P<name>[^>"]+)[>"]', re.MULTILINE)


def _resolve_include_target(name, quoted, including_file, cpp_root):
    """Resuelve un #include a un archivo existente, con normalización.
    "..." se busca relativo al archivo que incluye, luego en cpp_root y
    cpp_root/include. <...> solo en cpp_root y cpp_root/include.
    Devuelve un Path o None (p. ej. headers del sistema como <cmath>)."""
    bases = []
    if quoted:
        bases.append(including_file.parent)
    bases.append(cpp_root)
    bases.append(cpp_root / "include")
    for base in bases:
        cand = Path(os.path.normpath(str(base / name)))
        if cand.is_file():
            return cand
    return None


def _resolve_includes(entry, cpp_root):
    """Resolución transitiva de #include con normalización de paths.
    Devuelve el conjunto de archivos alcanzados (sin el entry).
    Evita ciclos con un conjunto de visitados."""
    reached = set()
    queue = [Path(entry)]
    seen = set()
    while queue:
        cur = queue.pop(0)
        if cur in seen:
            continue
        seen.add(cur)
        try:
            text = cur.read_text(errors="ignore")
        except OSError:
            continue
        for m in _INCLUDE_RE.finditer(text):
            target = _resolve_include_target(
                m.group("name"), m.group("delim") == '"', cur, cpp_root)
            if target is None or target in reached:
                continue
            reached.add(target)
            queue.append(target)
    reached.discard(Path(entry))
    return reached


def _is_guard_function(defn):
    """Una guarda es una función cuya DEFINICIÓN contiene std::isfinite
    en el cuerpo (semántica por definición, 0007)."""
    return "std::isfinite" in defn["body"]


def scan_saf_guards():
    """Detecta módulos SAF sin guarda isfinite con trazabilidad de origen.

    Guarda por DEFINICIÓN (0007, 2026-11-16): una guarda es una función
    cuyo cuerpo contiene `std::isfinite`, identificada con el extractor
    de definiciones por brace-matching (no por substring del archivo).
    Un módulo está guardado si una guarda está definida en:
      1) el propio .cpp (guard_source "cpp"), o
      2) cualquier archivo de su grafo de #include transitivo, con
         normalización de paths (guard_source "include"), o
      3) un header hermano (include/<base>.h, <base>.h, <base>.hpp)
         (guard_source "header", regla 3 histórica).
    Sin fallback legacy de substring "isfinite": el nombre de la guarda
    (`isFiniteLatent`) no contiene ese literal y un substring tampoco
    distingue una guarda real de una mención en un comentario.
    Cada hit conserva "file", "functions", "has_isfinite_guard" (False),
    "guard_source" ("none") y "header_checked" (primer header hermano
    existente revisado, o null).
    """
    hits = []
    cpp_root = REPO / "app/src/main/cpp"
    func_re = re.compile(r"(?:double|float|void|int)\s+(\w+)\s*\(")
    for path in sorted(cpp_root.rglob("saf_*.cpp")):
        src = path.read_text(errors="ignore")
        funcs = []
        seen = set()
        for name in func_re.findall(src):
            if name not in seen:
                seen.add(name)
                funcs.append(name)
        if not funcs:
            continue

        # 1) Guarda definida en el propio .cpp.
        if any(_is_guard_function(d)
               for d in _extract_function_definitions(src)):
            continue
        # 2) Guarda en el grafo de includes (transitivo, normalizado).
        guard_in_include = None
        for inc_path in sorted(_resolve_includes(path, cpp_root)):
            try:
                inc_text = inc_path.read_text(errors="ignore")
            except OSError:
                continue
            if any(_is_guard_function(d)
                   for d in _extract_function_definitions(inc_text)):
                guard_in_include = inc_path
                break
        if guard_in_include is not None:
            continue
        # 3) Guarda en header hermano (regla 3 histórica, por definición).
        base = path.stem
        header_candidates = [
            cpp_root / "include" / f"{base}.h",
            path.with_suffix(".h"),
            path.with_suffix(".hpp"),
        ]
        header_checked = None
        guarded_by_header = False
        for header in header_candidates:
            if not header.exists():
                continue
            header_checked = str(header.relative_to(REPO))
            try:
                h_text = header.read_text(errors="ignore")
            except OSError:
                continue
            if any(_is_guard_function(d)
                   for d in _extract_function_definitions(h_text)):
                guarded_by_header = True
                break
        if guarded_by_header:
            continue

        hits.append({
            "file": str(path.relative_to(REPO)),
            "functions": funcs,
            "has_isfinite_guard": False,
            "guard_source": "none",
            "header_checked": header_checked,
        })
    return hits


# Funciones muertas que se CONSERVAN a propósito (nunca borrar código).
# Cada entrada documenta por qué no se borra y por qué el scanner no
# debe reportarla. Si la razón deja de aplicar, se quita de aquí y el
# scanner vuelve a exigirla.
DEAD_FUNCTION_ALLOWLIST = {
    # SAF_IsModelLoaded (saf_runtime.cpp): sin llamadores — el estado
    # del modelo SAF ya se reporta en Kotlin vía safAssetAvailable
    # (FotoHoloTelemetryBridge) y el motor degrada sin modelo. Se
    # conserva por si un diagnóstico en vivo lo necesita (0012).
    "SAF_IsModelLoaded",
}


def _source_files_for_caller_search():
    """Archivos C/C++ del repo para buscar llamadores (árbol de trabajo,
    no solo git: un archivo nuevo sin commitear también tiene llamadores).
    Excluye third_party (googletest vendoreado) y directorios de build.
    """
    out = []
    for ext in ("*.cpp", "*.h", "*.hpp", "*.c", "*.cc"):
        out.extend((REPO / "app/src/main/cpp").rglob(ext))
    return sorted(
        p for p in out
        if "third_party" not in p.parts and "build" not in p.parts
    )


def _has_caller(name, def_path, def_line, file_lines):
    """True si `name` aparece en algún archivo en una línea que no sea la
    propia definición (0012 + 0019: búsqueda en el árbol de trabajo,
    no `git grep`, que no ve archivos sin commitear)."""
    word = re.compile(r"\b" + re.escape(name) + r"\b")
    for path, lines in file_lines.items():
        for lineno, line in enumerate(lines, start=1):
            if path == def_path and lineno == def_line:
                continue  # la propia definición
            if word.search(line):
                return True
    return False


def scan_dead_functions():
    """Funciones definidas en .cpp sin llamadores en todo el repo.

    Un llamador es cualquier mención del nombre que NO sea la propia
    definición (0012): antes se excluía el archivo definidor completo,
    así que los helpers de test llamados en su propio archivo
    (DriveEta, Pearson, RmsDiffDb, RunOrbe, ...) salían como muertos
    aunque sí se usan. La definición se identifica por su línea
    (la del nombre de la función); cualquier otra mención, en el mismo
    archivo o en otro, es un llamador.
    """
    # Índice de líneas por archivo (se lee una vez; la búsqueda de
    # llamadores es en memoria, sin depender del estado de git).
    file_lines = {}
    for path in _source_files_for_caller_search():
        try:
            file_lines[path.resolve()] = path.read_text(
                errors="ignore").splitlines()
        except OSError:
            continue
    dead = []
    for path in sorted((REPO / "app/src/main/cpp").rglob("*.cpp")):
        src = path.read_text(errors="ignore")
        for m in re.finditer(r"(?:^|\n)(?:double|float|void|int|bool)\s+([A-Z]\w+)\s*\(", src):
            name = m.group(1)
            if name in DEAD_FUNCTION_ALLOWLIST:
                continue
            def_line = src.count("\n", 0, m.start(1)) + 1
            if not _has_caller(name, path.resolve(), def_line, file_lines):
                dead.append({"function": name, "file": str(path.relative_to(REPO))})
    return dead


def scan_untested_modules():
    """Módulos .cpp de primer nivel sin referencia en tests/CMakeLists.txt.

    La referencia debe ser CÓDIGO (una fuente en add_executable o en la
    librería), no un comentario: antes un `#` que mencionaba el nombre
    (p. ej. IvannaFusionCore.cpp, comentado en la lista de la librería
    y citado en una explicación) lo daba por cableado aunque ningún
    target lo compila (0017). Se quitan los comentarios `#...` antes
    del substring, mismo patrón que check_header_wiring.
    """
    cmake_raw = (REPO / "app/src/main/cpp/tests/CMakeLists.txt").read_text()
    cmake = "\n".join(line.split("#", 1)[0] for line in cmake_raw.splitlines())
    out = []
    for path in sorted((REPO / "app/src/main/cpp").glob("*.cpp")):
        if path.name not in cmake:
            out.append(str(path.relative_to(REPO)))
    return out


def scan_host_checks_red():
    """Ejecuta los 6 chequeos de Fase 0 (scripts/check_*.py) y resume cuáles
    están en rojo. Evidencia medida 2026-11-14 (HEAD 8ac65b9d): los 6 fallan
    con exit 1 (~27 s en total: jni_wiring 12 hallazgos, kotlin_wrapper_wiring,
    header_wiring, rt_safety, docs_claims con README 978/978 vs telemetry
    974/974, datasets_deploy con 239 hallazgos SOFA). Antes de este detector
    el --scan era ciego a todos ellos.

    DETECTAR no es ARREGLAR: los fixes de scripts/ quedan fuera del ámbito
    del motor (los propone, no los aplica). Ningún chequeo puede tumbar el
    scan: timeout (120 s), script ausente o error se registran y se sigue.
    """
    checks = (
        "check_jni_wiring",
        "check_kotlin_wrapper_wiring",
        "check_header_wiring",
        "check_rt_safety",
        "check_docs_claims",
        "check_datasets_deploy",
    )
    results = []
    for name in checks:
        script = REPO / "scripts" / f"{name}.py"
        finding_lines = []
        exit_code = None
        ok = False
        error = None
        try:
            if not script.exists():
                raise FileNotFoundError(f"no existe: {script.name}")
            proc = run(["python3", str(script)], cwd=REPO, timeout=120)
            exit_code = proc.returncode
            ok = exit_code == 0
            for line in (proc.stdout or "").splitlines() + (proc.stderr or "").splitlines():
                # Hallazgos: "  - ..." en casi todos; datasets_deploy usa
                # "  [FAIL] ..." (si solo se contaran los "  - ", sus 239
                # hallazgos contarían como 0).
                if line.startswith("  -") or line.startswith("  [FAIL]"):
                    finding_lines.append(line)
            if not ok and not finding_lines:
                error = ((proc.stderr or proc.stdout or f"exit {exit_code}")
                         .strip().splitlines()[0][:120])
        except Exception as exc:  # noqa: BLE001 - ningún chequeo tumba el scan
            error = str(exc).strip().splitlines()[0][:120] if str(exc).strip() else "error"
            exit_code = None
            ok = False
        results.append({
            "check": name,
            "ok": ok,
            "exit_code": exit_code,
            "findings": len(finding_lines),
            "sample": finding_lines[:3],
            "error": error,
        })
    return results


def scan_todo_fixme():
    """Inventario TODO/FIXME en el C++ nativo (git grep, tope 50 entradas).

    El detector reporta; el analista decide (no filtra por antigüedad:
    un TODO viejo puede seguir siendo deuda real). Si git grep no encuentra
    nada (exit 1 de grep) el total es 0, no un error.

    Solo cuentan los MARCADORES DE TAREA (`TODO:` / `TODO(` / `FIXME:` /
    `FIXME(`). La palabra española TODO (= todo/all) en comentarios
    ("TODO el audio", "TODO apagado", "TODO cambio") NO es una tarea y
    antes inflaba el inventario a 28 cuando solo 1 era real (0019):
    TODO(route-arbiter-v2) en ivanna_omega_jni.cpp:1803. El resto eran
    español o referencias vagas ("ver TODO") o ya resueltos
    (usb_audio_pro_manager: "sustituye los dos TODO").
    """
    # third_party (googletest vendoreado) y nlohmann/json (vendoreado)
    # excluidos: sus TODOs no son deuda de IVANNA.
    # cwd explícito: el default de run() se liga en tiempo de definición
    # y no vería un REPO monkeado en tests (mismo bug que 0012).
    proc = run(["git", "grep", "-n", "-E", r"TODO[:\(]|FIXME[:\(]", "--",
                "app/src/main/cpp", ":!app/src/main/cpp/tests/third_party",
                ":!app/src/main/cpp/include/nlohmann"], cwd=REPO)
    pattern = re.compile(r"^(?P<file>.*?):(?P<line>\d+):(.*?\b(?P<tag>TODO|FIXME)[:\(].*)$")
    items = []
    for line in (proc.stdout or "").splitlines():
        m = pattern.match(line)
        if not m:
            continue
        items.append({
            "file": m.group("file"),
            "line": int(m.group("line")),
            "tag": m.group("tag"),
            "text": m.group(3).strip()[:120],
        })
    total = len(items)
    truncated = total > 50
    if truncated:
        items = items[:50]
    return {"items": items, "total": total, "truncated": truncated}


def build_knowledge():
    state = repo_state()
    knowledge = {
        "generated_by": "tools/evolution/evolution_engine.py --scan",
        "repo_state": state,
        "reference_pattern": {
            "name": "SAF_GUARD (2026-09-17)",
            "where": "app/src/main/cpp/include/saf_runtime.h :: SAFUpdate",
            "rules": [
                "guardar entradas no-finitas con std::isfinite (fail-closed)",
                "denominador degenerado (<=0 o no finito) -> paso cero seguro",
                "sanitizar parámetros ANTES de usarlos en el cálculo",
            ],
        },
        "detectors": {
            "saf_without_guards": scan_saf_guards(),
            "dead_functions": scan_dead_functions(),
            "untested_top_level_modules": scan_untested_modules(),
            "host_checks_red": scan_host_checks_red(),
            "todo_fixme": scan_todo_fixme(),
        },
    }
    KNOWLEDGE.parent.mkdir(parents=True, exist_ok=True)
    KNOWLEDGE.write_text(json.dumps(knowledge, indent=2, ensure_ascii=False))
    return knowledge


# ── Fase 5: validar una propuesta (MODO SCRATCH, 2026-11-14) ──────────────
# Reescritura redactada por el LLM vía OmniRoute (propuesta de motor):
# el validador anterior aplicaba el parche DIRECTO sobre el árbol de trabajo
# y luego revertía con `git checkout` / `unlink` — ese revert ya falló dos
# veces (ver comentarios de la versión anterior) y compilaba en el
# `build-tests` compartido del repo. Ahora NUNCA se toca el árbol real:
# todo ocurre en un `git worktree` desechable que se elimina al terminar.
# El entregable sigue siendo el diff + validation.json, no el cambio aplicado.
def validate_proposal(pid: str) -> dict:
    pdir = PROPOSALS / pid
    patch = next(pdir.glob("*.patch"), None)
    if patch is None:
        raise SystemExit(f"sin parche en {pdir}")
    patch_text = patch.read_text()
    # Gate 0: límites de tamaño/nº de archivos (contención).
    # Gate 1: el parche no debe tocar archivos prohibidos — INCLUIDOS
    # los borrados (patch_touched_files resuelve `--- a/` cuando el
    # `+++` es /dev/null; antes un borrado de un archivo prohibido
    # pasaba el gate porque su `+++` era "/dev/null").
    touched = check_patch_limits(patch_text)
    bad = [t for t in touched if is_forbidden(t)]
    if bad:
        raise SystemExit(f"GATE: la propuesta toca archivos prohibidos: {bad}")
    # Gate 2: secretos en el texto del parche (misma lista que
    # tools/autorepair/run_fix.py PATRONES_SECRETOS, sin el token del
    # puente, que no aplica a un diff). La key de OmniRoute nunca pasa
    # por aquí: el LLM solo recibe código + diagnóstico.
    secret_patterns = [
        re.compile(r"ghp_[A-Za-z0-9]{20,}"),
        re.compile(r"github_pat_[A-Za-z0-9_]{20,}"),
        re.compile(r"(?i)password\s*=\s*['\"][^'\"]{4,}"),
        re.compile(r"(?i)token\s*=\s*['\"][^'\"]{8,}"),
    ]
    secrets = [pat.pattern for pat in secret_patterns if pat.search(patch_text)]
    if secrets:
        raise SystemExit(f"GATE: la propuesta contiene secretos potenciales: {secrets}")
    # Foto del árbol antes: al final debe ser idéntica (nada tocado).
    repo_status_before = run(["git", "status", "--porcelain"]).stdout.strip()
    result = {"proposal": pid, "patch": patch.name, "files": touched}
    scratch = {"mode": "worktree", "removed": False, "worktree": None}
    result["scratch"] = scratch
    tmpdir = None
    wt = None
    try:
        tmpdir = tempfile.mkdtemp(prefix="ivanna-evo-", dir=str(REPO.parent))
        wt = Path(tmpdir) / "wt"
        scratch["worktree"] = str(wt)
        try:
            run(["git", "worktree", "add", "--detach", str(wt), "HEAD"], check=True)
            # Base pre-parche = el ÁRBOL ACTUAL, no HEAD pelado: el motor
            # tiene trabajo sin commitear (sus propios archivos están
            # modificados respecto a HEAD) y el parche se redactó contra
            # ese árbol. Se vuelca `git diff HEAD` encima del worktree.
            overlay = run(["git", "diff", "HEAD", "--binary"]).stdout
            if overlay.strip():
                overlay_patch = Path(tmpdir) / "dirty.patch"
                overlay_patch.write_text(overlay)
                run(["git", "apply", str(overlay_patch)], check=True, cwd=wt)
        except RuntimeError as e:
            result.update({"applied": False,
                           "error": f"no se pudo preparar el scratch: {e}"})
            return result
        # Archivos sin seguir del motor (tests nuevos, propuestas, estado):
        # el worktree no los tiene y `git diff HEAD` tampoco los cubre.
        # Se copian con `git status -uall` (son ~25 archivos, ~300 KB).
        # Sin esto, la validación en scratch no ve tools/evolution/tests/
        # y las auto-pruebas Python se saltan (medido en la 0003, 1ª pasada).
        # Archivos que el parche CREA (--- /dev/null): no se copian, el
        # parche los crea; copiarlos antes haría fallar el apply ("el
        # archivo ya existe"). Medido en la 0004. OJO: la línea '---'
        # lleva TAB + timestamp, así que NO se compara con string exacto.
        new_in_patch = set()
        _plines = patch_text.splitlines()
        for _i, _l in enumerate(_plines):
            if (_l.startswith("--- /dev/null") and _i + 1 < len(_plines)
                    and _plines[_i + 1].startswith("+++ b/")):
                new_in_patch.add(_plines[_i + 1][6:])
        untracked = run(["git", "status", "--porcelain", "-uall", "--",
                         "tools/evolution", "tools/autorepair"]).stdout
        for line in untracked.splitlines():
            if not line.startswith("??"):
                continue
            rel = line[3:].strip()
            if rel in new_in_patch:
                continue
            src = REPO / rel
            dst = wt / rel
            if src.is_dir():
                shutil.copytree(src, dst,
                                ignore=shutil.ignore_patterns("__pycache__"),
                                dirs_exist_ok=True)
            elif src.exists():
                dst.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, dst)
        # Base pre-parche para archivos NO seguidos: tools/evolution/ y
        # tools/autorepair/ son untracked en HEAD, así que el worktree no
        # los contiene; se copian del árbol real (contenido actual = base).
        for t in touched:
            if t in new_in_patch:
                continue  # lo crea el parche; copiar la versión del árbol
                # haría fallar el apply ("already exists in working
                # directory"). Medido en la 0004, 3ª pasada.
            wt_path = wt / t
            tracked = run(["git", "ls-files", "--", t]).stdout.strip()
            if tracked:
                continue
            src = REPO / t
            if src.exists():
                if src.is_dir():
                    result.update({"applied": False,
                                   "error": f"base desconocida: {t} es un directorio"})
                    return result
                wt_path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(src, wt_path)
            elif t not in new_in_patch:
                # No está en HEAD, no está en el árbol y el parche no lo
                # crea: no hay base conocida contra la que aplicar.
                result.update({"applied": False,
                               "error": f"base desconocida: {t} no existe ni en HEAD ni en el árbol"})
                return result
        # Gate 3: el parche aplica limpio DENTRO del scratch, no en el repo.
        r = run(["git", "apply", "--check", str(patch)], cwd=wt)
        if r.returncode != 0:
            result.update({"applied": False, "error": r.stderr})
            return result
        run(["git", "apply", str(patch)], check=True, cwd=wt)
        result["applied"] = True
        cmake = os.environ.get("IVANNA_CMAKE", str(Path.home() / ".local/bin/cmake"))
        test_bins = sorted(set(re.findall(r"add_executable\((\w+)", patch_text)))
        built, test_runs = [], {}
        cpp_touched = any(
            t.endswith((".cpp", ".c", ".h", ".hpp"))
            or t == "app/src/main/cpp/tests/CMakeLists.txt"
            for t in touched)
        if cpp_touched:
            # Parche C++: configurar + compilar + suite completa en scratch.
            run([cmake, "-S", "app/src/main/cpp/tests", "-B", "build-tests"],
                check=True, cwd=wt)
            for t in test_bins:
                b = run([cmake, "--build", "build-tests", "--target", t, "-j4"], cwd=wt)
                built.append({"target": t, "ok": b.returncode == 0,
                              "tail": (b.stdout + b.stderr)[-500:]})
                if b.returncode == 0:
                    tr = run([f"./{t}"], cwd=wt / "build-tests")
                    test_runs[t] = {"passed": tr.returncode == 0,
                                    "tail": tr.stdout[-500:]}
            run([cmake, "--build", "build-tests", "-j4"], check=True, cwd=wt)
            ct = run(["ctest", "--output-on-failure"], cwd=wt / "build-tests")
            result["full_suite"] = {"passed": ct.returncode == 0,
                                    "tail": ct.stdout[-800:]}
        else:
            # Parche sin C++: CTest no tiene nada que compilar; se verifican
            # las auto-pruebas del motor dentro del scratch y se deja
            # constancia explícita de que la suite C++ se saltó (y por qué).
            result["full_suite"] = {"skipped": True,
                                    "reason": "el parche no toca C++"}
            python_checks = {}
            tests_dir = wt / "tools" / "evolution" / "tests"
            if tests_dir.exists():
                ut = run(["python3", "-m", "unittest", "discover",
                          "-s", "tools/evolution/tests", "-v"], cwd=wt)
                python_checks["unittest"] = {
                    "passed": ut.returncode == 0,
                    "tail": (ut.stdout + ut.stderr)[-800:]}
            else:
                python_checks["unittest"] = {
                    "skipped": True, "reason": "no existe tools/evolution/tests"}
            scan = run(["python3", "tools/evolution/evolution_engine.py", "--scan"],
                       cwd=wt, timeout=300)
            python_checks["scan"] = {
                "passed": scan.returncode == 0,
                "tail": (scan.stdout + scan.stderr)[-800:],
                "knowledge_json": (wt / "tools" / "evolution" / "knowledge"
                                   / "knowledge.json").exists(),
            }
            result["python_checks"] = python_checks
        result["built"] = built
        result["new_tests"] = test_runs
    finally:
        # Eliminar el scratch SIEMPRE y verificar que el árbol quedó intacto.
        if wt is not None and wt.exists():
            try:
                run(["git", "worktree", "remove", "--force", str(wt)], check=True)
                scratch["removed"] = True
            except Exception:
                if tmpdir is not None:
                    shutil.rmtree(tmpdir, ignore_errors=True)
                scratch["removed"] = False
            finally:
                run(["git", "worktree", "prune"], check=False)
        # El padre tmpdir (con dirty.patch) no lo borra `worktree remove`:
        # sin esto cada validación deja un directorio huérfano junto al repo.
        if tmpdir is not None:
            shutil.rmtree(tmpdir, ignore_errors=True)
        repo_status_after = run(["git", "status", "--porcelain"]).stdout.strip()
        result["tree_untouched"] = (repo_status_before == repo_status_after)
        # El registro se escribe SIEMPRE (también en los retornos tempranos
        # de base desconocida o apply --check fallido): sin él, un fallo
        # de validación no dejaría evidencia.
        (pdir / "validation.json").write_text(
            json.dumps(result, indent=2, ensure_ascii=False))
    return result


# ── Fase 4: redacción por LLM (OmniRoute) ───────────────────────────────────
# El LLM recibe SOLO código fuente relevante + diagnóstico (autorizado por
# Esse 2026-11-14). NUNCA credenciales ni secretos. La redacción no aplica
# nada: devuelve un diff que luego se valida en scratch (Fase 5).
import difflib


def _llm():
    """Import perezoso de llm_client (mismo directorio). Así --scan funciona
    aunque el gateway esté caído: solo --draft/--evolve necesitan el LLM."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ivanna_llm_client", EVO / "llm_client.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def is_draft_forbidden(rel: str) -> bool:
    return (rel.startswith(DRAFT_FORBIDDEN)
            or rel.endswith(FORBIDDEN_SUFFIX)
            or "SECRET" in rel.upper()
            or "PAT" in rel)


def _code_blocks_to_patch(text: str):
    """LEGACY (v2.5.445): Plan B del draft de un solo modelo.

    Desde la 445 la reescritura pasa por el consejo (council.py), que
    tiene su propio Plan C (_extract_plain_diff). Esta función ya no la
    usa draft_patch; se conserva por si un flujo futuro la necesita
    (regla: nunca borrar código, marcarlo legacy)."""
    import re as _re
    blocks = _re.findall(r"```[^\n]*\n(.*?)```", text, _re.S)
    files = {}
    for b in blocks:
        lines = b.strip("\n").splitlines()
        if not lines or not lines[0].startswith("# FILE:"):
            continue
        files[lines[0][len("# FILE:"):].strip()] = "\n".join(lines[1:]) + "\n"
    if not files:
        return None
    out = []
    for path, new in sorted(files.items()):
        old_path = REPO / path
        old = old_path.read_text(errors="ignore") if old_path.exists() else ""
        if old == new:
            continue
        fromfile = f"a/{path}" if old else "/dev/null"
        out.extend(difflib.unified_diff(
            old.splitlines(keepends=True), new.splitlines(keepends=True),
            fromfile=fromfile, tofile=f"b/{path}"))
    return "".join(out) or None


def _council():
    """Import perezoso de council (mismo directorio). Así --scan funciona
    aunque el gateway esté caído: solo --draft/--evolve necesitan el LLM."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "ivanna_council", EVO / "council.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def draft_patch(finding: dict, llm=None, repo=None) -> dict:
    """Redacta una reescritura magistral para un hallazgo.

    v2.5.445 — LA REESCRITURA PASA POR EL CONSEJO (directiva de Esse
    2026-11-14 17:46: "hazlo real ya al producto"). El consejo
    multi-modelo (tools/evolution/council.py v2.0, 2026-11-17: DETECTOR
    mistral → VERIFICADOR 3.1-flash-lite → ARQUITECTO luna → IMPLEMENTER
    ENJAULADO gpt-oss-120b → CENTINELA haiku → ÁRBITRO mini, máx. 2
    rondas, gate mecánico) es LA PUERTA que decide si un parche es
    magistral o se rechaza, ANTES de validar en scratch y publicar. Sin acuerdo del consejo no hay draft:
    fail-closed. El consejo NO aplica nada: devuelve el parche
    acordado y su transcripción; este draft lo recibe, le aplica los
    gates de archivos prohibidos y lo devuelve para validar.
    La misma puerta cubre los tres caminos que redactan: el motor
    (--draft/--evolve), el RouteBDoctor (run_fix.py draft llama a esta
    misma función) y el ciclo autónomo (github_bridge.autonomous →
    cmd_evolve → aquí).

    finding: {"kind", "title", "evidence", "files": [rutas relativas]}.
    Devuelve {"patch", "raw_text", "model", "latency_s", "files",
    "council": <veredicto del consejo>}.
    Lanza SystemExit si el hallazgo toca algo prohibido para drafts, si
    el consejo rechaza el parche o si el LLM no devuelve un diff
    utilizable tras el diálogo.
    """
    files = finding.get("files") or []
    bad = [f for f in files if is_draft_forbidden(f)]
    if bad:
        raise SystemExit(f"GATE draft: el hallazgo toca archivos prohibidos "
                         f"para drafts: {bad}")
    llm = llm or _llm()
    ok, health_ms, _models = llm.health()
    if not ok:
        raise SystemExit(f"gateway OmniRoute no disponible (health {health_ms:.0f} ms)")
    import time
    t0 = time.monotonic()
    council = _council()
    verdict = council.council_review(finding, llm=llm, repo=repo)
    latency = time.monotonic() - t0
    if not verdict.get("approved") or not verdict.get("patch"):
        raise SystemExit(
            "el consejo RECHAZÓ la reescritura (fail-closed, no hay draft): "
            f"{verdict.get('reason', 'sin motivo')}")
    patch = verdict["patch"]
    # El proposer emite a veces hunks `@@` sin números de línea: `git
    # apply` los rechaza ("patch with only garbage") aunque el consejo
    # haya aprobado (ensayo 0006). Se normalizan por contexto exacto
    # contra el árbol; si no se pueden ubicar, no hay draft (fail-closed).
    # council.py NO se toca: la integración se repara aquí, en el motor.
    normalized = normalize_patch_hunks(patch, REPO)
    if normalized is None:
        raise SystemExit(
            "el consejo aprobó un parche con hunks sin numerar que no se "
            "pudieron ubicar por contexto exacto (fail-closed, no hay draft)")
    patch = normalized
    touched = check_patch_limits(patch)
    bad = [t for t in touched if is_draft_forbidden(t)]
    if bad:
        raise SystemExit(f"GATE draft: el parche del consejo toca archivos "
                         f"prohibidos para drafts: {bad}")
    rounds_text = "\n".join(
        f"Ronda {r['round']}: proposer={r.get('proposer')}; "
        f"implementer={r.get('implementer')}; "
        f"reviewer={r.get('reviewer')}"
        for r in verdict.get("rounds", []))
    det = verdict.get("detector") or {}
    raw_text = (
        f"CONSEJO (veredicto: {verdict.get('reason', '')})\n"
        f"Detector: {council.ROLES['detector']} "
        f"(real={det.get('real')})\n"
        f"Proposer: {council.ROLES['proposer']}\n"
        f"Implementer: {council.ROLES['implementer']}\n"
        f"Reviewer: {council.ROLES['reviewer']}\n"
        f"Arbiter: {council.ROLES['arbiter']}\n"
        f"{rounds_text}\n\n{patch}")
    return {"patch": patch, "raw_text": raw_text,
            "model": ("consejo: detector " + council.ROLES["detector"]
                      + " + proposer " + council.ROLES["proposer"]
                      + " + implementer " + council.ROLES["implementer"]
                      + " + reviewer " + council.ROLES["reviewer"]
                      + " + arbiter " + council.ROLES["arbiter"]),
            "latency_s": round(latency, 1), "files": touched,
            "council": verdict}


def _escribir_propuesta(pid: str, patch: str, draft: dict, finding: dict) -> Path:
    pdir = PROPOSALS / pid
    pdir.mkdir(parents=True, exist_ok=True)
    (pdir / f"{pid}.patch").write_text(patch)
    council = draft.get("council") or {}
    (pdir / "DRAFT.md").write_text(
        f"# Draft {pid} (consejo multi-modelo vía OmniRoute)\n\n"
        f"- **Modelo:** {draft['model']} (gateway http://localhost:20128/v1)\n"
        f"- **Latencia del consejo:** {draft['latency_s']} s\n"
        f"- **Veredicto del consejo:** {council.get('reason', 'sin consejo')}\n"
        f"- **Hallazgo:** {finding.get('kind', '?')}: {finding.get('title', '')}\n"
        f"- **Archivos del parche:** {', '.join(draft['files'])}\n\n"
        "## Transcripción del consejo\n\n"
        "```\n" + draft["raw_text"][:12000] + "\n```\n")
    (pdir / "COUNCIL.md").write_text(
        f"# Consejo {pid}\n\n"
        f"- **Veredicto:** {'APROBADO' if council.get('approved') else 'RECHAZADO'}\n"
        f"- **Motivo:** {council.get('reason', '')}\n"
        f"- **Rondas:** {len(council.get('rounds', []))} (máx. 3)\n\n"
        "```json\n"
        + json.dumps(council, indent=2, ensure_ascii=False)[:8000]
        + "\n```\n")
    return pdir


def cmd_evolve(pid: str, finding: dict) -> dict:
    """Ciclo detectar → reescribir → verificar. NUNCA aplica al árbol ni
    publica: deja la propuesta + validation.json + REPORT.md listos."""
    draft = draft_patch(finding)
    pdir = _escribir_propuesta(pid, draft["patch"], draft, finding)
    result = validate_proposal(pid)
    passed = bool(result.get("applied"))
    if "full_suite" in result:
        fs = result["full_suite"]
        passed = passed and (fs.get("passed") is True or fs.get("skipped") is True)
    if "python_checks" in result:
        pc = result["python_checks"]
        passed = passed and all(
            v.get("passed") is True or v.get("skipped") is True
            for v in pc.values())
    verdict = "VERDE — lista para revisión/publicación" if passed else \
        "ROJO — NO se publica"
    (pdir / "REPORT.md").write_text(
        f"# Propuesta {pid} — {finding.get('title', '')}\n\n"
        f"- **Hallazgo:** {finding.get('kind', '?')}\n"
        f"- **Evidencia:** {finding.get('evidence', '')}\n"
        f"- **Consejo:** {draft.get('council', {}).get('reason', 'sin consejo')} "
        f"({len(draft.get('council', {}).get('rounds', []))} rondas)\n"
        f"- **Draft:** modelo {draft['model']}, {draft['latency_s']} s\n"
        f"- **Archivos:** {', '.join(draft['files'])}\n"
        f"- **Veredicto:** {verdict}\n"
        f"- **Árbol intacto:** {result.get('tree_untouched')} "
        f"(scratch {result.get('scratch', {}).get('mode')}, "
        f"eliminado: {result.get('scratch', {}).get('removed')})\n\n"
        "## Validación (ver validation.json)\n\n"
        f"```json\n{json.dumps(result, indent=2, ensure_ascii=False)[:4000]}\n```\n")
    print(f"propuesta {pid}: {verdict}")
    print(f"  parche: {pdir / (pid + '.patch')}")
    print(f"  validación: {pdir / 'validation.json'}")
    print(f"  reporte: {pdir / 'REPORT.md'}")
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scan", action="store_true")
    ap.add_argument("--proposal")
    ap.add_argument("--validate", action="store_true")
    ap.add_argument("--draft")
    ap.add_argument("--evolve")
    ap.add_argument("--apply",
                    help="aplica una propuesta VERDE aprobada por el consejo "
                         "al árbol real (sin commit); ver apply_proposal.py")
    ap.add_argument("--allow-no-council-record", action="store_true",
                    help="con --apply: el operador asume la responsabilidad "
                         "de una propuesta sin registro de consejo")
    ap.add_argument("--finding", help="hallazgo JSON: {kind, title, evidence, files[]}")
    ap.add_argument("--status", action="store_true",
                    help="exporta el estado del motor como JSON (Vista de Dios)")
    ap.add_argument("--status-out", help="archivo donde escribir el estado")
    args = ap.parse_args()
    if args.status:
        # Foto del motor para la Vista de Dios (sin secretos, sin tocar
        # el árbol): proposals + consejo + gateway + auditoría.
        import importlib.util
        spec = importlib.util.spec_from_file_location(
            "ivanna_status_export", EVO / "status_export.py")
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        status = mod.export_status()
        text = json.dumps(status, indent=2, ensure_ascii=False)
        if args.status_out:
            Path(args.status_out).write_text(text, encoding="utf-8")
            print(f"estado del motor: {args.status_out}")
        else:
            print(text)
        return
    if args.scan or (not args.proposal and not args.validate
                     and not args.draft and not args.evolve
                     and not args.apply):
        k = build_knowledge()
        print(json.dumps(k, indent=2, ensure_ascii=False))
    if args.proposal and args.validate:
        print(json.dumps(validate_proposal(args.proposal), indent=2, ensure_ascii=False))
    if args.apply:
        # El eslabón que faltaba (2026-11-15): validar → aplicar.
        # Delega en apply_proposal.py; nunca commitea ni publica.
        import importlib.util as _ilu
        _spec = _ilu.spec_from_file_location(
            "ivanna_apply_proposal", EVO / "apply_proposal.py")
        _mod = _ilu.module_from_spec(_spec)
        _spec.loader.exec_module(_mod)
        _rep = _mod.apply_proposal(
            args.apply,
            allow_no_council_record=args.allow_no_council_record)
        print(json.dumps(_rep, indent=2, ensure_ascii=False))
        if _rep["status"] == "applied":
            print("\nListo para commit (lo hace el operador):")
            for _cmd in _rep["commit_commands"]:
                print(f"  {_cmd}")
        return
    if args.draft or args.evolve:
        if not args.finding:
            raise SystemExit("--draft/--evolve requieren --finding '<json>'")
        finding = json.loads(args.finding)
        pid = args.draft or args.evolve
        if args.draft and not args.evolve:
            draft = draft_patch(finding)
            pdir = _escribir_propuesta(pid, draft["patch"], draft, finding)
            print(f"draft {pid} escrito (SIN validar): {pdir / (pid + '.patch')}")
            print("valídalo con: --proposal " + pid + " --validate")
        else:
            cmd_evolve(pid, finding)


if __name__ == "__main__":
    main()
