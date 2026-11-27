# Bucle cerrado SOLO CON EL TELÉFONO — IVANNA (Moto G85, micrófono)

> Estado: CABLEADO v2.5.506 (2026-11-26). No hay diadema EEG, no existe.
> La integración EEG está CANCELADA. Todo funciona solo con el teléfono.
> El micrófono alimenta al organismo (Muselink) por el cableado de esta
> versión: agregador local + cola durable + POST HTTPS desde Kotlin.

## Qué es

El teléfono mismo es el sensor. Mientras suena música por el altavoz o por
la salida Aux (Genezi), el micrófono del teléfono captura la salida acústica
REAL de los altavoces en la habitación. El motor compara lo que mandó con
lo que el micrófono oyó y ajusta. Eso es el bucle cerrado: no hay hardware
externo, no hay diadema, no hay EEG. Solo el Moto G85.

## Flujo (micrófono → Φ → NEURO → Gemelo → organismo)

```
Micrófono (AudioRecord, UNPROCESSED, 48 kHz mono, PCM_FLOAT)
  → RealAcousticSensor (FFT 8192, hop 4096, ~85 ms/frame, 24 bandas ERB)
  → SensorFrame { magnitude_db[24], error_db[24], coherence[24],
                  level_dbfs, frame_index, valid }
  → HyperStateOperator::setSensorFrame()  (Φ, modo MEDIDO si frame válido
     y fresco < 500 ms; si no, modo APROXIMADO, sin inventar)
  → micErbToNeuroFrame() (24 ERB → bandas NEURO)
  → NeuroAcousticClosedLoop → ajustes acotados:
       holosGammaDelta, holosWidthScale, holosDiffuseScale,
       orbeDistanceDeltaM, orbeProximityDeltaDb, wetScale
  → HOLOS / ORBE (parámetros) + Gemelo (twin_estimator, registra
     características + ajustes para el modelo del oyente)
  → la sala real vuelve al micrófono: el lazo se cierra.
  → Y, en paralelo, al organismo (canal lateral, ver abajo).
```

## Micrófono con DAC conectado (requisito de Esse, v2.5.506)

El micrófono del teléfono DEBE seguir funcionando aunque el DAC USB esté
conectado. Antes, al conectar el DAC (JieLi, salida al Genezi), el
micrófono se cancelaba: Android enrutaba la entrada al USB. Eso NO debe
ser así: el bucle cerrado depende de ese micrófono justo cuando suena
música por el Genezi.

Cómo se garantiza (tres capas, ninguna toca el audio):

1. **Fuente:** `AudioRecord` con `MediaRecorder.AudioSource.UNPROCESSED`
   (fallback `MIC`), PCM_FLOAT mono a 48 kHz, en
   `RealAcousticSensor.start()` (sensor) y en `AudioPipeline.runPipeline()`
   (Ruta A). La fuente no cambia: sigue siendo el mic sin procesar.
2. **Ruta:** al crear cada `AudioRecord` se fija explícitamente el
   micrófono interno como dispositivo de entrada preferido
   (`MicRouteAnchor.anchor()`: `setPreferredDevice` al `TYPE_BUILTIN_MIC`
   de `AudioManager.getDevices(GET_DEVICES_INPUTS)`). Si no hay mic
   interno o el ancla falla, la captura SIGUE con la fuente que Android
   elija y el estado lo reporta — el ancla nunca bloquea el audio.
3. **Verificación en vivo:** el hilo de captura verifica la ruta ~1 s
   (`AudioRecord.getRoutedDevice()`). Si la ruta se fue al USB (o a otro
   lado), se **re-ancla automáticamente** al mic interno y se cuenta en
   `RealAcousticSensor.micRouteStatus` (`reanchorCount`, visible en el
   indicador del organismo). La verdad final es el frame del sensor: con
   el DAC conectado y música sonando por el Genezi, el frame debe seguir
   siendo válido (`valid=true`, ERB medido, no silencio).

Archivos: `app/src/main/java/com/ivanna/omega/audio/MicRouteAnchor.kt`
(lógica sobre `MicDevicePort`, testeable con puerto falso),
`RealAcousticSensor.kt` (ancla + vigilancia ~1 s + `micRouteStatus`),
`AudioPipeline.kt` (mismo ancla en el AudioRecord de Ruta A).

Test: `MicRouteAnchorTest` (ancla, deriva al USB, re-ancla, conteo).
La prueba en vivo (DAC conectado + música → frame válido) exige el
teléfono real y está pendiente (ver "Pendiente para validación en vivo").

## Cableado al organismo (v2.5.506)

Orden correcto (consejo Luna/Haiku): **Ruta A primero, después Ruta B**.
La latencia de red NO participa en el lazo DSP.

### Ruta A: agregador local + cola durable + sender Kotlin

```
SensorFrame (~85 ms) → SensorSnapshotConsumer (hilo propio, ~40 ms)
  → SessionAggregator (ventanas 1–5 s, default 5 s: medias ERB,
     dispersión, error_db, coherence, level_dbfs, frames válidos)
  → OrganismQueue (durable en disco: PENDING → SENDING → SENT /
     RETRY / QUARANTINED; idempotencia session_id/chunk_id)
  → OrganismSender (OkHttp, Dispatchers.IO, nunca hilo de audio ni UI)
  → POST /api/v1/muse/ivanna/session en muselink.ai.studio
     (alterno: muselink-667333806336.us-west1.run.app)
     con audioFeatures (24 bandas ERB, source microphone-erb).
     El token vive en EncryptedSharedPreferences (Keystore);
     NUNCA en un literal, ni en archivos, ni en logs.
```

- **No se envía cada frame de 85 ms.** Solo lotes agregados por ventana
  y un payload de sesión al cierre. En v2.5.506 el envío es ERB-only:
  no hay captura WAV (consejo 2026-11-26, defecto #12: el camino WAV
  era código muerto — ningún llamador lo alimentaba — y se eliminó).
- **Backoff con jitter:** 30 s, 60 s, 120 s… tope 15 min; 8 intentos y
  luego cuarentena. 401 = token inválido (reautorizar una vez) y NO
  consume intentos: el chunk vuelve a PENDING (defecto #3). 429/5xx/
  red = reintento. 400/403/404/422 = cuarentena.
- **Si se agota el espacio local** (tope 200 chunks / 50 MB), se
  descartan primero los RETRY más viejos y luego los PENDING más
  viejos; el ERB se conserva. Retención máxima: 7 días (defecto #9).
  La cola se puede borrar entera desde la tarjeta ("Borrar cola
  local"). Los JSON viven en filesDir (privado de la app, sin cifrar
  en reposo). El audio nunca se toca.
- **SENDING huérfano:** si la app muere con un chunk en SENDING, al
  reabrir la cola vuelve a PENDING sin consumir intentos (defecto #2).
- **Sin red o sin organismo**, la sesión se guarda local y se reintenta
  después; nunca se bloquea el audio por el envío.
- **Cierre diferido (defecto #5):** al apagar el organismo, el chunk
  final queda encolado durable y se envía al próximo arranque. No se
  intenta un último envío que el cancelado del scope mataría a medias.

### Ruta B: canal lateral, no desde el hilo de audio

El efecto publica un snapshot y un consumidor dedicado lo ingiere:

```
SensorSnapshot { version=1, source_id, sequence, timestamp, valid, frame }
  → SensorSnapshotChannel (latest value wins: si el consumidor se
     retrasa, se descartan snapshots viejos, nunca se bloquea nadie)
  → validateSensorSnapshot() (versión, fuente, secuencia == frame_index,
     frescura ≤ 1 s, rangos: mag [-120,30], err [-60,60], coh [0,1],
     level [-120,10], todo finito)
  → IvannaFusionEngine::ingestHyperSensorSnapshot() → setSensorFrame()
```

- `ingestHyperNeuroFrame` admite **kind 5 (AcousticMic)** solo tras la
  puerta `validateNeuroIngestArgs` (kind ∈ {2,3,4,5}, rangos [0,1],
  frescura ≤ 1 s, timestamp ≠ 0). El rechazo no toca el estado
  (ni `framesLearned`, ni firma, ni confianza).
- **Aceptar kind 5 NO implica aplicar su salida al audio:** ingestión,
  aprendizaje y aplicación van separados. La activación sigue exigiendo
  la ventana estricta de 250 ms (`kStaleTimeoutUs` en `compute()`).
- **Caza/oscilación acotada:** ganancia ±6 dB/banda, fase ±0.5 rad,
  slew 0.1 dB/bloque, zona muerta por calidad ≥ 0.40 y coherencia
  media ≥ 0.30, congelación ante baja coherencia, watchdog con rollback
  a identidad. Sin micrófono o con snapshot inválido, la contribución
  es exactamente cero.
- **Ruta B inicialmente solo observacional — y hoy "API LISTA"**
  (consejo 2026-11-26, defecto #10): `ingestHyperSensorSnapshot` no
  tiene productor en el proceso del efecto (audioserver); ningún
  llamador alimenta hoy ese camino. La API existe, está validada por
  tests y se conserva (no se elimina); la ruta viva es la Ruta A.
  La JNI mínima es `nativeSensorSnapshotIngest` (construir → publicar
  → validar), llamada solo por `SensorSnapshotConsumer` (hilo propio).
  OJO (defecto #1): la JNI ya NO llama a `setSensorFrame` — el único
  escritor del staging es el hilo de audio (`hyperProcessRouteA` lee
  `latestFrame()` en cada bloque); el consumidor solo publica en el
  canal. Dos escritores concurrentes sobre `stageFrame_` eran un data
  race (el `framePending_` atómico no protege la escritura).

### Autonomía desde que se abre la app

- `OrganismController.start()` corre en `PersistedStateRestorer` al
  arrancar la app (si el interruptor maestro está ON, default ON).
  La conexión se establece sola; la captura y el envío arrancan solos
  cuando suena música (frames válidos = música sonando; silencio =
  frames inválidos = nada que enviar).
- **UI mínima indispensable** (`OrganismCard`, en CONTROL): UN
  interruptor maestro ("Organismo automático: ON/OFF") + UN indicador
  de estado (Capturando / En cola / Sin conexión / Error recuperable /
  Apagado / Sin token / Token inválido). Nada más. El campo del token
  solo aparece cuando el estado es Sin token / Token inválido, y se
  muestra oculto (`PasswordVisualTransformation`, defecto #8). El
  indicador muestra en continuo el mic enrutado ("Mic: Mic interno /
  Mic USB / desconocido", defecto #11) y el contador de re-anclas.
  Con cola > 0 hay un botón "Borrar cola local" (defecto #9).
- Si algo falla, la app sigue sonando igual y lo reintenta sola.
- **Token inválido no se atasca (defecto #4):** se limpia al guardar
  un token nuevo y ante cualquier flush que no sea 401 (antes solo se
  limpiaba si el flush enviaba > 0 chunks, así que con la cola vacía
  quedaba en TOKEN_INVALID para siempre).

### Límites honestos de la autonomía (no se esconden)

- Android exige el permiso `RECORD_AUDIO` (se concede una vez,
  revocable) y la notificación persistente del foreground service para
  captura prolongada.
- Si el token expira sin endpoint de refresh, hay que reautorizar una
  vez (el estado lo dice: "Token inválido"). La cola se conserva; no
  se descarta.
- "Nunca se pierde el audio" solo es realizable mientras exista espacio
  local: hay tope de cola (200 chunks / 50 MB) y degradación explícita.

## Archivos clave (base 505 + cableado 506)

| Pieza | Archivo |
|---|---|
| Sensor micrófono | `app/src/main/cpp/spatial/RealAcousticSensor.hpp` |
| Snapshot + canal lateral | `app/src/main/cpp/spatial/SensorSnapshot.hpp` (nuevo) |
| Puerta kind 5 | `app/src/main/cpp/neuromorphic/neuro_acoustic_closed_loop.hpp` (`validateNeuroIngestArgs`) |
| Ruta B (motor) | `app/src/main/cpp/IvannaFusionCore.{h,cpp}` (`ingestHyperSensorSnapshot`, `ingestHyperNeuroFrame` con kind 5) |
| JNI mínima | `app/src/main/cpp/jni/ivanna_omega_jni.cpp` (`nativeSensorSnapshotIngest`, kind 5 en `nativeNeuroLoopIngest`) |
| Φ (consume SensorFrame) | `app/src/main/cpp/spatial/HyperStateOperator.hpp` (`setSensorFrame`, modos APROXIMADO/MEDIDO) |
| Mic → NEURO | `app/src/main/cpp/neuromorphic/neuro_acoustic_closed_loop.hpp` (`micErbToNeuroFrame`, `NeuroSensorKind::AcousticMic = 5`, ajustes acotados) |
| Ancla mic-con-DAC | `app/src/main/java/com/ivanna/omega/audio/MicRouteAnchor.kt` (nuevo) |
| Sensor Kotlin | `app/src/main/java/com/ivanna/omega/audio/RealAcousticSensor.kt` (ancla + vigilancia) |
| AudioRecord Ruta A | `app/src/main/java/com/ivanna/omega/audio/AudioPipeline.kt` (ancla) |
| Consumidor lateral | `app/src/main/java/com/ivanna/omega/organism/SensorSnapshotConsumer.kt` (nuevo) |
| Agregador | `app/src/main/java/com/ivanna/omega/organism/SessionAggregator.kt` (nuevo) |
| Cola durable | `app/src/main/java/com/ivanna/omega/organism/OrganismQueue.kt` (nuevo) |
| Payload Muselink | `app/src/main/java/com/ivanna/omega/organism/OrganismPayload.kt` (nuevo) |
| Sender HTTPS | `app/src/main/java/com/ivanna/omega/organism/OrganismSender.kt` (nuevo) |
| Token cifrado | `app/src/main/java/com/ivanna/omega/organism/OrganismTokenStore.kt` (nuevo) |
| Orquestador + estado | `app/src/main/java/com/ivanna/omega/organism/OrganismController.kt` (nuevo) |
| UI mínima | `app/src/main/java/com/ivanna/omega/ui/OrganismCard.kt` (nuevo) |
| Arranque autónomo | `app/src/main/java/com/ivanna/omega/core/PersistedStateRestorer.kt` |
| Gemelo (P10) | `app/src/main/cpp/fused/closed_loop/twin_estimator.hpp/.cpp` + `app/src/main/cpp/fused/acoustic_twin/acoustic_twin.hpp/.cpp` |
| Cadena (engage, trim, gates) | `app/src/main/cpp/fused/fused_chain.cpp` (`blendWetCore`, `applyCompiledExperience`, `pushEffectiveControls`) |
| Controles / neutro | `app/src/main/cpp/fused/fused_controls.hpp` (`SpatialControls`, `isNeutral`, `setNeutral`) |
| Espacial / HRTF efectiva | `app/src/main/cpp/fused/fused_spatial.hpp` (`effectiveHrtfWet`, `effectiveOrbeAmount`) |
| Compilador P8 | `app/src/main/cpp/fused/experience_compiler/experience_compiler.cpp` (`kNominalGainDb`, `compile`, trim = −Σ g·amount) |

## El sensor (RealAcousticSensor) — qué mide

- Por frame (~85 ms): FFT 8192, ventana Hann, 24 bandas ERB (60 Hz–16 kHz,
  las mismas que HOLOS).
- `magnitude_db[b]`: nivel medido por banda (dBFS RMS).
- `error_db[b]`: (magnitud − nivelRef 250 Hz–2 kHz) − objetivo Harman
  AE/OE 2018. Es el BALANCE TONAL, independiente del volumen.
- `coherence[b]`: |Sxy|²/(Sxx·Syy) mic×referencia [0..1]. Sin referencia,
  coherencia = 0 (no se finge).
- `level_dbfs`: nivel de banda ancha del mic.
- `valid`: false si silencio (< −70 dBFS), sin datos suficientes o muestras
  no finitas. Un frame inválido NO corrompe el estado suavizado.
- `mapErrorToRoomCorrection()`: error ERB (24) → corrección RoomProbe
  (10 bandas), clamp(−9, +3) dB. Recorta hasta −9, realza hasta +3.
- Publicación lock-free (seqlock). `latestFrame()` nunca bloquea.
- La referencia la empuja el hilo de audio (Ruta A, `pushReference`,
  anillo SPSC lock-free, la salida real al altavoz).

## Fail-closed (sin micrófono o sin medida)

- Sin frame válido o sin frescura (< 500 ms): Φ cae a modo APROXIMADO
  (Γ por balance espectral, sin inventar medida). NEURO no aplica ajustes
  sin medida válida (`valid=false` → no alimenta nada).
- Sin micrófono o sin medida, IVANNA suena EXACTAMENTE igual que antes:
  P0 intacto, bit-idéntico en bypass. La corrección es LENTA (~85 ms +
  suavizado 250 ms): es medida de sala, no corrección muestra a muestra.
- Sin red, sin token o sin organismo: la sesión se guarda local y se
  reintenta; el audio no se toca y no se bloquea.
- Limitaciones honestas: el micrófono NO está calibrado (balance relativo,
  no SPL absoluto); mide el punto donde está el teléfono, no los oídos;
  poco fiable bajo ~80 Hz; la coherencia exige referencia alineada en
  tiempo.

## Topes duros del lazo (NEURO)

Del header `neuro_acoustic_closed_loop.hpp`:

- Ganancia por banda: ±6 dB (`kMaxGainDb`).
- Fase por banda: ±0.5 rad (`kMaxPhaseRad`).
- Respuesta cortical objetivo: 0.65 (`kDesiredResponse`).
- Calidad mínima: 0.40 (`kMinQuality`); coherencia mínima: 0.30.
- Ajustes HOLOS/ORBE: `holosGammaDelta` (luego clamp), `holosWidthScale`
  ±0.3, `holosDiffuseScale` [0..1], `orbeDistanceDeltaM` ±0.2 m,
  `orbeProximityDeltaDb` ±3 dB, `wetScale` [0..1].
- Puerta de ingestión kind 5: frescura ≤ 1 s (`kIngestMaxAgeUs`); la
  activación exige ≤ 250 ms (`kStaleTimeoutUs`).

## Los tres fixes aplicados en el plan (ver IMPLEMENTATION_PLAN.md)

1. **P8 (NEURO −15.570 → −8.709 dB):** re-derivación apareada. NEURO toma
   LOO (−11.154) + interacción nueva (+2.445) = −8.709 dB. ORBE se
   re-deriva a su marginal (−6.336 dB, atribución 0) para que la suma
   siga −19.482 dB y el trim siga +19.482 dB. Solo cambiar NEURO rompería
   la Regla de Oro (~−6.9 dB de neto).
2. **Puerta HRTF al apagar ORBE:** en `SpatialControls::clamp()`, si
   `amount <= 0.0f` entonces `hrtfWet = 0.0f`. Así `isNeutral()` es true
   y la cadena hace bypass limpio en audífonos (sin HRTF sintética con
   notch de −44 dB en R a 11.5 kHz).
3. **Dip de enganche (−5.37 dB):** `blendWetCore` pasa de mezcla lineal a
   mezcla de potencia constante (cos/sin). Re-baseline del test
   `EngageDipAttribution977` al valor nuevo medido.

## Cómo se prueba (host, sin teléfono)

- `CTest`: P0 9/9, Regla de Oro (`D10Corona.MatchedLufsInvariantAcross7Operators`),
  `P1P4Ablation.LeaveOneOutAt1kHz`, `EvolGenezi.EngageDipAttribution977`,
  `CpuXruns`, bit-exactitud en bypass (`AmountZeroIsBitExactBypass`).
- Sensor: `test_real_acoustic_sensor.cpp` (seno de amplitud A → 10·log10(A²/2)
  dBFS; silencio → valid=false; coherencia sin referencia = 0).
- NEURO: `test_neuro_acoustic_closed_loop.cpp` (mic → NeuroFrame, topes).
- Canal lateral + kind 5: `test_sensor_snapshot.cpp` (15 tests: validación
  de snapshot, constructor ERB[74], latest-wins, puerta kind 5, rechazo
  sin tocar el estado, fail-closed a identidad).
- Kotlin (`testDebugUnitTest`): `SessionAggregatorTest` (ventanas 1–5 s,
  inválidos, dispersión), `OrganismQueueTest` (estados, reintento con
  backoff, cuarentena, SENDING huérfano, retención 7 días, borrado),
  `MicRouteAnchorTest` (ancla, deriva al USB, re-ancla, ruta desconocida),
  `OrganismPayloadTest` (schema Muselink), `OrganismSenderTest` (200/401/
  429/500/400 + fallback de host, contra interceptor OkHttp — sin sockets:
  el loopback Java dual-stack de la VM no entrega el accept),
  `OrganismControllerTest` (estado visible).

## Pendiente para validación en vivo (exige teléfono real + música)

- Que el micrófono realmente alimente al sensor en la Ruta A viva
  (AudioRecord UNPROCESSED corriendo, referencia empujada por el hilo
  de audio) y que Φ entre en modo MEDIDO (SensorFrame válido y fresco).
- **Mic con DAC conectado:** con el DAC JieLi conectado y música sonando
  por el Genezi, el frame debe seguir válido (`valid=true`, ERB medido)
  y el indicador debe mostrar "Mic interno" (si hubo deriva, el conteo
  de re-anclas lo dice). Si el mic se cancela con el DAC, el cableado
  NO está completo.
- Que la corrección de sala medida (RoomProbe) se escuche como mejora
  (no solo como número): el lazo es lento (~85 ms + 250 ms), así que el
  efecto es de balance tonal de sala, no de transitorio.
- Que un chunk agregado llegue al organismo (POST 201) con el token
  provisionado una vez, y que la cola se vacíe sola al volver la red.
- Veredicto de escucha de Esse: ¿el lazo cerrado suena mejor que abierto?
  ¿Hay oscilación o bombeo? (Si lo hay, bajar `wetScale` / topes.)
- Medir la sala real en 9 posiciones sigue aplazado (orden de Esse).
  ABX sigue cancelado (orden de Esse).
- WAV: eliminado en v2.5.506 (defecto #12 del consejo): era código muerto
  sin llamador. El envío es ERB-only, por decisión documentada.
