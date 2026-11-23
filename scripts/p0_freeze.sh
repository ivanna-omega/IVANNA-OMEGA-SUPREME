#!/bin/bash
# p0_freeze.sh — Congela P0: la referencia bloqueada del motor de IVANNA.
#
#   ./scripts/p0_freeze.sh          → mide y guarda PROVISIONAL (kFrozen=false)
#   ./scripts/p0_freeze.sh --final  → congela (kFrozen=true) y verifica
#
# --final EXIGE el fix APEX+NEURO commiteado: el techo final APEX debe
# estar presente en fused_chain.cpp Y fused_chain.cpp + StereoWidener.cpp
# deben estar limpios (sin cambios sin commitear). Si no, se niega y dice
# por qué. Así P0 nunca se congela sobre el motor con los defectos
# conocidos (clipping APEX + cola NEURO).
#
# Flujo: compila el target P0 en build/tests-p0 → corre el Freeze (mide y
# reescribe p0_reference_values.hpp + p0_reference.json + WAVs) → recompila
# (el header cambió) → corre los 9 TESTs P0. Con --final los TESTs deben
# salir en verde; sin --final hacen GTEST_SKIP (P0 sin congelar).
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-}"
FINAL=0
if [ "$MODE" = "--final" ]; then
  FINAL=1
elif [ -n "$MODE" ]; then
  echo "uso: $0 [--final]" >&2
  exit 2
fi

if [ "$FINAL" = 1 ]; then
  if ! grep -q "TECHO FINAL (P1" app/src/main/cpp/fused/fused_chain.cpp; then
    echo "[P0-FREEZE] FALTA el techo final APEX en fused_chain.cpp — el fix no está en el árbol." >&2
    exit 1
  fi
  if ! git diff --quiet -- app/src/main/cpp/fused/fused_chain.cpp app/src/main/cpp/dsp/StereoWidener.cpp; then
    echo "[P0-FREEZE] el fix APEX+NEURO NO está commiteado (cambios sin commitear en fused_chain.cpp y/o StereoWidener.cpp)." >&2
    echo "[P0-FREEZE] Congela en provisional (sin --final) o espera a que el fix aterrice como commit." >&2
    exit 1
  fi
  echo "[P0-FREEZE] fix APEX+NEURO presente y commiteado — congelación FINAL."
else
  echo "[P0-FREEZE] congelación PROVISIONAL (kFrozen=false). Para congelar de verdad: $0 --final tras el commit del fix."
fi

CMAKE330=/home/hatch/workspace/cmake-3.30.3-linux-x86_64/bin/cmake
CXX=g++-14 CC=gcc-14 "$CMAKE330" -S app/src/main/cpp/tests -B build/tests-p0 \
  -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
  -DIVANNA_TEST_ENABLE_ASAN=OFF -DIVANNA_TEST_ENABLE_TSAN=OFF
cmake --build build/tests-p0 --target test_p0_reference_lock -j2

cd build/tests-p0
if [ "$FINAL" = 1 ]; then
  P0_FREEZE=final ./test_p0_reference_lock --gtest_filter='P0ReferenceLock.Freeze'
else
  P0_FREEZE=1 ./test_p0_reference_lock --gtest_filter='P0ReferenceLock.Freeze'
fi
cd ../..
cmake --build build/tests-p0 --target test_p0_reference_lock -j2
cd build/tests-p0
./test_p0_reference_lock --gtest_filter='P0ReferenceLock.*'
echo "[P0-FREEZE] listo. Header: app/src/main/cpp/tests/p0_reference_values.hpp | JSON+WAVs: build/tests-p0/p0_reference.*"
