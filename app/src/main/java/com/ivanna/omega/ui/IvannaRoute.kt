package com.ivanna.omega.ui

/**
 * IvannaRoute — centraliza las rutas usadas por la navegación para evitar
 * literales esparcidas que provocan errores y dificultan refactors.
 *
 * Nota (Fase 2, 2026-11-22): las rutas sin destino (splash, intro, sound,
 * ope, binaural, perceptual_brain, perceptual_cortex, magistral,
 * adaptive_dash, adaptive_profiles, space, auditory, system, telemetry y
 * sus alias) se borraron junto con las 9 pantallas huérfanas: ninguna
 * estaba montada en el NavHost ni se navegaba a ella. Solo quedan las
 * rutas vivas (montadas o navegadas) y TIDAL, que se conserva como
 * const legacy documentada aunque su pantalla ya no existe.
 */
object IvannaRoute {

    const val DASHBOARD = "dashboard"

    // Cerebro / Perceptual
    const val ADAPTIVE = "adaptive"
    // FUSIÓN MAGISTRAL (7 operadores): pantalla propia (FusedOperatorsScreen).
    // FIX (pantalla huérfana, 2026-11-18): existía pero sin ruta ni destino:
    // nadie la abría. Ruta propia, distinta de MAGISTRAL (dashboard).
    const val FUSED_OPERATORS = "fused_operators"
    const val LAB = "lab"

    // Espacio / Auditory
    const val VISUALIZER = "visualizer"

    // Sistema
    const val MAGISK = "magisk"
    const val PROFILES = "profiles"
    // VISTA DE DIOS · FLOTA (v2.5.471 Repair 4): ciclos reales del consejo
    // leídos del puente (FleetViewScreen). Sin ciclos muestra la verdad.
    const val FLEET_VIEW = "fleet_view"

    // Legacy / aliases
    const val ABX_TEST = "abx_test"
    const val AUDIO_CONTROL_HUB = "audio_control_hub"  // SOFA · AF · RIR · SAF panel

    // IVANNA Conversational Acoustic Intelligence (FASE 12–19)
    const val IVANNA_ASSISTANT = "ivanna_assistant"

    // Ψ (v2.5.389): medición de cabeza por foto + tarjeta de referencia
    // (HeadMeasureScreen). Alimenta el radio personalizado de ORBE.
    const val HEAD_MEASURE = "head_measure"

    // Panel de conectividad WiFi / datos / Gemini Agent
    const val NETWORK = "network_status"

    // Colección de los 30 mejores conciertos de rock (v2.5.372)
    const val ROCK_CONCERTS = "rock_concerts"

    // TIDAL dentro de IVANNA (v2.5.416): reproductor propio, sin redirecciones.
    const val TIDAL = "tidal"

    // ── Pantallas nuevas magistrales (Fase 2, §3 Fase 4 del inventario) ───
    // Escucha magistral · Física Nueva por secciones · Mi sala · Mi equipo ·
    // Salud del sistema · Aprende. Todas montadas en el NavHost y con tarjeta
    // visible en SYSTEM (sin Modo desarrollador: son escucha diaria, no
    // diagnóstico interno).
    // Música local como ruta externa: la pestaña MÚSICA vive dentro del
    // NavHost interno de MainScaffold (sin deep-link desde fuera); esta ruta
    // permite abrirla desde Escucha magistral y demás pantallas nuevas.
    const val MUSICA_LOCAL = "musica_local"
    const val ESCUCHA_MAGISTRAL = "escucha_magistral"
    const val FISICA_NUEVA      = "fisica_nueva"
    const val MI_SALA           = "mi_sala"
    const val MI_EQUIPO         = "mi_equipo"
    const val SALUD_SISTEMA     = "salud_sistema"
    const val APRENDE           = "aprende"

    // ── 5 Ejes de Supremacía Cuántico-Neuromórfica (Prompt Maestro 2026) ─────
    const val SUPREME_AXES_HUB       = "supreme_axes_hub"
    const val SUPREME_AXIS_1_LATTICE = "supreme_axis_1_lattice"
    const val SUPREME_AXIS_2_CVNN    = "supreme_axis_2_cvnn"
    const val SUPREME_AXIS_3_SNN_HOA = "supreme_axis_3_snn_hoa"
    const val SUPREME_AXIS_4_PINNA_INR  = "supreme_axis_4_pinna_inr"
    const val SUPREME_AXIS_5_SHM_FARROW = "supreme_axis_5_shm_farrow"
}
