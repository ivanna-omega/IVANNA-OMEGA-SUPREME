cmake_minimum_required(VERSION 3.30.3)
project(ivanna_cpp_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(IVANNA_TEST_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(IVANNA_TEST_ENABLE_TSAN "Enable ThreadSanitizer" OFF)

find_package(Threads REQUIRED)

# ── GoogleTest vendoreado (cero red en CI) ────────────────────────────────────
# FetchContent de https://github.com/google/googletest/archive/v1.14.0.zip
# fallaba en CI por timeouts de red, rate-limiting de GitHub y DNS flaky.
# Cada fallo del job de tests bloqueaba el build de APK completo via needs:.
# GTest 1.14.0 está ahora en third_party/googletest/ — determinista y offline.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK   ON  CACHE BOOL "" FORCE)
add_subdirectory(third_party/googletest EXCLUDE_FROM_ALL)

enable_testing()
include(GoogleTest)

add_library(ivanna_dsp_under_test STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/PhysicalTargetDispatcher.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/Compressor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/MultibandCompressor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/SafetyLimiter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/variable_asrc.cpp
  # Propagación física de la onda + Ley de Curvatura de ORBE (Trabajador B,
  # 2026-11-18): funciones puras en doble precisión, sin I/O en camino
  # caliente. No reescribe fused_spatial.cpp (camino actual intacto).
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/physical_propagation.cpp
  # M0 (salto, Luna): contrato temporal único — seqlock AudioState.
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/closed_loop/audio_state.cpp
  # M3 (plan del salto, salto/m3-gemelo): estimador del gemelo con
  # confianza por variable. Solo depende de headers del gemelo (no
  # llama a símbolos de acoustic_twin.cpp), así que puede vivir en
  # esta librería sin duplicar símbolos.
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/closed_loop/twin_estimator.cpp
  # M8 (salto, Luna): supervisor de incertidumbre + rollback — el árbitro
  # único de intervención (twin_confidence.hpp / audio_state.hpp son los
  # contratos compartidos y no se compilan aquí: solo se leen sus structs).
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/closed_loop/uncertainty_supervisor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/ParametricEQ.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/HarmonicExciter.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/StereoWidener.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../dsp/GainStage.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../anti_dolby.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../neuromorphic/volterra_h2_symmetric.cpp
  # IvannaLab (coordinación con flanco IvannaLab, commit 2c0826c3): su sesión
  # dejó test_ivannalab.cpp listo pero sin target — engancharlo exigía tocar
  # este archivo (flanco Tests host). Enganchado por el dueño del flanco:
  # medidores reales (LUFS BS.1770-4 con gates, SNR estadístico, LRA) bajo CI.
  ${CMAKE_CURRENT_SOURCE_DIR}/../ivannalab/ivannalab.cpp
  # Módulo 1 (Fase Cero): pronosticador LPC 5–10 ms con validación en lazo
  # cerrado. Medido en host con señales sintéticas (test_predictive_audio_engine).
  ${CMAKE_CURRENT_SOURCE_DIR}/../predictive_audio_engine.cpp
  # Telemetría del control cerrado (plan del salto, Luna 3.14): anillo
  # lock-free de frames a 10 Hz + canal de fallos inmediato. Solo registra.
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/closed_loop/telemetry.cpp
  # M5 (salto, simulador de acciones): J + restricciones duras sobre
  # OperatorSimulator. Se reutiliza el simulador, no se duplica: entra
  # aquí para que test_action_simulator (y futuros tests de M6/M8) lo
  # enlacen vía ivanna_dsp_under_test. test_perceptual_mpc compila
  # operator_simulator.cpp directo y NO enlaza esta librería: sin
  # colisión de símbolos.
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/closed_loop/action_simulator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/perceptual_mpc/operator_simulator.cpp
  #   ${CMAKE_CURRENT_SOURCE_DIR}/../IvannaAudioClassifier.cpp
  #   ${CMAKE_CURRENT_SOURCE_DIR}/../IvannaFusionCore.cpp
  #   ${CMAKE_CURRENT_SOURCE_DIR}/../EvolutionaryEQ.cpp
  #   ${CMAKE_CURRENT_SOURCE_DIR}/../HrtfManager.cpp
  #   ${CMAKE_CURRENT_SOURCE_DIR}/../Psychoacoustics.cpp
)

target_include_directories(ivanna_dsp_under_test PUBLIC
  ..
  ../include
  ../visualizer
)

target_compile_options(ivanna_dsp_under_test PRIVATE -Wall -Wextra)

set(IVANNA_SAN_FLAGS)
if(IVANNA_TEST_ENABLE_ASAN)
  list(APPEND IVANNA_SAN_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
  add_compile_definitions(IVANNA_SANITIZER_ACTIVE=1)
elseif(IVANNA_TEST_ENABLE_TSAN)
  list(APPEND IVANNA_SAN_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
  add_compile_definitions(IVANNA_SANITIZER_ACTIVE=1)
endif()

foreach(tgt ivanna_dsp_under_test)
  if(IVANNA_SAN_FLAGS)
    target_compile_options(${tgt} PRIVATE ${IVANNA_SAN_FLAGS})
    target_link_options(${tgt} PRIVATE ${IVANNA_SAN_FLAGS})
  endif()
endforeach()

# C11/510 — cuarentena de tests flaky de wall-clock: ivanna_add_test(<name>
# LABELS QUARANTINED) etiqueta TODOS los tests descubiertos de ese binario
# con QUARANTINED (propiedad CTest LABELS). Sin LABELS el comportamiento es
# idéntico al de antes. Los tests en cuarentena NO bloquean la puerta por
# push/PR (scripts/run_ctest.sh corre con -LE QUARANTINED — línea pendiente,
# ver reporte C11) y corren en el carril semanal
# (.github/workflows/tests-quarantined-weekly.yml, lunes 04:23 UTC, 90 min)
# y en el self-hosted, donde la carga es conocida. Salida: N noches verdes
# seguidas (se proponen 4, a confirmar por Esse — propuesta, no dato medido)
# → se quita la etiqueta. Una noche roja mantiene el test en cuarentena.
function(ivanna_add_test name)
  cmake_parse_arguments(ARG "" "" "LABELS" ${ARGN})
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE GTest::gtest_main ivanna_dsp_under_test)
  target_include_directories(${name} PRIVATE .. ../include ../visualizer)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
  if(IVANNA_SAN_FLAGS)
    target_compile_options(${name} PRIVATE ${IVANNA_SAN_FLAGS})
    target_link_options(${name} PRIVATE ${IVANNA_SAN_FLAGS})
  endif()
  if(ARG_LABELS)
    gtest_discover_tests(${name} PROPERTIES LABELS "${ARG_LABELS}")
  else()
    gtest_discover_tests(${name})
  endif()
endfunction()

ivanna_add_test(gammatone_numerical_stability)
ivanna_add_test(no_denormals_low_level)
ivanna_add_test(test_phase_oracle_kalman)
ivanna_add_test(dsp_core_stability)
# Fatigue Mitigator local (v2.5.428): high-cut IIR 1er orden en Ruta A.
ivanna_add_test(test_fatigue_highcut)
# Paquete IVH2 de kernels Volterra + carga segura (coeficientes SINTÉTICOS; no calibra el Genezy).
ivanna_add_test(test_volterra_kernel_package)
# Kernel Volterra H1/H2 del Sony Genezi MHC-PZ1D (IVK1 GENEZY01, K=64,
# h1 normalizada por L1). El .ivk1 lo genera
# tools/volterra_h2/build_genezy_mhc_pz1d_ivk1.py; la ruta llega por
# definición para que el test lea el paquete generado (no embebido).
ivanna_add_test(test_volterra_genezy_mhc_pz1d)
target_compile_definitions(test_volterra_genezy_mhc_pz1d PRIVATE
  IVANNA_GENEZY_IVK1_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/kernels/genezy_mhc_pz1d.ivk1")
# Kernel Volterra H1/H2 REFINADO de la cadena Moto G85 5G + DAC USB-C +
# Sony Genezi MHC-PZ1D (IVK1 GENEZY01, K=64, h1 normalizada por L1,
# h2 solo diagonal, device MOTO_G85_PZ1D). El .ivk1 lo genera
# tools/volterra_h2/build_moto_g85_pz1d_ivk1.py; es el kernel DEFAULT
# de Volterra en v2.5.369 (el Genezi anterior queda como alternativo).
ivanna_add_test(test_volterra_moto_g85_pz1d)
target_compile_definitions(test_volterra_moto_g85_pz1d PRIVATE
  IVANNA_PZ1D_IVK1_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/kernels/moto_g85_pz1d.ivk1")
# Selector de kernel Volterra en runtime (v2.5.369): reemplazo PZ1D <->
# Genezi vía VolterraH2Symmetric::replaceKernelPackage (fail-closed,
# quiescencia con tope, puerta de producción antes de aplicar). Lee
# AMBOS .ivk1 (el default PZ1D y el alternativo Genezi).
ivanna_add_test(test_volterra_kernel_switch)
target_compile_definitions(test_volterra_kernel_switch PRIVATE
  IVANNA_PZ1D_IVK1_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/kernels/moto_g85_pz1d.ivk1"
  IVANNA_GENEZY_IVK1_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../assets/kernels/genezy_mhc_pz1d.ivk1")
ivanna_add_test(test_hires_config)
# Continuidad de ganancia del techo por etapa (tronidos al subir volumen).
ivanna_add_test(test_stage_ceiling_continuity)
# Codificación Ambisonics (HOA) real, plano horizontal, orden ≤2 — ver
# spatial/HoaGainMatrix.hpp. Header-only (pura matemática, sin estado), no
# necesita entrar a la librería ivanna_dsp_under_test.
ivanna_add_test(test_hoa_gain_matrix)
# Closed-form bounded MAP solver, exact minimizer and projected KKT residual.
ivanna_add_test(test_perceptual_optimizer_v2)
# Experimental Fisher-Rao natural-gradient step; isolated from production DSP.
ivanna_add_test(test_perceptual_geometry_v3)
# Production-path integration tests: exercise SaFRoomOptimizer, not only its helper.
ivanna_add_test(test_saf_room_optimizer)
ivanna_add_test(test_wasserstein_latent_flow)
# Bounded adaptive residual geometry feedback on the SAF 7-D control state.
ivanna_add_test(test_topological_feedback_operator)
# Bounded Berry-phase LUT and fixed-size complex FIR primitives.
ivanna_add_test(test_gauge_field_engine)
# Second-order phase/frequency tracking PLL (userspace; not a DAC clock driver).
ivanna_add_test(test_g_pll_core)
# Dinámica del bucle de escritura del BridgePlayer (Ruta A, v2.5.457):
# reproduce el fallo del snap 455 (buffer clavado en lleno + contenido
# perdido) y valida el fix raíz (pacing: nivel al objetivo sin soltar nada).
ivanna_add_test(test_bridgeplayer_level_control)
# FIX 5 (v2.5.465) — trim LUFS del reproductor: zona muerta ±0.5 dB,
# pendiente máx 0.5 dB/s (no por bloque), freeze en silencio, tope ±6 dB.
# Header-only (perceptual_loudness.hpp); valida que el trim ya no
# cabalgue la música dentro del tema (volumen sube/baja, Ruta A).
ivanna_add_test(test_bridgeplayer_lufs_trim)
# Monotonic software clock monitor for callback cadence (diagnostic only, not DAC phase).
ivanna_add_test(test_audio_callback_clock)
# EVOL-LIVEPATH (2026-11-26): tasa del FusionCore desde el reloj de
# callback medido (SET_CONFIG jamás llega en este HAL).
ivanna_add_test(test_effect_sample_rate)
# Estimator fed only by real HAL/driver presentation timestamps and frame positions.
ivanna_add_test(test_audio_hardware_clock)
ivanna_add_test(test_audio_hardware_clock_bridge)
# EVOL-LIVEPATH (2026-11-26): muestra de reloj HAL desde el músculo ASRC
# (hardware_rate_millihz en 0 por falta de publicador en vivo).
ivanna_add_test(test_hardware_clock_from_muscle)
# IVANNA-ASRC-X1: núcleo variable y control fail-closed (sin ruta productiva habilitada).
ivanna_add_test(test_variable_asrc)
ivanna_add_test(test_hal_output_rate_control)
# Route-B adaptive bypass: per-sample bounded ramp back to unity on disable.
ivanna_add_test(test_adaptive_gain_ramp)
# v2.5.386 — Ruta B: limitadores de pendiente del objetivo de ganancia
# adaptativa y tope 4:1 del compresor (bombeo de la 384/385). Header-only.
ivanna_add_test(test_rt_gain_slew)

# Detector de transientes real (envolvente rápida vs lenta) — sub-módulo
# propio de la Fase 1 del encargo HOA->Binaural->Upmixing, con sus propios
# tests por exigencia del encargo (NO mezclado dentro del upmixer).
# Header-only, sin estado compartido: no entra a ivanna_dsp_under_test.
ivanna_add_test(test_transient_detector)

# Módulo 1 (Fase Cero): PredictiveAudioEngine. Prueba lo que SÍ puede hacer
# (pronóstico de señal predecible, onset por residuo, cuantización entera) y lo
# que NO (ruido → confidence 0, el motor no finge acertar).
ivanna_add_test(test_predictive_audio_engine)

# FIX (gap encontrado en auditoría de Fase 3): test_adaptive_engine.cpp
# existía desde el merge de feature/safetylimiter pero no estaba
# enganchado a ningún target de CMake — CI nunca lo compilaba ni corría.
# Es standalone (su propio main(), sin gtest, sin dependencias del resto
# del árbol de DSP) por diseño explícito del propio archivo — se agrega
# como ejecutable aparte, no vía ivanna_add_test() (que asume gtest).
add_executable(test_adaptive_engine
  ../experimental/adaptive_engine/tests/test_adaptive_engine.cpp
  ../experimental/adaptive_engine/adaptive_decision_engine.cpp
)
target_include_directories(test_adaptive_engine PRIVATE
  ../experimental/adaptive_engine
  ..  # P3/3.5: adaptive_decision_engine.hpp incluye fused/fused_dynamics.hpp
)
target_compile_options(test_adaptive_engine PRIVATE -Wall -Wextra)
target_link_libraries(test_adaptive_engine PRIVATE Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_adaptive_engine PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_adaptive_engine PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_adaptive_engine COMMAND test_adaptive_engine)
# FIX (auditoría CI 2026-09-14, corrida 34319106345 — falso positivo real
# reproducido y confirmado, no una suposición): este ejecutable YA hace
# `return g_failures == 0 ? 0 : 1;` en su main(), así que CTest ya detecta
# el fallo real por código de salida sin ayuda de nada más. La propiedad
# FAIL_REGULAR_EXPRESSION de abajo (idéntica en 6 tests más de este mismo
# archivo: buscan "FALLARON"/"FAIL" como subcadena en TODO el stdout+stderr
# capturado) es puramente redundante con eso — y encima peligrosa: en la
# corrida citada, bajo `ctest -j4`, el output de ESTE test salió marcado
# como fallido porque contenía la subcadena "TEST(S) FALLARON." que en
# realidad pertenecía al resumen final de OTRO binario de test corriendo
# en paralelo (mismo mensaje, misma macro EXPECT, compartida por varios
# archivos de este árbol) — se verificó comparando el log crudo del job
# contra el código fuente de cada test: ninguna aserción de
# test_adaptive_engine falló esa corrida, el test pasó por código de
# salida y aun así CTest lo reportó "Failed" por el regex. Se elimina esta
# propiedad (y las 6 equivalentes) en vez de afinar el regex, porque
# cualquier subcadena compartida por varios binarios sigue siendo
# vulnerable al mismo problema bajo ejecución paralela.

# ── test_ihr1_format: barrera de regresion del lector IHR1 ──────────────────
# Vive en <root>/tests/hrtf/ (fuera de este dir) y antes NO se compilaba en
# ningun target: la regresion que cubre (dataset denso rechazado, IHR1
# truncado publicado a ceros) podia volver sin que CI se enterara.
add_executable(test_ihr1_format
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tests/hrtf/test_ihr1_format.cpp
)
target_include_directories(test_ihr1_format PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_compile_options(test_ihr1_format PRIVATE -Wall -Wextra)
add_test(NAME test_ihr1_format COMMAND test_ihr1_format)
# Ver comentario junto a test_adaptive_engine (arriba): FAIL_REGULAR_EXPRESSION
# removido por redundante con el código de salida y vulnerable a contaminación
# cruzada de stdout entre tests bajo ctest -j (falso positivo confirmado).

# ── test_shm_lifecycle: barrera de regresion del plano de control SHM ───────
# Roadmap flanco control-plane item 4. Compila el shm_manager.cpp REAL del
# daemon en host (su log degrada a stderr sin __ANDROID__) y ejercita el
# ciclo completo contra un backing file en /tmp: layout/ABI (ShmHeader==32,
# offsets SAF=32/heartbeat=48 en sync con Kotlin), init idempotente, seqlock,
# aislamiento de offsets, rechazo de overflow y reattach tras restart.
# Es la guardia que habria cazado el overflow sizeof(OmegaSharedState)>64KiB
# y el SHM_HEARTBEAT_OFF sin definir ANTES de llegar al dispositivo.
add_executable(test_shm_lifecycle
  test_shm_lifecycle.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/shm_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/android_log_compat.cpp
)
target_include_directories(test_shm_lifecycle PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_shm_lifecycle PRIVATE -Wall -Wextra)
target_link_libraries(test_shm_lifecycle PRIVATE Threads::Threads)
add_test(NAME test_shm_lifecycle COMMAND test_shm_lifecycle)

# ── test_daemon_command_responder: regresión v2.5.440 (Ruta B muerta) ──────
# El daemon v2 bindeaba @omega_daemon_socket pero nadie aceptaba: la app
# preguntaba GET_STATUS y jamás recibía respuesta, así que el efecto omega
# nunca se acoplaba a la sesión. Este test fija el contrato que repone
# daemon/control/daemon_command_responder.h (HELLO + GET_STATUS + mapeo
# beacon, y round-trip real por socket abstracto con un cliente que lee
# hasta '}' balanceado, igual que OmegaEngineBridge.exchange()).
# 0044 T5: OpenWriterStartsNeutral necesita la implementación real del bus
# (precedente: test_shm_lifecycle añade sus .cpp al target).
add_executable(test_daemon_command_responder
  test_daemon_command_responder.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/omega_control_bus.cpp
)
target_include_directories(test_daemon_command_responder PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_daemon_command_responder PRIVATE -Wall -Wextra)
target_link_libraries(test_daemon_command_responder PRIVATE Threads::Threads)
add_test(NAME test_daemon_command_responder COMMAND test_daemon_command_responder)

# ── test_route_b_perceptual_state: regresión Ruta B neutra (2026-11-14) ─────
# La app envía ORBE/HOLOS solo por SET_PERCEPTUAL_STATE, pero el daemon v2
# respondía "unsupported" a todo SET_* y republicaba makeDefault() (ORBE 0 /
# HOLOS 0): efecto acoplado, beacon creciendo, salida ≈ entrada. Este test
# fija el contrato que lo repone (perceptual_state.h + responder): parse +
# stash + merge + clamps del servidor viejo + alias SEND_PERCEPTUAL_STATE.
add_executable(test_route_b_perceptual_state test_route_b_perceptual_state.cpp
    ../daemon/core/omega_control_bus.cpp)
target_include_directories(test_route_b_perceptual_state PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_route_b_perceptual_state PRIVATE -Wall -Wextra)
target_link_libraries(test_route_b_perceptual_state PRIVATE Threads::Threads)
add_test(NAME test_route_b_perceptual_state COMMAND test_route_b_perceptual_state)

# ── test_route_b_output_mode: regresión FIX 1 (bofo Genezi, v2.5.465) ───────
# El snapshot traía ORBE=1.0, HOLOS=0.5 al Genezi aunque la puerta de la
# app dijera SPEAKERS: el efecto aplicaba el snapshot sin chequeo de
# dispositivo. El modo de salida viaja en el snapshot (temporal_field
# bits 18..19, OmegaOutputMode) y el efecto lo hace cumplir. Este test
# fija el contrato del control plane: codec, máscaras sin solape, ABI
# intacta (752 / v14), parse + merge + preservación del modo en
# perceptual_state.h + daemon_command_responder.h.
add_executable(test_route_b_output_mode test_route_b_output_mode.cpp
    ../daemon/core/omega_control_bus.cpp)
target_include_directories(test_route_b_output_mode PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_route_b_output_mode PRIVATE -Wall -Wextra)
target_link_libraries(test_route_b_output_mode PRIVATE Threads::Threads)
add_test(NAME test_route_b_output_mode COMMAND test_route_b_output_mode)

# ── test_corona_control_route: HOLOS EN VIVO (evol-holos, 2026-11-26) ──────
# El preset CORONA (HOLOS 1.0 / diffuse 0.85 / spread 1.2) no llegaba al
# engine: el efecto lo clampaba a 0.5/0.5/1.0 y el Genezi codificaba como
# SPEAKERS (puerta -> 0.0). Este test fija la ruta completa (responder +
# apply + clamps canónicos + puertas GENEZI): el engine recibe 1.0/0.85/1.2.
add_executable(test_corona_control_route test_corona_control_route.cpp
    ../daemon/core/omega_control_bus.cpp)
target_include_directories(test_corona_control_route PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_corona_control_route PRIVATE -Wall -Wextra)
target_link_libraries(test_corona_control_route PRIVATE Threads::Threads)
add_test(NAME test_corona_control_route COMMAND test_corona_control_route)

# ── test_p2_telemetry_preserve: P2 (2026-11-26, RMS=0 en vivo) ──────────
# El efecto medía RMS/pico por bloque pero cada adopción del snapshot del
# daemon borraba la telemetría con sus ceros (el daemon nunca escribe esos
# campos). omega_preserve_effect_telemetry() (include/) conserva los campos
# medidos al adoptar. Header-only: no necesita omega_control_bus.cpp.
ivanna_add_test(test_p2_telemetry_preserve)

# ── test_p2_telemetry_mirror: P2 (2026-11-26, RMS=0 en vivo) ─────────────
# El daemon nunca leía el bus local del efecto: el snapshot de control
# publicaba RMS/pico/frames en 0 con música sonando. EffectTelemetryMirror
# (daemon/control/) refleja esa telemetría antes de publicar. Compila el
# omega_control_bus.cpp REAL (writer/reader seqlock + CRC32) sobre SHM en
# /tmp, mismo patrón que test_upmixing_control_plane.
add_executable(test_p2_telemetry_mirror
  test_p2_telemetry_mirror.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/omega_control_bus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/android_log_compat.cpp
)
target_include_directories(test_p2_telemetry_mirror PRIVATE .. ../include)
target_compile_options(test_p2_telemetry_mirror PRIVATE -Wall -Wextra)
target_link_libraries(test_p2_telemetry_mirror PRIVATE GTest::gtest_main Threads::Threads)
gtest_discover_tests(test_p2_telemetry_mirror)

# ── test_p2_snapshot_watchdog: P2 (2026-11-26) ────────────────────────────
# Si el daemon deja de publicar, el efecto seguía eternamente con los
# controles viejos. El watchdog (include/omega_snapshot_freshness.h) cae
# a defaults seguros con rampa de 100 ms tras 3000 ms sin snapshot nuevo
# y recupera con la misma rampa. Header-only, simulación de daemon muerto
# en host con bloques de 40 ms.
ivanna_add_test(test_p2_snapshot_watchdog)

# ── test_route_b_snapshot_status: GET_STATUS expone el snapshot (2026-11-14)
# 4 campos aditivos (snapshot_generation, perceptual_generation,
# orbe_amount, holos_amount) para distinguir "neutro porque la app nunca
# publicó su estado" de "neutro por decisión del usuario".
# Link a omega_control_bus.cpp (2026-11-20, 3er test con este fallo: el
# responder que incluye usa OmegaControlBus y su dtor inline llama a
# close(); sin el .cpp, ld falla con undefined reference — lo mismo que
# perceptual_state y output_mode, corregidos en af8f7438).
add_executable(test_route_b_snapshot_status
  test_route_b_snapshot_status.cpp
  ../daemon/core/omega_control_bus.cpp
)
target_include_directories(test_route_b_snapshot_status PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_route_b_snapshot_status PRIVATE -Wall -Wextra)
target_link_libraries(test_route_b_snapshot_status PRIVATE Threads::Threads)
add_test(NAME test_route_b_snapshot_status COMMAND test_route_b_snapshot_status)

# ── test_route_b_kill_switches: kill switches de Ruta B (2026-11-14) ────────
# El daemon v2 miraba /data/vendor/ivanna_audio/pal_asrc.off (write-stage
# v1) en vez de /data/adb/ivanna_omega/pal_asr.off (sin c, el que usan
# service.sh/supervisor/asrcd); y el default de asrcd era pal_asrc.off.
# Fija el contrato: daemon pal_asr.off, efecto pal_asrc.off, distintos.
add_executable(test_route_b_kill_switches test_route_b_kill_switches.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../magisk_module/engine/asrc/ivanna_asrc_ipc.cpp
)
target_include_directories(test_route_b_kill_switches PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../magisk_module/engine
)
target_compile_options(test_route_b_kill_switches PRIVATE -Wall -Wextra)
target_link_libraries(test_route_b_kill_switches PRIVATE Threads::Threads)
add_test(NAME test_route_b_kill_switches COMMAND test_route_b_kill_switches)

# ── 0023 bofo Ruta B: snapshot neutro de verdad + evidencia espectral ──────
# CAUSA RAÍZ: makeDefault() es el default MAGISTRAL de Ruta A (HRTF on,
# widener 2.14x, intensity 0.92); el daemon v2 lo heredaba como "neutro" y
# el efecto corría HRTF wet 1.0 sobre música estéreo = bofo. makeNeutral()
# es la identidad acústica; los daemons la publican como base.
add_executable(test_route_b_neutral_snapshot test_route_b_neutral_snapshot.cpp)
target_include_directories(test_route_b_neutral_snapshot PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_route_b_neutral_snapshot PRIVATE -Wall -Wextra)
target_link_libraries(test_route_b_neutral_snapshot PRIVATE Threads::Threads)
add_test(NAME test_route_b_neutral_snapshot COMMAND test_route_b_neutral_snapshot)

# ── ABI v14 (Física Nueva): 17 campos np_* al final del snapshot ────────────
# sizeof 688 -> 752, crc32 680 -> 748, OMEGA_CTRL_VERSION 13 -> 14; ningún
# campo v13 ni anterior se mueve (compatibilidad de prefijo). Fija versión,
# offsets, defaults de makeDefault()/makeNeutral(), round-trip con CRC
# (incluida la prueba de que corromper un byte np_* invalida el CRC) y la
# compatibilidad de prefijo. Solo el header; mismo patrón standalone que
# test_route_b_neutral_snapshot (su propio main(), sin gtest).
# v2.5.486: ABI v16 reemplaza a v15 (spsc_ring_factor + yamnet_* añadidos; sizeof 768 -> 792).
add_executable(test_abi_v17_conservation_snapshot test_abi_v17_conservation_snapshot.cpp)
target_include_directories(test_abi_v17_conservation_snapshot PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_options(test_abi_v17_conservation_snapshot PRIVATE -Wall -Wextra)
target_link_libraries(test_abi_v17_conservation_snapshot PRIVATE Threads::Threads)
add_test(NAME test_abi_v17_conservation_snapshot COMMAND test_abi_v17_conservation_snapshot)

# Exoneración del ASRC del módulo: a ratio 1.0 es memcpy bit-exacto y a la
# deriva real (+8.5 ppm) su FIR es plano hasta 20 kHz. Si el bofo fuera el
# ASRC, este test fallaría; que pase lo descarta como causa.
add_executable(test_route_b_asrc_neutral_response test_route_b_asrc_neutral_response.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../magisk_module/engine/asrc/ivanna_asrc_core.cpp
)
target_include_directories(test_route_b_asrc_neutral_response PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../magisk_module/engine
)
target_compile_options(test_route_b_asrc_neutral_response PRIVATE -Wall -Wextra)
target_link_libraries(test_route_b_asrc_neutral_response PRIVATE Threads::Threads)
add_test(NAME test_route_b_asrc_neutral_response COMMAND test_route_b_asrc_neutral_response)

# Peine en captura: la mezcla (original 100% + procesado 40% retrasado
# 130 ms) es EN EL MEZCLADOR (dos AudioTracks), no acústica. Simulación
# en host con evidencia espectral: muescas cada 7.69 Hz, rizo 7.36 dB,
# y plano con mix 0 (no re-inyectar).
add_executable(test_capture_comb_filter test_capture_comb_filter.cpp)
target_compile_options(test_capture_comb_filter PRIVATE -Wall -Wextra)
target_link_libraries(test_capture_comb_filter PRIVATE Threads::Threads)
add_test(NAME test_capture_comb_filter COMMAND test_capture_comb_filter)

# -- test_effect_beacon: senal REAL de que omega_effect procesa (Ruta A/B) ------
# Contrato del beacon mmap (omega_effect_beacon.h): clasificacion pura, round-trip
# escritor->lector, instancias multiples sin underflow y rechazo de magic invalido.
add_executable(test_effect_beacon test_effect_beacon.cpp)
target_include_directories(test_effect_beacon PRIVATE .. ../include)
target_compile_options(test_effect_beacon PRIVATE -Wall -Wextra)
target_link_libraries(test_effect_beacon PRIVATE Threads::Threads)
add_test(NAME test_effect_beacon COMMAND test_effect_beacon)

# -- test_no_effect_rearm: re-arme del auto-enable tras DISABLE espurio (v2.5.470) --
# En este HAL EFFECT_CMD_ENABLE jamás llega pero EFFECT_CMD_DISABLE sí (al
# cambiar de música): el DISABLE bloqueaba el auto-enable para siempre y el
# efecto quedaba en passthrough hasta una sesión nueva. El fix re-arme tras
# ~1 s de audio sostenido (kNoEffectRearmFrames). omega_effect.cpp está
# BLOQUEADO para host (REPORT 0021), así que el test fija la decisión pura
# (include/omega_effect_no_effect.h); el cableado en omega_process() se
# verifica en el teléfono.
add_executable(test_no_effect_rearm test_no_effect_rearm.cpp)
target_include_directories(test_no_effect_rearm PRIVATE .. ../include)
target_compile_options(test_no_effect_rearm PRIVATE -Wall -Wextra)
target_link_libraries(test_no_effect_rearm PRIVATE Threads::Threads)
add_test(NAME test_no_effect_rearm COMMAND test_no_effect_rearm)

# -- test_omega_pcm_format: regresion v2.5.382 (sin audio + tronidos) ---------
# La 382 puso el default de creación del efecto en PCM_16_BIT, pero
# AudioFlinger entrega el buffer en FLOAT (el mixer mezcla en float). Con
# PCM16 por defecto, frameBytes era 4 en vez de 8 y el deinterleave leía
# float como int16 → sin audio + tronidos. Este test fija el default en
# PCM_FLOAT y el round-trip por formato (include/omega_pcm_format.h).
add_executable(test_omega_pcm_format test_omega_pcm_format.cpp)
target_include_directories(test_omega_pcm_format PRIVATE .. ../include)
target_compile_options(test_omega_pcm_format PRIVATE -Wall -Wextra)
target_link_libraries(test_omega_pcm_format PRIVATE Threads::Threads)
add_test(NAME test_omega_pcm_format COMMAND test_omega_pcm_format)

# ── test_effect_local_files: pre-creación 0666 de archivos locales de Ruta B ──
# Regresión del FIX Moto G85 2026-11-09 (daemon/effect_local_files.h): el efecto
# (UID no-root) no puede crear /data/local/tmp/omega_effect_ctrl_local ni
# omega_effect_beacon; el demonio (root) los pre-crea en cada arranque. El test
# fija: creación con 0666+tamaño exacto, no tocar archivo con tamaño correcto,
# dimensionar vacío, no truncar parcial, rechazar args inválidos, idempotencia.
add_executable(test_effect_local_files test_effect_local_files.cpp)
target_include_directories(test_effect_local_files PRIVATE .. ../include)
target_compile_options(test_effect_local_files PRIVATE -Wall -Wextra)
target_link_libraries(test_effect_local_files PRIVATE Threads::Threads)
add_test(NAME test_effect_local_files COMMAND test_effect_local_files)

# ── test_audio_effect_library_tag: AUDIO_EFFECT_LIBRARY_TAG == valor real AOSP ──
# Regresión del hallazgo v2.5.139: el shim audio_effect_compat.h tenía los
# shifts de bits de AUDIO_EFFECT_LIBRARY_TAG en orden inverso al real de AOSP
# (0x544C4541 en vez de 0x41454C54). EffectsXmlConfigLoader::loadLibrary() del
# audioserver real rechaza cualquier librería cuyo tag no coincida EXACTO,
# antes de cualquier otro chequeo (XML, SELinux, etc). Sin este test, un
# futuro cambio accidental al shim vuelve a romper esto en silencio.
add_executable(test_audio_effect_library_tag test_audio_effect_library_tag.cpp)
target_include_directories(test_audio_effect_library_tag PRIVATE .. ../include)
target_compile_options(test_audio_effect_library_tag PRIVATE -Wall -Wextra)
target_link_libraries(test_audio_effect_library_tag PRIVATE GTest::gtest_main)
gtest_discover_tests(test_audio_effect_library_tag)

# ── test_audio_effect_abi_layout: layouts de structs/vtables contra AOSP ──────
# Caza la corrupción ABI de effect_descriptor_t (cpuLoad/memoryUsage son uint16_t)
# y fija offsets/tamaño del descriptor y el orden de los slots de callbacks.
add_executable(test_audio_effect_abi_layout test_audio_effect_abi_layout.cpp)
target_include_directories(test_audio_effect_abi_layout PRIVATE .. ../include)
target_compile_options(test_audio_effect_abi_layout PRIVATE -Wall -Wextra)
target_link_libraries(test_audio_effect_abi_layout PRIVATE GTest::gtest_main)
gtest_discover_tests(test_audio_effect_abi_layout)

# ── test_upmixing_control_plane: evidencia del canal app->daemon->SHM->effect ──
# Compila el omega_control_bus.cpp REAL (writer/reader seqlock + CRC32) y
# verifica que el snapshot transporta upmixing_enabled/immersivity con
# integridad cross-process. Sin red, sin NDK: SHM en /tmp.
add_executable(test_upmixing_control_plane
  test_upmixing_control_plane.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/omega_control_bus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/android_log_compat.cpp
)
target_include_directories(test_upmixing_control_plane PRIVATE .. ../include)
target_compile_options(test_upmixing_control_plane PRIVATE -Wall -Wextra)
target_link_libraries(test_upmixing_control_plane PRIVATE GTest::gtest_main Threads::Threads)
gtest_discover_tests(test_upmixing_control_plane)
# Ver comentario junto a test_adaptive_engine (arriba): FAIL_REGULAR_EXPRESSION
# removido por redundante con el código de salida y vulnerable a contaminación
# cruzada de stdout entre tests bajo ctest -j (falso positivo confirmado).

# ── test_user_flags_channel (v2.5.16): app -> daemon -> snapshot -> efecto ────────────
# Compila el CommandServer REAL + omega_control_bus.cpp + shm_manager.cpp en host: un comando JSON
# SET_USER_FLAGS debe terminar como bit del snapshot publicado (y sobrevivir a otros comandos).
add_executable(test_user_flags_channel
  test_user_flags_channel.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/control/command_server.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/omega_control_bus.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/shm_manager.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../daemon/core/android_log_compat.cpp
)
target_include_directories(test_user_flags_channel PRIVATE .. ../include host_stubs)   # android/log.h de command_server.cpp
target_compile_options(test_user_flags_channel PRIVATE -Wall -Wextra)
target_link_libraries(test_user_flags_channel PRIVATE GTest::gtest_main Threads::Threads)
gtest_discover_tests(test_user_flags_channel)

# RIR dataset (200 salas medidas, integrado 2026-08-12): valida el loader
# contra el CSV+WAV reales shippeados en magisk_module/, no fixtures
# inventados. La ruta se pasa como macro de compilación, resuelta en
# configure-time desde CMAKE_SOURCE_DIR (tests/ -> cpp/ -> main/ -> app/ ->
# raíz del repo -> magisk_module/...) para no hardcodear una ruta absoluta
# que solo funcione en un entorno específico.
set(IVANNA_RIR_DIR "${CMAKE_SOURCE_DIR}/../../../../../magisk_module/system/etc/ivanna_omega/rir")
get_filename_component(IVANNA_RIR_DIR "${IVANNA_RIR_DIR}" ABSOLUTE)
add_executable(test_rir_dataset
  test_rir_dataset.cpp
  ../spatial/RirDataset.cpp
)
target_compile_definitions(test_rir_dataset PRIVATE
  IVANNA_RIR_DATASET_DIR="${IVANNA_RIR_DIR}"
)
target_compile_options(test_rir_dataset PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_rir_dataset PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_rir_dataset PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_rir_dataset COMMAND test_rir_dataset)
# Ver comentario junto a test_adaptive_engine (arriba): FAIL_REGULAR_EXPRESSION
# removido por redundante con el código de salida y vulnerable a contaminación
# cruzada de stdout entre tests bajo ctest -j (falso positivo confirmado).

# FIX (mismo gap encontrado en test_adaptive_engine, ahora en
# test_close_loop.cpp — "aceptación de Fase 4B", agregado en 74e51c4):
# tampoco estaba enganchado a ningún target de CMake. Verificado
# localmente antes de este commit (g++ directo): compila limpio y las
# 5 aserciones pasan. Usa assert() de <cassert> — este CMakeLists no
# define NDEBUG en ningún lado, así que quedan activos por default.
add_executable(test_close_loop
  ../experimental/adaptive_engine/tests/test_close_loop.cpp
  ../experimental/adaptive_engine/adaptive_decision_engine.cpp
)
target_include_directories(test_close_loop PRIVATE
  ../experimental/adaptive_engine
  ..  # P3/3.5: adaptive_decision_engine.hpp incluye fused/fused_dynamics.hpp
)
target_compile_options(test_close_loop PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_close_loop PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_close_loop PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_close_loop COMMAND test_close_loop)

# Gap D) del prompt de Fase 1 (cerrar loop adaptativo): existían tests de
# valores puntuales (test_close_loop) y de concurrencia corta (stress test
# dentro de test_adaptive_engine), pero ninguno corría el motor real
# (start() con su std::thread real de controlLoop a 50ms) bajo carga
# sostenida — el tipo de prueba que revela hangs de stop(), deriva
# numérica, o fugas de hilos en ciclos start/stop repetidos, que un test
# de assertions puntuales no puede detectar. Verificado localmente antes
# de este commit: 8/8 corridas limpias (cada una ~3.5s de motor real
# corriendo con productor/consumidor concurrentes).
add_executable(test_stability
  ../experimental/adaptive_engine/tests/test_stability.cpp
  ../experimental/adaptive_engine/adaptive_decision_engine.cpp
)
if(DEFINED ENV{IVANNA_CI_FAST})
  target_compile_definitions(test_stability PRIVATE IVANNA_CI_FAST=1)
endif()
target_include_directories(test_stability PRIVATE
  ../experimental/adaptive_engine
  ..  # P3/3.5: adaptive_decision_engine.hpp incluye fused/fused_dynamics.hpp
)
target_compile_options(test_stability PRIVATE -Wall -Wextra)
target_link_libraries(test_stability PRIVATE Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_stability PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_stability PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_stability COMMAND test_stability)
# Ver comentario junto a test_adaptive_engine (arriba): FAIL_REGULAR_EXPRESSION
# removido por redundante con el código de salida y vulnerable a contaminación
# cruzada de stdout entre tests bajo ctest -j (falso positivo confirmado).

# Stress test SPSC real para ControlFrameBus (control_frame.hpp) — el bus
# de producción que audio_control_plane.cpp publica y nativeProcess()
# consume en cada bloque de audio. El seqlock de este archivo ya tuvo un
# bug real de sincronización (continue-en-do-while, ver comentario en
# control_frame.hpp) encontrado y corregido — este test queda como
# barrera de regresión permanente en CI, algo que no existía antes pese
# a ser el bus más crítico de los tres con ese mismo patrón (se lee en
# CADA bloque de audio real, no solo en telemetría).
#
# Se descartó deliberadamente el enfoque std::atomic<ControlFrame> +
# static_assert(is_lock_free) que se propuso originalmente: ControlFrame
# tiene 21+ campos (~90+ bytes), muy por encima del límite práctico de
# lock-free real en hardware ARM64 existente — ese enfoque habría
# compilado pero degradado a un mutex interno oculto, o creado una
# segunda arquitectura de bus paralela y redundante a la que ya existe
# y ya está probada seleccionada correctamente. Este test ejercita el
# ControlFrameBus REAL, con seqlock, tal como corre en producción.
add_executable(test_control_frame_bus_stress
  test_control_frame_bus_stress.cpp
)
target_include_directories(test_control_frame_bus_stress PRIVATE
  ..
  ../include
)
target_compile_options(test_control_frame_bus_stress PRIVATE -Wall -Wextra)
target_link_libraries(test_control_frame_bus_stress PRIVATE Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_control_frame_bus_stress PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_control_frame_bus_stress PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_control_frame_bus_stress COMMAND test_control_frame_bus_stress)
# Ver comentario junto a test_adaptive_engine (arriba): FAIL_REGULAR_EXPRESSION
# removido por redundante con el código de salida y vulnerable a contaminación
# cruzada de stdout entre tests bajo ctest -j (falso positivo confirmado).

# audio_bus.h — bus lock-free genérico (SeqlockBus/SeqlockBusMulti),
# consolida el patrón seqlock ya usado y corregido en RawMetricsBus/
# AdaptiveStateBus (y ahora también test_control_frame_bus_stress, arriba)
# en una plantilla reutilizable para buses futuros — para que el próximo
# bus lo herede por composición en vez de copiar-pegar el seqlock una
# cuarta vez. Verificado localmente: g++ directo compila limpio, corridas
# repetidas en verde bajo build normal y bajo ASan+UBSan (0 torn reads,
# millones de publicaciones por hilo).
#
# CORRECCIÓN (auditoría 2026-09-14) a una nota previa de esta misma
# sección que decía que el aviso de TSan aquí ya estaba "documentado y
# validado" como el race formal esperado de todo seqlock: no se encontró
# ninguna verificación anterior real de esa afirmación, así que se trata
# como no confirmada (regla del proyecto: no fabricar métricas, verificar
# antes de confiar). Investigación real hecha esta sesión, con
# reproducción local: TSan reporta una carrera en el arranque de los dos
# hilos de testConcurrentStress (frames de std::thread::_State_impl<...>
# ::_M_run, sin ningún frame de código propio del DSP), idéntica tanto
# pasando los argumentos por std::ref como por lambda — ver el comentario
# dentro de test_audio_bus.cpp para el detalle completo. No se agregó
# supresión de TSan para esto: cualquier patrón lo bastante específico
# para no ocultar carreras reales en otras pruebas con hilos no se
# encontró, y una supresión demasiado amplia (por símbolo de runtime) es
# peor que dejar la prueba en rojo en el carril semanal (no bloqueante)
# hasta investigarlo con más herramientas (objdump/addr2line ya se usaron
# sin llegar a una causa raíz definitiva) o cambiar de toolchain.
add_executable(test_audio_bus
  test_audio_bus.cpp
)
target_include_directories(test_audio_bus PRIVATE
  ../include
)
target_compile_options(test_audio_bus PRIVATE -Wall -Wextra)
target_link_libraries(test_audio_bus PRIVATE Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_audio_bus PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_audio_bus PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_audio_bus COMMAND test_audio_bus)
# Ver comentario junto a test_adaptive_engine (arriba): FAIL_REGULAR_EXPRESSION
# removido por redundante con el código de salida y vulnerable a contaminación
# cruzada de stdout entre tests bajo ctest -j (falso positivo confirmado).

# Test de regresión para los bugs corregidos en Parches de Tuning 6-9:
# - SafetyLimiter: clipCount no se duplica (Parche 6)
# - SafetyLimiter: gainReduction se almacena en dB (Parche 6)
# - Compressor: makeup compensa runtimeAmount_ (Parche 7)
ivanna_add_test(test_regression_tuning)

# ── Audio Quality Metrics: SNR · THD · Latencia · Piso numérico ──────────────
# Tests que miden propiedades de calidad del audio procesado, no solo
# que el código no crasha. Umbrales basados en los valores de IvannaLabMonitor:
#   SNR > 90 dB en bypass, THD < 10% en clipping deliberado,
#   latencia < 1ms por bloque de 512 frames, 0 NaN en 1000 bloques.
ivanna_add_test(test_audio_quality_metrics)

# ── Overshoot del HarmonicExciter: barrera de regresión del clipping digital ──
# softClip() (Padé [3/2]) no saturaba hacia ±1 (llegaba a 1.94 con drive máximo)
# y la mezcla dry+wet alcanzaba 1.52 — clipping digital dentro del exciter,
# antes del SafetyLimiter. Este test barre drive × wet × 6 señales de peor
# caso y falla si cualquiera de los dos límites se rompe otra vez.
ivanna_add_test(test_harmonic_exciter_overshoot)

# ── Etapa final maestro 64-bit (v2.5.461): True-Peak 4x a -0.2 dBTP + TPDF ──
# Guarda inter-muestra tras SafetyLimiter + mezcla dry/wet en double + dither
# triangular exacto (±1 LSB, solo 16/24-bit con efecto activo, bypass bit-exacto).
ivanna_add_test(test_master_output_stage)

# ── Excitador: SOLO armónicos (sin ganancia lineal en la fundamental) ─────────
# Medido: el shaper sumaba la fundamental amplificada al seco (+3.4 dB a 3.5 kHz con los
# defaults de arranque) = realce de 3-8 kHz dependiente del nivel. 6 tests: ganancia lineal
# < 0.02 a 6 niveles x 3 frecuencias, peor caso drive/wet, armónicos presentes, drive
# efectivo, sin ráfaga en el onset y acotado/finito.
ivanna_add_test(test_harmonic_exciter_fundamental)

# ── Timing hi-res del SafetyLimiter: barrera de regresión del bug 2026-08-27 ──
# setSampleRate()/setParams() nunca se llamaban: soft-knee anulado (threshold
# == ceiling) y ataque/release a 48 kHz fijos (a 384 kHz: ataque 12 ms,
# release 400 ms -> clipping en transientes + bombeo). 4 tests miden el
# comportamiento temporal real del limiter a 48k y 384k.
ivanna_add_test(test_limiter_hires_timing)

# ── SafetyLimiter con los parámetros REALES de las rutas A y B (-1 dBFS / -0.1 dBFS) ──
# Transparente bajo el umbral, sin aplastar masters calientes (<= 1 dB a 0 dBFS), techo
# inviolable con senos +5 dBFS/impulsos/ruido/percusión, y release limpio. Medido; los
# defaults de setParams() (-4 dBFS) perderían 2.8 dB y ninguna ruta los usa.
ivanna_add_test(test_limiter_route_params)
ivanna_add_test(test_differential_reinjector)
# Anillo lock-free de eventos RT (v2.5.446): fija el fix de las 6 violaciones
# de check_rt_safety (LOG en omega_process / logDspShedStateThrottled).
ivanna_add_test(test_rt_log_ring)
# Matriz de aislamiento bofo/tronidos + presupuesto RT real (v2.5.446):
# peine original+copia, guardián del reinjector, crossfade Volterra 18 ms.
ivanna_add_test(test_bofo_isolation_matrix)
ivanna_add_test(test_neuromorphic_ab_harness)
ivanna_add_test(test_block_peak_guard)
ivanna_add_test(test_measure_baseline)
ivanna_add_test(test_npe_analysis_tap)
ivanna_add_test(test_genre_smooth_state)  # 0046: estabilizador género/mix

# ── Rescatados del olvido (flanco Tests host): huérfanos sin target ──────────
# test_audio_regression.cpp (677 líneas): cada TEST_F documenta un bug real de
# campo (tronidos, clipping, inestabilidad) con commit, síntoma y condición de
# reproducción. Dependía solo de include/*.h + dsp/*.cpp — ya todo en
# ivanna_dsp_under_test. Nunca tuvo target: la suite completa de regresión
# auditiva no se ejecutaba en ninguna parte.
ivanna_add_test(test_audio_regression)

# test_oem_stability_suite.cpp: suite de regresión OEM (transparencia de mix,
# estabilidad IIR, peak guard, smoothing, stress de hilos del daemon). Solo
# depende de gtest — corre en host sin tocar el CMakeLists del daemon (NDK),
# que es de otro flanco.
add_executable(test_oem_stability_suite regression/test_oem_stability_suite.cpp)
target_link_libraries(test_oem_stability_suite PRIVATE GTest::gtest_main)
target_compile_options(test_oem_stability_suite PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_oem_stability_suite PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_oem_stability_suite PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_oem_stability_suite)

# ── test_adaptive_eq_stress: 10 s @ 48 kHz contra ParametricEQ real ─────────
ivanna_add_test(test_adaptive_eq_stress)

# ── Curva real del ParametricEQ: shelves verdaderos (graves profundos / agudos sin plateau) ──
ivanna_add_test(test_eq_shelf_response)
ivanna_add_test(test_eq_fatigue_tilt)

# ── test_ivannalab: suite host del laboratorio de medición IvannaLab ────────
# Coordinación pendiente del flanco IvannaLab (commit 2c0826c3, marcado
# unconfirmed): engancharlo exigía añadir ivannalab.cpp a ivanna_dsp_under_test,
# que es de este flanco. Resuelto por el dueño del flanco Tests host —
# los 7 tests (Peak/True Peak, THD, IMD SMPTE, SNR estadístico, LUFS con
# gates, LRA, estado vacío) corren ahora en la puerta y en CI.
ivanna_add_test(test_ivannalab)

# ── ivanna_benchmark: benchmark de CPU/latencia del DSP — OPCIONAL, fuera de la puerta ──
# Flanco Benchmarks (CLAIMS/benchmarks.md): benchmark_suite.cpp llevaba CERO
# referencias y NUNCA compiló (llamaba HarmonicExciter::setAmount(), API
# inexistente). Corregido y enganchado como target manual: NO corre en la
# puerta de tests (no es un test, es medición de rendimiento — un lento runner
# de CI o una máquina cargada daría números falsos). Se corre a demanda:
#   cmake --build build/tests-host --target ivanna_benchmark
#   ./build/tests-host/ivanna_benchmark 48000 256 15
add_executable(ivanna_benchmark EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tools/benchmark_suite.cpp
)
target_include_directories(ivanna_benchmark PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../..
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../visualizer
)
target_link_libraries(ivanna_benchmark PRIVATE ivanna_dsp_under_test)
target_compile_options(ivanna_benchmark PRIVATE -O2 -Wall -Wextra)

# ── Kernel evolutivo v2: barrera de regresión de las entradas JNI ─────────────
# nativeEvolveStep()/nativeGetMutationRate() estaban declaradas en Kotlin y
# llamadas desde la UI sin implementación nativa (UnsatisfiedLinkError seguro),
# y nativeInitializeEvolution devolvía void contra una firma Boolean. Este test
# ejerce el motor C que respalda esas entradas: init, avance de generación con
# elitismo, y round-trip/clamping de la tasa de mutación.
add_executable(test_evolutionary_kernel
  test_evolutionary_kernel.cpp
  ../evolutionary_kernel_v2.cpp
  ../jni/evolutionary_kernel_jni.cpp
)
target_link_libraries(test_evolutionary_kernel PRIVATE GTest::gtest_main)
target_include_directories(test_evolutionary_kernel PRIVATE .. ../include host_stubs)
target_compile_options(test_evolutionary_kernel PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_evolutionary_kernel PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_evolutionary_kernel PRIVATE ${IVANNA_SAN_FLAGS})
endif()
set_target_properties(test_evolutionary_kernel PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_evolutionary_kernel COMMAND test_evolutionary_kernel)

# ── EQ evolutivo 512 bandas (EvolutionaryEQ.cpp real): barrera de regresión ──
# Sustituye la documentación pasiva de la auditoría dc2ea33f por evidencia
# ejecutable. Enlaza contra el EvolutionaryEQ.cpp REAL (no un fake) y prueba el
# criterio de audio de extremo a extremo: identidad estructural, gate de
# seguridad, calibración convergente, fitness real monótono y procesado finito.
add_executable(test_evolutionary_eq
  test_evolutionary_eq.cpp
  ../EvolutionaryEQ.cpp
)
target_link_libraries(test_evolutionary_eq PRIVATE GTest::gtest_main)
target_include_directories(test_evolutionary_eq PRIVATE .. ../include)
target_compile_options(test_evolutionary_eq PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_evolutionary_eq PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_evolutionary_eq PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_evolutionary_eq COMMAND test_evolutionary_eq)

# ── Auto-evolución 0001 (2026-11-09): PhiSAFInfinity con SAF_GUARD ──
# El motor de evolución detectó que esta ruta SAF no tenía las guardas
# NaN/denominador degenerado que su hermana SAFUpdate sí tiene
# (saf_runtime.h, 2026-09-17): una entrada no-finita o G degenerada
# clavaba el gain en 2.0. Enlaza contra el saf_phi_infinity.cpp REAL.
add_executable(test_saf_phi_infinity
  test_saf_phi_infinity.cpp
  ../saf_phi_infinity.cpp
)
target_link_libraries(test_saf_phi_infinity PRIVATE GTest::gtest_main)
target_include_directories(test_saf_phi_infinity PRIVATE .. ../include)
target_compile_options(test_saf_phi_infinity PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_saf_phi_infinity PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_saf_phi_infinity PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_saf_phi_infinity COMMAND test_saf_phi_infinity)

# ── Auto-Auditoría de Dispositivo (2026-11-09): perfilador nativo ──
# test_device_cpu_profiler.cpp es standalone (su propio main(), sin gtest),
# mismo patrón que test_adaptive_engine: se agrega como ejecutable aparte.
add_executable(test_device_cpu_profiler
  test_device_cpu_profiler.cpp
)
target_include_directories(test_device_cpu_profiler PRIVATE .. ../include)
target_compile_options(test_device_cpu_profiler PRIVATE -Wall -Wextra)
target_link_libraries(test_device_cpu_profiler PRIVATE Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_device_cpu_profiler PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_device_cpu_profiler PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_device_cpu_profiler COMMAND test_device_cpu_profiler)


# FIX (build rojo, 2026-09-15): HoaBinauralDecoder e IntelligentUpmixer usan
# ivanna::HRTFConvolver cuyas definiciones viven en spatial/hrtf_convolver.cpp.
# Ese .cpp solo se compilaba en el APK (CMakeLists raíz), nunca en los tests
# host -> "undefined reference" a init/set_position/process/setSharedDataset.
# Se enlaza la fuente REAL (no un fake: los tests miden el DSP real) + shim de
# android/log.h (header-only, sin .so).
add_executable(test_intelligent_upmixer
  test_intelligent_upmixer.cpp
  ../spatial/IntelligentUpmixer.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafPcaDecoder.cpp
  ../HRTFBinLoader.cpp
)
target_link_libraries(test_intelligent_upmixer PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_intelligent_upmixer PRIVATE .. ../include host_stubs)
target_compile_options(test_intelligent_upmixer PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_intelligent_upmixer PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_intelligent_upmixer PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_intelligent_upmixer)

# ORBE (Binaural Esférico de Campo Cercano): el modelo de cabeza es
# header-only; el núcleo STFT se compila directo (como
# test_intelligent_upmixer) porque IvannaFusionCore no se enlaza en host.
ivanna_add_test(test_near_field_head_model)
# Propagación física de la onda + Ley de Curvatura de ORBE (Trabajador B,
# 2026-11-18): piso (1/r, r/c, ISO 9613-1, Doppler, potencia constante) y
# ley nueva (ITD por curvatura κ=1/r: Woodworth exacto en κ→0, +11.7 % a
# 0.15 m/90°, inversión distancia-desde-exceso-ITD). Verificado contra
# tablas ISO publicadas y contra NearFieldHeadModel (ver
# fused/PHYSICAL_PROPAGATION_MAP.md).
ivanna_add_test(test_physical_propagation)
# Modelo físico de cabeza — esfera rígida exacta (Trabajador A, 2026-11-18):
# serie de Bessel/Hankel + Legendre, verificado contra implementación
# independiente en scipy y contra límites analíticos. Piso físico de ORBE·Ω;
# la comparación contra el SOFA MIT KEMAR medido vive en
# analysis-ivanna/PHYSICAL_HEAD_MODEL_2026-11-18.md.
add_executable(test_physical_head_model
  test_physical_head_model.cpp
  ../fused/physical_head_model.cpp
)
target_link_libraries(test_physical_head_model PRIVATE GTest::gtest_main)
target_include_directories(test_physical_head_model PRIVATE .. ../include)
target_compile_options(test_physical_head_model PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_physical_head_model PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_physical_head_model PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_physical_head_model)
# Física nueva — Operador de Interferencia Geodésica del pabellón
# (Trabajador A, 2026-11-18): Ley Geodésica del Notch, dos familias
# verificadas leave-one-out contra el SOFA MIT KEMAR (ver
# geodesic_pinna_model.hpp y analysis-ivanna/PHYSICAL_HEAD_MODEL_2026-11-18.md).
add_executable(test_geodesic_pinna_model
  test_geodesic_pinna_model.cpp
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_head_model.cpp
)
target_link_libraries(test_geodesic_pinna_model PRIVATE GTest::gtest_main)
target_include_directories(test_geodesic_pinna_model PRIVATE .. ../include)
target_compile_options(test_geodesic_pinna_model PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_geodesic_pinna_model PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_geodesic_pinna_model PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_geodesic_pinna_model)
# Kernel unificado — Operador Holográfico Único TΩ(ω) 2×2 en 64-bit double
# (IVANNA-APEX v2.6, 2026-11-18): matriz compleja única que reemplaza 3 STFT
# + 30 IIRs + shuffler XTC. STANDALONE (no conectado a FusedChain aún): la
# integración es el paso siguiente; estos 4 tests son su puerta de entrada.
add_executable(test_unified_field_kernel
  test_unified_field_kernel.cpp
  ../fused/physical_head_model.cpp
)
target_link_libraries(test_unified_field_kernel PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_unified_field_kernel PRIVATE .. ../include)
target_compile_options(test_unified_field_kernel PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_unified_field_kernel PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_unified_field_kernel PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_unified_field_kernel)
# NEU·Ω Fase 1 (SpectralCore, header-only): núcleo mínimo con FFT
# compartida L+jR, M=I, OLA único. Criterio de salida: E1 (identidad).
ivanna_add_test(test_spectral_core)
# NEU·Ω Fase 3 (0028): cola difusa en frecuencia — la cola RIR vive en la
# matriz espectral (un solo IFFT), con EDC/T60, coste y sin clic; el
# RirConvolver salta su cola FDL cuando NEU la lleva (tailEnabled).
# (add_executable explícito, no ivanna_add_test: RirConvolver.cpp no está
# en ivanna_dsp_under_test y hay que compilarlo en este target, como hace
# test_processor_evidence.)
add_executable(test_neu_fase3_tail
  test_neu_fase3_tail.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_neu_fase3_tail PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_neu_fase3_tail PRIVATE .. ../include ../visualizer)
target_compile_options(test_neu_fase3_tail PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_neu_fase3_tail PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_neu_fase3_tail PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_neu_fase3_tail)
add_executable(test_orbe_binaural
  test_orbe_binaural.cpp
  ../spatial/OrbeBinaural.cpp
)
target_link_libraries(test_orbe_binaural PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_orbe_binaural PRIVATE .. ../include host_stubs)
target_compile_options(test_orbe_binaural PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_orbe_binaural PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_orbe_binaural PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_orbe_binaural)

# ── test_orbe_power_conservation: fix L1/L2 del defecto M1 (Fase 1, 2026-11-24) ──
# BandPowerConserver + kSideGain=1.0 en OrbeBinaural, detrás del flag
# kOrbePowerConservation (ON por defecto, orden Esse 2026-11-25). El
# -D=1 de este target es redundante con el default y se conserva como
# cinturón: si alguien baja el default, este target sigue ejerciendo ON.
# Mismas fuentes que test_orbe_binaural (OrbeBinaural.cpp directo).
add_executable(test_orbe_power_conservation
  test_orbe_power_conservation.cpp
  ../spatial/OrbeBinaural.cpp
)
target_link_libraries(test_orbe_power_conservation PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_orbe_power_conservation PRIVATE .. ../include host_stubs)
target_compile_definitions(test_orbe_power_conservation PRIVATE IVANNA_ORBE_POWER_CONSERVATION=1)
target_compile_options(test_orbe_power_conservation PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_orbe_power_conservation PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_orbe_power_conservation PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_orbe_power_conservation)
add_executable(test_neu_compiler
  test_neu_compiler.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_neu_compiler PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_neu_compiler PRIVATE .. ../include host_stubs)
target_compile_options(test_neu_compiler PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_neu_compiler PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_neu_compiler PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_neu_compiler)
add_executable(test_orbe_chain
  test_orbe_chain.cpp
  ../spatial/OrbeBinaural.cpp
)
target_link_libraries(test_orbe_chain PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_orbe_chain PRIVATE .. ../include host_stubs)
target_compile_options(test_orbe_chain PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_orbe_chain PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_orbe_chain PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_orbe_chain)

# HOLOS — Operador Único de Campo Acústico (FUSION.md, v2.5.364).
# Mismo patrón que los tests ORBE. Las fuentes DSP (Compressor,
# ParametricEQ, SafetyLimiter, HarmonicExciter, volterra_h2_symmetric)
# ya están en ivanna_dsp_under_test: aquí solo se listan las fuentes
# HOLOS/RIR/ORBE que no están en la librería.
add_executable(test_holos_green_kernel
  test_holos_green_kernel.cpp
)
target_link_libraries(test_holos_green_kernel PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_green_kernel PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_green_kernel PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_green_kernel PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_green_kernel PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_green_kernel)

add_executable(test_holos_gain_field
  test_holos_gain_field.cpp
)
target_link_libraries(test_holos_gain_field PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_gain_field PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_gain_field PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_gain_field PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_gain_field PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_gain_field)

# Ley 1 — Dominio Total del Tiempo y la Fase (Γ complejo, alineación
# entera+fraccional, filtro inverso de fase mínima).
add_executable(test_ley1_tiempo_fase
  test_ley1_tiempo_fase.cpp
)
target_link_libraries(test_ley1_tiempo_fase PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_ley1_tiempo_fase PRIVATE .. ../include host_stubs)
target_compile_options(test_ley1_tiempo_fase PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_ley1_tiempo_fase PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_ley1_tiempo_fase PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_ley1_tiempo_fase)

add_executable(test_holos_volterra_kernel
  test_holos_volterra_kernel.cpp
  ../spatial/HolosVolterraKernel.cpp
)
target_link_libraries(test_holos_volterra_kernel PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_volterra_kernel PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_volterra_kernel PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_volterra_kernel PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_volterra_kernel PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_volterra_kernel)

add_executable(test_holos_operator
  test_holos_operator.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_holos_operator PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_operator PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_operator PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_operator PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_operator PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_operator)

# ── test_power_mix: mezcla seco/wet de potencia constante (P3/3.1) ──
# PowerMixer en ORBE y HOLOS: unidad (coherente/incorrelado/antifase/
# extremos exactos), fundido 15 ms del operador con tonos (ventanas 5 ms),
# régimen permanente con ruido (ventanas 1 s) y potencia por banda ERB
# (Goertzel a la fundamental, 100 Hz / 1 kHz).
add_executable(test_power_mix
  test_power_mix.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_power_mix PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_power_mix PRIVATE .. ../include host_stubs)
target_compile_options(test_power_mix PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_power_mix PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_power_mix PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_power_mix)

# ── test_holos_multidim: HOLOS multidimensional (P3/3.3) ──
# Equivalente a test_orbe_power_conservation pero para HOLOS: energía
# total, balance L/R (ILD) y coherencia por banda ERB (24 bandas,
# 60 Hz–16 kHz), con tono coherente, ruido dual-mono e independiente,
# estéreo asimétrico y transitorios.
add_executable(test_holos_multidim
  test_holos_multidim.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_holos_multidim PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_multidim PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_multidim PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_multidim PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_multidim PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_multidim)

# Perfil de coste de HOLOS por componente (F2 / v2.5.453): imprime
# media/p99 por bloque de 320 f con distintos parámetros. No es un test
# de pass/fail de rendimiento; orienta la optimización sin cambiar el sonido.
add_executable(test_holos_perf_profile
  test_holos_perf_profile.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_holos_perf_profile PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_perf_profile PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_perf_profile PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_perf_profile PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_perf_profile PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_perf_profile)

# Ω — Operador Universal (v2.5.444). Núcleo header-only (matemática pura,
# sin estado global): el test solo necesita el .cpp. Verificación §11.1
# del documento privado (U1–U9 + tope N1).
ivanna_add_test(test_omega_operator)

# ── Coste por etapa de la cadena DSP de Ruta A (v2.5.410, SOLO MEDICION) ──
# test_route_a_chain_cost_410.cpp: timea cada etapa con los defaults en vivo
# en bloques de 320 frames @48 kHz (200 warmup + 2000 medidos) + cadena total
# en serie + simulacion del DspLoadShedGovernor + THD+N a 1 kHz. Fuentes
# extra copiadas de los tests de cada etapa: OrbeBinaural.cpp (test_orbe),
# HolosOperator/RirConvolver (test_holos_operator), RouteAWfsEngine +
# WfsRenderer + wfs_controls_bridge (test_route_a_wfs_engine), phase_oracle
# (test_hyper_state_operator), evolutionary_kernel_v2 + audio_control_plane
# (simbolos evo_* y g_control_frame de PDEngine, cf. test_phase2_jni_natives).
add_executable(test_route_a_chain_cost_410
  test_route_a_chain_cost_410.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/RouteAWfsEngine.cpp
  ../spatial/WfsRenderer.cpp
  ../wfs_controls_bridge.cpp
  ../phase_oracle.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafPcaDecoder.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafModelLoader.cpp
  ../HRTFBinLoader.cpp
  ../evolutionary_kernel_v2.cpp
  ../audio_control_plane.cpp
)
target_link_libraries(test_route_a_chain_cost_410 PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
target_include_directories(test_route_a_chain_cost_410 PRIVATE .. ../include ../neuromorphic host_stubs)
target_compile_options(test_route_a_chain_cost_410 PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_route_a_chain_cost_410 PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_route_a_chain_cost_410 PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_route_a_chain_cost_410)

# ── Firma de rebalanceo de Ruta A (v2.5.410, SOLO MEDICION) ──
# test_route_a_signature_410.cpp: HOLOS 12 vs 24 bandas (coste + THD+N),
# OmniField THD+N con estereo real (tonos decorrelados + pseudo-musica) a
# amount 1.0/0.5/0.0, ORBE coste vs amount, confirmacion del reinjector y
# cadena completa con pseudo-musica. Mismas fuentes extra que el test de
# costes (OrbeBinaural, HolosOperator/RirConvolver, RouteAWfsEngine +
# WfsRenderer + wfs_controls_bridge, phase_oracle, hrtf, evolutionary_kernel_v2
# + audio_control_plane para los simbolos de PDEngine).
add_executable(test_route_a_signature_410
  test_route_a_signature_410.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/RouteAWfsEngine.cpp
  ../spatial/WfsRenderer.cpp
  ../wfs_controls_bridge.cpp
  ../phase_oracle.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafPcaDecoder.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafModelLoader.cpp
  ../HRTFBinLoader.cpp
  ../evolutionary_kernel_v2.cpp
  ../audio_control_plane.cpp
)
target_link_libraries(test_route_a_signature_410 PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
target_include_directories(test_route_a_signature_410 PRIVATE .. ../include ../neuromorphic host_stubs)
target_compile_options(test_route_a_signature_410 PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_route_a_signature_410 PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_route_a_signature_410 PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_route_a_signature_410)

add_executable(test_holos_chain
  test_holos_chain.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/OrbeBinaural.cpp
)
target_link_libraries(test_holos_chain PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_chain PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_chain PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_chain PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_chain PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_chain)

# ── test_fase5_quality_gate: FASE 5 — Banco de Laboratorio Automático en CI
# (Audio Quality Gate). Null-test maestro (ORBE 0 + HOLOS 0 en serie,
# < -140 dB bit-exacto) + THD+N línea base (ORBE < -139 dB, HOLOS retune
# producción < -94 dB @1 kHz, -1 dBFS). Compila los .cpp REALES de la
# cadena (HolosOperator + OrbeBinaural) como test_holos_chain. La
# integridad RT NO se duplica aquí (redefinir operator new chocaría con
# test_rt_no_alloc, ODR): la cubre test_rt_no_alloc, que la puerta CI
# de Fase 5 corre junto con este test.
add_executable(test_fase5_quality_gate
  test_fase5_quality_gate.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/OrbeBinaural.cpp
)
target_link_libraries(test_fase5_quality_gate PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_fase5_quality_gate PRIVATE .. ../include host_stubs)
target_compile_options(test_fase5_quality_gate PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fase5_quality_gate PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fase5_quality_gate PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fase5_quality_gate)

# Barrido de retuneo HOLOS v2.5.369: THD+N en rejilla (amount × harmonics)
# para elegir el default que cumpla THD+N ≤ −70 dB con efecto medible.
add_executable(test_holos_retune_sweep
  test_holos_retune_sweep.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/OrbeBinaural.cpp
)
target_link_libraries(test_holos_retune_sweep PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_holos_retune_sweep PRIVATE .. ../include host_stubs)
target_compile_options(test_holos_retune_sweep PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_holos_retune_sweep PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_holos_retune_sweep PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_holos_retune_sweep)

# Native 3D stem pose contract: the production NeuralUpmixer implementation is
# compiled directly so these tests exercise the same atomic buffer publication
# and validation used by JNI, rather than a parallel test-only implementation.
add_executable(test_native_stem_pose
  test_native_stem_pose.cpp
  ../neuromorphic/ivanna_neural_upmixer.cpp
)
target_link_libraries(test_native_stem_pose PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_native_stem_pose PRIVATE
  .. ../include ../spatial ../neuromorphic host_stubs
)
target_compile_options(test_native_stem_pose PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_native_stem_pose PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_native_stem_pose PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_native_stem_pose)

# Sensor timestamps use Android elapsed-realtime; the audio renderer must not
# compare them with a block-duration clock. Exercise the production tracker.
add_executable(test_head_tracker_latest_pose
  test_head_tracker_latest_pose.cpp
  ../spatial/ivanna_head_tracker.cpp
)
target_link_libraries(test_head_tracker_latest_pose PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_head_tracker_latest_pose PRIVATE
  .. ../include ../spatial host_stubs
)
target_compile_options(test_head_tracker_latest_pose PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_head_tracker_latest_pose PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_head_tracker_latest_pose PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_head_tracker_latest_pose)

# SO(3) completo + parallax por traslación (Fase 2, ítem E, v2.5.472):
# cuaternión yaw/pitch/roll, fusión gyro+RV, hook de traslación ARCore y
# NearFieldHeadModel con oyente trasladado.
add_executable(test_head_tracker_so3
  test_head_tracker_so3.cpp
  ../spatial/ivanna_head_tracker.cpp
)
target_link_libraries(test_head_tracker_so3 PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_head_tracker_so3 PRIVATE
  .. ../include ../spatial host_stubs
)
target_compile_options(test_head_tracker_so3 PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_head_tracker_so3 PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_head_tracker_so3 PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_head_tracker_so3)

# Contrato SHM IvannaFusionState v2 + etapa HAL F5 (Fase 2, ítem E,
# v2.5.472): round-trip v2, seqlock, gate engage/passthrough, kill
# switch, safe-mode y conteo de xruns. Header-only (sin .cpp del daemon).
add_executable(test_ivanna_fusion_shm
  test_ivanna_fusion_shm.cpp
)
target_link_libraries(test_ivanna_fusion_shm PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_ivanna_fusion_shm PRIVATE
  .. ../include host_stubs
)
target_compile_options(test_ivanna_fusion_shm PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_ivanna_fusion_shm PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_ivanna_fusion_shm PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_ivanna_fusion_shm)

add_executable(test_hoa_binaural_decoder
  test_hoa_binaural_decoder.cpp
  ../spatial/HoaBinauralDecoder.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafPcaDecoder.cpp
  ../HRTFBinLoader.cpp
)
target_link_libraries(test_hoa_binaural_decoder PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_hoa_binaural_decoder PRIVATE .. ../include host_stubs)
target_compile_options(test_hoa_binaural_decoder PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hoa_binaural_decoder PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hoa_binaural_decoder PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_hoa_binaural_decoder)


# ── Wave Field Synthesis (2026-09-18) ──
add_executable(test_route_a_wfs_engine
  test_route_a_wfs_engine.cpp
  ../spatial/RouteAWfsEngine.cpp
  ../spatial/WfsRenderer.cpp
  ../wfs_controls_bridge.cpp)
target_include_directories(test_route_a_wfs_engine PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_route_a_wfs_engine PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_route_a_wfs_engine PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_route_a_wfs_engine PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_route_a_wfs_engine COMMAND test_route_a_wfs_engine)

add_executable(test_wfs_renderer
  test_wfs_renderer.cpp
  ../spatial/WfsRenderer.cpp
)
target_include_directories(test_wfs_renderer PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_wfs_renderer PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_wfs_renderer PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_wfs_renderer PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_wfs_renderer COMMAND test_wfs_renderer)
set_tests_properties(test_wfs_renderer PROPERTIES FAIL_REGULAR_EXPRESSION "FALLARON")


# ── WFS activación glitch-free (2026-09-19) ──
add_executable(test_wfs_activation_crossfade
  test_wfs_activation_crossfade.cpp
  ../spatial/WfsRenderer.cpp
)
target_include_directories(test_wfs_activation_crossfade PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_wfs_activation_crossfade PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_wfs_activation_crossfade PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_wfs_activation_crossfade PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_wfs_activation_crossfade COMMAND test_wfs_activation_crossfade)
set_tests_properties(test_wfs_activation_crossfade PROPERTIES FAIL_REGULAR_EXPRESSION "FALLARON")

# ── Music Intelligence Engine bridge (2026-09-24) ──
add_executable(test_ime_bridge
  test_ime_bridge.cpp
  ../music_intelligence/MusicFeatureExtractor.cpp
  ../music_intelligence/MusicIntelligenceEngine.cpp
  ../music_intelligence/ImeBridge.cpp
  ../jni/ivanna_ime_jni.cpp
)
target_include_directories(test_ime_bridge PRIVATE .. ../include ../music_intelligence host_stubs)
target_compile_options(test_ime_bridge PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_ime_bridge PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_ime_bridge PRIVATE ${IVANNA_SAN_FLAGS})
endif()
set_target_properties(test_ime_bridge PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_ime_bridge COMMAND test_ime_bridge)
set_tests_properties(test_ime_bridge PROPERTIES FAIL_REGULAR_EXPRESSION "FALLARON")

# ── Master Acoustic Orchestrator: Arbitraje Unificado y Estabilidad de Referencia ──
add_executable(test_master_acoustic_orchestrator
  test_master_acoustic_orchestrator.cpp
  ../experimental/adaptive_engine/adaptive_decision_engine.cpp
)
target_link_libraries(test_master_acoustic_orchestrator PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_master_acoustic_orchestrator PRIVATE .. ../include ../experimental/adaptive_engine host_stubs)
target_compile_options(test_master_acoustic_orchestrator PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_master_acoustic_orchestrator PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_master_acoustic_orchestrator PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_master_acoustic_orchestrator COMMAND test_master_acoustic_orchestrator)

# EJE 1: descomposición estéreo→objetos WFS (decomposer + renderer reales).
add_executable(test_wfs_object_decomposition
  test_wfs_object_decomposition.cpp
  ../spatial/WfsRenderer.cpp)
target_include_directories(test_wfs_object_decomposition PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_wfs_object_decomposition PRIVATE -Wall -Wextra)
add_test(NAME test_wfs_object_decomposition COMMAND test_wfs_object_decomposition)
set_tests_properties(test_wfs_object_decomposition PROPERTIES FAIL_REGULAR_EXPRESSION "FALLARON")

# EJE 7: Final Performance, RT-Safety & Latency Certification
add_executable(test_perf_auditor
  test_perf_auditor.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
target_link_libraries(test_perf_auditor PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_perf_auditor PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_perf_auditor PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_perf_auditor PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_perf_auditor PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_perf_auditor COMMAND test_perf_auditor)

# C11/510 — CUARENTENA (wall-clock, verificado): WithinBudgetCompliance mide
# 1 s de CPU de pared (cpu_pct_p99 ≤ 12 %) bajo ctest -j4; tests-host.yml
# documenta que «pasa en solitario» y falla por carga del runner, no por
# DSP. No bloquea la puerta por push/PR; corre en el carril semanal
# (tests-quarantined-weekly.yml) y en el self-hosted, donde la carga es
# conocida. Salida: 4 noches verdes seguidas (N=4, propuesta a confirmar,
# no dato medido) → se quita esta etiqueta. Los otros 3 casos del binario
# (latencia por muestras, estabilidad numérica) son deterministas y viajan
# con el binario por granularidad CTest (add_test registra el binario
# entero); si el consejo quiere afinarlo, se parte con --gtest_filter.
set_tests_properties(test_perf_auditor PROPERTIES LABELS QUARANTINED)

# EJE 2/4 — auditoria 2026-09-24: canal_resonance_boost_db e itdScale_
# (HrtfPersonalizer) estaban calculados pero sin ningun consumidor real.
add_executable(test_hrtf_personalizer_wiring
  test_hrtf_personalizer_wiring.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
target_link_libraries(test_hrtf_personalizer_wiring PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_hrtf_personalizer_wiring PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_hrtf_personalizer_wiring PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hrtf_personalizer_wiring PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hrtf_personalizer_wiring PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_hrtf_personalizer_wiring COMMAND test_hrtf_personalizer_wiring)

# Cadena HRTF de la rejilla: HRTFDatabase (carga desde .ihr1 medidos) ->
# HRTFInterpolator -> BinauralRenderer/HybridRenderer (fallback observable).
# Causa raiz que fija: g_hrtfDatabase era `= {}` (ceros) y nada la llenaba.
# Usa los .ihr1 reales (KEMAR/CIPIC) del APK via IVANNA_HRTF_ASSET_DIR.
get_filename_component(IVANNA_HRTF_ASSET_DIR
  "${CMAKE_SOURCE_DIR}/../../assets/ivanna_omega/hrtf" ABSOLUTE)
add_executable(test_hrtf_database
  test_hrtf_database.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
# Las fuentes de producción del HRTF se compilan aquí con -fno-exceptions, igual que el build
# Android (CMakeLists.txt principal). Un try/catch en ellas rompía el APK mientras el host, con
# excepciones habilitadas, seguía en verde: ahora rompe también aquí.
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/HRTFInterpolator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/HRTFDatabase.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_link_libraries(test_hrtf_database PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_hrtf_database PRIVATE .. ../include ../spatial host_stubs)
target_compile_definitions(test_hrtf_database PRIVATE
  IVANNA_HRTF_ASSET_DIR="${IVANNA_HRTF_ASSET_DIR}"
)
target_compile_options(test_hrtf_database PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hrtf_database PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hrtf_database PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_hrtf_database COMMAND test_hrtf_database)

# Interpolación de HRIR alineada en el tiempo (HrirTimeAlign.hpp): mide peine de ITD y ITD interpolada.
add_executable(test_hrtf_time_align
  test_hrtf_time_align.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/HRTFInterpolator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/HRTFDatabase.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_link_libraries(test_hrtf_time_align PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_hrtf_time_align PRIVATE .. ../include ../spatial host_stubs)
target_compile_definitions(test_hrtf_time_align PRIVATE IVANNA_HRTF_ASSET_DIR="${IVANNA_HRTF_ASSET_DIR}")
target_compile_options(test_hrtf_time_align PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hrtf_time_align PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hrtf_time_align PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_hrtf_time_align COMMAND test_hrtf_time_align)

# Selección de HRIR por pose (az+el, con envoltura) y ley de distancia de la escena (Ruta B).
add_executable(test_hrir_pose_select test_hrir_pose_select.cpp)
target_link_libraries(test_hrir_pose_select PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_hrir_pose_select PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_hrir_pose_select PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hrir_pose_select PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hrir_pose_select PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_hrir_pose_select COMMAND test_hrir_pose_select)

# BinauralRenderer: el cambio de filtro HRTF (head-tracking / fuente móvil) se funde, no parte la onda.
add_executable(test_binaural_crossfade
  test_binaural_crossfade.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
set_source_files_properties(
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/HRTFInterpolator.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../spatial/HRTFDatabase.cpp
  PROPERTIES COMPILE_OPTIONS "-fno-exceptions")
target_link_libraries(test_binaural_crossfade PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_binaural_crossfade PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_binaural_crossfade PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_binaural_crossfade PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_binaural_crossfade PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_binaural_crossfade COMMAND test_binaural_crossfade)

# ── Eje Supremo Neuroacústico: Inversión Biomecánica Coclear Activa ───────────
# CochlearActiveInverseModel.hpp es header-only (cero .cpp propios):
# no requiere fuentes en ivanna_dsp_under_test ni librería auxiliar.
# Verifica: (1) 0 ms de latencia algorítmica / respuesta de impulso,
#           (2) estabilidad numérica con entrada subnormal (1e-25f),
#           (3) energía acotada con señal multitono de bandas Greenwood.
add_executable(test_cochlear_inverse_model
  test_cochlear_inverse_model.cpp
)
target_link_libraries(test_cochlear_inverse_model PRIVATE GTest::gtest_main)
target_include_directories(test_cochlear_inverse_model PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  host_stubs
)
target_compile_options(test_cochlear_inverse_model PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_cochlear_inverse_model PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_cochlear_inverse_model PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_cochlear_inverse_model)

# ── Interfaz Neuro-Acústica en Bucle Cerrado v1 ─────────────────────────────
# neuro_acoustic_closed_loop.hpp es header-only (cero .cpp propios):
# firma auditiva por usuario (8 bandas Greenwood) + LIF real del NPE +
# ajustes acotados HOLOS/ORBE + fail-closed. Sensor real = interfaz
# NeuroFrame; el test usa el sensor simulado (determinista) y frames
# sintéticos EEG/ABR/fNIRS. Sin hardware medido en v1.
add_executable(test_neuro_acoustic_closed_loop
  test_neuro_acoustic_closed_loop.cpp
)
target_link_libraries(test_neuro_acoustic_closed_loop PRIVATE GTest::gtest_main)
target_include_directories(test_neuro_acoustic_closed_loop PRIVATE
  ..
  ../include
  ../neuromorphic
  host_stubs
)
target_compile_options(test_neuro_acoustic_closed_loop PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_neuro_acoustic_closed_loop PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_neuro_acoustic_closed_loop PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_neuro_acoustic_closed_loop)

# ── M0 (salto, Luna): contrato temporal único ─────────────────────────────
# AudioStatePublisher (seqlock): round-trip, generación monótona,
# coherencia bajo concurrencia, bypass con valid=false y detección de
# cambio de ruta/calibration. La librería ivanna_dsp_under_test ya
# incluye fused/closed_loop/audio_state.cpp (arriba).
ivanna_add_test(test_audio_state)

# Ruta AUX Genezi / auto (v2.5.366): presets, puerta Volterra IVK1 y
# coste O(K^2) que hace inviable K=4096 en tiempo real.
add_executable(test_aux_genezi_route
  test_aux_genezi_route.cpp
)
target_link_libraries(test_aux_genezi_route PRIVATE GTest::gtest_main)
target_include_directories(test_aux_genezi_route PRIVATE
  ..
  ../include
  ../neuromorphic
  host_stubs
)
target_compile_options(test_aux_genezi_route PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_aux_genezi_route PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_aux_genezi_route PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_aux_genezi_route)

# Fusión Homogénea Φ (v2.5.367): un solo estado (neuro + ruta AUX +
# puerta Volterra), Z_emf siempre identidad, fail-closed.
add_executable(test_homogeneous_fusion
  test_homogeneous_fusion.cpp
)
target_link_libraries(test_homogeneous_fusion PRIVATE GTest::gtest_main)
target_include_directories(test_homogeneous_fusion PRIVATE
  ..
  ../include
  ../neuromorphic
  host_stubs
)
target_compile_options(test_homogeneous_fusion PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_homogeneous_fusion PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_homogeneous_fusion PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_homogeneous_fusion)

# Pilares pendientes (v2.5.368): Volterra diagonal K=4096, DoP v1.1,
# EQ de elevación por pinna (Blauert), estimador EMF Thiele-Small.
add_executable(test_pilares_pendientes
  test_pilares_pendientes.cpp
)
target_link_libraries(test_pilares_pendientes PRIVATE GTest::gtest_main)
target_include_directories(test_pilares_pendientes PRIVATE
  ..
  ../include
  ../neuromorphic
  host_stubs
)
target_compile_options(test_pilares_pendientes PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_pilares_pendientes PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_pilares_pendientes PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_pilares_pendientes)

# Hiper-Operador de Estado Φ v2.5.369: muestra a muestra, sin FFT.
# BiquadEnvelopeBank llama phase_oracle_tick (igual que test_npe_core):
# phase_oracle.cpp entra como fuente explícita. El operador es header-only.
add_executable(test_hyper_state_operator
  test_hyper_state_operator.cpp
  ../phase_oracle.cpp
)
target_link_libraries(test_hyper_state_operator PRIVATE GTest::gtest_main)
target_include_directories(test_hyper_state_operator PRIVATE
  ..
  ../include
  ../neuromorphic
  host_stubs
)
target_compile_options(test_hyper_state_operator PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hyper_state_operator PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hyper_state_operator PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_hyper_state_operator)

# Curvas objetivo ÉLITE v2.5.369 (Harman 2018, élite-lean, campo difuso,
# altavoces en sala): header-only, sin dependencias de DSP.
add_executable(test_elite_target_curves test_elite_target_curves.cpp)
target_link_libraries(test_elite_target_curves PRIVATE GTest::gtest_main)
target_include_directories(test_elite_target_curves PRIVATE
  ..
  ../include
  host_stubs
)
target_compile_options(test_elite_target_curves PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_elite_target_curves PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_elite_target_curves PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_elite_target_curves)

# ── 5 Ejes de Supremacía Neuroacústica (Warped Lattice, CVNN+DDSP, SNN+NMF HOA, Pinna INR, SHM+Farrow5) ──
add_executable(test_supreme_five_axes
  test_supreme_five_axes.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
target_link_libraries(test_supreme_five_axes PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_supreme_five_axes PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  host_stubs
)
target_compile_options(test_supreme_five_axes PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_supreme_five_axes PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_supreme_five_axes PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_supreme_five_axes)

# ── ACOUSTIC REALITY RECONSTRUCTION HYPERENGINE (Fases 1 a 8 + 6 Pruebas de Validación) ──
add_executable(test_acoustic_reality_hyperengine
  regression/test_acoustic_reality_hyperengine.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
)
target_link_libraries(test_acoustic_reality_hyperengine PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_acoustic_reality_hyperengine PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  ../experimental/adaptive_engine
  host_stubs
)
target_compile_options(test_acoustic_reality_hyperengine PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_acoustic_reality_hyperengine PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_acoustic_reality_hyperengine PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_acoustic_reality_hyperengine)

# ── SUPREME ZERO-POP STATE TRANSITION LAYER (Certificación Fases 5 y 6) ──
add_executable(test_supreme_zero_pop_transition
  test_supreme_zero_pop_transition.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
  ../neuromorphic/volterra_h2_symmetric.cpp
)
target_link_libraries(test_supreme_zero_pop_transition PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_supreme_zero_pop_transition PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  ../experimental/adaptive_engine
  host_stubs
)
target_compile_options(test_supreme_zero_pop_transition PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_supreme_zero_pop_transition PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_supreme_zero_pop_transition PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_supreme_zero_pop_transition)

# ── SUPREME ACOUSTIC STATE CONTINUITY ARCHITECTURE ──
add_executable(test_supreme_acoustic_continuity
  test_supreme_acoustic_continuity.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
  ../spatial/WfsRenderer.cpp
  ../neuromorphic/volterra_h2_symmetric.cpp
)
target_link_libraries(test_supreme_acoustic_continuity PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_supreme_acoustic_continuity PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  ../experimental/adaptive_engine
  host_stubs
)
target_compile_options(test_supreme_acoustic_continuity PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_supreme_acoustic_continuity PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_supreme_acoustic_continuity PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_supreme_acoustic_continuity)

# ── PROMPT MAESTRO v3: UNIFIED STAGE ARCHITECTURE & SAFETY BENCH (FASES 1–6) ──
add_library(ivanna_unified_pipeline_under_test STATIC
  ../audio_control_plane.cpp
  ../Psychoacoustics.cpp
  ../SofaHRTFLoader.cpp
  ../SafHRTFBridge.cpp
  ../SafPcaDecoder.cpp
  ../SafModelLoader.cpp
  ../SafGlobalBridge.cpp
  ../saf_latent_bridge.cpp
  ../SafHRTFDatasetBridge.cpp
  ../HRTFBinLoader.cpp
  ../IvannaVoiceProsodyEngine.cpp
  ../EvolutionaryEQ.cpp
  ../neuromorphic/ivanna_neural_upmixer.cpp
  ../anti_dolby.cpp
  ../neuromorphic/AntiDolbyAI.cpp
  ../neuromorphic/neuro_cochlear_manifold.cpp
  ../neuromorphic/volterra_h2_symmetric.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/hrtf_convolver.cpp
  ../IvannaTinyML.cpp
  ../IvannaNeuromorphicTinyML.cpp
  ../SaFOptimizer.cpp
  ../SaFStimulusRenderer.cpp
  ../SafSpatialModifier.cpp
  ../IvannaSelfHealingEngine.cpp
  ../IvannaSuperAgentMemory.cpp
)
target_include_directories(ivanna_unified_pipeline_under_test PUBLIC
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  ../experimental/adaptive_engine
  host_stubs
)
target_compile_options(ivanna_unified_pipeline_under_test PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(ivanna_unified_pipeline_under_test PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(ivanna_unified_pipeline_under_test PRIVATE ${IVANNA_SAN_FLAGS})
endif()

add_executable(test_unified_master_v3_safety_bench
  test_unified_master_v3_safety_bench.cpp
)
target_link_libraries(test_unified_master_v3_safety_bench PRIVATE ivanna_unified_pipeline_under_test GTest::gtest_main Threads::Threads m dl)
target_compile_options(test_unified_master_v3_safety_bench PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_unified_master_v3_safety_bench PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_unified_master_v3_safety_bench PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME UnifiedMasterV3SafetyBench COMMAND test_unified_master_v3_safety_bench)

# SofaHRTFLoader: valida la firma HDF5 y RECONSTRUYE el HRIR desde la base SOFA-PCA (no lee
# mediciones). Estas pruebas fijan ese contrato y los errores de archivo inexistente/truncado/
# sin firma.
add_executable(test_sofa_hrtf_loader test_sofa_hrtf_loader.cpp)
target_link_libraries(test_sofa_hrtf_loader PRIVATE ivanna_unified_pipeline_under_test GTest::gtest_main Threads::Threads m dl)
target_compile_options(test_sofa_hrtf_loader PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_sofa_hrtf_loader PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_sofa_hrtf_loader PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_sofa_hrtf_loader)

# ── FASE 2: 12 nativos conectados con external fun y consumidores Kotlin ──────
add_executable(test_phase2_jni_natives
  test_phase2_jni_natives.cpp
  ../phase_oracle.cpp
  ../shm_hyperplane.cpp
  ../spatial/spatial_engine.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafPcaDecoder.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafModelLoader.cpp
  ../HRTFBinLoader.cpp
  ../evolutionary_kernel_v2.cpp
  ../audio_control_plane.cpp
  ../daemon/core/android_log_compat.cpp
)
target_link_libraries(test_phase2_jni_natives PRIVATE GTest::gtest_main Threads::Threads m)
target_include_directories(test_phase2_jni_natives PRIVATE
  ..
  ../include
  ../spatial
  host_stubs
)
target_compile_options(test_phase2_jni_natives PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_phase2_jni_natives PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_phase2_jni_natives PRIVATE ${IVANNA_SAN_FLAGS})
endif()
set_target_properties(test_phase2_jni_natives PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_phase2_jni_natives COMMAND test_phase2_jni_natives)

# ── test_pi_lstm_bridge_jni: bridge JNI Pi-LSTM (0015) ────────────────────────
# pi_lstm_bridge_jni.cpp (278 líneas) estaba entre los 19 módulos sin tests:
# bridge JNI de PiLstmBridge con su propia instancia PILSTMMilenioEngine.
# Patrón test_phase2_jni_natives: se compila el bridge con el stub JNI de
# host (host_stubs/jni.h + android/log.h) y se llaman las funciones JNI
# directamente con un JNIEnv{} vacío. El engine es header-only
# (neuromorphic/pi_lstm_milenio.hpp), así que no hace falta más .cpp.
add_executable(test_pi_lstm_bridge_jni
  test_pi_lstm_bridge_jni.cpp
  ../pi_lstm_bridge_jni.cpp
)
target_link_libraries(test_pi_lstm_bridge_jni PRIVATE GTest::gtest_main)
target_include_directories(test_pi_lstm_bridge_jni PRIVATE
  ..
  ../include
  host_stubs
)
target_compile_options(test_pi_lstm_bridge_jni PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_pi_lstm_bridge_jni PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_pi_lstm_bridge_jni PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_pi_lstm_bridge_jni)

# ── test_benchmark_jni: latencia round-trip Ruta A (0016) ────────────────────
# ivanna_benchmark_jni.cpp (72 líneas) estaba entre los 19 módulos sin tests:
# nativeMeasureRoundTripLatencyUs mide la cadena DSP real in-process
# (ParametricEQ → Compressor → HarmonicExciter → StereoWidener → GainStage
# → SafetyLimiter) con un pulso. Los 6 módulos ya están en
# ivanna_dsp_under_test; aquí solo se compila el bridge JNI con el stub
# de host. El umbral del test es de cordura (0, 10000] µs, 100× el valor
# medido ~100 µs: un test de timing ajustado fallaría bajo ctest -j4.
add_executable(test_benchmark_jni
  test_benchmark_jni.cpp
  ../ivanna_benchmark_jni.cpp
)
target_link_libraries(test_benchmark_jni PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_benchmark_jni PRIVATE
  ..
  ../include
  host_stubs
)
target_compile_options(test_benchmark_jni PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_benchmark_jni PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_benchmark_jni PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_benchmark_jni)

# ── test_saf_jni_bridge: bridge JNI SAF (0017) ───────────────────────────────
# SaFJniBridge.cpp (130 líneas) estaba entre los 19 módulos sin tests:
# bridge JNI de SaFBridge ↔ SaFOptimizer. Se compila el bridge + el
# optimizador + SafGlobalBridge (getGlobalSaF) con el stub JNI de host.
# ivanna_saf_apply_latent (definida en el efecto AudioFlinger en producción)
# se sustituye por un stub en el test que captura el q propagado al
# renderer — es el cable que hace que el audio cambie tras el feedback.
add_executable(test_saf_jni_bridge
  test_saf_jni_bridge.cpp
  ../SaFJniBridge.cpp
  ../SaFOptimizer.cpp
  ../SafGlobalBridge.cpp
)
target_link_libraries(test_saf_jni_bridge PRIVATE GTest::gtest_main)
target_include_directories(test_saf_jni_bridge PRIVATE
  ..
  ../include
  host_stubs
)
target_compile_options(test_saf_jni_bridge PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_saf_jni_bridge PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_saf_jni_bridge PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_saf_jni_bridge)

# ── test_omega_daemon_bridge: puente SHM al daemon (0018) ────────────────────
# omega_daemon_bridge.cpp (156 líneas) estaba entre los 19 módulos sin tests:
# la app recibe el fd del SHM por SCM_RIGHTS en @omega_daemon_socket, valida
# el layout y lo mapea. El test levanta un mock daemon (hilo) que envía 1
# byte + el fd de un SHM válido (OmegaShmManager). Sin daemon devuelve
# nullptr; con el mock devuelve el estado y la 2ª llamada es el mismo
# puntero (caché, sin segundo handshake).
add_executable(test_omega_daemon_bridge
  test_omega_daemon_bridge.cpp
  ../omega_daemon_bridge.cpp
  ../daemon/core/shm_manager.cpp
)
target_link_libraries(test_omega_daemon_bridge PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_omega_daemon_bridge PRIVATE
  ..
  ../include
  host_stubs
)
target_compile_options(test_omega_daemon_bridge PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_omega_daemon_bridge PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_omega_daemon_bridge PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_omega_daemon_bridge
  PROPERTIES RUN_SERIAL TRUE)
# RUN_SERIAL (ver arriba): los 3 tests comparten el socket abstracto FIJO
# @omega_daemon_socket — el puente de producción lo tiene fijo, no se puede
# uniquificar — y CTest ejecuta cada caso gtest en un proceso aparte: bajo
# ctest -j4 dos procesos intentan bind() el mismo socket a la vez, uno
# pierde con EADDRINUSE, su mock daemon nunca señala listening y
# StartMockDaemon expira los 5 s (1059/1060 en CI, run 43879448473:
# #643 MockDaemonHandshakeMapsValidShm y #644 SecondCallReturnsCachedPointer
# arrancaron con 2 ms de diferencia; #643 perdió el bind). Serializados,
# cada proceso bindea el socket solo. NoDaemonReturnsNull también lo
# necesita: si corre a la vez que un mock daemon, el connect tiene éxito y
# deja de devolver nullptr. El fix anterior (espera 1→5 s + join RAII)
# solo evitó el SIGABRT, no la colisión del bind.

# ── FASE 3: 41 external fun conectadas con llamadores reales en Kotlin ─────────
add_executable(test_phase3_jni_callers
  test_phase3_jni_callers.cpp
  ../ivanna_hires_jni.cpp
  ../system_audio_capture.cpp
  ../benchmark_logger.cpp
  ../audio_control_plane.cpp
  ../jni/saf_stimulus_jni.cpp
  ../jni/ivanna_npe_jni.cpp
  ../jni/ivanna_visualizer_jni.cpp
  ../jni/ivanna_visualizer_v2_jni.cpp
  ../jni/ivanna_visualizer_bark64_jni.cpp
  ../phase_oracle.cpp
  ../neuromorphic/ivanna_neural_upmixer.cpp
  ../neuromorphic/neuro_cochlear_manifold.cpp
  ../neuromorphic/volterra_h2_symmetric.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../SaFStimulusRenderer.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafPcaDecoder.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafModelLoader.cpp
  ../HRTFBinLoader.cpp
  ../daemon/core/android_log_compat.cpp
)
target_link_libraries(test_phase3_jni_callers PRIVATE GTest::gtest_main Threads::Threads m dl)
target_include_directories(test_phase3_jni_callers PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  host_stubs
)
target_compile_options(test_phase3_jni_callers PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_phase3_jni_callers PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_phase3_jni_callers PRIVATE ${IVANNA_SAN_FLAGS})
endif()
set_target_properties(test_phase3_jni_callers PROPERTIES ENABLE_EXPORTS ON)
add_test(NAME test_phase3_jni_callers COMMAND test_phase3_jni_callers)

# ── FASE 4 y 5: 22 headers de producción consolidados + RT-Safety ─────────────
add_executable(test_phase4_5_header_rt_wiring
  test_phase4_5_header_rt_wiring.cpp
  ../SafPcaDecoder.cpp
  ../SafModelLoader.cpp
  ../saf_runtime.cpp
  ../anti_dolby.cpp
  ../neuromorphic/volterra_h2_symmetric.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/HRTFDatabase.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/RirConvolver.cpp
  ../experimental/adaptive_engine/adaptive_decision_engine.cpp
  ../daemon/core/omega_control_bus.cpp
  ../daemon/core/android_log_compat.cpp
)
target_link_libraries(test_phase4_5_header_rt_wiring PRIVATE GTest::gtest_main Threads::Threads m dl)
target_include_directories(test_phase4_5_header_rt_wiring PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  ../hexagon
  ../experimental/adaptive_engine
  host_stubs
)
target_compile_options(test_phase4_5_header_rt_wiring PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_phase4_5_header_rt_wiring PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_phase4_5_header_rt_wiring PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_phase4_5_header_rt_wiring COMMAND test_phase4_5_header_rt_wiring)

# ── FASE A4: Detector dinámico en host de 0 malloc / 0 new en callbacks RT ───
add_executable(test_rt_no_alloc
  test_rt_no_alloc.cpp
  ../evolutionary_kernel_v2.cpp
  ../phase_oracle.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
  ../spatial/IntelligentUpmixer.cpp
  ../spatial/HoaBinauralDecoder.cpp
  ../spatial/WfsRenderer.cpp
)
target_link_libraries(test_rt_no_alloc PRIVATE ivanna_unified_pipeline_under_test GTest::gtest_main Threads::Threads m dl)
target_compile_options(test_rt_no_alloc PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_rt_no_alloc PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_rt_no_alloc PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_rt_no_alloc COMMAND test_rt_no_alloc)

# ── 1.6 D6: Registrar test_hrtf_convolver_rt_safety, test_wfs_adaptive_protection y test_music_intelligence ──
add_executable(test_hrtf_convolver_rt_safety test_hrtf_convolver_rt_safety.cpp)
target_link_libraries(test_hrtf_convolver_rt_safety PRIVATE ivanna_unified_pipeline_under_test GTest::gtest_main ivanna_dsp_under_test Threads::Threads m dl)
target_include_directories(test_hrtf_convolver_rt_safety PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_hrtf_convolver_rt_safety PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hrtf_convolver_rt_safety PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hrtf_convolver_rt_safety PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_hrtf_convolver_rt_safety)

add_executable(test_wfs_adaptive_protection
  test_wfs_adaptive_protection.cpp
  ../spatial/WfsRenderer.cpp
)
target_link_libraries(test_wfs_adaptive_protection PRIVATE ivanna_dsp_under_test Threads::Threads m)
target_include_directories(test_wfs_adaptive_protection PRIVATE .. ../include ../spatial)
target_compile_options(test_wfs_adaptive_protection PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_wfs_adaptive_protection PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_wfs_adaptive_protection PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_wfs_adaptive_protection COMMAND test_wfs_adaptive_protection)

add_executable(test_music_intelligence
  test_music_intelligence.cpp
  ../music_intelligence/MusicFeatureExtractor.cpp
  ../music_intelligence/MusicIntelligenceEngine.cpp
  ../music_intelligence/ImeBridge.cpp
)
target_link_libraries(test_music_intelligence PRIVATE ivanna_dsp_under_test Threads::Threads m)
target_include_directories(test_music_intelligence PRIVATE .. ../include ../music_intelligence)
target_compile_options(test_music_intelligence PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_music_intelligence PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_music_intelligence PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_music_intelligence COMMAND test_music_intelligence)

# ── ATLAS-ESCENA 12D & SINGULARIDAD ACÚSTICA: Pruebas T3–T10 ────────────────
ivanna_add_test(test_ime_style_blender)
ivanna_add_test(test_scene_bus)
ivanna_add_test(test_late_reverb_suppressor)
ivanna_add_test(test_woodworth_itd)
ivanna_add_test(test_headphone_crossfeed)
ivanna_add_test(test_er_tap_time)
ivanna_add_test(test_cheb_shaper)

add_executable(test_cold_start_sim
  test_cold_start_sim.cpp
  ../music_intelligence/MusicFeatureExtractor.cpp
  ../music_intelligence/MusicIntelligenceEngine.cpp
  ../music_intelligence/ImeBridge.cpp
  ../IvannaAudioClassifier.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/HRTFInterpolator.cpp
  ../spatial/HRTFDatabase.cpp
  ../neuromorphic/volterra_h2_symmetric.cpp
)
target_link_libraries(test_cold_start_sim PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
target_include_directories(test_cold_start_sim PRIVATE
  ..
  ../include
  ../spatial
  ../neuromorphic
  ../supreme
  ../music_intelligence
  ../experimental/adaptive_engine
  host_stubs
)
target_compile_options(test_cold_start_sim PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_cold_start_sim PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_cold_start_sim PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_cold_start_sim)

add_executable(test_tinyml_kernel
  test_tinyml_kernel.cpp
  ../IvannaTinyMLKernel.cpp
)
target_link_libraries(test_tinyml_kernel PRIVATE GTest::gtest_main Threads::Threads m)
target_include_directories(test_tinyml_kernel PRIVATE .. ../include)
target_compile_options(test_tinyml_kernel PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_tinyml_kernel PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_tinyml_kernel PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_tinyml_kernel)

# ── CORONA Frente A, defecto 3: frontend mel real del clasificador NEURO ──
# FFT radix-2 + banco triangular detrás de flag (default OFF). El test
# congela el legacy (siempre MUSIC) y afirma que con el flag ON el banco
# ya no es cero, es selectivo en frecuencia y la salida depende del
# contenido; apagar el flag restaura el legacy exacto.
add_executable(test_audio_classifier_frontend
  test_audio_classifier_frontend.cpp
  ../IvannaAudioClassifier.cpp
)
target_link_libraries(test_audio_classifier_frontend PRIVATE GTest::gtest_main Threads::Threads m)
target_include_directories(test_audio_classifier_frontend PRIVATE .. ../include)
target_compile_options(test_audio_classifier_frontend PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_audio_classifier_frontend PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_audio_classifier_frontend PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_audio_classifier_frontend)

# ── IVANNA Acoustic Unity Engine (v2.6.0) ──
ivanna_add_test(test_acoustic_unity_engine)
ivanna_add_test(test_unity_closed_loop)
# M8 (salto, Luna): supervisor de incertidumbre + rollback. Umbrales
# NOMINALES del contrato (0.80 / 0.50 / 0.20, kMaxAgeSeconds 300 s);
# el umbral de la Puerta lo actualiza el coordinador al integrar.
ivanna_add_test(test_uncertainty_supervisor)
ivanna_add_test(test_true_peak_limiter)
ivanna_add_test(test_punch_engine)







# ── Campo de presion coherente para AUX/altavoces (2.5.36) ──
add_executable(test_aux_field_coherence test_aux_field_coherence.cpp)
target_link_libraries(test_aux_field_coherence PRIVATE GTest::gtest_main)
target_include_directories(test_aux_field_coherence PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_aux_field_coherence PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_aux_field_coherence PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_aux_field_coherence PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_aux_field_coherence)

add_executable(test_temporal_spatial test_temporal_spatial.cpp)
target_link_libraries(test_temporal_spatial PRIVATE GTest::gtest_main)
target_include_directories(test_temporal_spatial PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_temporal_spatial PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_temporal_spatial PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_temporal_spatial PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_temporal_spatial)

add_executable(test_room_probe test_room_probe.cpp)
target_link_libraries(test_room_probe PRIVATE GTest::gtest_main)
target_include_directories(test_room_probe PRIVATE .. ../include ../spatial ../dsp host_stubs)
target_compile_options(test_room_probe PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_room_probe PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_room_probe PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_room_probe)

# Downsampler sinc polifásico Kaiser (Fase 4): 192k→48k con rechazo de alias
# >130 dB medido. Header-only (polyphase_sinc_downsampler.hpp), solo GTest.
add_executable(test_polyphase_sinc_downsampler test_polyphase_sinc_downsampler.cpp)
target_link_libraries(test_polyphase_sinc_downsampler PRIVATE GTest::gtest_main)
target_include_directories(test_polyphase_sinc_downsampler PRIVATE .. ../include host_stubs)
target_compile_options(test_polyphase_sinc_downsampler PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_polyphase_sinc_downsampler PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_polyphase_sinc_downsampler PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_polyphase_sinc_downsampler)

# Sensor acústico real por micrófono (Moto G85, v2.5.369): header-only,
# sin ivanna_dsp_under_test (solo GTest + FFTRadix2 + RoomProbe).
add_executable(test_real_acoustic_sensor test_real_acoustic_sensor.cpp)
target_link_libraries(test_real_acoustic_sensor PRIVATE GTest::gtest_main)
target_include_directories(test_real_acoustic_sensor PRIVATE .. ../include ../spatial ../dsp host_stubs)
target_compile_options(test_real_acoustic_sensor PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_real_acoustic_sensor PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_real_acoustic_sensor PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_real_acoustic_sensor)

# Canal lateral del sensor (v2.5.506, cableado mic → organismo): snapshot
# versionado + puerta kind 5 + fail-closed. Header-only (SensorSnapshot +
# neuro_acoustic_closed_loop), sin ivanna_dsp_under_test.
add_executable(test_sensor_snapshot test_sensor_snapshot.cpp)
target_link_libraries(test_sensor_snapshot PRIVATE GTest::gtest_main)
target_include_directories(test_sensor_snapshot PRIVATE .. ../include ../spatial ../neuromorphic ../dsp host_stubs)
target_compile_options(test_sensor_snapshot PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_sensor_snapshot PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_sensor_snapshot PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_sensor_snapshot)

add_executable(test_ab_harness test_ab_harness.cpp)
target_link_libraries(test_ab_harness PRIVATE GTest::gtest_main)
target_include_directories(test_ab_harness PRIVATE .. ../include ../spatial ../dsp host_stubs)
target_compile_options(test_ab_harness PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_ab_harness PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_ab_harness PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_ab_harness)

# ── Cadena 4D de excitación (conductor predictivo 5–10 ms, háptica multibanda,
#    cancelación de diafonía para AUX y dosímetro de exposición WHO/H.870) ──
add_executable(test_4d_ultra_chain test_4d_ultra_chain.cpp)
target_link_libraries(test_4d_ultra_chain PRIVATE GTest::gtest_main)
target_include_directories(test_4d_ultra_chain PRIVATE .. ../include ../spatial
  ../neuromorphic ../supreme ../music_intelligence ../dsp host_stubs)
target_compile_options(test_4d_ultra_chain PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_4d_ultra_chain PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_4d_ultra_chain PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_4d_ultra_chain)

# ── Superficie de control GLOBAL de la Cadena 4D (cableado punta a punta, v2.5.47) ──
# Ejercita el MISMO camino que usa el dispositivo (ensureInit / setEnabled / feedPlanar /
# applyOutputPlanar / publishAnticipatedScene / status) sobre la cadena y el extractor
# REALES: identidad estricta apagada, apertura anticipada a 120 BPM, techo háptico,
# acumulación de dosis y publicación en el SceneTargetBus. Es la barrera del cableado
# JNI/UI antes de tocar el dispositivo. Compila la .cpp real de la superficie.
add_executable(test_4d_control_surface
  test_4d_control_surface.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../music_intelligence/FourDControlSurface.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../music_intelligence/MusicFeatureExtractor.cpp
)
target_link_libraries(test_4d_control_surface PRIVATE GTest::gtest_main)
target_include_directories(test_4d_control_surface PRIVATE .. ../include ../spatial
  ../neuromorphic ../supreme ../music_intelligence ../dsp host_stubs)
target_compile_options(test_4d_control_surface PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_4d_control_surface PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_4d_control_surface PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_4d_control_surface)

# ── Arco narrativo (MacroArcConductor, escala de segundos) ──
# Aislado con características MEDIDAS en el extractor real y, a nivel cadena, con el extractor
# y FourDExcitationChain reales: apagado = bit-exacto, detección del hueco/impacto del drop.
add_executable(test_macro_arc_conductor
  test_macro_arc_conductor.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../music_intelligence/MusicFeatureExtractor.cpp
)
target_link_libraries(test_macro_arc_conductor PRIVATE GTest::gtest_main)
target_include_directories(test_macro_arc_conductor PRIVATE .. ../include ../spatial
  ../neuromorphic ../supreme ../music_intelligence ../dsp host_stubs)
target_compile_options(test_macro_arc_conductor PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_macro_arc_conductor PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_macro_arc_conductor PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_macro_arc_conductor)

add_executable(test_npe_core test_npe_core.cpp ../phase_oracle.cpp)
target_link_libraries(test_npe_core PRIVATE GTest::gtest_main)
target_include_directories(test_npe_core PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_npe_core PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_npe_core PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_npe_core PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_npe_core)

# 0024 (elevación magistral del DSP): NPE a parámetros VIVOS de
# AdaptiveControlsPrefs (harm 0.60 / lat 0.50 / ohc 0.40 / master +2.5 dB),
# con THD+N (ThdMeter), respuesta en frecuencia y coste por bloque.
add_executable(test_route_a_npe_live_params test_route_a_npe_live_params.cpp ../phase_oracle.cpp)
target_link_libraries(test_route_a_npe_live_params PRIVATE GTest::gtest_main)
target_include_directories(test_route_a_npe_live_params PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_route_a_npe_live_params PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_route_a_npe_live_params PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_route_a_npe_live_params PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_route_a_npe_live_params)

# 0025 (hallazgos en vivo 2026-11-16 11:18 CST): pacing de nivel por
# descarte en la CAPTURA — el PLL se saturaba en el riel (-500 ppm,
# demanda -557.3) con el buffer al 99% (7619/7696). Espejo de 0020
# (BridgePlayer), adaptado a push: se descarta el bloque, no se pausa el
# productor. Modelo de tasas en host (sin Android).
add_executable(test_capture_level_control test_capture_level_control.cpp)
target_link_libraries(test_capture_level_control PRIVATE GTest::gtest_main)
target_include_directories(test_capture_level_control PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_capture_level_control PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_capture_level_control PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_capture_level_control PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_capture_level_control)

# 0027 (elevación omnipotente del DSP): compresor MULTIBANDA 3 bandas
# (crossovers Linkwitz-Riley 4º orden, 200 Hz / 4 kHz) — el monobanda
# g_comp comprime todo el espectro con un solo detector (pumping).
add_executable(test_multiband_compressor test_multiband_compressor.cpp)
target_link_libraries(test_multiband_compressor PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_multiband_compressor PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_multiband_compressor PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_multiband_compressor PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_multiband_compressor PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_multiband_compressor)

# ── Contrato de read-back del ancho espacial (Fotoholografía, v2.5.68) ──
# El read-back (nativeGetSpatialWidthDirect) vive en ivanna_omega_jni.cpp, que no se compila
# en host. Esta puerta verifica la función pura que el JNI usa (supreme/SpatialWidthReadback).
add_executable(test_spatial_width_readback test_spatial_width_readback.cpp)
target_link_libraries(test_spatial_width_readback PRIVATE GTest::gtest_main)
target_include_directories(test_spatial_width_readback PRIVATE .. ../include ../spatial
  ../neuromorphic ../supreme ../music_intelligence ../dsp host_stubs)
target_compile_options(test_spatial_width_readback PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_spatial_width_readback PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_spatial_width_readback PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_spatial_width_readback)
 
# RP-88 spatial angle command read-back: bounded native command, not acoustic measurement.
add_executable(test_spatial_angle_readback test_spatial_angle_readback.cpp)
target_link_libraries(test_spatial_angle_readback PRIVATE GTest::gtest_main)
target_include_directories(test_spatial_angle_readback PRIVATE .. ../include ../spatial ../supreme host_stubs)
target_compile_options(test_spatial_angle_readback PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_spatial_angle_readback PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_spatial_angle_readback PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_spatial_angle_readback)

add_executable(test_physical_scene_readback test_physical_scene_readback.cpp)
target_link_libraries(test_physical_scene_readback PRIVATE GTest::gtest_main)
target_include_directories(test_physical_scene_readback PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_physical_scene_readback PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_physical_scene_readback PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_physical_scene_readback PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_physical_scene_readback)


# ── IVANNA autonomous acoustic transaction and 3D geometry renderer ──
add_executable(test_scene_geometry_renderer test_scene_geometry_renderer.cpp)
target_link_libraries(test_scene_geometry_renderer PRIVATE GTest::gtest_main)
target_include_directories(test_scene_geometry_renderer PRIVATE .. ../include ../spatial ../agent ../dsp host_stubs)
target_compile_options(test_scene_geometry_renderer PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_scene_geometry_renderer PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_scene_geometry_renderer PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_scene_geometry_renderer)

add_executable(test_autonomous_acoustic_transaction test_autonomous_acoustic_transaction.cpp)
target_link_libraries(test_autonomous_acoustic_transaction PRIVATE GTest::gtest_main)
target_include_directories(test_autonomous_acoustic_transaction PRIVATE .. ../include ../spatial ../agent ../dsp host_stubs)
target_compile_options(test_autonomous_acoustic_transaction PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_autonomous_acoustic_transaction PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_autonomous_acoustic_transaction PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_autonomous_acoustic_transaction)


# Control-plane peer authentication: reject untrusted app UIDs before command/SHM handling.
ivanna_add_test(test_daemon_peer_auth)
# PAL integration test registration
# USB ASRC cutoff contract (kernel cutoff is normalized to Nyquist).
add_executable(test_usb_asrc_clock_servo test_usb_asrc_clock_servo.cpp)
target_link_libraries(test_usb_asrc_clock_servo PRIVATE GTest::gtest_main)
target_include_directories(test_usb_asrc_clock_servo PRIVATE ../include)
target_compile_options(test_usb_asrc_clock_servo PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_usb_asrc_clock_servo PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_usb_asrc_clock_servo PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_usb_asrc_clock_servo)

add_executable(test_usb_asrc_cutoff test_usb_asrc_cutoff.cpp)
target_link_libraries(test_usb_asrc_cutoff PRIVATE GTest::gtest_main)
target_include_directories(test_usb_asrc_cutoff PRIVATE ../include)
target_compile_options(test_usb_asrc_cutoff PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_usb_asrc_cutoff PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_usb_asrc_cutoff PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_usb_asrc_cutoff)

# Exercise the exact production USB ASRC coefficients in host spectral regressions.
add_executable(test_usb_asrc_spectral_response test_usb_asrc_spectral_response.cpp)
target_link_libraries(test_usb_asrc_spectral_response PRIVATE GTest::gtest_main)
target_include_directories(test_usb_asrc_spectral_response PRIVATE ../include)
target_compile_options(test_usb_asrc_spectral_response PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_usb_asrc_spectral_response PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_usb_asrc_spectral_response PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_usb_asrc_spectral_response)


# Full-circle MAJESTY 3D azimuth: preserve rear hemisphere and wrap head yaw.
add_executable(test_azimuth_math test_azimuth_math.cpp)
target_link_libraries(test_azimuth_math PRIVATE GTest::gtest_main)
target_include_directories(test_azimuth_math PRIVATE ../include)
target_compile_options(test_azimuth_math PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_azimuth_math PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_azimuth_math PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_azimuth_math)

ivanna_add_test(test_qualcomm_pal_clock_integration)
ivanna_add_test(test_pal_mmap_position_adapter)

# ── Productor del reloj HAL (daemon): descubrimiento /proc/asound, puerta de
# correlacion y publicacion real en el snapshot compartido. Fail-closed sin ASRC
# fisico identificado: el actuador del Moto G85 sigue sin enlazar a proposito.
add_executable(test_hal_clock_producer test_hal_clock_producer.cpp ../daemon/hal_clock_producer.cpp)
target_link_libraries(test_hal_clock_producer PRIVATE GTest::gtest_main)
target_include_directories(test_hal_clock_producer PRIVATE ../include ../daemon)
target_compile_options(test_hal_clock_producer PRIVATE -Wall -Wextra)
gtest_discover_tests(test_hal_clock_producer)

# AsrcClockController antes no tenia ningun consumidor real fuera de su propio
# test (2026-10-24). Este test valida el canal IPC nuevo (asrc_state_bridge.hpp)
# que el daemon ahora usa para publicar su decision de extremo a extremo.
add_executable(test_asrc_state_bridge test_asrc_state_bridge.cpp)
target_link_libraries(test_asrc_state_bridge PRIVATE GTest::gtest_main)
target_include_directories(test_asrc_state_bridge PRIVATE ../include ../dsp)
target_compile_options(test_asrc_state_bridge PRIVATE -Wall -Wextra)
gtest_discover_tests(test_asrc_state_bridge)

# Relevo del dump del write-stage ASRC PAL v5 (2026-11-10): el daemon lee
# /data/vendor/ivanna_audio/pal_asrc_stage.txt y lo releva en
# GET_CLOCK_TELEMETRY. Solo parser + frescura; el HAL vivo se verifica en
# el teléfono (analysis-ivanna/PLAN_VERIFICACION_ASRC_384.md).
add_executable(test_write_stage_asrc_telemetry test_write_stage_asrc_telemetry.cpp)
target_link_libraries(test_write_stage_asrc_telemetry PRIVATE GTest::gtest_main)
target_include_directories(test_write_stage_asrc_telemetry PRIVATE ../include)
target_compile_options(test_write_stage_asrc_telemetry PRIVATE -Wall -Wextra)
gtest_discover_tests(test_write_stage_asrc_telemetry)

# Regression: signed stereo 4x true-peak interpolation must preserve phase and
# cannot report a lower peak than sample peak when both observe the same stream.
add_executable(test_loudness_true_peak test_loudness_true_peak.cpp)
target_link_libraries(test_loudness_true_peak PRIVATE GTest::gtest_main)
target_include_directories(test_loudness_true_peak PRIVATE .. ../include)
target_compile_options(test_loudness_true_peak PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_loudness_true_peak PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_loudness_true_peak PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_loudness_true_peak)

# 16-to-5 logical bus dispatcher: bounded SIMD/scalar kernel and sensor-gated thermal guard.
ivanna_add_test(test_physical_target_dispatcher)

# ── Gobernador del spread WFS: apertura acotada, suavizado y defensa NaN ──
add_executable(test_wfs_spread_governor test_wfs_spread_governor.cpp)
target_link_libraries(test_wfs_spread_governor PRIVATE GTest::gtest_main)
target_include_directories(test_wfs_spread_governor PRIVATE ../ ../spatial ../include)
target_compile_options(test_wfs_spread_governor PRIVATE -Wall -Wextra)
gtest_discover_tests(test_wfs_spread_governor)

# Production SAF update and persistence regression coverage.
add_executable(test_saf_optimizer_state test_saf_optimizer_state.cpp)
target_link_libraries(test_saf_optimizer_state PRIVATE ivanna_unified_pipeline_under_test GTest::gtest_main Threads::Threads m dl)
target_include_directories(test_saf_optimizer_state PRIVATE .. host_stubs)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_saf_optimizer_state PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_saf_optimizer_state PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_saf_optimizer_state)


# FotoHolo Route B: 64-band Bark observer, fail-closed silence and bounded modulation.
ivanna_add_test(test_fotoholo_bark_meter)


# Evidencia medida por procesador: HRTF / RIR / SAF (con controles negativos).
add_executable(test_processor_evidence
  test_processor_evidence.cpp
  ../HrtfManager.cpp
  ../SofaHRTFLoader.cpp
  # LGN + Ley de Curvatura (2026-11-18): HrtfManager::synthesizeHrtf usa
  # geodesic_pinna_model (+ physical_head_model) y physical_propagation
  # (curva ITD cerrada; la inversión de distancia NO se usa).
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_head_model.cpp
  ../fused/physical_propagation.cpp
  ../HRTFBinLoader.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_processor_evidence PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_processor_evidence PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_processor_evidence PRIVATE -Wall -Wextra)
add_test(NAME test_processor_evidence COMMAND test_processor_evidence)

# LGN cableada en HrtfManager::synthesizeHrtf (2026-11-18): el pabellón se
# aplica al HRIR y el notch medido coincide con la predicción; OFF = esfera
# sola. Mismas fuentes que test_processor_evidence.
add_executable(test_hrtf_pinna_lgn
  test_hrtf_pinna_lgn.cpp
  ../HrtfManager.cpp
  ../SofaHRTFLoader.cpp
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_head_model.cpp
  ../fused/physical_propagation.cpp
  ../HRTFBinLoader.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_hrtf_pinna_lgn PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_hrtf_pinna_lgn PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_hrtf_pinna_lgn PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_hrtf_pinna_lgn PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_hrtf_pinna_lgn PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_hrtf_pinna_lgn)

# IVANNA-APEX (2026-11-18): motor holográfico unificado 64-bit, standalone
# (sin enchufar en FusedChain). El header es header-only; las fuentes son
# las implementaciones que llama (esfera rígida + propagación/curvatura).
add_executable(test_ivanna_apex_kernel
  test_ivanna_apex_kernel.cpp
  ../fused/physical_head_model.cpp
  ../fused/physical_propagation.cpp
)
target_link_libraries(test_ivanna_apex_kernel PRIVATE GTest::gtest_main Threads::Threads)
target_include_directories(test_ivanna_apex_kernel PRIVATE .. ../include host_stubs)
target_compile_options(test_ivanna_apex_kernel PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_ivanna_apex_kernel PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_ivanna_apex_kernel PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_ivanna_apex_kernel)


# HAL PAL -> bounded servo -> variable-ratio ASRC output path.
# This is a host contract test; it does not claim a Motorola vendor-HAL build.
add_executable(test_pal_asrc_output_path
  test_pal_asrc_output_path.cpp
  ../../../../../hal_integration/PalAsrcBridge.cpp
)
target_include_directories(test_pal_asrc_output_path PRIVATE
  ..
  ../include
  ../../../../../hal_integration
)
target_link_libraries(test_pal_asrc_output_path PRIVATE GTest::gtest_main Threads::Threads)
target_compile_options(test_pal_asrc_output_path PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_pal_asrc_output_path PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_pal_asrc_output_path PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_pal_asrc_output_path)


# Compile the Moto G85 write-stage header in host CI. This validates the
# source-level adapter and PAL byte-count callback contract; it is not a
# substitute for compiling against Motorola's private target PalApi.h.
add_executable(test_moto_g85_pal_asrc_write_stage
  test_moto_g85_pal_asrc_write_stage.cpp
  ../../../../../hal_integration/PalAsrcBridge.cpp
)
target_include_directories(test_moto_g85_pal_asrc_write_stage PRIVATE
  ..
  ../include
  ../../../../../hal_integration
)
target_link_libraries(test_moto_g85_pal_asrc_write_stage PRIVATE
  GTest::gtest_main
  Threads::Threads
)
target_compile_options(test_moto_g85_pal_asrc_write_stage PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_moto_g85_pal_asrc_write_stage PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_moto_g85_pal_asrc_write_stage PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_moto_g85_pal_asrc_write_stage)

# ── Perfil de sala MAGISTRAL + geometría del transaural derivada de la sala ──
add_executable(test_magistral_room_xtc test_magistral_room_xtc.cpp)
target_link_libraries(test_magistral_room_xtc PRIVATE GTest::gtest_main)
target_include_directories(test_magistral_room_xtc PRIVATE .. ../include ../spatial host_stubs)
target_compile_options(test_magistral_room_xtc PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_magistral_room_xtc PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_magistral_room_xtc PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_magistral_room_xtc)

# ── test_session_discovery: daemon root descubre sessionId real (Ruta B, v2.5.379) ──
# La sesión 0 es un callejón sin salida (el efecto se crea pero nunca recibe
# frames) y el sondeo por reflexión no ve sessionIds reales. El daemon (root)
# ejecuta `dumpsys media.audio_flinger` y parsea "Global session refs".
# Header-only (session_discovery.hpp); cubre parseo moderno/antiguo, selección
# de sesión de música y ABI v11 del snapshot (offsets viejos sin cambio).
ivanna_add_test(test_session_discovery)

# ── 5D FASE 0 (PROPUESTA_5D.md §5, v2.5.383): BioControl5D + EEG sintético ──
# Header-only (spatial/BioControl5D.hpp): dinámica neural (3.2), lazo de
# control Γ_b/r_ORBE/K con clamps y slew-rates (3.3), fail-closed sin sensor
# y tras perderlo, tono 884 Hz modulado por η (3.7). Host-only, sin cableado
# al DSP real ni al hilo RT. Sin sensor EEG real no hay Fases 1–3.
ivanna_add_test(test_5d_phase0)

# ── FORMULACIÓN MAGISTRAL Ψ (2026-11-10): campo unificado acústico-neural ──
# spatial/PsiField.hpp: Ec. 2 (ley de distancia campo cercano, corregida
# contra el modelo geométrico), Ec. 3 (operador de personalización CIPIC),
# Ec. 4 (factor Γ y EDO con pérdida en BioControl5D). Header-only.
ivanna_add_test(test_psi_field)

# ── P0-b (2026-11-10): planificación del hal-clock-asrc-poller ──────────────
# include/poller_sched.hpp: decisión pura (FIFO rtprio 2 si se permite,
# SCHED_OTHER si no). El daemon intenta FIFO primero y cae a best-effort.
ivanna_add_test(test_poller_sched)

# ── Reformulación Ψ por vía (2026-11-10): el campo conoce la vía ─────────────
# (audífonos/AUX/celular/estéreo/5.1/7.1/cine): ILD percibido por crosstalk,
# bocinas por ley dipolar, radio de cabeza personalizado cableado a ORBE.
ivanna_add_test(test_psi_field_routes)

# ── Operador de transducción universal (MANT Ec. 1, núcleo usable) ───────────
# Compensación por banda con límite WNG (mínimos cuadrados + Tikhonov por
# bisección). Sin medida del transductor devuelve no-op (no inventa).
ivanna_add_test(test_transducer_operator)

# ── FÍSICA NUEVA CONSOLIDADA (Fase 2, v2.5.472) ────────────────────────────
# Headers-only (fused/new_physics_*.hpp + neuromorphic/laguerre_volterra_h2.hpp):
# bypass bit-exacto, LGN 3D, Laguerre H2 68.5 Hz, Ω D_max + regla de banda,
# motor completo NaN/Inf + presupuesto CPU por hop (6670 µs).
ivanna_add_test(test_new_physics_consolidated)

# ── Tiempos por etapa del DSP (v2.5.411, tarea 4) ────────────────────────────
# ivanna_stage_timings.h: media/p99/max por etapa (lock-free, anillo 256).
# La instancia g_stageTimings vive en ../ivanna_route_a_asrc.cpp (misma
# definición que enlaza el .so); por eso se compila aquí también.
add_executable(test_stage_timings
  test_stage_timings.cpp
  ../ivanna_route_a_asrc.cpp
)
target_link_libraries(test_stage_timings PRIVATE GTest::gtest_main ivanna_dsp_under_test)
# ../ivanna_route_a_asrc.cpp incluye fir_v5_coeffs.h, que vive en
# hal_integration (mismo include dir que el CMakeLists principal añade
# para el .so; el archivo está trackeado, así que la CI lo tiene).
target_include_directories(test_stage_timings PRIVATE .. ../include host_stubs ../../../../../hal_integration/moto_g85/pal_preload_probe)
target_compile_options(test_stage_timings PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_stage_timings PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_stage_timings PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_stage_timings)

# ── test_route_a_asrc: ASRC magistral Ruta A (v2.5.404+, Ψ viva) ─────────────
# ivanna_route_a_asrc_test.cpp vivía en app/src/main/cpp/ SIN cablear en
# tests/CMakeLists.txt: sus 14 grupos de checks (a)-(n) — tasa, THD+N,
# continuidad, fail-closed, Ψ viva, coste por bloque — existían pero NUNCA
# corrían en CTest/CI. Es un ejecutable standalone con su propio main()
# (no gtest): se registra con add_test y CTest lo evalúa por código de
# salida (main retorna 0 si todos pasan). Sin FAIL_REGULAR_EXPRESSION: bajo
# ctest -j4 el regex compartido "FALLARON" marcaba fallos de OTRO binario
# (ver test_adaptive_engine). Los include dirs son los mismos que
# test_stage_timings (fir_v5_coeffs.h vive en hal_integration).
# RUN_SERIAL: la sección (n) mide COSTE por bloque (umbral 500 µs) y es
# sensible a contención de CPU — bajo ctest -j4 el coste medido sube a
# 874/991 µs (vs 333/376 µs solo) y falla por timing, no por regresión.
# Por eso corre serial, sin competir por CPU con otros tests.
add_executable(test_route_a_asrc
  ../ivanna_route_a_asrc_test.cpp
  ../ivanna_route_a_asrc.cpp
)
target_include_directories(test_route_a_asrc PRIVATE .. ../include host_stubs ../../../../../hal_integration/moto_g85/pal_preload_probe)
target_compile_options(test_route_a_asrc PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_route_a_asrc PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_route_a_asrc PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_route_a_asrc COMMAND test_route_a_asrc)
set_tests_properties(test_route_a_asrc PROPERTIES RUN_SERIAL TRUE)

# ── 0029 (doble precisión): acumulación float64 en el FIR ASRC 319 taps y
# en la FDL del RirConvolver, con fallback a float32 por coste. ──────────
# (add_executable explícito: ivanna_route_a_asrc.cpp y RirConvolver.cpp no
# están en ivanna_dsp_under_test; fir_v5_coeffs.h vive en hal_integration,
# mismo include dir que test_route_a_asrc.)
add_executable(test_double_precision
  test_double_precision.cpp
  ../ivanna_route_a_asrc.cpp
  ../spatial/RirConvolver.cpp
)
target_link_libraries(test_double_precision PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_double_precision PRIVATE .. ../include host_stubs ../../../../../hal_integration/moto_g85/pal_preload_probe)
target_compile_options(test_double_precision PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_double_precision PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_double_precision PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_double_precision)

# ── 0033: bypass nativo del ASRC (decisión de Esse 2026-11-16) ────────
# El ASRC no va en el Snapdragon: setEnabled(false) = passthrough
# bit-exacto. Mismo patrón que test_double_precision (el .cpp del ASRC
# no está en ivanna_dsp_under_test).
add_executable(test_route_a_asrc_bypass
  test_route_a_asrc_bypass.cpp
  ../ivanna_route_a_asrc.cpp
)
target_link_libraries(test_route_a_asrc_bypass PRIVATE GTest::gtest_main ivanna_dsp_under_test)
target_include_directories(test_route_a_asrc_bypass PRIVATE .. ../include host_stubs ../../../../../hal_integration/moto_g85/pal_preload_probe)
target_compile_options(test_route_a_asrc_bypass PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_route_a_asrc_bypass PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_route_a_asrc_bypass PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_route_a_asrc_bypass)

# ── 0031: gobernador de shed POR RUTA (tronidos continuos en la 458) ──
# Un solo DspLoadShedGovernor global servía a BridgePlayer (41.3 ms) y
# captura (6.67 ms): la deuda de la captura hacía soltar ~20% de los
# bloques al BridgePlayer (tronido continuo). RouteShedGovernors separa
# la deuda por periodo. El test reproduce el bug con el modelo compartido
# y verifica el aislamiento con el modelo separado. Header-only (sin .cpp).
add_executable(test_shed_governor_split
  test_shed_governor_split.cpp
)
target_link_libraries(test_shed_governor_split PRIVATE GTest::gtest_main)
target_include_directories(test_shed_governor_split PRIVATE .. ../include ../dsp host_stubs)
target_compile_options(test_shed_governor_split PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_shed_governor_split PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_shed_governor_split PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_shed_governor_split)

# ── 0021: tests para los módulos sin tests restantes (decisión del Consejo ──
# Supremo 2026-11-16, ronda 1 unánime: candidato (e)). audio_orchestrator.cpp
# y usb_audio_pro_manager.cpp eran 2 de los 3 módulos top-level sin tests
# (audit_2026-11-16.md §5). omega_effect.cpp queda documentado como
# BLOQUEADO para host en el REPORT de 0021 (IvannaFusionCore no enlaza en
# host + el drain callback no compila con g++-14); su beacon y su ABI YA
# tienen tests (test_effect_beacon, test_audio_effect_abi_layout,
# test_audio_effect_library_tag).
# ── test_audio_orchestrator: orquestador maestro (ganancia + widener + ────
# sanitizador + LUFS BS.1770-4). Compila el .cpp REAL con sus dependencias
# reales (audio_control_plane.cpp + evolutionary_kernel_v2.cpp) en host con
# host_stubs (jni.h + android/log.h).
add_executable(test_audio_orchestrator
  test_audio_orchestrator.cpp
  ../audio_orchestrator.cpp
  ../audio_control_plane.cpp
  ../evolutionary_kernel_v2.cpp
)
target_include_directories(test_audio_orchestrator PRIVATE .. ../include host_stubs)
target_compile_options(test_audio_orchestrator PRIVATE -Wall -Wextra)
target_link_libraries(test_audio_orchestrator PRIVATE GTest::gtest_main Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_audio_orchestrator PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_audio_orchestrator PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_audio_orchestrator)

# ── test_master_bypass: bypass maestro (2.5.461, propuesta 0035). Compila
# la TU REAL ivanna_jni_stub.cpp (flag g_masterBypass + stub
# nativeSetBypass) con sus dependencias reales (audio_orchestrator.cpp +
# audio_control_plane.cpp + evolutionary_kernel_v2.cpp) en host con
# host_stubs. Verifica el contrato del flag a través del JNI real y la
# colocación del guard en ivanna_omega_jni.cpp (test estructural).
# Límite: el early return de nativeProcess no compila en host
# (CMakeLists:2089); su colocación se verifica por revisión + escucha en
# dispositivo.
add_executable(test_master_bypass
  test_master_bypass.cpp
  ../jni/ivanna_jni_stub.cpp
  ../audio_orchestrator.cpp
  ../audio_control_plane.cpp
  ../evolutionary_kernel_v2.cpp
)
target_include_directories(test_master_bypass PRIVATE .. ../include host_stubs)
target_compile_options(test_master_bypass PRIVATE -Wall -Wextra)
# Ruta del .cpp real de nativeProcess para el test estructural
# MasterBypass.GuardPlacement (lee el archivo y verifica el orden
# bypass < guard < shed < GetFloatArrayElements dentro de nativeProcess).
target_compile_definitions(test_master_bypass PRIVATE
  IVANNA_OMEGA_JNI_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../jni/ivanna_omega_jni.cpp")
target_link_libraries(test_master_bypass PRIVATE GTest::gtest_main Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_master_bypass PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_master_bypass PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_master_bypass)

# ── test_usb_audio_pro_manager: gestor USB Pro (endpoint isócrono + ASRC + ──
# feedback UAC). Compila el .cpp REAL en host con host_stubs (jni.h
# extendido en 0021 con NewIntArray/SetIntArrayRegion). Sin DAC USB real:
# solo comportamiento fail-closed (inactivo por defecto, escribir devuelve
# 0, telemetría a cero, arrancar con fd inválido no arranca).
add_executable(test_usb_audio_pro_manager
  test_usb_audio_pro_manager.cpp
  ../usb_audio_pro_manager.cpp
)
target_include_directories(test_usb_audio_pro_manager PRIVATE .. ../include host_stubs)
target_compile_options(test_usb_audio_pro_manager PRIVATE -Wall -Wextra)
target_link_libraries(test_usb_audio_pro_manager PRIVATE GTest::gtest_main Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_usb_audio_pro_manager PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_usb_audio_pro_manager PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_usb_audio_pro_manager)

# ── 0022: medidor THD+N (Punto 3 de la crítica externa, mediciones físicas) ──
# Decisión del Consejo Supremo 2026-11-16, ronda 1 unánime: punto (A).
# ThdMeter.hpp es header-only (sin dependencias Android): la misma matemática
# mide en host y analiza las grabaciones del teléfono. El test verifica los
# valores exactos (seno puro < −120 dB; 2º armónico a −40/−60 dB medido a
# ±0.5 dB; clipping detectado).
add_executable(test_thd_meter test_thd_meter.cpp)
target_include_directories(test_thd_meter PRIVATE .. ../include)
target_compile_options(test_thd_meter PRIVATE -Wall -Wextra)
target_link_libraries(test_thd_meter PRIVATE GTest::gtest_main)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_thd_meter PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_thd_meter PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_thd_meter)

# ── FUSIÓN MAGISTRAL (2026-11-18, directiva de Esse) ──
# 7 operadores únicos fusionados de los ~45 efectos. Cada test sigue el
# contrato fused (NeutralIsBitExact, NeutralGainUnity, NoNaNOrInf,
# ControlClamping, EngagedSmoke, ThdAtNeutral). La cadena y la doble ruta
# verifican el núcleo compartido en Ruta A y Ruta B.
add_executable(test_fused_transducer
  test_fused_transducer.cpp
  ../fused/fused_transducer.cpp
)
target_include_directories(test_fused_transducer PRIVATE .. ../include host_stubs)
target_compile_options(test_fused_transducer PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_transducer PRIVATE GTest::gtest_main ivanna_dsp_under_test)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_transducer PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_transducer PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_transducer)

add_executable(test_fused_dynamics
  test_fused_dynamics.cpp
  ../fused/fused_dynamics.cpp
)
target_include_directories(test_fused_dynamics PRIVATE .. ../include host_stubs)
target_compile_options(test_fused_dynamics PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_dynamics PRIVATE GTest::gtest_main ivanna_dsp_under_test)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_dynamics PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_dynamics PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_dynamics)

add_executable(test_fused_tonal
  test_fused_tonal.cpp
  ../fused/fused_tonal.cpp
  ../EvolutionaryEQ.cpp
  ../Psychoacoustics.cpp
)
target_include_directories(test_fused_tonal PRIVATE .. ../include host_stubs)
target_compile_options(test_fused_tonal PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_tonal PRIVATE GTest::gtest_main ivanna_dsp_under_test)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_tonal PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_tonal PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_tonal)

add_executable(test_fused_brain
  test_fused_brain.cpp
  ../fused/fused_brain.cpp
  ../IvannaAudioClassifier.cpp
  ../IvannaVoiceProsodyEngine.cpp
  ../IvannaTinyML.cpp
  ../IvannaTinyMLKernel.cpp
  ../IvannaSelfHealingEngine.cpp
  ../IvannaSuperAgentMemory.cpp
  ../phase_oracle.cpp
)
target_include_directories(test_fused_brain PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_fused_brain PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_brain PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_brain PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_brain PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_brain)

add_executable(test_fused_spatial
  test_fused_spatial.cpp
  ../fused/fused_spatial.cpp
  ../spatial/IntelligentUpmixer.cpp
  ../spatial/HoaBinauralDecoder.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafPcaDecoder.cpp
  ../HRTFBinLoader.cpp
  ../HrtfManager.cpp
  ../SofaHRTFLoader.cpp
  # LGN + Ley de Curvatura (2026-11-18): HrtfManager::synthesizeHrtf usa
  # geodesic_pinna_model (+ physical_head_model) y physical_propagation
  # (curva ITD cerrada; la inversión de distancia NO se usa).
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_head_model.cpp
  ../fused/physical_propagation.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/WfsRenderer.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/ivanna_head_tracker.cpp
)
target_include_directories(test_fused_spatial PRIVATE .. ../include host_stubs)
target_compile_options(test_fused_spatial PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_spatial PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_spatial PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_spatial PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_spatial)

# ── test_poincare_pan: pan Poincaré de campo cercano (2026-11-23) ──────────
# Cableado en FusedSpatial (sustituye el spread WFS) detrás del flag
# kPoincarePanEnabled. ESTE TARGET ES EL ÚNICO QUE COMPILA CON EL FLAG ON
# (-DIVANNA_POINCARE_PAN_ENABLED=1): es el único que ejercita el camino
# ON; el resto de la suite (y producción) corre con el flag OFF. Mismas
# fuentes que test_fused_spatial (todo lo que FusedSpatial necesita).
add_executable(test_poincare_pan
  test_poincare_pan.cpp
  ../fused/fused_spatial.cpp
  ../spatial/IntelligentUpmixer.cpp
  ../spatial/HoaBinauralDecoder.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafPcaDecoder.cpp
  ../HRTFBinLoader.cpp
  ../HrtfManager.cpp
  ../SofaHRTFLoader.cpp
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_head_model.cpp
  ../fused/physical_propagation.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/WfsRenderer.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/ivanna_head_tracker.cpp
)
target_include_directories(test_poincare_pan PRIVATE .. ../include host_stubs)
target_compile_definitions(test_poincare_pan PRIVATE IVANNA_POINCARE_PAN_ENABLED=1)
target_compile_options(test_poincare_pan PRIVATE -Wall -Wextra)
target_link_libraries(test_poincare_pan PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_poincare_pan PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_poincare_pan PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_poincare_pan)

# ── test_sofa_comparison: arnes de comparacion SOFA (Trabajador C, 2026-11-18) ──
# HRTF medida (kemar.ihr1, conversion de MIT_KEMAR_normal_pinna.sofa) vs
# IR efectiva de ORBE·Ω (FusedSpatial) vs par reconstruido de SofaHRTFLoader.
# Metricas comunes en sofa_comparison_metrics.hpp; criterio "fisica nueva
# verificada" (a)+(b)+(c) en SOFA_COMPARISON_METRICS.md. Mismas fuentes que
# test_fused_spatial + SofaHRTFLoader.cpp (android/log.h sale de host_stubs).
get_filename_component(IVANNA_SOFA_ASSET_DIR
  "${CMAKE_SOURCE_DIR}/../../assets/ivanna_omega/sofa" ABSOLUTE)
add_executable(test_sofa_comparison
  test_sofa_comparison.cpp
  ../fused/fused_spatial.cpp
  ../spatial/IntelligentUpmixer.cpp
  ../spatial/HoaBinauralDecoder.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafPcaDecoder.cpp
  ../HRTFBinLoader.cpp
  ../HrtfManager.cpp
  ../SofaHRTFLoader.cpp
  ../fused/physical_head_model.cpp
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_propagation.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/WfsRenderer.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/ivanna_head_tracker.cpp
)
target_include_directories(test_sofa_comparison PRIVATE .. ../include host_stubs)
target_compile_options(test_sofa_comparison PRIVATE -Wall -Wextra)
target_link_libraries(test_sofa_comparison PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads)
target_compile_definitions(test_sofa_comparison PRIVATE
  IVANNA_HRTF_ASSET_DIR="${IVANNA_HRTF_ASSET_DIR}"
  IVANNA_SOFA_ASSET_DIR="${IVANNA_SOFA_ASSET_DIR}"
)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_sofa_comparison PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_sofa_comparison PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_sofa_comparison)

add_executable(test_fused_field
  test_fused_field.cpp
  ../fused/fused_field.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
  ../spatial/RirConvolver.cpp
  ../phase_oracle.cpp
)
target_include_directories(test_fused_field PRIVATE .. ../include host_stubs)
target_compile_options(test_fused_field PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_field PRIVATE GTest::gtest_main ivanna_dsp_under_test)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_field PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_field PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_field)

add_executable(test_fused_listener_room
  test_fused_listener_room.cpp
  ../fused/fused_listener_room.cpp
  ../spatial/RirConvolver.cpp
)
target_include_directories(test_fused_listener_room PRIVATE .. ../include host_stubs)
target_compile_options(test_fused_listener_room PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_listener_room PRIVATE GTest::gtest_main ivanna_dsp_under_test)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_listener_room PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_listener_room PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_listener_room)

# Fuentes compartidas de la cadena (unión de los 7 operadores + sus motores).
# volterra_h2_symmetric.cpp y predictive_audio_engine.cpp ya están en
# ivanna_dsp_under_test y NO se listan aquí (símbolo duplicado si se repiten).
set(FUSED_CHAIN_SOURCES
  ../fused/fused_chain.cpp
  ../fused/perceptual_conservation.cpp  # 8. Árbitro de campo (2026-11-21)
  ../fused/acoustic_twin/acoustic_twin.cpp  # gemelo + calibración P3 (call site)
  # CORONA P7–P9 (cableado 2026-11-24): FusedChain los usa (certificado,
  # compilador tras flag OFF, modelo de sala). P10 NO va aquí: banco de
  # pruebas sin call site en la cadena (su target propio está abajo).
  ../fused/total_conservation/total_conservation.cpp
  ../fused/experience_compiler/experience_compiler.cpp
  ../fused/living_room/living_room.cpp
  ../fused/fused_spatial.cpp
  ../fused/fused_field.cpp
  ../fused/fused_listener_room.cpp
  ../fused/fused_transducer.cpp
  ../fused/fused_tonal.cpp
  ../fused/fused_dynamics.cpp
  ../fused/fused_brain.cpp
  ../EvolutionaryEQ.cpp
  ../Psychoacoustics.cpp
  ../IvannaAudioClassifier.cpp
  ../IvannaVoiceProsodyEngine.cpp
  ../IvannaTinyML.cpp
  ../IvannaTinyMLKernel.cpp
  ../IvannaSelfHealingEngine.cpp
  ../IvannaSuperAgentMemory.cpp
  ../phase_oracle.cpp
  ../spatial/IntelligentUpmixer.cpp
  ../spatial/HoaBinauralDecoder.cpp
  ../spatial/hrtf_convolver.cpp
  ../SafHRTFDatasetBridge.cpp
  ../SafPcaDecoder.cpp
  ../HRTFBinLoader.cpp
  ../HrtfManager.cpp
  ../SofaHRTFLoader.cpp  # v2.5.472: HrtfManager::loadSofaAtPose/loadSofaDataset llaman SofaHRTFLoader::hrtfAt/load.
  # LGN + Ley de Curvatura (2026-11-18): HrtfManager::synthesizeHrtf usa
  # geodesic_pinna_model (+ physical_head_model) y physical_propagation
  # (curva ITD cerrada; la inversión de distancia NO se usa).
  ../fused/geodesic_pinna_model.cpp
  ../fused/physical_head_model.cpp
  ../fused/physical_propagation.cpp
  ../hexagon/ivanna_fastrpc_client.cpp
  ../hexagon/ivanna_dsp.cpp
  ../spatial/RirConvolver.cpp
  ../spatial/WfsRenderer.cpp
  ../spatial/OrbeBinaural.cpp
  ../spatial/ivanna_head_tracker.cpp
  ../spatial/HolosOperator.cpp
  ../spatial/HolosVolterraKernel.cpp
)

add_executable(test_fused_chain
  test_fused_chain.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_fused_chain PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_fused_chain PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_chain PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_chain PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_chain PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_chain)

# ── P1 (2026-11-22): matriz adversarial bypass vs IVANNA vs cada operador ─
# El núcleo de medición es el del banco adversarial (tests/adversarial, C++17,
# sin dependencias); se compila su .cpp aquí y se añade su include. El banco
# sigue siendo proyecto CTest independiente (7 tests); esta matriz vive en la
# suite principal (10 TESTs, uno por caso) y la Puerta cuenta sus 10 entradas.
add_executable(test_p1_adversarial_matrix
  test_p1_adversarial_matrix.cpp
  ${FUSED_CHAIN_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tests/adversarial/src/adversarial_core.cpp
)
target_include_directories(test_p1_adversarial_matrix PRIVATE .. ../include ../neuromorphic ../dsp host_stubs ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tests/adversarial/src)
target_compile_options(test_p1_adversarial_matrix PRIVATE -Wall -Wextra)
target_link_libraries(test_p1_adversarial_matrix PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_p1_adversarial_matrix PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_p1_adversarial_matrix PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_p1_adversarial_matrix)

# ── P0 (2026-11-22): referencia congelada REFERENCE_LOCKED ───────────────
# WAV de referencia fijo (10 s, 6 segmentos) + configuración IVANNA fija,
# con niveles, latencia, true peak, THD+N, IMD, CPU y xruns congelados en
# p0_reference_values.hpp (generado por scripts/p0_freeze.sh). Los TESTs
# hacen GTEST_SKIP hasta que P0 se congele (kFrozen=true). 9 TESTs.
add_executable(test_p0_reference_lock
  test_p0_reference_lock.cpp
  ${FUSED_CHAIN_SOURCES}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tests/adversarial/src/adversarial_core.cpp
)
target_include_directories(test_p0_reference_lock PRIVATE .. ../include ../neuromorphic ../dsp host_stubs ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tests/adversarial/src)
target_compile_definitions(test_p0_reference_lock PRIVATE P0_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(test_p0_reference_lock PRIVATE -Wall -Wextra)
target_link_libraries(test_p0_reference_lock PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_p0_reference_lock PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_p0_reference_lock PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_p0_reference_lock)

# ── Determinismo de NEURO·Ω (hallazgo mayor de P0, 2026-11-22) ─────────────
# op_neuro con dos cadenas frescas debe ser bit-idéntico; la causa raíz es
# el hilo de inferencia del clasificador -> guardián AntiDolby (ver
# test_neuro_determinism.cpp). Guarda + sonda del mecanismo.
add_executable(test_neuro_determinism
  test_neuro_determinism.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_neuro_determinism PRIVATE .. ../include ../neuromorphic ../dsp host_stubs ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tests/adversarial/src)
target_compile_options(test_neuro_determinism PRIVATE -Wall -Wextra)
target_link_libraries(test_neuro_determinism PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_neuro_determinism PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_neuro_determinism PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_neuro_determinism)
# ── Caza de los tronidos de la 493 (2026-11-22) ────────────────────────────
# Salto muestra a muestra por capa al cambiar parámetros en mitad del stream.
# Mismo patrón que la matriz P1 (FusedChain real + ${FUSED_CHAIN_SOURCES}).
add_executable(test_tronidos_layer_step
  test_tronidos_layer_step.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_tronidos_layer_step PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_tronidos_layer_step PRIVATE -Wall -Wextra)
target_link_libraries(test_tronidos_layer_step PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_tronidos_layer_step PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_tronidos_layer_step PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_tronidos_layer_step)

# ── S12/510 — fundido de aplicación por hop (slew) ───────────────────────
# setControlsRamped(): la aplicación de un perfil interpola EQ/trim/
# compresor/operadores en N hops de 128 muestras (≤0.5 dB por hop en
# AUTO); longitud 0 = comportamiento anterior (sin fundido).
add_executable(test_param_ramp_s12
  test_param_ramp_s12.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_param_ramp_s12 PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_param_ramp_s12 PRIVATE -Wall -Wextra)
target_link_libraries(test_param_ramp_s12 PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_param_ramp_s12 PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_param_ramp_s12 PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_param_ramp_s12)

# ── Piso double de la cadena enganchada (labor grande, v2.5.486) ─────────
# THD+N ≤ −200 dB con la cadena enganchada pero lineal a 1 kHz (ver
# test_double_floor.cpp) + coste por hop (128 muestras = 2667 µs).
add_executable(test_double_floor
  test_double_floor.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_double_floor PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_double_floor PRIVATE -Wall -Wextra)
target_link_libraries(test_double_floor PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_double_floor PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_double_floor PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_double_floor)

# ── HARNESS ACÚSTICO OFFLINE (2026-11-27, plan ajustado) ───────────────────
# Laboratorio offline: reproduce con números la degradación al subir HOLOS +
# hiperoperador (field.amount 1.0 / phi 1.0 / harmonics 0.002) sobre 5
# estímulos (tono 1 kHz, barrido log 20 Hz–20 kHz, ruido rosa, impulso,
# música sintética). Mide clipping, True Peak BS.1770, THD+N, tono
# emergente, crest factor, balance L/R, fase intercanal y CPU por hop
# (128 muestras = 2667 µs). Umbrales en el .cpp, justificados con líneas
# base medidas del proyecto. Registro con el patrón de test_double_floor
# (add_executable + ${FUSED_CHAIN_SOURCES}): ivanna_add_test NO vale aquí
# porque FusedChain no está en ivanna_dsp_under_test.
add_executable(test_acoustic_harness
  test_acoustic_harness.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_acoustic_harness PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_acoustic_harness PRIVATE -Wall -Wextra)
target_link_libraries(test_acoustic_harness PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_acoustic_harness PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_acoustic_harness PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_acoustic_harness)

# C11/510 — la puerta CPU por hop (AcousticHarness.CpuPerHopWithinBudget) NO
# lleva etiqueta QUARANTINED aquí a propósito: gtest_discover_tests con
# PROPERTIES etiquetaría los 5 casos del binario y sacaría de la puerta por
# push 4 tests deterministas (NeutralIsBitExact, FieldOnlyMax,
# LiveMaxFullChain, ErbSignaturePredictsRms — incl. la pre-pantalla S7).
# Su cuarentena es el split métrica-vs-puerta dentro del .cpp: en hosted es
# métrica registrada (se publica, no falla) y puerta dura solo con
# IVANNA_CPU_GATE_HARD=1 (self-hosted, carga conocida). El carril semanal
# la corre por nombre (-R AcousticHarness.CpuPerHopWithinBudget).

# ── CORONA D10: 5 tests (2026-11-25) ──────────────────────────────────────
# TΩ-2 LTI (bandas laterales ±375 Hz), neutro bit-exacto con D10 ON,
# Matched-LUFS con los 7 operadores, protección térmica de bobina y
# convergencia monótona de la acción hamiltoniana.
# Registro con el patrón de test_double_floor (add_executable +
# ${FUSED_CHAIN_SOURCES}): ivanna_add_test NO vale aquí porque
# FusedChain no está en ivanna_dsp_under_test (solo en
# ${FUSED_CHAIN_SOURCES}); un ivanna_add_test pelado fallaría al enlazar.
add_executable(test_d10_corona
  test_d10_corona.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_d10_corona PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_d10_corona PRIVATE -Wall -Wextra)
target_link_libraries(test_d10_corona PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_d10_corona PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_d10_corona PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_d10_corona)

# ── REFINAMIENTO TOTAL MAGISTRAL P1+P4 (2026-11-26) ───────────────────────
# Ablación por operador + Regla de Oro por banda ERB (test_p1p4_ablation),
# descomposición del −15.570 dB de NEURO + lazo F (test_p1p4_neuro) y
# verificación HRTF con telemetría de camino (test_p1p4_hrtf).
# Mismo patrón que test_d10_corona (FusedChain solo vive en
# ${FUSED_CHAIN_SOURCES}); test_p1p4_neuro es header-only (FreeEnergy /
# NewPhysicsEngine / NpeStereoCore) y usa ivanna_add_test; test_p1p4_hrtf
# enlaza la pipeline unificada (SofaHRTFLoader.cpp, como
# test_sofa_hrtf_loader).
add_executable(test_p1p4_ablation
  test_p1p4_ablation.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_p1p4_ablation PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_p1p4_ablation PRIVATE -Wall -Wextra)
target_link_libraries(test_p1p4_ablation PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_p1p4_ablation PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_p1p4_ablation PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_p1p4_ablation)

# ── MEJORA EVOLUTIVA, bloque GENEZI + NOTCH + 11.5 kHz (2026-11-26) ──────
# Carga real del asset .ivk1 Genezi en el motor fusionado + diferencia
# medible de salida + reemplazo en caliente sin clic; atribución del
# notch de enganche −15.8 dB a 977 Hz y de la interacción −25 dB a
# 11.5 kHz. Mismo patrón que test_p1p4_ablation (FusedChain solo vive
# en ${FUSED_CHAIN_SOURCES}).
add_executable(test_evol_genezi
  test_evol_genezi.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_evol_genezi PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_evol_genezi PRIVATE -Wall -Wextra)
target_link_libraries(test_evol_genezi PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_evol_genezi PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_evol_genezi PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_evol_genezi)

# test_p1p4_neuro: explícito (no ivanna_add_test) porque phase_oracle.cpp
# necesita el stub JNI de host (host_stubs/jni.h) y BiquadEnvelopeBank
# (dentro de NpeStereoCore) llama phase_oracle_tick — phase_oracle.cpp
# no está en ivanna_dsp_under_test (mismo patrón que
# test_hyper_state_operator / test_npe_core).
add_executable(test_p1p4_neuro
  test_p1p4_neuro.cpp
  ../phase_oracle.cpp
)
target_include_directories(test_p1p4_neuro PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_p1p4_neuro PRIVATE -Wall -Wextra)
target_link_libraries(test_p1p4_neuro PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_p1p4_neuro PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_p1p4_neuro PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_p1p4_neuro)

add_executable(test_p1p4_hrtf test_p1p4_hrtf.cpp)
target_link_libraries(test_p1p4_hrtf PRIVATE ivanna_unified_pipeline_under_test GTest::gtest_main Threads::Threads m dl)
target_compile_options(test_p1p4_hrtf PRIVATE -Wall -Wextra)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_p1p4_hrtf PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_p1p4_hrtf PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_p1p4_hrtf)

# ── Calibración P8 medida (P3, 3.4, 2026-11-26) ─────────────────────────────
# Bloquea la tabla medida operador x banda x canal x estímulo x nivel y
# documenta que la brecha nominal-real es estructural (dependiente de
# señal), no cerrable con un trim estático. kNominalGainDb NO se toca.
add_executable(test_p8_calibration
  test_p8_calibration.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_p8_calibration PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_p8_calibration PRIVATE -Wall -Wextra)
target_link_libraries(test_p8_calibration PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_p8_calibration PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_p8_calibration PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_p8_calibration)

# ── Capa de conservación perceptual (árbitro de campo, 2026-11-21) ─────────
# Solver (nunca crea energía, techo, silencio) + capa standalone (bypass
# bit-exacto, límite de energía, slew) + integración en FusedChain
# (neutro sigue bit-exacto, la conservación limita en la cadena).
add_executable(test_perceptual_conservation
  test_perceptual_conservation.cpp
  ../fused/acoustic_twin/acoustic_twin.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_perceptual_conservation PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_perceptual_conservation PRIVATE -Wall -Wextra)
target_link_libraries(test_perceptual_conservation PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_perceptual_conservation PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_perceptual_conservation PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_perceptual_conservation)

add_executable(test_fused_dual_route
  test_fused_dual_route.cpp
  ${FUSED_CHAIN_SOURCES}
)
target_include_directories(test_fused_dual_route PRIVATE .. ../include ../neuromorphic ../dsp host_stubs)
target_compile_options(test_fused_dual_route PRIVATE -Wall -Wextra)
target_link_libraries(test_fused_dual_route PRIVATE GTest::gtest_main ivanna_dsp_under_test Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_fused_dual_route PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_fused_dual_route PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_fused_dual_route)

# ── MPC perceptual en modo sombra (próximo salto magistral, 2026-11-21) ─────
# Controlador predictivo (vector P[t] por banda, simulador lineal de los 7
# operadores, MPC sombra con límites duros). Módulo nuevo e independiente:
# no toca FusedChain, JNI, UI ni daemon. Solo decide; nunca modifica audio.
add_executable(test_perceptual_mpc
  test_perceptual_mpc.cpp
  ../fused/perceptual_mpc/operator_simulator.cpp
  ../fused/perceptual_mpc/mpc_controller.cpp
  ../fused/acoustic_twin/acoustic_twin.cpp
)
target_include_directories(test_perceptual_mpc PRIVATE .. ../include ../fused/acoustic_twin)
target_compile_options(test_perceptual_mpc PRIVATE -Wall -Wextra)
target_link_libraries(test_perceptual_mpc PRIVATE GTest::gtest_main Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_perceptual_mpc PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_perceptual_mpc PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_perceptual_mpc)

# ── M5 — Simulador de acciones (salto, Luna) ─────────────────────────────────
# Función de costo J + restricciones duras sobre OperatorSimulator (que
# entra en ivanna_dsp_under_test, arriba). Pesos NOMINALES (calibrar con
# ABX, Orden 6); los tests verifican reglas y orden, no valores medidos.
ivanna_add_test(test_action_simulator)

# ── Banco adversarial del control cerrado (plan del salto, Luna) ─────────────
# Casos adversariales contra el sistema cerrado (M0/M3/M5/M6/M8/telemetría)
# + contra el código existente que el lazo usa (gemelo, conservación, MPC
# en sombra). Los .cpp de ../fused/closed_loop/ los aportan los módulos
# hermanos al integrar en salto/control-cerrado; se listan por ejecutable,
# como hace test_perceptual_mpc, SIN añadirlos a ivanna_dsp_under_test
# (eso duplicaría símbolos en los ejecutables que ya los compilan:
# test_fused_chain, test_perceptual_conservation y test_perceptual_mpc
# listan acoustic_twin.cpp / perceptual_conservation.cpp /
# mpc_controller.cpp en sus propias fuentes).
add_executable(test_closed_loop_adversarial
  test_closed_loop_adversarial.cpp
  ../fused/closed_loop/audio_state.cpp
  ../fused/closed_loop/twin_estimator.cpp
  ../fused/closed_loop/action_simulator.cpp
  ../fused/closed_loop/mpc_shadow.cpp
  ../fused/closed_loop/uncertainty_supervisor.cpp
  ../fused/closed_loop/telemetry.cpp
  ../fused/perceptual_mpc/operator_simulator.cpp
  ../fused/perceptual_mpc/mpc_controller.cpp
  ../fused/acoustic_twin/acoustic_twin.cpp
  ../fused/perceptual_conservation.cpp
)
target_include_directories(test_closed_loop_adversarial PRIVATE .. ../include ../fused/acoustic_twin)
target_compile_options(test_closed_loop_adversarial PRIVATE -Wall -Wextra)
target_link_libraries(test_closed_loop_adversarial PRIVATE GTest::gtest_main Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_closed_loop_adversarial PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_closed_loop_adversarial PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_closed_loop_adversarial)

# ── Gemelo acústico del trayecto completo (próximo salto, 2026-11-21) ───────
# Estimador del trayecto fuente→DSP→DAC→amp→transductor→sala→oído. Solo
# estima, nunca procesa audio. Standalone (propio main(), sin gtest), mismo
# patrón que test_route_b_neutral_snapshot. La línea exacta que pedía el
# CMakeLists del módulo (enganche a la suite principal, cierre 2026-11-21).
add_executable(test_acoustic_twin
  ../fused/acoustic_twin/acoustic_twin_test.cpp
  ../fused/acoustic_twin/acoustic_twin.cpp
)
target_include_directories(test_acoustic_twin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/acoustic_twin
)
target_compile_options(test_acoustic_twin PRIVATE -Wall -Wextra)
target_link_libraries(test_acoustic_twin PRIVATE Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_acoustic_twin PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_acoustic_twin PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_acoustic_twin COMMAND test_acoustic_twin)

# ── M3: estimador del gemelo (confianza por variable, plan del salto) ──
# TwinEstimator llena TwinConfidence desde AcousticTwinEstimate:
# calibración estimada 0.70 -> solo limita la conservación (nunca
# control fino); valores medidos inyectados -> kMeasured y control fino
# solo con confianza alta en TODAS las variables. GoogleTest, mismo
# patrón que test_perceptual_mpc (ejecutable explícito con sus .cpp;
# acoustic_twin.cpp NO va a ivanna_dsp_under_test porque los targets
# de la FusedChain ya lo compilan directo y duplicaría símbolos).
add_executable(test_twin_estimator
  test_twin_estimator.cpp
  ../fused/closed_loop/twin_estimator.cpp
  ../fused/acoustic_twin/acoustic_twin.cpp
)
target_include_directories(test_twin_estimator PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/acoustic_twin
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/closed_loop
)
target_compile_options(test_twin_estimator PRIVATE -Wall -Wextra)
target_link_libraries(test_twin_estimator PRIVATE GTest::gtest_main Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_twin_estimator PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_twin_estimator PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_twin_estimator)

# ── Modelo Universal de Audición P6 (Majestuosidad Total, 2026-11-21) ────
# Predictor de la experiencia completa P(experiencia | x, H, R, L, C).
# Solo predice, nunca procesa audio. Standalone (propio main(), sin
# gtest), mismo patrón que test_acoustic_twin.
add_executable(test_universal_hearing
  ../fused/universal_hearing/universal_hearing_test.cpp
  ../fused/universal_hearing/universal_hearing.cpp
)
target_include_directories(test_universal_hearing PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/universal_hearing
)
target_compile_options(test_universal_hearing PRIVATE -Wall -Wextra)
# WORKAROUND (2026-11-21, v2.5.491): GCC 14.3.0 (el de la CI) sufre un ICE
# (internal compiler error) en fold_relational_const durante el pase dom
# (jump threading) al compilar universal_hearing_test.cpp en -O3 (Release).
# El código es C++ válido: g++-14 14.2.0 local lo compila bien en -O3 y
# -O2, y el standalone pasa 119/119. Compilar solo el archivo del test en
# -O0 evita el pase que se rompe; el módulo (universal_hearing.cpp) se
# queda en -O3. El test es de corrección, no de rendimiento: -O0 no cuesta.
set_source_files_properties(../fused/universal_hearing/universal_hearing_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")
target_link_libraries(test_universal_hearing PRIVATE Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_universal_hearing PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_universal_hearing PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_universal_hearing COMMAND test_universal_hearing)

# ── CORONA P7–P10 (cableado 2026-11-24, rama corona-cableado) ─────────────
# Los 4 módulos entran a la suite principal con su propio main()
# (standalone, sin gtest), mismo patrón que test_acoustic_twin /
# test_universal_hearing. Cada uno es 1 entrada CTest (los checks
# internos los cuenta el propio test: P7 49, P8 33, P9 33, P10 38).
# El test del módulo en -O0 es el workaround P6 (v2.5.491): GCC 14.3.0
# (el de la CI) sufre un ICE en -O3 en estos tests; el módulo .cpp se
# queda en el nivel por defecto. P7/P8/P9 además se compilan en los .so
# de la app y en FUSED_CHAIN_SOURCES (FusedChain los usa); P10 NO: es
# banco de pruebas sin call site de audio, y su ÚNICO cableado es este
# registro en CTest (ver fused_chain.hpp: P10 no tiene miembro).
add_executable(test_total_conservation
  ../fused/total_conservation/total_conservation_test.cpp
  ../fused/total_conservation/total_conservation.cpp
)
target_include_directories(test_total_conservation PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/total_conservation
)
target_compile_options(test_total_conservation PRIVATE -Wall -Wextra)
set_source_files_properties(../fused/total_conservation/total_conservation_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")
target_link_libraries(test_total_conservation PRIVATE Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_total_conservation PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_total_conservation PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_total_conservation COMMAND test_total_conservation)

add_executable(test_experience_compiler
  ../fused/experience_compiler/experience_compiler_test.cpp
  ../fused/experience_compiler/experience_compiler.cpp
)
target_include_directories(test_experience_compiler PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/experience_compiler
)
target_compile_options(test_experience_compiler PRIVATE -Wall -Wextra)
set_source_files_properties(../fused/experience_compiler/experience_compiler_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")
target_link_libraries(test_experience_compiler PRIVATE Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_experience_compiler PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_experience_compiler PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_experience_compiler COMMAND test_experience_compiler)

add_executable(test_living_room
  ../fused/living_room/living_room_test.cpp
  ../fused/living_room/living_room.cpp
)
target_include_directories(test_living_room PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/living_room
)
target_compile_options(test_living_room PRIVATE -Wall -Wextra)
set_source_files_properties(../fused/living_room/living_room_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")
target_link_libraries(test_living_room PRIVATE Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_living_room PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_living_room PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_living_room COMMAND test_living_room)

add_executable(test_human_twin
  ../fused/human_twin/human_twin_test.cpp
  ../fused/human_twin/human_twin.cpp
)
target_include_directories(test_human_twin PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../fused/human_twin
)
target_compile_options(test_human_twin PRIVATE -Wall -Wextra)
set_source_files_properties(../fused/human_twin/human_twin_test.cpp
  PROPERTIES COMPILE_OPTIONS "-O0")
target_link_libraries(test_human_twin PRIVATE Threads::Threads m)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_human_twin PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_human_twin PRIVATE ${IVANNA_SAN_FLAGS})
endif()
add_test(NAME test_human_twin COMMAND test_human_twin)

# ── Telemetría del control cerrado (plan del salto, Luna 3.14) ───────────
# Solo registra, nunca decide: frames a 10 Hz + fallos inmediatos, anillo
# lock-free con sobrescritura del más viejo. 9 tests: round-trip, FIFO,
# fallo inmediato, NaN -> valid=false, buffer lleno conserva el reciente,
# dos productores concurrentes.
ivanna_add_test(test_telemetry)

# ── MPC perceptual en sombra (Luna M6, plan del salto, 2026-11-21) ───────────
# MpcShadow sobre el MpcController + OperatorSimulator existentes: horizonte
# de 5 decisiones a 50 ms (250 ms), argmax de la suma de J, guardas duras
# (gemelo < 0.20 / NaN / clipping / divergencia -> cero), rollback tras 2
# contradicciones seguidas, y crossfade equal-power 20–50 ms (solo se
# calcula; la sombra nunca aplica nada). Mismo patrón que test_perceptual_mpc
# (enlaza los .cpp reales, no fakes).
add_executable(test_mpc_shadow
  test_mpc_shadow.cpp
  ../fused/closed_loop/mpc_shadow.cpp
  ../fused/perceptual_mpc/operator_simulator.cpp
  ../fused/perceptual_mpc/mpc_controller.cpp
)
target_include_directories(test_mpc_shadow PRIVATE .. ../include ../fused/acoustic_twin)
target_compile_options(test_mpc_shadow PRIVATE -Wall -Wextra)
target_link_libraries(test_mpc_shadow PRIVATE GTest::gtest_main Threads::Threads)
if(IVANNA_SAN_FLAGS)
  target_compile_options(test_mpc_shadow PRIVATE ${IVANNA_SAN_FLAGS})
  target_link_options(test_mpc_shadow PRIVATE ${IVANNA_SAN_FLAGS})
endif()
gtest_discover_tests(test_mpc_shadow)

# ── FASE 3 — MODELO ACOPLADO NO LINEAL (D10) ─────────────────────────────
# 17 tests (test_coupled_model): modos de sala (35 exactos), Gamma_k
# corregido, Westervelt (-56.6 dB F2 / -53.6 dB Beyer-pico, divergencia
# registrada), P-A (Delta phi 2.71°; Delta f_s modal 0.017 Hz — F2 decía
# 0.2-0.8 Hz y la suma exacta NO lo reproduce), P-C, P-B2 (tau_1/2),
# balance energético por hop <= 1e-6, integrador exponencial exacto vs
# RK4 (fase exacta [D7]), aliasing Duffing (DC a 48 kHz, limpio a 192 kHz),
# tensor C_ijk disperso (1614 tripletes), J-A (3.28%), presupuesto RT
# (us/hop), modos RIR (rir_0000), Sabine/Z_w desde RIR, y lectura
# KEMAR dos oídos + 2.1 (mismo estado, varios receptores).
# Header-only: ivanna_add_test vale (sin FUSED_CHAIN_SOURCES).
ivanna_add_test(test_coupled_model)
