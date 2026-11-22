# Propuesta 0013 — route-a-asrc-test-wiring (Plan Omnipotencia)

**Hallazgo:** `ivanna_route_a_asrc_test.cpp` (800 líneas, 14 grupos de
checks (a)-(n) del ASRC magistral Ruta A) vivía en `app/src/main/cpp/`
SIN referencia en `tests/CMakeLists.txt` — existía pero NUNCA corría
en CTest/CI. Es el primer módulo de los 19 sin tests (scan
`scan_untested_modules`).

## Diagnóstico (verificado, no inferido)

- El test es un ejecutable standalone con su propio `main()` (no
  gtest): tasa, THD+N < −90 dB, continuidad entre bloques, fail-closed,
  Ψ viva, coste por bloque. Compila solo con g++ de host.
- Compila y pasa standalone: "TODOS LOS TESTS PASARON" (verificado
  antes de tocar CMakeLists).
- `ivanna_route_a_asrc.cpp` NO está en `ivanna_dsp_under_test` (por eso
  `test_stage_timings` lo compila directo); el test solo necesita esos
  dos archivos + los include dirs de `test_stage_timings`
  (`fir_v5_coeffs.h` en hal_integration, `psi_core.h` en include/).

## El fix

1. **Cableado en `tests/CMakeLists.txt`** (patrón `test_stage_timings`
   / `test_adaptive_engine`): `add_executable` con los dos .cpp,
   mismos include dirs, `add_test`. Sin `FAIL_REGULAR_EXPRESSION`
   (bajo ctest -j4 el regex compartido "FALLARON" marcaba fallos de
   OTRO binario — lección documentada en test_adaptive_engine).
2. **`RUN_SERIAL TRUE`:** la sección (n) mide COSTE por bloque (umbral
   500 µs) y es sensible a contención de CPU. Verificado: bajo
   `ctest -j4` el coste medido sube a 874/991 µs (vs 333/376 µs solo)
   y 2 checks fallan por timing, no por regresión. Con RUN_SERIAL el
   test corre sin competir por CPU.
3. **Conteo honesto 1037 → 1038:** README.md y
   `telemetry/ctest_summary.json` actualizados al total real (regla de
   Esse: nunca fabricar métricas; 0008 exige README = telemetría =
   realidad).

## Validación

- Build del target: OK.
- `ctest -R test_route_a_asrc`: PASSED (4.5 s).
- Suite CTest completa con `-j4`: **100% tests passed, 0 failed out
  of 1038** (56.91 s).
- `check_docs_claims`: [PASS] 0 hallazgos.
- Scan: módulos sin tests **19 → 18**.

## Archivos

- `app/src/main/cpp/tests/CMakeLists.txt` — cableado + RUN_SERIAL.
- `telemetry/ctest_summary.json` — 1038/1038.
- `README.md` — 1038/1038.

**Autoría:** coordinador del Plan Omnipotencia, vía directa
(2026-11-16). Sin consejo.
