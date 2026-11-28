#!/usr/bin/env python3
"""
check_header_wiring.py — Fase 0.2 Herramienta de verdad de headers C/C++ en producción.

Verifica que todo archivo `.h` / `.hpp` de producción bajo `app/src/main/cpp/`
(excluyendo `tests/`) sea alcanzado transitivamente por `#include` desde al menos
una unidad de traducción (`.cpp` / `.c`) compilada en `CMakeLists.txt` o
`daemon/CMakeLists.txt`.

Falla (exit 1) si algún header de runtime está huérfano. Los headers
de primitives probadas de forma aislada o adaptadores externos se enumeran
explícitamente abajo: no se deben confundir con código ya cableado al APK.
"""

from __future__ import annotations
import re
import sys
from pathlib import Path
from typing import List, Set

ROOT = Path(__file__).resolve().parent.parent
CPP_ROOT = ROOT / "app" / "src" / "main" / "cpp"
MAIN_CMAKE = CPP_ROOT / "CMakeLists.txt"
DAEMON_CMAKE = CPP_ROOT / "daemon" / "CMakeLists.txt"

# Interfaces deliberadamente fuera de las unidades de traducción de la app:
# - el adaptador legacy depende de headers del vendor Android y solo se compila
#   dentro del árbol del HAL objetivo;
# - los adaptadores de control HAL se consumen desde hal_integration/ (build
#   externo al APK);
# - G-PLL/GFE son primitivas cubiertas por tests dedicados, pero no se deben
#   presentar como activas en la ruta PCM de la app hasta que exista un caller
#   de runtime válido para fase/estado analítico.
STANDALONE_HEADERS = {
    "include/audio_hardware_clock_hal_adapter.hpp",
    "include/audio_hardware_clock_rate_servo.hpp",
    "include/g_pll_core.hpp",
    "include/gauge_field_engine.hpp",
    "include/hal_output_rate_control.hpp",
    # Controlador ASRC experimental: se prueba como primitiva pura, pero no debe
    # conectarse a la ruta PCM mientras no haya una ruta writable/correlacionada
    # validada. La reproducción Android usa su servo software existente; activar
    # este controlador en paralelo duplicaría los lazos de reloj.
    "dsp/asrc_clock_controller.hpp",
    # Adaptadores evolutivos legacy: la ruta activa usa evolutionary_kernel_v2
    # y su integración en pd_engine; estos headers no tienen callers de runtime
    # ni deben confundirse con el kernel activo del APK.
    "evolutionary_adapter.hpp",
    "evolutionary_adapter_enhanced.hpp",
    # --- 0011 (2026-11-16): 12 huérfanos clasificados, no escondidos. ---
    # Cada uno tiene su razón documentada aquí; si la razón deja de aplicar
    # (se cablea a producción), se QUITA de esta lista y el check lo exige.
    # Grupo LEGACY DAEMON v1 (2.5.401 FUSIÓN MAGISTRAL): el daemon viejo se
    # reemplazó por el CEREBRO v2 (ivanna_daemon.cpp); estos archivos ya no
    # se compilan por diseño y se conservan como referencia (nunca borrar).
    "daemon/control/command_server.h",
    "daemon/core/effect_integrity.h",
    "daemon/hal_clock_producer.hpp",
    # Solo lo usa command_server.cpp (legacy v1) + su test; telemetría del
    # write-stage ASRC, sin consumidor de producción activo.
    "include/write_stage_asrc_telemetry.hpp",
    # Grupo PRIMITIVAS DE TEST / TRABAJO FUTURO: diseñadas y probadas en
    # host, no cableadas a la ruta PCM todavía. Cada una dice en su propio
    # header o en docs por qué no está cableada.
    # Perfilador CPU: futuro puente JNI (documentado en el header).
    "device_cpu_profiler.hpp",
    # P0-b: política pura de scheduling para el poller del reloj HAL;
    # el poller hoy arranca en SCHED_OTHER y no la consume todavía.
    "include/poller_sched.hpp",
    # Pilares pendientes (docs/PILARES_PENDIENTES.md): DoP v1.1 y EQ
    # paramétrico de elevación por pinna, diseñados, no cableados.
    "spatial/DopPacketizer.hpp",
    "spatial/PinnaElevationEq.hpp",
    # Operador MANT Ec. 1: necesita una respuesta MEDIDA del transductor
    # para compensar; sin medida no puede cablearse (lo dice el header).
    "spatial/TransducerOperator.hpp",
    # Motor de filtro inverso cepstral (Ley 1, 0047): primitiva probada
    # en host (test_ley1_tiempo_fase.cpp); sin caller de producción
    # todavía — su consumidor es la inversión medida (Ley 2), que exige
    # una respuesta medida del transductor antes de cablearse. Se QUITA
    # de esta lista cuando se cablee.
    "spatial/ImpulseInverse.hpp",
    # --- 0022 (2026-11-16, crítica externa punto 3: mediciones físicas).
    # Medidor THD+N (header-only): primitiva de MEDICIÓN, no de DSP de
    # producción. Sus consumidores son el test host test_thd_meter.cpp
    # (TU de tests/, excluida del grafo por diseño) y la herramienta host
    # tools/measure/thd_analyze.cpp (analiza grabaciones del teléfono
    # fuera del dispositivo). NO va en la ruta PCM del APK: la medición
    # en vivo es off-device por diseño. Si algún día se cablea un medidor
    # THD+N en vivo en producción, se QUITA de esta lista.
    "dsp/ThdMeter.hpp",
    # Grupo OBJETOS STANDALONE Ψ/Φ: probados en host; sus VALORES llegan
    # a producción por otra vía (JNI), no por #include, así que la clase
    # en sí no aparece en el grafo de includes de producción.
    # BioControl5D: dinámica neural η (la usa PsiFieldRuntime).
    "spatial/BioControl5D.hpp",
    # PsiFieldRuntime: el campo Ψ como objeto; el cableado a ORBE es vía
    # OrbeBinaural::setHeadRadiusM + JNI (el radio, no la clase).
    "spatial/PsiFieldRuntime.hpp",
    # HomogeneousFusion: coordinador Φ v2.5.367; el Φ que corre en la
    # cadena es HyperStateOperator — este coordinador aún no lo reemplaza.
    "spatial/HomogeneousFusion.hpp",
    # --- Fase 0 (2026-11-19, release 2.5.473): 2 huérfanos clasificados. ---
    # Medidor THD+N en double (hermano de dsp/ThdMeter.hpp, misma
    # categoría): primitiva de MEDICIÓN, no de DSP de producción. Sus
    # únicos consumidores son los tests host test_fused_*.cpp (TUs de
    # tests/, excluidas del grafo por diseño). NO va en la ruta PCM del
    # APK: la medición en vivo es off-device por diseño (ver ThdMeter).
    # Se QUITA de esta lista cuando un caller de producción lo necesite.
    "fused/fused_measure.hpp",
    # Etapa HAL MÚSCULO F5 (Bloque 4, Fase 2 v2.5.472): vive en la .so del
    # HAL, que NO existe aún (lo dice el propio header: "lista para
    # enlazarse en 5A"). Su único consumidor hoy es el test host
    # test_ivanna_fusion_shm.cpp. NO es código muerto (contrato probado
    # para 5A) y NO se borra. Se QUITA de esta lista cuando el build del
    # HAL la enlace.
    "daemon/hal/ivanna_hal_stage.hpp",
    # --- Fase 0 (2026-11-20, release 2.5.483): 1 huérfano clasificado. ---
    # Volterra Laguerre H2 standalone (Física 4, Paquete FISICA_NUEVA_
    # CONSOLIDADA 2026-11-18): primitiva probada en host; su único
    # consumidor es el test host test_new_physics_consolidated.cpp
    # (LaguerreVolterra68Hz, sintonía a 68.5 Hz del Genezi). El motor
    # consolidado de producción (fused/new_physics_engine.hpp) lleva su
    # propio Klippel H2+H3 interno, así que este header no se cablea a
    # la ruta PCM. NO se borra: el test lo usa como referencia del
    # kernel Laguerre. Se QUITA de esta lista cuando un caller de
    # producción lo necesite.
    "neuromorphic/laguerre_volterra_h2.hpp",
    # --- Fase 0 (2026-11-21, release 2.5.490): 5 huérfanos clasificados. ---
    # MPC perceptual en sombra (v2.5.489): controlador que decide sin
    # tocar el audio, probado en host (11/11). Sus únicos consumidores
    # hoy son el test host test_perceptual_mpc.cpp (TU de tests/,
    # excluida del grafo por diseño) y los propios .cpp del módulo, que
    # NO están en CMakeLists.txt (el módulo no se compila en el APK).
    # NO está cableado a la ruta PCM todavía: el vector P[t] aún no se
    # alimenta con mediciones reales y la activación exige demostrar
    # decisiones sanas primero. El gemelo acústico SÍ está cableado
    # (lo usa la conservación, W3); el MPC es el consumidor en sombra
    # que falta. NO se borra. Se QUITA de esta lista cuando un caller
    # de producción instancie el controlador y alimente P[t] real.
    "fused/perceptual_mpc/mpc_controller.hpp",
    "fused/perceptual_mpc/operator_simulator.hpp",
    "fused/perceptual_mpc/perceptual_mpc.hpp",
    "fused/perceptual_mpc/perceptual_vector.hpp",
    "fused/perceptual_mpc/twin_bridge.hpp",
    # --- Fase 0 (2026-11-21, Majestuosidad Total P6): 1 huérfano
    # clasificado. ---
    # Modelo Universal de Audición (P6, visión de luna): predictor de la
    # experiencia completa P(experiencia | x, H, R, L, C), probado en
    # host (119/119). Sus únicos consumidores hoy son el test host
    # test_universal_hearing.cpp (TU de tests/, excluida del grafo por
    # diseño) y su propio .cpp, que NO está en CMakeLists.txt (el módulo
    # no se compila en el APK). NO está cableado a la ruta PCM todavía:
    # el modelo predice, no decide ni actúa, y sus constantes son
    # nominales (calibración por razonamiento, no medidas del Genezi).
    # Alimentarlo con datos reales del gemelo/conservación/MPC es el
    # puente futuro (estilo twin_bridge), no este módulo. NO se borra.
    # Se QUITA de esta lista cuando un caller de producción lo instancie.
    "fused/universal_hearing/universal_hearing.hpp",
    # --- Salto / control cerrado (2026-11-22, v2.5.494): 7 huérfanos
    # clasificados. ---
    # Módulos del sistema de control cerrado (M0 contrato AudioState,
    # M3 gemelo, M5 simulador, M6 MPC sombra, M8 supervisor + telemetría
    # y confianza del gemelo), probados en host (suite 1607/1607 con el
    # salto, luego 1619 con P1). Sus únicos consumidores hoy son los
    # tests host test_audio_state.cpp, test_twin_estimator.cpp,
    # test_telemetry.cpp, test_action_simulator.cpp, test_mpc_shadow.cpp,
    # test_uncertainty_supervisor.cpp y test_closed_loop_adversarial.cpp
    # (TUs de tests/, excluidas del grafo por diseño) y sus propios .cpp,
    # que SOLO están en tests/CMakeLists.txt (el módulo no se compila en
    # el APK). Verificado: ningún .cpp de producción incluye estos
    # headers, ninguna de sus clases se usa en producción (la única
    # mención de AudioState en producción es un comentario en
    # ivanna_omega_jni.cpp:1919) y no aparecen en CMakeLists.txt ni en
    # daemon/CMakeLists.txt. NO están cableados a la ruta PCM todavía:
    # el MPC es solo sombra y el gemelo se alimenta por la conservación
    # (W3), no por estos módulos. NO se borran. Se QUITA cada uno de
    # esta lista cuando un caller de producción lo instancie y alimente
    # con estado real. (Medido en worktree 6707684: el fallo ya existía
    # ANTES de la Fase 2 — no lo causó el borrado de las 9 pantallas,
    # lo causó el aterrizaje del salto sin cableado de producción.)
    "fused/closed_loop/action_simulator.hpp",
    "fused/closed_loop/audio_state.hpp",
    "fused/closed_loop/mpc_shadow.hpp",
    "fused/closed_loop/telemetry.hpp",
    "fused/closed_loop/twin_confidence.hpp",
    "fused/closed_loop/twin_estimator.hpp",
    "fused/closed_loop/uncertainty_supervisor.hpp",
    # --- Fase 0 (2026-11-27, release 2.5.509): 2 huérfanos clasificados
    # (informe Ronda 2 del consejo, H1: header_wiring era la causa exacta
    # del fallo determinista de Fase 0 en Tests host). ---
    # Modelo acoplado no lineal D10 (herramienta offline, v2.5.507):
    # header-only con la ecuación de estado acoplada (FORMULACION.md F1);
    # sus únicos consumidores son el CLI offline coupled_predict.cpp y
    # el test host test_coupled_model.cpp (17/17, TU de tests/,
    # excluida del grafo por diseño). NO va en la ruta PCM del APK: el
    # modelo completo es herramienta offline por latencia (5.2–8.5 ms/hop
    # vs presupuesto 2.67 ms/hop). Se QUITA de esta lista cuando un
    # caller de producción lo necesite.
    "fused/coupled_model/coupled_model.hpp",
    # Gemelo Humano P10 (módulo standalone `fused/human_twin/`): banco
    # de pruebas de calibración de Esse (8 pruebas breves, cada una con
    # su eje). Sin caller de producción: P10.2/P10.3/P10.4 están
    # BLOQUEADOS en las sesiones de Esse y en P7/P8/P9 (lo dice el
    # header). Su único consumidor hoy es su test host (TU de tests/,
    # excluida del grafo por diseño). NO se borra. Se QUITA de esta
    # lista cuando un caller de producción lo instancie.
    "fused/human_twin/human_twin.hpp",
}

INCLUDE_DIRS = [
    CPP_ROOT,
    CPP_ROOT / "include",
    CPP_ROOT / "neuromorphic",
    CPP_ROOT / "spatial",
    CPP_ROOT / "supreme",
    CPP_ROOT / "visualizer",
    CPP_ROOT / "experimental" / "adaptive_engine",
    CPP_ROOT / "ivannalab",
    CPP_ROOT / "daemon",
    CPP_ROOT / "daemon" / "core",
    CPP_ROOT / "daemon" / "control",
    CPP_ROOT / "music_intelligence",
    CPP_ROOT / "ime",
    CPP_ROOT / "hexagon",
]


def strip_comments(src: str) -> str:
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.DOTALL)
    src = re.sub(r'//[^\n]*', ' ', src)
    return src


def get_compiled_sources() -> Set[Path]:
    compiled: Set[Path] = set()
    for cmake_path, base_dir in [(MAIN_CMAKE, CPP_ROOT), (DAEMON_CMAKE, CPP_ROOT / "daemon")]:
        if not cmake_path.exists():
            continue
        raw = cmake_path.read_text(encoding="utf-8", errors="replace")
        cleaned = "\n".join(line.split("#", 1)[0] for line in raw.splitlines())
        for m in re.finditer(r'([A-Za-z0-9_./$-]+\.(?:cpp|c|cc))', cleaned):
            token = m.group(1)
            if ":" in token:
                token = token.split(":")[-1]
            full = (base_dir / token).resolve()
            if full.exists():
                try:
                    rel = full.relative_to(CPP_ROOT.resolve())
                    if rel.parts[0] != "tests":
                        compiled.add(full)
                except ValueError:
                    pass
    return compiled


def resolve_include(inc_str: str, parent_file: Path) -> Path | None:
    cand_local = (parent_file.parent / inc_str).resolve()
    if cand_local.exists() and cand_local.is_file():
        return cand_local
    for d in INCLUDE_DIRS:
        cand = (d / inc_str).resolve()
        if cand.exists() and cand.is_file():
            return cand
    return None


def main() -> int:
    all_headers: Set[Path] = set()
    for p in sorted(CPP_ROOT.rglob("*")):
        if not p.is_file() or p.suffix not in {".h", ".hpp"}:
            continue
        rel = p.resolve().relative_to(CPP_ROOT.resolve())
        if rel.parts[0] in {"tests", "legacy_no_build"}:
            continue
        if rel.as_posix() in STANDALONE_HEADERS:
            continue
        all_headers.add(p.resolve())

    compiled_tus = get_compiled_sources()
    visited_files: Set[Path] = set(compiled_tus)
    reached_headers: Set[Path] = set()

    queue: List[Path] = list(compiled_tus)
    inc_re = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.MULTILINE)

    while queue:
        curr = queue.pop()
        if not curr.exists():
            continue
        txt = strip_comments(curr.read_text(encoding="utf-8", errors="replace"))
        for m in inc_re.finditer(txt):
            inc_target = m.group(1)
            resolved = resolve_include(inc_target, curr)
            if resolved is None:
                continue
            try:
                rel = resolved.relative_to(CPP_ROOT.resolve())
            except ValueError:
                continue
            if rel.parts[0] == "tests":
                continue
            if resolved.suffix in {".h", ".hpp"}:
                reached_headers.add(resolved)
            if resolved not in visited_files:
                visited_files.add(resolved)
                queue.append(resolved)

    orphans = sorted(all_headers - reached_headers)

    print("=== [0.2] CHECK HEADER WIRING REPORT ===")
    print(f"Runtime headers checked (.h/.hpp)  : {len(all_headers)}")
    print(f"Reached from compiled CMake TUs    : {len(reached_headers)}")
    print(f"Standalone/test/HAL/experimental headers scoped out: {len(STANDALONE_HEADERS)}")
    for name in sorted(STANDALONE_HEADERS):
        print(f"  - {name}")
    print(f"Orphan headers                     : {len(orphans)}")
    print()

    if orphans:
        print(f"[FAIL] Headers de producción huérfanos ({len(orphans)}):")
        for h in orphans:
            print(f"  - {h.relative_to(ROOT)}")
        if "--ratchet" in sys.argv and len(orphans) <= 22:
            print(f"[RATCHET-OK] Headers huérfanos ({len(orphans)}) <= baseline (22).")
            return 0
        return 1

    print("[PASS] 0 headers de producción huérfanos.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
