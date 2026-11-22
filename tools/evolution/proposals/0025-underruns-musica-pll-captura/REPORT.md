# 0025 — Underruns en MÚSICA + PLL de captura saturado en el riel

**Hallazgos en vivo (2026-11-16 11:16 y 11:18 CST, Esse instaló la 2.5.457 vc 748).** Prioridad máxima del encargo 0024 (elevación magistral del DSP). Dos problemas separados.

## 1. MÚSICA: los cortes siguen pese a 0020 (underruns=294)

### 1.1 Lo que dice la telemetría en vivo
- BridgePlayer SANO: `pll_occ=46723` (objetivo 46128), `ppm=+2.4`, `pll_rail=0`, `pll_snap=0`, `dsp_us=6645`. El pacing de 0020 SÍ funciona.
- PERO AudioFlinger marca **underruns=294** (el track se queda sin audio = los cortes audibles).
- Corrección a la hipótesis inicial: la telemetría NO estaba congelada (se actualiza; lo de 11:16 era música en pausa / valores estables).

### 1.2 El DSP NO es demasiado lento (medido en host)
`dsp_us` es la EMA del coste de `nativeProcess` por chunk, y el periodo es `n * 1e6 / sampleRate` (jni:1843). El BridgePlayer procesa chunks de hasta 2048 frames (42.67 ms de periodo). `dsp_us=10266 µs` = **24% del presupuesto**. El DSP no es el cuello en media.

Distribución del coste por etapa (host, `test_route_a_chain_cost_410`, bloque 320, periodo 6666 µs, presupuesto 70% = 4666 µs):
- Cadena total en serie: media 1507.82 µs, p50 1546.55, p95 1905.59, p99 2263.64, **max 5371.94 µs**, RT 0.226x, **2/2000 bloques sobre presupuesto**.
- HOLOS: media 486 µs, **max 9302 µs**. ORBE: media 270 µs, max 956 µs. OmniField: media 238 µs, max 3122 µs. Φ: media 164 µs, max 317 µs.
- Picos en frío / operaciones periódicas: SafetyLimiter max 43857 µs, Unity cold-start max 46934 µs, ASRCv6 max 44870 µs, DiffReinjector max 42864 µs, PDEngine(modo 2) max 12143 µs, WFS max 16195 µs.
- Gobernador: shed en teléfono real **3.4%** (bloques en passthrough seco por sobre-presupuesto).

Lectura honesta: el DSP va al ~72% de tiempo real en media en el teléfono (estimado por el factor host→teléfono 3.203) y tiene picos de 140-150 ms escalados. Pero un solo pico NO vacía el colchón de 0.96 s (objetivo 46128 frames) del BridgePlayer. Los picos NO explican underruns en régimen estacionario. El shed no causa underruns (el bloque pasa en seco, no se pierde).

### 1.3 Causa raíz: hueco de priming en cada cambio de pista (verificado en código)
`IvannaBridgePlayer.runDecodeLoop` llama `codec.start()` y `track.play()` JUNTOS, ANTES del bucle de decodificación. El track arranca a sonar con el **buffer VACÍO** y el mezclador se queda sin audio hasta que llega el primer bloque decodificado + DSP (~10-30 ms, más con los picos de arranque en frío). Eso es un underrun en CADA cambio de pista. Coherente con lo que Esse reportó: "al brincar de canción regresan tronidos y distorsión" (2026-11-16 05:16 CST) — el disparador es el cambio de pista / re-priming, no el régimen estacionario.

**El fix (0025): play() diferido (priming).** `track.play()` se mueve al primer write aceptado (cuando ya hay audio en cola). El mezclador arranca con audio encolado y no hay underrun de arranque. Cambio en `IvannaBridgePlayer.kt`: `codec.start()` solo en setup; flag `trackStarted`; `track.play()` tras el primer `writtenSamples > 0`. Si el archivo no produce audio (EOS/error), el track se libera sin sonar nunca — correcto.

**Pendiente de verificación en vivo:** confirmar que el contador de underruns deja de crecer al cambiar de pista (protocolo en §4). Si los cortes persisten DURANTE una canción sin cambiar de pista, la causa es otra (no el priming) y hay que medir de nuevo.

## 2. CAPTURA: PLL saturado en el riel, buffer al 99%

### 2.1 Lo que dice la telemetría en vivo (11:18 CST)
- Ruta CAPTURA (cap 7696): `pll_occ=7619` (**99% LLENO**, objetivo 3848), `ppm=-500.0`, `pll_rail=1`, `pll_dem=-557.3`, `pll_ph=+0.49`.
- El PLL de captura está SATURADO EN EL RIEL (-500 ppm) Y NO PUEDE DRENAR EL BUFFER. Es el MISMO sobre-lleno que 0020 arregló en BridgePlayer, pero en la ruta de captura (0020 no la tocó).

### 2.2 Causa raíz: el ppm es un actuador de tasa, no de nivel (verificado en código)
- El ASRC aplica el ppm con el signo CORRECTO (`ratio = 1+ppm*1e-6`; ppm negativo = menos frames de salida; `ivanna_route_a_asrc.cpp:124-134` y `:466-468`). NO es un bug de signo.
- La contabilidad de `captureSubmittedFrames48k` es CORRECTA (suma `asrcFrames`, la salida real del ASRC; el priming y los reprimes también se cuentan). El sobre-lleno es real, no un artefacto de medida.
- El PLL real es PI (KP=800 ppm/buffer, KI=120 ppm/buffer·s, integrador cap 4 s, banda muerta 2%, anti-windup, riel ±500). Con el buffer al 99% (error de fase 0.49), la demanda llega a -557.3 (P ~384 + I ~173) y se clampa a -500.
- El drenado máximo por ppm es 500 ppm × 48 kHz = **24 frames/s**. El exceso en vivo (7619-3848 = 3771 frames) tardaría **3771/24 = 157 s** en drenar por ppm. El riel no basta. Es exactamente el diagnóstico de 0020: el ppm no puede con un error de NIVEL.
- El sobre-lleno se desarrolla por RÁFAGAS de catch-up: la captura es push (tiempo real, 320 frames/bloque). Tras cualquier stall (GC, planificador), el anillo del AudioRecord acumula audio y el bucle lo lee en ráfaga (varios bloques seguidos), escribiéndolos todos al track. El `writeAllToTrack` con WRITE_BLOCKING solo frena cuando el track está LLENO, no en el objetivo.

### 2.3 El fix (0025): pacing de nivel por descarte, espejo de 0020
En `PlaybackCaptureService.kt`, antes de escribir cada bloque: se estima la ocupación con `capturePll.estimateOccupancy(captureSubmittedFrames48k, nowNs)`; si supera el objetivo (`targetOccupancyFrames`, o capacidad/2), se **DESCARTA el bloque procesado** (no se escribe ni se cuenta como submitted) y la pista drena.

Por qué descarte y no pausa (la diferencia clave con BridgePlayer):
- BridgePlayer es pull (decoder): se puede PAUSAR el productor (el codec retiene sus buffers, sin perder audio). Por eso 0020 usa pausa.
- Captura es push (tiempo real): NO se puede pausar la fuente. Pero la fuente no puede rellenar el hueco más rápido que tiempo real, así que el descarte SÍ baja el nivel de forma duradera — a diferencia del snap en BridgePlayer, donde el decoder rellenaba el hueco al instante.
- El descarte es de 1 bloque (320 frames, 6.67 ms) y solo ocurre sobre el objetivo. Una vez en objetivo no hay más descartes y el ppm queda libre para la deriva real.

### 2.4 Medición en host (test nuevo)
`app/src/main/cpp/tests/test_capture_level_control.cpp` (4 tests, modelo de tasas con los números reales: cap 7696, objetivo 3848, bloque 320, KP/KI/integrador/banda muerta del PLL real, ráfagas de catch-up):
- **Sin pacing: nivel final 7696 (100% lleno), ppm -500.0, demanda -506.7, railed=1.** REPRODUCE el hallazgo en vivo (7619/7696, ppm -500, demanda -557.3).
- **Con pacing: nivel final 4160 (objetivo 3848, dentro de 1 bloque), ppm +110.6, demanda +110.6, descartados 11520 / escritos 483840 (2.3%).** El nivel converge y el servo sale del riel.
- **El ppm solo no drena:** 3771 frames a 24 frames/s = **157.1 s**. Por pacing: 11.8 bloques (**78.6 ms**).
- **Sin ráfagas no hay descartes:** el pacing no degrada el caso sano.

**Resultado: 4/4 PASS.**

## 3. Archivos del parche
- `app/src/main/java/com/ivanna/omega/audio/PlaybackCaptureService.kt` — pacing por descarte en captura (+56 líneas, con comentario 0025).
- `app/src/main/java/com/ivanna/omega/audio/IvannaBridgePlayer.kt` — play() diferido / priming (+30 líneas, con comentario 0025).
- `app/src/main/cpp/tests/test_capture_level_control.cpp` — nuevo, 4 tests.
- `app/src/main/cpp/tests/CMakeLists.txt` — registro del test.
- (0024, ya validado, va en el mismo parche: NPE `g_npeAOn{true}` + `test_route_a_npe_live_params.cpp` 7 tests + `tools/measure/npe_live_vs_native.cpp`.)

## 4. Validación y protocolo en vivo
- **CTest completo: 1104/1104 PASS, 0 fallos (55.07 s).** Total: 1093 base + 7 tests del NPE (0024) + 4 tests de captura (este parche). El test nuevo `test_capture_level_control` reproduce el fallo en vivo sin pacing (nivel 7696, ppm -500, demanda -506.7, railed) y valida el fix con pacing (nivel 4160, ppm +110.6).
- **Suite evolución: 168/168 PASS (OK, 177.2 s).** Incluye la actualización del pin de línea del test 0019 (1809 → 1825, por las 16 líneas del comentario 0024).
- **`check_docs_claims`:** PASS (0 hallazgos).
- **`telemetry/ctest_summary.json`:** actualizado a 1104.
- **Kotlin:** NO se puede compilar en host (sin Android SDK / kotlinc). Los dos cambios Kotlin (pacing en captura, priming en BridgePlayer) siguen el patrón probado de 0020 y el patrón estándar de priming de AudioTrack; **la CI debe compilarlos y correr los tests JVM**. Si la CI falla en Kotlin, el parche C++ (NPE + test de captura) sigue siendo válido por separado.
- **Protocolo en vivo (para Esse / agente con acceso al teléfono):**
  1. Instalar la build con 0025. Poner una canción en MÚSICA y anotar `track.underrunCount` al inicio.
  2. Dejar sonar 1 canción completa SIN cambiar de pista; anotar el contador al final. Si NO sube → los underruns eran del priming (fix confirmado). Si sube durante la canción → hay otra causa (medir de nuevo).
  3. Cambiar de pista 3 veces; anotar el contador tras cada cambio. Si no sube (o sube ≤1 por cambio) → el priming está cerrado.
  4. Prender la captura; verificar en telemetría que `pll_occ` de captura se queda cerca de 3848 (no 7619), `ppm` sale del riel (no -500) y `pll_rail=0`.

## 5. Lo que queda pendiente (no escondido)
1. **Verificación en vivo** de ambos fixes (protocolo §4). Sin ella, el fix es "validado en host", no "confirmado en el teléfono".
2. **El descarte en captura mete un hueco de 6.67 ms** cuando actúa. Si en vivo se oye un clic al descartar, el siguiente paso es un fundido cruzado en el borde del bloque descartado (o descartar solo una fracción del bloque). Solo actúa sobre el objetivo, así que debería ser raro.
3. **Los picos de DSP en frío** (SafetyLimiter / Unity / ASRC hasta ~47 ms en host) siguen existiendo; el colchón de 0.96 s del BridgePlayer los absorbe, pero valdría la pena perfilar por qué el primer bloque es tan caro (inicialización perezosa) y precargarlo.
4. **La telemetría de `dsp_us` no incluye el ASRC** (que corre después de `nativeProcess`). El coste total por chunk (DSP + ASRC + resampler) no tiene una sola métrica. Valdría una métrica unificada.
