# P7 — Conservación Total (Frente E, 2026-11-24)

P7 del plan `PLAN_P7_P10.md` §2. Módulo C++ standalone, sin cablear en
la app, sin tocar audio: solo estima y certifica.

## Qué se construyó (P7.1 + P7.2)

- `total_conservation.hpp/.cpp` — `estimateDelta()` por etapa (9 etapas:
  fuente, decodificación, DSP, conversión, amplificación, transductor,
  sala, posición, percepción) y `PlaybackCertificate` por reproducción.
- `total_conservation_test.cpp` — tests host (patrón standalone de P6).

## Qué está medido y qué está "sin estimar" (honesto, 2026-11-24)

| Etapa | Estado |
|---|---|
| Fuente WAV/FLAC | Δ = 0 estimado (sin pérdida, por definición) |
| Fuente MP3/AAC/Opus/desconocida | sin estimar (pérdida no medida en esta cadena) |
| Decodificación bit-exacta | Δ = 0 estimado |
| Decodificación con resampling | sin estimar (medible en host, pendiente) |
| DSP con amounts en 0 | Δ = 0 estimado |
| DSP con amount > 0 | sin estimar (falta la tabla medida del banco adversarial P1) |
| Conversión / Amplificación / Transductor / Sala / Posición | sin estimar (P3: gemelo ~0.46 < 0.80; potencia y sensibilidad del Genezi SIN MEDIR) |
| Percepción estimada | sin estimar (P10 no existe) |

Hoy el certificado de una reproducción lossless con DSP apagado tiene
3/9 etapas estimadas, Δtotal = 0.0 dB como **piso** y `totalValid=false`
con confianza 0. El certificado no finge saber lo que no sabe.

## Bloqueado

- **En P2 (audible NO hecho):** sin P2 audible no hay alteración por
  operador que totalizar; P7.1-DSP queda en "sin estimar" hasta que el
  banco adversarial mida cada operador.
- **En P3 (gemelo ~0.46, sin calibrar):** conversión, amplificación,
  transductor, sala y posición. La calibración con barrido (§6 del plan)
  exige una sesión de Esse (~15 min).
- **En P10:** el término de percepción estimada.
- **En Esse (escucha):** P7.3 trazabilidad audible (identificación de
  procedencia ≥8/10, ~30 min), P7.4 umbrales de intervención (1–2
  sesiones ~30 min), P7.5 certificado en la UI (código, sin teléfono,
  pero posterior a P7.3/P7.4).

## Cómo correr los tests

```sh
cmake -S app/src/main/cpp/fused/total_conservation -B /tmp/p7_build \
      -DCMAKE_CXX_COMPILER=g++-14
cmake --build /tmp/p7_build
ctest --test-dir /tmp/p7_build --output-on-failure
```

Regla de Oro: este módulo no aplica ganancia; ganancia neta 0.0 dB por
construcción (solo estima y certifica).
