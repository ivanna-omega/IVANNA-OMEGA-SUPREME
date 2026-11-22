# Propuesta 0017 — saf-jni-bridge-test + scanner fix (Plan Omnipotencia)

**Hallazgo (doble):** `SaFJniBridge.cpp` (130 líneas) estaba entre los 19
módulos sin tests. Y al cablearlo, el scanner reveló un bug propio: mi
comentario en CMakeLists mencionaba `omega_effect.cpp` y el scanner
(substring ingenuo) lo daba por cableado — un falso negativo.

## Diagnóstico (verificado, no inferido)

### El bridge
- Bridge JNI de SaFBridge ↔ SaFOptimizer. Testeable en host: el stub
  JNI implementa NewFloatArray / SetFloatArrayRegion / GetStringUTFChars
  y el engine es SaFOptimizer.cpp + SafGlobalBridge.cpp (compilan limpio).
- `ivanna_saf_apply_latent` (definida en el efecto en producción) se
  sustituye por un stub en el test que captura el q propagado — es el
  cable SAF → ObjectRenderer (sin él, el optimizador convergía pero el
  audio nunca cambiaba).

### El bug del scanner
- `scan_untested_modules` hacía substring sobre CMakeLists.txt SIN
  quitar comentarios. `IvannaFusionCore.cpp` aparecía solo en comentarios
  (lista comentada + explicación de por qué no se enlaza en host) y el
  scanner lo daba por cableado. Con comentarios ignorados el conteo
  real es 16, no 15.

## El fix

1. **`test_saf_jni_bridge.cpp`** (6 tests): reset propaga q=0;
   feedback cambia q y lo propaga (el stub registra la llamada con el
   q actualizado); status = params + flag modelo (0 sin init); init
   con ruta inexistente → false; error/converged sanos; dirección de
   feedback inválida se ignora (iteración no avanza).
2. **`scan_untested_modules`:** quita los comentarios `#...` antes del
   substring (mismo patrón que check_header_wiring). 3 tests nuevos
   (`test_scan_untested_modules.py`): solo-comentario es sin tests,
   referencia en código es con tests, y el repo real incluye
   IvannaFusionCore.cpp.
3. Conteo honesto 1046 → 1052 en README + telemetría.

## Validación

- `ctest -R SaFJniBridgeTest`: **6/6 PASSED**.
- Suite CTest completa `-j4`: **100% tests passed, 0 failed out of 1052**
  (57.07 s).
- `check_docs_claims`: [PASS] 0 hallazgos.
- Scan módulos sin tests: **16 (veraz, antes 15 por el falso negativo)**
  — IvannaFusionCore.cpp revelado como realmente sin tests.
- Parche validado en scratch: 0007+0012+0013+0015+0016+0017 aplican
  limpio sobre HEAD; 6/6 SAF + 14 tests evolución OK.
- Apply dry-run: exact.

## Dependencias

**0017 requiere 0007, 0012, 0013, 0015 y 0016 aplicados primero**
(toca `evolution_engine.py` y `tests/CMakeLists.txt` + conteos).

## Archivos

- `app/src/main/cpp/tests/test_saf_jni_bridge.cpp` — 6 tests nuevos.
- `app/src/main/cpp/tests/CMakeLists.txt` — cableado.
- `tools/evolution/evolution_engine.py` — scanner ignora comentarios.
- `tools/evolution/tests/test_scan_untested_modules.py` — 3 tests nuevos.
- `telemetry/ctest_summary.json` — 1052/1052.
- `README.md` — 1052/1052.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo.
