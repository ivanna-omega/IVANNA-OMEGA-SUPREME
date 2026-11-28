#!/usr/bin/env python3
"""Tests de tools/evolution/check_repro.py (C12).

Solo stdlib. Los tests de ejecución usan un repo temporal con un
script `pass.py`/`fail.py` mínimo y un fixture real con su sha256; nada
toca el árbol real ni la red.
"""

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import check_repro  # noqa: E402


def _sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


class _TempRepo(unittest.TestCase):
    """Repo temporal con proposals/<pid>/repro.json + script + fixture."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self.tmp.name)
        (self.repo / "tools" / "evolution" / "proposals").mkdir(parents=True)
        (self.repo / "scripts").mkdir()
        (self.repo / "tools" / "evolution" / "fixtures").mkdir(parents=True)
        (self.repo / "scripts" / "pass.py").write_text(
            "import sys; print('TODO BIEN'); sys.exit(0)\n")
        (self.repo / "scripts" / "fail.py").write_text(
            "import sys; print('TODO MAL'); sys.exit(3)\n")
        self.fixture = self.repo / "tools" / "evolution" / "fixtures" / "f.json"
        self.fixture.write_text("contenido real del fixture\n")

    def tearDown(self):
        self.tmp.cleanup()

    def _proposal(self, pid, repro):
        pdir = self.repo / "tools" / "evolution" / "proposals" / pid
        pdir.mkdir(parents=True, exist_ok=True)
        (pdir / "repro.json").write_text(json.dumps(repro))
        return pdir

    def _good_repro(self, **over):
        repro = {
            "proposal": "0001-x",
            "version": 1,
            "seed": 42,
            "commands": [{
                "name": "S4",
                "command": "python3 scripts/pass.py",
                "seed": 42,
                "fixtures": [{"path": "tools/evolution/fixtures/f.json",
                              "sha256": _sha(self.fixture)}],
                "expected": "TODO BIEN",
            }],
        }
        repro.update(over)
        return repro

    def _check(self, pid="0001-x"):
        return check_repro.check_proposal(pid, repo=self.repo)


class TestCheckReproHappy(_TempRepo):
    def test_pasa_con_ejemplo_valido(self):
        self._proposal("0001-x", self._good_repro())
        ok, messages, pending = self._check()
        self.assertTrue(ok, messages)
        self.assertIn("paso S4: OK", " ".join(messages))

    def test_expected_como_lista_todos_los_literales(self):
        repro = self._good_repro()
        repro["commands"][0]["expected"] = ["TODO", "BIEN"]
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertTrue(ok, messages)

    def test_expected_como_lista_falta_un_literal(self):
        repro = self._good_repro()
        repro["commands"][0]["expected"] = ["TODO", "AUSENTE"]
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("AUSENTE", " ".join(messages))

    def test_forma_un_solo_comando(self):
        repro = self._good_repro()
        cmd = repro.pop("commands")[0]
        repro.update({k: cmd[k] for k in
                      ("command", "seed", "fixtures", "expected")})
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertTrue(ok, messages)

    def test_no_run_solo_esquema_y_fixtures(self):
        self._proposal("0001-x", self._good_repro())
        ok, messages, _ = check_repro.check_proposal(
            "0001-x", repo=self.repo, run=False)
        self.assertTrue(ok, messages)


class TestCheckReproFails(_TempRepo):
    def test_falta_repro_json(self):
        (self.repo / "tools" / "evolution" / "proposals" / "0001-x").mkdir()
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("falta repro.json", " ".join(messages))

    def test_fixture_inexistente(self):
        repro = self._good_repro()
        repro["commands"][0]["fixtures"] = [
            {"path": "tools/evolution/fixtures/no-existe.json",
             "sha256": "0" * 64}]
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("inexistente", " ".join(messages))

    def test_sha256_distinto(self):
        repro = self._good_repro()
        repro["commands"][0]["fixtures"] = [
            {"path": "tools/evolution/fixtures/f.json",
             "sha256": "0" * 64}]
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("sha256 no coincide", " ".join(messages))

    def test_expected_ausente_en_salida(self):
        repro = self._good_repro()
        repro["commands"][0]["expected"] = "ESTO NO SALE"
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("NO contiene", " ".join(messages))

    def test_exit_code_distinto(self):
        repro = self._good_repro()
        repro["commands"][0]["command"] = "python3 scripts/fail.py"
        repro["commands"][0]["expected"] = "TODO MAL"
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("exit 3", " ".join(messages))

    def test_seed_no_entero(self):
        repro = self._good_repro(seed="no-entero")
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("seed", " ".join(messages))

    def test_command_y_commands_a_la_vez(self):
        repro = self._good_repro()
        repro["command"] = "python3 scripts/pass.py"
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("ambiguo", " ".join(messages))

    def test_proposal_no_coincide_con_directorio(self):
        repro = self._good_repro(proposal="otro-id")
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("debe ser '0001-x'", " ".join(messages))


class TestCheckReproPending(_TempRepo):
    def test_pendiente_sin_blocked_by_falla(self):
        repro = self._good_repro()
        repro["pending_commands"] = [{
            "name": "C7", "command": "python3 scripts/no-existe.py",
            "expected": "PROMOTED"}]
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("blocked_by", " ".join(messages))

    def test_pendiente_con_bloqueo_no_ejecuta_y_pasa(self):
        repro = self._good_repro()
        repro["pending_commands"] = [{
            "name": "C7", "command": "python3 scripts/no-existe.py",
            "expected": "PROMOTED",
            "blocked_by": "el script no existe aún"}]
        self._proposal("0001-x", repro)
        ok, messages, pending = self._check()
        self.assertTrue(ok, messages)
        self.assertEqual(1, len(pending))

    def test_pendiente_cuyo_programa_ya_existe_falla(self):
        # Tripwire de promoción: el bloqueo caducó.
        repro = self._good_repro()
        repro["pending_commands"] = [{
            "name": "C7", "command": "python3 scripts/pass.py",
            "expected": "TODO BIEN",
            "blocked_by": "bloqueo ya inexistente"}]
        self._proposal("0001-x", repro)
        ok, messages, _ = self._check()
        self.assertFalse(ok)
        self.assertIn("promueve el paso", " ".join(messages))


class TestChangedProposals(unittest.TestCase):
    def test_detecta_propuesta_tocada_en_el_diff(self):
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d)
            env = {**os.environ, "GIT_AUTHOR_NAME": "t",
                   "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                   "GIT_COMMITTER_EMAIL": "t@t"}
            subprocess.run(["git", "init", "-q"], cwd=repo, env=env,
                           check=True)
            (repo / "x.txt").write_text("base\n")
            subprocess.run(["git", "add", "."], cwd=repo, env=env,
                           check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=repo,
                           env=env, check=True)
            base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                                  capture_output=True, text=True,
                                  env=env, check=True).stdout.strip()
            pdir = repo / "tools" / "evolution" / "proposals" / "0034-x"
            pdir.mkdir(parents=True)
            (pdir / "repro.json").write_text("{}\n")
            subprocess.run(["git", "add", "."], cwd=repo, env=env,
                           check=True)
            subprocess.run(["git", "commit", "-qm", "nueva propuesta"],
                           cwd=repo, env=env, check=True)
            self.assertEqual(["0034-x"],
                             check_repro.changed_proposals(repo, base))

    def test_sin_cambios_no_hay_propuestas(self):
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d)
            env = {**os.environ, "GIT_AUTHOR_NAME": "t",
                   "GIT_AUTHOR_EMAIL": "t@t", "GIT_COMMITTER_NAME": "t",
                   "GIT_COMMITTER_EMAIL": "t@t"}
            subprocess.run(["git", "init", "-q"], cwd=repo, env=env,
                           check=True)
            (repo / "x.txt").write_text("base\n")
            subprocess.run(["git", "add", "."], cwd=repo, env=env,
                           check=True)
            subprocess.run(["git", "commit", "-qm", "base"], cwd=repo,
                           env=env, check=True)
            base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo,
                                  capture_output=True, text=True,
                                  env=env, check=True).stdout.strip()
            self.assertEqual([], check_repro.changed_proposals(repo, base))


class TestCanonicalExample(unittest.TestCase):
    """El ejemplo canónico v1.1 del repo real pasa el chequeo."""

    def test_ejemplo_v11_pasa(self):
        repo = Path(__file__).resolve().parents[3]
        ok, messages, pending = check_repro.check_proposal(
            "0034-v1.1-ab-candidate", repo=repo)
        self.assertTrue(ok, messages)
        self.assertEqual([], pending)
        joined = " ".join(messages)
        self.assertIn("paso C7: OK", joined)
        self.assertIn("paso S4: OK", joined)


if __name__ == "__main__":
    unittest.main()
