# P0 — Referencia congelada del motor (REFERENCE_LOCKED)

Fecha: 2026-11-22. Paso 3 de la orden de trabajo de Luna. Rama local
`p0-freeze`, SIN push. Test: `app/src/main/cpp/tests/test_p0_reference_lock.cpp`
(9 TESTs). Valores: `app/src/main/cpp/tests/p0_reference_values.hpp`
(generado por `scripts/p0_freeze.sh`, nunca a mano). Suite: 1619 → 1628
(Puerta + candado actualizados).

**ESTADO: P0 CONGELADO — 9/9 TESTs en verde, 0 skips.** Congelación final
2026-11-22 ~21:2x UTC con `scripts/p0_freeze.sh --final`, sobre el motor
CON el fix APEX+NEURO commiteado (`671ee5a` + `e2ecadb`). Los valores
congelados se reprodujeron BIT-IDÉNTICOS en dos congelaciones seguidas
(provisional y final) — la referencia es estable.

## Qué es P0

Una referencia CONGELADA: un WAV de referencia fijo + una configuración
fija de la cadena, con las métricas base medidas y bloqueadas en un test
que FALLA si la salida del motor cambia más allá de tolerancia. P1
detecta regresiones (guarda); P0 congela el carácter (línea base). Sin
P0 no hay "el motor suena igual que ayer" verificable.

## El WAV de referencia (p0_reference_v1)

48 kHz, estéreo, 10 s (480000 muestras/canal), generado en código con
fórmulas fijas y semilla fija — determinista y reproducible. Se escribe
a `p0_reference.wav` (entrada) y `p0_output.wav` (salida) en cada
corrida, en el directorio del build, para auditarlo.

| seg | nombre | rango | contenido |
|---|---|---|---|
| 0 | sine | 0.0–2.0 s | seno 1 kHz @ −12 dBFS pico — THD+N, niveles, latencia |
| 1 | imd | 2.0–4.0 s | dos tonos SMPTE 60 Hz + 7 kHz, 4:1, pico −12 dBFS — IMD |
| 2 | noise | 4.0–6.0 s | ruido blanco, RMS −12 dBFS (xorshift64*, semilla fija) — niveles, latencia xcorr |
| 3 | clicks | 6.0–7.0 s | impulsos 0.5 cada 0.25 s — latencia por impulso |
| 4 | hot | 7.0–8.0 s | seno 1 kHz @ −0.5 dBFS pico — true peak, techo final |
| 5 | silence | 8.0–10.0 s | silencio digital — cola residual (fix NEURO) |

Fronteras de segmento múltiplos del hop (128). Ventanas de medida saltan
los primeros 0.25 s de cada segmento (transitorio STFT del cambio de
segmento), salvo true peak (máximo, sin ventana) y clicks.

## La configuración fija

`engagedControls()`, IDÉNTICA a la "IVANNA completa" de la matriz P1
(mismos valores que `test_fused_chain::engagedControls()`): ORBE 1.0 /
azimut 30° / 1.0 m / HRTF 0.5, HOLOS 0.5 / Φ 0.5, sala 0.22, Volterra
1.0, tonal curva 1 / low +2 dB, compresor −12 dB ratio 2.0, safety 1.0,
cerebro AUTO adaptación 1.0. Hops de 128, como en el producto.

## Métricas congeladas (tolerancias en el test)

- **Niveles**: RMS y pico de salida por segmento (L/R), ganancia neta
  total. Tolerancia ±0.05 dB.
- **Latencia**: xcorr sobre ruido (L/R) + impulso por pico de la
  respuesta ensanchada (L/R). Tolerancia ±1 muestra.
- **True peak**: total y segmento HOT (dBTP, sinc 4x, mismo método que
  `ivanna_measure::truePeakLin` pero en double). Tolerancia ±0.05 dB.
  Guarda absoluta: ≤ 0 dBTP (techo final).
- **THD+N**: segmento seno, double (`fused_measure::measureThdDouble`,
  piso ~−310 dB). Tolerancia ±0.5 dB.
- **IMD**: segmento dos tonos, SMPTE, double. Tolerancia ±0.5 dB.
- **CPU**: µs/hop de CPU del HILO (p50/p99/máx) — congelados como
  LÍMITES (p50: 4× base o 400 µs; p99: 3× base o 1000 µs; máx: 2× el
  presupuesto = 5333 µs). Los xruns de pared solo se reportan.
- **Silencio**: RMS y pico del último segundo, guarda ≤ −90 dBFS
  (umbral P1). El valor exacto del residuo (~−199 dBFS, denormales)
  NO se congela con tolerancia: es ruido numérico.
- **Determinismo**: maxDiff entre dos corridas ≤ 1e-5 (el motor NO es
  bit-determinista — ver hallazgo 6).

## Valores congelados (medidos, congelación final)

| métrica | L | R |
|---|---|---|
| RMS salida sine / imd / noise / clicks / hot (dBFS) | −24.88 / −17.11 / −23.66 / −39.12 / −19.95 | −21.10 / −17.55 / −20.53 / −36.09 / −16.35 |
| Pico salida sine / imd / noise / clicks / hot (dBFS) | −20.65 / −13.41 / −9.68 / −9.29 / −10.32 | −17.01 / −12.22 / −7.97 / −9.27 / −6.52 |
| Ganancia neta total (dB) | −8.54 | −6.82 |
| Latencia xcorr (muestras) | 1277.78 | 1283.67 |
| Latencia impulso — pico de la respuesta (muestras) | 1279 | 1279 |
| True peak total / HOT (dBTP) | −0.199 / −0.199 | |
| THD+N seno 1 kHz (dB) | −29.23 | −30.63 |
| IMD SMPTE (dB) | −17.09 | −13.53 |
| Silencio RMS / pico último s (dBFS) | −199.71 / −196.60 | −199.44 / −196.33 |
| CPU hilo p50 / p99 / máx (µs) | 707.4 / 1286.4 / 1674.2 (límites 2829.5 / 3859.3 / 5333.3) | |
| CPU pared p50 / p99 / máx (µs), xruns | 717.0 / 53352.1 / 62158.1, xruns 34/2000 (reportado) | |
| maxDiff entre 2 corridas | 1.40e-06 (límite 1e-5) | |

JSON completo: `build/tests-p0/p0_reference.json`. WAVs:
`build/tests-p0/p0_reference.wav`, `build/tests-p0/p0_output.wav`.

## Hallazgos del diagnóstico (medidos, no supuestos)

1. **La latencia real del camino sostenido es ~1280 muestras (~26.7 ms).**
   La xcorr de la FORMA DE ONDA sobre ruido blanco (pico normalizado
   0.32 en lag 1278, muy por encima de los lóbulos) y el pico de la
   respuesta al impulso (+1279 muestras en los clicks 2–4) coinciden:
   el retardo es ~10 hops. Candidato: lookahead de APEX + STFT.
2. **La cadena tiene DOS latencias por diseño (APEX es paralelo).** El
   transitorio NO pasa por los operadores: se retrasa solo por la línea
   de Lagrange con el ITD de ORBE y se suma a la salida
   (`fused_chain.hpp`, comentario APEX). Por eso la respuesta al
   impulso es bimodal: un leak temprano (el click ya marca −26 dBFS a
   +2 muestras) + el grueso de la energía a +1279. La latencia de P1
   (`adv::measure`, onset + correlación de |x|) mide ~8 muestras: es
   la del camino TRANSITORIO / onset, no la del sostenido. Ambas son
   reales y miden caminos distintos; P0 congela la del sostenido
   (xcorr de forma de onda + pico del impulso). Los números de latencia
   de P1 NO son el retardo del motor — no citarlos como tal. La
   correlación de |x| sobre ruido además es engañosa (las envolventes
   tienen media positiva y correlacionan ~0.70 a cualquier lag).
3. **La latencia por primer cruce de umbral es un método roto** (leak
   temprano + click 1 sobre la cola del ruido). Se cambió a **pico de
   la respuesta ensanchada** (argmax en [click, click+4096), mediana
   de los 4 clicks), que coincide con la xcorr (1279).
4. **El true peak −0.199 dBTP es un transitorio real, no un bug del
   medidor.** El pico muestral −0.209 dBFS está en la muestra 337394
   (7.029 s, onset del segmento HOT, +1394): la cadena reacciona al
   cambio brusco de contenido con un sobreimpulso. Por debajo de
   0 dBFS: no hay clipping. Los picos por segmento (ventanas
   asentadas) no lo ven porque excluyen los primeros 0.25 s.
5. **El fix NEURO funciona: el silencio es silencio.** Último segundo
   −199.7 dBFS RMS (antes del fix: −77 dBFS). La cola era el DC
   blocker del StereoWidener, no NEURO.
6. **NEURO·Ω NO es determinista por corrida (hallazgo del motor).**
   Bisección por operador (2 corridas por config, referencia P0):
   bypass 0, IVANNA 1.4e-06, **op_neuro 3.36e-01** (−9.5 dBFS, muestra
   3496), resto de operadores 0 (bit-idénticos). Tres corridas
   op_neuro seguidas: run1–run2 0.329, run2–run3 0.336, run1–run3
   0.336 — todos los pares difieren igual: es **aleatoriedad por
   construcción de cadena, no adaptación** (si fuera calentamiento,
   run2–run3 sería mucho menor). No hay PRNG visible en
   `NpeStereoCore.hpp` ni `fused_brain.*` (sin random_device/rand/
   ruido); el `CLOCK_MONOTONIC` de `neuro_acoustic_closed_loop.hpp`
   es solo para timestamps de frames de sensor. En la cadena COMPLETA
   la aleatoriedad se amortigua ~240000× (1.4e-06, −117 dBFS,
   inaudible) y es INTERMITENTE (en algunas corridas maxDiff = 0.0):
   todos los valores congelados son estables frente a ella (efecto
   ≤0.0002 dB en niveles). **Consecuencia: la columna op_neuro de P1
   NO es reproducible corrida a corrida** (sus números son una muestra
   de una distribución); la columna IVANNA completa sí lo es. La
   corrección (sembrar/inicializar el estado de NEURO de forma
   determinista) es trabajo del motor, no de P0; P0 la detecta con el
   test de determinismo (límite 1e-5).
7. **CPU de pared en host compartido NO mide el motor.** Pared p50
   ~717–750 µs (plausible: cadena completa vs ~200 µs de la cadena
   lineal de DoubleFloor), pero p99 de pared 53–64 ms y 34–47 xruns /
   2000 son stalls de planificación (bucle P1 + coordinadores
   compilando en paralelo). Por eso P0 congela el **tiempo de CPU del
   hilo** (`CLOCK_THREAD_CPUTIME_ID`): hilo p50 ~707–776 µs estable,
   p99 ~1286–1623 µs, máx 1674–2588 µs en la mayoría de repeticiones,
   **xruns de hilo 0 en todas las repeticiones** — el motor nunca
   consume más CPU que su presupuesto en el caso típico. **Pregunta
   abierta: un pico de hilo de 4393 µs en la primera congelación
   final** (por encima del presupuesto 2666.67 µs). El máx es un
   estadístico extremo y por eso su límite es 2× el presupuesto
   (5333 µs); p50/p99 son los límites que vigilan el coste. Qué camino
   lento produce el pico de 4.4 ms (¿refresco del gemelo? ¿MPC sombra?
   ¿alocación?) queda por investigar — no bloquea P0.

## Cómo verificar que P0 está congelado

```
./scripts/p0_freeze.sh --final   # congela (exige el fix commiteado)
cd build/tests-p0
./test_p0_reference_lock --gtest_filter='P0ReferenceLock.*'  # 9/9 en verde, 0 skips
./test_p0_reference_lock --gtest_list_tests  # 9 TESTs P0ReferenceLock
```

Conteo total de la suite: 1619 (P1, medido por el bucle) + 9 P0 = **1628**
(Puerta + candado actualizados; la verificación completa de CTest corre
en la CI del próximo push).

Si un TEST P0 falla después, el motor cambió: revisar el diff de
valores en `p0_reference_values.hpp` como cambio de producto (o
re-congelar con `--final` si el cambio fue intencional). Re-congelar
sin el fix commiteado está bloqueado por el propio script.
