"""Tests de check_header_wiring STANDALONE_HEADERS (propuesta 0011,
coordinador del Plan Omnipotencia).

El consejo no procesó este hallazgo; el coordinador clasificó los 12
huérfanos directamente con evidencia medida (quién los incluye, si
tienen .cpp, si están en CMakeLists, qué dice su propio header/docs).

Los 12 (verdad medida 2026-11-16):
- Legacy daemon v1 (2.5.401, reemplazado por el CEREBRO v2, no se
  compila por diseño): daemon/control/command_server.h,
  daemon/core/effect_integrity.h, daemon/hal_clock_producer.hpp,
  include/write_stage_asrc_telemetry.hpp (solo la usa command_server).
- Primitivas de test / trabajo futuro: device_cpu_profiler.hpp (futuro
  puente JNI), include/poller_sched.hpp (P0-b, no consumida),
  spatial/DopPacketizer.hpp, spatial/PinnaElevationEq.hpp (pilares
  pendientes), spatial/TransducerOperator.hpp (necesita medida).
- Objetos standalone Ψ/Φ (valores vía JNI, no vía #include):
  spatial/BioControl5D.hpp, spatial/PsiFieldRuntime.hpp,
  spatial/HomogeneousFusion.hpp (el Φ activo es HyperStateOperator).

Casos:
- Los 12 están en STANDALONE_HEADERS (clasificados, no escondidos).
- Ninguna entrada de STANDALONE_HEADERS está obsoleta (todas existen
  en el repo): la lista no se pudre si se borra o mueve un header.
- El check pasa en el repo real (0 huérfanos, exit 0).
"""
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "scripts"))

import check_header_wiring as chw

REPO = Path(__file__).resolve().parents[3]

EXPECTED_0011 = {
    "daemon/control/command_server.h",
    "daemon/core/effect_integrity.h",
    "daemon/hal_clock_producer.hpp",
    "include/write_stage_asrc_telemetry.hpp",
    "device_cpu_profiler.hpp",
    "include/poller_sched.hpp",
    "spatial/DopPacketizer.hpp",
    "spatial/PinnaElevationEq.hpp",
    "spatial/TransducerOperator.hpp",
    "spatial/BioControl5D.hpp",
    "spatial/PsiFieldRuntime.hpp",
    "spatial/HomogeneousFusion.hpp",
}


class TestCheckHeaderWiringStandalone(unittest.TestCase):
    def test_0011_twelve_orphans_are_classified(self):
        missing = EXPECTED_0011 - set(chw.STANDALONE_HEADERS)
        self.assertEqual(missing, set(),
                         f"huérfanos sin clasificar: {sorted(missing)}")

    def test_0011_no_stale_standalone_entries(self):
        stale = [name for name in chw.STANDALONE_HEADERS
                 if not (chw.CPP_ROOT / name).is_file()]
        self.assertEqual(stale, [],
                         f"entradas obsoletas en STANDALONE_HEADERS: {stale}")

    def test_0011_check_passes_on_real_repo(self):
        proc = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "check_header_wiring.py")],
            capture_output=True, text=True, timeout=120)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def test_0022_thd_meter_is_classified_standalone(self):
        # 0022 (crítica externa punto 3): dsp/ThdMeter.hpp es primitiva de
        # medición host (test + thd_analyze), no DSP de producción; debe
        # estar clasificada en STANDALONE_HEADERS, no escondida ni huérfana.
        self.assertIn("dsp/ThdMeter.hpp", set(chw.STANDALONE_HEADERS))


if __name__ == "__main__":
    unittest.main()
