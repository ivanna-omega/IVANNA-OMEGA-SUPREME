"""Tests de ratio_is_ctest_count (propuesta 0008, coordinador del Plan Omnipotencia).

El consejo rechazó 2 veces el parche para este hallazgo (el implementer
metía una excepción global y regexes rotos); el coordinador redactó el fix
directo: la clasificación vive en una función pura testeable y la excepción
de "evolution suite" es posicional, como la de SOFA/HRTF.

Casos (verdad medida: CI run 43830881955, CTest 1037/1037):
- "evolution suite 117/117" (README:63, la suite Python, no CTest) -> NO
  es conteo CTest (se ignora).
- "evolution suite" DESPUÉS del ratio -> SÍ es conteo CTest (la excepción
  es posicional: solo vale lo que precede al ratio).
- Ratio CTest que no coincide con telemetry -> SÍ es conteo CTest.
- Ratio que coincide con el label vigente -> NO (ya está al día).
- SOFA/HRTF antes del ratio -> NO es conteo CTest.
- Ratio dentro de paréntesis con otro fuera -> NO (anotación histórica).
- n1 != n2, o n1 < 50 -> NO es conteo CTest.
"""
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))

import check_docs_claims as cdc

EXPECTED = "1037/1037"


def _classify(line, expected=EXPECTED):
    ratios = list(re.finditer(r'\b(\d+)/(\d+)\b', line))
    has_outside = any(
        (line[: m.start()].count("(") - line[: m.start()].count(")") == 0)
        for m in ratios
    )
    return [cdc.ratio_is_ctest_count(line, m, expected, has_outside)
            for m in ratios]


class TestRatioIsCtestCount(unittest.TestCase):
    def test_evolution_suite_before_ratio_is_not_ctest(self):
        line = ("- **v2.5.456 is in development** on `main` (Consejo Supremo; "
                "evolution suite 117/117).")
        self.assertEqual(_classify(line), [False])

    def test_evolution_suite_after_ratio_is_ctest(self):
        line = "| CTest (DSP host suite) | **999/999** | evolution suite |"
        self.assertEqual(_classify(line), [True])

    def test_ctest_mismatch_is_ctest(self):
        line = "| CTest (DSP host suite) | **999/999** | Host local |"
        self.assertEqual(_classify(line), [True])

    def test_ctest_matching_label_is_not_ctest(self):
        line = "| CTest (DSP host suite) | **1037/1037** | Host local |"
        self.assertEqual(_classify(line), [False])

    def test_sofa_before_ratio_is_not_ctest(self):
        self.assertEqual(_classify("SOFA 239/239 readable"), [False])
        self.assertEqual(_classify("HRTF 12/12 files"), [False])

    def test_sofa_after_ratio_is_ctest(self):
        self.assertEqual(_classify("999/999 SOFA files"), [True])

    def test_ratio_inside_parens_with_outside_is_not_ctest(self):
        # El ratio de fuera (1008/1008) SÍ es conteo CTest (True: no
        # coincide con el label vigente 1037/1037); el de dentro de
        # paréntesis (998/998) es anotación histórica y se ignora (False).
        line = "| CTest | **1008/1008** | (998/998 at v2.5.442 + 10 tests) |"
        self.assertEqual(_classify(line), [True, False])
        # Si el label vigente es el de fuera, ambos se ignoran.
        self.assertEqual(_classify(line, expected="1008/1008"),
                         [False, False])

    def test_lone_parens_ratio_is_ctest(self):
        self.assertEqual(_classify("(999/999)"), [True])

    def test_unequal_or_small_ratio_is_not_ctest(self):
        self.assertEqual(_classify("pasaron 999/1000 tests"), [False])
        self.assertEqual(_classify("ratio 12/12 ok"), [False])

    def test_real_readme_63_line(self):
        line = ("- **v2.5.456 is in development** on `main` (Consejo Supremo: "
                "6 roles with identities; evolution suite 117/117). Its CI "
                "is in progress; it is not yet released.")
        self.assertEqual(_classify(line), [False])

    def test_real_readme_508_line_matches(self):
        line = ("| CTest (DSP host suite) | **1037/1037** | Host local, suite "
                "completa (1037/1037, v2.5.456) |")
        self.assertEqual(_classify(line), [False, False])


if __name__ == "__main__":
    unittest.main()
