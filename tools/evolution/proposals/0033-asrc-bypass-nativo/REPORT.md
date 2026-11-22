# 0033 — Bypass del ASRC en Ruta A: el Snapdragon trabaja de nativo

## 1. Decisión

**El ASRC no va en el Snapdragon — se retira y el Snapdragon trabaja de nativo.** Decisión de Esse, 2026-11-16 18:28 CST. El código del ASRC NO se borra (regla: nunca borrar código): se bypassa con un flag conmutable, default OFF. El reproductor propio (MÚSICA / BridgePlayer, Ruta A) sigue tronando pese al fix 0031 (shed 28% → 3.4%).

## 2. Evidencia (telemetría en vivo de la 459, vc 750, 2026-11-16 17:53 CST)

| Señal | Valor | Lectura |
|---|---|---|
| PLL del ASRC | saturado en el riel **−500 ppm**; demanda **−635 ppm**, `pll_rail=1` | El PLL pide más corrección de la que el riel permite: **no corrige en este chip** |
| Buffer BridgePlayer | `pll_occ` 96694 > cap 92256 | **Sobrelleno** — el ASRC no drena al ritmo que produce el reproductor |
| Coste DSP | 16–21 ms/bloque, picos 24–70 ms, pico de sesión **109 ms** (`dsp_pk_ses=108944`) | El ASRC (FIR 319 taps + doble precisión) es un coste grande en el camino caliente |
| Shed (0031) | 0.034 (era 0.28) | 0031 funcionó, pero el tronido sigue: el shed ya no es la causa principal |
| Underruns AudioFlinger | solo 4 | Los underruns son un síntoma distinto y separado del tronido |

**Diagnóstico:** el ASRC es coste grande y su PLL no corrige en este chip. El sospechoso principal del tronido continuo pasa a ser el NPE (0024, siempre encendido en Ruta A, THD+N −10.35 dB en host) y los picos DSP de 61–109 ms. Quitar el ASRC del camino elimina una fuente de coste y de saturación del buffer, y deja el reloj en manos del AudioTrack nativo del Snapdragon.

## 3. Diseño

**Bypass con flag, default OFF (bypass).** El ASRC queda como código legacy activo solo si se enciende el flag.

- **Nativo (C++):** `RouteAAsrc` (`app/src/main/cpp/ivanna_route_a_asrc.h/.cpp`) gana `setEnabled(bool)` / `enabled()` con `std::atomic<bool> enabled_{true}` (default true = comportamiento existente en el core). Con `!enabled`, `process()` hace `memcpy` bit-exacto, los contadores avanzan y el estado FIR/posición NO se toca. JNI `nativeSetEnabled` en `app/src/main/cpp/jni/ivanna_route_a_asrc_jni.cpp`.
- **Kotlin:** `RouteAAsrc.kt` — companion `@Volatile var asrcEnabled: Boolean = false` (default OFF, se lee al crear cada instancia). `isAvailable` = `asrcEnabled && loaded && handle != 0L`; `init` salta `nativeCreate()` si bypass; `processInto` ya era passthrough con `!isAvailable`.
- **BridgePlayer** (`IvannaBridgePlayer.kt`, línea ~1225): con `isAvailable` → ppm al ASRC (camino histórico); `else if (RouteAAsrc.asrcEnabled)` → ppm al resampler Kotlin (fallback histórico); `else` → `resampler.setClockCorrectionPpm(0.0)` — **PLL en bypass, AudioTrack nativo manda el reloj**.
- **Telemetría:** `asrc=0`, `ppm=0.0` en bypass (automático vía `isAvailable`).

### Qué se pierde

- **Corrección de deriva sub-ppm de IVANNA** (PLL Theil-Sen ±500 ppm + ASRC FIR 319 taps + doble precisión, THD+N −151.7 dB en host). La deriva medida en este chip era +8.5 ppm.
- El manejo de reloj y el resampleo los hace **AudioTrack nativo del Snapdragon** (el mismo que usan todas las apps del teléfono). Sin corrección propia de IVANNA, sin saturación de riel, sin coste FIR en el camino caliente.

### Alcance: Ruta A solamente

La **captura** (`PlaybackCaptureService.kt:735`) usa la misma clase `RouteAAsrc`: con el flag OFF, su `processInto` es passthrough y su `setClockCorrectionPpm` es no-op con `!isAvailable`. **Queda dentro del bypass por compartir la clase, sin tocar su código.** No se tocó nada más de la captura.

### Cómo reactivarlo (pruebas futuras)

1. En la app: `RouteAAsrc.asrcEnabled = true` antes de crear el reproductor (o exponerlo en UI/debug). El flag se lee al crear cada instancia.
2. En nativo: `RouteAAsrc::setEnabled(true)` / JNI `nativeSetEnabled(handle, true)`.
3. Con el flag ON, el camino vuelve a ser el histórico: ASRC + PLL Theil-Sen ±500 ppm. El código sigue ahí, con sus tests.

## 4. Archivos

| Archivo | Cambio |
|---|---|
| `app/src/main/cpp/ivanna_route_a_asrc.h` | `setEnabled(bool)` / `enabled()` + `std::atomic<bool> enabled_` |
| `app/src/main/cpp/ivanna_route_a_asrc.cpp` | Passthrough bit-exacto en `process()` cuando `!enabled` |
| `app/src/main/cpp/jni/ivanna_route_a_asrc_jni.cpp` | JNI `nativeSetEnabled` |
| `app/src/main/java/com/ivanna/omega/audio/RouteAAsrc.kt` | Flag `asrcEnabled` (default OFF) + `isAvailable` + `init` salta `nativeCreate()` en bypass |
| `app/src/main/java/com/ivanna/omega/audio/IvannaBridgePlayer.kt` | PLL en bypass cuando el ASRC está en bypass (`setClockCorrectionPpm(0.0)`) |
| `app/src/main/cpp/tests/test_route_a_asrc_bypass.cpp` (NUEVO) | 4 tests host del bypass |
| `app/src/main/cpp/tests/CMakeLists.txt` | Registro del test (mismo patrón que `test_double_precision`) |
| `app/src/test/java/com/ivanna/omega/audio/RouteAAsrcBypassTest.kt` (NUEVO) | 3 tests Kotlin del flag |

## 5. Tests

**C++** `test_route_a_asrc_bypass` (4 tests): `EnabledByDefault`, `BypassIsBitExact` (memcmp entrada vs salida), `BypassRespectsOutCap`, `ToggleReactivates`.

**Kotlin** `RouteAAsrcBypassTest` (3 tests, con save/restore del flag global): default OFF, passthrough sin instancia nativa, reactivación con flag ON.

**Verificación en host:** bypass C++ 4/4, `test_double_precision` 4/4. **CTest completo: 1129/1129** (1125 existentes + 4 nuevos del bypass), 0 fallos, 68.97 s. Los tests existentes del ASRC siguen pasando — el código sigue ahí.

**Lecciones aplicadas:** no `IntArray?.orEmpty()`, no `AudioDescriptor`, no símbolos de companion object desde tests. Sin SDK en host: el Kotlin solo se verifica en CI (pendiente `compileReleaseKotlin` + `RouteAAsrcBypassTest` al mergear a main).

## 6. Verificación en vivo pendiente (tras publicar la 460)

- MÚSICA sin tronidos (o con tronido reducido): si el tronido persiste con el ASRC fuera del camino, el culpable es el NPE (0024) o los picos DSP — la prueba de apagar el NPE en la app sigue pendiente y es la siguiente en la lista.
- Telemetría esperada en bypass: `asrc=0`, `ppm=0.0`, `pll_occ` dentro de cap, `dsp_us` más bajo (sin el FIR 319 taps en el camino).
- La vía Qobuz/captura (Ruta B) no cambia de comportamiento por este bypass: su síntoma (volumen que baja + opaco) es el de 0032-ext, no del ASRC.

## 7. Follow-ups

1. Prueba NPE OFF en la app (escucha + `dsp_us`): si el tronido se va, el NPE es la causa principal y se decide su futuro (apagado por defecto, retuneo o reescritura).
2. Si el bypass confirma que el ASRC no aporta en este chip: valorar marcar el ASRC legacy definitivo en Ruta A y retirar el resampler Kotlin del camino caliente.
3. Medición en vivo 0022 (THD+N real IVANNA vs Files) con el bypass activo: el número cambia al salir el ASRC del camino.
4. Decisión de Esse pendiente: umbral CTest de la Puerta (1008 vs 1129 real) — bloquea la Puerta, no esta propuesta.
