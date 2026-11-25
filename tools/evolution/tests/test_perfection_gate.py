"""Tests de la puerta de perfección (L2 v3 + auditoría L6).

La puerta no es decorativa: estos tests SIEMBRAN un defecto (un marcador
TODO estricto en un archivo temporal + una capacidad marcada PENDIENTE en
un inventario temporal) y verifican que la puerta lo atrapa (contador > 0,
exit != 0). Luego lo retiran: todo vive en TemporaryDirectory, que se
limpia solo. Nada se siembra en el repo real.

También se verifica que un inventario limpio (todo en objetivo, sin TODOs)
da 0 y exit 0, que los umbrales están versionados y que el inventario real
lleva target_state válido en las 30 capacidades (corrección L4).
"""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

import perfection_gate as gate  # noqa: E402

# La línea sembrada se construye por concatenación para que el escáner
# estricto de la puerta NO la detecte dentro de ESTE archivo de test.
SEEDED_MARKER = "// " + "TODO" + "(perfection-gate-audit): trabajo pendiente sembrado"

INVENTORY_CLEAN = """schema_version: 1
producto: IVANNA-OMEGA-SUPREME
version: v2.5.445
version_code: 736
capacidades:
  - id: cap-001
    nombre: Prueba audio
    estado: VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE
    target_state: VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE
    evidencia:
      - ABX superado en dispositivo.
  - id: cap-002
    nombre: Prueba herramienta
    estado: VERIFICADA EN HOST
    target_state: VERIFICADA EN HOST/CI
    evidencia:
      - Tests en CI en verde.
pendientes_resumen: []
bloqueados_resumen: []
"""


def _write_fixture(base: Path, inventory_text: str, with_todo: bool = False) -> Path:
    (base / "docs" / "capabilities").mkdir(parents=True, exist_ok=True)
    (base / "docs" / "capabilities" / "inventory.yaml").write_text(
        inventory_text, encoding="utf-8")
    prod = base / "app" / "src" / "main" / "cpp"
    prod.mkdir(parents=True, exist_ok=True)
    code = "// fixture de auditoria\nint foo() { return 1; }\n"
    if with_todo:
        code += SEEDED_MARKER + "\n"
    (prod / "fixture.cpp").write_text(code, encoding="utf-8")
    return base


def _run_gate_cli(root: Path):
    proc = subprocess.run(
        [sys.executable, str(SCRIPTS / "perfection_gate.py"),
         "--root", str(root), "--output", str(root / "report.json"),
         "--skip-ctest", "--skip-checkers", "--skip-unittest", "--skip-sofa"],
        capture_output=True, text=True, timeout=120)
    report = json.loads((root / "report.json").read_text(encoding="utf-8"))
    return proc.returncode, report


class TestPerfectionGateAudit(unittest.TestCase):
    def test_defecto_sembrado_es_atrapado(self):
        """L6: TODO sembrado + capacidad PENDIENTE => contadores > 0, exit != 0."""
        with tempfile.TemporaryDirectory(prefix="gate-audit-") as tmp:
            base = Path(tmp)
            seeded = INVENTORY_CLEAN.replace(
                "    estado: VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE",
                "    estado: PENDIENTE", 1)
            _write_fixture(base, seeded, with_todo=True)
            rc, report = _run_gate_cli(base)
            self.assertNotEqual(rc, 0, "La puerta dio 0 con un defecto sembrado: es decorativa.")
            self.assertGreater(report["counters"]["PENDIENTES"], 0)
            self.assertGreater(report["counters"]["A_MEDIAS"], 0)
            self.assertEqual(report["verdict"], "NO PERFECTA")
            kinds = {it["kind"] for it in report["items"]}
            self.assertIn("capacidad_pendiente", kinds)
            self.assertIn("todo_fixme_stub", kinds)
        # Al salir del contexto el directorio temporal ya no existe: retirado.
        self.assertFalse(base.exists())

    def test_inventario_limpio_da_cero(self):
        """El mismo fixture sin defectos debe dar 0 en los 4 contadores."""
        with tempfile.TemporaryDirectory(prefix="gate-clean-") as tmp:
            base = Path(tmp)
            _write_fixture(base, INVENTORY_CLEAN, with_todo=False)
            rc, report = _run_gate_cli(base)
            self.assertEqual(rc, 0, f"Puerta en rojo con fixture limpio: {report['counters']}")
            self.assertEqual(report["counters"],
                             {"PENDIENTES": 0, "FALLOS": 0, "A_MEDIAS": 0,
                              "BLOQUEOS_ABIERTOS": 0})
            self.assertEqual(report["verdict"], "PERFECTA")

    def test_bloqueo_sin_decision_cuenta_y_con_decision_no(self):
        with tempfile.TemporaryDirectory(prefix="gate-block-") as tmp:
            base = Path(tmp)
            blocked = INVENTORY_CLEAN.replace(
                "    estado: VERIFICADA EN HOST\n",
                "    estado: BLOQUEADA\n", 1)
            _write_fixture(base, blocked)
            _, report = _run_gate_cli(base)
            self.assertEqual(report["counters"]["BLOQUEOS_ABIERTOS"], 1)
            decided = blocked.replace(
                "    estado: BLOQUEADA\n",
                "    estado: BLOQUEADA\n    decision_esse: Se desecha por decisión de Esse.\n", 1)
            _write_fixture(base, decided)
            _, report2 = _run_gate_cli(base)
            self.assertEqual(report2["counters"]["BLOQUEOS_ABIERTOS"], 0)


class TestPerfectionGateThresholds(unittest.TestCase):
    def test_umbrales_versionados(self):
        # 2026-11-21 (v2.5.490): la suite CTest creció de 1008 a 1416
        # (conservación 9 + gemelo 46 + MPC 11 + adversarial 134 + resto).
        # El umbral CTest se SUBE a 1416/1416; el SOFA se queda en 694
        # (el conteo real del gate, que excluye directorios build, es 694).
        # Este test es el candado: cambiar un umbral exige actualizarlo
        # aquí también, para que el cambio sea explícito y revisable.
        # 2026-11-21 (Majestuosidad Total P6): la suite crece de 1416 a
        # 1417 con test_universal_hearing (1 entrada ctest, 119 checks).
        # 2026-11-21 (v2.5.492): la suite crece de 1417 a 1423 con el call
        # site del gemelo (6 tests nuevos FusedChainTwinCallSite).
        # 2026-11-21 (v2.5.493): la suite crece de 1423 a 1430 con el control
        # del gemelo (7 tests nuevos FusedChainTwinControl).
        # 2026-11-22 (salto/control-cerrado): la suite crece de 1430 a 1607
        # con el sistema de control cerrado (M0 6 + M3 7 + telemetría 9 +
        # M5 16 + M6 9 + M8 11 + adversarial 119 = 177 tests nuevos).
        # 2026-11-22 (P1 banco adversarial permanente): la suite crece de
        # 1607 a 1619 con la matriz bypass vs IVANNA vs cada operador
        # (11 casos + 1 diagnóstico APEX/limiter).
        # 2026-11-22 (P0 referencia congelada): la suite crece de 1619 a
        # 1628 con test_p0_reference_lock (9 TESTs: Freeze, Levels,
        # TruePeak, Thdn, Imd, Latency, CpuXruns, SilenceTail,
        # Deterministic).
        # 2026-11-22 (fix no-determinismo NEURO): la suite crece de 1628
        # a 1631 con test_neuro_determinism (3 TESTs: op_neuro bit-idéntico,
        # sonda clasificador+guardián, IVANNA dentro de tolerancia).
        # 2026-11-22 (v2.5.498, fix del tronido): la suite crece de 1631
        # a 1634 con test_tronidos_layer_step (3 TESTs: AllLayers,
        # FixEngageNoStepAndModeNoSilence, ApexOffProbe).
        # 2026-11-23 (pan Poincaré de campo cercano, flag OFF por defecto):
        # la suite crece de 1634 a 1639 con test_poincare_pan (4 TESTs) y
        # test_fused_spatial.WfsStageGoldenWithPoincareOff (1 TEST).
        # 2026-11-24 (CORONA, cableado P7–P10): la suite crece de 1639 a
        # 1675 (+12 CORONA: 4 entradas standalone + 8 FusedChainCoronaWiring)
        # sobre una base ya en 1663 (umbral 1639 estancado desde la v2.5.498;
        # el Frente A midió 1,663 sin subir el candado). 1663 + 12 = 1675
        # medido con `ctest -N` en el build clang++-18 completo.
        self.assertEqual(gate.THRESHOLDS["ctest_expected_total"], 1675)
        self.assertEqual(gate.THRESHOLDS["ctest_expected_passed"], 1675)
        self.assertEqual(gate.THRESHOLDS["sofa_expected_total"], 694)
        self.assertTrue(gate.THRESHOLDS_VERSION)

    def test_target_state_del_inventario_real(self):
        """L4: las 30 capacidades reales llevan target_state válido por tipo."""
        caps, _ = gate.parse_inventory(ROOT / "docs" / "capabilities" / "inventory.yaml")
        self.assertEqual(len(caps), 30)
        for cap in caps:
            self.assertIn(cap["target_state"], gate.TARGET_STATES_VALIDOS,
                          f"{cap['id']} sin target_state válido")
        by_target = {}
        for cap in caps:
            by_target.setdefault(cap["target_state"], []).append(cap["id"])
        self.assertEqual(len(by_target["VERIFICADA EN DISPOSITIVO + VALIDADA PERCEPTUALMENTE"]), 12)
        self.assertEqual(len(by_target["VERIFICADA EN DISPOSITIVO"]), 11)
        self.assertEqual(len(by_target["VERIFICADA EN HOST/CI"]), 7)

    def test_estado_host_satisface_objetivo_host_ci(self):
        self.assertTrue(gate.estado_satisface_objetivo("VERIFICADA EN HOST",
                                                       "VERIFICADA EN HOST/CI"))
        self.assertFalse(gate.estado_satisface_objetivo("VERIFICADA EN HOST",
                                                        "VERIFICADA EN DISPOSITIVO"))
        self.assertFalse(gate.estado_satisface_objetivo("INTEGRADA",
                                                        "VERIFICADA EN HOST/CI"))

    def test_escaneo_todo_estricto_no_casa_palabra_espanola(self):
        """Un 'TODO' en prosa española o un placeholder XXXX no es un marcador."""
        with tempfile.TemporaryDirectory(prefix="gate-todo-") as tmp:
            base = Path(tmp)
            prod = base / "app" / "src" / "main" / "cpp"
            prod.mkdir(parents=True)
            (prod / "a.cpp").write_text(
                "// por defecto TODO apagado\n"
                '// formato "1:XXXXXXXXXXXX:android:XXXXXXXXXXXXXXXX"\n',
                encoding="utf-8")
            items: list = []
            self.assertEqual(gate.scan_todo(base, items), 0)
            (prod / "a.cpp").write_text(
                "// " + "TODO" + ": pendiente real\n", encoding="utf-8")
            items = []
            self.assertEqual(gate.scan_todo(base, items), 1)

    def test_todo_en_codigo_vendoreado_no_cuenta(self):
        """Un TODO dentro de nlohmann/json vendoreado no es del producto."""
        with tempfile.TemporaryDirectory(prefix="gate-vendored-") as tmp:
            base = Path(tmp)
            vendored = base / "app" / "src" / "main" / "cpp" / "include" / "nlohmann"
            vendored.mkdir(parents=True)
            (vendored / "json.hpp").write_text(
                "// " + "TODO" + "(niels): may we ignore N here?\n", encoding="utf-8")
            items: list = []
            self.assertEqual(gate.scan_todo(base, items), 0)

    def test_fallo_toolchain_no_es_fallo_del_producto(self):
        """cmake demasiado viejo => no ejecutado, no fallo del producto."""
        out = ("CMake Error at CMakeLists.txt:1 (cmake_minimum_required):\n"
               "  CMake 3.30.3 or higher is required.  You are running version 3.28.3\n")
        self.assertTrue(gate.is_toolchain_failure(out))
        self.assertFalse(gate.is_toolchain_failure("100% tests passed, 0 tests failed out of 5"))
        self.assertFalse(gate.is_toolchain_failure("error: use of undeclared identifier 'foo'"))


if __name__ == "__main__":
    unittest.main()
