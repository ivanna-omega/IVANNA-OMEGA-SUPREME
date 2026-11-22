# MPC perceptual en modo sombra — `fused/perceptual_mpc/`

Prototipo del próximo salto magistral (parte 3 del plan de luna, 2026-11-21):
un controlador predictivo perceptual que observa el campo, predice el futuro
de los 7 operadores y elige la mejor acción — **sin aplicarla** (modo sombra).

## Archivos

| Archivo | Qué hace |
|---|---|
| `perceptual_vector.hpp` | `PerceptualVector` (9 dimensiones en [0,1]) y `PerceptualField` (8 bandas nominales). `Action` = delta por operador en [-1,1]. |
| `operator_simulator.hpp/.cpp` | Modelo lineal auditable: `P' = clamp(P + Σ a_op·W[op])`; el presupuesto lo consume `0.02·||a||_1`. Sin red neuronal. |
| `mpc_controller.hpp/.cpp` | `MpcController`: candidatos deterministas, función objetivo, histéresis, límite de tasa, techo de presupuesto, modo seguro y `processShadow()` (memcpy bit-exacto + un paso). |
| `perceptual_mpc.hpp` | Umbrella. |

## Qué decide el MPC

En cada paso: genera candidatos (cero, ±maxDelta por operador, todos ±maxDelta,
y pseudo-aleatorios con semilla fija), simula el horizonte con el simulador,
puntúa cada futuro y devuelve la mejor acción. La acción **no** se aplica al
audio: solo se registra en `lastDecision()` y se cuenta en `decisionsLogged()`.

Función objetivo (mayor = mejor):
`ganancia(claridad, naturalidad, impacto, espacialidad, estabilidad)`
`− fatiga (con signo: bajarla premia) − |Δsonoridad| − caída de confianza`
`− incoherencia entre bandas − wMovement·||a||₂ − wLatency·||a||₁`
`− wBudget·presupuesto_predicho`.

## Límites duros (nunca se violan)

- **Tasa de cambio:** `|a[t]−a[t−1]| ≤ maxDeltaPerCycle` por operador (default 0.20).
- **Techo de presupuesto:** predicción con presupuesto medio > `maxBudget`
  (default 0.90) puntúa −∞ y no se elige.
- **Histéresis:** no se cambia de acción si la mejora es menor que
  `improvementThreshold` (default 0.01).
- **Modo seguro:** confianza media < `minConfidence` (default 0.20), campo no
  finito, o configuración inválida → acción cero, determinista.

## Honestidad sobre los números

- **Los pesos W del simulador son CALIBRACIÓN INICIAL por razonamiento, no
  valores medidos del motor real.** Cada operador empuja las dimensiones que
  por diseño debe mover. La calibración contra mediciones (ABX, escucha,
  telemetría) es trabajo pendiente.
- **Los pesos del objetivo (wClarity=1.5, etc.) son prioridades de diseño,
  no una métrica validada.** `wMovement`/`wLatency` son desempates; la guarda
  fuerte es el límite de tasa + el techo de presupuesto + la histéresis.
- **Los centros de banda (63…8000 Hz) son nominales**, no medidos.
- **No hay ninguna afirmación de mejora audible.** El módulo está en sombra:
  decide sin tocar el audio. El veredicto de escucha requiere instalar,
  activar y comparar con ABX — nada de eso está hecho.

## Qué NO hace este módulo

- No toca audio, FusedChain, JNI, UI ni daemon. No hay cableado a Ruta A/B
  (eso es un paso posterior, cuando la sombra demuestre decisiones sanas).
- No aprende: el modelo es una matriz lineal fija. No hay red neuronal y
  ninguna red actúa como autoridad final.
- No promete ser "el mejor DSP del mundo": es un prototipo de decisión con
  tests, no una medición comparativa.

## Tests

`app/src/main/cpp/tests/test_perceptual_mpc.cpp` (ctest, g++-14):
sombra bit-exacta, elección del argmax, límite de tasa en 200 pasos,
histéresis, modo seguro (confianza baja y NaN), techo de presupuesto,
identidad del simulador con acción cero y coincidencia con la fila de pesos.

## Pendiente (no hecho aquí)

1. Calibrar W contra mediciones reales del motor (hoy es razonamiento).
2. Alimentar P[t] con mediciones reales (hoy el campo lo pone el llamador).
3. Cablear la sombra a la telemetría para observar decisiones en vivo.
4. Activación real (fuera de sombra) solo tras demostrar decisiones sanas y
   con rollback instantáneo — no está en este módulo.
