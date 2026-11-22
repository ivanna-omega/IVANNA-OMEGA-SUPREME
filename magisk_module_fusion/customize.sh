#!/system/bin/sh
# IVANNA Fusion Magistral — customize.sh (corre en la instalacion, Magisk)
# Activa el anclaje LD_PRELOAD del ASRC (overlay.d/ivanna_audio.rc) y deja
# el daemon v2 instalado en system/bin/ivanna_daemon_v2 (lo arranca service.sh).
# Mismo mecanismo probado del modulo principal v2.5.397/399.
# ROLLBACK: borrar overlay.d/ivanna_audio.rc y .preload_optin y reiniciar.
# Kill switches: /data/vendor/ivanna_audio/pal_asrc.off (ASRC bypass) y
# /data/adb/ivanna_omega/ivanna_daemon.off (daemon no arranca).

ui_print "- IVANNA Fusion Magistral v1.0"
ui_print "- ASRC v6 + Daemon v2 en un solo modulo. Sin app, sin efecto, sin SOFA."

if [ -f "$MODPATH/overlay.d.disabled/ivanna_audio.rc" ]; then
    mkdir -p "$MODPATH/overlay.d"
    cp -f "$MODPATH/overlay.d.disabled/ivanna_audio.rc" "$MODPATH/overlay.d/ivanna_audio.rc"
    touch "$MODPATH/.preload_optin"
    ui_print "  ✓ ASRC anclado: overlay.d/ivanna_audio.rc activo."
    ui_print "    Se carga en el proximo arranque (LD_PRELOAD en vendor.audio-hal)."
else
    abort "  ERROR: falta overlay.d.disabled/ivanna_audio.rc en el zip."
fi

if [ -f "$MODPATH/system/vendor/lib64/libivanna_asrc_write_stage.so" ]; then
    ui_print "  ✓ libivanna_asrc_write_stage.so presente."
else
    abort "  ERROR: falta system/vendor/lib64/libivanna_asrc_write_stage.so en el zip."
fi

if [ -f "$MODPATH/system/bin/ivanna_daemon_v2" ]; then
    chmod 0755 "$MODPATH/system/bin/ivanna_daemon_v2"
    ui_print "  ✓ ivanna_daemon_v2 presente (lo arranca service.sh)."
else
    ui_print "  ⚠ ivanna_daemon_v2 ausente: el modulo arranca solo con ASRC."
    ui_print "    (El daemon se compila en CI; si falta, reinstalar el zip de la CI.)"
fi

ui_print "- Listo. Reinicia para activar ASRC + daemon."
ui_print "- Apagar ASRC sin desinstalar: crea /data/vendor/ivanna_audio/pal_asrc.off"
ui_print "- Apagar daemon sin desinstalar: crea /data/adb/ivanna_omega/ivanna_daemon.off"
