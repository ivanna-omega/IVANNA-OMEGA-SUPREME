# CMakeLists standalone del gemelo acústico (2026-11-21).
#
# Este módulo es NUEVO e independiente: no toca FusedChain, JNI, UI ni el
# daemon. Para no chocar con los otros agentes en paralelo (que trabajan
# sobre tests/CMakeLists.txt), su test vive aquí, autocontenido, y se corre
# así (g++-14, ctest):
#   cmake -S app/src/main/cpp/fused/acoustic_twin -B /tmp/acoustic_twin_build \
#         -DCMAKE_CXX_COMPILER=g++-14
#   cmake --build /tmp/acoustic_twin_build
#   ctest --test-dir /tmp/acoustic_twin_build --output-on-failure
#
# Enganche futuro a la suite host principal (lo hace el dueño del flanco
# Tests host, NO este módulo): una línea en tests/CMakeLists.txt,
#   add_executable(test_acoustic_twin
#     test_acoustic_twin.cpp ../fused/acoustic_twin/acoustic_twin.cpp)
#   ... (mismo patrón standalone que test_route_b_neutral_snapshot).
cmake_minimum_required(VERSION 3.16)
project(ivanna_acoustic_twin_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_acoustic_twin
  acoustic_twin_test.cpp
  acoustic_twin.cpp
)
target_include_directories(test_acoustic_twin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_acoustic_twin PRIVATE -Wall -Wextra)

add_test(NAME test_acoustic_twin COMMAND test_acoustic_twin)
