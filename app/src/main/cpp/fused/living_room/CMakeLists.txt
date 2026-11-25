# CMakeLists standalone de P9 Sala Viva (Frente E, 2026-11-24).
#
# Módulo NUEVO e independiente: no toca JNI, UI ni el daemon. Cableado
# 2026-11-24 (rama corona-cableado): su .cpp va en los .so de la app y
# FusedChain lo expone como modelo/datos (livingRoomDeltaDb y familia);
# NINGÚN modo se aplica como efecto activo. Patrón standalone de P6
# (universal_hearing):
#   cmake -S app/src/main/cpp/fused/living_room -B /tmp/p9_build \
#         -DCMAKE_CXX_COMPILER=g++-14
#   cmake --build /tmp/p9_build
#   ctest --test-dir /tmp/p9_build --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(ivanna_living_room_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

add_executable(test_living_room
  living_room_test.cpp
  living_room.cpp
)
target_include_directories(test_living_room PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(test_living_room PRIVATE -Wall -Wextra)
# Igual que P6 (v2.5.491): GCC 14.3.0 sufre un ICE en el test en -O3.
set_source_files_properties(living_room_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")

add_test(NAME test_living_room COMMAND test_living_room)
