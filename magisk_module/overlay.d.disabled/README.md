# overlay.d.disabled/ — RETIRADO (2026-11-14, consolidación Ruta B)
#
# El archivo `ivanna_audio.rc` (inyección LD_PRELOAD del ASRC v5 en el HAL)
# YA NO SE DISTRIBUYE en este módulo. Fue la causa de 3 bricks (la 384 lo
# activaba y el teléfono no arrancaba; el wrapper v2.5.403 que dependía de
# esta vía costó el 6º reset de fábrica). La vía LD_PRELOAD está MUERTA:
# no se reintenta, no hay opt-in, no hay marcador `.preload_optin` válido.
#
# Estado actual:
#   - El HAL arranca STOCK siempre. Nada en este módulo inyecta nada en él.
#   - La .so `libivanna_asrc_write_stage.so` sigue viajando en el zip SOLO
#     porque la CI lo exige (build.yml: ERROR si falta en el zip), pero es
#     INERTE: sin .rc que la precargue, nada la mapea jamás. El chequeo de
#     arranque la reporta NOT_LOADED como veredicto esperado, no como fallo.
#   - El ASRC vivo es el del efecto + asrcd (puente en omega_process,
#     servo Theil-Sen, IPC por SHM), no el del HAL.
#   - post-fs-data.sh SIGUE retirando cualquier `overlay.d/ivanna_audio.rc`
#     que sobreviva de un zip viejo o de una copia manual (sin excepciones
#     ni marcador opt-in): es protección, no una vía de activación.
#
# HISTORIAL (no reinstalar): la 384 lo activaba (2 bricks); la 385+ lo
# desactivó por defecto (HAL STOCK, .so inerte); la 397 lo reactivó por
# directiva (anclaje automático); la 401+ lo dejó dormido en este
# directorio como "fuente opt-in"; 2026-11-14 se retira la fuente misma.
# El módulo separado magisk_module_fusion/ tiene SU PROPIA copia y su
# propio customize.sh (fuera de este módulo; no se toca aquí).
