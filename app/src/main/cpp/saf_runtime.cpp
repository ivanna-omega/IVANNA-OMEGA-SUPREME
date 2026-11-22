#include "saf_runtime.h"
#include "saf_feedback.h"
#include "saf_socket_update.h"

SAFState g_saf_state;
SAFMetrics g_saf_metrics;


// ==========================================================
// SAF MODEL LOADER
// ==========================================================

#include <fstream>
#include <string>
#include "SafModelLoader.hpp"


static bool g_saf_model_loaded=false;


bool SAF_LoadModel()
{

    const char* path =
    "/data/adb/ivanna_omega/SAF_model.json";  // FIX 2026-08-09: unificar con customize.sh y SaFEngine.kt (path canónico app)


    Ivanna::SafModelLoader loader;


    if(!loader.load(path))
    {
        return false;
    }


    g_saf_model_loaded=true;


    return true;

}


// LEGACY (2026-11-16, auditoría 0012): SAF_IsModelLoaded no tiene
// llamadores — el estado del modelo SAF ya se reporta en Kotlin vía
// safAssetAvailable (FotoHoloTelemetryBridge) y el motor degrada sin
// modelo. Se conserva (nunca borrar código) por si un diagnóstico en
// vivo lo necesita; el scanner de funciones muertas lo excluye
// (DEAD_FUNCTION_ALLOWLIST en tools/evolution/evolution_engine.py).
bool SAF_IsModelLoaded()
{

    return g_saf_model_loaded;

}


