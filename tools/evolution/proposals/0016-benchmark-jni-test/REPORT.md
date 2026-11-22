# Propuesta 0016 — benchmark-jni-test (Plan Omnipotencia)

**Hallazgo:** `ivanna_benchmark_jni.cpp` (72 líneas) estaba entre los 19
módulos sin tests: una sola función JNI, `nativeMeasureRoundTripLatencyUs`,
que mide la cadena DSP real de la Ruta A in-process con un pulso.

## Diagnóstico (verificado, no inferido)

- La función mide `clock_gettime(CLOCK_MONOTONIC)` alrededor de la
  cadena real: ParametricEQ → Compressor → HarmonicExciter →
  StereoWidener → GainStage → SafetyLimiter (los 6 módulos de dsp/ del
  target ivanna_omega). Es la métrica honesta de la Ruta A.
- Los 6 módulos ya están en `ivanna_dsp_under_test`; el bridge solo
  necesita compilarse con el stub JNI de host. Compila limpio.
- Latencia medida en host 2026-11-16 (256 frames): ~94–120 µs.

## El fix

`test_benchmark_jni.cpp` (nuevo, 2 tests):
1. **Latencia positiva y razonable:** (0, 10000] µs. El umbral 10 ms
   es 100× el valor medido: guard de cordura, no benchmark de
   rendimiento (un test de timing ajustado fallaría bajo ctest -j4
   por contención de CPU — lección de test_route_a_asrc).
2. **Repetibilidad:** dos llamadas consecutivas en rango (la cadena
   corre de forma repetible, sin estado corrupto entre llamadas).

Cableado en `tests/CMakeLists.txt` (link `ivanna_dsp_under_test`).
Conteo honesto 1044 → 1046 en README + telemetría.

## Validación

- `ctest -R BenchmarkJniTest`: **2/2 PASSED**.
- Suite CTest completa `-j4`: **100% tests passed, 0 failed out of 1046**
  (58.21 s).
- `check_docs_claims`: [PASS] 0 hallazgos.
- Scan módulos sin tests: **17 → 16**.
- Parche validado en scratch: 0013+0015+0016 aplican limpio sobre
  HEAD; 2/2 tests OK.
- Apply dry-run: exact.

## Dependencia

**0016 requiere 0013 y 0015 aplicados primero** (todos tocan
`tests/CMakeLists.txt` + conteos).

## Archivos

- `app/src/main/cpp/tests/test_benchmark_jni.cpp` — 2 tests nuevos.
- `app/src/main/cpp/tests/CMakeLists.txt` — cableado.
- `telemetry/ctest_summary.json` — 1046/1046.
- `README.md` — 1046/1046.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo.
