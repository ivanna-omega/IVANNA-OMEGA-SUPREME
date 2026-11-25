# CMakeLists standalone de P7 Conservación Total (Frente E, 2026-11-24).
#
# Módulo NUEVO e independiente: no toca JNI, UI ni el daemon. Cableado
# 2026-11-24 (rama corona-cableado): su .cpp va en los .so de la app y
# FusedChain lo usa vía totalConservationCertificate() (solo lectura).
# Patrón standalone de P6 (universal_hearing): su test vive aquí,
# autocontenido, y se corre así:
#   cmake -S app/src/main/cpp/fused/total_conservation -B /tmp/p7_build \
#         -DCMAKE_CXX_COMPILER=g++-14
#   cmake --build /tmp/p7_build
#   ctest --test-dir /tmp/p7_build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(ivanna_total_conservation_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_total_conservation
  total_conservation_test.cpp
  total_conservation.cpp
)
target_include_directories(test_total_conservation PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_total_conservation PRIVATE -Wall -Wextra)
# Igual que P6 (v2.5.491): GCC 14.3.0 sufre un ICE en el test en -O3.
# Compilar solo el archivo del test en -O0 evita el pase que se rompe.
set_source_files_properties(total_conservation_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")

add_test(NAME test_total_conservation COMMAND test_total_conservation)
