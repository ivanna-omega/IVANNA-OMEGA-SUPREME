package com.ivanna.omega.assistant

/**
 * OpenQuestionRouter — v2.5.445.
 *
 * Decide si un texto del usuario es una PREGUNTA ABIERTA (conocimiento /
 * explicación) y por tanto debe ir al LLM — OmniRoute si el flag está
 * prendido, si no Gemini — o si es otra cosa (orden DSP, acción local,
 * saludo, reporte, diagnóstico) y sigue el flujo local de siempre.
 *
 * HALLAZGO (2026-11-14, verificado en el teléfono con la 444): la pregunta
 * "¿Qué es la holofonía y por qué se siente diferente al estéreo normal?"
 * recibió la frase enlatada "He dejado el sonido neutro, sin coloración".
 * Causa: el clasificador local (IvannaLanguageCore) intercepta por
 * substring — la palabra "normal" de "al estéreo normal" mapea a
 * FLAT_NEUTRAL (confianza 0.97) — y su respuesta enlatada pisaba la del
 * LLM. Este router corre ANTES que el clasificador local y las preguntas
 * abiertas ya no lo tocan.
 *
 * Reglas:
 *  - Pregunta abierta → true. El llamador la manda al LLM y, si el LLM
 *    falla, responde con el mensaje honesto (LLM_UNAVAILABLE_MESSAGE /
 *    LLM_EMPTY_MESSAGE / llmErrorMessage) — NUNCA una frase enlatada como
 *    si respondiera, y NUNCA ejecuta un comando DSP por una pregunta.
 *  - Orden DSP ("pon el ORBE a 1.0", "sube el HOLOS", "apaga el Phi") →
 *    false. Esas las resuelve VoiceSettingsControl, no el LLM.
 *  - Acciones locales (perfiles, reporte, diagnóstico, auto-reparación,
 *    saludo, chiste, presentación) → false. Siguen su rama local.
 *
 * Puro y determinista (sin Android, sin red): testeable con JUnit.
 */
object OpenQuestionRouter {

    /** El LLM no está disponible: decirlo claro, no inventar respuesta. */
    const val LLM_UNAVAILABLE_MESSAGE =
        "No pude conectar con la inteligencia en la nube para responder tu pregunta. " +
            "Revisa la conexión (OmniRoute o Gemini) y vuelve a intentarlo."

    /** El LLM respondió vacío: decirlo, no rellenar con una frase enlatada. */
    const val LLM_EMPTY_MESSAGE =
        "La inteligencia en la nube respondió vacía. Inténtalo de nuevo."

    /** El LLM falló con un error concreto: decirlo con el detalle. */
    fun llmErrorMessage(detail: String): String =
        "La inteligencia en la nube falló al responder ($detail). Inténtalo de nuevo."

    // ── Exclusiones: cosas que PARECEN pregunta pero son acción local ──────
    // Se evalúan ANTES que los patrones de pregunta: una orden o una acción
    // local nunca debe ir al LLM como pregunta abierta.

    /** Verbos de orden DSP (imperativo). */
    private val COMMAND_VERBS = listOf(
        "pon", "sube", "baja", "apaga", "enciende", "activa", "desactiva",
        "cambia", "quita", "aumenta", "reduce", "ajusta", "configura",
        "prende", "ponlo", "dejalo", "deja"
    )

    /** Objetivos DSP que, junto a un verbo de orden, forman un comando. */
    private val DSP_TARGETS = listOf(
        "orbe", "holos", "phi", "volterra", "kernel", "omega", "iso",
        "volumen", "graves", "agudos", "modo", "sonido", "musica"
    )

    /** Frases de acción local (perfiles, reporte, diagnóstico, reparación,
     *  saludo, chiste, presentación). Si el texto las contiene, no es una
     *  pregunta abierta: sigue su rama local. */
    private val LOCAL_ACTION_PHRASES = listOf(
        "que perfiles", "que presets", "que configuraciones",
        "muestrame los presets", "muestrame los perfiles",
        "que hiciste", "que cambiaste", "muestrame que", "reporte",
        "informe", "resumen de lo que", "como quedo",
        "como estas", "estado del sistema", "diagnostico",
        "reparate", "arreglalo", "arregla todo", "optimiza", "mejorate",
        "quien eres", "como te llamas", "cual es tu nombre",
        "como estas tu", "que tal estas", "como te sientes",
        "hola", "buenos dias", "buenas tardes", "buenas noches",
        "un chiste", "cuentame un chiste", "hazme reir",
        "gracias"
    )

    /** "por qué" sobre la última decisión → EXPLAIN local, no pregunta. */
    private val EXPLAIN_WORDS = listOf("cambiaste", "hiciste", "pusiste", "quitaste", "decidiste")

    // ── Patrones de pregunta abierta ─────────────────────────────────────────

    /** Patrones de conocimiento / explicación (cualquier coincidencia). */
    private val QUESTION_PHRASES = listOf(
        "que es", "que significa", "que son", "que quiere decir",
        "por que", "como funciona", "como se", "como suena", "como se usa",
        "como se hace", "como es", "como se siente", "como se escucha",
        "cual es la diferencia", "que diferencia hay",
        "en que se diferencia", "en que se diferencian",
        "para que sirve", "que hace",
        "que pasa si", "que pasaria si",
        "que piensas de", "que opinas de", "cual es tu opinion",
        "que sabes de", "sabes algo de"
    )

    /** Verbos de pedir explicación: solo cuentan con palabra interrogativa
     *  después ("explícame qué es X"); solos son EXPLAIN local. */
    private val EXPLAIN_VERBS = listOf("explica", "explicame", "dime", "hablame", "cuentame")
    private val QUESTION_WORDS_AFTER_VERB = listOf("que", "por que", "como", "cual", "de", "sobre")

    /** Peticiones de ANÁLISIS / evaluación (2026-11-14, reporte del
     *  propietario con captura): "analiza" NO es una orden DSP ni una
     *  pregunta con "qué es" — es un pedido abierto de analizar el
     *  sonido, la música o el sistema. El clasificador local lo marcaba
     *  TECHNICAL_QUESTION y el panel mostraba "IvannaStaticCore
     *  (bloqueó)" con la respuesta enlatada de la base local, aunque el
     *  LLM estuviera disponible. Estas peticiones van al LLM: es él
     *  quien analiza y explica; si el LLM no está, se dice honesto
     *  (LLM_UNAVAILABLE_MESSAGE), nunca una frase local como si
     *  respondiera. Se evalúan DESPUÉS de las exclusiones (una orden
     *  DSP real nunca es análisis) y ANTES de devolver false. */
    private val ANALYSIS_PHRASES = listOf(
        "analiza", "analiza el", "analiza la", "analiza este", "analiza esta",
        "evalua", "evaluame", "revisa", "examina", "diagnostica",
        "que te parece", "como esta el sonido", "como suena", "como va el sonido",
        "como esta sonando", "que tal suena", "como se escucha"
    )

    /** Devuelve true si el texto es una pregunta abierta → al LLM. */
    fun isOpenQuestion(rawText: String): Boolean {
        val t = normalize(rawText)
        if (t.isBlank()) return false

        // 1. Orden DSP: verbo de orden + objetivo DSP → comando, no pregunta.
        val hasCommandVerb = COMMAND_VERBS.any { verb -> hasWord(t, verb) }
        val hasDspTarget = DSP_TARGETS.any { target -> t.contains(target) }
        if (hasCommandVerb && hasDspTarget) return false

        // 2. Acción local explícita → sigue su rama, no al LLM.
        if (LOCAL_ACTION_PHRASES.any { t.contains(it) }) return false

        // 3. "por qué" sobre la última decisión → EXPLAIN local.
        if (t.contains("por que") && EXPLAIN_WORDS.any { t.contains(it) }) return false

        // 3b. Petición de análisis / evaluación → al LLM.
        if (ANALYSIS_PHRASES.any { t.contains(it) }) return true

        // 4. Patrones de pregunta abierta.
        if (QUESTION_PHRASES.any { t.contains(it) }) return true

        // 5. Verbo de explicación + palabra interrogativa.
        for (verb in EXPLAIN_VERBS) {
            val idx = t.indexOf(verb)
            if (idx < 0) continue
            val rest = t.substring(idx + verb.length).trim()
            if (QUESTION_WORDS_AFTER_VERB.any { rest.startsWith(it) }) return true
        }

        return false
    }

    /** Minúsculas + sin acentos + espacios colapsados (para matching robusto). */
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

    /** Coincidencia por palabra completa (evita que "pon" case con "poner"). */
    private fun hasWord(text: String, word: String): Boolean {
        val regex = Regex("(^|\\s)" + Regex.escape(word) + "(\\s|$|[.,!?¿¡])")
        return regex.containsMatchIn(text)
    }
}
