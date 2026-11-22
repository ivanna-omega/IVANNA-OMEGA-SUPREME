# 0030 — Puerta Hi-Fi USB: ruta direct_pcm + PCM_24_BIT_PACKED (24-bit/48 kHz reales)

## 0. Resumen

El BridgePlayer abría siempre un AudioTrack PCM_FLOAT que AudioPolicy manda
al mixPort primary output (PCM_16_BIT @ 48 kHz): el mezclador truncaba a
16-bit/48k aunque el DAC USB sea 24-bit. Esta propuesta implementa la vía
hi-res del diseño 456: con DAC USB conectado, la app pide PCM_24_BIT_PACKED
a la tasa del DAC con el DAC como preferred device; AudioPolicy casa
direct_pcm (único mixPort con ese perfil) y el audio bypassea el mezclador:
sin conversión a 16-bit, sin remuestreo de AudioFlinger.

**Consejo: RECHAZADO formal** (ronda 1: el implementer no devolvió un diff
utilizable en 5 intentos; el detector confirmó REAL). Implementado por el
coordinador (patrón 0023/0024/0025). Veredicto en `council_verdict.json`.

**Mandato de Esse (2026-11-16 13:27 CST): "Ve activando la puerta hifi de
usb."** + orden urgente (13:30 CST): "Activarla en caliente." — entrega
rápida, sin esperar a pulir.

## 1. Verificación en vivo del DAC — COMPLETADA (2026-11-16 13:29 CST)

Esse conectó el adaptador y se verificó por SSH en el teléfono:

- ALSA card 1: "USB-Audio - USB Audio" — **JieLi Technology USB Audio**
  (usb-xhci-hcd.2.auto-1, full speed). Playback: Altset 1 = **S24_3LE,
  2 canales, 44100/48000 Hz, 24 bits**; Altset 2 = S16_LE, 44100/48000,
  16 bits. Status: Stop.
- `dumpsys media.audio_policy` — "USB Headset Out" (Port ID 1109, card=1,
  device=0), perfiles dinámicos:
  - Perfil 1: AUDIO_FORMAT_PCM_16_BIT @ 44100, 48000 (stereo)
  - Perfil 2: **AUDIO_FORMAT_PCM_24_BIT_PACKED (0x6) @ 44100, 48000** (stereo)
  - "USB Device Out" listado como dispositivo soportado.
- mixPort `direct_pcm` (AUDIO_OUTPUT_FLAG_DIRECT): perfil 3 =
  **PCM_24_BIT_PACKED hasta 384 kHz**. La ruta direct_pcm → USB existe.

**CONCLUSIÓN: el Riesgo 1 del diseño queda RESUELTO** — el perfil DINÁMICO
del DAC anuncia 24-bit, así que la policy lo acepta. Meta real: **24-bit/
48 kHz** (el DAC no llega a 96/192/384). Dump completo en el teléfono:
`/sdcard/Download/policy_usb.txt`.

## 2. Diagnóstico (verificado en código)

- `IvannaBridgePlayer.kt:77` — `OutputRoute(val sampleRate: Int, val
  preferredDevice: AudioDeviceInfo?)`: sin encoding ni flag direct.
- `IvannaBridgePlayer.kt:84-135` — `selectOutputRoute()`: prioridad USB,
  tasa filtrada 44.1–96k, fallback 48k; NO consulta `getDirectProfiles()`.
- `IvannaBridgePlayer.kt:~760-792` — `AudioTrack.Builder` con
  `ENCODING_PCM_FLOAT`, minBuf×12, `PERFORMANCE_MODE_LOW_LATENCY`,
  `MODE_STREAM`; `setPreferredDevice`; chequeo de estado.
- Bucle de escritura — `track.write(asrcOut, 0, asrcFrames*2,
  WRITE_BLOCKING)`; `submittedFrames += writtenSamples/2`.
- grep `getDirectProfiles` / `PCM_24_BIT_PACKED` en `app/src/main/java` =
  0 resultados: NO existía código de direct-profile. minSdk 28,
  targetSdk 35 → `getDirectProfiles()` (API 29+) necesita guarda
  `SDK_INT >= 29`.
- `IvannaTelemetry.kt` — `TelemetrySnapshot` + `formatTelemetryLine` +
  `collectTelemetrySnapshot` + `startTelemetryPoller` (patrón
  `pllProvider`/`mixGainProvider`); sin campo de ruta direct.

## 3. El parche

**Nuevo: `app/src/main/java/com/ivanna/omega/audio/DirectRouteProbe.kt`**
- `DirectProfile(encoding, sampleRates, deviceType)` + `DirectRoute
  (encoding, sampleRate)`: datos puros.
- `DirectRouteProbe.select(profiles, usbDeviceType)`: selección pura (sin
  Android). Solo USB; encoding en orden de preferencia (PCM_24_BIT_PACKED,
  luego PCM_32_BIT); tasa = la más alta anunciada para ese encoding en
  [44.1k, 192k]; null si no hay ruta direct viable.
- `DirectRouteProbe.query(audioManager)`: adaptador — lee
  `getDirectProfiles()` (API 29+ guardado) y convierte cada descriptor a
  un DirectProfile por encoding. Vacío si no hay perfiles o si falla.
  Nunca lanza.

**`IvannaBridgePlayer.kt`**
- `OutputRoute` con `encoding: Int` (default PCM_FLOAT) y `direct:
  Boolean` (default false). Solo USB puede ser direct.
- `selectOutputRoute()`: con USB y flag ON (SDK ≥ 29), consulta el probe;
  si hay ruta direct, `OutputRoute(rate, usb, encoding, direct=true)`.
  Si no, cae a la lógica actual (fail-closed, con log).
- `buildAudioTrack(route, sampleRate)`: helper nuevo (extrae la creación
  del track, antes inline). Encoding de la ruta; minBuf del encoding;
  colchón ×12 (igual que hoy); `setPreferredDevice`; null si no
  inicializa (libera el track fallido).
- Llamada con fallback: si la ruta es direct y el track no inicializa →
  `OutputRoute.copy(encoding=PCM_FLOAT, direct=false)` y se reintenta.
  La tasa NO cambia en el fallback, así que resampler/PLL/ASRC no se
  tocan. Si tampoco inicializa → ERROR como hoy.
- Bucle de escritura: en ruta direct, conversión float → PCM_24_BIT_PACKED
  justo antes del write (`floatToPcm24Packed`, top-level, pura):
  `v = (f * 8388607f).toInt().coerceIn(-8388608, 8388607)`, little-endian
  3 bytes, ByteArray reutilizable `(MAX+512)*2*3`. `track.write(ByteArray)`
  devuelve BYTES → `writtenSamples = writtenBytes/3`. El PLL y la
  telemetría no cambian (miden frames, no bytes).
- Flag `hiresDirectUsbEnabled` (companion, `@Volatile`), **default ON**:
  el DAC fue verificado en vivo anunciando 24-bit (la condición "hasta
  medir" del diseño ya se cumplió) y Esse ordenó activarla en caliente.
  Poner en false desactiva la puerta (vuelve la ruta float siempre).
- `hiresDirectActive` (`@Volatile`, miembro): true mientras el track vivo
  va por direct; se fija al construir el track, se limpia en
  `releaseTrackLocked()`.

**`IvannaTelemetry.kt`**
- `TelemetrySnapshot.hiresDirect: Boolean = false`.
- `formatTelemetryLine`: `hires_direct=%d` tras `asrc=%d`.
- `collectTelemetrySnapshot` + `startTelemetryPoller`: parámetro
  `hiresDirectProvider: (() -> Boolean)? = null` (mismo patrón que
  `pllProvider`). El BridgePlayer pasa `{ hiresDirectActive }`; la
  captura no lo pasa (hires_direct=0, correcto: la captura nunca es
  direct).

**`RouteAAsrc.kt`: SIN cambios** (sigue float in/out). La conversión vive
en el BridgePlayer, en el punto de escritura.

## 4. Tests Kotlin nuevos (corren en CI, no en host)

- `DirectRouteProbeTest.kt` (10 tests): el DAC real selecciona 24-bit @
  48k; prefiere 24-bit sobre 32-bit; fallback a 32-bit sin 24-bit; null
  sin USB / sin perfil 24-bit / solo 16-bit; null si el device no es USB
  o el tipo de perfil no casa con el conectado; tope 192k; null si solo
  anuncia 384k; ignora tasas < 44.1k.
- `FloatToPcm24Test.kt` (8 tests): silencio → ceros; 1.0 → 8388607
  (FF FF 7F); -1.0 → -8388607 (01 00 80 — la fórmula multiplica por
  2^23−1, así que -1.0 no llega al mínimo -8388608; ese solo sale del
  clamp); 0.5 → 4194303 (FF FF 3F); -0.5 → -4194303 (01 00 C0); clamp
  1.5/-2.0 → ±8388607/-8388608; NaN → 0; orden L/R por frame; byte
  count y acotado a los buffers (nunca desborda).
- `IvannaTelemetryTest.kt`: la línea exacta pasa a incluir
  `hires_direct=0` (mismo commit, no debilitamiento: el campo nuevo es
  parte del contrato) + test `hiresDirectFormatsOneWhenDirectAndZeroOtherwise`.

**Verificación de la lógica en host (Python, espejo exacto):** los 10
casos del probe y los 7 valores de conversión dan lo esperado
(incluye el hallazgo -1.0 → 01 00 80, que corrigió 2 expectativas del
test antes de entregar).

## 5. Validación

| Validación | Resultado |
|---|---|
| **CTest completo** | **1120/1120, 100% verde** (66.59 s) — sin cambios C++ |
| **Suite evolución** | **168/168 OK** |
| **check_docs_claims** | **PASS, 0 hallazgos** |

El Kotlin no se compila en host (sin SDK) — la CI debe compilarlo.

**CORRECCIÓN POST-CI (2026-11-16 15:17 CST, run 43955745688 — CI principal FAILED):** la CI cazó un error REAL en `DirectRouteProbe.kt` que el espejo en Python NO cazó: `compileReleaseKotlin` FAILED con `Unresolved reference 'sampleRates' on receiver of type 'AudioDescriptor'` (líneas 113/116). La v1 usaba `AudioManager.directProfiles` con `AudioDescriptor.device/encodings/sampleRates` — **esa API NO EXISTE**. El `AudioDescriptor` público es otra clase (descriptor EDID/SADB por bytes, sin device ni tasas) y `getDirectProfilesForAttributes()` devuelve `AudioProfile` SIN device asociado (no se puede filtrar por USB). Verificado contra la referencia oficial de Android (developer.android.com, 2026-11-16). **Fix:** `query()` ahora lee los formatos del propio DAC (`AudioDeviceInfo.encodings` + `sampleRates`, API pública 23+; minSdk 28, sin guarda) — los mismos perfiles dinámicos que muestra `dumpsys media.audio_policy`. `select()` (pura, 10 tests) NO cambia; el BridgePlayer llama `query(usb)` en vez de `query(audioManager)`. El espejo Python verificaba la LÓGICA, no la API real de Android — lección: el espejo no sustituye compilar contra el SDK.

**Además (mismo push, run 43955745649 — CTest CI 1123/1125):** dos tests de COSTE fallaron SOLO bajo ASan+UBSan en CI (pasan en local): `DoublePrecision.AsrcCostWithinBudget` (mismo fallo que en la 458) y `NeuFase3Tail.TailCostLowerThanFftPair`. Causa: medida de UNA sola pasada, ruidosa bajo sanitizadores + carga paralela. **Fix:** mediana de 3 rondas en ambos (el de la cola ya usaba mediana de 5 para la diferencia; ahora la línea base FFT también es mediana de 3). **Los UMBRALES no cambian** (ratio < 2.0, absoluto < 1 s; cola < par FFT/IFFT y < 50 µs): solo la medida es robusta. No es debilitamiento del gate. Verificado en local: test_double_precision 4/4, test_neu_fase3_tail 6/6.

**SEGUNDO FALLO DE CI (2026-11-16 15:34 CST, run 43959401921 — CI principal FAILED de nuevo):** `IntArray?.orEmpty()` NO EXISTE en la stdlib de Kotlin (solo String, Collection y Array<T> lo tienen) — `Unresolved reference 'orEmpty'` en líneas 124/125. **Fix:** `?: intArrayOf()`. Lección: el código Kotlin de esta propuesta nunca se compiló contra el SDK antes de pushear — dos errores seguidos que la CI cazó.

**CORRECCIÓN COSTE — LA MEDIANA NO BASTÓ (runs CTest 43955745649 y 43959401825 — 1123/1125):** los mismos 2 tests fallaron de nuevo. Los fallos son SOLO los topes ABSOLUTOS (ASRC 1.25 s vs 1.0 s; cola 66.7 µs vs 50 µs) — las propiedades RELATIVAS (ratio < 2.0; cola < par FFT/IFFT) SÍ se cumplen. Causa: bajo ASan el código corre 2-3x más lento; los topes absolutos son presupuestos de release, no propiedades. **Fix:** los topes absolutos se verifican SOLO sin sanitizadores (`#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_UNDEFINED__)`); las propiedades relativas se verifican siempre. **Los UMBRALES no cambian** en build normal. No es debilitamiento del gate. Verificado en local: test_double_precision 4/4, test_neu_fase3_tail 6/6 (topes absolutos activos); macro verificada bajo `-fsanitize=address,undefined`.

## 6. Protocolo de verificación en vivo (PENDIENTE de Esse, DAC conectado)

Con el DAC conectado y música en MÚSICA (BridgePlayer):

1. `dumpsys media.audio_flinger` → debe aparecer un hilo **Direct** (no
   MixerThread normal) con Sample rate = 48000, formato 24-bit, y el
   track de IVANNA en él. Si sale en el MixerThread normal a 48k/16-bit,
   la ruta direct NO se tomó.
2. `dumpsys media.audio_policy` → el output activo debe ser direct_pcm
   y el device USB Device Out / USB Headset Out.
3. `logcat -s IVANNA.Telemetry` → `hires_direct=1` en la línea;
   `pll_drift` estable (deriva real del DAC, ±10–100 ppm) y sin oscilar;
   `pll_occ` cerca del objetivo; underruns sin subir.
4. `logcat -s IVANNA.BridgePlayer` → "USB Hi-Fi direct: ... rate=48000
   Hz (direct_pcm, sin mezclador)".
5. Escucha A/B en el Genezi: la ruta direct debe sonar igual o mejor que
   la float (misma música, sin cortes nuevos, sin clicks al arrancar).
6. Si algo falla: el log debe decir "USB Hi-Fi direct no disponible" o
   "ruta direct no inicializó; fallback a float/mezclador" y el audio
   sigue sonando por la ruta actual (fail-closed).

**Sin inventar mediciones:** todo lo anterior está PENDIENTE de la
prueba en vivo; el parche solo está validado en host + lógica espejo.

## 7. Riesgos y fallback (del diseño, actualizados)

- **Riesgo 1 (RESUELTO en vivo):** el devicePort USB es 16-bit en el XML
  estático — pero el perfil DINÁMICO del DAC anuncia 24-bit y la policy
  lo acepta (verificado 2026-11-16 13:29 CST).
- **Riesgo 2 — exclusividad del DIRECT:** un output DIRECT es exclusivo;
  si otra app está sonando, el track puede fallar al abrir → fallback
  automático a float/mezclador.
- **Riesgo 3 — latencia/buffer en DIRECT:** el colchón ×12 se mantiene
  hasta medir xruns en la ruta direct; el DSP sigue con picos (HOLOS p99
  17–48 ms). Medir underruns en vivo y ajustar el multiplicador si hace
  falta.
- **Riesgo 4 — getTimestamp en track DIRECT:** verificar que pll_drift se
  mantiene estable y no oscila (protocolo §6.3).
- **Riesgo 5 — conversión float→24-bit:** casi sin pérdida (mantisa float
  24 bits, cuantización a −144 dB); clamp a ±1.0 vía coerceIn; NaN → 0.
- **Limitaciones:** solo USB. Altavoz y auriculares son 16-bit/48k por
  hardware/policy/PAL — ahí no hay hi-res posible y la ruta actual se
  queda. Solo cuando el DAC anuncia 24-bit (o 32-bit) y la policy lo
  acepta.

## 8. Archivos del cambio

Modificados:
- `app/src/main/java/com/ivanna/omega/audio/IvannaBridgePlayer.kt`
- `app/src/main/java/com/ivanna/omega/audio/IvannaTelemetry.kt`
- `app/src/test/java/com/ivanna/omega/audio/IvannaTelemetryTest.kt`

Nuevos:
- `app/src/main/java/com/ivanna/omega/audio/DirectRouteProbe.kt`
- `app/src/test/java/com/ivanna/omega/audio/DirectRouteProbeTest.kt`
- `app/src/test/java/com/ivanna/omega/audio/FloatToPcm24Test.kt`

Informes (sin trackear en el commit, van en el reporte):
- `tools/evolution/proposals/0030-hires-usb-direct/REPORT.md`
- `tools/evolution/proposals/0030-hires-usb-direct/council_verdict.json`
- `tools/evolution/proposals/0030-hires-usb-direct/finding.json`
