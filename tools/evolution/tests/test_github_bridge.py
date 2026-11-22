"""Tests del puente GitHub (v2.5.445): contención + potencia + publicación.

La guardia de contención (directiva de Esse 2026-11-14 17:21) bloquea
cualquier acción fuera del dominio IVANNA; la potencia (directiva
2026-11-14 17:24) permite reescribir CUALQUIER código del producto
dentro del repo. Los tests corren contra un repo git temporal, nunca
contra el repo real.
"""
import difflib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import github_bridge  # noqa: E402
from github_bridge import ContainmentGuard, ContainmentViolation  # noqa: E402


def _unified_patch(rel_path, old_text, new_text):
    return "".join(difflib.unified_diff(
        old_text.splitlines(keepends=True),
        new_text.splitlines(keepends=True),
        fromfile=f"a/{rel_path}", tofile=f"b/{rel_path}"))


class BridgeTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        # El registro auditable va al tmp: las violaciones de estos tests
        # no deben ensuciar el audit.jsonl real del operador.
        self._old_audit = os.environ.get("IVANNA_AUDIT_FILE")
        os.environ["IVANNA_AUDIT_FILE"] = str(
            Path(self._tmp.name) / "audit.jsonl")
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        self._git("init")
        self._git("config", "user.email", "test@ivanna.local")
        self._git("config", "user.name", "Ivanna Test")
        # Archivo de producto (DSP) + las 6 fuentes de versión (2.5.444).
        (self.repo / "app/src/main/cpp/spatial").mkdir(parents=True)
        (self.repo / "app/src/main/cpp/spatial/foo.cpp").write_text(
            "// foo v1\nint foo() { return 1; }\n")
        (self.repo / "magisk_module").mkdir()
        (self.repo / "version.properties").write_text(
            "versionName=2.5.444\nversionCode=735\n")
        (self.repo / "magisk_module/module.prop").write_text(
            "id=ivanna_omega_supreme\nversion=v2.5.444\nversionCode=735\n"
            "description=v2.5.444: test\n")
        (self.repo / "magisk_module/update.json").write_text(json.dumps({
            "version": "v2.5.444", "versionCode": 735,
            "zipUrl": "https://github.com/x/releases/download/v2.5.444/"
                      "ivanna_omega_supreme_v2.5.444.zip"}, indent=2) + "\n")
        (self.repo / "CHANGELOG.md").write_text(
            "# v2.5.444 (735) - 2026-11-14\n\n- old\n")
        (self.repo / "README.md").write_text(
            "### Versiones recientes\n\n| **v2.5.444** (735) | Old release. |\n")
        tidal = self.repo / "app/src/main/java/com/ivanna/omega/tidal"
        tidal.mkdir(parents=True)
        (tidal / "TidalApiClient.kt").write_text(
            '            "User-Agent" to "IVANNA-Tidal/2.5.444 (Android; '
            'com.ivanna.omega)"\n')
        self._git("add", "-A")
        self._git("commit", "-m", "test: base")
        self.proposals = Path(self._tmp.name) / "proposals"
        self.proposals.mkdir()
        self.guard = ContainmentGuard(self.repo)

    def tearDown(self):
        if self._old_audit is None:
            os.environ.pop("IVANNA_AUDIT_FILE", None)
        else:
            os.environ["IVANNA_AUDIT_FILE"] = self._old_audit
        self._tmp.cleanup()

    def _git(self, *args, **kwargs):
        return subprocess.run(["git", *args], cwd=self.repo,
                              capture_output=True, text=True, check=True,
                              **kwargs)

    def _make_proposal(self, pid, patch_text, green=True):
        pdir = self.proposals / pid
        pdir.mkdir(parents=True)
        (pdir / f"{pid}.patch").write_text(patch_text)
        result = {"applied": True, "full_suite": {"passed": True},
                  "python_checks": {"unittest": {"passed": True}},
                  "tree_untouched": True} if green else {"applied": False}
        (pdir / "validation.json").write_text(json.dumps(result))
        return pdir


class TestContainmentGuard(BridgeTestBase):
    def test_patch_escape_blocked(self):
        patch = _unified_patch("../escape.txt", "", "x\n")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch(patch)

    def test_patch_absolute_path_blocked(self):
        patch = ("--- a//etc/passwd\n+++ b//etc/passwd\n@@ -0,0 +1 @@\n+x\n")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch(patch)

    def test_patch_forbidden_file_blocked(self):
        patch = _unified_patch(
            "app/src/main/java/com/ivanna/omega/security/IntegrityGate.kt",
            "old\n", "new\n")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch(patch)

    def test_patch_delete_forbidden_file_blocked(self):
        # Borrado de una puerta (--- a/X + +++ /dev/null): el guard lo
        # resuelve por la ruta vieja. Antes de la auditoría 2026-11-14
        # el Gate 1 del motor no lo veía; el puente sí lo bloquea.
        patch = ("diff --git a/app/build.gradle.kts b/app/build.gradle.kts\n"
                 "--- a/app/build.gradle.kts\n"
                 "+++ /dev/null\n"
                 "@@ -1,1 +0,0 @@\n"
                 "-old\n")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch(patch)

    def test_patch_too_many_files_blocked(self):
        parts = []
        for i in range(51):
            parts.append(_unified_patch(f"f{i}.py", "x\n", "y\n"))
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch("".join(parts))

    def test_patch_workflow_blocked(self):
        patch = _unified_patch(".github/workflows/ci.yml", "old\n", "new\n")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch(patch)

    def test_patch_secret_blocked(self):
        patch = _unified_patch("app/src/main/cpp/spatial/foo.cpp",
                               "// foo v1\n", "// ghp_" + "A" * 30 + "\n")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_patch(patch)

    def test_patch_dsp_file_allowed(self):
        # POTENCIA (17:24): rediseñar un módulo DSP dentro de IVANNA sí
        # está permitido — la contención es afuera, no adentro.
        patch = _unified_patch("app/src/main/cpp/spatial/foo.cpp",
                               "// foo v1\nint foo() { return 1; }\n",
                               "// foo v2\nint foo() { return 2; }\n")
        files = self.guard.check_patch(patch)
        self.assertEqual(["app/src/main/cpp/spatial/foo.cpp"], files)

    def test_remote_mismatch_blocked(self):
        with self.assertRaises(ContainmentViolation):
            self.guard.check_remote("https://github.com/other/repo.git")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_remote("https://evil.example/ivanna.git")

    def test_remote_ivanna_allowed(self):
        self.guard.check_remote(
            "https://github.com/luisurielpimentelperez814-design/"
            "IVANNA-OMEGA-SUPREME.git")
        self.guard.check_remote(
            "git@github.com:luisurielpimentelperez814-design/"
            "IVANNA-OMEGA-SUPREME.git")

    def test_command_allowlist(self):
        self.guard.check_command(["git", "push", "origin", "main"])
        self.guard.check_command(["git", "status", "--porcelain"])
        self.guard.check_command(["gh", "run", "list", "--branch", "main",
                                  "--limit", "5", "--json", "databaseId"])
        with self.assertRaises(ContainmentViolation):
            self.guard.check_command(["git", "push", "other", "main"])
        with self.assertRaises(ContainmentViolation):
            self.guard.check_command(["git", "commit", "--amend", "-m", "x"])
        with self.assertRaises(ContainmentViolation):
            self.guard.check_command(["curl", "https://example.com"])
        with self.assertRaises(ContainmentViolation):
            self.guard.check_command(["gh", "run", "list", "--repo", "other/x"])
        with self.assertRaises(ContainmentViolation):
            self.guard.check_command(["rm", "-rf", "/"])

    def test_path_outside_repo_blocked(self):
        with self.assertRaises(ContainmentViolation):
            self.guard.check_path("/etc/passwd")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_path("../fuera.txt")

    def test_host_allowlist(self):
        self.guard.check_host("github.com")
        self.guard.check_host("localhost")
        with self.assertRaises(ContainmentViolation):
            self.guard.check_host("evil.example")


class TestVerdictAndBump(BridgeTestBase):
    def test_verdict_green(self):
        self.assertTrue(github_bridge.is_verdict_green(
            {"applied": True, "full_suite": {"passed": True},
             "python_checks": {"unittest": {"passed": True}}}))
        self.assertTrue(github_bridge.is_verdict_green(
            {"applied": True, "full_suite": {"skipped": True}}))
        self.assertFalse(github_bridge.is_verdict_green({"applied": False}))
        self.assertFalse(github_bridge.is_verdict_green(
            {"applied": True, "full_suite": {"passed": False}}))
        self.assertFalse(github_bridge.is_verdict_green(
            {"applied": True, "full_suite": {"passed": True},
             "python_checks": {"unittest": {"passed": False}}}))

    def test_bump_updates_six_sources(self):
        nueva, code = github_bridge.bump_version(self.repo, "test bump")
        self.assertEqual("2.5.445", nueva)
        self.assertEqual(736, code)
        props = (self.repo / "version.properties").read_text()
        self.assertIn("versionName=2.5.445", props)
        self.assertIn("versionCode=736", props)
        mod = (self.repo / "magisk_module/module.prop").read_text()
        self.assertIn("version=v2.5.445", mod)
        self.assertIn("versionCode=736", mod)
        upd = json.loads((self.repo / "magisk_module/update.json").read_text())
        self.assertEqual("v2.5.445", upd["version"])
        self.assertEqual(736, upd["versionCode"])
        self.assertIn("v2.5.445/ivanna_omega_supreme_v2.5.445.zip",
                      upd["zipUrl"])
        cl = (self.repo / "CHANGELOG.md").read_text()
        self.assertTrue(cl.startswith("# v2.5.445 (736) - "))
        readme = (self.repo / "README.md").read_text()
        self.assertIn("| **v2.5.445** (736) | test bump |", readme)
        self.assertLess(readme.index("v2.5.445"), readme.index("v2.5.444"))
        ua = (self.repo / "app/src/main/java/com/ivanna/omega/tidal"
              / "TidalApiClient.kt").read_text()
        self.assertIn("IVANNA-Tidal/2.5.445", ua)


class TestPublish(BridgeTestBase):
    def _foo_patch(self):
        return _unified_patch(
            "app/src/main/cpp/spatial/foo.cpp",
            "// foo v1\nint foo() { return 1; }\n",
            "// foo v2\nint foo() { return 2; }\n")

    def test_publish_dry_run_reaches_commit(self):
        self._make_proposal("0009-test", self._foo_patch(), green=True)
        rep = github_bridge.publish_proposal(
            self.repo, self.proposals, "0009-test",
            "fix(spatial): foo devuelve 2", execute=False)
        self.assertTrue(rep["published"])
        self.assertTrue(rep["dry_run"])
        log = self._git("log", "--oneline").stdout
        self.assertIn("fix(spatial): foo devuelve 2", log)
        self.assertIn("chore(release): bump a v2.5.445 (vc 736)", log)
        self.assertEqual("2.5.445",
                         (self.repo / "version.properties").read_text()
                         .split("versionName=")[1].splitlines()[0])
        self.assertIn("return 2;",
                      (self.repo / "app/src/main/cpp/spatial/foo.cpp")
                      .read_text())

    def test_publish_refuses_red(self):
        self._make_proposal("0009-test", self._foo_patch(), green=False)
        rep = github_bridge.publish_proposal(
            self.repo, self.proposals, "0009-test",
            "fix(spatial): foo devuelve 2", execute=False)
        self.assertFalse(rep["published"])
        self.assertIn("refused", rep)
        log = self._git("log", "--oneline").stdout
        self.assertNotIn("fix(spatial)", log)

    def test_publish_refuses_dirty_tree(self):
        self._make_proposal("0009-test", self._foo_patch(), green=True)
        # Cambio trackeado ajeno a la reparación: la puerta lo bloquea.
        (self.repo / "README.md").write_text("cambio ajeno\n")
        with self.assertRaises(ContainmentViolation):
            github_bridge.publish_proposal(
                self.repo, self.proposals, "0009-test",
                "fix(spatial): foo devuelve 2", execute=False)

    def test_publish_refuses_bad_message(self):
        self._make_proposal("0009-test", self._foo_patch(), green=True)
        with self.assertRaises(SystemExit):
            github_bridge.publish_proposal(
                self.repo, self.proposals, "0009-test",
                "arreglo foo", execute=False)

    def test_publish_refuses_forbidden_patch(self):
        bad = _unified_patch(
            "app/src/main/java/com/ivanna/omega/security/X.kt",
            "old\n", "new\n")
        self._make_proposal("0009-test", bad, green=True)
        with self.assertRaises(ContainmentViolation):
            github_bridge.publish_proposal(
                self.repo, self.proposals, "0009-test",
                "fix(security): x", execute=False)


if __name__ == "__main__":
    unittest.main()
