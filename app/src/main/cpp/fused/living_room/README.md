# P9 — Sala Viva (Frente E, 2026-11-24)

P9 del plan `PLAN_P7_P10.md` §4. Módulo C++ standalone, sin cablear en
la app, sin tocar audio: solo simula.

## Qué se construyó (simulador en host)

- `living_room.hpp/.cpp` — modelo de sala del gemelo: volumen,
  frecuencia de Schroeder, distancia crítica, modos axiales por
  posición, Δsala por distancia del oyente y predicción del cambio de
  Δsala al mover al oyente 1 m (gate §4.3).
- Los 7 modos de la tabla del plan como **datos** (nombre, función
  objetivo, intervención), no como efectos activos.
- `living_room_test.cpp` — tests host con valores esperados calculados
  a mano.

## Números (verificados a mano en el test)

Sala 4.0 × 7.0 m, RT60 ~0.34 s, escucha a 3 m. Altura **nominal 2.5 m
(no medida)**: V = 70 m³, Schroeder ≈ 139.4 Hz, Dc ≈ 0.82 m — coinciden
con los valores del plan. Δsala: 11.60 dB a 3 m; mover 1 m alejándose
(3 → 4 m) predice **+2.37 dB**; acercándose (3 → 2 m) predice
**−3.16 dB**; a 1.5 m −5.20 dB y a 4.5 m +3.35 dB respecto a 3 m.

## Bloqueado

- **En P3 (gemelo ~0.46, sin calibrar):** la predicción del simulador
  debe coincidir con la medición de P3 dentro de tolerancia declarada
  (§4.3) — hoy no hay medición contra la que contrastar. La altura de
  la sala (2.5 m nominal) también la mide P3.
- **En Esse (escucha):** cada modo se prueba en su posición (3 m) y en
  segunda posición (1.5 m y 4.5 m); gate por modo: distinguirlo del
  modo Conservación en prueba ciega y describirlo en sus términos.
- **Límite honesto:** con un solo par de bocinas y un solo micrófono
  de teléfono, la Sala Viva es control de un punto de escucha, no de
  toda la sala. Arrays distribuidos y multipunto quedan fuera de
  alcance hasta que exista ese hardware.

## Cómo correr los tests

```sh
cmake -S app/src/main/cpp/fused/living_room -B /tmp/p9_build \
      -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/p9_build
ctest --test-dir /tmp/p9_build --output-on-failure
```

## Cableado (2026-11-24, rama corona-cableado)

- **En la app, como modelo/datos — NO como efecto activo:**
  `living_room.cpp` se compila en `libivanna_omega.so` y
  `libomega_effect.so` (`app/src/main/cpp/CMakeLists.txt`) y su test
  está registrado en la suite principal (`tests/CMakeLists.txt`,
  `test_living_room`).
- **Enganche:** `FusedChain::livingRoomDeltaDb()`,
  `predictLivingRoomDeltaChange()`, `livingRoomModeInfo()` y
  `livingRoomGeometry()` delegan en las funciones puras del módulo con
  la geometría por defecto. Ningún método toca audio y **ningún modo
  de sala se aplica en `process()`**: convertir un modo en efecto
  activo cambiaría el sonido sin ABX y queda explícitamente NO
  cableado hasta la escucha en 3 posiciones por modo (gate P9).
