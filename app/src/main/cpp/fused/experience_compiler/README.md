# P8 — Compilador de Experiencia (Frente E, 2026-11-24)

P8 del plan `PLAN_P7_P10.md` §3. Módulo C++ standalone, sin cablear en
la app, sin tocar audio: solo compila intenciones a parámetros.

## Qué se construyó (P8.1 + P8.2 + P8.3)

- `experience_compiler.hpp/.cpp` — `ExperienceVector` E = [I, D, S, T,
  M, F, A] en [0,1] + incertidumbre; `ReachableRegion`; compilador
  intención → amounts por operador + trim maestro.
- `experience_compiler_test.cpp` — tests host, incluido el gate P8.3:
  50 intenciones compilan sin violar 0.0 dB ni salir de [0,1].

## Qué es nominal y qué está medido (honesto, 2026-11-24)

- **Mapa E → operadores: NOMINAL** (diagonal I→ORBE, D→HOLOS,
  S→SALA_VIVA, T→VOLTERRA, M→ARMONÍA, F→PULSO, A→NEURO). Punto de
  partida por razonamiento, no calibración.
- **Coeficientes de ganancia por operador: NOMINALES** (ORBE +0.10,
  HOLOS −0.05, SALA_VIVA +0.08, VOLTERRA +0.12, ARMONÍA +0.06,
  PULSO −0.04, NEURO +0.02 dB por unidad de amount). El trim maestro
  los compensa exactamente: ganancia neta 0.0 dB **dentro de este modelo
  nominal**. La ganancia real de cada operador se mide en P2/P3.
- **Región alcanzable: NADA MEDIDO.** Los 7 componentes quedan
  `measured=false`; un objetivo en [0,1] se clasifica `Unknown` (no se
  puede probar alcanzabilidad ni negarla) y uno fuera de [0,1] es
  `Unreachable` con su punto más cercano. Cuando P3 dé los límites
  físicos y P4 los de control, los rangos se rellenan sin cambiar la
  interfaz.

## Bloqueado

- **En P3 (gemelo ~0.46, sin calibrar):** los límites físicos de la
  región alcanzable (P8.2 real).
- **En P4 (pesos sin calibrar):** el controlador que ejecuta las
  cantidades que el compilador propone.
- **En P6 (nominal):** la predicción perceptual que afina la región.
- **En Esse (escucha):** P8.4 reglas de adaptación (moverse 2 m sin
  percibir bombeo) y P8.5 intenciones predefinidas en prueba ciega.

## Cómo correr los tests

```sh
cmake -S app/src/main/cpp/fused/experience_compiler -B /tmp/p8_build \
      -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/p8_build
ctest --test-dir /tmp/p8_build --output-on-failure
```

Regla de Oro: ganancia neta 0.0 dB como restricción dura del compilador
(trim maestro; test verifica |neta| ≤ 1e-9 dB en las 50 intenciones).

## Cableado (2026-11-24, rama corona-cableado)

- **En la app, de verdad:** `experience_compiler.cpp` se compila en
  `libivanna_omega.so` y `libomega_effect.so` (`app/src/main/cpp/
  CMakeLists.txt`) y su test está registrado en la suite principal
  (`tests/CMakeLists.txt`, `test_experience_compiler`).
- **Enganche:** `FusedChain::compileExperience()` (puro, funciona con
  el flag en OFF o en ON) y `FusedChain::applyCompiledExperience()`,
  tras el flag `experienceCompilerEnabled` **default OFF** (mismo
  patrón que la clasificación adaptativa NEURO). Con OFF, aplicar es
  no-op y la cadena es bit-idéntica a antes (TEST
  `P8FlagOffProcessBitIdentical`); con ON aplica los 6 amounts con
  escalar en `FusedControls` + trim maestro recomputado sobre lo
  aplicado, ganancia neta nominal 0.0 dB (TEST
  `P8FlagOnAppliesWithZeroNetNominal`).
- **ARMONÍA NO aplicada:** `FusedControls` no tiene escalar de amount
  tonal; inventar uno falsearía la semántica. Pendiente de decisión
  de Esse (definir el escalar tonal o aceptar ARMONÍA fija en P8).
  Encender el flag cambia el sonido: exige ABX + decisión explícita.
