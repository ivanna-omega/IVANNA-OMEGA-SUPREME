# Ruta B — Protocolo de verificación en dispositivo (para Esse)

> **Solo con la luz verde de Esse y el teléfono en la mano.** Este
> protocolo se ejecuta DESPUÉS de `route-b-install-recovery.md` (módulo
> instalado, daemon vivo, app abierta una vez). Sin verificación en
> dispositivo, Ruta B queda **PENDIENTE**, no "perfecta".

## 0. Definición de "efectiva" (v3 L4)

Ruta B es **efectiva** si y solo si hay una diferencia **MEDIBLE** entre
la entrada y la salida de la sesión de música:

- El beacon crece (`dsp_frames` sube) **Y**
- `perceptual_generation >= 1` con `orbe_amount` / `holos_amount`
  iguales a los valores de la app **Y**
- La salida difiere de la entrada de forma audible y medible
  (escucha A/B con kill switch: con Ruta B se nota el espacial;
  sin ella, no).

**No basta** con "se acopla", "el efecto existe" o "el beacon existe".
Un efecto acoplado con snapshot neutro (ORBE 0 / HOLOS 0) procesa
bloques pero suena igual que sin él: eso es **SNAPSHOT_NEUTRAL**, no
sano. El doctor lo diagnostica así desde la consolidación 2026-11-14.

## 1. Qué log leer: `IvannaOmegaEffect`

```sh
su -c "logcat -d -t 300 -s IvannaOmegaEffect OmegaCtrlBus"
```

Cada salida temprana de `omega_process()` deja una línea
`omega_process passthrough: <motivo>` (throttle 5 s por motivo).
**El log manda sobre el beacon**: el log dice POR QUÉ no procesa; el
beacon solo dice SI procesa.

| Línea del log | Puerta del doctor | ¿Fallo? |
|---|---|---|
| `!enabled && bypass silencioso` | NOT_ENABLED_NATIVE | Sí (el auto-enable no se disparó; re-acoplar) |
| `canales != 2 (no estereo)` | NOT_STEREO | **No**: correcto (mono/5.1 pasa intacto por diseño) |
| `integrity_self_check FALLO` | INTEGRITY_FAIL | Sí (`.so` en ruta no legítima; reinstalar el módulo) |
| `antitamper LOCKDOWN` | ANTITAMPER_LOCKDOWN | Sí (debugger/hook en audioserver; quitarlo) |
| `fusionCore NULO` / `FusionEngine lazy FALLO` | FUSION_CORE_NULL | Sí (el motor no se creó; re-acoplar lo reintenta) |
| `bypass beginBlock fallo` | BYPASS_BEGIN_BLOCK_FAIL | Sí (fallo de la instancia; re-acoplar) |
| `buffers RT nulos` | RT_BUFFERS_NULL | Sí (SET_CONFIG jamás llegó o calloc falló; re-acoplar) |
| `beacon open(...) failed` | BEACON_FILE_MISSING | Sí (el daemon no pre-creó el beacon; ver §3) |
| *(ninguna línea passthrough)* | — | El efecto no está cortado por ninguna puerta: si aun así no se nota, mirar §4 (snapshot neutro) |

Líneas de vida (el efecto SÍ procesa):

```sh
omega_process: auto-enable (EFFECT_CMD_ENABLE no llego del HAL; beacon onEnable)
omega_process: FusionEngine creado lazy (1er bloque)
```

Si NO aparece `FusionEngine creado lazy` tras sonar 10 s, el motor no
se creó: puerta FUSION_CORE_NULL (o el efecto nunca recibió bloques).

## 2. Qué dumpsys leer

```sh
su -c "dumpsys media.audio_flinger" | grep -A 15 -i "omega\|Effect 899"
su -c "dumpsys audio" | grep -i "ivanna\|omega" | head
```

Valores esperados con Tidal sonando:

- Un efecto `omega` en la sesión real de Tidal (la que descubre el
  daemon; en 2026-11-13 era la 2169, cambia en cada reproducción),
  `Enabled`, con tracks activos.
- Sesión huérfana (efecto existe pero sin hilo de salida / frames
  congelados) → puerta ORPHAN_FROZEN: el doctor lo re-acopla solo.

## 3. GET_STATUS (el daemon responde)

Por el socket abstracto `@omega_daemon_socket` (lo hace la app; por
shell, usar el cliente del módulo o leer el heartbeat). Campos y
valores esperados con Tidal sonando y la app abierta:

| Campo | Esperado | Si no |
|---|---|---|
| `ok` | `true` | Daemon caído o socket caído → DAEMON_UNREACHABLE |
| `discovered_session_id` | `> 0` (sesión real de Tidal) | `0` → NO_SESSION_DISCOVERED (sin música o descubrimiento fallido) |
| `effect_state` | `2` (PROCESSING) | `1` ENABLED_IDLE (sin bloques recientes), `0` NO_EFFECT, `-1` sin beacon |
| `dsp_frames` | **Creciendo** entre dos lecturas separadas 5 s | Congelado → ORPHAN_FROZEN / BEACON_NEVER_EXISTED |
| `dsp_applied_generation` | `== snapshot_generation` | `<` → el efecto no ha aplicado el último snapshot |
| `snapshot_generation` | `> 0` y creciendo | `0` → el daemon no publica (SHM caída) |
| `perceptual_generation` | `>= 1` (tras abrir la app) | `0` → **SNAPSHOT_NEUTRAL**: la app nunca envió su estado; abrir la app |
| `orbe_amount` | `==` ORBE de la app (`1.0` por defecto) | `0` con `perceptual_generation == 0` → neutro por ausencia |
| `holos_amount` | `==` HOLOS de la app (`0.5` por defecto) | `0` con `perceptual_generation == 0` → neutro por ausencia |
| `status` | `bypass` (ver nota) | — |

### Nota: `fusion_active=0`, `psi_active=0` y `ratio=1.0` NO son fallos

- `fusion_active=1` exige veredicto LOADED, que exige que el músculo
  HAL mida la deriva. El músculo es el ASRC del HAL por LD_PRELOAD,
  **retirado por diseño**: el veredicto es BYPASS siempre y
  `fusion_active=0`, `psi_active=0` **siempre**. No es evidencia de que
  el efecto no procese; es una métrica estructuralmente incapaz de
  valer 1 en esta arquitectura. La señal de procesado real es el
  beacon (`effect_state == 2` + `dsp_frames` creciendo).
- `ratio=1.0` **puede ser correcto**: si los relojes están alineados,
  la deriva es ~0 y el ratio es 1.0. La prueba del ASRC NO es
  "ratio distinto de 1", sino **seguimiento ante deriva conocida**:
  con el teléfono en uso real (bocina/USB), el ratio debe moverse
  dentro del riel ±500 ppm y el audio no debe tener underruns nuevos.
  En host está medido: residuo ≤0.011 ppm, THD+N ≥ −80.83 dB en todo
  el riel, underflow 0 en 34 s a ±70 ppm.

### Beacon (lectura directa, root)

```sh
ls -l /data/local/tmp/omega_effect_beacon   # 72 B, 0666
ls -l /data/local/tmp/omega_effect_ctrl_local
cat /data/local/tmp/ivanna_fusion_heartbeat
```

El heartbeat (`ivanna_fusion_heartbeat`) lo escribe el daemon en
`/data/local/tmp` para que la app lo lea. Si no existe, el daemon no
está publicando (o no arrancó).

## 4. Matriz de aislamiento (bofo / tronidos)

Si el sonido es "bofo feo" o hay tronidos, aislar ANTES de tocar nada:

| Prueba | Qué decide |
|---|---|
| Tidal solo, SIN IVANNA instalada (o con kill switches puestos) | Si suena mal igual → NO es IVANNA (es Tidal/red/ruta) |
| Tidal dentro de IVANNA (BridgePlayer) | Si suena bien aquí y mal en captura → el problema es la vía de captura |
| Tidal nativo + Ruta B (kill switches quitados) | Si suena bien aquí → Ruta B funciona; comparar con la fila anterior |
| Mismo tema en WiFi vs datos móviles | Tronidos solo en datos → red, no DSP |
| Tema descargado en Tidal | Tronidos con descarga → no es red; mirar DSP/doctor |

Regla: **un cambio a la vez, con medición**. No cambiar dos cosas y
atribuir el resultado a una.

## 5. Criterio de aceptación (todo debe cumplirse)

1. `perceptual_generation >= 1` tras abrir la app (snapshot NO neutro).
2. `orbe_amount == 1.0` y `holos_amount == 0.5` (defaults de Esse) en
   GET_STATUS, iguales a los de la app.
3. `effect_state == 2` y `dsp_frames` creciendo ≥ 48000 frames en 10 s
   de música (48 kHz estéreo) — el beacon lo confirma.
4. `dsp_applied_generation == snapshot_generation` (el efecto aplica
   el último snapshot).
5. Escucha A/B con kill switches: con Ruta B el espacial se nota;
   con `pal_asr.off` + `pal_asrc.off` puestos, suena plano/stock.
   La diferencia es audible y el cambio es inmediato (sin reiniciar).
6. Sin tronidos nuevos ni cortes atribuibles a Ruta B (matriz §4).
7. Sin líneas `omega_process passthrough` en 60 s de música (salvo
   `canales != 2` en contenido multicanal, que es correcto).
8. Kill switches apagan en < 1 s sin reiniciar nada ni silenciar el audio.

Si algo falla: el doctor nombra la puerta (ver §1). Si la puerta es
UNKNOWN, el log + GET_STATUS + beacon van al reporte — no se declara
"perfecta" por cansancio.

## 6. Pendiente / bloqueado (no es un fallo, es el límite actual)

- **Verificación en dispositivo**: todo lo anterior exige el teléfono
  con la luz verde de Esse. En host solo están medidas las piezas
  (tests C++ y ASRC); el acople real, el beacon en vivo y la escucha
  A/B no existen sin el teléfono.
- **Parse Kotlin de los 4 campos nuevos de GET_STATUS**
  (`snapshot_generation`, `perceptual_generation`, `orbe_amount`,
  `holos_amount`): el daemon ya los publica; `MagiskBridge.kt` aún no
  los lee, así que el doctor recibe nulls y la puerta
  SNAPSHOT_NEUTRAL está inerte hasta ese cableado (otro agente).
- **SET_USER_FLAGS / SET_HEAD_POSE / SET_INTENSITY y resto de SET_***:
  el daemon v2 sigue respondiendo `unsupported` (como antes). Solo
  `SET_PERCEPTUAL_STATE` se repuso. Los flags (Volterra, Unity,
  FastRPC, ATI) no llegan al snapshot por el socket.
- **Fusión LOADED / `fusion_active=1`**: imposible sin el músculo HAL
  (retirado). Si Esse quiere `fusion_active=1` algún día, hace falta
  una fuente de reloj del músculo distinta del LD_PRELOAD (pendiente
  de diseño, no de este protocolo).
