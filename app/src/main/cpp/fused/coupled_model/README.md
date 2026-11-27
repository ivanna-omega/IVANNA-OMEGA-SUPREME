# Modelo acoplado no lineal — código D10 / Fase 3

Herramienta OFFLINE de predicción + correcciones identificadas para
tiempo real. Todo en `double` (IEEE 754 binary64), determinista, sin
I/O en el núcleo (el I/O vive solo en `coupled_predict.cpp`).

## Archivos

| Archivo | Qué es |
|---|---|
| `coupled_model.hpp` | El modelo: modos de sala, Z_rad, Westervelt, Jiles-Atherton, integrador exponencial exacto, banda coclear ERB, simulador acoplado RK4 (offline), energía modal re-derivada, correcciones RT. Header-only. |
| `coupled_predict.cpp` | CLI offline: predicciones P-A…P-D, curva Δφ (CSV), simulación con volcado de p_l(t) (CSV). |
| `build_offline.sh` | Compila la CLI y corre un humo rápido. |
| `../tests/test_coupled_model.cpp` | 12 tests GTest (ver `CODIGO_D10.md` en `~/workspace/modelo-acoplado-no-lineal/`). |

## Compilar y correr (host, cero gasto)

```sh
cd app/src/main/cpp/fused/coupled_model
./build_offline.sh            # compila con g++-14 -O2 -std=c++17
./coupled_predict             # predicciones P-A..P-D
./coupled_predict pa > pa.csv # curva Δφ 40–140 Hz
./coupled_predict run 100 1 1 > run_100Hz.csv   # seno 100 Hz, 1 V, 1 s
./coupled_predict rundec 100 1 1 > dec_100Hz.csv # control DESACOPLADO (R-G)
```

Los tests GTest van por el CMake de `app/src/main/cpp/tests`
(`test_coupled_model`, registrado con `ivanna_add_test`).

## Reglas

- El modelo completo es OFFLINE. En el hilo de audio solo van las tres
  correcciones RT de `coupled_model.hpp` (§RT): realimentación modal
  truncada (≤10 modos), estimador Westervelt y trim J-A. Medido:
  ~4–30 µs/hop frente a 1330 µs de presupuesto.
- f_s offline = 192 kHz por defecto (aliasing no lineal, F2 §3.3).
  A 48 kHz los términos no lineales exigen banda limitada < 8 kHz.
- Campo modal válido solo f < 139.4 Hz (35 modos). P-B (1–4 kHz) y P-D
  proceden porque no usan la base modal. Fase 2-bis (régimen difuso)
  queda fuera de este código, documentada en `CODIGO_D10.md`.
- Parámetros del Genezi NO identificados: van marcados UNIDENTIFIED en
  el header con su rango supuesto. Ningún número de predicción es un
  dato medido hasta la identificación en hardware (Fase 3).
