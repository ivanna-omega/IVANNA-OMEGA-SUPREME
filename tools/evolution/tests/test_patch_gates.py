"""Tests de las puertas de parche del motor (tools/evolution/evolution_engine.py).

Cubren los dos agujeros reales hallados en la auditoría 2026-11-14:
  1. Un parche que BORRA un archivo prohibido pasaba el Gate 1 de
     validate_proposal (su `+++` es /dev/null y nunca se revisaba).
  2. El proposer del consejo emite a veces hunks `@@` sin números de
     línea: `git apply` los rechaza ("patch with only garbage") aunque
     el consejo haya aprobado (ensayo 0006-rehearsal-seeded: APROBADO
     en 1 ronda, validación ROJO). normalize_patch_hunks los repara
     por contexto exacto, o falla cerrado.
  3. Sin límites de tamaño/nº de archivos: un draft podía reescribir
     medio producto de una tacada.
"""
import difflib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import evolution_engine  # noqa: E402


def _unified_patch(rel_path, old_text, new_text):
    return "".join(difflib.unified_diff(
        old_text.splitlines(keepends=True),
        new_text.splitlines(keepends=True),
        fromfile=f"a/{rel_path}", tofile=f"b/{rel_path}"))


class TestPatchTouchedFiles(unittest.TestCase):
    def test_modify_lists_file(self):
        patch = _unified_patch("a/b.py", "x\n", "y\n")
        self.assertEqual(["a/b.py"],
                         evolution_engine.patch_touched_files(patch))

    def test_delete_lists_deleted_file(self):
        # Borrado: `--- a/X` + `+++ /dev/null`. ANTES: no se listaba.
        patch = ("diff --git a/app/build.gradle.kts b/app/build.gradle.kts\n"
                 "--- a/app/build.gradle.kts\n"
                 "+++ /dev/null\n"
                 "@@ -1,1 +0,0 @@\n"
                 "-old\n")
        self.assertEqual(["app/build.gradle.kts"],
                         evolution_engine.patch_touched_files(patch))

    def test_new_file_lists_file(self):
        patch = ("diff --git a/nuevo.py b/nuevo.py\n"
                 "new file mode 100644\n"
                 "--- /dev/null\n"
                 "+++ b/nuevo.py\n"
                 "@@ -0,0 +1 @@\n"
                 "+x\n")
        self.assertEqual(["nuevo.py"],
                         evolution_engine.patch_touched_files(patch))

    def test_delete_of_forbidden_is_forbidden(self):
        patch = ("diff --git a/app/build.gradle.kts b/app/build.gradle.kts\n"
                 "--- a/app/build.gradle.kts\n"
                 "+++ /dev/null\n"
                 "@@ -1,1 +0,0 @@\n"
                 "-old\n")
        touched = evolution_engine.patch_touched_files(patch)
        self.assertTrue(any(evolution_engine.is_forbidden(t)
                            for t in touched))

    def test_dedup(self):
        patch = _unified_patch("a/b.py", "x\n", "y\n") + \
            _unified_patch("a/b.py", "y\n", "z\n")
        self.assertEqual(["a/b.py"],
                         evolution_engine.patch_touched_files(patch))


class TestPatchLimits(unittest.TestCase):
    def test_too_many_files_blocked(self):
        parts = []
        for i in range(evolution_engine.MAX_PATCH_FILES + 1):
            parts.append(_unified_patch(f"f{i}.py", "x\n", "y\n"))
        with self.assertRaises(SystemExit):
            evolution_engine.check_patch_limits("".join(parts))

    def test_oversize_blocked(self):
        big = "x" * (evolution_engine.MAX_PATCH_BYTES + 1)
        with self.assertRaises(SystemExit):
            evolution_engine.check_patch_limits(big)

    def test_small_patch_allowed(self):
        files = evolution_engine.check_patch_limits(
            _unified_patch("a/b.py", "x\n", "y\n"))
        self.assertEqual(["a/b.py"], files)


class TestNormalizePatchHunks(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        (self.base / "mod.py").write_text(
            "def clamp_amount(x: float) -> float:\n"
            '    """Cantidad DSP 0..1."""\n'
            "    return x\n")

    def tearDown(self):
        self._tmp.cleanup()

    def test_bare_hunk_gets_real_numbers(self):
        # El parche exacto que el consejo aprobó en el ensayo 0006
        # (hunk `@@` pelado): normalizado debe aplicar con git apply.
        patch = ("diff --git a/mod.py b/mod.py\n"
                 "--- a/mod.py\n"
                 "+++ b/mod.py\n"
                 "@@\n"
                 "-def clamp_amount(x: float) -> float:\n"
                 '-    """Cantidad DSP 0..1."""\n'
                 "-    return x\n"
                 "+def clamp_amount(x: float) -> float:\n"
                 '+    """Cantidad DSP 0..1."""\n'
                 "+    return max(0.0, min(1.0, x))\n")
        norm = evolution_engine.normalize_patch_hunks(patch, self.base)
        self.assertIsNotNone(norm)
        self.assertIn("@@ -1,3 +1,3 @@", norm)
        # Y git apply --check lo acepta sobre el archivo real.
        import subprocess
        p = self.base / "n.patch"
        p.write_text(norm)
        r = subprocess.run(["git", "apply", "--check", str(p)],
                           cwd=self.base, capture_output=True, text=True)
        self.assertEqual(0, r.returncode, r.stderr)

    def test_numbered_hunk_untouched(self):
        patch = _unified_patch("mod.py",
                               "def clamp_amount(x: float) -> float:\n"
                               '    """Cantidad DSP 0..1."""\n'
                               "    return x\n",
                               "def clamp_amount(x: float) -> float:\n"
                               '    """Cantidad DSP 0..1."""\n'
                               "    return max(0.0, min(1.0, x))\n")
        norm = evolution_engine.normalize_patch_hunks(patch, self.base)
        self.assertEqual(patch, norm)

    def test_ambiguous_context_returns_none(self):
        # El mismo contexto aparece 2 veces: no se adivina.
        (self.base / "mod.py").write_text("return x\nreturn x\n")
        patch = ("diff --git a/mod.py b/mod.py\n"
                 "--- a/mod.py\n"
                 "+++ b/mod.py\n"
                 "@@\n"
                 "-return x\n"
                 "+return 1.0\n")
        self.assertIsNone(
            evolution_engine.normalize_patch_hunks(patch, self.base))

    def test_missing_context_returns_none(self):
        patch = ("diff --git a/mod.py b/mod.py\n"
                 "--- a/mod.py\n"
                 "+++ b/mod.py\n"
                 "@@\n"
                 "-def inexistente():\n"
                 "+def inexistente2():\n")
        self.assertIsNone(
            evolution_engine.normalize_patch_hunks(patch, self.base))

    def test_missing_file_returns_none(self):
        patch = ("diff --git a/noexiste.py b/noexiste.py\n"
                 "--- a/noexiste.py\n"
                 "+++ b/noexiste.py\n"
                 "@@\n"
                 "-x\n"
                 "+y\n")
        self.assertIsNone(
            evolution_engine.normalize_patch_hunks(patch, self.base))


class TestValidateProposalDeletionGate(unittest.TestCase):
    def test_delete_forbidden_raises_before_worktree(self):
        # validate_proposal con un parche que borra app/build.gradle.kts:
        # el Gate 1 debe disparar SIN crear worktree ni tocar el árbol.
        # Se monkeypatchea PROPOSALS a un dir temporal (el repo real no
        # se toca: el gate dispara antes de cualquier operación git).
        import evolution_engine as eng
        with tempfile.TemporaryDirectory() as tmp:
            pdir = Path(tmp) / "0009-del"
            pdir.mkdir()
            patch = ("diff --git a/app/build.gradle.kts b/app/build.gradle.kts\n"
                     "--- a/app/build.gradle.kts\n"
                     "+++ /dev/null\n"
                     "@@ -1,1 +0,0 @@\n"
                     "-old\n")
            (pdir / "0009-del.patch").write_text(patch)
            old = eng.PROPOSALS
            eng.PROPOSALS = Path(tmp)
            try:
                with self.assertRaises(SystemExit) as ctx:
                    eng.validate_proposal("0009-del")
                self.assertIn("prohibidos", str(ctx.exception))
            finally:
                eng.PROPOSALS = old


if __name__ == "__main__":
    unittest.main()
