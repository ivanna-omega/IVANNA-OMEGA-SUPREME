# P10 — Gemelo Humano (Frente E, 2026-11-24)

P10 del plan `PLAN_P7_P10.md` §5. Módulo C++ standalone, sin cablear en
la app, sin tocar audio: solo banco de pruebas y modelo de sensibilidad.

## Qué se construyó (P10.1, estructura)

- `human_twin.hpp/.cpp` — banco de pruebas: las 8 pruebas breves de
  calibración (§5.2), cada una con su eje (§5.1, nunca mezclados);
  `TrialRecord` (estímulo, respuesta, tiempo, eje); `TrialLog` con
  serialización; `SensitivityModel` (P10.3, estructura + prior).
- `human_twin_test.cpp` — tests host en verde.

## Qué es prior y qué es medición (honesto, 2026-11-24)

- **Sin baseline, el modelo es azar:** `predict()` devuelve P(detecta)
  = 0.5, P(molesta) = 0.5, confianza 0, `calibrated=false`. No finge
  conocer a Esse.
- **`fit()` exige ≥20 ensayos del mismo tipo**; con menos se niega
  (fail-closed). Con suficientes datos usa la tasa empírica de acierto
  como estimador crudo (documentado: no es un ajuste psicométrico
  completo; P(molesta) = P(detecta) hasta tener datos de molestia
  separados).
- **El log rechaza ejes mezclados:** un registro cuyo eje no coincide
  con el eje de su prueba no se guarda (la prueba queda invalidada, no
  promediada).

## Bloqueado

- **En Esse (sus sesiones):** P10.2 baseline — 2 sesiones de ~45 min
  con la primera pasada de todas las pruebas; repetibilidad verificada
  re-haciendo 2 pruebas en la segunda sesión. Sin sus respuestas no hay
  umbrales de detección, ni preferencia tímbrica, ni tolerancia a
  latencia.
- **En P7/P8/P9:** P10.3 modelo de sensibilidad real (predice una
  prueba retenida mejor que azar con confianza calibrada) y P10.4
  cierre del bucle (el compilador P8 consulta al gemelo humano).
- **UI de pruebas en la app:** no construida aquí — exige tocar la app
  y este frente no cablea nada en ella.

## Cómo correr los tests

```sh
cmake -S app/src/main/cpp/fused/human_twin -B /tmp/p10_build \
      -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/p10_build
ctest --test-dir /tmp/p10_build --output-on-failure
```
