"""Tests de la integración consejo → draft (v2.5.445).

Directiva de Esse 2026-11-14 17:46 ("hazlo real ya al producto"): la
reescritura pasa por el consejo multi-modelo ANTES de validar en scratch
y publicar. Estos tests usan un LLM falso (mock) para no depender del
gateway: verifican que draft_patch consulta al consejo, que un parche
acordado sale y que un rechazo (reviewer o arbiter) es fail-closed
(sin draft, sin parche).
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

import council  # noqa: E402
import evolution_engine  # noqa: E402

FAKE_PATCH = (
    "diff --git a/x.py b/x.py\n"
    "--- a/x.py\n"
    "+++ b/x.py\n"
    "@@ -1,1 +1,1 @@\n"
    "-a = 1\n"
    "+a = 2\n"
)
FINDING = {"kind": "test", "title": "hallazgo de prueba",
           "evidence": "evidencia", "files": []}


class FakeLlm:
    """LLM falso: respuestas programadas por rol + health()."""

    def __init__(self, reviewer_texts, arbiter_text="APROBADO\nDe acuerdo.",
                 implementer_patch=FAKE_PATCH, healthy=True):
        self.reviewer_texts = list(reviewer_texts)
        self.arbiter_text = arbiter_text
        self.implementer_patch = implementer_patch
        self.healthy = healthy
        self.calls = []

    def health(self):
        return (self.healthy, 10.0, ["ivanna-free"])

    def draft_rewrite(self, system, user, model=None, max_tokens=8192,
                      base_dir=None):
        # Lo llama el IMPLEMENTER para redactar el parche.
        self.calls.append(("implementer", model))
        if self.implementer_patch is None:
            return "sin diff", None
        return "```diff\n" + self.implementer_patch + "\n```", self.implementer_patch

    def chat(self, messages, model=None, temperature=0.2, max_tokens=8192,
             read_timeout_s=60):
        # Se distingue por el MODELO.
        if model == council.ROLES["detector"]:
            self.calls.append(("detector", model))
            return "REAL — el defecto existe en el código."
        if model == council.ROLES["verifier"]:
            self.calls.append(("verifier", model))
            return "VERIFICADO — cifras con fuente."
        if model == council.ROLES["proposer"]:
            self.calls.append(("proposer", model))
            return "SOLUCIÓN: cambiar x a 2."
        if model == council.ROLES["arbiter"]:
            self.calls.append(("arbiter", model))
            return self.arbiter_text
        self.calls.append(("reviewer", model))
        text = self.reviewer_texts.pop(0) if self.reviewer_texts else "APROBADO\nOk."
        return text


class TestDraftCouncilIntegration(unittest.TestCase):
    def setUp(self):
        # El gate mecánico (v2.0) valida el diff contra un repo real:
        # los tests usan un repo temporal con x.py ("a = 1") para que el
        # FAKE_PATCH aplique limpio.
        import subprocess
        import tempfile
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        (self.repo / "x.py").write_text("a = 1\n")
        subprocess.run(["git", "init", "-q"], cwd=self.repo, check=True)

    def tearDown(self):
        self._tmp.cleanup()

    def test_draft_uses_council_and_returns_agreed_patch(self):
        llm = FakeLlm(["APROBADO\nBien."])
        draft = evolution_engine.draft_patch(FINDING, llm=llm, repo=self.repo)
        self.assertEqual(FAKE_PATCH, draft["patch"])
        self.assertTrue(draft["council"]["approved"])
        self.assertIn("consejo", draft["model"])
        # El consejo fue consultado de verdad: los 6 roles del Consejo
        # Nuclear (reviewer2 en cuarentena, no se consulta).
        models = [m for (_, m) in llm.calls]
        self.assertIn(council.ROLES["detector"], models)
        self.assertIn(council.ROLES["verifier"], models)
        self.assertIn(council.ROLES["proposer"], models)
        self.assertIn(council.ROLES["implementer"], models)
        self.assertIn(council.ROLES["reviewer"], models)
        self.assertIn(council.ROLES["arbiter"], models)

    def test_draft_rejected_by_reviewer_raises(self):
        llm = FakeLlm(["RECHAZADO\nMal.", "RECHAZADO\nSigue mal."])
        with self.assertRaises(SystemExit) as ctx:
            evolution_engine.draft_patch(FINDING, llm=llm, repo=self.repo)
        self.assertIn("consejo", str(ctx.exception).lower())
        # 2 rondas de diálogo (v2.0), sin consenso → fail-closed. El
        # implementer redacta un parche por ronda (el proposer solo
        # inventa 1 vez: los rechazos son de implementación, no de enfoque).
        self.assertEqual(2, len([c for c in llm.calls if c[0] == "implementer"]))
        self.assertEqual(1, len([c for c in llm.calls if c[0] == "proposer"]))

    def test_draft_rejected_by_arbiter_raises(self):
        llm = FakeLlm(["APROBADO\nBien."],
                      arbiter_text="RECHAZADO\nNo es magistral.")
        with self.assertRaises(SystemExit):
            evolution_engine.draft_patch(FINDING, llm=llm, repo=self.repo)

    def test_draft_no_usable_diff_raises(self):
        llm = FakeLlm(["APROBADO\nBien."], implementer_patch=None)
        with self.assertRaises(SystemExit):
            evolution_engine.draft_patch(FINDING, llm=llm, repo=self.repo)

    def test_draft_forbidden_files_blocked_before_llm(self):
        llm = FakeLlm(["APROBADO\nBien."])
        finding = dict(FINDING, files=[
            "app/src/main/java/com/ivanna/omega/security/X.kt"])
        with self.assertRaises(SystemExit):
            evolution_engine.draft_patch(finding, llm=llm)
        self.assertEqual([], llm.calls)

    def test_draft_gateway_down_raises(self):
        llm = FakeLlm(["APROBADO\nBien."], healthy=False)
        with self.assertRaises(SystemExit):
            evolution_engine.draft_patch(FINDING, llm=llm, repo=self.repo)
        self.assertEqual([], llm.calls)


if __name__ == "__main__":
    unittest.main()
