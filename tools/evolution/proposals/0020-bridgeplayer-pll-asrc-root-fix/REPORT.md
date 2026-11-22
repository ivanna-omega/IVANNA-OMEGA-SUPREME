# 0020 — BridgePlayer PLL/ASRC: fix de raíz del control de nivel (457)

## Diagnóstico verificado en vivo (2026-11-16 04:41 CST, IVANNA 2.5.456)

- `pll_occ` clavado ~98k (cap 92_256, objetivo 46_128), `pll_ph` +0.56,
  `pll_dem` ~−435, ppm ~−450, `pll_snap=2048` disparando SIEMPRE, pero el
  buffer NO baja (97_568 → 97_967 → 98_429, SUBE).
- Síntomas de Esse: MÚSICA = "metralleta" (micro-cortes); con snap apagado =
  "cámara rápida" (audio acelerado). Files (reproductor del sistema) suena
  BIEN. Qobuz = feo pero PROBADO que NO es IVANNA (force-stop total, Qobuz
  sigue distorsionando al cambiar de canción).
- PAL no tiene control de reloj/ppm/ratio (196 controles, cero): el ÚNICO
  actuador de reloj es el ASRC de IVANNA en el write-stage. La deriva real
  es +8.5 ppm (pequeña). Criterio de éxito en vivo: ppm ~+8.5 y buffer en
  el objetivo; si se queda en −450, sigue roto.

## Causa raíz (verificada en código)

El nivel del buffer lo fija el equilibrio de `track.write(WRITE_BLOCKING)`,
no el snap ni el ppm:

1. El productor (decodificador) no tiene límite de tasa: decodifica por
   delante y llena el buffer (minBuf×12 = 92_256 frames) al arrancar
   (priming a lleno, no al objetivo 46_128). Después, `write()` bloqueante
   lo mantiene lleno: el productor escribe un bloque completo cada vez que
   hay sitio.
2. El snap (455) soltaba 2048 frames de CONTENIDO por sondeo, pero el
   productor rellenaba el hueco con el siguiente bloque: el nivel no bajaba,
   solo se perdía audio — 2048 frames cada 250 ms = hasta 17% del contenido
   saltado (la metralleta). El modelo host mide 10.8% perdido con el snap.
3. El ppm solo drena 24 frames/s (±500 ppm): no puede con 52k de exceso.
   Su demanda −450 ppm era casi toda error de NIVEL, no deriva (la deriva
   real es +8.5 ppm).

El cableado del snap EXISTÍA y el nativo SÍ aplicaba el ppm (−450 medido en
`routeAAsrc.clockCorrectionPpm`): no era un problema de cableado, era el
actuador equivocado para el nivel.

## Fix (control de nivel por RITMO DE ESCRITURA)

El nivel lo controla el ritmo al que el productor escribe, no un salto
discreto:

- **`IvannaBridgePlayer.kt` — pacing:** en cada vuelta del bucle, antes de
  decodificar/escribir, estima la ocupación viva
  (`bufferPll.estimateOccupancy`). Si supera el objetivo (capacidad/2), el
  productor se PAUSA (`delay` acotado a 50 ms/vuelta, re-evaluado cada
  vuelta) y deja que el track drene. Sin pérdida de audio: el codec retiene
  sus buffers de salida (backpressure natural) y el ASRC/FIFO no se toca.
  El exceso inicial (~52k frames) drena en ~1 s, progresivo; el track nunca
  se queda sin audio (siempre guarda ~el objetivo, ~0.96 s). Al pausar, el
  reproductor resetea el integrador de nivel del PI (ver abajo).
- **`AudioTrackBufferPll.kt`:**
  - `estimateOccupancy(submittedFrames, nowNs)`: ocupación viva entre
    sondeos (ancla = último sondeo: consumed + timestamp; el consumo se
    estima al ritmo nominal — un error de deriva de decenas de ppm mueve la
    estima <1 frame en 250 ms). Antes del primer sondeo, la ocupación es lo
    enviado (frena el priming antes de que el buffer se llene).
  - `resetLevelIntegral()`: resetea solo el integrador de nivel del PI (la
    estima de reloj no se toca). Sin esto, 50 sondeos sobre-llenos acumulan
    ~1.6 s de error y la demanda seguiría en ~−200 ppm aunque el nivel ya
    esté en el objetivo (el reset del snap 455 hacía esto; el pacing toma
    su lugar).
  - **Snap DESACTIVADO (legacy):** `levelSnapFrames` siempre es 0; las
    constantes se conservan como referencia histórica. El ppm queda solo
    para la deriva.
- **`RouteAAsrc.kt` — snap legacy:** `setLevelSnapFrames` es no-op (el
  reproductor ya no lo llama); `applyLevelSnap` queda como pass-through
  (pendingSnapFrames siempre 0). No se borra código (regla del repo).
- **ASRC nativo C++ NO tocado** (su ratio es correcto; el problema era el
  actuador de nivel, no el remuestreador). IntegrityGate no tocado.

## Validación

- **Test host C++ nuevo** (`test_bridgeplayer_level_control.cpp`, 5 tests,
  modelo de la dinámica del bucle con los números reales): reproduce el
  fallo con el snap (nivel final 92_016 de 92_256, 10.8% del contenido
  perdido, ppm −389) y valida el fix con pacing (nivel final 46_128 =
  objetivo exacto, máx 48_112 = objetivo + 1 bloque, 0 perdido,
  ppm +8.5 exacto). **Cambio de pista (2 pistas seguidas, 5 s cada una):**
  con snap, la 2ª pista también se sobre-llena (máx >90% cap) y pierde
  contenido; con pacing, la 2ª pista arranca al objetivo (máx ≤ objetivo +
  2 bloques), converge al objetivo y pierde 0. 5/5 PASSED.
- **Tests Kotlin del PLL actualizados en el mismo parche** (regla del repo):
  los 4 tests del snap 455 ahora afirman snap=0 y el reset por pacing;
  3 tests nuevos (`estimateOccupancy` ×2, `resetLevelIntegral` preserva el
  reloj). Corren en CI (no hay SDK Android en host).
- CTest completo: **1060/1060, 100% passed** (1055 + 5 nuevos), 55.67 s.
  `check_docs_claims` [PASS]; README + `telemetry/ctest_summary.json`
  actualizados a 1060/1060. **Nota (flaky, no bloqueo):** en la primera
  corrida completa bajo `ctest -j4`, `OmegaDaemonBridgeTest.
  SecondCallReturnsCachedPointer` abortó una vez (0018, mock daemon /
  SHM fijo bajo carga paralela); al re-correr la suite completa dio
  1060/1060 y ese test pasó. Queda anotado como flaky de ejecución
  paralela, no como regresión de 0020.
- Suite evolución: **167/167 OK** (243.6 s).
- Parche validado en scratch (`git worktree` HEAD + 0013+0015+0016+0017+
  0018): aplica limpio, configura, build del test nuevo y **5/5 PASSED**.
  `apply_proposal.py --dry-run`: `already_applied: exact`.

## Verificación en vivo pendiente (457, con luz verde de Esse)

- MÚSICA ('Tom Sawyer', 'Fly Like an Eagle' — los tracks que sonaron
  metralleta): sin micro-cortes, sin cámara rápida.
- **Cambio de pista:** al brincar de canción, sin tronidos ni distorsión
  (el disparador era el re-priming; el pacing frena el priming de cada
  pista al objetivo desde el primer bloque).
- Telemetría: `pll_occ` ≈ `pll_tgt` (46_128), `pll_snap=0`, ppm ~+8.5
  (no −450), `pll_rail=0`.
- Log `Pacing de nivel activo` al arrancar (drena el exceso inicial) y
  luego silencio (nivel en objetivo, sin pacing).

## Hipótesis "TAPÓN" de Esse (2026-11-16 05:17 CST) — investigada

Esse: IVANNA influye en el audio global de tal forma que aunque después se
apague IVANNA, el buffer nativo se queda LLENO (tapón que no drena); el
tapón sigue ahí aunque IVANNA ya esté apagada. Evidencia: con IVANNA en
force-stop total, Qobuz SEGUÍA distorsionando al cambiar de canción
(04:50 CST); solo al REINICIAR Qobuz sonó bien (05:13 CST).

**Hallazgo — la liberación de IVANNA está verificada limpia (código):**
- Captura: `stopEngineLocked()` llama `engine.stop()` + `engine.cleanup()`
  (AudioTrack stop+release, AudioRecord stop+release, sesión de efectos
  global cerrada, callback de proyección desregistrado, hilos terminados);
  `SystemAudioCapture.stopCapture()` detiene el AudioRecord, hace join del
  hilo, libera el AudioRecord y llama `mediaProjection.stop()` + null
  (FIX-3 explícito para no dejar el token activo). `onDestroy()` llama
  `stopEngine()`.
- BridgePlayer: `stop()` → `releaseTrackLocked()` (pause + flush + stop +
  release del AudioTrack); el `finally` de `runDecodeLoop` cierra el ASRC,
  cancela la telemetría y libera codec/track.
- Force-stop (SIGKILL) no corre esa limpieza, pero Android limpia por
  muerte del binder: AudioFlinger retira los AudioTracks/AudioRecords del
  proceso muerto y el sistema libera la MediaProjection. **No hay mecanismo
  en la arquitectura de audio de Android por el que una app muerta deje un
  tapón persistente en el mezclador compartido** (la captura es solo
  lectura: no escribe ni modifica el track de la app capturada; el efecto
  Ruta B está INACTIVO).

**Conclusión honesta:** la evidencia apunta a que el estado malo estaba en
el propio Qobuz (su pista/sesión), no en un tapón del sistema dejado por
IVANNA: Qobuz se arregló solo con reiniciar Qobuz (proceso y pista nuevos)
mientras IVANNA ya estaba muerta. Si el tapón fuera del mezclador
compartido, la pista nueva de Qobuz seguiría distorsionando. Lo más
probable es que **Qobuz tenga su propio bug de cambio de pista** (la misma
clase de sobre-lleno que el BridgePlayer, pero en su propio reproductor).
El fix 0020 SÍ cubre el sobre-lleno de IVANNA (BridgePlayer) en el cambio
de pista; el de Qobuz no es código de IVANNA y no se puede arreglar desde
aquí. Si Esse vuelve a ver el tapón con IVANNA viva y Qobuz sonando bien
tras reiniciar, se reabre con esa evidencia.

## Click 1 Hz (2026-11-16 05:09–05:12 CST) — bug SEPARADO, no en 0020

Con IVANNA sonando, Qobuz solo daba UN CLICK POR SEGUNDO, sin música; al
hacer force-stop total de IVANNA, LOS CLICKS SE FUERON (probado que eran
de IVANNA). Tras reiniciar Qobuz + subir el volumen, Qobuz YA SUENA BIEN
(05:13 CST). Es un bug de la ruta de CAPTURA (Qobuz vía
PlaybackCaptureService), no del BridgePlayer (MÚSICA): el click es
exactamente 1 Hz (la cadencia del poller de telemetría) y la captura tiene
su propio PLL + ASRC + AudioTrack de salida. No se arregla en 0020 (que es
el control de nivel del BridgePlayer); queda documentado para su propio
fix (candidato 0021): investigar la salida de captura (PLL/ASRC/track de
re-inyección, antiPopEngine, masterTiming) y si el click coincide con el
sondeo de telemetría. La captura podría beneficiarse del mismo pacing en
su track de salida en ese fix.

## Archivos

- `app/src/main/java/com/ivanna/omega/audio/AudioTrackBufferPll.kt`
- `app/src/main/java/com/ivanna/omega/audio/IvannaBridgePlayer.kt`
- `app/src/main/java/com/ivanna/omega/audio/RouteAAsrc.kt`
- `app/src/test/java/com/ivanna/omega/audio/AudioTrackBufferPllTest.kt`
- `app/src/main/cpp/tests/test_bridgeplayer_level_control.cpp` (nuevo)
- `app/src/main/cpp/tests/CMakeLists.txt` (+1 test)
- `README.md`, `telemetry/ctest_summary.json` (conteo 1055 → 1058)
