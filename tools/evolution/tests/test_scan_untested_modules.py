"""Tests de scan_untested_modules (propuesta 0017, coordinador del Plan
Omnipotencia).

El scanner hacía substring sobre tests/CMakeLists.txt SIN quitar
comentarios: un `#` que mencionaba el nombre (IvannaFusionCore.cpp,
comentado en la lista de la librería y citado en una explicación de
por qué no se enlaza en host) lo daba por cableado aunque ningún
target lo compila. El fix quita los comentarios `#...` antes del
substring, mismo patrón que check_header_wiring.

Casos:
- Un módulo mencionado SOLO en un comentario -> SÍ es sin tests.
- Un módulo en un add_executable -> NO es sin tests.
- El repo real reporta 16 e incluye IvannaFusionCore.cpp.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import evolution_engine  # noqa: E402


class TestScanUntestedModules(unittest.TestCase):
    def setUp(self):
        self._old_repo = evolution_engine.REPO
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        (self.repo / "app/src/main/cpp/tests").mkdir(parents=True, exist_ok=True)
        evolution_engine.REPO = self.repo

    def tearDown(self):
        evolution_engine.REPO = self._old_repo
        self._tmp.cleanup()

    def _write(self, cmake_text, modules):
        (self.repo / "app/src/main/cpp/tests/CMakeLists.txt").write_text(
            cmake_text, encoding="utf-8")
        for name in modules:
            (self.repo / "app/src/main/cpp" / name).write_text(
                "// stub\n", encoding="utf-8")

    def test_0017_comment_only_is_untested(self):
        """Módulo mencionado solo en comentarios: SÍ es sin tests."""
        self._write(
            "# IvannaFusionCore.cpp no se enlaza en host (explicación).\n"
            "#   ${CMAKE_CURRENT_SOURCE_DIR}/../IvannaFusionCore.cpp\n"
            "add_executable(test_x test_x.cpp ../Other.cpp)\n",
            ["IvannaFusionCore.cpp", "Other.cpp"],
        )
        mods = evolution_engine.scan_untested_modules()
        self.assertEqual(mods, ["app/src/main/cpp/IvannaFusionCore.cpp"])

    def test_0017_code_reference_is_tested(self):
        """Módulo en un add_executable: NO es sin tests."""
        self._write(
            "add_executable(test_x test_x.cpp ../Wired.cpp)\n",
            ["Wired.cpp"],
        )
        mods = evolution_engine.scan_untested_modules()
        self.assertEqual(mods, [])

    def test_0017_real_repo_includes_fusion_core(self):
        """El repo real reporta IvannaFusionCore.cpp como sin tests
        (antes lo escondían los comentarios)."""
        evolution_engine.REPO = self._old_repo
        mods = evolution_engine.scan_untested_modules()
        self.assertIn("app/src/main/cpp/IvannaFusionCore.cpp", mods)
