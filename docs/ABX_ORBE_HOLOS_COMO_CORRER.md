# ABX ORBE·Ω+HOLOS·Ω ON vs OFF — Cómo correrlo (Repair 6 / F1, v2.5.471)

Herramienta lista para que el dueño corra el estudio. **Este documento no
contiene resultados ni veredictos**: eso lo pone el dueño tras correr las
2 sesiones. No declarar el ABX como "pasado" ni como evidencia hasta
entonces.

## Protocolo

- **Protocolo:** `ABX_OrbeHolos_On_vs_Off` v1.
- **Condición:** ORBE·Ω+HOLOS·Ω **ON vs OFF**. ON = los controles FUSIÓN
  guardados del usuario (tal cual); OFF = exactamente los mismos controles
  con `orbeAmount = 0` y `holosAmount = 0`. Todo lo demás (Φ, Volterra,
  EQ, sala, dinámica, cerebro) queda idéntico: la única diferencia
  audible entre A y B es ORBE·Ω+HOLOS·Ω.
- **Ensayos:** 16 por sesión. En cada ensayo A y B se sortean (X es uno
  de los dos al azar) y el oyente responde "X es igual a A o a B".
- **Level-match:** ±0.2 LU. Sin igualado no se puede iniciar
  (fail-closed). Se mide con el medidor LUFS vivo de la app
  (BS.1770-4, EMA ~400 ms) y el trim se aplica con el master gain
  global (solo nivel, no cambia el procesado).
- **Sesiones:** el dueño corre **2 sesiones** (32 ensayos en total).

## Pasos

1. **Preparar:** pon música a sonar con el mismo pasaje de referencia
   durante toda la sesión (Tidal/Qobuz en la app nativa o el reproductor
   propio — lo que suene por la cadena viva). El estado maestro debe
   estar en **ACTIVO** y los controles FUSIÓN guardados deben tener
   `orbeAmount > 0` o `holosAmount > 0` (si ambos están en 0, ON sería
   idéntico a OFF y la pantalla no arranca).
2. **Abrir:** Hub → **PRUEBA ABX** → botón
   «ABX ORBE·Ω+HOLOS·Ω — ON vs OFF, 16 ensayos (±0.2 LU)».
3. **Igualar:** pulsa «Igualar LUFS (±0.2 LU)». Mide OFF y ON (3 s por
   condición, mismo pasaje) y aplica el trim a ON. Si el residual queda
   > ±0.2 LU, reintenta con el mismo pasaje.
4. **Correr:** pulsa «Iniciar ABX (16 ensayos)». Escucha A, B y X de cada
   ensayo, responde «X es igual a A o a B» y pulsa «Enviar y registrar».
   Cada ensayo queda guardado en `AbxResultStore` al momento. La pantalla
   **nunca** muestra cuál es ON ni el acierto/fallo por ensayo (para no
   sesgar los restantes).
5. **Resumen:** al terminar los 16 ensayos aparece aciertos/16, p-valor
   (binomial exacto, H0=0.5), IC 95% (Wilson) y el veredicto
   («Diferenciación reportada» solo si n>=10 y p<0.05; si no,
   «NO DEMOSTRADO»). «Exportar JSON» escribe el historial completo
   (la sesión queda en `history` del JSON; el resumen de cabecera del
   export es del protocolo HRTF, no de este).
6. **Salir:** al salir se restauran los controles FUSIÓN guardados y el
   master gain vuelve a 0 dB. El ON/OFF del ABX es transitorio y nunca
   se guarda en `ParameterStore`.

## Archivos

- `app/src/main/java/com/ivanna/omega/spatial/OrbeHolosAbxSession.kt`
  — lógica de la sesión (16 ensayos, gate LUFS ±0.2 LU, JSON por ensayo).
- `app/src/main/java/com/ivanna/omega/ui/AbxTestScreen.kt`
  — `OrbeHolosAbxScreen` (modo `ORBE_HOLOS` de `AbxTestScreen`) + botón
  de entrada. La navegación ya existía: Hub → PRUEBA ABX
  (`IvannaRoute.ABX_TEST`); no hizo falta tocar navegación.
- Reutiliza `AbxTrialSession`, `AbxResultStore`, `AbxStats`,
  `LufsMatch`, `AudioEngine.getLufs()/setMasterGain()` y
  `FusedControlsBridge.safeSetFusedControls()` (Ruta A por JNI + Ruta B
  por SET_PERCEPTUAL_STATE).

## Pendiente de verificar (en el G85, no en host)

- Que la pantalla abre y el igualado LUFS llega a ±0.2 LU.
- Que los 16 ensayos registran y el resumen muestra aciertos/16 + p-valor.
- Que al salir los controles vuelven a los guardados y el master gain a 0 dB.
- Kotlin no compila en la VM (sin Android SDK); lo verifica la CI.
