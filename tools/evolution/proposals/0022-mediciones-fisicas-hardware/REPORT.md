# 0022 — Mediciones físicas en hardware real (Punto 3 de la crítica externa)

## Decisión del Consejo Supremo (2026-11-16 07:47–07:55 CST)

**Orden de Esse (07:46 CST):** ATACAR la crítica externa DE VERDAD — no por
encimita, no borrar, no esconder, no degradar. Revivir, desenterrar, corregir,
mejorar. Perfección. La crítica completa está en
`~/workspace/analysis-ivanna/CRITICA_2026-11-16.md` (NO va al repo público).

**Deliberación — RONDA 1 (unánime, sin necesidad de más rondas):**
- **PROPOSER** (ddgw/tinfoil/gpt-oss-120b, interino): recomienda **(A)**.
- **REVIEWER** (ddgw/gpt-5.6-luna): recomienda **(A)**.
- **ARBITER** (ddgw/gpt-5.4-mini): **CONFIRMA (A)**.

Los pro (gemini-4-argon, gemini-3.1-pro-preview) siguen en cooldown hasta
2026-11-17 ~02:00 CST; se trabajó con los interinos, sin esperarlos.

**Candidatos evaluados (criterios: viable de implementar Y validar en esta
corrida; ataca de verdad la crítica; no degrada el sonido; evidencia medible):**
- (A) **Punto 3 — Mediciones físicas — ELEGIDO.** SSH al teléfono funciona;
  la instrumentación es solo lectura (no toca la cadena de audio, no
  degrada); la matemática es testeable en host; produce los números que la
  crítica exige (THD+N, latencia, underruns, clipping).
- (B) **Punto 6/1-RIR — NO ahora.** El default del snapshot YA es wet 0.22 /
  RT60 0.34 y el permiso de sala es 1.0; hay que verificar runtime antes de
  afirmar que está en 0. Si ya está prendida, la crítica está desactualizada
  en este punto y el trabajo es demostrarlo con evidencia (ver Plan).
- (C) **Punto 1-coclear — NO ahora.** g_cochlearEnabled=false por defecto
  porque fue apagado por artefactos a 1.0 ("FIX artefactos"). Reactivarlo sin
  entender los artefactos puede DEGRADAR (prohibido por Esse).
- (D) **Punto 2 — Bofo/filtro peine — NO ahora.** Fix arquitectónico complejo
  (flujo recapturado ~130 ms + trayectoria directa); verificación solo en
  dispositivo. Queda en el Plan como prioridad alta.
- (E) **Punto 4 — ORBE pinna + pitch — NO ahora.** Diseño DSP grande.
- (F) **Punto 5 — SofaHRTFLoader SOFA crudo — NO ahora.** Cambio grande de
  loader (HDF5, memoria, tiempo de carga).

## Qué se hizo (parche 0022)

### 1. Medidor THD+N (nuevo, `app/src/main/cpp/dsp/ThdMeter.hpp`, header-only)
La instrumentación que produce la evidencia física que la crítica exige.
Sin dependencias de Android: la misma matemática corre en host (CTest) y en
el análisis de las grabaciones del teléfono.
- Método: ajuste por mínimos cuadrados de un seno a la fundamental (para un
  número entero de ciclos, seno y coseno son ortogonales). THD+N =
  (potencia total − potencia fundamental) / fundamental. Sin FFT ni ventanas:
  exacto para tonos con ciclos enteros, robusto (el residuo incluye TODO lo
  que no es la fundamental).
- Reporta: THD+N (dB y %), fundamental (dBFS), total/RMS/pico (dBFS),
  muestras recortadas (clipping), válido/no-válido (sin fundamental medible).
- `fillTestTone()`: generador de tono de prueba (seno puro o con 2º
  armónico a nivel conocido) para validar el medidor y para el protocolo en
  dispositivo.

### 2. Tests de host (`app/src/main/cpp/tests/test_thd_meter.cpp`, 7 tests)
Números exactos, no aproximados:
- Seno puro: THD+N < −120 dB (límite de float); fundamental −6.02 dBFS,
  RMS −9.03 dBFS (seno 0.5) exactos.
- Seno + 2º armónico a −40 dB: THD+N = −40.00 dB (±0.5 dB), 1.0001%.
- Seno + 2º armónico a −60 dB: THD+N = −60.00 dB (±0.5 dB), 0.1%.
- Seno recortado: THD alto, clippedSamples > 0, pico 0.0 dBFS.
- Silencio y parámetros inválidos: valid=false, sin crash.
- fillTestTone genera el seno exacto (pico 0.25, cuarto de ciclo en muestra 12).

### 3. Herramienta de análisis host (`tools/measure/thd_analyze.cpp`)
Lee un WAV PCM 16-bit (mono/estéreo, usa canal izquierdo) y reporta THD+N
con la misma matemática. Validada end-to-end:
- Tono puro 1 kHz 16-bit: THD+N = **−91.09 dB** (piso de cuantización
  16-bit a −6 dBFS, correcto), fundamental −6.02 dBFS.
- Tono + 2º armónico −40 dB: THD+N = **−40.00 dB** exacto.
Uso: `thd_analyze <archivo.wav> <frecuencia_Hz> [tasa]`. Recorta al mayor
número entero de ciclos de la fundamental.

### 4. Medición en dispositivo (protocolo + intento)
**Tono de prueba:** seno 1 kHz, amplitud 0.5 (−6 dBFS), 5 s, 48 kHz/16-bit
mono (`/tmp/tone_1k.wav`, 480044 B). Transferencia al teléfono por SCP
(vía proxy Tailscale) en curso al cierre de este reporte; el teléfono
tiene `tinycap` (`/system/bin/tinycap`) para grabar la salida del altavoz.

**Protocolo exacto (para Esse o para una corrida con el teléfono receptivo):**
1. Copiar el tono: el archivo debe estar en `/sdcard/Download/tone_1k.wav`.
2. Grabar la salida del altavoz mientras suena el tono (micrófono):
   `su -c 'tinycap /sdcard/rec_ivanna.wav -D 0 -d 0 -c 1 -r 48000 -b 16 -p 1024 -n 4 -t 6'`
   (tinycap graba 6 s; el tono dura 5 s).
3. Reproducir el tono POR IVANNA (MÚSICA, BridgePlayer, DSP activo) mientras
   graba; repetir reproduciéndolo por Files (reproductor del sistema, sin
   IVANNA) para la línea base. El altavoz y la sala son comunes a ambas:
   la DIFERENCIA de THD+N es lo que IVANNA aporta.
4. Bajar las grabaciones y analizarlas en host:
   `thd_analyze rec_ivanna.wav 1000` y `thd_analyze rec_files.wav 1000`.
5. **Qué valor cierra el punto:** THD+N medido en ambas grabaciones (número
   real, no estimado) + latencia (dumpsys audio_flinger, conocida: 21.00 ms)
   + underruns (logcat `IVANNA.Telemetry`, campo xruns) + clipping (pico
   reportado por thd_analyze; clipped=0 es lo esperado).
**Estado al cierre:** el SSH al teléfono está intermitente (conexiones
cerradas, timeouts) y la descarga de la 457 consume ancho de banda; el
protocolo queda listo y el tono en transferencia. NO se inventaron
mediciones de dispositivo: los únicos números de dispositivo en este
reporte son los verificados antes (latencia AudioFlinger 21.00 ms,
2026-11-15) y los de host (arriba).

## Archivos (0022)
- `app/src/main/cpp/dsp/ThdMeter.hpp` (nuevo)
- `app/src/main/cpp/tests/test_thd_meter.cpp` (nuevo, 7 tests)
- `app/src/main/cpp/tests/CMakeLists.txt` (+13 líneas: target test_thd_meter)
- `tools/measure/thd_analyze.cpp` (nuevo, herramienta host; no va al APK)

## Validación (números reales, sin fabricar)
- test_thd_meter: **7/7 PASSED** (host directo, g++-14; también en CTest
  completo, scratch ~/workspace/build0021).
- thd_analyze end-to-end: seno puro −91.09 dB; seno+2º −40 dB → −40.00 dB.
- CTest completo (con 0021 + 0022): **1090/1090 PASSED, 0 fallos (100%)**,
  56.90 s, scratch ~/workspace/build0021 (2026-11-16 ~08:05 CST).
  Base 1060 + 23 (0021) + 7 (0022) = 1090.
- Suite evolución: **168/168 OK (169.0 s)** tras corregir 2 pins
  desactualizados (ver "Fixes a la suite" abajo). La suite pasó de 167 a
  168 tests (+1 pin 0022: ThdMeter.hpp clasificado).
- check_docs_claims: **PASS, 0 hallazgos** (exit 0, 2026-11-16).
- **Incidente CMakeLists:** el commit `1adc5fe6` del coordinador Repo
  Perfecto barrió por accidente los hunks 0021/0022 de
  `tests/CMakeLists.txt` y `18276f9f` los quitó (dejó solo su RUN_SERIAL).
  Los hunks se re-aplicaron íntegros (61 líneas) y el CTest completo se
  re-validó **1090/1090 en 59.94 s** sobre el árbol restaurado.

## Fixes a la suite de evolución (pins desactualizados, no debilitamiento)
1. **test_0011 (header wiring):** `dsp/ThdMeter.hpp` aparecía como
   huérfano de producción (1). Es primitiva de medición host: sus únicos
   consumidores son test_thd_meter.cpp (TU de tests/, excluida del grafo
   por diseño) y tools/measure/thd_analyze.cpp (herramienta host, no TU
   CMake del APK). NO va en la ruta PCM del APK. Fix: clasificado en
   `STANDALONE_HEADERS` (`scripts/check_header_wiring.py`, comentario
   0022 con su razón) + test nuevo
   `test_0022_thd_meter_is_classified_standalone`. El check vuelve a
   [PASS] 0 huérfanos. El invariante se mantiene: 0 huérfanos sin
   clasificar.
2. **test_0019 (TODO scanner):** el TODO route-arbiter-v2 se movió de la
   línea 1803 a la 1809 por el fix(rtlog) `598db138` ya en HEAD (+6 líneas
   netas en `ivanna_omega_jni.cpp` antes del TODO, drenaje
   rtLogDrainCallbackForShed). Fix: pin actualizado a 1809 con comentario.
   El invariante se mantiene: exactamente 1 TODO, y es el route-arbiter-v2.

## PLAN PRIORIZADO — resto de la crítica (orden, qué se necesita, qué evidencia lo cierra)

### Prioridad 1 — Punto 2: Bofo / filtro peine en captura (~130 ms)
- **Qué se necesita:** determinar en código si la mezcla es (a) acústica
  (el altavoz suena el original + IVANNA lo recaptura y lo reinyecta) o
  (b) en el mezclador (dos AudioTracks simultáneos). Leer
  PlaybackCaptureService + la ruta de re-inyección; medir el retardo real
  entre original y reinyectado (correlación cruzada en una grabación).
- **Evidencia que lo cierra:** grabación que muestre el peine (muescas
  periódicas en el espectro, período = 1/retardo) ANTES del fix y espectro
  limpio DESPUÉS; o demostración de que la re-inyección no duplica la señal.
- **Riesgo:** el fix puede ser no re-inyectar (solo analizar) o compensar
  el retardo; cambiarlo afecta la experiencia de escucha de Tidal/Qobuz.
  Requiere luz verde de Esse para probar en vivo.

### Prioridad 2 — Punto 6/1-RIR: verificar si la RIR está realmente en 0
- **Qué se necesita:** leer en vivo (logcat `IVANNA.Telemetry` / snapshot)
  el wetDry efectivo del RirConvolver y el `room_wet` del snapshot con
  IVANNA sonando. El default del snapshot YA es 0.22/0.34 y el permiso es
  1.0, así que es posible que la crítica esté desactualizada.
- **Evidencia que lo cierra:** telemetría que muestre `wetDry > 0` y
  `rir_active=1` con audio sonando, + una grabación A/B (sala ON/OFF) que
  demuestre la cola de reverberación. Si está en 0, encontrar el gate que
  la apaga (snapshot no publicado, permiso 0, dataset sin salas) y
  corregirlo con test en host.
- **Nota:** la RIR NO se enciende a ciegas: si estaba apagada por artefactos
  o coste, encenderla sin medir degradaría (prohibido).

### Prioridad 3 — Punto 1 (resto): módulos en passthrough
- **Coclear:** entender POR QUÉ se apagó ("FIX artefactos: era 1.0f").
  Reproducir el artefacto en host (tono + coclear a 1.0), caracterizarlo
  (¿qué frecuencia/condición?), y reactivar con intensidad segura
  (p. ej. 0.35) con test que demuestre que el artefacto no vuelve.
  Evidencia: THD+N con coclear ON vs OFF (thd_analyze) + escucha de Esse.
- **Crossfeed ITD:** es para AUDÍFONOS (strength 0 por defecto). Esse no
  tiene audífonos; no aplica a su cadena (bocinas/DAC USB). NO se activa
  para bocinas. Se documenta como "no aplicable a la cadena de Esse" y se
  deja opt-in para quien use audífonos. Evidencia: ninguna necesaria (es
  una decisión de producto, no un bug).
- **Bucle neuronal (identidad) / IA genérica (passthrough):** no hay
  hardware biométrico ni inferencia neuronal acústica activa. La vía
  honesta es: (a) el sensor acústico por micrófono (Φ, 449/450) YA es el
  bucle cerrado real — documentarlo como tal; (b) la IA genérica es el
  asistente (voz/parámetros), no DSP — no presentarla como procesamiento
  neuronal de audio. No se "activa" una red que no existe; se corrige la
  narrativa con hechos (esto lo lleva el README simple, no un parche DSP).
- **Wi-Fi CSI:** sin proveedor verificado. No se implementa un proveedor
  que no existe. Se retira de la lista de capacidades activas (sin borrar
  el código; se marca como no disponible) o se deja como investigación.
  Evidencia: ninguna (no hay hardware/proveedor).

### Prioridad 4 — Punto 4: ORBE pinna + pitch
- **Qué se necesita:** incorporar la respuesta del pabellón (pinna) a ORBE.
  La HRTF medida (SOFA/IHR1) YA contiene la pinna; ORBE usa hoy solo el
  modelo esférico (ITD Woodworth + ILD). El trabajo es usar la HRTF completa
  (o su parte pinna) para la elevación y el frente/atrás, y añadir pitch
  (inclinación vertical) al head-tracking (hoy solo yaw).
- **Evidencia que lo cierra:** test en host que demuestre diferencia
  medible entre arriba/abajo y frente/atrás (hoy ORBE no las distingue) +
  escucha de Esse con head-tracking.
- **Orden:** después de las mediciones (0022) y del bofo/RIR, porque es
  diseño DSP grande.

### Prioridad 5 — Punto 5: SofaHRTFLoader interpreta SOFA crudo
- **Qué se necesita:** leer Data.IR (HRIR medidas) del SOFA en vez de
  reconstruir desde la base PCA compilada. Implica parseo HDF5 en runtime
  (o precarga), memoria y tiempo de carga.
- **Evidencia que lo cierra:** test en host que cargue un SOFA real y
  compare la HRIR reconstruida vs la medida (error en dB) + medición de
  coste (tiempo/memoria).
- **Orden:** después de ORBE-pinna (depende de la misma HRTF); es el
  cambio más grande de loader.

### Prioridad 6 — Punto 8: certificaciones (AES, IEC 60268, ISO/IEC 25010)
- **Qué se necesita:** evaluación, no código. Determinar qué normas son
  auto-certificables (mediciones propias con protocolo documentado, como
  0022) y cuáles exigen laboratorio acreditado (coste, fuera de alcance
  con gasto cero). Documentar la evaluación.
- **Evidencia que lo cierra:** documento de evaluación con el alcance
  realista (p. ej. "IEC 60268-3: medible con 0022; certificación formal
  requiere laboratorio").

### Prioridad 7 — Punto 9: Ruta B pausada
- **Qué se necesita:** luz verde EXPLÍCITA de Esse para probar el daemon v2
  (nunca sin ella). El wrapper HAL está muerto (6º reset) y NO se reintenta.
  La vía es el módulo v3 (efecto + daemon) con kill switch, o estabilizar
  la captura en Ruta A (puntos 1–2 de este plan).
- **Evidencia que lo cierra:** acoplamiento al mezclador sin latencia
  medido (fusión activa, ratio ≠ 1.0, sin bofo) o captura Ruta A estable
  (sin metralleta, sin clicks, sin peine).

### No son parches de código (van por otra vía)
- **Punto 7 (TIDAL):** MÚSICA local ya es la vía principal (447); TIDAL es
  LEGACY. Acuerdo comercial oficial: probabilidad honesta ~1/10 (dicho a
  Esse 2026-11-12). No es trabajo de esta corrida.
- **Punto 10 (ventas 0 / XDA):** el README simple (coordinador repo-perfecto)
  ya va en esa dirección. Las ventas llegan con el producto medido (0022)
  + el resto del plan, no con un parche.
- **Punto 11 (BridgePlayer metralleta):** fix 0020 ya en la 457;
  verificación en vivo pendiente (no se rehace).

## Qué quedó pendiente (esta corrida)
- CTest completo + suite evolución + check_docs_claims con 0021+0022
  (build en curso en scratch).
- Medición acústica en dispositivo (protocolo listo; tono en transferencia;
  SSH intermitente + descarga 457 en curso). Los números de dispositivo NO
  se inventaron.
- 0021 (tests de módulos) queda validado por el mismo CTest completo.
