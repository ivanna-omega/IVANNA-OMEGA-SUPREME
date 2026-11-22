# 0023 — Bofo del módulo Magisk / Ruta B

**Estado del consejo: RECHAZADO (fail-closed) — pero por fallo del implementer, no del enfoque.**
**Estado del parche: IMPLEMENTADO Y VALIDADO por el coordinador (patrón 0021/0022). Sin commitear.**
**Fecha:** 2026-11-16. **Rama:** `ivanna-real-completa` (no pushear a main).

## 1. Decisión del consejo

- **Veredicto formal (`council_verdict.json`): `approved: false`, parche `null`.**
- Ronda 1: el implementer (gpt-oss-120b interino) devolvió un **diff vacío** ("diff redactado").
  El reviewer (luna) **RECHAZÓ el enfoque de esa ronda** por no tocar el snapshot/bit HRTF/widener
  y por el diff vacío — **pero EXIGIÓ exactamente el fix propuesto**:
  > "Implementar `OmegaDspSnapshot::makeNeutral()` con los campos neutros que indicas.
  > Sustituir `makeDefault()`/`buildNeutralSnapshot` por `makeNeutral()` en daemon v2 y en el
  > arranque de app daemon. Añadir los tests host propuestos: snapshot neutro vs default,
  > ASRC a ratio ~1.0, y simulación del peine en captura. No introducir cambios parciales en
  > `Psychoacoustics.cpp` que no ataquen el snapshot ni el activado de HRTF."
- Ronda 2: el implementer **no devolvió un diff utilizable en 5 intentos**.
  Razón final: "ronda 2: el implementer no devolvió un diff utilizable en 5 intentos".
  El arbiter no llegó a deliberar.
- Detector (haiku interino): **"FALSO POSITIVO"** — pero su razón es un artefacto de contexto:
  el contexto le llegó recortado (`omega_control_bus.h` / `omega_effect.cpp` recortados por el
  tope de 10k) y pidió que le pegaran `buildNeutralSnapshot`, `makeDefault()` y el bloque de
  `IvannaFusionCore::process()` en texto. El coordinador verificó esas funciones línea por
  línea en el código (ver §2); el "falso positivo" es **no-verificación por contexto recortado,
  no una refutación**. No se re-discute: la evidencia está abajo con archivo y línea.
- **Lectura honesta:** el consejo NO aprobó (sin consenso en 3 rondas → RECHAZADO por regla).
  La sustancia, sin embargo, está de acuerdo: el reviewer prescribe exactamente este parche.
  El coordinador lo implementó y validó siguiendo el patrón 0021/0022 (el consejo propone,
  el coordinador implementa y valida). **El agente principal decide si lo commitea.**

## 2. Causa raíz (verificada en código, con archivo y línea)

### 2a. El bofo de Ruta B: el snapshot "neutro" del daemon NO es neutro

- `OmegaDspSnapshot::makeDefault()` (`app/src/main/cpp/include/omega_control_bus.h:645-800`)
  es el **default MAGISTRAL de la app (Ruta A)**, no un neutro. Pone:
  - `temporal_field |= omega_effect_features_encode(true, false)` — **bit HRTF ENCENDIDO**.
  - `intensity = 0.92f`, `spatial_width = 1.55f`, `widener_mult = 1.38f`,
    `anti_dolby = 0.85f`, `harmonic_gain = 0.78f`, `saf_gain = 0.78f`,
    `saf_q_valid = 1u`, `listen_phon = 62.f` / `ref_phon = 82.f` (phonDelta +1 dB),
    `cochlear_intensity = 0.35f`, `room_wet = 0.22f`, `room_rt60_s = 0.34f`,
    `room_idx = 51`, `compressor = -5.5f`, `loudness_target = -16.0f`,
    flags `PERCEPTUAL_ON | ANTI_DOLBY_ON | ATI_ON`.
- El daemon v2 (`magisk_module_fusion/src/ivanna_daemon_v2.cpp:225-244`,
  `buildNeutralSnapshot`) heredaba `makeDefault()` y solo ponía a cero
  EQ/bajos/sala/upmix/compresor/harmonic — **dejaba VIVOS**: bit HRTF,
  anti_dolby 0.85, spatial_width 1.55, widener_mult 1.38, intensity 0.92.
  Además ponía `target_gain = 0.0f` → en el efecto se hace
  `clamp(target_gain, 0.5, 1.0) = 0.5` → **-6 dB** (`omega_effect.cpp:1610`).
- `omega_apply_snapshot` (`app/src/main/cpp/omega_effect.cpp:424`) aplica el bit:
  `fc->setHrtfEnabled(true)`.
- `IvannaFusionCore` (`IvannaFusionCore.h:499`): `m_hrtfEnabled_{true}`,
  `g_hrtf_wet_dry{1.0f}` → `process()` (`IvannaFusionCore.cpp:394`) llama
  `m_hrtf->processBinauralScene(buffer)` con **wet 1.0: convolución HRTF COMPLETA
  sobre música estéreo**. La HRTF es para audífonos; el propio código dice que
  por altavoces "degrada la imagen y el timbre". **Ese es el bofo.**
- **Widener 2.14×:** `process()` calcula
  `rawTargetW = spatial_width × widener_mult = 1.55 × 1.38 = 2.14`
  → ensanchador M/S ACTIVO con el snapshot neutro (`IvannaFusionCore.cpp:667`,
  se aplica si |targetW−1| > 1e-4).
- **Intensity 0.92** → trim global 0.92 (−0.7 dB, `IvannaFusionCore.cpp:749`).
- **Fatiga LPF (contribuyente menor, NO el diferencial):**
  `predictAndMitigateFatigue` corre SIEMPRE (LPF 1er orden, alpha [0.85, 1.0],
  hasta −1.5 dB a 12 kHz). También está en Ruta A (que no tiene bofo).
  `applyMaskingCompensation` es expansor upward (+1 dB máx), no apagado.

### 2b. El peine en captura: EN EL MEZCLADOR, no acústico

- `PlaybackCaptureService.kt` documenta que la captura reproduce el audio procesado
  por su PROPIO AudioTrack mientras el original de la app sigue sonando a nivel pleno
  (no hay API pública para silenciarlo). Dos copias de la misma señal con ~130 ms de
  diferencia (original 100% + procesado 40% = −8 dB, `HAAS_SAFE_GAIN`) se SUMAN EN
  AudioFlinger = **comb filtering en el mezclador**, no acústico.
- Evidencia espectral medida en host (test `test_capture_comb_filter`, §4):
  muescas cada 1/0.13 ≈ **7.69 Hz**, picos 1.4 (+2.92 dB), muescas 0.6 (−4.44 dB),
  **rizo pico-a-muesca 7.36 dB**. La ganancia Haas 0.4 MITIGA pero NO elimina el peine:
  es inherente a la arquitectura de doble AudioTrack.
- **El fix real del peine es la Ruta B** (sin copia, sin delay, sin audio doble) —
  por eso hay que arreglar primero el bofo de Ruta B (este parche).

### 2c. EXONERADOS (con evidencia, no por descarte)

- **ASRC del módulo a ratio 1.0 es memcpy bit-exacto** (`Core::processS16`, test 1 del
  nuevo test; verificado 0 LSB de diferencia). Su FIR tiene corte 0.45 (21.6 kHz a
  48 kHz, plano hasta ~19 kHz) y solo actúa si ratio ≠ 1.0. A la deriva real (+8.5 ppm)
  es **plano ±0.00 dB hasta 20 kHz** (medido, test §4). **El ASRC NO es el bofo.**
- ATI_ON, PERCEPTUAL_ON y pf_params NO se consumen en el core (no-ops).
- `high_cut_hz` (19500) NO se consume en C++ (parámetro muerto).
- El campo Ψ del daemon v2 nunca lo lee el efecto (cero referencias a psi en
  `omega_effect.cpp`).
- El compresor con el snapshot del daemon v2 está OFF (compressor 0.0f → se salta
  `setCompressorParams`; el default del core es ratio 1.0 → `compActive` falso).
- El asrcd (`magisk_module/engine/asrcd/ivanna_asrcd.cpp`) publica el ratio a
  `asrc_ratio.shm` con servo Theil-Sen; el puente del efecto lo lee de ahí. El
  daemon v2 escribe su ratio al SHM de fusión, que el efecto NO lee (cero
  referencias a FusionState en `omega_effect.cpp`).

## 3. El parche (implementado por el coordinador, sin commitear)

### (A) `OmegaDspSnapshot::makeNeutral()` — `app/src/main/cpp/include/omega_control_bus.h`
Parte de `makeDefault()` y pone cada campo que colorea en su valor identidad:
intensity 1.0; listen = ref phon (82/82, phonDelta 0); spatial_width 1.0;
widener_mult 1.0; harmonic_gain 0; anti_dolby 0; compressor 0 (≥ 0: apply_snapshot
lo salta; ratio del core 1.0 = off); comp_amount 0; exciter 0; target_gain 1.0;
loudness_target 0; bass/dialog 0; room_wet 0 / rt60 0 / idx −1; upmixing 0;
WFS 0; ORBE/HOLOS 0; cochlear 0; high_cut 24000; saf_gain 1.0; saf_q_valid 0;
flags solo `OMEGA_FLAG_EQ_CALIBRATED` (sin PERCEPTUAL_ON/ANTI_DOLBY_ON/ATI_ON,
sin ningún flag SUPREME); `temporal_field = omega_effect_features_encode(false, false)`
(HRTF OFF). `makeDefault()` **no cambia**: sigue siendo el default de Ruta A.

### (B) Daemon v2 — `magisk_module_fusion/src/ivanna_daemon_v2.cpp`
`buildNeutralSnapshot()` ahora es `makeNeutral()` + sesión/generación/timestamp.
Se borran los ceros parciales (que dejaban HRTF/widener/intensity vivos y
target_gain 0.0 → −6 dB). Compilación verificada por sintaxis
(`g++-14 -fsyntax-only`, exit 0; no está en CTest host, se compila en CI para ARM64).

### (C) Daemon de la app — `app/src/main/cpp/daemon/ivanna_daemon.cpp:348`
El snapshot inicial pasa de `makeDefault()` a `makeNeutral()`. Es el primer publish
antes de que la app publique el suyo; con makeDefault() coloreaba el audio aunque
nadie pidiera nada. Cada vuelta se sigue adoptando el snapshot de la app, así que
los efectos del usuario llegan ENCIMA. Compilación verificada por sintaxis
(`g++-14 -fsyntax-only`, exit 0).

### (D) Tests host (3 nuevos, 64 checks)
- `tests/test_route_b_neutral_snapshot.cpp` (46 checks): afirma cada campo neutro de
  `makeNeutral()` y que `makeDefault()` NO es neutro (pincha el bug para que nadie
  vuelva a usarlo como base del daemon). Incluye los predicados de identidad del
  FusionCore con las mismas fórmulas del core (compresor inactivo, widener salta,
  GoldenEar inactivo, targetTrim 1.0, anti-dolby ×1, requestedGain 1.0).
- `tests/test_route_b_asrc_neutral_response.cpp` (10 checks): ratio 1.0 bit-exacto
  (memcpy, 0 LSB); respuesta en magnitud a ratio 1.0000085 (+8.5 ppm, deriva real)
  plana ±0.5 dB a 100/1000/6000/12000/16000/19000/20000 Hz (medido ±0.00 dB);
  sanidad: a 23 kHz atenúa −91.40 dB (el FIR sí es un paso-bajo; el test no es degenerado).
- `tests/test_capture_comb_filter.cpp` (8 checks): mezcla simulada (original + 0.4×
  retrasada 130 ms) coincide con la teoría |1+0.4·e^(−j2πf·0.13)| en picos y muescas
  (±3%); rizo 7.36 dB; muescas cada 7.69 Hz; con mix 0 el espectro es plano (1.0);
  con mix 1.0 la muesca cancela total (el viejo desface 100/100).
- Registrados en `tests/CMakeLists.txt` (mismo patrón que los tests de Ruta B).

## 4. Validación

- Tests nuevos (standalone, g++-14): **64/64 PASS**
  (neutral_snapshot 46/46, asrc_neutral_response 10/10, capture_comb_filter 8/8).
- Daemons: sintaxis OK (exit 0) para `ivanna_daemon_v2.cpp` e `ivanna_daemon.cpp`.
- **CTest completo: 1093/1093, 100%, 0 fallos (62.72 s)** — base 1090 + 3 nuevos.
  El flaky `OmegaDaemonBridgeTest.SecondCallReturnsCachedPointer` pasó en esta corrida.
- **Suite de evolución: 168/168 OK** (174.3 s).
- **check_docs_claims: [PASS], 0 hallazgos.**
- `telemetry/ctest_summary.json` actualizado a **1093/1093**.

**Incidentes del build:** el build completo se cortó por SIGTERM a los ~3.6 min
(mismo incidente que en 0020; no por código). Al reanudar, `/usr/bin/g++-14` ya no
existía (el paquete del sistema desapareció); se reconfiguró con el toolchain del
workspace `~/workspace/toolchain/gcc14/usr/bin/g++-14` (14.2.0, misma versión).

## 5. Qué prueba en vivo lo cerraría (exige luz verde EXPLÍCITA de Esse — NO la tiene)

1. **Instalar el módulo con este fix** (solo con luz verde de Esse; NO instalar, NO
   reiniciar, NO tocar audioserver sin su orden). Kill switch `pal_asr.off` listo.
2. **Escucha A/B en bocinas (la cadena real de Esse):** música por Ruta B con el
   snapshot neutro del daemon → debe sonar **idéntica a la APK sola / a Files**
   (sin bofo, sin apagado). Antes del fix sonaba bofa; después debe ser indistinguible.
3. **Telemetría del efecto:** confirmar que el snapshot que llega al efecto tiene
   HRTF off y widener 1.0 (beacon / dump del daemon).
4. **Medición 0022 (THD+N) con el tono 1 kHz** (`/sdcard/Download/tone_1k.wav`):
   tinycap mientras suena por Ruta B vs por Files; `thd_analyze` en host. La
   diferencia es lo que Ruta B aporta (debe ser ~0 dB con el neutro).
5. **Peine en captura:** con Ruta B activa, la captura ya no es la vía (sin doble
   AudioTrack) → el peine desaparece. Verificar que Tidal por captura ya no tiene
   el rizo de 7.36 dB (medición con el tono, o escucha).

## 6. Archivos tocados (sin commitear)

- `app/src/main/cpp/include/omega_control_bus.h` — `makeNeutral()` nuevo.
- `app/src/main/cpp/daemon/ivanna_daemon.cpp` — snapshot inicial `makeNeutral()`.
- `app/src/main/cpp/tests/test_route_b_neutral_snapshot.cpp` — nuevo (46 checks).
- `app/src/main/cpp/tests/test_route_b_asrc_neutral_response.cpp` — nuevo (10 checks).
- `app/src/main/cpp/tests/test_capture_comb_filter.cpp` — nuevo (8 checks).
- `app/src/main/cpp/tests/CMakeLists.txt` — registro de los 3 tests.
- `telemetry/ctest_summary.json` — 1090 → 1093.
- `tools/evolution/proposals/0023-bofo-modulo-magisk/finding.json` — hallazgo.
- `tools/evolution/proposals/0023-bofo-modulo-magisk/council_verdict.json` — veredicto (rechazado).
- `tools/evolution/proposals/0023-bofo-modulo-magisk/REPORT.md` — este informe.
- `magisk_module_fusion/src/ivanna_daemon_v2.cpp` — `buildNeutralSnapshot()` usa
  `makeNeutral()`.

⚠️ **CAVEAT — `magisk_module_fusion/` NO está en git (directorio sin trackear, 1.3 MB,
cero archivos trackeados) y la CI no lo referencia** (ningún workflow lo menciona).
El cambio del daemon v2 está ahí, verificado por sintaxis, pero NO es commiteable
con el resto sin un `git add` explícito de ese archivo (o del directorio). El
módulo trackeado que publica la CI es `magisk_module/` (distinto). El agente
principal decide si añade el archivo del daemon v2 al commit o lo deja como
cambio del módulo v3 en draft. El núcleo del parche (makeNeutral + daemon app +
tests) SÍ está en archivos trackeados y validado por CTest.

`tools/evolution/knowledge/knowledge.json` se modificó por el escaneo de la suite
y se REVERTIÓ (no es parte del parche; va excluido del commit, como en 0020).
