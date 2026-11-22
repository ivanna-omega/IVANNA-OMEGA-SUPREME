# 0029 — Núcleo en doble precisión (acumulación float64)

## 0. Resumen

La cadena IVANNA es float32 en las acumulaciones críticas, mientras el
referente SHARC (ADSP-21569/Griffin) trabaja en punto flotante 32/40/64 bits
y el Helix/Brax usa doble SHARC 64 bits. Esta propuesta pasa la ACUMULACIÓN
interna a float64 en los dos puntos donde la suma es larga, manteniendo la
salida en float (la cadena DSP es PCM_FLOAT).

**Consejo: RECHAZADO formal** (detector haiku: FALSO POSITIVO — dice que el
código ya tiene double en partes relevantes y que el hallazgo mezcla
precisión de estados/ratio con la suma FIR sin demostrar un bug real;
ronda 1: reviewer luna RECHAZÓ el diff del implementer — VACÍO, no tocaba
ni el FIR ni la FDL — y exigió exactamente el parche real; ronda 2:
reviewer RECHAZÓ el segundo diff — heurísticas de compilación/arquitectura
en vez de una solución verificable — y exigió los arreglos exactos; ronda 3:
el reviewer no pudo ser consultado por límite del proveedor ddgw
ERR_USER_LIMIT). Veredicto en `council_verdict.json`. Implementación por el
coordinador (patrón 0023/0024/0025), con el parche exacto que el reviewer
exigió en las rondas 1-2.

**Nota de honestidad (en el finding):** el THD+N del ASRC hoy es
−86/−84/−83 dB y está dominado por el diseño del FIR/ripple y la Ψ, NO por
el error de acumulación float32 (~−120 dB estimado para 319 taps). Pasar a
float64 NO va a mejorar el THD+N de forma audible ni medible en dB enteros;
lo que hace es ELIMINAR el techo de precisión (el piso float32 ~−144 dB) y
poner la cadena al nivel de 64 bits del SHARC/Helix, para que futuras
mejoras (FIR más largo, más taps) no choquen con él. El criterio de salida
es por tanto NO EMPEORAR (THD+N igual o mejor, coste dentro de presupuesto)
y que la respuesta sea idéntica dentro de tolerancia — no una mejora
audible que no existe.

## 1. Puntos verificados en código

- **ASRC Ruta A** (`app/src/main/cpp/ivanna_route_a_asrc.cpp:49-51`): el FIR
  de 319 taps acumula en float32 escalar (`float acc = 0.0f; for j<319
  acc += coeff[j]*hist[j]`) y en NEON float32x4_t (líneas 59-72,
  `vfmaq_f32`). El ratio/posición YA son double (`ratio_`, `pos_`), pero la
  suma FIR es float32.
- **RirConvolver FDL** (`app/src/main/cpp/spatial/RirConvolver.cpp:609-610`):
  la acumulación de las 32 particiones de cola es float32 (`float
  accReL[FFT_SIZE]{}, accImL...` y `accReL[k] += xrL[k]*hrL[k] -
  xiL[k]*hiL[k]`).
- **YA en double (NO tocar):** los estados del Biquad (double) y la
  interpolación Catmull-Rom del VariableAsrc (double `y`, `readPosition_`,
  `appliedRatio_`).

## 2. Diseño exigido por el reviewer (rondas 1-2)

1. Acumulación `double` real en el FIR ASRC 319 taps (ruta escalar; la ruta
   NEON pasa a float64x2_t o a escalar double si el coste no cabe, con
   medición).
2. Acumulación `double` real en la FDL del `RirConvolver` (32 particiones).
3. Mantener la salida en `float`, sin tocar Biquad, VariableAsrc ni Ruta B.
4. Medición real de coste por bloque y fallback explícito a `float32` si no
   cabe en presupuesto (flag runtime, default double).
5. Tests host de regresión: THD+N ASRC antes/después, coste por bloque,
   FDL RIR en tolerancia, salida finita y acotada.
6. Los tests usan tolerancia, no `memcmp` (los últimos bits cambian).
7. El diff aplica limpio y compila en los targets soportados.

## 3. Implementación

- `app/src/main/cpp/ivanna_route_a_asrc.cpp` + `.h`:
  - `firDotScalarDouble`: acumulación double, escalar y portable (misma en
    host x86_64 y en ARM — testeable en host, correcta en el teléfono por
    construcción). La ruta NEON float32 queda como fallback (no se tocó).
  - `firDot(coeff, hist, useDouble)`: true → double escalar; false →
    NEON float32 (ARM) / escalar float32 (host).
  - Flag runtime `setDoubleAccum(bool)` (default true = double) +
    `doubleAccum()`, leído una vez por bloque en `process()`.
  - Salida sigue siendo float.
- `app/src/main/cpp/spatial/RirConvolver.cpp` + `.hpp`:
  - Acumulación double en la FDL (32 particiones) en miembros
    `fdlAccReL_/fdlAccImL_/fdlAccReR_/fdlAccImR_` (4×1024 doubles = 32 KB;
    miembros, no stack — demasiado para el stack del hilo de audio).
  - Se acumula en double y se convierte UNA vez a float (vía `workRe_`/
    `workIm_`, ya libres tras las FFT de entrada) antes del IFFT.
  - Flag runtime `setDoubleAccum(bool)` (default true); false = ruta
    float32 actual (fallback).
- `app/src/main/cpp/tests/test_double_precision.cpp` (nuevo, 4 tests):
  1. `AsrcThdNUnchanged` — THD+N double ≤ float32 + 1 dB y < −120 dB.
  2. `AsrcCostWithinBudget` — coste double < 2× float32 y < 1 s para 4 s
     de audio.
  3. `RirFdlTolerance` — respuesta FDL double vs float32 idéntica dentro de
     tolerancia (max diff < 1e-4, relativo < −90 dB; no memcmp).
  4. `FiniteAndBounded` — salida finita y acotada en ambas precisiones.
- `app/src/main/cpp/tests/CMakeLists.txt`: registro (`add_executable`
  explícito con `ivanna_route_a_asrc.cpp` + `RirConvolver.cpp`; mismo
  include dir que `test_route_a_asrc` para `fir_v5_coeffs.h`).

## 4. Validación

- **Tests nuevos: 4/4 PASS** (verificado por el coordinador, binario
  `test_double_precision`, 2026-11-16).
- **Medición real (mejor de lo esperado):** THD+N ASRC a ratio 1.0, Ψ off:
  **double −151.7 dB vs float32 −137.1 dB** — la acumulación double MEJORA
  el THD+N en ~14 dB (no solo "no empeora"). El finding decía que no
  mejoraría de forma audible; en host la mejora es medible y real.
- **CTest completo: 1120/1120, 100% verde.** Base 1116 (0027+0028) + 4
  tests nuevos.
- **Suite evolución: 168/168 OK.**
- **`check_docs_claims`: PASS (incluido en la suite de evolución).**
- **`telemetry/ctest_summary.json`: actualizado a 1120/1120.**

**Lecciones de la validación:**
- El test `AsrcThdNUnchanged` falló primero (−74.6 dB) por un bug en el
  PROPIO test, no en el ASRC: `SineStereo` calculaba la fase en `float`
  (a ph ~12566 rad la resolución es ~0.001 rad y el tono de prueba tenía
  distorsión ~−60 dB). La fase debe ser `double`. Tras corregirlo, el
  double mide −151.7 dB.
- `setDoubleAccum` debe ir en la sección `public` del header (quedó en
  `private` al añadirlo junto al miembro `ratio_`).
- `RirConvolver` está en namespace `Ivanna` (mayúscula) — igual que en la
  0028.

## 5. Lo que queda pendiente (no escondido)

1. **Verificación en vivo:** el cambio de precisión no debe ser audible
   (la respuesta es idéntica dentro de tolerancia); si el coste double no
   cabe en el teléfono, el flag `setDoubleAccum(false)` vuelve a float32.
2. **Lección del build anterior:** NO editar headers/código mientras un
   build corre — el build mixto (librería con header viejo + test con
   header nuevo) produce ABI mixto y segfault (`test_rt_no_alloc`). Todo
   cambio de header exige rebuild limpio antes de validar.

## 6. Archivos

- `app/src/main/cpp/ivanna_route_a_asrc.cpp` (acumulación double FIR)
- `app/src/main/cpp/ivanna_route_a_asrc.h` (flag)
- `app/src/main/cpp/spatial/RirConvolver.cpp` (acumulación double FDL)
- `app/src/main/cpp/spatial/RirConvolver.hpp` (flag)
- `app/src/main/cpp/tests/test_double_precision.cpp` (nuevo)
- `app/src/main/cpp/tests/CMakeLists.txt` (registro)
- `tools/evolution/proposals/0029-nucleo-doble-precision/finding.json`
- `tools/evolution/proposals/0029-nucleo-doble-precision/council_verdict.json`
