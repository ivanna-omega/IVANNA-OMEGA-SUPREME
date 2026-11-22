# 0019 — TODO/FIXME: la palabra española TODO no es una tarea

## Hallazgo

El scanner `scan_todo_fixme` contaba 28 TODO/FIXME. Solo **1 era real**. El resto
eran falsos positivos de tres clases:

| Clase | Ejemplos | n |
|---|---|---|
| Español ("todo" = todo/all) | "TODO el audio", "TODO apagado", "se procesa TODO" | ~24 |
| Ya resuelto / dudoso | "Sustituye por completo los dos TODO" (usb_audio_pro_manager.cpp:36); "ver TODO" (PsiField.hpp:159) | 2 |
| Vendoreado | `TODO(niels)` en nlohmann/json.hpp:10997 | 1 |
| **REAL** | `TODO(route-arbiter-v2)` en `jni/ivanna_omega_jni.cpp:1803` | **1** |

## Fix (2 archivos)

`tools/evolution/evolution_engine.py`:

- **`scan_todo_fixme`**: solo cuentan marcadores `TODO:` / `TODO(` / `FIXME:` /
  `FIXME(` (con dos puntos o paréntesis — la convención de marcador de tarea).
  La palabra suelta TODO (español) ya no se cuenta.
- Excluye `app/src/main/cpp/include/nlohmann` (vendoreado, no deuda de IVANNA).
- `cwd=REPO` explícito (mismo bug latente que 0012: `run()` liga el default
  en tiempo de definición).

**El TODO real (no se toca en este parche, se documenta):**
`jni/ivanna_omega_jni.cpp:1803` — cuando PlaybackCapture y omega_effect coexistan,
leer `OmegaControlBus::readLatest()` y hacer passthrough solo si
`route==SYSTEM_WIDE` Y el buffer viene de MediaProjection loopback.
Es trabajo de Ruta B / arbitraje de rutas, fuera del alcance de este scanner.

## Incidente descubierto en validación (resuelto en este mismo parche)

Al validar en scratch, `scan_dead_functions` reportaba 8 muertos y en el repo
real 6, no 0. Causa verificada: el scanner usaba `git grep`, que **no ve
archivos untracked**. Los helpers de los tests nuevos (SendOneFd, MockDaemon,
FillSine, ReadParams, AllFinite…) viven en archivos aún sin commitear
(`test_omega_daemon_bridge.cpp`, `test_saf_jni_bridge.cpp` en `??`;
`test_pi_lstm_bridge_jni.cpp` staged como `A` sí se veía).

Fix: `scan_dead_functions` ya no usa `git grep`. Nueva búsqueda en Python
(`_source_files_for_caller_search()` + `_has_caller()`): lee el árbol de
trabajo una vez (`.cpp/.h/.hpp/.c/.cc` bajo `app/src/main/cpp`, excluye
`third_party` y `build`), busca el nombre con `\b` y descarta solo la línea
de la propia definición. El resultado ya no depende del estado de git.

## Validación

- 4 tests nuevos (`test_scan_todo_fixme.py`) 4/4 OK: español no se cuenta,
  marcador real sí, nlohmann excluido, repo real reporta 1 (jni:1803).
- 1 test nuevo (`test_scan_dead_functions.py`, `test_0019_untracked_file_caller_is_found`)
  5/5 OK: un helper llamado en un archivo SIN commitear cuenta como usado.
- Scan real tras el fix: **TODO/FIXME 28 → 1**; **funciones muertas 0**
  (antes 6 por el incidente de `git grep`).
- Parche validado en scratch: base 0007+0012+0017, aplica limpio,
  19 tests OK (4 todo + 5 dead + 7 saf guards + 3 untested).
- `apply_proposal.py --dry-run`: `already_applied` exact.

## Dependencias

Requiere 0007, 0012 y 0017 aplicados primero (motor). Parche: 242 líneas,
3 archivos (`evolution_engine.py`, `test_scan_todo_fixme.py` nuevo,
`test_scan_dead_functions.py` +1 test).
