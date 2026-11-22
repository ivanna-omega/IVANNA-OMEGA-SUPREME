"""Tests de check_globals_symmetry (propuesta 0014, coordinador del Plan
Omnipotencia).

Guard de regresión de la lección 389 (2026-11-10): cada atomic nuevo g_*
necesita definición en AMBOS lados (app y audioserver); si falta el stub,
el NDK falla con `undefined symbol` y la CI no publica.

Verdad medida 2026-11-16:
- 4 pares bridge/effect idénticos (nombres+tipos+valores): holos (19),
  hyper (3), orbe (7), wfs (11).
- hrtf: app en audio_orchestrator.cpp, effect en hrtf_globals_effect.cpp,
  mismos valores (g_hrtf_wet_dry{1.0f}, g_hrtf_flush_req{false}).
- App-only documentados: omni g_omni_audiogram_dirty, g_omni_yaw_rad;
  upmixing g_upmixing_enabled, g_upmixing_immersivity (módulo entero).
  Ninguno se referencia en el effect (verificado por git grep).

Casos:
- El check pasa en el repo real (0 asimetrías, exit 0).
- extract_globals extrae (tipo, valor inicial) e ignora comentarios.
- Un par que difiere en valor inicial -> exit 1.
- Un atomic app-only referenciado en el effect -> exit 1.
- Un atomic nuevo en un solo lado sin registro -> exit 1.
"""
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))

import check_globals_symmetry as cgs  # noqa: E402

REPO = Path(__file__).resolve().parents[3]

# Repo mínimo que satisface el invariante (base para los casos de fallo).
BASE_FILES = {
    "holos_controls_bridge.cpp": (
        "std::atomic<float> g_holos_amount{0.0f};\n"
        "std::atomic<int> g_holos_band_count{24};\n"
    ),
    "holos_globals_effect.cpp": (
        "std::atomic<float> g_holos_amount{0.0f};\n"
        "std::atomic<int> g_holos_band_count{24};\n"
    ),
    "hyper_controls_bridge.cpp": "std::atomic<float> g_hyper_amount{0.0f};\n",
    "hyper_globals_effect.cpp": "std::atomic<float> g_hyper_amount{0.0f};\n",
    "orbe_controls_bridge.cpp": "std::atomic<float> g_orbe_amount{1.0f};\n",
    "orbe_globals_effect.cpp": "std::atomic<float> g_orbe_amount{1.0f};\n",
    "wfs_controls_bridge.cpp": "std::atomic<float> g_wfs_enabled{1.0f};\n",
    "wfs_globals_effect.cpp": "std::atomic<float> g_wfs_enabled{1.0f};\n",
    "audio_orchestrator.cpp": (
        "std::atomic<float> g_hrtf_wet_dry{1.0f};\n"
        "std::atomic<bool> g_hrtf_flush_req{false};\n"
    ),
    "hrtf_globals_effect.cpp": (
        "std::atomic<float> g_hrtf_wet_dry{1.0f};\n"
        "std::atomic<bool> g_hrtf_flush_req{false};\n"
    ),
    "omni_controls_bridge.cpp": (
        "std::atomic<float> g_omni_amount{0.0f};\n"
        "std::atomic<float> g_omni_yaw_rad{0.0f};\n"
        "std::atomic<bool> g_omni_audiogram_dirty{false};\n"
    ),
    "omni_globals_effect.cpp": "std::atomic<float> g_omni_amount{0.0f};\n",
    "upmixing_controls_bridge.cpp": (
        "std::atomic<bool> g_upmixing_enabled{false};\n"
        "std::atomic<float> g_upmixing_immersivity{0.5f};\n"
    ),
    "omega_effect.cpp": "// efecto AudioFlinger (sin referencias a app-only)\n",
}


class TestCheckGlobalsSymmetry(unittest.TestCase):
    def setUp(self):
        self._old_root = cgs.CPP_ROOT
        self._tmp = tempfile.TemporaryDirectory()
        self.cpp = Path(self._tmp.name)
        for name, content in BASE_FILES.items():
            (self.cpp / name).write_text(content, encoding="utf-8")
        cgs.CPP_ROOT = self.cpp

    def tearDown(self):
        cgs.CPP_ROOT = self._old_root
        self._tmp.cleanup()

    def test_0014_extract_globals_ignores_comments(self):
        p = self.cpp / "probe.cpp"
        p.write_text(
            "// std::atomic<float> g_fake{9.9f}; en comentario\n"
            "std::atomic<float> g_real{0.5f};  // trailing\n"
            "std::atomic<bool> g_flag{true};\n",
            encoding="utf-8",
        )
        got = cgs.extract_globals(p)
        self.assertEqual(got, {
            "g_real": ("float", "0.5f"),
            "g_flag": ("bool", "true"),
        })

    def test_0014_baseline_passes(self):
        self.assertEqual(cgs.main(), 0)

    def test_0014_pair_value_mismatch_is_caught(self):
        (self.cpp / "holos_globals_effect.cpp").write_text(
            "std::atomic<float> g_holos_amount{0.5f};\n"
            "std::atomic<int> g_holos_band_count{24};\n",
            encoding="utf-8",
        )
        self.assertEqual(cgs.main(), 1)

    def test_0014_pair_missing_side_is_caught(self):
        (self.cpp / "orbe_globals_effect.cpp").write_text(
            "", encoding="utf-8",
        )
        self.assertEqual(cgs.main(), 1)

    def test_0014_app_only_referenced_in_effect_is_caught(self):
        (self.cpp / "omega_effect.cpp").write_text(
            "// el effect lee el yaw (ya no es app-only)\n"
            "float y = g_omni_yaw_rad.load();\n",
            encoding="utf-8",
        )
        self.assertEqual(cgs.main(), 1)

    def test_0014_undocumented_one_sided_is_caught(self):
        (self.cpp / "hyper_controls_bridge.cpp").write_text(
            "std::atomic<float> g_hyper_amount{0.0f};\n"
            "std::atomic<float> g_hyper_new_param{1.0f};\n",
            encoding="utf-8",
        )
        self.assertEqual(cgs.main(), 1)

    def test_0014_check_passes_on_real_repo(self):
        import subprocess
        proc = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "check_globals_symmetry.py")],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("[PASS] check_globals_symmetry: 0 asimetrías",
                      proc.stdout)


if __name__ == "__main__":
    unittest.main()
