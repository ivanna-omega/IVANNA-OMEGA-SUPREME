# IVANNA — Instalación por un tercero (no-Esse)

Refinamiento P6+P7, 2026-11-26. Estado: **documentación solamente** (no se
tocó código para esta tarea). Versión de referencia: **v2.5.504**
(`module.prop`: versionCode 795; APK ~209 MB; zip del módulo ~97 MB).

## 1. Declaración: quién puede instalar IVANNA hoy

Hay **dos niveles**, y no son el mismo producto:

| Nivel | Qué obtiene | Requisitos |
|---|---|---|
| **A. Solo APK (Ruta A)** | Procesado IVANNA dentro de la app: reproductor propio (música local), ORBE/HOLOS/ARMONÍA y resto de operadores en proceso. Sin Ruta B, sin daemon, sin ASRC. | Android 13+, APK, permisos de micrófono + almacenamiento. **Sin root.** |
| **B. Completa (Ruta A + Ruta B)** | Todo lo anterior + efecto en AudioFlinger para cualquier app de música (Tidal/Qobuz nativas), daemon, ASRC y acoplo por sesión real. | Android 13+, **root + Magisk**, APK con permiso de superusuario. |

**No-root:** Ruta A sin root **sí existe** (nivel A). Para el nivel B
**no hay ni habrá vía no-root**: el módulo es Magisk por definición, el
daemon corre como root (descubre la sesión de música por `dumpsys`,
publica el snapshot por SHM) y la app necesita superusuario para acoplar
el efecto a la sesión real. Cualquier "IVANNA completa sin root" que se
prometa es falso.

## 2. Gate previo: acceso a los archivos

El repositorio es **privado** (`ivanna-omega/IVANNA-OMEGA-SUPREME`) y las
releases **exigen autenticación de GitHub** para descargar. Un tercero
**no puede descargar nada por su cuenta**: Esse debe darle acceso al
repo o entregarle en mano la APK + el zip del módulo de la v2.5.504
(o la versión vigente). Sin ese paso, el resto de este checklist no
se puede ejecutar.

**Defecto encontrado (2026-11-26, no corregido aquí):** `magisk_module/update.json`
en HEAD dice `version: v2.5.504` / `versionCode: 795` pero su `zipUrl`
apunta al zip de la **v2.5.503**. El canal de actualización en la app
serviría la 503, no la 504. Corregir el `zipUrl` antes de que un tercero
use la auto-actualización.

## 3. Requisitos duros (verificar antes de tocar nada)

- [ ] Android **13 o superior** (`minSdk = 33`; `targetSdk = 35`).
- [ ] ~350 MB libres (APK 209 MB + zip 97 MB + datos SOFA/RIR del módulo).
- [ ] Para el nivel B: **bootloader desbloqueado** (borra todos los datos
      del teléfono: hacer copia antes) + **Magisk instalado y funcional**
      (el instalador es un zip Magisk estándar; `customize.sh` aborta si
      falta `SAF_model.json` o no puede copiarlo byte a byte).
- [ ] Para el nivel B: saber que **Magisk puede romper Play Integrity**
      y el acceso a apps de banco (en el G85 de Esse se mitiga con
      PlayIntegrityFork + Shamiko; eso es configuración propia de Esse,
      no parte de este checklist).
- [ ] NO instalar el zip **v2.5.403** ni ningún zip con wrapper del HAL:
      muerto tras 6 bricks (bootloop). El módulo actual ya no lo trae.
- [ ] NO reiniciar `audioserver` a mano ni forzar reinicios del teléfono
      durante la instalación: el módulo nuevo solo toma efecto en el
      próximo arranque.

## 4. Checklist nivel A (solo APK, sin root)

- [ ] Instalar la APK (misma firma que las actualizaciones anteriores;
      si viene de una build con llave efímera anterior a la v2.5.425,
      desinstalar primero: no actualiza encima).
- [ ] Abrir la app una vez; conceder micrófono (bucle Φ) y acceso a
      música local (Download/Música, reproductor BridgePlayer).
- [ ] Poner música local: debe sonar con IVANNA dentro de la app.
- [ ] Verificación: la UI marca el motor activo; sin superusuario la
      Ruta B queda inactiva por diseño (eso es lo esperado, no un fallo).

## 5. Checklist nivel B (completa, con root + Magisk)

> Hacerlo con el teléfono en la mano y tiempo: hay un reinicio en medio.

- [ ] Nivel A completo y verificado.
- [ ] Magisk: instalar el zip `ivanna_omega_supreme_v2.5.504.zip`
      desde la app de Magisk (Instalar → seleccionar zip). No tocar nada
      hasta que `customize.sh` termine sin `abort`.
- [ ] Reiniciar el teléfono (el módulo solo se activa en el arranque:
      `libomega_effect.so`, daemon v2, `ivanna_asrcd`, `service.sh`).
- [ ] Tras el arranque: abrir la app de IVANNA una vez y **conceder el
      permiso de superusuario** cuando lo pida (sin él, la Ruta B no se
      acopla: solo funciona la Ruta A).
- [ ] Poner música en la app nativa de Tidal o Qobuz (no dentro de
      IVANNA: esa es la vía de captura de Ruta B).
- [ ] Verificación (detalle completo en
      `docs/09-operations/route-b-verification.md`):
  - [ ] `su -c "dumpsys media.audio_flinger" | grep -A 15 -i "omega"`:
        efecto `omega` en la sesión real de música, `Enabled`.
  - [ ] Beacon creciendo: `dsp_frames` sube entre dos lecturas a 5 s
        (`GET_STATUS` del daemon; `effect_state == 2` PROCESSING).
  - [ ] `perceptual_generation >= 1` y `orbe_amount` / `holos_amount`
        iguales a los de la app (si es 0: snapshot neutro, abrir la app).
  - [ ] `logcat -d -t 300 -s IvannaOmegaEffect OmegaCtrlBus` sin líneas
        `omega_process passthrough` (cada una dice el motivo).
  - [ ] Escucha A/B: con Ruta B se nota el espacial; con los kill
        switches puestos (`pal_asr.off` / `pal_asrc.off`), no.
- [ ] Si algo falla: **no reinstalar a ciegas**. Leer
      `docs/09-operations/route-b-install-recovery.md` (instalación y
      recuperación) y `route-b-verification.md` §4 (matriz de aislamiento:
      un cambio a la vez, con medición).

## 6. Qué NO se promete

- No se promete que funcione en cualquier teléfono: el desarrollo y
  todas las mediciones en vivo son en **Moto G85** (Android 15). En
  otro modelo, la Ruta B (sesión 0, HAL, DAC USB) puede comportarse
  distinto; la Ruta A es la parte portable.
- No se promete alta resolución por el altavoz del teléfono (16-bit);
  el hi-res (24-bit) es por DAC USB.
- No hay soporte para instalar sin desbloquear el bootloader ni sin
  Magisk para el nivel B. Quien no quiera rootear, nivel A.
