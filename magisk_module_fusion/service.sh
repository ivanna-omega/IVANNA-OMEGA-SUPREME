#!/system/bin/sh
# IVANNA Fusion Magistral — service.sh (cada arranque, despues de servicios)
# 1. Confirma vendor.audio-hal corriendo y resetea el contador del safe-mode.
# 2. Arranca ivanna_daemon_v2 (si existe y no hay kill switch
#    /data/adb/ivanna_omega/ivanna_daemon.off). Si el daemon muere, NO se
#    relanza en bucle: se registra y se deja (el ASRC del HAL no depende de el).
# 3. Re-asegura el directorio de datos y su etiqueta.

MODDIR="${0%/*}"
DATA_DIR="/data/vendor/ivanna_audio"
OMEGA_DIR="/data/adb/ivanna_omega"
CTX="u:object_r:vendor_audio_data_file:s0"
STATE_DIR="/data/adb/ivanna_fusion"
COUNT_FILE="$STATE_DIR/boot_streak"
DAEMON_OFF="$OMEGA_DIR/ivanna_daemon.off"
DAEMON_BIN="/vendor/bin/ivanna_daemon_v2"
[ -x "$DAEMON_BIN" ] || DAEMON_BIN="$MODDIR/system/bin/ivanna_daemon_v2"

mkdir -p "$DATA_DIR" "$OMEGA_DIR" 2>/dev/null
chcon "$CTX" "$DATA_DIR" 2>/dev/null

i=0
while [ $i -lt 60 ]; do
    [ "$(getprop init.svc.vendor.audio-hal)" = "running" ] && break
    sleep 1
    i=$((i + 1))
done

if [ "$(getprop init.svc.vendor.audio-hal)" = "running" ]; then
    echo 0 > "$COUNT_FILE" 2>/dev/null
    echo "$(date) FUSION OK: vendor.audio-hal running, pid=$(getprop init.svc_debug_pid.vendor.audio-hal)" >> "$STATE_DIR/boot.log" 2>/dev/null
fi

# Daemon v2 (opcional: el ASRC no depende de el).
if [ -f "$DAEMON_OFF" ]; then
    echo "$(date) FUSION: daemon apagado por kill switch ($DAEMON_OFF)" >> "$STATE_DIR/boot.log" 2>/dev/null
    exit 0
fi
if [ -x "$DAEMON_BIN" ]; then
    # Matar un resto anterior (si lo hubiera) y arrancar limpio, en fondo.
    pkill -f ivanna_daemon_v2 2>/dev/null
    sleep 1
    nohup "$DAEMON_BIN" >> "$OMEGA_DIR/daemon_v2.log" 2>&1 &
    echo "$(date) FUSION: ivanna_daemon_v2 arrancado (pid $!)" >> "$STATE_DIR/boot.log" 2>/dev/null
else
    echo "$(date) FUSION: ivanna_daemon_v2 no encontrado, solo ASRC" >> "$STATE_DIR/boot.log" 2>/dev/null
fi
exit 0
