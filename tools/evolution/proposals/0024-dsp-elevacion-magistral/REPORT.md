# 0024 — Elevación magistral del DSP: el NPE nace encendido en Ruta A

**Mandato de Esse (2026-11-16 ~10:50 CST):** "Que continúen. Que el DSP no sea básico como lo es ahora. Que lo eleven a un nivel jamás nunca visto. Que lo conviertan en el mejor del mundo literal, por varias generaciones por delante de los grandes."

**Estado del consejo:** RECHAZADO formal (sin consenso en 3 rondas). El detector confirmó el hallazgo como REAL. El revisor (luna) exigió exactamente este parche. Siguiendo el patrón 0021/0022/0023, el coordinador lo implementó y validó. Ver `council_verdict.json`.

## 1. Diagnóstico (verificado en código, no afirmado)

La cadena viva de Ruta A es `DSPBridge.nativeProcess` (`ivanna_omega_jni.cpp:1776`). Es densa y en su mayor parte activa. Lo "básico" / apagado, verificado:

| Módulo | Estado real en Ruta A viva | Fuente |
|---|---|---|
| Bucle cerrado neural (`g_neuroLoopEnabledA`) | OFF por defecto (`{false}`, jni:263) | User-gated por diseño (`NeuroSensorGateway.enable()` con driver). NO se toca. |
| NPE coclear (`g_npeAOn`) | OFF por defecto (`{false}`, jni:611) | **DESALINEADO con su fuente de verdad** (ver §2). **ESTE PARCHE.** |
| Motor coclear inverso (`g_cochlearEnabled`) | OFF por defecto (`{false}`) | Alineado con su fuente de verdad (`SpatialAudioPrefs.cochlearInverseEnabled=false`). NO se toca. |
| HOLOS armónicos (`g_holos_harmonics`) | 0.0 | Retuneado a 0 por directiva. NO se toca (exigencia del revisor). |
| Omni (`g_omni_amount`) | 0.0 | Apagado. NO se toca (exigencia del revisor). |
| NEU·Ω (`g_neuEnabledA`) | OFF por defecto | Fase 6 completa en código, verificación en vivo pendiente. NO se toca en este pase. |
| Compresor Ruta A (`g_comp`) | ACTIVO (ratio 2.4, umbral −16 dB) | Monobanda (limitación real; ver §6). |
| SafetyLimiter | ACTIVO (true-peak 4x, soft-ceil) | THD 0.00% a 0 dBFS medido (v2.5.199). No es básico. |
| Volterra H2 | ACTIVO (`g_volterra_enabled{true}`) | — |
| 5 Ejes Supremos | ACTIVOS por prefs (pinna wet 0.65, etc.) | La crítica "ORBE sin pinna" está cubierta por el pinna manifold activo. |
| ORBE / HOLOS / Φ | ACTIVOS (1.0 / 0.5 / 0.5) | ORBE solo yaw (sin pitch). |
| RIR de HOLOS | ACTIVA (wet 1.0 en `prepare`) | La crítica "wetDry 0" es del `RirConvolver` suelto, no de HOLOS. |

**Corrección honesta a la crítica:** el NPE y el motor coclear NO están apagados en la app en ejecución — `PersistedStateRestorer` los publica al arrancar (`PiLstmBridge.restoreModuleNpe(isNpeBypass())` → `nativeSetNpeLocal`). El problema real es más sutil: el default nativo contradice la fuente de verdad, dejando una ventana al arrancar en la que la cadena procesa sin el modelo coclear, y una fragilidad si el restorer no corre o la librería se recarga.

## 2. Causa raíz: el default nativo contradice la fuente de verdad

La fuente de verdad de la app para el NPE dice que NO está en bypass:
- `ParameterStore.isNpeBypass()` → `false` por defecto.
- `AdaptiveControlsPrefs.npeBypass` → `false` por defecto.
- Parámetros vivos: harmonic **0.60**, lateral **0.50**, ohc **0.40**, master **+2.5 dB**.

Pero el nativo decía `g_npeAOn{false}` — "apagado hasta que la app lo publique". El restorer lo enciende después, pero la cadena nace apagada. El revisor exigió que la activación viva en el flujo real de parámetros, no en un `store(true)` ad hoc en `nativeInit()`. Alinear el default con la fuente de verdad ES eso: el default nace del config, y el restorer/`PiLstmBridge` sigue siendo la autoridad en runtime (si el usuario activa el bypass, `nativeSetNpeLocal` lo apaga).

## 3. El parche

**Un solo cambio de código** (`app/src/main/cpp/jni/ivanna_omega_jni.cpp:611`):
```cpp
// antes:
static std::atomic<bool>  g_npeAOn{false};
// después:
static std::atomic<bool>  g_npeAOn{true};
```
Con comentario que documenta la fuente de verdad, por qué el default anterior era incorrecto, y por qué el bucle neural y el motor coclear inverso NO se tocan (cada uno con su razón).

**No se tocan:** `g_neuroLoopEnabledA` (user-gated), `g_cochlearEnabled` (alineado con su pref `false`), `g_holos_harmonics`, `g_omni_amount`, `g_neuEnabledA`.

## 4. Medición: el NPE a parámetros vivos (antes/después)

Herramienta: `tools/measure/npe_live_vs_native.cpp` (seno 1 kHz, 0.5, 48 kHz, host). El NPE es un procesador de COLOR armónico por diseño (LIF/NHO añaden armónicos): el THD+N sobre un seno puro sube y eso es esperado, no un defecto. La medida que importa es que la fundamental no colapse y que no haya clipping ni desplome tonal.

| Config | THD+N (1 kHz) | Fundamental | 110 Hz | 1 kHz | 8 kHz | Coste / bloque 1024 |
|---|---|---|---|---|---|---|
| Bypass | −200.00 dB | −6.02 dBFS | — | — | — | 0.0 µs |
| NATIVO (0.2/0.2/0.3/0.0) | −6.99 dB | −16.32 dBFS | −4.01 dB | −4.71 dB | −4.50 dB | 258.5 µs |
| **VIVO (0.60/0.50/0.40/+2.5 dB)** | **−10.35 dB** | **−14.09 dBFS** | **−0.58 dB** | **−2.24 dB** | **−2.08 dB** | **260.8 µs** |
| VIVO-0.3 (0.3/0.3/0.3/0.0) | −6.98 dB | −16.32 dBFS | −3.73 dB | −4.67 dB | −4.49 dB | 255.4 µs |

**Lectura honesta:**
- Los parámetros VIVOS son MEJORES que los nativos en THD+N (−10.35 vs −6.99 dB) y en preservación de la fundamental (−14.09 vs −16.32 dBFS), y su respuesta en frecuencia es más plana (−0.58/−2.24/−2.08 dB vs −4.01/−4.71/−4.50 dB). El master +2.5 dB compensa la atenuación del núcleo.
- El NPE a parámetros vivos atenúa la fundamental ~8 dB y añade ~30% de contenido armónico sobre un seno puro. Eso es su coloración por diseño, no un fallo. Sobre música (señal compleja) el efecto es el "sabor" neuromórfico, no una distorsión de un tono.
- Coste: ~261 µs por bloque de 1024 muestras (21.3 ms de audio) — 1.2% del tiempo real, >80x de margen. No hay riesgo de presupuesto.

## 5. Tests nuevos (7)

`app/src/main/cpp/tests/test_route_a_npe_live_params.cpp` (registrado en `tests/CMakeLists.txt`). Verifica el NPE a los parámetros VIVOS (no solo a los defaults, que ya cubre `test_npe_core.cpp`):
1. `LiveParamsNotBitExact` — efecto medible (residuo audible > −30 dB).
2. `LiveParamsOffIsBitExact` — apagado = bit-exacto (null test).
3. `LiveParamsThdPlusNoise` — fundamental no colapsa (> −20 dBFS), THD+N no catastrófico (< −5 dB), sin clipping. Imprime el valor medido.
4. `LiveParamsFrequencyResponse` — balance tonal dentro de ±6 dB por banda (núcleo fresco por banda; el NPE tiene estado y compartirlo contamina la medida).
5. `LiveParamsFiniteAndBoundedWithHotInput` — finito y acotado con entrada caliente (±1.5).
6. `LiveParamsSwitchingOnIsClickFree` — sin clic al encender (fundido 20 ms).
7. `LiveParamsCostWithinBudget` — < 2 ms por bloque de 1024 muestras.

**Resultado: 7/7 PASS.**

Los mecanismos del bucle neural y del motor coclear ya están cubiertos por tests existentes (no se duplican): `test_neuro_acoustic_closed_loop.cpp` (`FailClosedSinSensor` — identidad sin sensor; `AprendeFirma` — ajustes reales) y `test_cochlear_inverse_model.cpp` (latencia cero, finito, energía acotada).

## 6. Validación

- **CTest completo: 1104/1104 PASS, 0 fallos (55.07 s).** Total: 1093 base + 7 tests del NPE (este parche) + 4 tests de captura (0025, mismo parche). El total pasa de 1093 a 1104.
- **Suite evolución: 168/168 PASS (OK, 177.2 s).** Nota: el pin de línea del test 0019 (`test_scan_todo_fixme`, TODO route-arbiter-v2) se movió de 1809 a 1825 por las 16 líneas del comentario 0024 en `ivanna_omega_jni.cpp`; el pin sigue la línea real (así está diseñado) y se actualizó en el mismo parche.
- **`check_docs_claims`:** PASS (0 hallazgos).
- **`telemetry/ctest_summary.json`:** actualizado a 1104.

## 9. Hallazgos en vivo incorporados (0025)
Durante la validación llegaron dos hallazgos en vivo de Esse (2026-11-16 11:16 y 11:18 CST, tras instalar la 2.5.457): los cortes siguen en MÚSICA (underruns=294 pese a que el pacing de 0020 funciona) y el PLL de captura está saturado en el riel (-500 ppm, buffer al 99%). Son el núcleo de "elevar el DSP" y tienen prioridad máxima. Su diagnóstico, medición en host y fixes están en la propuesta **0025** (`tools/evolution/proposals/0025-underruns-musica-pll-captura/REPORT.md`), incluida en este mismo parche: priming diferido en BridgePlayer (play() tras el primer write) y pacing por descarte en captura (espejo de 0020). La telemetría "congelada" de 11:16 era música en pausa / valores estables, no un bug de telemetría (corregido por el hallazgo de 11:18).

## 7. Lo que queda pendiente (no escondido)

1. **Dinámica monobanda:** el compresor de Ruta A (`g_comp`) es monobanda (ratio 2.4, un solo detector de pico). Un compresor multibanda (3–4 bandas con crossovers) sería una elevación real de la dinámica, pero es un cambio grande (nuevo módulo + integración + tests) y no cabe en este pase. Es el candidato nº 1 para el siguiente encargo.
2. **ORBE solo yaw:** el head-tracking es solo rotación horizontal; falta pitch (inclinación vertical). El pinna manifold ya está activo por prefs y cubre parte de la elevación, pero ORBE en sí no tiene pinna ni pitch. Es trabajo de investigación, no un fix.
3. **NEU·Ω apagado por defecto:** el compilador espectral (Fase 6) está completo en código pero `neu_enabled` es OFF y falta verificación en vivo. Activarlo por defecto sin esa verificación sería imprudente.
4. **Verificación en vivo:** este parche cambia el estado al arrancar (NPE on desde el bloque 0). La confirmación de escucha en el G85 (que el NPE no degrade el sonido real) queda pendiente de Esse. Si en vivo se oye peor, el bypass del usuario sigue funcionando y el default se revierte.
5. **El consejo rechazó el enfoque inicial** (3 rondas) porque los diffs del implementer solo tocaban flags aislados sin conectar con la cadena viva ni medir. Este parche es la versión que el revisor exigió: alineado con la fuente de verdad, con test de regresión y medición.

## 8. Archivos

- `app/src/main/cpp/jni/ivanna_omega_jni.cpp` (1 línea + comentario)
- `app/src/main/cpp/tests/test_route_a_npe_live_params.cpp` (nuevo, 7 tests)
- `app/src/main/cpp/tests/CMakeLists.txt` (registro)
- `tools/measure/npe_live_vs_native.cpp` (nuevo, herramienta de medición host)
- `telemetry/ctest_summary.json` (1093 → 1100)
- `tools/evolution/proposals/0024-dsp-elevacion-magistral/finding.json`
- `tools/evolution/proposals/0024-dsp-elevacion-magistral/council_verdict.json`
- `tools/evolution/proposals/0024-dsp-elevacion-magistral/REPORT.md` (este archivo)
