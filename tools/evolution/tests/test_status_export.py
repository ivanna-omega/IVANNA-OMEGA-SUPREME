"""Tests de la exportación de estado para la Vista de Dios
(tools/evolution/status_export.py).

La exportación es la pieza del repo que alimenta la Vista de Dios con
el interior del motor (propuestas, consejo, contención). Debe ser
verdad medible: sin secretos, sin inventar propuestas y sin fallar si
el gateway está caído.
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import status_export  # noqa: E402


class TestStatusExport(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.base = Path(self._tmp.name)
        self.props = self.base / "proposals"
        self.props.mkdir()
        pdir = self.props / "0006-rehearsal-seeded"
        pdir.mkdir()
        (pdir / "validation.json").write_text(json.dumps({
            "applied": True,
            "full_suite": {"passed": True},
            "files": ["tools/evolution/rehearsal_seed.py"],
            "tree_untouched": True,
        }))
        (pdir / "COUNCIL.md").write_text(
            "# Consejo 0006\n\n- **Veredicto:** APROBADO\n"
            "- **Rondas:** 1 (máx. 3)\n")
        (pdir / "REPORT.md").write_text(
            "# Propuesta 0006\n\n- **Veredicto:** VERDE — lista\n")

    def tearDown(self):
        self._tmp.cleanup()

    def test_proposal_row_reads_measured_verdict(self):
        rows = status_export._proposal_rows(self.props)
        self.assertEqual(1, len(rows))
        row = rows[0]
        self.assertEqual("0006-rehearsal-seeded", row["id"])
        self.assertTrue(row["applied"])
        self.assertEqual("passed", row["suite"])
        self.assertEqual("APROBADO", row["council"])
        self.assertEqual(1, row["council_rounds"])
        self.assertIn("VERDE", row["verdict"])

    def test_empty_proposals_dir_is_empty(self):
        empty = self.base / "empty"
        empty.mkdir()
        self.assertEqual([], status_export._proposal_rows(empty))

    def test_export_has_no_secrets(self):
        import subprocess
        repo = self.base / "repo"
        repo.mkdir()
        subprocess.run(["git", "init"], cwd=repo, capture_output=True)
        subprocess.run(["git", "config", "user.email", "t@t"],
                       cwd=repo, capture_output=True)
        subprocess.run(["git", "config", "user.name", "t"],
                       cwd=repo, capture_output=True)
        (repo / "f.txt").write_text("x\n")
        subprocess.run(["git", "add", "-A"], cwd=repo, capture_output=True)
        subprocess.run(["git", "commit", "-m", "base"],
                       cwd=repo, capture_output=True)
        st = status_export.export_status(repo=repo,
                                         proposals_dir=self.props,
                                         include_gateway=False)
        text = json.dumps(st)
        self.assertNotIn("ghp_", text)
        self.assertNotIn("ivanna-app-key", text)
        self.assertEqual(1, len(st["proposals"]))
        self.assertTrue(st["repo"]["branch"])  # rama real, la que sea
        self.assertIsNotNone(st["generated_at"])
        self.assertEqual("ddgw/tinfoil/gpt-oss-120b",
                         st["council"]["proposer"])

    def test_gateway_down_is_data_not_exception(self):
        # include_gateway=True contra el gateway real: en esta VM está
        # vivo, pero el contrato es que un gateway caído da ok=False,
        # nunca una excepción.
        st = status_export.export_status(
            repo=self.base, proposals_dir=self.props, include_gateway=True)
        self.assertIn("ok", st["gateway"])
        self.assertIn("ivanna_free_present", st["gateway"])


if __name__ == "__main__":
    unittest.main()
