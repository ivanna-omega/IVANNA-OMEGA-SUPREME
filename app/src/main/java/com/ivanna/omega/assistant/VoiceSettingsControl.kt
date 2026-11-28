package com.ivanna.omega.assistant

import android.content.Context
import android.util.Log
import com.ivanna.omega.audio.EffectsOffTransition
import com.ivanna.omega.audio.Iso226Calibrator
import com.ivanna.omega.audio.RealAcousticSensor
import com.ivanna.omega.audio.VolterraSwitch
import com.ivanna.omega.core.IVANNAApplication
import com.ivanna.omega.core.IvannaNativeLib
import com.ivanna.omega.core.OmegaEquipment
import com.ivanna.omega.core.ParameterStore
import com.ivanna.omega.neuromorphic.IvannaNpeEngine

/**
 * VoiceSettingsControl — v2.5.445. Control por voz de ajustes DSP.
 *
 * Directiva de Esse (2026-11-14): la asistente DEBÍA controlar y auto
 * arreglar, no solo explicar. Este módulo interpreta órdenes de voz/texto
 * ("pon el ORBE a 1.0", "sube el HOLOS", "apaga el Phi", "pon el kernel
 * GENEZI") y las APLICA de verdad vía los setters/JNI existentes, con
 * confirmación hablada del cambio real.
 *
 * ESQUEMA DE ACCIONES (lo que la voz PUEDE y NO PUEDE hacer):
 *  - PUEDE: amounts ORBE/HOLOS/Φ 0..1; kernel Volterra GENEZI/PZ1D +
 *    on/off; ISO 226 on/off; Ω on/off + equipo; graves del sensor
 *    ±3 dB; volumen maestro 0..1; corrección de sala en vivo 0..1.
 *    Todo valor se clampa; un solo objetivo por turno.
 *  - NO PUEDE: auto-reparación, publicar, reiniciar, cambiar tasas de
 *    muestreo, tocar seguridad/credenciales ni ningún ajuste fuera de
 *    esta lista. Lo garantiza la construcción: parse() solo produce
 *    estos objetivos; lo demás devuelve null (o Negated/Ambiguous).
 *  - NEGACIÓN ("no subas el ORBE"): nunca se ejecuta; se responde que
 *    no se cambió nada. AMBIGÜEDAD ("sube el ORBE y el HOLOS"): nunca
 *    se ejecuta el primero e ignora el segundo; se pide aclaración.
 *  - "Sube el grave" NO es ORBE: es el trim de graves del sensor
 *    (control musical apropiado). "Sube el volumen" es el volumen
 *    maestro, no un efecto espacial.
 *  - READBACK: la confirmación dice lo que el MOTOR tiene, no lo
 *    pedido (nativeGetEffectAmounts para ORBE/HOLOS, getPhi tras
 *    setPhi, activeVolterraKernelId tras el cambio de kernel,
 *    roomCorrectionStatus para la corrección de sala). Sin librería
 *    nativa (modo offline) se persiste y se dice "guardado; se aplica
 *    al arrancar", nunca se afirma que suena ya.
 *
 * Diseño en dos capas (testeable):
 *  - `parseDetailed(text)`: puro y determinista, sin Android. Devuelve
 *    Command, None, Negated o Ambiguous.
 *  - `parse(text)`: compatibilidad; solo Command, resto null.
 *  - `apply(context, command)`: persiste (ParameterStore) + empuja al
 *    nativo (JNI) en vivo, igual que hace la UI (ControlTabScreen). Si el
 *    motor no está listo o la librería no cargó, lo dice honesto en vez
 *    de afirmar que se aplicó.
 *
 * Alcance (mínimo de la 445): ORBE amount, HOLOS amount, Φ amount,
 * kernel Volterra (selector), ISO 226 on/off, Ω enabled + equipment.
 * Ampliado: graves del sensor, volumen maestro, corrección de sala.
 *
 * Notas de implementación:
 *  - ORBE/HOLOS: por el puente fusionado — saveFusedControls (persist) +
 *    FusedControlsBridge.safeSetFusedControls (vivo: Ruta A por JNI +
 *    Ruta B por SET_PERCEPTUAL_STATE). Unificación 2.5.509 (paso 1): ya
 *    NO se escribe por setOrbeParams/setHolosParams ni por los JNI
 *    legacy. La sincronización al daemon la hace el propio puente.
 *  - Φ: IvannaNpeEngine.setPhi() (nuevo en la 445; reutiliza los últimos
 *    parámetros de setParameters). Runtime solamente: NO se persiste;
 *    al reiniciar vuelve a 0.5 (valor de la directiva). El readback es
 *    getPhi() (caché del valor aplicado por el motor; NO existe un
 *    getter nativo de Φ: se documenta, no se finge).
 *  - ISO 226: on = applyAll con los phon guardados (62→82 por defecto);
 *    off = applyAll con listenPhon == refPhon (compensación cero en las
 *    tres capas: EQ + DSPBridge + daemon) + clearIso226() persistido.
 *    Sin getter nativo: la confirmación dice los phon pedidos.
 *  - Ω: setOmegaParams + OmegaEquipment.applyToNative (resuelve Auto por
 *    ruta). Elegir equipo implica encender Ω (si no, el equipo no suena).
 *    Sin getter nativo: la confirmación dice el estado persistido.
 *  - Volterra: setVolterraKernel (0 = Ok) + VolterraSwitch.enabled.
 *    Readback: activeVolterraKernelId (fuente de verdad de Ruta A).
 *  - Graves: trim ±3 dB del sensor (RealAcousticSensor.setBassTrimDb,
 *    persistido). Sin getter nativo del trim: la confirmación usa el
 *    booleano del setter + el valor persistido.
 *  - Corrección de sala: bucle en vivo del sensor (persistido + setters;
 *    apagado por defecto). Readback: roomCorrectionStatus().
 */
object VoiceSettingsControl {

    private const val TAG = "VoiceSettingsControl"

    // ── Modelo ─────────────────────────────────────────────────────────────

    enum class Target(val labelEs: String) {
        ORBE("ORBE"),
        HOLOS("HOLOS"),
        PHI("Φ"),
        VOLTERRA("Volterra"),
        ISO226("ISO 226"),
        OMEGA("Ω"),
        /** Trim de graves del sensor (±3 dB). NO es ORBE ni un espacial. */
        BASS("el grave"),
        /** Volumen maestro (0..1). */
        VOLUME("el volumen"),
        /** Corrección de sala en vivo del sensor (0..1). */
        ROOM_CORRECTION("la corrección de sala")
    }

    enum class Action {
        SET,            // pon X a <valor>
        RAISE,          // sube X [paso]
        LOWER,          // baja X [paso]
        ENABLE,         // enciende / activa X
        DISABLE,        // apaga / desactiva X
        SELECT_KERNEL,  // pon el kernel <GENEZI|PZ1D>
        SELECT_EQUIPMENT // pon el Omega en <equipo>
    }

    data class VoiceSettingCommand(
        val target: Target,
        val action: Action,
        /** Valor en la unidad del objetivo: amounts/volumen/corrección de
         *  sala 0..1 (paso en RAISE/LOWER); graves en dB −3..+3. */
        val value: Float?,
        /** Id de kernel para SELECT_KERNEL. */
        val kernelId: String?,
        /** Clase de equipo Ω (0..8) para SELECT_EQUIPMENT. */
        val equipment: Int?,
        val raw: String
    )

    /** Resultado del parseo determinista (sin Android, sin red). */
    sealed class ParseOutcome {
        /** Una orden ejecutable (un solo objetivo). */
        data class Command(val command: VoiceSettingCommand) : ParseOutcome()
        /** No es una orden de ajuste: sigue el flujo normal. */
        data object None : ParseOutcome()
        /** Negación ("no subas el ORBE"): NO ejecutar nada. */
        data object Negated : ParseOutcome()
        /** Dos o más objetivos ("sube el ORBE y el HOLOS"): NO ejecutar
         *  el primero e ignorar el segundo; pedir aclaración. */
        data class Ambiguous(val targets: List<Target>) : ParseOutcome()
    }

    data class VoiceSettingResult(
        val applied: Boolean,
        val confirmation: String,
        val target: Target,
        val action: Action
    )

    // ── Parseo (puro) ──────────────────────────────────────────────────────

    private val ENABLE_WORDS = listOf("enciende", "activa", "prende", "ponlo", "activar", "encender", "prender", "aplica", "aplicar")
    private val DISABLE_WORDS = listOf("apaga", "desactiva", "apagar", "desactivar", "quita", "silencia")
    private val RAISE_WORDS = listOf("sube", "subir", "aumenta", "aumentar", "mas", "incrementa", "dale mas")
    private val LOWER_WORDS = listOf("baja", "bajar", "reduce", "reducir", "menos", "disminuye")
    private val SET_WORDS = listOf("pon", "cambia", "ajusta", "configura", "deja", "dejalo", "ponlo", "aplica", "aplicar")

    /** Palabras de valor sin número. */
    private val WORD_VALUES = mapOf(
        "maximo" to 1.0f, "maxima" to 1.0f, "todo" to 1.0f,
        "mitad" to 0.5f, "medio" to 0.5f,
        "minimo" to 0.0f, "minima" to 0.0f, "nada" to 0.0f, "cero" to 0.0f
    )

    /** Equipo Ω por palabra (se revisan las frases largas primero). */
    private val EQUIPMENT_WORDS: List<Pair<String, Int>> = listOf(
        "bocinas del telefono" to OmegaEquipment.PHONE_SPEAKERS,
        "bocina del telefono" to OmegaEquipment.PHONE_SPEAKERS,
        "estereo de casa" to OmegaEquipment.HOME_STEREO,
        "audifonos" to OmegaEquipment.HEADPHONES,
        "auriculares" to OmegaEquipment.HEADPHONES,
        "bluetooth" to OmegaEquipment.BLUETOOTH,
        "bocinas" to OmegaEquipment.PHONE_SPEAKERS,
        "bocina" to OmegaEquipment.PHONE_SPEAKERS,
        "telefono" to OmegaEquipment.PHONE_SPEAKERS,
        "estereo" to OmegaEquipment.HOME_STEREO,
        "cine" to OmegaEquipment.CINEMA,
        "auto" to OmegaEquipment.CAR,
        "coche" to OmegaEquipment.CAR,
        "carro" to OmegaEquipment.CAR,
        "club" to OmegaEquipment.PA,
        "estadio" to OmegaEquipment.STADIUM,
        "automatico" to OmegaEquipment.AUTO
    )

    /** Compatibilidad: solo Command; Negated/Ambiguous/None → null. */
    fun parse(rawText: String): VoiceSettingCommand? =
        when (val outcome = parseDetailed(rawText)) {
            is ParseOutcome.Command -> outcome.command
            else -> null
        }

    /** Parseo completo: distingue orden, negación y ambigüedad. */
    fun parseDetailed(rawText: String): ParseOutcome {
        val t = normalize(rawText)
        if (t.isBlank()) return ParseOutcome.None
        // Pregunta ("¿cómo se aplica el ORBE?", "¿qué hace el HOLOS?"): NO
        // es una orden — va al LLM, no se ejecuta nada. Sin esta guarda,
        // "aplica" en ENABLE_WORDS/SET_WORDS convertiría la pregunta en
        // un comando y encendería el efecto solo por preguntar por él.
        // (2026-11-14: el propietario pedía "aplicar algo" y el verbo
        // "aplica" no existía en el parser — esa era la causa de que lo
        // escrito/hablado no aplicara nada y solo respondiera.)
        if (isQuestion(t)) return ParseOutcome.None
        // Negación ANTES del verbo/objetivo ("no subas el ORBE", "nunca
        // apagues el HOLOS"): no ejecutar nada, ni por error.
        if (isNegated(t)) return ParseOutcome.Negated
        val targets = detectTargets(t)
        if (targets.isEmpty()) return ParseOutcome.None
        if (targets.size > 1) return ParseOutcome.Ambiguous(targets)
        val command = parseForTarget(t, targets[0], rawText)
            ?: return ParseOutcome.None
        return ParseOutcome.Command(command)
    }

    /** True si el texto es una pregunta (no una orden): lleva "?" o empieza
     *  por palabra interrogativa. Las preguntas van al LLM; nunca se
     *  ejecutan como ajuste. */
    private fun isQuestion(t: String): Boolean {
        if (t.contains("?")) return true
        return t.startsWith("como ") || t.startsWith("que ") || t.startsWith("cual ") ||
            t.startsWith("por que ") || t.startsWith("donde ") || t.startsWith("cuando ") ||
            t.startsWith("cuanto ") || t.startsWith("para que ")
    }

    private val NEGATION_WORDS = listOf("no", "nunca", "jamas")

    /** Formas verbales de orden (imperativo + subjuntivo: "subas"). */
    private val NEGATED_VERB_FORMS = listOf(
        "subas", "bajes", "apagues", "enciendas", "pongas", "cambies",
        "quites", "toques", "dejes", "sube", "baja", "apaga", "enciende",
        "pon", "cambia", "quita", "aumenta", "reduce", "ajusta",
        "configura", "prende", "deja"
    )

    /** Palabras de diagnóstico: "no suena el ORBE" es un reporte, no una
     *  orden negada (va al flujo normal, no a "no cambio nada"). */
    private val DIAGNOSTIC_WORDS = listOf("suena", "escucha", "oye", "funciona")

    /**
     * True si una negación aparece antes del primer verbo u objetivo Y el
     * texto no es una pregunta ni un reporte de diagnóstico.
     * ("no apaga el HOLOS" ejecutaría DISABLE sin esto; "¿por qué no
     * suena el ORBE?" NO es negación: va al LLM/diagnóstico.)
     */
    private fun isNegated(t: String): Boolean {
        if (t.contains("?")) return false
        if (t.startsWith("que ") || t.startsWith("por que") || t.startsWith("como ") ||
            t.startsWith("cual ") || t.startsWith("donde ") || t.startsWith("cuando ")
        ) return false
        if (DIAGNOSTIC_WORDS.any { hasWord(t, it) }) return false
        val negIdx = NEGATION_WORDS.mapNotNull { findWordIndex(t, it) }.minOrNull()
            ?: return false
        val firstAction = (NEGATED_VERB_FORMS.mapNotNull { findWordIndex(t, it) } +
            TARGET_MARKERS.mapNotNull { findWordIndex(t, it) }).minOrNull()
            ?: return false
        return negIdx < firstAction
    }

    /** Índice de la palabra completa, o null. */
    private fun findWordIndex(text: String, word: String): Int? {
        val regex = Regex("(^|\\s)" + Regex.escape(word) + "(\\s|$|[.,!?¿¡])")
        return regex.find(text)?.range?.first?.let { start ->
            // El match incluye el espacio previo; ajustar al inicio real.
            if (text[start] == ' ') start + 1 else start
        }
    }

    /** Marcadores de objetivo (para negación y ambigüedad). */
    private val TARGET_MARKERS = listOf(
        "volterra", "kernel", "iso", "loudness", "omega", "orbe", "holos",
        "phi", "fi", "hiperoperador", "hiper-operador", "npe",
        "correccion", "grave", "graves", "bajo", "bajos", "volumen"
    )

    /** Todos los objetivos mencionados, en orden fijo, sin duplicados. */
    private fun detectTargets(t: String): List<Target> {
        val found = mutableListOf<Target>()
        fun add(target: Target, present: Boolean) {
            if (present && target !in found) found.add(target)
        }
        // Orden: el más específico primero.
        add(Target.VOLTERRA, hasWord(t, "volterra") || hasWord(t, "kernel"))
        add(Target.ISO226, hasWord(t, "iso") || t.contains("iso 226") ||
            t.contains("iso226") || hasWord(t, "loudness"))
        add(Target.OMEGA, hasWord(t, "omega") || t.contains("operador universal"))
        add(Target.ORBE, hasWord(t, "orbe"))
        add(Target.HOLOS, hasWord(t, "holos"))
        add(Target.PHI, hasWord(t, "phi") || hasWord(t, "fi") ||
            t.contains("hiperoperador") || t.contains("hiper-operador") ||
            t.contains("operador phi") || hasWord(t, "npe"))
        add(Target.ROOM_CORRECTION, hasWord(t, "correccion"))
        // Graves vs volumen: "pon el volumen bajo" es VOLUMEN (el "bajo"
        // es el nivel), no graves. Sin "volumen", bajo/bajos es graves.
        val hasVolume = hasWord(t, "volumen")
        add(Target.VOLUME, hasVolume)
        add(Target.BASS, !hasVolume && (hasWord(t, "grave") || hasWord(t, "graves") ||
            hasWord(t, "bajo") || hasWord(t, "bajos")))
        return found
    }

    private fun parseForTarget(t: String, target: Target, raw: String): VoiceSettingCommand? {
        return when (target) {
            Target.VOLTERRA -> parseVolterra(t, raw)
            Target.ISO226 -> parseIso226(t, raw)
            Target.OMEGA -> parseOmega(t, raw)
            Target.ORBE, Target.HOLOS, Target.PHI -> parseAmountTarget(t, target, raw)
            Target.BASS -> parseBass(t, raw)
            Target.VOLUME -> parseVolume(t, raw)
            Target.ROOM_CORRECTION -> parseRoomCorrection(t, raw)
        }
    }

    private fun parseBass(t: String, raw: String): VoiceSettingCommand? {
        // Nivel por palabra: "grave bajo" = menos graves.
        if (hasWord(t, "bajo") && extractDbValue(t) == null) {
            return VoiceSettingCommand(Target.BASS, Action.SET, -1.0f, null, null, raw)
        }
        if (hasWord(t, "alto") && extractDbValue(t) == null) {
            return VoiceSettingCommand(Target.BASS, Action.SET, 1.5f, null, null, raw)
        }
        // Graves en dB (−3..+3): el número es dB tal cual ("a 2" = +2 dB,
        // no 2 %). Paso de subir/bajar: 0.5 dB. Encender = +1 dB realce
        // moderado; apagar = 0 dB (neutro, el trim es un offset).
        val db = extractDbValue(t)
        if (db != null && SET_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.BASS, Action.SET, db, null, null, raw)
        }
        if (RAISE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.BASS, Action.RAISE, db ?: 0.5f, null, null, raw)
        }
        if (LOWER_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.BASS, Action.LOWER, db ?: 0.5f, null, null, raw)
        }
        if (ENABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.BASS, Action.ENABLE, null, null, null, raw)
        }
        if (DISABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.BASS, Action.DISABLE, null, null, null, raw)
        }
        if (hasWord(t, "pon")) {
            return VoiceSettingCommand(Target.BASS, Action.ENABLE, null, null, null, raw)
        }
        return null
    }

    private fun parseVolume(t: String, raw: String): VoiceSettingCommand? {
        // Nivel por palabra: "volumen bajo" = bajo, "volumen alto" = alto.
        // (Sin esto, "pon el volumen bajo" caía en ENABLE = 0.85, al revés.)
        if (hasWord(t, "bajo") && extractValue(t) == null) {
            return VoiceSettingCommand(Target.VOLUME, Action.SET, 0.2f, null, null, raw)
        }
        if (hasWord(t, "alto") && extractValue(t) == null) {
            return VoiceSettingCommand(Target.VOLUME, Action.SET, 0.85f, null, null, raw)
        }
        return parseAmountTarget(t, Target.VOLUME, raw)
    }

    private fun parseRoomCorrection(t: String, raw: String): VoiceSettingCommand? =
        parseAmountTarget(t, Target.ROOM_CORRECTION, raw)

    /**
     * Extrae un valor en dB (−3..+3) del texto: "2" → 2.0, "−1.5" → −1.5,
     * "al máximo" → 3.0, "a cero" → 0.0. null si no hay valor.
     */
    private fun extractDbValue(t: String): Float? {
        if (hasWord(t, "maximo") || hasWord(t, "maxima") || hasWord(t, "todo")) return 3.0f
        if (hasWord(t, "minimo") || hasWord(t, "minima")) return -3.0f
        if (hasWord(t, "cero") || hasWord(t, "nada")) return 0.0f
        if (hasWord(t, "mitad") || hasWord(t, "medio")) return 1.5f
        val num = Regex("([+-]?\\d+(?:[.,]\\d+)?)").find(t)
        if (num != null) {
            val v = num.groupValues[1].replace(',', '.').toFloatOrNull() ?: return null
            return v.coerceIn(-3f, 3f)
        }
        return null
    }

    private fun parseAmountTarget(t: String, target: Target, raw: String): VoiceSettingCommand? {
        // 1. SET con valor explícito ("pon el ORBE a 0.7", "al 70%", "a la mitad").
        val setValue = extractValue(t)
        if (setValue != null && SET_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(target, Action.SET, setValue, null, null, raw)
        }
        // 2. Subir / bajar (con paso opcional: "sube el HOLOS 0.2").
        if (RAISE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(target, Action.RAISE, setValue ?: 0.1f, null, null, raw)
        }
        if (LOWER_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(target, Action.LOWER, setValue ?: 0.1f, null, null, raw)
        }
        // 3. Encender / apagar.
        if (ENABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(target, Action.ENABLE, null, null, null, raw)
        }
        if (DISABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(target, Action.DISABLE, null, null, null, raw)
        }
        // 4. "pon el ORBE" sin valor → encender (interpretación del usuario).
        if (hasWord(t, "pon")) {
            return VoiceSettingCommand(target, Action.ENABLE, null, null, null, raw)
        }
        return null
    }

    private fun parseVolterra(t: String, raw: String): VoiceSettingCommand? {
        // Kernel por nombre (prioridad: el nombre manda sobre on/off).
        if (t.contains("genezi") || t.contains("genezy")) {
            return VoiceSettingCommand(
                Target.VOLTERRA, Action.SELECT_KERNEL, null,
                IvannaNativeLib.VOLTERRA_KERNEL_GENEZI, null, raw
            )
        }
        if (t.contains("pz1d") || t.contains("pz 1d") || t.contains("refinado")) {
            return VoiceSettingCommand(
                Target.VOLTERRA, Action.SELECT_KERNEL, null,
                IvannaNativeLib.VOLTERRA_KERNEL_PZ1D, null, raw
            )
        }
        if (ENABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.VOLTERRA, Action.ENABLE, null, null, null, raw)
        }
        if (DISABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.VOLTERRA, Action.DISABLE, null, null, null, raw)
        }
        return null
    }

    private fun parseIso226(t: String, raw: String): VoiceSettingCommand? {
        if (ENABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.ISO226, Action.ENABLE, null, null, null, raw)
        }
        if (DISABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.ISO226, Action.DISABLE, null, null, null, raw)
        }
        // "pon el ISO 226" sin verbo claro → encender.
        if (hasWord(t, "pon")) {
            return VoiceSettingCommand(Target.ISO226, Action.ENABLE, null, null, null, raw)
        }
        return null
    }

    private fun parseOmega(t: String, raw: String): VoiceSettingCommand? {
        // Equipo por palabra ("pon el Omega en audífonos").
        for ((word, equipment) in EQUIPMENT_WORDS) {
            if (t.contains(word)) {
                return VoiceSettingCommand(
                    Target.OMEGA, Action.SELECT_EQUIPMENT, null, null, equipment, raw
                )
            }
        }
        if (ENABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.OMEGA, Action.ENABLE, null, null, null, raw)
        }
        if (DISABLE_WORDS.any { hasWord(t, it) }) {
            return VoiceSettingCommand(Target.OMEGA, Action.DISABLE, null, null, null, raw)
        }
        if (hasWord(t, "pon")) {
            return VoiceSettingCommand(Target.OMEGA, Action.ENABLE, null, null, null, raw)
        }
        return null
    }

    /**
     * Extrae un valor 0..1 del texto: "70%" → 0.7, "0.7" → 0.7, "70" → 0.7,
     * "a la mitad" → 0.5, "al máximo" → 1.0. null si no hay valor.
     *
     * OJO — decimales >1 NO son porcentajes: "pon el ORBE a 2.0" es un
     * valor fuera de rango que se clampa a 1.0 (antes se dividía entre
     * 100 y daba 0.02, lo contrario de lo pedido). Solo los ENTEROS >1
     * son porcentajes sin el signo ("a 70" = 0.7).
     */
    private fun extractValue(t: String): Float? {
        for ((word, value) in WORD_VALUES) {
            if (hasWord(t, word)) return value
        }
        val pct = Regex("(\\d+(?:[.,]\\d+)?)\\s*%").find(t)
        if (pct != null) {
            val v = pct.groupValues[1].replace(',', '.').toFloatOrNull() ?: return null
            return (v / 100f).coerceIn(0f, 1f)
        }
        val num = Regex("(\\d+(?:[.,]\\d+)?)").find(t)
        if (num != null) {
            val raw = num.groupValues[1]
            val v = raw.replace(',', '.').toFloatOrNull() ?: return null
            val isDecimal = raw.contains('.') || raw.contains(',')
            // Decimal >1 → fuera de rango, clampar (no es un porcentaje).
            // Entero >1 → porcentaje sin el signo.
            return if (isDecimal) v.coerceIn(0f, 1f)
            else if (v > 1f) (v / 100f).coerceIn(0f, 1f) else v.coerceIn(0f, 1f)
        }
        return null
    }

    private fun normalize(text: String): String {
        val lower = text.lowercase().trim()
        val sb = StringBuilder(lower.length)
        for (ch in lower) {
            sb.append(
                when (ch) {
                    'á', 'à', 'ä' -> 'a'
                    'é', 'è', 'ë' -> 'e'
                    'í', 'ì', 'ï' -> 'i'
                    'ó', 'ò', 'ö' -> 'o'
                    'ú', 'ù', 'ü' -> 'u'
                    else -> ch
                }
            )
        }
        return sb.toString().replace(Regex("\\s+"), " ")
    }

    private fun hasWord(text: String, word: String): Boolean {
        val regex = Regex("(^|\\s)" + Regex.escape(word) + "(\\s|$|[.,!?¿¡])")
        return regex.containsMatchIn(text)
    }

    // ── Aplicación (Android: persist + JNI en vivo) ────────────────────────

    /** Aplica el comando y devuelve si se aplicó de verdad + confirmación. */
    fun apply(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        return when (command.target) {
            Target.ORBE -> applyAmount(context, command, isOrbe = true)
            Target.HOLOS -> applyAmount(context, command, isOrbe = false)
            Target.PHI -> applyPhi(command)
            Target.VOLTERRA -> applyVolterra(context, command)
            Target.ISO226 -> applyIso226(context, command)
            Target.OMEGA -> applyOmega(context, command)
            Target.BASS -> applyBass(context, command)
            Target.VOLUME -> applyVolume(context, command)
            Target.ROOM_CORRECTION -> applyRoomCorrection(context, command)
        }
    }

    /**
     * Readback del motor: amounts [orbe, holos, phi, volterra] en vivo
     * (nativeGetEffectAmounts, FloatArray[4]). null si la librería no
     * está cargada o la lectura falla. La confirmación usa ESTO, no lo
     * pedido: si el motor quedó en otro valor, se dice.
     */
    private fun readEffectAmount(index: Int): Float? {
        if (!IvannaNativeLib.isLoaded) return null
        return runCatching {
            val amounts = IvannaNativeLib.nativeGetEffectAmounts()
            if (amounts != null && amounts.size > index) amounts[index] else null
        }.getOrNull()
    }

    /** "0.7" / "1" / "0.5" — sin ceros de más, para hablarlo. */
    private fun fmt(v: Float): String =
        "%.2f".format(v).trimEnd('0').trimEnd('.').ifEmpty { "0" }

    /** "+1.5 dB" / "0 dB" / "−1 dB" — con signo, para hablarlo. */
    private fun fmtDb(db: Float): String {
        val s = "%.1f".format(db).trimEnd('0').trimEnd('.').ifEmpty { "0" }
        return (if (db > 0f) "+" else "") + s + " dB"
    }

    /** ORBE / HOLOS: por el puente fusionado (saveFusedControls +
     * safeSetFusedControls: Ruta A por JNI + Ruta B por SET). Unificación
     * 2.5.509 (paso 1): ya NO se escribe por setOrbeParams/setHolosParams
     * ni por los JNI legacy (el motor legacy del proceso app está muerto
     * en la práctica; el vivo es la cadena fusionada). */
    private fun applyAmount(context: Context, command: VoiceSettingCommand, isOrbe: Boolean): VoiceSettingResult {
        val store = ParameterStore(context)
        val name = if (isOrbe) "ORBE" else "HOLOS"
        // Unificación 2.5.509 (paso 1): el onValue de HOLOS para ENABLE
        // pasa de 0.5f a 1.0f (ATREVIDA 502, directiva Esse 2026-11-24).
        val onValue = 1.0f
        // Unificación 2.5.509 (paso 1): el amount actual se lee de la
        // fuente única (loadFusedControls); los getters legacy delegan
        // ahí, pero aquí se afirma la fuente, no el delegado.
        val fusedNow = store.loadFusedControls()
        val current = if (isOrbe) fusedNow.spatial.amount else fusedNow.field.amount
        val target: Float = when (command.action) {
            Action.SET -> command.value ?: current
            Action.RAISE -> (current + (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.LOWER -> (current - (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.ENABLE -> onValue
            Action.DISABLE -> 0f
            else -> return VoiceSettingResult(false, "No sé cómo aplicar esa orden a $name.", command.target, command.action)
        }
        return runCatching {
            val fused = store.loadFusedControls()
            val updated = if (isOrbe) {
                fused.copy(spatial = fused.spatial.copy(amount = target))
            } else {
                fused.copy(field = fused.field.copy(amount = target))
            }
            store.saveFusedControls(updated)
            com.ivanna.omega.audio.FusedControlsBridge.safeSetFusedControls(updated)
            // P2 (2.5.461): re-evaluar "todo apagado" tras persistir el amount.
            EffectsOffTransition.evaluate(context)
            // READBACK: lo que el motor tiene de verdad (no lo pedido).
            // Sin librería (modo offline) no hay lectura: se persiste y
            // se dice "guardado", nunca que ya suena.
            val readback = readEffectAmount(if (isOrbe) 0 else 1)
            val confirmation = when {
                readback == null && !IvannaNativeLib.isLoaded ->
                    "$name a ${fmt(target)} (guardado; se aplica al arrancar el motor)."
                readback == null ->
                    "$name a ${fmt(target)} (guardado; no pude leer el motor)."
                kotlin.math.abs(readback - target) > 0.02f ->
                    "$name: pedí ${fmt(target)} y el motor quedó en ${fmt(readback)}."
                command.action == Action.DISABLE -> "$name apagado (motor)."
                command.action == Action.ENABLE -> "$name encendido (${fmt(readback)}, motor)."
                command.action == Action.RAISE || command.action == Action.LOWER ->
                    "$name a ${fmt(readback)} (antes ${fmt(current)}, motor)."
                else -> "$name a ${fmt(readback)} (motor)."
            }
            Log.i(TAG, "Voz: $name -> pedido ${fmt(target)}, motor ${readback?.let { fmt(it) } ?: "sin lectura"} (antes ${fmt(current)})")
            VoiceSettingResult(true, confirmation, command.target, command.action)
        }.getOrElse {
            Log.w(TAG, "Voz: no se pudo aplicar $name: ${it.message}")
            VoiceSettingResult(false, "No pude cambiar $name ahora mismo.", command.target, command.action)
        }
    }

    /** Φ: NPE setPhi (runtime; false si el motor no está listo). */
    private fun applyPhi(command: VoiceSettingCommand): VoiceSettingResult {
        val current = IvannaNpeEngine.getPhi()
        val target: Float = when (command.action) {
            Action.SET -> command.value ?: current
            Action.RAISE -> (current + (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.LOWER -> (current - (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.ENABLE -> 0.5f // valor de la directiva
            Action.DISABLE -> 0f
            else -> return VoiceSettingResult(false, "No sé cómo aplicar esa orden a Φ.", command.target, command.action)
        }
        val ok = runCatching { IvannaNpeEngine.setPhi(target) }.getOrDefault(false)
        return if (ok) {
            // READBACK: getPhi() es el valor que el motor aplicó (la
            // caché se actualiza SOLO si el nativo aceptó; no existe un
            // getter nativo de Φ, y se documenta, no se finge).
            val after = IvannaNpeEngine.getPhi()
            val confirmation = when {
                kotlin.math.abs(after - target) > 0.001f ->
                    "Φ: pedí ${fmt(target)} y el motor quedó en ${fmt(after)}."
                command.action == Action.DISABLE -> "Φ apagado."
                command.action == Action.ENABLE -> "Φ encendido (${fmt(after)})."
                command.action == Action.RAISE || command.action == Action.LOWER ->
                    "Φ a ${fmt(after)} (antes ${fmt(current)})."
                else -> "Φ a ${fmt(after)}."
            }
            Log.i(TAG, "Voz: Φ -> ${fmt(after)} (antes ${fmt(current)})")
            VoiceSettingResult(true, confirmation, command.target, command.action)
        } else {
            VoiceSettingResult(
                false,
                "El motor neural (Φ) no está listo ahora mismo; no pude cambiarlo. Inténtalo cuando suene música por la app.",
                command.target, command.action
            )
        }
    }

    /** Volterra: selector de kernel + on/off. */
    private fun applyVolterra(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        val store = ParameterStore(context)
        return when (command.action) {
            Action.SELECT_KERNEL -> {
                val kernelId = command.kernelId
                    ?: return VoiceSettingResult(false, "No sé qué kernel Volterra es ese.", command.target, command.action)
                val label = if (kernelId == IvannaNativeLib.VOLTERRA_KERNEL_GENEZI) "GENEZI" else "PZ1D refinado"
                val rc = runCatching { IvannaNativeLib.setVolterraKernel(context, kernelId) }.getOrDefault(-1)
                if (rc == 0) {
                    // READBACK: el kernel que suena de verdad en Ruta A.
                    val active = runCatching { IvannaNativeLib.activeVolterraKernelId(context) }.getOrDefault("")
                    Log.i(TAG, "Voz: kernel Volterra -> pedido $label, motor $active")
                    if (active == kernelId) {
                        VoiceSettingResult(true, "Kernel Volterra: $label (motor).", command.target, command.action)
                    } else {
                        VoiceSettingResult(
                            true,
                            "Kernel Volterra: pedí $label y el motor tiene " +
                                if (active.isEmpty()) "ninguno." else "$active.",
                            command.target, command.action
                        )
                    }
                } else {
                    VoiceSettingResult(false, "No pude cambiar el kernel Volterra (código $rc).", command.target, command.action)
                }
            }
            Action.ENABLE -> {
                VolterraSwitch.enabled = true
                store.setVolterraEnabled(true)
                // P2 (2.5.461): re-evaluar "todo apagado" tras cambiar Volterra.
                EffectsOffTransition.evaluate(context)
                VoiceSettingResult(true, "Volterra encendido.", command.target, command.action)
            }
            Action.DISABLE -> {
                VolterraSwitch.enabled = false
                store.setVolterraEnabled(false)
                // P2 (2.5.461): re-evaluar "todo apagado" tras cambiar Volterra.
                EffectsOffTransition.evaluate(context)
                VoiceSettingResult(true, "Volterra apagado.", command.target, command.action)
            }
            else -> VoiceSettingResult(false, "No sé cómo aplicar esa orden a Volterra.", command.target, command.action)
        }
    }

    /** ISO 226: on = applyAll con los phon guardados; off = compensación cero. */
    private fun applyIso226(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        val store = ParameterStore(context)
        val app = context.applicationContext as? IVANNAApplication
        val manager = app?.globalEffectManager
        return when (command.action) {
            Action.ENABLE -> {
                val listen = store.loadIso226ListenPhon()
                val ref = store.loadIso226RefPhon()
                val ok = runCatching {
                    if (manager != null) {
                        val result = Iso226Calibrator.applyAll(context, listen, ref, manager)
                        store.saveIso226(listen, ref, result.gains)
                        result.anyApplied
                    } else {
                        // Sin EffectManager no hay capa EQ; la curva sí llega
                        // al DSPBridge (nativo) con los gains calculados.
                        val gains = Iso226Calibrator.computeCompensation(listen, ref)
                        Iso226Calibrator.applyToDSPBridge(context, gains)
                        store.saveIso226(listen, ref, gains)
                        true
                    }
                }.getOrDefault(false)
                if (ok) VoiceSettingResult(true, "ISO 226 encendido (${listen.toInt()}→${ref.toInt()} phon).", command.target, command.action)
                else VoiceSettingResult(false, "No pude encender ISO 226 ahora mismo.", command.target, command.action)
            }
            Action.DISABLE -> {
                val ref = store.loadIso226RefPhon()
                val ok = runCatching {
                    if (manager != null) {
                        // listenPhon == refPhon → compensación cero en las
                        // tres capas (EQ + DSPBridge + daemon).
                        val result = Iso226Calibrator.applyAll(context, ref, ref, manager)
                        result.anyApplied
                    } else {
                        Iso226Calibrator.applyToDSPBridge(context, FloatArray(10))
                        true
                    }
                }.getOrDefault(false)
                store.clearIso226()
                if (ok) VoiceSettingResult(true, "ISO 226 apagado (compensación a cero).", command.target, command.action)
                else VoiceSettingResult(false, "No pude apagar ISO 226 ahora mismo.", command.target, command.action)
            }
            else -> VoiceSettingResult(false, "No sé cómo aplicar esa orden a ISO 226.", command.target, command.action)
        }
    }

    /** Ω: on/off + equipo (elegir equipo implica encender Ω). */
    private fun applyOmega(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        val store = ParameterStore(context)
        return runCatching {
            when (command.action) {
                Action.ENABLE -> {
                    store.setOmegaParams(true, store.getOmegaEquipment(), store.getOmegaBoostMaxDb(), store.isOmegaIldPrecomp())
                    OmegaEquipment.applyToNative(store)
                    VoiceSettingResult(true, "Ω encendido (${OmegaEquipment.label(store.getOmegaEquipment())}).", command.target, command.action)
                }
                Action.DISABLE -> {
                    store.setOmegaParams(false, store.getOmegaEquipment(), store.getOmegaBoostMaxDb(), store.isOmegaIldPrecomp())
                    OmegaEquipment.applyToNative(store)
                    VoiceSettingResult(true, "Ω apagado.", command.target, command.action)
                }
                Action.SELECT_EQUIPMENT -> {
                    val equipment = command.equipment ?: OmegaEquipment.AUTO
                    store.setOmegaParams(true, equipment, store.getOmegaBoostMaxDb(), store.isOmegaIldPrecomp())
                    OmegaEquipment.applyToNative(store)
                    VoiceSettingResult(true, "Ω encendido en ${OmegaEquipment.label(equipment)}.", command.target, command.action)
                }
                else -> VoiceSettingResult(false, "No sé cómo aplicar esa orden a Ω.", command.target, command.action)
            }
        }.getOrElse {
            Log.w(TAG, "Voz: no se pudo aplicar Ω: ${it.message}")
            VoiceSettingResult(false, "No pude cambiar Ω ahora mismo.", command.target, command.action)
        }
    }

    /** Graves del sensor: trim ±3 dB (el control musical apropiado para
     *  "sube el grave": NO es ORBE). Persiste + empuja al sensor en vivo.
     *  Sin getter nativo del trim: el booleano del setter + el valor
     *  persistido son la evidencia; sin sensor activo, "guardado". */
    private fun applyBass(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        val store = ParameterStore(context)
        val current = store.getSensorBassTrimDb()
        val target: Float = when (command.action) {
            Action.SET -> command.value ?: current
            Action.RAISE -> (current + (command.value ?: 0.5f)).coerceIn(-3f, 3f)
            Action.LOWER -> (current - (command.value ?: 0.5f)).coerceIn(-3f, 3f)
            Action.ENABLE -> 1.0f
            Action.DISABLE -> 0f
            else -> return VoiceSettingResult(false, "No sé cómo aplicar esa orden al grave.", command.target, command.action)
        }
        store.setSensorBassTrimDb(target)
        val ok = runCatching { RealAcousticSensor.setBassTrimDb(target) }.getOrDefault(false)
        return when {
            ok -> {
                Log.i(TAG, "Voz: grave -> ${fmtDb(target)} (antes ${fmtDb(current)}, motor)")
                VoiceSettingResult(true, "Grave a ${fmtDb(target)} (motor).", command.target, command.action)
            }
            RealAcousticSensor.isRunning -> {
                VoiceSettingResult(false, "No pude cambiar el grave (el sensor no respondió).", command.target, command.action)
            }
            else -> {
                Log.i(TAG, "Voz: grave -> ${fmtDb(target)} (guardado, sensor inactivo)")
                VoiceSettingResult(true, "Grave a ${fmtDb(target)} (guardado; se aplica cuando el sensor esté activo).", command.target, command.action)
            }
        }
    }

    /** Volumen maestro 0..1: persiste; lo lee la ruta de audio. */
    private fun applyVolume(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        val store = ParameterStore(context)
        val current = store.getMasterVolume()
        val target: Float = when (command.action) {
            Action.SET -> command.value ?: current
            Action.RAISE -> (current + (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.LOWER -> (current - (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.ENABLE -> 0.85f
            Action.DISABLE -> 0f
            else -> return VoiceSettingResult(false, "No sé cómo aplicar esa orden al volumen.", command.target, command.action)
        }
        store.setMasterVolume(target)
        val after = store.getMasterVolume()
        Log.i(TAG, "Voz: volumen -> ${fmt(after)} (antes ${fmt(current)})")
        return VoiceSettingResult(true, "Volumen a ${fmt(after)}.", command.target, command.action)
    }

    /** Corrección de sala en vivo del sensor: persist + setters + readback. */
    private fun applyRoomCorrection(context: Context, command: VoiceSettingCommand): VoiceSettingResult {
        val store = ParameterStore(context)
        val currentAmount = store.getSensorRoomCorrectionAmount()
        val wasEnabled = store.isSensorRoomCorrectionEnabled()
        val (targetEnabled, targetAmount) = when (command.action) {
            Action.SET -> true to (command.value ?: currentAmount)
            Action.RAISE -> true to (currentAmount + (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.LOWER -> true to (currentAmount - (command.value ?: 0.1f)).coerceIn(0f, 1f)
            Action.ENABLE -> true to currentAmount
            Action.DISABLE -> false to currentAmount
            else -> return VoiceSettingResult(false, "No sé cómo aplicar esa orden a la corrección de sala.", command.target, command.action)
        }
        store.setSensorRoomCorrectionEnabled(targetEnabled)
        store.setSensorRoomCorrectionAmount(targetAmount)
        runCatching {
            RealAcousticSensor.setRoomCorrectionAmount(targetAmount)
            RealAcousticSensor.setRoomCorrectionEnabled(targetEnabled)
        }
        if (!IvannaNativeLib.isLoaded) {
            return VoiceSettingResult(
                true,
                "Corrección de sala ${if (targetEnabled) "encendida (${fmt(targetAmount)})" else "apagada"} " +
                    "(guardado; se aplica al arrancar el sensor).",
                command.target, command.action
            )
        }
        // READBACK: [enabled, amount, frame válido, coherencia media].
        val status = runCatching { RealAcousticSensor.roomCorrectionStatus() }.getOrNull()
        val readEnabled = status != null && status.size > 0 && status[0] != 0f
        val readAmount = if (status != null && status.size > 1) status[1] else null
        return when {
            readEnabled != targetEnabled ->
                VoiceSettingResult(
                    true,
                    "Corrección de sala: pedí ${if (targetEnabled) "encendida" else "apagada"} " +
                        "y el motor quedó ${if (readEnabled) "encendida" else "apagada"}.",
                    command.target, command.action
                )
            readAmount != null && kotlin.math.abs(readAmount - targetAmount) > 0.02f ->
                VoiceSettingResult(
                    true,
                    "Corrección de sala: pedí ${fmt(targetAmount)} y el motor quedó en ${fmt(readAmount)}.",
                    command.target, command.action
                )
            targetEnabled ->
                VoiceSettingResult(true, "Corrección de sala encendida (${fmt(targetAmount)}, motor).", command.target, command.action)
            else ->
                VoiceSettingResult(true, "Corrección de sala apagada (motor).", command.target, command.action)
        }.also {
            Log.i(TAG, "Voz: corrección de sala -> enabled=$targetEnabled amount=${fmt(targetAmount)} (antes enabled=$wasEnabled amount=${fmt(currentAmount)})")
        }
    }
}
