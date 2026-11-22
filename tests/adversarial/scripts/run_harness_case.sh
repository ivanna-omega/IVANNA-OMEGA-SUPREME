#!/bin/sh
# run_harness_case.sh — Driver de un caso del banco para ctest.
# Uso: run_harness_case.sh GEN HARNESS WORKDIR CASO "TRANSFORM_ARGS" "HARNESS_ARGS"
# Genera el caso, aplica la transformada (o copia si no hay), mide con el
# harness y devuelve el exit del harness. Los args entre comillas se parten
# por espacios al expandirlos sin comillas.
set -u
GEN=$1; HARNESS=$2; WORK=$3; CASE=$4; TRANSFORM_ARGS=$5; HARNESS_ARGS=$6
mkdir -p "$WORK"
REF="$WORK/ref.wav"; DUT="$WORK/dut.wav"
"$GEN" generate "$CASE" "$REF" --seed 0 || exit 2
if [ -n "$TRANSFORM_ARGS" ]; then
    # shellcheck disable=SC2086
    "$GEN" transform "$REF" "$DUT" $TRANSFORM_ARGS || exit 2
else
    cp "$REF" "$DUT" || exit 2
fi
# shellcheck disable=SC2086
"$HARNESS" measure "$REF" "$DUT" $HARNESS_ARGS
