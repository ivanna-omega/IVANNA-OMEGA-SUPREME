"""Tests de scan_todo_fixme (propuesta 0019, coordinador del Plan
Omnipotencia).

El scanner contaba la palabra española TODO (= todo/all) en comentarios
como si fuera un marcador de tarea: "TODO el audio", "TODO apagado",
"TODO cambio" inflaban el inventario a 28 cuando solo 1 era real.
El fix exige marcador de tarea (`TODO:` / `TODO(` / `FIXME:` /
`FIXME(`) y excluye nlohmann/json vendoreado.

Casos:
- "TODO el audio" (español) -> NO se cuenta.
- "TODO(route-arbiter-v2): ..." (marcador real) -> SÍ se cuenta.
- "TODO(niels)" en nlohmann/json vendoreado -> NO se cuenta.
- El repo real reporta 1: TODO(route-arbiter-v2) en
  ivanna_omega_jni.cpp:1803.

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


class TestScanTodoFixme(unittest.TestCase):
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

    def test_0019_spanish_todo_is_not_counted(self):
        """'TODO el audio' (español) no es una tarea: no se cuenta."""
        (self.repo / "app/src/main/cpp/a.cpp").write_text(
            "// El músculo las aplica a TODO el audio.\n"
            "// Por defecto TODO apagado.\n"
            "// TODO cambio de ruta.\n",
            encoding="utf-8",
        )
        self._commit()
        r = evolution_engine.scan_todo_fixme()
        self.assertEqual(r["total"], 0)

    def test_0019_real_task_marker_is_counted(self):
        """TODO: y TODO(...) sí son tareas: se cuentan."""
        (self.repo / "app/src/main/cpp/b.cpp").write_text(
            "// TODO(route-arbiter-v2): hacer passthrough solo si route==SYSTEM_WIDE.\n"
            "// FIXME: el buffer puede llegar vacío.\n",
            encoding="utf-8",
        )
        self._commit()
        r = evolution_engine.scan_todo_fixme()
        self.assertEqual(r["total"], 2)
        self.assertEqual(
            {i["tag"] for i in r["items"]}, {"TODO", "FIXME"})

    def test_0019_vendored_nlohmann_is_excluded(self):
        """TODO(niels) en nlohmann/json vendoreado no es deuda de IVANNA."""
        d = self.repo / "app/src/main/cpp/include/nlohmann"
        d.mkdir(parents=True, exist_ok=True)
        (d / "json.hpp").write_text(
            "get();  // TODO(niels): may we ignore N here?\n",
            encoding="utf-8",
        )
        self._commit()
        r = evolution_engine.scan_todo_fixme()
        self.assertEqual(r["total"], 0)

    def test_0019_real_repo_reports_one(self):
        """El repo real reporta 1: el route-arbiter-v2."""
        evolution_engine.REPO = self._old_repo
        r = evolution_engine.scan_todo_fixme()
        self.assertEqual(r["total"], 1)
        self.assertEqual(r["items"][0]["file"],
                         "app/src/main/cpp/jni/ivanna_omega_jni.cpp")
        # Línea 2244 (antes 1848, antes 1836, antes 1825, antes 1809, antes
        # 1803): la 486 añadió ~396 líneas netas antes del TODO (motor
        # double b10266c2 + NEURO 4c04b26f + fixes 486/487 en el JNI). La
        # 0031 añadió 12 líneas netas antes del TODO (declaración de
        # RouteShedGovernors + comentario, y el log throttled del
        # gobernador grande, líneas ~1728-1784). La 0027 había añadido
        # 11 líneas netas (include + declaración del multibanda + 2x
        # setParams). La 0024 había añadido 16 líneas (comentario del
        # default g_npeAOn{true}, línea ~611). El fix(rtlog) 598db138
        # había añadido 6 líneas antes (drenaje rtLogDrainCallbackForShed,
        # línea ~1748). El TODO es el mismo (route-arbiter-v2); el pin
        # sigue la línea real, no la histórica.
        self.assertEqual(r["items"][0]["line"], 2244)


if __name__ == "__main__":
    unittest.main()
