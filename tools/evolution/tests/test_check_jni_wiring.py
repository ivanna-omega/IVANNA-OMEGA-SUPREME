"""Tests de regresión para check_jni_wiring.py (propuesta 0009).

Hallazgo original: 4 external fun SIN llamador en Kotlin
(nativeSetOmniEnabled, nativeSetOmniHeadPose, nativeSetNeuEnabled,
nativeSetAuxGeneziPreset). NO son código muerto: son APIs DIFERIDAS
intencionales con JNI completo, pendientes de decisión de Esse.
Fix robusto (exigido por el reviewer del consejo, que rechazó el
enfoque por comentarios sueltos): REGISTRO explícito
tools/evolution/knowledge/deferred_jni_api.json con reason +
unlocks_when por entrada; el check NO las reporta pero las lista como
[DEFERRED], y FALLA si el registro queda obsoleto o incompleto.
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPT = ROOT / "scripts" / "check_jni_wiring.py"
REGISTRY = ROOT / "tools" / "evolution" / "knowledge" / "deferred_jni_api.json"

sys.path.insert(0, str(SCRIPT.parent))
import check_jni_wiring as cjw  # noqa: E402


EXPECTED_DEFERRED = {
    "com.ivanna.omega.core.IvannaNativeLib.nativeSetOmniEnabled",
    "com.ivanna.omega.core.IvannaNativeLib.nativeSetOmniHeadPose",
    "com.ivanna.omega.core.IvannaNativeLib.nativeSetNeuEnabled",
    "com.ivanna.omega.core.IvannaNativeLib.nativeSetAuxGeneziPreset",
    # 5 más registradas después (2026-11-20, 487): APIs reales sin
    # llamador Kotlin aún, cada una con reason + unlocks_when en el
    # registro — son diferidas intencionales, no código muerto.
    "com.ivanna.omega.core.IvannaNativeLib.nativeResetDSP",
    "com.ivanna.omega.core.IvannaNativeLib.nativeLoadEvoState",
    "com.ivanna.omega.core.IvannaNativeLib.nativeGetNaelCorrections",
    "com.ivanna.omega.core.IvannaNativeLib.nativeSetFastRpcEnabled",
    "com.ivanna.omega.core.IvannaNativeLib.nativeSetAtiEnabled",
}


class TestDeferredRegistry(unittest.TestCase):
    def test_registry_exists_and_has_the_known_deferred(self):
        registry = cjw.load_deferred_jni_api()
        self.assertEqual(set(registry), EXPECTED_DEFERRED)

    def test_each_entry_has_reason_and_unlock(self):
        registry = cjw.load_deferred_jni_api()
        for qn, entry in registry.items():
            self.assertTrue(entry["reason"], qn)
            self.assertTrue(entry["unlocks_when"], qn)

    def test_is_deferred(self):
        registry = cjw.load_deferred_jni_api()
        for qn in EXPECTED_DEFERRED:
            self.assertTrue(cjw.is_deferred(qn, registry))
        self.assertFalse(
            cjw.is_deferred("com.ivanna.omega.core.IvannaNativeLib.nativeProcess", registry))
        self.assertFalse(cjw.is_deferred("cualquier.otra.Cosa", registry))

    def test_registry_missing_file_means_empty(self):
        self.assertEqual(cjw.load_deferred_jni_api(Path("/no/existe.json")), {})

    def test_registry_entry_without_reason_is_invalid(self):
        bad = {"entries": [{"qualified_name": "a.b.C.m",
                            "unlocks_when": "nunca"}]}
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(bad, f)
            path = Path(f.name)
        with self.assertRaises(ValueError):
            cjw.load_deferred_jni_api(path)

    def test_registry_entry_without_unlock_is_invalid(self):
        bad = {"entries": [{"qualified_name": "a.b.C.m",
                            "reason": "porque sí"}]}
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump(bad, f)
            path = Path(f.name)
        with self.assertRaises(ValueError):
            cjw.load_deferred_jni_api(path)


class TestCheckJniWiringIntegration(unittest.TestCase):
    def test_check_is_green_on_real_tree(self):
        p = subprocess.run(
            [sys.executable, str(SCRIPT)],
            capture_output=True, text=True, timeout=300)
        self.assertEqual(p.returncode, 0, p.stdout[-2000:] + p.stderr[-2000:])
        self.assertIn("[PASS] 0 hallazgos de cableado JNI.", p.stdout)
        self.assertIn("[DEFERRED] APIs JNI diferidas sin llamador (9", p.stdout)


if __name__ == "__main__":
    unittest.main()
