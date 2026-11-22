# Gemelo acústico del trayecto completo

Módulo C++17 nuevo e independiente (`app/src/main/cpp/fused/acoustic_twin/`).
Próximo salto magistral, parte 2 (plan de luna, 2026-11-21). No toca
FusedChain, JNI, UI, daemon ni el módulo `perceptual_mpc`.

## Qué hace

Estima el trayecto físico completo — fuente → DSP → mezcla → DAC →
amplificador → transductor → sala → oído — en 9 bandas logarítmicas
(63 Hz–16 kHz). Por banda devuelve: ganancia efectiva, latencia
(buffer + aire), rango dinámico usable, THD estimado, ruido ambiente,
RT60 (Sabine), coherencia (directo/reverberante), compresión estimada,
SPL máximo y confianza.

**Solo estima.** Es una función pura: sin estado global, sin I/O, sin
tocar audio. Determinista (misma entrada → misma salida bit a bit).

## Física (modelos, no mediciones)

- Sabine: `RT60 = 0.161·V/A`; la absorción media se deriva del RT60
  medido y se reparte por banda con un factor nominal.
- Ganancia en escucha: `10·log10(1/d² + 4/A)` (directo + reverberante).
- Distancia crítica `Dc = 0.057·√(V/RT60)`; coherencia `1/(1+(d/Dc)²)`.
- Schroeder `fs = 2000·√(RT60/V)`: por debajo, la sala es modal y la
  confianza baja.
- THD y compresión por margen de potencia a cada SPL (modelos suaves
  documentados en el `.cpp`).
- Confianza: penalizaciones por lo que no se sabe (potencia y
  sensibilidad desconocidas, RT60 inconsistente, distancia fuera de
  sala, SPL inalcanzable) y por la región modal.
- **`interventionScale = confidence`**: a confianza baja se interviene
  MENOS. Esto corrige el diseño de ivanna-free, que lo tenía invertido
  (`1 - confidence`).

## Trayecto de Esse (preset, no hardcode)

`AcousticTwinParams::esseGenezi()`: DAC USB JieLi 24-bit/48 kHz, Sony
Genezi MHC-PZ1D (estéreo, flat, sub ON neutro, surround OFF), sala
4.0×7.0 m, escucha a 3 m, RT60 0.34 s. La potencia del Genezi y la
sensibilidad de sus bocinas NO están medidas: quedan en 0
(desconocidas) y la confianza lo refleja (~0.46). `esseHeadphones()` es
el mismo DAC con audífono (sin sala, sin aire).

## Archivos

- `acoustic_twin.hpp` — API, structs, presets.
- `acoustic_twin.cpp` — implementación (física + confianza).
- `acoustic_twin_test.cpp` — tests host standalone (propio `main()`).
- `CMakeLists.txt` — build + ctest standalone.
- `README.md` — este archivo.

## Cómo correr los tests

```sh
cmake -S app/src/main/cpp/fused/acoustic_twin -B /tmp/acoustic_twin_build \
      -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/acoustic_twin_build
ctest --test-dir /tmp/acoustic_twin_build --output-on-failure
```

## Pendiente (no es de este módulo)

- Enganchar `test_acoustic_twin` a la suite host principal
  (`tests/CMakeLists.txt`): lo hace el dueño del flanco Tests host.
- Cablear la estimación al MPC / a la conservación perceptual: eso lo
  decide el dueño de esos módulos (aquí solo se expone la API).
- Medir la potencia del Genezi y la sensibilidad de sus bocinas para
  subir la confianza del preset de Esse (requiere medición en vivo).
