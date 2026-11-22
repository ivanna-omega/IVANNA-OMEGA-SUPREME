# tests/adversarial/CMakeLists.txt — Proyecto CTest autocontenido del banco adversarial.
# No depende del resto del repo: C++17, sin librerías externas, g++-14.
#   cmake -S tests/adversarial -B /tmp/build-adversarial
#   cmake --build /tmp/build-adversarial -j
#   ctest --test-dir /tmp/build-adversarial --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(adversarial_bank CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-O2 -Wall -Wextra)

add_library(adversarial_core src/adversarial_core.cpp)
target_include_directories(adversarial_core PUBLIC src)
target_link_libraries(adversarial_core PUBLIC m)

add_executable(adversarial_gen src/adversarial_gen.cpp)
target_link_libraries(adversarial_gen PRIVATE adversarial_core)

add_executable(adversarial_harness src/adversarial_harness.cpp)
target_link_libraries(adversarial_harness PRIVATE adversarial_core)

add_executable(adversarial_tests tests/test_adversarial_harness.cpp)
target_link_libraries(adversarial_tests PRIVATE adversarial_core)

enable_testing()

add_test(NAME adversarial_unit COMMAND adversarial_tests)

add_test(NAME adversarial_gen_all
  COMMAND adversarial_gen all ${CMAKE_CURRENT_BINARY_DIR}/bank --seed 0)

# Casos del harness por CLI (generar -> transformar -> medir).
set(RUN_CASE ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_harness_case.sh)
add_test(NAME adversarial_harness_passthrough_click
  COMMAND sh ${RUN_CASE}
    $<TARGET_FILE:adversarial_gen> $<TARGET_FILE:adversarial_harness>
    ${CMAKE_CURRENT_BINARY_DIR}/case_click click_01 "" "--case click_01")
add_test(NAME adversarial_harness_passthrough_sine
  COMMAND sh ${RUN_CASE}
    $<TARGET_FILE:adversarial_gen> $<TARGET_FILE:adversarial_harness>
    ${CMAKE_CURRENT_BINARY_DIR}/case_sine silence_01 "" "--freq 1000 --case silence_01")
add_test(NAME adversarial_harness_delay
  COMMAND sh ${RUN_CASE}
    $<TARGET_FILE:adversarial_gen> $<TARGET_FILE:adversarial_harness>
    ${CMAKE_CURRENT_BINARY_DIR}/case_delay click_01 "--delay 128"
    "--case click_01 --expect-latency 128")
add_test(NAME adversarial_harness_inverted
  COMMAND sh ${RUN_CASE}
    $<TARGET_FILE:adversarial_gen> $<TARGET_FILE:adversarial_harness>
    ${CMAKE_CURRENT_BINARY_DIR}/case_inverted silence_01 "--invert"
    "--freq 1000 --case silence_01")
set_tests_properties(adversarial_harness_inverted PROPERTIES WILL_FAIL TRUE)
add_test(NAME adversarial_harness_gain
  COMMAND sh ${RUN_CASE}
    $<TARGET_FILE:adversarial_gen> $<TARGET_FILE:adversarial_harness>
    ${CMAKE_CURRENT_BINARY_DIR}/case_gain silence_01 "--gain-db 6"
    "--freq 1000 --case silence_01")
set_tests_properties(adversarial_harness_gain PROPERTIES WILL_FAIL TRUE)
