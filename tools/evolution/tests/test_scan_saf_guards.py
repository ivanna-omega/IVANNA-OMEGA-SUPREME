"""Tests de scan_saf_guards (propuesta 0002, redactada por el LLM vía OmniRoute).

Corrección del draft del LLM al integrar: el import original
(`tools.evolution.evolution_engine`) fallaba con `python3 -m unittest
discover -s tools/evolution/tests` porque la raíz del repo no está en
sys.path y tools/ no es un paquete. Se importa como módulo top-level
con la raíz del repo insertada en sys.path; el test hace monkeypatch de
evolution_engine.REPO al árbol falso y lo restaura en tearDown.

Casos (falsos positivos medidos 2026-11-14):
- .cpp sin funciones y sin guarda -> NO es hit (regla 1).
- .cpp con función, sin guarda en el .cpp pero con isfinite en el header
  hermano -> NO es hit (regla 3).
- .cpp con función repetida (declaración + definición), sin guarda en .cpp
  ni header -> UN solo hit, función una sola vez (reglas 1-4).

Casos 0007 (2026-11-16, guarda por DEFINICIÓN, extractor brace-matching):
- .cpp que incluye un header donde se define isFiniteLatent (cuerpo con
  std::isfinite) y la llama -> NO es hit (el falso positivo real).
- Guarda en include TRANSITIVO (a incluye b, b incluye el header de la
  guarda) -> NO es hit.
- Header incluido con función SIN std::isfinite en el cuerpo -> SÍ es hit
  (negativo: definir una función no basta, debe ser guarda).
- Guarda definida en el propio .cpp -> NO es hit.
- Extractor: firmas multilínea, inline/static/constexpr, noexcept,
  llaves anidadas; `std::isfinite` en comentario o en literal de cadena
  NO es guarda.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import evolution_engine  # noqa: E402


class TestScanSafGuards(unittest.TestCase):
    def setUp(self):
        self._old_repo = evolution_engine.REPO
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        (self.repo / "app/src/main/cpp/include").mkdir(parents=True, exist_ok=True)
        evolution_engine.REPO = self.repo

    def tearDown(self):
        evolution_engine.REPO = self._old_repo
        self._tmp.cleanup()

    def test_scan_saf_guards_filters_false_positives_and_deduplicates(self):
        (self.repo / "app/src/main/cpp/saf_empty.cpp").write_text(
            "int x = 0;\n", encoding="utf-8"
        )

        (self.repo / "app/src/main/cpp/saf_header_guard.cpp").write_text(
            "void guarded(void) {\n  return;\n}\n", encoding="utf-8"
        )
        (self.repo / "app/src/main/cpp/include/saf_header_guard.h").write_text(
            "#pragma once\ninline bool safe_header() { return std::isfinite(1.0); }\n",
            encoding="utf-8",
        )

        (self.repo / "app/src/main/cpp/saf_latent_bridge.cpp").write_text(
            "void ivanna_saf_apply_latent();\n"
            "void ivanna_saf_apply_latent() {\n"
            "  int a = 1;\n"
            "}\n",
            encoding="utf-8",
        )

        hits = evolution_engine.scan_saf_guards()

        self.assertEqual(len(hits), 1)
        hit = hits[0]
        self.assertEqual(hit["file"], "app/src/main/cpp/saf_latent_bridge.cpp")
        self.assertEqual(hit["functions"], ["ivanna_saf_apply_latent"])
        self.assertFalse(hit["has_isfinite_guard"])
        self.assertEqual(hit["guard_source"], "none")
        self.assertIsNone(hit["header_checked"])

    def test_0007_guard_in_included_header_is_not_a_hit(self):
        """El falso positivo real: isFiniteLatent vive en un header
        incluido (spatial/SafmusLatentGuard.hpp) y el .cpp la llama."""
        (self.repo / "app/src/main/cpp/spatial").mkdir(parents=True, exist_ok=True)
        (self.repo / "app/src/main/cpp/spatial/SafmusLatentGuard.hpp").write_text(
            "#pragma once\n"
            "#include <cmath>\n"
            "namespace ivanna::safmus {\n"
            "inline bool isFiniteLatent(const float* q) noexcept {\n"
            "    if (!q) return false;\n"
            "    for (int i = 0; i < 7; ++i) {\n"
            "        if (!std::isfinite(q[i])) return false;\n"
            "    }\n"
            "    return true;\n"
            "}\n"
            "} // namespace ivanna::safmus\n",
            encoding="utf-8",
        )
        (self.repo / "app/src/main/cpp/saf_latent_bridge.cpp").write_text(
            '#include "spatial/SafmusLatentGuard.hpp"\n'
            "void ivanna_saf_apply_latent(const float* q) {\n"
            "    if (!ivanna::safmus::isFiniteLatent(q)) return;\n"
            "}\n",
            encoding="utf-8",
        )
        hits = evolution_engine.scan_saf_guards()
        self.assertEqual(hits, [])

    def test_0007_transitive_include_guard_is_not_a_hit(self):
        """La guarda vive a dos niveles: el .cpp incluye a.h, a.h incluye
        el header de la guarda."""
        (self.repo / "app/src/main/cpp/spatial").mkdir(parents=True, exist_ok=True)
        (self.repo / "app/src/main/cpp/spatial/Guard.hpp").write_text(
            "#pragma once\n"
            "inline bool guard_fn(double x) { return std::isfinite(x); }\n",
            encoding="utf-8",
        )
        (self.repo / "app/src/main/cpp/saf_mid.h").write_text(
            '#pragma once\n#include "spatial/Guard.hpp"\n',
            encoding="utf-8",
        )
        (self.repo / "app/src/main/cpp/saf_transitive.cpp").write_text(
            '#include "saf_mid.h"\n'
            "void work(double x) {\n"
            "    if (!guard_fn(x)) return;\n"
            "}\n",
            encoding="utf-8",
        )
        hits = evolution_engine.scan_saf_guards()
        self.assertEqual(hits, [])

    def test_0007_included_function_without_isfinite_is_a_hit(self):
        """Negativo: el header incluido define una función, pero su cuerpo
        NO contiene std::isfinite -> el módulo sigue sin guarda."""
        (self.repo / "app/src/main/cpp/saf_plain.h").write_text(
            "#pragma once\n"
            "inline bool not_a_guard(double x) { return x > 0.0; }\n",
            encoding="utf-8",
        )
        (self.repo / "app/src/main/cpp/saf_plain.cpp").write_text(
            '#include "saf_plain.h"\n'
            "void work(double x) {\n"
            "    double y = x * 2.0;\n"
            "    (void)y;\n"
            "}\n",
            encoding="utf-8",
        )
        hits = evolution_engine.scan_saf_guards()
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0]["file"], "app/src/main/cpp/saf_plain.cpp")
        self.assertEqual(hits[0]["guard_source"], "none")

    def test_0007_guard_defined_in_cpp_is_not_a_hit(self):
        """Guarda definida en el propio .cpp (firma multilínea,
        static, noexcept, llaves anidadas)."""
        (self.repo / "app/src/main/cpp/saf_local.cpp").write_text(
            "static bool local_guard(\n"
            "    double x,\n"
            "    double y) noexcept {\n"
            "    if (x > 0.0) {\n"
            "        if (!std::isfinite(y)) { return false; }\n"
            "    }\n"
            "    return std::isfinite(x);\n"
            "}\n"
            "void work(double x) {\n"
            "    if (!local_guard(x, x)) return;\n"
            "}\n",
            encoding="utf-8",
        )
        hits = evolution_engine.scan_saf_guards()
        self.assertEqual(hits, [])

    def test_0007_isfinite_in_comment_or_string_is_not_a_guard(self):
        """`std::isfinite` en un comentario o en un literal de cadena NO
        es una guarda: el módulo sigue siendo hit."""
        (self.repo / "app/src/main/cpp/saf_fake.cpp").write_text(
            "// usa std::isfinite aqui (mentira: solo es un comentario)\n"
            'const char* s = "std::isfinite";\n'
            "void work(double x) {\n"
            "    double y = x * 2.0;\n"
            "    (void)y; (void)s;\n"
            "}\n",
            encoding="utf-8",
        )
        hits = evolution_engine.scan_saf_guards()
        self.assertEqual(len(hits), 1)
        self.assertEqual(hits[0]["file"], "app/src/main/cpp/saf_fake.cpp")

    def test_0007_extractor_handles_qualifiers_and_nested_braces(self):
        """El extractor no confunde llamadas ni control con definiciones,
        y casa cuerpos con llaves anidadas."""
        defs = evolution_engine._extract_function_definitions(
            "inline bool guard_fn(double x) noexcept {\n"
            "    if (x > 0.0) { return std::isfinite(x); }\n"
            "    return false;\n"
            "}\n"
            "void work(double x) {\n"
            "    if (guard_fn(x)) { int a = 1; (void)a; }\n"
            "    for (int i = 0; i < 3; ++i) { int b = i; (void)b; }\n"
            "}\n"
        )
        names = [d["name"] for d in defs]
        self.assertEqual(names, ["guard_fn", "work"])
        guard = next(d for d in defs if d["name"] == "guard_fn")
        self.assertTrue(evolution_engine._is_guard_function(guard))
        work = next(d for d in defs if d["name"] == "work")
        self.assertFalse(evolution_engine._is_guard_function(work))


if __name__ == "__main__":
    unittest.main()
