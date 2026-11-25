# CMakeLists standalone de P10 Gemelo Humano (Frente E, 2026-11-24).
#
# Módulo NUEVO e independiente: no toca FusedChain, JNI, UI ni el daemon.
# Patrón standalone de P6 (universal_hearing):
#   cmake -S app/src/main/cpp/fused/human_twin -B /tmp/p10_build \
#         -DCMAKE_CXX_COMPILER=g++-14
#   cmake --build /tmp/p10_build
#   ctest --test-dir /tmp/p10_build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(ivanna_human_twin_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_human_twin
  human_twin_test.cpp
  human_twin.cpp
)
target_include_directories(test_human_twin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_human_twin PRIVATE -Wall -Wextra)
# Igual que P6 (v2.5.491): GCC 14.3.0 sufre un ICE en el test en -O3.
set_source_files_properties(human_twin_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")

add_test(NAME test_human_twin COMMAND test_human_twin)
