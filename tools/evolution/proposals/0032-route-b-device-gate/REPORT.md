# 0032 — ORBE/HOLOS/widener conscientes del dispositivo de salida (fix HPF en bocinas)

## 1. Problema

Tras correr la APK, **TODAS las apps suenan como filtro paso alto** (sin graves). El módulo Magisk SOLO suena bien. El único arreglo era reiniciar IVANNA y cerrar la app de música. Reportado en vivo por Esse, 2026-11-16 ~16:00–16:21 CST.

## 2. Evidencia

- **Módulo solo (sin APK desde el arranque): suena BIEN.** El fix 0023 (base del daemon en `makeNeutral()`) funciona: el módulo en reposo es neutro.
- **Al prender la APK: paso alto.** Y persiste hasta reiniciar IVANNA + cerrar la app de música.
- **Prueba decisiva (16:21 CST): con OTRAS aplicaciones.** Tras correr la APK, música de Files/Tidal también suena con el filtro. Eso prueba que el estado está en el **daemon/módulo (Ruta B)**, no solo en la cadena interna de la APK (Ruta A): el efecto de AudioFlinger procesa todo el audio del teléfono.

## 3. Mecanismo (verificado en código)

`PersistedStateRestorer.kt` manda 3 `SET_PERCEPTUAL_STATE` al daemon del módulo al arrancar:

| Comando | Valores que manda | Efecto en Ruta B |
|---|---|---|
| General (`OmegaEngineBridge.sendPerceptualState`) | spatialWidth=1.55, harmonicGain=0.78, antiDolby=0.85, compressor=-5.5, loudnessTarget=-16 | widener 1.55x + coloración |
| ORBE (`MagiskBridge.sendCommand`) | orbeAmount=1.0 (default), distance 0.35, spread 1.0, proximity 3.0, diffuse 0.5 | render binaural completo |
| HOLOS (`MagiskBridge.sendCommand`) | holosAmount=0.5 (default) + 12 params | operador de campo activo |

El daemon (`ivanna_daemon.cpp`, bucle principal) adopta el snapshot de la app cada vuelta — "TODOS los efectos de la APK llegan a la Ruta B" — y lo publica al efecto de AudioFlinger. El fix 0023 solo puso la **BASE** del daemon en `makeNeutral()` (HRTF OFF, widener 1.0); en cuanto la APK arranca y publica su estado "magistral", el módulo vuelve a colorear.

Por qué suena a paso alto: **ORBE es binaural (para audífonos)** — aplicado por el módulo a bocinas, sus claves espectrales no funcionan y el sonido se adelgaza; el **widener** (M/S) se come el medio donde viven los graves; HOLOS suma su propio campo. La combinación = graves ausentes = filtro paso alto.

## 4. Diseño

**ORBE/HOLOS/widener conscientes del dispositivo de salida.** NO se apagan (siguen ACTIVOS por defecto, directiva Esse 2026-11-08): esto es **ENRUTADO por dispositivo, no apagado**.

- **Audífonos** → los montos del usuario pasan tal cual al daemon.
- **Bocinas** → neutro: orbeAmount=0, holosAmount=0, spatialWidth=1.0, harmonicGain=0, antiDolby=0.
- **Compressor, loudnessTarget y exciterRed TAMBIÉN se neutralizan en bocinas** (extensión 2026-11-16, ver §10): compressor=0 (bypass), loudnessTarget=0 (bypass), exciterRed=0. El set neutro original no los tocaba; el reporte en vivo de Esse (volumen que baja + sonido opaco al instante de prender la APK) mostró que el compresor y el loudness eran parte del síntoma, no solo ORBE/HOLOS/widener.
- **Fail-closed:** sin AudioManager, sin dispositivos, o si la lectura falla → neutro. Nunca se mandan montos binaurales sin un audífono determinado.

### Clasificación por tipo (no por ruta)

`AudioRouteManager.detectOutputRoute()` colapsa USB_HEADSET y USB_DEVICE en la misma ruta USB, así que el gate NO puede reusarlo: debe ser por **tipo de dispositivo** (`AudioDeviceInfo.type`, API 23+).

| Clase | Audífonos (pasan montos) | Bocinas (neutro) |
|---|---|---|
| USB | TYPE_USB_HEADSET (22) | TYPE_USB_DEVICE (13), TYPE_USB_ACCESSORY (15) |
| BT | TYPE_BLUETOOTH_A2DP (8), TYPE_BLE_HEADSET (26), TYPE_HEARING_AID (23), TYPE_BLE_HEARING_AID (33) | TYPE_BLE_SPEAKER (27) |
| Cableado | TYPE_WIRED_HEADSET (3), TYPE_WIRED_HEADPHONES (4) | TYPE_AUX_LINE (19), TYPE_LINE_ANALOG (5) |
| Resto | — | TYPE_BUILTIN_SPEAKER (2), TYPE_HDMI (9), desconocido, sin dispositivo |

**Precedencia por clase (USB > BT > cableado > bocina)**, igual que `OutputRoutePolicy.select`: `getDevices(GET_DEVICES_OUTPUTS)` enumera TODOS los endpoints conectados, no la ruta activa de AudioPolicy. La clase ganadora decide; dentro de cada clase, el tipo audífono gana al tipo bocina. Caso mixto ambiguo (DAC USB + audífono BT conectados) → gana USB → neutro (fail-closed hacia no colorear bocinas).

### Dónde se aplica

1. **Arranque** — `PersistedStateRestorer.restore()`: resuelve el gate UNA VEZ y lo aplica a los 3 comandos. Los 3 envíos se extraen a helpers compartidos (`sendOrbeToDaemon`, `sendHolosToDaemon`, `sendGeneralToDaemon`).
2. **Cambio de salida** — nuevo `PersistedStateRestorer.pushRouteBState(context)`, llamado por `AudioRouteManager.applyRoute()` (que ya tiene `AudioDeviceCallback` registrado en `start()`). Va ANTES del early-return de `applyRoute`: un cambio DAC USB_DEVICE → audífono USB_HEADSET no cambia la categoría (sigue USB) pero sí el gate; sin este re-envío el daemon se quedaría en neutro con audífonos puestos. Corre en hilo propio (`Thread "RouteBStatePush"`): el envío es socket con timeout 500 ms y el callback corre en el hilo principal.
3. **Ruta A NO se toca**: los JNI `nativeSet*` (ORBE/HOLOS internos) siguen con los montos del usuario siempre. El gate solo enruta lo que se manda al daemon.

## 5. Archivos

| Archivo | Cambio |
|---|---|
| `app/src/main/java/com/ivanna/omega/audio/RouteBDeviceGate.kt` (NUEVO) | Gate puro (`gate`/`isHeadphonesOutput` sobre tipos) + adaptadores `AudioManager` + `ROUTE_B_NEUTRAL_AMOUNTS` (top-level); **extensión: `RouteBGeneralAmounts` + `ROUTE_B_NEUTRAL_GENERAL_AMOUNTS` (todo 0) + `gateGeneral` (2 overloads)** |
| `app/src/test/java/com/ivanna/omega/audio/RouteBDeviceGateTest.kt` (NUEVO) | 12 tests unitarios; **extensión: +4 tests de gateGeneral (16 total)** |
| `app/src/main/java/com/ivanna/omega/core/PersistedStateRestorer.kt` | Gate en arranque + 3 helpers + `pushRouteBState()`; **extensión: `sendGeneralToDaemon` usa `gateGeneral`** |
| `app/src/main/java/com/ivanna/omega/audio/AudioRouteManager.kt` | Hook de re-envío en `applyRoute()` |
| `app/src/main/java/com/ivanna/omega/magisk/OmegaEngineBridge.kt` | **Extensión: rango sanitizador de loudness `[-30, -5]` → `[-30, 0]`** (para que el 0 neutro pase; los llamadores con -16 no cambian) |

## 6. Tests

`RouteBDeviceGateTest` (12 tests, estilo `DirectRouteProbeTest`, JUnit4 en host):

- Genezi DAC (USB_DEVICE) → neutro (todos los campos verificados).
- Bocina integrada, HDMI, LINE_ANALOG → neutro.
- A2DP, WIRED_HEADPHONES, WIRED_HEADSET, USB_HEADSET, HEARING_AID → montos del usuario intactos (`assertEquals` del data class completo).
- Sin dispositivos → neutro (fail-closed).
- USB_DEVICE + A2DP (mixto) → neutro (precedencia USB).
- A2DP + bocina → usuario (precedencia BT sobre bocina).

Montos de usuario de los tests = los defaults reales del teléfono de Esse: ORBE 1.0, HOLOS 0.5, spatialWidth 1.55, harmonicGain 0.78, antiDolby 0.85.

**Extensión (§10): +4 tests de `gateGeneral`** (16 total): `neutralGeneralAmountsAreZero`, `speakersNeutralGeneralAmounts`, `headphonesKeepGeneralAmounts`, `gateGeneralBooleanOverload`. Montos de usuario: compressor -5.5, exciterRed 0.15, loudnessTarget -16 (los valores reales que la APK manda al daemon).

## 7. Verificación

- **Tabla de decisión portada a Python (18 casos, 0 fallos):** los 12 casos de los tests + 6 bordes extra (USB_DEVICE+bocina = caso real de Esse, USB_HEADSET+USB_DEVICE, BLE_SPEAKER solo, AUX_LINE solo, BLE hearing aid, HEARING_AID+bocina).
- **Bug cazado por el puerto antes de pushear:** `TYPE_HEARING_AID` (23) solo caía al `return false` final — no estaba en ninguna clase. Fix: va en la clase BT (commit `3fef5c87`).
- **Llaves balanceadas** en los 4 archivos tocados (braces/parens/brackets = 0).
- **APIs verificadas contra la referencia oficial de Android** (`developer.android.com`, `AudioDeviceInfo`): `TYPE_HEARING_AID`=23, `TYPE_HDMI`=9, `TYPE_LINE_ANALOG`=5, `TYPE_BLE_HEARING_AID`=33 existen. `AudioManager.getDevices(GET_DEVICES_OUTPUTS)` y `AudioDeviceInfo.type` son API 23+ (minSdk 28, sin guarda). Lecciones 0030/459 aplicadas: NO `AudioDescriptor`, NO `IntArray?.orEmpty()`, NO símbolos de companion object desde tests (el neutro es `val` top-level; el gate es `object` con funciones miembro, patrón probado por `DirectRouteProbe`).
- **Sin SDK en host: el Kotlin solo se verifica en CI.** Pendiente: que la CI compile `compileReleaseKotlin` + corra `RouteBDeviceGateTest` (16 tests tras la extensión) al mergear a main.
- **Verificación en vivo pendiente (tras publicar la 460):** con el DAC JieLi conectado y la APK corriendo, música de Files/Tidal debe sonar con graves (sin HPF); al conectar audífonos BT, ORBE/HOLOS deben volver (escucha A/B); al desconectarlos, neutralizar sin reiniciar la app.
- **Extensión (§10): tabla portada a Python, 18/18 casos OK para AMBOS conjuntos** (espacial + general), incluido el caso real de Esse (USB_DEVICE → neutro completo). Llaves balanceadas en los 3 archivos Kotlin tocados por la extensión (los desequilibrios de conteo en `OmegaEngineBridge.kt` (-1) y `RouteAAsrc.kt` (parens -2) son preexistentes en HEAD — llaves en literales de cadena — no introducidos por la extensión).

## 8. Limitaciones

- **USB DAC + audífonos reales en el mismo adaptador:** Android sigue reportando `TYPE_USB_DEVICE` aunque haya audífonos conectados al DAC JieLi (el descriptor USB no distingue audífonos de la entrada analógica del amplificador). En ese caso el gate manda neutro y los audífonos pierden ORBE/HOLOS en Ruta B. Es la misma limitación que ya documenta `ExternalStereoPolicy` ("A USB descriptor cannot tell headphones from an amplifier's analog input"). **Follow-up (NO en esta propuesta):** override manual en UI ("estos audífonos van por USB-DAC") que fuerce el gate a audífonos para ese dispositivo.
- **Precedencia ≠ ruta activa de AudioPolicy:** `getDevices()` enumera endpoints conectados; si el usuario escucha por bocinas con un audífono BT meramente conectado (sin usar), el gate puede mandar montos de audífono. Es la misma heurística que ya usa `OutputRoutePolicy.select` en toda la app; el caso inverso (DAC conectado + audífono en uso) se resuelve fail-closed a neutro.
- **La Ruta A (cadena interna de la APK) NO está gateada aquí:** si Esse reproduce desde MÚSICA (BridgePlayer) por bocinas, la cadena interna sigue aplicando ORBE/HOLOS (activos por defecto). El síntoma reportado era con otras apps (Ruta B), que es lo que fixa esta propuesta. Gatear la Ruta A por dispositivo es follow-up separado (misma función pura, otro punto de aplicación).
- **El comando de upmixing NO se gatea:** `SET_PERCEPTUAL_STATE` de upmixing ya viene gateado por `hrtfAllowed` (ExternalStereoPolicy) y no es uno de los 3 comandos del problema. Sin cambio.
- **RIR/sala (setRoom/disableRoom) NO se gatea:** fuera del alcance; la sala RIR 0.22 del default no es el paso alto (el culpable es ORBE/widener).

## 9. Follow-ups

1. Override manual en UI para audífonos por USB-DAC (ver §8).
2. Gatear la Ruta A (cadena interna) por dispositivo con la misma función pura.
3. Telemetría: exponer el estado del gate (`route_b_headphones=0/1`) en el snapshot de telemetría para verificar en vivo qué decidió el gate (con su test actualizado en el mismo commit, por regla).
4. Decisión de Esse pendiente: umbral CTest de la Puerta (1008 vs 1125 real) — bloquea la Puerta, no esta propuesta.
5. `highCut` 19500 → 24000 Hz en el set neutro (no es la causa del opaco; follow-up).

## 10. Extensión: compresor + loudness + exciter en el set neutro (2026-11-16, reporte de Esse 18:26 CST)

**Síntoma nuevo, distinto del HPF:** Qobuz + módulo solo = BIEN. Al prender la APK, **INMEDIATAMENTE el volumen BAJA y el sonido se vuelve OPACO (sordo)**, sin tronidos. La caída es inmediata — no es deriva ni priming.

**Mecanismo verificado en código** (`magisk_module/ivanna_daemon.cpp` → `omega_apply_snapshot`): el snapshot general que la APK manda al daemon lleva `compressor=-5.5` y `loudnessTarget=-16 LUFS`. El compresor SÍ se aplica en el efecto (`apply_snapshot` lo aplica con valor < 0; `compressor=0` lo salta = bypass real). El `loudness_target` NO se usa en `omega_apply_snapshot` (el volumen no cae por loudness en el efecto), pero se neutraliza igual a 0 por coherencia con `makeNeutral()` (que ya lo pone en 0.0f) y porque el snapshot completo debe ser identidad en bocinas. `highCut` se queda en 19500 Hz (no es la causa; subirlo a 24000 es follow-up).

**Cambio:** `RouteBGeneralAmounts(compressor, exciterRed, loudnessTarget)` + `ROUTE_B_NEUTRAL_GENERAL_AMOUNTS` (todo 0f) + `gateGeneral` (overloads por `IntArray` y por `Boolean`, misma clasificación que `gate`). `PersistedStateRestorer.sendGeneralToDaemon` usa `gateGeneral`; el re-envío en `pushRouteBState()` cubre el cambio de dispositivo sin tocar código extra. `OmegaEngineBridge.sendPerceptualState`: el sanitizador de loudness pasaba `[-30, -5]` y clampaba el 0 neutro a -5 — ahora es `[-30, 0]`; los llamadores con target real (-16) no cambian.

**Verificación:** tabla de decisión portada a Python, 18/18 casos OK para AMBOS conjuntos (espacial + general), incluido el caso real de Esse (USB_DEVICE → neutro completo). Llaves balanceadas en los 3 archivos Kotlin tocados (los desequilibrios de conteo en `OmegaEngineBridge.kt` (-1) y `RouteAAsrc.kt` (parens -2) son preexistentes en HEAD — llaves en literales de cadena — no introducidos por esta extensión).
