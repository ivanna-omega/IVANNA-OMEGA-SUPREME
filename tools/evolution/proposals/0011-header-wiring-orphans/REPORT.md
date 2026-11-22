# Propuesta 0011-header-wiring-orphans — check_header_wiring en rojo

- **Hallazgo:** `check_header_wiring.py` en rojo (12 headers de
  producción huérfanos, no alcanzados desde ningún TU compilado en
  CMakeLists). Verdad medida 2026-11-16, header por header (quién lo
  incluye, si tiene .cpp, si está en CMakeLists, qué dice su propio
  header/docs): NINGUNO es un huérfano por accidente. Son 3 grupos:
  - **Legacy daemon v1 (4):** `daemon/control/command_server.h`,
    `daemon/core/effect_integrity.h`, `daemon/hal_clock_producer.hpp`,
    `include/write_stage_asrc_telemetry.hpp` (solo la usa
    command_server). El daemon v1 se reemplazó por el CEREBRO v2 en la
    2.5.401 (lo documenta `daemon/CMakeLists.txt:19-25`); ya no se
    compilan por diseño y se conservan como referencia (nunca borrar).
  - **Primitivas de test / trabajo futuro (5):**
    `device_cpu_profiler.hpp` (futuro puente JNI, documentado en el
    header), `include/poller_sched.hpp` (P0-b, el poller no la consume
    todavía), `spatial/DopPacketizer.hpp` y
    `spatial/PinnaElevationEq.hpp` (pilares pendientes,
    docs/PILARES_PENDIENTES.md), `spatial/TransducerOperator.hpp`
    (operador MANT Ec. 1; necesita una respuesta MEDIDA del transductor,
    lo dice el header).
  - **Objetos standalone Ψ/Φ (3):** `spatial/BioControl5D.hpp`
    (dinámica neural η), `spatial/PsiFieldRuntime.hpp` (el campo Ψ como
    objeto; el cableado a ORBE es vía `OrbeBinaural::setHeadRadiusM` +
    JNI, no vía #include), `spatial/HomogeneousFusion.hpp`
    (coordinador Φ v2.5.367; el Φ que corre es HyperStateOperator).
- **Consejo:** no procesó este hallazgo. El coordinador lo clasificó
  directo con la evidencia medida.
- **Autor del parche:** COORDINADOR (no el consejo).
  `apply_proposal.py --dry-run --allow-no-council-record`:
  `already_applied` exact (el parche es exactamente lo que está en el
  árbol).
- **Parche (2 archivos):**
  - `scripts/check_header_wiring.py`: los 12 entran a
    `STANDALONE_HEADERS` con su razón documentada por grupo, siguiendo
    el patrón que el propio check ya tenía para los 8 excluidos.
    Clasificar no es esconder: si la razón deja de aplicar (se cablea
    a producción), se quita de la lista y el check lo exige. El check
    ahora distingue "huérfano por diseño (documentado)" de "huérfano
    por accidente (hallazgo real)".
  - `tools/evolution/tests/test_check_header_wiring.py` (nuevo, 3
    tests): los 12 están clasificados; ninguna entrada de
    STANDALONE_HEADERS está obsoleta (todas existen en el repo — la
    lista no se pudre); el check pasa en el repo real.
- **Oportunidades de cableado (documentadas, NO parte de este fix):**
  `device_cpu_profiler.hpp` (puente JNI), `poller_sched.hpp` (poller
  del daemon), `PsiFieldRuntime`/`BioControl5D` (clase en ORBE),
  `HomogeneousFusion` (reemplazar a HyperStateOperator). Cada una es
  un cambio de DSP/arquitectura que necesita su propio diseño y
  verificación en vivo; no se fuerzan dentro de un fix de auditoría.
- **Validación:**
  - `python3 scripts/check_header_wiring.py` → **[PASS] 0 headers de
    producción huérfanos** (exit 0; antes 12).
  - Nuevos tests: **3/3 OK**.
  - Suite evolución completa: **148/148 OK** (145 + 3 nuevos).
  - `apply_proposal.py --dry-run`: `already_applied` exact.
- **Archivos:** `scripts/check_header_wiring.py`,
  `tools/evolution/tests/test_check_header_wiring.py`
- **Veredicto:** VERDE — lista para revisión/commit (por el operador).
  Con esto, **los 5 checks host quedan en VERDE**.
