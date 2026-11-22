#!/system/bin/sh
# IVANNA Fusion Magistral — post-fs-data.sh (cada arranque, antes de servicios)
# 1. Crea /data/vendor/ivanna_audio y /data/adb/ivanna_omega; etiqueta el
#    primero con el tipo STOCK vendor_audio_data_file (el HAL ya puede
#    escribir ahi sin reglas nuevas; verificado en vivo 2026-11-09).
#    NUNCA adb_data_file.
# 2. Safe-mode: cuenta arranques sin confirmacion del HAL. service.sh
#    resetea el contador cuando confirma vendor.audio-hal corriendo.
#    Si llega a 3, retira el .rc LD_PRELOAD (el HAL arranca STOCK).

MODDIR="${0%/*}"
DATA_DIR="/data/vendor/ivanna_audio"
OMEGA_DIR="/data/adb/ivanna_omega"
CTX="u:object_r:vendor_audio_data_file:s0"
STATE_DIR="/data/adb/ivanna_fusion"
COUNT_FILE="$STATE_DIR/boot_streak"

mkdir -p "$DATA_DIR" "$OMEGA_DIR" 2>/dev/null
chcon "$CTX" "$DATA_DIR" 2>/dev/null
chmod 0771 "$DATA_DIR" "$OMEGA_DIR" 2>/dev/null

mkdir -p "$STATE_DIR" 2>/dev/null
COUNT=0
[ -f "$COUNT_FILE" ] && COUNT=$(cat "$COUNT_FILE" 2>/dev/null || echo 0)
case "$COUNT" in ''|*[!0-9]*) COUNT=0;; esac

if [ "$COUNT" -ge 3 ]; then
    rm -f "$MODDIR/overlay.d/ivanna_audio.rc" "$MODDIR/.preload_optin"
    echo 0 > "$COUNT_FILE"
    echo "$(date) FUSION SAFE-MODE: 3 arranques sin HAL, .rc retirado" >> "$STATE_DIR/safe_mode.log" 2>/dev/null
else
    COUNT=$((COUNT + 1))
    echo "$COUNT" > "$COUNT_FILE"
fi
exit 0
