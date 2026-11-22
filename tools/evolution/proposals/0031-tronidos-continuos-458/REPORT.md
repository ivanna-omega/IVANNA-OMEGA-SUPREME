# 0031 — Tronidos continuos en la 2.5.458: gobernador de shed compartido entre BridgePlayer y captura

Fecha: 2026-11-16. Rama: `ivanna-real-completa` (HEAD `afef3ed7`). Para la 2.5.459.

## 1. El síntoma (en vivo, verificado por SSH)

Esse instaló la 2.5.458 (vc 749) y reporta que **sigue tronando "de inicio a fin"**
(2026-11-16 14:17 CST): el tronido es CONTINUO durante toda la canción, no solo
al cambiar de pista. Esto descarta el priming de cambio de pista (0025) como
causa principal.

Evidencia en vivo:

| Dato | 11:16 (457) | 13:44 (458) | 14:17 (458) |
|---|---|---|---|
| AudioFlinger underruns | 294 | 350 | 354 |
| AudioFlinger overruns | 588 | 713 | 761 |
| BridgePlayer pll_occ (objetivo 46128) | 46723–48388 | 44799–50303 | — |
| BridgePlayer pll_rail / pll_snap | 0 / 0 | 0 / 0 | — |
| BridgePlayer dsp_us | 6645–10266 | 4633–7923 | — |
| Captura pll_occ (objetivo 3848) | 7619 (99%) | 2736 | 3559 |
| Captura pll_rail | 1 | 0 | 0 |
| **shed** | — | **0.015** | **0.280** |
| dsp_pk_ses | — | 89182 µs | 89182 µs (mismo valor) |
| gate_holos_tier | — | 2 | 2 |

Dos lecturas:

1. **Los underruns (~1 cada 8 min en la 458) son demasiado raros para explicar
   un tronido continuo.** Un underrun es un hueco aislado cada ~8 minutos; el
   tronido que oye Esse es continuo, de inicio a fin. Son DOS síntomas
   distintos. Los underruns residuales quedan como tema secundario (§6); el
   tronido continuo es el tema de esta propuesta.
2. **El shed subió de 1.5% a 28% entre las 13:44 y las 14:17.** Y el pico de
   sesión (89 ms) es el MISMO valor en ambas lecturas: un pico aislado
   (arranque/GC), no recurrente. El shed es el sospechoso que se mueve con
   el síntoma.

## 2. Causa raíz (verificada en código)

**Un solo `DspLoadShedGovernor` global servía a los dos llamadores de
`nativeProcess` (`DSPBridge.process`), que tienen periodos opuestos.**

- `app/src/main/cpp/jni/ivanna_omega_jni.cpp:1734` (antes del parche):
  `static ivanna::dsp::DspLoadShedGovernor g_dspShedGovernor;` — una sola
  instancia, compartida por todos los llamadores.
- BridgePlayer (MÚSICA): `~1984 frames @48 kHz = 41.3 ms` de periodo,
  presupuesto 85% = 35.1 ms, coste DSP ~6 ms (**muy por debajo**).
- Captura (`PlaybackCaptureService.kt:80`, `BLOCK_FRAMES = 320`):
  `320 frames @48 kHz = 6.67 ms`, presupuesto 5.67 ms, coste DSP ~8 ms
  (**por encima**).
- La captura genera deuda crónica (+2.3 ms/bloque) y esa deuda hacía que el
  BridgePlayer soltara bloques aunque su propio coste estaba muy por debajo
  de su presupuesto.

**Por qué un bloque en shed suena como tronido** (ya documentado en el repo,
`DspLoadTierPolicy.kt`, medido en vivo 2026-11-12): "Cada cambio
procesado->seco es un tronido, ~cada 20-30 ms: 'puro tronido, casi no hay
melodía'". Al soltar un bloque, el DSP no avanza su estado (STFT
overlap-add, IIR, envolvente del compresor) pero el audio sí avanza; al
reanudar, el DSP procesa con estado atrasado y el transitorio se oye como
tronido. A ~5 transitorios por segundo (24 bloques/s × ~20% de shed), el
tronido es continuo, "de inicio a fin".

**Simulación del gobernador compartido** (modelo exacto de
`dsp_load_shed_governor.hpp`, 10 s, costes medidos en vivo, captura cada
6.67 ms + BridgePlayer cada 41.3 ms): captura shed 16.1%, **BridgePlayer
shed 20.2% sin estar sobrecargado**, global 16.7%. El orden de magnitud
coincide con el shed=28% en vivo. La simulación vive como test
(`test_shed_governor_split.cpp`, `SharedGovernorLeaksCaptureDebtIntoBridgePlayer`).

**Contaminación adicional:** el mismo global contaminaba `blockBudgetNs` [3]
(periodo del ÚLTIMO bloque: alterna 6.67 ms y 41.3 ms) y por tanto la carga
(`emaUs/budgetNs`) que lee `DspLoadTierPolicy`/`ThermalGovernor`: el tier no
podía distinguir qué ruta estaba sobrecargada. La 0025 (priming + pacing de
captura) no tocó el gobernador: por eso el tronido persistió en la 458
aunque la captura estuviera sana en telemetría.

## 3. El fix (0031)

**Un gobernador por ruta, enrutado por el PERIODO del bloque.**

- `app/src/main/cpp/dsp/dsp_load_shed_governor.hpp` (nuevo):
  `RouteShedGovernors` — dos instancias `DspLoadShedGovernor` (`large_` para
  BridgePlayer, `small_` para captura) + `forPeriod(periodUs)`. Umbral
  `kShedLargeBlockPeriodUs = 20000` µs (20 ms), entre 6.67 ms y 41.3 ms con
  margen 3x por cada lado. El presupuesto es 85% del periodo, así que dos
  periodos distintos no pueden compartir deuda sin contaminarse.
- `app/src/main/cpp/jni/ivanna_omega_jni.cpp`:
  - `g_dspShedGovernor` → `g_shedGovernors` (`RouteShedGovernors`).
  - `nativeProcess`: el chequeo de shed, `observeShed` y `observeRun` usan
    `forPeriod(periodUs)` — la deuda va al gobernador de LA RUTA del bloque.
  - `blockBudgetNs` solo se publica para bloques grandes (BridgePlayer):
    `DspLoadTierPolicy` divide `emaUs [1]` entre `budgetNs [3]` y ambos
    deben ser de la misma ruta; si la captura publicara su periodo, [3]
    alternaría 6.67 ms / 41.3 ms y la carga oscilaría entre 1.06 y 0.17 con
    el mismo coste (flapping del tier). La captura no necesita publicar: su
    protección es su propio gobernador pequeño + el `LoopLoadGovernor` de
    Kotlin.
  - `logDspShedStateThrottled()`: loguea solo el gobernador grande
    (BridgePlayer, la ruta que se escucha). El gobernador pequeño suelta
    ~29% por diseño y eso es operación normal, no anomalía.
  - `nativeReset()`: resetea ambos gobernadores.
  - `nativeGetDspShed()`: array de 26 → **27 floats** (índices 0..25 sin
    cambio, patrón establecido). [0]/[1]/[2]/[25] son ahora del gobernador
    grande (BridgePlayer); **[26] nuevo = `captureShedFraction`** (shed del
    gobernador pequeño, captura). La telemetría `shed=` de MÚSICA ahora
    mide solo el BridgePlayer.
- Sin cambios Kotlin (el fix es 100% nativo; la CI debe compilar el JNI
  con el NDK, como siempre — el JNI no se compila en host).

Efecto esperado en vivo: el BridgePlayer no suelta nunca en régimen (coste
~6 ms << presupuesto 35.1 ms) → **se acaban los tronidos continuos**. La
captura sigue soltando su ~29% natural (su protección diseñada contra el
desborde del RecordThread) y el tier de HOLOS vuelve a medir la carga del
BridgePlayer (HOLOS ya no se degrada a Limited por la sobrecarga de la
captura).

## 4. Consejo

**NO DISPONIBLE.** Tres intentos de `council.py` fallaron en el proposer con
gateway HTTP 429 (DuckDuckGo: `ERR_INPUT_LIMIT` y luego `ERR_USER_LIMIT`,
rate limit del proveedor gratuito ddgw). No se emitió veredicto
(`council_verdict.json` documenta la indisponibilidad). El coordinador
implementó el parche directamente (patrón 0023/0024/0025/0027/0028/0029),
con la causa raíz verificada en código, simulación del gobernador y la cita
de `DspLoadTierPolicy.kt`. Los pro de Gemini siguen en cooldown hasta
2026-11-17 ~02:00 CST.

## 5. Validación (todo verde)

- Test nuevo `test_shed_governor_split`: **5/5 PASS** (binario compilado y
  corrido en host). Reproduce el bug con el modelo compartido y verifica el
  aislamiento con el modelo separado, el enrutado por periodo, que un
  BridgePlayer genuinamente sobrecargado sigue soltando, y el reset de ambos.
- CTest completo: **1125/1125 PASS** (1120 antes + 5 nuevos de 0031).
  Toolchain de respaldo `~/workspace/toolchain/gcc14` (el g++-14 del sistema
  desapareció tras el reciclaje de la VM).
- Suite evolución: **168/168 OK**. Un fallo apareció en la primera pasada
  (`test_scan_todo_fixme.test_0019_real_repo_reports_one`): el TODO
  `route-arbiter-v2` en `ivanna_omega_jni.cpp` se movió de la línea 1836 a
  la 1848 por las 12 líneas netas que 0031 añade antes de él. NO es un
  debilitamiento: es el pin de línea real que el propio test documenta
  ("el pin sigue la línea real, no la histórica"), actualizado con su
  comentario de historial como en 0024/0027. Re-verificado: 168/168 OK.
- check_docs_claims: **PASS** (0 hallazgos).
- El JNI no se compila en host (sin NDK): la CI debe compilarlo al pushear.
  Los cambios JNI son mecánicos (mismo API del gobernador, solo cambia la
  instancia por ruta) y el header nuevo compila limpio en host (lo prueba
  el test nuevo).

## 6. Temas que este parche NO resuelve (documentado, no escondido)

- **Underruns residuales (~1 cada 8 min, 350 → 354):** el shed no causa
  underruns (el bloque en shed escribe el audio seco, sin perder muestras),
  así que este parche no los toca. Son un tema aparte y raro. Candidatos:
  un bloque lento aislado (el pico de sesión 89 ms no basta solo: el
  colchón es ~0.96 s), un stall del decodificador en algún archivo, o una
  preemption del hilo. Prueba que lo cierra: loguear `underruns` por minuto
  de escucha junto a `dsp_pk` (ventana) y al archivo que suena; si un
  underrun coincide con un `dsp_pk` alto, es el DSP; si no, es el
  decodificador/almacenamiento.
- **La captura sigue soltando ~29%:** es su régimen natural por diseño
  (coste ~8 ms > presupuesto 5.67 ms). Su salida es una copia con delay
  del mezclador y su shed no se oye en MÚSICA. Reducir su coste (HOLOS más
  barato en captura, o subir su presupuesto) es trabajo futuro, no de
  esta propuesta.
- **El tier de HOLOS ahora mide solo el BridgePlayer:** si solo corre la
  captura (Tidal en app nativa, sin MÚSICA), el tier se queda en 0 y la
  captura no degrada HOLOS por carga. Su protección sigue siendo el
  gobernador pequeño + el `LoopLoadGovernor` de Kotlin. Aceptado por diseño.

## 7. Protocolo de verificación en vivo (con la 2.5.459 instalada)

Con MÚSICA sonando (y la captura activa, como la usa Esse):

1. `logcat -s IVANNA.Telemetry | grep route=A` → **`shed=` debe ser ~0.00**
   (antes 0.28). Si `shed=` sigue alto, el aislamiento no funcionó.
2. `dumpsys media.audio_flinger | grep underruns` → anotar el valor al
   empezar y tras 10 min de escucha. El incremento por minuto debe bajar
   respecto a ~1 cada 8 min (los underruns son tema aparte, §6, pero no
   deben subir).
3. `logcat -s IVANNA.Telemetry` → `dsp_us` estable (4–8 ms), `dsp_pk`
   (ventana) < 35 ms, `gate_holos_tier` en 0 (ya no en 2 por la captura).
4. **Escucha de Esse (la prueba que cuenta):** MÚSICA de inicio a fin sin
   tronidos. Si truena, anotar en qué segundo de la canción y si coincide
   con un cambio de pista (priming, 0025) o a mitad de canción (este fix).
5. Telemetría nueva `[26]` (shed de captura, ~0.29 esperado): se lee por
   `nativeGetDspShed()`; el Kotlin aún no la muestra en el log (sin cambios
   Kotlin en esta propuesta). Verificación por `adb` + llamada al getter o
   en la siguiente propuesta que toque `IvannaTelemetry.kt`.

## 8. Archivos del parche (sin commitear)

Modificados (2):
- `app/src/main/cpp/dsp/dsp_load_shed_governor.hpp` (+~60, `RouteShedGovernors` + umbral)
- `app/src/main/cpp/jni/ivanna_omega_jni.cpp` (gobernador por ruta, `blockBudgetNs` solo bloques grandes, telemetría 27 floats, reset doble)

Nuevos (2):
- `app/src/main/cpp/tests/test_shed_governor_split.cpp` (5 tests)
- `app/src/main/cpp/tests/CMakeLists.txt` (registro del test)

Informes (3):
- `tools/evolution/proposals/0031-tronidos-continuos-458/REPORT.md` (este)
- `tools/evolution/proposals/0031-tronidos-continuos-458/council_verdict.json`
- `tools/evolution/proposals/0031-tronidos-continuos-458/finding.json`

NO commitear (pre-existentes, excluidos por regla): los archivos de 0030
(`IvannaBridgePlayer.kt`, `IvannaTelemetry.kt`, `IvannaTelemetryTest.kt`,
`DirectRouteProbe.kt`, `DirectRouteProbeTest.kt`, `FloatToPcm24Test.kt`),
`ivanna_daemon_v1_legacy.cpp.bak`, `hal_integration/.../pal_preload_probe/`,
`magisk_module_fusion/`, `monetization/`, `perfection_gate_report.json`,
propuestas 0008/0009/0010.
