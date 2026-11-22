# Protocolo F5 — Consolidación HAL (ASRC v6 + Daemon v2)

Fase 2, ítem E (v2.5.472). CÓDIGO SOLAMENTE: nada de este protocolo se ha
instalado en el teléfono ni se ha reiniciado nada. La .so del HAL no existe
aún; `ivanna_hal_stage.hpp` (mismo directorio) es la etapa MÚSCULO lista
para enlazarse. Este documento es el plan por etapas + el contrato.

## 1. Contratos (dos SHM distintos, no confundir)

| Contrato | Archivo | Magic / versión | Quién escribe | Quién lee |
|---|---|---|---|---|
| Fusión (cerebro↔músculo) | `include/ivanna_fusion_state.h` | `0x4956414E` ("IVAN"), **v2** | daemon v2 (cerebro) + etapa HAL (músculo) | ambos |
| Snapshot DSP (app↔daemon↔efecto) | `include/omega_control_bus.h` | `0x4F4D4543` ("OMEC"), **v13** | daemon v2 | efecto omega (audioserver) + app |

El snapshot ABI vigente es **v13** (no v10: la v10 no existe en el árbol;
la v13 añade Φ + Volterra, 0038+0040). NO se cambia el ABI en esta fase:
la pose de Ruta B ya viaja en `head_yaw_rad` / `head_pitch_rad` del
snapshot v13 y el efecto la aplica vía `updateHeadPose(yaw, pitch, 0)`.
El roll NO tiene campo en el snapshot v13 (el efecto lo fija en 0):
añadirlo exigiría un bump de ABI y es trabajo futuro, no de esta fase.

### Discrepancia registrada (no un cambio)
La consolidación integral §3.4 cita magic `0x49564653` ("IVFS") para
`IvannaFusionState`. El contrato VIVO usa `0x4956414E` ("IVAN") desde su
creación y el daemon v2 ya lo publica así. **No se cambia el magic.**
El valor del documento queda registrado aquí como discrepancia.

### Campos v2 de IvannaFusionState (añadidos al final, lector v1 compatible)
- Músculo→Cerebro: `dsp_us_avg` (µs/bloque), `xrun_count`.
- Cerebro→Músculo: `daemon_heartbeat_ns` (CLOCK_MONOTONIC, ns; expira
  a los 100 ms), `head_yaw_rad` / `head_pitch_rad` / `head_roll_rad`
  (pose compacta, para 5B), `parallax_amount` (0..1, SpatialControls),
  `asrc_bypass` (1 = BYPASS 0033, 0 = ASRC v6 activo).
- El daemon escribe `daemon_heartbeat_ns` y `asrc_bypass = 1` en cada
  vuelta de su bucle (~20 Hz). La pose compacta y `parallax_amount` las
  escribe el daemon cuando las tenga (hoy: valores por defecto 0 / 1.0;
  el alimentador desde el estado deseado es trabajo de 5B, no de esta fase).

## 2. Reglas anti-bootloop (obligatorias, lección del 4º brick)

1. En constructor / `open()` / dlopen: SOLO `open` + `mmap` del SHM
   (sin `O_CREAT`: el músculo nunca crea el SHM). Cero `new`, cero
   lectura de archivos de datos (SOFA/IVK1/config), cero hilos.
2. Init lazy de UN SOLO intento con `new (std::nothrow)` en el primer
   `processBlock()` (primer `pal_stream_write`). Si falla: passthrough
   para siempre, sin reintentos.
3. Fail-closed a passthrough + ASRC off si: SHM ausente/inválido,
   heartbeat expirado (>100 ms), `verdict != LOADED`, kill switch
   presente, o safe-mode.
4. Safe-mode tras 3 fallos consecutivos (lectura SHM o init del motor):
   passthrough latched hasta reiniciar el proceso.
5. `fstat` ≥ `sizeof(IvannaFusionState)` antes de `mmap` (sin SIGBUS).
6. Seqlock con `sequence` (`ivanna_fusion_read_seq`): reintentar si el
   escritor estaba a mitad. Limitación conocida: el seqlock evita
   lecturas rotas, no escrituras perdidas entre los dos escritores
   (cerebro y músculo hacen read-modify-write); el cerebro escribe a
   20 Hz y el músculo conserva lo que leyó, así que la ventana de
   pérdida es de un ciclo de cerebro como mucho.

## 3. Kill switches (no confundir)

- Músculo (esta etapa): `/data/adb/ivanna_omega/pal_asrc.off` (CON la c).
  Presente → passthrough bit-exacto. Es el que apaga el ASRC en caliente.
- Daemon v2 (cerebro): `/data/adb/ivanna_omega/pal_asr.off` (SIN la c).
  Presente → fusión en bypass (ratio 1.0, efectos off).
- Write-stage v5 (legado): `/data/vendor/ivanna_audio/pal_asrc.off`.
  Solo lo mira la .so v5; el daemon v2 NO debe mirarlo.
- Arranque del daemon: `/data/adb/ivanna_omega/ivanna_daemon.off`.

Vía de prueba (cuando Esse dé luz verde, no antes): instalar con
`pal_asrc.off` PRESENTE, verificar HAL vivo + dump, y solo entonces
quitarlo en caliente. Sin luz verde no se instala nada.

## 4. Plan por etapas (criterios de paso, no fechas)

### 5A — ASRC v6 + EqMinPhase + Limiter
- Qué: enlazar el motor ASRC v6 (FIR polifásico 287 taps, interp
  cúbica, TPDF 2º orden) en el slot `HalEngine`, con EqMinPhase
  (fase mínima cepstral) y SafetyLimiter (TruePeak 4× + softCeil).
  El `asrc_bypass` del SHM pasa a 0 solo cuando el motor esté medido.
- Criterio: `asrc_verdict=LOADED`, `clock=mmap`, THD+N < −100 dB en
  passthrough ASRC, 0 xruns, sin bootloop en 5 reinicios.
- Si falla: el ASRC vuelve a BYPASS 0033 (fail-closed) y se reporta;
  NO se reactiva a ciegas.

### 5B — + NEU·Ω / TΩ
- Qué: compilar `M[k]` (TΩ 64-bit + NEU·Ω) en el músculo, con la pose
  compacta del SHM (`head_yaw/pitch/roll_rad` + `parallax_amount`)
  alimentando ORBE/HRTF en el HAL.
- Criterio: `dsp_us` medio < 5.5 ms (margen bajo 6.67 ms), p99 < 6.67 ms,
  0 xruns nuevos en 10 min con Tidal a 48 kHz / 24 bandas.
- Si falla: Reloj 2 adaptativo (estimación cada 2–3 hops) y bandas
  24→12 solo bajo tier térmico real.

### 5C — + Volterra GENEZY01 + NPE
- Qué: Volterra sub-banda decimado (D=16, 3 kHz) o Laguerre (P=6) con
  el kernel GENEZY01 medido (ver protocolo F4), + NPE.
- Criterio: el kernel .ivk1 predice THD/IMD de un multi-tono no usado
  en el ajuste con error < 1 dB por armónico.
- Si falla: `K_b` permanece como identidad (fail-closed) y no se
  publica como kernel físico. Si la CPU de HAL + cadena supera 1 núcleo
  A78 en tiempo real, mantener Ω/TΩ en frecuencia y bajar Volterra a
  K=32 solo en <1 kHz.

## 5. Estado actual (v2.5.472, ítem E)

- Contrato SHM v2 implementado (`ivanna_fusion_state.h` + daemon que
  escribe heartbeat y `asrc_bypass=1`). Test: `test_ivanna_fusion_shm`.
- Etapa músculo implementada (`ivanna_hal_stage.hpp`): open/mmap,
  init lazy, watchdog de heartbeat, safe-mode, kill switch, medición
  y publicación del bloque músculo. ASRC en BYPASS 0033 (siempre
  passthrough en esta etapa).
- NO instalado en el teléfono. NO reiniciado nada. La .so del HAL, el
  `.rc` y el módulo Magisk de F5 NO existen aún: son trabajo de 5A con
  luz verde de Esse.
- El write-stage v5 (`hal_integration/moto_g85/pal_preload_probe/
  ivanna_asrc_write_stage.c`, LD_PRELOAD) sigue siendo el único ASRC
  en el HAL; esta etapa NO lo sustituye ni lo toca.
