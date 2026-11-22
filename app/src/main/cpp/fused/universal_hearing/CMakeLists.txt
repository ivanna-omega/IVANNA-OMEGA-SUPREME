# CMakeLists standalone del Modelo Universal de Audición (P6, 2026-11-21).
#
# Este módulo es NUEVO e independiente: no toca FusedChain, JNI, UI ni el
# daemon. Para no chocar con los otros agentes en paralelo (que trabajan
# sobre tests/CMakeLists.txt), su test vive aquí, autocontenido, y se corre
# así (g++-14, ctest):
#   cmake -S app/src/main/cpp/fused/universal_hearing -B /tmp/uh_build \
#         -DCMAKE_CXX_COMPILER=g++-14
#   cmake --build /tmp/uh_build
#   ctest --test-dir /tmp/uh_build --output-on-failure
#
# Enganche futuro a la suite host principal (lo hace el dueño del flanco
# Tests host, NO este módulo): una línea en tests/CMakeLists.txt,
#   add_executable(test_universal_hearing
#     test_universal_hearing.cpp
#     ../fused/universal_hearing/universal_hearing.cpp)
#   ... (mismo patrón standalone que test_acoustic_twin).
cmake_minimum_required(VERSION 3.16)
project(ivanna_universal_hearing_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_universal_hearing
  universal_hearing_test.cpp
  universal_hearing.cpp
)
target_include_directories(test_universal_hearing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_universal_hearing PRIVATE -Wall -Wextra)
# WORKAROUND (2026-11-21, v2.5.491): GCC 14.3.0 (el de la CI) sufre un ICE
# (internal compiler error) en fold_relational_const durante el pase dom
# (jump threading) al compilar universal_hearing_test.cpp en -O3 (Release).
# El código es C++ válido: g++-14 14.2.0 local lo compila bien en -O3 y
# -O2. Compilar solo el archivo del test en -O0 evita el pase que se
# rompe; el módulo (universal_hearing.cpp) se queda en -O3.
set_source_files_properties(universal_hearing_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")

add_test(NAME test_universal_hearing COMMAND test_universal_hearing)
