"""Tests de regresión para check_datasets_deploy.py (propuesta 0010).

Hallazgo original: 46 "[sofa] tamaño distinto APK/módulo" — FALSO POSITIVO.
Los SOFA del módulo se re-codifican a float32+gzip9 (v2.5.393) y el tamaño
por archivo varía en ambas direcciones (medido: 114/216 más grandes en el
módulo, 102/216 más pequeños; total 75.06 MB APK -> 66.40 MB módulo).
La paridad real de SOFA es presencia + cabecera HDF5 + no truncado.
"""
import ast
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "check_datasets_deploy.py"


def _load_size_parity_error():
    """Carga solo la función size_parity_error del script (sin ejecutar
    el chequeo completo: el script no tiene guarda __main__)."""
    tree = ast.parse(SCRIPT.read_text())
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == "size_parity_error":
            ns = {}
            exec(compile(ast.Module(body=[node], type_ignores=[]), str(SCRIPT), "exec"), ns)
            return ns["size_parity_error"]
    raise AssertionError("size_parity_error no encontrada en check_datasets_deploy.py")


size_parity_error = _load_size_parity_error()


class TestSizeParityRule(unittest.TestCase):
    def test_sofa_never_errors_on_size(self):
        # Re-codificación: el tamaño varía en ambas direcciones.
        self.assertFalse(size_parity_error("sofa", 161574, 163896))  # módulo mayor
        self.assertFalse(size_parity_error("sofa", 621212, 330719))  # módulo menor
        self.assertFalse(size_parity_error("sofa", 1000, 1000))      # igual

    def test_hrtf_size_must_match(self):
        self.assertFalse(size_parity_error("hrtf", 1000, 1000))
        self.assertTrue(size_parity_error("hrtf", 1000, 1001))

    def test_rir_size_must_match(self):
        self.assertFalse(size_parity_error("rir", 5000, 5000))
        self.assertTrue(size_parity_error("rir", 5000, 4999))


class TestCheckDatasetsDeployIntegration(unittest.TestCase):
    def test_check_is_green_on_real_tree(self):
        p = subprocess.run(
            [sys.executable, str(SCRIPT)],
            capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout[-2000:] + p.stderr[-2000:])
        self.assertIn("[PASS]", p.stdout)


if __name__ == "__main__":
    unittest.main()
