"""Tests del registro auditable, la rama de publicación y el gancho pre-push.

La barrera crítica de publicación debe aplicarla git, fuera del proceso
del agente: el gancho pre-push bloquea el push a origin/main sin el
archivo de aprobación del operador. Estos tests lo demuestran de verdad
(push real a un remote bare local), no con mocks del gancho.
"""
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import audit  # noqa: E402
import install_publish_hook  # noqa: E402
import github_bridge  # noqa: E402
from github_bridge import ContainmentGuard, ContainmentViolation  # noqa: E402


class AuditTestBase(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.audit_file = Path(self._tmp.name) / "audit.jsonl"
        os.environ["IVANNA_AUDIT_FILE"] = str(self.audit_file)

    def tearDown(self):
        os.environ.pop("IVANNA_AUDIT_FILE", None)
        self._tmp.cleanup()


class TestAuditLog(AuditTestBase):
    def test_record_writes_jsonl(self):
        audit.record("publish_decision", "prueba")
        lines = self.audit_file.read_text().splitlines()
        self.assertEqual(1, len(lines))
        row = json.loads(lines[0])
        self.assertEqual("publish_decision", row["event"])
        self.assertIn("prueba", row["detail"])
        self.assertIn("ts", row)

    def test_record_never_raises(self):
        os.environ["IVANNA_AUDIT_FILE"] = "/no/existe/audit.jsonl"
        audit.record("publish_decision", "prueba")  # no debe lanzar

    def test_tail_reads_last_lines(self):
        for i in range(5):
            audit.record("e", f"n{i}")
        rows = audit.tail(2)
        self.assertEqual(2, len(rows))
        self.assertEqual("n4", rows[-1]["detail"])

    def test_tail_missing_file_is_empty(self):
        self.assertEqual([], audit.tail())

    def test_violation_auto_records(self):
        with self.assertRaises(ContainmentViolation):
            raise ContainmentViolation("prueba de auto-registro")
        rows = audit.tail(5)
        self.assertTrue(any(r["event"] == "containment_violation"
                            and "prueba de auto-registro" in r["detail"]
                            for r in rows))


class HookTestBase(AuditTestBase):
    def setUp(self):
        super().setUp()
        self.repo = Path(self._tmp.name) / "repo"
        self.bare = Path(self._tmp.name) / "bare.git"
        self.repo.mkdir()
        self._git("init", cwd=self.repo)
        self._git("config", "user.email", "test@ivanna.local", cwd=self.repo)
        self._git("config", "user.name", "Ivanna Test", cwd=self.repo)
        (self.repo / "f.txt").write_text("v1\n")
        self._git("add", "-A", cwd=self.repo)
        self._git("commit", "-m", "test: base", cwd=self.repo)
        self._git("branch", "-M", "main", cwd=self.repo)
        self._git("init", "--bare", str(self.bare))
        self._git("remote", "add", "origin", str(self.bare), cwd=self.repo)
        self.head = self._git("rev-parse", "HEAD", cwd=self.repo).stdout.strip()
        self.branch = self._git("rev-parse", "--abbrev-ref", "HEAD",
                                cwd=self.repo).stdout.strip()

    def _git(self, *args, cwd=None):
        return subprocess.run(["git", *args], cwd=cwd or self.repo,
                              capture_output=True, text=True, check=True)


class TestPublishBranch(HookTestBase):
    def test_non_main_branch_blocked(self):
        guard = ContainmentGuard(self.repo)
        if self.branch == "main":
            self._git("checkout", "-b", "feature", cwd=self.repo)
            with self.assertRaises(ContainmentViolation):
                guard.check_publish_branch()
        else:
            with self.assertRaises(ContainmentViolation):
                guard.check_publish_branch()

    def test_main_branch_allowed(self):
        self._git("checkout", "-B", "main", cwd=self.repo)
        guard = ContainmentGuard(self.repo)
        self.assertEqual("main", guard.check_publish_branch())


class TestPrePushHook(HookTestBase):
    def _install_hook(self):
        old_repo, old_hook = install_publish_hook.REPO, install_publish_hook.HOOK
        install_publish_hook.REPO = self.repo
        install_publish_hook.HOOK = self.repo / ".git" / "hooks" / "pre-push"
        try:
            self.assertEqual(0, install_publish_hook.install())
            self.assertEqual(0, install_publish_hook.status())
        finally:
            install_publish_hook.REPO, install_publish_hook.HOOK = old_repo, old_hook

    def _push_main(self):
        return subprocess.run(["git", "push", "origin", "main"],
                              cwd=self.repo, capture_output=True, text=True)

    def test_push_to_main_blocked_without_approval(self):
        self._install_hook()
        r = self._push_main()
        self.assertNotEqual(0, r.returncode)
        self.assertIn("BLOQUEADO", r.stderr)
        # El remote bare NO recibió main: nada se publicó.
        bare_heads = subprocess.run(
            ["git", "--git-dir", str(self.bare), "rev-parse", "--verify",
             "refs/heads/main"], capture_output=True, text=True)
        self.assertNotEqual(0, bare_heads.returncode)

    def test_push_to_main_allowed_with_approval(self):
        self._install_hook()
        (self.repo / ".ivanna-publish-approval").write_text(
            f"proposal=0006-rehearsal-seeded\nhead={self.head}\n")
        r = self._push_main()
        self.assertEqual(0, r.returncode, r.stderr)
        bare_head = subprocess.run(
            ["git", "--git-dir", str(self.bare), "rev-parse", "refs/heads/main"],
            capture_output=True, text=True, check=True).stdout.strip()
        self.assertEqual(self.head, bare_head)

    def test_push_to_main_blocked_with_stale_approval(self):
        self._install_hook()
        (self.repo / ".ivanna-publish-approval").write_text(
            "proposal=0006-rehearsal-seeded\nhead=0000000000000000000000000000000000000000\n")
        r = self._push_main()
        self.assertNotEqual(0, r.returncode)
        self.assertIn("BLOQUEADO", r.stderr)

    def test_push_to_other_branch_not_blocked(self):
        self._install_hook()
        self._git("checkout", "-b", "feature", cwd=self.repo)
        r = subprocess.run(["git", "push", "origin", "feature"],
                           cwd=self.repo, capture_output=True, text=True)
        self.assertEqual(0, r.returncode, r.stderr)


if __name__ == "__main__":
    unittest.main()
