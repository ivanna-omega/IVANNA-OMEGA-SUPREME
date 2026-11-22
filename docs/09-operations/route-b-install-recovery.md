# Ruta B — Instalación y recuperación

> Estado: rama `ivanna-real-completa`, commits de consolidación Ruta B
> (2026-11-14). **Nada de esto está en un release publicado todavía.**
> La instalación exige el release que contenga estos commits
> (post-v2.5.445) + la luz verde explícita de Esse. **No instalar, no
> reiniciar, no tocar el teléfono sin esa luz verde.**

## 0. Qué es el módulo ahora (y qué NO es)

El módulo Magisk es el **mínimo**: librería de efectos + configuración +
daemon + ASRC del efecto.

| Contiene | Para qué |
|---|---|
| `libomega_effect.so` (`system/vendor/lib64/soundfx/`) | El efecto AudioFlinger (AELI + UUID `4956414e-…`) |
| `audio_effects_ivanna*.xml` | Registro del efecto en AudioFlinger |
| `ivanna_daemon` (v2, cerebro) | Descubre la sesión, sirve GET_STATUS, publica el snapshot |
| `ivanna_asrcd` (servo Theil-Sen) | Publica el ratio ASRC por SHM |
| `service.sh` / `supervisor.sh` / `post-fs-data.sh` | Arranque, vigilancia y limpieza de restos viejos |
| `system/etc/ivanna_omega/` (HRTF, RIR, SOFA, PCA) | Datos del motor |

| NO contiene (retirado 2026-11-14) | Por qué |
|---|---|
| Wrapper del HAL (v2.5.403) | **MUERTO**: 6º reset de fábrica. No se reintenta, no se instala. |
| `ivanna_audio.rc` / `overlay.d/` / LD_PRELOAD | **RETIRADO**: 3 bricks. `post-fs-data.sh` retira cualquier `.rc` superviviente sin excepciones. Regla `noatsecure` eliminada de `sepolicy.rule`. |
| `libivanna_asrc_write_stage.so` activa | Viaja en el zip SOLO porque la CI lo exige; es **inerte** (sin `.rc` nada la mapea jamás). El ASRC vivo es el del efecto + asrcd. |

## 1. Requisitos previos (verificar antes de tocar nada)

1. Teléfono limpio tras el 7º reset: sin Magisk, sin módulos, sin daemon.
   Confirmar: `ls /data/adb/modules` no muestra `ivanna_omega_supreme`.
2. Magisk instalado y funcional (el teléfono ya estuvo rooteado antes).
3. APK con los fixes de consolidación Ruta B instalada (el release que
   contenga `fix(daemon): SET_PERCEPTUAL_STATE…` — sin él, el daemon
   publica el snapshot neutro y Ruta B suena neutra aunque se acople).
4. Termux + SSH levantado a mano (`sshd` como usuario normal, nunca
   como root) para poder diagnosticar sin tocar la pantalla.

## 2. Instalación paso a paso (verificar tras CADA paso)

### Paso 1 — Instalar la APK
- Instalar el APK del release (misma keystore: actualiza encima).
- Verificar: Ajustes → versión muestra la versión del release
  (no una 2.5.72 rancia ni la 445 si el release es posterior).

### Paso 2 — Instalar el zip Magisk
- En Magisk → Módulos → instalar el zip del mismo release.
- Verificar (root, sin reiniciar todavía):
  `ls /data/adb/modules/ivanna_omega_supreme/system/vendor/lib64/soundfx/libomega_effect.so`
  y `ls …/system/bin/ivanna_daemon …/system/bin/ivanna_asrcd`.
- Verificar que NO existe `…/overlay.d/ivanna_audio.rc`
  (si existe, es un resto viejo: borrarlo antes de reiniciar).

### Paso 3 — Reiniciar (SOLO con luz verde de Esse)
- Un reinicio. Nada más. No `killall audioserver`, no reinicios del
  servicio de audio: eso ya costó 4 bricks.
- Tras el arranque, esperar 60 s (el daemon y el supervisor arrancan
  con el boot).

### Paso 4 — Daemon vivo
- `pidof ivanna_daemon ivanna_asrcd` → dos PIDs.
- `grep -a omega_daemon_socket /proc/net/unix` → el socket abstracto.
- `cat /data/adb/ivanna_omega/daemon_v2.log` → última línea
  `ivanna_daemon_v2 (CEREBRO) iniciado` + `local-files: beacon OK` +
  `local-files: ctrl OK` (el daemon pre-crea beacon y control 0666).
- Si el log dice `FALLO` en local-files: el beacon no existe y el
  doctor no verá procesado. No seguir: ir a §4.

### Paso 5 — Abrir la app IVANNA (obligatorio)
- Abrir IVANNA una vez (no basta con que esté instalada): al arrancar,
  restaura su estado y envía `SET_PERCEPTUAL_STATE` al daemon.
- Verificar con GET_STATUS (ver `route-b-verification.md`):
  `perceptual_generation >= 1`, `orbe_amount == 1.0`,
  `holos_amount == 0.5`. Si `perceptual_generation == 0`, el snapshot
  es neutro (ORBE/HOLOS en 0) y Ruta B sonará neutra aunque se acople.
  **No seguir hasta que esto sea ≥ 1.**

### Paso 6 — Sonar música en la app nativa de Tidal
- Reproducir en Tidal (la vía de Esse: app nativa, no Tidal dentro de
  IVANNA). Esperar 10 s.
- Verificar: GET_STATUS `discovered_session_id > 0`,
  `effect_state == 2` (PROCESSING), `dsp_frames` creciendo,
  `dsp_applied_generation == snapshot_generation`.
- Verificar el beacon: `ls -l /data/local/tmp/omega_effect_beacon`
  (72 B, 0666) y frames creciendo entre dos lecturas separadas 5 s.

### Paso 7 — Criterio "efectiva"
- Ver `route-b-verification.md` § "Criterio de aceptación". Si no se
  cumple, NO se declara perfecta: se diagnostica con el log
  `IvannaOmegaEffect` (la puerta que corta el procesado está ahí).

## 3. Kill switches (apagan sin reiniciar nada)

| Archivo (crear con `touch` como root) | Efecto |
|---|---|
| `/data/adb/ivanna_omega/pal_asr.off` | **Daemon v2 + asrcd OFF** (sin la `c`). El cerebro pasa a bypass (ratio 1.0, efectos off); el asrcd sale limpio. El audio sigue. |
| `/data/adb/ivanna_omega/pal_asrc.off` | **Puente ASRC OFF** (con la `c`). El efecto pasa el audio bit-exacto. El resto del DSP sigue. |
| `/data/adb/ivanna_omega/asrc.disable` | Motor ASRC OFF (el puente pone bypass). |
| `/data/adb/ivanna_omega/ivanna_daemon.off` | El daemon **no arranca** en el próximo boot. Para apagarlo ya (sin reiniciar): `kill <pid ivanna_daemon>` — el supervisor NO lo relanza si el archivo existe (ver `supervisor.sh`). |

Para **revertir** un kill switch: borrar el archivo. El daemon/asrcd
vuelven solos en el próximo arranque (o al relanzarlos a mano con
`service.sh` — nunca con `killall audioserver`).

**Regla de oro:** el kill switch es SIEMPRE el primer paso ante
cualquier síntoma raro (sonido extraño, tronidos, silencio). Solo si el
kill switch no lo resuelve se pasa a §4.

## 4. Recuperación / rollback

### El audio suena mal con el módulo activo
1. `touch /data/adb/ivanna_omega/pal_asr.off` (apaga daemon+asrcd).
2. `touch /data/adb/ivanna_omega/pal_asrc.off` (apaga el puente).
3. Si sigue mal: el problema NO es Ruta B (es Ruta A, Tidal o el
   sistema). Quitar los dos archivos solo cuando se sepa la causa.
4. Si el problema SÍ era Ruta B: dejar los kill switches puestos y
   reportar con el log `IvannaOmegaEffect` + GET_STATUS (ver
   `route-b-verification.md`).

### El teléfono no arranca tras instalar el módulo
1. **Magisk safe mode**: reiniciar con el botón de bajar volumen
   pulsado (o `adb wait-for-device` + Magisk safe mode). Con safe mode,
   Magisk no carga módulos y el teléfono arranca stock.
2. Desde safe mode (o recovery + `adb shell`): borrar el módulo:
   `rm -rf /data/adb/modules/ivanna_omega_supreme`.
3. Reiniciar normal. El teléfono vuelve stock (sin Magisk no hay
   Ruta B, pero hay teléfono).
4. **NUNCA** reinstalar el mismo zip sin entender la causa: si el
   módulo nuevo (sin `.rc`, sin wrapper) impide el arranque, la causa
   está en otro archivo del zip y hay que auditarlo antes.

### El daemon no arranca (pero el teléfono sí)
1. Leer `/data/adb/ivanna_omega/daemon.log` (service.sh) y
   `daemon_v2.log` (daemon). La causa está ahí, no en adivinar.
2. Si dice `ivanna_asrcd AUSENTE`: el binario no vino en el zip (fallo
   de CI): el módulo arranca igual con ASRC en 1.0; reinstalar el zip
   del release correcto.
3. Si dice `pal_asr.off presente`: es el kill switch, no un fallo.
   Borrar el archivo si se quiere el daemon.
4. Si el socket `@omega_daemon_socket` no existe: `service.sh` mata y
   relanza instancias colgadas (hasta 5 caídas, luego `.safe_mode`).
   Con `.safe_mode` presente, el daemon no arranca: borrar
   `/data/adb/ivanna_omega/.safe_mode` solo tras leer el log.

### Desinstalación completa (volver a stock)
1. Magisk → Módulos → quitar `IVANNA OMEGA SUPREME` → reiniciar.
2. Verificar: `ls /data/adb/modules/ivanna_omega_supreme` no existe;
   `pidof ivanna_daemon` vacío; el audio de Tidal suena stock.
3. La APK puede quedarse instalada (Ruta A sigue funcionando sin
   módulo; Ruta B simplemente no existe).

## 5. Qué NO hacer jamás

- `killall audioserver` / reiniciar audioserver / reiniciar el HAL.
- Instalar el zip v2.5.403 ni ningún wrapper del HAL.
- Copiar un `ivanna_audio.rc` a `overlay.d/` (vía retirada).
- Crear `.preload_optin` (ya no es válido; `post-fs-data.sh` lo borra).
- Tocar `/data/adb` con `chmod 0755 -R` (el tránsito es 0711, puesto
  por `service.sh`; un 0755 recursivo expone Magisk).
- Declarar Ruta B "perfecta" sin pasar el protocolo de
  `route-b-verification.md` en el teléfono real.
