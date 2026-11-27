#!/bin/bash
# build_offline.sh — compila la herramienta offline de Fase 3 (host).
# Cero red, cero gasto: solo g++-14 local.
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++-14}"
"$CXX" -O2 -std=c++17 -Wall -Wextra -o coupled_predict coupled_predict.cpp
echo "OK: ./coupled_predict compilado"
./coupled_predict | head -30
echo "--- humo: run 100 Hz 1 V 0.1 s ---"
./coupled_predict run 100 1 0.1 | head -5
