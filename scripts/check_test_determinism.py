#!/usr/bin/env python3
"""check_test_determinism.py — Contrato de determinismo C10 (release 510).

Falla si algún test C++ usa azar sin semilla (un resultado sin su semilla
no es un resultado):

  - std::random_device (cualquier uso: azar de verdad, no reproducible)
  - std::rand()/rand() sin std::srand() con semilla determinista en el
    mismo bloque (TEST, TEST_F o función que lo usa)
  - std::srand() / .seed() / motores (mt19937, minstd_rand, ...) sembrados
    con time(), getpid(), random_device o un reloj (system_clock,
    steady_clock, high_resolution_clock)
  - time() / std::time() / std::clock() como semilla (llamadas libres;
    los métodos .time() de un objeto no cuentan)
  - std::chrono::system_clock en código de test (el reloj de pared no
    tiene nada que hacer en un test determinista; para medir tiempo se
    usa steady_clock, que es territorio de C11, no de C10)
  - drand48()/lrand48()/random()/arc4random()/getrandom() (sin disciplina
    de semilla) y tmpnam()/tempnam() (nombre anónimo)
  - rutas /tmp con nombre genérico (sin nombre propio): se exige un
    nombre distintivo del test; las sondas de no-existencia
    (no_existe_*) y la base "/tmp" como directorio pasan
  - todo archivo que construya un motor o llame a srand()/rand() debe
    incluir "ivanna_test_seed.hpp" y documentar la semilla con
    PrintIvannaTestSeed() (la línea `seed=...` al inicio del log)

Excepciones (allowlist con motivo, no agujeros):
  - tests/adversarial/src/adversarial_core.cpp: la semilla llega por
    CLI --seed (GenOptions.seed, default 0); la librería no lee entorno.

Solo stdlib. Uso:
  python3 scripts/check_test_determinism.py [--root DIR] [--verbose]
Sale 0 si pasa, 1 si falla (una línea por hallazgo, formato file:line).
"""

import argparse
import re
import sys
from pathlib import Path

# Raíces con tests C++ (relativas a la raíz del repo). third_party se
# excluye siempre (googletest vendoreado: no es código nuestro).
TEST_ROOTS = [
    "app/src/main/cpp/tests",
    "magisk_module/engine/tests",
    "tests",
    "hal_integration",
    "research",
]
SOURCE_SUFFIXES = {".cpp", ".cc", ".c", ".h", ".hpp"}
EXCLUDE_DIRS = {"third_party", "__pycache__", "node_modules"}

# Archivo que construye motores pero cuya semilla llega por CLI, no por
# entorno: no se le exige el include de ivanna_test_seed.hpp.
SEED_INCLUDE_ALLOWLIST = {
    "tests/adversarial/src/adversarial_core.cpp",
}

# Motores de <random> cuya semilla hay que vigilar.
ENGINES = (
    "mt19937",
    "mt19937_64",
    "minstd_rand",
    "minstd_rand0",
    "ranlux24",
    "ranlux24_base",
    "ranlux48",
    "ranlux48_base",
    "knuth_b",
    "default_random_engine",
    "random_device",
)

# Basenames genéricos de /tmp: "sin nombre propio".
GENERIC_TMP_STEMS = {
    "test", "tmp", "temp", "out", "data", "file", "output", "input",
    "x", "a", "b", "t", "1", "log", "dump", "foo", "bar", "xxx",
}

# Tokens que delatan una semilla no determinista dentro de un argumento
# de siembra (srand, .seed, ctor de motor, seed_seq).
BAD_SEED_TOKENS = (
    "time(", "random_device", "getpid", "system_clock", "steady_clock",
    "high_resolution_clock", "clock(", "arc4random", "getrandom",
)


def strip_comments(text):
    """Quita // y /* */ respetando literales; preserva saltos de línea."""
    token = re.compile(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)

    def repl(m):
        s = m.group(0)
        if s.startswith('"') or s.startswith("'"):
            return s
        return "".join("\n" if ch == "\n" else " " for ch in s)

    return token.sub(repl, text)


def line_no(text, pos):
    return text.count("\n", 0, pos) + 1


def matching_delim(text, open_pos, open_ch="(", close_ch=")"):
    """Posición del delimitador de cierre que casa con open_pos (o None)."""
    depth = 0
    i = open_pos
    while i < len(text):
        c = text[i]
        if c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return None


def matching_paren(text, open_pos):
    """Posición del ')' que cierra el '(' en open_pos (o None)."""
    return matching_delim(text, open_pos, "(", ")")


def enclosing_block_opens(text, pos):
    """Posiciones de apertura de los bloques { } que encierran pos,
    del más interno al más externo."""
    stack = []
    for i in range(pos):
        c = text[i]
        if c == "{":
            stack.append(i)
        elif c == "}":
            if stack:
                stack.pop()
    return list(reversed(stack))


# Un uso de motor: construcción directa (mt19937(1234)), declaración
# (mt19937 rng(1234)) o init con llaves (mt19937 rng{1234}). El nombre
# más largo primero para que mt19937_64 no case como mt19937.
_ENG_ALT = "|".join(sorted(ENGINES, key=len, reverse=True))
_ENG_PATTERNS = (
    re.compile(r"(?:\bstd::)?(?:" + _ENG_ALT + r")\s*(?:<[^>]*>)?\s*\("),
    re.compile(r"(?:\bstd::)?(?:" + _ENG_ALT + r")\s*(?:<[^>]*>)?\s+\w+\s*\("),
    re.compile(r"(?:\bstd::)?(?:" + _ENG_ALT + r")\s*(?:<[^>]*>)?\s+\w+\s*\{"),
)


def find_engine_uses(text):
    """[(pos, arg)] de cada construcción de motor con su argumento."""
    # Pre-filtro barato (C): la mayoría de archivos no tiene motores.
    if not any(k in text for k in
               ("mt19937", "minstd_rand", "ranlux", "knuth_b",
                "default_random_engine", "random_device")):
        return []
    uses = []
    seen = set()
    for pat in _ENG_PATTERNS:
        for m in pat.finditer(text):
            open_ch = "{" if m.group(0).rstrip().endswith("{") else "("
            close_ch = "}" if open_ch == "{" else ")"
            open_pos = m.end() - 1
            if open_pos in seen:
                continue
            seen.add(open_pos)
            close = matching_delim(text, open_pos, open_ch, close_ch)
            arg = text[open_pos + 1:close] if close is not None else ""
            uses.append((m.start(), arg))
    return sorted(uses)


def bad_seed_reason(arg):
    """Motivo por el que un argumento de siembra no es determinista."""
    if "IVANNA_TEST_SEED" in arg:
        return None  # la semilla global es el contrato, no una fuga
    for tok in BAD_SEED_TOKENS:
        if tok in arg:
            return "sembrado con %s" % tok
    if re.search(r"\bgetenv\s*\(", arg):
        return "sembrado con getenv (que no sea IVANNA_TEST_SEED)"
    return None


class Checker:
    def __init__(self, root, verbose=False):
        self.root = Path(root)
        self.verbose = verbose
        self.errors = []

    def err(self, path, lineno, msg):
        self.errors.append("%s:%d: %s" % (path, lineno, msg))

    def rel(self, path):
        try:
            return str(path.relative_to(self.root))
        except ValueError:
            return str(path)

    def check_file(self, path):
        rel = self.rel(path)
        try:
            raw = path.read_text(encoding="utf-8", errors="strict")
        except (UnicodeDecodeError, OSError) as exc:
            self.err(rel, 1, "no se pudo leer (%s)" % exc)
            return
        text = strip_comments(raw)

        # 1. random_device: azar de verdad, nunca reproducible.
        for m in re.finditer(r"\brandom_device\b", text):
            self.err(rel, line_no(text, m.start()),
                     "random_device: azar sin semilla (usar IvannaTestSeed*)")

        # 2. time()/clock() libres como semilla (los .time() de un
        #    objeto no cuentan: se mira el carácter previo).
        for m in re.finditer(r"\b(time|clock)\s*\(", text):
            j = m.start() - 1
            while j >= 0 and text[j] in " \t":
                j -= 1
            prev = text[j] if j >= 0 else ""
            if prev in ".>":
                continue  # s.time(), p->clock(): método, no ::time()
            self.err(rel, line_no(text, m.start()),
                     "%s( como semilla: no determinista (usar IvannaTestSeed*)"
                     % m.group(1))

        # 3. system_clock en tests: reloj de pared, nunca como semilla.
        for m in re.finditer(r"\bsystem_clock\b", text):
            self.err(rel, line_no(text, m.start()),
                     "system_clock: reloj de pared en un test (usar "
                     "IvannaTestSeed* para sembrar; steady_clock para medir)")

        # 4. Generadores sin disciplina de semilla.
        for pat, name in (
            (r"\bdrand48\s*\(", "drand48()"),
            (r"\blrand48\s*\(", "lrand48()"),
            (r"\bsrand48\s*\(", "srand48()"),
            (r"\barc4random\w*\s*\(", "arc4random()"),
            (r"\bgetrandom\s*\(", "getrandom()"),
            (r"\btmpnam\s*\(", "tmpnam()"),
            (r"\btempnam\s*\(", "tempnam()"),
        ):
            for m in re.finditer(pat, text):
                self.err(rel, line_no(text, m.start()),
                         "%s sin semilla determinista" % name)
        for m in re.finditer(r"(?<![\w:])random\s*\(", text):
            self.err(rel, line_no(text, m.start()),
                     "random() sin semilla determinista (usar IvannaTestSeed*)")

        # 5. srand() / .seed() con argumento no determinista.
        for m in re.finditer(r"\bsrand\s*\(", text):
            close = matching_paren(text, m.end() - 1)
            arg = text[m.end():close] if close is not None else ""
            reason = bad_seed_reason(arg)
            if reason is not None:
                self.err(rel, line_no(text, m.start()),
                         "srand(%s)" % reason)
        for m in re.finditer(r"\.seed\s*\(", text):
            close = matching_paren(text, m.end() - 1)
            arg = text[m.end():close] if close is not None else ""
            reason = bad_seed_reason(arg)
            if reason is not None:
                self.err(rel, line_no(text, m.start()),
                         ".seed(%s)" % reason)

        # 6. Motores de <random> sembrados con fuente no determinista.
        #    (Se calcula una sola vez: también lo usa la regla 9.)
        engine_uses = find_engine_uses(text)
        for pos, arg in engine_uses:
            if not arg.strip():
                continue  # default-construido: semilla fija 5489
            reason = bad_seed_reason(arg)
            if reason is not None:
                self.err(rel, line_no(text, pos),
                         "motor %s" % reason)
        for m in re.finditer(r"\bseed_seq\b", text):
            seg = text[m.start():m.start() + 200].split(";", 1)[0]
            reason = bad_seed_reason(seg)
            if reason is not None:
                self.err(rel, line_no(text, m.start()),
                         "seed_seq %s" % reason)

        # 7. rand() sin srand() en el mismo bloque o en un ancestro
        #    (TEST, TEST_F o función que lo usa; el srand debe aparecer
        #    antes del rand dentro de ese bloque).
        rand_uses = list(re.finditer(r"(?:\bstd::)?\brand\s*\(", text))
        for m in rand_uses:
            seeded = False
            for open_pos in enclosing_block_opens(text, m.start()):
                if re.search(r"\bsrand\s*\(", text[open_pos:m.start()]):
                    seeded = True
                    break
            if not seeded:
                self.err(rel, line_no(text, m.start()),
                         "rand() sin srand(seed) en el mismo bloque "
                         "(usar IvannaTestSeed32)")

        # 8. /tmp sin nombre propio (literal genérica).
        for m in re.finditer(r'"/tmp([^"]*)"', text):
            suffix = m.group(1)
            if suffix == "":
                continue  # "/tmp" como base de directorio / sonda
            if "no_existe" in suffix:
                continue  # sonda de no-existencia: nunca crea nada
            base = suffix.rsplit("/", 1)[-1]
            stem = base.rsplit(".", 1)[0] if "." in base else base
            stem = re.sub(r"_+$", "", stem)
            if stem in GENERIC_TMP_STEMS or stem == "":
                self.err(rel, line_no(text, m.start()),
                         'ruta /tmp sin nombre propio: "%s"' % m.group(0))

        # 9. Quien siembra azar documenta la semilla: include del
        #    contrato + PrintIvannaTestSeed() en el archivo.
        seed_pos = None
        for m in (re.search(r"\bsrand\s*\(", text),
                  re.search(r"(?:\bstd::)?\brand\s*\(", text)):
            if m is not None:
                seed_pos = m.start()
                break
        if seed_pos is None and engine_uses:
            seed_pos = engine_uses[0][0]
        if seed_pos is not None and rel not in SEED_INCLUDE_ALLOWLIST:
            if "ivanna_test_seed.hpp" not in text:
                self.err(rel, line_no(text, seed_pos),
                         "genera azar sin incluir ivanna_test_seed.hpp "
                         "(contrato C10: semilla global + línea seed=...)")
            elif "PrintIvannaTestSeed" not in text:
                self.err(rel, line_no(text, seed_pos),
                         "genera azar sin llamar a PrintIvannaTestSeed() "
                         "(contrato C10: línea seed=... al inicio del log)")

    def run(self):
        files = []
        for root in TEST_ROOTS:
            base = self.root / root
            if not base.is_dir():
                continue
            for path in sorted(base.rglob("*")):
                if path.suffix not in SOURCE_SUFFIXES:
                    continue
                if any(part in EXCLUDE_DIRS for part in path.parts):
                    continue
                files.append(path)
        if self.verbose:
            print("check_test_determinism: %d archivos en %s"
                  % (len(files), self.root))
        for path in files:
            self.check_file(path)
        return files


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="C10: falla si algún test usa azar sin semilla.")
    ap.add_argument("--root", default=None,
                    help="raíz del repo (default: padre de scripts/)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args(argv)
    root = Path(args.root) if args.root else Path(__file__).resolve().parent.parent
    checker = Checker(root, verbose=args.verbose)
    files = checker.run()
    if checker.errors:
        for e in checker.errors:
            print("FAIL: " + e)
        print("check_test_determinism: FAIL (%d hallazgos en %d archivos)"
              % (len(checker.errors), len(files)))
        return 1
    print("check_test_determinism: OK (%d archivos, sin azar sin semilla)"
          % len(files))
    return 0


if __name__ == "__main__":
    sys.exit(main())
