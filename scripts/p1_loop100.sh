#!/bin/bash
# p1_loop100.sh — P1 (2026-11-22): bucle de 100 ejecuciones consecutivas
# de la suite CTest completa (criterio de Luna).
#
# Uso:
#   nohup bash scripts/p1_loop100.sh > /tmp/p1_loop100.log 2>&1 &
#
# Estado (se actualiza tras cada corrida):
#   ~/workspace/ivanna-transfer/p1_loop100_state.txt  (resumen)
#   ~/workspace/ivanna-transfer/p1_loop100_runs.csv   (una línea por corrida)
#   /tmp/p1_loop100.log                               (salida completa)
#
# Cada corrida tarda ~2.5-3 min (la matriz P1 sola ~68 s); 100 corridas
# son ~4-5 h. El script es reanudable: si se interrumpe, vuelve a
# arrancar desde la corrida siguiente a la última registrada en el CSV.
#
# HALLAZGO P1 (2026-11-22): 3 tests de la matriz fallan por 2 defectos
# reales del motor (APEX rodea el safety limiter -> clipping en hotmix;
# NEURO deja cola residual de -77 dBFS). Hasta que el motor se arregle,
# el bucle NO será verde: registrará las mismas 3 fallas en cada corrida.
# Eso es lo que el bucle debe PROBAR: determinismo (las mismas fallas,
# sin flakiness, sin fallas nuevas). Cuando el motor se arregle, el
# bucle deberá dar 100/100 en verde.
#
# P0 (REFERENCE_LOCKED) sigue sin existir: P1 detecta regresiones, no
# sustituye la referencia congelada.
set -u
REPO="$HOME/workspace/ivanna-transfer"
BUILD="$REPO/build/tests-host"
STATE="$REPO/p1_loop100_state.txt"
CSV="$REPO/p1_loop100_runs.csv"
TOTAL=100

cd "$BUILD" || exit 1

# Reanudar: contar corridas ya registradas.
done_n=0
if [ -f "$CSV" ]; then
    done_n=$(tail -n +2 "$CSV" 2>/dev/null | wc -l)
fi

write_state() {
    local run=$1 pass=$2 fail=$3
    {
        echo "P1 bucle de 100 — estado $(date '+%Y-%m-%d %H:%M:%S %Z')"
        echo "corridas completadas: $run / $TOTAL"
        echo "última corrida: $pass pasaron, $fail fallaron (de 1619)"
        echo "verdes (0 fallos): $(awk -F, 'NR>1 && $3==0 {c++} END {print c+0}' "$CSV" 2>/dev/null)"
        echo "fallas recurrentes (top):"
        grep -h "FAILED" /tmp/p1_loop100.log 2>/dev/null | sort | uniq -c | sort -rn | head -5
        echo "CSV: $CSV"
        echo "log: /tmp/p1_loop100.log"
    } > "$STATE"
}

# Encabezado del CSV si es nuevo.
if [ ! -f "$CSV" ]; then
    echo "run,passed,failed,seconds,failed_tests" > "$CSV"
fi

i=$((done_n + 1))
while [ "$i" -le "$TOTAL" ]; do
    start=$(date +%s)
    out=$(ctest -j2 2>&1)
    end=$(date +%s)
    secs=$((end - start))
    # Resumen ctest: "99% tests passed, 3 tests failed out of 1619".
    total_line=$(echo "$out" | grep -E "tests passed.*tests failed out of" | tail -1)
    failed=$(echo "$total_line" | grep -oE "[0-9]+ tests failed" | grep -oE "^[0-9]+" || echo "?")
    total=$(echo "$total_line" | grep -oE "out of [0-9]+" | grep -oE "[0-9]+$" || echo "?")
    if [ "$failed" != "?" ] && [ "$total" != "?" ]; then
        passed=$((total - failed))
    else
        passed="?"
    fi
    failed_tests=$(echo "$out" | grep -E "^\s+[0-9]+ - .*\(Failed\)" | sed 's/^ *[0-9]* - //;s/ (Failed)//' | tr '\n' ';' | sed 's/;$//')
    # Fallback si el formato del resumen cambia: contar "Failed" en la lista.
    if [ -z "$failed_tests" ]; then
        failed_tests=$(echo "$out" | grep -i "failed" | grep -v "tests failed" | head -5 | tr '\n' ';')
    fi
    echo "$i,$passed,$failed,$secs,\"$failed_tests\"" >> "$CSV"
    echo "[$(date '+%H:%M:%S')] corrida $i/$TOTAL: $passed pasaron, $failed fallaron, ${secs}s :: $failed_tests" | tee -a /tmp/p1_loop100.log
    write_state "$i" "$passed" "$failed"
    i=$((i + 1))
done
echo "BUCLE COMPLETO: $TOTAL corridas. Ver $STATE y $CSV." | tee -a /tmp/p1_loop100.log
