# 0028 — NEU·Ω Fase 3: cola difusa en frecuencia (E5)

## 0. Resumen

La cola RIR deja de tener su propio STFT y su propio IFFT: usa el espectro
`X[k]` ya calculado por el núcleo NEU y se suma a `M·X` antes del único IFFT
del `SpectralCore`. Se eliminan el STFT y el IFFT que la cola en tiempo
necesitaba (un par FFT/IFFT 1024 por bloque en el `RirConvolver`), y la cola
pasa a ser un estado complejo por bin con feedback (decay por RT60 +
rotación de fase por bin, ángulo áureo). La cola es difusa y ruidosa por
naturaleza: la equivalencia con una convolución exacta NO es el criterio —
lo es la curva de decaimiento energético (EDC): mismo T60 y EDC ±1 dB por
banda (criterio E5 del diseño).

**Consejo: RECHAZADO formal** (detector haiku: FALSO POSITIVO — "Fase 3 no
implementada" es fase de diseño, no bug; ronda 1: reviewer luna RECHAZÓ el
diff del implementer — VACÍO — y exigió exactamente esta implementación;
ronda 2: implementer sin diff utilizable en 5 intentos). Veredicto en
`council_verdict.json`. Implementado por el coordinador (patrón
0023/0024/0025).

## 1. Diagnóstico (por qué la cola en tiempo es un desperdicio)

El `RirConvolver` procesa la cola con convolución particionada (FDL):
32 particiones, FFT 1024 por bloque, más su propio IFFT. Eso es un segundo
pipeline espectral completo corriendo en paralelo al del `SpectralCore`
(FFT 512, hop 128), solo para la cola. El `SpectralCore` ya calcula `X[k]`
cada hop — la cola puede vivir ahí, como un término `T[k]` que se suma a
`M·X` antes del único IFFT. Ahorro: el par FFT/IFFT 1024 de la cola
desaparece; la cola en frecuencia cuesta ~257 bins × ~20 flops por hop
(medido: menos que un par FFT/IFFT 512, ver §4).

Referentes: Trinnov Altitude32 (Optimizer) y Lake LM44 (linealización de
fase) hacen su corrección de sala en el dominio espectral, no con una
segunda convolución en tiempo. IVANNA ahora hace lo mismo — y además la
cola es difusa (decorrelada por bin), que es lo que una sala real hace.

## 2. Diseño (verificado en docs y código)

### 2.1 Cola en frecuencia (SpectralCore)

Estado complejo por bin (0..N/2), por banda ERB (el decay es el mismo para
todos los bins; la EDC por banda hereda el mismo T60):

```
T[k] <- T[k] * decay * rot[k] + X[k] * feed
Y[k] += T[k] * wet
```

- `decay` por hop desde RT60 (amplitud): `g = 10^(-3*hopSec/T60)`
  (hop 2.667 ms; T60=0.34 s → g≈0.947). Recalculado al cambiar RT60 o SR.
- `rot[k]` = rotación de fase pseudo-aleatoria fija por bin (ángulo áureo
  2.399963 rad × k, determinista, sin RNG en el hot path): tasas de giro
  inconmensurables entre bins = decorrelación (la cola suena difusa, no
  como una copia retardada).
- `feed` ≈ 0.05 (nivel: en régimen, cola ≈ feed/(1-decay)×|X|×wet
  ≈ 0.21×|X| con wet 0.22). Configurable.
- `wet` = nivel de cola [0,1], con fundido suave de 20 ms (one-pole por
  hop) al activar/desactivar (sin clic).
- La cola se suma DESPUÉS de aplicar la matriz y ANTES del único IFFT
  (sobre `reL_/imL_/reR_/imR_` desempaquetados, con espejo hermitiano
  para k>N/2). El bloque `if (matrixEnabled_)` de `runFrame()` ahora es
  `if (matrixEnabled_ || tailEnabled_)`, para que la cola funcione aunque
  la matriz esté desactivada (M=I).
- El estado SIEMPRE decae+rota (drena cuando está desactivada); solo se
  alimenta (feed) y se suma (wet) cuando está activada y no falló.
- Fail-closed: OFF por defecto (T=0, como antes). Si el estado se vuelve
  no-finito, la cola se desactiva sola (`tailFailed_`) y el integrador
  (JNI) vuelve a la cola en tiempo (`RirConvolver`).

### 2.2 RirConvolver: solo el head cuando NEU lleva la cola

Flag atómico `tailEnabled_` (default true). El paso FDL de cola en
`process()` (~línea 577) ahora exige `tailEnabled_`:

```cpp
if (tailEnabled_.load(...) && tailPartsActive_ > 0 && loaded_ && ...)
```

Con `setTailEnabled(false)`, `process()` solo convoluciona el HEAD
(primeros 512 muestras, latencia cero, reflexiones tempranas) y salta el
paso FDL de cola. Con true (default), el comportamiento es el de siempre
(head + cola). Atómico en tiempo real.

### 2.3 Integración JNI (Ruta A, `ivanna_omega_jni.cpp`)

En la sección RIR de `nativeProcess` (donde se lee el snapshot
`room_rt60_s`/`room_wet` y se configura el `RirConvolver`), antes de
`process()`:

```cpp
const float rirWetNow = s_rirConvolver->wetDry();  // wet efectivo (con permiso)
const bool neuTailOk = neuEnabled && rirWetNow > 0.001f &&
                       s_rirRt60 >= 0.01f && !g_neuCoreRouteA.tailFailed();
if (neuTailOk) {
    g_neuCoreRouteA.setTailRt60(s_rirRt60);
    g_neuCoreRouteA.setTailWet(rirWetNow);
    g_neuCoreRouteA.setTailEnabled(true);
    s_rirConvolver->setTailEnabled(false);
} else {
    g_neuCoreRouteA.setTailEnabled(false);
    s_rirConvolver->setTailEnabled(true);
}
```

- `s_rirRt60` es un static nuevo que persiste el RT60 del último snapshot
  (como `s_rirWetBase` persiste el wet).
- El wet de la cola es el wet EFECTIVO del RIR (`wetDry()`, con el permiso
  de sala ya aplicado), no solo la base.
- La configuración se fija en la sección RIR (tras leer el snapshot) y el
  núcleo NEU la usa en el bloque siguiente (los parámetros de sala cambian
  despacio: un bloque de retardo es inaudible). Si la cola espectral falla
  en un bloque, el RIR vuelve a la cola en tiempo en el bloque siguiente
  (como mucho un bloque ~5 ms sin cola: inaudible; y dentro del núcleo la
  cola ya es no-op al fallar, así que no se suma basura).
- Ruta B NO se toca (el núcleo NEU es Ruta A; la Ruta B sigue con su cola
  en tiempo, sin cambios).

## 3. Implementación

- `app/src/main/cpp/spatial/SpectralCore.hpp`:
  - API: `setTailEnabled(bool)` (default false), `setTailRt60(float)`
    (default 0.34 s, clamp [0.05, 5.0]), `setTailWet(float)`,
    `setTailFeed(float)` (default 0.05); `tailEnabled()`, `tailFailed()`,
    `tailRt60()`, `tailWet()`.
  - Estado: `tailReL_/tailImL_/tailReR_/tailImR_` (por bin 0..N/2),
    `tailRotRe_/tailRotIm_` (ángulo áureo, calculado en `prepare()`),
    `tailDecay_` (desde RT60, `updateTailDecay()`), `tailWetSmooth_`
    (fundido 20 ms).
  - `applyTail()`: decay+rota siempre; feed+suma solo si activa y no
    falló; espejo hermitiano; fail-closed a no-finito.
  - `runFrame()`: el bloque de desempaquetar/matriz/re-empaquetar corre
    si `matrixEnabled_ || tailEnabled_`; la matriz sigue protegida por
    `matrixEnabled_`; `applyTail()` se llama tras la matriz y antes de
    re-empaquetar.
  - `reset()`: estado de cola a cero, `tailFailed_=false`,
    `tailWetSmooth_=0`.
- `app/src/main/cpp/spatial/RirConvolver.hpp` + `.cpp`:
  - `setTailEnabled(bool)` / `tailEnabled()` (atómico, default true);
    el paso FDL de cola exige `tailEnabled_`.
- `app/src/main/cpp/jni/ivanna_omega_jni.cpp`:
  - `static float s_rirRt60 = 0.34f;` + actualización en el snapshot.
  - Ruteo de cola antes de `s_rirConvolver->process()` (ver §2.3).
- `app/src/main/cpp/tests/test_neu_fase3_tail.cpp` (nuevo, 6 tests):
  1. `TailOffIsIdentity` — cola OFF = E1 intacta (identidad, error ≤1e-6).
  2. `TailEdcMatchesRt60` — pendiente de decaimiento −60/RT60 dB/s (±15%,
     R²>0.9) tras un impulso; la cola está presente.
  3. `TailLevelReasonable` — pico de cola en (0.001, 1.0); salida finita
     y acotada (<2.0) en 1 s de ruido.
  4. `TailCostLowerThanFftPair` — cola por hop < par FFT/IFFT 512 y <50 µs.
  5. `TailToggleClickFree` — pendiente ≤3× el régimen al activar/desactivar.
  6. `RirTailSkipHeadOnly` — con tail OFF, el head es idéntico y la cola
     (muestras 1024+) es ~0; con tail ON la cola está presente.
- `app/src/main/cpp/tests/CMakeLists.txt`: `ivanna_add_test(test_neu_fase3_tail)`.

## 4. Validación

- **Tests nuevos: 6/6 PASS** (verificado por el coordinador, binario
  `test_neu_fase3_tail`, 2026-11-16): TailOffIsIdentity (E1 intacta),
  TailEdcMatchesRt60 (pendiente −60/RT60 dB/s, R²>0.9), TailLevelReasonable,
  TailCostLowerThanFftPair (cola por hop < par FFT/IFFT 512 y <50 µs),
  TailToggleClickFree (fundido 20 ms), RirTailSkipHeadOnly (el flag salta
  el paso FDL; la cola ON tiene ~50× la energía de la OFF).
- **CTest completo: 1120/1120, 100% verde** (rebuild limpio, 0027+0028+0029
  juntas; la 0028 sola aportó 1116).
- **Suite evolución: 168/168 OK.**
- **`check_docs_claims`: PASS (incluido en la suite de evolución).**
- **`telemetry/ctest_summary.json`: actualizado a 1120/1120.**
- **Kotlin:** sin cambios Kotlin (la integración es C++ + JNI; la CI debe
  compilarlo). La Ruta B no se toca.

**Lecciones de la validación:**
- El test `RirTailSkipHeadOnly` falló primero porque con tail OFF el
  residuo tras el head NO es cero: el DC-blocker de salida del RIR (polo
  0.9993) sigue sonando ("ringing") tras el head. El criterio correcto es
  RELATIVO (ratio ON/OFF >10×), no cero absoluto.
- `RirConvolver` está en namespace `Ivanna` (mayúscula), no `ivanna` — el
  test no compilaba hasta corregirlo. Y `RirConvolver.cpp` NO está en
  `ivanna_dsp_under_test`: hay que añadirlo como fuente del target (como
  hace `test_processor_evidence`).
- `ASSERT_*` dentro de una lambda que devuelve un valor no compila
  (el ASSERT hace `return;` void): usar `EXPECT_*`.
- Al pasar de OFF a ON, `setTailEnabled(true)` limpia el estado de cola
  (si no, un estado congelado de audio viejo entraría como "fantasma").
  `tailFailed_` NO se limpia ahí (fail-closed estricto: solo `reset()`).

## 5. Lo que queda pendiente (no escondido)

1. **Verificación en vivo:** que en bocinas G85 + DAC → Genezi la sala
   suene igual (o mejor) con la cola en frecuencia que con la cola en
   tiempo, y que el RT60 percibido coincida con el configurado. Si en vivo
   se oye peor, NEU OFF vuelve a la cola en tiempo (el fallback es
   automático y el flag `tailEnabled_` del RIR la restaura).
2. **Per-band EDC:** el test mide el T60 de banda ancha (la pendiente es
   la misma para todos los bins por construcción, así que cada banda ERB
   hereda el mismo T60). Una medición EDC por banda explícita (con
   filtrado por banda) sería un refinamiento posterior.
3. **Fase 4 (NEU·Ω):** la siguiente fase del compilador espectral
   (documentada en el diseño NEU·Ω).
4. **Siguiente oleada:** 0029 (núcleo en doble precisión).

## 6. Archivos

- `app/src/main/cpp/spatial/SpectralCore.hpp` (cola en frecuencia)
- `app/src/main/cpp/spatial/RirConvolver.hpp` (flag `tailEnabled_`)
- `app/src/main/cpp/spatial/RirConvolver.cpp` (gate del paso FDL)
- `app/src/main/cpp/jni/ivanna_omega_jni.cpp` (ruteo JNI + `s_rirRt60`)
- `app/src/main/cpp/tests/test_neu_fase3_tail.cpp` (nuevo, 6 tests)
- `app/src/main/cpp/tests/CMakeLists.txt` (registro)
- `tools/evolution/proposals/0028-neu-fase3-cola-e5/finding.json`
- `tools/evolution/proposals/0028-neu-fase3-cola-e5/council_verdict.json`
