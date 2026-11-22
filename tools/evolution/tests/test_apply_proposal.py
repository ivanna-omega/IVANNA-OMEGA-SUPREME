"""Tests del cableado aplicar-propuesta (tools/evolution/apply_proposal.py).

El eslabón que faltaba (2026-11-15): entre validar en scratch y
publicar no había ningún paso que aplicara el parche al árbol y lo
dejara listo para commit. Estos tests cubren las puertas fail-closed
(consejo, ya-aplicado, contención, re-validación) con repos git
temporales; nunca tocan el repo real.
"""
import difflib
import json
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import apply_proposal  # noqa: E402


def _unified_patch(rel_path, old_text, new_text, new_file=False):
    old = "" if new_file else old_text
    fromfile = "/dev/null" if new_file else f"a/{rel_path}"
    return "".join(difflib.unified_diff(
        old.splitlines(keepends=True),
        new_text.splitlines(keepends=True),
        fromfile=fromfile, tofile=f"b/{rel_path}"))


class _FakeEngine:
    """Motor mínimo para los tests: puertas + validación enlatada."""

    def __init__(self, validation_result):
        self._validation_result = validation_result

    def check_patch_limits(self, patch_text):
        return list(apply_proposal._parse_patch_sections(patch_text).keys())

    def is_forbidden(self, rel):
        return rel.startswith("app/build.gradle.kts") or "SECRET" in rel.upper()

    def validate_proposal(self, pid):
        return dict(self._validation_result)


GREEN = {"applied": True, "full_suite": {"passed": True},
         "python_checks": {"unittest": {"passed": True}},
         "tree_untouched": True}
RED = {"applied": False, "error": "no aplica"}


class _TempRepo(unittest.TestCase):
    def setUp(self):
        import tempfile
        self._td = tempfile.TemporaryDirectory()
        self.repo = Path(self._td.name) / "repo"
        self.repo.mkdir()
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)
        subprocess.run(["git", "config", "user.email", "t@t"],
                       cwd=self.repo, check=True)
        subprocess.run(["git", "config", "user.name", "t"],
                       cwd=self.repo, check=True)
        (self.repo / "a.py").write_text("x = 1\n")
        subprocess.run(["git", "add", "."], cwd=self.repo, check=True)
        subprocess.run(["git", "commit", "-qm", "init"], cwd=self.repo,
                       check=True)
        self.props = self.repo / "tools" / "evolution" / "proposals"
        self._orig_engine = apply_proposal._engine
        self._orig_audit = apply_proposal._audit
        apply_proposal._audit = lambda: _FakeAudit()

    def tearDown(self):
        apply_proposal._engine = self._orig_engine
        apply_proposal._audit = self._orig_audit
        self._td.cleanup()

    def _proposal(self, pid, patch_text, validation=None, council=None):
        pdir = self.props / pid
        pdir.mkdir(parents=True)
        (pdir / f"{pid}.patch").write_text(patch_text)
        (pdir / "validation.json").write_text(
            json.dumps(validation if validation is not None else GREEN))
        if council is not None:
            (pdir / "COUNCIL.md").write_text(council)
        return pdir

    def _run_apply(self, pid, **kw):
        apply_proposal._engine = lambda: _FakeEngine(GREEN)
        return apply_proposal.apply_proposal(
            pid, repo=self.repo, proposals_dir=self.props, **kw)


class _FakeAudit:
    def __init__(self):
        self.events = []

    def record(self, event, detail):
        self.events.append((event, detail))


class TestCouncilApproval(_TempRepo):
    def test_aprobado_en_council(self):
        pdir = self.props / "p1"
        pdir.mkdir(parents=True)
        (pdir / "COUNCIL.md").write_text(
            "# Consejo p1\n\n- **Veredicto:** APROBADO\n")
        ok, ev = apply_proposal.council_approval(pdir)
        self.assertTrue(ok)
        self.assertIn("COUNCIL.md", ev)

    def test_rechazado_en_council(self):
        pdir = self.props / "p1"
        pdir.mkdir(parents=True)
        (pdir / "COUNCIL.md").write_text(
            "# Consejo p1\n\n- **Veredicto:** RECHAZADO\n")
        ok, _ = apply_proposal.council_approval(pdir)
        self.assertFalse(ok)

    def test_sin_registro(self):
        pdir = self.props / "p1"
        pdir.mkdir(parents=True)
        ok, ev = apply_proposal.council_approval(pdir)
        self.assertFalse(ok)
        self.assertIn("sin registro", ev)


class TestVerdictGreen(_TempRepo):
    def test_verde(self):
        self.assertTrue(apply_proposal.is_verdict_green(GREEN))

    def test_rojo_si_no_aplico(self):
        self.assertFalse(apply_proposal.is_verdict_green(RED))

    def test_rojo_si_suite_fallo(self):
        bad = dict(GREEN)
        bad["full_suite"] = {"passed": False}
        self.assertFalse(apply_proposal.is_verdict_green(bad))


class TestAlreadyApplied(_TempRepo):
    def test_exact_reverse_check(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        (self.repo / "a.py").write_text("x = 2\n")
        state, _ = apply_proposal.patch_already_applied(patch, self.repo)
        self.assertEqual("exact", state)

    def test_content_drift(self):
        # El contenido está pero el contexto derivó (líneas extra arriba).
        patch = _unified_patch("a.py", "x = 1\n", "x = 1\ny = 2\n")
        (self.repo / "a.py").write_text("# comentario nuevo\nx = 1\ny = 2\n")
        state, _ = apply_proposal.patch_already_applied(patch, self.repo)
        self.assertEqual("content", state)

    def test_absent(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        state, _ = apply_proposal.patch_already_applied(patch, self.repo)
        self.assertEqual("absent", state)

    def test_moot_target_gone(self):
        # El parche modifica un archivo que ya no existe y su contenido
        # viejo tampoco está: el objetivo fue eliminado por otra vía.
        patch = _unified_patch("gone.py", "old()\n", "new()\n")
        state, _ = apply_proposal.patch_already_applied(patch, self.repo)
        self.assertEqual("moot", state)

    def test_partial(self):
        patch = (_unified_patch("a.py", "x = 1\n", "x = 1\ny = 2\n")
                 + _unified_patch("b.py", "z = 1\n", "z = 2\n"))
        (self.repo / "a.py").write_text("x = 1\ny = 2\n")
        (self.repo / "b.py").write_text("z = 1\n")
        subprocess.run(["git", "add", "."], cwd=self.repo, check=True)
        state, _ = apply_proposal.patch_already_applied(patch, self.repo)
        self.assertEqual("partial", state)


class TestApplyEndToEnd(_TempRepo):
    def test_apply_ok_leaves_tree_dirty_without_commit(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        self._proposal("p1", patch, council="# C\n\n- **Veredicto:** APROBADO\n")
        rep = self._run_apply("p1")
        self.assertEqual("applied", rep["status"])
        self.assertEqual("x = 2\n", (self.repo / "a.py").read_text())
        # Sin commit: el historial sigue con 1 commit.
        log = subprocess.run(["git", "log", "--oneline"], cwd=self.repo,
                             capture_output=True, text=True).stdout
        self.assertEqual(1, len(log.strip().splitlines()))
        self.assertTrue((self.props / "p1" / "apply_report.json").exists())

    def test_already_applied_noop(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        (self.repo / "a.py").write_text("x = 2\n")
        subprocess.run(["git", "add", "."], cwd=self.repo, check=True)
        subprocess.run(["git", "commit", "-qm", "ya"], cwd=self.repo,
                       check=True)
        self._proposal("p1", patch, council="# C\n\n- **Veredicto:** APROBADO\n")
        rep = self._run_apply("p1")
        self.assertEqual("already_applied", rep["status"])
        # a.py no quedó modificado (la propuesta vive sin seguimiento
        # en tools/, así que el status global nunca está vacío).
        status = subprocess.run(["git", "status", "--porcelain", "--", "a.py"],
                                cwd=self.repo, capture_output=True,
                                text=True).stdout.strip()
        self.assertEqual("", status)

    def test_no_council_record_refused_without_override(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        self._proposal("p1", patch)  # sin COUNCIL.md
        rep = self._run_apply("p1")
        self.assertEqual("refused", rep["status"])
        self.assertIn("consejo", rep["reason"])
        self.assertEqual("x = 1\n", (self.repo / "a.py").read_text())

    def test_no_council_record_allowed_with_override(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        self._proposal("p1", patch)  # sin COUNCIL.md
        rep = self._run_apply("p1", allow_no_council_record=True)
        self.assertEqual("applied", rep["status"])
        self.assertTrue(rep.get("council_override"))

    def test_forbidden_refused(self):
        patch = _unified_patch("app/build.gradle.kts", "a\n", "b\n")
        self._proposal("p1", patch, council="# C\n\n- **Veredicto:** APROBADO\n")
        rep = self._run_apply("p1")
        self.assertEqual("refused", rep["status"])
        self.assertIn("prohibido", rep["reason"])

    def test_red_validation_refused(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        self._proposal("p1", patch, council="# C\n\n- **Veredicto:** APROBADO\n")
        apply_proposal._engine = lambda: _FakeEngine(RED)
        rep = apply_proposal.apply_proposal(
            "p1", repo=self.repo, proposals_dir=self.props)
        self.assertEqual("refused", rep["status"])
        self.assertEqual("x = 1\n", (self.repo / "a.py").read_text())

    def test_dry_run_does_not_touch_tree(self):
        patch = _unified_patch("a.py", "x = 1\n", "x = 2\n")
        self._proposal("p1", patch, council="# C\n\n- **Veredicto:** APROBADO\n")
        rep = self._run_apply("p1", dry_run=True)
        self.assertEqual("dry_run_green", rep["status"])
        self.assertEqual("x = 1\n", (self.repo / "a.py").read_text())


if __name__ == "__main__":
    unittest.main()
