# P1+P4 — Regla de Oro, ablación, Genezi, NEURO y HRTF

**Fecha:** 2026-11-26 · **Rama de trabajo:** `~/workspace/refine-p1p4/` (copia de
`ivanna-transfer` HEAD `998bcc3`, v2.5.504) · **Toolchain:** g++-14 14.2.0,
cmake 3.30.3, Release · **Tests:** `test_p1p4_ablation.cpp`,
`test_p1p4_neuro.cpp`, `test_p1p4_hrtf.cpp` (14 tests, 14/14 en verde).

## Método (lo que se midió, exactamente)

- **Señal:** tono dual-mono, −3 dBFS pico (amp 0.7071, RMS −6.020683 dBFS),
  2 s @ 48 kHz, salvo indicación. **Ventana de medida:** 2º segundo
  (1.0–2.0 s), RMS por canal; la ganancia neta es RMS-salida − RMS-entrada.
  El 1er segundo se descarta (asentamiento del AGC de NEURO, ~1 s).
- **Configs:** `ALL7` = defaults de `FusedControls` + 7 operadores a 1.0 +
  `trimDb = 0` (idéntica a la sonda #1432); `P0-engaged` = controles exactos
  de `test_p0_reference_lock.cpp::engagedControls()`; `zero7` = los 7
  operadores a 0 sobre defaults. Volumétrico ON en todas (default).
  **El árbitro D10 está activo por defecto** (`tenth_dimension_action.hpp:369`,
  `enabled = true`, orden «todo prendido»): todas las medidas de cadena lo
  incluyen; la interacción que se reporta es con árbitro ON.
- **ITD/ILD:** tono L = sen(1 kHz), R = mismo tono 10 muestras tarde y −6 dB
  (ITD +10.00 muestras ≈ +208 µs, ILD +6.02 dB); fase/amplitud del fundamental
  por proyección en el 2º segundo.
- **HRTF:** parseo real de los 239 `.sofa` del módulo con `SofaHRTFLoader`
  + contadores nuevos `loadCounters()` (medidos/reconstruidos/fallidos).

---

## 1. Regla de Oro por banda ERB — VEREDICTO: LA AFIRMACIÓN «0.0 dB» QUEDA RETIRADA

### 1.1 Tabla por banda ERB (ganancia neta, dB; media L/R salvo P0, que da L/R)

| f (Hz) | P0-engaged L | P0-engaged R | ALL7-trim0 L | ALL7-trim0 R |
|---:|---:|---:|---:|---:|
| 102 | −9.555 | −9.544 | −12.993 | −12.890 |
| 217 | −12.990 | −12.908 | −14.608 | −15.004 |
| 384 | −15.619 | −14.310 | −16.082 | −15.974 |
| 626 | −14.387 | −12.925 | −14.908 | −13.944 |
| 977 | −15.136 | −14.741 | −14.304 | −13.995 |
| 1486 | −18.754 | −18.193 | −13.052 | −12.947 |
| 2226 | −16.148 | −14.734 | −11.924 | −11.926 |
| 3300 | −8.576 | −7.118 | −9.336 | −5.831 |
| 11500 | −17.560 | −24.296 | −13.764 | −13.764 |

- **Neutro: 0.000 dB exacto** (bit-exacto, ±1e-9 dB). La Regla de Oro SÍ se
  cumple en neutro y SOLO en neutro.
- **P0-engaged a 977 Hz (banda ERB de 1 kHz): −14.938 dB** (media L/R).
  El P0 congelado (−6.065/−6.274 dB) es la referencia de `p0_reference_values.hpp`,
  no la ganancia neta de la cadena enganchada: son dos cosas distintas y el
  código las confundía.
- **Ninguna banda, en ninguna de las dos configs enganchadas, está dentro de
  ±0.5 dB.** La banda más cercana es 3300 Hz en ALL7 (R −5.831 dB). La regla
  «±0.5 dB por banda» como propiedad de la cadena enganchada es **falsa hoy**.
- **Con el trim P8 compilado (+19.482 dB) la cosa mejora pero no cierra:**
  1000 Hz −0.077/+0.076 dB (dentro de ±0.10 dB — el test D10 marcador
  PASA hoy, ver §1.2), pero el residuo por banda es −0.86…+2.65 dB:

| f (Hz) | net con trim compilado L | net con trim compilado R |
|---:|---:|---:|
| 102 | +2.554 | +2.654 |
| 217 | +2.505 | +2.108 |
| 384 | −0.502 | −0.395 |
| 626 | −0.864 | +0.080 |
| 977 | −0.227 | +0.035 |
| 1486 | +0.711 | +0.887 |
| 2226 | +1.931 | +1.908 |
| 3300 | +0.131 | +0.763 |
| 11500 | +0.076 | +0.076 |

  Peor banda: 102 Hz, +2.604 dB (media). **4 de 9 bandas quedan fuera de
  ±0.5 dB** (102, 217, 1486, 2226 Hz). El trim es escalar y su ganancia
  efectiva es dependiente de frecuencia (a 1 kHz compensa +14.18 dB, a
  3300 Hz solo +8.0 dB): un solo número no puede cerrar el espectro.
  Test: `P1P4Ablation.CompiledTrimClosesOnly1kHz`.

### 1.2 Qué se retira, por qué, y qué lo reemplaza

**Se retira** la afirmación «ganancia neta 0.0 dB» como propiedad de la cadena
enganchada, allí donde aparecía sin calificar:

- `fused/fused_controls.hpp:13` (ahora: «0.0 dB SOLO EN NEUTRO y como
  objetivo NOMINAL de P8»).
- `fused/fused_chain.hpp:21` (ahora acotado a neutro + nominal P8, con cita
  del P0 −6.065/−6.274 dB y del ALL7 −14.18 dB).
- `tests/test_p0_reference_lock.cpp:447` (el comentario «debe ser ~0 dB»
  era falso frente al propio valor congelado).
- El comentario de `TonalControls::trimDb` (ahora: compensador, nominal
  0.0 dB; la cadena sin trim NO es 0.0 dB).

**Por qué:** medido, no opinado — la tabla 1.1. El test
`D10Corona.MatchedLufsInvariantAcross7Operators` **PASA hoy a 1 kHz**
(net −0.077/+0.076 dB con el trim compilado +19.482 dB; el comentario de
ese test que decía «sigue FALLANDO» era de 2026-11-25 y se corrigió en
este trabajo). Lo que sigue refutado es el **modelo nominal** (afirma
`netGainNominal = +0.000 dB` mientras la cadena sin trim pierde −14.18 dB
a 1 kHz) y la regla **por banda**: con el trim que cierra 1 kHz, 4 de 9
bandas quedan fuera de ±0.5 dB (peor +2.604 dB a 102 Hz). No se tocó el
umbral del test D10 (±0.10 dB a 1 kHz): sigue siendo el testigo a 1 kHz,
y el testigo por banda es `CompiledTrimClosesOnly1kHz`.

**Qué lo reemplaza** (definición nueva, medible):

1. **Neutro-exacto:** en neutro la cadena es bit-exacta, ganancia neta
   0.000 dB por banda, por nivel y por tiempo. Esto SÍ es la Regla de Oro
   y SÍ se cumple (test `GoldenRulePerErbBand`).
2. **Nominal-vía-trim-medido:** con operadores enganchados, el trim de P8
   es un compensador cuyo valor se fija POR MEDICIÓN (los coeficientes
   nominales de `experience_compiler.cpp:29-69` son el punto de partida,
   no la verdad). El trim compilado (+19.482 dB) **sí cierra 1 kHz**
   (−0.077/+0.076 dB), pero deja un residuo espectral de −0.86…+2.65 dB:
   cerrar POR BANDA exige un trim por banda, no un escalar. El defecto
   #1432 (modelo nominal ≠ ganancia real) sigue vivo como defecto del
   modelo, no del trim compilado.
3. **Por banda, por nivel y por tiempo:** la tabla 1.1 ES la Regla de Oro
   por banda para tono −3 dBFS en régimen (2º s). Por nivel y por tiempo
   quedan pendientes: el AGC de NEURO es dependiente de nivel y de tiempo
   (asienta en ~1 s, §4.1), así que una sola cifra por banda no basta —
   el trim correcto depende del nivel de entrada y del instante.

---

## 2. Ablación por operador (leave-one-out, medición digital)

### 2.1 LOO a 1 kHz (ALL7-trim0; ref sonda #1432, reproducida exacta)

| Config | netL | netR | marginal L | marginal R | marginal media |
|---|---:|---:|---:|---:|---:|
| ALL7 | −14.258 | −14.105 | — | — | — |
| sin ORBE | −13.285 | −16.128 | −0.973 | +2.023 | **+0.525** |
| sin HOLOS | −14.311 | −14.165 | +0.052 | +0.060 | **+0.056** |
| sin SALA | −14.138 | −14.138 | −0.120 | +0.033 | **−0.044** |
| sin VOLTERRA | −14.258 | −14.105 | −0.000 | +0.000 | **+0.000** |
| sin ARMONÍA | −15.128 | −14.951 | +0.870 | +0.847 | **+0.858** |
| sin PULSO | −14.250 | −14.097 | −0.008 | −0.008 | **−0.008** |
| sin NEURO | −3.117 | −2.939 | −11.142 | −11.165 | **−11.153** |

Suma de marginales **−9.766 dB** | total **−14.182 dB** | **interacción −4.416 dB**.
La sonda #1432 da exactamente los mismos números (suma −9.766, interacción
−4.416): la ablación reproduce la medición externa al dB.

Lectura: NEURO es el 78.6% del coste total (−11.153/−14.182). ORBE tiene
marginal asimétrico L/R (−0.973/+2.023): a 1 kHz ORBE redistribuye energía
entre canales (HRTF a 30° en P0; aquí azimut 0° por default — ver §2.3).
VOLTERRA es exactamente 0.000 dB en tono estacionario (el kernel aproximado
no mueve la ganancia RMS a 1 kHz). PULSO −0.008 dB (el widener M/S conserva
la energía a primer orden).

### 2.2 LOO por banda ERB (ALL7 + delta por operador, media L/R, dB)

| f (Hz) | ALL7 | dORBE | dHOLOS | dSALA | dVOLT | dARMON | dPULSO | dNEURO |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 102 | −12.941 | −8.079 | +0.192 | −0.031 | −0.000 | +1.640 | +0.002 | −13.061 |
| 217 | −14.806 | −10.031 | −0.046 | −0.260 | +0.000 | +0.470 | −0.023 | −13.131 |
| 384 | −16.028 | −7.399 | −0.123 | −0.412 | −0.000 | −0.554 | −0.020 | −12.148 |
| 626 | −14.426 | −5.457 | −0.403 | −0.058 | −0.000 | +1.288 | −0.010 | −10.967 |
| 977 | −14.150 | −0.409 | +0.118 | −0.130 | −0.000 | +1.005 | −0.008 | −11.160 |
| 1486 | −12.999 | +1.557 | +0.191 | −0.098 | +0.000 | +1.845 | −0.012 | −11.261 |
| 2226 | −11.925 | −0.818 | +0.053 | −0.044 | +0.000 | +2.866 | −0.022 | −10.605 |
| 3300 | −7.583 | +3.465 | +1.803 | −0.064 | −0.000 | +4.880 | −0.027 | −7.638 |
| 11500 | −13.764 | **+21.718** | +0.089 | −0.140 | −0.000 | +0.738 | −0.000 | −10.724 |

Lectura:

- **NEURO domina en todas las bandas** (−7.6…−13.1 dB). Es el único operador
  con marginal grande en las 9 bandas.
- **ORBE es el segundo actor y es fuertemente dependiente de frecuencia:**
  atenúa graves (−8.1 dB a 102 Hz, −10.0 dB a 217 Hz) y realza agudos
  (+21.7 dB a 11500 Hz — el marginal mayor de toda la tabla). El +21.7 dB
  significa: sin ORBE, el tono de 11.5 kHz cae a −35.5 dB; con ORBE vuelve
  a −13.8 dB. La interacción a 11.5 kHz es de **−25 dB** (suma de marginales
  +11.68 dB vs total −13.76 dB): **no atribuida** — candidatos: árbitro D10
  (activo por defecto), AGC de NEURO en alta frecuencia, limitador de
  seguridad. Aislarla exige re-medir con el árbitro apagado (pendiente, §7).
- **HOLOS** es casi neutro en ganancia (−0.4…+0.2 dB) salvo a 3300 Hz
  (+1.803 dB). **SALA VIVA** mueve −0.4…0 dB en RMS estacionario (su efecto
  es temporal, no de ganancia — ver §2.3). **ARMONÍA** es un realce de
  +0.5…+4.9 dB, mayor en 2226–3300 Hz. **VOLTERRA** 0.000 dB en las 9 bandas
  (tono estacionario). **PULSO** ≤0.027 dB en todas (conserva energía).

### 2.3 ARMONÍA plana vs activa, SALA VIVA, roomSizeM

- **ARMONÍA (base P0-engaged; plana = curva 0 + low/mid/high 0 dB):**
  102 Hz activa −9.550 / plana −13.561 → **delta +4.012 dB**;
  977 Hz −14.938 / −15.846 → **+0.908 dB**;
  3300 Hz −7.847 / −11.584 → **+3.737 dB**.
  La etapa tonal P0 (curva 1, low +2 dB) es un realce de +0.9…+4.0 dB,
  mayor en graves y agudos. Es la coloración menos atribuida y es real.
- **SALA VIVA off vs 0.22 (base P0, roomSizeM 5.0):**
  977 Hz wet −14.938 / off −16.371 → **delta +1.433 dB**;
  217 Hz wet −12.949 / off −14.216 → **+1.267 dB**.
  Reproduce el +1.40…+1.54 dB tonal del consejo (977 Hz: +1.433 dB).
  La cola SDRF añade energía en régimen estacionario.
- **roomSizeM 5.0 vs 7.0 (977 Hz, wet 0.22): −14.938 vs −14.938 → delta
  +0.000 dB.** El perfil vivo corre a **5.0 m** (default en
  `fused_controls.hpp:147`, `FusedControls.kt:188`, `ProfileManager.kt:103`
  y en los dos perfiles de `audio_profiles.json`; ningún perfil pone 7.0).
  `roomSizeM` escala la **geometría** de la sala (ancho/profundidad/altura
  y posiciones de oyente y altavoces, `fused_listener_room.cpp:139`), no
  una ganancia: por eso no mueve el RMS estacionario a 977 Hz. Su efecto
  (tiempos de reflexión) no se midió aquí.

### 2.4 PULSO widener — ITD/ILD (entrada ITD +10.00 muestras, ILD +6.02 dB)

| Config | ITD (muestras) | ILD (dB) |
|---|---:|---:|
| Bypass (neutro) | +10.00 | +6.02 |
| Width 1.35 HEADPHONES | **+12.96** | **+6.14** |
| Width 1.35 SPEAKERS | **+5.53** | **+2.02** |
| Control TΩ SPEAKERS width 1.0 | +4.11 | +1.58 |

- **En HEADPHONES el widener M/S destruye el ITD, no la ILD:** ITD +2.96
  muestras, ILD +0.12 dB. El álgebra M/S lo predice (con ITD 10 la
  componente side está en cuadratura parcial: ITD teórico 12.88, ILD 6.17 dB).
  La intuición «el widener destruye la ILD» es falsa para esta señal; lo
  que destruye es la fase (ITD).
- **En SPEAKERS el cambio grande es la matriz TΩ (XTC Kirkeby Genezi,
  `fused_chain.cpp:994-1006`), no el widener:** el control sin widener
  (width 1.0) da casi el mismo cambio (ITD 4.11 vs 5.53, ILD 1.58 vs 2.02).
  La TΩ engancha siempre que la cadena no es neutra en modo SPEAKERS
  (por diseño: cancelación de diafonía para altavoces). El widener añade
  +1.42 muestras de rotación M/S sobre la TΩ.
- **Contención del guardián (Fallo 3b) VERIFICADA en señal** (telemetría
  `guardianWidener` = 1.350 en las 3 configs cuando dispara — es el valor
  PRE-arbitraje, no refleja la supresión; la prueba es la señal):

| Config | ITD | ILD | Control sin NEURO | Δ vs control |
|---|---:|---:|---|---:|
| NEURO solo HP | +13.00 | +6.13 | — | — |
| NEURO solo SP | +4.13 | +1.59 | TΩ SP +4.09 / +1.58 | 0.04 / 0.01 |
| NEURO+ORBE (binaural) | −18.92 | +1.57 | ORBE solo −19.06 / +1.61 | 0.14 / 0.04 |

  Con y sin NEURO la ILD/ITD son iguales en SPEAKERS y en binaural: el
  guardián NO añade nada ahí (contención real). El cambio de ILD en SP es
  de la TΩ y en binaural del render ORBE.

---

## 3. Genezi (4.1) — el kernel medido NUNCA se carga en vivo

- **Call-site SÍ existe:** `app/src/main/cpp/jni/ivanna_omega_jni.cpp:1493`
  (`nativeLoadNewPhysicsCalibration` → `g_newPhysicsRouteA.loadGeneziVolterra(pL, pR)`),
  y `new_physics_engine.hpp:167` (`loadGeneziVolterra` → `loadKlippelH2H3`,
  polo exp(−1/(0.0125·fs)) ≈ 0.9983 @48 kHz).
- **Pero no hay llamador Kotlin:** `DSPBridge.loadNewPhysicsCalibration`
  (`DSPBridge.kt:422`) y `NewPhysicsBridge.applyCalibration`
  (`NewPhysicsBridge.kt:491`) tienen **cero llamadores** (grep sobre
  todos los .kt/.cpp/.hpp). No existe `genezi_np_cal.json` ni arrays tri21
  en ningún lugar del repo ni de los assets.
- **Lo que corre Volterra en vivo es el kernel APROXIMADO de Klippel** de
  `wakeV3Organs()` (`fused/np483_defaults.hpp`: H2 0.50% @85 dB SPL,
  Σ|k| = 0.0031, polo 0.9983 — el mismo valor calibrado del Genezi,
  reutilizado para el kernel aproximado; H3 0.20%), cargado vía
  `DSPBridge.wakeNewPhysicsV3Organs()` desde `NewPhysicsBridge.kt:352`.
- **Admisión escrita en el código** (comentario sobre
  `nativeLoadNewPhysicsCalibration` en el JNI): el kernel medido del
  MHC-PZ1D real nunca se carga; Volterra es un **ajuste de curva nominal,
  no una identificación del Genezi real**. El CHANGELOG ya lo admitía
  («el teléfono no tiene kernel Genezi»). Producir el kernel exige correr
  `tools/genezi_calibrate.py` sobre barridos Farina reales (ESS 10 s,
  20 Hz–20 kHz) — nunca se ha hecho.
- Consecuencia medible: VOLTERRA marginal **0.000 dB en las 9 bandas ERB**
  (§2.2) en tono estacionario — el operador, con el kernel aproximado, no
  mueve la ganancia RMS. Su efecto (si lo hay) es armónico/transitorio,
  no de ganancia.

---

## 4. NEURO (4.2) — origen del −15.570 dB

### 4.1 Descomposición del NPE standalone (tono 1 kHz, −3 dBFS, 2 s)

| Config NPE | Ganancia neta | AGC final | AGC por ventana de 0.25 s |
|---|---:|---:|---|
| Completo (harm 0.2 / lat 0.2 / ohc 0.3) | **−14.382 dB** | 0.3017 | 0.495 0.347 0.312 0.304 0.302 0.302 0.302 0.302 |
| Mínimo (0 / 0 / 0, solo AGC+NHO) | **−14.378 dB** | 0.2724 | 0.472 0.320 0.284 0.275 0.273 0.273 0.272 0.272 |
| Apagado | +0.000 dB | 1.0000 | 1.000 ×8 |

- **El −15.57 dB es ~100% el AGC del NPE** (`neuromorphic/NpeStereoCore.hpp`,
  dentro de `FusedBrain::runActuationChunk`): objetivo −18 dBFS
  (`kAgcTargetDb = −18.0`); un tono de RMS −6.02 dBFS pide ganancia ≈ 0.30
  (−10.5 dB) y el resto es compresión OHC/inhibición en el camino.
  **OHC + inhibición lateral + armónicos aportan −0.004 dB** (completo vs
  mínimo). El AGC asienta en ~1 s (0.495 → 0.302).
- En cadena el marginal LOO de NEURO es −11.153 dB (no −14.382 dB): la
  diferencia es la interacción −4.416 dB con el resto de la cadena
  (el NPE es el primer operador; aguas abajo el limitador de seguridad y
  ARMONÍA/ORBE actúan sobre la señal ya atenuada).
- El `ClosedLoopController.compute(0)` devuelve identidad sin frames
  frescos del sensor (fail-closed, `fused_brain.hpp`): **no aporta nada**
  al −15.57 dB.

### 4.2 FreeEnergyLoop — NO es un bucle cerrado variacional

- **`bandEnv24`/`bandTransient24` vienen de la FFT de la propia señal de
  entrada del motor** (`new_physics_engine.hpp:615-655`, `instEnergy` por
  banda ERB en `runStftHop`). **No hay micrófono conectado a este motor.**
  Prueba: silencio 2 s → máx |γ| = **0.000 dB** (sin señal no hay lazo);
  tono 1 kHz 8 s → γ(977 Hz) = **−3.000 dB** (tope) en ~6 s.
- **No hay funcional F ni ∂F/∂Γ.** La línea «Gradiente variacional:
  −∂F/∂Γ_b = …» era una heurística escrita a mano (`onsetNeed`,
  `fatiguePen`, `targetDb = clamp((1.8·onsetNeed − 2.5·fatiguePen)·mix,
  −3, +2)`, slew 0.5 dB/s). **Renombrado** en código y comentarios a
  «lazo F (heurística MOC + onsets)» (`free_energy_loop.hpp`,
  `new_physics_engine.hpp`, `new_physics_controls.hpp`,
  `NewPhysicsControlCard.kt:219`, `ControlHelpCatalog.kt:56,205`) — sin
  borrar nada: lo viejo queda marcado legacy en el comentario.
- **Respuesta al escalón medida (lazo directo, banda 13, 977 Hz, env
  sostenido 1.0):** γ = −0.495 (1 s) → −0.995 (2 s) → −1.495 (3 s) →
  **−1.625 dB (4 s, tope)** = −2.5·(1.0−0.35), a slew 0.5 dB/s, y se queda.
  Onset transitorio → **+0.500 dB** tras 1 s (acotado por el slew).
  En motor completo el γ llega al tope de −3 dB (el `mocState` del motor
  supera 1.0 con tono sostenido).
- **El FreeEnergyLoop NO explica el −15.57 dB:** vive en `NewPhysicsEngine`
  (motor separado, NO en `FusedChain`), su `freeEnergyMix` por defecto es
  **0.0 (apagado)** y aun a tope γ está acotado a [−3, +2] dB.
  Añadido accesor `gammaModDb(banda)` / `freeEnergyGammaModDb(banda)`
  para telemetría.

---

## 5. HRTF (4.3) — 0/239 medidos en el módulo

### 5.1 Parseo real

| Inventario | Archivos | Medidos (Data.IR) | Fallback PCA | Fallidos |
|---|---:|---:|---:|---:|
| Módulo (`magisk_module/.../sofa/`) | 239 | **0 (0.0%)** | 239 | 0 |
| Contadores `loadCounters()` | — | 0 | 239 | 0 |

- **«239/239 legible» es CIERTO** (`load()` devuelve true en los 239,
  porque el fallback PCA también devuelve true). **«239/239 medidos» es
  FALSO (0/239).** La distinción no existía en telemetría hasta este trabajo:
  añadidos contadores globales `LoadCounters {measured, reconstructed, failed}`,
  `loadCounters()`, `resetLoadCountersForTest()` (`SofaHRTFLoader.hpp/.cpp`,
  incrementos en los 9 puntos de retorno de `load()`).
- **Causa raíz medida:** los 239 archivos del módulo son **HDF5 superblock
  v0** (byte 8 = 0; verificado en `MIT_KEMAR_normal_pinna.sofa`,
  `hpir_nh10.sofa`, `hpir_AKGK271MKII_nh719.sofa` del módulo). El lector
  mínimo de `SofaHRTFLoader` solo soporta **superblock v2** (v0/v1/v3 →
  `UNSUPPORTED` → fallback PCA). Los archivos de `app/src/main/assets/`
  (MIT KEMAR, TU-Berlin, demo) **sí son v2** y sí parsean como medidos
  (lo prueban los tests de catálogo existentes). La re-codificación
  float32+gzip9 de 2026-11-10 (`tools/reencode_sofa.py`) produjo los v0
  del módulo (default `libver earliest` de h5py).
- **Consecuencia:** en vivo, con el inventario del módulo, **ORBE renderiza
  siempre con la reconstrucción PCA, nunca con una HRTF medida** — incluido
  el MIT KEMAR que se embarca. Qué inventario carga el teléfono en vivo
  (módulo v0 vs assets v2) queda pendiente de verificación en vivo con los
  contadores nuevos (§7).
- `HrtfManager::loadSofaDataset` solo instala SOFA con
  `Status::PARSED_MEASURED`: los v0 no desplazan el dataset vigente (el
  fallback PCA sigue siendo el que suena).

### 5.2 Artefacto del vecino más cercano (MIT KEMAR, azimut 0→30°, paso 1°)

- **24 pasos idénticos** (mismo vecino) y **6 saltos** en 30 pasos de 1°.
- Salto máximo: **0.0443 (100.4% del RMS del HRIR)** en 17°→18°. Otros saltos:
  2°→3° 83.8%, 7°→8° 94.2%, 12°→13° 99.8%, 22°→23° 95.5%, 27°→28° 87.6%.
- Un movimiento de 1° puede cambiar el HRIR entero: `hrtfAt` es vecino más
  cercano sin interpolación (`SofaHRTFLoader.cpp:678-692`). Con head-tracking
  activo, esto es un escalón audible por grado en 6 de cada 30 grados.
- Contadores distinguen los 3 caminos (test: medidos=1, reconstruidos=1,
  fallidos=1 con KEMAR real + archivo con firma HDF5 falsa + ruta inexistente).

---

## 6. Defectos nuevos encontrados (no estaban en el brief)

1. **HRTF del módulo 0/239 medidos (§5.1):** el inventario que se embarca
   no lo parsea el lector mínimo (superblock v0 vs v2). ORBE suena con PCA
   siempre. Fix candidato: re-codificar con superblock v2 (h5py
   `libver='latest'`) o ampliar el lector a v0 — no hecho aquí (toca el
   inventario, no el código medido).
2. **ORBE a azimut 0° re-codifica el ITD (con HRTF PCA):** entrada ITD +10.00
   / ILD +6.02 dB → salida ORBE-solo ITD **−19.06** muestras / ILD +1.61 dB.
   A azimut 0° el render debería preservar la imagen (ITD ≈ 0); en su lugar
   la invierte (−396 µs). Pendiente verificar si es artefacto de la PCA o
   bug de ORBE (re-medir con HRTF medida v2 de assets).
3. **La constante de referencia del tono estaba mal por 8.3e-05 dB:**
   `kInRmsDb = −6.0206` (literal redondeado) vs el valor exacto
   20·log10(0.7071/√2) = −6.020683 dB. Lo detectó el test de neutralidad
   (dio −8.32e-05 dB en vez de 0.0). Corregido a cálculo exacto en el test.
4. **`guardianWidener` es telemetría PRE-arbitraje:** vale 1.350 aunque el
   factor esté suprimido en SPEAKERS/binaural. Quien lea la telemetría
   creerá que el widener está activo donde no lo está. La contención solo
   se verifica en señal (§2.4).
5. **Interacción de −25 dB a 11.5 kHz sin atribuir** (§2.2): el marginal de
   ORBE a 11500 Hz (+21.718 dB) es el mayor de la tabla y la interacción
   total a esa banda es −25 dB. Candidato principal: árbitro D10 (activo
   por defecto). Sin aislar.

## 7. Pendiente (y por qué)

- **Medición en vivo en el teléfono (solo lectura, sin acceso en esta tarea):**
  (a) `loadCounters()` en vivo: ¿qué inventario carga el teléfono, módulo
  v0 (PCA) o assets v2 (medido)? (b) contención del guardián en vivo con
  música real (el clasificador MUSIC en host disparó con tono de 1 kHz;
  con música puede no disparar igual). (c) `roomSizeM` vivo confirmado 5.0
  (el código dice 5.0 en todas partes; falta el valor en el snapshot vivo).
  (d) ORBE a azimut 0° con HRTF medida: ¿persiste la inversión de ITD?
- **Aislar la interacción de −25 dB a 11.5 kHz:** re-medir LOO por banda con
  el árbitro D10 apagado (`D10Config.enabled = false`) y con el limitador
  de seguridad apagado, por separado.
- **Regla de Oro por nivel y por tiempo:** el trim que cierra la cadena a
  0.0 dB depende del nivel (AGC de NEURO) y del instante (asentamiento ~1 s).
  Falta la tabla trim-vs-nivel (p. ej. −12/−6/−3 dBFS) y la curva temporal.
- **Kernel Genezi medido:** correr `tools/genezi_calibrate.py` sobre
  barridos Farina reales del MHC-PZ1D y cablear el archivo resultante
  (requiere la medición acústica; fuera del alcance digital de esta tarea).
- **HRTF:** re-codificar el inventario del módulo a superblock v2 o ampliar
  el lector mínimo a v0 (decisión de inventario, no tomada aquí).
- **ABX:** cancelado por Esse el 2026-11-25; nada de esto es una prueba ciega.
  Todo lo anterior es medición digital, no escucha.

## Archivos tocados (resumen)

| Archivo | Cambio |
|---|---|
| `tests/test_p1p4_ablation.cpp` (nuevo) | LOO 1 kHz + por banda ERB, Regla de Oro por banda, trim compilado por banda, ARMONÍA, SALA VIVA, widener ITD/ILD, guardián |
| `tests/test_d10_corona.cpp` | Comentario «sigue FALLANDO» corregido: el test PASA hoy a 1 kHz con trim compilado (medido −0.077/+0.076 dB); la refutación que queda es por banda |
| `tests/test_p1p4_neuro.cpp` (nuevo) | Descomposición NPE, respuesta al escalón FreeEnergy, motor (señal interna vs mic) |
| `tests/test_p1p4_hrtf.cpp` (nuevo) | Parseo 239 SOFA, artefacto vecino más cercano, contadores 1/1/1 |
| `tests/CMakeLists.txt` | 3 targets nuevos (neuro explícito con `phase_oracle.cpp` + `host_stubs`) |
| `fused/free_energy_loop.hpp` | Accesor `gammaModDb()`; «gradiente variacional» → heurística (legacy) |
| `fused/new_physics_engine.hpp` | Accesor `freeEnergyGammaModDb()`; «lazo variacional F» → heurístico |
| `fused/new_physics_controls.hpp` | «lazo variacional F» → heurístico |
| `fused/fused_controls.hpp` | Regla de Oro: «0.0 dB» retirada como propiedad enganchada; `trimDb` acotado |
| `fused/fused_chain.hpp` | Matched-LUFS acotado a neutro + nominal P8 |
| `tests/test_p0_reference_lock.cpp` | Comentario «debe ser ~0 dB» marcado FALSO |
| `SofaHRTFLoader.hpp/.cpp` | Contadores `LoadCounters`, `loadCounters()`, `resetLoadCountersForTest()` |
| `jni/ivanna_omega_jni.cpp` | Admisión: kernel Genezi medido nunca cargado en vivo |
| `NewPhysicsControlCard.kt`, `ControlHelpCatalog.kt` | «Lazo Variacional F» → «Lazo F (heurística…)» |
