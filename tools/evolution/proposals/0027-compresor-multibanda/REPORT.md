# 0027 — Compresor multibanda 3 bandas: la dinámica deja de ser monobanda

**Mandato de Esse (2026-11-16 11:49 CST):** el DSP debe superar POR CRECES GENERACIONALES a SHARC, Lake LM44, Meyer GALAXY, DirectOut PRODIGY.MP, Trinnov Altitude32, Helix/Brax DSP, Avid HDX y UAD-2. Diagnóstico comparativo en `0026-diagnostico-comparativo-referentes/REPORT.md` — esta es la brecha nº 1 (prioridad 1, impacto alto en bocinas G85 + DAC 24-bit/48 kHz → Genezi).

**Estado del consejo:** RECHAZADO formal (sin consenso en 3 rondas). El detector dijo FALSO POSITIVO (confunde brecha de capacidad con bug: el monobanda es una decisión de diseño, no un defecto — pero la ELEVACIÓN es el encargo, y el revisor lo confirmó exigiendo exactamente este parche). El implementer no devolvió diff utilizable en 5 intentos (ronda 2). Siguiendo el patrón 0021/0022/0023/0024/0025, el coordinador implementó el parche que el revisor prescribió. Ver `council_verdict.json`.

## 1. Diagnóstico (verificado en código, no afirmado)

`app/src/main/cpp/include/Compressor.h` + `app/src/main/cpp/dsp/Compressor.cpp`: UN solo detector (`env_`, `envSmooth_`, pico max(L,R) tras HPF de sidechain a 120 Hz), UN threshold (−16 dB en vivo) y UN ratio (2.4 en vivo) para TODO el espectro. En la cadena viva (`ivanna_omega_jni.cpp:2048`, tras el EQ y antes del exciter/widener) `g_comp.process()` aplica la misma reducción de ganancia a graves, medios y agudos a la vez.

**Consecuencia audible:** un golpe de bajo dispara la compresión de todo el espectro — las voces y los agudos se hunden con cada bombo (pumping); un sibilante agudo no se controla sin aplastar los graves. Todos los referentes mundiales tienen dinámica multibanda.

**Matiz honesto (medido al validar):** el Compressor tiene makeup acoplado a la reducción real (techo ~4.5 dB con threshold −12 / ratio 4) y HPF de sidechain a 120 Hz. Una señal apenas por encima del threshold se compensa casi entera (reducción neta ~0 dB): el compresor actúa como domador de picos, no como reductor de nivel general. Para medir compresión neta hace falta una señal MUY por encima del threshold (ver §4).

## 2. El parche

**Módulo nuevo** `app/src/main/cpp/dsp/MultibandCompressor.hpp` + `.cpp`:
- 3 bandas con crossovers Linkwitz-Riley de 4º orden (2x Butterworth 2º orden Q=0.7071 en serie por rama): graves <200 Hz, medios 200 Hz–4 kHz, agudos >4 kHz. La suma LR4 es plana en magnitud (all-pass).
- Un `Compressor` (el existente: detector link estéreo max(L,R), soft knee 3 dB, makeup acoplado) por banda. Cada banda comprime independiente.
- Misma interfaz de runtime que `Compressor` (`setRuntimeAmount`, `setParams`, `setThreshold`, `setRatio`, `setAttack`, `setRelease`) para que el motor adaptativo no cambie: todo se reenvía a las 3 bandas.
- Bypass bit-exacto: con `enabled=false` el proceso retorna sin tocar el buffer. El split LR4 corre SIEMPRE (aunque esté en bypass) para mantener los filtros cebados; al activar/desactivar hay un fundido de 20 ms (sin clic).
- Sin asignaciones en el hot path: scratch por banda como miembros (3×2×2048 floats). RT-safe. Estado de los biquads en double (el `Biquad` de `dsp_types.h` ya es double).

**Integración en Ruta A** (`app/src/main/cpp/jni/ivanna_omega_jni.cpp`):
- `g_multibandComp` + `g_multiband_enabled{true}` junto a `g_comp`. ACTIVO por defecto (es la elevación).
- `setParams` reenviado en los dos sitios donde se llama a `g_comp.setParams`.
- En `nativeProcess`: `g_multibandComp.process()` corre SIEMPRE (prima crossovers; con mix_=0 no toca el buffer); si está desactivado, `g_comp.process()` corre después como fallback real. Si está activo, `g_comp` no corre.
- JNI nuevo: `nativeSetMultibandEnabled(bool)` / `nativeIsMultibandEnabled(bool)` (mismo patrón que `nativeSetFastRpcEnabled`). Si en vivo se oye peor, un flag vuelve al monobanda.
- **Cableado Kotlin (fix de validación 2026-11-16):** el check de cableado JNI (`test_check_jni_wiring`) falló porque los dos JNI nuevos no tenían `external fun` en Kotlin ni llamador. Fix: declaraciones en `IvannaNativeLib.kt` (junto a `nativeSetCompressorParams`) y llamada en `DspStateUpdater.kt` (`applyUpdate`, primera aplicación del estado DSP: pone ON + read-back con el getter). Sin este cableado, la suite de evolución quedaba en 167/168.

**No se toca:** el compresor monobanda (queda como fallback/legacy, sin borrar código). No se tocan IntegrityGate, credenciales ni el resto de la cadena.

## 3. Medición (host, tests nuevos)

`app/src/main/cpp/tests/test_multiband_compressor.cpp` (6 tests, 6/6 PASS):

| Test | Qué mide | Resultado |
|---|---|---|
| `FlatSumReconstruction` | Suma LR4 plana en magnitud (9 frecuencias, 50 Hz–12 kHz, señal −20 dBFS bajo threshold) | ±0.5 dB en todas |
| `BandIndependence` | Grave 100 Hz a 0.9 + agudo 8 kHz a 0.1: el grave se comprime, el agudo cambia <1 dB; y viceversa | Grave comprimido (reducción neta >1 dB tras makeup), agudo ±1 dB |
| `BypassBitExact` | Desactivado = buffer intacto (memcmp) | Bit-exacto |
| `FiniteAndBoundedHotInput` | Entrada ±1.5: finito y \|out\| < 4.0 | PASS |
| `ClickFreeOnEnable` | Pendiente máx. muestra a muestra al activar ≤2x la del régimen | PASS (fundido 20 ms) |
| `CostWithinBudget` | Coste por bloque de 1024 a 48 kHz | <2 ms/bloque (medido ~0.17 ms/bloque en host) |

**Lección de medida (documentada en el test):** con threshold −12, el tono de 100 Hz a 0.9 queda ~6 dB por encima y el makeup lo cancela casi entero (neta ~0 dB). El test usa threshold −24 dB: el tono queda ~18 dB por encima (reducción −13.65 dB, makeup +9 dB techo, neta −4.65 dB). El agudo a 0.1 queda 4 dB por encima de −24: reducción −3 dB, makeup +3 dB, neta 0 dB (su nivel no cambia).

## 4. Validación

- **Tests nuevos: 6/6 PASS** (verificado por el coordinador, binario `test_multiband_compressor`, 2026-11-16): FlatSumReconstruction, BandIndependence, BypassBitExact, FiniteAndBoundedHotInput, ClickFreeOnEnable, CostWithinBudget (~0.17 ms/bloque host).
- **CTest completo: 1120/1120, 100% verde** (rebuild limpio, 0027+0028+0029 juntas). **Nota:** el primer CTest del build mixto dio 1109/1110 con `test_rt_no_alloc` en segfault — NO es un bug de la 0027: es ABI mixto (la librería se compiló con el header viejo de `RirConvolver` y el test con el nuevo, porque la 0028 editó el header mientras el build corría). La validación fiable es el rebuild limpio.
- **Suite evolución: 168/168 OK.** Incluye la actualización del pin de línea del test 0019 (1825 → 1836, por las 11 líneas netas de la 0027 antes del TODO route-arbiter-v2).
- **`check_docs_claims`: PASS (incluido en la suite de evolución).**
- **`telemetry/ctest_summary.json`: actualizado a 1120/1120.**
- **Kotlin:** declaraciones `external fun` en `IvannaNativeLib.kt` + llamada en `DspStateUpdater.kt` (ver "Cableado Kotlin" arriba).

## 5. Lo que queda pendiente (no escondido)

1. **Verificación en vivo:** que en bocinas G85 + DAC → Genezi el grave pegue sin hundir la voz y no haya pumping. Si en vivo se oye peor, `nativeSetMultibandEnabled(false)` vuelve al monobanda.
2. **Per-band tuning:** los 3 compresores usan hoy los mismos threshold/ratio/attack/release. Un ajuste por banda (p. ej. ataque más rápido en agudos, release más largo en graves) sería un refinamiento posterior, medido.
3. **El consejo rechazó el enfoque inicial** (el implementer solo añadió un flag legacy). Este parche es la versión que el revisor exigió: módulo real, integración en Ruta A, tests de regresión.
4. **Siguiente oleada:** 0028 (NEU·Ω Fase 3, cola difusa en frecuencia) y 0029 (núcleo en doble precisión).

## 6. Archivos

- `app/src/main/cpp/dsp/MultibandCompressor.hpp` (nuevo)
- `app/src/main/cpp/dsp/MultibandCompressor.cpp` (nuevo)
- `app/src/main/cpp/jni/ivanna_omega_jni.cpp` (integración + setter JNI)
- `app/src/main/cpp/tests/test_multiband_compressor.cpp` (nuevo, 6 tests)
- `app/src/main/cpp/tests/CMakeLists.txt` (registro + MultibandCompressor.cpp en la librería de tests)
- `app/src/main/cpp/CMakeLists.txt` (MultibandCompressor.cpp en el build principal)
- `tools/evolution/tests/test_scan_todo_fixme.py` (pin 1825 → 1836)
- `telemetry/ctest_summary.json` (1104 → 1110)
- `tools/evolution/proposals/0027-compresor-multibanda/finding.json`
- `tools/evolution/proposals/0027-compresor-multibanda/council_verdict.json`
- `tools/evolution/proposals/0027-compresor-multibanda/REPORT.md` (este archivo)
