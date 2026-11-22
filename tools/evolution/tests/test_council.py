"""Tests del consejo multi-modelo (tools/evolution/council.py).

El consejo dialoga con el LLM real vía OmniRoute, pero estos tests usan
un LLM falso (mock) para no depender del gateway: verifican la lógica del
diálogo (veredictos, rondas, fail-closed), no las respuestas del modelo.
"""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import council  # noqa: E402


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
    """LLM falso: respuestas programadas por rol
    (detector/proposer/implementer/reviewer/arbiter)."""

    def __init__(self, reviewer_texts, arbiter_text="APROBADO\nDe acuerdo.",
                 implementer_patch=FAKE_PATCH,
                 proposer_solution="SOLUCIÓN: cambiar x a 2.",
                 detector_text="REAL — el defecto existe en el código.",
                 verifier_text="VERIFICADO — cifras con fuente."):
        # reviewer_texts: lista de respuestas del reviewer por ronda.
        self.reviewer_texts = list(reviewer_texts)
        self.arbiter_text = arbiter_text
        self.implementer_patch = implementer_patch
        self.proposer_solution = proposer_solution
        self.detector_text = detector_text
        self.verifier_text = verifier_text
        self.calls = []

    def draft_rewrite(self, system, user, model=None, max_tokens=8192,
                      base_dir=None):
        # Lo llama el IMPLEMENTER para redactar el parche.
        self.calls.append(("implementer", model))
        if self.implementer_patch is None:
            return "sin diff", None
        return "```diff\n" + self.implementer_patch + "\n```", self.implementer_patch

    def chat(self, messages, model=None, temperature=0.2, max_tokens=8192,
             read_timeout_s=60):
        # Se distingue por el MODELO (no por el texto del sistema: el
        # sistema del árbitro menciona al revisor y confundía el mock).
        if model == council.ROLES["detector"]:
            self.calls.append(("detector", model))
            return self.detector_text
        if model == council.ROLES["verifier"]:
            self.calls.append(("verifier", model))
            return self.verifier_text
        if model == council.ROLES["proposer"]:
            self.calls.append(("proposer", model))
            return self.proposer_solution
        if model == council.ROLES["arbiter"]:
            self.calls.append(("arbiter", model))
            return self.arbiter_text
        if model == council.ROLES.get("reviewer2"):
            self.calls.append(("reviewer2", model))
        else:
            self.calls.append(("reviewer", model))
        text = self.reviewer_texts.pop(0) if self.reviewer_texts else "APROBADO\nOk."
        return text


class TestParseVerdict(unittest.TestCase):
    def test_aprobado(self):
        ok, _ = council._parse_verdict("APROBADO\nTodo bien.")
        self.assertTrue(ok)

    def test_rechazado(self):
        ok, _ = council._parse_verdict("RECHAZADO\nDegrada el sonido.")
        self.assertFalse(ok)

    def test_ambiguo_es_none(self):
        # Sin veredicto explícito = ambiguo = fail-closed (None, no True).
        ok, reason = council._parse_verdict("Me parece bien, creo.")
        self.assertIsNone(ok)
        self.assertIn("sin veredicto", reason)

    def test_vacio_es_none(self):
        ok, _ = council._parse_verdict("")
        self.assertIsNone(ok)


class TestExtractPlainDiff(unittest.TestCase):
    PLAIN = ("```diff\n--- a/x.py\n+++ b/x.py\n"
             "@@ -1,1 +1,1 @@\n-a = 1\n+a = 2\n```")

    def test_acepta_diff_plano_sin_diff_git(self):
        patch = council._extract_plain_diff(self.PLAIN)
        self.assertIsNotNone(patch)
        self.assertTrue(patch.startswith("diff --git a/x.py b/x.py"))
        self.assertIn("@@ -1,1 +1,1 @@", patch)
        self.assertIn("+a = 2", patch)

    def test_acepta_diff_plano_multi_archivo(self):
        # 0045: un diff plano de DOS archivos debe llevar cabecera
        # 'diff --git' en CADA archivo; antes solo la ponía en el primero
        # y git apply lo rechazaba como "corrupt patch".
        plain = ("```diff\n--- a/x.py\n+++ b/x.py\n@@ -1,1 +1,1 @@\n"
                 "-a = 1\n+a = 2\n--- a/y.py\n+++ b/y.py\n"
                 "@@ -1,1 +1,1 @@\n-b = 1\n+b = 2\n```")
        patch = council._extract_plain_diff(plain)
        self.assertIsNotNone(patch)
        self.assertEqual(2, patch.count("diff --git"))
        self.assertIn("diff --git a/x.py b/x.py", patch)
        self.assertIn("diff --git a/y.py b/y.py", patch)

    def test_rechaza_texto_sin_diff(self):
        self.assertIsNone(council._extract_plain_diff("Solo prosa, sin diff."))

    def test_rechaza_diff_sin_hunk(self):
        self.assertIsNone(council._extract_plain_diff(
            "--- a/x.py\n+++ b/x.py\n(sin hunks)"))

    def test_rechaza_vacio(self):
        self.assertIsNone(council._extract_plain_diff(""))


class TestCouncilDialogue(unittest.TestCase):
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

    def review(self, fake):
        return council.council_review(FINDING, llm=fake, repo=self.repo)

    def test_aprueba_a_la_primera(self):
        fake = FakeLlm(["APROBADO\nEl parche corrige la raíz."])
        v = self.review(fake)
        self.assertTrue(v["approved"])
        self.assertEqual(v["patch"], FAKE_PATCH)
        self.assertEqual(len(v["rounds"]), 1)
        # Los 7 roles del Consejo Nuclear participan, cada uno con su
        # modelo (0045: reviewer2 = gemini-3-flash-preview SÍ se consulta
        # tras aprobar el centinela; su rechazo bloquea fail-closed).
        models = [c[1] for c in fake.calls]
        self.assertIn(council.ROLES["detector"], models)
        self.assertIn(council.ROLES["verifier"], models)
        self.assertIn(council.ROLES["proposer"], models)
        self.assertIn(council.ROLES["implementer"], models)
        self.assertIn(council.ROLES["reviewer"], models)
        self.assertIn(council.ROLES["reviewer2"], models)
        self.assertIn(council.ROLES["arbiter"], models)
        # El detector verifica primero, el verificador valida las cifras,
        # el arquitecto prescribe (chat) y el implementer redacta el diff.
        roles = [c[0] for c in fake.calls]
        self.assertEqual(["detector", "verifier", "proposer", "implementer",
                          "reviewer", "reviewer2", "arbiter"],
                         roles)
        self.assertTrue(v["detector"]["real"])

    def test_detector_falso_positivo_queda_registrado(self):
        # El detector marca FALSO POSITIVO: no bloquea el ciclo (el
        # proposer decide con esa información), pero su veredicto queda
        # registrado en el resultado para que nadie crea que era real.
        fake = FakeLlm(["APROBADO\nEl parche corrige la raíz."],
                       detector_text="FALSO POSITIVO — la guarda sí existe "
                       "en el header incluido.")
        v = self.review(fake)
        self.assertTrue(v["approved"])
        self.assertFalse(v["detector"]["real"])
        self.assertIn("FALSO POSITIVO", v["detector"]["text"])

    def test_detector_no_disponible_no_tumba_el_consejo(self):
        # Si el detector lanza (proveedor caído), el consejo continúa sin
        # él y lo registra como no disponible.
        class DetectorCaido(FakeLlm):
            def chat(self, messages, model=None, **kw):
                if model == council.ROLES["detector"]:
                    raise RuntimeError("gateway caído")
                return super().chat(messages, model=model, **kw)
        fake = DetectorCaido(["APROBADO\nEl parche corrige la raíz."])
        v = self.review(fake)
        self.assertTrue(v["approved"])
        self.assertIsNone(v["detector"]["real"])
        self.assertIn("NO DISPONIBLE", v["detector"]["text"])

    def test_reviewer2_se_consulta_y_su_rechazo_bloquea(self):
        # 0045: reviewer2 (gemini-3-flash-preview, CRITIQUE 1.0 medido)
        # ocupa el asiento: se consulta tras aprobar el centinela y su
        # rechazo bloquea (fail-closed). El centinela solo ya no basta.
        fake = FakeLlm(["APROBADO\nBien.",
                        "RECHAZADO\nDegrada el sonido.",
                        "APROBADO\nBien.",
                        "RECHAZADO\nSigue degradando."])
        v = self.review(fake)
        self.assertFalse(v["approved"])
        self.assertIsNone(v["patch"])
        roles = [c[0] for c in fake.calls]
        self.assertEqual(2, roles.count("reviewer2"))

    def test_reviewer2_no_disponible_no_tumba_el_consejo(self):
        # Si reviewer2 lanza (proveedor caído), se registra NO DISPONIBLE
        # y se continúa solo con reviewer1 + arbiter (fail-closed con
        # constancia explícita, no se cree que aprobó).
        class Reviewer2Caido(FakeLlm):
            def chat(self, messages, model=None, **kw):
                if model == council.ROLES["reviewer2"]:
                    raise RuntimeError("gemini caído")
                return super().chat(messages, model=model, **kw)
        fake = Reviewer2Caido(["APROBADO\nEl parche corrige la raíz."])
        v = self.review(fake)
        self.assertTrue(v["approved"])
        r2 = v["rounds"][0]["reviewer2"]
        self.assertIsNone(r2["approved"])
        self.assertIn("NO DISPONIBLE", r2["text"])

    def test_rechazo_y_revision_aprueba_en_ronda_2(self):
        fake = FakeLlm(["RECHAZADO\nFalta el test de regresión.",
                        "APROBADO\nAhora sí, con el test."])
        v = self.review(fake)
        self.assertTrue(v["approved"])
        self.assertEqual(len(v["rounds"]), 2)
        self.assertEqual(v["reviewer"]["round"], 2)
        # Rechazo de IMPLEMENTACIÓN: el implementer revisa, el proposer
        # NO vuelve (el enfoque no cambió): 1 llamada al proposer.
        roles = [c[0] for c in fake.calls]
        self.assertEqual(1, roles.count("proposer"))
        self.assertEqual(2, roles.count("implementer"))

    def test_rechazo_de_enfoque_proposer_reinventa(self):
        # El reviewer marca ENFOQUE: el proposer reinventa la solución
        # en la ronda 2; el implementer redacta el parche nuevo.
        fake = FakeLlm(["RECHAZADO\nENFOQUE: la estrategia es incorrecta, "
                        "hay que atacar la causa en otro sitio.",
                        "APROBADO\nCon el enfoque nuevo sí."])
        v = self.review(fake)
        self.assertTrue(v["approved"])
        self.assertEqual(len(v["rounds"]), 2)
        roles = [c[0] for c in fake.calls]
        self.assertEqual(2, roles.count("proposer"))
        self.assertEqual(2, roles.count("implementer"))
        self.assertEqual("enfoque reinventado", v["rounds"][1]["proposer"])

    def test_sin_consenso_en_2_rondas_rechaza(self):
        fake = FakeLlm(["RECHAZADO\nNo.",
                        "RECHAZADO\nSigue mal."])
        v = self.review(fake)
        self.assertFalse(v["approved"])
        self.assertIsNone(v["patch"])
        self.assertEqual(len(v["rounds"]), 2)
        self.assertIn("sin consenso", v["reason"])
        # 2 rondas = 2 parches del implementer, 1 sola solución del proposer.
        roles = [c[0] for c in fake.calls]
        self.assertEqual(2, roles.count("implementer"))
        self.assertEqual(1, roles.count("proposer"))

    def test_arbiter_rechaza_aunque_reviewer_apruebe(self):
        fake = FakeLlm(["APROBADO\nBien."],
                       arbiter_text="RECHAZADO\nNo es magistral, es a medias.")
        v = self.review(fake)
        self.assertFalse(v["approved"])
        self.assertIsNone(v["patch"])
        self.assertFalse(v["arbiter"]["approved"])

    def test_implementer_sin_diff_rechaza_fail_closed(self):
        fake = FakeLlm(["APROBADO\nBien."], implementer_patch=None)
        v = self.review(fake)
        self.assertFalse(v["approved"])
        self.assertIsNone(v["patch"])
        self.assertIn("implementer no devolvió un diff", v["reason"])

    def test_implementer_titular_falla_pasa_a_respaldo(self):
        # 0045: si el titular (gpt-oss-120b) falla 2 intentos, entra el
        # respaldo (gemma4-31b, CODE 2/2 medido) y el consejo aprueba con
        # su diff. Sin respaldo, la ronda moriría en "sin diff utilizable".
        class TitularCaido(FakeLlm):
            def draft_rewrite(self, system, user, model=None,
                              max_tokens=8192, base_dir=None):
                self.calls.append(("implementer", model))
                if model == council.ROLES["implementer"]:
                    return "sin diff", None
                return ("```diff\n" + self.implementer_patch + "\n```",
                        self.implementer_patch)
        fake = TitularCaido(["APROBADO\nEl parche corrige la raíz."])
        v = self.review(fake)
        self.assertTrue(v["approved"])
        self.assertEqual(v["patch"], FAKE_PATCH)
        models = [c[1] for c in fake.calls]
        self.assertIn(council.ROLES["implementer"], models)
        self.assertIn(council.ROLES["implementer_backup"], models)

    def test_reviewer_ambiguo_no_aprueba(self):
        # Veredicto ambiguo = None = no aprueba → el proposer revisa y,
        # si en 2 rondas no hay APROBADO explícito, se rechaza.
        fake = FakeLlm(["Me parece bien, creo.",
                        "Tal vez sí.",
                        "No sé."])
        v = self.review(fake)
        self.assertFalse(v["approved"])
        self.assertIsNone(v["patch"])


if __name__ == "__main__":
    unittest.main()
