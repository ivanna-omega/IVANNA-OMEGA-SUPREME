# Protocolo de evaluación perceptual ABX — IVANNA-OMEGA-SUPREME

- **Versión del producto:** v2.5.445 (versionCode 736)
- **Fecha:** 2026-11-14 · **Base:** HEAD 74337b5a, rama `ivanna-real-completa`
- **Estado:** protocolo definido; **ninguna prueba ABX ha sido ejecutada bajo
  este protocolo**. Todo resultado anterior (mediciones THD+N, tests de
  escucha informales, "suena magistral") es evidencia de ingeniería, no
  validación perceptual.
- **Principio rector:** distinguir A de B **no demuestra que B sea mejor**.
  Discriminación, preferencia, localización, externalización, fatiga y
  calidad son preguntas distintas y se prueban por separado.

## 0. Definiciones

- **A / B:** dos condiciones de procesado (p. ej. A = cadena IVANNA activa,
  B = bypass bit-exacto; o A = ORBE 1.0, B = ORBE 0.0). Una de ellas es
  siempre una referencia definida, nunca "lo que suene mejor".
- **X:** estímulo incógnita, igual a A o a B, que el participante debe
  identificar (ABX clásico).
- **Ensayo (trial):** una presentación A, B, X y una respuesta.
- **Sesión:** bloque de ensayos con un solo par A/B y una sola pregunta.
- **Participante:** oyente que firma consentimiento; se registra experiencia,
  edad y audición conocida (sin datos médicos sensibles más allá de lo
  declarado voluntariamente).

## 1. Preguntas separadas (nunca mezcladas en una sesión)

| # | Pregunta | Tarea del participante | Métrica primaria |
|---|---|---|---|
| P1 | ¿Se distingue A de B? (discriminación) | ABX: "¿X es A o B?" | Proporción de aciertos vs azar (50%) |
| P2 | ¿Cuál se prefiere? (preferencia) | Elección forzada A/B + escala | Proporción de preferencia + intervalo |
| P3 | ¿Dónde se localiza el sonido? (localización) | Apuntar la dirección percibida (azimut/distancia) | Error angular medio y dispersión |
| P4 | ¿El sonido sale de la cabeza? (externalización) | Escala de externalización (fuera/dentro) | Puntuación media por condición |
| P5 | ¿Cansa escuchar? (fatiga) | Escucha prolongada + escala de fatiga/cómodidad | Cambio de fatiga en el tiempo |
| P6 | ¿Cuál es la calidad global? (calidad) | Escala de calidad (p. ej. 0–100) por condición | Diferencia media + intervalo |

Regla: **una sesión = una pregunta**. Mezclar P1 con P2 en la misma
sesión contamina ambas (el participante usa la preferencia para adivinar
la identidad y viceversa).

## 2. Hipótesis y análisis definidos ANTES de medir

Antes de la primera sesión, el experimentador registra por escrito:

1. **Hipótesis nula (H0) y alternativa (H1)** para esa pregunta. P. ej. P1:
   H0 "aciertos = 50% (azar)"; H1 "aciertos > 50%".
2. **Tamaño de muestra:** número de participantes y ensayos por
   participante, justificado por potencia (ver §3). No se añade muestra
   "hasta que salga significativo".
3. **Criterio de éxito:** umbral numérico (p. ej. P1: aciertos ≥ 65% con
   p < 0.05; P2: preferencia ≥ 60% con intervalo que excluya el 50%).
4. **Análisis:** test estadístico exacto (binomial para P1/P2; t-test o
   equivalente no paramétrico para P3–P6, con corrección por comparaciones
   múltiples si hay más de una).
5. **Regla de parada:** número fijo de sesiones; no se para antes por
   "ya se ve claro" ni se prolonga por "casi sale".

Todo esto se registra antes de medir; cambiarlo después es p-hacking y se
declara como tal si ocurre.

## 3. Tamaño de muestra (mínimos, no objetivos ideales)

- **P1 discriminación (ABX):** mínimo 12 participantes × 16 ensayos
  (192 ensayos). Con 192 ensayos, 65% de aciertos excluye el azar con
  p < 0.001 (binomial). Menos de 8 participantes no se publica como
  validación, solo como piloto.
- **P2 preferencia:** mínimo 12 participantes × 10 comparaciones; la
  preferencia se analiza por participante (no por ensayo) para no inflar
  la muestra con ensayos correlacionados.
- **P3 localización:** mínimo 10 participantes × 12 direcciones; se
  reporta error medio **y** dispersión (un error medio bajo con
  dispersión enorme no es localización).
- **P4/P6 escalas:** mínimo 12 participantes; se reporta media, desviación
  e intervalo de confianza del 95%.
- **P5 fatiga:** mínimo 8 participantes × 30 min de escucha por condición,
  con pausas; la fatiga se mide en el tiempo, no en un instante.

Estos mínimos son puntos de partida. Si el efecto es pequeño, hace falta
más muestra; si el efecto es enorme, menos basta — pero eso se decide
antes, no después de ver los datos.

## 4. Controles obligatorios

1. **Niveles comparables:** A y B se igualan en sonoridad (LUFS / dB SPL
   en el punto de escucha) antes de cada sesión. Una diferencia de nivel
   de 0.5 dB ya decide una preferencia; sin igualación, la prueba no vale.
   Se registra el método de igualación y el nivel final de cada condición.
2. **Aleatorización:** el orden A/B y la identidad de X se aleatorizan por
   ensayo (semilla registrada); el participante no ve etiquetas; el
   experimentador que entrega los estímulos no sabe cuál es cuál cuando
   sea posible (doble ciego; si no es posible, se declara ciego simple).
3. **Mismo material, mismo equipo, misma sala:** mismo track (se registra
   título, formato y tasa), mismos auriculares/altavoces, mismo volumen
   base, misma posición. Cambiar dos cosas a la vez invalida la atribución.
4. **Bypass real:** la condición de referencia debe ser bypass bit-exacto
   (amount 0, verificado), no "otro preset que suena parecido".
5. **Descanso:** pausas entre bloques; la fatiga auditiva dentro de la
   sesión es un confusor, no un resultado.
6. **Ruta declarada:** se registra si la escucha fue por BridgePlayer
   (100% cadena) o por captura (original 100% + copia tardía, ver
   `cap-011`): los resultados de una ruta NO se transfieren a la otra.

## 5. Resultados negativos incluidos (obligatorio)

- Un resultado negativo (no se distingue, no se prefiere, no se
  externaliza) **se publica en el registro con el mismo peso** que uno
  positivo. No se descartan sesiones "porque salieron mal" salvo causa
  técnica documentada (fallo de equipo, nivel mal igualado), y esa causa
  se registra.
- Se reportan siempre: n de participantes, n de ensayos, aciertos,
  proporción, intervalo de confianza del 95% y p. Nunca solo "salió bien".
- **Interpretación prohibida:** "no se distinguió A de B" no significa
  "A y B suenan igual"; significa "con esta muestra y este material, no
  se demostró diferencia". La ausencia de evidencia no es evidencia de
  ausencia.

## 6. Incertidumbre

- Toda métrica lleva su intervalo de confianza del 95% (o intervalo
  creíble con su prior declarado). Un "65% de aciertos" sin intervalo no
  es un resultado.
- Se reporta la variabilidad entre participantes (no solo la media): si
  3 de 12 distinguen perfectamente y 9 no distinguen nada, eso es un
  hallazgo (subgrupo), no ruido que promediar.
- Las mediciones de ingeniería (THD+N, deriva ASRC, latencia) se reportan
  junto a las perceptuales pero no las sustituyen: un THD+N excelente no
  prueba que se escuche mejor, y un ABX negativo no invalida una medición
  de ingeniería.

## 7. Registro de cada sesión (plantilla mínima)

```
fecha / hora / lugar (sala)
versión IVANNA + versionCode + ruta (BridgePlayer / captura / Ruta B)
participante (código, no nombre) / n total
pregunta (P1–P6) / hipótesis H0-H1 / criterio de éxito (pre-registrado)
par A/B (definición exacta + amounts) / material (track, formato, tasa)
igualación de nivel (método + dB resultantes)
aleatorización (semilla) / ciego (doble/simple)
ensayos: n, aciertos, proporción, IC95, p
resultado: positivo / negativo / no concluyente
observaciones (fallos técnicos, fatiga, comentarios del participante)
```

## 8. Estado actual (2026-11-14)

- **Ninguna prueba ABX ejecutada bajo este protocolo.**
- Medición citada sin re-verificar en este inventario: HOLOS THD+N
  −94.0 dB @1 kHz (2026-11-08); ASRC host 25/25, residuo ~0 ppm.
- El bofo ("bofo feo") sigue sin resolver tras el fix ISO 226 de la
  v2.5.441: es el primer candidato a sesión P1/P2 (¿se distingue el fix?
  ¿se prefiere?), con la ruta declarada (BridgePlayer vs captura) como
  variable de control — la pregunta de qué camino de escucha se usó sigue
  sin respuesta.
- El test de aislamiento Tidal-solo (Tidal sin IVANNA, WiFi vs datos,
  track descargado) es previo a cualquier ABX con Tidal: si los tronidos
  existen sin IVANNA, no son de IVANNA.
