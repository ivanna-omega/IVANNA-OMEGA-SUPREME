// perceptual_mpc.hpp — Umbrella del módulo MPC perceptual (modo sombra).
//
// Incluye el vector perceptual, el simulador de operadores y el controlador
// MPC. Módulo nuevo e independiente: solo decide; nunca toca audio,
// FusedChain, JNI, UI ni daemon.

#pragma once

#include "mpc_controller.hpp"
#include "operator_simulator.hpp"
#include "perceptual_vector.hpp"
