package com.ivanna.omega.organism

import com.google.gson.Gson
import com.google.gson.JsonArray
import com.google.gson.JsonElement
import com.google.gson.JsonObject
import com.google.gson.JsonParser
import com.google.gson.JsonSyntaxException
import java.io.File

/**
 * ProfileApplier — validación fail-closed de perfiles `.ivanna.json`
 * descargados (C2, release 510; bloqueante de la VUELTA).
 *
 * Un perfil malicioso, corrupto o con un error de compilación del servidor
 * nunca debe llegar al motor, aunque el servidor esté comprometido o el
 * token robado. El clamp de runtime (±18 dB) no protege: aquí se valida
 * con los rangos del esquema, antes de cualquier aplicación.
 *
 * Espejo en Kotlin de `scripts/validate_ivanna_profile.py`, adaptado a la
 * forma de sesión en vivo (refactorización §3.2 paso 1): los campos de
 * medición de archivo (loudness, peak, crest, espectro por banda, estéreo)
 * son opcionales para un perfil de sesión en vivo, y los campos ERB
 * (`mean_coherence`, `spectral_error_rms_db`, `erb_bands_measured`,
 * `detected_issues`) son obligatorios — los mismos 4 que exige el modo
 * `--live-session` de `scripts/validate_ivanna_profile.py` (S4).
 *
 * Puro Kotlin + Gson + java.io.File, sin dependencias de Android (misma
 * disciplina que [OrganismQueue]): la validación es una función pura y
 * testeable en JVM. La aplicación al DSP NO está implementada aquí
 * (eso es T1): [apply] valida, pone en cuarentena lo inválido y devuelve
 * [ApplyResult.Validated] para que T1 traduzca e instale.
 *
 * Limitaciones conocidas de Gson, documentadas no escondidas:
 * - Gson es permisivo al parsear (`NaN`/`Infinity` literales, `1e999`):
 *   se compensan comprobando `isFinite()` valor por valor y con un
 *   recorrido de todo el árbol antes de cualquier otra comprobación.
 * - Claves JSON duplicadas: Gson conserva la última sin avisar; el
 *   perfil se valida sobre el valor superviviente.
 */
class ProfileApplier(
    private val dir: File,
    private val gson: Gson = Gson(),
    private val clockMs: () -> Long = System::currentTimeMillis
) {

    // ------------------------------------------------------------------
    // Resultados: sellados, nunca una excepción silenciosa.
    // ------------------------------------------------------------------

    sealed class ValidationResult {
        /** Perfil válido; [profile] lleva los valores ya parseados para T1. */
        data class Valid(val profile: ValidatedProfile) : ValidationResult()

        /** Perfil inválido; [reasons] lista TODOS los motivos, no solo el primero. */
        data class Invalid(val reasons: List<String>) : ValidationResult()
    }

    sealed class ApplyResult {
        /** Validado. T1 traduce a `IvannaAudioProfile` e instala/aplica. */
        data class Validated(val profile: ValidatedProfile) : ApplyResult()

        /** Rechazado y puesto en cuarentena ([quarantineFile]); nunca aplicado. */
        data class Rejected(
            val validation: ValidationResult.Invalid,
            val quarantineFile: File
        ) : ApplyResult()
    }

    /** Estado visible de la última cuarentena (lo lee T1 para el `State`). */
    data class QuarantineStatus(
        val version: Long,
        val reasons: List<String>,
        val fileName: String,
        val atMs: Long
    )

    // ------------------------------------------------------------------
    // Punto de entrada para T1.
    // ------------------------------------------------------------------

    /**
     * Puerta obligatoria antes de cualquier aplicación: valida y, si el
     * perfil es inválido, lo pone en cuarentena y devuelve [ApplyResult.Rejected].
     * Si es válido, devuelve [ApplyResult.Validated] SIN tocar el DSP ni
     * el perfil estable: la traducción (`IvannaAudioProfile`), el
     * `saveOverride()` y el `applyProfile("muselink-live", …)` son T1.
     * El último bueno queda intacto en todo caso.
     */
    fun apply(
        body: String,
        version: Long,
        appliedVersion: Long?,
        rolledBack: Boolean = false,
        previous: ValidatedProfile? = null
    ): ApplyResult {
        return when (val result = validate(body, version, appliedVersion, rolledBack, previous)) {
            is ValidationResult.Valid -> ApplyResult.Validated(result.profile)
            is ValidationResult.Invalid -> {
                val file = quarantine(body, version, result.reasons)
                ApplyResult.Rejected(result, file)
            }
        }
    }

    // ------------------------------------------------------------------
    // Cuarentena (C2.h): misma disciplina que OrganismQueue.markQuarantined.
    // ------------------------------------------------------------------

    /**
     * Perfil inválido → archivo en cuarentena + estado visible, nunca
     * aplicado. Escritura atómica tmp+rename (patrón de
     * `OrganismQueue.writeItem`). No toca el perfil estable ni el
     * candidato: solo añade el archivo de cuarentena y el estado.
     * Si el body supera 1 MB no se guarda el cuerpo (riesgo de llenar el
     * disco con un body hostil); el estado conserva los motivos.
     */
    fun quarantine(body: String, version: Long, reasons: List<String>): File {
        dir.mkdirs()
        val atMs = clockMs()
        val file = File(dir, quarantineFileName(version, atMs))
        val stored = if (body.toByteArray(Charsets.UTF_8).size <= MAX_QUARANTINE_BODY_BYTES) {
            body
        } else {
            "{ \"quarantined\": true, \"note\": \"body mayor de 1 MB, no guardado\" }"
        }
        writeAtomic(file, stored)
        val status = JsonObject().apply {
            addProperty("version", version)
            addProperty("fileName", file.name)
            addProperty("atMs", atMs)
            val arr = JsonArray()
            reasons.forEach { arr.add(it) }
            add("reasons", arr)
        }
        writeAtomic(File(dir, QUARANTINE_STATUS_FILE_NAME), gson.toJson(status))
        return file
    }

    /** Última cuarentena registrada, o null si nunca hubo una. */
    fun readQuarantineStatus(): QuarantineStatus? {
        val file = File(dir, QUARANTINE_STATUS_FILE_NAME)
        if (!file.isFile) return null
        return runCatching {
            val obj = gson.fromJson(file.readText(), JsonObject::class.java)
            val reasons = obj.getAsJsonArray("reasons")?.map { it.asString } ?: emptyList()
            QuarantineStatus(
                version = obj.get("version").asLong,
                reasons = reasons,
                fileName = obj.get("fileName").asString,
                atMs = obj.get("atMs").asLong
            )
        }.getOrNull()
    }

    private fun writeAtomic(dest: File, text: String) {
        val tmp = File(dir, "${dest.name}.tmp")
        tmp.writeText(text)
        if (!tmp.renameTo(dest)) {
            tmp.copyTo(dest, overwrite = true)
            tmp.delete()
        }
    }

    companion object {
        /** (a) Tope de tamaño: un `.ivanna.json` legítimo pesa ~2 KB. */
        const val MAX_BODY_BYTES = 64 * 1024

        /** Tope del cuerpo que se guarda en cuarentena (anti-llenado de disco). */
        const val MAX_QUARANTINE_BODY_BYTES = 1024 * 1024

        const val FORMAT = "IVANNA OMEGA"
        const val FORMAT_VERSION = "1.0"

        /** (d)(g) Tolerancia de la ganancia neta cero: ±0.01 dB. */
        const val NET_GAIN_TOLERANCE_DB = 0.01

        /** (g) Salto máximo de un operador entre versiones: 0.2. */
        const val MAX_OPERATOR_STEP = 0.2

        /** Nombre del último perfil bueno (destino del rollback; T1 lo gestiona). */
        const val STABLE_FILE_NAME = "muselink-profile-stable.ivanna.json"

        const val QUARANTINE_STATUS_FILE_NAME = "muselink-profile-quarantine.json"

        fun candidateFileName(version: Long): String = "muselink-profile-$version.ivanna.json"

        fun quarantineFileName(version: Long, atMs: Long): String =
            "muselink-profile-quarantine-v$version-$atMs.ivanna.json"

        /**
         * Validación pura y testeable (C2.a–g). No toca disco ni Android.
         *
         * @param body JSON crudo del `.ivanna.json` descargado.
         * @param version versión del sobre (`GET /profile/current`): el
         *   `.ivanna.json` NO lleva `version` dentro; si la llevara, se
         *   rechaza como clave desconocida. La versión vive en el sobre.
         * @param appliedVersion versión aplicada actualmente, o null si
         *   aún no hay ninguna.
         * @param rolledBack true solo si el servidor marcó el perfil
         *   `rolled_back` (única vía para aceptar una versión menor).
         * @param previous perfil válido aplicado anteriormente, o null;
         *   solo se usa para el límite de salto de operadores (C2.g).
         */
        fun validate(
            body: String,
            version: Long,
            appliedVersion: Long?,
            rolledBack: Boolean = false,
            previous: ValidatedProfile? = null
        ): ValidationResult = ProfileValidator(body, version, appliedVersion, rolledBack, previous).run()
    }
}

/** Valores ya parseados y validados de un perfil; T1 los traduce sin re-parsear. */
data class EqBandsDb(
    val low60_120: Double,
    val mid500_1k: Double,
    val high2k_4k: Double,
    val presence4k_8k: Double
) {
    fun sumDb(): Double = low60_120 + mid500_1k + high2k_4k + presence4k_8k
}

data class CompressorParams(
    val thresholdDb: Double,
    val ratio: Double,
    val attackMs: Double,
    val releaseMs: Double
)

data class ErbBand(val coherence: Double, val errorDb: Double)

data class ValidatedProfile(
    val version: Long,
    val track: String,
    val artist: String,
    val source: String,
    val durationS: Double,
    val liveSession: Boolean,
    val meanCoherence: Double,
    val spectralErrorRmsDb: Double,
    val erbBandsMeasured: Map<String, ErbBand>,
    val eqBandsDb: EqBandsDb,
    val trimDb: Double,
    val netGainDb: Double,
    val computedNetGainDb: Double,
    val orbeAmount: Double,
    val orbeDistanceM: Double,
    val holosAmount: Double,
    val phiAmount: Double,
    val exciterAmount: Double,
    val volterraEnabled: Boolean,
    val volterraMix: Double,
    val compressor: CompressorParams,
    val structuralSplitTransientAmount: Double,
    val structuralSplitTonalAmount: Double
)

/**
 * Validador interno. Acumula TODOS los motivos de rechazo en [reasons]
 * (como el validador Python, que lista cada error) en vez de parar en
 * el primero: un perfil rechazado debe decir por qué, completo.
 */
private class ProfileValidator(
    private val body: String,
    private val version: Long,
    private val appliedVersion: Long?,
    private val rolledBack: Boolean,
    private val previous: ValidatedProfile?
) {
    private val reasons = mutableListOf<String>()

    private fun err(path: String, msg: String) {
        reasons.add("$path: $msg")
    }

    fun run(): ProfileApplier.ValidationResult {
        // (a) Tope de tamaño.
        if (body.toByteArray(Charsets.UTF_8).size > ProfileApplier.MAX_BODY_BYTES) {
            return ProfileApplier.ValidationResult.Invalid(
                listOf("\$: body supera ${ProfileApplier.MAX_BODY_BYTES} bytes (tope 64 KB)")
            )
        }
        val root = try {
            JsonParser.parseString(body)
        } catch (e: JsonSyntaxException) {
            return ProfileApplier.ValidationResult.Invalid(listOf("\$: JSON inválido (${e.message})"))
        }
        if (!root.isJsonObject) {
            return ProfileApplier.ValidationResult.Invalid(listOf("\$: la raíz debe ser un objeto"))
        }
        val data = root.asJsonObject

        // (b) No-finitos valor por valor + campos prohibidos, en todo el árbol.
        checkFinite(root, "\$")
        checkForbiddenKeys(root, "\$")

        // (b) Forma estricta de la raíz.
        checkKeys(
            data, ROOT_KEYS, ROOT_KEYS, "\$",
            "claves obligatorias del schema v1.0"
        )
        if (data.get("format")?.takeIf { it.isJsonPrimitive }?.asString != ProfileApplier.FORMAT) {
            err("\$.format", "debe ser exactamente '${ProfileApplier.FORMAT}'")
        }
        if (data.get("format_version")?.takeIf { it.isJsonPrimitive }?.asString != ProfileApplier.FORMAT_VERSION) {
            err("\$.format_version", "debe ser exactamente '${ProfileApplier.FORMAT_VERSION}' (única versión)")
        }
        val track = reqStr(data, "track", "\$")
        val artist = reqStr(data, "artist", "\$")
        val source = reqStr(data, "source", "\$")
        val durationS = reqNum(data, "duration_s", "\$", 0.0, 1e9)

        // (e) duration_s == 0.0 solo con source de sesión en vivo.
        val liveSession = source?.let { isLiveSessionSource(it) } ?: false
        if (durationS != null && durationS == 0.0 && !liveSession) {
            err("\$.duration_s", "0.0 solo se admite con source de sesión en vivo")
        }

        // (e) Fingerprint ERB (variante de sesión en vivo).
        var meanCoherence: Double? = null
        var spectralErrorRmsDb: Double? = null
        var erbBands: Map<String, ErbBand>? = null
        val fp = data.get("fingerprint")
        if (fp == null || !fp.isJsonObject) {
            err("\$.fingerprint", "debe ser un objeto")
        } else {
            val f = fp.asJsonObject
            checkKeys(f, FINGERPRINT_REQUIRED, FINGERPRINT_ALLOWED, "\$.fingerprint", "fingerprint ERB")
            meanCoherence = reqNum(f, "mean_coherence", "\$.fingerprint", 0.0, 1.0)
            // C2/510: cota de cordura [0, 100] dB, igual que el validador
            // Python (scripts/validate_ivanna_profile.py, modo --live-session).
            // La v1.1 trae 10.46 dB; el tope solo rechaza valores imposibles.
            spectralErrorRmsDb = reqNum(f, "spectral_error_rms_db", "\$.fingerprint", 0.0, 100.0)
            erbBands = checkErbBands(f)
            checkDetectedIssues(f)
            checkFileFingerprintFields(f)
        }

        // (b)(c) Engine: claves exactas + rangos del esquema.
        var eq: EqBandsDb? = null
        var trimDb: Double? = null
        var netGainDb: Double? = null
        var orbeAmount: Double? = null
        var orbeDistanceM: Double? = null
        var holosAmount: Double? = null
        var phiAmount: Double? = null
        var exciterAmount: Double? = null
        var volterraEnabled: Boolean? = null
        var volterraMix: Double? = null
        var compressor: CompressorParams? = null
        var splitTransientAmount: Double? = null
        var splitTonalAmount: Double? = null
        val en = data.get("engine")
        if (en == null || !en.isJsonObject) {
            err("\$.engine", "debe ser un objeto")
        } else {
            val e = en.asJsonObject
            checkKeys(e, ENGINE_KEYS, ENGINE_KEYS, "\$.engine", "claves obligatorias de engine")
            orbeAmount = reqNum(e, "orbe_amount", "\$.engine", 0.0, 1.0)
            orbeDistanceM = reqNum(e, "orbe_distance_m", "\$.engine", 0.15, 2.0)
            holosAmount = reqNum(e, "holos_amount", "\$.engine", 0.0, 1.0)
            phiAmount = reqNum(e, "phi_amount", "\$.engine", 0.0, 1.0)
            volterraMix = reqNum(e, "volterra_mix", "\$.engine", 0.0, 1.0)
            exciterAmount = reqNum(e, "exciter_amount", "\$.engine", 0.0, 1.0)
            trimDb = reqNum(e, "trim_db", "\$.engine", -12.0, 12.0)
            netGainDb = reqNum(e, "net_gain_db", "\$.engine", -0.01, 0.01)
            val volterra = e.get("volterra_enabled")
            if (volterra == null || !volterra.isJsonPrimitive || !volterra.asJsonPrimitive.isBoolean) {
                err("\$.engine.volterra_enabled", "debe ser boolean")
            } else {
                volterraEnabled = volterra.asBoolean
            }
            eq = checkEqBands(e)
            compressor = checkCompressor(e)
            val split = checkStructuralSplit(e)
            splitTransientAmount = split?.first
            splitTonalAmount = split?.second
        }

        // (d) Ganancia neta cero verificada en el teléfono.
        var computedNetGain: Double? = null
        if (eq != null && trimDb != null) {
            computedNetGain = eq.sumDb() + trimDb
            if (kotlin.math.abs(computedNetGain) > ProfileApplier.NET_GAIN_TOLERANCE_DB) {
                err(
                    "\$.engine",
                    "ganancia neta calculada $computedNetGain dB (suma EQ ${eq.sumDb()} + trim $trimDb), " +
                        "debe ser 0.0 dB (±${ProfileApplier.NET_GAIN_TOLERANCE_DB})"
                )
            }
        }

        // Derivation: claves obligatorias, strings no vacíos (espejo Python).
        val dv = data.get("derivation")
        if (dv == null || !dv.isJsonObject) {
            err("\$.derivation", "debe ser un objeto")
        } else {
            val d = dv.asJsonObject
            checkKeys(d, DERIVATION_KEYS, DERIVATION_KEYS, "\$.derivation", "claves obligatorias de derivation")
            DERIVATION_KEYS.forEach { reqStr(d, it, "\$.derivation") }
        }

        // (f) Versión monótona.
        if (version < 1L) {
            err("\$version", "la versión del sobre debe ser >= 1")
        }
        if (appliedVersion != null) {
            when {
                version < appliedVersion && !rolledBack ->
                    err("\$version", "versión $version menor que la aplicada $appliedVersion sin rollback explícito")
                version == appliedVersion ->
                    err("\$version", "versión $version ya aplicada (no es más nueva)")
            }
        }

        // (g) Sin saltos de operador de más de 0.2 por versión.
        if (previous != null) {
            checkOperatorStep("orbe_amount", orbeAmount, previous.orbeAmount)
            checkOperatorStep("holos_amount", holosAmount, previous.holosAmount)
            checkOperatorStep("phi_amount", phiAmount, previous.phiAmount)
            checkOperatorStep("exciter_amount", exciterAmount, previous.exciterAmount)
            checkOperatorStep("volterra_mix", volterraMix, previous.volterraMix)
        }

        if (reasons.isNotEmpty()) return ProfileApplier.ValidationResult.Invalid(reasons.toList())
        // Todos los obligatorios están presentes si no hubo motivos.
        return ProfileApplier.ValidationResult.Valid(
            ValidatedProfile(
                version = version,
                track = track!!,
                artist = artist!!,
                source = source!!,
                durationS = durationS!!,
                liveSession = liveSession,
                meanCoherence = meanCoherence!!,
                spectralErrorRmsDb = spectralErrorRmsDb!!,
                erbBandsMeasured = erbBands!!,
                eqBandsDb = eq!!,
                trimDb = trimDb!!,
                netGainDb = netGainDb!!,
                computedNetGainDb = computedNetGain!!,
                orbeAmount = orbeAmount!!,
                orbeDistanceM = orbeDistanceM!!,
                holosAmount = holosAmount!!,
                phiAmount = phiAmount!!,
                exciterAmount = exciterAmount!!,
                volterraEnabled = volterraEnabled!!,
                volterraMix = volterraMix!!,
                compressor = compressor!!,
                structuralSplitTransientAmount = splitTransientAmount!!,
                structuralSplitTonalAmount = splitTonalAmount!!
            )
        )
    }

    // ------------------------------------------------------------------
    // (b) Recorrido del árbol: no-finitos y campos prohibidos.
    // ------------------------------------------------------------------

    private fun checkFinite(el: JsonElement, path: String) {
        when {
            el.isJsonPrimitive -> {
                val p = el.asJsonPrimitive
                if (p.isNumber) {
                    val d = p.asDouble
                    if (!d.isFinite()) err(path, "número no finito ($d): NaN/Infinity no se admiten")
                }
            }
            el.isJsonArray -> el.asJsonArray.forEachIndexed { i, child -> checkFinite(child, "$path[$i]") }
            el.isJsonObject -> el.asJsonObject.entrySet().forEach { (k, v) -> checkFinite(v, "$path.$k") }
        }
    }

    private fun checkForbiddenKeys(el: JsonElement, path: String) {
        if (!el.isJsonObject) return
        el.asJsonObject.entrySet().forEach { (k, v) ->
            if (k.lowercase() in FORBIDDEN_KEYS) {
                err("$path.$k", "campo prohibido en un perfil descargado (red/permisos/token/rutas)")
            }
            checkForbiddenKeys(v, "$path.$k")
        }
    }

    // ------------------------------------------------------------------
    // Ayudas de estructura y tipos.
    // ------------------------------------------------------------------

    private fun checkKeys(obj: JsonObject, required: Set<String>, allowed: Set<String>, path: String, what: String) {
        required.forEach { k ->
            if (!obj.has(k)) err(path, "falta la clave obligatoria '$k' ($what)")
        }
        obj.keySet().forEach { k ->
            if (k !in allowed) err("$path.$k", "clave desconocida ($what)")
        }
    }

    private fun reqNum(obj: JsonObject, key: String, path: String, lo: Double, hi: Double): Double? {
        val el = obj.get(key) ?: return null // checkKeys ya reportó la falta
        if (!el.isJsonPrimitive || !el.asJsonPrimitive.isNumber) {
            err("$path.$key", "debe ser número")
            return null
        }
        val v = el.asDouble
        if (!v.isFinite()) {
            err("$path.$key", "debe ser finito")
            return null
        }
        if (v < lo || v > hi) err("$path.$key", "$v fuera de [$lo, $hi]")
        return v
    }

    private fun reqStr(obj: JsonObject, key: String, path: String): String? {
        val el = obj.get(key) ?: return null // checkKeys ya reportó la falta
        if (!el.isJsonPrimitive || !el.asJsonPrimitive.isString) {
            err("$path.$key", "debe ser string")
            return null
        }
        val v = el.asString
        if (v.isBlank()) err("$path.$key", "debe ser string no vacío")
        return v
    }

    private fun checkEqBands(e: JsonObject): EqBandsDb? {
        val eqEl = e.get("eq_bands_db")
        if (eqEl == null || !eqEl.isJsonObject) {
            err("\$.engine.eq_bands_db", "debe ser un objeto")
            return null
        }
        val eq = eqEl.asJsonObject
        checkKeys(eq, EQ_BAND_KEYS, EQ_BAND_KEYS, "\$.engine.eq_bands_db", "slots EQ")
        val low = reqNum(eq, "low_60_120", "\$.engine.eq_bands_db", -2.0, 2.0)
        val mid = reqNum(eq, "mid_500_1k", "\$.engine.eq_bands_db", -2.0, 2.0)
        val high = reqNum(eq, "high_2k_4k", "\$.engine.eq_bands_db", -2.0, 2.0)
        val presence = reqNum(eq, "presence_4k_8k", "\$.engine.eq_bands_db", -2.0, 2.0)
        return if (low != null && mid != null && high != null && presence != null) {
            EqBandsDb(low, mid, high, presence)
        } else {
            null
        }
    }

    private fun checkCompressor(e: JsonObject): CompressorParams? {
        val cEl = e.get("compressor")
        if (cEl == null || !cEl.isJsonObject) {
            err("\$.engine.compressor", "debe ser un objeto")
            return null
        }
        val c = cEl.asJsonObject
        checkKeys(c, COMPRESSOR_KEYS, COMPRESSOR_KEYS, "\$.engine.compressor", "claves del compresor")
        val threshold = reqNum(c, "threshold_db", "\$.engine.compressor", -60.0, 0.0)
        val ratio = reqNum(c, "ratio", "\$.engine.compressor", 1.0, 20.0)
        val attack = c.get("attack_ms")?.let {
            if (!it.isJsonPrimitive || !it.asJsonPrimitive.isNumber) {
                err("\$.engine.compressor.attack_ms", "debe ser número"); null
            } else {
                val v = it.asDouble
                if (!v.isFinite() || v <= 0.0 || v > 1000.0) {
                    err("\$.engine.compressor.attack_ms", "$v fuera de (0, 1000]")
                }
                v
            }
        }
        val release = c.get("release_ms")?.let {
            if (!it.isJsonPrimitive || !it.asJsonPrimitive.isNumber) {
                err("\$.engine.compressor.release_ms", "debe ser número"); null
            } else {
                val v = it.asDouble
                if (!v.isFinite() || v <= 0.0 || v > 5000.0) {
                    err("\$.engine.compressor.release_ms", "$v fuera de (0, 5000]")
                }
                v
            }
        }
        return if (threshold != null && ratio != null && attack != null && release != null) {
            CompressorParams(threshold, ratio, attack, release)
        } else {
            null
        }
    }

    private fun checkStructuralSplit(e: JsonObject): Pair<Double, Double>? {
        val sEl = e.get("structural_split")
        if (sEl == null || !sEl.isJsonObject) {
            err("\$.engine.structural_split", "debe ser un objeto")
            return null
        }
        val s = sEl.asJsonObject
        checkKeys(s, SPLIT_KEYS, SPLIT_KEYS, "\$.engine.structural_split", "claves del split estructural")
        val transientAmount = reqNum(s, "transient_amount", "\$.engine.structural_split", 0.0, 1.0)
        val tonalAmount = reqNum(s, "tonal_amount", "\$.engine.structural_split", 0.0, 1.0)
        val transientRatio = reqNum(s, "transient_energy_ratio", "\$.engine.structural_split", 0.0, 1.0)
        val tonalRatio = reqNum(s, "tonal_energy_ratio", "\$.engine.structural_split", 0.0, 1.0)
        if (transientRatio != null && tonalRatio != null &&
            kotlin.math.abs((transientRatio + tonalRatio) - 1.0) > 0.01
        ) {
            err("\$.engine.structural_split", "transient+tonal = ${transientRatio + tonalRatio}, debe ser 1.0 (±0.01)")
        }
        return if (transientAmount != null && tonalAmount != null) {
            transientAmount to tonalAmount
        } else {
            null
        }
    }

    // ------------------------------------------------------------------
    // (e) Fingerprint ERB.
    // ------------------------------------------------------------------

    private fun checkErbBands(f: JsonObject): Map<String, ErbBand>? {
        val el = f.get("erb_bands_measured")
        if (el == null || !el.isJsonObject) {
            err("\$.fingerprint.erb_bands_measured", "debe ser un objeto")
            return null
        }
        val bands = el.asJsonObject
        if (bands.size() == 0) {
            err("\$.fingerprint.erb_bands_measured", "debe tener al menos una banda medida")
            return emptyMap()
        }
        val out = mutableMapOf<String, ErbBand>()
        bands.entrySet().forEach { (band, v) ->
            val path = "\$.fingerprint.erb_bands_measured.$band"
            if (!v.isJsonObject) {
                err(path, "debe ser un objeto {coherence, error_db}")
            } else {
                val b = v.asJsonObject
                checkKeys(b, ERB_BAND_KEYS, ERB_BAND_KEYS, path, "claves de banda ERB")
                val coherence = reqNum(b, "coherence", path, 0.0, 1.0)
                val errorDb = reqNum(b, "error_db", path, -80.0, 80.0)
                if (coherence != null && errorDb != null) out[band] = ErbBand(coherence, errorDb)
            }
        }
        return out
    }

    private fun checkDetectedIssues(f: JsonObject) {
        val el = f.get("detected_issues") ?: return
        if (!el.isJsonArray) {
            err("\$.fingerprint.detected_issues", "debe ser un array de strings")
            return
        }
        el.asJsonArray.forEachIndexed { i, item ->
            if (!item.isJsonPrimitive || !item.asJsonPrimitive.isString) {
                err("\$.fingerprint.detected_issues[$i]", "debe ser string")
            }
        }
    }

    /**
     * Campos de medición de archivo: opcionales en sesión en vivo, pero
     * si vienen, se validan con los rangos del validador Python.
     */
    private fun checkFileFingerprintFields(f: JsonObject) {
        reqNumIfPresent(f, "loudness_lufs_integrated", "\$.fingerprint", -70.0, 0.0)
        reqNumIfPresent(f, "loudness_rms_dbfs", "\$.fingerprint", -90.0, 0.0)
        reqNumIfPresent(f, "peak_dbfs", "\$.fingerprint", -120.0, 0.0)
        reqNumIfPresent(f, "crest_db", "\$.fingerprint", 0.0, 40.0)
        reqNumIfPresent(f, "stereo_correlation", "\$.fingerprint", -1.0, 1.0)
        reqNumIfPresent(f, "stereo_width_side_mid", "\$.fingerprint", 0.0, 2.0)
        val tr = reqNumIfPresent(f, "transient_energy_ratio", "\$.fingerprint", 0.0, 1.0)
        val to = reqNumIfPresent(f, "tonal_energy_ratio", "\$.fingerprint", 0.0, 1.0)
        if (tr != null && to != null && kotlin.math.abs((tr + to) - 1.0) > 0.01) {
            err("\$.fingerprint", "transient+tonal = ${tr + to}, debe ser 1.0 (±0.01)")
        }
        val sr = f.get("sample_rate_hz")
        if (sr != null) {
            val v = if (sr.isJsonPrimitive && sr.asJsonPrimitive.isNumber) sr.asDouble else Double.NaN
            if (v !in listOf(44100.0, 48000.0, 96000.0, 192000.0)) {
                err("\$.fingerprint.sample_rate_hz", "$v no es una tasa soportada (44100/48000/96000/192000)")
            }
        }
        val ch = f.get("channels")
        if (ch != null) {
            val v = if (ch.isJsonPrimitive && ch.asJsonPrimitive.isNumber) ch.asDouble else Double.NaN
            if (v != 1.0 && v != 2.0) err("\$.fingerprint.channels", "debe ser 1 o 2")
        }
        val spec = f.get("spectrum_db_rel")
        if (spec != null) {
            if (!spec.isJsonObject) {
                err("\$.fingerprint.spectrum_db_rel", "debe ser un objeto")
            } else {
                val s = spec.asJsonObject
                checkKeys(s, SPECTRUM_BAND_KEYS, SPECTRUM_BAND_KEYS, "\$.fingerprint.spectrum_db_rel", "bandas del espectro")
                SPECTRUM_BAND_KEYS.forEach { reqNum(s, it, "\$.fingerprint.spectrum_db_rel", -80.0, 0.0) }
            }
        }
    }

    private fun reqNumIfPresent(obj: JsonObject, key: String, path: String, lo: Double, hi: Double): Double? {
        if (!obj.has(key)) return null
        return reqNum(obj, key, path, lo, hi)
    }

    private fun checkOperatorStep(name: String, candidate: Double?, previousValue: Double) {
        if (candidate == null) return
        if (kotlin.math.abs(candidate - previousValue) > ProfileApplier.MAX_OPERATOR_STEP + 1e-9) {
            err(
                "\$.engine.$name",
                "salto de $previousValue a $candidate (|Δ| ${kotlin.math.abs(candidate - previousValue)}), " +
                    "máximo ${ProfileApplier.MAX_OPERATOR_STEP} por versión"
            )
        }
    }

    companion object {
        private val ROOT_KEYS = setOf(
            "format", "format_version", "track", "artist", "source",
            "duration_s", "fingerprint", "engine", "derivation"
        )
        private val ENGINE_KEYS = setOf(
            "orbe_amount", "orbe_distance_m", "holos_amount", "phi_amount",
            "volterra_enabled", "volterra_mix", "eq_bands_db", "compressor",
            "exciter_amount", "trim_db", "net_gain_db", "structural_split"
        )
        private val EQ_BAND_KEYS = setOf("low_60_120", "mid_500_1k", "high_2k_4k", "presence_4k_8k")
        private val COMPRESSOR_KEYS = setOf("threshold_db", "ratio", "attack_ms", "release_ms")
        private val SPLIT_KEYS = setOf(
            "transient_amount", "tonal_amount", "transient_energy_ratio", "tonal_energy_ratio"
        )
        private val DERIVATION_KEYS = setOf("eq", "compressor", "structural_split", "loudness", "note")
        private val ERB_BAND_KEYS = setOf("coherence", "error_db")

        /** (e) Obligatorios en sesión en vivo (C2.e + refactorización §3.2). */
        private val FINGERPRINT_REQUIRED = setOf(
            "mean_coherence", "spectral_error_rms_db", "erb_bands_measured",
            "detected_issues"
        )

        /** Fingerprint permitido: ERB (vivo) + medición de archivo (opcional). */
        private val FINGERPRINT_ALLOWED = FINGERPRINT_REQUIRED + setOf(
            "loudness_lufs_integrated", "loudness_rms_dbfs", "peak_dbfs", "crest_db",
            "spectrum_db_rel", "transient_energy_ratio", "tonal_energy_ratio",
            "stereo_correlation", "stereo_width_side_mid", "sample_rate_hz", "channels"
        )
        private val SPECTRUM_BAND_KEYS = setOf(
            "sub", "low_60_120", "lowmid", "mid_500_1k", "highmid",
            "high_2k_4k", "presence_4k_8k", "air", "ultra"
        )

        /** (g) Campos prohibidos en un perfil descargado (red, permisos, token, rutas). */
        private val FORBIDDEN_KEYS = setOf(
            "network", "red", "permissions", "permisos", "token", "api_key", "apikey",
            "password", "secret", "path", "paths", "filepath", "route", "routes",
            "url", "host", "server", "command", "exec", "shell", "file",
            "directory", "credentials", "auth", "authorization", "bearer"
        )

        /** Marcadores de source de sesión en vivo (minúsculas). */
        private val LIVE_SOURCE_MARKERS = listOf("en vivo", "live", "muselink", "sesi", "erb")

        /** `duration_s == 0.0` solo vale si el source es de sesión en vivo. */
        private fun isLiveSessionSource(source: String): Boolean {
            val s = source.lowercase()
            return LIVE_SOURCE_MARKERS.any { s.contains(it) }
        }
    }
}
