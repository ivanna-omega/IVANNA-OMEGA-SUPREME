# Propuesta 0015 — pi-lstm-bridge-jni-test (Plan Omnipotencia)

**Hallazgo:** `pi_lstm_bridge_jni.cpp` (278 líneas) estaba entre los 19
módulos sin tests: bridge JNI de PiLstmBridge (Kotlin) con su propia
instancia PILSTMMilenioEngine. Sin cobertura, un cambio en el mailbox
de parámetros o en la telemetría no se detectaba hasta el teléfono.

## Diagnóstico (verificado, no inferido)

- El bridge es testeable en host: el engine es header-only
  (`neuromorphic/pi_lstm_milenio.hpp`) y existen stubs
  (`tests/host_stubs/jni.h`, `android/log.h`). Compila limpio en host.
- El entry point de audio es `ivanna_pilstm_bridge_tick_block` (C
  plano, no JNI): aplica los controles pendientes, avanza el LSTM RK4
  y publica la telemetría (np_sat, error) en atomics.
- Los setters clampan (alpha/beta/gamma 0.01–20, eta 0–5, np_max
  0.1–10, harmonic_gain 0–1) y son no-op antes de `nativeInit`.
  `NP_max` clamp a `h`/`c` y es el denominador de np_sat.

## El fix

`test_pi_lstm_bridge_jni.cpp` (nuevo, 6 tests, patrón
`test_phase2_jni_natives`): JNI con `JNIEnv{}` vacío.
1. **NotReady:** antes de init, getters devuelven 0 y los setters son
   no-op.
2. **Init+tick:** tras init + seno, np_sat y error finitos en [0,1].
3. **NP_max fluye al engine:** mismo input y reset — con NP_max=0.1 la
   saturación es MAYOR que con NP_max=10 (el setter llega al LSTM, no
   se queda en el mailbox).
4. **Clamps:** alpha NaN, beta 100, gamma -3, eta -5, np_max 0,
   harmonic_gain 5 → tras el tick la telemetría sigue finita y en
   rango.
5. **Tick nulo/cero:** punteros null y 0/-4 frames son no-op (no
   crashea, la telemetría no cambia).
6. **ResetTelemetry:** no crashea, la telemetría sigue finita.

Cableado en `tests/CMakeLists.txt` (patrón test_phase2_jni_natives,
con `host_stubs` en los includes). Conteo honesto 1038 → 1044 en
README + telemetría.

## Validación

- Build del target: OK.
- `ctest -R PiLstmBridgeJniTest`: **6/6 PASSED**.
- Suite CTest completa `-j4`: **100% tests passed, 0 failed out of 1044**
  (58.33 s).
- `check_docs_claims`: [PASS] 0 hallazgos.
- Scan módulos sin tests: **18 → 17**.
- Parche validado en scratch: 0013 + 0015 aplican limpio sobre HEAD;
  7/7 tests (6 PiLSTM + 1 ASRC) OK.
- Apply dry-run: exact.

## Dependencia

**0015 requiere 0013 aplicado primero** (ambos tocan
`tests/CMakeLists.txt` y `telemetry/ctest_summary.json` + `README.md`
para el conteo).

## Archivos

- `app/src/main/cpp/tests/test_pi_lstm_bridge_jni.cpp` — 6 tests nuevos.
- `app/src/main/cpp/tests/CMakeLists.txt` — cableado.
- `telemetry/ctest_summary.json` — 1044/1044.
- `README.md` — 1044/1044.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo.
