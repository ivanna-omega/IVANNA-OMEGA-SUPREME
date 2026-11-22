#!/system/bin/sh
# IVANNA Fusion Magistral — uninstall.sh (al desinstalar el modulo)
# Limpia el estado del safe-mode y del daemon v2. El directorio
# /data/vendor/ivanna_audio y el dump se dejan (no estorban).
pkill -f ivanna_daemon_v2 2>/dev/null
rm -rf /data/adb/ivanna_fusion
rm -f /data/adb/ivanna_omega/daemon_v2_heartbeat /data/adb/ivanna_omega/daemon_v2.log
exit 0
