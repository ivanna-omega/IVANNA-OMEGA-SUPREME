# Medición en vivo en el G85 — cadena, formato HAL y latencia (P6 6.2)

Refinamiento P6+P7, 2026-11-26. **Parte de código hecha** (contadores en
`omega_effect.cpp`); **la medición en vivo está pendiente** porque exige
el teléfono con música sonando y solo-lectura. Este documento es el
procedimiento exacto para ejecutarla cuando Esse dé luz verde.

## 1. Qué se hizo en código (6.2)

`omega_process()` ya medía la carga por bloque (`usedNs` / `blockNs`,
`omega_effect.cpp`, bloque de `load`), pero la descartaba salvo por la
máquina de estados `cpuShed`. Ahora acumula, por bloque y sin malloc ni
locks (solo el hilo de audio escribe):

| Contador (ctx) | Significado |
|---|---|
| `cpuBlockCount` | bloques procesados desde la creación del efecto |
| `cpuUsedNsTotal` | ns de CPU acumulados (media = total / bloques) |
| `cpuUsedNsMax` | peor bloque (ns) |
| `cpuOverBudgetBlocks` | bloques con `load > 1.0` = xrun real del HAL |
| `cpuOver80Blocks` | bloques con `load > 0.80` (umbral que dispara `cpuShed`) |

Exposición: `omega_effect_get_cpu_stats(effect_handle_t, OmegaEffectCpuStats*)`
(`extern "C"`, mismo patrón que `omega_test_*`, al final de
`omega_effect.cpp`). Devuelve `false` con handle o puntero nulos.
La lectura es lock-free (el hilo de audio solo escribe).
**Límite verificado (2026-11-26, `libomega_effect.so` compilada con el
NDK): el build usa `-fvisibility=hidden`, así que el getter NO queda en
`.dynsym` — igual que las `omega_test_*` existentes — y no es visible
por `dlsym` desde fuera: solo sirve en proceso (código enlazado en la
misma `.so`, o un test que compile el TU). La vía viva real sigue
siendo ABI v18 o volcado a archivo (ver §2.3).

**No se tocó el snapshot SHM**: la ABI v17 tiene `sizeof == 808`
(`static_assert` en `include/omega_control_bus.h`) y añadir campos la
rompería (la app valida `version == 17`). La visibilidad en vivo por
snapshot es una **propuesta ABI v18** (ver §4), no implementada aquí.

**Validación pendiente del código:** `omega_effect.cpp` está bloqueado
para host (REPORT 0021) y solo compila con el NDK (target
`libomega_effect.so`). La comprobación es compilar ese target con el
NDK 27.2 (`/home/hatch/android-sdk/ndk/27.2.12479018`) — ver §5.

## 2. Procedimiento vivo (solo lectura, con música sonando)

Precondiciones: teléfono con la v2.5.504 instalada, Ruta B acoplada
(ver `docs/09-operations/route-b-verification.md`), música sonando en
la app nativa (Tidal/Qobuz), SSH a Termux levantado (`sshd` como
usuario normal). **No instalar nada, no reiniciar nada.**

### 2.1 Formato real negociado por el HAL (16 vs 24-bit)

El DAC USB JieLi anuncia 24-bit, pero lo que el HAL negocia de verdad
se lee en AudioFlinger, no en la ficha del DAC:

```sh
su -c "dumpsys media.audio_flinger" > /sdcard/Download/af_dump.txt
```

En el volcado, buscar el **hilo de salida (output thread)** que lleva
la sesión de música (la que descubre el daemon; cambia en cada
reproducción) y leer:

- **Format**: `AUDIO_FORMAT_PCM_16_BIT` / `PCM_24_BIT` / `PCM_FLOAT` —
  es el formato real del hilo. Si dice 16-bit, la cadena corre a 16-bit
  aunque el DAC soporte 24-bit (ese era el estado medido en 2026-11-20:
  HAL a 16-bit / 48 kHz con el DAC JieLi).
- **Sample rate** del hilo (48000 esperado; 96000/192000 solo en Ruta A
  hi-res por USB).
- **Frame count / buffer size** del hilo (para §2.2).
- El efecto `omega` debe aparecer `Enabled` en la sesión real
  (mismo comando, `grep -A 15 -i "omega"`).

Las etiquetas exactas varían por ROM: leerlas del volcado real, no
asumirlas. Guardar `af_dump.txt` como evidencia.

### 2.2 Latencia del HAL

Del mismo volcado (§2.1), en el hilo de salida:

- **Latency** del hilo (ms, si la ROM la publica) + **frame count** y
  **sample rate**: latencia del buffer = `frameCount / sampleRate`
  (p. ej. 960 frames @ 48 kHz = 20 ms).
- Latencia total de la cadena = latencia del hilo + latencia del motor
  P0 (xcorr 26.6 ms @ 48 kHz, medida en host; la del G85 es la que
  falta medir) + latencia del DAC USB. **No sumar cifras de fuentes
  distintas sin decir de dónde sale cada una.**

### 2.3 Contadores de CPU en vivo (los de §1)

Hoy los contadores viven en el `ctx` del efecto (proceso audioserver)
y el getter es `extern "C"` en `libomega_effect.so`, pero **no es
visible por `dlsym` desde fuera** (build con `-fvisibility=hidden`,
verificado en la `.so` compilada con el NDK: ni el getter ni las
`omega_test_*` aparecen en `.dynsym`). Para leerlos en vivo hace falta
una de estas vías (en orden de preferencia):

1. **ABI v18 (propuesta, §4):** publicar un resumen en el snapshot y
   leerlo con el lector existente (app o `GET_STATUS`). Es la vía
   limpia; exige acuerdo app+efecto y bump de versión.
2. **Volcado periódico a archivo** (sin tocar la ABI): un hilo no-RT
   del efecto vuelca los contadores cada ~10 s a
   `/data/adb/ivanna_omega/omega_cpu_stats.txt`, legible por SSH/root.
   No implementado aquí (escribe en el teléfono; requiere decisión de
   Esse por el I/O, aunque sea fuera del hilo de audio).

Mientras no exista 1 o 2, la medición de CPU en vivo se hace con lo
que ya existe: **snapshot `effect_frames` + `raw_rms`/`raw_peak`** (si
el RMS es 0.0 con música sonando, el efecto NO mide la señal: anomalía
confirmada en vivo 2026-11-26 ~02:44 CST) y el log
`IvannaOmegaEffect` (motivos de passthrough) + `cpuShed` indirecto
(vía tier de HOLOS / `shed` en telemetría, si la app lo expone).

### 2.4 Frescura del snapshot (watchdog)

- `snapshot_generation` debe crecer (~1/s con la app re-empujando);
  `perceptual_generation >= 1` tras abrir la app.
- Si `generation` se congela con música sonando: el snapshot está
  rancio (el watchdog de frescura es P2 del plan del consejo, no de
  este refinamiento).

## 3. Criterio de aceptación de la medición

La medición está completa cuando se pueda escribir, con evidencia
(`af_dump.txt` + lecturas del snapshot):

1. Formato real del hilo de salida (16 vs 24-bit) y tasa real.
2. Latencia del hilo HAL (ms) + latencia del motor en el G85.
3. `load` media/p99 en vivo (de los contadores §1, vía 1 o 2) y
   `over_budget_blocks` en 60 s de música (xruns reales del HAL).
4. `effect_frames` creciendo + `raw_rms > 0` con música sonando.

## 4. Propuesta ABI v18 (no implementada; decisión de Esse)

Añadir al **final** de `OmegaDspSnapshot` (antes de `crc32`, sin
desplazar ningún offset existente, mismo patrón que v11–v17) y subir
`OMEGA_CTRL_VERSION` a 18 + actualizar `isVersionValid()` y el lector
de la app en el mismo release:

```c
uint64_t cpu_blocks;        // bloques procesados (monotónico)
uint64_t cpu_used_ns_total; // ns de CPU acumulados
uint64_t cpu_used_ns_max;   // peor bloque (ns)
uint64_t cpu_over_budget;   // bloques con load > 1.0 (xruns)
uint32_t cpu_shed;          // 1 = cpuShed activo
```

`omega_effect.cpp` ya tiene los valores en el ctx (§1): publicarlos
es copiar 5 campos a `pendingSnap` en el mismo bloque donde se escribe
`effect_frames`. No hacerlo solo en el lado del efecto: con la versión
en 18 y la app en 17, `isVersionValid()` rechazaría el snapshot entero.

## 5. Cómo validar la compilación del código nuevo

`omega_effect.cpp` no entra en los tests host. Validación:

```sh
# En la VM (NDK 27.2 disponible en /home/hatch/android-sdk/ndk/27.2.12479018):
# compilar el target libomega_effect.so con el toolchain del NDK
# (app/src/main/cpp/CMakeLists.txt) y verificar que AELI sigue exportado:
nm -D --defined-only libomega_effect.so | grep AELI
```

Verificado 2026-11-26 en esta copia: `libomega_effect.so` compila
(NDK 27.2, `arm64-v8a`, Release) y `AELI` sigue exportado. El getter
`omega_effect_get_cpu_stats` compila dentro del TU pero NO se espera
en `.dynsym` (build con `-fvisibility=hidden`, igual que `omega_test_*`):
si aparece en `nm -D`, algo cambió la visibilidad y hay que revisarlo.
