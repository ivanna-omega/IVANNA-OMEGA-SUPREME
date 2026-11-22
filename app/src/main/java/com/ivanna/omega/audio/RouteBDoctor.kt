/*
 * © 2026 Luis Uriel Pimentel Pérez — IVANNA N-P-E
 * RouteBDoctor.kt — Doctor de auto-reparación de Ruta B (runtime-only).
 *
 * v2.5.443 — Orden directa de Esse (2026-11-14 12:56 CST): IVANNA enlazada a
 * OmniRoute y con auto-reparación desde el código núcleo. El doctor LOCALIZA
 * por qué la Ruta B no procesa y LO REPARA automáticamente, en runtime, con
 * whitelist y fail-closed. Si no puede diagnosticar o reparar con seguridad,
 * REPORTA y no fuerza nada.
 *
 * El problema (verificado en vivo 2026-11-13): el efecto omega SE CREA y SE
 * ACOPLA a la sesión descubierta (AudioEffect existe, sesión por daemon),
 * pero procesa 0 frames — fusion_active=0, psi_active=0, ratio 1.0, sin
 * asrc_frames. Las puertas están en omega_process() (omega_effect.cpp) y
 * cada salida temprana registra su motivo bajo el tag IvannaOmegaEffect
 * (throttle 5 s por motivo, desde v2.5.381). El doctor lee ese log + el
 * beacon + el heartbeat de fusión + GET_STATUS y NOMBRA la puerta exacta.
 *
 * PUERTAS (omega_effect.cpp, omega_process):
 *   NOT_ENABLED_NATIVE — "!enabled && bypass silencioso" (EFFECT_CMD_ENABLE
 *     jamás llegó del HAL y el auto-enable no se disparó).
 *   NOT_STEREO — "canales != 2 (no estereo)" (mono o 5.1/7.1; el DSP solo
 *     asume estéreo — es comportamiento CORRECTO, no un bug).
 *   INTEGRITY_FAIL — "integrity_self_check FALLO" (.so cargada desde una
 *     ruta no legítima).
 *   ANTITAMPER_LOCKDOWN — "antitamper LOCKDOWN" (debugger/hook/emulador en
 *     audioserver; root por sí solo NO dispara).
 *   FUSION_CORE_NULL — "fusionCore NULO" / "FusionEngine lazy FALLO"
 *     (la creación lazy del FusionEngine falló; un solo intento por
 *     instancia del efecto).
 *   BYPASS_BEGIN_BLOCK_FAIL — "bypass beginBlock fallo" (la envolvente
 *     de bypass no arranca el bloque; fallo de la instancia).
 *   RT_BUFFERS_NULL — "buffers RT nulos" (SET_CONFIG jamás llegó del HAL
 *     o el calloc de los buffers falló en la creación).
 *   SNAPSHOT_NEUTRAL — el beacon dice PROCESSING pero el snapshot es
 *     neutro: el daemon nunca recibió el estado de la app
 *     (perceptual_generation==0, ORBE 0 / HOLOS 0). La salida es ≈ la
 *     entrada aunque el efecto procese bloques. No hay reparación runtime:
 *     hay que abrir la app para que publique su estado.
 *   BEACON_FILE_MISSING — el archivo /data/adb/ivanna_omega/omega_effect_beacon
 *     no existe: audioserver (UID no-root) NO PUEDE crearlo solo y el daemon
 *     nunca lo pre-creó (precreateLocalFile existe y está testeado, pero
 *     NADIE lo llama — hallazgo de esta release). Sin archivo no hay señal
 *     de procesado. (0043: antes vivía en /data/local/tmp, donde el kernel
 *     lo veía como `unlabeled` pese a chcon.)
 *   BEACON_NEVER_EXISTED — el archivo existe pero el efecto nunca escribió
 *     en él mientras suena (punto ciego del detector viejo: beacon < 0 se
 *     trataba como "fail-closed, conservar estado" y NUNCA se recreaba).
 *   ORPHAN_FROZEN — frames congelados N ciclos mientras suena (el efecto
 *     está en "Orphan Effect Chains": existe sin hilo de salida).
 *
 * REPARACIONES (whitelist, SOLO runtime — nunca reescribe código fuente):
 *   PRECREATE_BEACON — crear /data/adb/ivanna_omega/omega_effect_beacon 0666 vía
 *     root (touch + chmod), para que el efecto pueda abrirlo O_RDWR.
 *   RECREATE_ORPHAN — soltar el efecto huérfano y re-acoplarlo a la sesión
 *     descubierta (una instancia nueva reintenta la creación lazy del
 *     FusionEngine y el auto-enable). Resetea el detector de huérfanos.
 *   RETRY_ATTACH — reintentar el acople a la sesión descubierta (limpia el
 *     backoff de ese intento fallido y vuelve a intentar).
 *   RESET_ORPHAN_DETECTOR — resetear la línea base del detector (el beacon
 *     nunca existió y el estado quedó en "sin línea base" para siempre).
 *   REPORT_ONLY — no hay reparación segura: se reporta (log + telemetría)
 *     y no se fuerza nada. Aplica a INTEGRITY_FAIL, ANTITAMPER_LOCKDOWN,
 *     NOT_STEREO (correcto), DAEMON_UNREACHABLE y NO_SESSION_DISCOVERED.
 *
 * VERIFICACIÓN: tras una reparación, el doctor espera y re-lee el estado
 * (beacon frames creciendo / effectState PROCESSING / fusion_active=1). Si
 * verifica → éxito. Si tras N ciclos sigue sin procesar → la reparación
 * falló: se reporta y no se reintenta en bucle (cooldown 60 s, máx 3
 * intentos por sesión).
 *
 * LLM (advisory, SOLO datos de diagnóstico): cuando la puerta es UNKNOWN,
 * el doctor puede consultar al LLM activo (OmniRoute si está habilitado,
 * si no Gemini) con SOLO el snapshot de diagnóstico (logs, telemetría,
 * estado, códigos de error — NUNCA código fuente). La respuesta es un
 * consejo; el doctor solo ejecuta reparaciones de la whitelist. Sin LLM
 * (o si falla), el doctor funciona 100% determinista.
 */
package com.ivanna.omega.audio

import android.util.Log
import com.ivanna.omega.core.RootAccess
import com.ivanna.omega.dsp.DSPBridge
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import org.json.JSONObject
import java.io.File

// ── Puertas ──────────────────────────────────────────────────────────────────

/** Puerta exacta que corta el procesado de Ruta B (o el estado sano). */
enum class RouteBGate {
    /** El efecto procesa (beacon frames creciendo / effectState PROCESSING). */
    HEALTHY_PROCESSING,
    /**
     * El efecto procesa bloques pero con el snapshot NEUTRO: el daemon
     * nunca recibió el estado de la app (perceptualGeneration==0) y
     * publica makeDefault() (ORBE 0 / HOLOS 0). El beacon crece pero la
     * salida es ≈ la entrada. No es un fallo del efecto: es ausencia del
     * plano de control (la app no envió SET_PERCEPTUAL_STATE).
     */
    SNAPSHOT_NEUTRAL,
    /** GET_STATUS no responde: el daemon no está vivo o el socket está caído. */
    DAEMON_UNREACHABLE,
    /** Daemon vivo pero sin sesión descubierta (sin música o el descubrimiento falló). */
    NO_SESSION_DISCOVERED,
    /** Sesión descubierta pero el efecto no está acoplado a ella. */
    NOT_ATTACHED,
    /** Log: "!enabled && bypass silencioso" — no habilitado a nivel nativo. */
    NOT_ENABLED_NATIVE,
    /** Log: "canales != 2" — el stream no es estéreo (correcto, no es un bug). */
    NOT_STEREO,
    /** Log: "integrity_self_check FALLO" — .so en ruta no legítima. */
    INTEGRITY_FAIL,
    /** Log: "antitamper LOCKDOWN" — debugger/hook/emulador en audioserver. */
    ANTITAMPER_LOCKDOWN,
    /** Log: "fusionCore NULO" / "FusionEngine lazy FALLO" — motor no creado. */
    FUSION_CORE_NULL,
    /** Log: "bypass beginBlock fallo" — la envolvente de bypass no arranca. */
    BYPASS_BEGIN_BLOCK_FAIL,
    /** Log: "buffers RT nulos" — SET_CONFIG jamás llegó o calloc falló. */
    RT_BUFFERS_NULL,
    /** El archivo del beacon no existe (nunca pre-creado; audioserver no puede crearlo). */
    BEACON_FILE_MISSING,
    /** El beacon existe pero el efecto nunca escribió en él mientras suena. */
    BEACON_NEVER_EXISTED,
    /** El beacon es ciego por permiso/etiqueta SELinux (no por orfandad):
     *  la cadena de AudioFlinger no tiene el efecto habilitado y el archivo
     *  no es escribible por audioserver aunque exista. No seguir recreando. */
    BEACON_PERMISSION_OR_LABEL_FAIL,
    /** Frames congelados N ciclos mientras la música suena (huérfano). */
    ORPHAN_FROZEN,
    /** Efecto habilitado pero sin bloques recientes (pausa o hueco transitorio). */
    EFFECT_IDLE,
    /** No se puede determinar con la evidencia disponible. */
    UNKNOWN
}

// ── Evidencia ────────────────────────────────────────────────────────────────

/**
 * Snapshot del estado real de Ruta B en un ciclo del watchdog.
 * Todo es lectura (sin efectos secundarios): el doctor decide con esto.
 */
data class RouteBEvidence(
    /** Sesión a la que está acoplado omega (0 = ninguna). */
    val attachedSessionId: Int,
    /** Sesión descubierta por el daemon (0 = ninguna). */
    val discoveredSessionId: Int,
    /** GET_STATUS respondió. */
    val daemonReachable: Boolean,
    /** activeSessions[attached]?.omega != null. */
    val effectAttached: Boolean,
    /** Frames del beacon (-1 = beacon no disponible). */
    val beaconFrames: Long,
    /** Estado del beacon (NO_EFFECT / ENABLED_IDLE / PROCESSING / UNAVAILABLE). */
    val effectState: DSPBridge.EffectState,
    /** ¿Existe el archivo /data/adb/ivanna_omega/omega_effect_beacon? */
    val beaconFileExists: Boolean,
    /** fusion_active del heartbeat (null = sin heartbeat fresco). */
    val fusionActive: Boolean?,
    /** psi_active del heartbeat (null = sin heartbeat fresco). */
    val psiActive: Boolean?,
    /** ratio ASRC del heartbeat (null = sin heartbeat fresco). */
    val ratio: Float?,
    /** ¿Está sonando música ahora (AudioManager.isMusicActive)? */
    val musicPlaying: Boolean,
    /** Puertas vistas en el log IvannaOmegaEffect / OmegaCtrlBus. */
    val logGates: Set<RouteBGate>,
    /** Última clase de fallo al crear omega (ver classifyOmegaFailure). */
    val lastAttachFailure: OmegaFailureKind,
    /** Ciclos consecutivos con beacon -1 mientras está acoplado y suena. */
    val beaconNeverSeenCycles: Int,
    /** Ciclos consecutivos con frames congelados mientras suena. */
    val frozenCycles: Int,
    /** Generación del snapshot publicado por el daemon (GET_STATUS snapshot_generation; null = sin dato). */
    val snapshotGeneration: Long? = null,
    /** Nº de SET_PERCEPTUAL_STATE recibidos por el daemon (GET_STATUS perceptual_generation; null = sin dato; 0 = la app nunca envió su estado). */
    val perceptualGeneration: Long? = null,
    /** ORBE amount del snapshot publicado (GET_STATUS orbe_amount; null = sin dato). */
    val snapshotOrbeAmount: Float? = null,
    /** HOLOS amount del snapshot publicado (GET_STATUS holos_amount; null = sin dato). */
    val snapshotHolosAmount: Float? = null,
    /**
     * FIX 4 (v2.5.465) — frames del beacon CRECIENDO entre el ciclo anterior
     * y este (el llamador lo calcula con su línea base; el doctor también
     * lo calcula en runCycle con lastBeaconFrames). Es la FUENTE DE VERDAD
     * de "Ruta B procesa" en este HAL Qualcomm: EFFECT_CMD_ENABLE jamás
     * llega, enabled_count queda en 0 y effectState nunca es PROCESSING
     * aunque el efecto procese (vivo 2026-11-18: frames 0 → 5.046.272).
     * enabled_count / consumer_gen quedan como señales secundarias: nunca
     * condicionan el diagnóstico ni la telemetría. false = sin dato o sin
     * crecimiento probado (fail-closed: el primer ciclo no puede probarlo).
     */
    val beaconFramesGrowing: Boolean = false
)

// ── Diagnóstico y reparación ─────────────────────────────────────────────────

/** Diagnóstico del doctor: la puerta + explicación legible. */
data class RouteBDiagnosis(
    val gate: RouteBGate,
    /** Explicación en español: qué está pasando y por qué. */
    val explanation: String,
    /** Consejo del LLM (si se consultó), null si no. Solo advisory. */
    val llmAdvice: String? = null
)

/** Reparaciones permitidas (whitelist, SOLO runtime). */
enum class RouteBRepair {
    /** Sano o sin reparación segura — no hacer nada. */
    NONE,
    /** Crear el beacon 0666 vía root para que el efecto pueda escribirlo. */
    PRECREATE_BEACON,
    /** Soltar el efecto huérfano y re-acoplarlo a la sesión descubierta. */
    RECREATE_ORPHAN,
    /** Reintentar el acople (limpia el backoff de ese intento fallido). */
    RETRY_ATTACH,
    /** Resetear la línea base del detector de huérfanos. */
    RESET_ORPHAN_DETECTOR,
    /** Sin reparación segura: reportar y no forzar nada. */
    REPORT_ONLY
}

/** Resultado de un ciclo del doctor. */
data class RouteBDoctorOutcome(
    val diagnosis: RouteBDiagnosis,
    val repair: RouteBRepair,
    /** Una reparación anterior se verificó (el efecto ya procesa). */
    val repairVerified: Boolean,
    /** Una reparación anterior falló tras N ciclos sin procesar. */
    val repairFailed: Boolean
)

// ── Log: parseo puro ─────────────────────────────────────────────────────────

/** Subcadenas del log (tag IvannaOmegaEffect / OmegaCtrlBus) por puerta. */
internal val ROUTE_B_LOG_GATE_PATTERNS: List<Pair<String, RouteBGate>> = listOf(
    "canales != 2" to RouteBGate.NOT_STEREO,
    "integrity_self_check FALLO" to RouteBGate.INTEGRITY_FAIL,
    "antitamper LOCKDOWN" to RouteBGate.ANTITAMPER_LOCKDOWN,
    "fusionCore NULO" to RouteBGate.FUSION_CORE_NULL,
    "FusionEngine lazy FALLO" to RouteBGate.FUSION_CORE_NULL,
    "bypass beginBlock fallo" to RouteBGate.BYPASS_BEGIN_BLOCK_FAIL,
    "buffers RT nulos" to RouteBGate.RT_BUFFERS_NULL,
    "!enabled && bypass silencioso" to RouteBGate.NOT_ENABLED_NATIVE,
    "beacon open" to RouteBGate.BEACON_FILE_MISSING
)

/**
 * Extrae las puertas presentes en las líneas del log. Pura (testeable en host).
 * Una línea "beacon open(...) failed" solo cuenta como BEACON_FILE_MISSING si
 * dice failed (el open exitoso no loguea nada).
 */
internal fun parseOmegaEffectLog(lines: List<String>): Set<RouteBGate> {
    val found = mutableSetOf<RouteBGate>()
    for (line in lines) {
        for ((pattern, gate) in ROUTE_B_LOG_GATE_PATTERNS) {
            if (!line.contains(pattern)) continue
            if (gate == RouteBGate.BEACON_FILE_MISSING && !line.contains("failed")) continue
            found.add(gate)
        }
    }
    return found
}

/**
 * Parsea el estado Enabled de omega en un dumpsys de AudioFlinger. Pura
 * (testeable en host). Best-effort y fail-closed: devuelve null ante
 * cualquier duda (UUID ausente, sesión no visible en la ventana, o estado
 * no reconocible). Solo un "Suspended"/"Disabled" explícito junto al UUID
 * de omega y la sesión da false; solo un "Enabled" explícito da true.
 */
internal fun parseChainEnabledState(dumpsys: String, sessionId: Int): Boolean? {
    val uuid = "4956414e-4e41-4f4d-4547-415355505245"
    val idx = dumpsys.indexOf(uuid, ignoreCase = true)
    if (idx < 0) return null
    val lines = dumpsys.lines()
    val lineIdx = dumpsys.substring(0, idx).count { it == '\n' }
    val from = maxOf(0, lineIdx - 15)
    val to = minOf(lines.size, lineIdx + 16)
    val window = lines.subList(from, to).joinToString("\n")
    if (!window.contains(sessionId.toString())) return null
    val lower = window.lowercase()
    if ("suspended" in lower || "disabled" in lower) return false
    if ("enabled" in lower) return true
    return null
}

// ── Diagnóstico puro ─────────────────────────────────────────────────────────

/** Ciclos congelados para declarar huérfano (3 × 3 s = 9 s, igual que el detector viejo). */
internal const val ROUTE_B_FROZEN_CYCLES_TO_ORPHAN: Int = 3
/** Ciclos con beacon ausente para declarar que nunca existió. */
internal const val ROUTE_B_NEVER_SEEN_CYCLES_TO_DIAGNOSE: Int = 3

/**
 * Diagnóstico puro: nombra la puerta exacta con la evidencia disponible.
 * Orden de prioridad: lo que se puede afirmar con certeza primero; el log
 * manda sobre el beacon (el log dice POR QUÉ; el beacon solo dice SI procesa).
 * Pura para poder probarla en host.
 */
internal fun diagnoseRouteB(evidence: RouteBEvidence): RouteBDiagnosis {
    val log = evidence.logGates
    fun diag(gate: RouteBGate, explanation: String) = RouteBDiagnosis(gate, explanation)

    // FIX 4 (v2.5.465) — FUENTE DE VERDAD: frames del beacon avanzando =
    // Ruta B procesando. En este HAL Qualcomm EFFECT_CMD_ENABLE jamás llega,
    // enabled_count queda en 0 y effectState nunca es PROCESSING aunque el
    // efecto procese (vivo 2026-11-18: frames 0 → 5.046.272 con el banner
    // "Ruta B sin confirmar"). Un effectState PROCESSING también vale, pero
    // ya no es el ÚNICO camino: frames creciendo basta. enabled_count /
    // consumer_gen son secundarios y NUNCA condicionan este diagnóstico.
    val beaconProcessing = evidence.effectState == DSPBridge.EffectState.PROCESSING ||
        evidence.beaconFramesGrowing

    // Procesa bloques pero con el snapshot NEUTRO: el daemon nunca recibió
    // el estado de la app (perceptualGeneration==0) y publica makeDefault()
    // (ORBE 0 / HOLOS 0). El beacon crece pero la salida es ≈ la entrada:
    // "acoplado pero no procesa (efectivamente)". Va ANTES del sano: un
    // beacon PROCESSING con snapshot neutro NO es sano. null = sin dato
    // (el manager aún no parsea los 4 campos de GET_STATUS): entonces no
    // se dispara y el diagnóstico sigue como antes. Si los amounts vienen
    // > 0 con perceptualGeneration==0, el dato es contradictorio y manda
    // el sano (no se acusa un neutro que no existe).
    if (beaconProcessing &&
        evidence.perceptualGeneration != null && evidence.perceptualGeneration == 0L &&
        (evidence.snapshotOrbeAmount == null || evidence.snapshotOrbeAmount == 0f) &&
        (evidence.snapshotHolosAmount == null || evidence.snapshotHolosAmount == 0f)) {
        return diag(
            RouteBGate.SNAPSHOT_NEUTRAL,
            "Ruta B procesa bloques pero con el snapshot NEUTRO: el daemon nunca recibió el estado " +
                "de la app (perceptual_generation=0) y publica makeDefault() (ORBE 0 / HOLOS 0). " +
                "El beacon crece pero la salida es ≈ la entrada. No es un fallo del efecto: hay que " +
                "abrir la app para que publique su estado (SET_PERCEPTUAL_STATE) o revisar el socket " +
                "del daemon. Recrear el efecto no ayuda: la instancia nueva recibiría el mismo snapshot neutro."
        )
    }
    // Sano: el efecto procesa (el beacon lo dice directamente: estado
    // PROCESSING o frames creciendo entre ciclos — FIX 4).
    if (beaconProcessing) {
        return diag(
            RouteBGate.HEALTHY_PROCESSING,
            "Ruta B procesa: el beacon reporta bloques recientes" +
                (if (evidence.beaconFramesGrowing) " (frames creciendo: ${evidence.beaconFrames})" else "") +
                (if (evidence.fusionActive == true) " y fusion_active=1" else "") +
                (if (evidence.ratio != null) " (ratio=${evidence.ratio})" else "")
        )
    }
    // Sin daemon no hay descubrimiento ni sesión: nada que acoplar.
    if (!evidence.daemonReachable) {
        return diag(
            RouteBGate.DAEMON_UNREACHABLE,
            "El daemon no responde a GET_STATUS: sin daemon no hay descubrimiento de sesión ni Ruta B. " +
                "Revisar que el módulo esté activo y el daemon vivo; el doctor no reinicia el daemon."
        )
    }
    // Daemon vivo pero sin sesión: no hay música que acoplar (o el descubrimiento falló).
    if (!evidence.isUsableDiscoveredSession()) {
        return diag(
            RouteBGate.NO_SESSION_DISCOVERED,
            "El daemon vive pero no descubre ninguna sesión de música" +
                (if (evidence.musicPlaying) " aunque la música suena (el descubrimiento del daemon falló)" else " (sin música sonando)") +
                ". Sin sesión descubierta no se crea ningún efecto (fail-closed)."
        )
    }
    // Sesión descubierta pero el efecto no está acoplado.
    if (!evidence.effectAttached) {
        return diag(
            RouteBGate.NOT_ATTACHED,
            "Sesión ${evidence.discoveredSessionId} descubierta pero omega no está acoplado. " +
                "Último fallo de creación: ${evidence.lastAttachFailure}."
        )
    }
    // Acoplado: el log nombra la puerta exacta (manda sobre el beacon).
    if (RouteBGate.INTEGRITY_FAIL in log) {
        return diag(
            RouteBGate.INTEGRITY_FAIL,
            "omega_process pasa de largo: integrity_self_check FALLO — libomega_effect.so cargada desde una ruta no legítima. " +
                "No es reparable en runtime (es la instalación del módulo); se reporta."
        )
    }
    if (RouteBGate.ANTITAMPER_LOCKDOWN in log) {
        return diag(
            RouteBGate.ANTITAMPER_LOCKDOWN,
            "omega_process pasa de largo: antitamper LOCKDOWN — debugger, Frida, hook o emulador en audioserver. " +
                "No es reparable en runtime; se reporta."
        )
    }
    if (RouteBGate.NOT_STEREO in log) {
        return diag(
            RouteBGate.NOT_STEREO,
            "omega_process pasa de largo: el stream no es estéreo (canales != 2). " +
                "Es comportamiento CORRECTO (el DSP solo asume estéreo; mono/5.1/7.1 pasa intacto), no un bug."
        )
    }
    if (RouteBGate.FUSION_CORE_NULL in log) {
        return diag(
            RouteBGate.FUSION_CORE_NULL,
            "omega_process pasa de largo: el FusionEngine no se creó (creación lazy fallida, un solo intento por instancia). " +
                "Una instancia nueva del efecto reintenta la creación."
        )
    }
    if (RouteBGate.BYPASS_BEGIN_BLOCK_FAIL in log) {
        return diag(
            RouteBGate.BYPASS_BEGIN_BLOCK_FAIL,
            "omega_process pasa de largo: masterBypassEnv.beginBlock() FALLO — la envolvente de bypass no arranca el bloque. " +
                "Es un fallo de la instancia (no del stream): una instancia nueva lo reintenta."
        )
    }
    if (RouteBGate.RT_BUFFERS_NULL in log) {
        return diag(
            RouteBGate.RT_BUFFERS_NULL,
            "omega_process pasa de largo: buffers RT nulos o rtCapacity<=0 — EFFECT_CMD_SET_CONFIG jamás llegó del HAL o el calloc " +
                "de los buffers falló en la creación. Sin buffers no se puede procesar sin asignar en el hilo de audio. " +
                "Una instancia nueva re-ejecuta la inicialización."
        )
    }
    if (RouteBGate.NOT_ENABLED_NATIVE in log) {
        return diag(
            RouteBGate.NOT_ENABLED_NATIVE,
            "omega_process pasa de largo: el efecto no está habilitado a nivel nativo (EFFECT_CMD_ENABLE jamás llegó y el auto-enable no se disparó). " +
                "Una instancia nueva puede auto-habilitarse en su primer bloque."
        )
    }
    if (RouteBGate.BEACON_FILE_MISSING in log || !evidence.beaconFileExists) {
        return diag(
            RouteBGate.BEACON_FILE_MISSING,
            "El archivo del beacon /data/adb/ivanna_omega/omega_effect_beacon no existe: audioserver no puede crearlo " +
                "solo y el daemon nunca lo pre-creó. Sin archivo no hay señal de procesado. " +
                "Pre-crearlo 0666 vía root lo desbloquea."
        )
    }
    // Beacon ausente N ciclos mientras está acoplado y suena: el punto ciego
    // del detector viejo (beacon < 0 = "conservar estado" para siempre).
    if (evidence.beaconFrames < 0 && evidence.musicPlaying &&
        evidence.beaconNeverSeenCycles >= ROUTE_B_NEVER_SEEN_CYCLES_TO_DIAGNOSE) {
        return diag(
            RouteBGate.BEACON_NEVER_EXISTED,
            "Efecto acoplado a la sesión ${evidence.attachedSessionId} y la música suena, pero el beacon nunca apareció " +
                "en ${evidence.beaconNeverSeenCycles} ciclos: el efecto no escribe onBlock (huérfano o puerta silenciosa). " +
                "El detector viejo nunca lo recreaba (beacon < 0 = conservar estado)."
        )
    }
    // Frames congelados N ciclos mientras suena: huérfano clásico.
    if (evidence.beaconFrames >= 0 && evidence.musicPlaying &&
        evidence.frozenCycles >= ROUTE_B_FROZEN_CYCLES_TO_ORPHAN) {
        return diag(
            RouteBGate.ORPHAN_FROZEN,
            "Efecto acoplado a la sesión ${evidence.attachedSessionId} con ${evidence.beaconFrames} frames congelados " +
                "durante ${evidence.frozenCycles} ciclos mientras la música suena: huérfano (existe sin hilo de salida)."
        )
    }
    // Habilitado pero sin bloques recientes. El beacon SÍ se ha visto antes
    // (frames >= 0): si nunca apareció (frames < 0) no es EFFECT_IDLE sino
    // UNKNOWN hasta el umbral de 3 ciclos (BEACON_NEVER_EXISTED). En pausa
    // (musicPlaying=false) los frames congelados son lo esperado: tampoco
    // es orfandad, es EFFECT_IDLE (el efecto está habilitado y quieto).
    if (evidence.effectState == DSPBridge.EffectState.ENABLED_IDLE && evidence.beaconFrames >= 0) {
        return diag(
            RouteBGate.EFFECT_IDLE,
            "El efecto está habilitado pero sin bloques recientes" +
                (if (evidence.musicPlaying) " mientras suena: hueco transitorio o puerta silenciosa. Se sigue observando; si persiste se declara huérfano."
                else " (música en pausa: los frames congelados son lo esperado, no es orfandad).")
        )
    }
    return diag(
        RouteBGate.UNKNOWN,
        "No se puede determinar la puerta con la evidencia disponible " +
            "(efecto acoplado=${evidence.effectAttached}, beacon=${evidence.beaconFrames}, " +
            "estado=${evidence.effectState}, fusión=${evidence.fusionActive}). Se consulta al LLM si está disponible."
    )
}

internal fun RouteBEvidence.isUsableDiscoveredSession(): Boolean = discoveredSessionId > 0

/**
 * Reparación de la whitelist para un diagnóstico. Pura (testeable en host).
 * Solo las puertas con una acción runtime segura devuelven una reparación;
 * el resto es REPORT_ONLY (o NONE si está sano / es transitorio).
 */
internal fun decideRouteBRepair(diagnosis: RouteBDiagnosis): RouteBRepair = when (diagnosis.gate) {
    RouteBGate.HEALTHY_PROCESSING -> RouteBRepair.NONE
    RouteBGate.BEACON_FILE_MISSING -> RouteBRepair.PRECREATE_BEACON
    RouteBGate.BEACON_PERMISSION_OR_LABEL_FAIL -> RouteBRepair.REPORT_ONLY
    RouteBGate.FUSION_CORE_NULL -> RouteBRepair.RECREATE_ORPHAN
    RouteBGate.BYPASS_BEGIN_BLOCK_FAIL -> RouteBRepair.RECREATE_ORPHAN
    RouteBGate.RT_BUFFERS_NULL -> RouteBRepair.RECREATE_ORPHAN
    RouteBGate.NOT_ENABLED_NATIVE -> RouteBRepair.RECREATE_ORPHAN
    RouteBGate.BEACON_NEVER_EXISTED -> RouteBRepair.RECREATE_ORPHAN
    RouteBGate.ORPHAN_FROZEN -> RouteBRepair.RECREATE_ORPHAN
    RouteBGate.NOT_ATTACHED -> RouteBRepair.RETRY_ATTACH
    RouteBGate.EFFECT_IDLE -> RouteBRepair.NONE
    RouteBGate.DAEMON_UNREACHABLE,
    RouteBGate.NO_SESSION_DISCOVERED,
    RouteBGate.INTEGRITY_FAIL,
    RouteBGate.ANTITAMPER_LOCKDOWN,
    RouteBGate.NOT_STEREO,
    RouteBGate.SNAPSHOT_NEUTRAL,
    RouteBGate.UNKNOWN -> RouteBRepair.REPORT_ONLY
}

/**
 * Snapshot de diagnóstico como JSON para el LLM. SOLO datos de diagnóstico
 * (logs, telemetría, estado, códigos de error). NUNCA código fuente.
 * Pura (testeable en host).
 */
internal fun routeBDiagnosisToJson(evidence: RouteBEvidence, diagnosis: RouteBDiagnosis): String {
    return JSONObject().apply {
        put("gate", diagnosis.gate.name)
        put("explanation", diagnosis.explanation)
        put("attachedSessionId", evidence.attachedSessionId)
        put("discoveredSessionId", evidence.discoveredSessionId)
        put("daemonReachable", evidence.daemonReachable)
        put("effectAttached", evidence.effectAttached)
        put("beaconFrames", evidence.beaconFrames)
        put("beaconFramesGrowing", evidence.beaconFramesGrowing)
        put("effectState", evidence.effectState.name)
        put("beaconFileExists", evidence.beaconFileExists)
        put("fusionActive", evidence.fusionActive?.toString() ?: "null")
        put("psiActive", evidence.psiActive?.toString() ?: "null")
        put("ratio", evidence.ratio?.toString() ?: "null")
        put("musicPlaying", evidence.musicPlaying)
        put("logGates", evidence.logGates.joinToString(",") { it.name })
        put("lastAttachFailure", evidence.lastAttachFailure.name)
        put("beaconNeverSeenCycles", evidence.beaconNeverSeenCycles)
        put("frozenCycles", evidence.frozenCycles)
        put("snapshotGeneration", evidence.snapshotGeneration?.toString() ?: "null")
        put("perceptualGeneration", evidence.perceptualGeneration?.toString() ?: "null")
        put("snapshotOrbeAmount", evidence.snapshotOrbeAmount?.toString() ?: "null")
        put("snapshotHolosAmount", evidence.snapshotHolosAmount?.toString() ?: "null")
    }.toString()
}

// ── LLM (advisory) ───────────────────────────────────────────────────────────

/**
 * Seam del LLM para el doctor. El proveedor lo registra la UI (el
 * ViewModel, que posee el IvannaGeminiAgent) con la función
 * `diagnoseWithLlm` del agente (OmniRoute si está habilitado, si no
 * Gemini). Null = sin LLM: el doctor es 100% determinista.
 * La respuesta es un consejo; el doctor solo ejecuta la whitelist.
 */
object RouteBDoctorLlmProvider {
    @Volatile
    var advise: (suspend (String) -> String?)? = null
}

/** Prompt del doctor al LLM: solo diagnóstico, respuesta corta y accionable. */
internal fun routeBDoctorLlmPrompt(snapshotJson: String): String = buildString {
    append("Eres el doctor de diagnóstico de IVANNA OMEGA (Ruta B, audio Android). ")
    append("El efecto omega se acopló a una sesión de música pero NO procesa audio. ")
    append("Te doy SOLO datos de diagnóstico (sin código fuente). ")
    append("Nombra la causa más probable entre: NOT_ENABLED_NATIVE, NOT_STEREO, INTEGRITY_FAIL, ")
    append("ANTITAMPER_LOCKDOWN, FUSION_CORE_NULL, BYPASS_BEGIN_BLOCK_FAIL, RT_BUFFERS_NULL, ")
    append("SNAPSHOT_NEUTRAL, BEACON_FILE_MISSING, BEACON_NEVER_EXISTED, ")
    append("ORPHAN_FROZEN, DAEMON_UNREACHABLE, NO_SESSION_DISCOVERED, NOT_ATTACHED, EFFECT_IDLE. ")
    append("Y recomienda UNA acción de esta lista: PRECREATE_BEACON, RECREATE_ORPHAN, RETRY_ATTACH, ")
    append("RESET_ORPHAN_DETECTOR, REPORT_ONLY, NONE. ")
    append("Responde en español, en 3 líneas: 1) causa, 2) por qué, 3) acción. ")
    append("Si no lo sabes, di UNKNOWN y REPORT_ONLY. No inventes datos.\n\n")
    append("SNAPSHOT: ")
    append(snapshotJson)
}

/**
 * Extrae una puerta y una reparación de la respuesta libre del LLM.
 * Best-effort: busca los nombres de enum; si no hay coincidencia limpia,
 * devuelve null (el consejo queda solo como texto, sin acción).
 */
internal fun parseLlmAdvice(text: String): Pair<RouteBGate, RouteBRepair>? {
    val upper = text.uppercase()
    val gate = enumValues<RouteBGate>().firstOrNull { upper.contains(it.name) } ?: return null
    val repair = enumValues<RouteBRepair>().firstOrNull { upper.contains(it.name) }
        ?: return null
    return gate to repair
}

// ── Doctor (estado entre ciclos) ─────────────────────────────────────────────

/**
 * Doctor de Ruta B. Lo posee IvannaGlobalEffectManager; el watchdog lo llama
 * cada 3 s con la evidencia del ciclo. Mantiene los contadores entre ciclos
 * (frames congelados, beacon nunca visto, reparación pendiente de verificar)
 * y decide diagnóstico + reparación. Las acciones con efectos (soltar el
 * efecto, re-acoplar, pre-crear el beacon) las ejecuta el llamador con los
 * métodos del manager; el doctor solo las DECIDE y las VERIFICA.
 *
 * Ciclo de verificación de una reparación: tras aplicarla, el doctor espera
 * hasta [verifyCycles] ciclos a que el efecto procese (frames creciendo o
 * effectState PROCESSING). Si verifica → repairVerified. Si no → repairFailed
 * (se reporta; la misma reparación no se reintenta hasta el cooldown).
 *
 * @param logReader lector del log inyectable (por defecto, el real por root).
 *   En tests se pasa `{ emptySet() }` para no invocar `su`/`logcat`.
 */
class RouteBDoctor(
    private val logReader: (() -> Set<RouteBGate>)? = null
) {
    private val tag = "IvannaNPE.RouteBDoctor"

    // Contadores entre ciclos.
    private var lastBeaconFrames: Long = -1L
    private var frozenCycles: Int = 0
    private var neverSeenCycles: Int = 0
    private var lastLogReadMs: Long = 0L
    private var lastLlmMs: Long = 0L
    private var pendingLlmAdvice: String? = null
    // Puerta del ciclo anterior (reset de intentos al cambiar de puerta).
    private var lastDiagnosisGate: RouteBGate? = null
    // Cache del segundo señal (cadena vía dumpsys): sesión + resultado.
    private var lastChainCheckMs: Long = 0L
    private var cachedChainSession: Int = 0
    private var cachedChainEnabled: Boolean? = null

    // Reparación pendiente de verificar.
    private var pendingRepair: RouteBRepair? = null
    private var pendingRepairSession: Int = 0
    private var pendingRepairCycles: Int = 0
    private var repairAttempts: MutableMap<String, Int> = mutableMapOf()
    private var lastRepairMsByKey: MutableMap<String, Long> = mutableMapOf()

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    companion object {
        /** Ciclos para verificar una reparación (5 × 3 s = 15 s). */
        internal const val VERIFY_CYCLES: Int = 5
        /** Cooldown entre reintentos de la misma reparación en la misma sesión. */
        internal const val REPAIR_COOLDOWN_MS: Long = 60_000L
        /** Máximo de intentos de la misma reparación por sesión antes de rendirse. */
        internal const val MAX_REPAIR_ATTEMPTS: Int = 3
        /** Throttle de lectura del log (el logcat por root es caro). */
        internal const val LOG_READ_MIN_INTERVAL_MS: Long = 30_000L
        /** Throttle de consulta al LLM. */
        internal const val LLM_MIN_INTERVAL_MS: Long = 60_000L
        /** Ruta del beacon (misma que OMEGA_EFFECT_BEACON_PATH). */
        const val BEACON_PATH: String = "/data/adb/ivanna_omega/omega_effect_beacon"
        /** Throttle del segundo señal (dumpsys es caro). */
        internal const val CHAIN_CHECK_MIN_INTERVAL_MS: Long = 30_000L
    }

    /** Contadores actuales (para la evidencia del manager). */
    val currentFrozenCycles: Int get() = frozenCycles
    val currentNeverSeenCycles: Int get() = neverSeenCycles

    /**
     * Un ciclo del doctor. Actualiza contadores, verifica reparaciones
     * pendientes, lee el log solo si hay un problema sospechoso, diagnostica
     * y decide la reparación. No ejecuta reparaciones (lo hace el llamador).
     */
    fun runCycle(evidence: RouteBEvidence): RouteBDoctorOutcome {
        // FIX 4 (v2.5.465): crecimiento del beacon ANTES de actualizar la
        // línea base (paso 2): frames de este ciclo > frames del ciclo
        // anterior = el efecto procesa, aunque effectState nunca sea
        // PROCESSING (este HAL no manda EFFECT_CMD_ENABLE). El primer ciclo
        // con frames válidos no puede probarlo (sin línea base): false.
        val framesGrowing = evidence.beaconFrames > 0 && lastBeaconFrames >= 0 &&
            evidence.beaconFrames > lastBeaconFrames
        // 0. Si cambió la sesión acoplada, la verificación pendiente es de
        // otra sesión (Qobuz rota: 1697→1969): invalidarla, no contarla
        // como fallo de la reparación.
        if (pendingRepair != null && evidence.attachedSessionId > 0 &&
            pendingRepairSession != evidence.attachedSessionId) {
            Log.i(tag, "Doctor: sesión acoplada cambió $pendingRepairSession→${evidence.attachedSessionId} — verificación pendiente de $pendingRepair invalidada")
            pendingRepair = null
            pendingRepairCycles = 0
        }
        // 1. Verificación de una reparación pendiente.
        var verified = false
        var failed = false
        val pending = pendingRepair
        if (pending != null) {
            pendingRepairCycles++
            val nowProcessing = evidence.effectState == DSPBridge.EffectState.PROCESSING ||
                (evidence.beaconFrames > 0 && evidence.beaconFrames > lastBeaconFrames)
            if (nowProcessing) {
                Log.i(tag, "Doctor: reparación $pending VERIFICADA en sesión $pendingRepairSession " +
                    "(efecto procesa: estado=${evidence.effectState}, frames=${evidence.beaconFrames})")
                verified = true
                pendingRepair = null
                pendingRepairCycles = 0
            } else if (pendingRepairCycles >= VERIFY_CYCLES) {
                Log.w(tag, "Doctor: reparación $pending FALLÓ en sesión $pendingRepairSession " +
                    "(${VERIFY_CYCLES} ciclos sin procesar) — se reporta, no se reintenta en bucle")
                failed = true
                pendingRepair = null
                pendingRepairCycles = 0
            }
        }

        // 2. Contadores de beacon (solo si está acoplado y suena).
        val attachedPlaying = evidence.effectAttached && evidence.musicPlaying &&
            evidence.attachedSessionId > 0 &&
            evidence.attachedSessionId == evidence.discoveredSessionId
        if (attachedPlaying) {
            if (evidence.beaconFrames < 0) {
                neverSeenCycles++
                frozenCycles = 0
            } else if (lastBeaconFrames >= 0 && evidence.beaconFrames == lastBeaconFrames) {
                frozenCycles++
                neverSeenCycles = 0
            } else {
                frozenCycles = 0
                neverSeenCycles = 0
            }
            if (evidence.beaconFrames >= 0) lastBeaconFrames = evidence.beaconFrames
        } else {
            frozenCycles = 0
            neverSeenCycles = 0
            if (evidence.beaconFrames >= 0) lastBeaconFrames = evidence.beaconFrames
        }

        // 3. Log (solo si hay un problema sospechoso y toca leerlo).
        var logGates = evidence.logGates
        val suspectsProblem = evidence.effectAttached && evidence.musicPlaying &&
            evidence.effectState != DSPBridge.EffectState.PROCESSING
        val now = System.currentTimeMillis()
        if (suspectsProblem && now - lastLogReadMs >= LOG_READ_MIN_INTERVAL_MS) {
            lastLogReadMs = now
            logGates = logReader?.invoke() ?: readOmegaEffectLogGates()
        }

        // 4. Diagnóstico con los contadores actualizados.
        val fullEvidence = evidence.copy(
            logGates = logGates,
            frozenCycles = frozenCycles,
            beaconNeverSeenCycles = neverSeenCycles,
            // FIX 4: el crecimiento se calculó con la línea base anterior;
            // si el llamador ya lo trae (manager), el OR lo conserva.
            beaconFramesGrowing = framesGrowing || evidence.beaconFramesGrowing
        )
        var diagnosis = diagnoseRouteB(fullEvidence)

        // 4b. La puerta cambió: el problema es otro — resetear los intentos
        // de la reparación anterior (un fallo sistemático de permiso no debe
        // quemar los 3 intentos de la puerta nueva).
        if (diagnosis.gate != lastDiagnosisGate) {
            if (lastDiagnosisGate != null) {
                Log.i(tag, "Doctor: puerta cambió $lastDiagnosisGate→${diagnosis.gate} — intentos reseteados")
                repairAttempts.clear()
            }
            lastDiagnosisGate = diagnosis.gate
        }

        // 4c. Segundo señal, independiente del beacon (0038): si el beacon
        // sigue ciego (frames<0) y la cadena de AudioFlinger NO tiene el
        // efecto habilitado en la sesión, el problema es permiso/etiqueta,
        // no orfandad: no seguir recreando.
        if (diagnosis.gate == RouteBGate.BEACON_NEVER_EXISTED &&
            evidence.beaconFrames < 0) {
            val chainEnabled = readChainEnabledState(evidence.attachedSessionId)
            if (chainEnabled == false) {
                diagnosis = diagnosis.copy(
                    gate = RouteBGate.BEACON_PERMISSION_OR_LABEL_FAIL,
                    explanation = diagnosis.explanation +
                        " La cadena de AudioFlinger no tiene el efecto habilitado en la sesión " +
                        "${evidence.attachedSessionId} (dumpsys): el beacon es ciego por permiso/etiqueta, no por orfandad. Se deja de recrear."
                )
            }
        }

        // 5. LLM advisory solo si es UNKNOWN (y hay LLM y no está throttled).
        if (diagnosis.gate == RouteBGate.UNKNOWN) {
            val advise = RouteBDoctorLlmProvider.advise
            if (advise != null && now - lastLlmMs >= LLM_MIN_INTERVAL_MS) {
                lastLlmMs = now
                val snapshot = routeBDiagnosisToJson(fullEvidence, diagnosis)
                scope.launch {
                    runCatching {
                        val text = advise(routeBDoctorLlmPrompt(snapshot))
                        if (!text.isNullOrBlank()) {
                            pendingLlmAdvice = text.take(600)
                            Log.i(tag, "Doctor: consejo LLM (advisory): ${text.take(200)}")
                        }
                    }.onFailure { Log.w(tag, "Doctor: LLM no disponible: ${it.message}") }
                }
            }
            // Si hay un consejo LLM pendiente que mapea a la whitelist, úsalo
            // como diagnóstico (sin ejecutar nada fuera de la whitelist).
            pendingLlmAdvice?.let { advice ->
                parseLlmAdvice(advice)?.let { (gate, _) ->
                    diagnosis = diagnosis.copy(
                        gate = gate,
                        explanation = diagnosis.explanation + " Consejo LLM: " + advice.take(200),
                        llmAdvice = advice
                    )
                }
                pendingLlmAdvice = null
            }
        }

        // 6. Reparación (whitelist) con cooldown y tope de intentos.
        var repair = decideRouteBRepair(diagnosis)
        if (repair != RouteBRepair.NONE && repair != RouteBRepair.REPORT_ONLY) {
            val key = "${repair.name}:${evidence.attachedSessionId}"
            val attempts = repairAttempts[key] ?: 0
            val lastMs = lastRepairMsByKey[key] ?: 0L
            if (attempts >= MAX_REPAIR_ATTEMPTS) {
                Log.w(tag, "Doctor: $repair en sesión ${evidence.attachedSessionId} llegó a $MAX_REPAIR_ATTEMPTS intentos — se rinde y reporta")
                repair = RouteBRepair.REPORT_ONLY
            } else if (now - lastMs < REPAIR_COOLDOWN_MS && pendingRepair == null) {
                // Cooldown: no reintentar la misma reparación tan pronto.
                repair = RouteBRepair.NONE
            }
        }
        if (diagnosis.gate == RouteBGate.HEALTHY_PROCESSING) {
            // Sano: limpiar contadores de intentos (una reparación anterior funcionó).
            repairAttempts.clear()
        }
        return RouteBDoctorOutcome(diagnosis, repair, verified, failed)
    }

    /**
     * El llamador registra que aplicó la reparación: el doctor la verificará
     * en los próximos ciclos (frames creciendo / PROCESSING).
     */
    fun noteRepairExecuted(repair: RouteBRepair, sessionId: Int) {
        if (repair == RouteBRepair.NONE || repair == RouteBRepair.REPORT_ONLY) return
        pendingRepair = repair
        pendingRepairSession = sessionId
        pendingRepairCycles = 0
        val key = "${repair.name}:$sessionId"
        repairAttempts[key] = (repairAttempts[key] ?: 0) + 1
        lastRepairMsByKey[key] = System.currentTimeMillis()
        Log.i(tag, "Doctor: reparación $repair aplicada en sesión $sessionId (intento ${repairAttempts[key]}) — verificando en los próximos ciclos")
    }

    /** Resetea el detector de huérfanos (punto ciego: beacon nunca existió). */
    fun resetOrphanCounters() {
        lastBeaconFrames = -1L
        frozenCycles = 0
        neverSeenCycles = 0
    }

    // ── Lectura del log (root, best-effort, throttled por el llamador) ───────

    /**
     * Lee las líneas del log de IvannaOmegaEffect y OmegaCtrlBus vía root
     * (la app no tiene READ_LOGS para los logs de audioserver). Best-effort:
     * sin root o si logcat falla, devuelve vacío (el doctor diagnostica sin
     * el log). Nunca lanza.
     */
    fun readOmegaEffectLogGates(): Set<RouteBGate> {
        if (!RootAccess.cachedRoot && !RootAccess.probeSu()) return emptySet()
        val lines = runCatching {
            val proc = ProcessBuilder(
                "su", "-c",
                "logcat -d -t 200 -s IvannaOmegaEffect OmegaCtrlBus 2>/dev/null"
            ).redirectErrorStream(true).start()
            val finished = proc.waitFor(5, java.util.concurrent.TimeUnit.SECONDS)
            if (!finished) {
                runCatching { proc.destroy() }
                return emptySet()
            }
            proc.inputStream.bufferedReader().use { it.readLines() }
        }.getOrElse { return emptySet() }
        return parseOmegaEffectLog(lines)
    }

    /**
     * Segundo señal independiente del beacon (0038): ¿está el efecto omega
     * habilitado en la cadena de la sesión? Lee `dumpsys media.audio_flinger`
     * vía root (throttle 30 s, cache por sesión). Devuelve true si la cadena
     * lo muestra habilitado, false si lo muestra suspendido/deshabilitado o
     * no aparece en ninguna cadena, null si no se puede determinar (sin root,
     * dumpsys falló o formato irreconocible — fail-closed: con null no se
     * diagnostica nada). Nunca lanza.
     */
    fun readChainEnabledState(sessionId: Int): Boolean? {
        if (sessionId <= 0) return null
        val now = System.currentTimeMillis()
        if (now - lastChainCheckMs < CHAIN_CHECK_MIN_INTERVAL_MS &&
            cachedChainSession == sessionId) {
            return cachedChainEnabled
        }
        lastChainCheckMs = now
        cachedChainSession = sessionId
        cachedChainEnabled = null
        if (!RootAccess.cachedRoot && !RootAccess.probeSu()) return null
        val out = runCatching {
            val proc = ProcessBuilder(
                "su", "-c",
                "dumpsys media.audio_flinger 2>/dev/null"
            ).redirectErrorStream(true).start()
            val finished = proc.waitFor(5, java.util.concurrent.TimeUnit.SECONDS)
            if (!finished) {
                runCatching { proc.destroy() }
                return null
            }
            proc.inputStream.bufferedReader().use { it.readText() }
        }.getOrElse { return null }
        val result = parseChainEnabledState(out, sessionId)
        cachedChainEnabled = result
        return result
    }

    // ── Pre-creación del beacon (root, best-effort) ──────────────────────────

    /**
     * Pre-crea /data/adb/ivanna_omega/omega_effect_beacon vía root (el equivalente
     * en shell de precreateAndRelabelLocalFile del daemon): existencia +
     * tamaño 72 B + 0666 + contexto ivanna_data_file. Devuelve true solo si
     * el archivo quedó existiendo con 0666 Y con el contexto aplicado
     * (verificado por ls -Z); sin el contexto, el efecto sigue con EACCES.
     * 0043: la ruta ya no es /data/local/tmp (el kernel veía el archivo
     * como `unlabeled` pese al chcon); en /data/adb/ivanna_omega/ hereda
     * ivanna_data_file del directorio padre.
     */
    fun precreateBeaconFile(): Boolean {
        if (!RootAccess.cachedRoot && !RootAccess.probeSu()) return false
        return runCatching {
            // 0038: completar el precreate — ftruncate a 72 B + chcon a
            // ivanna_data_file (el tipo que sepolicy.rule ya permite
            // escribir a audioserver) + chmod 0666. El `ls -Z` verifica
            // que el chcon quedó aplicado; sin él, el doctor no debe
            // darlo por reparado. 0043: la ruta es BEACON_PATH
            // (/data/adb/ivanna_omega/), no /data/local/tmp.
            val proc = ProcessBuilder(
                "su", "-c",
                "f=$BEACON_PATH; " +
                    "[ -f \"\$f\" ] || : > \"\$f\"; truncate -s 72 \"\$f\"; " +
                    "chmod 0666 \"\$f\" 2>/dev/null; " +
                    "chcon u:object_r:ivanna_data_file:s0 \"\$f\" 2>/dev/null; " +
                    "ls -l \"\$f\" 2>/dev/null; ls -Z \"\$f\" 2>/dev/null"
            ).redirectErrorStream(true).start()
            val finished = proc.waitFor(5, java.util.concurrent.TimeUnit.SECONDS)
            if (!finished) {
                runCatching { proc.destroy() }
                return false
            }
            val out = proc.inputStream.bufferedReader().use { it.readText() }
            val ok = proc.exitValue() == 0 && out.contains("omega_effect_beacon") &&
                out.contains("ivanna_data_file")
            if (ok) Log.i(tag, "Doctor: beacon pre-creado 0666 vía root: ${out.trim().take(120)}")
            else Log.w(tag, "Doctor: no se pudo pre-crear el beacon vía root")
            ok
        }.getOrElse { false }
    }
}
