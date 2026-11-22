"""Tests de scan_dead_functions (propuesta 0012, coordinador del Plan
Omnipotencia).

El scanner tenía un bug: excluía el archivo definidor completo al
buscar llamadores, así que los helpers de test llamados en su propio
archivo (DriveEta, Pearson, RmsDiffDb, RunOrbe, RunHolos, ...) salían
como muertos aunque sí se usan (10 falsos positivos medidos 2026-11-16).
El fix cuenta como llamador cualquier mención que NO sea la propia
definición (misma línea del nombre), en el mismo archivo o en otro.

Casos:
- Función llamada solo en su propio archivo -> NO es muerta.
- Función llamada en otro archivo -> NO es muerta.
- Función sin ninguna mención fuera de su definición -> SÍ es muerta.
- Función en DEAD_FUNCTION_ALLOWLIST -> NO se reporta aunque esté
  muerta (SAF_IsModelLoaded, legacy documentado).

El test crea un repo git temporal porque el scanner usa `git grep`.
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import evolution_engine  # noqa: E402


def _git(repo, *args):
    subprocess.run(["git", *args], cwd=repo, check=True,
                   capture_output=True, text=True)


class TestScanDeadFunctions(unittest.TestCase):
    def setUp(self):
        self._old_repo = evolution_engine.REPO
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        (self.repo / "app/src/main/cpp").mkdir(parents=True, exist_ok=True)
        _git(self.repo, "init", "-q")
        _git(self.repo, "config", "user.email", "test@ivanna.local")
        _git(self.repo, "config", "user.name", "Test")
        evolution_engine.REPO = self.repo

    def tearDown(self):
        evolution_engine.REPO = self._old_repo
        self._tmp.cleanup()

    def _commit(self):
        _git(self.repo, "add", "-A")
        _git(self.repo, "commit", "-qm", "fixture")

    def test_0012_same_file_caller_is_not_dead(self):
        """Helper de test llamado en su propio archivo: NO es muerto."""
        (self.repo / "app/src/main/cpp/tests").mkdir(parents=True, exist_ok=True)
        (self.repo / "app/src/main/cpp/tests/test_x.cpp").write_text(
            "double Helper(double x) { return x * 2.0; }\n"
            "TEST(X, UsesHelper) {\n"
            "    double y = Helper(1.0);\n"
            "    EXPECT_GT(y, 0.0);\n"
            "}\n",
            encoding="utf-8",
        )
        self._commit()
        dead = evolution_engine.scan_dead_functions()
        self.assertEqual(dead, [])

    def test_0012_other_file_caller_is_not_dead(self):
        """Función llamada desde otro archivo: NO es muerta."""
        (self.repo / "app/src/main/cpp/saf_a.cpp").write_text(
            "bool SafHelper() { return true; }\n",
            encoding="utf-8",
        )
        (self.repo / "app/src/main/cpp/saf_b.cpp").write_text(
            "bool SafHelper();\n"
            "TEST(B, UsesHelper) {\n"
            "    bool ok = SafHelper();\n"
            "    EXPECT_TRUE(ok);\n"
            "}\n",
            encoding="utf-8",
        )
        self._commit()
        dead = evolution_engine.scan_dead_functions()
        self.assertEqual(dead, [])

    def test_0012_never_called_is_dead(self):
        """Función sin ninguna mención fuera de su definición: SÍ es
        muerta (la definición sola no cuenta como llamador)."""
        (self.repo / "app/src/main/cpp/saf_dead.cpp").write_text(
            "bool SafOrphan() { return false; }\n"
            "TEST(X, Alive) {\n"
            "    bool ok = true;\n"
            "    EXPECT_TRUE(ok);\n"
            "}\n",
            encoding="utf-8",
        )
        self._commit()
        dead = evolution_engine.scan_dead_functions()
        self.assertEqual(len(dead), 1)
        self.assertEqual(dead[0]["function"], "SafOrphan")
        self.assertEqual(dead[0]["file"], "app/src/main/cpp/saf_dead.cpp")

    def test_0012_allowlisted_is_not_reported(self):
        """SAF_IsModelLoaded está en el allowlist: no se reporta aunque
        esté muerta (legacy documentado, nunca borrar)."""
        (self.repo / "app/src/main/cpp/saf_runtime.cpp").write_text(
            "static bool g_saf_model_loaded = false;\n"
            "bool SAF_IsModelLoaded() { return g_saf_model_loaded; }\n",
            encoding="utf-8",
        )
        self._commit()
        dead = evolution_engine.scan_dead_functions()
        self.assertEqual(dead, [])

    def test_0019_untracked_file_caller_is_found(self):
        """Un helper llamado en un archivo SIN commitear también cuenta:
        la búsqueda es en el árbol de trabajo, no `git grep` (que no ve
        archivos untracked). Regresión de 0019: los helpers de los tests
        nuevos (SendOneFd, FillSine, ...) salían como muertos mientras
        los archivos estaban sin commitear."""
        (self.repo / "app/src/main/cpp/tests").mkdir(parents=True, exist_ok=True)
        (self.repo / "app/src/main/cpp/tests/test_new.cpp").write_text(
            "bool HelperNew() { return true; }\n"
            "TEST(X, UsesHelper) {\n"
            "    EXPECT_TRUE(HelperNew());\n"
            "}\n",
            encoding="utf-8",
        )
        # Sin _commit(): el archivo queda untracked a propósito.
        dead = evolution_engine.scan_dead_functions()
        self.assertEqual(dead, [])


if __name__ == "__main__":
    unittest.main()
