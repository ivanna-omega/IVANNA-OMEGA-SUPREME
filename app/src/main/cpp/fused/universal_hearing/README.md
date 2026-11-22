# Modelo Universal de Audición (P6)

Módulo C++17 nuevo e independiente
(`app/src/main/cpp/fused/universal_hearing/`). P6 de la visión de luna
("Majestuosidad Total", 2026-11-21). No toca FusedChain, JNI, UI, daemon,
ni los módulos `acoustic_twin` ni `perceptual_mpc` (no los incluye: misma
semántica por construcción, para que un puente futuro mapee 1:1).

## Qué hace

Predice la experiencia auditiva como

```
P(experiencia | x, H, R, L, C)
```

donde x = señal, H = cadena/hardware, R = recinto, L = oyente,
C = contexto y estado perceptual. Por banda (9 bandas logarítmicas,
63 Hz–16 kHz) devuelve un estado de 16 dimensiones en [0,1]: las 9 de
P[t] del MPC en el mismo orden y semántica, más 7 pérdidas nuevas de P6.

**Solo predice.** Es una función pura: sin estado global, sin I/O, sin
tocar audio. Determinista (misma entrada → misma salida bit a bit).

## Las 16 dimensiones

| # | Dimensión | Sentido |
|---|-----------|---------|
| 0 | Audibilidad | 1 = pleno |
| 1 | Claridad | 1 = cristalina |
| 2 | Naturalidad | 1 = natural |
| 3 | Impacto | 1 = máxima pegada |
| 4 | Espacialidad | 1 = envolvente |
| 5 | Fatiga | 0 = sin fatiga (¡es la fatiga de P[t], no se duplica!) |
| 6 | Estabilidad | 1 = roca |
| 7 | Confianza | 1 = segura |
| 8 | Presupuesto | 0 = libre |
| 9 | Pérdida de profundidad | 0 = sin pérdida |
| 10 | Compresión espacial | 0 = sin pérdida |
| 11 | Enmascaramiento tímbrico | 0 = sin pérdida |
| 12 | Pérdida de inteligibilidad | 0 = sin pérdida |
| 13 | Desplazamiento de imagen | 0 = sin pérdida (30° = 1.0) |
| 14 | Pérdida de microdinámica | 0 = sin pérdida |
| 15 | Pérdida de causalidad gesto-respuesta | 0 = sin pérdida |

Las 9 dims de P[t] se DERIVAN de las pérdidas (audibilidad del nivel y
el enmascaramiento; claridad de inteligibilidad y enmascaramiento;
naturalidad de THD y enmascaramiento; impacto de transitorios, crest y
nivel; espacialidad de profundidad y compresión; estabilidad de
desplazamiento de imagen y movimiento de cabeza; confianza del modelo;
presupuesto 0 — el modelo predice, no consume). La 9ª banda (16 kHz)
no tiene contraparte en el MPC (8 bandas): en un puente futuro se
mapean 1:1 las 8 primeras, igual que hace `twin_bridge.hpp`.

## Modelos (todos NOMINALES, no medidos)

Constantes de calibración inicial por razonamiento. Dan órdenes de
magnitud y tendencias con la monotonía correcta; NO son valores
calibrados del Genezi ni de la sala de Esse. Cada fórmula está
documentada en el `.cpp`.

- **Profundidad:** `0.55·RT60norm + 0.45·(1−coherencia)`.
  RT60norm = RT60banda/1 s. Crece con RT60 y con d/Dc.
- **Compresión espacial:** `0.40·RT60norm + 0.35·(1−coherencia)
  + 0.25·(|azimut|/90°)`.
- **Enmascaramiento tímbrico:** `0.40·RT60norm·pesoGrave
  + 0.25·nivel + 0.20·ruidoAmbiente + 0.15·THDnorm`.
  Referencias: nivel 0 dBFS = 1 (−60 dBFS = 0); ambiente 20 dB SPL = 0,
  60 dB SPL = 1; THD 5 % = 1. Peso grave 1.0 a 63–125 Hz → 0.3 en agudos.
- **Fatiga (dim 5):** `fatigaPrevia + exposición·nivelSobre85·
  (0.5+0.5·nivelBanda)·(1+0.5·THDnorm) + 0.10·(sesión/240 min)`.
  Referencias: 120 min de exposición = 1; 85 dB SPL = 0 (nivel seguro),
  105 dB = 1. La sesión larga suma hasta 0.10 aunque el nivel sea seguro.
- **Inteligibilidad:** `factorContenido·(0.50·RT60medios
  + 0.30·ruidoAmbiente + 0.20·(1−coherencia))`. RT60medios con pico en
  500 Hz–2 kHz; voz ×1.2, mixto ×1.1, música ×1.0.
- **Desplazamiento de imagen:** `|azimut| + desbalance·5°/dB`
  (NOMINAL: 1 dB ≈ 5°). Normalizado: 30° = 1.0. También en grados
  (`imageShiftDegrees`).
- **Microdinámica:** `0.50·(1−crestNorm) + 0.30·ruidoPiso + 0.20·THDnorm`.
  Referencias: crest 20 dB = plena; piso −120 dBFS = 0, −60 dBFS = 1.
- **Causalidad:** `0.60·latenciaNorm + 0.40·RT60norm`. Latencia total =
  buffer + aire (d/343); 50 ms = pérdida plena. El RT60 aporta smearing.
- **RT60 por banda:** `RT60/alphaFactor`, mismo factor nominal que el
  gemelo acústico (absorción sube con la frecuencia).

## Confianza

Base 1.0 menos penalizaciones nominales por lo que NO se sabe:

| Lo desconocido / inconsistente | Penalización |
|---|---|
| THD desconocido | −0.10 |
| Piso de ruido desconocido | −0.05 |
| Crest desconocido | −0.05 |
| Transitorios desconocidos | −0.05 |
| Tonal desconocido | −0.03 |
| Exposición desconocida | −0.05 |
| Ambiente desconocido (altavoz) | −0.05 |
| Coherencia derivada (no medida) | −0.05 |
| Distancia crítica derivada (no medida) | −0.05 |
| Sample rate no estándar | −0.05 |
| Bits no 16/24/32 | −0.05 |
| RT60 > 2 s | −0.20 |
| Volumen < 5 o > 10000 m³ | −0.20 |
| SPL fuera de [40, 120] | −0.10 |
| Desbalance > 3 dB | −0.10 |

Por banda (solo altavoz): ×0.85 por debajo de Schroeder
(`fs = 2000·√(RT60/V)`), donde la sala es modal. **`interventionScale
= confidence`**: a confianza baja se interviene MENOS (igual que el
gemelo, nunca al revés).

**El trayecto de Esse queda en confianza ~0.73** (base 0.75: THD y piso
sin medir, Dc y coherencia derivadas; media 0.725 con la penalización
modal en 63/125 Hz). Es MAYOR que la del gemelo (~0.46) por una razón
real, no por optimismo: el gemelo necesita potencia y sensibilidad para
ESTIMAR el THD (y ambas están sin medir); este módulo RECIBE el THD
como entrada, así que no lo penaliza la potencia sin medir — lo
penaliza el THD sin medir (−0.10) y el piso sin medir (−0.05).

## Contrato fail-closed

`valid=false` significa "sin predicción": confianza 0,
`interventionScale` 0 y TODAS las predicciones en 0.0 (definido, nunca
NaN/inf). **El consumidor DEBE checar `valid`**: un 0.0 en una
predicción inválida significa "no sé", no "pérdida cero".

Se invalida con: NaN/inf en cualquier entrada, valor fuera de rango,
o dato REQUERIDO desconocido: nivel de señal, SPL objetivo y, en
altavoz, RT60, volumen de sala y distancia del oyente. En audífono la
sala no se requiere (se ignora por completo) y una entrada por defecto
(todo desconocido) es inválida por construcción.

Un dato OPCIONAL desconocido aporta 0 a su término y baja la
confianza: la predicción es entonces un PISO, no un valor central.
Si la distancia crítica o la coherencia no se dan pero sí volumen,
RT60 y distancia, se DERIVAN con las fórmulas del gemelo
(`Dc = 0.057·√(V/RT60)`, `coherencia = 1/(1+(d/Dc)²)`) y penalizan la
confianza (−0.05 cada una): derivado, no inventado.

## Correspondencia con el gemelo (para un puente futuro, SIN acoplar)

| Entrada de este módulo | Campo del gemelo que la alimentaría |
|---|---|
| `ChainDesc.thdPercent` | `AcousticTwinBand.thdPercent` (0 = no estimable → desconocido aquí) |
| `ChainDesc.noiseFloorDb` | Derivado de `AcousticTwinBand.noiseDbSpl` menos SPL (no directo; hoy desconocido) |
| `ChainDesc.bufferLatencyMs` | `AcousticTwinEstimate.bufferLatencyMs` |
| `RoomDesc.rt60Sec` | `AcousticTwinBand.rt60Sec` (1 kHz) |
| `RoomDesc.roomVolumeM3` | `AcousticTwinEstimate.roomVolumeM3` |
| `RoomDesc.criticalDistanceM` | `AcousticTwinEstimate.criticalDistanceM` |
| `RoomDesc.coherence` | `AcousticTwinBand.coherence` (1 kHz) |
| `RoomDesc.ambientNoiseDbSpl` | Media de `AcousticTwinBand.noiseDbSpl` (aprox.) |
| `ListenerDesc.distanceM` | `AcousticTwinParams.listenerDistanceM` |
| `SignalDesc.rmsDbPerBand` | Medido del stream (no lo da el gemelo) |
| `ContextDesc.targetSplDb` | `AcousticTwinParams.targetSplDb` |

El puente futuro debe ser fail-closed estilo `twin_bridge.hpp`:
gemelo inválido → no se toca nada; y debe multiplicar la confianza de
P[t] por el `interventionScale` de aquí, nunca al revés.

## Trayecto de Esse (preset, no hardcode)

`UniversalHearingInput::esseGenezi()`: DAC JieLi 24-bit/48 kHz, Genezi
(flat, sub ON neutro, surround OFF), sala 4.0×7.0 m (V = 70 m³),
escucha a 3 m, RT60 0.34 s, SPL 85 dB, música, 30 min de sesión.
Valores a mano: Dc derivada 0.818 m, coherencia 0.069, Schroeder
139.4 Hz, latencia total 11.41 ms, profundidad media ~0.62,
compresión espacial ~0.47, enmascaramiento ~0.30, inteligibilidad
~0.40, microdinámica 0.20, causalidad ~0.28, fatiga ~0.01,
imagen 0°. `esseHeadphones()` es el mismo DAC y señal con audífono
(sin sala: profundidad 0, imagen 0, causalidad ~0.03 solo por buffer,
confianza 0.85).

## Archivos

- `universal_hearing.hpp` — API, structs, presets, estado de 16 dims.
- `universal_hearing.cpp` — implementación (modelos + confianza).
- `universal_hearing_test.cpp` — tests host standalone (propio `main()`).
- `CMakeLists.txt` — build + ctest standalone.
- `README.md` — este archivo.

## Cómo correr los tests

```sh
cmake -S app/src/main/cpp/fused/universal_hearing -B /tmp/uh_build \
      -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/uh_build
ctest --test-dir /tmp/uh_build --output-on-failure
```

## Pendiente (no es de este módulo)

- Enganchar `test_universal_hearing` a la suite host principal
  (`tests/CMakeLists.txt`): lo hace el dueño del flanco Tests host.
- Puente gemelo → P6 y P6 → MPC (estilo `twin_bridge.hpp`): lo decide
  el dueño de esos módulos (aquí solo se expone la API).
- Medir el THD y el piso de ruido de la cadena real de Esse, y medir
  (no derivar) la distancia crítica y la coherencia en su sala, para
  subir la confianza del preset (hoy ~0.73) con datos reales.
- Calibrar las constantes nominales contra mediciones y escucha ciega
  (ABX): hoy son razonamiento, no medición.
