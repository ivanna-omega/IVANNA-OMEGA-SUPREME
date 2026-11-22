# 0021 — Tests para los 3 módulos sin tests restantes

## Decisión del Consejo Supremo (2026-11-16 07:29–07:35 CST)

**Orden de Esse (07:28 CST):** que trabajen TODOS, incluidos los pro, sin
llevarse la eternidad, y OBLIGATORIAMENTE tomar una DECISIÓN para IMPLEMENTAR
lo más viable. Los pro (gemini-4-argon, gemini-3.1-pro-preview) siguen en
cooldown de 24 h hasta 2026-11-17 ~02:00 CST (verificado 07:20 CST: las 3
llaves Gemini, incluida la nueva de kimosibernet, en enfriamiento). Se trabajó
con los INTERINOS, sin esperarlos.

**Deliberación — RONDA 1 (unánime, sin necesidad de más rondas):**
- **PROPOSER** (ddgw/tinfoil/gpt-oss-120b, interino): recomienda **(e)**.
- **REVIEWER** (ddgw/gpt-5.6-luna): recomienda **(e)**.
- **ARBITER** (ddgw/gpt-5.4-mini): **CONFIRMA (e)**.

Primera aprobación del consejo (rompe el 0/7). Los pro no participaron
(cooldown); la decisión es de los interinos + el coordinador, y es unánime.

**Candidatos evaluados (criterios: viable de implementar Y validar en host
en esta corrida; impacto real; riesgo bajo de quedarse a medias):**
- (a) **0021 click 1 Hz en captura — RECHAZADO para esta corrida.** Síntoma
  real y probado (click 1 Hz con IVANNA sonando, se va con force-stop), pero
  causa raíz NO confirmada. El snap YA está desactivado (legacy,
  pass-through) tras 0020, así que no es el snap. El poller de telemetría
  solo hace Log.i (no escribe audio). Requiere investigar
  PlaybackCaptureService.kt (1870 líneas: PLL + ASRC + AudioTrack +
  antiPopEngine + masterTiming). Alto riesgo de no cerrar en esta corrida.
  Queda como candidato 0022 (su propio fix, con investigación de causa).
- (b) **NEU·Ω Fase 3 (cola E5) — RECHAZADO.** Fase grande, mucho código
  nuevo, validación compleja; no cabe en una corrida.
- (c) **Ω v2 — RECHAZADO.** Diseño grande, especificación no cerrada.
- (d) **Elevar HOLOS — RECHAZADO.** Vago: sin cambio concreto definido.
- (e) **Tests para los 3 módulos sin tests — ELEGIDO.** Concreto, definido,
  patrón probado (0013–0018), validable en host, cierra el pendiente de
  auditoría "15 módulos sin tests" (11 ya cubiertos por el guard 0014).

## Estado real de los 3 módulos (verificado en código, no asumido)

- **audio_orchestrator.cpp (19 KB): COMPLETAMENTE sin tests.** Solo una
  referencia de cableado de header en test_phase4_5_header_rt_wiring.cpp.
  **Compila y enlaza en host** con sus dependencias reales
  (audio_control_plane.cpp + evolutionary_kernel_v2.cpp) usando host_stubs.
- **usb_audio_pro_manager.cpp (43 KB): núcleo YA testado, gestor sin tests.**
  Su algoritmo (usb_asrc_kernel / usb_asrc_clock_servo / usb_asrc_cutoff)
  YA tenía tests (test_usb_asrc_cutoff, test_usb_asrc_spectral_response,
  test_usb_asrc_clock_servo). Lo que faltaba era el PROPIO gestor (.cpp).
  **Compila y enlaza en host** tras extender el shim JNI (ver abajo).
- **omega_effect.cpp (181 KB): beacon y ABI YA testados; lógica BLOQUEADA.**
  El beacon (test_effect_beacon, que también cubre omega_effect_evidence.h)
  y el ABI (test_audio_effect_abi_layout, test_audio_effect_library_tag) YA
  existen. La lógica del efecto (omega_process) NO enlaza en host: 198
  símbolos sin resolver (HrtfManager, RirConvolver, SaFOptimizer,
  EvolutionaryEQ, Psychoacoustics, TinyML, HoaBinauralDecoder,
  IntelligentUpmixer, AntiDolby, ivanna_asrc_bridge, ...) — el mismo
  bloqueo documentado para IvannaFusionCore.cpp (que omega_effect.cpp
  incluye directamente). **NO se finge cobertura: se documenta el bloqueo.**

## Qué se hizo

### 1. test_audio_orchestrator.cpp (nuevo, 14 tests)
Compila el orquestador REAL + audio_control_plane.cpp +
evolutionary_kernel_v2.cpp en host. Verifica comportamiento, no
implementación (números medidos en host antes de escribir los asserts):
- Seguridad: buffer nulo / 0 muestras / muestras negativas / tasa fuera de
  rango (100 Hz y 1 MHz → 48000) no cuelgan.
- Sanitizador: NaN/+Inf/-Inf en entrada salen como 0 y nunca como NaN/Inf.
- Silencio entra, silencio sale.
- Ganancia master −6 dB aplica 0.501× exacto (medido: ratio 0.501).
- Master +100 dB se clampa a +24 dB (medido: pico 0.1585 = 0.01×15.849).
- EQ +100 dB se clampa a +12 dB (medido: pico 0.0398 = 0.01×3.981).
- Setters ignoran NaN (la ganancia no cambia; si se aceptara, la salida
  sería NaN).
- Anti-Dolby: scores se guardan y se clampan a [0,1]; silencio =
  1−(voz+música+graves); NaN se ignora.
- Width/exciter/HRTF se clampan a [0,1] (observables en g_control_frame /
  g_hrtf_wet_dry). Flush HRTF levanta el flag.
- Widener con señal caliente (0.9, width 1.0) nunca supera ±1.0 (medido:
  pico 0.8897; soft-clip knee 0.9 + clamp duro).
- LUFS/pico BS.1770-4 finitos y en rango sano (medido: LUFS −16.00,
  pico −6.02 dBFS para seno 0.5). **Corrección durante la validación:** el
  pico del medidor es MÁXIMO DE SESIÓN (retiene el máximo), no por bloque:
  el primer assert (== −6.02) falló porque un test previo (widener, seno
  0.9) lo dejó en −0.915. Se corrigió el assert a sesión-máximo
  (>= −7.02, <= 0.0) con el porqué documentado en el test.
- Mono funciona (RMS 0.3536 para seno 0.5).

**Fix colateral (1 línea, header):** `ivanna_set_exciter_amount` existía
en el .cpp y la usaba ivanna_jni_stub.cpp, pero FALTABA en
audio_orchestrator.h. Se añadió la declaración (la API queda completa).

### 2. test_usb_audio_pro_manager.cpp (nuevo, 9 tests)
Compila el gestor REAL en host. Verifica comportamiento fail-closed SIN
dispositivo USB (no se arranca el motor: sin DAC real, el hilo haría
ioctls USB sobre un fd que no es USB):
- Inactivo por defecto (ivanna_usb_pro_active() == false).
- Escribir sin motor devuelve 0 (nativeWriteFrames con array, con null,
  con 0 frames; ivanna_usb_pro_write con array y con null).
- Telemetría sin motor: stats a cero, feedback ausente, no isócrono.
- Arrancar con fd inválido (−1) no arranca. Parar sin motor es seguro.
- ConfigureEndpoint fija el estado de feedback (hasFeedback=1, ep 0x81) y
  el centinela feedbackEpAddress<=0 lo apaga (comportamiento pre-cambio).

**Extensión del shim JNI (host_stubs/jni.h, +24 líneas):** el gestor usa
NewIntArray/SetIntArrayRegion (nativeGetFeedbackInfo y nativeGetEngineStats
devuelven jintArray) y el shim solo tenía las variantes float. Se añadieron
NewIntArray / GetIntArrayElements / ReleaseIntArrayElements /
SetIntArrayRegion / GetIntArrayRegion, espejando el patrón float con
estáticos thread_local separados (para no pisar el estado float).

### 3. omega_effect.cpp — fix de compilación en host + bloqueo documentado
**Fix (3 líneas):** `rtLogDrainCallback` hacía
`static_cast<ivanna::rtlog::Event>(r.code)` donde `r.code` es
`std::atomic<uint8_t>` desde fix(rtlog) 8ef5fcf3 (payload atómico). Eso NO
compila con g++-14 (host). Se cambió a `r.code.load(relaxed)` — semántica
idéntica, válido en g++ y en Clang/NDK. Tras el fix, omega_effect.cpp
**compila en host** (solo un warning preexistente de AELI extern).
**Bloqueo de enlazado (documentado, no escondido):** enlazar el efecto
completo en host exige 198 símbolos de IvannaFusionCore y sus dependencias
(HrtfManager, RirConvolver, SaFOptimizer, EvolutionaryEQ, Psychoacoustics,
TinyML, HoaBinauralDecoder, IntelligentUpmixer, AntiDolby,
ivanna_asrc_bridge). Es el mismo bloqueo documentado para IvannaFusionCore.
El beacon, la evidencia y el ABI del efecto YA tienen tests; la lógica de
omega_process queda para cuando IvannaFusionCore sea enlazable en host
(o se extraiga su núcleo testable).

## Archivos
- `app/src/main/cpp/tests/test_audio_orchestrator.cpp` (nuevo, 14 tests)
- `app/src/main/cpp/tests/test_usb_audio_pro_manager.cpp` (nuevo, 9 tests)
- `app/src/main/cpp/tests/CMakeLists.txt` (+45 líneas: 2 targets)
- `app/src/main/cpp/tests/host_stubs/jni.h` (+24 líneas: int arrays)
- `app/src/main/cpp/audio_orchestrator.h` (+1 línea: declara ivanna_set_exciter_amount)
- `app/src/main/cpp/omega_effect.cpp` (+3 líneas: r.code.load() en rtLogDrainCallback)
- `telemetry/ctest_summary.json` (conteo actualizado tras la corrida completa)

## Validación (números reales, sin fabricar)
- test_audio_orchestrator: **14/14 PASSED** (en scratch ~/workspace/build0021).
- test_usb_audio_pro_manager: **9/9 PASSED** (mismo scratch).
- CTest completo (0021 + 0022): **1090/1090 PASSED, 0 fallos (100%)**,
  56.90 s, scratch ~/workspace/build0021 (2026-11-16 ~08:05 CST).
  Base 1060 + 23 (0021) + 7 (0022) = 1090.
- Suite evolución: **168/168 OK (169.0 s)** tras corregir 2 pins
  desactualizados revelados por el parche (no por el parche en sí) —
  test_0011 (ThdMeter.hpp huérfano, clasificado en STANDALONE_HEADERS)
  y test_0019 (TODO route-arbiter-v2 movido 1803→1809 por el fix rtlog
  598db138 ya en HEAD). Ambos corregidos en esta corrida (detalle en
  REPORT 0022). La suite pasó de 167 a 168 tests (+1 pin 0022).
- check_docs_claims: **PASS, 0 hallazgos** (exit 0, 2026-11-16).
- **Incidente CMakeLists:** el commit `1adc5fe6` del coordinador Repo
  Perfecto barrió por accidente los hunks 0021/0022 de
  `tests/CMakeLists.txt` y `18276f9f` los quitó (dejó solo su RUN_SERIAL).
  Los hunks se re-aplicaron íntegros (61 líneas) y el CTest completo se
  re-validó **1090/1090 en 59.94 s** sobre el árbol restaurado.

## Qué quedó pendiente
- **0022 (click 1 Hz, ruta de captura):** su propio fix. Requiere
  investigación de causa raíz en PlaybackCaptureService.kt (el snap ya está
  descartado; el poller solo loguea). Verificación en vivo solo en el
  teléfono de Esse (escucha).
- **omega_effect.cpp enlazado en host:** bloqueado por IvannaFusionCore
  (198 símbolos). Cuando el núcleo sea enlazable, cablear un test de
  omega_process fail-closed (passthrough bit-exacto sin FusionCore).
- **NEU·Ω Fase 3, Ω v2, HOLOS:** siguen pendientes del plan omnipotencia
  (rechazados para esta corrida por tamaño/vaguedad, no por falta de valor).
- **Módulos sin tests restantes:** 13 (de 15; audio_orchestrator y
  usb_audio_pro_manager ya tienen; omega_effect queda bloqueado con su
  beacon/ABI testados). IvannaFusionCore.cpp sigue bloqueado para host.
