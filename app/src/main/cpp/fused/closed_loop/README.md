# fused/closed_loop/ — Control cerrado perceptual (plan del salto, Luna)

Meta: convertir IVANNA en un sistema de renderizado acústico
perceptualmente cerrado, autoidentificable y consciente de su
incertidumbre. Un controlador universal del trayecto
señal–transductor–sala–oyente.

## Mapa de módulos (Luna, M0–M8)

| Módulo | Archivo(s) | Dueño | Estado |
|---|---|---|---|
| M0 Contrato temporal | `audio_state.hpp` (+ `audio_state.cpp`) | salto/m0-contrato | implementado 2026-11-21 (6 tests) |
| M3 Gemelo estimador | `twin_confidence.hpp` + `twin_estimator.hpp/.cpp` (extiende `../acoustic_twin/`) + `tests/test_twin_estimator.cpp` | salto/m3-gemelo | implementado 2026-11-21 (7 tests) |
| M5 Simulador de acciones | `action_simulator.hpp/.cpp` (extiende `../perceptual_mpc/operator_simulator`) | salto/m5-simulador | implementado 2026-11-21 (16 tests) |
| M6 MPC en sombra | `mpc_shadow.hpp/.cpp` (extiende `../perceptual_mpc/mpc_controller`) | salto/m6-mpc-sombra | implementado 2026-11-21 (9 tests, en sombra) |
| M8 Supervisor de incertidumbre | `uncertainty_supervisor.hpp/.cpp` | salto/m8-supervisor | implementado 2026-11-21 (11 tests) |
| Telemetría | `telemetry.hpp/.cpp` | salto/telemetria | implementado (anillo lock-free, sobrescribe el más viejo) |
| Banco adversarial | `tests/test_closed_loop_adversarial*.cpp` | salto/adversarial | implementado 2026-11-21 (119 tests) |

M1 (identificación de entrada), M2 (cadena DSP), M4 (vector perceptual,
ya en `../perceptual_mpc/perceptual_vector.hpp`) y M7 (identificador
automático por firma) NO se tocan en esta fase: M4 ya existe, M1/M2 son la
cadena actual, y M7 exige los datos de las Órdenes 0–2 (pendientes de
medición real).

## Contratos compartidos (NO se cambian sin coordinar)

- `audio_state.hpp` — `AudioState` + `AudioStatePublisher` (seqlock).
  Ruta A y B consumen el MISMO estado. Si generation, routeId o
  calibrationId no coinciden: bypass seguro.
- `twin_confidence.hpp` — `ParameterConfidence`, `TwinConfidence`,
  `InterventionLevel`, umbrales NOMINALES 0.80 / 0.50 / 0.20.
- `../perceptual_mpc/perceptual_vector.hpp` — `PerceptualVector`,
  `PerceptualField` (9 dims, 8 bandas), `Action` (7 operadores).
  Ya existe; se reutiliza, no se duplica.
- `../perceptual_mpc/operator_simulator.hpp` — simulador lineal W.
  M5 lo extiende con la función de costo J y las restricciones duras.
- `../acoustic_twin/acoustic_twin.hpp` — el gemelo. M3 lo extiende con
  confianza por variable; no lo reescribe.

## Reglas duras (Luna)

- Fail-closed siempre. Sin medición ni confianza, un módulo NO controla:
  solo telemetría. MPC en sombra hasta que sus pesos se calibren con ABX.
- El gemelo 0.70 estimado SOLO gobierna la intensidad máxima de
  conservación. NUNCA EQ fino, fase, Volterra ni sala.
- Sala: SOLO cut-only (`G_room(f) = min(0, T(f) - M(f))`). Nunca boost
  de valles. El módulo de sala nace DESACTIVADO hasta tener IR medida.
- Restricciones duras de M5: true peak <= -1.0 dBTP, THD +<= 1 dB,
  ruido +<= 1 dB, rate <= 0.20/ciclo, EQ boost <= 0 dB (sala),
  ganancia neta de sala <= 0 dB, latencia +<= 1 ms, fase <= límite.
- M6: confianza < 0.20, NaN, clipping o divergencia -> acción cero.
  Si el modelo predice mejora pero la medición contradice 2 veces ->
  rollback. Crossfade equal-power 20–50 ms al aplicar una acción.
- Hilo de audio: sin malloc, sin locks, sin I/O. MPC y análisis fuera
  del callback. Parámetros por snapshot atómico. CPU < 70%.
- Nunca fabricar métricas. Los pesos W del simulador son NOMINALES hasta
  que se calibren con mediciones y ABX (Orden 6).

## Cómo se construye y verifica

Vía canónica (desde la raíz del repo):

```bash
bash scripts/run_ctest.sh
```

Equivalente manual:

```bash
cmake -S app/src/main/cpp/tests -B build/tests-host -DCMAKE_BUILD_TYPE=Release
cmake --build build/tests-host -j
ctest --test-dir build/tests-host --output-on-failure
```

Cada módulo registra su test en `app/src/main/cpp/tests/CMakeLists.txt`
(siguiendo el patrón existente: añadir su .cpp a
`ivanna_dsp_under_test` y su ejecutable + `add_test`). La suite debe
quedar VERDE antes de dar un módulo por terminado. El umbral de la
Puerta (`tools/evolution/perfection_gate.py` + `test_umbrales_versionados`)
NO se toca en las ramas de módulo: lo actualiza el coordinador al
integrar, con el total real de tests.

## Lo que NO se hace aquí

- No se toca main, no se pushea a main. La rama de integración es
  `salto/control-cerrado`; cada módulo trabaja en su propia rama.
- No se instala nada en el teléfono, no se reinicia nada, no se toca
  audioserver. Los pasos 6 (barrido Genezi), 8 (ABX) y 9 (IR de sala)
  necesitan a Esse: sus procedimientos están en el documento de datos.
- No se activa ningún control nuevo en el teléfono. Todo lo de esta
  fase es host-side (C++ + tests). La activación en el teléfono es
  trabajo futuro, con medición real primero.
